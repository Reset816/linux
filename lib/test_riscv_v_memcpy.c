// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>

#define TEST_MAX_SIZE		8192
#define TEST_GUARD_SIZE		64
#define TEST_BUFFER_SIZE	(TEST_MAX_SIZE + TEST_GUARD_SIZE * 2 + 16)

static unsigned int iterations = 20000;
module_param(iterations, uint, 0444);
MODULE_PARM_DESC(iterations, "iterations per memcpy benchmark case");

static unsigned int repeats = 8;
module_param(repeats, uint, 0444);
MODULE_PARM_DESC(repeats, "correctness repeats per case");

extern void *__memcpy(void *dst, const void *src, size_t n);

static const size_t test_sizes[] = {
	64, 127, 128, 256, 1024, 2047, 2048, 2049, 4096, 8192,
};

static const unsigned int test_offsets[] = {
	0, 1, 3, 7, 8, 15,
};

static u8 pattern_byte(size_t index, unsigned int repeat)
{
	return (u8)(0x5a ^ (index * 131) ^ (repeat * 17));
}

static void fill_pattern(u8 *buf, size_t size, unsigned int repeat)
{
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = pattern_byte(i, repeat);
}

static bool check_pattern(const u8 *buf, size_t size, unsigned int repeat)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (buf[i] != pattern_byte(i, repeat)) {
			pr_err("mismatch at byte %zu: expected 0x%02x got 0x%02x\n",
			       i, pattern_byte(i, repeat), buf[i]);
			return false;
		}
	}

	return true;
}

static bool check_guard(const u8 *buf, size_t size, u8 value)
{
	size_t i;

	for (i = 0; i < size; i++) {
		if (buf[i] != value) {
			pr_err("guard changed at byte %zu: expected 0x%02x got 0x%02x\n",
			       i, value, buf[i]);
			return false;
		}
	}

	return true;
}

static bool should_benchmark(unsigned int dst_off, unsigned int src_off)
{
	return (dst_off == 0 && src_off == 0) ||
	       (dst_off == 1 && src_off == 0) ||
	       (dst_off == 0 && src_off == 1) ||
	       (dst_off == 7 && src_off == 3) ||
	       (dst_off == 15 && src_off == 8);
}

static int run_case(u8 *dst_base, u8 *src_base, size_t size,
		    unsigned int dst_off, unsigned int src_off)
{
	u8 *src = src_base + TEST_GUARD_SIZE + src_off;
	u8 *dst = dst_base + TEST_GUARD_SIZE + dst_off;
	unsigned int repeat;
	u64 start, elapsed;

	for (repeat = 0; repeat < repeats; repeat++) {
		memset(dst_base, 0xa5, TEST_BUFFER_SIZE);
		fill_pattern(src, size, repeat);

		if (__memcpy(dst, src, size) != dst) {
			pr_err("bad return size=%zu dst_off=%u src_off=%u\n",
			       size, dst_off, src_off);
			return -EINVAL;
		}

		if (!check_pattern(dst, size, repeat) ||
		    !check_guard(dst_base, TEST_GUARD_SIZE + dst_off, 0xa5) ||
		    !check_guard(dst + size,
				 TEST_BUFFER_SIZE - TEST_GUARD_SIZE - dst_off - size,
				 0xa5))
			return -EINVAL;
	}

	if (!should_benchmark(dst_off, src_off))
		return 0;

	start = ktime_get_ns();
	for (repeat = 0; repeat < iterations; repeat++)
		__memcpy(dst, src, size);
	elapsed = ktime_get_ns() - start;
	if (!elapsed)
		elapsed = 1;

	pr_info("bench size=%zu dst_off=%u src_off=%u iter=%u ns=%llu bytes_per_ns=%llu.%03llu\n",
		size, dst_off, src_off, iterations, elapsed,
		div64_u64((u64)size * iterations, elapsed),
		div64_u64(((u64)size * iterations % elapsed) * 1000, elapsed));

	return 0;
}

static int run_irq_disabled_fallback(u8 *dst_base, u8 *src_base)
{
	u8 *src = src_base + TEST_GUARD_SIZE;
	u8 *dst = dst_base + TEST_GUARD_SIZE;
	unsigned long flags;

	memset(dst_base, 0xa5, TEST_BUFFER_SIZE);
	fill_pattern(src, 4096, 0);

	local_irq_save(flags);
	__memcpy(dst, src, 4096);
	local_irq_restore(flags);

	if (!check_pattern(dst, 4096, 0))
		return -EINVAL;

	pr_info("irq-disabled fallback copy passed\n");

	return 0;
}

static int __init test_riscv_v_memcpy_init(void)
{
	u8 *src_base;
	u8 *dst_base;
	unsigned int i, d, s;
	int ret = 0;

	src_base = kzalloc(TEST_BUFFER_SIZE, GFP_KERNEL);
	dst_base = kzalloc(TEST_BUFFER_SIZE, GFP_KERNEL);
	if (!src_base || !dst_base) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(test_sizes); i++) {
		for (d = 0; d < ARRAY_SIZE(test_offsets); d++) {
			for (s = 0; s < ARRAY_SIZE(test_offsets); s++) {
				ret = run_case(dst_base, src_base, test_sizes[i],
					       test_offsets[d], test_offsets[s]);
				if (ret)
					goto out;
			}
		}
	}

	ret = run_irq_disabled_fallback(dst_base, src_base);
	if (!ret)
		pr_info("all cases passed\n");

out:
	kfree(dst_base);
	kfree(src_base);
	return ret;
}
module_init(test_riscv_v_memcpy_init);

MODULE_DESCRIPTION("RISC-V vector memcpy correctness and benchmark test");
MODULE_LICENSE("GPL");
