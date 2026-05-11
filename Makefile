KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += src/simplefs.o

.PHONY: all module userspace clean

all: module userspace

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace:
	$(CC) -Wall -Wextra -O2 -o simplefsctl userspace/simplefsctl.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) simplefsctl
