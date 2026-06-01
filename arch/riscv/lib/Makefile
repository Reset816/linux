# SPDX-License-Identifier: GPL-2.0-only
lib-y			+= delay.o
lib-y			+= memcpy.o
KASAN_SANITIZE_memcpy_vector_wrapper.o := n
KCOV_INSTRUMENT_memcpy_vector_wrapper.o := n
UBSAN_SANITIZE_memcpy_vector_wrapper.o := n
CFLAGS_memcpy_vector_wrapper.o := -ffreestanding
lib-$(CONFIG_RISCV_ISA_V)	+= memcpy_vector_wrapper.o
lib-$(CONFIG_RISCV_ISA_V)	+= memcpy_vector.o
lib-y			+= memset.o
lib-y			+= memmove.o
ifeq ($(CONFIG_KASAN_GENERIC)$(CONFIG_KASAN_SW_TAGS),)
lib-y			+= strcmp.o
lib-y			+= strlen.o
lib-y			+= strncmp.o
endif
lib-y			+= csum.o
ifeq ($(CONFIG_MMU), y)
lib-$(CONFIG_RISCV_ISA_V)	+= uaccess_vector.o
endif
lib-$(CONFIG_MMU)	+= uaccess.o
lib-$(CONFIG_64BIT)	+= tishift.o
lib-$(CONFIG_RISCV_ISA_ZICBOZ)	+= clear_page.o
obj-$(CONFIG_FUNCTION_ERROR_INJECTION) += error-inject.o
lib-$(CONFIG_RISCV_ISA_V)	+= xor.o
lib-$(CONFIG_RISCV_ISA_V)	+= riscv_v_helpers.o
