#include <linux/string.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/kmsan-checks.h>

#define movs(type, to, from)                                             \
	do {                                                             \
		if ((type)[0] == 'b') {                                  \
			unsigned char *__to_b = (unsigned char *)(to);   \
			const unsigned char *__from_b =                  \
				(const unsigned char *)(from);           \
			__to_b[0] = __from_b[0];                         \
			(to) = (void *)&__to_b[1];                       \
			(from) = (const void *)&__from_b[1];             \
		} else if ((type)[0] == 'w') {                           \
			unsigned short *__to_w = (unsigned short *)(to); \
			const unsigned short *__from_w =                 \
				(const unsigned short *)(from);          \
			__to_w[0] = __from_w[0];                         \
			(to) = (void *)&__to_w[1];                       \
			(from) = (const void *)&__from_w[1];             \
		} else {                                                 \
			unsigned long *__to_l = (unsigned long *)(to);   \
			const unsigned long *__from_l =                  \
				(const unsigned long *)(from);           \
			__to_l[0] = __from_l[0];                         \
			(to) = (void *)&__to_l[1];                       \
			(from) = (const void *)&__from_l[1];             \
		}                                                        \
	} while (0)

/* Originally from i386/string.h */
static __always_inline void rep_movs(void *to, const void *from, size_t n)
{
	unsigned long d0, d1, d2;
	unsigned char *to_bytes = (unsigned char *)to;
	const unsigned char *from_bytes = (const unsigned char *)from;
	size_t i;
	for (i = 0; i < n; i++) {
		to_bytes[i] = from_bytes[i];
	}
	d0 = 0;
	d1 = (unsigned long)&to_bytes[n];
	d2 = (unsigned long)&from_bytes[n];
}

static void string_memcpy_fromio(void *to, const volatile void __iomem *from, size_t n)
{
	if (unlikely(!n))
		return;

	/* Align any unaligned source IO */
	if (unlikely(1 & (unsigned long)from)) {
		movs("b", to, from);
		n--;
	}
	if (n > 1 && unlikely(2 & (unsigned long)from)) {
		movs("w", to, from);
		n-=2;
	}
	rep_movs(to, (const void *)from, n);
	/* KMSAN must treat values read from devices as initialized. */
	kmsan_unpoison_memory(to, n);
}

static void string_memcpy_toio(volatile void __iomem *to, const void *from, size_t n)
{
	if (unlikely(!n))
		return;

	/* Make sure uninitialized memory isn't copied to devices. */
	kmsan_check_memory(from, n);
	/* Align any unaligned destination IO */
	if (unlikely(1 & (unsigned long)to)) {
		movs("b", to, from);
		n--;
	}
	if (n > 1 && unlikely(2 & (unsigned long)to)) {
		movs("w", to, from);
		n-=2;
	}
	rep_movs((void *)to, (const void *) from, n);
}

static void unrolled_memcpy_fromio(void *to, const volatile void __iomem *from, size_t n)
{
	const volatile char __iomem *in = from;
	char *out = to;
	int i;

	for (i = 0; i < n; ++i)
		out[i] = readb(&in[i]);
}

static void unrolled_memcpy_toio(volatile void __iomem *to, const void *from, size_t n)
{
	volatile char __iomem *out = to;
	const char *in = from;
	int i;

	for (i = 0; i < n; ++i)
		writeb(in[i], &out[i]);
}

static void unrolled_memset_io(volatile void __iomem *a, int b, size_t c)
{
	volatile char __iomem *mem = a;
	int i;

	for (i = 0; i < c; ++i)
		writeb(b, &mem[i]);
}

void memcpy_fromio(void *to, const volatile void __iomem *from, size_t n)
{
	if (cc_platform_has(CC_ATTR_GUEST_UNROLL_STRING_IO))
		unrolled_memcpy_fromio(to, from, n);
	else
		string_memcpy_fromio(to, from, n);
}
EXPORT_SYMBOL(memcpy_fromio);

void memcpy_toio(volatile void __iomem *to, const void *from, size_t n)
{
	if (cc_platform_has(CC_ATTR_GUEST_UNROLL_STRING_IO))
		unrolled_memcpy_toio(to, from, n);
	else
		string_memcpy_toio(to, from, n);
}
EXPORT_SYMBOL(memcpy_toio);

void memset_io(volatile void __iomem *a, int b, size_t c)
{
	if (cc_platform_has(CC_ATTR_GUEST_UNROLL_STRING_IO)) {
		unrolled_memset_io(a, b, c);
	} else {
		/*
		 * TODO: memset can mangle the IO patterns quite a bit.
		 * perhaps it would be better to use a dumb one:
		 */
		memset((void *)a, b, c);
	}
}
EXPORT_SYMBOL(memset_io);
