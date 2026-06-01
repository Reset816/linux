// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/bpf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BPF_INSN_RAW(CODE, DST, SRC, OFF, IMM) \
	((struct bpf_insn){ .code = (CODE), .dst_reg = (DST), .src_reg = (SRC), .off = (OFF), .imm = (IMM) })
#define BPF_MOV64_IMM(DST, IMM) BPF_INSN_RAW(BPF_ALU64 | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define BPF_ADD64_REG(DST, SRC) BPF_INSN_RAW(BPF_ALU64 | BPF_ADD | BPF_X, DST, SRC, 0, 0)
#define BPF_XOR64_IMM(DST, IMM) BPF_INSN_RAW(BPF_ALU64 | BPF_XOR | BPF_K, DST, 0, 0, IMM)
#define BPF_RSH64_IMM(DST, IMM) BPF_INSN_RAW(BPF_ALU64 | BPF_RSH | BPF_K, DST, 0, 0, IMM)
#define BPF_MOV32_IMM(DST, IMM) BPF_INSN_RAW(BPF_ALU | BPF_MOV | BPF_K, DST, 0, 0, IMM)
#define BPF_RSH32_IMM(DST, IMM) BPF_INSN_RAW(BPF_ALU | BPF_RSH | BPF_K, DST, 0, 0, IMM)
#define BPF_EXIT_INSN() BPF_INSN_RAW(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

#define RV_INSN_LEN_MASK	0x3
#define RV_INSN_LEN_32		0x3
#define RVZBS_MASK		0xfc00707fU
#define RVZBS_BSETI		0x28001013U
#define RVZBS_BCLRI		0x48001013U

enum mode {
	MODE_DEDICATED,
	MODE_GENERAL,
	MODE_CONTROL,
	MODE_ALU32,
};

struct counts {
	uint32_t insns;
	uint32_t bytes;
	uint32_t bseti;
	uint32_t bclri;
};

static int sysctl_write(const char *path, const char *val, char *old, size_t old_sz)
{
	ssize_t len;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0)
		return -errno;

	len = read(fd, old, old_sz - 1);
	if (len < 0) {
		int err = -errno;

		close(fd);
		return err;
	}
	old[len] = '\0';

	if (pwrite(fd, val, strlen(val), 0) < 0) {
		int err = -errno;

		close(fd);
		return err;
	}

	close(fd);
	return 0;
}

static void sysctl_restore(const char *path, const char *old)
{
	int fd;

	if (!old[0])
		return;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return;
	pwrite(fd, old, strlen(old), 0);
	close(fd);
}

static long bpf_sys(enum bpf_cmd cmd, union bpf_attr *attr, unsigned int size)
{
	return syscall(__NR_bpf, cmd, attr, size);
}

static int load_prog(const struct bpf_insn *insns, size_t insn_cnt, char *log, size_t log_sz)
{
	union bpf_attr attr = {};
	long ret;

	attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
	attr.insn_cnt = insn_cnt;
	attr.insns = (uint64_t)(uintptr_t)insns;
	attr.license = (uint64_t)(uintptr_t)"GPL";
	attr.log_buf = (uint64_t)(uintptr_t)log;
	attr.log_size = log_sz;
	attr.log_level = 1;

	ret = bpf_sys(BPF_PROG_LOAD, &attr, sizeof(attr));
	if (ret < 0)
		return -errno;

	return ret;
}

static int prog_info(int fd, struct counts *counts)
{
	union bpf_attr attr = {};
	struct bpf_prog_info info = {};
	uint8_t *image;
	uint32_t len;
	long ret;

	attr.info.bpf_fd = fd;
	attr.info.info_len = sizeof(info);
	attr.info.info = (uint64_t)(uintptr_t)&info;
	ret = bpf_sys(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr));
	if (ret < 0)
		return -errno;

	len = info.jited_prog_len;
	counts->bytes = len;
	if (!len)
		return 0;

	image = calloc(1, len);
	if (!image)
		return -ENOMEM;

	memset(&attr, 0, sizeof(attr));
	memset(&info, 0, sizeof(info));
	attr.info.bpf_fd = fd;
	attr.info.info_len = sizeof(info);
	attr.info.info = (uint64_t)(uintptr_t)&info;
	info.jited_prog_insns = (uint64_t)(uintptr_t)image;
	info.jited_prog_len = len;
	ret = bpf_sys(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr));
	if (ret < 0) {
		int err = -errno;

		free(image);
		return err;
	}

	for (uint32_t pc = 0; pc + 2 <= len;) {
		uint16_t half = image[pc] | ((uint16_t)image[pc + 1] << 8);

		if ((half & RV_INSN_LEN_MASK) != RV_INSN_LEN_32) {
			pc += 2;
			counts->insns++;
			continue;
		}

		if (pc + 4 <= len) {
			uint32_t insn = image[pc] | ((uint32_t)image[pc + 1] << 8) |
					((uint32_t)image[pc + 2] << 16) |
					((uint32_t)image[pc + 3] << 24);

			if ((insn & RVZBS_MASK) == RVZBS_BSETI)
				counts->bseti++;
			else if ((insn & RVZBS_MASK) == RVZBS_BCLRI)
				counts->bclri++;
		}
		pc += 4;
		counts->insns++;
	}

	free(image);
	return 0;
}

static int run_prog(int fd, uint32_t repeat, uint32_t *retval)
{
	union bpf_attr attr = {};
	uint8_t pkt[64] = {};
	long ret;

	attr.test.prog_fd = fd;
	attr.test.data_in = (uint64_t)(uintptr_t)pkt;
	attr.test.data_size_in = sizeof(pkt);
	attr.test.repeat = repeat;

	ret = bpf_sys(BPF_PROG_TEST_RUN, &attr, sizeof(attr));
	if (ret < 0)
		return -errno;

	*retval = attr.test.retval;
	return 0;
}

static uint64_t nsec_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void emit_ld_imm64(struct bpf_insn **p, uint8_t dst, uint64_t imm)
{
	*(*p)++ = BPF_INSN_RAW(BPF_LD | BPF_DW | BPF_IMM, dst, 0, 0, (uint32_t)imm);
	*(*p)++ = BPF_INSN_RAW(0, 0, 0, 0, (uint32_t)(imm >> 32));
}

static size_t build_dedicated(struct bpf_insn *p, int reps)
{
	struct bpf_insn *start = p;

	*p++ = BPF_MOV64_IMM(0, 0);
	for (int i = 0; i < reps; i++) {
		emit_ld_imm64(&p, 1, 0x8000000000000000ULL);
		*p++ = BPF_RSH64_IMM(1, 63);
		*p++ = BPF_ADD64_REG(0, 1);
		emit_ld_imm64(&p, 1, 0x0000000000000800ULL);
		*p++ = BPF_RSH64_IMM(1, 11);
		*p++ = BPF_ADD64_REG(0, 1);
		emit_ld_imm64(&p, 1, 0x7fffffffffffffffULL);
		*p++ = BPF_RSH64_IMM(1, 62);
		*p++ = BPF_ADD64_REG(0, 1);
		emit_ld_imm64(&p, 1, 0xfffffeffffffffffULL);
		*p++ = BPF_XOR64_IMM(1, -1);
		*p++ = BPF_RSH64_IMM(1, 40);
		*p++ = BPF_ADD64_REG(0, 1);
	}
	*p++ = BPF_EXIT_INSN();

	return p - start;
}

static size_t build_general(struct bpf_insn *p, int reps)
{
	struct bpf_insn *start = p;

	*p++ = BPF_MOV64_IMM(0, 0);
	for (int i = 0; i < reps; i++) {
		emit_ld_imm64(&p, 1, 0x100000000ULL);
		*p++ = BPF_RSH64_IMM(1, 32);
		*p++ = BPF_ADD64_REG(0, 1);
		emit_ld_imm64(&p, 1, 0x400000000000ULL);
		*p++ = BPF_RSH64_IMM(1, 46);
		*p++ = BPF_ADD64_REG(0, 1);
		emit_ld_imm64(&p, 1, 0xffffffffffffff7fULL);
		*p++ = BPF_XOR64_IMM(1, -1);
		*p++ = BPF_RSH64_IMM(1, 7);
		*p++ = BPF_ADD64_REG(0, 1);
		emit_ld_imm64(&p, 1, 0xbadc0ffee0ddf00dULL);
		*p++ = BPF_RSH64_IMM(1, 63);
		*p++ = BPF_ADD64_REG(0, 1);
	}
	*p++ = BPF_EXIT_INSN();

	return p - start;
}

static size_t build_control(struct bpf_insn *p, int reps)
{
	struct bpf_insn *start = p;

	*p++ = BPF_MOV64_IMM(0, 0);
	for (int i = 0; i < reps; i++) {
		*p++ = BPF_MOV64_IMM(1, 7);
		*p++ = BPF_ADD64_REG(0, 1);
		*p++ = BPF_MOV64_IMM(1, 2047);
		*p++ = BPF_ADD64_REG(0, 1);
		emit_ld_imm64(&p, 1, 0x123456789abcdef0ULL);
		*p++ = BPF_RSH64_IMM(1, 63);
		*p++ = BPF_ADD64_REG(0, 1);
		emit_ld_imm64(&p, 1, 0x3333333333333333ULL);
		*p++ = BPF_RSH64_IMM(1, 63);
		*p++ = BPF_ADD64_REG(0, 1);
	}
	*p++ = BPF_EXIT_INSN();

	return p - start;
}

static size_t build_alu32(struct bpf_insn *p)
{
	struct bpf_insn *start = p;

	*p++ = BPF_MOV64_IMM(0, 0);
	emit_ld_imm64(&p, 1, 0x0000000080000000ULL);
	*p++ = BPF_RSH64_IMM(1, 31);
	*p++ = BPF_ADD64_REG(0, 1);
	*p++ = BPF_MOV32_IMM(1, 0x80000000U);
	*p++ = BPF_RSH32_IMM(1, 31);
	*p++ = BPF_ADD64_REG(0, 1);
	emit_ld_imm64(&p, 1, 0xffffffff7fffffffULL);
	*p++ = BPF_XOR64_IMM(1, -1);
	*p++ = BPF_RSH64_IMM(1, 31);
	*p++ = BPF_ADD64_REG(0, 1);
	*p++ = BPF_EXIT_INSN();

	return p - start;
}

static const char *mode_name(enum mode mode)
{
	switch (mode) {
	case MODE_DEDICATED:
		return "dedicated";
	case MODE_GENERAL:
		return "general";
	case MODE_CONTROL:
		return "control";
	case MODE_ALU32:
		return "alu32";
	}

	return "unknown";
}

static int run_mode(enum mode mode, int reps, uint32_t repeat, int rounds)
{
	struct bpf_insn insns[4096];
	char log[65536] = {};
	struct counts counts = {};
	uint32_t retval = 0;
	size_t insn_cnt;
	int err, fd;

	switch (mode) {
	case MODE_DEDICATED:
		insn_cnt = build_dedicated(insns, reps);
		break;
	case MODE_GENERAL:
		insn_cnt = build_general(insns, reps);
		break;
	case MODE_CONTROL:
		insn_cnt = build_control(insns, reps);
		break;
	case MODE_ALU32:
		insn_cnt = build_alu32(insns);
		break;
	default:
		return -EINVAL;
	}

	fd = load_prog(insns, insn_cnt, log, sizeof(log));
	if (fd < 0) {
		fprintf(stderr, "load %s failed: %s\n%s\n", mode_name(mode), strerror(-fd), log);
		return fd;
	}

	err = prog_info(fd, &counts);
	if (err)
		fprintf(stderr, "info %s failed: %s\n", mode_name(mode), strerror(-err));

	err = run_prog(fd, 1, &retval);
	if (err)
		fprintf(stderr, "run %s failed: %s\n", mode_name(mode), strerror(-err));

	printf("RESULT mode=%s bpf_insns=%zu expected_zbs_consts=%d retval=%u jited_bytes=%u jited_insns=%u bseti=%u bclri=%u\n",
	       mode_name(mode), insn_cnt,
	       mode == MODE_DEDICATED ? reps * 4 :
	       mode == MODE_GENERAL ? reps * 2 :
	       mode == MODE_ALU32 ? 2 : 0,
	       retval, counts.bytes, counts.insns, counts.bseti, counts.bclri);

	for (int i = 0; i < rounds; i++) {
		uint64_t t0, t1;

		t0 = nsec_now();
		err = run_prog(fd, repeat, &retval);
		t1 = nsec_now();
		if (err) {
			fprintf(stderr, "bench %s failed: %s\n", mode_name(mode), strerror(-err));
			close(fd);
			return err;
		}
		printf("TIME mode=%s round=%d repeat=%u retval=%u ns=%" PRIu64 " ns_per_run=%.3f\n",
		       mode_name(mode), i, repeat, retval, t1 - t0, (double)(t1 - t0) / repeat);
	}

	close(fd);
	return 0;
}

static enum mode parse_mode(const char *mode)
{
	if (!strcmp(mode, "dedicated"))
		return MODE_DEDICATED;
	if (!strcmp(mode, "general"))
		return MODE_GENERAL;
	if (!strcmp(mode, "control"))
		return MODE_CONTROL;
	if (!strcmp(mode, "alu32"))
		return MODE_ALU32;

	return MODE_DEDICATED;
}

int main(int argc, char **argv)
{
	uint32_t repeat = 200000;
	enum mode mode = MODE_DEDICATED;
	char old_harden[16] = {};
	char old_kptr[16] = {};
	bool keep_sysctls = false;
	int reps = 32;
	int rounds = 7;
	int err;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mode") && i + 1 < argc)
			mode = parse_mode(argv[++i]);
		else if (!strcmp(argv[i], "--reps") && i + 1 < argc)
			reps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--repeat") && i + 1 < argc)
			repeat = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
			rounds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--keep-sysctls"))
			keep_sysctls = true;
	}

	if (reps < 1)
		reps = 1;
	if (reps > 128)
		reps = 128;

	if (!keep_sysctls) {
		err = sysctl_write("/proc/sys/net/core/bpf_jit_harden", "0", old_harden, sizeof(old_harden));
		if (err)
			fprintf(stderr, "WARN: could not set bpf_jit_harden=0: %s\n", strerror(-err));
	}

	if (!keep_sysctls) {
		err = sysctl_write("/proc/sys/kernel/kptr_restrict", "0", old_kptr, sizeof(old_kptr));
		if (err)
			fprintf(stderr, "WARN: could not set kptr_restrict=0: %s\n", strerror(-err));
	}

	err = run_mode(mode, reps, repeat, rounds);

	sysctl_restore("/proc/sys/net/core/bpf_jit_harden", old_harden);
	sysctl_restore("/proc/sys/kernel/kptr_restrict", old_kptr);
	return err ? 1 : 0;
}
