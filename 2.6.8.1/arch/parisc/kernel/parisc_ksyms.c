/*
 *    Architecture-specific kernel symbols
 *
 *    Copyright (C) 2000-2001 Richard Hirst <rhirst with parisc-linux.org>
 *    Copyright (C) 2001 Dave Kennedy
 *    Copyright (C) 2001 Paul Bame <bame at parisc-linux.org>
 *    Copyright (C) 2001-2003 Grant Grundler <grundler with parisc-linux.org>
 *    Copyright (C) 2002-2003 Matthew Wilcox <willy at parisc-linux.org>
 *    Copyright (C) 2002 Randolph Chung <tausq at parisc-linux.org>
 *    Copyright (C) 2002-2003 Helge Deller <deller with parisc-linux.org>
 * 
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>

#include <linux/string.h>
EXPORT_SYMBOL(memchr);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memscan);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(strcat);
EXPORT_SYMBOL(strchr);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strncat);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strnlen);
EXPORT_SYMBOL(strrchr);
EXPORT_SYMBOL(strstr);
EXPORT_SYMBOL(strpbrk);

#include <linux/pm.h>
EXPORT_SYMBOL(pm_power_off);

#include <asm/atomic.h>
EXPORT_SYMBOL(__xchg8);
EXPORT_SYMBOL(__xchg32);
EXPORT_SYMBOL(__cmpxchg_u32);
#ifdef CONFIG_SMP
EXPORT_SYMBOL(__atomic_hash);
#endif
#ifdef __LP64__
EXPORT_SYMBOL(__xchg64);
EXPORT_SYMBOL(__cmpxchg_u64);
#endif

#include <asm/uaccess.h>
EXPORT_SYMBOL(lcopy_to_user);
EXPORT_SYMBOL(lcopy_from_user);
EXPORT_SYMBOL(lcopy_in_user);
EXPORT_SYMBOL(lstrncpy_from_user);
EXPORT_SYMBOL(lclear_user);
EXPORT_SYMBOL(lstrnlen_user);

#ifndef __LP64__
/* Needed so insmod can set dp value */
extern int $global$;
EXPORT_SYMBOL($global$);
#endif

#include <asm/io.h>
EXPORT_SYMBOL(__ioremap);
EXPORT_SYMBOL(iounmap);
EXPORT_SYMBOL(__memcpy_toio);
EXPORT_SYMBOL(__memcpy_fromio);
EXPORT_SYMBOL(__memset_io);

#include <asm/unistd.h>
EXPORT_SYMBOL(sys_open);
EXPORT_SYMBOL(sys_lseek);
EXPORT_SYMBOL(sys_read);
EXPORT_SYMBOL(sys_write);

#include <asm/semaphore.h>
EXPORT_SYMBOL(__up);
EXPORT_SYMBOL(__down_interruptible);
EXPORT_SYMBOL(__down);

extern void $$divI(void);
extern void $$divU(void);
extern void $$remI(void);
extern void $$remU(void);
extern void $$mulI(void);
extern void $$divU_3(void);
extern void $$divU_5(void);
extern void $$divU_6(void);
extern void $$divU_9(void);
extern void $$divU_10(void);
extern void $$divU_12(void);
extern void $$divU_7(void);
extern void $$divU_14(void);
extern void $$divU_15(void);
extern void $$divI_3(void);
extern void $$divI_5(void);
extern void $$divI_6(void);
extern void $$divI_7(void);
extern void $$divI_9(void);
extern void $$divI_10(void);
extern void $$divI_12(void);
extern void $$divI_14(void);
extern void $$divI_15(void);

EXPORT_SYMBOL($$divI);
EXPORT_SYMBOL($$divU);
EXPORT_SYMBOL($$remI);
EXPORT_SYMBOL($$remU);
EXPORT_SYMBOL($$mulI);
EXPORT_SYMBOL($$divU_3);
EXPORT_SYMBOL($$divU_5);
EXPORT_SYMBOL($$divU_6);
EXPORT_SYMBOL($$divU_9);
EXPORT_SYMBOL($$divU_10);
EXPORT_SYMBOL($$divU_12);
EXPORT_SYMBOL($$divU_7);
EXPORT_SYMBOL($$divU_14);
EXPORT_SYMBOL($$divU_15);
EXPORT_SYMBOL($$divI_3);
EXPORT_SYMBOL($$divI_5);
EXPORT_SYMBOL($$divI_6);
EXPORT_SYMBOL($$divI_7);
EXPORT_SYMBOL($$divI_9);
EXPORT_SYMBOL($$divI_10);
EXPORT_SYMBOL($$divI_12);
EXPORT_SYMBOL($$divI_14);
EXPORT_SYMBOL($$divI_15);

extern void __ashrdi3(void);
extern void __ashldi3(void);
extern void __lshrdi3(void);
extern void __muldi3(void);

EXPORT_SYMBOL(__ashrdi3);
EXPORT_SYMBOL(__ashldi3);
EXPORT_SYMBOL(__lshrdi3);
EXPORT_SYMBOL(__muldi3);

asmlinkage void * __canonicalize_funcptr_for_compare(void *);
EXPORT_SYMBOL(__canonicalize_funcptr_for_compare);

#ifdef __LP64__
extern void __divdi3(void);
extern void __udivdi3(void);
extern void __umoddi3(void);
extern void __moddi3(void);

EXPORT_SYMBOL(__divdi3);
EXPORT_SYMBOL(__udivdi3);
EXPORT_SYMBOL(__umoddi3);
EXPORT_SYMBOL(__moddi3);
#endif

#ifndef __LP64__
extern void $$dyncall(void);
EXPORT_SYMBOL($$dyncall);
#endif

#ifdef CONFIG_DISCONTIGMEM
#include <asm/mmzone.h>
EXPORT_SYMBOL(node_data);
EXPORT_SYMBOL(pfnnid_map);
#endif
