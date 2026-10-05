# Changelog

What each release adds, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[semver](https://semver.org/) (pre-1.0: a minor bump means new features, a
patch bump means fixes). "HIL-verified" means confirmed against a real SInput
device over BLE; see [`docs/research.md`](docs/research.md) for each run.

## [Unreleased]

## [0.4.0] - 2026-10-05

### Added

- Back paddles, power and misc 4-10 buttons are now reported instead of
  silently dropped. Paddles map to `BTN_GRIPL`/`BTN_GRIPR`/`BTN_GRIPL2`/
  `BTN_GRIPR2` (the codes `hid-steam` uses for the Steam Deck's back levers;
  `BTN_TRIGGER_HAPPY9`-`12` on kernels whose headers predate them). Power and
  misc map to `BTN_TRIGGER_HAPPY1`-`8`, deliberately not `KEY_POWER`, so a
  gamepad button can't shut the host down through systemd-logind.
- `make check` verifies that every SInput button bit is mapped exactly once.

### Fixed

- The `SInput IMU` input device now carries the HID vendor/product/version
  (it showed as `0000:0000`), so userspace can pair it with its gamepad.

### Testing

- First live module load/unload HIL run on the RPi 3B+ rig: 5 `modprobe` /
  `modprobe -r` cycles under a live BLE connection, `dmesg` clean, and first
  hardware coverage of every mapped button, all axes, IMU, touchpad, battery
  and rumble. It found the two issues fixed above.

## [0.3.0] - 2026-09-25

### Added

- Touchpad support: one input device per pad, or one shared multi-touch pad,
  depending on what the device advertises, with pad click as `BTN_LEFT`.
  HIL-verified.
- Touchpad devices are rebuilt live if a late `FEATURES` response changes
  the touchpad shape.
- Runtime VID/PID registration for DIY controllers, with no module rebuild:
  list IDs in `/etc/sinput/ids.conf` and a udev rule rebinds the device to
  `sinput`. Both `.deb` packages install it.
- Diagnostics through the kernel's dynamic debug facility: probe, raw report
  hex dumps, decoded state, touchpad reconcile decisions and every outgoing
  command.

## [0.2.2] - 2026-09-25

### Added

- IMU axes are scaled to physical units using the device's advertised
  accelerometer and gyro ranges.
- At probe, the driver checks the HID report descriptor's report sizes
  against the sizes it assumes, and logs a warning on any mismatch.
  HIL-verified: all three sizes match.

### Fixed

- `FEATURES` negotiation over BLE was unreliable because `probe()` never
  called `hid_device_io_start()` (issue #8). The request is now also retried
  rather than waited on once.

## [0.2.1] - 2026-09-23

### Added

- Rumble as a standard `FF_RUMBLE` force-feedback device. HIL-verified.
- Contact info and driver version in the module and package metadata.

## [0.2.0] - 2026-09-23

### Added

- Four player LEDs (`led_classdev`) and an RGB indicator
  (`led_classdev_multicolor`). HIL-verified. LEDs are optional: if one fails
  to register (for example on a kernel without
  `CONFIG_LEDS_CLASS_MULTICOLOR`), the controller still binds.
- GitLab release automation, matching GitHub's.

### Changed

- `sinput.c` split into one source file per subsystem.

## [0.1.3] - 2026-09-22

### Added

- Battery and charge state as a standard `power_supply` device.
  HIL-verified.

## [0.1.2] - 2026-09-22

### Fixed

- CI checks and DKMS scripts no longer hardcode the package version.

## [0.1.1] - 2026-09-22

### Added

- Button registration is gated on the device's advertised usage mask, like
  axes and IMU already were.

### Fixed

- The driver didn't bind over Bluetooth LE. Found on the first real-hardware
  run.
- Face buttons were mapped to the wrong evdev codes.
- A race with a late `FEATURES` response, and a window where a state report
  could reach a NULL input device.

## [0.1.0] - 2026-09-18

First release.

### Added

- HID driver for the SInput generic test VID/PID (`2E8A:10C6`) over USB and
  Bluetooth: `FEATURES` capability discovery with a fallback, state report
  decoding, an evdev gamepad (buttons, D-pad, sticks, triggers) and a
  separate IMU input device.
- `make check`: host-side protocol decode tests.
- Source DKMS `.deb` and precompiled Raspberry Pi 3 binary `.deb` packaging.
- CI on Gitea, GitHub and GitLab, and release tooling (`scripts/release.sh`
  plus release workflows that attach both `.deb` files).

[Unreleased]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.2.2...v0.3.0
[0.2.2]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/LeeNX/linux-hid-sinput/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/LeeNX/linux-hid-sinput/releases/tag/v0.1.0
