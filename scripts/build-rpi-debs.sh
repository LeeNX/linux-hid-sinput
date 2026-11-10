#!/bin/sh
set -eu

# Builds precompiled sinput-modules-<kernel-release> .debs for one or more
# exact Raspberry Pi OS kernel releases, plus the sinput-common .deb they
# depend on. This is the single entry point for every Raspberry Pi binary
# build: CI, the release workflows, the manual "build for kernel X"
# workflows, and local podman builds all call it.
#
# It installs Raspberry Pi's own kernel headers from archive.raspberrypi.com
# (the module's vermagic has to match the device's exact kernel build, and
# Raspberry Pi OS runs its own +rpt kernels, not Debian's), so it must run
# as root on an arm64 Debian trixie system -- in practice a throwaway or
# dedicated `debian:trixie` arm64 container, never the Pi itself. See
# docs/rpi-hil.md.
#
# Usage: ./scripts/build-rpi-debs.sh [options] [KERNEL_RELEASE...]
# Run from anywhere; paths are resolved against the repository root.

usage() {
	cat <<'EOF'
Usage: scripts/build-rpi-debs.sh [options] [KERNEL_RELEASE...]

Build sinput-common plus one sinput-modules-<release> .deb per
KERNEL_RELEASE, against Raspberry Pi's own kernel headers.

KERNEL_RELEASE is what `uname -r` prints on the Pi, e.g.
6.18.60+rpt-rpi-v8. The short form 6.18.60 is also accepted and expanded
with the current flavour (+rpt-rpi-v8 by default). With no
KERNEL_RELEASE, builds for whatever kernel the archive currently calls
current (the linux-headers-<flavour> metapackage's target).

Options:
  -o DIR     Output directory (default: out).
  --list     Print every kernel release the archive has headers for,
             oldest first, and exit.
  -h, --help Show this help.

Environment:
  RPI_FLAVOUR  Kernel flavour (default: rpi-v8, used by Pi 3/4/Zero 2 W
               and also bootable on Pi 5; rpi-2712 is Pi 5's own).
  APT_PROXY    Optional apt HTTP proxy, e.g. http://192.0.2.10:3142.

Must run as root on arm64 Debian trixie (e.g. a debian:trixie container).
EOF
}

die() {
	echo "error: $*" >&2
	exit 1
}

FLAVOUR="${RPI_FLAVOUR:-rpi-v8}"
OUTDIR="out"
LIST_ONLY=false
RELEASES=""
KEYRING_URL="https://archive.raspberrypi.com/debian/pool/main/r/raspberrypi-archive-keyring/raspberrypi-archive-keyring_2025.1+rpt1_all.deb"

while [ $# -gt 0 ]; do
	case "$1" in
	-o)
		[ $# -ge 2 ] || die "-o needs a directory"
		OUTDIR="$2"
		shift 2
		;;
	--list) LIST_ONLY=true; shift ;;
	-h | --help) usage; exit 0 ;;
	-*) usage >&2; die "unknown option: $1" ;;
	*) RELEASES="$RELEASES $1"; shift ;;
	esac
done

[ "$(id -u)" -eq 0 ] || die "must run as root (it installs kernel headers with apt)"
[ "$(dpkg --print-architecture)" = "arm64" ] ||
	die "Raspberry Pi only publishes ${FLAVOUR} headers for arm64; run this in an arm64 container"

ROOTDIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOTDIR"
case "$OUTDIR" in /*) ;; *) OUTDIR="$ROOTDIR/$OUTDIR" ;; esac

export DEBIAN_FRONTEND=noninteractive

if [ -n "${APT_PROXY:-}" ]; then
	printf 'Acquire::http::Proxy "%s";\n' "$APT_PROXY" > /etc/apt/apt.conf.d/01proxy
fi

apt-get update -qq
apt-get install -y -qq curl ca-certificates build-essential bc flex bison \
	libssl-dev libelf-dev kmod >/dev/null
if [ ! -e /etc/apt/sources.list.d/raspi.list ]; then
	curl -fsSL "$KEYRING_URL" -o /tmp/raspberrypi-archive-keyring.deb
	dpkg -i /tmp/raspberrypi-archive-keyring.deb >/dev/null
	echo 'deb http://archive.raspberrypi.com/debian/ trixie main' \
		> /etc/apt/sources.list.d/raspi.list
	apt-get update -qq
fi

available() {
	apt-cache pkgnames linux-headers- |
		sed -n "s/^linux-headers-\([0-9][^ ]*+rpt-${FLAVOUR}\)\$/\1/p" |
		sort -V
}

if $LIST_ONLY; then
	available
	exit 0
fi

if [ -z "$RELEASES" ]; then
	RELEASES="$(apt-cache depends "linux-headers-${FLAVOUR}" |
		sed -n "s/^ *Depends: linux-headers-\([0-9][^ ]*+rpt-${FLAVOUR}\)\$/\1/p" | head -1)"
	[ -n "$RELEASES" ] || die "couldn't resolve linux-headers-${FLAVOUR} to a kernel release"
	echo "No kernel release given; archive's current ${FLAVOUR} kernel is ${RELEASES}"
fi

mkdir -p "$OUTDIR"
for rel in $RELEASES; do
	case "$rel" in *+*) ;; *) rel="${rel}+rpt-${FLAVOUR}" ;; esac
	if ! available | grep -qxF "$rel"; then
		echo "Kernel releases with headers in the archive:" >&2
		available >&2
		die "no linux-headers-${rel} in the Raspberry Pi archive"
	fi
	apt-get install -y -qq "linux-headers-${rel}" >/dev/null
	# Must be the /lib/modules/<release>/build symlink, not the
	# /usr/src/linux-headers-<release> path it points at:
	# build-binary-deb.sh names the package after KDIR's parent dir.
	./scripts/build-binary-deb.sh "/lib/modules/${rel}/build" "$OUTDIR"
done

./scripts/build-common-deb.sh "$OUTDIR"

echo
echo "Built in ${OUTDIR}:"
ls -1 "$OUTDIR"
