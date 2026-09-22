// SPDX-License-Identifier: GPL-2.0-only
/*
 * SInput player LED and RGB LED output commands.
 *
 * Both are capability-gated (caps.player_leds / caps.rgb_led, from the
 * FEATURES response) and registered once from probe(), same point as
 * sinput_imu_init(). Unlike the battery subsystem, there is no incoming-
 * report race to guard against here: these are pure host -> device output,
 * nothing in raw_event ever reads LED state. There is still a write-write
 * race to worry about, though: two LEDs' brightness_set_blocking can run
 * concurrently from different processes, so state update + wire send is
 * done as one sdev->output_lock-held critical section per call, not two
 * separately-locking steps -- see sinput_send_output_command()'s comment.
 */

#include <linux/hid.h>
#include <linux/kernel.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/mutex.h>

#include "sinput.h"
#include "sinput_protocol.h"

/*
 * SInput's PLAYER_LED command takes a single scalar "player number" (see
 * SI_OUT_PLAYER_LED_NUM in sinput_protocol.h), not a bitmask of discrete
 * LEDs -- the device decides how to display the number on whatever LED
 * hardware it has. This driver still exposes N separate on/off
 * led_classdevs (PlayStation-style, see hid-playstation.c's player_leds[]),
 * since that's more familiar to desktop LED tooling than a single
 * brightness-encodes-the-number LED. hweight8() of "which LEDs are lit" is
 * the number actually sent: a simple, deterministic rule for arbitrary
 * sysfs writes, not just contiguous "thermometer" patterns.
 */
static int sinput_player_led_set(struct led_classdev *led, enum led_brightness value)
{
	struct hid_device *hdev = to_hid_device(led->dev->parent);
	struct sinput_device *sdev = hid_get_drvdata(hdev);
	unsigned int led_index = led - sdev->player_leds;
	u8 player_num;
	int ret;

	mutex_lock(&sdev->output_lock);
	if (value == LED_OFF)
		sdev->player_leds_state &= ~BIT(led_index);
	else
		sdev->player_leds_state |= BIT(led_index);
	player_num = hweight8(sdev->player_leds_state);
	ret = sinput_send_output_command(sdev, SINPUT_CMD_PLAYER_LED, &player_num, 1);
	mutex_unlock(&sdev->output_lock);

	return ret;
}

static enum led_brightness sinput_player_led_get(struct led_classdev *led)
{
	struct hid_device *hdev = to_hid_device(led->dev->parent);
	struct sinput_device *sdev = hid_get_drvdata(hdev);
	unsigned int led_index = led - sdev->player_leds;

	return !!(sdev->player_leds_state & BIT(led_index));
}

static int sinput_player_leds_init(struct sinput_device *sdev)
{
	static const char * const player_led_names[SINPUT_NUM_PLAYER_LEDS] = {
		LED_FUNCTION_PLAYER1, LED_FUNCTION_PLAYER2,
		LED_FUNCTION_PLAYER3, LED_FUNCTION_PLAYER4,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < SINPUT_NUM_PLAYER_LEDS; i++) {
		struct led_classdev *led = &sdev->player_leds[i];

		led->name = devm_kasprintf(&sdev->hdev->dev, GFP_KERNEL, "%s:white:%s",
					    dev_name(&sdev->hdev->dev), player_led_names[i]);
		if (!led->name)
			return -ENOMEM;

		led->brightness = 0;
		led->max_brightness = 1;
		led->brightness_set_blocking = sinput_player_led_set;
		led->brightness_get = sinput_player_led_get;

		ret = devm_led_classdev_register(&sdev->hdev->dev, led);
		if (ret) {
			hid_err(sdev->hdev, "could not register player LED %u: %d\n", i, ret);
			return ret;
		}
	}

	return 0;
}

static u8 sinput_rgb_scale(u8 val)
{
	/* Wire values are 0-63 (6-bit); see SI_OUT_RGB_RED in sinput_protocol.h. */
	return (val * 63 + 127) / 255;
}

static int sinput_rgb_led_set(struct led_classdev *led, enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led);
	struct hid_device *hdev = to_hid_device(led->dev->parent);
	struct sinput_device *sdev = hid_get_drvdata(hdev);
	u8 payload[3];
	int ret;

	led_mc_calc_color_components(mc_cdev, brightness);
	payload[0] = sinput_rgb_scale(mc_cdev->subled_info[0].brightness);
	payload[1] = sinput_rgb_scale(mc_cdev->subled_info[1].brightness);
	payload[2] = sinput_rgb_scale(mc_cdev->subled_info[2].brightness);

	mutex_lock(&sdev->output_lock);
	ret = sinput_send_output_command(sdev, SINPUT_CMD_RGB_LED, payload, sizeof(payload));
	mutex_unlock(&sdev->output_lock);

	return ret;
}

static int sinput_rgb_led_init(struct sinput_device *sdev)
{
	struct mc_subled *subled_info;
	struct led_classdev *led_cdev;
	int ret;

	subled_info = devm_kcalloc(&sdev->hdev->dev, 3, sizeof(*subled_info), GFP_KERNEL);
	if (!subled_info)
		return -ENOMEM;

	subled_info[0].color_index = LED_COLOR_ID_RED;
	subled_info[1].color_index = LED_COLOR_ID_GREEN;
	subled_info[2].color_index = LED_COLOR_ID_BLUE;

	sdev->rgb_led.subled_info = subled_info;
	sdev->rgb_led.num_colors = 3;

	led_cdev = &sdev->rgb_led.led_cdev;
	led_cdev->name = devm_kasprintf(&sdev->hdev->dev, GFP_KERNEL, "%s:rgb:indicator",
					 dev_name(&sdev->hdev->dev));
	if (!led_cdev->name)
		return -ENOMEM;
	led_cdev->max_brightness = 255;
	led_cdev->brightness_set_blocking = sinput_rgb_led_set;

	ret = devm_led_classdev_multicolor_register(&sdev->hdev->dev, &sdev->rgb_led);
	if (ret)
		hid_err(sdev->hdev, "could not register RGB LED device: %d\n", ret);

	return ret;
}

int sinput_led_init(struct sinput_device *sdev)
{
	int ret;

	if (sdev->caps.player_leds) {
		ret = sinput_player_leds_init(sdev);
		if (ret)
			return ret;
	}

	if (sdev->caps.rgb_led) {
		ret = sinput_rgb_led_init(sdev);
		if (ret)
			return ret;
	}

	return 0;
}
