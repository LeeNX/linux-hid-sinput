/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal shared types and cross-file entry points for the SInput driver's
 * per-subsystem source files (sinput_core.c, sinput_input.c,
 * sinput_battery.c, sinput_led.c). See sinput_protocol.h for the SInput
 * wire-format constants these subsystems decode/encode.
 */

#ifndef SINPUT_H
#define SINPUT_H

#include <linux/completion.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include <linux/types.h>

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
	 * sinput_battery_update() updating them from raw_event context -- so
	 * they need a real lock, not the "fixed after registration" trick
	 * used for input capabilities.
	 */
	spinlock_t battery_lock;
	u8 battery_capacity;     /* 0-100 */
	u8 battery_plug_status;  /* SI_PLUG_STATUS_* */

	struct power_supply_desc battery_desc;
	struct power_supply *battery;
};

/* sinput_input.c: gamepad buttons/axes and the IMU input device. */
int sinput_input_init(struct sinput_device *sdev);
int sinput_imu_init(struct sinput_device *sdev);
void sinput_input_report(struct sinput_device *sdev, const u8 *data);

/* sinput_battery.c: power_supply battery device. */
int sinput_battery_init(struct sinput_device *sdev);
void sinput_battery_update(struct sinput_device *sdev, const u8 *data);

#endif /* SINPUT_H */
