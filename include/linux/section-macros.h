/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SECTION_MACROS_H
#define _LINUX_SECTION_MACROS_H

#include <linux/stringify.h>
#include <linux/compiler_types.h>

#ifdef __ASSEMBLY__

.macro  _PUSHSECTION label:req, section:req, args:vararg
#ifdef CONFIG_AS_HAS_RELOC
        .reloc  ., BFD_RELOC_NONE, \label
#endif
        /* 把 \section 和 .\@ 在 GAS 里直接拼成：<基名>.<唯一号> */
        .pushsection \section\().\@, \args
\label:
        .endm

.macro  PUSHSECTION section:req, args:vararg
        _PUSHSECTION .Lpushsec\@, \section, \args
.endm


#else /* !__ASSEMBLY__ */

#define __ASM_UNIQUE_LBL(kind)   ".L" __UNIQUE_ID(kind)

#ifdef CONFIG_AS_HAS_RELOC
# define __ASM_BFD_RELOC_NONE(lbl)  ".reloc ., BFD_RELOC_NONE, " lbl "\n\t"
#else
# define __ASM_BFD_RELOC_NONE(lbl)  /* no-op */
#endif

#define _PUSHSECTION(lbl, sec, ...)                                             \
	__ASM_BFD_RELOC_NONE(lbl)                                                   \
	".pushsection " sec ", " #__VA_ARGS__ "\n\t"                   \
	lbl ":\n\t"

#define PUSHSECTION(sec, ...)                                                   \
	_PUSHSECTION(__SECTION_NAME(sec%=), __SECTION_NAME(sec.%=), __VA_ARGS__)

#endif /* __ASSEMBLY__ */

#endif /* _LINUX_SECTION_MACROS_H */
