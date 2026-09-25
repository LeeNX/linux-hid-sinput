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
/*
 * Two independent touch slots. Whether they represent two separate
 * one-finger touchpads or two fingers on one touchpad is a capability
 * decision (SI_FEAT_TOUCHPAD_COUNT/FINGERS below), not a wire-format one --
 * the wire always carries exactly these two X/Y/pressure triples. Confirmed
 * against both SDL_hidapi_sinput.c's SINPUT_REPORT_IDX_TOUCH1_X et al. and
 * leenx-foss/Bluepad32/hil's ref-ble-gamepad/BleSInput.h's SINPUT_IN_IDX_
 * TOUCH1_X et al. (the latter numbered from the byte after the report ID;
 * +1 from those values lines up exactly with the offsets below, and every
 * other already-verified offset in this file lines up the same way, e.g.
 * its SINPUT_IN_IDX_PLUG_STATUS=0 vs this file's SI_PLUG_STATUS=1). X/Y are
 * signed 16-bit spanning the full pad; pressure is unsigned 16-bit, 0
 * meaning "no finger" (SDL: `touch1P > 0` is its presence test).
 */
#define SI_TOUCH1_X      35
#define SI_TOUCH1_Y      37
#define SI_TOUCH1_P      39
#define SI_TOUCH2_X      41
#define SI_TOUCH2_Y      43
#define SI_TOUCH2_P      45

/* Output command requests (SINPUT_REPORT_ID_OUTPUT), byte 1 selects command. */
#define SINPUT_CMD_HAPTIC     0x01
#define SINPUT_CMD_FEATURES   0x02
#define SINPUT_CMD_PLAYER_LED 0x03
#define SINPUT_CMD_RGB_LED    0x04

/*
 * SINPUT_CMD_HAPTIC payload. SDL_hidapi_sinput.c defines two haptic
 * encodings: "type 1" (per-side frequency/amplitude pairs, for precise
 * waveform haptics) and "type 2" (per-side amplitude+brake, for
 * traditional ERM-motor-style rumble). SDL's own driver only ever sends
 * type 2 (HIDAPI_DriverSInput_RumbleJoystick() / HapticsType2Pack()); this
 * is also the only encoding that maps directly onto Linux's FF_RUMBLE
 * model (one strong + one weak magnitude, no per-side frequency), so this
 * driver only implements type 2. As with PLAYER_LED/RGB_LED, the spec does
 * not document this layout at all -- trust SDL's send site over guessing.
 */
#define SI_HAPTIC_TYPE_ERM        2
#define SI_OUT_HAPTIC_TYPE        2
#define SI_OUT_HAPTIC_LEFT_AMP    3
#define SI_OUT_HAPTIC_LEFT_BRAKE  4
#define SI_OUT_HAPTIC_RIGHT_AMP   5
#define SI_OUT_HAPTIC_RIGHT_BRAKE 6

/*
 * Output report (SINPUT_REPORT_ID_OUTPUT) payload offsets, report ID at
 * byte 0, command byte (one of SINPUT_CMD_*) at SI_OUT_CMD. Confirmed
 * against leenx-foss/Bluepad32/hil's ref-ble-gamepad/BleSInput.cpp's
 * onWrite() -- real reverse-engineered host traffic, not the spec (which
 * doesn't document this layout at all).
 */
#define SI_OUT_CMD             1
/*
 * SINPUT_CMD_PLAYER_LED payload: a single "player number" byte, not a
 * bitmask -- SDL_hidapi_sinput.c sends its own generic 0-based joystick
 * player index +1 (0 = unassigned), clamped to 0-255. The device decides
 * how to display that number on whatever LED hardware it has.
 */
#define SI_OUT_PLAYER_LED_NUM  2
/*
 * SINPUT_CMD_RGB_LED payload: R/G/B, each 0-63 (6-bit), NOT 0-255.
 * BleSInput.h's own comment: "hosts send e.g. 64 for #FFFFFF -- scale to
 * 8-bit via (v*255+31)/63". The .cpp has a comment about a real off-by-one
 * bug that project hit and fixed in this exact byte layout, so trust it.
 */
#define SI_OUT_RGB_RED         2
#define SI_OUT_RGB_GREEN       3
#define SI_OUT_RGB_BLUE        4

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
/*
 * Touchpad click (the digital "press down on the pad" button, separate from
 * finger presence/position above), one bit per possible touchpad. Unlike
 * the face-button case, this bit numbering agrees across every source
 * checked: SDL_hidapi_sinput.c's SINPUT_BUTTONMASK_TOUCHPAD1/2 (0x40/0x80 in
 * usage-mask byte 2 -- i.e. bits 22/23 of the 32-bit word), and
 * ref-ble-gamepad's BleSInput.h SINPUT_BTN2_TOUCHPAD1/2 (same 0x40/0x80 in
 * its own byte-2 mask) and BleGamepad.cpp's actual button-16/17 wiring.
 * Deliberately not added to sinput_input.c's sinput_buttons[] table: like
 * hid-playstation.c's touchpad click, this belongs to the touchpad's own
 * input_dev (BTN_LEFT), not the main gamepad's.
 */
#define SINPUT_BTN_IDX_TOUCHPAD1     22
#define SINPUT_BTN_IDX_TOUCHPAD2     23

#endif /* SINPUT_PROTOCOL_H */
