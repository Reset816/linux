// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(arr)		(sizeof(arr) / sizeof((arr)[0]))
#define COPY_PAGE_TEST_DIR	"/sys/kernel/debug/riscv_copy_page_test"
#define COPY_PAGE_TEST_STATS	COPY_PAGE_TEST_DIR "/stats"
#define COPY_PAGE_TEST_RESET	COPY_PAGE_TEST_DIR "/reset_stats"

#define PAGE_BYTES		4096UL
#define DEFAULT_PAGES		4096UL
#define DEFAULT_LOOPS		16U

struct copy_page_stats {
	unsigned long long calls;
	unsigned long long vector_calls;
	unsigned long long scalar_calls;
	unsigned int vector_eligible;
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;

	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void fail(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

static void ensure_debugfs(void)
{
	struct stat st;

	if (!stat(COPY_PAGE_TEST_STATS, &st))
		return;

	mkdir("/sys/kernel/debug", 0755);
	if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) &&
	    errno != EBUSY)
		fail("mount debugfs");
}

static void write_file(const char *path, const char *buf)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		fail(path);
	if (write(fd, buf, strlen(buf)) < 0)
		fail(path);
	close(fd);
}

static void reset_stats(void)
{
	write_file(COPY_PAGE_TEST_RESET, "1\n");
}

static void read_stats(struct copy_page_stats *stats)
{
	char buf[512];
	ssize_t len;
	FILE *f;
	int fd;

	memset(stats, 0, sizeof(*stats));

	fd = open(COPY_PAGE_TEST_STATS, O_RDONLY);
	if (fd < 0)
		fail(COPY_PAGE_TEST_STATS);

	len = read(fd, buf, sizeof(buf) - 1);
	if (len < 0)
		fail(COPY_PAGE_TEST_STATS);
	close(fd);
	buf[len] = '\0';

	f = fmemopen(buf, len, "r");
	if (!f)
		fail("fmemopen");

	while (fgets(buf, sizeof(buf), f)) {
		if (sscanf(buf, "calls: %llu", &stats->calls) == 1)
			continue;
		if (sscanf(buf, "vector_calls: %llu",
			   &stats->vector_calls) == 1)
			continue;
		if (sscanf(buf, "scalar_calls: %llu",
			   &stats->scalar_calls) == 1)
			continue;
		if (sscanf(buf, "copy_page_vector_eligible: %u",
			   &stats->vector_eligible) == 1)
			continue;
	}

	fclose(f);
}

static void touch_write(unsigned char *buf, size_t pages, unsigned int seed)
{
	size_t i;

	for (i = 0; i < pages; i++)
		buf[i * PAGE_BYTES] = (unsigned char)(i + seed);
}

static unsigned long checksum_pages(const unsigned char *buf, size_t pages)
{
	unsigned long sum = 0;
	size_t i;

	for (i = 0; i < pages; i++)
		sum += buf[i * PAGE_BYTES];

	return sum;
}

static void *map_private(size_t pages)
{
	void *p = mmap(NULL, pages * PAGE_BYTES, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		fail("mmap");

	return p;
}

static void workload_anon_cow(size_t pages, unsigned int loops)
{
	unsigned char *buf = map_private(pages);
	unsigned int l;

	touch_write(buf, pages, 0x11);

	for (l = 0; l < loops; l++) {
		pid_t pid = fork();

		if (pid < 0)
			fail("fork");

		if (!pid) {
			touch_write(buf, pages, l);
			_exit(checksum_pages(buf, pages) ? 0 : 2);
		}

		if (waitpid(pid, NULL, 0) < 0)
			fail("waitpid");
	}

	munmap(buf, pages * PAGE_BYTES);
}

static void workload_tmpfs_fault(size_t pages, unsigned int loops)
{
	const char *path = "/tmp/copy-page-tmpfs.bin";
	unsigned char fill[PAGE_BYTES];
	size_t len = pages * PAGE_BYTES;
	size_t i;
	unsigned int l;
	int fd;

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0)
		fail(path);

	memset(fill, 0x5a, sizeof(fill));
	for (i = 0; i < pages; i++) {
		fill[0] = (unsigned char)i;
		if (write(fd, fill, sizeof(fill)) != sizeof(fill))
			fail("write tmpfs");
	}

	for (l = 0; l < loops; l++) {
		unsigned char *buf;

		buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
		if (buf == MAP_FAILED)
			fail("mmap tmpfs");

		touch_write(buf, pages, l + 0x41);
		munmap(buf, len);
	}

	close(fd);
	unlink(path);
}

static void workload_fork_exec(size_t pages, unsigned int loops)
{
	unsigned char *buf = map_private(pages);
	char *const argv[] = { "/bin/true", NULL };
	unsigned int l;

	touch_write(buf, pages, 0x22);

	for (l = 0; l < loops; l++) {
		pid_t pid = fork();

		if (pid < 0)
			fail("fork");

		if (!pid) {
			execv(argv[0], argv);
			_exit(127);
		}

		if (waitpid(pid, NULL, 0) < 0)
			fail("waitpid");
	}

	munmap(buf, pages * PAGE_BYTES);
}

static void workload_cpu_control(size_t pages, unsigned int loops)
{
	unsigned long acc = 1;
	size_t iterations = pages * loops * 512;
	size_t i;

	for (i = 0; i < iterations; i++)
		acc = acc * 1103515245 + 12345 + i;

	asm volatile("" : "+r" (acc));
	if (!acc)
		printf("# impossible accumulator value\n");
}

static void run_one(const char *name, size_t pages, unsigned int loops)
{
	struct copy_page_stats stats;
	uint64_t start, elapsed;

	reset_stats();
	start = monotonic_ns();

	if (!strcmp(name, "anon-cow")) {
		workload_anon_cow(pages, loops);
	} else if (!strcmp(name, "tmpfs-private")) {
		workload_tmpfs_fault(pages, loops);
	} else if (!strcmp(name, "fork-exec")) {
		workload_fork_exec(pages, loops);
	} else if (!strcmp(name, "cpu-control")) {
		workload_cpu_control(pages, loops);
	} else {
		fprintf(stderr, "unknown workload: %s\n", name);
		exit(1);
	}

	elapsed = monotonic_ns() - start;
	read_stats(&stats);

	printf("workload: %s\n", name);
	printf("pages: %zu\n", pages);
	printf("loops: %u\n", loops);
	printf("elapsed_ns: %llu\n", (unsigned long long)elapsed);
	printf("copy_page_calls: %llu\n", stats.calls);
	printf("copy_page_vector_calls: %llu\n", stats.vector_calls);
	printf("copy_page_scalar_calls: %llu\n", stats.scalar_calls);
	printf("copy_page_vector_eligible: %u\n", stats.vector_eligible);
	printf("\n");
}

int main(int argc, char **argv)
{
	size_t pages = argc > 1 ? strtoul(argv[1], NULL, 0) : DEFAULT_PAGES;
	unsigned int loops = argc > 2 ? strtoul(argv[2], NULL, 0) :
				DEFAULT_LOOPS;
	static const char * const workloads[] = {
		"anon-cow",
		"tmpfs-private",
		"fork-exec",
		"cpu-control",
	};
	size_t i;

	if (!pages)
		pages = DEFAULT_PAGES;
	if (!loops)
		loops = DEFAULT_LOOPS;

	ensure_debugfs();
	if (access(COPY_PAGE_TEST_STATS, R_OK))
		fail("missing copy_page debugfs stats");

	for (i = 0; i < ARRAY_SIZE(workloads); i++)
		run_one(workloads[i], pages, loops);

	return 0;
}
