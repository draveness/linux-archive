#ifndef _X86_64_PGALLOC_H
#define _X86_64_PGALLOC_H

#include <asm/processor.h>
#include <asm/fixmap.h>
#include <asm/pda.h>
#include <linux/threads.h>
#include <linux/mm.h>

#define pmd_populate_kernel(mm, pmd, pte) \
		set_pmd(pmd, __pmd(_PAGE_TABLE | __pa(pte)))
#define pgd_populate(mm, pgd, pmd) \
		set_pgd(pgd, __pgd(_PAGE_TABLE | __pa(pmd)))

static inline void pmd_populate(struct mm_struct *mm, pmd_t *pmd, struct page *pte)
{
	set_pmd(pmd, __pmd(_PAGE_TABLE | (page_to_pfn(pte) << PAGE_SHIFT)));
}

extern __inline__ pmd_t *get_pmd(void)
{
	return (pmd_t *)get_zeroed_page(GFP_KERNEL);
}

extern __inline__ void pmd_free(pmd_t *pmd)
{
	BUG_ON((unsigned long)pmd & (PAGE_SIZE-1));
	free_page((unsigned long)pmd);
}

static inline pmd_t *pmd_alloc_one (struct mm_struct *mm, unsigned long addr)
{
	return (pmd_t *)get_zeroed_page(GFP_KERNEL|__GFP_REPEAT);
}

static inline pgd_t *pgd_alloc (struct mm_struct *mm)
{
	return (pgd_t *)get_zeroed_page(GFP_KERNEL|__GFP_REPEAT);
}

static inline void pgd_free (pgd_t *pgd)
{
	BUG_ON((unsigned long)pgd & (PAGE_SIZE-1));
	free_page((unsigned long)pgd);
}

static inline pte_t *pte_alloc_one_kernel(struct mm_struct *mm, unsigned long address)
{
	return (pte_t *)get_zeroed_page(GFP_KERNEL|__GFP_REPEAT);
}

static inline struct page *pte_alloc_one(struct mm_struct *mm, unsigned long address)
{
	void *p = (void *)get_zeroed_page(GFP_KERNEL|__GFP_REPEAT);
	if (!p)
		return NULL;
	return virt_to_page(p);
}

/* Should really implement gc for free page table pages. This could be
   done with a reference count in struct page. */

extern __inline__ void pte_free_kernel(pte_t *pte)
{
	BUG_ON((unsigned long)pte & (PAGE_SIZE-1));
	free_page((unsigned long)pte); 
}

extern inline void pte_free(struct page *pte)
{
	__free_page(pte);
} 

#define __pte_free_tlb(tlb,pte) tlb_remove_page((tlb),(pte))
#define __pmd_free_tlb(tlb,x)   pmd_free(x)

#endif /* _X86_64_PGALLOC_H */
