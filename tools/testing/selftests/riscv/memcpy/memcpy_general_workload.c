// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
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

static void fill_buf(uint8_t *buf, size_t len, uint32_t seed)
{
	size_t i;

	for (i = 0; i < len; i++) {
		seed = seed * 1664525U + 1013904223U;
		buf[i] = seed >> 24;
	}
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

static size_t full_read_some(int fd, void *buf, size_t len)
{
	for (;;) {
		ssize_t ret = read(fd, buf, len);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			exit(1);
		}
		return ret;
	}
}

static void print_result(const char *name, unsigned int sample, uint64_t bytes,
			 uint64_t nsec, uint64_t checksum)
{
	uint64_t mib_s = bytes * 1000000000ULL / (nsec ? nsec : 1) / 1048576ULL;

	printf("RESULT workload=%s sample=%u bytes=%" PRIu64
	       " ns=%" PRIu64 " MiB_s=%" PRIu64 " checksum=%" PRIu64 "\n",
	       name, sample, bytes, nsec, mib_s, checksum);
	fflush(stdout);
}

static void make_source_file(const char *path, uint64_t bytes, size_t chunk)
{
	uint8_t *buf = malloc(chunk);
	uint64_t left = bytes;
	int fd;

	if (!buf) {
		perror("malloc");
		exit(1);
	}
	fill_buf(buf, chunk, 0xdecafbadU);

	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd < 0) {
		perror(path);
		exit(1);
	}

	while (left) {
		size_t n = left > chunk ? chunk : left;

		full_write(fd, buf, n);
		left -= n;
	}

	if (close(fd)) {
		perror("close source");
		exit(1);
	}

	free(buf);
}

static void workload_filecopy(uint64_t bytes, size_t chunk, unsigned int samples)
{
	uint8_t *buf = malloc(chunk);
	unsigned int i;

	if (!buf) {
		perror("malloc");
		exit(1);
	}

	make_source_file("/tmp/memcpy-general-src.bin", bytes, chunk);

	for (i = 0; i < samples; i++) {
		uint64_t copied = 0, checksum = 0;
		uint64_t start, end;
		int in, out;

		in = open("/tmp/memcpy-general-src.bin", O_RDONLY);
		out = open("/tmp/memcpy-general-dst.bin", O_CREAT | O_TRUNC | O_WRONLY, 0600);
		if (in < 0 || out < 0) {
			perror("open filecopy");
			exit(1);
		}

		start = now_ns();
		while (copied < bytes) {
			size_t want = bytes - copied > chunk ? chunk : bytes - copied;
			size_t got = full_read_some(in, buf, want);

			if (!got)
				break;
			full_write(out, buf, got);
			checksum += buf[(copied + got - 1) % got];
			copied += got;
		}
		end = now_ns();

		close(in);
		close(out);
		unlink("/tmp/memcpy-general-dst.bin");
		print_result("filecopy_tmpfs", i, copied, end - start, checksum);
	}

	unlink("/tmp/memcpy-general-src.bin");
	free(buf);
}

static void writer_loop(int fd, const uint8_t *buf, size_t chunk, uint64_t bytes)
{
	uint64_t written = 0;

	while (written < bytes) {
		size_t n = bytes - written > chunk ? chunk : bytes - written;

		full_write(fd, buf, n);
		written += n;
	}
}

static void workload_pipe(uint64_t bytes, size_t chunk, unsigned int samples)
{
	uint8_t *wbuf = malloc(chunk), *rbuf = malloc(chunk);
	unsigned int i;

	if (!wbuf || !rbuf) {
		perror("malloc");
		exit(1);
	}
	fill_buf(wbuf, chunk, 0x12345678U);

	for (i = 0; i < samples; i++) {
		uint64_t read_bytes = 0, checksum = 0;
		uint64_t start, end;
		int p[2], status;
		pid_t pid;

		if (pipe(p)) {
			perror("pipe");
			exit(1);
		}

		pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(1);
		}
		if (!pid) {
			close(p[0]);
			writer_loop(p[1], wbuf, chunk, bytes);
			close(p[1]);
			_exit(0);
		}

		close(p[1]);
		start = now_ns();
		while (read_bytes < bytes) {
			size_t got = full_read_some(p[0], rbuf, chunk);

			if (!got)
				break;
			checksum += rbuf[(read_bytes + got - 1) % got];
			read_bytes += got;
		}
		end = now_ns();
		close(p[0]);
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			exit(1);

		print_result("pipe_copy", i, read_bytes, end - start, checksum);
	}

	free(wbuf);
	free(rbuf);
}

static void workload_tcp(uint64_t bytes, size_t chunk, unsigned int samples)
{
	uint8_t *wbuf = malloc(chunk), *rbuf = malloc(chunk);
	unsigned int i;

	if (!wbuf || !rbuf) {
		perror("malloc");
		exit(1);
	}
	fill_buf(wbuf, chunk, 0x0badc0deU);

	for (i = 0; i < samples; i++) {
		struct sockaddr_in addr = { .sin_family = AF_INET };
		socklen_t addrlen = sizeof(addr);
		uint64_t read_bytes = 0, checksum = 0;
		uint64_t start, end;
		int srv, cli, acc, status;
		pid_t pid;

		srv = socket(AF_INET, SOCK_STREAM, 0);
		if (srv < 0) {
			perror("socket server");
			exit(1);
		}
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) ||
		    listen(srv, 1) ||
		    getsockname(srv, (struct sockaddr *)&addr, &addrlen)) {
			perror("server setup");
			exit(1);
		}

		pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(1);
		}
		if (!pid) {
			cli = socket(AF_INET, SOCK_STREAM, 0);
			if (cli < 0 || connect(cli, (struct sockaddr *)&addr, sizeof(addr))) {
				perror("client connect");
				_exit(1);
			}
			writer_loop(cli, wbuf, chunk, bytes);
			close(cli);
			_exit(0);
		}

		acc = accept(srv, NULL, NULL);
		if (acc < 0) {
			perror("accept");
			exit(1);
		}
		close(srv);

		start = now_ns();
		while (read_bytes < bytes) {
			size_t got = full_read_some(acc, rbuf, chunk);

			if (!got)
				break;
			checksum += rbuf[(read_bytes + got - 1) % got];
			read_bytes += got;
		}
		end = now_ns();
		close(acc);
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			exit(1);

		print_result("tcp_loopback", i, read_bytes, end - start, checksum);
	}

	free(wbuf);
	free(rbuf);
}

static void workload_compute(uint64_t iters, unsigned int samples)
{
	unsigned int i;

	for (i = 0; i < samples; i++) {
		uint64_t x = 0x9e3779b97f4a7c15ULL + i;
		uint64_t start = now_ns();
		uint64_t j;

		for (j = 0; j < iters; j++) {
			x ^= x >> 12;
			x ^= x << 25;
			x ^= x >> 27;
			x *= 2685821657736338717ULL;
		}

		print_result("compute_xorshift", i, iters, now_ns() - start, x);
	}
}

static void workload_command(const char *name, uint64_t bytes, unsigned int samples,
			     char **cmd)
{
	unsigned int i;

	for (i = 0; i < samples; i++) {
		uint64_t start = now_ns();
		int status;
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			exit(1);
		}
		if (!pid) {
			execvp(cmd[0], cmd);
			perror("execvp");
			_exit(127);
		}
		if (waitpid(pid, &status, 0) < 0) {
			perror("waitpid");
			exit(1);
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "command failed for %s sample=%u status=%d\n",
				name, i, status);
			exit(1);
		}

		print_result(name, i, bytes, now_ns() - start, status);
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s filecopy|pipe|tcp|compute [bytes/iters] [chunk] [samples]\n"
			"       %s command name bytes samples -- command [args...]\n",
			argv[0],
			argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "compute")) {
		uint64_t iters = argc > 2 ? parse_u64(argv[2], "iters") : 200000000ULL;
		unsigned int samples = argc > 3 ? parse_u64(argv[3], "samples") : 5;

		workload_compute(iters, samples);
		return 0;
	}

	if (!strcmp(argv[1], "command")) {
		if (argc < 7 || strcmp(argv[5], "--")) {
			fprintf(stderr,
				"usage: %s command name bytes samples -- command [args...]\n",
				argv[0]);
			return 2;
		}

		workload_command(argv[2], parse_u64(argv[3], "bytes"),
				 parse_u64(argv[4], "samples"), &argv[6]);
		return 0;
	}

	{
		uint64_t bytes = argc > 2 ? parse_u64(argv[2], "bytes") : 67108864ULL;
		size_t chunk = argc > 3 ? parse_u64(argv[3], "chunk") : 65536;
		unsigned int samples = argc > 4 ? parse_u64(argv[4], "samples") : 5;

		if (!strcmp(argv[1], "filecopy")) {
			workload_filecopy(bytes, chunk, samples);
			return 0;
		}
		if (!strcmp(argv[1], "pipe")) {
			workload_pipe(bytes, chunk, samples);
			return 0;
		}
		if (!strcmp(argv[1], "tcp")) {
			workload_tcp(bytes, chunk, samples);
			return 0;
		}
	}

	fprintf(stderr, "unknown workload: %s\n", argv[1]);
	return 2;
}
