// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/cache.h>
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/linkage.h>
#include <linux/sysfs.h>

#include <asm/simd.h>
#include <asm/vector.h>

#define RISCV_ISA_V_MEMCPY_THRESHOLD	2048
#define RISCV_ISA_V_MEMCPY_PROFILE_TOP	8

asmlinkage void *__memcpy_scalar(void *dst, const void *src, size_t n);
void __asm_vector_memcpy(void *dst, const void *src, size_t n);

static bool riscv_v_memcpy_disabled __read_mostly;
static unsigned long riscv_v_memcpy_calls;
static unsigned long riscv_v_memcpy_bytes;
static bool riscv_v_memcpy_profile_requested __read_mostly;
static DEFINE_STATIC_KEY_FALSE(riscv_v_memcpy_profile_key);

enum riscv_v_memcpy_bucket {
	RISCV_V_MEMCPY_BUCKET_LT128,
	RISCV_V_MEMCPY_BUCKET_128,
	RISCV_V_MEMCPY_BUCKET_256,
	RISCV_V_MEMCPY_BUCKET_512,
	RISCV_V_MEMCPY_BUCKET_1024,
	RISCV_V_MEMCPY_BUCKET_2048,
	RISCV_V_MEMCPY_BUCKET_4096,
	RISCV_V_MEMCPY_BUCKET_8192,
	RISCV_V_MEMCPY_BUCKET_16K,
	RISCV_V_MEMCPY_BUCKET_64K,
	RISCV_V_MEMCPY_BUCKET_NR,
};

enum riscv_v_memcpy_reason {
	RISCV_V_MEMCPY_REASON_VECTOR,
	RISCV_V_MEMCPY_REASON_SMALL,
	RISCV_V_MEMCPY_REASON_NOT_RUNNING,
	RISCV_V_MEMCPY_REASON_DISABLED,
	RISCV_V_MEMCPY_REASON_OVERLAP,
	RISCV_V_MEMCPY_REASON_NO_VECTOR,
	RISCV_V_MEMCPY_REASON_DISALLOWED,
	RISCV_V_MEMCPY_REASON_CONFIG,
	RISCV_V_MEMCPY_REASON_OTHER,
	RISCV_V_MEMCPY_REASON_NR,
};

enum riscv_v_memcpy_align {
	RISCV_V_MEMCPY_ALIGN_BOTH_16,
	RISCV_V_MEMCPY_ALIGN_DST_16,
	RISCV_V_MEMCPY_ALIGN_SRC_16,
	RISCV_V_MEMCPY_ALIGN_NEITHER_16,
	RISCV_V_MEMCPY_ALIGN_NR,
};

struct riscv_v_memcpy_counter {
	atomic64_t calls;
	atomic64_t bytes;
};

static struct riscv_v_memcpy_counter riscv_v_memcpy_profile_total;
static struct riscv_v_memcpy_counter
	riscv_v_memcpy_profile_reason[RISCV_V_MEMCPY_REASON_NR];
static struct riscv_v_memcpy_counter
	riscv_v_memcpy_profile_bucket[RISCV_V_MEMCPY_BUCKET_NR];
static struct riscv_v_memcpy_counter
	riscv_v_memcpy_profile_vector_bucket[RISCV_V_MEMCPY_BUCKET_NR];
static struct riscv_v_memcpy_counter
	riscv_v_memcpy_profile_align[RISCV_V_MEMCPY_ALIGN_NR];
static struct riscv_v_memcpy_counter riscv_v_memcpy_profile_nonoverlap;
static struct riscv_v_memcpy_counter riscv_v_memcpy_profile_overlap;

static const char * const riscv_v_memcpy_bucket_name[] = {
	"<128",
	"128-255",
	"256-511",
	"512-1023",
	"1024-2047",
	"2048-4095",
	"4096-8191",
	"8192-16383",
	"16384-65535",
	">=65536",
};

static const char * const riscv_v_memcpy_reason_name[] = {
	"vector",
	"small",
	"not_running",
	"disabled",
	"overlap",
	"no_vector",
	"disallowed",
	"config",
	"other",
};

static const char * const riscv_v_memcpy_align_name[] = {
	"both_16",
	"dst_16",
	"src_16",
	"neither_16",
};

static int __init riscv_v_memcpy_disable(char *str)
{
	riscv_v_memcpy_disabled = true;

	return 1;
}
__setup("riscv_v_memcpy_disable", riscv_v_memcpy_disable);

static int __init riscv_v_memcpy_profile(char *str)
{
	riscv_v_memcpy_profile_requested = true;

	return 1;
}
__setup("riscv_v_memcpy_profile", riscv_v_memcpy_profile);

static bool riscv_v_memcpy_no_overlap(const void *dst, const void *src,
				      size_t n)
{
	unsigned long d = (unsigned long)dst;
	unsigned long s = (unsigned long)src;

	if (d <= s)
		return n <= s - d;

	return n <= d - s;
}

static enum riscv_v_memcpy_bucket riscv_v_memcpy_bucket(size_t n)
{
	if (n < 128)
		return RISCV_V_MEMCPY_BUCKET_LT128;
	if (n < 256)
		return RISCV_V_MEMCPY_BUCKET_128;
	if (n < 512)
		return RISCV_V_MEMCPY_BUCKET_256;
	if (n < 1024)
		return RISCV_V_MEMCPY_BUCKET_512;
	if (n < 2048)
		return RISCV_V_MEMCPY_BUCKET_1024;
	if (n < 4096)
		return RISCV_V_MEMCPY_BUCKET_2048;
	if (n < 8192)
		return RISCV_V_MEMCPY_BUCKET_4096;
	if (n < 16384)
		return RISCV_V_MEMCPY_BUCKET_8192;
	if (n < 65536)
		return RISCV_V_MEMCPY_BUCKET_16K;

	return RISCV_V_MEMCPY_BUCKET_64K;
}

static enum riscv_v_memcpy_align riscv_v_memcpy_align(const void *dst,
						      const void *src)
{
	bool dst_aligned = !((unsigned long)dst & 15);
	bool src_aligned = !((unsigned long)src & 15);

	if (dst_aligned && src_aligned)
		return RISCV_V_MEMCPY_ALIGN_BOTH_16;
	if (dst_aligned)
		return RISCV_V_MEMCPY_ALIGN_DST_16;
	if (src_aligned)
		return RISCV_V_MEMCPY_ALIGN_SRC_16;

	return RISCV_V_MEMCPY_ALIGN_NEITHER_16;
}

static void riscv_v_memcpy_counter_add(struct riscv_v_memcpy_counter *counter,
				       size_t n)
{
	atomic64_inc(&counter->calls);
	atomic64_add(n, &counter->bytes);
}

static void riscv_v_memcpy_counter_reset(struct riscv_v_memcpy_counter *counter)
{
	atomic64_set(&counter->calls, 0);
	atomic64_set(&counter->bytes, 0);
}

static void riscv_v_memcpy_profile_account(const void *dst, const void *src,
					   size_t n,
					   enum riscv_v_memcpy_reason reason)
{
	enum riscv_v_memcpy_bucket bucket = riscv_v_memcpy_bucket(n);
	bool no_overlap = riscv_v_memcpy_no_overlap(dst, src, n);

	riscv_v_memcpy_counter_add(&riscv_v_memcpy_profile_total, n);
	riscv_v_memcpy_counter_add(&riscv_v_memcpy_profile_reason[reason], n);
	riscv_v_memcpy_counter_add(&riscv_v_memcpy_profile_bucket[bucket], n);
	riscv_v_memcpy_counter_add(&riscv_v_memcpy_profile_align[
				   riscv_v_memcpy_align(dst, src)], n);

	if (reason == RISCV_V_MEMCPY_REASON_VECTOR)
		riscv_v_memcpy_counter_add(
			&riscv_v_memcpy_profile_vector_bucket[bucket], n);

	if (no_overlap)
		riscv_v_memcpy_counter_add(&riscv_v_memcpy_profile_nonoverlap,
					   n);
	else
		riscv_v_memcpy_counter_add(&riscv_v_memcpy_profile_overlap, n);
}

static enum riscv_v_memcpy_reason riscv_v_memcpy_fallback_reason(
	const void *dst, const void *src, size_t n)
{
	if (!IS_ENABLED(CONFIG_64BIT))
		return RISCV_V_MEMCPY_REASON_CONFIG;
	if (n < RISCV_ISA_V_MEMCPY_THRESHOLD)
		return RISCV_V_MEMCPY_REASON_SMALL;
	if (unlikely(system_state != SYSTEM_RUNNING))
		return RISCV_V_MEMCPY_REASON_NOT_RUNNING;
	if (riscv_v_memcpy_disabled)
		return RISCV_V_MEMCPY_REASON_DISABLED;
	if (!riscv_v_memcpy_no_overlap(dst, src, n))
		return RISCV_V_MEMCPY_REASON_OVERLAP;
	if (!has_vector())
		return RISCV_V_MEMCPY_REASON_NO_VECTOR;
	if (!may_use_simd())
		return RISCV_V_MEMCPY_REASON_DISALLOWED;

	return RISCV_V_MEMCPY_REASON_OTHER;
}

static void riscv_v_memcpy_profile_reset(void)
{
	unsigned int i;

	WRITE_ONCE(riscv_v_memcpy_calls, 0);
	WRITE_ONCE(riscv_v_memcpy_bytes, 0);
	riscv_v_memcpy_counter_reset(&riscv_v_memcpy_profile_total);
	riscv_v_memcpy_counter_reset(&riscv_v_memcpy_profile_nonoverlap);
	riscv_v_memcpy_counter_reset(&riscv_v_memcpy_profile_overlap);

	for (i = 0; i < RISCV_V_MEMCPY_REASON_NR; i++)
		riscv_v_memcpy_counter_reset(&riscv_v_memcpy_profile_reason[i]);
	for (i = 0; i < RISCV_V_MEMCPY_BUCKET_NR; i++) {
		riscv_v_memcpy_counter_reset(&riscv_v_memcpy_profile_bucket[i]);
		riscv_v_memcpy_counter_reset(
			&riscv_v_memcpy_profile_vector_bucket[i]);
	}
	for (i = 0; i < RISCV_V_MEMCPY_ALIGN_NR; i++)
		riscv_v_memcpy_counter_reset(&riscv_v_memcpy_profile_align[i]);
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

static ssize_t profile_enabled_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  static_key_enabled(&riscv_v_memcpy_profile_key.key));
}

static ssize_t profile_enabled_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	if (enable)
		static_branch_enable(&riscv_v_memcpy_profile_key);
	else
		static_branch_disable(&riscv_v_memcpy_profile_key);

	return count;
}

static ssize_t profile_reset_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	bool reset;
	int ret;

	ret = kstrtobool(buf, &reset);
	if (ret)
		return ret;
	if (!reset)
		return -EINVAL;

	riscv_v_memcpy_profile_reset();

	return count;
}

static ssize_t profile_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	unsigned int i;
	ssize_t len = 0;

	len += sysfs_emit_at(buf, len, "enabled %d\n",
			     static_key_enabled(&riscv_v_memcpy_profile_key.key));
	len += sysfs_emit_at(buf, len, "threshold %u\n",
			     RISCV_ISA_V_MEMCPY_THRESHOLD);
	len += sysfs_emit_at(buf, len, "total %lld %lld\n",
			     atomic64_read(&riscv_v_memcpy_profile_total.calls),
			     atomic64_read(&riscv_v_memcpy_profile_total.bytes));

	for (i = 0; i < RISCV_V_MEMCPY_REASON_NR; i++)
		len += sysfs_emit_at(buf, len, "reason %s %lld %lld\n",
				     riscv_v_memcpy_reason_name[i],
				     atomic64_read(&riscv_v_memcpy_profile_reason[i].calls),
				     atomic64_read(&riscv_v_memcpy_profile_reason[i].bytes));

	for (i = 0; i < RISCV_V_MEMCPY_BUCKET_NR; i++)
		len += sysfs_emit_at(buf, len, "bucket %s %lld %lld %lld %lld\n",
				     riscv_v_memcpy_bucket_name[i],
				     atomic64_read(&riscv_v_memcpy_profile_bucket[i].calls),
				     atomic64_read(&riscv_v_memcpy_profile_bucket[i].bytes),
				     atomic64_read(&riscv_v_memcpy_profile_vector_bucket[i].calls),
				     atomic64_read(&riscv_v_memcpy_profile_vector_bucket[i].bytes));

	for (i = 0; i < RISCV_V_MEMCPY_ALIGN_NR; i++)
		len += sysfs_emit_at(buf, len, "align %s %lld %lld\n",
				     riscv_v_memcpy_align_name[i],
				     atomic64_read(&riscv_v_memcpy_profile_align[i].calls),
				     atomic64_read(&riscv_v_memcpy_profile_align[i].bytes));

	len += sysfs_emit_at(buf, len, "nonoverlap %lld %lld\n",
			     atomic64_read(&riscv_v_memcpy_profile_nonoverlap.calls),
			     atomic64_read(&riscv_v_memcpy_profile_nonoverlap.bytes));
	len += sysfs_emit_at(buf, len, "overlap %lld %lld\n",
			     atomic64_read(&riscv_v_memcpy_profile_overlap.calls),
			     atomic64_read(&riscv_v_memcpy_profile_overlap.bytes));

	return len;
}

static struct kobj_attribute memcpy_threshold_attr =
	__ATTR_RO(memcpy_threshold);
static struct kobj_attribute memcpy_disabled_attr =
	__ATTR_RO(memcpy_disabled);
static struct kobj_attribute memcpy_calls_attr =
	__ATTR_RO(memcpy_calls);
static struct kobj_attribute memcpy_bytes_attr =
	__ATTR_RO(memcpy_bytes);
static struct kobj_attribute profile_enabled_attr =
	__ATTR_RW(profile_enabled);
static struct kobj_attribute profile_reset_attr =
	__ATTR_WO(profile_reset);
static struct kobj_attribute profile_attr =
	__ATTR_RO(profile);

static struct attribute *riscv_v_memcpy_attrs[] = {
	&memcpy_threshold_attr.attr,
	&memcpy_disabled_attr.attr,
	&memcpy_calls_attr.attr,
	&memcpy_bytes_attr.attr,
	&profile_enabled_attr.attr,
	&profile_reset_attr.attr,
	&profile_attr.attr,
	NULL,
};

static const struct attribute_group riscv_v_memcpy_attr_group = {
	.name = "riscv_v_memcpy",
	.attrs = riscv_v_memcpy_attrs,
};

static int __init riscv_v_memcpy_sysfs_init(void)
{
	if (riscv_v_memcpy_profile_requested)
		static_branch_enable(&riscv_v_memcpy_profile_key);

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
		if (static_branch_unlikely(&riscv_v_memcpy_profile_key))
			riscv_v_memcpy_profile_account(
				dst, src, n, RISCV_V_MEMCPY_REASON_VECTOR);

		return dst;
	}

	if (static_branch_unlikely(&riscv_v_memcpy_profile_key))
		riscv_v_memcpy_profile_account(
			dst, src, n, riscv_v_memcpy_fallback_reason(dst, src, n));

	return __memcpy_scalar(dst, src, n);
}
#undef memcpy
void *memcpy(void *dst, const void *src, size_t n) __weak __alias(__memcpy);
