#!/bin/sh
set -eu

NAME=sinput
VERSION=0.1.0

sudo dkms remove "${NAME}/${VERSION}" --all
