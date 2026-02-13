/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM myinst

#if !defined(_TRACE_MYINST_RW_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MYINST_RW_H

#include <linux/tracepoint.h>

TRACE_EVENT(rw,

	TP_PROTO(unsigned long ret_ip, unsigned long addr, size_t size, u8 write),

	TP_ARGS(ret_ip, addr, size, write),

	TP_STRUCT__entry(
		__field(unsigned long, ret_ip)
		__field(unsigned long, addr)
		__field(u32, size)
		__field(u8, write)
	),

	TP_fast_assign(
		__entry->ret_ip = ret_ip;
		__entry->addr   = addr;
		__entry->size   = (u32)size;
		__entry->write = write;
	),

	TP_printk("%px,%px,%u,%u",
		  (void *)__entry->ret_ip, (void *)__entry->addr, __entry->size, __entry->write)
);

TRACE_EVENT(ic,
	TP_PROTO(void *ret_ip, void *target),

	TP_ARGS(ret_ip, target),

	TP_STRUCT__entry(
		__field(void *, ret_ip)
		__field(void *, target)
	),

	TP_fast_assign(
		__entry->ret_ip = ret_ip;
		__entry->target = target;
	),

	TP_printk("%px,%px",
		  __entry->ret_ip, __entry->target)
);

#endif /* _TRACE_MYINST_RW_H */

#include <trace/define_trace.h>
