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
4. decodes the SInput state report;
5. exposes buttons, D-pad, sticks and analog triggers;
6. exposes a separate IMU input device when IMU capability is advertised by the
   feature response;
7. records battery/power fields for future `power_supply` integration.

Feature-report discovery, rumble, player LEDs, RGB LEDs, touchpads, and a proper
`power_supply` class device are intentionally left as follow-up work.

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
sudo insmod sinput.ko
```

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
* [ ] capability report discovery
* [ ] dynamic capability-driven input mapping
* [ ] battery / `power_supply`
* [ ] IMU / Linux sensor representation
* [ ] force feedback
* [ ] player LEDs
* [ ] RGB LED
* [ ] touchpads
* [ ] output-command serialization and locking
* [ ] USB + Bluetooth transport testing
* [ ] suspend/resume
* [ ] kernel version compatibility matrix
* [ ] HIL tests using ESP32 SInput firmware
* [ ] evaluate whether an upstream Linux HID driver is justified
