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
# same file conflict.
#
# dpkg has no wildcard for package names, so the per-kernel names are
# listed explicitly. Checked 2026-11-10:
#   - Only 0.3.0 and 0.4.0 module packages contain the shared files
#     (added in 6f3935e, first released in v0.3.0); 0.2.x and older can't
#     conflict with this package.
#   - Every GitHub release (v0.1.0-v0.4.0) and GitLab release
#     (v0.2.0-v0.4.0) shipped exactly one module package, for
#     6.18.50+rpt-rpi-v8. Gitea releases are attached by hand from the same
#     tag builds (RELEASE.md), but its API wasn't reachable to confirm.
#   - 6.18.60+rpt-rpi-v8 at 0.4.0 was only ever built ad hoc for the
#     rp3b-ble-hil rig, never published.
# An unlisted package (another ad hoc build) still conflicts: remove it
# before installing this, as docs/rpi-hil.md says. Add a name here if a
# published 0.4.0-or-older package for another kernel ever turns up.
#
# Deliberately Replaces: without Breaks:. Breaks would force an old
# 0.4.0 module package to be upgraded or removed before this could
# install -- but an old package for a kernel you're still able to boot
# keeps working fine after losing these files to this package, which is
# the whole point of letting kernel packages coexist.
#
# The flip side: such an old package doesn't depend on this one, so dpkg
# would happily remove this package from under it, taking the udev rule
# and helper with it. The prerm below refuses that while any of these
# packages is still installed at a pre-split version.
SPLIT_VERSION="0.4.1~"
LEGACY_PKGS="${PACKAGE_NAME}-dkms"
for old_kernel in 6.18.50+rpt-rpi-v8 6.18.60+rpt-rpi-v8; do
	LEGACY_PKGS="${LEGACY_PKGS} ${PACKAGE_NAME}-modules-${old_kernel}"
done
REPLACES=""
for pkg in $LEGACY_PKGS; do
	REPLACES="${REPLACES:+${REPLACES}, }${pkg} (<< ${SPLIT_VERSION})"
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

# Only on an actual removal ("remove"; purge runs prerm with "remove" too).
# Upgrades of this package ("upgrade") and dpkg's error-unwind paths
# ("deconfigure", "failed-upgrade") must go through.
cat > "$WORKDIR/DEBIAN/prerm" <<PREEOF
#!/bin/sh
set -e
[ "\$1" = "remove" ] || exit 0
blocking=""
for pkg in ${LEGACY_PKGS}; do
	status="\$(dpkg-query -W -f='\${db:Status-Status} \${Version}' "\$pkg" 2>/dev/null || true)"
	case "\$status" in
	"installed "*)
		if dpkg --compare-versions "\${status#installed }" lt "${SPLIT_VERSION}"; then
			blocking="\$blocking \$pkg"
		fi
		;;
	esac
done
if [ -n "\$blocking" ]; then
	echo "${PKG}: not removing; these older packages still rely on its" >&2
	echo "udev rule and helper script:\$blocking" >&2
	echo "Remove or upgrade them first." >&2
	exit 1
fi
PREEOF
chmod 0755 "$WORKDIR/DEBIAN/prerm"

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
