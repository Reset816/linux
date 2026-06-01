// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/cache.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/linkage.h>
#include <linux/sysfs.h>

#include <asm/simd.h>
#include <asm/vector.h>

#define RISCV_ISA_V_MEMCPY_THRESHOLD	2048

asmlinkage void *__memcpy_scalar(void *dst, const void *src, size_t n);
void __asm_vector_memcpy(void *dst, const void *src, size_t n);

static bool riscv_v_memcpy_disabled __read_mostly;
static unsigned long riscv_v_memcpy_calls;
static unsigned long riscv_v_memcpy_bytes;

static int __init riscv_v_memcpy_disable(char *str)
{
	riscv_v_memcpy_disabled = true;

	return 1;
}
__setup("riscv_v_memcpy_disable", riscv_v_memcpy_disable);

static bool riscv_v_memcpy_no_overlap(const void *dst, const void *src,
				      size_t n)
{
	unsigned long d = (unsigned long)dst;
	unsigned long s = (unsigned long)src;

	if (d <= s)
		return n <= s - d;

	return n <= d - s;
}

static ssize_t memcpy_threshold_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", RISCV_ISA_V_MEMCPY_THRESHOLD);
}

static ssize_t memcpy_disabled_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", riscv_v_memcpy_disabled);
}

static ssize_t memcpy_calls_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(riscv_v_memcpy_calls));
}

static ssize_t memcpy_bytes_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(riscv_v_memcpy_bytes));
}

static struct kobj_attribute memcpy_threshold_attr =
	__ATTR_RO(memcpy_threshold);
static struct kobj_attribute memcpy_disabled_attr =
	__ATTR_RO(memcpy_disabled);
static struct kobj_attribute memcpy_calls_attr =
	__ATTR_RO(memcpy_calls);
static struct kobj_attribute memcpy_bytes_attr =
	__ATTR_RO(memcpy_bytes);

static struct attribute *riscv_v_memcpy_attrs[] = {
	&memcpy_threshold_attr.attr,
	&memcpy_disabled_attr.attr,
	&memcpy_calls_attr.attr,
	&memcpy_bytes_attr.attr,
	NULL,
};

static const struct attribute_group riscv_v_memcpy_attr_group = {
	.name = "riscv_v_memcpy",
	.attrs = riscv_v_memcpy_attrs,
};

static int __init riscv_v_memcpy_sysfs_init(void)
{
	return sysfs_create_group(kernel_kobj, &riscv_v_memcpy_attr_group);
}
late_initcall(riscv_v_memcpy_sysfs_init);

void *__memcpy(void *dst, const void *src, size_t n)
{
	if (IS_ENABLED(CONFIG_64BIT) &&
	    n >= RISCV_ISA_V_MEMCPY_THRESHOLD &&
	    likely(system_state == SYSTEM_RUNNING) &&
	    !riscv_v_memcpy_disabled &&
	    riscv_v_memcpy_no_overlap(dst, src, n) &&
	    has_vector() &&
	    may_use_simd()) {
		kernel_vector_begin();
		__asm_vector_memcpy(dst, src, n);
		kernel_vector_end();

		WRITE_ONCE(riscv_v_memcpy_calls, riscv_v_memcpy_calls + 1);
		WRITE_ONCE(riscv_v_memcpy_bytes, riscv_v_memcpy_bytes + n);

		return dst;
	}

	return __memcpy_scalar(dst, src, n);
}
#undef memcpy
void *memcpy(void *dst, const void *src, size_t n) __weak __alias(__memcpy);
