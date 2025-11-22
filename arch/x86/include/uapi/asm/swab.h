/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_X86_SWAB_H
#define _ASM_X86_SWAB_H

#include <linux/types.h>
#include <linux/compiler.h>

static inline __attribute_const__ __u32 __arch_swab32(__u32 val)
{
	val = ((val & (__u32)0x000000ffU) << 24) |
	      ((val & (__u32)0x0000ff00U) << 8) |
	      ((val & (__u32)0x00ff0000U) >> 8) |
	      ((val & (__u32)0xff000000U) >> 24);
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
	__u32 orig_a;
	__u32 orig_b;
	__u32 swapped_a;
	__u32 swapped_b;

	v.u = val;
	orig_a = v.s.a;
	orig_b = v.s.b;
	swapped_a =
		((orig_a & 0x000000ffU) << 24) | ((orig_a & 0x0000ff00U) << 8) |
		((orig_a & 0x00ff0000U) >> 8) | ((orig_a & 0xff000000U) >> 24);
	swapped_b =
		((orig_b & 0x000000ffU) << 24) | ((orig_b & 0x0000ff00U) << 8) |
		((orig_b & 0x00ff0000U) >> 8) | ((orig_b & 0xff000000U) >> 24);
	v.s.a = swapped_b;
	v.s.b = swapped_a;
	return v.u;
#else /* __i386__ */
	val = ((val & 0x00000000000000ffULL) << 56) |
	      ((val & 0x000000000000ff00ULL) << 40) |
	      ((val & 0x0000000000ff0000ULL) << 24) |
	      ((val & 0x00000000ff000000ULL) << 8) |
	      ((val & 0x000000ff00000000ULL) >> 8) |
	      ((val & 0x0000ff0000000000ULL) >> 24) |
	      ((val & 0x00ff000000000000ULL) >> 40) |
	      ((val & 0xff00000000000000ULL) >> 56);
	return val;
#endif
}
#define __arch_swab64 __arch_swab64

#endif /* _ASM_X86_SWAB_H */
