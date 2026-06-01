// SPDX-License-Identifier: GPL-2.0

#include <linux/bpf.h>
#include <stdbool.h>
#include <bpf/bpf_helpers.h>
#include "bpf_misc.h"

extern bool CONFIG_RISCV_ISA_ZBS __kconfig __weak;

/* Force CONFIG_RISCV_ISA_ZBS into BTF so the userspace test can inspect it. */
int unused(void)
{
	return CONFIG_RISCV_ISA_ZBS ? 0 : 1;
}

SEC("socket")
__naked int emit_imm_zbs(void)
{
	asm volatile ("r0 = 0x8000000000000000 ll;"
		      "r0 >>= 63;"
		      "r1 = 0x800;"
		      "r1 >>= 11;"
		      "r0 += r1;"
		      "r1 = 0x7fffffffffffffff ll;"
		      "r1 >>= 62;"
		      "r0 += r1;"
		      "r1 = 0xfffffeffffffffff ll;"
		      "r1 ^= -1;"
		      "r1 >>= 40;"
		      "r0 += r1;"
		      "exit;"
		      ::: __clobber_all);
}

char _license[] SEC("license") = "GPL";
