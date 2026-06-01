// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <linux/uaccess.h>

enum memcpy_len_bucket {
	MEMCPY_LT_128,
	MEMCPY_128_TO_1K,
	MEMCPY_1K_TO_4K,
	MEMCPY_GE_4K,
	MEMCPY_LEN_BUCKETS,
};

struct memcpy_bucket_stats {
	atomic64_t calls;
	atomic64_t bytes;
};

struct memcpy_trace_stats {
	atomic64_t calls;
	atomic64_t bytes;
	struct memcpy_bucket_stats len[MEMCPY_LEN_BUCKETS];
	struct memcpy_bucket_stats same_align;
	struct memcpy_bucket_stats mismatch_align;
	struct memcpy_bucket_stats mismatch_len[MEMCPY_LEN_BUCKETS];
};

static struct memcpy_trace_stats stats;
static struct kprobe memcpy_kp;
static struct proc_dir_entry *proc_entry;
static atomic_t enabled = ATOMIC_INIT(0);

static enum memcpy_len_bucket len_bucket(unsigned long len)
{
	if (len < 128)
		return MEMCPY_LT_128;
	if (len < 1024)
		return MEMCPY_128_TO_1K;
	if (len < 4096)
		return MEMCPY_1K_TO_4K;

	return MEMCPY_GE_4K;
}

static void add_bucket(struct memcpy_bucket_stats *bucket, unsigned long len)
{
	atomic64_inc(&bucket->calls);
	atomic64_add(len, &bucket->bytes);
}

static void reset_stats(void)
{
	int i;

	atomic64_set(&stats.calls, 0);
	atomic64_set(&stats.bytes, 0);
	atomic64_set(&stats.same_align.calls, 0);
	atomic64_set(&stats.same_align.bytes, 0);
	atomic64_set(&stats.mismatch_align.calls, 0);
	atomic64_set(&stats.mismatch_align.bytes, 0);

	for (i = 0; i < MEMCPY_LEN_BUCKETS; i++) {
		atomic64_set(&stats.len[i].calls, 0);
		atomic64_set(&stats.len[i].bytes, 0);
		atomic64_set(&stats.mismatch_len[i].calls, 0);
		atomic64_set(&stats.mismatch_len[i].bytes, 0);
	}
}

static int memcpy_pre_handler(struct kprobe *kp, struct pt_regs *regs)
{
	unsigned long dst = regs->a0;
	unsigned long src = regs->a1;
	unsigned long len = regs->a2;
	enum memcpy_len_bucket bucket = len_bucket(len);

	if (!atomic_read(&enabled))
		return 0;

	atomic64_inc(&stats.calls);
	atomic64_add(len, &stats.bytes);
	add_bucket(&stats.len[bucket], len);

	if (((dst ^ src) & 7) == 0) {
		add_bucket(&stats.same_align, len);
	} else {
		add_bucket(&stats.mismatch_align, len);
		add_bucket(&stats.mismatch_len[bucket], len);
	}

	return 0;
}

static void show_bucket(struct seq_file *m, const char *name,
			const struct memcpy_bucket_stats *bucket)
{
	seq_printf(m, "%s_calls=%lld\n", name, atomic64_read(&bucket->calls));
	seq_printf(m, "%s_bytes=%lld\n", name, atomic64_read(&bucket->bytes));
}

static int memcpy_trace_show(struct seq_file *m, void *v)
{
	static const char * const names[] = {
		[MEMCPY_LT_128] = "len_lt_128",
		[MEMCPY_128_TO_1K] = "len_128_1k",
		[MEMCPY_1K_TO_4K] = "len_1k_4k",
		[MEMCPY_GE_4K] = "len_ge_4k",
	};
	int i;

	seq_printf(m, "enabled=%d\n", atomic_read(&enabled));
	seq_printf(m, "calls=%lld\n", atomic64_read(&stats.calls));
	seq_printf(m, "bytes=%lld\n", atomic64_read(&stats.bytes));
	show_bucket(m, "same_align", &stats.same_align);
	show_bucket(m, "mismatch_align", &stats.mismatch_align);

	for (i = 0; i < MEMCPY_LEN_BUCKETS; i++) {
		show_bucket(m, names[i], &stats.len[i]);
		seq_printf(m, "mismatch_%s_calls=%lld\n", names[i],
			   atomic64_read(&stats.mismatch_len[i].calls));
		seq_printf(m, "mismatch_%s_bytes=%lld\n", names[i],
			   atomic64_read(&stats.mismatch_len[i].bytes));
	}

	return 0;
}

static int memcpy_trace_open(struct inode *inode, struct file *file)
{
	return single_open(file, memcpy_trace_show, NULL);
}

static ssize_t memcpy_trace_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char cmd;

	if (get_user(cmd, buf))
		return -EFAULT;

	if (cmd == '0') {
		atomic_set(&enabled, 0);
	} else {
		reset_stats();
		atomic_set(&enabled, 1);
	}

	return count;
}

static const struct proc_ops memcpy_trace_fops = {
	.proc_open = memcpy_trace_open,
	.proc_read = seq_read,
	.proc_write = memcpy_trace_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init trace_riscv_memcpy_args_init(void)
{
	int ret;

	atomic_set(&enabled, 0);
	reset_stats();

	memcpy_kp.symbol_name = "__memcpy";
	memcpy_kp.pre_handler = memcpy_pre_handler;

	ret = register_kprobe(&memcpy_kp);
	if (ret)
		return pr_err("register_kprobe(__memcpy) failed: %d\n", ret), ret;

	proc_entry = proc_create("riscv_memcpy_trace", 0600, NULL,
				 &memcpy_trace_fops);
	if (!proc_entry) {
		unregister_kprobe(&memcpy_kp);
		return -ENOMEM;
	}

	pr_info("tracing __memcpy args via /proc/riscv_memcpy_trace\n");

	return 0;
}

static void __exit trace_riscv_memcpy_args_exit(void)
{
	proc_remove(proc_entry);
	unregister_kprobe(&memcpy_kp);
}

module_init(trace_riscv_memcpy_args_init);
module_exit(trace_riscv_memcpy_args_exit);

MODULE_DESCRIPTION("RISC-V __memcpy argument tracing test module");
MODULE_LICENSE("GPL");
