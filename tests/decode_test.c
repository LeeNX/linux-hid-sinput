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
	pkt[SI_PLUG_STATUS] = 4;   /* on battery */
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
	pkt[1] = SINPUT_CMD_FEATURES;

	CHECK(sizeof(pkt) == SINPUT_OUTPUT_REPORT_SIZE, "output report size mismatch");
	CHECK(pkt[0] == SINPUT_REPORT_ID_OUTPUT, "output report id mismatch");
	CHECK(pkt[1] == SINPUT_CMD_FEATURES, "features command byte mismatch");
}

int main(void)
{
	test_state_report();
	test_features_response();
	test_usage_mask();
	test_output_report_layout();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}

	printf("All SInput protocol decode checks passed\n");
	return 0;
}
