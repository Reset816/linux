# SPDX-License-Identifier: GPL-2.0

MEMCPY_TESTMODS_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m += test_riscv_memcpy_fast_unaligned.o

modules:
	$(Q)$(MAKE) -C $(KDIR) M=$(MEMCPY_TESTMODS_DIR) modules

clean:
	$(Q)$(MAKE) -C $(KDIR) M=$(MEMCPY_TESTMODS_DIR) clean
