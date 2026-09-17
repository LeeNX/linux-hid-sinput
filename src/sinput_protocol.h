/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SInput HID wire protocol constants.
 *
 * Report IDs, byte offsets and flag bits below are derived from SDL's
 * SInput HIDAPI driver (src/joystick/hidapi/SDL_hidapi_sinput.c), which is
 * currently the only executable reference implementation of the protocol.
 * Treat this as reference data, not a stable ABI: the upstream SInput
 * specification says the layout is subject to change.
 *
 * This header intentionally uses only plain preprocessor constants (no
 * kernel or SDL types) so it can be shared between the kernel driver and
 * host-side test/tooling code.
 */

#ifndef SINPUT_PROTOCOL_H
#define SINPUT_PROTOCOL_H

/* SInput development/test VID/PID. Real products should register their own. */
#define USB_VENDOR_ID_SINPUT   0x2e8a
#define USB_DEVICE_ID_SINPUT   0x10c6

#define SINPUT_REPORT_ID_STATE  0x01 /* device -> host, joystick state    */
#define SINPUT_REPORT_ID_CMD    0x02 /* device -> host, command response  */
#define SINPUT_REPORT_ID_OUTPUT 0x03 /* host -> device, command request   */

#define SINPUT_INPUT_REPORT_SIZE  64
#define SINPUT_OUTPUT_REPORT_SIZE 48

/* State report (SINPUT_REPORT_ID_STATE) byte offsets, report ID at byte 0. */
#define SI_PLUG_STATUS   1
#define SI_CHARGE_LEVEL  2
#define SI_BUTTONS_0     3
#define SI_BUTTONS_1     4
#define SI_BUTTONS_2     5
#define SI_BUTTONS_3     6
#define SI_LEFT_X        7
#define SI_LEFT_Y        9
#define SI_RIGHT_X       11
#define SI_RIGHT_Y       13
#define SI_LEFT_TRIGGER  15
#define SI_RIGHT_TRIGGER 17
#define SI_IMU_TIMESTAMP 19
#define SI_ACCEL_X       23
#define SI_ACCEL_Y       25
#define SI_ACCEL_Z       27
#define SI_GYRO_X        29
#define SI_GYRO_Y        31
#define SI_GYRO_Z        33

/* Output command requests (SINPUT_REPORT_ID_OUTPUT), byte 1 selects command. */
#define SINPUT_CMD_HAPTIC     0x01
#define SINPUT_CMD_FEATURES   0x02
#define SINPUT_CMD_PLAYER_LED 0x03
#define SINPUT_CMD_RGB_LED    0x04

/*
 * Command/feature response (SINPUT_REPORT_ID_CMD) byte offsets, report ID
 * at byte 0 and the echoed command byte at SI_CMD_ECHO.
 */
#define SI_CMD_ECHO              1
#define SI_FEAT_PROTOCOL_VER     2
#define SI_FEAT_FLAGS_0          4
#define SI_FEAT_FLAGS_1          5
#define SI_FEAT_GAMEPAD_TYPE     6
#define SI_FEAT_SUBTYPE          7
#define SI_FEAT_POLL_RATE_US     8
#define SI_FEAT_ACCEL_RANGE      10
#define SI_FEAT_GYRO_RANGE       12
#define SI_FEAT_USAGE_MASK_0     14
#define SI_FEAT_TOUCHPAD_COUNT   18
#define SI_FEAT_TOUCHPAD_FINGERS 19

#define SI_FLAG0_RUMBLE        0x01
#define SI_FLAG0_PLAYER_LEDS   0x02
#define SI_FLAG0_ACCEL         0x04
#define SI_FLAG0_GYRO          0x08
#define SI_FLAG0_LEFT_STICK    0x10
#define SI_FLAG0_RIGHT_STICK   0x20
#define SI_FLAG0_LEFT_TRIGGER  0x40
#define SI_FLAG0_RIGHT_TRIGGER 0x80

#define SI_FLAG1_TOUCHPAD 0x01
#define SI_FLAG1_RGB_LED  0x02
#define SI_FLAG1_HANDHELD 0x04

#endif /* SINPUT_PROTOCOL_H */
