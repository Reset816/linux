// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
		perror("clock_gettime");
		exit(1);
	}

	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint64_t parse_u64(const char *s, const char *name)
{
	char *end;
	uint64_t v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || *end) {
		fprintf(stderr, "bad %s: %s\n", name, s);
		exit(2);
	}

	return v;
}

static void full_write(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t ret = write(fd, p, len);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("write");
			exit(1);
		}
		p += ret;
		len -= ret;
	}
}

static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
	size_t i;

	for (i = 0; i < len; i++) {
		seed = seed * 1664525U + 1013904223U;
		buf[i] = 'a' + ((seed >> 24) % 26);
	}
}

static void print_result(const char *workload, unsigned int sample,
			 uint64_t ops, uint64_t bytes, uint64_t ns,
			 uint64_t checksum)
{
	uint64_t ops_s = ops * 1000000000ULL / (ns ? ns : 1);
	uint64_t bytes_s = bytes * 1000000000ULL / (ns ? ns : 1);

	printf("RESULT workload=%s sample=%u ops=%" PRIu64
	       " bytes=%" PRIu64 " ns=%" PRIu64 " ops_s=%" PRIu64
	       " bytes_s=%" PRIu64 " checksum=%" PRIu64 "\n",
	       workload, sample, ops, bytes, ns, ops_s, bytes_s, checksum);
	fflush(stdout);
}

static void path_join3(char *buf, size_t len, const char *dir,
		       const char *prefix, unsigned int idx,
		       const char *suffix)
{
	int ret = snprintf(buf, len, "%s/%s%05u%s", dir, prefix, idx, suffix);

	if (ret < 0 || ret >= len) {
		fprintf(stderr, "path too long under %s\n", dir);
		exit(1);
	}
}

static void remove_tree(const char *path)
{
	DIR *dir = opendir(path);
	struct dirent *de;

	if (!dir)
		return;

	while ((de = readdir(dir))) {
		char child[PATH_MAX];
		struct stat st;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
		if (lstat(child, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			remove_tree(child);
		else
			unlink(child);
	}

	closedir(dir);
	rmdir(path);
}

static void ensure_dir(const char *path)
{
	if (mkdir(path, 0700) && errno != EEXIST) {
		perror(path);
		exit(1);
	}
}

static void workload_ext4_meta(const char *root, unsigned int samples,
			       unsigned int files)
{
	uint8_t buf[96];
	unsigned int sample;

	fill_pattern(buf, sizeof(buf), 0x10203040U);

	for (sample = 0; sample < samples; sample++) {
		char dir[PATH_MAX];
		uint64_t start, end, checksum = 0;
		unsigned int i;
		int dfd;

		snprintf(dir, sizeof(dir), "%s/meta-%u", root, sample);
		remove_tree(dir);
		ensure_dir(dir);
		dfd = open(dir, O_RDONLY | O_DIRECTORY);
		if (dfd < 0) {
			perror("open metadata dir");
			exit(1);
		}

		start = now_ns();
		for (i = 0; i < files; i++) {
			char a[PATH_MAX], b[PATH_MAX];
			int fd;

			path_join3(a, sizeof(a), dir, "file-", i, ".tmp");
			path_join3(b, sizeof(b), dir, "file-", i, ".dat");

			fd = open(a, O_CREAT | O_TRUNC | O_RDWR, 0600);
			if (fd < 0) {
				perror("open meta file");
				exit(1);
			}
			full_write(fd, buf, sizeof(buf));
			if (fsync(fd))
				perror("fsync file");
			close(fd);
			if (rename(a, b)) {
				perror("rename");
				exit(1);
			}
			if ((i & 7) == 7 && fsync(dfd))
				perror("fsync dir");
			if ((i & 1) == 1 && unlink(b)) {
				perror("unlink");
				exit(1);
			}
			checksum += i;
		}
		if (fsync(dfd))
			perror("fsync dir final");
		end = now_ns();
		close(dfd);

		print_result("ext4_meta", sample, files, (uint64_t)files * sizeof(buf),
			     end - start, checksum);
		remove_tree(dir);
	}
}

static void workload_xattr(const char *root, unsigned int samples,
			   unsigned int files, size_t value_len)
{
	uint8_t *value = malloc(value_len);
	uint8_t *readback = malloc(value_len);
	unsigned int sample;

	if (!value || !readback) {
		perror("malloc");
		exit(1);
	}
	fill_pattern(value, value_len, 0x55667788U);

	for (sample = 0; sample < samples; sample++) {
		char dir[PATH_MAX];
		uint64_t start, end, checksum = 0;
		unsigned int i;

		snprintf(dir, sizeof(dir), "%s/xattr-%u", root, sample);
		remove_tree(dir);
		ensure_dir(dir);

		start = now_ns();
		for (i = 0; i < files; i++) {
			char path[PATH_MAX];
			ssize_t got;
			int fd;

			path_join3(path, sizeof(path), dir, "file-", i, "");
			fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
			if (fd < 0) {
				perror("open xattr file");
				exit(1);
			}
			full_write(fd, value, 64);
			close(fd);

			if (setxattr(path, "user.big", value, value_len, 0)) {
				perror("setxattr");
				exit(1);
			}
			got = getxattr(path, "user.big", readback, value_len);
			if (got != (ssize_t)value_len) {
				perror("getxattr");
				exit(1);
			}
			checksum += readback[(i * 131U) % value_len];
			if (removexattr(path, "user.big")) {
				perror("removexattr");
				exit(1);
			}
			unlink(path);
		}
		end = now_ns();

		print_result("ext4_xattr", sample, files,
			     (uint64_t)files * value_len * 2, end - start,
			     checksum);
		remove_tree(dir);
	}

	free(value);
	free(readback);
}

static int read_file_hash(const char *path, uint8_t *buf, size_t buf_len,
			  uint64_t *bytes, uint64_t *checksum)
{
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;

	for (;;) {
		ssize_t got = read(fd, buf, buf_len);

		if (got < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			exit(1);
		}
		if (!got)
			break;
		*bytes += got;
		*checksum += buf[(got - 1) / 2];
	}

	close(fd);
	return 0;
}

static void scan_tree(const char *path, uint8_t *buf, size_t buf_len,
		      uint64_t *files, uint64_t *bytes, uint64_t *checksum)
{
	DIR *dir = opendir(path);
	struct dirent *de;

	if (!dir)
		return;

	while ((de = readdir(dir))) {
		char child[PATH_MAX];
		struct stat st;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
		if (lstat(child, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			scan_tree(child, buf, buf_len, files, bytes, checksum);
		} else if (S_ISREG(st.st_mode)) {
			if (!read_file_hash(child, buf, buf_len, bytes, checksum))
				(*files)++;
		}
	}

	closedir(dir);
}

static void workload_read_tree(const char *name, const char *root,
			       unsigned int samples)
{
	uint8_t *buf = malloc(65536);
	unsigned int sample;

	if (!buf) {
		perror("malloc");
		exit(1);
	}

	for (sample = 0; sample < samples; sample++) {
		uint64_t files = 0, bytes = 0, checksum = 0;
		uint64_t start = now_ns();

		scan_tree(root, buf, 65536, &files, &bytes, &checksum);
		print_result(name, sample, files, bytes, now_ns() - start,
			     checksum);
	}

	free(buf);
}

static void workload_f2fs_compress(const char *root, unsigned int samples,
				   unsigned int files, size_t file_size)
{
	uint8_t *buf = malloc(file_size);
	unsigned int sample;

	if (!buf) {
		perror("malloc");
		exit(1);
	}
	memset(buf, 'A', file_size);

	for (sample = 0; sample < samples; sample++) {
		char dir[PATH_MAX];
		uint64_t start, end, checksum = 0;
		unsigned int i;

		snprintf(dir, sizeof(dir), "%s/f2fs-%u", root, sample);
		remove_tree(dir);
		ensure_dir(dir);

		start = now_ns();
		for (i = 0; i < files; i++) {
			char path[PATH_MAX];
			int fd;

			path_join3(path, sizeof(path), dir, "file-", i, "");
			fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
			if (fd < 0) {
				perror("open f2fs file");
				exit(1);
			}
			full_write(fd, buf, file_size);
			if (fsync(fd))
				perror("fsync f2fs file");
			lseek(fd, 0, SEEK_SET);
			if (read(fd, buf, file_size) != (ssize_t)file_size) {
				perror("read f2fs file");
				exit(1);
			}
			checksum += buf[(i * 257U) % file_size];
			close(fd);
		}
		sync();
		end = now_ns();

		print_result("f2fs_compress", sample, files,
			     (uint64_t)files * file_size * 2, end - start,
			     checksum);
		remove_tree(dir);
	}

	free(buf);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s ext4_meta root samples files\n"
			"       %s ext4_xattr root samples files value_len\n"
			"       %s read_tree name root samples\n"
			"       %s f2fs_compress root samples files file_size\n",
			argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "ext4_meta")) {
		if (argc != 5)
			return 2;
		workload_ext4_meta(argv[2], parse_u64(argv[3], "samples"),
				   parse_u64(argv[4], "files"));
		return 0;
	}

	if (!strcmp(argv[1], "ext4_xattr")) {
		if (argc != 6)
			return 2;
		workload_xattr(argv[2], parse_u64(argv[3], "samples"),
			       parse_u64(argv[4], "files"),
			       parse_u64(argv[5], "value_len"));
		return 0;
	}

	if (!strcmp(argv[1], "read_tree")) {
		if (argc != 5)
			return 2;
		workload_read_tree(argv[2], argv[3], parse_u64(argv[4], "samples"));
		return 0;
	}

	if (!strcmp(argv[1], "f2fs_compress")) {
		if (argc != 6)
			return 2;
		workload_f2fs_compress(argv[2], parse_u64(argv[3], "samples"),
				       parse_u64(argv[4], "files"),
				       parse_u64(argv[5], "file_size"));
		return 0;
	}

	fprintf(stderr, "unknown workload: %s\n", argv[1]);
	return 2;
}
