#!/bin/sh
set -eu

make
modinfo ./src/sinput.ko | sed -n '1,20p'
echo
echo "Build OK"
