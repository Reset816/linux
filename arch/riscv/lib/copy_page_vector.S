/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/csr.h>
#include <asm/page.h>

#define pDst	a0
#define pSrc	a1
#define iNum	a2
#define iVL	a3

#define vData	v0

/* void __riscv_copy_page_vector(void *to, const void *from) */
SYM_FUNC_START(__riscv_copy_page_vector)
	li	iNum, PAGE_SIZE

	.option push
	.option arch, +zve32x
	csrw	CSR_VSTART, x0
1:
	vsetvli	iVL, iNum, e8, m8, ta, ma
	vle8.v	vData, (pSrc)
	sub	iNum, iNum, iVL
	add	pSrc, pSrc, iVL
	vse8.v	vData, (pDst)
	add	pDst, pDst, iVL
	bnez	iNum, 1b
	.option pop

	ret
SYM_FUNC_END(__riscv_copy_page_vector)
