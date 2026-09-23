// SPDX-License-Identifier: GPL-2.0-only
/*
 * SInput gamepad buttons/axes and IMU input devices.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include "sinput.h"
#include "sinput_protocol.h"

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

/*
 * data is a full, validated SINPUT_REPORT_ID_STATE report -- report ID and
 * size are checked once by the raw_event dispatcher in sinput_core.c before
 * calling here (and into sinput_battery_update()).
 */
void sinput_input_report(struct sinput_device *sdev, const u8 *data)
{
	struct input_dev *in = sdev->input;
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

int sinput_input_init(struct sinput_device *sdev)
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

int sinput_imu_init(struct sinput_device *sdev)
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
