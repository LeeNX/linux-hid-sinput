// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental SInput HID driver.
 *
 * This is an out-of-tree research driver. Do not treat it as an upstream
 * Linux implementation yet.
 */

#include <linux/completion.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include "sinput_protocol.h"

#define DRV_NAME "sinput"

/*
 * Capabilities decoded from the SInput feature response. Every field
 * defaults to "supported" so that a device which never answers the
 * features request (such as the generic bring-up test ID) keeps today's
 * behaviour of exposing every axis and the IMU unconditionally.
 */
struct sinput_caps {
	bool valid;
	u16 protocol_version;
	u16 polling_rate_us;
	bool rumble;
	bool player_leds;
	bool accel;
	bool gyro;
	bool left_stick;
	bool right_stick;
	bool left_trigger;
	bool right_trigger;
	bool touchpad;
	bool rgb_led;
};

struct sinput_device {
	struct hid_device *hdev;
	struct input_dev *input;
	struct input_dev *imu;
	struct sinput_caps caps;
	struct completion caps_done;
};

static s16 si_s16(const u8 *d, unsigned int off)
{
	return (s16)get_unaligned_le16(d + off);
}

static void sinput_report_gamepad(struct sinput_device *sdev,
				  const u8 *data, size_t size)
{
	struct input_dev *in = sdev->input;
	u32 buttons;

	if (size < SINPUT_INPUT_REPORT_SIZE)
		return;

	if (data[0] != SINPUT_REPORT_ID_STATE)
		return;

	buttons = get_unaligned_le32(data + SI_BUTTONS_0);

	/* SInput button indices 0..23 are mapped to common Linux gamepad keys. */
	input_report_key(in, BTN_EAST,     !!(buttons & BIT(0)));
	input_report_key(in, BTN_SOUTH,    !!(buttons & BIT(1)));
	input_report_key(in, BTN_NORTH,    !!(buttons & BIT(2)));
	input_report_key(in, BTN_WEST,     !!(buttons & BIT(3)));

	input_report_key(in, BTN_DPAD_UP,    !!(buttons & BIT(4)));
	input_report_key(in, BTN_DPAD_DOWN,  !!(buttons & BIT(5)));
	input_report_key(in, BTN_DPAD_LEFT,  !!(buttons & BIT(6)));
	input_report_key(in, BTN_DPAD_RIGHT, !!(buttons & BIT(7)));

	input_report_key(in, BTN_THUMBL, !!(buttons & BIT(8)));
	input_report_key(in, BTN_THUMBR, !!(buttons & BIT(9)));
	input_report_key(in, BTN_TL,     !!(buttons & BIT(10)));
	input_report_key(in, BTN_TR,     !!(buttons & BIT(11)));

	input_report_key(in, BTN_TL2, !!(buttons & BIT(12)));
	input_report_key(in, BTN_TR2, !!(buttons & BIT(13)));

	input_report_key(in, BTN_START,  !!(buttons & BIT(16)));
	input_report_key(in, BTN_SELECT, !!(buttons & BIT(17)));
	input_report_key(in, BTN_MODE,   !!(buttons & BIT(18)));
	input_report_key(in, BTN_MISC,   !!(buttons & BIT(19)));

	if (sdev->caps.left_stick) {
		input_report_abs(in, ABS_X, si_s16(data, SI_LEFT_X));
		input_report_abs(in, ABS_Y, si_s16(data, SI_LEFT_Y));
	}
	if (sdev->caps.right_stick) {
		input_report_abs(in, ABS_RX, si_s16(data, SI_RIGHT_X));
		input_report_abs(in, ABS_RY, si_s16(data, SI_RIGHT_Y));
	}
	if (sdev->caps.left_trigger)
		input_report_abs(in, ABS_Z, si_s16(data, SI_LEFT_TRIGGER));
	if (sdev->caps.right_trigger)
		input_report_abs(in, ABS_RZ, si_s16(data, SI_RIGHT_TRIGGER));

	input_sync(in);

	if (sdev->imu) {
		if (sdev->caps.accel) {
			input_report_abs(sdev->imu, ABS_X, si_s16(data, SI_ACCEL_X));
			input_report_abs(sdev->imu, ABS_Y, si_s16(data, SI_ACCEL_Y));
			input_report_abs(sdev->imu, ABS_Z, si_s16(data, SI_ACCEL_Z));
		}
		if (sdev->caps.gyro) {
			input_report_abs(sdev->imu, ABS_RX, si_s16(data, SI_GYRO_X));
			input_report_abs(sdev->imu, ABS_RY, si_s16(data, SI_GYRO_Y));
			input_report_abs(sdev->imu, ABS_RZ, si_s16(data, SI_GYRO_Z));
		}
		input_sync(sdev->imu);
	}
}

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

	caps->valid = true;

	hid_info(sdev->hdev,
		 "SInput protocol v%u, poll rate %u us, sticks=%d/%d triggers=%d/%d accel=%d gyro=%d\n",
		 caps->protocol_version, caps->polling_rate_us,
		 caps->left_stick, caps->right_stick,
		 caps->left_trigger, caps->right_trigger,
		 caps->accel, caps->gyro);
}

static int sinput_request_features(struct sinput_device *sdev)
{
	u8 *buf;
	int ret;

	buf = kzalloc(SINPUT_OUTPUT_REPORT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = SINPUT_REPORT_ID_OUTPUT;
	buf[1] = SINPUT_CMD_FEATURES;

	ret = hid_hw_output_report(sdev->hdev, buf, SINPUT_OUTPUT_REPORT_SIZE);

	kfree(buf);
	return ret;
}

static int sinput_input_init(struct sinput_device *sdev)
{
	struct input_dev *in;
	int ret;

	in = devm_input_allocate_device(&sdev->hdev->dev);
	if (!in)
		return -ENOMEM;

	in->name = "SInput Gamepad";
	in->phys = sdev->hdev->phys;
	in->id.bustype = sdev->hdev->bus;
	in->id.vendor = sdev->hdev->vendor;
	in->id.product = sdev->hdev->product;
	in->id.version = sdev->hdev->version;

	__set_bit(EV_KEY, in->evbit);
	__set_bit(EV_ABS, in->evbit);

	__set_bit(BTN_EAST, in->keybit);
	__set_bit(BTN_SOUTH, in->keybit);
	__set_bit(BTN_NORTH, in->keybit);
	__set_bit(BTN_WEST, in->keybit);
	__set_bit(BTN_DPAD_UP, in->keybit);
	__set_bit(BTN_DPAD_DOWN, in->keybit);
	__set_bit(BTN_DPAD_LEFT, in->keybit);
	__set_bit(BTN_DPAD_RIGHT, in->keybit);
	__set_bit(BTN_THUMBL, in->keybit);
	__set_bit(BTN_THUMBR, in->keybit);
	__set_bit(BTN_TL, in->keybit);
	__set_bit(BTN_TR, in->keybit);
	__set_bit(BTN_TL2, in->keybit);
	__set_bit(BTN_TR2, in->keybit);
	__set_bit(BTN_START, in->keybit);
	__set_bit(BTN_SELECT, in->keybit);
	__set_bit(BTN_MODE, in->keybit);
	__set_bit(BTN_MISC, in->keybit);

	if (sdev->caps.left_stick) {
		input_set_abs_params(in, ABS_X, -32768, 32767, 0, 0);
		input_set_abs_params(in, ABS_Y, -32768, 32767, 0, 0);
	}
	if (sdev->caps.right_stick) {
		input_set_abs_params(in, ABS_RX, -32768, 32767, 0, 0);
		input_set_abs_params(in, ABS_RY, -32768, 32767, 0, 0);
	}
	if (sdev->caps.left_trigger)
		input_set_abs_params(in, ABS_Z, -32768, 32767, 0, 0);
	if (sdev->caps.right_trigger)
		input_set_abs_params(in, ABS_RZ, -32768, 32767, 0, 0);

	sdev->input = in;
	ret = input_register_device(in);
	if (ret)
		return ret;

	return 0;
}

static int sinput_imu_init(struct sinput_device *sdev)
{
	struct input_dev *imu;

	imu = devm_input_allocate_device(&sdev->hdev->dev);
	if (!imu)
		return -ENOMEM;

	imu->name = "SInput IMU";
	imu->phys = sdev->hdev->phys;
	imu->id.bustype = sdev->hdev->bus;

	__set_bit(EV_ABS, imu->evbit);
	if (sdev->caps.accel) {
		input_set_abs_params(imu, ABS_X, -32768, 32767, 0, 0);
		input_set_abs_params(imu, ABS_Y, -32768, 32767, 0, 0);
		input_set_abs_params(imu, ABS_Z, -32768, 32767, 0, 0);
	}
	if (sdev->caps.gyro) {
		input_set_abs_params(imu, ABS_RX, -32768, 32767, 0, 0);
		input_set_abs_params(imu, ABS_RY, -32768, 32767, 0, 0);
		input_set_abs_params(imu, ABS_RZ, -32768, 32767, 0, 0);
	}

	sdev->imu = imu;
	return input_register_device(imu);
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
	 * Until a feature response says otherwise, assume every axis and the
	 * IMU are present. This preserves today's behaviour for devices that
	 * do not implement the SInput command/feature protocol, such as the
	 * generic bring-up test ID.
	 */
	sdev->caps.left_stick = true;
	sdev->caps.right_stick = true;
	sdev->caps.left_trigger = true;
	sdev->caps.right_trigger = true;
	sdev->caps.accel = true;
	sdev->caps.gyro = true;

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

	if (sdev->caps.accel || sdev->caps.gyro) {
		ret = sinput_imu_init(sdev);
		if (ret)
			goto stop;
	}

	hid_info(hdev, "SInput driver attached (experimental)\n");
	return 0;

stop:
	hid_hw_stop(hdev);
	return ret;
}

static void sinput_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
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
		sinput_report_gamepad(sdev, data, size);
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
