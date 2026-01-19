/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mycov_rw

#if !defined(_TRACE_MYCOV_RW_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MYCOV_RW_H

#include <linux/tracepoint.h>

TRACE_EVENT(mycov_rw_write,

	TP_PROTO(unsigned long ret_ip, unsigned long addr, size_t size),

	TP_ARGS(ret_ip, addr, size),

	TP_STRUCT__entry(
		__field(unsigned long, ret_ip)
		__field(unsigned long, addr)
		__field(u32, size)
	),

	TP_fast_assign(
		__entry->ret_ip = ret_ip;
		__entry->addr   = addr;
		__entry->size   = (u32)size;
	),

	TP_printk("%px,%px,%u",
		  (void *)__entry->ret_ip, (void *)__entry->addr, __entry->size)
);

#endif /* _TRACE_MYCOV_RW_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/events
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE mycov_rw
#include <trace/define_trace.h>