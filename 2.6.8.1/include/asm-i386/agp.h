#ifndef AGP_H
#define AGP_H 1

#include <asm/pgtable.h>
#include <asm/cacheflush.h>

/* 
 * Functions to keep the agpgart mappings coherent with the MMU.
 * The GART gives the CPU a physical alias of pages in memory. The alias region is
 * mapped uncacheable. Make sure there are no conflicting mappings
 * with different cachability attributes for the same page. This avoids
 * data corruption on some CPUs.
 */

#define map_page_into_agp(page) change_page_attr(page, 1, PAGE_KERNEL_NOCACHE)
#define unmap_page_from_agp(page) change_page_attr(page, 1, PAGE_KERNEL)
#define flush_agp_mappings() global_flush_tlb()

/* Could use CLFLUSH here if the cpu supports it. But then it would
   need to be called for each cacheline of the whole page so it may not be 
   worth it. Would need a page for it. */
#define flush_agp_cache() asm volatile("wbinvd":::"memory")

#endif
