// SPDX-License-Identifier: GPL-2.0-only
/*
 * SInput battery/charge state, exposed as a standard Linux power_supply
 * device. SI_PLUG_STATUS/SI_CHARGE_LEVEL have no capability bit -- they're
 * always present in every state report -- so this subsystem never depends
 * on sdev->caps or the FEATURES round-trip at all, unlike sinput_input.c.
 */

#include <linux/hid.h>
#include <linux/kernel.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>

#include "sinput.h"
#include "sinput_protocol.h"

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
 * data is a full, validated SINPUT_REPORT_ID_STATE report -- report ID and
 * size are checked once by the raw_event dispatcher in sinput_core.c before
 * calling here (and into sinput_input_report()).
 */
void sinput_battery_update(struct sinput_device *sdev, const u8 *data)
{
	unsigned long flags;
	u8 new_plug_status, new_capacity;
	bool battery_changed;

	new_plug_status = data[SI_PLUG_STATUS];
	/* Protocol says 0-100; clamp defensively against a malformed/buggy device. */
	new_capacity = min_t(u8, data[SI_CHARGE_LEVEL], 100);

	spin_lock_irqsave(&sdev->battery_lock, flags);
	battery_changed = sdev->battery_plug_status != new_plug_status ||
			   sdev->battery_capacity != new_capacity;
	sdev->battery_plug_status = new_plug_status;
	sdev->battery_capacity = new_capacity;
	spin_unlock_irqrestore(&sdev->battery_lock, flags);

	/* See README.md "Diagnostics". Only on an actual change, not every
	 * report -- plug/capacity is otherwise unchanged far more often than
	 * not, unlike buttons/axes.
	 */
	if (battery_changed)
		hid_dbg(sdev->hdev, "battery: plug_status=%u capacity=%u%%\n",
			new_plug_status, new_capacity);

	/*
	 * sdev->battery is only set once sinput_battery_init() completes; a
	 * report can land before that (same race sdev->input has in
	 * sinput_input_report()), so check it rather than assume it's ready.
	 */
	if (battery_changed && sdev->battery)
		power_supply_changed(sdev->battery);
}

/*
 * SI_PLUG_STATUS/SI_CHARGE_LEVEL have no capability bit (see the comment in
 * sinput_protocol.h), so unlike sinput_input_init()/sinput_imu_init() this
 * never depends on sdev->caps and can run regardless of whether a FEATURES
 * response ever arrives.
 */
int sinput_battery_init(struct sinput_device *sdev)
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
