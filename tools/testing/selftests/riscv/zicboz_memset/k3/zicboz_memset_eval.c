// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <asm/page.h>

static char mode[16] = "all";
static unsigned int passes = 1;
static unsigned int mixed_calls = 120000;

module_param_string(mode, mode, sizeof(mode), 0444);
module_param(passes, uint, 0444);
module_param(mixed_calls, uint, 0444);

static volatile u8 sink;

static void fill_pattern(u8 *buf, size_t size)
{
	for (size_t i = 0; i < size; i++)
		buf[i] = (u8)(0x51 + ((i * 17) & 0x7f));
}

static int verify_window(const u8 *buf, size_t size, size_t offset,
			 size_t len, u8 value)
{
	for (size_t i = 0; i < size; i++) {
		u8 expected = (i >= offset && i < offset + len) ?
			      value : (u8)(0x51 + ((i * 17) & 0x7f));

		if (buf[i] != expected) {
			pr_err("mismatch i=%zu got=0x%02x expected=0x%02x offset=%zu len=%zu value=0x%02x\n",
			       i, buf[i], expected, offset, len, value);
			return -EINVAL;
		}
	}

	return 0;
}

static int run_correctness(void)
{
	static const size_t sizes[] = {
		1, 7, 15, 16, 31, 32, 63, 64, 65, 127, 128, 129,
		255, 256, 257, 511, 512, 513, 1023, 1024, 2048,
		4095, 4096, 4097, 8192,
	};
	size_t block = READ_ONCE(riscv_cboz_block_size) ?: 64;
	size_t buf_size = 4 * PAGE_SIZE + 8192;
	u8 *buf, *vbuf;
	int ret = 0;

	buf = kmalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (size_t offset = 0; offset < min_t(size_t, block * 2, 256); offset++) {
		for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
			if (offset + sizes[i] > buf_size)
				break;

			fill_pattern(buf, buf_size);
			memset(buf + offset, 0, sizes[i]);
			ret = verify_window(buf, buf_size, offset, sizes[i], 0);
			if (ret)
				goto out_free;
		}
		cond_resched();
	}

	fill_pattern(buf, buf_size);
	memset(buf + PAGE_SIZE - 17, 0, PAGE_SIZE + 61);
	ret = verify_window(buf, buf_size, PAGE_SIZE - 17, PAGE_SIZE + 61, 0);
	if (ret)
		goto out_free;

	fill_pattern(buf, buf_size);
	memset(buf + 13, 0x5a, 4096);
	ret = verify_window(buf, buf_size, 13, 4096, 0x5a);
	if (ret)
		goto out_free;

	vbuf = vmalloc(buf_size);
	if (!vbuf) {
		ret = -ENOMEM;
		goto out_free;
	}

	fill_pattern(vbuf, buf_size);
	memset(vbuf + PAGE_SIZE - 23, 0, PAGE_SIZE + 71);
	ret = verify_window(vbuf, buf_size, PAGE_SIZE - 23, PAGE_SIZE + 71, 0);
	vfree(vbuf);
	if (ret)
		goto out_free;

	pr_info("correctness passed block_size=%zu page_boundary=yes vmalloc_fallback=yes ioremap_fallback=skipped_mmio_safety\n",
		block);

out_free:
	kfree(buf);
	return ret;
}

static unsigned long iters_for_len(size_t len)
{
	unsigned long iters = (64UL * 1024 * 1024) / len;

	return clamp_t(unsigned long, iters, 2000, 200000);
}

static void bench_one_size(u8 *buf, size_t len, bool zero, bool aligned)
{
	unsigned long iters = iters_for_len(len);
	size_t offset = aligned ? 0 : 13;
	ktime_t start;
	s64 elapsed;
	u8 value = zero ? 0 : 0x5a;

	memset(buf, 0xa5, len + 128);
	start = ktime_get();
	for (unsigned long i = 0; i < iters; i++) {
		memset(buf + offset, value, len);
		sink ^= buf[offset + (i & 63)];
	}
	elapsed = ktime_to_ns(ktime_sub(ktime_get(), start));

	pr_info("direct kind=%s align=%s size=%zu calls=%lu ns=%lld ns_per_call=%lld mib_per_s=%llu\n",
		zero ? "zero" : "nonzero", aligned ? "aligned" : "unaligned",
		len, iters, elapsed, div_s64(elapsed, iters),
		div64_u64((u64)len * iters * 1000000000ULL,
			  (u64)elapsed * 1024 * 1024));
}

static int run_direct_bench(void)
{
	static const size_t sizes[] = {
		16, 32, 64, 128, 255, 256, 512, 1024, 2048, 4096,
		8192, 16384, 32768, 65536,
	};
	u8 *buf;

	buf = kmalloc(max_t(size_t, 65536 + 256, 2 * PAGE_SIZE), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	pr_info("direct bench start passes=%u cboz_block_size=%u\n",
		passes, READ_ONCE(riscv_cboz_block_size));

	for (unsigned int pass = 0; pass < passes; pass++) {
		for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
			bench_one_size(buf, sizes[i], true, true);
			bench_one_size(buf, sizes[i], true, false);
			bench_one_size(buf, sizes[i], false, true);
		}
	}

	kfree(buf);
	return 0;
}

static u32 lcg_next(u32 *state)
{
	*state = *state * 1664525U + 1013904223U;
	return *state;
}

static int run_mixed_workload(void)
{
	static const size_t sizes[] = {
		32, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
		1536, 2048, 4096, 8192, 16384, 32768,
	};
	size_t buf_size = 512 * 1024;
	unsigned long zero_calls = 0, nonzero_calls = 0;
	u64 zero_bytes = 0, nonzero_bytes = 0;
	ktime_t start;
	s64 elapsed;
	u8 *buf;
	u32 rnd = 0x12345678;

	buf = kmalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memset(buf, 0xa5, buf_size);
	start = ktime_get();
	for (unsigned int i = 0; i < mixed_calls; i++) {
		size_t len = sizes[lcg_next(&rnd) % ARRAY_SIZE(sizes)];
		size_t offset = lcg_next(&rnd) % (buf_size - len - 64);
		bool zero = (lcg_next(&rnd) % 100) < 82;

		offset += lcg_next(&rnd) & 63;
		if (zero) {
			memset(buf + offset, 0, len);
			zero_calls++;
			zero_bytes += len;
		} else {
			memset(buf + offset, 0x5a, len);
			nonzero_calls++;
			nonzero_bytes += len;
		}
		sink ^= buf[offset + (i & 31)];
		cond_resched();
	}
	elapsed = ktime_to_ns(ktime_sub(ktime_get(), start));

	pr_info("mixed calls=%u zero_calls=%lu zero_bytes=%llu nonzero_calls=%lu nonzero_bytes=%llu ns=%lld ns_per_call=%lld mib_per_s=%llu\n",
		mixed_calls, zero_calls, zero_bytes, nonzero_calls, nonzero_bytes,
		elapsed, div_s64(elapsed, mixed_calls),
		div64_u64((zero_bytes + nonzero_bytes) * 1000000000ULL,
			  (u64)elapsed * 1024 * 1024));

	kfree(buf);
	return 0;
}

static int __init zicboz_memset_eval_init(void)
{
	int ret = 0;

	if (!strcmp(mode, "all") || !strcmp(mode, "correctness")) {
		ret = run_correctness();
		if (ret)
			return ret;
	}

	if (!strcmp(mode, "all") || !strcmp(mode, "direct")) {
		ret = run_direct_bench();
		if (ret)
			return ret;
	}

	if (!strcmp(mode, "all") || !strcmp(mode, "mixed")) {
		ret = run_mixed_workload();
		if (ret)
			return ret;
	}

	return 0;
}

static void __exit zicboz_memset_eval_exit(void)
{
}

module_init(zicboz_memset_eval_init);
module_exit(zicboz_memset_eval_exit);

MODULE_DESCRIPTION("K3 Zicboz memset evaluation workload helper");
MODULE_LICENSE("GPL");
