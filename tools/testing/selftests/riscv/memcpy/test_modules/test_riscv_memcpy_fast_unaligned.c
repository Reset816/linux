// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufeature.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/types.h>

#define TEST_ALIGN 8
#define GUARD_SIZE 64
#define MAX_COPY_SIZE (128 * 1024)
#define BUFFER_SIZE (MAX_COPY_SIZE + 2 * GUARD_SIZE + 2 * TEST_ALIGN)
#define BENCH_BYTES (32ULL * 1024 * 1024)
#define MIXED_BENCH_BYTES (64ULL * 1024 * 1024)

static bool run_bench;
module_param(run_bench, bool, 0444);
MODULE_PARM_DESC(run_bench, "run memcpy benchmark cases");

static u8 *src_buf;
static u8 *dst_buf;
static u8 *ref_buf;

static void fill_source(u8 *buf, size_t size, u32 seed)
{
	size_t i;

	for (i = 0; i < size; i++) {
		seed = seed * 1664525U + 1013904223U;
		buf[i] = seed >> 24;
	}
}

static void fill_pattern(u8 *buf, size_t size, u8 base)
{
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = base + (i * 37U);
}

static void byte_copy(u8 *dst, const u8 *src, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		dst[i] = src[i];
}

static int check_one(size_t size, size_t dst_off, size_t src_off)
{
	u8 *src = src_buf + GUARD_SIZE + src_off;
	u8 *dst = dst_buf + GUARD_SIZE + dst_off;
	u8 *ref = ref_buf + GUARD_SIZE + dst_off;
	size_t left = GUARD_SIZE + dst_off;
	size_t right = BUFFER_SIZE - left - size;

	fill_source(src_buf, BUFFER_SIZE, 0x13579bdfU + src_off + size);
	fill_pattern(dst_buf, BUFFER_SIZE, 0x5a);
	byte_copy(ref_buf, dst_buf, BUFFER_SIZE);
	byte_copy(ref, src, size);

	if (__memcpy(dst, src, size) != dst)
		return pr_err("bad return size=%zu dst_off=%zu src_off=%zu\n",
			      size, dst_off, src_off), -EINVAL;

	if (memcmp(dst_buf, ref_buf, left))
		return pr_err("left guard changed size=%zu dst_off=%zu src_off=%zu\n",
			      size, dst_off, src_off), -EINVAL;

	if (memcmp(dst, src, size))
		return pr_err("payload mismatch size=%zu dst_off=%zu src_off=%zu\n",
			      size, dst_off, src_off), -EINVAL;

	if (memcmp(dst + size, ref + size, right))
		return pr_err("right guard changed size=%zu dst_off=%zu src_off=%zu\n",
			      size, dst_off, src_off), -EINVAL;

	return 0;
}

static int run_correctness(void)
{
	static const size_t sizes[] = {
		0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33,
		63, 64, 65, 95, 96, 97, 120, 127, 128, 129, 130, 135,
		191, 192, 193, 255, 256, 257, 511, 512, 513, 1023,
		1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097,
		8191, 8192, 8193, 16384, 32768, 65536, 131071,
	};
	size_t i, dst_off, src_off;
	int ret;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (dst_off = 0; dst_off < TEST_ALIGN; dst_off++) {
			for (src_off = 0; src_off < TEST_ALIGN; src_off++) {
				ret = check_one(sizes[i], dst_off, src_off);
				if (ret)
					return ret;
			}
		}
		cond_resched();
	}

	return 0;
}

static void bench_case(size_t size, size_t dst_off, size_t src_off)
{
	u8 *src = src_buf + GUARD_SIZE + src_off;
	u8 *dst = dst_buf + GUARD_SIZE + dst_off;
	u64 iters = BENCH_BYTES / size;
	u64 start, end, nsec;
	u32 checksum = 0;
	u64 mib_s;
	size_t i;

	if (!iters)
		iters = 1;

	fill_source(src_buf, BUFFER_SIZE, 0x2468ace0U + src_off + dst_off);
	memset(dst_buf, 0, BUFFER_SIZE);

	start = ktime_get_ns();
	for (i = 0; i < iters; i++) {
		__memcpy(dst, src, size);
		checksum += dst[(i + dst_off + src_off) & (size - 1)];
	}
	end = ktime_get_ns();
	nsec = max_t(u64, end - start, 1);
	mib_s = div64_u64(size * iters * 1000000000ULL,
			  nsec * 1048576ULL);

	pr_info("bench size=%zu dst_off=%zu src_off=%zu iters=%llu ns=%llu MiB_s=%llu checksum=%u\n",
		size, dst_off, src_off, iters, nsec, mib_s, checksum);
}

static void run_benchmarks(void)
{
	static const size_t sizes[] = { 128, 192, 256, 512, 1024, 4096, 16384, 65536 };
	static const struct {
		size_t dst_off;
		size_t src_off;
	} offsets[] = {
		{ 0, 0 },
		{ 1, 1 },
		{ 1, 2 },
		{ 3, 5 },
		{ 7, 1 },
	};
	size_t i, j;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (j = 0; j < ARRAY_SIZE(offsets); j++) {
			bench_case(sizes[i], offsets[j].dst_off, offsets[j].src_off);
			cond_resched();
		}
	}
}

static u32 next_prng(u32 *state)
{
	*state = *state * 1103515245U + 12345U;
	return *state;
}

static size_t mixed_size(u32 rnd)
{
	u32 bucket = rnd % 100;

	if (bucket < 50)
		return 16 + ((rnd >> 8) & 0x1f8);
	if (bucket < 80)
		return 512 + ((rnd >> 8) & 0xf00);
	if (bucket < 95)
		return 4096 + ((rnd >> 8) & 0x7000);

	return 32768 + ((rnd >> 8) & 0x10000);
}

static void run_mixed_benchmark(void)
{
	u64 bytes = 0, calls = 0, mismatch_bytes = 0, same_bytes = 0;
	u64 mismatch_calls = 0, same_calls = 0;
	u64 start, end, nsec, mib_s;
	u32 state = 0x31415926U;
	u32 checksum = 0;

	fill_source(src_buf, BUFFER_SIZE, 0x10203040U);
	memset(dst_buf, 0, BUFFER_SIZE);

	start = ktime_get_ns();
	while (bytes < MIXED_BENCH_BYTES) {
		u32 rnd = next_prng(&state);
		size_t size = mixed_size(rnd);
		size_t dst_off = (rnd >> 16) & 7;
		size_t src_off;
		u8 *dst;
		u8 *src;

		if ((rnd & 7) < 5)
			src_off = dst_off;
		else
			src_off = (dst_off + 1 + ((rnd >> 24) & 6)) & 7;

		if (size > MAX_COPY_SIZE)
			size = MAX_COPY_SIZE;

		dst = dst_buf + GUARD_SIZE + dst_off;
		src = src_buf + GUARD_SIZE + src_off;
		__memcpy(dst, src, size);
		checksum += dst[(checksum + calls) % size];

		if ((dst_off & (TEST_ALIGN - 1)) == (src_off & (TEST_ALIGN - 1))) {
			same_calls++;
			same_bytes += size;
		} else {
			mismatch_calls++;
			mismatch_bytes += size;
		}

		bytes += size;
		calls++;
		if (!(calls & 0x3ff))
			cond_resched();
	}
	end = ktime_get_ns();

	nsec = max_t(u64, end - start, 1);
	mib_s = div64_u64(bytes * 1000000000ULL, nsec * 1048576ULL);

	pr_info("mixed_bench calls=%llu bytes=%llu same_calls=%llu mismatch_calls=%llu same_bytes=%llu mismatch_bytes=%llu ns=%llu MiB_s=%llu checksum=%u\n",
		calls, bytes, same_calls, mismatch_calls, same_bytes,
		mismatch_bytes, nsec, mib_s, checksum);
}

static int __init test_riscv_memcpy_fast_unaligned_init(void)
{
	int ret;

	src_buf = kmalloc(BUFFER_SIZE, GFP_KERNEL);
	dst_buf = kmalloc(BUFFER_SIZE, GFP_KERNEL);
	ref_buf = kmalloc(BUFFER_SIZE, GFP_KERNEL);
	if (!src_buf || !dst_buf || !ref_buf) {
		ret = -ENOMEM;
		goto out;
	}

	ret = run_correctness();
	if (ret)
		goto out;

	pr_info("correctness passed for all src/dst low-bit combinations\n");

	if (run_bench) {
		run_benchmarks();
		run_mixed_benchmark();
	}

out:
	kfree(src_buf);
	kfree(dst_buf);
	kfree(ref_buf);

	return ret;
}

static void __exit test_riscv_memcpy_fast_unaligned_exit(void)
{
}

module_init(test_riscv_memcpy_fast_unaligned_init);
module_exit(test_riscv_memcpy_fast_unaligned_exit);

MODULE_DESCRIPTION("RISC-V memcpy fast unaligned path test");
MODULE_LICENSE("GPL");
