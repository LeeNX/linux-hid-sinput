#!/bin/sh
# Run by udev (see udev/99-sinput.rules) for every HID device that lands on
# hid-generic. Rebinds it to the sinput driver instead if its VID:PID is
# listed in /etc/sinput/ids.conf. See README.md "Adding your own VID/PID"
# for what belongs in that file and why this indirection is needed at all:
# hid-generic claims unknown HID devices before sinput ever gets a look, and
# the sinput driver's built-in id_table only knows the generic testing
# VID/PID (2E8A:10C6) -- new_id/bind is the kernel's own supported way to
# extend that at runtime, without rebuilding the module.
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

# Best-effort throughout: another instance of this script may already have
# done this (a device can generate more than one hid-generic "add"), and
# none of these failures should make udev treat the event as an error.
echo "$bus $vendor_hex $product_hex" > "$DRV/new_id" 2>/dev/null || true

devname="${DEVPATH##*/}"
if [ -e "/sys/bus/hid/drivers/hid-generic/$devname" ]; then
	printf '%s' "$devname" > /sys/bus/hid/drivers/hid-generic/unbind 2>/dev/null || true
fi
printf '%s' "$devname" > "$DRV/bind" 2>/dev/null || true
