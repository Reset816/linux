# SPDX-License-Identifier: GPL-2.0

TEST_GEN_PROGS := memcpy_user_test
TEST_GEN_MODS_DIR := test_modules

include ../../lib.mk

$(OUTPUT)/memcpy_user_test: memcpy_user_test.c
	mkdir -p $(@D)
	$(CC) -static -o$@ $(CFLAGS) $(LDFLAGS) $^
