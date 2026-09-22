# Roadmap

## 0.1 - bring-up
- [x] DKMS packaging
- [x] USB HID binding
- [x] basic state report decoding
- [x] evdev gamepad
- [x] prototype IMU event device
- [x] release process (`scripts/release.sh` + Gitea/GitHub release
      workflows attaching both `.deb` flavors; see `RELEASE.md`) -- tooling
      only so far, v0.1.0 not yet cut

## 0.2 - protocol discovery
- [ ] decode report descriptor and verify report sizes
- [x] retrieve SInput capability/feature response
- [x] capability-driven axis registration (sticks, triggers, accel, gyro)
- [x] capability-driven button registration
- [x] protocol version logging
- [x] polling-rate reporting

## 0.3 - Linux-native features
- [x] `power_supply` -- plug status/charge level (no capability bit; always
      present in every state report) exposed as a standard battery
      power_supply device (STATUS/PRESENT/CAPACITY/SCOPE). Real-hardware
      HIL-verified: charging/discharging/no-battery all correctly mapped,
      including a conservative default (present=0 until a real report
      confirms otherwise). See `research.md`, 2026-09-22.
- [ ] force feedback
- [ ] player LED class
- [ ] RGB LED class
- [ ] touchpad / multitouch
- [ ] sensor integration

## 0.4 - reliability
- [ ] USB hotplug stress
- [ ] suspend/resume
- [ ] reset/reconnect (several *organic* BLE reconnects observed clean during
      the 2026-09-22 HIL run, see `research.md`, but not a deliberate test)
- [x] command timeout handling (single features request on probe only)
- [ ] output command serialization (needed once more than one command exists)
- [x] malformed packet handling (zero-length raw_event guard)
- [ ] 1 kHz soak tests

## 0.5 - HIL
- [x] ESP32 SInput firmware fixture -- not built in this repo; borrowed the
      sibling `leenx-foss/Bluepad32/hil` rig's ESP32-C3 emulator (real
      `ESP32-BLE-Gamepad` firmware, SInput mode)
- [ ] golden packet corpus
- [x] evdev event comparison -- manual, not automated: commanded each of the
      four face buttons individually over the emulator's NuS bridge and read
      back the resulting evdev key codes (see `research.md`, 2026-09-22);
      D-pad/bumpers/sticks/triggers not yet isolated the same way
- [ ] SDL event comparison
- [x] automated kernel matrix (Gitea Actions `kernel-build` job, bookworm/6.1
      and trixie/6.12; still compile-only, no real hardware yet)
- [x] host-side protocol decode test (`make check`, no hardware needed)
- [x] real dkms add/build/install/remove regression test in CI
      (`dkms-source-deb` job; caught two real bugs no other check did)
- [x] precompiled binary `.deb` for a real Raspberry Pi-family target --
      built for `rp4b-ble-hil` (Raspberry Pi 4B, matches RPi3's `rpi-v8`
      kernel variant) in a local `podman` container pinned to its exact
      kernel headers, `dpkg -i` + `modprobe` installed and loaded clean; see
      `research.md`. CI's `rpi3-binary-deb` job still only builds/inspects,
      not installed via CI on physical hardware.
- [x] real device bind test over BLE -- `sinput` (not `hid-generic`) claimed
      a real SInput device by vendor/product ID over Bluetooth, feature
      response parsed correctly, capability-driven registration confirmed
      (see `research.md`, 2026-09-22)

## 0.6 - upstream assessment
- [ ] compare with kernel HID maintainer patterns
- [ ] SPDX/copyright cleanup
- [ ] check Linux stable API constraints
- [ ] decide whether upstream submission is worthwhile
