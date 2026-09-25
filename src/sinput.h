/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal shared types and cross-file entry points for the SInput driver's
 * per-subsystem source files (sinput_core.c, sinput_input.c,
 * sinput_battery.c, sinput_led.c, sinput_ff.c). See sinput_protocol.h for
 * the SInput wire-format constants these subsystems decode/encode.
 */

#ifndef SINPUT_H
#define SINPUT_H

#include <linux/completion.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

/* Universal 4-player convention (Xbox/PS/Switch); no SInput spec says a fixed N. */
#define SINPUT_NUM_PLAYER_LEDS 4

/*
 * SDL_hidapi_sinput.c's SINPUT_MAX_ALLOWED_TOUCHPADS: the wire only ever
 * carries two touch slots (SI_TOUCH1_x and SI_TOUCH2_x in
 * sinput_protocol.h), so two is a hard ceiling, not a policy choice.
 */
#define SINPUT_MAX_TOUCHPADS 2

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
 * instead. See sinput_input_report() in sinput_input.c.
 */
struct sinput_caps {
	bool valid;
	u16 protocol_version;
	u16 polling_rate_us;
	bool rumble;
	bool player_leds;
	bool accel;
	bool gyro;
	/*
	 * Full-scale range the device reports for its accelerometer (+/- g)
	 * and gyroscope (+/- degrees/second) -- SI_FEAT_ACCEL_RANGE/
	 * SI_FEAT_GYRO_RANGE in the FEATURES response, e.g. accel_range=8
	 * means the raw -32768..32767 axis spans -8g..+8g. Zero (the
	 * zero-initialized default) means "unknown": no real FEATURES
	 * response has told us a range, so sinput_imu_init() reports plain
	 * unscaled axes instead of fabricating a resolution/INPUT_PROP_
	 * ACCELEROMETER claim it can't back up. See SDL_hidapi_sinput.c's
	 * CalculateAccelScale()/CalculateGyroScale() for the reference this
	 * mirrors, and struct input_absinfo's doc comment in
	 * include/uapi/linux/input.h for what INPUT_PROP_ACCELEROMETER
	 * changes resolution units to.
	 */
	u16 accel_range;
	u16 gyro_range;
	bool left_stick;
	bool right_stick;
	bool left_trigger;
	bool right_trigger;
	/*
	 * touchpad is the SI_FLAG1_TOUCHPAD capability bit; touchpad_count/
	 * touchpad_finger_count come from SI_FEAT_TOUCHPAD_COUNT/FINGERS and
	 * decide the *shape* of what gets registered (sinput_touchpad_init()
	 * in sinput_touchpad.c): touchpad_count>1 means N independent
	 * single-finger touchpads (one input_dev each), touchpad_count==1
	 * means one touchpad with touchpad_finger_count fingers (one input_dev,
	 * multiple MT slots) -- mirrors SDL_hidapi_sinput.c's own
	 * HIDAPI_DriverSInput_UpdateDevice() clamp/branch on these same two
	 * values. Zero touchpad_count with touchpad true would mean "capable
	 * but the response didn't say how many" -- sinput_touchpad_init()
	 * treats that as "none" rather than guessing a shape.
	 */
	bool touchpad;
	u8 touchpad_count;
	u8 touchpad_finger_count;
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
	/*
	 * touchpad[0] is always the first (or only) touchpad if any is
	 * registered; touchpad[1] only exists when caps.touchpad_count > 1.
	 * See sinput_touchpad_report()'s comment in sinput_touchpad.c for how
	 * the two wire touch slots map onto these.
	 */
	struct input_dev *touchpad[SINPUT_MAX_TOUCHPADS];
	struct sinput_caps caps;
	struct completion caps_done;

	/*
	 * Battery/charge state. Unlike sdev->caps (only read at registration
	 * time, see the comment above struct sinput_caps), these are read
	 * throughout the device's life by sinput_battery_get_property() --
	 * called from arbitrary process context via sysfs, concurrently with
	 * sinput_battery_update() updating them from raw_event context -- so
	 * they need a real lock, not the "fixed after registration" trick
	 * used for input capabilities.
	 */
	spinlock_t battery_lock;
	u8 battery_capacity;     /* 0-100 */
	u8 battery_plug_status;  /* SI_PLUG_STATUS_* */

	struct power_supply_desc battery_desc;
	struct power_supply *battery;

	/*
	 * Serializes every hid_hw_output_report() call (FEATURES request,
	 * player LED, RGB LED). Callers hold this around their whole
	 * state-update-plus-send critical section, not just around the send:
	 * an LED's brightness_set_blocking can run from any process context,
	 * so two different player LEDs' callbacks racing each other while
	 * updating the shared player_leds_state bitmask below (and then each
	 * sending what they computed) could otherwise reorder and leave the
	 * device showing a stale value with nothing to ever correct it. See
	 * sinput_send_output_command()'s comment in sinput_core.c.
	 */
	struct mutex output_lock;

	/*
	 * Set (under output_lock) before hid_hw_stop() is called, on every
	 * path that calls it. devm doesn't unregister the LED classdevs (or
	 * the FF-capable gamepad input_dev) until after sinput_remove()
	 * returns, and both led_classdev_unregister() (via flush_work()) and
	 * input_unregister_device()'s FF teardown can still drive one more
	 * brightness_set_blocking call, or schedule one more ff_work run via
	 * play_effect(), at that point -- i.e. our own output-command sends
	 * can still happen after hid_hw_stop() already ran.
	 * sinput_send_output_command() checks this and bails out instead of
	 * calling into an already-stopped HID transport. sinput_remove()
	 * also cancel_work_sync()s ff_work itself, both to avoid leaking a
	 * queued item and to close the window before this flag is even
	 * checked; this comment's residual "can still fire after hid_hw_stop()"
	 * case is deliberately not chased further than that, same risk
	 * posture as the LED classdevs already accept.
	 */
	bool removing;

	/* Player LEDs: N on/off led_classdevs, translated to a single wire
	 * number (see SI_OUT_PLAYER_LED_NUM in sinput_protocol.h). Bit i of
	 * player_leds_state is player_leds[i]'s on/off state.
	 */
	struct led_classdev player_leds[SINPUT_NUM_PLAYER_LEDS];
	u8 player_leds_state;

	/* RGB indicator LED. */
	struct led_classdev_mc rgb_led;

	/*
	 * Force feedback. sinput_play_effect() (sinput_ff.c) is invoked by
	 * the kernel's ff-memless helper with dev->event_lock held as a
	 * spinlock -- with IRQs disabled, in the case of ml_effect_timer()'s
	 * periodic re-arm -- never from a sleepable context, despite the
	 * "workqueue context" this driver originally assumed (wrong; caught
	 * by real review, see docs/research.md). output_lock is a mutex and
	 * sinput_send_output_command() allocates with GFP_KERNEL and can
	 * block in hid_hw_output_report(), none of which is safe there. So
	 * play_effect() only records the latest requested magnitudes here
	 * (under ff_lock, a plain spinlock safe to take from either context)
	 * and schedules ff_work to do the real send from process context.
	 */
	spinlock_t ff_lock;
	u16 ff_strong_magnitude;
	u16 ff_weak_magnitude;
	struct work_struct ff_work;
};

/* sinput_core.c */
int sinput_send_output_command(struct sinput_device *sdev, u8 cmd,
				const u8 *payload, size_t payload_len);

/* sinput_input.c: gamepad buttons/axes and the IMU input device. */
int sinput_input_init(struct sinput_device *sdev);
int sinput_imu_init(struct sinput_device *sdev);
void sinput_input_report(struct sinput_device *sdev, const u8 *data);

/*
 * sinput_touchpad.c: touchpad input device(s), capability-gated on
 * caps.touchpad. Optional like the LEDs/rumble -- a registration failure is
 * logged and swallowed by the caller, not fatal to probe().
 */
int sinput_touchpad_init(struct sinput_device *sdev);
void sinput_touchpad_report(struct sinput_device *sdev, const u8 *data);

/* sinput_battery.c: power_supply battery device. */
int sinput_battery_init(struct sinput_device *sdev);
void sinput_battery_update(struct sinput_device *sdev, const u8 *data);

/* sinput_led.c: player LED + RGB LED, both capability-gated. */
int sinput_led_init(struct sinput_device *sdev);

/*
 * sinput_ff.c: force feedback (rumble), capability-gated on caps.rumble.
 * Must be called on the gamepad input_dev before input_register_device(),
 * from sinput_input_init() -- see sinput_ff.c's header comment. Failure is
 * logged and swallowed (rumble is an optional enhancement, same as the
 * LEDs), so this never fails its caller.
 */
int sinput_ff_init(struct sinput_device *sdev, struct input_dev *in);

#endif /* SINPUT_H */
