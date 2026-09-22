# SInput Linux driver research

## 1. Protocol status

The Hand Held Legend SInput development document describes SInput as a targetable
HID setup intended to improve native SDL compatibility. It says `2E8A:10C6` may
be used for generic testing, while real devices should register their own PID.

The development document lists:

* up to 32 digital buttons
* 3-axis gyro
* 3-axis accelerometer
* 1 kHz polling
* stereo haptics
* player LEDs
* up to two touchpads

Important: the development document explicitly says aspects are subject to change.

## 2. SDL is currently the best executable protocol reference

SDL's `src/joystick/hidapi/SDL_hidapi_sinput.c` is a particularly useful reference
because it contains the actual packet offsets and capability handling used by a
large cross-platform project.

Current SDL source defines:

* input report ID `0x01`
* feature/command response input report ID `0x02`
* command output report ID `0x03`
* input report size 64 bytes
* command output size 48 bytes

Current state packet offsets include:

| Offset | Meaning |
|---:|---|
| 1 | plug/power status |
| 2 | charge level |
| 3-6 | four button bytes |
| 7-8 | left X |
| 9-10 | left Y |
| 11-12 | right X |
| 13-14 | right Y |
| 15-16 | left trigger |
| 17-18 | right trigger |
| 19-22 | IMU timestamp |
| 23-28 | accelerometer XYZ |
| 29-34 | gyro XYZ |
| 35-40 | touchpad 1 |
| 41-46 | touchpad 2 |

The values above are based on the current SDL implementation and should be
treated as protocol-reference data, not copied blindly into a future upstream
kernel driver.

## 3. Capability discovery

SDL's current implementation decodes feature information including:

* rumble
* player LEDs
* accelerometer
* gyroscope
* left/right sticks
* left/right analog triggers
* touchpad
* RGB LED
* handheld flag
* touchpad count/finger count
* polling rate
* subtype and mapping information

A kernel driver should use this capability information to decide which Linux
interfaces to register. Avoid creating a fixed six-axis device when the SInput
capability report says those axes do not exist.

## 4. Linux architecture

The Linux HID framework already supports transport-independent HID drivers. The
kernel documentation describes `hid_driver` callbacks such as `probe`, `remove`,
`raw_event`, and `input_configured`.

For a first DKMS experiment, this project intentionally uses:

    HID transport
        |
        v
    sinput HID driver
        |
        +--> evdev gamepad
        +--> future FF
        +--> future LEDs
        +--> future power_supply
        +--> future sensor input

A mature upstream driver should be compared closely with:

* `drivers/hid/hid-playstation.c`
* `drivers/hid/hid-nintendo.c`

Those drivers show how Linux-specific output features, force feedback, LEDs,
power and lifecycle handling can be integrated rather than exposing everything
through a generic userspace protocol.

## 5. Important design question: kernel driver vs SDL

SDL already has native SInput support. A kernel driver should not exist merely to
make SDL see the controller.

The useful Linux-native goals are:

* evdev compatibility
* FF through `/dev/input/event*`
* LED class integration
* `power_supply`
* sensor interfaces
* suspend/resume
* predictable behaviour for non-SDL applications

The driver should preserve the raw HID path where possible so that HIDAPI and
diagnostic tools remain useful.

## 6. Generic VID/PID strategy

For the development/test device:

    VID = 0x2E8A
    PID = 0x10C6

For production firmware, use the device's real VID/PID and add it to the driver
without making the generic test ID the permanent identity of unrelated products.

The initial module therefore matches the generic SInput ID only.

## 7. DKMS strategy

Keep this repository out-of-tree until the interface is stable.

DKMS should:

* build against the currently installed kernel headers;
* install one module under `/updates/dkms`;
* allow rapid testing across Ubuntu/Debian kernel versions;
* make it easy to remove the experiment.

Once the driver is mature, compare it against the kernel HID maintainer expectations
before considering an upstream submission.

## 8. Test plan

### Stage A - input

* USB connect/disconnect
* button press/release
* D-pad
* left/right stick
* analog triggers
* 1 kHz report stream
* no stuck keys after unplug

### Stage B - capabilities

* feature response retrieval
* no-IMU controller
* IMU controller
* one-stick controller
* two-stick controller
* digital trigger controller
* touchpad controller

### Stage C - output

* rumble
* player LED
* RGB LED
* command serialization
* timeout/error handling

### Stage D - power

* no battery
* charging
* charged
* discharging
* battery percentage changes

### Stage E - transport

* USB HID
* Bluetooth HID, if the SInput device supports it
* suspend/resume
* reconnect

### Stage F - HIL

Use the ESP32 SInput firmware as a deterministic test target. Capture known
reports and compare:

* raw report bytes
* kernel events
* SDL events
* timestamps
* output commands

## Sources

* SInput development specification:
  https://docs.handheldlegend.com/s/sinput
* SInput-HID reference repository (Hand Held Legend):
  https://github.com/HandHeldLegend/SInput-HID
* SDL SInput implementation:
  https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_sinput.c
  — fetched from `main`; the same code shipped in the SDL 3.4.x release
  series (https://github.com/libsdl-org/SDL/releases/tag/release-3.4.0),
  first publicly available around 3.4.6.
* Linux HID introduction:
  https://www.kernel.org/doc/html/latest/hid/hidintro.html
* Linux HID configuration:
  https://github.com/torvalds/linux/blob/master/drivers/hid/Kconfig
* Linux PlayStation HID driver:
  https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c
* Linux Nintendo HID driver:
  https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c

## Hardware and testing status

No dedicated SInput product is owned. Most of the above is still derived from
the published specification and SDL's reference implementation, not from
capturing a real device's traffic — treat protocol details as best-effort
until further validated. One piece is no longer best-effort: see the BLE HIL
result below, which used a real ESP32-BLE-Gamepad running actual SInput
firmware, not a synthetic packet.

The planned test target is a DIY SInput-compatible controller built on
lemmingDev's ESP32-BLE-Gamepad firmware
(https://github.com/lemmingDev/ESP32-BLE-Gamepad), with hardware-in-the-loop
testing tracked separately in
https://github.com/LeeNX/ESP32-BLE-Gamepad-HIL against a Raspberry Pi 3 (see
[`rpi-hil.md`](rpi-hil.md) for the Pi-side packaging/build story).

### 2026-09-22: first real-hardware BLE HIL run, and a real bug it caught

Ran the module against a real SInput device for the first time, over
Bluetooth LE rather than USB: the `leenx-foss/Bluepad32/hil` rig's ESP32-C3
emulator (real `ESP32-BLE-Gamepad` v0.8.0 firmware, SInput mode, VID:PID
`2E8A:10C6`), paired to a Raspberry Pi 4B (`rp4b-ble-hil`, Debian trixie,
kernel `6.18.50+rpt-rpi-v8`) running this module. The `.deb` was cross-built
in a local `podman` container (`debian:trixie`, native arm64 on Apple
Silicon) pinned to the Pi's exact kernel-headers version, per
[`rpi-hil.md`](rpi-hil.md); vermagic matched exactly and `dpkg -i` +
`modprobe` loaded cleanly.

Two real findings, not simulated ones:

1. **The device table was USB-only.** `sinput_devices[]` only had
   `HID_USB_DEVICE()`; a Bluetooth-connected SInput device would never bind
   to this driver at all (it fell through to `hid-generic`). Fixed by adding
   a matching `HID_BLUETOOTH_DEVICE()` entry. Obvious in hindsight, invisible
   from a synthetic-packet test — `make check` cannot catch a missing bus
   match, only a real transport can.
2. **The face-button bit-to-name mapping was wrong**, inherited directly
   from SDL's own naming. `SDL_hidapi_sinput.c` defines
   `SINPUT_BUTTON_IDX_EAST = 0` and `SINPUT_BUTTON_IDX_SOUTH = 1`, and this
   driver copied those names verbatim into `SINPUT_BTN_IDX_*`. Commanding
   the emulator's button 1 (documented by the HIL rig's own
   `CLAUDE.md` as SInput's south/A button, and independently confirmed by
   watching its BLE-notified button number in the raw state report) produced
   `BTN_EAST` (Linux code 305) from this driver, not `BTN_SOUTH` (304) —
   confirmed for all four face buttons by commanding each individually over
   the emulator's NuS bridge (a Nordic UART Service GATT characteristic
   alongside its HID service on the same BLE connection) and reading back
   the resulting evdev key codes. The actual hardware order is south, east,
   west, north for bits 0-3 — SDL's own `#define` names do not match the
   gamepad button mapping string SDL itself builds at runtime. This matches
   a gotcha the Bluepad32 HIL project had already hit independently
   (`leenx-foss/Bluepad32/hil` `CLAUDE.md`: "SInput face buttons: bit 0 =
   A/south (SDL's constants name it 'east')"). Fixed by correcting the
   `SINPUT_BTN_IDX_{SOUTH,EAST,WEST,NORTH}` values in
   `sinput_protocol.h` to match the verified hardware order, re-flashed and
   re-verified clean (`press 1` -> `BTN_SOUTH`, etc.) after the fix.

Also confirmed clean on real hardware: the SInput `FEATURES` command/response
round-trip (protocol v1, poll rate 5000 us, both sticks, both triggers,
accel, gyro, all mapped buttons present per the usage mask), capability-driven
axis/IMU/button registration, and repeated probe/remove cycles across several
organic BLE reconnects with no kernel warnings, oopses, or leaks observed in
`dmesg`.

Not yet exercised: USB transport (BLE only so far), D-pad/bumper/stick-click/
trigger button bits individually (registration was observed via the feature
response and the noisy soak-generator capture, but not isolated
press/release-checked the way the four face buttons were), rumble/LEDs,
touchpad, IMU data values, and suspend/resume.

### 2026-09-22: `power_supply` integration, HIL-verified

Added a standard Linux `power_supply` battery device (`POWER_SUPPLY_PROP_STATUS/
PRESENT/CAPACITY/SCOPE`), following the `hid-playstation.c` pattern already
flagged as a design reference. `SI_PLUG_STATUS`/`SI_CHARGE_LEVEL` have no
capability bit in the feature response -- both bytes are always present in
every state report, present-battery or not -- so registration doesn't depend
on the FEATURES round-trip at all, unlike every other input this driver
exposes.

Wire semantics for `SI_PLUG_STATUS` (0=unknown, 1=no-battery, 2=charging,
3=charged, 4=on-battery) came from the Bluepad32 HIL rig's own
`ref-ble-gamepad/BleSInput.h`, which documents the real
`SDL_hidapi_sinput.c HandleStatePacket` switch directly rather than trusting
SDL's `#define` names on their own -- the same header independently flags
SDL's `SINPUT_BUTTON_IDX_EAST/SOUTH` as "unused dead constants", which lines
up exactly with the face-button bug found and fixed earlier today.

HIL-verified against the same ESP32-BLE-Gamepad emulator: drove `battery
<0-100>` and `power <b> <d> <c> <l>` commands over its NuS bridge and read
back `/sys/class/power_supply/sinput-battery-*/{status,present,capacity}`.
All three reachable states matched (`on-battery` -> discharging/present,
`charging` -> charging/present, `no-battery` -> not-charging/absent);
`charged` is untestable with this rig -- the emulator library's own comment
says it never produces that value ("no dedicated signal for 'finished
charging'"). One test-methodology trap along the way: `setPowerStateAll()`
on the emulator only updates a separate BLE Battery-Service characteristic
and never triggers a new SInput report by itself, so the power-state command
needs a following `battery <n>` (which does call `sendReport()`) to actually
flush a report carrying the change -- not a driver bug, just how that
firmware is wired.

Local review (three passes, not the CI matrix) caught three real issues
before any of this touched hardware a second time: `POWER_SUPPLY_PROP_PRESENT`
defaulted to true for the unconfirmed initial state (fixed to an allow-list
of confirmed-present plug states rather than a deny-list of confirmed-absent
ones); `SI_CHARGE_LEVEL` was trusted unclamped past the documented 0-100
range; and `battery_lock` was `spin_lock_init()`'d inside
`sinput_battery_init()`, which runs *after* `sinput_input_init()` already
opens the window for `raw_event` to reach it -- the exact same class of
"lock/flag not ready before `hid_hw_start()`" bug as the NULL `sdev->input`
fix from earlier today, recurring in new code. Moved the lock init and
default field values up into `sinput_probe()` alongside the `caps` defaults,
before `hid_hw_start()`, closing the window the same way.

### 2026-09-22: module split, player LED + RGB LED, HIL-verified

Split `sinput.c` (~600 lines) into per-subsystem files
(`sinput_core.c`/`sinput_input.c`/`sinput_battery.c`/`sinput_led.c` + a new
shared `sinput.h`), matching `hid-playstation.c`/`hid-nintendo.c`'s layout,
ahead of adding LEDs into the new structure. Pure refactor, re-verified on
`rp4b-ble-hil` before adding anything new: driver still binds over BLE, a
face button still decodes correctly, battery `power_supply` still reports
correct defaults.

Real Kbuild gotcha hit along the way: a composite module's `KBUILD_MODNAME`
gets C-token-pasted (e.g. in `MODULE_DEVICE_TABLE`), so `obj-m += src/sinput.o`
with a path-prefixed `src/sinput-y` at the repo-root `Makefile` fails to
compile (`error: expected '=', ',', ';', 'asm' or '__attribute__' before '/'
token`) -- the "/" in the derived token breaks the paste. Fixed with a
proper per-directory `src/Makefile` (`obj-m := sinput.o`, unprefixed) and
pointing the top-level `Makefile`'s `M=` at `src/` directly, the same way
in-tree multi-file drivers actually do this.

Added player LED and RGB LED as the first *output* features since the
FEATURES request -- both capability-gated on `caps.player_leds`/
`caps.rgb_led` (existing fields, never consulted by anything until now).
Wire format for both commands came from `leenx-foss/Bluepad32/hil`'s
`ref-ble-gamepad/BleSInput.cpp`'s `onWrite()` -- real reverse-engineered
host traffic, the same source that caught the face-button and plug-status
bugs fixed earlier this project. Two things worth recording:

* `SINPUT_CMD_PLAYER_LED`'s payload is a single scalar "player number" byte
  (confirmed from `SDL_hidapi_sinput.c`'s own send site: SDL's generic
  0-based joystick player index, `+1`'d, clamped 0-255), not a bitmask of
  discrete LEDs. User decision: still expose it as 4 separate on/off
  `led_classdev`s (PlayStation-style, more familiar to desktop LED tooling),
  translated to the wire's scalar via `hweight8()` of which LEDs are lit.
* `SINPUT_CMD_RGB_LED`'s payload is R/G/B each **0-63 (6-bit)**, not 0-255 --
  `BleSInput.h`'s own comment documents a real off-by-one bug that project
  hit and fixed in this exact byte layout, so this offset/range is trusted
  over guessing. Scaled via `(v*63+127)/255`.

Since LED writes can now happen at any time from userspace, unlike the
single FEATURES request that used to be the only output command, added an
`output_lock` mutex around every `hid_hw_output_report()` call. Deliberately
not `hid-playstation.c`'s workqueue-plus-combined-report pattern -- that
exists because DualSense batches rumble+LEDs+lightbar into one big periodic
report; SInput's commands are already discrete single-purpose packets, so a
direct blocking send under a mutex is the right fit.

HIL-verified on `rp4b-ble-hil` against the same ESP32-BLE-Gamepad emulator,
using its NuS `led?`/`rgb?` queries (built specifically for verifying
host-sent output commands): writing `1` then `1` to two different player
LEDs produced `event led 2` on the emulator (`hweight8` of two lit LEDs);
setting the RGB LED to pure red at full brightness produced
`event rgb r=63 g=0 b=0` (exact `(255*63+127)/255` match); a mixed color
(128, 64, 200) produced `r=32 g=16 b=49`, exactly matching the scaling
formula computed independently. All 5 LED class devices
(`<hid-id>:white:player-{1..4}`, `<hid-id>:rgb:indicator`) appeared under
`/sys/class/leds/` with the expected naming.

One more real gap review caught: `struct sinput_caps`'s own comment claims
"every field defaults to supported" for a device that never answers
FEATURES, but `player_leds`/`rgb_led` (unlike every axis/IMU field) were
never actually added to that default-true block in `sinput_probe()` --
harmless until now since nothing consulted them, but it meant a device that
times out on FEATURES (which this emulator does most of the time in this
rig, per every earlier HIL session) would silently get no LEDs at all.
Fixed by adding them to the same defaults block, consistent with every
other capability and harmless for an output-only feature (worst case is an
LED class device userspace can toggle that a real unsupported device
silently ignores).

A second review pass (after the first fix already looked hardware-verified)
caught a real write-write race: `sinput_player_led_set()` originally
computed the new player number under `output_lock` but sent it via a
*separate*, later lock/unlock in `sinput_send_output_command()`. Two
concurrent writes to different player LEDs could then have their sends
reordered relative to their state updates, leaving the device showing a
stale number that nothing would ever correct (no periodic resync for output
commands). Fixed by making `sinput_send_output_command()` require the
caller to already hold `output_lock` (`lockdep_assert_held()`), so every
call site holds the lock across its whole state-update-plus-send critical
section instead of composing two separately-locking steps. Re-verified
after the fix: player LED and dmesg cleanliness both re-checked on real
hardware, no regression.
