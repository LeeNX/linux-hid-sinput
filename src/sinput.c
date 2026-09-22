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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
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
 * behaviour of exposing every axis, button, and the IMU unconditionally.
 *
 * Only consulted at registration time (sinput_input_init()/sinput_imu_init(),
 * called once from probe()). A FEATURES response can arrive after that --
 * there is no lock between this struct and the raw_event path that mutates
 * it -- so reporting must never branch on these fields afterwards; it reads
 * back the input device's own (fixed-after-registration) capability bitmaps
 * instead. See sinput_report_gamepad().
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
	/*
	 * Bit N set means the SInput usage mask reports button N (see
	 * SINPUT_BTN_IDX_* in sinput_protocol.h) as present. Defaults to
	 * all-ones so a device that never answers the features request
	 * keeps every mapped button registered, same as the other caps.
	 */
	u32 button_mask;
};

struct sinput_device {
	struct hid_device *hdev;
	struct input_dev *input;
	struct input_dev *imu;
	struct sinput_caps caps;
	struct completion caps_done;

	/*
	 * Battery/charge state. Unlike sdev->caps (only read at registration
	 * time, see the comment above struct sinput_caps), these are read
	 * throughout the device's life by sinput_battery_get_property() --
	 * called from arbitrary process context via sysfs, concurrently with
	 * sinput_report_gamepad() updating them from raw_event context -- so
	 * they need a real lock, not the "fixed after registration" trick
	 * used for input capabilities.
	 */
	spinlock_t battery_lock;
	u8 battery_capacity;     /* 0-100 */
	u8 battery_plug_status;  /* SI_PLUG_STATUS_* */

	struct power_supply_desc battery_desc;
	struct power_supply *battery;
};

/* Single source of truth for which SInput button bit maps to which Linux key. */
struct sinput_button_map {
	u8 idx;
	u16 code;
};

static const struct sinput_button_map sinput_buttons[] = {
	{ SINPUT_BTN_IDX_SOUTH,         BTN_SOUTH },
	{ SINPUT_BTN_IDX_EAST,          BTN_EAST },
	{ SINPUT_BTN_IDX_WEST,          BTN_WEST },
	{ SINPUT_BTN_IDX_NORTH,         BTN_NORTH },
	{ SINPUT_BTN_IDX_DPAD_UP,       BTN_DPAD_UP },
	{ SINPUT_BTN_IDX_DPAD_DOWN,     BTN_DPAD_DOWN },
	{ SINPUT_BTN_IDX_DPAD_LEFT,     BTN_DPAD_LEFT },
	{ SINPUT_BTN_IDX_DPAD_RIGHT,    BTN_DPAD_RIGHT },
	{ SINPUT_BTN_IDX_LEFT_STICK,    BTN_THUMBL },
	{ SINPUT_BTN_IDX_RIGHT_STICK,   BTN_THUMBR },
	{ SINPUT_BTN_IDX_LEFT_BUMPER,   BTN_TL },
	{ SINPUT_BTN_IDX_RIGHT_BUMPER,  BTN_TR },
	{ SINPUT_BTN_IDX_LEFT_TRIGGER,  BTN_TL2 },
	{ SINPUT_BTN_IDX_RIGHT_TRIGGER, BTN_TR2 },
	{ SINPUT_BTN_IDX_START,         BTN_START },
	{ SINPUT_BTN_IDX_BACK,          BTN_SELECT },
	{ SINPUT_BTN_IDX_GUIDE,         BTN_MODE },
	{ SINPUT_BTN_IDX_CAPTURE,       BTN_MISC },
};

static s16 si_s16(const u8 *d, unsigned int off)
{
	return (s16)get_unaligned_le16(d + off);
}

static void sinput_report_gamepad(struct sinput_device *sdev,
				  const u8 *data, size_t size)
{
	struct input_dev *in = sdev->input;
	unsigned long flags;
	u32 buttons;
	unsigned int i;

	/*
	 * hid_hw_start() enables raw_event delivery before sinput_input_init()
	 * assigns sdev->input (it runs later in probe(), after the FEATURES
	 * request/timeout). An unsolicited state report in that window would
	 * otherwise dereference a NULL input_dev.
	 */
	if (!in)
		return;

	if (size < SINPUT_INPUT_REPORT_SIZE)
		return;

	if (data[0] != SINPUT_REPORT_ID_STATE)
		return;

	/*
	 * SI_PLUG_STATUS/SI_CHARGE_LEVEL have no capability bit -- they're
	 * always present -- so unlike everything else in this function they
	 * don't need a "was this registered" check, just the lock that
	 * sinput_battery_get_property() also takes.
	 */
	spin_lock_irqsave(&sdev->battery_lock, flags);
	sdev->battery_plug_status = data[SI_PLUG_STATUS];
	/* Protocol says 0-100; clamp defensively against a malformed/buggy device. */
	sdev->battery_capacity = min_t(u8, data[SI_CHARGE_LEVEL], 100);
	spin_unlock_irqrestore(&sdev->battery_lock, flags);

	buttons = get_unaligned_le32(data + SI_BUTTONS_0);

	/*
	 * Gate on what actually got registered (in->keybit/absbit), not on
	 * sdev->caps: a FEATURES response can arrive after sinput_input_init()
	 * already ran (e.g. past the probe timeout), and caps is mutated in
	 * place with no synchronization against this raw_event path. Input
	 * capability bitmaps are fixed at input_register_device() time and
	 * never change afterwards, so reading them here is race-free and keeps
	 * reporting consistent with what was actually registered.
	 */
	for (i = 0; i < ARRAY_SIZE(sinput_buttons); i++) {
		const struct sinput_button_map *b = &sinput_buttons[i];

		if (test_bit(b->code, in->keybit))
			input_report_key(in, b->code, !!(buttons & BIT(b->idx)));
	}

	if (test_bit(ABS_X, in->absbit)) {
		input_report_abs(in, ABS_X, si_s16(data, SI_LEFT_X));
		input_report_abs(in, ABS_Y, si_s16(data, SI_LEFT_Y));
	}
	if (test_bit(ABS_RX, in->absbit)) {
		input_report_abs(in, ABS_RX, si_s16(data, SI_RIGHT_X));
		input_report_abs(in, ABS_RY, si_s16(data, SI_RIGHT_Y));
	}
	if (test_bit(ABS_Z, in->absbit))
		input_report_abs(in, ABS_Z, si_s16(data, SI_LEFT_TRIGGER));
	if (test_bit(ABS_RZ, in->absbit))
		input_report_abs(in, ABS_RZ, si_s16(data, SI_RIGHT_TRIGGER));

	input_sync(in);

	if (sdev->imu) {
		if (test_bit(ABS_X, sdev->imu->absbit)) {
			input_report_abs(sdev->imu, ABS_X, si_s16(data, SI_ACCEL_X));
			input_report_abs(sdev->imu, ABS_Y, si_s16(data, SI_ACCEL_Y));
			input_report_abs(sdev->imu, ABS_Z, si_s16(data, SI_ACCEL_Z));
		}
		if (test_bit(ABS_RX, sdev->imu->absbit)) {
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

	caps->button_mask = get_unaligned_le32(data + SI_FEAT_USAGE_MASK_0);

	caps->valid = true;

	hid_info(sdev->hdev,
		 "SInput protocol v%u, poll rate %u us, sticks=%d/%d triggers=%d/%d accel=%d gyro=%d buttons=0x%08x\n",
		 caps->protocol_version, caps->polling_rate_us,
		 caps->left_stick, caps->right_stick,
		 caps->left_trigger, caps->right_trigger,
		 caps->accel, caps->gyro, caps->button_mask);
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
	unsigned int i;
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

	for (i = 0; i < ARRAY_SIZE(sinput_buttons); i++) {
		if (sdev->caps.button_mask & BIT(sinput_buttons[i].idx))
			__set_bit(sinput_buttons[i].code, in->keybit);
	}

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

static int sinput_battery_status(u8 plug_status)
{
	switch (plug_status) {
	case SI_PLUG_STATUS_CHARGING:
		return POWER_SUPPLY_STATUS_CHARGING;
	case SI_PLUG_STATUS_CHARGED:
		return POWER_SUPPLY_STATUS_FULL;
	case SI_PLUG_STATUS_ON_BATTERY:
		return POWER_SUPPLY_STATUS_DISCHARGING;
	case SI_PLUG_STATUS_NO_BATTERY:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	case SI_PLUG_STATUS_UNKNOWN:
	default:
		return POWER_SUPPLY_STATUS_UNKNOWN;
	}
}

/*
 * Allow-list rather than deny-list: SI_PLUG_STATUS_UNKNOWN (the value this
 * driver initializes to before the first state report, and what a device
 * that never reports plug status at all would stay at forever) means "not
 * confirmed present", not "present". Only report present for the plug
 * states that actually confirm a battery exists.
 */
static bool sinput_battery_present(u8 plug_status)
{
	return plug_status == SI_PLUG_STATUS_CHARGING ||
	       plug_status == SI_PLUG_STATUS_CHARGED ||
	       plug_status == SI_PLUG_STATUS_ON_BATTERY;
}

static enum power_supply_property sinput_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
};

static int sinput_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct sinput_device *sdev = power_supply_get_drvdata(psy);
	unsigned long flags;
	u8 plug_status, capacity;
	int ret = 0;

	spin_lock_irqsave(&sdev->battery_lock, flags);
	plug_status = sdev->battery_plug_status;
	capacity = sdev->battery_capacity;
	spin_unlock_irqrestore(&sdev->battery_lock, flags);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = sinput_battery_status(plug_status);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = sinput_battery_present(plug_status);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = capacity;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/*
 * SI_PLUG_STATUS/SI_CHARGE_LEVEL have no capability bit (see the comment in
 * sinput_protocol.h), so unlike sinput_input_init()/sinput_imu_init() this
 * never depends on sdev->caps and can run regardless of whether a FEATURES
 * response ever arrives.
 */
static int sinput_battery_init(struct sinput_device *sdev)
{
	struct power_supply_config battery_cfg = { .drv_data = sdev };
	struct power_supply *battery;
	int ret;

	sdev->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	sdev->battery_desc.properties = sinput_battery_props;
	sdev->battery_desc.num_properties = ARRAY_SIZE(sinput_battery_props);
	sdev->battery_desc.get_property = sinput_battery_get_property;
	sdev->battery_desc.name = devm_kasprintf(&sdev->hdev->dev, GFP_KERNEL,
						  "sinput-battery-%s",
						  dev_name(&sdev->hdev->dev));
	if (!sdev->battery_desc.name)
		return -ENOMEM;

	battery = devm_power_supply_register(&sdev->hdev->dev, &sdev->battery_desc,
					      &battery_cfg);
	if (IS_ERR(battery)) {
		hid_err(sdev->hdev, "could not register battery device: %ld\n",
			PTR_ERR(battery));
		return PTR_ERR(battery);
	}
	sdev->battery = battery;

	/*
	 * Only links the battery's sysfs dir to the HID device for topology
	 * discovery (udev/upower); doesn't affect POWER_SUPPLY_PROP_* at all.
	 * Not worth tearing down the already-registered gamepad/IMU input
	 * devices over, so log and keep going rather than fail probe().
	 */
	ret = power_supply_powers(sdev->battery, &sdev->hdev->dev);
	if (ret)
		hid_info(sdev->hdev, "could not link battery power topology: %d\n", ret);

	return 0;
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
	sdev->caps.button_mask = ~0u;

	/*
	 * Must be ready before hid_hw_start(): that call enables raw_event
	 * delivery immediately, and sinput_report_gamepad() takes this lock
	 * on every state report from then on (it only checks sdev->input for
	 * readiness, not the battery fields -- see the comment there).
	 * Initializing it later, inside sinput_battery_init() alongside the
	 * power_supply registration itself, left exactly this kind of window
	 * open once before (see sinput_report_gamepad()'s NULL sdev->input
	 * comment) and did again here until review caught it.
	 */
	spin_lock_init(&sdev->battery_lock);
	sdev->battery_plug_status = SI_PLUG_STATUS_UNKNOWN;
	sdev->battery_capacity = 100;

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
