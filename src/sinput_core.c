// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental SInput HID driver.
 *
 * This is an out-of-tree research driver. Do not treat it as an upstream
 * Linux implementation yet.
 *
 * HID probe/remove/raw_event dispatch and the SInput FEATURES command
 * round-trip live here; each subsystem (gamepad/IMU input, battery
 * power_supply, LEDs) has its own source file -- see sinput.h.
 */

#include <linux/completion.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include "sinput.h"
#include "sinput_protocol.h"

#define DRV_NAME "sinput"

static void sinput_parse_features(struct sinput_device *sdev,
				  const u8 *data, size_t size)
{
	struct sinput_caps *caps = &sdev->caps;
	u8 flags0, flags1;

	if (size < SINPUT_INPUT_REPORT_SIZE)
		return;

	flags0 = data[SI_FEAT_FLAGS_0];
	flags1 = data[SI_FEAT_FLAGS_1];

	caps->protocol_version = get_unaligned_le16(data + SI_FEAT_PROTOCOL_VER);
	caps->polling_rate_us = get_unaligned_le16(data + SI_FEAT_POLL_RATE_US);

	caps->rumble        = !!(flags0 & SI_FLAG0_RUMBLE);
	caps->player_leds   = !!(flags0 & SI_FLAG0_PLAYER_LEDS);
	caps->accel         = !!(flags0 & SI_FLAG0_ACCEL);
	caps->gyro          = !!(flags0 & SI_FLAG0_GYRO);
	caps->left_stick    = !!(flags0 & SI_FLAG0_LEFT_STICK);
	caps->right_stick   = !!(flags0 & SI_FLAG0_RIGHT_STICK);
	caps->left_trigger  = !!(flags0 & SI_FLAG0_LEFT_TRIGGER);
	caps->right_trigger = !!(flags0 & SI_FLAG0_RIGHT_TRIGGER);

	caps->touchpad = !!(flags1 & SI_FLAG1_TOUCHPAD);
	caps->rgb_led  = !!(flags1 & SI_FLAG1_RGB_LED);

	caps->button_mask = get_unaligned_le32(data + SI_FEAT_USAGE_MASK_0);

	caps->valid = true;

	hid_info(sdev->hdev,
		 "SInput protocol v%u, poll rate %u us, sticks=%d/%d triggers=%d/%d accel=%d gyro=%d buttons=0x%08x\n",
		 caps->protocol_version, caps->polling_rate_us,
		 caps->left_stick, caps->right_stick,
		 caps->left_trigger, caps->right_trigger,
		 caps->accel, caps->gyro, caps->button_mask);
}

/*
 * Shared by every SInput output command (FEATURES here, player LED / RGB
 * LED in sinput_led.c). Unlike hid-playstation.c's DualSense driver, which
 * batches rumble+LEDs+lightbar into one big periodic report via a
 * workqueue, SInput's commands are already discrete single-purpose packets
 * -- a direct blocking send is the right fit here, not an added workqueue
 * indirection.
 *
 * Caller must already hold output_lock. This is deliberately not taken
 * internally: a caller with driver-owned state to update alongside the
 * send (e.g. sinput_led.c's player_leds_state bitmask) needs the update and
 * the send to be one atomic critical section, or two concurrent callers can
 * race and the device ends up displaying a stale value that never gets
 * corrected -- there is no periodic resync for output commands.
 */
int sinput_send_output_command(struct sinput_device *sdev, u8 cmd,
				const u8 *payload, size_t payload_len)
{
	u8 *buf;
	int ret;

	lockdep_assert_held(&sdev->output_lock);

	if (payload_len > SINPUT_OUTPUT_REPORT_SIZE - (SI_OUT_CMD + 1))
		return -EINVAL;

	buf = kzalloc(SINPUT_OUTPUT_REPORT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = SINPUT_REPORT_ID_OUTPUT;
	buf[SI_OUT_CMD] = cmd;
	if (payload && payload_len)
		memcpy(buf + SI_OUT_CMD + 1, payload, payload_len);

	ret = hid_hw_output_report(sdev->hdev, buf, SINPUT_OUTPUT_REPORT_SIZE);

	kfree(buf);
	return ret;
}

static int sinput_request_features(struct sinput_device *sdev)
{
	int ret;

	mutex_lock(&sdev->output_lock);
	ret = sinput_send_output_command(sdev, SINPUT_CMD_FEATURES, NULL, 0);
	mutex_unlock(&sdev->output_lock);

	return ret;
}

static int sinput_probe(struct hid_device *hdev,
			const struct hid_device_id *id)
{
	struct sinput_device *sdev;
	int ret;

	sdev = devm_kzalloc(&hdev->dev, sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	sdev->hdev = hdev;
	hid_set_drvdata(hdev, sdev);
	init_completion(&sdev->caps_done);

	/*
	 * Until a feature response says otherwise, assume every axis, the
	 * IMU, and both LEDs are present. This preserves today's behaviour
	 * for devices that do not implement the SInput command/feature
	 * protocol, such as the generic bring-up test ID. Harmless for the
	 * LEDs even if wrong: worst case is an LED class device userspace can
	 * toggle that a real device without that LED silently ignores, unlike
	 * assuming an axis that isn't really there and misreading garbage.
	 */
	sdev->caps.left_stick = true;
	sdev->caps.right_stick = true;
	sdev->caps.left_trigger = true;
	sdev->caps.right_trigger = true;
	sdev->caps.accel = true;
	sdev->caps.gyro = true;
	sdev->caps.player_leds = true;
	sdev->caps.rgb_led = true;
	sdev->caps.button_mask = ~0u;

	/*
	 * Must be ready before hid_hw_start(): that call enables raw_event
	 * delivery immediately, and sinput_battery_update() takes this lock
	 * on every state report from then on. Initializing it later, inside
	 * sinput_battery_init() alongside the power_supply registration
	 * itself, left exactly this kind of window open once before (see
	 * sinput_input_report()'s NULL sdev->input comment) and did again
	 * here until review caught it.
	 */
	spin_lock_init(&sdev->battery_lock);
	sdev->battery_plug_status = SI_PLUG_STATUS_UNKNOWN;
	sdev->battery_capacity = 100;

	/*
	 * Also must be ready before hid_hw_start(): the FEATURES request is
	 * the first output command and is sent right after, via
	 * sinput_send_output_command() below.
	 */
	mutex_init(&sdev->output_lock);

	/*
	 * Deliberately do not request HID_CONNECT_HIDINPUT. This prevents
	 * hid-generic from creating a second input device for this HID
	 * collection while we develop the SInput-specific input path.
	 */
	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = sinput_request_features(sdev);
	if (ret < 0)
		hid_info(hdev, "could not send SInput features request: %d\n", ret);
	else if (!wait_for_completion_timeout(&sdev->caps_done, msecs_to_jiffies(200)))
		hid_info(hdev, "no SInput features response, assuming full capability set\n");

	ret = sinput_input_init(sdev);
	if (ret)
		goto stop;

	ret = sinput_battery_init(sdev);
	if (ret)
		goto stop;

	if (sdev->caps.accel || sdev->caps.gyro) {
		ret = sinput_imu_init(sdev);
		if (ret)
			goto stop;
	}

	ret = sinput_led_init(sdev);
	if (ret)
		goto stop;

	hid_info(hdev, "SInput driver attached (experimental)\n");
	return 0;

stop:
	hid_hw_stop(hdev);
	return ret;
}

static void sinput_remove(struct hid_device *hdev)
{
	struct sinput_device *sdev = hid_get_drvdata(hdev);

	hid_hw_stop(hdev);
	mutex_destroy(&sdev->output_lock);
}

static int sinput_raw_event(struct hid_device *hdev,
			    struct hid_report *report,
			    u8 *data, int size)
{
	struct sinput_device *sdev = hid_get_drvdata(hdev);

	if (!sdev)
		return 0;

	if (size < 1)
		return 0;

	if (data[0] == SINPUT_REPORT_ID_STATE) {
		if (size >= SINPUT_INPUT_REPORT_SIZE) {
			sinput_input_report(sdev, data);
			sinput_battery_update(sdev, data);
		}
	} else if (data[0] == SINPUT_REPORT_ID_CMD && size > SI_CMD_ECHO &&
		   data[SI_CMD_ECHO] == SINPUT_CMD_FEATURES) {
		sinput_parse_features(sdev, data, size);
		complete(&sdev->caps_done);
	}

	/* Returning 0 leaves the report available to hidraw. */
	return 0;
}

static const struct hid_device_id sinput_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_SINPUT, USB_DEVICE_ID_SINPUT) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_SINPUT, USB_DEVICE_ID_SINPUT) },
	{ }
};
MODULE_DEVICE_TABLE(hid, sinput_devices);

static struct hid_driver sinput_driver = {
	.name = DRV_NAME,
	.id_table = sinput_devices,
	.probe = sinput_probe,
	.remove = sinput_remove,
	.raw_event = sinput_raw_event,
};

module_hid_driver(sinput_driver);

MODULE_AUTHOR("LeeNX");
MODULE_DESCRIPTION("Experimental Linux HID driver for SInput gamepads");
MODULE_LICENSE("GPL");
