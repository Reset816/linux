# SPDX-License-Identifier: GPL-2.0

TEST_GEN_PROGS := memcpy_user_test memcpy_general_workload memcpy_subsystem_workload
TEST_GEN_MODS_DIR := test_modules

include ../../lib.mk

$(OUTPUT)/memcpy_user_test: memcpy_user_test.c
	mkdir -p $(@D)
	$(CC) -static -o$@ $(CFLAGS) $(LDFLAGS) $^

$(OUTPUT)/memcpy_general_workload: memcpy_general_workload.c
	mkdir -p $(@D)
	$(CC) -static -o$@ $(CFLAGS) $(LDFLAGS) $^

$(OUTPUT)/memcpy_subsystem_workload: memcpy_subsystem_workload.c
	mkdir -p $(@D)
	$(CC) -static -o$@ $(CFLAGS) $(LDFLAGS) $^
