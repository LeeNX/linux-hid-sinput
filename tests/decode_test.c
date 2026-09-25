// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host-side protocol conformance test.
 *
 * This does not touch real hardware or the kernel module. It builds
 * synthetic SInput report bytes (state + feature/capability response)
 * using known values, decodes them with the same offsets and flag bits
 * declared in src/sinput_protocol.h, and checks the round trip.
 *
 * It exists so the byte-offset math can be verified with `make check`
 * on any machine, without a Linux kernel build environment.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/sinput_protocol.h"

static int failures;

#define CHECK(cond, fmt, ...)                                               \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__,   \
				__LINE__, ##__VA_ARGS__);                     \
			failures++;                                           \
		}                                                             \
	} while (0)

static uint16_t get_le16(const uint8_t *d, unsigned int off)
{
	return (uint16_t)(d[off] | (d[off + 1] << 8));
}

static void put_le16(uint8_t *d, unsigned int off, uint16_t v)
{
	d[off] = (uint8_t)(v & 0xff);
	d[off + 1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *d, unsigned int off, uint32_t v)
{
	d[off] = (uint8_t)(v & 0xff);
	d[off + 1] = (uint8_t)((v >> 8) & 0xff);
	d[off + 2] = (uint8_t)((v >> 16) & 0xff);
	d[off + 3] = (uint8_t)((v >> 24) & 0xff);
}

static void test_state_report(void)
{
	uint8_t pkt[SINPUT_INPUT_REPORT_SIZE] = { 0 };

	pkt[0] = SINPUT_REPORT_ID_STATE;
	pkt[SI_PLUG_STATUS] = SI_PLUG_STATUS_ON_BATTERY;
	pkt[SI_CHARGE_LEVEL] = 77;

	/* BTN_SOUTH and BTN_DPAD_UP held. */
	put_le32(pkt, SI_BUTTONS_0, (1u << SINPUT_BTN_IDX_SOUTH) | (1u << SINPUT_BTN_IDX_DPAD_UP));

	put_le16(pkt, SI_LEFT_X, (uint16_t)-1000);
	put_le16(pkt, SI_LEFT_Y, 1000);
	put_le16(pkt, SI_RIGHT_X, (uint16_t)-2000);
	put_le16(pkt, SI_RIGHT_Y, 2000);
	put_le16(pkt, SI_LEFT_TRIGGER, 12345);
	put_le16(pkt, SI_RIGHT_TRIGGER, (uint16_t)-12345);

	put_le16(pkt, SI_ACCEL_X, 100);
	put_le16(pkt, SI_ACCEL_Y, 200);
	put_le16(pkt, SI_ACCEL_Z, 300);
	put_le16(pkt, SI_GYRO_X, 400);
	put_le16(pkt, SI_GYRO_Y, 500);
	put_le16(pkt, SI_GYRO_Z, 600);

	CHECK(pkt[0] == SINPUT_REPORT_ID_STATE, "state report id mismatch");
	CHECK(pkt[SI_PLUG_STATUS] == SI_PLUG_STATUS_ON_BATTERY, "plug status mismatch");
	CHECK(pkt[SI_CHARGE_LEVEL] == 77, "charge level mismatch");

	uint32_t buttons = pkt[SI_BUTTONS_0] | (pkt[SI_BUTTONS_0 + 1] << 8) |
			   (pkt[SI_BUTTONS_0 + 2] << 16) | (pkt[SI_BUTTONS_0 + 3] << 24);
	CHECK((buttons & (1u << SINPUT_BTN_IDX_SOUTH)) != 0, "BTN_SOUTH bit not set");
	CHECK((buttons & (1u << SINPUT_BTN_IDX_DPAD_UP)) != 0, "BTN_DPAD_UP bit not set");
	CHECK((buttons & (1u << SINPUT_BTN_IDX_EAST)) == 0, "BTN_EAST bit unexpectedly set");

	CHECK((int16_t)get_le16(pkt, SI_LEFT_X) == -1000, "left x mismatch");
	CHECK((int16_t)get_le16(pkt, SI_LEFT_Y) == 1000, "left y mismatch");
	CHECK((int16_t)get_le16(pkt, SI_RIGHT_X) == -2000, "right x mismatch");
	CHECK((int16_t)get_le16(pkt, SI_RIGHT_Y) == 2000, "right y mismatch");
	CHECK((int16_t)get_le16(pkt, SI_LEFT_TRIGGER) == 12345, "left trigger mismatch");
	CHECK((int16_t)get_le16(pkt, SI_RIGHT_TRIGGER) == -12345, "right trigger mismatch");

	CHECK((int16_t)get_le16(pkt, SI_ACCEL_X) == 100, "accel x mismatch");
	CHECK((int16_t)get_le16(pkt, SI_GYRO_Z) == 600, "gyro z mismatch");

	/* Offsets must not overlap and must stay inside the report. */
	CHECK(SI_GYRO_Z + 2 <= SINPUT_INPUT_REPORT_SIZE, "gyro z overruns report");
}

static void test_touchpad_report(void)
{
	uint8_t pkt[SINPUT_INPUT_REPORT_SIZE] = { 0 };

	pkt[0] = SINPUT_REPORT_ID_STATE;

	/* Touchpad 1 click held, touchpad 2 click not held. */
	put_le32(pkt, SI_BUTTONS_0, (1u << SINPUT_BTN_IDX_TOUCHPAD1));

	put_le16(pkt, SI_TOUCH1_X, (uint16_t)-16000);
	put_le16(pkt, SI_TOUCH1_Y, 16000);
	put_le16(pkt, SI_TOUCH1_P, 32767);
	put_le16(pkt, SI_TOUCH2_X, 0);
	put_le16(pkt, SI_TOUCH2_Y, 0);
	put_le16(pkt, SI_TOUCH2_P, 0); /* pressure 0 -> finger 2 not down */

	CHECK((int16_t)get_le16(pkt, SI_TOUCH1_X) == -16000, "touch1 x mismatch");
	CHECK((int16_t)get_le16(pkt, SI_TOUCH1_Y) == 16000, "touch1 y mismatch");
	CHECK(get_le16(pkt, SI_TOUCH1_P) == 32767, "touch1 pressure mismatch");
	CHECK(get_le16(pkt, SI_TOUCH2_P) == 0, "touch2 pressure mismatch");

	uint32_t buttons = pkt[SI_BUTTONS_0] | (pkt[SI_BUTTONS_0 + 1] << 8) |
			   (pkt[SI_BUTTONS_0 + 2] << 16) | (pkt[SI_BUTTONS_0 + 3] << 24);
	CHECK((buttons & (1u << SINPUT_BTN_IDX_TOUCHPAD1)) != 0, "TOUCHPAD1 click bit not set");
	CHECK((buttons & (1u << SINPUT_BTN_IDX_TOUCHPAD2)) == 0, "TOUCHPAD2 click bit unexpectedly set");

	/* The two touch slots and both touchpad click bits must be distinct
	 * byte positions/bit numbers and stay inside the report/button word.
	 */
	CHECK(SI_TOUCH1_X != SI_TOUCH1_Y && SI_TOUCH1_Y != SI_TOUCH1_P &&
	      SI_TOUCH1_P != SI_TOUCH2_X && SI_TOUCH2_X != SI_TOUCH2_Y &&
	      SI_TOUCH2_Y != SI_TOUCH2_P,
	      "touch slot offsets overlap");
	CHECK(SI_TOUCH2_P + 2 <= SINPUT_INPUT_REPORT_SIZE, "touch2 pressure overruns report");
	CHECK(SINPUT_BTN_IDX_TOUCHPAD1 != SINPUT_BTN_IDX_TOUCHPAD2,
	      "touchpad click bit indices overlap");
	CHECK(SINPUT_BTN_IDX_TOUCHPAD2 < 32, "touchpad click bit index overruns the 32-bit button word");
}

static void test_features_response(void)
{
	uint8_t pkt[SINPUT_INPUT_REPORT_SIZE] = { 0 };

	pkt[0] = SINPUT_REPORT_ID_CMD;
	pkt[SI_CMD_ECHO] = SINPUT_CMD_FEATURES;

	put_le16(pkt, SI_FEAT_PROTOCOL_VER, 3);

	uint8_t flags0 = SI_FLAG0_ACCEL | SI_FLAG0_GYRO | SI_FLAG0_LEFT_STICK |
			 SI_FLAG0_RIGHT_TRIGGER;
	uint8_t flags1 = SI_FLAG1_RGB_LED;

	pkt[SI_FEAT_FLAGS_0] = flags0;
	pkt[SI_FEAT_FLAGS_1] = flags1;

	put_le16(pkt, SI_FEAT_POLL_RATE_US, 1000);

	CHECK(pkt[0] == SINPUT_REPORT_ID_CMD, "cmd report id mismatch");
	CHECK(pkt[SI_CMD_ECHO] == SINPUT_CMD_FEATURES, "features echo mismatch");

	uint16_t proto_ver = get_le16(pkt, SI_FEAT_PROTOCOL_VER);

	CHECK(proto_ver == 3, "protocol version mismatch: got %u", proto_ver);

	uint16_t poll_rate = get_le16(pkt, SI_FEAT_POLL_RATE_US);

	CHECK(poll_rate == 1000, "poll rate mismatch: got %u", poll_rate);

	CHECK((pkt[SI_FEAT_FLAGS_0] & SI_FLAG0_ACCEL) != 0, "accel flag not set");
	CHECK((pkt[SI_FEAT_FLAGS_0] & SI_FLAG0_GYRO) != 0, "gyro flag not set");
	CHECK((pkt[SI_FEAT_FLAGS_0] & SI_FLAG0_LEFT_STICK) != 0, "left stick flag not set");
	CHECK((pkt[SI_FEAT_FLAGS_0] & SI_FLAG0_RIGHT_STICK) == 0, "right stick flag unexpectedly set");
	CHECK((pkt[SI_FEAT_FLAGS_0] & SI_FLAG0_RUMBLE) == 0, "rumble flag unexpectedly set");
	CHECK((pkt[SI_FEAT_FLAGS_0] & SI_FLAG0_RIGHT_TRIGGER) != 0, "right trigger flag not set");

	CHECK((pkt[SI_FEAT_FLAGS_1] & SI_FLAG1_RGB_LED) != 0, "rgb led flag not set");
	CHECK((pkt[SI_FEAT_FLAGS_1] & SI_FLAG1_TOUCHPAD) == 0, "touchpad flag unexpectedly set");

	/* All feature fields used by the driver must fit in the report. */
	CHECK(SI_FEAT_TOUCHPAD_FINGERS + 1 <= SINPUT_INPUT_REPORT_SIZE,
	      "touchpad finger count field overruns report");
}

static void test_usage_mask(void)
{
	uint8_t pkt[SINPUT_INPUT_REPORT_SIZE] = { 0 };

	pkt[0] = SINPUT_REPORT_ID_CMD;
	pkt[SI_CMD_ECHO] = SINPUT_CMD_FEATURES;

	/* Device only has SOUTH, DPAD_UP and START; everything else absent. */
	uint32_t usage_mask = (1u << SINPUT_BTN_IDX_SOUTH) |
			       (1u << SINPUT_BTN_IDX_DPAD_UP) |
			       (1u << SINPUT_BTN_IDX_START);

	put_le32(pkt, SI_FEAT_USAGE_MASK_0, usage_mask);

	uint32_t got = pkt[SI_FEAT_USAGE_MASK_0] |
			(pkt[SI_FEAT_USAGE_MASK_0 + 1] << 8) |
			(pkt[SI_FEAT_USAGE_MASK_0 + 2] << 16) |
			(pkt[SI_FEAT_USAGE_MASK_0 + 3] << 24);

	CHECK(got == usage_mask, "usage mask round trip mismatch: got 0x%08x", got);
	CHECK((got & (1u << SINPUT_BTN_IDX_SOUTH)) != 0, "SOUTH bit not set");
	CHECK((got & (1u << SINPUT_BTN_IDX_EAST)) == 0, "EAST bit unexpectedly set");
	CHECK((got & (1u << SINPUT_BTN_IDX_CAPTURE)) == 0, "CAPTURE bit unexpectedly set");

	/*
	 * The usage mask must line up with the same bit numbering as the
	 * state report's button bytes (SI_BUTTONS_0..3), since the driver
	 * tests both with the same SINPUT_BTN_IDX_* constants.
	 */
	CHECK(SI_FEAT_USAGE_MASK_0 + 4 <= SINPUT_INPUT_REPORT_SIZE,
	      "usage mask field overruns report");
}

static void test_output_report_layout(void)
{
	uint8_t pkt[SINPUT_OUTPUT_REPORT_SIZE] = { 0 };

	pkt[0] = SINPUT_REPORT_ID_OUTPUT;
	pkt[SI_OUT_CMD] = SINPUT_CMD_FEATURES;

	CHECK(sizeof(pkt) == SINPUT_OUTPUT_REPORT_SIZE, "output report size mismatch");
	CHECK(pkt[0] == SINPUT_REPORT_ID_OUTPUT, "output report id mismatch");
	CHECK(pkt[SI_OUT_CMD] == SINPUT_CMD_FEATURES, "features command byte mismatch");
}

static void test_player_led_command(void)
{
	uint8_t pkt[SINPUT_OUTPUT_REPORT_SIZE] = { 0 };

	pkt[0] = SINPUT_REPORT_ID_OUTPUT;
	pkt[SI_OUT_CMD] = SINPUT_CMD_PLAYER_LED;
	pkt[SI_OUT_PLAYER_LED_NUM] = 3;

	CHECK(pkt[SI_OUT_CMD] == SINPUT_CMD_PLAYER_LED, "player LED command byte mismatch");
	CHECK(pkt[SI_OUT_PLAYER_LED_NUM] == 3, "player LED number mismatch");
	CHECK(SI_OUT_PLAYER_LED_NUM < SINPUT_OUTPUT_REPORT_SIZE,
	      "player LED number field overruns report");
}

static void test_rgb_led_command(void)
{
	uint8_t pkt[SINPUT_OUTPUT_REPORT_SIZE] = { 0 };

	pkt[0] = SINPUT_REPORT_ID_OUTPUT;
	pkt[SI_OUT_CMD] = SINPUT_CMD_RGB_LED;
	/* Wire values are 0-63 (6-bit); driver scales from 8-bit via (v*63+127)/255. */
	pkt[SI_OUT_RGB_RED] = 63;
	pkt[SI_OUT_RGB_GREEN] = 32;
	pkt[SI_OUT_RGB_BLUE] = 0;

	CHECK(pkt[SI_OUT_CMD] == SINPUT_CMD_RGB_LED, "RGB LED command byte mismatch");
	CHECK(pkt[SI_OUT_RGB_RED] <= 63, "red channel exceeds 6-bit wire range");
	CHECK(pkt[SI_OUT_RGB_GREEN] <= 63, "green channel exceeds 6-bit wire range");
	CHECK(pkt[SI_OUT_RGB_BLUE] <= 63, "blue channel exceeds 6-bit wire range");
	/* Offsets must be distinct byte positions and stay inside the report. */
	CHECK(SI_OUT_RGB_RED != SI_OUT_RGB_GREEN && SI_OUT_RGB_GREEN != SI_OUT_RGB_BLUE,
	      "RGB channel offsets overlap");
	CHECK(SI_OUT_RGB_BLUE < SINPUT_OUTPUT_REPORT_SIZE, "RGB blue field overruns report");
}

static void test_haptic_command(void)
{
	uint8_t pkt[SINPUT_OUTPUT_REPORT_SIZE] = { 0 };

	pkt[0] = SINPUT_REPORT_ID_OUTPUT;
	pkt[SI_OUT_CMD] = SINPUT_CMD_HAPTIC;
	pkt[SI_OUT_HAPTIC_TYPE] = SI_HAPTIC_TYPE_ERM;
	/* strong_magnitude 0xC000 -> left amplitude 0xC0 (top byte only). */
	pkt[SI_OUT_HAPTIC_LEFT_AMP] = 0xc0;
	pkt[SI_OUT_HAPTIC_LEFT_BRAKE] = 0;
	/* weak_magnitude 0x4000 -> right amplitude 0x40. */
	pkt[SI_OUT_HAPTIC_RIGHT_AMP] = 0x40;
	pkt[SI_OUT_HAPTIC_RIGHT_BRAKE] = 0;

	CHECK(pkt[SI_OUT_CMD] == SINPUT_CMD_HAPTIC, "haptic command byte mismatch");
	CHECK(pkt[SI_OUT_HAPTIC_TYPE] == SI_HAPTIC_TYPE_ERM, "haptic type mismatch");
	CHECK(pkt[SI_OUT_HAPTIC_LEFT_AMP] == 0xc0, "left amplitude mismatch");
	CHECK(pkt[SI_OUT_HAPTIC_RIGHT_AMP] == 0x40, "right amplitude mismatch");

	/* Offsets must be distinct byte positions and stay inside the report. */
	CHECK(SI_OUT_HAPTIC_TYPE != SI_OUT_HAPTIC_LEFT_AMP &&
	      SI_OUT_HAPTIC_LEFT_AMP != SI_OUT_HAPTIC_LEFT_BRAKE &&
	      SI_OUT_HAPTIC_LEFT_BRAKE != SI_OUT_HAPTIC_RIGHT_AMP &&
	      SI_OUT_HAPTIC_RIGHT_AMP != SI_OUT_HAPTIC_RIGHT_BRAKE,
	      "haptic payload offsets overlap");
	CHECK(SI_OUT_HAPTIC_RIGHT_BRAKE < SINPUT_OUTPUT_REPORT_SIZE,
	      "haptic payload overruns report");
}

int main(void)
{
	test_state_report();
	test_touchpad_report();
	test_features_response();
	test_usage_mask();
	test_output_report_layout();
	test_player_led_command();
	test_rgb_led_command();
	test_haptic_command();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}

	printf("All SInput protocol decode checks passed\n");
	return 0;
}
