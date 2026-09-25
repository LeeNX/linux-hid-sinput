#!/bin/sh
set -eu

# Builds a precompiled binary .deb containing sinput.ko for one exact
# kernel release/arch, tied to whatever kernel headers KDIR points at.
# This does NOT build against the machine's own running kernel -- it
# builds against KDIR only, so KDIR must already match the target
# device's kernel release exactly. See docs/rpi-hil.md.
#
# Usage: ./scripts/build-binary-deb.sh <KDIR> [output-dir]
# Run from the repository root.

KDIR="${1:?usage: $0 <KDIR> [output-dir]}"
OUTDIR="${2:-.}"

PACKAGE_NAME="sinput"
PACKAGE_VERSION="$(grep '^PACKAGE_VERSION=' dkms.conf | cut -d'"' -f2)"
ARCH="$(dpkg --print-architecture)"
KERNEL_RELEASE="$(basename "$(dirname "$KDIR")")"

echo "Building ${PACKAGE_NAME} ${PACKAGE_VERSION} for kernel ${KERNEL_RELEASE} (${ARCH})"

make KDIR="$KDIR" clean
make KDIR="$KDIR"

PKG="${PACKAGE_NAME}-modules-${KERNEL_RELEASE}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
chmod 0755 "$WORKDIR"

MODDIR="$WORKDIR/lib/modules/${KERNEL_RELEASE}/extra"
mkdir -p "$MODDIR" "$WORKDIR/DEBIAN"
cp src/sinput.ko "$MODDIR/sinput.ko"

# Runtime VID/PID support (see README.md "Adding your own VID/PID"): lets
# DIY builders claim their own hardware's VID:PID via /etc/sinput/ids.conf
# instead of patching and rebuilding the module.
mkdir -p "$WORKDIR/usr/lib/sinput" "$WORKDIR/usr/lib/udev/rules.d" "$WORKDIR/etc/sinput"
install -m 0755 scripts/sinput-claim-id.sh "$WORKDIR/usr/lib/sinput/sinput-claim-id.sh"
install -m 0644 udev/99-sinput.rules "$WORKDIR/usr/lib/udev/rules.d/99-sinput.rules"
install -m 0644 etc/sinput/ids.conf "$WORKDIR/etc/sinput/ids.conf"
printf '/etc/sinput/ids.conf\n' > "$WORKDIR/DEBIAN/conffiles"

cat > "$WORKDIR/DEBIAN/control" <<CONTROLEOF
Package: ${PKG}
Version: ${PACKAGE_VERSION}
Section: kernel
Priority: optional
Architecture: ${ARCH}
Maintainer: LeeNX <clinton.lee.taylor@gmail.com>
Homepage: https://github.com/LeeNX/linux-hid-sinput
Description: Precompiled SInput HID kernel module for Linux ${KERNEL_RELEASE}
 Experimental out-of-tree SInput HID driver, precompiled for kernel
 release ${KERNEL_RELEASE} (${ARCH}) only. A vermagic mismatch makes
 insmod/modprobe refuse to load this on any other kernel build --
 install the source DKMS package instead if the target kernel differs.
CONTROLEOF

cat > "$WORKDIR/DEBIAN/postinst" <<POSTEOF
#!/bin/sh
set -e
depmod -a ${KERNEL_RELEASE}
udevadm control --reload || true
udevadm trigger --subsystem-match=hid --action=add || true
POSTEOF
chmod 0755 "$WORKDIR/DEBIAN/postinst"

cat > "$WORKDIR/DEBIAN/postrm" <<POSTEOF
#!/bin/sh
set -e
depmod -a ${KERNEL_RELEASE}
udevadm control --reload || true
POSTEOF
chmod 0755 "$WORKDIR/DEBIAN/postrm"

mkdir -p "$OUTDIR"
OUTFILE="${OUTDIR}/${PKG}_${PACKAGE_VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$WORKDIR" "$OUTFILE"

echo "Built ${OUTFILE}"
