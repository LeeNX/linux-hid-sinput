#!/bin/sh
set -eu

# Builds precompiled sinput-modules-<kernel-release> .debs for one or more
# exact Ubuntu kernel releases, plus the sinput-common .deb they depend on.
# The Ubuntu counterpart of build-rpi-debs.sh.
#
# It installs the kernel's own linux-headers-<release> package with apt,
# so it must run as root inside an ubuntu:<version> container (e.g.
# ubuntu:26.04) matching the target machine's Ubuntu release (that's where its
# kernel's headers, and the gcc they were built with, come from). Never
# run it on the target itself. scripts/podman-build-ubuntu-debs.sh sets
# that container up and calls this.
#
# Usage: ./scripts/build-ubuntu-debs.sh [options] [KERNEL_RELEASE...]
# Run from anywhere; paths are resolved against the repository root.

usage() {
	cat <<'EOF'
Usage: scripts/build-ubuntu-debs.sh [options] [KERNEL_RELEASE...]

Build sinput-common plus one sinput-modules-<release> .deb per
KERNEL_RELEASE, against Ubuntu's own kernel headers.

KERNEL_RELEASE is what `uname -r` prints on the target, e.g.
7.0.0-43-generic. The short form 7.0.0-43 is also accepted and expanded
with the flavour (-generic by default). It can also be one of these,
looked up in apt:

  latest    this Ubuntu release's current kernel (the
            linux-headers-<flavour> metapackage's target). The default.
  previous  the kernel before latest in the same series
  first     the oldest kernel in the same series: the one the Ubuntu
            release shipped with, e.g. 7.0.0-14 for 26.04

"Same series" keeps a release's own kernel line apart from newer
hardware-enablement (HWE) kernels, e.g. on 24.04 previous of 6.8.0-149
is 6.8.0-147, not a 7.0 kernel. Releases build `first previous latest`.
A kernel that two of them name is built once.

Options:
  -o DIR     Output directory (default: out).
  --list     Print every kernel release apt has headers for, oldest
             first, and exit.
  -h, --help Show this help.

Environment:
  FLAVOUR    Kernel flavour (default: generic).
  APT_PROXY  Optional apt HTTP proxy, e.g. http://192.0.2.10:3142.
             Skipped, with a warning, if it can't be reached.

Module packages get the Ubuntu version in their version and file name,
e.g. sinput-modules-7.0.0-43-generic_0.4.4-ubuntu26.04_amd64.deb, since
Ubuntu releases can share a kernel release name (24.04's HWE kernels).

Must run as root on Ubuntu (e.g. an ubuntu:26.04 container).
EOF
}

die() {
	echo "error: $*" >&2
	exit 1
}

FLAVOUR="${FLAVOUR:-generic}"
OUTDIR="out"
LIST_ONLY=false
RELEASES=""

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
# shellcheck disable=SC1091 # the container's, not in the repo
. /etc/os-release
[ "${ID:-}" = ubuntu ] || die "not Ubuntu (ID=${ID:-unknown}); use an ubuntu:<version> container, e.g. ubuntu:26.04"

ROOTDIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOTDIR"
case "$OUTDIR" in /*) ;; *) OUTDIR="$ROOTDIR/$OUTDIR" ;; esac

export DEBIAN_FRONTEND=noninteractive

# Always rewrite (or drop) the proxy config: a persistent build container
# must not keep using a proxy from an earlier run.
rm -f /etc/apt/apt.conf.d/01proxy
if [ -n "${APT_PROXY:-}" ]; then
	hostport="${APT_PROXY#*://}"
	hostport="${hostport%%/*}"
	# perl-base is Essential on Ubuntu, so this works in a bare image.
	if perl -MIO::Socket::INET -e \
		'exit !IO::Socket::INET->new(PeerAddr => $ARGV[0], Timeout => 3)' "$hostport"; then
		# security.ubuntu.com goes direct: an apt-cacher-ng that fails on
		# it (seen as HTTP 500s) makes apt silently drop the -security
		# index, which can hide a kernel's headers. Its pocket is small,
		# so caching it gains little anyway.
		printf 'Acquire::http::Proxy "%s";\nAcquire::http::Proxy::security.ubuntu.com "DIRECT";\n' \
			"$APT_PROXY" > /etc/apt/apt.conf.d/01proxy
		echo "Using apt proxy ${APT_PROXY}"
	else
		echo "warning: apt proxy ${APT_PROXY} unreachable, downloading directly" >&2
	fi
fi

apt-get update -qq
apt-get install -y -qq build-essential kmod >/dev/null

available() {
	apt-cache pkgnames linux-headers- |
		sed -n "s/^linux-headers-\([0-9][0-9.]*-[0-9][0-9]*-${FLAVOUR}\)\$/\1/p" |
		sort -V
}

if $LIST_ONLY; then
	available
	exit 0
fi

# The kernel the linux-headers-<flavour> metapackage currently points at.
current() {
	apt-cache depends "linux-headers-${FLAVOUR}" |
		sed -n "s/^ *Depends: linux-headers-\([0-9][^ ]*-${FLAVOUR}\)\$/\1/p" | head -1
}

# Every kernel in the same series as $1, oldest first.
series() {
	prefix="$(printf '%s\n' "$1" | sed 's/-.*/-/')"
	available | awk -v p="$prefix" 'index($0, p) == 1'
}

# latest / previous / first -> a kernel release (series = same x.y.z, e.g. 7.0.0-);
# anything else is passed through as a release.
resolve() {
	case "$1" in
	latest | previous | first)
		cur="$(current)"
		[ -n "$cur" ] || die "couldn't resolve linux-headers-${FLAVOUR} to a kernel release"
		case "$1" in
		latest) echo "$cur" ;;
		first) series "$cur" | head -1 ;;
		previous) series "$cur" | awk -v c="$cur" '$0 == c { print p; exit } { p = $0 }' ;;
		esac
		;;
	*) echo "$1" ;;
	esac
}

mkdir -p "$OUTDIR"
export VERSION_SUFFIX="-ubuntu${VERSION_ID}"
BUILT=" "
for want in ${RELEASES:-latest}; do
	rel="$(resolve "$want")"
	if [ -z "$rel" ]; then
		# resolve's die only left its $(...) subshell.
		[ -n "$(current)" ] || die "couldn't resolve linux-headers-${FLAVOUR} to a kernel release"
		echo "No ${want} kernel: the current kernel is the only one in its series. Skipping."
		continue
	fi
	# 7.0.0-43 -> 7.0.0-43-generic; anything with a third dash-field is
	# already a full release.
	case "$rel" in *-*-*) ;; *) rel="${rel}-${FLAVOUR}" ;; esac
	case "$BUILT" in *" $rel "*)
		echo "${want} is ${rel}, already built. Skipping."
		continue
		;;
	esac
	BUILT="${BUILT}${rel} "
	[ "$want" = "$rel" ] || echo "${want}: ${rel}"
	if ! apt-cache show "linux-headers-${rel}" >/dev/null 2>&1; then
		echo "Kernel releases with headers in Ubuntu ${VERSION_ID}'s apt sources:" >&2
		available >&2
		die "no linux-headers-${rel} in Ubuntu ${VERSION_ID}'s apt sources (older kernels drop out of the index; get the headers .debs from Launchpad)"
	fi
	apt-get install -y -qq "linux-headers-${rel}" >/dev/null
	# Must be the /lib/modules/<release>/build symlink, not the
	# /usr/src/linux-headers-<release> path it points at:
	# build-binary-deb.sh names the package after KDIR's parent dir.
	./scripts/build-binary-deb.sh "/lib/modules/${rel}/build" "$OUTDIR"
	echo "vermagic: $(modinfo -F vermagic src/sinput.ko)"
done

./scripts/build-common-deb.sh "$OUTDIR"

echo
echo "Built in ${OUTDIR}:"
ls -1 "$OUTDIR"
