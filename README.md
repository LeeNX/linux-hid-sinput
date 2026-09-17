# SInput Linux DKMS driver

Experimental out-of-tree Linux HID driver for the SInput gamepad protocol.

This repository is deliberately a **starter** rather than a finished upstream-quality
driver. The first goal is to establish a clean DKMS/module test loop against SInput
devices, then add the protocol features that Linux's generic HID/gamepad path does
not expose cleanly.

## Research snapshot

As of 2026-09-16:

* SInput is a HID format developed by Hand Held Legend and documented in the SInput
  HID development specification.
* The documented generic testing VID/PID is `0x2E8A:0x10C6`; the specification
  recommends registering a device-specific PID for real products.
* SDL has a native SInput HIDAPI implementation on its main branch.
* SDL's implementation currently understands a 64-byte input report, 48-byte
  command/output report, report IDs 1/2/3, capability discovery, dynamic button
  mappings, battery/power state, IMU data, haptics and player/RGB LED commands.
* The Linux kernel already has a mature HID framework and gamepad-specific drivers
  such as `hid-playstation` and `hid-nintendo`. Those drivers are useful design
  references for a future in-tree driver.
* DKMS is useful here as the experimental delivery mechanism while the protocol
  and Linux-facing API are being worked out.

See `docs/research.md` for sources and design notes.

## Current prototype

The module:

1. binds to the SInput generic test VID/PID (`2E8A:10C6`);
2. uses the Linux HID framework;
3. creates an explicit evdev input device instead of relying on `hid-generic`;
4. sends the SInput `FEATURES` command on probe and decodes the response
   (protocol version, polling rate, sticks/triggers/accel/gyro/rumble/LED
   support) with a short timeout, falling back to "assume everything is
   present" if the device never answers;
5. decodes the SInput state report;
6. exposes buttons, D-pad, and only the sticks/triggers the feature response
   (or the fallback) says exist;
7. exposes a separate IMU input device, with only the accel/gyro axes the
   device actually advertises;
8. records battery/power fields for future `power_supply` integration.

Rumble, player LEDs, RGB LEDs, touchpads, and a proper `power_supply` class
device are intentionally left as follow-up work. The feature-response layout
is reverse-derived from SDL's SInput HIDAPI driver (see `docs/research.md`),
not from a stable spec, so treat the byte offsets as best-effort.

## Why a kernel driver?

SDL already supports SInput through HIDAPI, so a kernel driver is not required for
SDL applications. The interesting reason to build one is to investigate a proper
Linux-native representation:

* evdev input
* Linux force feedback
* LED class / multicolor LEDs
* `power_supply`
* sensor input
* suspend/resume
* consistent behaviour for applications that do not use SDL/HIDAPI

The project should therefore avoid simply duplicating SDL in kernel space.

## Build

On Debian/Ubuntu:

```sh
sudo apt install build-essential dkms linux-headers-$(uname -r)
```

Build locally:

```sh
make
sudo insmod src/sinput.ko
```

Run the host-side protocol decode checks (no kernel headers required, works
on any machine including macOS/CI):

```sh
make check
```

This only exercises the report byte-offset/flag math in
`src/sinput_protocol.h` against synthetic packets; it is not a substitute for
testing against real hardware.

## CI

`.gitea/workflows/ci.yml` runs on every push/PR and covers everything that is
possible without real hardware:

* `protocol-check` — `make check` (the decode test above)
* `shellcheck` — lints `scripts/*.sh`
* `checkpatch` — Linux kernel style check against `src/`. `LINUX_VERSION_CODE`
  and `CONSTANT_COMPARISON` are ignored: those checkpatch rules assume in-tree
  code that targets a single kernel version, but this driver is out-of-tree
  and has to compile across a range of kernel versions.
* `kernel-build` — compiles the module against real kernel headers on a small
  matrix of Debian releases (currently bookworm/6.1 and trixie/6.12), plus a
  `W=1` extra-warnings pass and a `sparse` pass. This is a compile check only;
  it cannot catch runtime/protocol bugs without a real SInput device.

To reproduce the kernel-build job locally without Docker/Gitea, install
`linux-headers-$(dpkg --print-architecture)` in a matching container and run
`make KDIR=/lib/modules/$(ls /lib/modules)/build`.

Or install through DKMS:

```sh
sudo ./scripts/dkms-install.sh
```

Remove:

```sh
sudo ./scripts/dkms-remove.sh
```

Inspect:

```sh
lsmod | grep sinput
dmesg | tail -n 100
cat /proc/bus/input/devices
```

For an attached controller, also inspect:

```sh
udevadm info /sys/bus/hid/devices/*/hidraw*/device 2>/dev/null
evtest
```

## Safety during development

This is an experimental kernel module. Keep a second console available and test
first on a disposable Linux installation or VM. Do not use the module's current
feature set as evidence that SInput is ready for upstream Linux.

## Planned stages

* [x] DKMS skeleton
* [x] HID binding
* [x] basic SInput state report decoding
* [x] evdev buttons/D-pad/axes
* [x] capability/feature report request + response parsing
* [x] capability-driven axis + IMU registration (sticks, triggers, accel, gyro)
* [x] protocol version / polling-rate logging
* [x] host-side protocol decode test (`make check`)
* [ ] capability-driven *button* mapping (buttons are still always registered)
* [ ] battery / `power_supply`
* [ ] force feedback / rumble output command
* [ ] player LEDs
* [ ] RGB LED
* [ ] touchpads
* [ ] output-command serialization and locking (only a single request-on-probe today)
* [ ] USB + Bluetooth transport testing
* [ ] suspend/resume
* [ ] kernel version compatibility matrix (compile-verified on 6.1 and 6.12 so far; no real hardware yet)
* [ ] HIL tests using ESP32 SInput firmware
* [ ] evaluate whether an upstream Linux HID driver is justified (where would this posted/reported?)
