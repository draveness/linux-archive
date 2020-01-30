#ifndef _PARISC_CACHEFLUSH_H
#define _PARISC_CACHEFLUSH_H

#include <linux/config.h>
#include <linux/mm.h>

/* The usual comment is "Caches aren't brain-dead on the <architecture>".
 * Unfortunately, that doesn't apply to PA-RISC. */

/* Cache flush operations */

#ifdef CONFIG_SMP
#define flush_cache_mm(mm) flush_cache_all()
#else
#define flush_cache_mm(mm) flush_cache_all_local()
#endif

#define flush_kernel_dcache_range(start,size) \
	flush_kernel_dcache_range_asm((start), (start)+(size));

extern void flush_cache_all_local(void);

static inline void cacheflush_h_tmp_function(void *dummy)
{
	flush_cache_all_local();
}

static inline void flush_cache_all(void)
{
	on_each_cpu(cacheflush_h_tmp_function, NULL, 1, 1);
}

#define flush_cache_vmap(start, end)		flush_cache_all()
#define flush_cache_vunmap(start, end)		flush_cache_all()

/* The following value needs to be tuned and probably scaled with the
 * cache size.
 */

#define FLUSH_THRESHOLD 0x80000

static inline void
flush_user_dcache_range(unsigned long start, unsigned long end)
{
#ifdef CONFIG_SMP
	flush_user_dcache_range_asm(start,end);
#else
	if ((end - start) < FLUSH_THRESHOLD)
		flush_user_dcache_range_asm(start,end);
	else
		flush_data_cache();
#endif
}

static inline void
flush_user_icache_range(unsigned long start, unsigned long end)
{
#ifdef CONFIG_SMP
	flush_user_icache_range_asm(start,end);
#else
	if ((end - start) < FLUSH_THRESHOLD)
		flush_user_icache_range_asm(start,end);
	else
		flush_instruction_cache();
#endif
}

extern void flush_dcache_page(struct page *page);

#define flush_dcache_mmap_lock(mapping) \
	spin_lock_irq(&(mapping)->tree_lock)
#define flush_dcache_mmap_unlock(mapping) \
	spin_unlock_irq(&(mapping)->tree_lock)

#define flush_icache_page(vma,page)	do { flush_kernel_dcache_page(page_address(page)); flush_kernel_icache_page(page_address(page)); } while (0)

#define flush_icache_range(s,e)		do { flush_kernel_dcache_range_asm(s,e); flush_kernel_icache_range_asm(s,e); } while (0)

#define copy_to_user_page(vma, page, vaddr, dst, src, len) \
do { memcpy(dst, src, len); \
     flush_kernel_dcache_range_asm((unsigned long)dst, (unsigned long)dst + len); \
} while (0)
#define copy_from_user_page(vma, page, vaddr, dst, src, len) \
	memcpy(dst, src, len)

static inline void flush_cache_range(struct vm_area_struct *vma,
		unsigned long start, unsigned long end)
{
	int sr3;

	if (!vma->vm_mm->context) {
		BUG();
		return;
	}

	sr3 = mfsp(3);
	if (vma->vm_mm->context == sr3) {
		flush_user_dcache_range(start,end);
		flush_user_icache_range(start,end);
	} else {
		flush_cache_all();
	}
}

/* Simple function to work out if we have an existing address translation
 * for a user space vma. */
static inline pte_t *__translation_exists(struct mm_struct *mm,
					  unsigned long addr)
{
	pgd_t *pgd = pgd_offset(mm, addr);
	pmd_t *pmd;
	pte_t *pte;

	if(pgd_none(*pgd))
		return NULL;

	pmd = pmd_offset(pgd, addr);
	if(pmd_none(*pmd) || pmd_bad(*pmd))
		return NULL;

	pte = pte_offset_map(pmd, addr);

	/* The PA flush mappings show up as pte_none, but they're
	 * valid none the less */
	if(pte_none(*pte) && ((pte_val(*pte) & _PAGE_FLUSH) == 0))
		return NULL;
	return pte;
}
#define	translation_exists(vma, addr)	__translation_exists((vma)->vm_mm, addr)


/* Private function to flush a page from the cache of a non-current
 * process.  cr25 contains the Page Directory of the current user
 * process; we're going to hijack both it and the user space %sr3 to
 * temporarily make the non-current process current.  We have to do
 * this because cache flushing may cause a non-access tlb miss which
 * the handlers have to fill in from the pgd of the non-current
 * process. */
static inline void
flush_user_cache_page_non_current(struct vm_area_struct *vma,
				  unsigned long vmaddr)
{
	/* save the current process space and pgd */
	unsigned long space = mfsp(3), pgd = mfctl(25);

	/* we don't mind taking interrups since they may not
	 * do anything with user space, but we can't
	 * be preempted here */
	preempt_disable();

	/* make us current */
	mtctl(__pa(vma->vm_mm->pgd), 25);
	mtsp(vma->vm_mm->context, 3);

	flush_user_dcache_page(vmaddr);
	if(vma->vm_flags & VM_EXEC)
		flush_user_icache_page(vmaddr);

	/* put the old current process back */
	mtsp(space, 3);
	mtctl(pgd, 25);
	preempt_enable();
}

static inline void
__flush_cache_page(struct vm_area_struct *vma, unsigned long vmaddr)
{
	if (likely(vma->vm_mm->context == mfsp(3))) {
		flush_user_dcache_page(vmaddr);
		if (vma->vm_flags & VM_EXEC)
			flush_user_icache_page(vmaddr);
	} else {
		flush_user_cache_page_non_current(vma, vmaddr);
	}
}

static inline void
flush_cache_page(struct vm_area_struct *vma, unsigned long vmaddr)
{
	BUG_ON(!vma->vm_mm->context);

	if(likely(translation_exists(vma, vmaddr)))
		__flush_cache_page(vma, vmaddr);

}
#endif

