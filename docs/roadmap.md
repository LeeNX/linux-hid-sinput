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
- [x] decode report descriptor and verify report sizes -- `hid_parse()`'s
      result cross-checked at probe time (`sinput_verify_report_sizes()`)
      against every `*_REPORT_SIZE` constant `sinput_protocol.h` assumes.
      HIL-verified against `rp4b-ble-hil`: all three report sizes (state,
      command response, output command) matched exactly. Report-level only
      -- individual field offsets within each report are still not decoded
      from the descriptor itself. See `research.md`, 2026-09-24.
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
- [x] force feedback -- `sinput_ff.c`, `FF_RUMBLE` via
      `input_ff_create_memless()`, capability-gated on `caps.rumble`.
      HIL-verified against `rp4b-ble-hil`: captured BLE traffic showed the
      exact expected output-report bytes, and the emulator's own
      unprompted NuS notification confirmed the same values received. Also
      caught and fixed a real bug this HIL run: `caps.rumble` was missing
      from `sinput_probe()`'s default-capability block, the same class of
      bug already fixed once for player/RGB LED. See `research.md`,
      2026-09-23.
- [x] player LED class -- N (4) on/off `led_classdev`s, PlayStation-style,
      translated to SInput's single scalar "player number" wire value via
      `hweight8()` of which LEDs are lit. HIL-verified: emulator's `led?`
      NuS query confirms the exact number received. See `research.md`,
      2026-09-22.
- [x] RGB LED class -- one `led_classdev_multicolor`, scaled from Linux's
      0-255 per channel to the wire's 0-63 (6-bit) range. HIL-verified
      against the emulator's `rgb?` query, exact byte match including a
      non-trivial mixed color. See `research.md`, 2026-09-22.
- [ ] touchpad / multitouch
- [ ] sensor integration

## 0.4 - reliability
- [ ] USB hotplug stress
- [ ] suspend/resume
- [ ] reset/reconnect (several *organic* BLE reconnects observed clean during
      the 2026-09-22 HIL run, see `research.md`, but not a deliberate test)
- [x] command timeout handling (single features request on probe only)
- [x] output command serialization -- `output_lock` mutex added once player
      LED/RGB LED became the second and third output commands (FEATURES was
      the only one before). Caller-held around the whole state-update-plus-
      send critical section, not just the send, after review caught a
      write-write race that could leave the device showing a stale value
      with nothing to ever correct it. See `research.md`, 2026-09-22.
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
