#!/bin/sh
set -eu

# Installs the runtime VID/PID support (see README.md "Adding your own
# VID/PID") for a plain `make`/dkms-install.sh install -- the two .deb
# packages carry the same three files via their own postinst instead, so
# this script is only needed outside of that packaging.
#
# Usage: sudo ./scripts/install-udev-support.sh
# Run from the repository root.

ROOTDIR="$(cd "$(dirname "$0")/.." && pwd)"

install -d -m 0755 /usr/lib/sinput
install -m 0755 "$ROOTDIR/scripts/sinput-claim-id.sh" /usr/lib/sinput/sinput-claim-id.sh

install -d -m 0755 /usr/lib/udev/rules.d
install -m 0644 "$ROOTDIR/udev/99-sinput.rules" /usr/lib/udev/rules.d/99-sinput.rules

install -d -m 0755 /etc/sinput
if [ -e /etc/sinput/ids.conf ]; then
	echo "Keeping existing /etc/sinput/ids.conf"
else
	install -m 0644 "$ROOTDIR/etc/sinput/ids.conf" /etc/sinput/ids.conf
fi

udevadm control --reload
# Re-evaluate already-connected devices against the (possibly just-created)
# config, in case a device was plugged in before this ran.
udevadm trigger --subsystem-match=hid --action=add

echo
echo "Installed. Add your device's VID:PID to /etc/sinput/ids.conf, then"
echo "unplug/replug it (or re-pair, for Bluetooth) -- no reboot needed."
