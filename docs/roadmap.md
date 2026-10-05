# Roadmap

Open work only. What has shipped, and in which version, is in
[`CHANGELOG.md`](../CHANGELOG.md); the README's
[Planned stages](../README.md#planned-stages) is the at-a-glance checklist,
and [`research.md`](research.md) has the evidence behind every "verified".

## Hardware verification gaps

Things the driver implements but no real-hardware run has confirmed yet.

- [ ] Paddle, power and misc 4-10 buttons on their evdev codes. Their wire
      bits were seen on the raw report in the 2026-10-04 run, but that run
      predates the mapping, so the evdev side is unobserved.
- [ ] LED class devices driven through sysfs on the 3B+ rig (their
      `brightness` files are root-only and the HIL wrapper doesn't expose
      them). Player and RGB LED were verified on `rp4b-ble-hil` via NuS
      queries on 2026-09-22.
- [ ] USB transport. Every hardware run so far has been BLE.
- [ ] Two independent touchpads (`touchpad_count > 1`) and the touchpad
      shape-change reconcile path. Neither is reachable with the emulators'
      fixed 1-pad/2-finger config; verified by code and lock review only.
- [ ] `FF_GAIN`, and effect duration/envelope fields on rumble.

## Reliability

- [ ] suspend/resume
- [ ] deliberate reset/reconnect test (organic BLE reconnects have been
      clean, and live `modprobe`/`modprobe -r` under a connection passed 5
      cycles on 2026-10-04, but a forced disconnect/reconnect hasn't been
      tested on purpose)
- [ ] USB hotplug stress
- [ ] 1 kHz soak test

## Protocol

- [ ] Decode field offsets from the HID report descriptor rather than
      `sinput_protocol.h`'s constants. Today only the report *sizes* are
      cross-checked at probe.
- [ ] Revisit the feature-response layout if Hand Held Legend publishes it
      in the spec; it is currently reverse-derived from SDL.

## Test automation

- [ ] golden packet corpus for `make check`
- [ ] SDL event comparison (same device, evdev vs SDL's HIDAPI driver)
- [ ] CI install of the binary `.deb` on physical hardware (CI's
      `rpi3-binary-deb` job builds and inspects it only)
- [ ] run `tester/sinput_hil.py` (ESP32-BLE-Gamepad-HIL) from CI on the
      3B+ rig instead of by hand
- [ ] kernel matrix beyond Debian bookworm/6.1 and trixie/6.12
      (compile-only today; 6.18 is covered by the HIL rigs, not CI)

## Upstream assessment

- [ ] compare with kernel HID maintainer patterns
- [ ] SPDX/copyright cleanup
- [ ] check Linux stable API constraints
- [ ] decide whether upstream submission is worthwhile, and where it would
      be posted
