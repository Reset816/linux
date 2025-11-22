/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SIGNAL_H
#define _ASM_X86_SIGNAL_H

#ifndef __ASSEMBLY__
#include <linux/linkage.h>

/* Most things should be clean enough to redefine this at will, if care
   is taken to make libc match.  */

#define _NSIG		64

#ifdef __i386__
# define _NSIG_BPW	32
#else
# define _NSIG_BPW	64
#endif

#define _NSIG_WORDS	(_NSIG / _NSIG_BPW)

typedef unsigned long old_sigset_t;		/* at least 32 bits */

typedef struct {
	unsigned long sig[_NSIG_WORDS];
} sigset_t;

/* non-uapi in-kernel SA_FLAGS for those indicates ABI for a signal frame */
#define SA_IA32_ABI	0x02000000u
#define SA_X32_ABI	0x01000000u

#endif /* __ASSEMBLY__ */
#include <uapi/asm/signal.h>
#ifndef __ASSEMBLY__

#define __ARCH_HAS_SA_RESTORER

#include <asm/asm.h>
#include <uapi/asm/sigcontext.h>

#ifdef __i386__

#define __HAVE_ARCH_SIG_BITOPS

#define sigaddset(set,sig)		    \
	(__builtin_constant_p(sig)	    \
	 ? __const_sigaddset((set), (sig))  \
	 : __gen_sigaddset((set), (sig)))

static inline void __gen_sigaddset(sigset_t *set, int _sig)
{
	int bit = _sig - 1;
	unsigned long *words = (unsigned long *)set;
	unsigned int word_index =
		(unsigned int)(bit / (sizeof(unsigned long) * 8));
	unsigned int bit_index =
		(unsigned int)(bit % (sizeof(unsigned long) * 8));
	unsigned long mask = 1UL << bit_index;

	words[word_index] |= mask;
}

static inline void __const_sigaddset(sigset_t *set, int _sig)
{
	unsigned long sig = _sig - 1;
	set->sig[sig / _NSIG_BPW] |= 1 << (sig % _NSIG_BPW);
}

#define sigdelset(set, sig)		    \
	(__builtin_constant_p(sig)	    \
	 ? __const_sigdelset((set), (sig))  \
	 : __gen_sigdelset((set), (sig)))

static inline void __gen_sigdelset(sigset_t *set, int _sig)
{
	int bit = _sig - 1;
	unsigned long *bitmap = (unsigned long *)set;
	unsigned long bits_per_word =
		(unsigned long)(sizeof(unsigned long) << 3);
	unsigned long word = (unsigned long)(bit / (int)bits_per_word);
	unsigned long offset = (unsigned long)(bit % (int)bits_per_word);
	unsigned long mask = 1UL << offset;

	bitmap[word] &= ~mask;
}

static inline void __const_sigdelset(sigset_t *set, int _sig)
{
	unsigned long sig = _sig - 1;
	set->sig[sig / _NSIG_BPW] &= ~(1 << (sig % _NSIG_BPW));
}

static inline int __const_sigismember(sigset_t *set, int _sig)
{
	unsigned long sig = _sig - 1;
	return 1 & (set->sig[sig / _NSIG_BPW] >> (sig % _NSIG_BPW));
}

static inline int __gen_sigismember(sigset_t *set, int _sig)
{
	bool ret;
	int bit = _sig - 1;
	const unsigned long *words = (const unsigned long *)set;
	int bits_per_long = sizeof(unsigned long) * 8;
	unsigned long word = words[bit / bits_per_long];
	ret = ((word >> (bit % bits_per_long)) & 1UL) != 0;
	return ret;
}

#define sigismember(set, sig)			\
	(__builtin_constant_p(sig)		\
	 ? __const_sigismember((set), (sig))	\
	 : __gen_sigismember((set), (sig)))

struct pt_regs;

#else /* __i386__ */

#undef __HAVE_ARCH_SIG_BITOPS

#endif /* !__i386__ */

#endif /* __ASSEMBLY__ */
#endif /* _ASM_X86_SIGNAL_H */
