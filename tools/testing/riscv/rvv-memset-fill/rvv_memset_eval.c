// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vfs.h>
#include <linux/vmalloc.h>
#include <linux/xattr.h>

#include <asm/ptrace.h>
#include <asm/string.h>

#define EVAL_GUARD		32
#define EVAL_BUF_SIZE		(SZ_128K + PAGE_SIZE)
#define EVAL_MAX_ROWS		512
#define EVAL_MAX_BUCKETS	8
#define EVAL_MAX_TEXT		8192
#define EVAL_MAGIC		0xa5

typedef void *(*memset_fn_t)(void *, int, size_t);

enum eval_mode {
	EVAL_MODE_NORMAL,
	EVAL_MODE_IRQOFF,
};

struct eval_row {
	char kind[16];
	char mode[16];
	u8 value;
	unsigned int offset;
	size_t size;
	unsigned int iterations;
	u64 bytes;
	u64 ns;
	u64 mib_per_sec_x100;
	u64 ns_per_kib;
};

struct probe_stats {
	u64 calls;
	u64 bytes;
	u64 zero_calls;
	u64 zero_bytes;
	u64 nonzero_calls;
	u64 nonzero_bytes;
	u64 lt_1024_calls;
	u64 eq_1024_calls;
	u64 gt_1024_calls;
	u64 bucket_calls[EVAL_MAX_BUCKETS];
	u64 bucket_bytes[EVAL_MAX_BUCKETS];
};

struct eval_state {
	u64 failures;
	char first_failure[192];
	u64 correct_checks;
	struct eval_row rows[EVAL_MAX_ROWS];
	unsigned int nr_rows;
	struct probe_stats memset;
	struct probe_stats vector_entry;
	struct probe_stats vector_asm;
	struct probe_stats scalar;
	u64 irqoff_calls;
	u64 irqoff_bytes;
	bool probes_on;
	char text[EVAL_MAX_TEXT];
};

static memset_fn_t memset_call = __memset;
static struct proc_dir_entry *proc_entry;
static u8 *eval_buf;
static struct eval_state state;

static struct kprobe kp_memset;
static struct kprobe kp_vector_entry;
static struct kprobe kp_vector_asm;
static struct kprobe kp_scalar;

static void reset_state(void)
{
	bool probes_on = state.probes_on;
	u8 *p = (u8 *)&state;
	size_t i;

	for (i = 0; i < sizeof(state); i++)
		p[i] = 0;
	state.probes_on = probes_on;
}

static void record_failure(const char *fmt, ...)
{
	va_list args;

	if (state.failures++ != 0)
		return;

	va_start(args, fmt);
	vsnprintf(state.first_failure, sizeof(state.first_failure), fmt, args);
	va_end(args);
}

static void poison_buffer(u8 *buf, size_t size)
{
	volatile u8 *p = buf;
	size_t i;

	for (i = 0; i < size; i++)
		p[i] = EVAL_MAGIC;
}

static void *call_memset_mode(void *dst, int value, size_t bytes,
			      enum eval_mode mode)
{
	unsigned long flags;
	void *ret;

	if (mode == EVAL_MODE_IRQOFF) {
		local_irq_save(flags);
		ret = memset_call(dst, value, bytes);
		local_irq_restore(flags);
		state.irqoff_calls++;
		state.irqoff_bytes += bytes;
		return ret;
	}

	return memset_call(dst, value, bytes);
}

static unsigned int bucket_for_len(size_t len)
{
	if (len < 16)
		return 0;
	if (len < 64)
		return 1;
	if (len < 256)
		return 2;
	if (len < 1024)
		return 3;
	if (len == 1024)
		return 4;
	if (len < 4096)
		return 5;
	if (len < 16384)
		return 6;
	return 7;
}

static void note_probe(struct probe_stats *stats, int value, size_t len)
{
	unsigned int bucket = bucket_for_len(len);

	stats->calls++;
	stats->bytes += len;
	if ((value & 0xff) == 0) {
		stats->zero_calls++;
		stats->zero_bytes += len;
	} else {
		stats->nonzero_calls++;
		stats->nonzero_bytes += len;
	}
	if (len < 1024)
		stats->lt_1024_calls++;
	else if (len == 1024)
		stats->eq_1024_calls++;
	else
		stats->gt_1024_calls++;
	stats->bucket_calls[bucket]++;
	stats->bucket_bytes[bucket] += len;
}

static int probe_memset_pre(struct kprobe *p, struct pt_regs *regs)
{
	note_probe(&state.memset, regs_get_kernel_argument(regs, 1),
		   regs_get_kernel_argument(regs, 2));
	return 0;
}

static int probe_vector_entry_pre(struct kprobe *p, struct pt_regs *regs)
{
	note_probe(&state.vector_entry, regs_get_kernel_argument(regs, 1),
		   regs_get_kernel_argument(regs, 2));
	return 0;
}

static int probe_vector_asm_pre(struct kprobe *p, struct pt_regs *regs)
{
	note_probe(&state.vector_asm, regs_get_kernel_argument(regs, 1),
		   regs_get_kernel_argument(regs, 2));
	return 0;
}

static int probe_scalar_pre(struct kprobe *p, struct pt_regs *regs)
{
	note_probe(&state.scalar, regs_get_kernel_argument(regs, 1),
		   regs_get_kernel_argument(regs, 2));
	return 0;
}

static int set_probes(bool on)
{
	int ret;

	if (on == state.probes_on)
		return 0;

	if (!on) {
		unregister_kprobe(&kp_memset);
		unregister_kprobe(&kp_vector_entry);
		unregister_kprobe(&kp_vector_asm);
		unregister_kprobe(&kp_scalar);
		state.probes_on = false;
		return 0;
	}

	kp_memset = (struct kprobe) {
		.symbol_name = "__memset",
		.pre_handler = probe_memset_pre,
	};
	ret = register_kprobe(&kp_memset);
	if (ret)
		return ret;

	kp_vector_entry = (struct kprobe) {
		.symbol_name = "enter_vector_memset",
		.pre_handler = probe_vector_entry_pre,
	};
	ret = register_kprobe(&kp_vector_entry);
	if (ret)
		goto err_memset;

	kp_vector_asm = (struct kprobe) {
		.symbol_name = "__asm_rvv_memset",
		.pre_handler = probe_vector_asm_pre,
	};
	ret = register_kprobe(&kp_vector_asm);
	if (ret)
		goto err_vector_entry;

	kp_scalar = (struct kprobe) {
		.symbol_name = "__memset_scalar",
		.pre_handler = probe_scalar_pre,
	};
	ret = register_kprobe(&kp_scalar);
	if (ret)
		goto err_vector_asm;

	state.probes_on = true;
	return 0;

err_vector_asm:
	unregister_kprobe(&kp_vector_asm);
err_vector_entry:
	unregister_kprobe(&kp_vector_entry);
err_memset:
	unregister_kprobe(&kp_memset);
	return ret;
}

static void verify_one(size_t size, int value, unsigned int offset,
		       enum eval_mode mode)
{
	u8 *dst = eval_buf + EVAL_GUARD + offset;
	size_t start = EVAL_GUARD + offset;
	size_t end = start + size;
	u8 expected = value;
	void *ret;
	size_t i;

	if (end > EVAL_BUF_SIZE) {
		record_failure("range overflow mode=%d offset=%u size=%zu",
			       mode, offset, size);
		return;
	}

	state.correct_checks++;
	poison_buffer(eval_buf, EVAL_BUF_SIZE);
	ret = call_memset_mode(dst, value, size, mode);

	if (ret != dst) {
		record_failure("return mismatch mode=%d offset=%u size=%zu value=0x%x ret=%px dst=%px",
			       mode, offset, size, value, ret, dst);
		return;
	}

	for (i = 0; i < start; i++) {
		if (eval_buf[i] != EVAL_MAGIC) {
			record_failure("left guard mode=%d index=%zu offset=%u size=%zu value=0x%x got=0x%x",
				       mode, i, offset, size, value, eval_buf[i]);
			return;
		}
	}

	for (i = 0; i < size; i++) {
		if (dst[i] != expected) {
			record_failure("fill mode=%d index=%zu offset=%u size=%zu value=0x%x got=0x%x",
				       mode, i, offset, size, value, dst[i]);
			return;
		}
	}

	for (i = end; i < EVAL_BUF_SIZE; i++) {
		if (eval_buf[i] != EVAL_MAGIC) {
			record_failure("right guard mode=%d index=%zu offset=%u size=%zu value=0x%x got=0x%x",
				       mode, i, offset, size, value, eval_buf[i]);
			return;
		}
	}
}

static u64 mib_per_sec_x100(u64 bytes, u64 ns)
{
	if (!ns)
		return 0;

	return div64_u64(bytes * 100000000000ULL, ns * 1048576ULL);
}

static void add_row(const char *kind, const char *mode, int value,
		    unsigned int offset, size_t size, unsigned int iterations,
		    u64 bytes, u64 ns)
{
	struct eval_row *row;

	if (state.nr_rows >= EVAL_MAX_ROWS)
		return;

	row = &state.rows[state.nr_rows++];
	strscpy(row->kind, kind, sizeof(row->kind));
	strscpy(row->mode, mode, sizeof(row->mode));
	row->value = value;
	row->offset = offset;
	row->size = size;
	row->iterations = iterations;
	row->bytes = bytes;
	row->ns = ns;
	row->mib_per_sec_x100 = mib_per_sec_x100(bytes, ns);
	row->ns_per_kib = div64_u64(ns * 1024, bytes);
}

static void bench_one(size_t size, int value, unsigned int offset,
		      enum eval_mode mode, u64 target_bytes,
		      unsigned int max_iterations)
{
	u8 *dst = eval_buf + EVAL_GUARD + offset;
	unsigned int iterations;
	u64 bytes;
	u64 start;
	u64 ns;
	unsigned int i;

	if (!size)
		return;

	iterations = max_t(unsigned int, 1, div64_u64(target_bytes, size));
	iterations = min(iterations, max_iterations);
	bytes = (u64)iterations * size;

	poison_buffer(eval_buf, EVAL_BUF_SIZE);
	for (i = 0; i < 8; i++)
		call_memset_mode(dst, value, size, mode);

	barrier();
	start = ktime_get_ns();
	for (i = 0; i < iterations; i++)
		call_memset_mode(dst, value, size, mode);
	ns = ktime_get_ns() - start;
	barrier();

	add_row("bench", mode == EVAL_MODE_IRQOFF ? "irqoff" : "normal",
		value, offset, size, iterations, bytes, ns);
}

static void split_next(char **cursor, char *token, size_t token_size)
{
	char *start = *cursor;
	char *end;
	size_t len;

	while (*start == ' ' || *start == '\n' || *start == '\t')
		start++;
	end = start;
	while (*end && *end != ' ' && *end != '\n' && *end != '\t')
		end++;

	len = min_t(size_t, end - start, token_size - 1);
	memcpy(token, start, len);
	token[len] = '\0';
	*cursor = end;
}

static bool parse_key_value(const char *token, const char *key,
			    char *value, size_t value_size)
{
	size_t key_len = strlen(key);
	size_t len;

	if (strncmp(token, key, key_len) || token[key_len] != '=')
		return false;

	len = strscpy(value, token + key_len + 1, value_size);
	if (len < 0)
		value[value_size - 1] = '\0';
	return true;
}

static unsigned int parse_u32_list(const char *text, unsigned int *out,
				   unsigned int max_out, bool allow_all)
{
	char tmp[256];
	char *p;
	char *tok;
	unsigned int count = 0;

	if (allow_all && !strcmp(text, "all")) {
		unsigned int i;

		for (i = 0; i < 64 && count < max_out; i++)
			out[count++] = i;
		return count;
	}

	strscpy(tmp, text, sizeof(tmp));
	p = tmp;
	while ((tok = strsep(&p, ",")) && count < max_out) {
		unsigned int val;

		if (!*tok)
			continue;
		if (kstrtouint(tok, 0, &val))
			continue;
		out[count++] = val;
	}

	return count;
}

static unsigned int parse_size_list(const char *text, size_t *out,
				    unsigned int max_out)
{
	char tmp[256];
	char *p;
	char *tok;
	unsigned int count = 0;

	strscpy(tmp, text, sizeof(tmp));
	p = tmp;
	while ((tok = strsep(&p, ",")) && count < max_out) {
		unsigned long long val;

		if (!*tok)
			continue;
		if (kstrtoull(tok, 0, &val))
			continue;
		out[count++] = val;
	}

	return count;
}

static void run_correct_cmd(char *args)
{
	size_t sizes[32] = { 512, 768, 1023, 1024, 1025, 1536, 4096 };
	unsigned int nr_sizes = 7;
	unsigned int values[16] = { 0, 0x5a };
	unsigned int nr_values = 2;
	unsigned int offsets[80];
	unsigned int nr_offsets;
	char token[256];
	char value[256];
	unsigned int i, j, k;

	nr_offsets = parse_u32_list("all", offsets, ARRAY_SIZE(offsets), true);

	while (*args) {
		split_next(&args, token, sizeof(token));
		if (!token[0])
			break;
		if (parse_key_value(token, "sizes", value, sizeof(value)))
			nr_sizes = parse_size_list(value, sizes, ARRAY_SIZE(sizes));
		else if (parse_key_value(token, "values", value, sizeof(value)))
			nr_values = parse_u32_list(value, values, ARRAY_SIZE(values), false);
		else if (parse_key_value(token, "offsets", value, sizeof(value)))
			nr_offsets = parse_u32_list(value, offsets, ARRAY_SIZE(offsets), true);
	}

	for (i = 0; i < nr_values; i++) {
		for (j = 0; j < nr_offsets; j++) {
			for (k = 0; k < nr_sizes; k++) {
				verify_one(sizes[k], values[i], offsets[j], EVAL_MODE_NORMAL);
				verify_one(sizes[k], values[i], offsets[j], EVAL_MODE_IRQOFF);
			}
		}
	}
}

static void run_bench_cmd(char *args)
{
	size_t sizes[32] = { 1024, 1025, 4096, 65536 };
	unsigned int nr_sizes = 4;
	unsigned int values[16] = { 0, 0x5a };
	unsigned int nr_values = 2;
	unsigned int offsets[16] = { 0, 1 };
	unsigned int nr_offsets = 2;
	unsigned int max_iterations = 200;
	u64 target_bytes = 64 * 1024 * 1024ULL;
	bool do_normal = true;
	bool do_irqoff = true;
	char token[256];
	char value[256];
	unsigned int i, j, k;

	while (*args) {
		split_next(&args, token, sizeof(token));
		if (!token[0])
			break;
		if (parse_key_value(token, "sizes", value, sizeof(value)))
			nr_sizes = parse_size_list(value, sizes, ARRAY_SIZE(sizes));
		else if (parse_key_value(token, "values", value, sizeof(value)))
			nr_values = parse_u32_list(value, values, ARRAY_SIZE(values), false);
		else if (parse_key_value(token, "offsets", value, sizeof(value)))
			nr_offsets = parse_u32_list(value, offsets, ARRAY_SIZE(offsets), false);
		else if (parse_key_value(token, "iterations", value, sizeof(value))) {
			if (kstrtouint(value, 0, &max_iterations))
				record_failure("bad iterations=%s", value);
		} else if (parse_key_value(token, "bytes", value, sizeof(value))) {
			if (kstrtoull(value, 0, &target_bytes))
				record_failure("bad bytes=%s", value);
		}
		else if (parse_key_value(token, "modes", value, sizeof(value))) {
			do_normal = strstr(value, "normal") != NULL;
			do_irqoff = strstr(value, "irqoff") != NULL;
		}
	}

	for (i = 0; i < nr_values; i++) {
		for (j = 0; j < nr_offsets; j++) {
			for (k = 0; k < nr_sizes; k++) {
				if (do_normal)
					bench_one(sizes[k], values[i], offsets[j],
						  EVAL_MODE_NORMAL, target_bytes,
						  max_iterations);
				if (do_irqoff)
					bench_one(sizes[k], values[i], offsets[j],
						  EVAL_MODE_IRQOFF, target_bytes,
						  max_iterations);
			}
		}
	}
}

static void workload_alloc(unsigned int iterations, size_t size, int value)
{
	unsigned int i;
	u64 start;

	start = ktime_get_ns();
	for (i = 0; i < iterations; i++) {
		void *p = kmalloc(size, GFP_KERNEL);

		if (!p) {
			record_failure("kmalloc failed size=%zu", size);
			break;
		}
		memset_call(p, value, size);
		kfree(p);
	}
	add_row("workload", "alloc", value, 0, size, iterations,
		(u64)iterations * size, ktime_get_ns() - start);
}

static void workload_xattr(const char *path, unsigned int iterations,
			   size_t size, int value)
{
	struct path p;
	char name[32];
	u8 *buf;
	unsigned int i;
	u64 start;
	int ret;

	buf = vmalloc(size);
	if (!buf) {
		record_failure("xattr vmalloc failed size=%zu", size);
		return;
	}
	for (i = 0; i < size; i++)
		buf[i] = value;

	ret = kern_path(path, LOOKUP_DIRECTORY, &p);
	if (ret) {
		record_failure("kern_path failed path=%s ret=%d", path, ret);
		vfree(buf);
		return;
	}

	start = ktime_get_ns();
	for (i = 0; i < iterations; i++) {
		snprintf(name, sizeof(name), "user.rvv%u", i & 31);
		ret = vfs_setxattr(&nop_mnt_idmap, p.dentry, name, buf, size, 0);
		if (ret && ret != -EOPNOTSUPP && ret != -ENOTSUPP) {
			record_failure("vfs_setxattr ret=%d iteration=%u", ret, i);
			break;
		}
	}
	add_row("workload", "xattr", value, 0, size, i, (u64)i * size,
		ktime_get_ns() - start);

	path_put(&p);
	vfree(buf);
}

static void run_workload_cmd(char *args)
{
	char token[256];
	char value[256];
	char type[32] = "";
	char path[192] = "/tmp";
	unsigned int iterations = 1000;
	size_t size = 2048;
	unsigned int fill = 0x5a;

	split_next(&args, type, sizeof(type));

	while (*args) {
		split_next(&args, token, sizeof(token));
		if (!token[0])
			break;
		if (parse_key_value(token, "iterations", value, sizeof(value))) {
			if (kstrtouint(value, 0, &iterations))
				record_failure("bad iterations=%s", value);
		} else if (parse_key_value(token, "size", value, sizeof(value))) {
			unsigned long long parsed;

			if (kstrtoull(value, 0, &parsed))
				record_failure("bad size=%s", value);
			else
				size = parsed;
		} else if (parse_key_value(token, "value", value, sizeof(value))) {
			if (kstrtouint(value, 0, &fill))
				record_failure("bad value=%s", value);
		}
		else if (parse_key_value(token, "path", value, sizeof(value)))
			strscpy(path, value, sizeof(path));
	}

	if (!strcmp(type, "alloc"))
		workload_alloc(iterations, size, fill);
	else if (!strcmp(type, "xattr"))
		workload_xattr(path, iterations, size, fill);
}

static void run_command_line(char *line)
{
	char *args;
	int ret;

	args = strchr(line, ' ');
	if (args)
		*args++ = '\0';
	else
		args = line + strlen(line);

	if (!strcmp(line, "reset")) {
		reset_state();
		return;
	}
	if (!strcmp(line, "probe=1")) {
		ret = set_probes(true);
		if (ret)
			record_failure("set_probes(true) ret=%d", ret);
		return;
	}
	if (!strcmp(line, "probe=0")) {
		ret = set_probes(false);
		if (ret)
			record_failure("set_probes(false) ret=%d", ret);
		return;
	}
	if (!strcmp(line, "correct")) {
		run_correct_cmd(args);
		return;
	}
	if (!strcmp(line, "bench")) {
		run_bench_cmd(args);
		return;
	}
	if (!strcmp(line, "workload")) {
		run_workload_cmd(args);
		return;
	}
}

static ssize_t rvv_memset_eval_write(struct file *file,
				     const char __user *ubuf,
				     size_t len, loff_t *ppos)
{
	char *buf;
	char *p;
	char *line;

	if (!len)
		return 0;

	buf = memdup_user_nul(ubuf, min_t(size_t, len, EVAL_MAX_TEXT - 1));
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (!line[0] || line[0] == '#')
			continue;
		run_command_line(line);
	}

	kfree(buf);
	return len;
}

static void show_probe_stats(struct seq_file *m, const char *name,
			     const struct probe_stats *stats)
{
	static const char * const bucket_names[EVAL_MAX_BUCKETS] = {
		"lt16", "16_63", "64_255", "256_1023",
		"eq1024", "1025_4095", "4096_16383", "ge16384",
	};
	unsigned int i;

	seq_printf(m,
		   "probe %s calls=%llu bytes=%llu zero_calls=%llu zero_bytes=%llu nonzero_calls=%llu nonzero_bytes=%llu lt1024=%llu eq1024=%llu gt1024=%llu\n",
		   name, stats->calls, stats->bytes, stats->zero_calls,
		   stats->zero_bytes, stats->nonzero_calls,
		   stats->nonzero_bytes, stats->lt_1024_calls,
		   stats->eq_1024_calls, stats->gt_1024_calls);

	for (i = 0; i < EVAL_MAX_BUCKETS; i++)
		seq_printf(m, "probe_bucket %s %s calls=%llu bytes=%llu\n",
			   name, bucket_names[i], stats->bucket_calls[i],
			   stats->bucket_bytes[i]);
}

static int rvv_memset_eval_show(struct seq_file *m, void *v)
{
	u64 large_candidates = state.vector_entry.calls;
	u64 small_scalar = state.memset.lt_1024_calls;
	u64 zero_zicboz_scalar = state.scalar.zero_calls;
	u64 vector_calls = state.vector_asm.calls;
	u64 vector_bytes = state.vector_asm.bytes;
	u64 fallback_calls = 0;
	unsigned int i;

	if (large_candidates > vector_calls)
		fallback_calls = large_candidates - vector_calls;

	seq_printf(m, "status: %s\n", state.failures ? "FAIL" : "PASS");
	seq_printf(m, "failures: %llu\n", state.failures);
	if (state.failures)
		seq_printf(m, "first_failure: %s\n", state.first_failure);
	seq_printf(m, "correct_checks: %llu\n", state.correct_checks);
	seq_printf(m, "probes_on: %u\n", state.probes_on);
	seq_printf(m, "irqoff_calls: %llu\n", state.irqoff_calls);
	seq_printf(m, "irqoff_bytes: %llu\n", state.irqoff_bytes);
	seq_printf(m,
		   "summary large_candidates=%llu vector_calls=%llu vector_bytes=%llu fallback_calls=%llu small_scalar_calls=%llu zero_zicboz_scalar_calls=%llu irqoff_calls=%llu\n",
		   large_candidates, vector_calls, vector_bytes, fallback_calls,
		   small_scalar, zero_zicboz_scalar, state.irqoff_calls);

	for (i = 0; i < state.nr_rows; i++) {
		const struct eval_row *row = &state.rows[i];

		seq_printf(m,
			   "row kind=%s mode=%s value=0x%02x offset=%u size=%zu iterations=%u bytes=%llu ns=%llu mib_per_sec_x100=%llu ns_per_kib=%llu\n",
			   row->kind, row->mode, row->value, row->offset,
			   row->size, row->iterations, row->bytes, row->ns,
			   row->mib_per_sec_x100, row->ns_per_kib);
	}

	show_probe_stats(m, "__memset", &state.memset);
	show_probe_stats(m, "enter_vector_memset", &state.vector_entry);
	show_probe_stats(m, "__asm_rvv_memset", &state.vector_asm);
	show_probe_stats(m, "__memset_scalar", &state.scalar);

	return 0;
}

static int rvv_memset_eval_open(struct inode *inode, struct file *file)
{
	return single_open(file, rvv_memset_eval_show, NULL);
}

static const struct proc_ops rvv_memset_eval_ops = {
	.proc_open = rvv_memset_eval_open,
	.proc_read = seq_read,
	.proc_write = rvv_memset_eval_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init rvv_memset_eval_init(void)
{
	eval_buf = vmalloc(EVAL_BUF_SIZE);
	if (!eval_buf)
		return -ENOMEM;

	proc_entry = proc_create("rvv_memset_eval", 0600, NULL,
				 &rvv_memset_eval_ops);
	if (!proc_entry) {
		vfree(eval_buf);
		return -ENOMEM;
	}

	pr_info("rvv_memset_eval: loaded\n");
	return 0;
}

static void __exit rvv_memset_eval_exit(void)
{
	set_probes(false);
	remove_proc_entry("rvv_memset_eval", NULL);
	vfree(eval_buf);
	pr_info("rvv_memset_eval: unloaded\n");
}

module_init(rvv_memset_eval_init);
module_exit(rvv_memset_eval_exit);

MODULE_DESCRIPTION("RISC-V RVV memset K3 KVM evaluation helper");
MODULE_LICENSE("GPL");
