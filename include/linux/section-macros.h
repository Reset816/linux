/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SECTION_MACROS_H
#define _LINUX_SECTION_MACROS_H

#include <linux/stringify.h>
#include <linux/compiler_types.h>

#ifdef __ASSEMBLY__


#ifdef CONFIG_AS_HAS_RELOC
# define __ASM_BFD_RELOC_NONE(lbl)  .reloc ., BFD_RELOC_NONE, lbl;
#else
# define __ASM_BFD_RELOC_NONE(lbl) /* no-op */
#endif

#define _PUSHSECTION(lbl, sec, ...)                                             \
	__ASM_BFD_RELOC_NONE(lbl)                                                   \
	.pushsection sec, __VA_ARGS__ ;                                             \
	lbl:

#define PUSHSECTION(sec, ...)                                                   \
	_PUSHSECTION(__SECTION_NAME(sec), __SECTION_NAME(sec), __VA_ARGS__)


#else /* !__ASSEMBLY__ */

#define __ASM_UNIQUE_LBL(kind)   ".L" __stringify(__UNIQUE_ID(kind))

#ifdef CONFIG_AS_HAS_RELOC
# define __ASM_BFD_RELOC_NONE(lbl)  ".reloc ., BFD_RELOC_NONE, " lbl "\n\t"
#else
# define __ASM_BFD_RELOC_NONE(lbl)  /* no-op */
#endif

#define _PUSHSECTION(lbl, sec, ...)                                             \
	__ASM_BFD_RELOC_NONE(lbl)                                                   \
	".pushsection " __stringify(sec) ", " #__VA_ARGS__ "\n\t"                   \
	lbl ":\n\t"

#define PUSHSECTION(sec, ...)                                                   \
	_PUSHSECTION(__SECTION_NAME(sec%=), __SECTION_NAME(sec), __VA_ARGS__)

#endif /* __ASSEMBLY__ */

#endif /* _LINUX_SECTION_MACROS_H */
