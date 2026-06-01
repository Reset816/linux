// SPDX-License-Identifier: GPL-2.0
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
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <asm/hwprobe.h>

#include "../../kselftest.h"

#ifndef RISCV_HWPROBE_KEY_MISALIGNED_SCALAR_PERF
#define RISCV_HWPROBE_KEY_MISALIGNED_SCALAR_PERF 9
#endif

#define ALIGNMENT 8
#define GUARD_SIZE 64
#define MAX_COPY_SIZE (128 * 1024)
#define BUFFER_SIZE (MAX_COPY_SIZE + 2 * GUARD_SIZE + 2 * ALIGNMENT)
#define BENCH_BYTES (32ULL * 1024 * 1024)

static unsigned char *src_buf;
static unsigned char *dst_buf;
static unsigned char *ref_buf;

static long riscv_hwprobe(struct riscv_hwprobe *pairs, size_t pair_count,
			  size_t cpusetsize, unsigned long *cpus,
			  unsigned int flags)
{
	return syscall(258, pairs, pair_count, cpusetsize, cpus, flags);
}

static void fill_source(unsigned char *buf, size_t size, unsigned int seed)
{
	for (size_t i = 0; i < size; i++) {
		seed = seed * 1664525U + 1013904223U;
		buf[i] = (unsigned char)(seed >> 24);
	}
}

static void fill_pattern(unsigned char *buf, size_t size, unsigned char base)
{
	for (size_t i = 0; i < size; i++)
		buf[i] = (unsigned char)(base + (i * 37U));
}

static int check_one(size_t size, size_t dst_off, size_t src_off)
{
	unsigned char *src = src_buf + GUARD_SIZE + src_off;
	unsigned char *dst = dst_buf + GUARD_SIZE + dst_off;
	unsigned char *ref = ref_buf + GUARD_SIZE + dst_off;
	size_t left = GUARD_SIZE + dst_off;
	size_t right = BUFFER_SIZE - left - size;

	fill_source(src_buf, BUFFER_SIZE, 0x13579bdfU + src_off + size);
	fill_pattern(dst_buf, BUFFER_SIZE, 0x5a);
	memcpy(ref_buf, dst_buf, BUFFER_SIZE);
	memcpy(ref, src, size);

	if (memcpy(dst, src, size) != dst) {
		ksft_print_msg("memcpy returned wrong pointer size=%zu dst_off=%zu src_off=%zu\n",
			       size, dst_off, src_off);
		return -1;
	}

	if (memcmp(dst_buf, ref_buf, left) != 0) {
		ksft_print_msg("left guard changed size=%zu dst_off=%zu src_off=%zu\n",
			       size, dst_off, src_off);
		return -1;
	}

	if (memcmp(dst, src, size) != 0) {
		ksft_print_msg("payload mismatch size=%zu dst_off=%zu src_off=%zu\n",
			       size, dst_off, src_off);
		return -1;
	}

	if (memcmp(dst + size, ref + size, right) != 0) {
		ksft_print_msg("right guard changed size=%zu dst_off=%zu src_off=%zu\n",
			       size, dst_off, src_off);
		return -1;
	}

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

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (size_t dst_off = 0; dst_off < ALIGNMENT; dst_off++) {
			for (size_t src_off = 0; src_off < ALIGNMENT; src_off++) {
				if (check_one(sizes[i], dst_off, src_off))
					return -1;
			}
		}
	}

	return 0;
}

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void bench_case(size_t size, size_t dst_off, size_t src_off)
{
	unsigned char *src = src_buf + GUARD_SIZE + src_off;
	unsigned char *dst = dst_buf + GUARD_SIZE + dst_off;
	uint64_t iters = BENCH_BYTES / size;
	uint64_t start, end;
	unsigned int checksum = 0;

	if (!iters)
		iters = 1;

	fill_source(src_buf, BUFFER_SIZE, 0x2468ace0U + src_off + dst_off);
	memset(dst_buf, 0, BUFFER_SIZE);

	start = nsec_now();
	for (uint64_t i = 0; i < iters; i++) {
		memcpy(dst, src, size);
		checksum += dst[(i + dst_off + src_off) & (size - 1)];
	}
	end = nsec_now();

	printf("bench size=%zu dst_off=%zu src_off=%zu iters=%" PRIu64
	       " ns=%" PRIu64 " MiB_s=%.2f checksum=%u\n",
	       size, dst_off, src_off, iters, end - start,
	       ((double)size * (double)iters * 1000000000.0) /
	       (double)(end - start) / 1048576.0, checksum);
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

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++)
		for (size_t j = 0; j < ARRAY_SIZE(offsets); j++)
			bench_case(sizes[i], offsets[j].dst_off, offsets[j].src_off);
}

static long scalar_misaligned_perf(void)
{
	struct riscv_hwprobe pair = {
		.key = RISCV_HWPROBE_KEY_MISALIGNED_SCALAR_PERF,
	};
	long ret = riscv_hwprobe(&pair, 1, 0, NULL, 0);

	if (ret)
		return ret;

	return pair.value;
}

static int pin_to_first_cpu(void)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(0, &set);
	if (sched_setaffinity(0, sizeof(set), &set) && errno != EINVAL)
		return -errno;

	return 0;
}

int main(int argc, char **argv)
{
	bool bench = argc > 1 && !strcmp(argv[1], "--bench");
	long perf;

	ksft_print_header();
	ksft_set_plan(bench ? 2 : 1);

	if (posix_memalign((void **)&src_buf, ALIGNMENT, BUFFER_SIZE) ||
	    posix_memalign((void **)&dst_buf, ALIGNMENT, BUFFER_SIZE) ||
	    posix_memalign((void **)&ref_buf, ALIGNMENT, BUFFER_SIZE))
		ksft_exit_fail_msg("buffer allocation failed\n");

	pin_to_first_cpu();

	perf = scalar_misaligned_perf();
	if (perf < 0)
		ksft_print_msg("hwprobe scalar misaligned perf unavailable: %ld\n", perf);
	else
		ksft_print_msg("hwprobe scalar misaligned perf: %ld\n", perf);

	if (run_correctness())
		ksft_test_result_fail("all size/alignment memcpy cases\n");
	else
		ksft_test_result_pass("all size/alignment memcpy cases\n");

	if (bench) {
		run_benchmarks();
		ksft_test_result_pass("benchmark completed\n");
	}

	ksft_finished();
}
