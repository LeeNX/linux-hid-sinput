#!/bin/sh
set -eu

# Builds sinput-common, the kernel-independent half of the packaging: the
# runtime VID/PID support files (see README.md "Adding your own VID/PID")
# that every other sinput package needs exactly one copy of:
#
#   /etc/sinput/ids.conf                    (conffile)
#   /usr/lib/sinput/sinput-claim-id.sh
#   /usr/lib/udev/rules.d/99-sinput.rules
#
# sinput-dkms and every sinput-modules-<kernel-release> package depend on
# this instead of shipping those files themselves. Before 0.4.1 they each
# shipped their own copy, so dpkg refused to install two of them side by
# side ("trying to overwrite '/etc/sinput/ids.conf', which is also in
# package ...") -- e.g. module packages for the old and the new kernel
# across a Raspberry Pi kernel upgrade.
#
# Usage: ./scripts/build-common-deb.sh [output-dir]
# Run from the repository root.

OUTDIR="${1:-.}"

PACKAGE_NAME="sinput"
PACKAGE_VERSION="$(grep '^PACKAGE_VERSION=' dkms.conf | cut -d'"' -f2)"
ARCH="all"
PKG="${PACKAGE_NAME}-common"

# First version where the shared files live in sinput-common. Older
# sinput-dkms / sinput-modules-* packages still own them, so Replaces:
# lets this package take them over on upgrade instead of failing on the
# same file conflict. There's no wildcard for package names, so every
# per-kernel package name that was ever published is listed explicitly
# (all GitHub/GitLab/Gitea releases up to 0.4.0 shipped 6.18.50; 6.18.60
# was built ad hoc for 0.4.0 and may be installed on a test rig).
#
# Deliberately Replaces: without Breaks:. Breaks would force an old
# 0.4.0 module package to be upgraded or removed before this could
# install -- but an old package for a kernel you're still able to boot
# keeps working fine after losing these files to this package, which is
# the whole point of letting kernel packages coexist.
SPLIT_VERSION="0.4.1~"
REPLACES="${PACKAGE_NAME}-dkms (<< ${SPLIT_VERSION})"
for old_kernel in 6.18.50+rpt-rpi-v8 6.18.60+rpt-rpi-v8; do
	REPLACES="${REPLACES}, ${PACKAGE_NAME}-modules-${old_kernel} (<< ${SPLIT_VERSION})"
done

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
chmod 0755 "$WORKDIR"

mkdir -p "$WORKDIR/DEBIAN" "$WORKDIR/usr/lib/sinput" \
	"$WORKDIR/usr/lib/udev/rules.d" "$WORKDIR/etc/sinput"
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
Replaces: ${REPLACES}
Maintainer: LeeNX <clinton.lee.taylor@gmail.com>
Homepage: https://github.com/LeeNX/linux-hid-sinput
Description: SInput HID driver - kernel-independent support files
 Udev rule, helper script and /etc/sinput/ids.conf that let the SInput
 HID driver claim extra VID:PIDs at runtime. Shared by sinput-dkms and
 every precompiled sinput-modules-<kernel-release> package, so module
 packages for several kernels can be installed at the same time.
CONTROLEOF

cat > "$WORKDIR/DEBIAN/postinst" <<'POSTEOF'
#!/bin/sh
set -e
udevadm control --reload || true
POSTEOF
chmod 0755 "$WORKDIR/DEBIAN/postinst"

cat > "$WORKDIR/DEBIAN/postrm" <<'POSTEOF'
#!/bin/sh
set -e
udevadm control --reload || true
POSTEOF
chmod 0755 "$WORKDIR/DEBIAN/postrm"

mkdir -p "$OUTDIR"
OUTFILE="${OUTDIR}/${PKG}_${PACKAGE_VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$WORKDIR" "$OUTFILE"

echo "Built ${OUTFILE}"
