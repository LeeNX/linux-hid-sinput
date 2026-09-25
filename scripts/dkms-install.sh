#!/bin/sh
set -eu

NAME=sinput
VERSION="$(grep '^PACKAGE_VERSION=' "$(dirname "$0")/../dkms.conf" | cut -d'"' -f2)"

sudo dkms add "$(pwd)"
sudo dkms build "${NAME}/${VERSION}"
sudo dkms install "${NAME}/${VERSION}"

echo
echo "Installed ${NAME}/${VERSION}"
echo "Load with: sudo modprobe ${NAME}"
echo "For your own hardware's VID/PID (not the generic testing one), also run:"
echo "  sudo ./scripts/install-udev-support.sh"
