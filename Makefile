# sinput.ko is built from multiple .c files (see src/Makefile for the
# sinput-y object list) -- Kbuild derives KBUILD_MODNAME from the M= path's
# basename, and that name gets C-token-pasted (e.g. in MODULE_DEVICE_TABLE),
# so M must point directly at src/, not repo root with an "src/sinput.o"
# obj-m entry (a "/" in that derived token breaks the token-paste).
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD)/src modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/src clean
	$(RM) tests/decode_test

install:
	$(MAKE) -C $(KDIR) M=$(PWD)/src modules_install

# Host-side protocol decode test. Needs only a C compiler, no kernel
# headers, so it runs on any dev machine, unlike `make all`.
check: tests/decode_test
	./tests/decode_test

tests/decode_test: tests/decode_test.c src/sinput_protocol.h
	$(CC) -Wall -Wextra -std=c99 -o $@ tests/decode_test.c

.PHONY: all clean install check
