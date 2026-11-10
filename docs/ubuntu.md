# Ubuntu: precompiled binary `.deb`

How to build `sinput` for one exact Ubuntu kernel in a podman container,
then install it on the target with a single command. Nothing needs to be
compiled on the target, and it needs no compiler or kernel headers.

The catch is that a precompiled module only loads on the exact kernel
build it was compiled for (its *vermagic* has to match `uname -r`). Ubuntu
ships new kernels often, so for a machine that takes kernel updates on its
own, the source DKMS package (`scripts/build-source-deb.sh`, see the
[README](../README.md#packaging-deb-packages)) rebuilds the module on the
target itself after each kernel update. Use a precompiled `.deb` when you
want a kernel pinned exactly, or don't want build tools on the target.

## Building

[`scripts/podman-build-ubuntu-debs.sh`](../scripts/podman-build-ubuntu-debs.sh)
runs the build in an `ubuntu:<codename>` container and copies the result
to a local directory. The quickest way is to point it at the target with
`-t`. It reads the target's Ubuntu release, architecture and running
kernel over ssh and builds for exactly that:

```sh
scripts/podman-build-ubuntu-debs.sh -t my-ubuntu-box -o out/my-ubuntu-box
```

This writes two packages:

```
out/my-ubuntu-box/sinput-common_0.4.1_all.deb
out/my-ubuntu-box/sinput-modules-7.0.0-43-generic_0.4.1_amd64.deb
```

`sinput-common` holds the parts that don't depend on the kernel (udev rule,
claim helper, `/etc/sinput/ids.conf`). `sinput-modules-<kernel-release>`
holds `sinput.ko` for that one kernel and depends on `sinput-common`.

Other ways to call it:

```sh
# Explicit Ubuntu release and kernel, no target access needed:
scripts/podman-build-ubuntu-debs.sh -r noble 6.8.0-90-generic

# The short form 6.8.0-90 means 6.8.0-90-generic; several kernels at once:
scripts/podman-build-ubuntu-debs.sh -r noble 6.8.0-88 6.8.0-90

# Which kernels the release has headers for (oldest first):
scripts/podman-build-ubuntu-debs.sh -r noble --list

# Run podman on another machine (e.g. a faster Linux box) over ssh; the
# .debs still land in -o on this machine:
scripts/podman-build-ubuntu-debs.sh -H build-box -t my-ubuntu-box -o out/my-ubuntu-box
```

`--help` lists every option. Notes:

* **Apt proxy.** Set `APT_PROXY` (e.g. `http://192.0.2.10:3142` for an
  apt-cacher-ng) to cache package downloads. Use an IP address: the
  container may not be able to resolve LAN-only hostnames. If the proxy
  can't be reached, the build warns and downloads directly.
* **Container reuse.** One container per release and arch
  (`sinput-ubuntu-<codename>-<arch>`) is kept and reused, so later builds
  skip most of the apt work (about a minute instead of a few). Pass `--rm`
  to delete it afterwards, or `podman rm -f` it yourself.
* **What gets built.** The working tree as it is: uncommitted changes and
  new untracked files are included, gitignored files are not.
* **Older kernels.** Ubuntu's apt index only lists recent kernels. If the
  headers for a kernel can't be found, the build lists the ones that can.
  For anything older, get the `linux-headers-*` `.deb`s from Launchpad.
* **arm64 targets.** `-t` picks the target's arch. Building arm64 on an
  amd64 host runs under emulation, which is slow but works.
* **Harmless warnings.** `the compiler differs from the one used to build
  the kernel` is expected. The kernel's compiler is called
  `x86_64-linux-gnu-gcc` and the container's is `gcc`, but they are the
  same version.

The build prints the module's vermagic. Its first word must be the
target's `uname -r`:

```
vermagic: 7.0.0-43-generic SMP preempt mod_unload modversions
```

## Installing on the target: one line

```sh
HOST=my-ubuntu-box
scp out/$HOST/*.deb $HOST:/tmp/ && ssh -t $HOST 'sudo apt install -y /tmp/sinput-common_*_all.deb /tmp/sinput-modules-$(uname -r)_*.deb && sudo modprobe sinput && rm /tmp/sinput-*.deb'
```

What it does:

* Copies the `.deb`s to the target's `/tmp`.
* Installs `sinput-common` and the module package for the kernel the
  target is *running*. The `$(uname -r)` is inside single quotes, so it
  is worked out on the target, not locally. If no package matches that
  kernel, the install fails without changing anything.
* Loads the module, then deletes the copied files.

`ssh -t` is there so `sudo` can ask for a password.

Use `apt install`, not `dpkg -i`. apt reads the dependency from the
module package and installs `sinput-common` (and anything else needed) in
the right order. `dpkg -i` on one package by itself leaves it unpacked but
not configured if its dependencies are missing (`iU` in `dpkg -l`), and
`apt` then refuses other installs until that is fixed (see
[Troubleshooting](#troubleshooting)).

To install a kernel's package before booting into that kernel (see
[Kernel upgrades](#kernel-upgrades)), name the file explicitly instead of
using `$(uname -r)`, and leave out the `modprobe`:

```sh
ssh -t $HOST 'sudo apt install -y /tmp/sinput-common_*_all.deb /tmp/sinput-modules-7.0.0-45-generic_*.deb'
```

## Checking it worked

On the target:

```sh
modinfo -F vermagic sinput     # matches uname -r
lsmod | grep sinput
sudo dmesg | grep -i sinput
```

Plug in an SInput controller and it should be bound to the `sinput`
driver rather than `hid-generic`. The [README](../README.md#diagnostics)
shows how to check.

## Secure Boot

The module isn't signed. On a machine booted with Secure Boot, Ubuntu's
kernel refuses unsigned modules:

```
modprobe: ERROR: could not insert 'sinput': Key was rejected by service
```

Check before installing:

```sh
mokutil --sb-state                    # "SecureBoot enabled" means it will be refused
cat /sys/kernel/security/lockdown     # [none] means unsigned modules load
```

With Secure Boot off (or on a machine without EFI) the module loads. The
kernel logs `module verification failed: signature and/or required key
missing - tainting kernel` once, which is expected.

With Secure Boot on, either:

* use the source DKMS package instead. Ubuntu's `dkms` signs the modules
  it builds with a local key, which you enroll once with `mokutil
  --import` and confirm at the next boot, or
* sign `sinput.ko` with a key already enrolled in MOK, using the kernel's
  `scripts/sign-file`, before packaging.

## Kernel upgrades

Each module package is tied to one kernel release, and packages for
several kernels can be installed side by side. When Ubuntu offers a new
kernel:

1. Build for it, e.g. `-r noble 6.8.0-91` (`--list` shows when the
   headers are out).
2. Install its package on the target *before* rebooting (the explicit
   filename form above).
3. Reboot. The module is there for whichever kernel boots.
4. Remove the old kernel's package once you no longer boot it:
   `sudo apt remove sinput-modules-6.8.0-90-generic`.

## Removing

```sh
sudo modprobe -r sinput
sudo apt remove 'sinput-modules-*' sinput-common
```

apt removes any module package along with `sinput-common`, since they
depend on it. Use `apt purge` instead of `apt remove` to also delete
`/etc/sinput/ids.conf`, including local edits.

## Troubleshooting

* **`Unmet dependencies` / `iU` in `dpkg -l 'sinput*'`.** An earlier
  `dpkg -i` left a package half installed. Install its dependencies with
  `sudo apt --fix-broken install`, or remove it with `sudo dpkg -r <package>`,
  then use the `apt install` line above.
* **Don't mix DKMS and precompiled packages for the same kernel.** If
  `sinput-dkms` is installed, its build in `/lib/modules/<release>/updates/dkms`
  takes precedence over the precompiled one in `.../extra`, so the
  precompiled package has no effect. Pick one and remove the other.
* **`Invalid module format` / `disagrees about version of symbol`.** The
  package was built for a different kernel build. Check that `modinfo
  -F vermagic sinput` matches `uname -r`, and rebuild with `-t`.
