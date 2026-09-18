#!/bin/sh
set -eu

# Builds a source DKMS .deb: ships the module source under
# /usr/src/sinput-<version>/ plus a postinst that hands off to dkms's own
# /usr/lib/dkms/common.postinst helper (add + build + install for
# whatever kernel(s) are present) and a prerm that unregisters it via
# `dkms remove`.
#
# Unlike scripts/build-binary-deb.sh, this needs a compiler and matching
# kernel headers on the TARGET machine, not the build machine -- but in
# exchange it keeps working across kernel upgrades automatically,
# because dkms rebuilds it for each new kernel as that kernel's headers
# become available.
#
# Usage: ./scripts/build-source-deb.sh [output-dir]
# Run from the repository root.

OUTDIR="${1:-.}"

PACKAGE_NAME="sinput"
PACKAGE_VERSION="$(grep '^PACKAGE_VERSION=' dkms.conf | cut -d'"' -f2)"
ARCH="all"
PKG="${PACKAGE_NAME}-dkms"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
chmod 0755 "$WORKDIR"

SRCDIR="$WORKDIR/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
mkdir -p "$SRCDIR" "$WORKDIR/DEBIAN"
cp Makefile dkms.conf "$SRCDIR/"
cp -r src "$SRCDIR/"

cat > "$WORKDIR/DEBIAN/control" <<CONTROLEOF
Package: ${PKG}
Version: ${PACKAGE_VERSION}
Section: kernel
Priority: optional
Architecture: ${ARCH}
Depends: dkms (>= 2.2.0.3~)
Maintainer: LeeNX
Description: SInput HID kernel module source for DKMS
 Experimental out-of-tree SInput HID driver, packaged for DKMS. Rebuilds
 automatically against whatever kernel is running, including after
 kernel upgrades, as long as build-essential and matching kernel headers
 are installed separately -- dkms's own postinst skips (rather than
 fails) any kernel it can't find headers for. See docs/rpi-hil.md for a
 precompiled binary alternative that needs neither, at the cost of only
 working on one exact kernel build.
CONTROLEOF

cat > "$WORKDIR/DEBIAN/postinst" <<POSTEOF
#!/bin/sh
set -e
/usr/lib/dkms/common.postinst ${PACKAGE_NAME} ${PACKAGE_VERSION}
POSTEOF
chmod 0755 "$WORKDIR/DEBIAN/postinst"

cat > "$WORKDIR/DEBIAN/prerm" <<PREEOF
#!/bin/sh
set -e
dkms remove -m ${PACKAGE_NAME} -v ${PACKAGE_VERSION} --all || true
PREEOF
chmod 0755 "$WORKDIR/DEBIAN/prerm"

mkdir -p "$OUTDIR"
OUTFILE="${OUTDIR}/${PKG}_${PACKAGE_VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$WORKDIR" "$OUTFILE"

echo "Built ${OUTFILE}"
