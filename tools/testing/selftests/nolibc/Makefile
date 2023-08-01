# SPDX-License-Identifier: GPL-2.0
# Makefile for nolibc tests
include ../../../scripts/Makefile.include
# We need this for the "cc-option" macro.
include ../../../build/Build.include

# we're in ".../tools/testing/selftests/nolibc"
ifeq ($(srctree),)
srctree := $(patsubst %/tools/testing/selftests/,%,$(dir $(CURDIR)))
endif

# add objtree for O= argument, required by IMAGE and .config
objtree ?= $(srctree)

ifeq ($(ARCH),)
include $(srctree)/scripts/subarch.include
ARCH = $(SUBARCH)
endif

# XARCH extends the kernel's ARCH with a few variants of the same
# architecture that only differ by the configuration, the toolchain
# and the Qemu program used. It is copied as-is into ARCH except for
# a few specific values which are mapped like this:
#
#  XARCH        | ARCH      | config
#  -------------|-----------|-------------------------
#  ppc          | powerpc   | 32 bits
#  ppc64        | powerpc   | 64 bits big endian
#  ppc64le      | powerpc   | 64 bits little endian
#
# It is recommended to only use XARCH, though it does not harm if
# ARCH is already set. For simplicity, ARCH is sufficient for all
# architectures where both are equal.

# configure default variants for target kernel supported architectures
XARCH            = $(or $(XARCH_$(ARCH)),$(ARCH))

# map from user input variants to their kernel supported architectures
ARCH            := $(or $(ARCH_$(XARCH)),$(XARCH))

# kernel image names by architecture
IMAGE_i386       = arch/x86/boot/bzImage
IMAGE_x86_64     = arch/x86/boot/bzImage
IMAGE_x86        = arch/x86/boot/bzImage
IMAGE_arm64      = arch/arm64/boot/Image
IMAGE_arm        = arch/arm/boot/zImage
IMAGE_mips       = vmlinuz
IMAGE_riscv      = arch/riscv/boot/Image
IMAGE_s390       = arch/s390/boot/bzImage
IMAGE_loongarch  = arch/loongarch/boot/vmlinuz.efi
IMAGE            = $(IMAGE_$(XARCH))
IMAGE_NAME       = $(notdir $(IMAGE))

# default kernel configurations that appear to be usable
DEFCONFIG_i386       = defconfig
DEFCONFIG_x86_64     = defconfig
DEFCONFIG_x86        = defconfig
DEFCONFIG_arm64      = defconfig
DEFCONFIG_arm        = multi_v7_defconfig
DEFCONFIG_mips       = malta_defconfig
DEFCONFIG_riscv      = defconfig
DEFCONFIG_s390       = defconfig
DEFCONFIG_loongarch  = defconfig
DEFCONFIG            = $(DEFCONFIG_$(XARCH))

# extra configs/ files appended to .config during the nolibc-test-config target
# include common + architecture specific
NOLIBC_TEST_CONFIG   = nolibc-test-common.config nolibc-test-$(XARCH).config

# optional tests to run (default = all)
TEST =

# QEMU_ARCH: arch names used by qemu
QEMU_ARCH_i386       = i386
QEMU_ARCH_x86_64     = x86_64
QEMU_ARCH_x86        = x86_64
QEMU_ARCH_arm64      = aarch64
QEMU_ARCH_arm        = arm
QEMU_ARCH_mips       = mipsel  # works with malta_defconfig
QEMU_ARCH_riscv      = riscv64
QEMU_ARCH_s390       = s390x
QEMU_ARCH_loongarch  = loongarch64
QEMU_ARCH            = $(QEMU_ARCH_$(XARCH))

# QEMU_ARGS : some arch-specific args to pass to qemu
QEMU_ARGS_i386       = -M pc -append "console=ttyS0,9600 i8042.noaux panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS_x86_64     = -M pc -append "console=ttyS0,9600 i8042.noaux panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS_x86        = -M pc -append "console=ttyS0,9600 i8042.noaux panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS_arm64      = -M virt -cpu cortex-a53 -append "panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS_arm        = -M virt -append "panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS_mips       = -M malta -append "panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS_riscv      = -M virt -append "console=ttyS0 panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS_s390       = -M s390-ccw-virtio -m 1G -append "console=ttyS0 panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS_loongarch  = -M virt -append "console=ttyS0,115200 panic=-1 $(TEST:%=NOLIBC_TEST=%)"
QEMU_ARGS            = $(QEMU_ARGS_$(XARCH)) $(QEMU_ARGS_EXTRA)

# OUTPUT is only set when run from the main makefile, otherwise
# it defaults to this nolibc directory.
OUTPUT ?= $(CURDIR)/

ifeq ($(V),1)
Q=
else
Q=@
endif

CFLAGS_s390 = -m64
CFLAGS_mips = -EL
CFLAGS_STACKPROTECTOR ?= $(call cc-option,-mstack-protector-guard=global $(call cc-option,-fstack-protector-all))
CFLAGS  ?= -Os -fno-ident -fno-asynchronous-unwind-tables -std=c89 \
		$(call cc-option,-fno-stack-protector) \
		$(CFLAGS_$(XARCH)) $(CFLAGS_STACKPROTECTOR)
LDFLAGS := -s

REPORT  ?= awk '/\[OK\][\r]*$$/{p++} /\[FAIL\][\r]*$$/{if (!f) printf("\n"); f++; print;} /\[SKIPPED\][\r]*$$/{s++} \
		END{ printf("\n%3d test(s): %3d passed, %3d skipped, %3d failed => status: ", p+s+f, p, s, f); \
		if (f) printf("failure\n"); else if (s) printf("warning\n"); else printf("success\n");; \
		printf("\nSee all results in %s\n", ARGV[1]); }'

help:
	@echo "Supported targets under selftests/nolibc:"
	@echo "  all                    call the \"run\" target below"
	@echo "  help                   this help"
	@echo "  sysroot                create the nolibc sysroot here (uses \$$ARCH)"
	@echo "  nolibc-test            build the executable (uses \$$CC and \$$CROSS_COMPILE)"
	@echo "  libc-test              build an executable using the compiler's default libc instead"
	@echo "  run-user               runs the executable under QEMU (uses \$$XARCH, \$$TEST)"
	@echo "  initramfs              prepare the initramfs with nolibc-test"
	@echo "  nolibc-test-config     create a fresh new \$$(CONFIG) config with extra options from \$$NOLIBC_TEST_CONFIG files (uses \$$XARCH)"
	@echo "  defconfig              do nolibc-test-config with default config (uses \$$XARCH)"
	@echo "  kernel                 (re)build the kernel with the initramfs (uses \$$XARCH)"
	@echo "  run                    runs the kernel in QEMU after building it (uses \$$XARCH, \$$TEST)"
	@echo "  rerun                  runs a previously prebuilt kernel in QEMU (uses \$$XARCH, \$$TEST)"
	@echo "  clean                  clean the sysroot, initramfs, build and output files"
	@echo ""
	@echo "The output file is \"run.out\". Test ranges may be passed using \$$TEST."
	@echo ""
	@echo "Currently using the following variables:"
	@echo "  ARCH                   = $(ARCH)"
	@echo "  XARCH                  = $(XARCH)"
	@echo "  CROSS_COMPILE          = $(CROSS_COMPILE)"
	@echo "  CC                     = $(CC)"
	@echo "  OUTPUT                 = $(OUTPUT)"
	@echo "  TEST                   = $(TEST)"
	@echo "  QEMU_ARCH              = $(if $(QEMU_ARCH),$(QEMU_ARCH),UNKNOWN_ARCH) [determined from \$$XARCH]"
	@echo "  IMAGE_NAME             = $(if $(IMAGE_NAME),$(IMAGE_NAME),UNKNOWN_ARCH) [determined from \$$XARCH]"
	@echo "  NOLIBC_TEST_CONFIG     = $(strip $(foreach c,$(NOLIBC_TEST_CONFIG),$(wildcard $(CURDIR)/configs/$c))) [determined from \$$XARCH]"
	@echo ""

all: run

sysroot: sysroot/$(ARCH)/include

sysroot/$(ARCH)/include:
	$(Q)rm -rf sysroot/$(ARCH) sysroot/sysroot
	$(QUIET_MKDIR)mkdir -p sysroot
	$(Q)$(MAKE) -C ../../../include/nolibc ARCH=$(ARCH) OUTPUT=$(CURDIR)/sysroot/ headers_standalone
	$(Q)mv sysroot/sysroot sysroot/$(ARCH)

ifneq ($(NOLIBC_SYSROOT),0)
nolibc-test: nolibc-test.c sysroot/$(ARCH)/include
	$(QUIET_CC)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ \
	  -nostdlib -static -Isysroot/$(ARCH)/include $< -lgcc
else
nolibc-test: nolibc-test.c
	$(QUIET_CC)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ \
	  -nostdlib -static -include ../../../include/nolibc/nolibc.h $< -lgcc
endif

libc-test: nolibc-test.c
	$(QUIET_CC)$(CC) -o $@ $<

# common macros for logging
RUN_OUT = $(CURDIR)/run.out

# local libc-test
run-libc-test: libc-test
	$(Q)./libc-test > "$(RUN_OUT)" || :
	$(Q)$(REPORT) "$(RUN_OUT)"

# local nolibc-test
run-nolibc-test: nolibc-test
	$(Q)./nolibc-test > "$(RUN_OUT)" || :
	$(Q)$(REPORT) "$(RUN_OUT)"

# qemu user-land test
run-user: nolibc-test
	$(Q)qemu-$(QEMU_ARCH) ./nolibc-test > "$(RUN_OUT)" || :
	$(Q)$(REPORT) "$(RUN_OUT)"

initramfs: nolibc-test
	$(QUIET_MKDIR)mkdir -p initramfs
	$(call QUIET_INSTALL, initramfs/init)
	$(Q)cp nolibc-test initramfs/init

# common macros for kernel targets
MAKE_KERNEL   = $(MAKE) -C $(srctree) ARCH=$(ARCH) CC=$(CC) CROSS_COMPILE=$(CROSS_COMPILE)
KERNEL_CONFIG = $(objtree)/.config
KERNEL_IMAGE  = $(objtree)/$(IMAGE)

# kernel config for nolibc-test
#
# - delete the current configuration and all generated files via 'mrproper' target
# - generate .config via '$(CONFIG)' or '$(DEFCONFIG_$(XARCH))' target
# - merge extra config options from $(NOLIBC_TEST_CONFIG) files to .config
# - use merged .config as base and fills in any missing symbols with '# CONFIG_* is not set' via 'allnoconfig' target
# - prepare things we need to do before we recursively start building the kernel via 'prepare' target
#
nolibc-test-config:
	$(Q)$(MAKE_KERNEL) mrproper
	$(Q)$(MAKE_KERNEL) $(or $(CONFIG),$(DEFCONFIG))
	$(Q)$(srctree)/scripts/kconfig/merge_config.sh -Q -O "$(objtree)" -m "$(KERNEL_CONFIG)" $(foreach c,$(NOLIBC_TEST_CONFIG),$(wildcard $(CURDIR)/configs/$c))
	$(Q)$(MAKE_KERNEL) KCONFIG_ALLCONFIG=$(KERNEL_CONFIG) allnoconfig
	$(Q)$(MAKE_KERNEL) prepare

defconfig: nolibc-test-config

kernel: initramfs
	$(Q)$(MAKE_KERNEL) $(IMAGE_NAME) CONFIG_INITRAMFS_SOURCE=$(CURDIR)/initramfs

# common macros for qemu run/rerun targets
QEMU_SYSTEM_RUN = qemu-system-$(QEMU_ARCH) -display none -no-reboot -kernel "$(KERNEL_IMAGE)" -serial stdio $(QEMU_ARGS)

# run the tests after building the kernel
run: kernel
	$(Q)$(QEMU_SYSTEM_RUN) > "$(RUN_OUT)"
	$(Q)$(REPORT) "$(RUN_OUT)"

# re-run the tests from an existing kernel
rerun:
	$(Q)$(QEMU_SYSTEM_RUN) > "$(RUN_OUT)"
	$(Q)$(REPORT) "$(RUN_OUT)"

clean:
	$(call QUIET_CLEAN, sysroot)
	$(Q)rm -rf sysroot
	$(call QUIET_CLEAN, nolibc-test)
	$(Q)rm -f nolibc-test
	$(call QUIET_CLEAN, libc-test)
	$(Q)rm -f libc-test
	$(call QUIET_CLEAN, initramfs)
	$(Q)rm -rf initramfs
	$(call QUIET_CLEAN, run.out)
	$(Q)rm -rf run.out

.PHONY: sysroot/$(ARCH)/include
