// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../kselftest.h"

#define COPY_PAGE_TEST_DIR	"/sys/kernel/debug/riscv_copy_page_test"
#define COPY_PAGE_TEST_RUN	COPY_PAGE_TEST_DIR "/run"
#define COPY_PAGE_TEST_ITERS	COPY_PAGE_TEST_DIR "/iterations"

static void write_iterations(const char *iterations)
{
	int fd;

	fd = open(COPY_PAGE_TEST_ITERS, O_WRONLY);
	if (fd < 0)
		ksft_exit_fail_msg("open %s: %s\n", COPY_PAGE_TEST_ITERS,
				   strerror(errno));

	if (write(fd, iterations, strlen(iterations)) < 0)
		ksft_exit_fail_msg("write %s: %s\n", COPY_PAGE_TEST_ITERS,
				   strerror(errno));

	close(fd);
}

static void ensure_debugfs(void)
{
	struct stat st;

	if (!stat(COPY_PAGE_TEST_RUN, &st))
		return;

	mkdir("/sys/kernel/debug", 0755);
	if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) &&
	    errno != EBUSY)
		ksft_print_msg("debugfs mount failed: %s\n", strerror(errno));
}

static bool output_has(const char *buf, const char *needle)
{
	return strstr(buf, needle);
}

int main(int argc, char **argv)
{
	char buf[4096];
	const char *iterations = argc > 1 ? argv[1] : "10000";
	ssize_t len;
	int fd;

	ksft_print_header();
	ksft_set_plan(1);

	ensure_debugfs();
	if (access(COPY_PAGE_TEST_RUN, R_OK))
		ksft_exit_skip("missing %s; enable or load test_copy_page\n",
			       COPY_PAGE_TEST_RUN);

	write_iterations(iterations);

	fd = open(COPY_PAGE_TEST_RUN, O_RDONLY);
	if (fd < 0)
		ksft_exit_fail_msg("open %s: %s\n", COPY_PAGE_TEST_RUN,
				   strerror(errno));

	len = read(fd, buf, sizeof(buf) - 1);
	if (len < 0)
		ksft_exit_fail_msg("read %s: %s\n", COPY_PAGE_TEST_RUN,
				   strerror(errno));
	close(fd);
	buf[len] = '\0';

	ksft_print_msg("%s", buf);

	if (!output_has(buf, "status: ok\n"))
		ksft_test_result_fail("copy_page correctness\n");
	else if (!output_has(buf, "irq_disabled_vector_eligible: 0\n") &&
		 !output_has(buf, "preemptive_kernel_vector: 1\n"))
		ksft_test_result_fail("copy_page IRQ-disabled fallback\n");
	else
		ksft_test_result_pass("copy_page correctness and fallback\n");

	ksft_finished();
}
