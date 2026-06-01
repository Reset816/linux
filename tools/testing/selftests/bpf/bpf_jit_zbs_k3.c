// SPDX-License-Identifier: GPL-2.0-only
/*
 * Small syscall-only BPF JIT workload for RISC-V Zbs measurements.
 *
 * The program intentionally avoids libbpf so it can be copied into a tiny
 * initramfs and run under QEMU/KVM.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/unistd.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_bpf
#define __NR_bpf 280
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define BPF_RAW_INSN(CODE, DST, SRC, OFF, IMM)	\
	((struct bpf_insn){			\
		.code = (CODE),			\
		.dst_reg = (DST),		\
		.src_reg = (SRC),		\
		.off = (OFF),			\
		.imm = (IMM),			\
	})

#define BPF_MOV64_IMM(DST, IMM) \
	BPF_RAW_INSN(BPF_ALU64 | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define BPF_MOV32_IMM(DST, IMM) \
	BPF_RAW_INSN(BPF_ALU | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define BPF_ALU64_IMM(OP, DST, IMM) \
	BPF_RAW_INSN(BPF_ALU64 | (OP) | BPF_K, DST, 0, 0, IMM)
#define BPF_ALU32_IMM(OP, DST, IMM) \
	BPF_RAW_INSN(BPF_ALU | (OP) | BPF_K, DST, 0, 0, IMM)
#define BPF_JMP_IMM(OP, DST, IMM, OFF) \
	BPF_RAW_INSN(BPF_JMP | (OP) | BPF_K, DST, 0, OFF, IMM)
#define BPF_JMP32_IMM(OP, DST, IMM, OFF) \
	BPF_RAW_INSN(BPF_JMP32 | (OP) | BPF_K, DST, 0, OFF, IMM)
#define BPF_EMIT_CALL(FUNC) \
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, FUNC)
#define BPF_EXIT_INSN() \
	BPF_RAW_INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

struct case_def {
	const char *name;
	const char *group;
	const struct bpf_insn *insns;
	size_t insn_cnt;
	unsigned int repeat;
	unsigned int expected_ret;
	unsigned int zbs_expected;
	unsigned int fallback_insns;
	unsigned int zbs_insns;
	const char *zbs_ops;
};

static const struct bpf_insn dedicated_alu64[] = {
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ALU64_IMM(BPF_OR, BPF_REG_0, 1 << 11),
	BPF_ALU64_IMM(BPF_XOR, BPF_REG_0, 1 << 12),
	BPF_ALU64_IMM(BPF_AND, BPF_REG_0, ~(1 << 11)),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
};

static const struct bpf_insn dedicated_jset[] = {
	BPF_EMIT_CALL(BPF_FUNC_get_prandom_u32),
	BPF_JMP_IMM(BPF_JSET, BPF_REG_0, 1 << 11, 2),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
};

static const struct bpf_insn dedicated_alu32_bit31[] = {
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ALU32_IMM(BPF_OR, BPF_REG_0, 0x80000000),
	BPF_ALU32_IMM(BPF_XOR, BPF_REG_0, 0x80000000),
	BPF_ALU32_IMM(BPF_AND, BPF_REG_0, 0x7fffffff),
	BPF_EMIT_CALL(BPF_FUNC_get_prandom_u32),
	BPF_JMP32_IMM(BPF_JSET, BPF_REG_0, 0x80000000, 2),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
};

static const struct bpf_insn control_12bit[] = {
	BPF_EMIT_CALL(BPF_FUNC_get_prandom_u32),
	BPF_ALU64_IMM(BPF_OR, BPF_REG_0, 1 << 10),
	BPF_ALU64_IMM(BPF_XOR, BPF_REG_0, 1 << 10),
	BPF_ALU64_IMM(BPF_AND, BPF_REG_0, ~1),
	BPF_JMP_IMM(BPF_JSET, BPF_REG_0, 1 << 9, 2),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
};

static const struct bpf_insn control_multi_bit[] = {
	BPF_EMIT_CALL(BPF_FUNC_get_prandom_u32),
	BPF_ALU64_IMM(BPF_OR, BPF_REG_0, (1 << 11) | (1 << 12)),
	BPF_ALU64_IMM(BPF_XOR, BPF_REG_0, (1 << 11) | (1 << 12)),
	BPF_ALU64_IMM(BPF_AND, BPF_REG_0, ~((1 << 11) | (1 << 12))),
	BPF_JMP_IMM(BPF_JSET, BPF_REG_0, (1 << 13) | (1 << 14), 2),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
	BPF_MOV64_IMM(BPF_REG_0, 7),
	BPF_EXIT_INSN(),
};

static const struct bpf_insn broad_packet_flags[] = {
	BPF_EMIT_CALL(BPF_FUNC_get_prandom_u32),
	BPF_ALU64_IMM(BPF_OR, BPF_REG_0, 1 << 11),
	BPF_ALU64_IMM(BPF_OR, BPF_REG_0, 1 << 12),
	BPF_JMP_IMM(BPF_JSET, BPF_REG_0, 1 << 13, 1),
	BPF_MOV64_IMM(BPF_REG_6, 0),
	BPF_ALU64_IMM(BPF_XOR, BPF_REG_0, 1 << 11),
	BPF_ALU64_IMM(BPF_AND, BPF_REG_0, ~(1 << 12)),
	BPF_MOV64_IMM(BPF_REG_0, 9),
	BPF_EXIT_INSN(),
};

static const struct case_def cases[] = {
	{
		.name = "dedicated_alu64_zbs",
		.group = "dedicated",
		.insns = dedicated_alu64,
		.insn_cnt = ARRAY_SIZE(dedicated_alu64),
		.repeat = 2000000,
		.expected_ret = 7,
		.zbs_expected = 3,
		.fallback_insns = 14,
		.zbs_insns = 9,
		.zbs_ops = "bseti,binvi,bclri",
	},
	{
		.name = "dedicated_jset_zbs",
		.group = "dedicated",
		.insns = dedicated_jset,
		.insn_cnt = ARRAY_SIZE(dedicated_jset),
		.repeat = 2000000,
		.expected_ret = 7,
		.zbs_expected = 1,
		.fallback_insns = 4,
		.zbs_insns = 2,
		.zbs_ops = "bexti",
	},
	{
		.name = "dedicated_alu32_bit31",
		.group = "regression",
		.insns = dedicated_alu32_bit31,
		.insn_cnt = ARRAY_SIZE(dedicated_alu32_bit31),
		.repeat = 2000000,
		.expected_ret = 7,
		.zbs_expected = 4,
		.fallback_insns = 16,
		.zbs_insns = 10,
		.zbs_ops = "bseti,bexti,binvi,bclri",
	},
	{
		.name = "control_12bit_masks",
		.group = "control",
		.insns = control_12bit,
		.insn_cnt = ARRAY_SIZE(control_12bit),
		.repeat = 2000000,
		.expected_ret = 7,
		.zbs_expected = 0,
		.fallback_insns = 9,
		.zbs_insns = 9,
		.zbs_ops = "none",
	},
	{
		.name = "control_multi_bit_masks",
		.group = "control",
		.insns = control_multi_bit,
		.insn_cnt = ARRAY_SIZE(control_multi_bit),
		.repeat = 2000000,
		.expected_ret = 7,
		.zbs_expected = 0,
		.fallback_insns = 14,
		.zbs_insns = 14,
		.zbs_ops = "none",
	},
	{
		.name = "broad_packet_flags",
		.group = "broad",
		.insns = broad_packet_flags,
		.insn_cnt = ARRAY_SIZE(broad_packet_flags),
		.repeat = 2000000,
		.expected_ret = 9,
		.zbs_expected = 5,
		.fallback_insns = 17,
		.zbs_insns = 10,
		.zbs_ops = "bseti,bseti,bexti,binvi,bclri",
	},
};

static long sys_bpf(enum bpf_cmd cmd, union bpf_attr *attr, unsigned int size)
{
	return syscall(__NR_bpf, cmd, attr, size);
}

static int write_file(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t ret;

	if (fd < 0)
		return -errno;

	ret = write(fd, value, strlen(value));
	close(fd);
	if (ret < 0)
		return -errno;
	return 0;
}

static int read_file(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t ret;

	if (fd < 0)
		return -errno;
	ret = read(fd, buf, size - 1);
	close(fd);
	if (ret < 0)
		return -errno;
	buf[ret] = '\0';
	return 0;
}

static int load_prog(const struct case_def *c)
{
	char log_buf[65536];
	union bpf_attr attr;
	long fd;

	memset(&attr, 0, sizeof(attr));
	memset(log_buf, 0, sizeof(log_buf));
	attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
	attr.insn_cnt = c->insn_cnt;
	attr.insns = (uintptr_t)c->insns;
	attr.license = (uintptr_t)"GPL";
	attr.log_level = 1;
	attr.log_size = sizeof(log_buf);
	attr.log_buf = (uintptr_t)log_buf;
	snprintf(attr.prog_name, sizeof(attr.prog_name), "%s", c->name);

	fd = sys_bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
	if (fd < 0) {
		fprintf(stderr, "LOAD_FAIL name=%s errno=%d (%s)\n%s\n",
			c->name, errno, strerror(errno), log_buf);
		return -1;
	}

	return fd;
}

static int run_prog(int fd, const struct case_def *c, unsigned int repeat,
		    uint32_t *retval, uint32_t *duration)
{
	uint8_t pkt[64] = {};
	union bpf_attr attr;
	long ret;

	memset(&attr, 0, sizeof(attr));
	attr.test.prog_fd = fd;
	attr.test.data_in = (uintptr_t)pkt;
	attr.test.data_size_in = sizeof(pkt);
	attr.test.repeat = repeat;

	ret = sys_bpf(BPF_PROG_TEST_RUN, &attr, sizeof(attr));
	if (ret < 0) {
		fprintf(stderr, "RUN_FAIL name=%s errno=%d (%s)\n",
			c->name, errno, strerror(errno));
		return -1;
	}

	*retval = attr.test.retval;
	*duration = attr.test.duration;
	return 0;
}

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void print_cpu_features(void)
{
	char buf[8192];

	if (!read_file("/proc/cpuinfo", buf, sizeof(buf))) {
		char *isa = strstr(buf, "isa");

		if (isa) {
			char *end = strchr(isa, '\n');

			if (end)
				*end = '\0';
			printf("CPUINFO_%s\n", isa);
		}
	}
}

static void dump_jit_log(void)
{
	char line[8192];
	int fd;

	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
		fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	if (fd < 0)
		return;

	for (;;) {
		char *msg;
		ssize_t ret = read(fd, line, sizeof(line) - 1);

		if (ret <= 0)
			break;

		line[ret] = '\0';
		msg = strchr(line, ';');
		msg = msg ? msg + 1 : line;
		if (strstr(msg, "flen=") || strstr(msg, "JIT code:") ||
		    strstr(msg, "bpf_jit") || strstr(msg, "BPF") ||
		    strstr(msg, "zbs"))
			fputs(msg, stdout);
	}

	close(fd);
}

static void init_mounts(void)
{
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
}

int main(int argc, char **argv)
{
	bool init_mode = strstr(argv[0], "init") || (argc > 1 && !strcmp(argv[1], "--init"));
	const char *label = init_mode ? getenv("BPF_ZBS_LABEL") : argc > 1 ? argv[1] : "run";
	const char *scale = init_mode ? getenv("BPF_ZBS_REPEAT_SCALE") : argc > 2 ? argv[2] : "1";
	const char *harden = getenv("BPF_ZBS_HARDEN");
	unsigned int repeat_scale = scale ? strtoul(scale, NULL, 0) : 1;
	char buf[128] = {};
	int failed = 0;
	size_t i;

	if (!label)
		label = "init";
	if (!repeat_scale)
		repeat_scale = 1;

	if (init_mode)
		init_mounts();

	write_file("/proc/sys/net/core/bpf_jit_enable", "2\n");
	write_file("/proc/sys/net/core/bpf_jit_kallsyms", "1\n");
	write_file("/proc/sys/net/core/bpf_jit_harden", harden ? harden : "0\n");
	write_file("/proc/sys/kernel/printk", "8\n");

	if (!read_file("/proc/sys/net/core/bpf_jit_enable", buf, sizeof(buf)))
		printf("SYSCTL bpf_jit_enable=%s", buf);
	memset(buf, 0, sizeof(buf));
	if (!read_file("/proc/sys/net/core/bpf_jit_harden", buf, sizeof(buf)))
		printf("SYSCTL bpf_jit_harden=%s", buf);

	print_cpu_features();
	printf("RUN_LABEL %s repeat_scale=%u cases=%zu\n", label, repeat_scale, ARRAY_SIZE(cases));

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct case_def *c = &cases[i];
		unsigned int repeat = c->repeat * repeat_scale;
		uint32_t retval = 0, duration = 0;
		uint64_t start, elapsed;
		int fd;

		fd = load_prog(c);
		if (fd < 0) {
			failed = 1;
			continue;
		}

		if (run_prog(fd, c, 1, &retval, &duration) || retval != c->expected_ret) {
			fprintf(stderr, "VERIFY_FAIL name=%s retval=%u expected=%u duration_ns=%u\n",
				c->name, retval, c->expected_ret, duration);
			close(fd);
			failed = 1;
			continue;
		}

		start = nsec_now();
		if (run_prog(fd, c, repeat, &retval, &duration)) {
			close(fd);
			failed = 1;
			continue;
		}
		elapsed = nsec_now() - start;

		printf("RESULT label=%s group=%s name=%s repeat=%u retval=%u avg_ns=%u wall_ns=%llu "
		       "zbs_expected=%u fallback_jit_insns=%u zbs_jit_insns=%u saved_insns=%d zbs_ops=%s\n",
		       label, c->group, c->name, repeat, retval, duration,
		       (unsigned long long)elapsed, c->zbs_expected, c->fallback_insns,
		       c->zbs_insns, (int)c->fallback_insns - (int)c->zbs_insns,
		       c->zbs_ops);
		close(fd);
	}

	if (init_mode) {
		printf("END_BPF_JIT_ZBS_K3 rc=%d\n", failed ? 1 : 0);
		dump_jit_log();
		sync();
		reboot(RB_POWER_OFF);
		reboot(RB_AUTOBOOT);
	}

	return failed ? 1 : 0;
}
