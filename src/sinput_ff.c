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
 * Unlike the LED classdevs in sinput_led.c, play_effect() runs from the
 * memless helper's own workqueue context rather than an arbitrary sysfs
 * writer, but the wire send still needs the same output_lock serialization
 * as every other output command -- see sinput_send_output_command()'s
 * comment in sinput_core.c. It is also subject to the same post-hid_hw_stop()
 * callback race as the LED classdevs; see sdev->removing's comment in
 * sinput.h.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/mutex.h>

#include "sinput.h"
#include "sinput_protocol.h"

static int sinput_play_effect(struct input_dev *in, void *data, struct ff_effect *effect)
{
	struct sinput_device *sdev = input_get_drvdata(in);
	u8 payload[SI_OUT_HAPTIC_RIGHT_BRAKE - SI_OUT_HAPTIC_TYPE + 1];
	int ret;

	if (effect->type != FF_RUMBLE)
		return 0;

	/*
	 * Linux's ff_rumble_effect convention: strong_magnitude is the
	 * large/low-frequency motor, weak_magnitude the small/high-frequency
	 * one. SDL's own low_frequency_rumble/high_frequency_rumble -> left/
	 * right mapping (HIDAPI_DriverSInput_RumbleJoystick()) matches this
	 * directly: strong -> left, weak -> right. SInput's amplitude is
	 * 8-bit; Linux's magnitude is 16-bit, so keep only the high byte,
	 * the same truncation SDL itself does. No brake control is exposed
	 * through Linux's FF_RUMBLE model, so always send brake=0.
	 */
	payload[SI_OUT_HAPTIC_TYPE - SI_OUT_HAPTIC_TYPE]        = SI_HAPTIC_TYPE_ERM;
	payload[SI_OUT_HAPTIC_LEFT_AMP - SI_OUT_HAPTIC_TYPE]    = effect->u.rumble.strong_magnitude >> 8;
	payload[SI_OUT_HAPTIC_LEFT_BRAKE - SI_OUT_HAPTIC_TYPE]  = 0;
	payload[SI_OUT_HAPTIC_RIGHT_AMP - SI_OUT_HAPTIC_TYPE]   = effect->u.rumble.weak_magnitude >> 8;
	payload[SI_OUT_HAPTIC_RIGHT_BRAKE - SI_OUT_HAPTIC_TYPE] = 0;

	mutex_lock(&sdev->output_lock);
	ret = sinput_send_output_command(sdev, SINPUT_CMD_HAPTIC, payload, sizeof(payload));
	mutex_unlock(&sdev->output_lock);

	return ret;
}

int sinput_ff_init(struct sinput_device *sdev, struct input_dev *in)
{
	int ret;

	if (!sdev->caps.rumble)
		return 0;

	input_set_capability(in, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(in, NULL, sinput_play_effect);
	if (ret)
		hid_info(sdev->hdev, "could not create FF device, rumble unavailable: %d\n", ret);

	/*
	 * Rumble is an optional enhancement, same reasoning as sinput_led_init()
	 * in probe(): a failure here should not cost the user their gamepad
	 * over a missing FF_RUMBLE device.
	 */
	return 0;
}
