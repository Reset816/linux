// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static void run_anon(size_t mib, unsigned int passes)
{
	size_t len = mib * 1024 * 1024;
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	volatile unsigned char sum = 0;
	uint64_t start = nsec_now();

	for (unsigned int pass = 0; pass < passes; pass++) {
		unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			die("mmap");

		for (size_t off = 0; off < len; off += page) {
			p[off] = (unsigned char)pass;
			sum ^= p[off];
		}

		if (munmap(p, len))
			die("munmap");
	}

	printf("anon_fault mib=%zu passes=%u ns=%" PRIu64 " pages=%zu sum=%u\n",
	       mib, passes, nsec_now() - start, (len / page) * passes, sum);
}

static void fill_buffer(unsigned char *buf, size_t len, unsigned char value)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = value + (unsigned char)i;
}

static void run_fs(const char *label, const char *dir, size_t mib)
{
	char path[512];
	size_t chunk = 64 * 1024;
	size_t total = mib * 1024 * 1024;
	unsigned char *buf;
	uint64_t start;
	int fd;

	if (posix_memalign((void **)&buf, 4096, chunk))
		die("posix_memalign");
	fill_buffer(buf, chunk, 0x31);

	snprintf(path, sizeof(path), "%s/zicboz-workload.bin", dir);
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (fd < 0)
		die("open workload file");

	start = nsec_now();
	for (size_t off = 0; off < total; off += chunk) {
		if (write(fd, buf, chunk) != (ssize_t)chunk)
			die("write");
	}

	if (ftruncate(fd, (off_t)total * 2))
		die("ftruncate grow");

	if (lseek(fd, (off_t)total * 2 + 4096, SEEK_SET) < 0)
		die("lseek sparse");
	if (write(fd, buf, chunk) != (ssize_t)chunk)
		die("write sparse tail");

	if (ftruncate(fd, (off_t)total / 2))
		die("ftruncate shrink");

	fsync(fd);
	close(fd);
	unlink(path);

	printf("%s_fs mib=%zu ns=%" PRIu64 " chunk=%zu\n",
	       label, mib, nsec_now() - start, chunk);
	free(buf);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s anon <mib> <passes> | tmpfs <dir> <mib> | ext4 <dir> <mib>\n",
		argv0);
	exit(2);
}

int main(int argc, char **argv)
{
	if (argc < 2)
		usage(argv[0]);

	if (!strcmp(argv[1], "anon")) {
		if (argc != 4)
			usage(argv[0]);
		run_anon(strtoull(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
	} else if (!strcmp(argv[1], "tmpfs")) {
		if (argc != 4)
			usage(argv[0]);
		run_fs("tmpfs", argv[2], strtoull(argv[3], NULL, 0));
	} else if (!strcmp(argv[1], "ext4")) {
		if (argc != 4)
			usage(argv[0]);
		run_fs("ext4", argv[2], strtoull(argv[3], NULL, 0));
	} else {
		usage(argv[0]);
	}

	return 0;
}
