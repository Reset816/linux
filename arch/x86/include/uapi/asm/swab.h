/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_X86_SWAB_H
#define _ASM_X86_SWAB_H

#include <linux/types.h>
#include <linux/compiler.h>

static inline __attribute_const__ __u32 __arch_swab32(__u32 val)
{
	asm("bswapl %0" : "=r" (val) : "0" (val));
	return val;
}
#define __arch_swab32 __arch_swab32

static inline __attribute_const__ __u64 __arch_swab64(__u64 val)
{
#ifdef __i386__
	union {
		struct {
			__u32 a;
			__u32 b;
		} s;
		__u64 u;
	} v;
	v.u = val;
	{
		__u32 a = v.s.a;
		__u32 b = v.s.b;
		__u32 a_swab =
			((a & 0x000000ffU) << 24) | ((a & 0x0000ff00U) << 8) |
			((a & 0x00ff0000U) >> 8) | ((a & 0xff000000U) >> 24);
		__u32 b_swab =
			((b & 0x000000ffU) << 24) | ((b & 0x0000ff00U) << 8) |
			((b & 0x00ff0000U) >> 8) | ((b & 0xff000000U) >> 24);
		v.s.a = b_swab;
		v.s.b = a_swab;
	}
	return v.u;
#else /* __i386__ */
	{
		__u64 x = val;
		__u64 y = 0;
		y |= (x & 0x00000000000000ffULL) << 56;
		y |= (x & 0x000000000000ff00ULL) << 40;
		y |= (x & 0x0000000000ff0000ULL) << 24;
		y |= (x & 0x00000000ff000000ULL) << 8;
		y |= (x & 0x000000ff00000000ULL) >> 8;
		y |= (x & 0x0000ff0000000000ULL) >> 24;
		y |= (x & 0x00ff000000000000ULL) >> 40;
		y |= (x & 0xff00000000000000ULL) >> 56;
		val = y;
	}
	return val;
#endif
}
#define __arch_swab64 __arch_swab64

#endif /* _ASM_X86_SWAB_H */
