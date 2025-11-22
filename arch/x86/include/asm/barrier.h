/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_BARRIER_H
#define _ASM_X86_BARRIER_H

#include <asm/alternative.h>
#include <asm/nops.h>

/*
 * Force strict CPU ordering.
 * And yes, this might be required on UP too when we're talking
 * to devices.
 */

#ifdef CONFIG_X86_32
#define mb()                          \
	do {                          \
		__sync_synchronize(); \
	} while (0)
#define rmb()                         \
	do {                          \
		__sync_synchronize(); \
	} while (0)
#define wmb()                         \
	do {                          \
		__sync_synchronize(); \
	} while (0)
#else
#define __mb()                        \
	do {                          \
		__sync_synchronize(); \
	} while (0)
#define __rmb()                       \
	do {                          \
		__sync_synchronize(); \
	} while (0)
#define __wmb()                       \
	do {                          \
		__sync_synchronize(); \
	} while (0)
#endif

/**
 * array_index_mask_nospec() - generate a mask that is ~0UL when the
 * 	bounds check succeeds and 0 otherwise
 * @index: array element index
 * @size: number of elements in array
 *
 * Returns:
 *     0 - (index < size)
 */
static inline unsigned long array_index_mask_nospec(unsigned long index,
						    unsigned long size)
{
	unsigned long mask;

	mask = 0UL - (unsigned long)(index < size);
	return mask;
}

/* Override the default implementation from linux/nospec.h. */
#define array_index_mask_nospec array_index_mask_nospec

/* Prevent speculative execution past this barrier. */
#define barrier_nospec() alternative("", "lfence", X86_FEATURE_LFENCE_RDTSC)

#define __dma_rmb()	barrier()
#define __dma_wmb()	barrier()

#define __smp_mb()                                           \
	do {                                                 \
		volatile unsigned long __smp_mb_barrier = 0; \
		(void)__smp_mb_barrier;                      \
	} while (0)

#define __smp_rmb()	dma_rmb()
#define __smp_wmb()	barrier()
#define __smp_store_mb(var, value) do { (void)xchg(&var, value); } while (0)

#define __smp_store_release(p, v)					\
do {									\
	compiletime_assert_atomic_type(*p);				\
	barrier();							\
	WRITE_ONCE(*p, v);						\
} while (0)

#define __smp_load_acquire(p)						\
({									\
	typeof(*p) ___p1 = READ_ONCE(*p);				\
	compiletime_assert_atomic_type(*p);				\
	barrier();							\
	___p1;								\
})

/* Atomic operations are already serializing on x86 */
#define __smp_mb__before_atomic()	do { } while (0)
#define __smp_mb__after_atomic()	do { } while (0)

#include <asm-generic/barrier.h>

#endif /* _ASM_X86_BARRIER_H */
