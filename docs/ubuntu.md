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

## Quick install or upgrade from a release

Releases include packages for Ubuntu 26.04 and 24.04 LTS (amd64): the
kernel each shipped with, the previous one and the latest. If the
target runs one of those, run this on the target, with `V` set to the
release:

```sh
V=0.4.4; D=$(mktemp -d) && cd "$D" && u=https://github.com/LeeNX/linux-hid-sinput/releases/download/v$V && curl -fLO "$u/sinput-common_${V}_all.deb" && curl -fLO "$u/sinput-modules-$(uname -r)_${V}-ubuntu$(. /etc/os-release; echo "$VERSION_ID")_$(dpkg --print-architecture).deb" && sudo apt install -y ./*.deb && { [ ! -d /sys/module/sinput ] || sudo modprobe -r sinput; } && sudo modprobe sinput; s=$?; cd / && rm -rf "$D"; (exit $s)
```

It downloads `sinput-common` and the module package for the running
kernel and Ubuntu release into a private temporary directory, installs or
upgrades them, and (re)loads the module. If the old module can't be
unloaded (e.g. something holds it open), the command fails rather than
leaving the old driver running: unplug the controller and run it again, or
reboot. `modinfo -F version sinput` should then print the release's
version (without a suffix), and `dpkg -l 'sinput*'` the package versions,
with `-ubuntuXX.YY` on 0.4.4 and later. Releases up to 0.4.3 don't have
that suffix in their Ubuntu file names: drop `-ubuntu$(...)` from the
command for those.

If `sinput-dkms` is installed, remove it first (`sudo apt remove
sinput-dkms`): its module takes precedence over the precompiled one (see
[Troubleshooting](#troubleshooting)).

If `curl` reports a 404, the release has no package for this kernel, e.g.
24.04's newer HWE kernels (`7.0.0-*` on 24.04). Build one instead, below.

## Building

[`scripts/podman-build-ubuntu-debs.sh`](../scripts/podman-build-ubuntu-debs.sh)
runs the build in an `ubuntu:<version>` container (e.g. `ubuntu:26.04`)
and copies the result
to a local directory. The quickest way is to point it at the target with
`-t`. It reads the target's Ubuntu release, architecture and running
kernel over ssh and builds for exactly that:

```sh
scripts/podman-build-ubuntu-debs.sh -t my-ubuntu-box -o out/my-ubuntu-box
```

This writes two packages:

```
out/my-ubuntu-box/sinput-common_0.4.4_all.deb
out/my-ubuntu-box/sinput-modules-7.0.0-43-generic_0.4.4-ubuntu26.04_amd64.deb
```

The module package's version ends in the Ubuntu version (`-ubuntu26.04`):
24.04's HWE kernels can have the same release name as 26.04's own, e.g.
`7.0.0-43-generic`, but they're different builds, and a module built for
one won't load on the other. The suffix keeps the files apart.

`sinput-common` holds the parts that don't depend on the kernel (udev rule,
claim helper, `/etc/sinput/ids.conf`). `sinput-modules-<kernel-release>`
holds `sinput.ko` for that one kernel and depends on `sinput-common`.

Other ways to call it:

```sh
# Explicit Ubuntu release and kernel, no target access needed:
scripts/podman-build-ubuntu-debs.sh -r 24.04 6.8.0-90-generic

# The short form 6.8.0-90 means 6.8.0-90-generic; several kernels at once:
scripts/podman-build-ubuntu-debs.sh -r 24.04 6.8.0-88 6.8.0-90

# What a release builds: the shipped kernel, the previous one and the latest:
scripts/podman-build-ubuntu-debs.sh -r 24.04 first previous latest

# Which kernels the release has headers for (oldest first):
scripts/podman-build-ubuntu-debs.sh -r 24.04 --list

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
  (`sinput-ubuntu-<version>-<arch>`) is kept and reused, so later builds
  skip most of the apt work (about a minute instead of a few). Pass `--rm`
  to delete it afterwards, or `podman rm -f` it yourself.
* **What gets built.** The working tree as it is: uncommitted changes and
  new untracked files are included, gitignored files are not.
* **Older kernels.** Ubuntu's apt index only lists recent kernels. If the
  headers for a kernel can't be found, the build lists the ones that can.
  For anything older, get the `linux-headers-*` `.deb`s from Launchpad.
* **Build on a host of the target's arch.** `-t` picks the target's arch
  (amd64 or arm64), and the container runs that arch. Making sure that
  works is up to you: use a podman host of the same arch (with `-H`), or
  the GitHub workflow below, which uses native runners. A cross-arch
  container only starts if the podman host has user-mode QEMU registered
  with `binfmt_misc` (e.g. `qemu-user-static`), which podman doesn't set
  up. Without it the container fails with `exec format error`.
* **Harmless warnings.** `the compiler differs from the one used to build
  the kernel` is expected. The kernel's compiler is called
  `x86_64-linux-gnu-gcc` and the container's is `gcc`, but they are the
  same version.

### On GitHub

Every release already includes packages for Ubuntu 26.04 and 24.04 LTS
(amd64): the kernel each shipped with, the previous one and the latest.

For anything else, the "Ubuntu targeted .debs" workflow
([`.github/workflows/ubuntu-kernel-debs.yml`](../.github/workflows/ubuntu-kernel-debs.yml))
does the same build from the Actions tab. Pick the Ubuntu release from
the list, and paste in kernel release(s) or leave them empty for the
release's current kernel. Optionally give a tag to build and attach the
`.deb`s to its release. The `.deb`s are a workflow artifact.

The build prints the module's vermagic. Its first word must be the
target's `uname -r`:

```
vermagic: 7.0.0-43-generic SMP preempt mod_unload modversions
```

## Installing on the target: one line

```sh
HOST=my-ubuntu-box
D=$(ssh $HOST mktemp -d) && scp out/$HOST/*.deb "$HOST:$D/" && ssh -t $HOST "sudo apt install -y $D/sinput-common_*_all.deb $D/sinput-modules-\$(uname -r)_*.deb && sudo modprobe sinput; s=\$?; rm -rf $D; exit \$s"
```

What it does:

* Creates a private directory on the target (`mktemp -d`: mode 0700, so
  only your user can put files in it) and copies the `.deb`s there.
  Don't copy them to `/tmp` itself and install with a glob from there:
  on a shared machine another user could leave a matching `.deb` in
  `/tmp`, and `sudo apt install` would run its install scripts as root.
* Installs `sinput-common` and the module package for the kernel the
  target is *running*. `$D` is filled in locally, but the escaped
  `\$(uname -r)` is worked out on the target. If no package matches that
  kernel, the install fails without changing anything.
* Loads the module, and deletes the directory whether or not the install
  worked. The command still fails if the install or `modprobe` did.

`ssh -t` is there so `sudo` can ask for a password. apt may print `Download
is performed unsandboxed as root` because its `_apt` user can't read the
private directory. That's expected and harmless.

Use `apt install`, not `dpkg -i`. apt reads the dependency from the
module package and installs `sinput-common` (and anything else needed) in
the right order. `dpkg -i` on one package by itself leaves it unpacked but
not configured if its dependencies are missing (`iU` in `dpkg -l`), and
`apt` then refuses other installs until that is fixed (see
[Troubleshooting](#troubleshooting)).

To install a kernel's package before booting into that kernel (see
[Kernel upgrades](#kernel-upgrades)), copy just that kernel's package
(plus `sinput-common`) and leave out the `modprobe`:

```sh
D=$(ssh $HOST mktemp -d) && scp out/$HOST/sinput-common_*_all.deb out/$HOST/sinput-modules-7.0.0-45-generic_*.deb "$HOST:$D/" && ssh -t $HOST "sudo apt install -y $D/*.deb; s=\$?; rm -rf $D; exit \$s"
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

Neither the module nor the `.deb`s are signed, and these scripts don't
do signing. If you need either, that's up to you. With Secure Boot on,
the simplest route is the source DKMS package: Ubuntu's `dkms` signs the
modules it builds on the target with a local key, which you enroll once
with `mokutil --import` and confirm at the next boot.

## Kernel upgrades

Each module package is tied to one kernel release, and packages for
several kernels can be installed side by side. When Ubuntu offers a new
kernel:

1. Build for it, e.g. `-r 24.04 6.8.0-91` (`--list` shows when the
   headers are out).
2. Copy and install its package on the target *before* rebooting (the
   second command [above](#installing-on-the-target-one-line)).
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
