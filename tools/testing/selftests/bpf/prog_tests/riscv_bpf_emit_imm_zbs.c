// SPDX-License-Identifier: GPL-2.0

#include <test_progs.h>

#include "riscv_bpf_emit_imm_zbs.skel.h"

#if defined(__riscv) && __riscv_xlen == 64
#include <asm/hwprobe.h>
#include <asm/unistd.h>
#include <fcntl.h>
#include <network_helpers.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_riscv_hwprobe
#define __NR_riscv_hwprobe 258
#endif

#define RV_INSN_LEN_MASK	0x3
#define RV_INSN_LEN_32		0x3
#define RVZBS_MASK		0xfc00707f
#define RVZBS_BSETI		0x28001013
#define RVZBS_BCLRI		0x48001013

struct zbs_counts {
	int bseti;
	int bclri;
};

static __u16 get_u16(const __u8 *p)
{
	return p[0] | ((__u16)p[1] << 8);
}

static __u32 get_u32(const __u8 *p)
{
	return p[0] | ((__u32)p[1] << 8) | ((__u32)p[2] << 16) |
	       ((__u32)p[3] << 24);
}

static void count_zbs_insns(const __u8 *image, __u32 len, struct zbs_counts *counts)
{
	__u32 pc = 0;

	while (pc + 2 <= len) {
		if ((get_u16(image + pc) & RV_INSN_LEN_MASK) != RV_INSN_LEN_32) {
			pc += 2;
			continue;
		}

		if (pc + 4 <= len) {
			__u32 insn = get_u32(image + pc);

			if ((insn & RVZBS_MASK) == RVZBS_BSETI)
				counts->bseti++;
			else if ((insn & RVZBS_MASK) == RVZBS_BCLRI)
				counts->bclri++;
		}
		pc += 4;
	}
}

static int get_zbs_counts(int fd, struct zbs_counts *counts)
{
	struct bpf_prog_info info = {};
	__u32 info_len = sizeof(info);
	__u32 len, nfuncs, pc = 0;
	__u32 *func_lens = NULL;
	__u8 *image = NULL;
	int err, i;

	err = bpf_prog_get_info_by_fd(fd, &info, &info_len);
	if (!ASSERT_OK(err, "bpf_prog_get_info_by_fd #1"))
		return err;

	len = info.jited_prog_len;
	if (!len) {
		test__skip();
		return -ENOTSUP;
	}

	image = malloc(len);
	if (!ASSERT_OK_PTR(image, "malloc jited image"))
		return -ENOMEM;

	nfuncs = info.nr_jited_func_lens ?: 1;
	func_lens = calloc(nfuncs, sizeof(*func_lens));
	if (!ASSERT_OK_PTR(func_lens, "calloc jited func lens")) {
		err = -ENOMEM;
		goto out;
	}

	memset(&info, 0, sizeof(info));
	info_len = sizeof(info);
	info.jited_prog_insns = (__u64)(unsigned long)image;
	info.jited_prog_len = len;
	info.jited_func_lens = (__u64)(unsigned long)func_lens;
	info.nr_jited_func_lens = nfuncs;
	err = bpf_prog_get_info_by_fd(fd, &info, &info_len);
	if (!ASSERT_OK(err, "bpf_prog_get_info_by_fd #2"))
		goto out;

	if (!info.nr_jited_func_lens) {
		count_zbs_insns(image, len, counts);
		goto out;
	}

	for (i = 0; i < info.nr_jited_func_lens && pc < len; i++) {
		__u32 func_len = func_lens[i];

		if (pc + func_len > len)
			func_len = len - pc;
		count_zbs_insns(image + pc, func_len, counts);
		pc += func_len;
	}

out:
	free(func_lens);
	free(image);
	return err;
}

static int set_jit_harden(int value, char *old, size_t old_sz)
{
	int fd, err = 0;
	ssize_t len;
	char new[4];

	fd = open("/proc/sys/net/core/bpf_jit_harden", O_RDWR);
	if (fd < 0)
		return -errno;

	len = read(fd, old, old_sz - 1);
	if (len < 0) {
		err = -errno;
		goto out;
	}
	old[len] = '\0';

	snprintf(new, sizeof(new), "%d", value);
	if (pwrite(fd, new, strlen(new), 0) < 0)
		err = -errno;

out:
	close(fd);
	return err;
}

static void restore_jit_harden(const char *old)
{
	int fd;

	if (!old[0])
		return;

	fd = open("/proc/sys/net/core/bpf_jit_harden", O_WRONLY);
	if (fd < 0)
		return;
	pwrite(fd, old, strlen(old), 0);
	close(fd);
}

static int hwprobe_has_zbs(bool *has_zbs)
{
	struct riscv_hwprobe pair = {
		.key = RISCV_HWPROBE_KEY_IMA_EXT_0,
	};
	long ret;

	ret = syscall(__NR_riscv_hwprobe, &pair, 1, 0, NULL, 0);
	if (ret)
		return -errno;
	if (pair.key != RISCV_HWPROBE_KEY_IMA_EXT_0)
		return -EOPNOTSUPP;

	*has_zbs = pair.value & RISCV_HWPROBE_EXT_ZBS;
	return 0;
}

#endif /* defined(__riscv) && __riscv_xlen == 64 */

void test_riscv_bpf_emit_imm_zbs(void)
{
#if defined(__riscv) && __riscv_xlen == 64
	LIBBPF_OPTS(bpf_test_run_opts, opts,
		    .data_in = &pkt_v4,
		    .data_size_in = sizeof(pkt_v4),
		    .repeat = 1,
	);
	struct riscv_bpf_emit_imm_zbs *skel;
	struct zbs_counts counts = {};
	char old_harden[8] = {};
	bool config_zbs = false;
	bool zbs_enabled;
	bool has_zbs = false;
	int err, fd;

	err = hwprobe_has_zbs(&has_zbs);
	if (err) {
		test__skip();
		return;
	}

	err = set_jit_harden(0, old_harden, sizeof(old_harden));
	if (err) {
		test__skip();
		return;
	}

	skel = riscv_bpf_emit_imm_zbs__open();
	if (!ASSERT_OK_PTR(skel, "riscv_bpf_emit_imm_zbs__open"))
		goto restore_harden;

	err = bpf_program__set_flags(skel->progs.emit_imm_zbs,
				     bpf_program__flags(skel->progs.emit_imm_zbs) &
				     ~BPF_F_TEST_RND_HI32);
	if (!ASSERT_OK(err, "bpf_program__set_flags")) {
		riscv_bpf_emit_imm_zbs__destroy(skel);
		goto restore_harden;
	}

	err = riscv_bpf_emit_imm_zbs__load(skel);
	if (!ASSERT_OK(err, "riscv_bpf_emit_imm_zbs__load")) {
		riscv_bpf_emit_imm_zbs__destroy(skel);
		goto restore_harden;
	}
	config_zbs = skel->kconfig->CONFIG_RISCV_ISA_ZBS;

	fd = bpf_program__fd(skel->progs.emit_imm_zbs);
	err = bpf_prog_test_run_opts(fd, &opts);
	if (!ASSERT_OK(err, "bpf_prog_test_run_opts"))
		goto out;
	if (!ASSERT_EQ(opts.retval, 4, "retval"))
		goto out;

	err = get_zbs_counts(fd, &counts);
	if (err)
		goto out;

	zbs_enabled = has_zbs && config_zbs;
	if (zbs_enabled) {
		ASSERT_GE(counts.bseti, 2, "bseti count");
		ASSERT_GE(counts.bclri, 2, "bclri count");
	} else {
		ASSERT_EQ(counts.bseti, 0, "fallback bseti count");
		ASSERT_EQ(counts.bclri, 0, "fallback bclri count");
	}

out:
	riscv_bpf_emit_imm_zbs__destroy(skel);
restore_harden:
	restore_jit_harden(old_harden);
#else
	test__skip();
#endif
}
