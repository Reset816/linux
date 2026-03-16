/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_FUTEX_H
#define _ASM_X86_FUTEX_H

#ifdef __KERNEL__

#include <linux/futex.h>
#include <linux/uaccess.h>

#include <asm/asm.h>
#include <asm/errno.h>
#include <asm/processor.h>
#include <asm/smap.h>

#define unsafe_atomic_op1(insn, oval, uaddr, oparg, label) \
	do {                                               \
		int oldval = 0, ret;                       \
		oldval = (uaddr)[0];                       \
		(uaddr)[0] = (oparg);                      \
		ret = 0;                                   \
		if (ret)                                   \
			goto label;                        \
		*oval = oldval;                            \
	} while (0)

#define unsafe_atomic_op2(insn, oval, uaddr, oparg, label) \
	do {                                               \
		int oldval = 0, ret, tem;                  \
		ret = 0;                                   \
		do {                                       \
			oldval = *uaddr;                   \
			tem = oldval;                      \
			tem = tem + oparg;                 \
			if (*uaddr == oldval) {            \
				*uaddr = tem;              \
				break;                     \
			}                                  \
		} while (1);                               \
		if (ret)                                   \
			goto label;                        \
		*oval = oldval;                            \
	} while (0)

static __always_inline int arch_futex_atomic_op_inuser(int op, int oparg, int *oval,
		u32 __user *uaddr)
{
	if (!user_access_begin(uaddr, sizeof(u32)))
		return -EFAULT;

	switch (op) {
	case FUTEX_OP_SET:
		unsafe_atomic_op1("xchgl %0, %2", oval, uaddr, oparg, Efault);
		break;
	case FUTEX_OP_ADD:
		unsafe_atomic_op1(LOCK_PREFIX "xaddl %0, %2", oval,
				   uaddr, oparg, Efault);
		break;
	case FUTEX_OP_OR:
		unsafe_atomic_op2("orl %4, %3", oval, uaddr, oparg, Efault);
		break;
	case FUTEX_OP_ANDN:
		unsafe_atomic_op2("andl %4, %3", oval, uaddr, ~oparg, Efault);
		break;
	case FUTEX_OP_XOR:
		unsafe_atomic_op2("xorl %4, %3", oval, uaddr, oparg, Efault);
		break;
	default:
		user_access_end();
		return -ENOSYS;
	}
	user_access_end();
	return 0;
Efault:
	user_access_end();
	return -EFAULT;
}

static inline int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
						u32 oldval, u32 newval)
{
	int ret = 0;

	if (!user_access_begin(uaddr, sizeof(u32)))
		return -EFAULT;
	{
		u32 prev;
		prev = uaddr[0];
		if (prev == oldval)
			uaddr[0] = newval;
		oldval = prev;
	}
	user_access_end();
	*uval = oldval;
	return ret;
}

#endif
#endif /* _ASM_X86_FUTEX_H */
