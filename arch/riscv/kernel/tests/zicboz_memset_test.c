// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cacheflush.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

static void fill_pattern(u8 *buf, size_t size)
{
	for (size_t i = 0; i < size; i++)
		buf[i] = (u8)(0x40 + ((i * 37) & 0x3f));
}

static int verify_zero_range(const u8 *buf, size_t size, size_t offset, size_t len)
{
	for (size_t i = 0; i < size; i++) {
		u8 expected = (i >= offset && i < offset + len) ?
			      0 : (u8)(0x40 + ((i * 37) & 0x3f));

		if (buf[i] != expected) {
			pr_err("mismatch at byte %zu: got 0x%02x expected 0x%02x offset=%zu len=%zu\n",
			       i, buf[i], expected, offset, len);
			return -EINVAL;
		}
	}

	return 0;
}

static int verify_nonzero_range(const u8 *buf, size_t size, size_t offset,
				size_t len, u8 value)
{
	for (size_t i = 0; i < size; i++) {
		u8 expected = (i >= offset && i < offset + len) ?
			      value : (u8)(0x40 + ((i * 37) & 0x3f));

		if (buf[i] != expected) {
			pr_err("nonzero mismatch at byte %zu: got 0x%02x expected 0x%02x offset=%zu len=%zu\n",
			       i, buf[i], expected, offset, len);
			return -EINVAL;
		}
	}

	return 0;
}

static int run_zero_boundaries(u8 *buf, size_t buf_size, size_t block_size,
			       const char *name)
{
	static const size_t sizes[] = {
		1, 7, 15, 16, 31, 32, 63, 64, 65, 127, 128, 129,
		255, 256, 257, 511, 512, 513, 1023, 1024, 4096,
	};
	size_t offset_limit = min_t(size_t, block_size * 2, 256);
	int ret;

	for (size_t offset = 0; offset < offset_limit; offset++) {
		for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
			size_t len = sizes[i];

			if (offset + len > buf_size)
				break;

			fill_pattern(buf, buf_size);
			memset(buf + offset, 0, len);
			ret = verify_zero_range(buf, buf_size, offset, len);
			if (ret) {
				pr_err("%s zero boundary failed\n", name);
				return ret;
			}
		}
		cond_resched();
	}

	pr_info("%s zero boundary checks passed\n", name);
	return 0;
}

static int run_nonzero_fallback(u8 *buf, size_t buf_size)
{
	int ret;

	fill_pattern(buf, buf_size);
	memset(buf + 13, 0x5a, 1024);
	ret = verify_nonzero_range(buf, buf_size, 13, 1024, 0x5a);
	if (ret)
		return ret;

	pr_info("nonzero fallback check passed\n");
	return 0;
}

static void run_bench(u8 *buf, size_t buf_size)
{
	static const size_t sizes[] = { 64, 128, 256, 512, 1024, 4096, 16384 };
	volatile u8 sink = 0;

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		size_t len = sizes[i];
		unsigned long iters = 10000000UL / len + 20000;
		ktime_t start;
		s64 elapsed;

		memset(buf, 0xa5, buf_size);
		start = ktime_get();
		for (unsigned long j = 0; j < iters; j++) {
			memset(buf + (j & 63), 0, len);
			sink ^= buf[j & 63];
		}
		elapsed = ktime_to_ns(ktime_sub(ktime_get(), start));
		pr_info("zero memset size=%zu: %lu calls in %lld ns, %lld ns/call\n",
			len, iters, elapsed, div_s64(elapsed, iters));
	}

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		size_t len = sizes[i];
		unsigned long iters = 10000000UL / len + 20000;
		ktime_t start;
		s64 elapsed;

		memset(buf, 0xa5, buf_size);
		start = ktime_get();
		for (unsigned long j = 0; j < iters; j++) {
			memset(buf + (j & 63), 0x5a, len);
			sink ^= buf[j & 63];
		}
		elapsed = ktime_to_ns(ktime_sub(ktime_get(), start));
		pr_info("nonzero memset size=%zu: %lu calls in %lld ns, %lld ns/call\n",
			len, iters, elapsed, div_s64(elapsed, iters));
	}

	if (sink == 1)
		pr_info("benchmark sink: %u\n", sink);
}

static int __init zicboz_memset_test_init(void)
{
	size_t block_size = riscv_cboz_block_size ?: 64;
	size_t buf_size = 32768;
	u8 *linear, *vmalloc_buf;
	int ret;

	linear = kmalloc(buf_size, GFP_KERNEL);
	if (!linear)
		return -ENOMEM;

	ret = run_zero_boundaries(linear, buf_size, block_size, "linear");
	if (!ret)
		ret = run_nonzero_fallback(linear, buf_size);
	if (!ret)
		run_bench(linear, buf_size);
	kfree(linear);
	if (ret)
		return ret;

	vmalloc_buf = vmalloc(buf_size);
	if (!vmalloc_buf)
		return -ENOMEM;

	ret = run_zero_boundaries(vmalloc_buf, buf_size, block_size, "vmalloc");
	vfree(vmalloc_buf);
	if (ret)
		return ret;

	pr_info("all checks passed (cboz block size %zu)\n", block_size);
	return 0;
}

static void __exit zicboz_memset_test_exit(void)
{
}

module_init(zicboz_memset_test_init);
module_exit(zicboz_memset_test_exit);

MODULE_DESCRIPTION("RISC-V Zicboz memset zero path test");
MODULE_LICENSE("GPL");
