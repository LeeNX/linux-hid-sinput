#!/bin/sh
# Run by udev (see udev/99-sinput.rules) for every HID device "add" event.
# Rebinds it to the sinput driver instead of whatever currently claims it
# (hid-generic, in the common case) if its VID:PID is listed in
# /etc/sinput/ids.conf. See README.md "Adding your own VID/PID" for what
# belongs in that file and why this indirection is needed at all:
# hid-generic claims unknown HID devices before sinput ever gets a look, and
# the sinput driver's built-in id_table only knows the generic testing
# VID/PID (2E8A:10C6) -- new_id/bind is the kernel's own supported way to
# extend that at runtime, without rebuilding the module.
#
# The udev rule matches on the bare "add" event, not on
# DRIVER=="hid-generic": device_add() (drivers/base/core.c) fires the "add"
# uevent *before* bus_probe_device() runs, so at the moment udev evaluates
# rules for a genuinely first-time plug-in, the device has no driver bound
# yet at all and DRIVER== would not match -- only a later re-synthesized
# `udevadm trigger` event (used by the installers, for already-connected
# devices) reads the driver back after it's already bound, which would have
# masked this at install-test time while silently never firing on a real
# first plug. This script itself determines whatever the device's current
# driver actually is, instead of relying on udev to have already resolved
# that by the time this runs.
set -eu

CONF="${SINPUT_IDS_CONF:-/etc/sinput/ids.conf}"
DRV=/sys/bus/hid/drivers/sinput

[ -r "$CONF" ] || exit 0
[ -d "$DRV" ] || exit 0
[ -n "${HID_ID:-}" ] || exit 0
[ -n "${DEVPATH:-}" ] || exit 0

# HID_ID is "bus:vendor:product" in hex, e.g. "0003:0000045E:000000F0".
bus="${HID_ID%%:*}"
rest="${HID_ID#*:}"
vendor_hex="${rest%%:*}"
product_hex="${rest#*:}"
# $((16#...)) is a bashism/kshism, not POSIX -- dash rejects it outright, so
# hex-to-decimal goes through printf instead (works the same in dash, bash
# and every other /bin/sh).
vendor_dec=$(printf '%d' "0x${vendor_hex}")
product_dec=$(printf '%d' "0x${product_hex}")

matched=0
while IFS=: read -r cfg_vendor cfg_product; do
	case "$cfg_vendor" in ''|'#'*) continue ;; esac
	# Allow trailing comments/whitespace after the PID, e.g. "2e8a:1a01 # my pad".
	cfg_product="${cfg_product%%[	 ]*}"
	cfg_vendor="${cfg_vendor#0x}"
	cfg_vendor="${cfg_vendor#0X}"
	cfg_product="${cfg_product#0x}"
	cfg_product="${cfg_product#0X}"
	# Reject anything that isn't plain hex before it ever reaches printf,
	# so a malformed config line can't abort the script under `set -e`.
	case "$cfg_vendor" in *[!0-9a-fA-F]*|'') continue ;; esac
	case "$cfg_product" in *[!0-9a-fA-F]*|'') continue ;; esac
	if [ "$(printf '%d' "0x${cfg_vendor}")" -eq "$vendor_dec" ] && \
	   [ "$(printf '%d' "0x${cfg_product}")" -eq "$product_dec" ]; then
		matched=1
		break
	fi
done < "$CONF"

[ "$matched" -eq 1 ] || exit 0

devname="${DEVPATH##*/}"
[ -e "/sys/bus/hid/devices/$devname" ] || exit 0

# Find whatever driver currently owns this device, if any -- not assumed to
# be hid-generic specifically, and not assumed to be bound at all (a
# raced-in duplicate event, or this script running twice for the same
# device, are both possible).
cur_driver=""
if [ -L "/sys/bus/hid/devices/$devname/driver" ]; then
	cur_driver="$(basename "$(readlink "/sys/bus/hid/devices/$devname/driver")")"
fi

# Already ours -- a re-triggered event, or a previous run of this script
# already won the race. Nothing to do.
[ "$cur_driver" = "sinput" ] && exit 0

# unbind before bind is mandatory (the HID bus, like most, refuses to bind a
# device that already has a driver) -- but only once we know there's
# something to unbind from, and only proceeding to touch sinput at all if
# that unbind actually succeeds. Getting this order backwards (unbinding
# unconditionally, before knowing the sinput bind below will work) can
# leave the device claimed by nothing if the bind then fails.
if [ -n "$cur_driver" ]; then
	printf '%s' "$devname" > "/sys/bus/hid/drivers/${cur_driver}/unbind" 2>/dev/null || exit 0
fi

# Try binding directly first -- this already succeeds with no new_id call
# at all for any VID:PID this driver instance has already registered
# earlier in its lifetime (including the generic testing VID/PID, which is
# in the static id_table from the start). new_id has no dedup and no
# remove-by-value (only remove_id, by exact bus/vendor/product match), so
# only ever calling it when a plain bind has just demonstrably failed is
# what keeps this from appending a duplicate entry to the kernel's dynamic
# ID list on every single replug of an already-known device.
if ! printf '%s' "$devname" > "$DRV/bind" 2>/dev/null; then
	echo "$bus $vendor_hex $product_hex" > "$DRV/new_id" 2>/dev/null || true
	if ! printf '%s' "$devname" > "$DRV/bind" 2>/dev/null; then
		# Restore the original binding rather than leave the device
		# claimed by nothing.
		if [ -n "$cur_driver" ]; then
			printf '%s' "$devname" > "/sys/bus/hid/drivers/${cur_driver}/bind" 2>/dev/null || true
		fi
		exit 0
	fi
fi
