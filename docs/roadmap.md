# Roadmap

## 0.1 - bring-up
- [x] DKMS packaging
- [x] USB HID binding
- [x] basic state report decoding
- [x] evdev gamepad
- [x] prototype IMU event device

## 0.2 - protocol discovery
- [ ] decode report descriptor and verify report sizes
- [x] retrieve SInput capability/feature response
- [x] capability-driven axis registration (sticks, triggers, accel, gyro)
- [ ] capability-driven button registration
- [x] protocol version logging
- [x] polling-rate reporting

## 0.3 - Linux-native features
- [ ] `power_supply`
- [ ] force feedback
- [ ] player LED class
- [ ] RGB LED class
- [ ] touchpad / multitouch
- [ ] sensor integration

## 0.4 - reliability
- [ ] USB hotplug stress
- [ ] suspend/resume
- [ ] reset/reconnect
- [x] command timeout handling (single features request on probe only)
- [ ] output command serialization (needed once more than one command exists)
- [x] malformed packet handling (zero-length raw_event guard)
- [ ] 1 kHz soak tests

## 0.5 - HIL
- [ ] ESP32 SInput firmware fixture
- [ ] golden packet corpus
- [ ] evdev event comparison
- [ ] SDL event comparison
- [x] automated kernel matrix (Gitea Actions `kernel-build` job, bookworm/6.1
      and trixie/6.12; still compile-only, no real hardware yet)
- [x] host-side protocol decode test (`make check`, no hardware needed)
- [x] real dkms add/build/install/remove regression test in CI
      (`dkms-source-deb` job; caught two real bugs no other check did)
- [x] precompiled binary `.deb` for a real Raspberry Pi 3 target
      (`rpi3-binary-deb` job + [`rpi-hil.md`](rpi-hil.md)) -- built and inspected in
      CI, not yet installed on physical RPi3 hardware

## 0.6 - upstream assessment
- [ ] compare with kernel HID maintainer patterns
- [ ] SPDX/copyright cleanup
- [ ] check Linux stable API constraints
- [ ] decide whether upstream submission is worthwhile
