/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <asm/asm.h>

#define pDst a0
#define pSrc a1
#define iNum a2

#define iVL a3

#define ELEM_LMUL_SETTING m8
#define vData v0

/* void __asm_vector_memcpy(void *dst, const void *src, size_t n) */
SYM_FUNC_START(__asm_vector_memcpy)
1:
	vsetvli	iVL, iNum, e8, ELEM_LMUL_SETTING, ta, ma
	vle8.v	vData, (pSrc)
	sub	iNum, iNum, iVL
	add	pSrc, pSrc, iVL
	vse8.v	vData, (pDst)
	add	pDst, pDst, iVL
	bnez	iNum, 1b

	ret
SYM_FUNC_END(__asm_vector_memcpy)
