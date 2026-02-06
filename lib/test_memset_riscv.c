// SPDX-License-Identifier: GPL-2.0
/*
 * RISC-V kernel memset() microbenchmark with Zicboz path visibility.
 *
 * The module measures memset() throughput for a fixed matrix of
 * (len, value, alignment offset) and reports:
 * - expected_zicboz: whether the current __memset logic should use cbo.zero
 * - probe_hits: number of executed cbo.zero instructions during the run
 *
 * The probe uses a kprobe on the cbo.zero instruction in __memset.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/align.h>
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include <asm/cacheflush.h>
#include <asm/bug.h>
#include <asm/cpufeature-macros.h>
#include <asm/hwcap.h>

#define CBOZ_MEMSET_THRESHOLD	128

#define CBO_ZERO_MASK		0xfff07fffU
#define CBO_ZERO_MATCH		0x0040200fU
#define CBO_SCAN_MAX_BYTES	512

static unsigned long bytes_per_case = 64UL * SZ_1M;
module_param(bytes_per_case, ulong, 0644);
MODULE_PARM_DESC(bytes_per_case, "Target number of bytes written per test case");

static unsigned int min_loops = 1000;
module_param(min_loops, uint, 0644);
MODULE_PARM_DESC(min_loops, "Minimum memset() calls per test case");

static unsigned long verify_bytes_per_case = 1UL * SZ_1M;
module_param(verify_bytes_per_case, ulong, 0644);
MODULE_PARM_DESC(verify_bytes_per_case, "Target bytes per case in probe verification phase");

static unsigned int verify_min_loops = 128;
module_param(verify_min_loops, uint, 0644);
MODULE_PARM_DESC(verify_min_loops, "Minimum loops per case in probe verification phase");

static unsigned int warmup_loops = 200;
module_param(warmup_loops, uint, 0644);
MODULE_PARM_DESC(warmup_loops, "Warmup memset() calls before timing");

static bool require_probe = true;
module_param(require_probe, bool, 0644);
MODULE_PARM_DESC(require_probe, "Fail module init if cbo.zero probe setup fails");

static struct kprobe cbo_probe;
static bool cbo_probe_ready;
static unsigned int cbo_probe_off;
static atomic64_t cbo_hits = ATOMIC64_INIT(0);
static struct task_struct *bench_task;
static bool probe_counting;
static unsigned long probe_dst_start;
static unsigned long probe_dst_end;

static const size_t sizes[] = {
	64, 96, 127, 128, 129, 256, 512, 1024, 4096, 65536,
};

static const u8 values[] = {
	0x00, 0x5a,
};

static noinline void bench_memset_call(void *dst, int value, size_t len)
{
	memset(dst, value, len);
}

static int cbo_probe_handler(struct kprobe *p, struct pt_regs *regs)
{
	if (READ_ONCE(probe_counting) && READ_ONCE(bench_task) == current) {
		unsigned long t0 = READ_ONCE(regs->t0);
		unsigned long start = READ_ONCE(probe_dst_start);
		unsigned long end = READ_ONCE(probe_dst_end);

		if (t0 >= start && t0 < end)
			atomic64_inc(&cbo_hits);
	}

	return 0;
}

static int find_cbo_path_probe_offset(unsigned long memset_addr, unsigned int *off)
{
	const u16 *insn16 = (const u16 *)memset_addr;
	unsigned int pos = 0;

	while (pos + sizeof(*insn16) <= CBO_SCAN_MAX_BYTES) {
		u16 half = READ_ONCE(*insn16);
		size_t insn_len = GET_INSN_LENGTH(half);

		if (insn_len == sizeof(u32) &&
		    pos + sizeof(u32) <= CBO_SCAN_MAX_BYTES) {
			u32 v = READ_ONCE(*(const u32 *)insn16);

			if ((v & CBO_ZERO_MASK) == CBO_ZERO_MATCH) {
				/*
				 * kprobes on RISC-V reject SYSTEM instructions, and
				 * cbo.zero is a SYSTEM instruction. Probe the next
				 * instruction in the same loop.
				 */
				*off = pos + insn_len;
				return 0;
			}
		}

		insn16 = (const u16 *)((const u8 *)insn16 + insn_len);
		pos += insn_len;
	}

	return -ENOENT;
}

static int setup_cbo_probe(void)
{
	struct kprobe lookup = {
		.symbol_name = "__memset",
	};
	unsigned long memset_addr;
	int ret;

	ret = register_kprobe(&lookup);
	if (ret)
		return ret;

	memset_addr = (unsigned long)lookup.addr;
	unregister_kprobe(&lookup);

	ret = find_cbo_path_probe_offset(memset_addr, &cbo_probe_off);
	if (ret)
		return ret;

	cbo_probe.addr = (kprobe_opcode_t *)(memset_addr + cbo_probe_off);
	cbo_probe.pre_handler = cbo_probe_handler;
	ret = register_kprobe(&cbo_probe);
	if (ret)
		return ret;

	cbo_probe_ready = true;
	pr_info("probe armed at __memset+0x%x (post-cbo.zero)\n", cbo_probe_off);

	return 0;
}

static bool zicboz_hw_enabled(void)
{
	if (IS_ENABLED(CONFIG_RISCV_MEMSET_DISABLE_ZICBOZ))
		return false;

	if (!IS_ENABLED(CONFIG_RISCV_ISA_ZICBOZ))
		return false;

	return riscv_has_extension_unlikely(RISCV_ISA_EXT_ZICBOZ);
}

static bool expect_zicboz_path(void *dst, int value, size_t len)
{
	unsigned long start = (unsigned long)dst;
	unsigned long end;
	unsigned long head;
	unsigned long body_end;
	unsigned int block_size = READ_ONCE(riscv_cboz_block_size);

	if ((u8)value != 0)
		return false;
	if (len < CBOZ_MEMSET_THRESHOLD)
		return false;
	if (!zicboz_hw_enabled())
		return false;
	if (!block_size || !is_power_of_2(block_size))
		return false;
	if (check_add_overflow(start, len, &end))
		return false;

	head = ALIGN(start, block_size);
	body_end = end & ~(unsigned long)(block_size - 1);

	return head < body_end;
}

static u64 loops_for_size_cfg(size_t len, u64 target_bytes, u32 min_iter)
{
	u64 loops;

	if (!len)
		return min_iter;

	loops = DIV_ROUND_UP_ULL(target_bytes, len);
	if (loops < min_iter)
		loops = min_iter;

	return loops;
}

static void run_verify_case(void *dst, size_t len, u8 value, size_t off)
{
	u64 loops = loops_for_size_cfg(len, verify_bytes_per_case, verify_min_loops);
	u64 warmup = min_t(u64, warmup_loops, loops);
	u64 i;
	s64 hits_before = 0;
	s64 hits_after = 0;
	s64 hits_delta = 0;
	unsigned long start = (unsigned long)dst;
	unsigned long end;
	bool expect = expect_zicboz_path(dst, value, len);
	bool match;

	migrate_disable();

	for (i = 0; i < warmup; i++)
		bench_memset_call(dst, value, len);

	if (cbo_probe_ready)
		hits_before = atomic64_read(&cbo_hits);

	if (check_add_overflow(start, len, &end))
		end = start;

	WRITE_ONCE(probe_dst_start, start);
	WRITE_ONCE(probe_dst_end, end);
	WRITE_ONCE(bench_task, current);
	WRITE_ONCE(probe_counting, true);
	barrier();

	for (i = 0; i < loops; i++)
		bench_memset_call(dst, value, len);

	barrier();
	WRITE_ONCE(probe_counting, false);
	WRITE_ONCE(bench_task, NULL);
	WRITE_ONCE(probe_dst_start, 0);
	WRITE_ONCE(probe_dst_end, 0);

	if (cbo_probe_ready)
		hits_after = atomic64_read(&cbo_hits);

	migrate_enable();

	hits_delta = hits_after - hits_before;
	match = expect ? (hits_delta > 0) : (hits_delta <= 0);

	if (match)
		pr_info("verify len=%6zu val=0x%02x off=%4zu loops=%8llu expect_zicboz=%d probe_hits=%lld verify=match\n",
			len, value, off, loops, expect, hits_delta);
	else
		pr_warn("verify len=%6zu val=0x%02x off=%4zu loops=%8llu expect_zicboz=%d probe_hits=%lld verify=mismatch\n",
			len, value, off, loops, expect, hits_delta);
}

static void run_perf_case(void *dst, size_t len, u8 value, size_t off)
{
	u64 loops = loops_for_size_cfg(len, bytes_per_case, min_loops);
	u64 warmup = min_t(u64, warmup_loops, loops);
	u64 i;
	u64 start_ns;
	u64 end_ns;
	u64 ns;
	u64 bytes;
	u64 mibps = 0;
	bool expect = expect_zicboz_path(dst, value, len);

	migrate_disable();

	for (i = 0; i < warmup; i++)
		bench_memset_call(dst, value, len);

	start_ns = ktime_get_ns();
	for (i = 0; i < loops; i++)
		bench_memset_call(dst, value, len);
	end_ns = ktime_get_ns();

	migrate_enable();

	ns = end_ns - start_ns;
	bytes = loops * len;

	if (ns && bytes)
		mibps = div64_u64(bytes * NSEC_PER_SEC, ns * SZ_1M);

	pr_info("perf   len=%6zu val=0x%02x off=%4zu loops=%10llu time=%10lluns MiB/s=%6llu expect_zicboz=%d\n",
		len, value, off, loops, ns, mibps, expect);
}

static int __init test_memset_riscv_init(void)
{
	size_t max_len = sizes[ARRAY_SIZE(sizes) - 1];
	unsigned int block_size = READ_ONCE(riscv_cboz_block_size);
	unsigned int align_bs;
	size_t offsets[3];
	size_t alloc_size;
	size_t max_off;
	u8 *raw;
	u8 *aligned;
	int ret;
	unsigned int i, j, k;

	if (!block_size || !is_power_of_2(block_size))
		block_size = 64;

	align_bs = block_size;
	offsets[0] = 0;
	offsets[1] = 1;
	offsets[2] = align_bs > 1 ? align_bs - 1 : 0;

	max_off = offsets[0];
	for (i = 1; i < ARRAY_SIZE(offsets); i++)
		max_off = max(max_off, offsets[i]);

	alloc_size = max_len + 2 * align_bs + max_off;
	raw = kmalloc(alloc_size, GFP_KERNEL);
	if (!raw)
		return -ENOMEM;

	if (IS_ENABLED(CONFIG_RISCV_MEMSET_DISABLE_ZICBOZ)) {
		pr_info("zicboz memset optimization disabled by config; skip probe phase\n");
	} else {
		ret = setup_cbo_probe();
		if (ret) {
			pr_warn("failed to arm cbo.zero probe (%d)\n", ret);
			if (require_probe) {
				kfree(raw);
				return ret;
			}
		}
	}

	pr_info("start benchmark: zicboz_cfg=%d zicboz_hw=%d cboz_block_size=%u perf_bytes_per_case=%lu verify_bytes_per_case=%lu\n",
		IS_ENABLED(CONFIG_RISCV_ISA_ZICBOZ), zicboz_hw_enabled(),
		READ_ONCE(riscv_cboz_block_size), bytes_per_case, verify_bytes_per_case);

	if (cbo_probe_ready) {
		pr_info("phase1: verify Zicboz path with probe\n");
		for (i = 0; i < ARRAY_SIZE(sizes); i++) {
			for (j = 0; j < ARRAY_SIZE(values); j++) {
				for (k = 0; k < ARRAY_SIZE(offsets); k++) {
					aligned = PTR_ALIGN(raw, align_bs);
					run_verify_case(aligned + offsets[k],
							sizes[i], values[j],
							offsets[k]);
				}
			}
		}

		unregister_kprobe(&cbo_probe);
		cbo_probe_ready = false;
		pr_info("phase1 done: probe disabled for performance phase\n");
	} else {
		pr_info("phase1 skipped: probe unavailable\n");
	}

	pr_info("phase2: measure performance without probe\n");
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (j = 0; j < ARRAY_SIZE(values); j++) {
			for (k = 0; k < ARRAY_SIZE(offsets); k++) {
				aligned = PTR_ALIGN(raw, align_bs);
				run_perf_case(aligned + offsets[k], sizes[i],
					      values[j], offsets[k]);
			}
		}
	}
	kfree(raw);

	return 0;
}
module_init(test_memset_riscv_init);

static void __exit test_memset_riscv_exit(void)
{
	if (cbo_probe_ready) {
		unregister_kprobe(&cbo_probe);
		cbo_probe_ready = false;
	}
}
module_exit(test_memset_riscv_exit);

MODULE_DESCRIPTION("RISC-V memset microbenchmark with Zicboz probe");
MODULE_LICENSE("GPL");
