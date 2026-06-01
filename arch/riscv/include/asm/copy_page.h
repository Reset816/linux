/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _ASM_RISCV_COPY_PAGE_H
#define _ASM_RISCV_COPY_PAGE_H

#include <linux/kconfig.h>
#include <linux/types.h>

void __riscv_copy_page_vector(void *to, const void *from);


#endif /* _ASM_RISCV_COPY_PAGE_H */
