# Raspberry Pi HIL: precompiled binary `.deb`

This documents how [`scripts/build-binary-deb.sh`](../scripts/build-binary-deb.sh) produces a precompiled
`sinput.ko` `.deb` for a Raspberry Pi 3 running 64-bit Raspberry Pi OS,
built in a container (including on a Gitea Actions runner hosted on an
RPi4/5), with no compiler needed on the target Pi.

This is the "binary" install path. It complements, rather than replaces,
the DKMS source path ([`scripts/dkms-install.sh`](../scripts/dkms-install.sh)): DKMS rebuilds
automatically whenever the Pi's kernel is upgraded, at the cost of needing
`build-essential` + kernel headers on the Pi. A precompiled binary `.deb`
needs nothing but `dpkg -i` on the Pi, at the cost of being tied to one
exact kernel build — install the wrong one and `insmod`/`modprobe` will
simply refuse to load it (vermagic mismatch), which is safe but useless.

## Why this is harder than it looks

A kernel module's `vermagic` string encodes the exact kernel release plus
build flags (SMP, preempt, modversions, arch). It has to match the
*running* kernel exactly. Building a `.ko` on an RPi4/5 host and assuming
it'll work on an RPi3 does not hold:

* RPi3 and RPi4 both use Raspberry Pi's `rpi-v8` 64-bit kernel variant.
* RPi5 uses a different variant, `rpi-2712` (its BCM2712 SoC includes the
  RP1 southbridge chip, which needs different kernel support). A module
  built against `rpi-2712` headers will not load on an RPi3 or RPi4.
* Even within the same variant, the exact kernel *version* still has to
  match (e.g. `6.18.50+rpt-rpi-v8` and `6.12.75+rpt-rpi-v8` are different
  builds with different vermagic strings).

So building on RPi4/5 hardware is fine and desirable (native ARM, no
QEMU), but only because the build uses kernel headers matching the RPi3
*target's* exact kernel, not whatever kernel the RPi4/5 build host happens
to be running.

## Raspberry Pi OS release matters: bookworm vs trixie

As of this writing, `archive.raspberrypi.com/debian` serves both:

* `bookworm` (oldstable) — ships a single rolling `raspberrypi-kernel-headers`
  package with no version in its name. Only the current version is ever
  installable; there is no way to `apt-get install` an older, historical
  build once it's been superseded. Header directory naming:
  `/usr/src/linux-headers-<version>-v8+`.
* `trixie` (current stable) — ships proper versioned packages:
  `linux-headers-<version>+rpt-rpi-v8` for each still-published build,
  plus a `linux-headers-rpi-v8` meta-package that depends on whichever is
  current. This means specific historical versions can be pinned exactly,
  which bookworm cannot do. Header directory naming:
  `/usr/src/linux-headers-<version>+rpt-rpi-v8`.

Check which release a given Pi is running with `cat /etc/os-release` (look
at `VERSION_CODENAME`). A freshly imaged Pi today will almost certainly be
on trixie.

**Practical implication for bookworm**: since only the current version is
ever available, the Pi's installed kernel and the build container's
headers can only match if the Pi is kept fully updated (`apt update &&
apt full-upgrade && reboot`) close to when the `.deb` is built. There is
no way to target an older, frozen bookworm kernel build after the fact.

**Practical implication for trixie**: an exact version can be pinned on
both sides (Pi and build container), so a stable HIL rig can freeze on a
known-good kernel build indefinitely, independent of whatever the
Raspberry Pi apt repo currently considers "latest".

## The apt keyring gotcha on trixie

Fetching `https://archive.raspberrypi.com/debian/raspberrypi.gpg.key`
directly and `gpg --dearmor`-ing it (the "normal" ad hoc way to bootstrap
a third-party apt repo) fails on trixie:

```
W: OpenPGP signature verification failed: ... Sub-process /usr/bin/sqv
   returned an error code (1) ... Policy rejected non-revocation
   signature (PositiveCertification) requiring second pre-image
   resistance because: SHA1 is not considered secure since 2026-02-01
```

That raw key file is the old (2012) key, whose self-certification uses
SHA-1, which trixie's apt (via `sqv`) now refuses outright. Real Raspberry
Pi OS images do not hit this because they ship the keyring package
pre-installed rather than bootstrapping it over HTTPS at first use.

The fix is to install the actual `raspberrypi-archive-keyring` package
instead of the loose key file:

```sh
curl -fsSL \
  https://archive.raspberrypi.com/debian/pool/main/r/raspberrypi-archive-keyring/raspberrypi-archive-keyring_2025.1+rpt1_all.deb \
  -o /tmp/keyring.deb
dpkg -i /tmp/keyring.deb
echo 'deb http://archive.raspberrypi.com/debian/ trixie main' \
  > /etc/apt/sources.list.d/raspi.list
apt-get update
```

`dpkg -i` doesn't invoke apt's Release-signature check, so this sidesteps
the SHA-1 policy rejection; trust still rests on fetching it over HTTPS
from the canonical `archive.raspberrypi.com` domain. If Raspberry Pi
publishes a newer keyring version, update the URL/version accordingly —
check `dists/trixie/main/binary-arm64/Packages.gz` for the current
`raspberrypi-archive-keyring` version and pool path.

The exact package filename/version in the command above was current at
the time this was written and *will* go stale; re-derive it rather than
assuming it still matches.

## Building the `.deb`

Once the correct kernel headers package is installed and the repo is
trusted, [`scripts/build-binary-deb.sh`](../scripts/build-binary-deb.sh) does the rest — it doesn't know or
care about Raspberry Pi specifically, it just needs a `KDIR` pointing at
already-installed headers:

```sh
kdir=$(ls -d /lib/modules/*/build | head -1)
./scripts/build-binary-deb.sh "$kdir" out/
```

It derives the kernel release from `KDIR`'s parent directory name, so the
output is always named for the exact kernel it was built against, e.g.
`sinput-modules-6.18.50+rpt-rpi-v8_0.1.0_arm64.deb`.

## Verifying before trusting an artifact

Before copying a `.deb` to the physical RPi3:

```sh
# On the build machine, from the .ko inside the .deb, or after `make`:
modinfo src/sinput.ko | grep vermagic

# On the RPi3:
uname -r
```

The kernel release portion must match exactly. If it doesn't,
`insmod`/`modprobe` will refuse to load the module — safe, but it means
the `.deb` was built against the wrong kernel and needs rebuilding
against headers matching what the Pi is actually running.

## Installing on the Pi

```sh
sudo dpkg -i sinput-modules-<release>_<version>_arm64.deb
sudo modprobe sinput
```

The package's `postinst`/`postrm` run `depmod -a <release>` so the module
shows up in `modules.dep` for that kernel; it does not touch any other
installed kernel's module list.

## Reproducing the CI job locally

See the `rpi3-binary-deb` job in [`.gitea/workflows/ci.yml`](../.gitea/workflows/ci.yml) for the exact
commands run in CI. It targets trixie's `linux-headers-rpi-v8`
meta-package (i.e. "whatever trixie currently considers current"). To
pin an exact historical version instead — recommended once you have a
real Pi and want a stable, non-drifting HIL rig — replace
`linux-headers-rpi-v8` with the specific `linux-headers-<version>+rpt-rpi-v8`
package name found in the Packages index, and record that pinned version
here or in the workflow.
