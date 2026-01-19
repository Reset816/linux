#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/memblock.h>
#include <linux/mycov.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/smp.h>
#include <linux/uaccess.h>

struct mycov_interval {
	unsigned long start;
	unsigned long end; /* half-open: [start, end) */
};

static struct mycov_interval *g_intervals;
static size_t g_num_intervals;
static bool g_ready = false;

static int __init set_intervals(char *str)
{
	char *p = NULL, *tok = NULL;
	struct mycov_interval *arr = NULL;
	size_t n = 0, cap = 0;
	ssize_t ret = 0;
	unsigned long prev_end = 0;
	bool have_prev = false;

	/* __setup passes the value after "intervals="; str can be NULL/empty */
	if (!str || !*str)
		return 0; /* not handled (or malformed) */

	/* Allow only one successful initialization */
	if (READ_ONCE(g_ready))
		return 0;

	/* Count intervals: number of ';' + 1 (if non-empty) */
	if (str[0] == '\0') {
		goto out;
	}
	for (p = str; *p; ++p)
		if (*p == ';')
			cap++;
	cap = cap + 1;

	// arr = kcalloc(cap, sizeof(*arr), GFP_KERNEL);
	arr = memblock_alloc(cap * sizeof(*arr), SMP_CACHE_BYTES);
	if (!arr) {
		goto out;
	}

	/* Parse "start,end" tokens separated by ';' */
	p = str;
	while ((tok = strsep(&p, ";")) != NULL) {
		char *comma;
		char *start_s, *end_s;
		unsigned long start, end;

		/* strict: no empty tokens */
		if (*tok == '\0') {
			goto out;
		}

		comma = strchr(tok, ',');
		if (!comma) {
			goto out;
		}

		/* strict: exactly one comma and both sides non-empty */
		*comma = '\0';
		start_s = tok;
		end_s = comma + 1;

		if (*start_s == '\0' || *end_s == '\0') {
			goto out;
		}
		/* strict: no extra commas */
		if (strchr(end_s, ',')) {
			goto out;
		}

		/* Parse numbers (base auto-detect; change to 10 if you want strict decimal only) */
		if (kstrtoul(start_s, 0, &start) || kstrtoul(end_s, 0, &end)) {
			goto out;
		}

		if (start >= end) {
			goto out;
		}

		/* Validate sorted & non-overlapping (half-open => start >= prev_end OK) */
		if (have_prev && start < prev_end) {
			goto out;
		}

		if (n >= cap) {
			/* Should not happen given our counting, but be safe */
			goto out;
		}

		arr[n].start = start;
		arr[n].end = end;
		printk(KERN_ERR "[set_intervals] %lu, %lu", start, end);
		n++;

		prev_end = end;
		have_prev = true;
	}

	/* Must parse at least one interval */
	if (n == 0) {
		goto out;
	}

	/*
	 * Publish: store fully initialized table, then mark ready.
	 * Readers use acquire to see consistent g_intervals/g_num_intervals.
	 */
	WRITE_ONCE(g_intervals, arr);
	WRITE_ONCE(g_num_intervals, n);
	smp_store_release(&g_ready, true);

	/* success: ownership transferred; don't free arr below */
	arr = NULL;
	ret = 1;

out:
	return ret;
}
__setup("intervals=", set_intervals);

/* O(log N) check using binary search */
bool is_in_any_interval(unsigned long point)
{
	struct mycov_interval *arr;
	size_t n, lo, hi;

	if (!smp_load_acquire(&g_ready))
		return false;

	arr = READ_ONCE(g_intervals);
	n = READ_ONCE(g_num_intervals);
	if (!arr || n == 0)
		return false;

	lo = 0;
	hi = n;
	while (lo < hi) {
		size_t mid = lo + ((hi - lo) >> 1);
		unsigned long s = arr[mid].start;
		unsigned long e = arr[mid].end;

		if (point < s) {
			hi = mid;
		} else if (point >= e) {
			lo = mid + 1;
		} else {
			/* s <= point < e */
			return true;
		}
	}
	return false;
}
