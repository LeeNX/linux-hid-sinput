#!/bin/sh
set -eu

NAME=sinput
VERSION="$(grep '^PACKAGE_VERSION=' "$(dirname "$0")/../dkms.conf" | cut -d'"' -f2)"

sudo dkms remove "${NAME}/${VERSION}" --all
