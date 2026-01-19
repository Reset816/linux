#ifndef _LINUX_MYCOV_H
#define _LINUX_MYCOV_H
#include <linux/atomic.h>
#include <linux/types.h>

extern int my_state;

#define MYCOV_RW_RET_IP_HASHTABLE_BITS 28
extern atomic_t *rw_ret_ip_counter; // for deduplication of write accesses

extern bool is_in_any_interval(unsigned long point);

#endif /* _LINUX_MYCOV_H */
