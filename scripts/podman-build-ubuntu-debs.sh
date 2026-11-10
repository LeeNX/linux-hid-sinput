#!/usr/bin/env bash
set -euo pipefail

# Builds Ubuntu sinput .debs (sinput-common plus one
# sinput-modules-<kernel-release>) in a podman container and copies them
# into a local output directory. Wraps scripts/build-ubuntu-debs.sh, which
# does the actual build inside the container.
#
# One persistent container per Ubuntu release and arch
# (sinput-ubuntu-<codename>-<arch>) is reused between runs, so apt's
# package lists and already installed headers aren't fetched again every
# time. Pass --rm to remove it afterwards.
#
# Usage: scripts/podman-build-ubuntu-debs.sh [options] [KERNEL_RELEASE...]

usage() {
	cat <<'EOF'
Usage: scripts/podman-build-ubuntu-debs.sh [options] [KERNEL_RELEASE...]

Build sinput-common plus one sinput-modules-<release> .deb per
KERNEL_RELEASE in an ubuntu:<codename> podman container, and copy them
into OUTDIR.

Options:
  -t HOST      Target machine: read its Ubuntu codename, arch and running
               kernel (`uname -r`) over ssh and use them as the defaults.
  -r CODENAME  Ubuntu release, e.g. noble or resolute (default: resolute,
               or the target's).
  -a ARCH      Debian arch, amd64 or arm64 (default: amd64, or the
               target's).
  -H HOST      Run podman on HOST over ssh instead of locally (e.g. a
               faster Linux build box). The .debs still end up in OUTDIR
               on this machine.
  -o DIR       Output directory on this machine (default: out).
  --list       List the kernel releases the Ubuntu release has headers
               for, and exit.
  --rm         Remove the build container when done.
  -h, --help   Show this help.

KERNEL_RELEASE: as in `uname -r`, e.g. 7.0.0-43-generic. Defaults to the
target's running kernel with -t, otherwise the release's current
generic kernel.

Environment:
  APT_PROXY  Optional apt HTTP proxy for the container, e.g. an
             apt-cacher-ng at http://192.0.2.10:3142 (an IP is safest:
             the container may not resolve LAN-only names). Unset by
             default. An unreachable proxy is skipped with a warning.
  FLAVOUR    Kernel flavour for short releases and --list (default:
             generic).

The source copied into the container is the git working tree as it is,
uncommitted and untracked files included, minus anything gitignored.

Examples:
  scripts/podman-build-ubuntu-debs.sh -t my-ubuntu-box
  scripts/podman-build-ubuntu-debs.sh -H build-box -t my-ubuntu-box -o out/my-ubuntu-box
  scripts/podman-build-ubuntu-debs.sh -r noble 6.8.0-90-generic
EOF
}

die() {
	echo "error: $*" >&2
	exit 1
}

TARGET=""
CODENAME=""
ARCH=""
PODMAN_HOST=""
OUTDIR="out"
LIST=false
REMOVE=false
RELEASES=()
APT_PROXY="${APT_PROXY:-}"
FLAVOUR="${FLAVOUR:-generic}"

need_arg() { [ $# -ge 2 ] || die "$1 needs an argument"; }

while [ $# -gt 0 ]; do
	case "$1" in
	-t) need_arg "$@"; TARGET="$2"; shift 2 ;;
	-r) need_arg "$@"; CODENAME="$2"; shift 2 ;;
	-a) need_arg "$@"; ARCH="$2"; shift 2 ;;
	-H) need_arg "$@"; PODMAN_HOST="$2"; shift 2 ;;
	-o) need_arg "$@"; OUTDIR="$2"; shift 2 ;;
	--list) LIST=true; shift ;;
	--rm) REMOVE=true; shift ;;
	-h | --help) usage; exit 0 ;;
	-*) usage >&2; die "unknown option: $1" ;;
	*) RELEASES+=("$1"); shift ;;
	esac
done

# The shared ControlMaster socket for these LAN hosts is flaky (bare exit
# 255s); don't use it.
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 -o ControlMaster=no -o ControlPath=none)

if [ -n "$TARGET" ]; then
	# shellcheck disable=SC2016 # expanded on the target, not here
	info="$("${SSH[@]}" "$TARGET" '. /etc/os-release; echo "$ID $VERSION_CODENAME $(dpkg --print-architecture) $(uname -r)"')" ||
		die "couldn't read the target's release over ssh: $TARGET"
	read -r t_id t_codename t_arch t_kernel <<<"$info"
	[ "$t_id" = ubuntu ] || die "$TARGET runs $t_id, not Ubuntu"
	echo "Target $TARGET: Ubuntu $t_codename, $t_arch, kernel $t_kernel"
	: "${CODENAME:=$t_codename}"
	: "${ARCH:=$t_arch}"
	if [ ${#RELEASES[@]} -eq 0 ] && ! $LIST; then
		RELEASES=("$t_kernel")
	fi
fi
: "${CODENAME:=resolute}"
: "${ARCH:=amd64}"

# podman, locally or on PODMAN_HOST. ssh joins its arguments into one
# string for the remote shell, so quote each one for it.
pm() {
	if [ -n "$PODMAN_HOST" ]; then
		"${SSH[@]}" "$PODMAN_HOST" "podman $(printf '%q ' "$@")"
	else
		podman "$@"
	fi
}

ROOTDIR="$(cd "$(dirname "$0")/.." && pwd)"
NAME="sinput-ubuntu-${CODENAME}-${ARCH}"

if ! pm container exists "$NAME"; then
	echo "Creating build container $NAME (ubuntu:$CODENAME, $ARCH)${PODMAN_HOST:+ on $PODMAN_HOST}"
	pm run -d --name "$NAME" --arch "$ARCH" "docker.io/library/ubuntu:$CODENAME" sleep infinity >/dev/null
fi
pm start "$NAME" >/dev/null

# The working tree as it is: tracked files, uncommitted edits and new
# untracked files, but not gitignored build output (*.ko, out/, ...).
# Tracked files deleted in the tree are skipped, or tar would fail on
# them. --no-xattrs stops macOS bsdtar adding headers GNU tar warns about.
(
	cd "$ROOTDIR"
	git ls-files -z --cached --others --exclude-standard |
		while IFS= read -r -d '' f; do
			[ -e "$f" ] && printf '%s\0' "$f"
		done |
		COPYFILE_DISABLE=1 tar -c --no-xattrs --null -T - -f -
) | pm exec -i "$NAME" sh -c 'rm -rf /src /out && mkdir /src && tar -x -C /src'

if $LIST; then
	pm exec -e APT_PROXY="$APT_PROXY" -e FLAVOUR="$FLAVOUR" "$NAME" /src/scripts/build-ubuntu-debs.sh --list
else
	pm exec -e APT_PROXY="$APT_PROXY" -e FLAVOUR="$FLAVOUR" "$NAME" \
		/src/scripts/build-ubuntu-debs.sh -o /out ${RELEASES[@]+"${RELEASES[@]}"}
	mkdir -p "$OUTDIR"
	pm exec "$NAME" tar -c -C /out . | tar -x -C "$OUTDIR"
	echo
	echo "Copied to $OUTDIR:"
	(cd "$OUTDIR" && ls -1 -- *.deb)
fi

if $REMOVE; then
	pm rm -f "$NAME" >/dev/null
	echo "Removed build container $NAME"
fi
