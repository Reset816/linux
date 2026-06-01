/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <asm/asm.h>

#define pDst a0
#define iNum a2
#define iVL a3

#define ELEM_LMUL_SETTING m8
#define vData v0

/* void __asm_rvv_memset(void *s, int c, size_t count) */
SYM_FUNC_START(__asm_rvv_memset)
	beqz	iNum, 2f

	vsetvli	iVL, iNum, e8, ELEM_LMUL_SETTING, ta, ma
	vmv.v.x	vData, a1
1:
	vse8.v	vData, (pDst)
	sub	iNum, iNum, iVL
	add	pDst, pDst, iVL
	beqz	iNum, 2f
	vsetvli	iVL, iNum, e8, ELEM_LMUL_SETTING, ta, ma
	j	1b
2:
	ret
SYM_FUNC_END(__asm_rvv_memset)
