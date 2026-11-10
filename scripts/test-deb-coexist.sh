#!/bin/sh
set -eu

# Packaging regression test: sinput-modules-<kernel-release> packages for
# two different kernels must install side by side with sinput-common, and
# removing one must leave the other and the shared files alone. Before
# 0.4.1 every module package shipped its own /etc/sinput/ids.conf, so the
# second `dpkg -i` failed with "trying to overwrite '/etc/sinput/ids.conf'".
#
# It needs real kernel headers to compile against, but not two sets of
# them: it builds the same module twice under two made-up kernel release
# names (symlinked to KDIR), which is all dpkg cares about. The resulting
# .ko vermagic won't match those names -- nothing here loads the module.
#
# Usage: sudo ./scripts/test-deb-coexist.sh <KDIR>
# Run from the repository root, in a disposable container: it installs
# and removes packages.

KDIR="${1:?usage: $0 <KDIR>}"
KDIR="$(readlink -f "$KDIR")"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT" /lib/modules/0.0.1-sinput-coexist-a /lib/modules/0.0.1-sinput-coexist-b' EXIT

for rel in 0.0.1-sinput-coexist-a 0.0.1-sinput-coexist-b; do
	mkdir -p "/lib/modules/${rel}"
	ln -sfn "$KDIR" "/lib/modules/${rel}/build"
	./scripts/build-binary-deb.sh "/lib/modules/${rel}/build" "$OUT" >/dev/null
done
./scripts/build-common-deb.sh "$OUT" >/dev/null
make KDIR="$KDIR" clean >/dev/null

echo "== a module package can't install without sinput-common"
if dpkg -i "$OUT"/sinput-modules-0.0.1-sinput-coexist-a_*.deb >/dev/null 2>&1; then
	fail "sinput-modules installed without sinput-common"
fi
dpkg -r sinput-modules-0.0.1-sinput-coexist-a >/dev/null 2>&1 || true

echo "== install sinput-common, then one module package per kernel"
dpkg -i "$OUT"/sinput-common_*.deb
dpkg -i "$OUT"/sinput-modules-0.0.1-sinput-coexist-a_*.deb
dpkg -i "$OUT"/sinput-modules-0.0.1-sinput-coexist-b_*.deb

for rel in 0.0.1-sinput-coexist-a 0.0.1-sinput-coexist-b; do
	test -e "/lib/modules/${rel}/extra/sinput.ko" || fail "no module for ${rel}"
done
for f in /etc/sinput/ids.conf /usr/lib/sinput/sinput-claim-id.sh \
	/usr/lib/udev/rules.d/99-sinput.rules; do
	owner="$(dpkg -S "$f" | cut -d: -f1)"
	[ "$owner" = "sinput-common" ] || fail "$f owned by '$owner', not sinput-common"
done

echo "== removing one kernel's package leaves the other and the shared files"
dpkg -P sinput-modules-0.0.1-sinput-coexist-a
test ! -e /lib/modules/0.0.1-sinput-coexist-a/extra/sinput.ko || fail "module a not removed"
test -e /lib/modules/0.0.1-sinput-coexist-b/extra/sinput.ko || fail "module b went with a"
test -e /etc/sinput/ids.conf || fail "ids.conf went with module a"

echo "== sinput-common can't be removed while a module package needs it"
if dpkg -r sinput-common >/dev/null 2>&1; then
	fail "sinput-common removed while sinput-modules-0.0.1-sinput-coexist-b depends on it"
fi

dpkg -P sinput-modules-0.0.1-sinput-coexist-b sinput-common
test ! -e /etc/sinput/ids.conf || fail "ids.conf left behind after purging sinput-common"

echo "PASS: kernel module packages coexist"
