#ifndef _LINUX_MYCOV_H
#define _LINUX_MYCOV_H
#include <linux/atomic.h>
#include <linux/types.h>

extern int log_state;

#define MYINST_RW_RET_IP_HASHTABLE_BITS 26
extern atomic_t *rw_ret_ip_counter; // for deduplication of write accesses
extern atomic_t rw_ret_ip_counter_usage;

#define MYINST_ICALL_HASHTABLE_BITS 25
extern atomic_t *icall_counter; // for deduplication of indirect calls
extern atomic_t icall_counter_usage;

extern bool is_in_any_interval(unsigned long point);
extern void log_indirect_call(void *target);

#endif /* _LINUX_MYCOV_H */
