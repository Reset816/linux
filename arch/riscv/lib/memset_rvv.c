// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026
 */

#include <linux/compiler_attributes.h>
#include <linux/init.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/types.h>

#include <asm/cpufeature.h>
#include <asm/hwcap.h>
#include <asm/simd.h>
#include <asm/vector.h>

asmlinkage void *__memset_scalar(void *s, int c, size_t count);
asmlinkage void __asm_rvv_memset(void *s, int c, size_t count);
asmlinkage void *enter_vector_memset(void *s, int c, size_t count);

static __always_inline bool riscv_rvv_memset_should_use(int c)
{
	if (system_state < SYSTEM_RUNNING)
		return false;

	if (!may_use_simd())
		return false;

	if ((c & 0xff) == 0 && IS_ENABLED(CONFIG_RISCV_ISA_ZICBOZ) &&
	    riscv_has_extension_unlikely(RISCV_ISA_EXT_ZICBOZ))
		return false;

	return true;
}

asmlinkage notrace void *enter_vector_memset(void *s, int c, size_t count)
{
	if (!riscv_rvv_memset_should_use(c))
		return __memset_scalar(s, c, count);

	kernel_vector_begin();
	__asm_rvv_memset(s, c, count);
	kernel_vector_end();
	return s;
}
