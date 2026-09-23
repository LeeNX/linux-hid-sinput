// SPDX-License-Identifier: GPL-2.0-only
/*
 * SInput force feedback (rumble) output command.
 *
 * Capability-gated on caps.rumble (an existing struct sinput_caps field,
 * never consulted by anything until now -- same story player_leds/rgb_led
 * had before sinput_led.c). Uses input_ff_create_memless() rather than a
 * custom FF_RUMBLE upload/erase implementation: SInput's HAPTIC command is
 * a direct, stateless "set both motors to this amplitude now" write with
 * nothing to upload an effect into on the device side, so the kernel's
 * generic memless helper -- which turns Linux's upload/play/timer effect
 * model into repeated play_effect() calls with the current combined
 * magnitude -- is the right fit, the same pattern several simple
 * rumble-only USB/HID gamepad drivers use (e.g. drivers/input/joystick/
 * xpad.c).
 *
 * sinput_play_effect() is called by drivers/input/ff-memless.c with
 * dev->event_lock held as a spinlock -- IRQs disabled, for the periodic
 * re-arm via ml_effect_timer(), a kernel timer/softirq callback, not a
 * workqueue as this file originally (wrongly) assumed. output_lock is a
 * mutex, and sinput_send_output_command() allocates with GFP_KERNEL and
 * can block inside hid_hw_output_report() -- none of that is safe from
 * that context. So play_effect() only records the latest requested
 * magnitudes (under sdev->ff_lock, a plain spinlock safe from either
 * context) and defers the actual wire send to sdev->ff_work, which runs
 * from process context where output_lock/GFP_KERNEL/blocking sends are
 * fine -- see sdev->ff_lock's comment in sinput.h.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "sinput.h"
#include "sinput_protocol.h"

static void sinput_ff_work(struct work_struct *work)
{
	struct sinput_device *sdev = container_of(work, struct sinput_device, ff_work);
	u8 payload[SI_OUT_HAPTIC_RIGHT_BRAKE - SI_OUT_HAPTIC_TYPE + 1];
	u16 strong, weak;
	unsigned long flags;

	spin_lock_irqsave(&sdev->ff_lock, flags);
	strong = sdev->ff_strong_magnitude;
	weak = sdev->ff_weak_magnitude;
	spin_unlock_irqrestore(&sdev->ff_lock, flags);

	/*
	 * See sinput_play_effect()'s comment for the strong/weak -> left/right
	 * mapping and the 16-bit -> 8-bit truncation, both taken from SDL's
	 * HIDAPI_DriverSInput_RumbleJoystick(). No brake control is exposed
	 * through Linux's FF_RUMBLE model, so always send brake=0.
	 */
	payload[SI_OUT_HAPTIC_TYPE - SI_OUT_HAPTIC_TYPE]        = SI_HAPTIC_TYPE_ERM;
	payload[SI_OUT_HAPTIC_LEFT_AMP - SI_OUT_HAPTIC_TYPE]    = strong >> 8;
	payload[SI_OUT_HAPTIC_LEFT_BRAKE - SI_OUT_HAPTIC_TYPE]  = 0;
	payload[SI_OUT_HAPTIC_RIGHT_AMP - SI_OUT_HAPTIC_TYPE]   = weak >> 8;
	payload[SI_OUT_HAPTIC_RIGHT_BRAKE - SI_OUT_HAPTIC_TYPE] = 0;

	mutex_lock(&sdev->output_lock);
	sinput_send_output_command(sdev, SINPUT_CMD_HAPTIC, payload, sizeof(payload));
	mutex_unlock(&sdev->output_lock);
}

static int sinput_play_effect(struct input_dev *in, void *data, struct ff_effect *effect)
{
	struct sinput_device *sdev = input_get_drvdata(in);
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	/*
	 * Linux's ff_rumble_effect convention: strong_magnitude is the
	 * large/low-frequency motor, weak_magnitude the small/high-frequency
	 * one. SDL's own low_frequency_rumble/high_frequency_rumble -> left/
	 * right mapping (HIDAPI_DriverSInput_RumbleJoystick()) matches this
	 * directly: strong -> left, weak -> right.
	 */
	spin_lock_irqsave(&sdev->ff_lock, flags);
	sdev->ff_strong_magnitude = effect->u.rumble.strong_magnitude;
	sdev->ff_weak_magnitude = effect->u.rumble.weak_magnitude;
	spin_unlock_irqrestore(&sdev->ff_lock, flags);

	/*
	 * Safe to call from this context (unlike everything ff_work itself
	 * does): schedule_work() never sleeps and does its own locking.
	 * Coalesces naturally if a previous request hasn't run yet -- the
	 * work item's own read of sdev->ff_*_magnitude above always picks up
	 * the latest values, so a still-queued run isn't a second, stale
	 * send waiting to happen.
	 */
	schedule_work(&sdev->ff_work);

	return 0;
}

int sinput_ff_init(struct sinput_device *sdev, struct input_dev *in)
{
	int ret;

	spin_lock_init(&sdev->ff_lock);
	INIT_WORK(&sdev->ff_work, sinput_ff_work);

	if (!sdev->caps.rumble)
		return 0;

	input_set_capability(in, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(in, NULL, sinput_play_effect);
	if (ret) {
		hid_info(sdev->hdev, "could not create FF device, rumble unavailable: %d\n", ret);
		/*
		 * input_set_capability() above already set EV_FF/FF_RUMBLE in
		 * in->evbit/in->ffbit, but in->ff is still NULL since creation
		 * failed. Left as-is, input_ff_upload() (EVIOCSFF) would pass
		 * its capability checks and then dereference dev->ff->ffbit --
		 * a NULL pointer deref reachable from userspace on the very
		 * first EVIOCSFF call. Clear both bits so the device stops
		 * advertising a capability it doesn't actually have, same
		 * "optional enhancement, must not look present when it isn't"
		 * policy this function already applies by not failing probe.
		 */
		__clear_bit(FF_RUMBLE, in->ffbit);
		__clear_bit(EV_FF, in->evbit);
	}

	/*
	 * Rumble is an optional enhancement, same reasoning as sinput_led_init()
	 * in probe(): a failure here should not cost the user their gamepad
	 * over a missing FF_RUMBLE device.
	 */
	return 0;
}
