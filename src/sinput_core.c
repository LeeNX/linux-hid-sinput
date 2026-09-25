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

/*
 * probe() sends the FEATURES request up to this many times, waiting
 * SINPUT_FEATURES_RETRY_MS between each, before giving up and falling back
 * to "assume everything present" (see the defaults block below).
 *
 * This used to be a single request plus one longer wait (200ms, then
 * 3000ms after that was found not long enough -- see docs/research.md,
 * 2026-09-24). Neither fixed it, because the real problem was never how
 * long we waited: root-caused via bluetoothd debug logging
 * (profiles/input/hog-lib.c) on rp4b-ble-hil that a FEATURES response can
 * arrive on the wire (confirmed via real BLE capture) and still never
 * reach raw_event(), because bluetoothd only starts actually listening for
 * notifications on a given Report characteristic once its own internal
 * CCC-write-confirm chain completes for that specific characteristic
 * (report_ccc_written_cb() -> g_attrib_register(), itself gated on a
 * read-CCC + write-CCC round trip run independently per report,
 * interleaved with unrelated GATT discovery for other services). A
 * response landing before that chain finishes for this report is silently
 * dropped by bluetoothd's own ATT dispatcher -- no error path exists for
 * it, and no amount of client-side waiting helps once it's already gone.
 *
 * A single fixed pre-request delay (tried 2000ms) didn't reliably clear
 * this either, because that internal timing is genuinely variable, not a
 * fixed offset. Retrying instead of guessing one delay works with that
 * variability rather than against it: each resend gives the response a
 * fresh chance to land after bluetoothd's internal state has caught up,
 * whenever that actually happens to be, instead of betting everything on
 * one exact guess.
 */
#define SINPUT_FEATURES_RETRY_COUNT 5
#define SINPUT_FEATURES_RETRY_MS    1000

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
	caps->accel_range   = get_unaligned_le16(data + SI_FEAT_ACCEL_RANGE);
	caps->gyro_range    = get_unaligned_le16(data + SI_FEAT_GYRO_RANGE);
	caps->left_stick    = !!(flags0 & SI_FLAG0_LEFT_STICK);
	caps->right_stick   = !!(flags0 & SI_FLAG0_RIGHT_STICK);
	caps->left_trigger  = !!(flags0 & SI_FLAG0_LEFT_TRIGGER);
	caps->right_trigger = !!(flags0 & SI_FLAG0_RIGHT_TRIGGER);

	caps->rgb_led  = !!(flags1 & SI_FLAG1_RGB_LED);

	/*
	 * Unlike every other field here, touchpad/touchpad_count/
	 * touchpad_finger_count are written under touchpad_lock: a late
	 * response changing these three *does* change what gets registered
	 * (sinput_touchpad_request_reconcile() below), unlike every other
	 * capability, where a late response is simply ignored for
	 * registration purposes (see struct sinput_caps's comment in
	 * sinput.h). sinput_touchpad_report() and the reconcile work both
	 * read these same three fields under the same lock, so this write
	 * must not race a read that mixes an old and a new response's values.
	 *
	 * Mirrors SDL_hidapi_sinput.c's own clamp exactly: touchpad_count is
	 * clamped to [1, SINPUT_MAX_TOUCHPADS] whenever the device claims
	 * touchpad support at all (a device that sets the flag bit but sends
	 * count=0 still gets one touchpad, not zero -- SDL does the same),
	 * and touchpad_count > 1 forces finger_count to 1 since each of the
	 * two wire touch slots is then a separate pad's only finger, not two
	 * fingers on one pad. See caps->touchpad_count's comment in sinput.h.
	 */
	{
		unsigned long flags;

		spin_lock_irqsave(&sdev->touchpad_lock, flags);
		caps->touchpad = !!(flags1 & SI_FLAG1_TOUCHPAD);
		if (caps->touchpad) {
			caps->touchpad_count = clamp_val(data[SI_FEAT_TOUCHPAD_COUNT], 1,
							  SINPUT_MAX_TOUCHPADS);
			if (caps->touchpad_count > 1)
				caps->touchpad_finger_count = 1;
			else
				caps->touchpad_finger_count = clamp_val(
					data[SI_FEAT_TOUCHPAD_FINGERS], 1, SINPUT_MAX_TOUCHPADS);
		} else {
			caps->touchpad_count = 0;
			caps->touchpad_finger_count = 0;
		}
		spin_unlock_irqrestore(&sdev->touchpad_lock, flags);
	}

	caps->button_mask = get_unaligned_le32(data + SI_FEAT_USAGE_MASK_0);

	caps->valid = true;

	hid_info(sdev->hdev,
		 "SInput protocol v%u, poll rate %u us, sticks=%d/%d triggers=%d/%d accel=%d (+/-%ug) gyro=%d (+/-%u dps) touchpad=%d (pads=%u fingers=%u) buttons=0x%08x\n",
		 caps->protocol_version, caps->polling_rate_us,
		 caps->left_stick, caps->right_stick,
		 caps->left_trigger, caps->right_trigger,
		 caps->accel, caps->accel_range, caps->gyro, caps->gyro_range,
		 caps->touchpad, caps->touchpad_count, caps->touchpad_finger_count,
		 caps->button_mask);

	/*
	 * Unlike every other capability parsed above, touchpad registration
	 * must stay in sync with this response for the device's whole life,
	 * not just at first registration -- schedules sinput_touchpad.c's
	 * reconcile work, never runs it inline (this function is called from
	 * raw_event, which cannot sleep). A no-op if the registered shape
	 * already matches.
	 */
	sinput_touchpad_request_reconcile(sdev);
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

	/*
	 * hid_hw_stop() may already have run (see sdev->removing's comment
	 * in sinput.h): an LED's brightness_set_blocking driven by
	 * led_classdev_unregister()'s own flush_work() during devm teardown
	 * can still reach here after that. Don't touch the stopped transport.
	 */
	if (sdev->removing)
		return -ENODEV;

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

struct sinput_report_expect {
	enum hid_report_type type;
	unsigned int id;
	unsigned int size; /* full wire size in bytes, report ID byte included */
	const char *name;
};

static const struct sinput_report_expect sinput_expected_reports[] = {
	{ HID_INPUT_REPORT,  SINPUT_REPORT_ID_STATE,  SINPUT_INPUT_REPORT_SIZE,  "state" },
	{ HID_INPUT_REPORT,  SINPUT_REPORT_ID_CMD,    SINPUT_INPUT_REPORT_SIZE,  "command response" },
	{ HID_OUTPUT_REPORT, SINPUT_REPORT_ID_OUTPUT, SINPUT_OUTPUT_REPORT_SIZE, "output command" },
};

/*
 * Every byte offset in sinput_protocol.h is reverse-derived from SDL's
 * SInput HIDAPI driver, not from this project ever having decoded a real
 * SInput report descriptor -- see that header's own top comment. hid_parse()
 * (called just before this, in sinput_probe()) already did that decoding for
 * us; this cross-checks its result against what every offset in this driver
 * assumes, purely as diagnostic logging. Never fatal: an out-of-tree driver
 * whose whole reason to exist is testing those assumptions against real
 * hardware should surface a mismatch, not refuse to load over one -- the
 * existing HIL-verified byte-exact button/axis/LED/rumble traffic (see
 * docs/research.md) already proves the offsets work in practice even if a
 * given device's descriptor phrases the sizes unexpectedly.
 */
static void sinput_verify_report_sizes(struct hid_device *hdev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sinput_expected_reports); i++) {
		const struct sinput_report_expect *exp = &sinput_expected_reports[i];
		struct hid_report_enum *renum = &hdev->report_enum[exp->type];
		struct hid_report *report = renum->report_id_hash[exp->id];
		unsigned int descriptor_bytes;

		if (!report) {
			hid_info(hdev,
				 "report descriptor has no report id %u for the assumed %s report (type %d) -- sinput_protocol.h's offsets are unverified for this device\n",
				 exp->id, exp->name, exp->type);
			continue;
		}

		/*
		 * hid_report_len() rounds report->size (the descriptor's data
		 * field width, in bits) up to a whole byte -- report->size / 8
		 * would silently truncate a report whose fields don't land on
		 * a byte boundary, e.g. 505 bits reporting as 64 bytes instead
		 * of the real 65 (CodeRabbit caught this) -- and adds back the
		 * report ID byte the descriptor itself excludes (the transport
		 * prepends it separately on the wire; it is not a HID field).
		 * sinput_protocol.h's *_REPORT_SIZE constants count the report
		 * ID as byte 0 (see e.g. SI_PLUG_STATUS's comment), so this
		 * now compares like for like without hand-rolling the same
		 * arithmetic the kernel already provides.
		 */
		descriptor_bytes = hid_report_len(report);

		if (descriptor_bytes != exp->size)
			hid_warn(hdev,
				 "report id %u (%s): descriptor says %u bytes (numbered=%u), sinput_protocol.h assumes %u -- byte offsets may be wrong for this device\n",
				 exp->id, exp->name, descriptor_bytes, renum->numbered, exp->size);
		else
			hid_info(hdev, "report id %u (%s): descriptor size matches assumed %u bytes\n",
				 exp->id, exp->name, exp->size);
	}
}

static int sinput_probe(struct hid_device *hdev,
			const struct hid_device_id *id)
{
	struct sinput_device *sdev;
	int ret;
	int i;

	sdev = devm_kzalloc(&hdev->dev, sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	sdev->hdev = hdev;
	hid_set_drvdata(hdev, sdev);
	init_completion(&sdev->caps_done);

	/*
	 * Until a feature response says otherwise, assume every axis, the
	 * IMU, and every output command (LEDs, rumble) are present. This
	 * preserves today's behaviour for devices that do not implement the
	 * SInput command/feature protocol, such as the generic bring-up test
	 * ID -- and, in practice, this HIL rig's own ESP32-BLE-Gamepad
	 * emulator, which usually never answers FEATURES either (see
	 * docs/research.md). Harmless for LEDs/rumble even if wrong: worst
	 * case is an output command userspace can send that a real device
	 * without that feature silently ignores, unlike assuming an axis
	 * that isn't really there and misreading garbage.
	 *
	 * caps.rumble was missing from this block when sinput_ff.c was first
	 * added -- every other output-command capability (player_leds,
	 * rgb_led) had already been caught missing here once before (see
	 * docs/research.md, 2026-09-22) and fixed the same way; this is that
	 * same bug recurring in new code, caught this time by an actual HIL
	 * run against rp4b-ble-hil rather than by review (see docs/research.md,
	 * 2026-09-23): FEATURES timed out as usual, and the live gamepad
	 * input device's EV bitmap had no EV_FF bit at all.
	 */
	sdev->caps.left_stick = true;
	sdev->caps.right_stick = true;
	sdev->caps.left_trigger = true;
	sdev->caps.right_trigger = true;
	sdev->caps.accel = true;
	sdev->caps.gyro = true;
	sdev->caps.rumble = true;
	sdev->caps.player_leds = true;
	sdev->caps.rgb_led = true;
	sdev->caps.button_mask = ~0u;
	/*
	 * Set explicitly rather than left zero-initialized: this project has
	 * hit the "existing cap field, never defaulted for the never-answers
	 * case" bug three times already (player_leds/rgb_led, then rumble --
	 * see docs/research.md), each time because a new capability field
	 * quietly kept its zero/false default here while every sibling field
	 * got set true. One touchpad with two fingers matches this HIL rig's
	 * own emulator's real default (ref-ble-gamepad's SInput mode
	 * auto-config), so it is a real value, not an arbitrary guess.
	 */
	sdev->caps.touchpad = true;
	sdev->caps.touchpad_count = 1;
	sdev->caps.touchpad_finger_count = 2;

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
	 * Also must be ready before hid_hw_start(): a FEATURES response can
	 * arrive as soon as raw_event can fire, and sinput_parse_features()
	 * takes touchpad_lock on every one it parses. See sinput.h's
	 * touchpad_lock comment for why this is a spinlock, not a mutex.
	 */
	sinput_touchpad_early_init(sdev);

	/*
	 * Deliberately do not request HID_CONNECT_HIDINPUT. This prevents
	 * hid-generic from creating a second input device for this HID
	 * collection while we develop the SInput-specific input path.
	 */
	ret = hid_parse(hdev);
	if (ret)
		return ret;

	sinput_verify_report_sizes(hdev);

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	/*
	 * hid_hw_start() alone is not enough: per <linux/hid.h>'s
	 * hid_driver kerneldoc, "During probe, input will not be passed to
	 * raw_event unless hid_device_io_start is called." Without this,
	 * hid_input_report()'s driver_input_lock trylock always loses to
	 * the lock hid_device_probe() is still holding around this entire
	 * function, so every incoming report -- including the FEATURES
	 * response the retry loop below is about to wait on -- is silently
	 * dropped for as long as probe() runs. hid_device_io_start() hands
	 * that lock back early so raw_event can actually fire while we
	 * wait.
	 */
	hid_device_io_start(hdev);

	/* See SINPUT_FEATURES_RETRY_COUNT's comment for why this retries. */
	for (i = 0; i < SINPUT_FEATURES_RETRY_COUNT; i++) {
		ret = sinput_request_features(sdev);
		if (ret < 0) {
			hid_info(hdev, "could not send SInput features request: %d\n", ret);
			break;
		}
		if (wait_for_completion_timeout(&sdev->caps_done,
						msecs_to_jiffies(SINPUT_FEATURES_RETRY_MS)))
			break;
	}
	if (!sdev->caps.valid)
		hid_info(hdev, "no SInput features response after %d attempt(s), assuming full capability set\n",
			 SINPUT_FEATURES_RETRY_COUNT);

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

	/*
	 * Not fatal to probe(), same reasoning as sinput_led_init() below: a
	 * touchpad registration failure (e.g. input_mt_init_slots() running
	 * out of memory) should not cost the user their gamepad, IMU, and
	 * battery. Errors are logged internally by the reconcile work itself,
	 * not returned here -- see sinput_touchpad.c. This is a no-op if
	 * caps.touchpad is false, or if a FEATURES response during the retry
	 * wait above already raced a reconcile in ahead of this call.
	 */
	sinput_touchpad_reconcile_and_wait(sdev);

	/*
	 * Not fatal to probe(): LEDs are an optional enhancement (same
	 * reasoning as the battery's power_supply_powers() topology link).
	 * A kernel without CONFIG_LEDS_CLASS_MULTICOLOR, or any other LED
	 * registration failure, should not cost the user their gamepad,
	 * IMU, and battery over an RGB indicator. sinput_led_init() logs
	 * the details itself.
	 */
	sinput_led_init(sdev);

	hid_info(hdev, "SInput driver attached (experimental)\n");
	return 0;

stop:
	/*
	 * No LED can be registered yet on this path (sinput_led_init() is
	 * the last init step and never jumps here itself), but set the flag
	 * before hid_hw_stop() on every path that calls it anyway -- keeps
	 * the invariant true regardless of future reordering in this function.
	 */
	mutex_lock(&sdev->output_lock);
	sdev->removing = true;
	mutex_unlock(&sdev->output_lock);

	/*
	 * Safe even if sinput_ff_init() was never reached on this path
	 * (e.g. devm_input_allocate_device() itself failed first): sdev is
	 * devm_kzalloc'd, and cancel_work_sync() on a zeroed, never-
	 * INIT_WORK()'d work_struct is a safe no-op -- it only ever consults
	 * the PENDING bit, which is 0 either way. See sdev->ff_lock's
	 * comment in sinput.h for why this exists at all.
	 */
	cancel_work_sync(&sdev->ff_work);

	/*
	 * touchpad_lock/touchpad_reinit_work are always initialized by the
	 * time this path can be reached (sinput_touchpad_early_init() runs
	 * before hid_hw_start(), and every goto stop here happens after
	 * that), unlike ff_work's more lenient "possibly still zeroed" case
	 * above -- so this can unregister anything a raced-in reconcile
	 * (from a FEATURES response during the retry wait) already built,
	 * with no special-casing needed.
	 */
	sinput_touchpad_remove(sdev);

	hid_hw_stop(hdev);
	return ret;
}

static void sinput_remove(struct hid_device *hdev)
{
	struct sinput_device *sdev = hid_get_drvdata(hdev);

	/*
	 * Must happen before hid_hw_stop(): devm doesn't unregister the LED
	 * classdevs until after this function returns, and
	 * led_classdev_unregister() synchronously drives brightness to
	 * LED_OFF via flush_work() at that point, which can still call back
	 * into sinput_send_output_command() -- see sdev->removing's comment
	 * in sinput.h. Setting this under output_lock, before stopping the
	 * transport, is what lets that callback see it and bail out cleanly
	 * (-ENODEV) instead of touching a stopped transport.
	 */
	mutex_lock(&sdev->output_lock);
	sdev->removing = true;
	mutex_unlock(&sdev->output_lock);

	/*
	 * Also before hid_hw_stop(): waits out any ff_work already running
	 * (from a play_effect() that fired just before "removing" was set
	 * above) and cancels anything merely queued, so no more rumble sends
	 * happen once the transport is about to go down and no work item is
	 * left referencing sdev past this point. See sdev->ff_lock's comment
	 * in sinput.h.
	 */
	cancel_work_sync(&sdev->ff_work);

	/*
	 * Also before hid_hw_stop(): unregisters whatever touchpad input_dev(s)
	 * are currently registered and cancels/waits out any reconcile work
	 * in flight. Touchpad devices are not devm-managed (see
	 * sinput_touchpad.c's top comment for why), so unlike the LED
	 * classdevs and every other input_dev in this driver, nothing else
	 * will ever unregister them if this doesn't.
	 */
	sinput_touchpad_remove(sdev);

	/*
	 * Deliberately no mutex_destroy(&sdev->output_lock) here: sdev and
	 * the LED classdevs are devm-managed and only actually torn down
	 * *after* this function returns, so LED sysfs files (and their
	 * brightness_set_blocking callbacks, which take output_lock) are
	 * still live at this point. Destroying the mutex here would be a
	 * real use-after-destroy race against a concurrent sysfs write, not
	 * just the debug-build lockdep nicety it looks like -- worse than
	 * the missing-destroy gap it was meant to fix.
	 */
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
		if (size >= SINPUT_INPUT_REPORT_SIZE) {
			sinput_input_report(sdev, data);
			sinput_battery_update(sdev, data);
			sinput_touchpad_report(sdev, data);
		}
	} else if (data[0] == SINPUT_REPORT_ID_CMD && size > SI_CMD_ECHO &&
		   data[SI_CMD_ECHO] == SINPUT_CMD_FEATURES) {
		/*
		 * The peripheral can notify before BLE ATT MTU negotiation
		 * finishes, truncating this report to the 23-byte default
		 * MTU. That still passes the size/echo check above (both
		 * live well inside the first 23 bytes), so a short, genuine
		 * fragment reaches here -- but sinput_parse_features() below
		 * rejects anything shorter than SINPUT_INPUT_REPORT_SIZE and
		 * leaves caps->valid false. Gate the completion on that
		 * instead of firing unconditionally: otherwise this first
		 * truncated fragment ends the probe() wait immediately,
		 * discarding every later full-size retry that would
		 * otherwise have succeeded.
		 */
		sinput_parse_features(sdev, data, size);
		if (sdev->caps.valid)
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

MODULE_AUTHOR("LeeNX <clinton.lee.taylor@gmail.com>");
MODULE_DESCRIPTION("Experimental Linux HID driver for SInput gamepads");
MODULE_LICENSE("GPL");
/* See src/Makefile's SINPUT_VERSION comment: sourced from dkms.conf, not hand-duplicated here. */
MODULE_VERSION(SINPUT_VERSION);
