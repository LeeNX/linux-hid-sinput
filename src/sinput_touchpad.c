// SPDX-License-Identifier: GPL-2.0-only
/*
 * SInput touchpad input device(s).
 *
 * Unlike every other capability in this driver, the touchpad's registered
 * shape (one input_dev with N MT slots, vs N independent single-slot
 * input_devs) can change *after* initial registration: sdev->caps is
 * mutated in place by any later FEATURES response (see struct sinput_caps's
 * comment in sinput.h), and a response landing after sinput_probe()'s
 * retry loop gave up can still report a different touchpad_count/
 * touchpad_finger_count than the fallback assumed. Every other capability
 * in this driver accepts that a late response just gets ignored for
 * registration purposes (documented, deliberate). Touchpad does not: a
 * late response changing shape from "one pad, two fingers" to "two
 * independent pads" (or back) is reconciled by tearing down the
 * no-longer-correct registration and building the new one, via
 * sinput_touchpad_reinit_work -- not by a synchronous call, since
 * raw_event (which is what notices the change, in sinput_core.c's
 * sinput_parse_features()) can run in atomic context on some transports
 * (USB), and building/registering an input_dev sleeps.
 *
 * sdev->touchpad_lock protects the three pieces of state a reconcile pass
 * and sinput_touchpad_report() both need to agree on: sdev->touchpad[0/1],
 * sdev->touchpad_slots, and (only while sinput_core.c is updating them)
 * caps->touchpad/touchpad_count/touchpad_finger_count. It's a plain
 * spinlock, not a mutex, and every acquisition uses the _irqsave form:
 * sinput_touchpad_report() is called from raw_event, which this project
 * has already found can run with interrupts disabled on some transports
 * (see sinput_ff.c's header comment for the same finding applied to FF).
 * sinput_touchpad_report() holds the lock across its input_report_abs()/
 * input_sync() calls too, not just around reading the pointers -- those
 * calls never sleep (input core's own dev->event_lock is itself a
 * spinlock), and holding the lock for the *entire* use of a touchpad
 * input_dev, not just the pointer read, is what makes it safe for the
 * reconcile work to unregister+free the old one afterwards: any report()
 * call that observed the old pointer must have already finished using it
 * by the time it releases the lock, and the reconcile work's swap can't
 * proceed until that same lock is free.
 *
 * Building/registering a new shape (sleeps: GFP_KERNEL allocations,
 * input_register_device()'s internal locking) always happens *outside*
 * the lock, before the swap. Unregistering the old shape (also sleeps)
 * always happens *outside* the lock too, but *after* the swap -- by then
 * nothing can still be using the old pointers per the paragraph above.
 *
 * Because touchpad input_devs can be replaced more than once during the
 * parent HID device's lifetime, they are deliberately NOT
 * devm_input_allocate_device()'d the way every other input_dev in this
 * driver is: devm's automatic cleanup assumes one allocate-register-done
 * lifecycle tied to the parent's own teardown, not repeated
 * register/unregister cycles within it. sinput_touchpad_remove() unregisters
 * whatever is currently registered explicitly, from sinput_remove().
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/workqueue.h>

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
 * see sinput_touchpad_report_slot()'s presence test below for why 0 means
 * "no finger" rather than "zero pressure but still touching".
 *
 * Plain input_allocate_device(), not the devm_ variant -- see this file's
 * top comment for why. Caller owns the returned device on success (must
 * eventually input_unregister_device() it) and on register failure (this
 * function frees it itself before returning the error).
 */
static struct input_dev *sinput_touchpad_create(struct sinput_device *sdev,
						  const char *name, int slots)
{
	struct input_dev *tp;
	int ret;

	tp = input_allocate_device();
	if (!tp)
		return ERR_PTR(-ENOMEM);

	tp->name = name;
	tp->phys = sdev->hdev->phys;
	tp->dev.parent = &sdev->hdev->dev;
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
	/*
	 * SI_TOUCH*_P (sinput_protocol.h) is decoded as a full u16, so the
	 * wire can carry values up to 65535, not 32767 -- advertising 32767
	 * here would let a real value above it reach userspace outside the
	 * axis's own declared range (CodeRabbit). SDL_hidapi_sinput.c's own
	 * `touch1P / 32768.0f` normalization treats 32768 as only a nominal
	 * full-scale reference for its own float scaling, not a hard ceiling
	 * on the wire type -- it doesn't clamp the raw value either.
	 */
	input_set_abs_params(tp, ABS_MT_PRESSURE, 0, 65535, 0, 0);

	ret = input_mt_init_slots(tp, slots, INPUT_MT_POINTER);
	if (ret) {
		input_free_device(tp);
		return ERR_PTR(ret);
	}

	ret = input_register_device(tp);
	if (ret) {
		input_free_device(tp);
		return ERR_PTR(ret);
	}

	return tp;
}

/* A fully-built (or deliberately empty, for "no touchpad") target shape,
 * built outside sdev->touchpad_lock -- see this file's top comment.
 */
struct sinput_touchpad_shape {
	struct input_dev *pad[SINPUT_MAX_TOUCHPADS];
	u8 slots; /* meaningful only when pad[1] is NULL */
};

/*
 * touchpad/count/fingers are a snapshot taken under sdev->touchpad_lock by
 * the caller, not read live from sdev->caps here -- caps has no lock of
 * its own outside the specific fields sinput_core.c's sinput_parse_features()
 * updates under this same lock, and re-reading it mid-build could mix an
 * old and a new FEATURES response's values if another one raced in.
 */
static int sinput_touchpad_build(struct sinput_device *sdev, bool touchpad,
				  u8 count, u8 fingers,
				  struct sinput_touchpad_shape *out)
{
	memset(out, 0, sizeof(*out));

	if (!touchpad || count == 0)
		return 0;

	if (count > 1) {
		/*
		 * N independent single-finger touchpads, one wire touch slot
		 * each. count is already clamped to SINPUT_MAX_TOUCHPADS by
		 * sinput_parse_features(), matching the two wire slots
		 * available.
		 */
		out->pad[0] = sinput_touchpad_create(sdev, "SInput Touchpad 1", 1);
		if (IS_ERR(out->pad[0])) {
			int ret = PTR_ERR(out->pad[0]);

			out->pad[0] = NULL;
			return ret;
		}

		out->pad[1] = sinput_touchpad_create(sdev, "SInput Touchpad 2", 1);
		if (IS_ERR(out->pad[1])) {
			int ret = PTR_ERR(out->pad[1]);

			input_unregister_device(out->pad[0]);
			out->pad[0] = NULL;
			out->pad[1] = NULL;
			return ret;
		}
		out->slots = 1;
	} else {
		/* One touchpad, up to both wire touch slots as its fingers. */
		int f = clamp_val(fingers, 1, SINPUT_MAX_TOUCHPADS);

		out->pad[0] = sinput_touchpad_create(sdev, "SInput Touchpad", f);
		if (IS_ERR(out->pad[0])) {
			int ret = PTR_ERR(out->pad[0]);

			out->pad[0] = NULL;
			return ret;
		}
		out->slots = f;
	}

	return 0;
}

/* Caller holds sdev->touchpad_lock. */
static bool sinput_touchpad_shape_matches(struct sinput_device *sdev,
					   bool touchpad, u8 count, u8 fingers)
{
	if (!touchpad || count == 0)
		return sdev->touchpad[0] == NULL;

	if (count > 1)
		return sdev->touchpad[0] != NULL && sdev->touchpad[1] != NULL;

	if (!sdev->touchpad[0] || sdev->touchpad[1])
		return false;

	return sdev->touchpad_slots == clamp_val(fingers, 1, SINPUT_MAX_TOUCHPADS);
}

static void sinput_touchpad_reinit_work_fn(struct work_struct *work)
{
	struct sinput_device *sdev = container_of(work, struct sinput_device,
						   touchpad_reinit_work);
	unsigned long flags;
	bool touchpad, need_change;
	u8 count, fingers;
	struct sinput_touchpad_shape new_shape;
	struct input_dev *old0, *old1;
	int ret;

	spin_lock_irqsave(&sdev->touchpad_lock, flags);
	touchpad = sdev->caps.touchpad;
	count = sdev->caps.touchpad_count;
	fingers = sdev->caps.touchpad_finger_count;
	need_change = !sinput_touchpad_shape_matches(sdev, touchpad, count, fingers);
	spin_unlock_irqrestore(&sdev->touchpad_lock, flags);

	if (!need_change)
		return;

	/*
	 * Best-effort only: sdev->removing is normally read under
	 * output_lock (see its own comment in sinput.h), but this is a plain
	 * bool and the narrow window this closes -- a FEATURES response
	 * racing in between sinput_touchpad_remove()'s cancel_work_sync()
	 * and hid_hw_stop() actually quiescing raw_event -- only risks
	 * leaking one orphaned, never-torn-down input_dev, not a
	 * use-after-free. Not worth taking a second lock domain over.
	 */
	if (sdev->removing)
		return;

	ret = sinput_touchpad_build(sdev, touchpad, count, fingers, &new_shape);
	if (ret) {
		hid_info(sdev->hdev, "SInput touchpad (re)registration failed: %d\n", ret);
		return;
	}

	spin_lock_irqsave(&sdev->touchpad_lock, flags);
	old0 = sdev->touchpad[0];
	old1 = sdev->touchpad[1];
	sdev->touchpad[0] = new_shape.pad[0];
	sdev->touchpad[1] = new_shape.pad[1];
	sdev->touchpad_slots = new_shape.slots;
	spin_unlock_irqrestore(&sdev->touchpad_lock, flags);

	if (old1)
		input_unregister_device(old1);
	if (old0)
		input_unregister_device(old0);
}

void sinput_touchpad_early_init(struct sinput_device *sdev)
{
	spin_lock_init(&sdev->touchpad_lock);
	INIT_WORK(&sdev->touchpad_reinit_work, sinput_touchpad_reinit_work_fn);
}

/* Called from sinput_core.c's sinput_parse_features(), i.e. from
 * raw_event -- must never sleep, so this only schedules the work above.
 */
void sinput_touchpad_request_reconcile(struct sinput_device *sdev)
{
	schedule_work(&sdev->touchpad_reinit_work);
}

/* Called once from sinput_probe(), plain process context: make sure the
 * shape matching whatever caps looks like right now is built and
 * registered before probe() returns, the same way sinput_imu_init() et al.
 * are synchronous. Reuses the same work item every other reconcile does,
 * rather than calling the build logic directly, so there is never more
 * than one path that can be registering/unregistering these devices at a
 * time (a raw_event during sinput_probe()'s own FEATURES retry wait can
 * already race a schedule_work() in ahead of this call; flush_work() here
 * simply waits for whichever one actually runs).
 */
void sinput_touchpad_reconcile_and_wait(struct sinput_device *sdev)
{
	schedule_work(&sdev->touchpad_reinit_work);
	flush_work(&sdev->touchpad_reinit_work);
}

void sinput_touchpad_remove(struct sinput_device *sdev)
{
	unsigned long flags;
	struct input_dev *tp0, *tp1;

	cancel_work_sync(&sdev->touchpad_reinit_work);

	spin_lock_irqsave(&sdev->touchpad_lock, flags);
	tp0 = sdev->touchpad[0];
	tp1 = sdev->touchpad[1];
	sdev->touchpad[0] = NULL;
	sdev->touchpad[1] = NULL;
	spin_unlock_irqrestore(&sdev->touchpad_lock, flags);

	if (tp1)
		input_unregister_device(tp1);
	if (tp0)
		input_unregister_device(tp0);
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
 * SI_TOUCH2_x); what they mean depends on the shape currently registered
 * (touchpad[]/touchpad_slots, both read under touchpad_lock -- see this
 * file's top comment for why this can legitimately change mid-life and
 * why the lock must stay held across the report/sync calls below, not
 * just the read):
 *
 *   - touchpad[1] set (N>1 independent touchpads): touch1 -> touchpad[0]'s
 *     only slot, touch2 -> touchpad[1]'s only slot. Each pad's own click
 *     bit (SINPUT_BTN_IDX_TOUCHPAD1/2) reports on that same pad's BTN_LEFT.
 *   - touchpad[1] unset (one touchpad, N fingers): touch1 -> slot 0, touch2
 *     -> slot 1 (only touched if touchpad_slots > 1). Only TOUCHPAD1's
 *     click bit applies -- there is no second physical pad to have its
 *     own click.
 *
 * This mirrors SDL_hidapi_sinput.c's HIDAPI_DriverSInput_HandleStatePacket()
 * touchpad branch exactly (see its "touchpad > 0 || finger > 0" bump), and
 * BleGamepad::setTouchpad()'s pad-0-is-touch1/pad-1-is-touch2 wiring on the
 * emulator side confirms the same mapping from the sender.
 */
void sinput_touchpad_report(struct sinput_device *sdev, const u8 *data)
{
	unsigned long flags;
	struct input_dev *tp0, *tp1;
	u8 slots;
	u32 buttons;
	s16 x1, y1, x2, y2;
	u16 p1, p2;

	spin_lock_irqsave(&sdev->touchpad_lock, flags);

	tp0 = sdev->touchpad[0];
	if (!tp0) {
		spin_unlock_irqrestore(&sdev->touchpad_lock, flags);
		return;
	}
	tp1 = sdev->touchpad[1];
	slots = sdev->touchpad_slots;

	buttons = get_unaligned_le32(data + SI_BUTTONS_0);
	x1 = si_s16(data, SI_TOUCH1_X);
	y1 = si_s16(data, SI_TOUCH1_Y);
	p1 = get_unaligned_le16(data + SI_TOUCH1_P);
	x2 = si_s16(data, SI_TOUCH2_X);
	y2 = si_s16(data, SI_TOUCH2_Y);
	p2 = get_unaligned_le16(data + SI_TOUCH2_P);

	if (tp1) {
		sinput_touchpad_report_slot(tp0, 0, x1, y1, p1);
		input_mt_sync_frame(tp0);
		input_report_key(tp0, BTN_LEFT,
				  !!(buttons & BIT(SINPUT_BTN_IDX_TOUCHPAD1)));
		input_sync(tp0);

		sinput_touchpad_report_slot(tp1, 0, x2, y2, p2);
		input_mt_sync_frame(tp1);
		input_report_key(tp1, BTN_LEFT,
				  !!(buttons & BIT(SINPUT_BTN_IDX_TOUCHPAD2)));
		input_sync(tp1);
	} else {
		sinput_touchpad_report_slot(tp0, 0, x1, y1, p1);
		if (slots > 1)
			sinput_touchpad_report_slot(tp0, 1, x2, y2, p2);
		input_mt_sync_frame(tp0);
		input_report_key(tp0, BTN_LEFT,
				  !!(buttons & BIT(SINPUT_BTN_IDX_TOUCHPAD1)));
		input_sync(tp0);
	}

	spin_unlock_irqrestore(&sdev->touchpad_lock, flags);
}
