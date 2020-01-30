#ifndef _M68KNOMMU_CACHEFLUSH_H
#define _M68KNOMMU_CACHEFLUSH_H

/*
 * (C) Copyright 2000-2002, Greg Ungerer <gerg@snapgear.com>
 */
#include <linux/mm.h>

#define flush_cache_all()			__flush_cache_all()
#define flush_cache_mm(mm)			do { } while (0)
#define flush_cache_range(vma, start, end)	do { } while (0)
#define flush_cache_page(vma, vmaddr)		do { } while (0)
#define flush_dcache_range(start,len)		do { } while (0)
#define flush_dcache_page(page)			do { } while (0)
#define flush_dcache_mmap_lock(mapping)		do { } while (0)
#define flush_dcache_mmap_unlock(mapping)	do { } while (0)
#define flush_icache_range(start,len)		__flush_cache_all()
#define flush_icache_page(vma,pg)		do { } while (0)
#define flush_icache_user_range(vma,pg,adr,len)	do { } while (0)
#define flush_cache_vmap(start, end)		flush_cache_all()
#define flush_cache_vunmap(start, end)		flush_cache_all()

#define copy_to_user_page(vma, page, vaddr, dst, src, len) \
	memcpy(dst, src, len)
#define copy_from_user_page(vma, page, vaddr, dst, src, len) \
	memcpy(dst, src, len)

extern inline void __flush_cache_all(void)
{
#ifdef CONFIG_M5407
	/*
	 *	Use cpushl to push and invalidate all cache lines.
	 *	Gas doesn't seem to know how to generate the ColdFire
	 *	cpushl instruction... Oh well, bit stuff it for now.
	 */
	__asm__ __volatile__ (
		"nop\n\t"
		"clrl	%%d0\n\t"
		"1:\n\t"
		"movel	%%d0,%%a0\n\t"
		"2:\n\t"
		".word	0xf468\n\t"
		"addl	#0x10,%%a0\n\t"
		"cmpl	#0x00000800,%%a0\n\t"
		"blt	2b\n\t"
		"addql	#1,%%d0\n\t"
		"cmpil	#4,%%d0\n\t"
		"bne	1b\n\t"
		"movel	#0xb6088500,%%d0\n\t"
		"movec	%%d0,%%CACR\n\t"
		: : : "d0", "a0" );
#endif /* CONFIG_M5407 */
#ifdef CONFIG_M5272
	__asm__ __volatile__ (
        	"movel	#0x01000000, %%d0\n\t"
        	"movec	%%d0, %%CACR\n\t"
		"nop\n\t"
        	"movel	#0x80000100, %%d0\n\t"
        	"movec	%%d0, %%CACR\n\t"
		"nop\n\t"
		: : : "d0" );
#endif /* CONFIG_M5272 */
#if 0 /* CONFIG_M5249 */
	__asm__ __volatile__ (
        	"movel	#0x01000000, %%d0\n\t"
        	"movec	%%d0, %%CACR\n\t"
		"nop\n\t"
        	"movel	#0xa0000200, %%d0\n\t"
        	"movec	%%d0, %%CACR\n\t"
		"nop\n\t"
		: : : "d0" );
#endif /* CONFIG_M5249 */
}

#endif /* _M68KNOMMU_CACHEFLUSH_H */
