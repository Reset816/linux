/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Regents of the University of California
 */


#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/asm-offsets.h>
#include <asm/alternative-macros.h>
#include <asm/hwcap.h>
#include <asm/insn-def.h>

#if defined(CONFIG_RISCV_ISA_ZICBOZ) && defined(CONFIG_64BIT) && \
	defined(CONFIG_MMU) && !defined(CONFIG_XIP_KERNEL) && \
	!defined(__RISCV_PURGATORY__)
#define CBOZ_ALT(order, old, new)				\
	ALTERNATIVE(old, new, 0,				\
		    ((order) << 16) | RISCV_ISA_EXT_ZICBOZ,	\
		    CONFIG_RISCV_ISA_ZICBOZ)
#endif

/* void *memset(void *, int, size_t) */
SYM_FUNC_START(__memset)
	move t0, a0  /* Preserve return value */

#if defined(CONFIG_RISCV_ISA_ZICBOZ) && defined(CONFIG_64BIT) && \
	defined(CONFIG_MMU) && !defined(CONFIG_XIP_KERNEL) && \
	!defined(__RISCV_PURGATORY__)
	/* Try cbo.zero only for large zero fills in ordinary linear memory. */
	bnez a1, .Lscalar
	sltiu a3, a2, 256
	bnez a3, .Lscalar
	CBOZ_ALT(12, "j .Lscalar", "nop")

	lw a6, riscv_cboz_block_size
	beqz a6, .Lscalar
	sltiu a3, a6, 16
	bnez a3, .Lscalar

	la a5, kernel_map
	REG_L a5, KERNEL_MAP_PAGE_OFFSET(a5)
	bltu t0, a5, .Lscalar
	la a4, high_memory
	REG_L a4, 0(a4)
	beqz a4, .Lscalar
	add a3, t0, a2
	bltu a3, t0, .Lscalar
	bltu a4, a3, .Lscalar

	/* a4 = align_up(start, block), a5 = align_down(end, block) */
	addi a4, a6, -1
	add a4, t0, a4
	neg a7, a6
	and a4, a4, a7
	and a5, a3, a7
	bgeu a4, a5, .Lscalar

	/*
	 * Fill the unaligned head with scalar byte stores. Only the full block
	 * interval [a4, a5) is cleared with cbo.zero.
	 */
	beq t0, a4, .Lcboz_head_done
.Lcboz_head:
	sb zero, 0(t0)
	addi t0, t0, 1
	bltu t0, a4, .Lcboz_head
.Lcboz_head_done:
	mv t0, a4

.Lcboz_loop:
	CBO_ZERO(t0)
	add t0, t0, a6
	bltu t0, a5, .Lcboz_loop

	beq t0, a3, .Ldone
.Lcboz_tail:
	sb zero, 0(t0)
	addi t0, t0, 1
	bltu t0, a3, .Lcboz_tail
	j .Ldone

.Lscalar:
#endif
	/* Defer to byte-oriented fill for small sizes */
	sltiu a3, a2, 16
	bnez a3, 4f

	/*
	 * Round to nearest XLEN-aligned address
	 * greater than or equal to start address
	 */
	addi a3, t0, SZREG-1
	andi a3, a3, ~(SZREG-1)
	beq a3, t0, 2f  /* Skip if already aligned */
	/* Handle initial misalignment */
	sub a4, a3, t0
1:
	sb a1, 0(t0)
	addi t0, t0, 1
	bltu t0, a3, 1b
	sub a2, a2, a4  /* Update count */

2: /* Duff's device with 32 XLEN stores per iteration */
	/* Broadcast value into all bytes */
	andi a1, a1, 0xff
	slli a3, a1, 8
	or a1, a3, a1
	slli a3, a1, 16
	or a1, a3, a1
#ifdef CONFIG_64BIT
	slli a3, a1, 32
	or a1, a3, a1
#endif

	/* Calculate end address */
	andi a4, a2, ~(SZREG-1)
	add a3, t0, a4

	andi a4, a4, 31*SZREG  /* Calculate remainder */
	beqz a4, 3f            /* Shortcut if no remainder */
	neg a4, a4
	addi a4, a4, 32*SZREG  /* Calculate initial offset */

	/* Adjust start address with offset */
	sub t0, t0, a4

	/* Jump into loop body */
	/* Assumes 32-bit instruction lengths */
	la a5, 3f
#ifdef CONFIG_64BIT
	srli a4, a4, 1
#endif
	add a5, a5, a4
	jr a5
3:
	REG_S a1,        0(t0)
	REG_S a1,    SZREG(t0)
	REG_S a1,  2*SZREG(t0)
	REG_S a1,  3*SZREG(t0)
	REG_S a1,  4*SZREG(t0)
	REG_S a1,  5*SZREG(t0)
	REG_S a1,  6*SZREG(t0)
	REG_S a1,  7*SZREG(t0)
	REG_S a1,  8*SZREG(t0)
	REG_S a1,  9*SZREG(t0)
	REG_S a1, 10*SZREG(t0)
	REG_S a1, 11*SZREG(t0)
	REG_S a1, 12*SZREG(t0)
	REG_S a1, 13*SZREG(t0)
	REG_S a1, 14*SZREG(t0)
	REG_S a1, 15*SZREG(t0)
	REG_S a1, 16*SZREG(t0)
	REG_S a1, 17*SZREG(t0)
	REG_S a1, 18*SZREG(t0)
	REG_S a1, 19*SZREG(t0)
	REG_S a1, 20*SZREG(t0)
	REG_S a1, 21*SZREG(t0)
	REG_S a1, 22*SZREG(t0)
	REG_S a1, 23*SZREG(t0)
	REG_S a1, 24*SZREG(t0)
	REG_S a1, 25*SZREG(t0)
	REG_S a1, 26*SZREG(t0)
	REG_S a1, 27*SZREG(t0)
	REG_S a1, 28*SZREG(t0)
	REG_S a1, 29*SZREG(t0)
	REG_S a1, 30*SZREG(t0)
	REG_S a1, 31*SZREG(t0)
	addi t0, t0, 32*SZREG
	bltu t0, a3, 3b
	andi a2, a2, SZREG-1  /* Update count */

4:
	/* Handle trailing misalignment */
	beqz a2, 6f
	add a3, t0, a2
5:
	sb a1, 0(t0)
	addi t0, t0, 1
	bltu t0, a3, 5b
6:
.Ldone:
	ret
SYM_FUNC_END(__memset)
SYM_FUNC_ALIAS_WEAK(memset, __memset)
SYM_FUNC_ALIAS(__pi_memset, __memset)
SYM_FUNC_ALIAS(__pi___memset, __memset)
