// SPDX-License-Identifier: GPL-2.0-only
/*
 * SInput touchpad input device(s).
 *
 * Registration shape (one input_dev with N MT slots, or N input_devs with
 * one slot each) is decided once from sdev->caps.touchpad_count/
 * touchpad_finger_count in sinput_touchpad_init(), called from probe() the
 * same way sinput_imu_init() is. See struct sinput_caps's touchpad_count
 * comment in sinput.h for why these two fields decide the shape, and
 * sinput_touchpad_report()'s comment below for how the wire's two touch
 * slots map onto whatever shape got registered.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/kernel.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include "sinput.h"
#include "sinput_protocol.h"

static s16 si_s16(const u8 *d, unsigned int off)
{
	return (s16)get_unaligned_le16(d + off);
}

/*
 * Matches hid-playstation.c's ps_touchpad_create(): a separate input_dev
 * per touchpad, INPUT_PROP_BUTTONPAD (there is a click button underneath),
 * ABS_MT_POSITION_X/Y plus input_mt_init_slots() for however many fingers
 * this one supports. Unlike hid-playstation.c's DualShock/DualSense pad
 * (digital touch presence only), SInput's touch data carries a real
 * pressure value (SI_TOUCH*_P), so ABS_MT_PRESSURE is also registered --
 * see sinput_touchpad_report()'s presence test below for why 0 means "no
 * finger" rather than "zero pressure but still touching".
 */
static struct input_dev *sinput_touchpad_create(struct sinput_device *sdev,
						  const char *name, int slots)
{
	struct input_dev *tp;
	int ret;

	tp = devm_input_allocate_device(&sdev->hdev->dev);
	if (!tp)
		return ERR_PTR(-ENOMEM);

	tp->name = name;
	tp->phys = sdev->hdev->phys;
	tp->id.bustype = sdev->hdev->bus;
	tp->id.vendor = sdev->hdev->vendor;
	tp->id.product = sdev->hdev->product;
	tp->id.version = sdev->hdev->version;

	__set_bit(EV_KEY, tp->evbit);
	__set_bit(EV_ABS, tp->evbit);
	__set_bit(INPUT_PROP_POINTER, tp->propbit);
	__set_bit(INPUT_PROP_BUTTONPAD, tp->propbit);
	input_set_capability(tp, EV_KEY, BTN_LEFT);

	input_set_abs_params(tp, ABS_MT_POSITION_X, -32768, 32767, 0, 0);
	input_set_abs_params(tp, ABS_MT_POSITION_Y, -32768, 32767, 0, 0);
	input_set_abs_params(tp, ABS_MT_PRESSURE, 0, 32767, 0, 0);

	ret = input_mt_init_slots(tp, slots, INPUT_MT_POINTER);
	if (ret)
		return ERR_PTR(ret);

	ret = input_register_device(tp);
	if (ret)
		return ERR_PTR(ret);

	return tp;
}

int sinput_touchpad_init(struct sinput_device *sdev)
{
	struct input_dev *tp;

	if (!sdev->caps.touchpad || sdev->caps.touchpad_count == 0)
		return 0;

	if (sdev->caps.touchpad_count > 1) {
		/*
		 * N independent single-finger touchpads, one wire touch slot
		 * each. caps.touchpad_count is already clamped to
		 * SINPUT_MAX_TOUCHPADS by sinput_parse_features(), matching
		 * the two wire slots available.
		 */
		tp = sinput_touchpad_create(sdev, "SInput Touchpad 1", 1);
		if (IS_ERR(tp))
			return PTR_ERR(tp);
		sdev->touchpad[0] = tp;

		tp = sinput_touchpad_create(sdev, "SInput Touchpad 2", 1);
		if (IS_ERR(tp))
			return PTR_ERR(tp);
		sdev->touchpad[1] = tp;
	} else {
		/* One touchpad, up to both wire touch slots as its fingers. */
		int fingers = clamp_val(sdev->caps.touchpad_finger_count, 1,
					 SINPUT_MAX_TOUCHPADS);

		tp = sinput_touchpad_create(sdev, "SInput Touchpad", fingers);
		if (IS_ERR(tp))
			return PTR_ERR(tp);
		sdev->touchpad[0] = tp;
	}

	return 0;
}

static void sinput_touchpad_report_slot(struct input_dev *tp, int slot,
					 s16 x, s16 y, u16 pressure)
{
	input_mt_slot(tp, slot);
	input_mt_report_slot_state(tp, MT_TOOL_FINGER, pressure > 0);
	if (pressure > 0) {
		input_report_abs(tp, ABS_MT_POSITION_X, x);
		input_report_abs(tp, ABS_MT_POSITION_Y, y);
		input_report_abs(tp, ABS_MT_PRESSURE, pressure);
	}
}

/*
 * data is a full, validated SINPUT_REPORT_ID_STATE report -- same
 * precondition as sinput_input_report(), see its comment in sinput_input.c.
 *
 * The wire always carries exactly two touch slots (SI_TOUCH1_x and
 * SI_TOUCH2_x); what they mean depends on the shape sinput_touchpad_init()
 * registered:
 *
 *   - touchpad[1] set (N>1 independent touchpads): touch1 -> touchpad[0]'s
 *     only slot, touch2 -> touchpad[1]'s only slot. Each pad's own click
 *     bit (SINPUT_BTN_IDX_TOUCHPAD1/2) reports on that same pad's BTN_LEFT.
 *   - touchpad[1] unset (one touchpad, N fingers): touch1 -> slot 0, touch2
 *     -> slot 1 (only touched if caps.touchpad_finger_count > 1). Only
 *     TOUCHPAD1's click bit applies -- there is no second physical pad to
 *     have its own click.
 *
 * This mirrors SDL_hidapi_sinput.c's HIDAPI_DriverSInput_HandleStatePacket()
 * touchpad branch exactly (see its "touchpad > 0 || finger > 0" bump), and
 * BleGamepad::setTouchpad()'s pad-0-is-touch1/pad-1-is-touch2 wiring on the
 * emulator side confirms the same mapping from the sender.
 */
void sinput_touchpad_report(struct sinput_device *sdev, const u8 *data)
{
	u32 buttons;
	s16 x1, y1, x2, y2;
	u16 p1, p2;

	if (!sdev->touchpad[0])
		return;

	buttons = get_unaligned_le32(data + SI_BUTTONS_0);
	x1 = si_s16(data, SI_TOUCH1_X);
	y1 = si_s16(data, SI_TOUCH1_Y);
	p1 = get_unaligned_le16(data + SI_TOUCH1_P);
	x2 = si_s16(data, SI_TOUCH2_X);
	y2 = si_s16(data, SI_TOUCH2_Y);
	p2 = get_unaligned_le16(data + SI_TOUCH2_P);

	if (sdev->touchpad[1]) {
		sinput_touchpad_report_slot(sdev->touchpad[0], 0, x1, y1, p1);
		input_mt_sync_frame(sdev->touchpad[0]);
		input_report_key(sdev->touchpad[0], BTN_LEFT,
				  !!(buttons & BIT(SINPUT_BTN_IDX_TOUCHPAD1)));
		input_sync(sdev->touchpad[0]);

		sinput_touchpad_report_slot(sdev->touchpad[1], 0, x2, y2, p2);
		input_mt_sync_frame(sdev->touchpad[1]);
		input_report_key(sdev->touchpad[1], BTN_LEFT,
				  !!(buttons & BIT(SINPUT_BTN_IDX_TOUCHPAD2)));
		input_sync(sdev->touchpad[1]);
	} else {
		sinput_touchpad_report_slot(sdev->touchpad[0], 0, x1, y1, p1);
		if (sdev->caps.touchpad_finger_count > 1)
			sinput_touchpad_report_slot(sdev->touchpad[0], 1, x2, y2, p2);
		input_mt_sync_frame(sdev->touchpad[0]);
		input_report_key(sdev->touchpad[0], BTN_LEFT,
				  !!(buttons & BIT(SINPUT_BTN_IDX_TOUCHPAD1)));
		input_sync(sdev->touchpad[0]);
	}
}
