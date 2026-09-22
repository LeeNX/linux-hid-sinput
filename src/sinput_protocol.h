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

/*
 * SI_PLUG_STATUS values. Unlike the feature-response flags, battery/charge
 * state has no capability bit -- these two bytes are always present in
 * every state report, present or not. Confirmed against SDL_hidapi_sinput.c's
 * HandleStatePacket switch (not SDL's own #define names -- see the
 * SINPUT_BTN_IDX_* comment above the button table in sinput.c for why those
 * aren't trustworthy on their own) and cross-checked against a real SInput
 * BLE device's ESP32-BLE-Gamepad firmware, which produces exactly these five
 * values (leenx-foss/Bluepad32/hil's BleSInput.h). SDL forces charge level to
 * 0 for NO_BATTERY and 100 for CHARGED.
 */
#define SI_PLUG_STATUS_UNKNOWN    0
#define SI_PLUG_STATUS_NO_BATTERY 1
#define SI_PLUG_STATUS_CHARGING   2
#define SI_PLUG_STATUS_CHARGED    3
#define SI_PLUG_STATUS_ON_BATTERY 4
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

/*
 * Button bit indices. The state report's four button bytes (SI_BUTTONS_0..
 * SI_BUTTONS_3, read as one little-endian 32-bit word) and the feature
 * response's 4-byte usage mask (SI_FEAT_USAGE_MASK_0, same 32-bit layout)
 * share this numbering: usage mask bit N clear means button N does not
 * exist on this device and its state bit should be ignored.
 *
 * Only indices the driver currently maps to a Linux key code are listed;
 * see SDL_hidapi_sinput.c for the full 32-bit layout (paddles, touchpad
 * clicks, power, misc4-10) if more are mapped later.
 *
 * The face-button values below deliberately do NOT match SDL_hidapi_sinput.c's
 * own SINPUT_BUTTON_IDX_* names for these four bits. HIL testing against a
 * real ESP32-BLE-Gamepad SInput device (2026-09-22, rp4b-ble-hil, see
 * docs/research.md) showed bits 0-3 are physically south/east/west/north in
 * that order, not SDL's east/south/north/west -- SDL's own constant names
 * are inconsistent with the gamepad mapping string it actually builds at
 * runtime. This was independently caught by the Bluepad32 HIL project too
 * (leenx-foss/Bluepad32/hil CLAUDE.md: "bit 0 = A/south (SDL's constants
 * name it 'east')"). Trust the hardware, not SDL's #define names.
 */
#define SINPUT_BTN_IDX_SOUTH         0
#define SINPUT_BTN_IDX_EAST          1
#define SINPUT_BTN_IDX_WEST          2
#define SINPUT_BTN_IDX_NORTH         3
#define SINPUT_BTN_IDX_DPAD_UP       4
#define SINPUT_BTN_IDX_DPAD_DOWN     5
#define SINPUT_BTN_IDX_DPAD_LEFT     6
#define SINPUT_BTN_IDX_DPAD_RIGHT    7
#define SINPUT_BTN_IDX_LEFT_STICK    8
#define SINPUT_BTN_IDX_RIGHT_STICK   9
#define SINPUT_BTN_IDX_LEFT_BUMPER   10
#define SINPUT_BTN_IDX_RIGHT_BUMPER  11
#define SINPUT_BTN_IDX_LEFT_TRIGGER  12
#define SINPUT_BTN_IDX_RIGHT_TRIGGER 13
#define SINPUT_BTN_IDX_START         16
#define SINPUT_BTN_IDX_BACK          17
#define SINPUT_BTN_IDX_GUIDE         18
#define SINPUT_BTN_IDX_CAPTURE       19

#endif /* SINPUT_PROTOCOL_H */
