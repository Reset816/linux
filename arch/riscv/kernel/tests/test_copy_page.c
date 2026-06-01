// SPDX-License-Identifier: GPL-2.0-only

#include <linux/debugfs.h>
#include <linux/irqflags.h>
#include <linux/kconfig.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/copy_page.h>
#include <asm/cpufeature.h>
#include <asm/simd.h>
#include <asm/vector.h>

#define COPY_PAGE_TEST_ITERS_DEFAULT	10000U
#define COPY_PAGE_TEST_GUARD		64U

static struct dentry *copy_page_test_dir;
static u32 test_iterations = COPY_PAGE_TEST_ITERS_DEFAULT;

static void fill_source(u8 *src)
{
	unsigned int i;

	for (i = 0; i < PAGE_SIZE; i++)
		src[i] = (u8)(i * 251 + (i >> 3) + 0x5a);
}

static bool check_guard(const u8 *buf, size_t len, u8 val)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (buf[i] != val)
			return false;
	}

	return true;
}

static u32 checksum_page(const u8 *buf)
{
	u32 sum = 0;
	unsigned int i;

	for (i = 0; i < PAGE_SIZE; i++)
		sum = (sum << 5) - sum + buf[i];

	return sum;
}

static void copy_page_test_run_one(struct seq_file *m)
{
	struct riscv_copy_page_test_stats bench_stats;
	u8 *src_alloc, *dst_alloc, *src, *dst;
	unsigned long flags;
	bool vector_before;
	bool vector_irq_disabled;
	bool direct_vector_legal;
	bool copy_user_ok;
	bool ok = true;
	u64 copy_ns;
	u64 scalar_ns;
	u64 direct_vector_ns = 0;
	ktime_t start;
	u32 src_sum;
	u32 dst_sum;
	u32 iterations = test_iterations ? test_iterations : 1;
	u32 i;

	src_alloc = kmalloc(PAGE_SIZE + 2 * COPY_PAGE_TEST_GUARD, GFP_KERNEL);
	dst_alloc = kmalloc(PAGE_SIZE + 2 * COPY_PAGE_TEST_GUARD, GFP_KERNEL);
	if (!src_alloc || !dst_alloc) {
		seq_puts(m, "status: alloc-failed\n");
		goto out_free;
	}

	src = src_alloc + COPY_PAGE_TEST_GUARD;
	dst = dst_alloc + COPY_PAGE_TEST_GUARD;
	memset(src_alloc, 0xa5, PAGE_SIZE + 2 * COPY_PAGE_TEST_GUARD);
	memset(dst_alloc, 0x3c, PAGE_SIZE + 2 * COPY_PAGE_TEST_GUARD);
	fill_source(src);

	vector_before = riscv_copy_page_test_uses_vector();
	copy_page(dst, src);
	src_sum = checksum_page(src);
	dst_sum = checksum_page(dst);
	ok &= !memcmp(dst, src, PAGE_SIZE);
	ok &= src_sum == dst_sum;
	ok &= check_guard(dst_alloc, COPY_PAGE_TEST_GUARD, 0x3c);
	ok &= check_guard(dst + PAGE_SIZE, COPY_PAGE_TEST_GUARD, 0x3c);

	memset(dst, 0, PAGE_SIZE);
	copy_user_page(dst, src, 0, NULL);
	copy_user_ok = !memcmp(dst, src, PAGE_SIZE);
	ok &= copy_user_ok;

	local_irq_save(flags);
	vector_irq_disabled = riscv_copy_page_test_uses_vector();
	copy_page(dst, src);
	local_irq_restore(flags);
	if (!IS_ENABLED(CONFIG_RISCV_ISA_V_PREEMPTIVE))
		ok &= !vector_irq_disabled;
	ok &= !memcmp(dst, src, PAGE_SIZE);

	direct_vector_legal = riscv_copy_page_test_vector_legal();
	if (direct_vector_legal) {
		memset(dst, 0, PAGE_SIZE);
		riscv_copy_page_test_vector_copy(dst, src);
		ok &= !memcmp(dst, src, PAGE_SIZE);
	}

	riscv_copy_page_test_stats_reset();
	start = ktime_get();
	for (i = 0; i < iterations; i++)
		copy_page(dst, src);
	copy_ns = ktime_to_ns(ktime_sub(ktime_get(), start));
	riscv_copy_page_test_stats_read(&bench_stats);

	start = ktime_get();
	for (i = 0; i < iterations; i++)
		memcpy(dst, src, PAGE_SIZE);
	scalar_ns = ktime_to_ns(ktime_sub(ktime_get(), start));

	if (direct_vector_legal) {
		start = ktime_get();
		for (i = 0; i < iterations; i++)
			riscv_copy_page_test_vector_copy(dst, src);
		direct_vector_ns = ktime_to_ns(ktime_sub(ktime_get(), start));
	}

	seq_printf(m, "status: %s\n", ok ? "ok" : "fail");
	seq_printf(m, "vector_available: %u\n", has_vector());
	seq_printf(m, "vector_vlen_bits: %d\n", riscv_vector_vlen());
	seq_printf(m, "may_use_simd: %u\n", may_use_simd());
	seq_printf(m, "preemptive_kernel_vector: %u\n",
		   IS_ENABLED(CONFIG_RISCV_ISA_V_PREEMPTIVE));
	seq_printf(m, "copy_page_vector_eligible: %u\n", vector_before);
	seq_printf(m, "copy_user_page_ok: %u\n", copy_user_ok);
	seq_printf(m, "direct_vector_legal: %u\n", direct_vector_legal);
	seq_printf(m, "irq_disabled_vector_eligible: %u\n", vector_irq_disabled);
	seq_printf(m, "iterations: %u\n", iterations);
	seq_printf(m, "checksum: 0x%08x\n", dst_sum);
	seq_printf(m, "copy_page_ns_total: %llu\n", copy_ns);
	seq_printf(m, "copy_page_ns_per_iter: %llu\n",
		   div_u64(copy_ns, iterations));
	seq_printf(m, "copy_page_calls: %llu\n", bench_stats.calls);
	seq_printf(m, "copy_page_vector_calls: %llu\n",
		   bench_stats.vector_calls);
	seq_printf(m, "memcpy_ns_total: %llu\n", scalar_ns);
	seq_printf(m, "memcpy_ns_per_iter: %llu\n",
		   div_u64(scalar_ns, iterations));
	seq_printf(m, "direct_vector_ns_total: %llu\n", direct_vector_ns);
	seq_printf(m, "direct_vector_ns_per_iter: %llu\n",
		   direct_vector_legal ? div_u64(direct_vector_ns, iterations) : 0);

out_free:
	kfree(dst_alloc);
	kfree(src_alloc);
}

static int copy_page_test_show(struct seq_file *m, void *private)
{
	copy_page_test_run_one(m);
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(copy_page_test);

static int copy_page_test_stats_show(struct seq_file *m, void *private)
{
	struct riscv_copy_page_test_stats stats;

	riscv_copy_page_test_stats_read(&stats);
	seq_printf(m, "calls: %llu\n", stats.calls);
	seq_printf(m, "vector_calls: %llu\n", stats.vector_calls);
	seq_printf(m, "scalar_calls: %llu\n", stats.calls - stats.vector_calls);
	seq_printf(m, "copy_page_vector_eligible: %u\n",
		   riscv_copy_page_test_uses_vector());

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(copy_page_test_stats);

static ssize_t copy_page_test_reset_write(struct file *file,
					  const char __user *buf, size_t count,
					  loff_t *ppos)
{
	riscv_copy_page_test_stats_reset();
	return count;
}

static const struct file_operations copy_page_test_reset_fops = {
	.owner = THIS_MODULE,
	.write = copy_page_test_reset_write,
	.llseek = noop_llseek,
};

static int __init copy_page_test_init(void)
{
	copy_page_test_dir = debugfs_create_dir("riscv_copy_page_test", NULL);
	if (IS_ERR(copy_page_test_dir))
		return PTR_ERR(copy_page_test_dir);

	debugfs_create_u32("iterations", 0600, copy_page_test_dir,
			   &test_iterations);
	debugfs_create_file("run", 0400, copy_page_test_dir, NULL,
			    &copy_page_test_fops);
	debugfs_create_file("stats", 0400, copy_page_test_dir, NULL,
			    &copy_page_test_stats_fops);
	debugfs_create_file("reset_stats", 0200, copy_page_test_dir, NULL,
			    &copy_page_test_reset_fops);

	return 0;
}
module_init(copy_page_test_init);

static void __exit copy_page_test_exit(void)
{
	debugfs_remove_recursive(copy_page_test_dir);
}
module_exit(copy_page_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RISC-V copy_page() runtime test");
