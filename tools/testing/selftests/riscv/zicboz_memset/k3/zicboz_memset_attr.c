// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/cacheflush.h>
#include <linux/debugfs.h>
#include <linux/kprobes.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>

enum memset_bucket {
	BUCKET_LT_128,
	BUCKET_128_1K,
	BUCKET_1K_4K,
	BUCKET_GE_4K,
	BUCKET_NR,
};

struct bucket_stats {
	u64 calls;
	u64 bytes;
	u64 zero_calls;
	u64 zero_bytes;
	u64 nonzero_calls;
	u64 nonzero_bytes;
};

struct memset_stats {
	u64 calls;
	u64 bytes;
	u64 zero_calls;
	u64 zero_bytes;
	u64 nonzero_calls;
	u64 nonzero_bytes;
	u64 eligible_calls;
	u64 eligible_bytes;
	u64 overflow_calls;
	struct bucket_stats buckets[BUCKET_NR];
};

static DEFINE_PER_CPU(struct memset_stats, pcpu_stats);
static struct dentry *debugfs_dir;
static bool enabled = true;
static char symbol[KSYM_NAME_LEN] = "__memset";

module_param(enabled, bool, 0600);
module_param_string(symbol, symbol, KSYM_NAME_LEN, 0444);

static const char * const bucket_names[BUCKET_NR] = {
	[BUCKET_LT_128] = "<128",
	[BUCKET_128_1K] = "128-1K",
	[BUCKET_1K_4K] = "1K-4K",
	[BUCKET_GE_4K] = ">=4K",
};

static enum memset_bucket bucket_for_len(unsigned long len)
{
	if (len < 128)
		return BUCKET_LT_128;
	if (len < 1024)
		return BUCKET_128_1K;
	if (len < 4096)
		return BUCKET_1K_4K;
	return BUCKET_GE_4K;
}

static u64 aligned_cbo_eligible_bytes(unsigned long dst, unsigned long len)
{
	unsigned long block_size = READ_ONCE(riscv_cboz_block_size);
	unsigned long end, aligned_start, aligned_end;

	if (!len || len < 256 || block_size < 16 || !is_power_of_2(block_size))
		return 0;

	if (check_add_overflow(dst, len, &end))
		return 0;

	if (!virt_addr_valid((void *)dst) ||
	    !virt_addr_valid((void *)(end - 1)))
		return 0;

	aligned_start = ALIGN(dst, block_size);
	aligned_end = round_down(end, block_size);
	if (aligned_start >= aligned_end)
		return 0;

	return aligned_end - aligned_start;
}

static int __kprobes memset_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct memset_stats *stats;
	struct bucket_stats *bucket;
	unsigned long dst = regs->a0;
	unsigned long value = regs->a1;
	unsigned long len = regs->a2;
	u64 eligible;

	if (!READ_ONCE(enabled))
		return 0;

	stats = this_cpu_ptr(&pcpu_stats);
	bucket = &stats->buckets[bucket_for_len(len)];

	stats->calls++;
	stats->bytes += len;
	bucket->calls++;
	bucket->bytes += len;

	if (value == 0) {
		stats->zero_calls++;
		stats->zero_bytes += len;
		bucket->zero_calls++;
		bucket->zero_bytes += len;

		eligible = aligned_cbo_eligible_bytes(dst, len);
		if (eligible) {
			stats->eligible_calls++;
			stats->eligible_bytes += eligible;
		}
	} else {
		stats->nonzero_calls++;
		stats->nonzero_bytes += len;
		bucket->nonzero_calls++;
		bucket->nonzero_bytes += len;
	}

	return 0;
}

static struct kprobe memset_kprobe = {
	.pre_handler = memset_pre_handler,
};

static void zero_stats(struct memset_stats *stats)
{
	u64 *p = (u64 *)stats;

	for (size_t i = 0; i < sizeof(*stats) / sizeof(*p); i++)
		p[i] = 0;
}

static void reset_all_stats(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		zero_stats(per_cpu_ptr(&pcpu_stats, cpu));
}

static void sum_stats(struct memset_stats *sum)
{
	int cpu;

	zero_stats(sum);

	for_each_possible_cpu(cpu) {
		struct memset_stats *stats = per_cpu_ptr(&pcpu_stats, cpu);

		sum->calls += stats->calls;
		sum->bytes += stats->bytes;
		sum->zero_calls += stats->zero_calls;
		sum->zero_bytes += stats->zero_bytes;
		sum->nonzero_calls += stats->nonzero_calls;
		sum->nonzero_bytes += stats->nonzero_bytes;
		sum->eligible_calls += stats->eligible_calls;
		sum->eligible_bytes += stats->eligible_bytes;
		sum->overflow_calls += stats->overflow_calls;

		for (int i = 0; i < BUCKET_NR; i++) {
			sum->buckets[i].calls += stats->buckets[i].calls;
			sum->buckets[i].bytes += stats->buckets[i].bytes;
			sum->buckets[i].zero_calls += stats->buckets[i].zero_calls;
			sum->buckets[i].zero_bytes += stats->buckets[i].zero_bytes;
			sum->buckets[i].nonzero_calls += stats->buckets[i].nonzero_calls;
			sum->buckets[i].nonzero_bytes += stats->buckets[i].nonzero_bytes;
		}
	}
}

static int stats_show(struct seq_file *m, void *v)
{
	struct memset_stats sum;

	sum_stats(&sum);

	seq_printf(m, "symbol=%s enabled=%u cboz_block_size=%u\n",
		   symbol, READ_ONCE(enabled), READ_ONCE(riscv_cboz_block_size));
	seq_printf(m, "total calls=%llu bytes=%llu\n", sum.calls, sum.bytes);
	seq_printf(m, "zero calls=%llu bytes=%llu\n",
		   sum.zero_calls, sum.zero_bytes);
	seq_printf(m, "nonzero calls=%llu bytes=%llu\n",
		   sum.nonzero_calls, sum.nonzero_bytes);
	seq_printf(m, "eligible calls=%llu bytes=%llu\n",
		   sum.eligible_calls, sum.eligible_bytes);

	for (int i = 0; i < BUCKET_NR; i++) {
		struct bucket_stats *b = &sum.buckets[i];

		seq_printf(m,
			   "bucket %s calls=%llu bytes=%llu zero_calls=%llu zero_bytes=%llu nonzero_calls=%llu nonzero_bytes=%llu\n",
			   bucket_names[i], b->calls, b->bytes,
			   b->zero_calls, b->zero_bytes,
			   b->nonzero_calls, b->nonzero_bytes);
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(stats);

static ssize_t reset_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	reset_all_stats();
	return count;
}

static const struct file_operations reset_fops = {
	.owner = THIS_MODULE,
	.write = reset_write,
};

static int __init zicboz_memset_attr_init(void)
{
	int ret;

	memset_kprobe.symbol_name = symbol;
	reset_all_stats();

	ret = register_kprobe(&memset_kprobe);
	if (ret) {
		pr_err("register_kprobe(%s) failed: %d\n", symbol, ret);
		return ret;
	}

	debugfs_dir = debugfs_create_dir("zicboz_memset_attr", NULL);
	debugfs_create_file("stats", 0400, debugfs_dir, NULL, &stats_fops);
	debugfs_create_file("reset", 0200, debugfs_dir, NULL, &reset_fops);
	debugfs_create_bool("enabled", 0600, debugfs_dir, &enabled);

	pr_info("tracking %s at %p\n", symbol, memset_kprobe.addr);
	return 0;
}

static void __exit zicboz_memset_attr_exit(void)
{
	debugfs_remove_recursive(debugfs_dir);
	unregister_kprobe(&memset_kprobe);
	pr_info("unregistered %s\n", symbol);
}

module_init(zicboz_memset_attr_init);
module_exit(zicboz_memset_attr_exit);

MODULE_DESCRIPTION("K3 Zicboz memset attribution helper");
MODULE_LICENSE("GPL");
