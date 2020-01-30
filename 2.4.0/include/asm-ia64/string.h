#ifndef _ASM_IA64_STRING_H
#define _ASM_IA64_STRING_H

/*
 * Here is where we want to put optimized versions of the string
 * routines.
 *
 * Copyright (C) 1998-2000 Hewlett-Packard Co
 * Copyright (C) 1998-2000 David Mosberger-Tang <davidm@hpl.hp.com>
 */

#include <linux/config.h>	/* remove this once we remove the A-step workaround... */
#ifndef CONFIG_ITANIUM_ASTEP_SPECIFIC

#define __HAVE_ARCH_STRLEN	1 /* see arch/ia64/lib/strlen.S */
#define __HAVE_ARCH_MEMSET	1 /* see arch/ia64/lib/memset.S */
#define __HAVE_ARCH_MEMCPY	1 /* see arch/ia64/lib/memcpy.S */
#define __HAVE_ARCH_BCOPY	1 /* see arch/ia64/lib/memcpy.S */

extern __kernel_size_t strlen (const char *);
extern void *memset (void *, int, __kernel_size_t);
extern void *memcpy (void *, const void *, __kernel_size_t);

#endif /* CONFIG_ITANIUM_ASTEP_SPECIFIC */

#endif /* _ASM_IA64_STRING_H */
