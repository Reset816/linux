// SPDX-License-Identifier: GPL-2.0-only
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kconfig.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/string.h>

#include <asm/copy_page.h>
#include <asm/simd.h>
#include <asm/vector.h>

#define RISCV_COPY_PAGE_VECTOR_MIN_VLEN	128
#define RISCV_COPY_PAGE_VECTOR_PROBE_ITERS 512

enum riscv_copy_page_policy {
	RISCV_COPY_PAGE_AUTO,
	RISCV_COPY_PAGE_SCALAR,
	RISCV_COPY_PAGE_VECTOR,
};

static DEFINE_STATIC_KEY_FALSE(riscv_copy_page_vector_key);
static enum riscv_copy_page_policy riscv_copy_page_policy = RISCV_COPY_PAGE_AUTO;

static int __init riscv_copy_page_setup(char *str)
{
	if (!strcmp(str, "auto"))
		riscv_copy_page_policy = RISCV_COPY_PAGE_AUTO;
	else if (!strcmp(str, "scalar") || !strcmp(str, "off"))
		riscv_copy_page_policy = RISCV_COPY_PAGE_SCALAR;
	else if (!strcmp(str, "vector") || !strcmp(str, "on"))
		riscv_copy_page_policy = RISCV_COPY_PAGE_VECTOR;
	else
		pr_warn("riscv: copy_page: ignoring unknown policy '%s'\n", str);

	return 1;
}
__setup("riscv.copy_page=", riscv_copy_page_setup);

static __always_inline bool riscv_copy_page_vector_legal(void)
{
	return has_vector() &&
	       riscv_vector_vlen() >= RISCV_COPY_PAGE_VECTOR_MIN_VLEN &&
	       may_use_simd();
}

static __always_inline bool riscv_copy_page_use_vector(void)
{
	return static_branch_unlikely(&riscv_copy_page_vector_key) &&
	       riscv_copy_page_vector_legal();
}

void copy_page(void *to, const void *from)
{
	if (riscv_copy_page_use_vector()) {
		kernel_vector_begin();
		__riscv_copy_page_vector(to, from);
		kernel_vector_end();
		return;
	}

	memcpy(to, from, PAGE_SIZE);
}
EXPORT_SYMBOL(copy_page);

static void __init riscv_copy_page_probe_fill(void *page)
{
	u8 *buf = page;
	unsigned int i;

	for (i = 0; i < PAGE_SIZE; i++)
		buf[i] = (u8)(i * 251 + (i >> 3) + 0x5a);
}

static u32 __init riscv_copy_page_probe_checksum(const void *page)
{
	const u8 *buf = page;
	u32 sum = 0;
	unsigned int i;

	for (i = 0; i < PAGE_SIZE; i++)
		sum = (sum << 5) - sum + buf[i];

	return sum;
}

static u64 __init riscv_copy_page_probe_memcpy(void *to, const void *from)
{
	ktime_t start;
	unsigned int i;

	start = ktime_get();
	for (i = 0; i < RISCV_COPY_PAGE_VECTOR_PROBE_ITERS; i++)
		memcpy(to, from, PAGE_SIZE);

	return ktime_to_ns(ktime_sub(ktime_get(), start));
}

static u64 __init riscv_copy_page_probe_vector(void *to, const void *from)
{
	ktime_t start;
	unsigned int i;

	start = ktime_get();
	for (i = 0; i < RISCV_COPY_PAGE_VECTOR_PROBE_ITERS; i++) {
		kernel_vector_begin();
		__riscv_copy_page_vector(to, from);
		kernel_vector_end();
	}

	return ktime_to_ns(ktime_sub(ktime_get(), start));
}

static int __init riscv_copy_page_vector_probe(void)
{
	struct page *src_page, *dst_page;
	u64 scalar_ns, vector_ns;
	void *src, *dst;
	u32 src_sum, dst_sum;

	if (!has_vector() || riscv_vector_vlen() < RISCV_COPY_PAGE_VECTOR_MIN_VLEN ||
	    !may_use_simd()) {
		if (riscv_copy_page_policy == RISCV_COPY_PAGE_VECTOR)
			pr_warn("riscv: copy_page: vector copy requested but unavailable, vlen=%d\n",
				riscv_vector_vlen());
		return 0;
	}

	src_page = alloc_page(GFP_KERNEL);
	dst_page = alloc_page(GFP_KERNEL);
	if (!src_page || !dst_page)
		goto out;

	src = page_address(src_page);
	dst = page_address(dst_page);
	riscv_copy_page_probe_fill(src);

	memcpy(dst, src, PAGE_SIZE);
	kernel_vector_begin();
	__riscv_copy_page_vector(dst, src);
	kernel_vector_end();

	scalar_ns = min(riscv_copy_page_probe_memcpy(dst, src),
			riscv_copy_page_probe_memcpy(dst, src));
	vector_ns = min(riscv_copy_page_probe_vector(dst, src),
			riscv_copy_page_probe_vector(dst, src));

	src_sum = riscv_copy_page_probe_checksum(src);
	dst_sum = riscv_copy_page_probe_checksum(dst);
	if (src_sum != dst_sum || memcmp(dst, src, PAGE_SIZE)) {
		pr_warn("riscv: copy_page: vector probe failed correctness check\n");
		goto out;
	}

	if (riscv_copy_page_policy == RISCV_COPY_PAGE_SCALAR) {
		pr_info("riscv: copy_page: forced scalar copy, vlen=%d, vector=%lluns, scalar=%lluns per %u pages\n",
			riscv_vector_vlen(), vector_ns, scalar_ns,
			RISCV_COPY_PAGE_VECTOR_PROBE_ITERS);
	} else if (riscv_copy_page_policy == RISCV_COPY_PAGE_VECTOR ||
		   vector_ns < scalar_ns) {
		static_branch_enable(&riscv_copy_page_vector_key);
		pr_info("riscv: copy_page: using vector copy%s, vlen=%d, vector=%lluns, scalar=%lluns per %u pages\n",
			riscv_copy_page_policy == RISCV_COPY_PAGE_VECTOR ?
			" by request" : "",
			riscv_vector_vlen(), vector_ns, scalar_ns,
			RISCV_COPY_PAGE_VECTOR_PROBE_ITERS);
	} else {
		pr_info("riscv: copy_page: keeping scalar copy, vlen=%d, vector=%lluns, scalar=%lluns per %u pages\n",
			riscv_vector_vlen(), vector_ns, scalar_ns,
			RISCV_COPY_PAGE_VECTOR_PROBE_ITERS);
	}

out:
	if (dst_page)
		__free_page(dst_page);
	if (src_page)
		__free_page(src_page);

	return 0;
}
late_initcall(riscv_copy_page_vector_probe);
