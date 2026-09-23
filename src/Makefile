obj-m := sinput.o
sinput-y := sinput_core.o sinput_input.o sinput_battery.o sinput_led.o sinput_ff.o

# Single source of truth for the driver version is dkms.conf's
# PACKAGE_VERSION (same value the .deb packaging scripts read) -- pulled in
# here via a -D define rather than hand-duplicated in sinput_core.c, so
# `modinfo sinput`'s version: line can't drift out of sync with the
# packages it ships in. $(src) is kbuild's own absolute path to this
# directory (from the top-level Makefile's M=$(PWD)/src), so this resolves
# correctly regardless of the caller's working directory.
SINPUT_VERSION := $(shell grep '^PACKAGE_VERSION=' $(src)/../dkms.conf | cut -d'"' -f2)
ccflags-y += -DSINPUT_VERSION=\"$(SINPUT_VERSION)\"
