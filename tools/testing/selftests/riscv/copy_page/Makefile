# SPDX-License-Identifier: GPL-2.0

TEST_GEN_PROGS := copy_page_debugfs copy_page_workloads

include ../../lib.mk

$(OUTPUT)/copy_page_debugfs: copy_page_debugfs.c
	$(CC) -static -o$@ $(CFLAGS) $(LDFLAGS) $^

$(OUTPUT)/copy_page_workloads: copy_page_workloads.c
	$(CC) -static -o$@ $(CFLAGS) $(LDFLAGS) $^
