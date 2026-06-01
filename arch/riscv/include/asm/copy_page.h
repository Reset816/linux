/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _ASM_RISCV_COPY_PAGE_H
#define _ASM_RISCV_COPY_PAGE_H

#include <linux/kconfig.h>
#include <linux/types.h>

void __riscv_copy_page_vector(void *to, const void *from);

#if IS_ENABLED(CONFIG_RISCV_COPY_PAGE_TEST)
struct riscv_copy_page_test_stats {
	u64 calls;
	u64 vector_calls;
};

bool riscv_copy_page_test_uses_vector(void);
bool riscv_copy_page_test_vector_legal(void);
void riscv_copy_page_test_vector_copy(void *to, const void *from);
void riscv_copy_page_test_stats_reset(void);
void riscv_copy_page_test_stats_read(struct riscv_copy_page_test_stats *stats);
#endif

#endif /* _ASM_RISCV_COPY_PAGE_H */
