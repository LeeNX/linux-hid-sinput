#!/bin/sh
set -eu

NAME=sinput
VERSION=0.1.0

sudo dkms add "$(pwd)"
sudo dkms build "${NAME}/${VERSION}"
sudo dkms install "${NAME}/${VERSION}"

echo
echo "Installed ${NAME}/${VERSION}"
echo "Load with: sudo modprobe ${NAME}"
