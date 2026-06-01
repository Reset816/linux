// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../hwprobe/hwprobe.h"
#include "../../kselftest.h"

static bool is_power_of_2(uint64_t n)
{
	return n && !(n & (n - 1));
}

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int get_zicboz_block_size(uint64_t *block_size)
{
	struct riscv_hwprobe pairs[] = {
		{ .key = RISCV_HWPROBE_KEY_IMA_EXT_0, },
		{ .key = RISCV_HWPROBE_KEY_ZICBOZ_BLOCK_SIZE, },
	};
	long rc;

	rc = riscv_hwprobe(pairs, ARRAY_SIZE(pairs), 0, NULL, 0);
	if (rc)
		return rc;

	if (pairs[0].key != RISCV_HWPROBE_KEY_IMA_EXT_0 ||
	    !(pairs[0].value & RISCV_HWPROBE_EXT_ZICBOZ))
		return -ENODEV;

	if (pairs[1].key != RISCV_HWPROBE_KEY_ZICBOZ_BLOCK_SIZE ||
	    !is_power_of_2(pairs[1].value))
		return -EINVAL;

	*block_size = pairs[1].value;
	return 0;
}

static void fill_pattern(unsigned char *buf, size_t size)
{
	for (size_t i = 0; i < size; i++)
		buf[i] = (unsigned char)(0x40 + ((i * 37) & 0x3f));
}

static int check_zero_window(const unsigned char *buf, size_t size,
			     size_t offset, size_t len)
{
	for (size_t i = 0; i < size; i++) {
		unsigned char expected;

		if (i >= offset && i < offset + len)
			expected = 0;
		else
			expected = (unsigned char)(0x40 + ((i * 37) & 0x3f));

		if (buf[i] != expected) {
			ksft_print_msg("mismatch at byte %zu: got 0x%02x expected 0x%02x"
				       " offset=%zu len=%zu\n",
				       i, buf[i], expected, offset, len);
			return -1;
		}
	}

	return 0;
}

static void test_user_memset_boundaries(uint64_t block_size)
{
	static const size_t sizes[] = {
		1, 7, 15, 16, 31, 32, 63, 64, 65, 127, 128, 129,
		255, 256, 257, 511, 512, 513, 1023, 1024,
	};
	size_t buf_size = 8192 + block_size * 4;
	unsigned char *buf;
	int failed = 0;

	if (posix_memalign((void **)&buf, 4096, buf_size)) {
		ksft_test_result_fail("userspace memset boundary checks\n");
		return;
	}

	for (size_t offset = 0; offset < block_size * 2 && !failed; offset++) {
		for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
			size_t len = sizes[i];

			if (offset + len > buf_size)
				break;

			fill_pattern(buf, buf_size);
			memset(buf + offset, 0, len);

			if (check_zero_window(buf, buf_size, offset, len)) {
				failed = 1;
				break;
			}
		}
	}

	free(buf);
	ksft_test_result(!failed, "userspace memset boundary checks\n");
}

static void bench_user_memset(void)
{
	static const size_t sizes[] = { 64, 128, 256, 512, 1024, 4096, 16384 };
	unsigned char *buf;
	volatile unsigned long checksum = 0;

	if (posix_memalign((void **)&buf, 4096, 32768)) {
		ksft_print_msg("userspace bench allocation failed\n");
		return;
	}

	for (size_t s = 0; s < ARRAY_SIZE(sizes); s++) {
		size_t len = sizes[s];
		unsigned long iters = 10000000UL / len + 20000;
		uint64_t start, elapsed;

		memset(buf, 0xa5, 32768);
		start = nsec_now();
		for (unsigned long i = 0; i < iters; i++) {
			memset(buf + (i & 63), 0, len);
			checksum += buf[i & 63];
		}
		elapsed = nsec_now() - start;
		ksft_print_msg("userspace memset size=%zu: %lu calls in %" PRIu64
			       " ns, %.2f ns/call\n",
			       len, iters, elapsed, (double)elapsed / iters);
	}

	if (checksum == 1)
		ksft_print_msg("checksum guard: %lu\n", checksum);
	free(buf);
}

int main(void)
{
	uint64_t block_size = 0;
	int rc;

	ksft_print_header();
	ksft_set_plan(2);

	rc = get_zicboz_block_size(&block_size);
	if (rc == -ENODEV) {
		ksft_print_msg("Zicboz not reported by hwprobe\n");
		block_size = 64;
		ksft_test_result_skip("Zicboz hwprobe block size\n");
	} else {
		ksft_test_result(rc == 0, "Zicboz hwprobe block size\n");
		if (rc)
			block_size = 64;
	}

	ksft_print_msg("Zicboz block size used for boundary sweep: %" PRIu64 "\n",
		       block_size);

	test_user_memset_boundaries(block_size);
	bench_user_memset();

	ksft_finished();
}
