# SPDX-License-Identifier: GPL-2.0

CFLAGS += -I$(top_srcdir)/tools/include

TEST_GEN_PROGS := zicboz_memset

include ../../lib.mk

$(OUTPUT)/zicboz_memset: zicboz_memset.c ../hwprobe/sys_hwprobe.S
	$(CC) -static -o$@ $(CFLAGS) $(LDFLAGS) $^
