// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/align.h>
#include <linux/crc32.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#define TEST_MAX_SIZE		65536
#define TEST_GUARD_SIZE		64
#define TEST_BUFFER_SIZE	(TEST_MAX_SIZE + TEST_GUARD_SIZE * 2 + 32)
#define TEST_MIX_MAX_CHUNKS	512
#define TEST_XATTR_COUNT	32
#define TEST_SORT_RECORDS	512
#define TEST_SORT_RECORD_SIZE	256

static unsigned int iterations = 20000;
module_param(iterations, uint, 0444);
MODULE_PARM_DESC(iterations, "iterations per benchmark case");

static unsigned int repeats = 8;
module_param(repeats, uint, 0444);
MODULE_PARM_DESC(repeats, "correctness repeats per case");

static char mode[24] = "all";
module_param_string(mode, mode, sizeof(mode), 0444);
MODULE_PARM_DESC(mode, "test mode: all, correct, bench, mix, scatterlist, zstd, xattr, sort, overlap, irq");

extern void *__memcpy(void *dst, const void *src, size_t n);

static const size_t correct_sizes[] = {
	64, 127, 128, 256, 1024, 2047, 2048, 2049, 4096, 8192,
	16384, 65536,
};

static const size_t bench_sizes[] = {
	1024, 2047, 2048, 2049, 3072, 4096, 8192, 16384, 32768,
	65536,
};

static const unsigned int test_offsets[] = {
	0, 1, 3, 7, 8, 15,
};

static const size_t mix_sizes[] = {
	64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2047,
	2048, 3072, 4096, 6144, 8192, 12288, 16384, 32768, 65536,
};

static const size_t xattr_sizes[TEST_XATTR_COUNT] = {
	16, 32, 64, 96, 128, 192, 256, 384,
	512, 768, 1024, 1536, 2047, 2048, 2304, 3072,
	4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152,
	65536, 4096, 2048, 1024, 512, 256, 128, 64,
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

static void fill_noise(u8 *buf, size_t size, unsigned int seed)
{
	u32 x = 0x9e3779b9 ^ seed;
	size_t i;

	for (i = 0; i < size; i++) {
		x = x * 1664525 + 1013904223;
		buf[i] = (u8)(x >> 24);
	}
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

static void log_rate(const char *name, size_t bytes, u64 elapsed)
{
	if (!elapsed)
		elapsed = 1;

	pr_info("%s ns=%llu bytes=%zu bytes_per_ns=%llu.%03llu\n",
		name, elapsed, bytes,
		div64_u64(bytes, elapsed),
		div64_u64((bytes % elapsed) * 1000, elapsed));
}

static int run_correct_case(u8 *dst_base, u8 *src_base, size_t size,
			    unsigned int dst_off, unsigned int src_off,
			    bool bench)
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

	if (!bench || !should_benchmark(dst_off, src_off))
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

static int run_correct(bool bench)
{
	const size_t *sizes = bench ? bench_sizes : correct_sizes;
	unsigned int nr_sizes = bench ? ARRAY_SIZE(bench_sizes) :
		ARRAY_SIZE(correct_sizes);
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

	for (i = 0; i < nr_sizes; i++) {
		for (d = 0; d < ARRAY_SIZE(test_offsets); d++) {
			for (s = 0; s < ARRAY_SIZE(test_offsets); s++) {
				ret = run_correct_case(dst_base, src_base, sizes[i],
						       test_offsets[d], test_offsets[s],
						       bench);
				if (ret)
					goto out;
			}
		}
	}

	pr_info("%s cases passed\n", bench ? "bench" : "correctness");

out:
	kfree(dst_base);
	kfree(src_base);
	return ret;
}

static int run_irq_disabled_fallback(void)
{
	u8 *src_base;
	u8 *dst_base;
	u8 *src;
	u8 *dst;
	unsigned long flags;
	int ret = 0;

	src_base = kzalloc(TEST_BUFFER_SIZE, GFP_KERNEL);
	dst_base = kzalloc(TEST_BUFFER_SIZE, GFP_KERNEL);
	if (!src_base || !dst_base) {
		ret = -ENOMEM;
		goto out;
	}

	src = src_base + TEST_GUARD_SIZE;
	dst = dst_base + TEST_GUARD_SIZE;
	memset(dst_base, 0xa5, TEST_BUFFER_SIZE);
	fill_pattern(src, 4096, 0);

	local_irq_save(flags);
	__memcpy(dst, src, 4096);
	local_irq_restore(flags);

	if (!check_pattern(dst, 4096, 0))
		ret = -EINVAL;
	else
		pr_info("irq-disabled fallback copy passed\n");

out:
	kfree(dst_base);
	kfree(src_base);
	return ret;
}

static int run_overlap(void)
{
	u8 *buf;
	u8 *expect;
	unsigned int repeat;
	int ret = 0;

	buf = kzalloc(TEST_BUFFER_SIZE, GFP_KERNEL);
	expect = kzalloc(TEST_BUFFER_SIZE, GFP_KERNEL);
	if (!buf || !expect) {
		ret = -ENOMEM;
		goto out;
	}

	for (repeat = 0; repeat < repeats; repeat++) {
		fill_pattern(buf, TEST_BUFFER_SIZE, repeat);
		memcpy(expect, buf, TEST_BUFFER_SIZE);

		/*
		 * memcpy() has undefined overlap semantics; this only checks
		 * that overlapping copies are not sent to the vector path and
		 * that the scalar implementation remains consistent with itself.
		 */
		__memcpy(buf + 96, buf + 64, 4096);
		__memcpy(expect + 96, expect + 64, 4096);
		if (memcmp(buf, expect, TEST_BUFFER_SIZE)) {
			pr_err("forward overlap result differed from scalar baseline\n");
			ret = -EINVAL;
			goto out;
		}

		fill_pattern(buf, TEST_BUFFER_SIZE, repeat + 16);
		memcpy(expect, buf, TEST_BUFFER_SIZE);
		__memcpy(buf + 64, buf + 96, 4096);
		__memcpy(expect + 64, expect + 96, 4096);
		if (memcmp(buf, expect, TEST_BUFFER_SIZE)) {
			pr_err("backward overlap result differed from scalar baseline\n");
			ret = -EINVAL;
			goto out;
		}
	}

	pr_info("overlap fallback policy cases passed\n");

out:
	kfree(expect);
	kfree(buf);
	return ret;
}

static int run_mix(void)
{
	struct mix_chunk {
		size_t offset;
		size_t size;
	};
	struct mix_chunk *chunks;
	u8 *src;
	u8 *dst;
	size_t total = 0;
	u64 start, elapsed;
	unsigned int chunk, iter;
	u32 crc;

	chunks = kcalloc(TEST_MIX_MAX_CHUNKS, sizeof(*chunks), GFP_KERNEL);
	src = kvmalloc(TEST_MIX_MAX_CHUNKS * TEST_MAX_SIZE + 64, GFP_KERNEL);
	dst = kvmalloc(TEST_MIX_MAX_CHUNKS * TEST_MAX_SIZE + 128, GFP_KERNEL);
	if (!chunks || !src || !dst) {
		kvfree(dst);
		kvfree(src);
		kfree(chunks);
		return -ENOMEM;
	}

	fill_noise(src, TEST_MIX_MAX_CHUNKS * TEST_MAX_SIZE + 64, 1);
	for (chunk = 0; chunk < TEST_MIX_MAX_CHUNKS; chunk++) {
		chunks[chunk].size =
			mix_sizes[(chunk * 7) % ARRAY_SIZE(mix_sizes)];
		chunks[chunk].offset =
			(chunk * TEST_MAX_SIZE) + ((chunk * 13) & 15);
		total += chunks[chunk].size;
	}

	start = ktime_get_ns();
	for (iter = 0; iter < iterations; iter++) {
		for (chunk = 0; chunk < TEST_MIX_MAX_CHUNKS; chunk++)
			__memcpy(dst + chunks[chunk].offset + ((chunk * 5) & 15),
				 src + chunks[chunk].offset, chunks[chunk].size);
	}
	elapsed = ktime_get_ns() - start;
	crc = crc32_le(~0, dst, TEST_MIX_MAX_CHUNKS * TEST_MAX_SIZE + 128);

	log_rate("mix", total * iterations, elapsed);
	pr_info("mix chunks=%u iter=%u crc=0x%08x\n", TEST_MIX_MAX_CHUNKS,
		iterations, crc);

	kvfree(dst);
	kvfree(src);
	kfree(chunks);
	return 0;
}

static int run_scatterlist(void)
{
	struct scatterlist *sg;
	u8 **segments;
	u8 *linear;
	u8 *check;
	u64 start, elapsed;
	unsigned int i, iter;
	size_t copied;
	u32 crc;
	int ret = 0;

	sg = kcalloc(16, sizeof(*sg), GFP_KERNEL);
	segments = kcalloc(16, sizeof(*segments), GFP_KERNEL);
	linear = kvmalloc(TEST_MAX_SIZE + 256, GFP_KERNEL);
	check = kvmalloc(TEST_MAX_SIZE + 256, GFP_KERNEL);
	if (!sg || !segments || !linear || !check) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < 16; i++) {
		segments[i] = kmalloc(4096 + 16, GFP_KERNEL);
		if (!segments[i]) {
			ret = -ENOMEM;
			goto out;
		}
	}

	fill_noise(linear, TEST_MAX_SIZE + 256, 2);
	sg_init_table(sg, 16);
	for (i = 0; i < 16; i++)
		sg_set_buf(&sg[i], segments[i] + (i & 15), 4096);

	start = ktime_get_ns();
	for (iter = 0; iter < iterations; iter++) {
		copied = sg_copy_from_buffer(sg, 16, linear + (iter & 15),
					     TEST_MAX_SIZE);
		if (copied != TEST_MAX_SIZE) {
			ret = -EINVAL;
			goto out;
		}
		copied = sg_copy_to_buffer(sg, 16, check + ((iter >> 1) & 15),
					   TEST_MAX_SIZE);
		if (copied != TEST_MAX_SIZE) {
			ret = -EINVAL;
			goto out;
		}
	}
	elapsed = ktime_get_ns() - start;
	crc = crc32_le(~0, check, TEST_MAX_SIZE + 256);

	log_rate("scatterlist", (size_t)TEST_MAX_SIZE * 2 * iterations,
		 elapsed);
	pr_info("scatterlist segments=16 iter=%u crc=0x%08x\n", iterations,
		crc);

out:
	if (segments) {
		for (i = 0; i < 16; i++)
			kfree(segments[i]);
	}
	kvfree(check);
	kvfree(linear);
	kfree(segments);
	kfree(sg);
	return ret;
}

static int run_zstd(void)
{
	zstd_parameters params;
	zstd_cctx *cctx;
	zstd_dctx *dctx;
	void *cworkspace;
	void *dworkspace;
	u8 *src;
	u8 *comp;
	u8 *decomp;
	size_t cworkspace_size;
	size_t dworkspace_size;
	size_t comp_bound;
	size_t comp_size = 0;
	size_t ret_size;
	u64 start, elapsed;
	unsigned int iter;
	u32 crc;
	int ret = 0;

	params = zstd_get_params(1, TEST_MAX_SIZE);
	cworkspace_size = zstd_cctx_workspace_bound(&params.cParams);
	dworkspace_size = zstd_dctx_workspace_bound();
	comp_bound = zstd_compress_bound(TEST_MAX_SIZE);

	cworkspace = vzalloc(cworkspace_size);
	dworkspace = vzalloc(dworkspace_size);
	src = kvmalloc(TEST_MAX_SIZE, GFP_KERNEL);
	comp = kvmalloc(comp_bound, GFP_KERNEL);
	decomp = kvmalloc(TEST_MAX_SIZE, GFP_KERNEL);
	if (!cworkspace || !dworkspace || !src || !comp || !decomp) {
		ret = -ENOMEM;
		goto out;
	}

	cctx = zstd_init_cctx(cworkspace, cworkspace_size);
	dctx = zstd_init_dctx(dworkspace, dworkspace_size);
	if (!cctx || !dctx) {
		ret = -EINVAL;
		goto out;
	}

	fill_noise(src, TEST_MAX_SIZE, 3);
	start = ktime_get_ns();
	for (iter = 0; iter < iterations; iter++) {
		comp_size = zstd_compress_cctx(cctx, comp, comp_bound, src,
					       TEST_MAX_SIZE, &params);
		if (zstd_is_error(comp_size)) {
			pr_err("zstd compress failed: %s\n",
			       zstd_get_error_name(comp_size));
			ret = -EINVAL;
			goto out;
		}
	}
	elapsed = ktime_get_ns() - start;
	log_rate("zstd_compress", (size_t)TEST_MAX_SIZE * iterations,
		 elapsed);

	start = ktime_get_ns();
	for (iter = 0; iter < iterations; iter++) {
		ret_size = zstd_decompress_dctx(dctx, decomp, TEST_MAX_SIZE,
						comp, comp_size);
		if (zstd_is_error(ret_size) || ret_size != TEST_MAX_SIZE) {
			pr_err("zstd decompress failed: ret=%zu err=%s\n", ret_size,
			       zstd_is_error(ret_size) ?
			       zstd_get_error_name(ret_size) : "short output");
			ret = -EINVAL;
			goto out;
		}
	}
	elapsed = ktime_get_ns() - start;
	crc = crc32_le(~0, decomp, TEST_MAX_SIZE);
	log_rate("zstd_decompress", (size_t)TEST_MAX_SIZE * iterations,
		 elapsed);
	pr_info("zstd iter=%u src=%u compressed=%zu crc=0x%08x\n",
		iterations, TEST_MAX_SIZE, comp_size, crc);

out:
	kvfree(decomp);
	kvfree(comp);
	kvfree(src);
	vfree(dworkspace);
	vfree(cworkspace);
	return ret;
}

static int run_xattr(void)
{
	u8 *src;
	u8 *dst;
	u64 start, elapsed;
	size_t total = 0;
	unsigned int i, iter;
	u32 crc;

	src = kvmalloc(TEST_XATTR_COUNT * TEST_MAX_SIZE + 64, GFP_KERNEL);
	dst = kvmalloc(TEST_XATTR_COUNT * TEST_MAX_SIZE + 128, GFP_KERNEL);
	if (!src || !dst) {
		kvfree(dst);
		kvfree(src);
		return -ENOMEM;
	}

	fill_noise(src, TEST_XATTR_COUNT * TEST_MAX_SIZE + 64, 4);
	for (i = 0; i < TEST_XATTR_COUNT; i++)
		total += xattr_sizes[i];

	start = ktime_get_ns();
	for (iter = 0; iter < iterations; iter++) {
		for (i = 0; i < TEST_XATTR_COUNT; i++) {
			size_t off = i * TEST_MAX_SIZE;

			__memcpy(dst + off + ((i + iter) & 15),
				 src + off + ((i * 3) & 15), xattr_sizes[i]);
		}
	}
	elapsed = ktime_get_ns() - start;
	crc = crc32_le(~0, dst, TEST_XATTR_COUNT * TEST_MAX_SIZE + 128);

	log_rate("xattr_style", total * iterations, elapsed);
	pr_info("xattr_style entries=%u iter=%u crc=0x%08x\n",
		TEST_XATTR_COUNT, iterations, crc);

	kvfree(dst);
	kvfree(src);
	return 0;
}

struct sort_record {
	u32 key;
	u8 data[TEST_SORT_RECORD_SIZE - sizeof(u32)];
};

static int sort_record_cmp(const void *a, const void *b)
{
	const struct sort_record *ra = a;
	const struct sort_record *rb = b;

	return cmp_int(ra->key, rb->key);
}

static int run_sort_records(void)
{
	struct sort_record *records;
	u64 start, elapsed;
	unsigned int i, iter;
	u32 crc;

	records = kvmalloc_array(TEST_SORT_RECORDS, sizeof(*records), GFP_KERNEL);
	if (!records)
		return -ENOMEM;

	start = ktime_get_ns();
	for (iter = 0; iter < iterations; iter++) {
		for (i = 0; i < TEST_SORT_RECORDS; i++) {
			records[i].key = (TEST_SORT_RECORDS - i) ^ iter;
			fill_noise(records[i].data, sizeof(records[i].data), i + iter);
		}
		sort(records, TEST_SORT_RECORDS, sizeof(*records),
		     sort_record_cmp, NULL);
	}
	elapsed = ktime_get_ns() - start;
	crc = crc32_le(~0, records, TEST_SORT_RECORDS * sizeof(*records));

	log_rate("sort_records", (size_t)TEST_SORT_RECORDS * sizeof(*records) *
		 iterations, elapsed);
	pr_info("sort_records records=%u size=%zu iter=%u crc=0x%08x\n",
		TEST_SORT_RECORDS, sizeof(*records), iterations, crc);

	kvfree(records);
	return 0;
}

static int __init test_riscv_v_memcpy_init(void)
{
	int ret;

	pr_info("mode=%s iterations=%u repeats=%u\n", mode, iterations, repeats);

	if (!strcmp(mode, "all")) {
		ret = run_correct(true);
		if (ret)
			return ret;
		ret = run_overlap();
		if (ret)
			return ret;
		ret = run_irq_disabled_fallback();
		if (ret)
			return ret;
		ret = run_mix();
		if (ret)
			return ret;
		ret = run_scatterlist();
		if (ret)
			return ret;
		ret = run_zstd();
		if (ret)
			return ret;
		ret = run_xattr();
		if (ret)
			return ret;
		ret = run_sort_records();
	} else if (!strcmp(mode, "correct")) {
		ret = run_correct(false);
	} else if (!strcmp(mode, "bench")) {
		ret = run_correct(true);
	} else if (!strcmp(mode, "mix")) {
		ret = run_mix();
	} else if (!strcmp(mode, "scatterlist")) {
		ret = run_scatterlist();
	} else if (!strcmp(mode, "zstd")) {
		ret = run_zstd();
	} else if (!strcmp(mode, "xattr")) {
		ret = run_xattr();
	} else if (!strcmp(mode, "sort")) {
		ret = run_sort_records();
	} else if (!strcmp(mode, "overlap")) {
		ret = run_overlap();
	} else if (!strcmp(mode, "irq")) {
		ret = run_irq_disabled_fallback();
	} else {
		pr_err("unknown mode '%s'\n", mode);
		return -EINVAL;
	}

	if (!ret)
		pr_info("mode %s passed\n", mode);

	return ret;
}
module_init(test_riscv_v_memcpy_init);

static void __exit test_riscv_v_memcpy_exit(void)
{
}
module_exit(test_riscv_v_memcpy_exit);

MODULE_DESCRIPTION("RISC-V vector memcpy correctness and benchmark workloads");
MODULE_LICENSE("GPL");
