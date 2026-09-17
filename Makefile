obj-m += src/sinput.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) tests/decode_test

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

# Host-side protocol decode test. Needs only a C compiler, no kernel
# headers, so it runs on any dev machine, unlike `make all`.
check: tests/decode_test
	./tests/decode_test

tests/decode_test: tests/decode_test.c src/sinput_protocol.h
	$(CC) -Wall -Wextra -std=c99 -o $@ tests/decode_test.c

.PHONY: all clean install check
