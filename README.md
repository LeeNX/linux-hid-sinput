# SInput Linux DKMS driver

Experimental out-of-tree Linux HID driver for the SInput gamepad protocol.

This repository is deliberately a **starter** rather than a finished upstream-quality
driver. The first goal is to establish a clean DKMS/module test loop against SInput
devices, then add the protocol features that Linux's generic HID/gamepad path does
not expose cleanly.

**No dedicated SInput product is owned.** Development is based on the published
[SInput specification](https://docs.handheldlegend.com/s/sinput) and Hand Held
Legend's [SInput-HID](https://github.com/HandHeldLegend/SInput-HID) reference
repository, cross-checked against SDL's SInput HIDAPI implementation (the same
code shipped in the [SDL 3.4.x release series](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.0),
first publicly available around 3.4.6). Most protocol details are still
best-effort until further validated, but the module has now been run once
against a real ESP32-BLE-Gamepad SInput device over BLE, which caught (and
fixed) a real transport gap and a real button-mapping bug — see
[`docs/research.md`](docs/research.md#2026-09-22-first-real-hardware-ble-hil-run-and-a-real-bug-it-caught).
The planned test target is a DIY SInput-compatible controller built
on [lemmingDev/ESP32-BLE-Gamepad](https://github.com/lemmingDev/ESP32-BLE-Gamepad),
with hardware-in-the-loop testing tracked in
[LeeNX/ESP32-BLE-Gamepad-HIL](https://github.com/LeeNX/ESP32-BLE-Gamepad-HIL)
against a Raspberry Pi 3 — see [`docs/rpi-hil.md`](docs/rpi-hil.md) for the Pi-side story.

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

See [`docs/research.md`](docs/research.md) for sources and design notes.

## Current prototype

The module:

1. binds to the SInput generic test VID/PID (`2E8A:10C6`) over USB or Bluetooth;
2. uses the Linux HID framework;
3. cross-checks the parsed HID report descriptor's own field sizes against
   every report-size constant this driver assumes, logging a warning
   (never fatal) on any mismatch -- HIL-verified: all three report sizes
   (state, command response, output command) matched exactly on real
   hardware (see [`docs/research.md`](docs/research.md));
4. creates an explicit evdev input device instead of relying on `hid-generic`;
5. sends the SInput `FEATURES` command on probe and decodes the response
   (protocol version, polling rate, sticks/triggers/accel/gyro/rumble/LED
   support) with a short timeout, falling back to "assume everything is
   present" if the device never answers;
6. decodes the SInput state report;
7. exposes only the buttons, D-pad, and sticks/triggers the feature response's
   usage mask (or the fallback) says exist;
8. exposes a separate IMU input device, with only the accel/gyro axes the
   device actually advertises;
9. exposes battery/charge state as a standard Linux `power_supply` battery
   device (always present in every state report, no capability bit);
10. exposes player LEDs (4 on/off `led_classdev`s) and an RGB indicator
    (`led_classdev_multicolor`) as output commands, gated on the feature
    response the same way the input side is;
11. exposes rumble as a standard `FF_RUMBLE` force-feedback device
    (`input_ff_create_memless()`), gated on `caps.rumble` -- HIL-verified
    against real hardware: captured BLE traffic showed the exact expected
    output-report bytes (see [`docs/research.md`](docs/research.md));
12. exposes one or more touchpad input devices (one per pad, or one shared
    multi-touch pad, depending on what the feature response says), rebuilt
    live if a later response changes that shape -- HIL-verified against
    real hardware (see [`docs/research.md`](docs/research.md)).

The source lives in `src/`, one file per subsystem (`sinput_core.c` for
probe/dispatch and the FEATURES/output-command plumbing, `sinput_input.c`,
`sinput_battery.c`, `sinput_led.c`, `sinput_ff.c`, `sinput_touchpad.c`),
Kbuild-linked into a single `sinput.ko` -- see `src/Makefile`.

The feature-response layout is reverse-derived from SDL's SInput HIDAPI
driver (see [`docs/research.md`](docs/research.md)), not from a stable
spec, so treat the byte offsets as best-effort.

## Adding your own VID/PID

The driver's built-in `id_table` ([`src/sinput_core.c`](src/sinput_core.c))
only matches the generic SInput testing VID/PID (`2E8A:10C6`). If you're
building your own SInput-compatible controller, do **not** ship it under
that VID/PID or under Raspberry Pi Foundation's `2E8A` -- register a real
product ID instead. If you don't want to go through full USB-IF vendor
registration, [pid.codes](https://pid.codes/) is a shared VID (`1209`) for
open-source hardware projects; the SInput spec itself recommends a
device-specific PID for real products (see [Research snapshot](#research-snapshot)).

There's no need to patch and rebuild the module for this. The Linux HID bus
already supports adding device IDs to a driver at runtime (`new_id`/`bind`
under `/sys/bus/hid/drivers/`), which this repo wraps in a udev rule so it
happens automatically:

* **Persistent (recommended):** add a `vendor:product` line, in hex, to
  `/etc/sinput/ids.conf` (see the comments in that file for the exact
  format), then unplug/replug the device (or re-pair it, for Bluetooth).
  The installed udev rule ([`udev/99-sinput.rules`](udev/99-sinput.rules),
  via [`scripts/sinput-claim-id.sh`](scripts/sinput-claim-id.sh)) reads that
  file on every matching HID "add" event and rebinds the device from
  whatever currently claims it (`hid-generic`, in the common case) to
  `sinput`. No module rebuild, no reboot.
  * The two `.deb` packages install this automatically.
  * For a plain `make`/[`dkms-install.sh`](scripts/dkms-install.sh) install,
    run [`scripts/install-udev-support.sh`](scripts/install-udev-support.sh)
    once (as root) to install the rule, helper script, and config file.
* **One-off/manual test**, without installing anything:

  ```sh
  echo "0003 2e8a 1a01" | sudo tee /sys/bus/hid/drivers/sinput/new_id
  # find the device's current hid-generic instance name, e.g. from:
  ls /sys/bus/hid/drivers/hid-generic/
  echo -n "0003:2E8A:1A01.000X" | sudo tee /sys/bus/hid/drivers/hid-generic/unbind
  echo -n "0003:2E8A:1A01.000X" | sudo tee /sys/bus/hid/drivers/sinput/bind
  ```

  (bus `0003` is USB, `0005` is Bluetooth; find your device's actual
  vendor/product/instance name under `/sys/bus/hid/drivers/hid-generic/`.)

## Diagnostics

The driver has no custom debug ABI (no module parameter, no debugfs file) --
it uses the kernel's own dynamic debug facility (`CONFIG_DYNAMIC_DEBUG`,
enabled on effectively every distro kernel) for every diagnostic message.
Off by default, near-zero overhead when off (jump-label gated), and doesn't
need a module reload or reboot to turn on or off.

Enable everything the driver logs:

```sh
echo 'module sinput +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
```

Or scope it to one thing, e.g. only the raw report hex dump (the highest-
signal single thing to enable when a decode looks wrong -- see exactly what's
on the wire without an external BLE/USB capture):

```sh
echo 'func sinput_raw_event +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
```

`dmesg -w` (or `journalctl -kf`) to watch it live. Turn it back off the same
way, with `-p` instead of `+p`. See
[Documentation/admin-guide/dynamic-debug-howto.rst](https://www.kernel.org/doc/html/latest/admin-guide/dynamic-debug-howto.html)
for the full `<query>` syntax (by function, file, line range, or module).

What's covered: probe (device identity, FEATURES request/response retries),
every raw incoming report (hex dump), decoded gamepad/touchpad state per
report, touchpad reconcile decisions (see
[`src/sinput_touchpad.c`](src/sinput_touchpad.c)'s top comment for why that
can happen more than once per device), and every outgoing command (rumble,
player LEDs, RGB LED).

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
[`src/sinput_protocol.h`](src/sinput_protocol.h) against synthetic packets; it is not a substitute for
testing against real hardware.

Install through DKMS directly ([`scripts/dkms-install.sh`](scripts/dkms-install.sh);
builds against whatever kernel is currently running):

```sh
sudo ./scripts/dkms-install.sh
```

Remove ([`scripts/dkms-remove.sh`](scripts/dkms-remove.sh)):

```sh
sudo ./scripts/dkms-remove.sh
```

### Packaging: two `.deb` flavors

* **Source DKMS `.deb`** — [`./scripts/build-source-deb.sh`](scripts/build-source-deb.sh) `[output-dir]`. Ships
  the source under `/usr/src/sinput-<version>/`; its `postinst` hands off to
  dkms's own `/usr/lib/dkms/common.postinst` to build and install for
  whatever kernel(s) are present, and keeps working across kernel upgrades.
  Needs `build-essential` and matching kernel headers on the *target*, not
  the build machine.
* **Precompiled binary `.deb`** — [`./scripts/build-binary-deb.sh`](scripts/build-binary-deb.sh) `<KDIR>
  [output-dir]`. Ships a prebuilt `sinput.ko` for one exact kernel release; no
  compiler needed on the target at all, but it will not load on any other
  kernel build (vermagic mismatch). See [`docs/rpi-hil.md`](docs/rpi-hil.md) for how to do this
  for a Raspberry Pi 3 target, including two real gotchas it took actually
  testing in containers to find (Raspberry Pi's kernel-headers packaging
  differs completely between OS releases, and a real apt-trust bootstrap
  failure on the current release).

Both `.deb` flavors also install the runtime VID/PID support (udev rule,
helper script, `/etc/sinput/ids.conf`) described in
[Adding your own VID/PID](#adding-your-own-vidpid) — nothing extra to run.

Both scripts are plain POSIX `sh`, no `dh`/`dpkg-buildpackage` involved —
`dkms mkdeb`/`mkbmdeb`, despite being documented in Debian's own
`/usr/share/doc/dkms/HOWTO.Debian`, no longer exist in the dkms shipped with
current Debian (verified against 3.0.10); the doc is stale.

## CI

GitHub is the canonical public repo. [GitLab](https://gitlab.com/leet/linux-hid-sinput)
is a public mirror. Gitea is a self-hosted, internal mirror (also used for
local CI and for reaching self-hosted ARM64 hardware, including the
Raspberry Pi HIL runner) rather than a public home for the project.

Three separate CI configs, one per platform this repo can be pushed to —
[`.gitea/workflows/ci.yml`](.gitea/workflows/ci.yml), [`.github/workflows/ci.yml`](.github/workflows/ci.yml), [`.gitlab-ci.yml`](.gitlab-ci.yml) —
rather than one shared file. They run the same six checks below via the
same underlying scripts, but the platform-specific plumbing genuinely
differs (see each file's header comment for specifics): action versions
that work on one platform and not another, how JavaScript actions get a
`node` binary inside a job's container (or, on GitLab, don't need one at
all — checkout and artifacts are native runner features there, not
actions), and runner/architecture labels. All three were dry-run locally
against real container images before being committed (`act` for the
GitHub file, manual container replay for GitLab since `gitlab-runner
exec` has been removed from current versions); only a real push confirms
each platform's actual runners/registries behave the same way.

Every push/PR (and manual trigger) runs:

* `protocol-check` — `make check` (the decode test above)
* `shellcheck` — lints `scripts/*.sh`
* `checkpatch` — Linux kernel style check against [`src/`](src/). `LINUX_VERSION_CODE`
  and `CONSTANT_COMPARISON` are ignored: those checkpatch rules assume in-tree
  code that targets a single kernel version, but this driver is out-of-tree
  and has to compile across a range of kernel versions.
* `kernel-build` — compiles the module against real kernel headers on a small
  matrix of Debian releases (currently bookworm/6.1 and trixie/6.12), plus a
  `W=1` extra-warnings pass and a `sparse` pass. This is a compile check only;
  it cannot catch runtime/protocol bugs without a real SInput device.
* `dkms-source-deb` — builds the source `.deb` above and actually
  `dpkg -i`/`dpkg -r`s it, i.e. runs the real dkms add/build/install/remove
  path, not just a compile. This caught two real bugs during development
  that every other check above missed ([`dkms.conf`](dkms.conf)'s `MAKE` line ignoring
  the kernel dkms was actually targeting, and the module landing in the
  wrong build-output location) — worth keeping as a regression test rather
  than trimming down to "just build the module" again.
* `rpi3-binary-deb` — builds the binary `.deb` for a Raspberry Pi 3 (see
  [`docs/rpi-hil.md`](docs/rpi-hil.md)) and uploads it as a downloadable CI artifact. Needs an
  arm64 runner (`linux-headers-rpi-v8` isn't published for amd64): pinned to
  the `arm64` label on Gitea (already registered on the RPi4/5 runner) and
  `ubuntu-24.04-arm` on GitHub (free hosted arm64 runner for public repos,
  unverified as of writing). The GitLab file's `tags: [saas-linux-small-arm64]`
  is a guess at GitLab.com's shared arm64 runner tag — check
  <https://docs.gitlab.com/ci/runners/hosted_runners/linux/> for the current
  name/tier before relying on it, or point it at a self-hosted arm64 runner
  instead (the same RPi4/5 box could double as one).

To reproduce the kernel-build job locally without Docker/Gitea, install
`linux-headers-$(dpkg --print-architecture)` in a matching container and run
`make KDIR=/lib/modules/$(ls /lib/modules)/build`.

### Keeping CI dependencies current

* **GitHub**: [`.github/dependabot.yml`](.github/dependabot.yml) watches `.github/workflows/*.yml` for
  new `actions/*` releases and opens PRs weekly.
* **Gitea**: [`.gitea/workflows/renovate.yml`](.gitea/workflows/renovate.yml) runs [Renovate](https://docs.renovatebot.com/)
  itself weekly (self-hosted platforms don't get a hosted Dependabot/Renovate
  app), configured by [`renovate.json`](renovate.json) at the repo root. Needs two one-time
  manual steps: create a Gitea access token (a dedicated bot account keeps PR
  attribution clean) with repo read/write scope as a `RENOVATE_TOKEN` secret,
  **and** a read-only github.com personal access token as
  `RENOVATE_GITHUB_COM_TOKEN` — confirmed by actually running Renovate
  locally (`RENOVATE_PLATFORM=local`) that the second one is genuinely
  required, not just nice-to-have for rate limits: without it, Renovate's
  github-actions manager hard-skips every `actions/*` dependency
  (`skipReason: github-token-required`) no matter which platform it's
  opening PRs against. Until both secrets exist, the job runs and
  fails/no-ops at the relevant step rather than silently doing nothing.
  [`renovate.json`](renovate.json) also carries the one rule that has to stay
  platform-specific: it caps `actions/upload-artifact` below v4 in
  [`.gitea/workflows/ci.yml`](.gitea/workflows/ci.yml) only (the Results-API backend issue above),
  while leaving [`.github/workflows/ci.yml`](.github/workflows/ci.yml) free to track latest.

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

## Releases

See [`RELEASE.md`](RELEASE.md). Short version: `scripts/release.sh 0.1.0`
bumps `dkms.conf`, commits, and tags locally; pushing the tag triggers
`.gitea/workflows/release.yml` and `.github/workflows/release.yml` to build
both `.deb` flavors. GitHub attaches them to a Release automatically; Gitea
uploads them as a downloadable artifact for now (a container-network DNS
issue on that runner blocks it from reaching its own release API — see
`RELEASE.md`), so creating the actual Gitea Release from the pushed tag is
currently a manual step.

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
* [x] source DKMS `.deb` packaging ([`scripts/build-source-deb.sh`](scripts/build-source-deb.sh)), with a
      real dpkg-i/dpkg-r regression test in CI
* [x] precompiled binary `.deb` packaging for one exact kernel build
      ([`scripts/build-binary-deb.sh`](scripts/build-binary-deb.sh)), verified for Raspberry Pi 3 (see
      [`docs/rpi-hil.md`](docs/rpi-hil.md))
* [x] capability-driven *button* mapping (registration and reporting both
      gated on the feature response's usage mask, falling back to "assume
      every mapped button exists" like the other capabilities)
* [x] battery / `power_supply` (HIL-verified against real hardware; see
      [`docs/research.md`](docs/research.md))
* [x] force feedback / rumble output command (`FF_RUMBLE` via
      `input_ff_create_memless()`, HIL-verified against real hardware; see
      [`docs/research.md`](docs/research.md))
* [x] player LEDs (HIL-verified against real hardware; see
      [`docs/research.md`](docs/research.md))
* [x] RGB LED (HIL-verified against real hardware; see
      [`docs/research.md`](docs/research.md))
* [x] touchpads (one input_dev per pad or per finger depending on the
      feature response's shape, reconciled live if a late response changes
      it; HIL-verified against real hardware; see
      [`docs/research.md`](docs/research.md))
* [x] output-command serialization and locking (`output_lock` mutex, added
      once player/RGB LED became the second and third output commands; see
      [`docs/research.md`](docs/research.md))
* [x] runtime VID/PID registration for DIY builds, no module rebuild (udev
      rule + `/etc/sinput/ids.conf`; see
      [Adding your own VID/PID](#adding-your-own-vidpid))
* [x] diagnostics via the kernel's dynamic debug facility, no custom debug
      ABI (see [Diagnostics](#diagnostics))
* [ ] USB + Bluetooth transport testing (Bluetooth binding added and verified
      against a real device over BLE; USB still untested; see
      [`docs/research.md`](docs/research.md))
* [ ] suspend/resume
* [ ] kernel version compatibility matrix (compile-verified on 6.1 and 6.12; now also
      real-hardware-verified on 6.18 via [`docs/rpi-hil.md`](docs/rpi-hil.md))
* [x] HIL tests using ESP32 SInput firmware (real device, BLE, against
      `rp4b-ble-hil`; see [`docs/research.md`](docs/research.md))
* [ ] evaluate whether an upstream Linux HID driver is justified (where would this posted/reported?)
