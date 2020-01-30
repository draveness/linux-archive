/*
 *  linux/include/asm-arm/cacheflush.h
 *
 *  Copyright (C) 1999-2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _ASMARM_CACHEFLUSH_H
#define _ASMARM_CACHEFLUSH_H

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/mm.h>

#include <asm/mman.h>
#include <asm/glue.h>

/*
 *	Cache Model
 *	===========
 */
#undef _CACHE
#undef MULTI_CACHE

#if defined(CONFIG_CPU_ARM610) || defined(CONFIG_CPU_ARM710)
# ifdef _CACHE
#  define MULTI_CACHE 1
# else
#  define _CACHE v3
# endif
#endif

#if defined(CONFIG_CPU_ARM720T)
# ifdef _CACHE
#  define MULTI_CACHE 1
# else
#  define _CACHE v4
# endif
#endif

#if defined(CONFIG_CPU_ARM920T) || defined(CONFIG_CPU_ARM922T) || \
    defined(CONFIG_CPU_ARM925T) || defined(CONFIG_CPU_ARM1020)
# define MULTI_CACHE 1
#endif

#if defined(CONFIG_CPU_ARM926T)
# ifdef _CACHE
#  define MULTI_CACHE 1
# else
#  define _CACHE arm926
# endif
#endif

#if defined(CONFIG_CPU_SA110) || defined(CONFIG_CPU_SA1100)
# ifdef _CACHE
#  define MULTI_CACHE 1
# else
#  define _CACHE v4wb
# endif
#endif

#if defined(CONFIG_CPU_XSCALE)
# ifdef _CACHE
#  define MULTI_CACHE 1
# else
#  define _CACHE xscale
# endif
#endif

#if defined(CONFIG_CPU_V6)
//# ifdef _CACHE
#  define MULTI_CACHE 1
//# else
//#  define _CACHE v6
//# endif
#endif

#if !defined(_CACHE) && !defined(MULTI_CACHE)
#error Unknown cache maintainence model
#endif

/*
 * This flag is used to indicate that the page pointed to by a pte
 * is dirty and requires cleaning before returning it to the user.
 */
#define PG_dcache_dirty PG_arch_1

/*
 *	MM Cache Management
 *	===================
 *
 *	The arch/arm/mm/cache-*.S and arch/arm/mm/proc-*.S files
 *	implement these methods.
 *
 *	Start addresses are inclusive and end addresses are exclusive;
 *	start addresses should be rounded down, end addresses up.
 *
 *	See Documentation/cachetlb.txt for more information.
 *	Please note that the implementation of these, and the required
 *	effects are cache-type (VIVT/VIPT/PIPT) specific.
 *
 *	flush_cache_kern_all()
 *
 *		Unconditionally clean and invalidate the entire cache.
 *
 *	flush_cache_user_mm(mm)
 *
 *		Clean and invalidate all user space cache entries
 *		before a change of page tables.
 *
 *	flush_cache_user_range(start, end, flags)
 *
 *		Clean and invalidate a range of cache entries in the
 *		specified address space before a change of page tables.
 *		- start - user start address (inclusive, page aligned)
 *		- end   - user end address   (exclusive, page aligned)
 *		- flags - vma->vm_flags field
 *
 *	coherent_kern_range(start, end)
 *
 *		Ensure coherency between the Icache and the Dcache in the
 *		region described by start, end.  If you have non-snooping
 *		Harvard caches, you need to implement this function.
 *		- start  - virtual start address
 *		- end    - virtual end address
 *
 *	DMA Cache Coherency
 *	===================
 *
 *	dma_inv_range(start, end)
 *
 *		Invalidate (discard) the specified virtual address range.
 *		May not write back any entries.  If 'start' or 'end'
 *		are not cache line aligned, those lines must be written
 *		back.
 *		- start  - virtual start address
 *		- end    - virtual end address
 *
 *	dma_clean_range(start, end)
 *
 *		Clean (write back) the specified virtual address range.
 *		- start  - virtual start address
 *		- end    - virtual end address
 *
 *	dma_flush_range(start, end)
 *
 *		Clean and invalidate the specified virtual address range.
 *		- start  - virtual start address
 *		- end    - virtual end address
 */

struct cpu_cache_fns {
	void (*flush_kern_all)(void);
	void (*flush_user_all)(void);
	void (*flush_user_range)(unsigned long, unsigned long, unsigned int);

	void (*coherent_kern_range)(unsigned long, unsigned long);
	void (*flush_kern_dcache_page)(void *);

	void (*dma_inv_range)(unsigned long, unsigned long);
	void (*dma_clean_range)(unsigned long, unsigned long);
	void (*dma_flush_range)(unsigned long, unsigned long);
};

/*
 * Select the calling method
 */
#ifdef MULTI_CACHE

extern struct cpu_cache_fns cpu_cache;

#define __cpuc_flush_kern_all		cpu_cache.flush_kern_all
#define __cpuc_flush_user_all		cpu_cache.flush_user_all
#define __cpuc_flush_user_range		cpu_cache.flush_user_range
#define __cpuc_coherent_kern_range	cpu_cache.coherent_kern_range
#define __cpuc_flush_dcache_page	cpu_cache.flush_kern_dcache_page

/*
 * These are private to the dma-mapping API.  Do not use directly.
 * Their sole purpose is to ensure that data held in the cache
 * is visible to DMA, or data written by DMA to system memory is
 * visible to the CPU.
 */
#define dmac_inv_range			cpu_cache.dma_inv_range
#define dmac_clean_range		cpu_cache.dma_clean_range
#define dmac_flush_range		cpu_cache.dma_flush_range

#else

#define __cpuc_flush_kern_all		__glue(_CACHE,_flush_kern_cache_all)
#define __cpuc_flush_user_all		__glue(_CACHE,_flush_user_cache_all)
#define __cpuc_flush_user_range		__glue(_CACHE,_flush_user_cache_range)
#define __cpuc_coherent_kern_range	__glue(_CACHE,_coherent_kern_range)
#define __cpuc_flush_dcache_page	__glue(_CACHE,_flush_kern_dcache_page)

extern void __cpuc_flush_kern_all(void);
extern void __cpuc_flush_user_all(void);
extern void __cpuc_flush_user_range(unsigned long, unsigned long, unsigned int);
extern void __cpuc_coherent_kern_range(unsigned long, unsigned long);
extern void __cpuc_flush_dcache_page(void *);

/*
 * These are private to the dma-mapping API.  Do not use directly.
 * Their sole purpose is to ensure that data held in the cache
 * is visible to DMA, or data written by DMA to system memory is
 * visible to the CPU.
 */
#define dmac_inv_range			__glue(_CACHE,_dma_inv_range)
#define dmac_clean_range		__glue(_CACHE,_dma_clean_range)
#define dmac_flush_range		__glue(_CACHE,_dma_flush_range)

extern void dmac_inv_range(unsigned long, unsigned long);
extern void dmac_clean_range(unsigned long, unsigned long);
extern void dmac_flush_range(unsigned long, unsigned long);

#endif

/*
 * flush_cache_vmap() is used when creating mappings (eg, via vmap,
 * vmalloc, ioremap etc) in kernel space for pages.  Since the
 * direct-mappings of these pages may contain cached data, we need
 * to do a full cache flush to ensure that writebacks don't corrupt
 * data placed into these pages via the new mappings.
 */
#define flush_cache_vmap(start, end)		flush_cache_all()
#define flush_cache_vunmap(start, end)		flush_cache_all()

/*
 * Copy user data from/to a page which is mapped into a different
 * processes address space.  Really, we want to allow our "user
 * space" model to handle this.
 */
#define copy_to_user_page(vma, page, vaddr, dst, src, len) \
do { memcpy(dst, src, len); \
     flush_icache_user_range(vma, page, vaddr, len); \
} while (0)
#define copy_from_user_page(vma, page, vaddr, dst, src, len) \
	memcpy(dst, src, len)

/*
 * Convert calls to our calling convention.
 */
#define flush_cache_all()		__cpuc_flush_kern_all()

static inline void flush_cache_mm(struct mm_struct *mm)
{
	if (current->active_mm == mm)
		__cpuc_flush_user_all();
}

static inline void
flush_cache_range(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	if (current->active_mm == vma->vm_mm)
		__cpuc_flush_user_range(start & PAGE_MASK, PAGE_ALIGN(end),
					vma->vm_flags);
}

static inline void
flush_cache_page(struct vm_area_struct *vma, unsigned long user_addr)
{
	if (current->active_mm == vma->vm_mm) {
		unsigned long addr = user_addr & PAGE_MASK;
		__cpuc_flush_user_range(addr, addr + PAGE_SIZE, vma->vm_flags);
	}
}

/*
 * Perform necessary cache operations to ensure that data previously
 * stored within this range of addresses can be executed by the CPU.
 */
#define flush_icache_range(s,e)		__cpuc_coherent_kern_range(s,e)

/*
 * Perform necessary cache operations to ensure that the TLB will
 * see data written in the specified area.
 */
#define clean_dcache_area(start,size)	cpu_dcache_clean_area(start, size)

/*
 * flush_dcache_page is used when the kernel has written to the page
 * cache page at virtual address page->virtual.
 *
 * If this page isn't mapped (ie, page_mapping == NULL), or it might
 * have userspace mappings, then we _must_ always clean + invalidate
 * the dcache entries associated with the kernel mapping.
 *
 * Otherwise we can defer the operation, and clean the cache when we are
 * about to change to user space.  This is the same method as used on SPARC64.
 * See update_mmu_cache for the user space part.
 */
extern void flush_dcache_page(struct page *);

#define flush_dcache_mmap_lock(mapping) \
	spin_lock_irq(&(mapping)->tree_lock)
#define flush_dcache_mmap_unlock(mapping) \
	spin_unlock_irq(&(mapping)->tree_lock)

#define flush_icache_user_range(vma,page,addr,len) \
	flush_dcache_page(page)

/*
 * We don't appear to need to do anything here.  In fact, if we did, we'd
 * duplicate cache flushing elsewhere performed by flush_dcache_page().
 */
#define flush_icache_page(vma,page)	do { } while (0)

#endif
