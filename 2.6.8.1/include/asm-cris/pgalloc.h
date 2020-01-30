#ifndef _CRIS_PGALLOC_H
#define _CRIS_PGALLOC_H

#include <asm/page.h>
#include <linux/threads.h>
#include <linux/mm.h>

#define pmd_populate_kernel(mm, pmd, pte) pmd_set(pmd, pte)
#define pmd_populate(mm, pmd, pte) pmd_set(pmd, page_address(pte))

/*
 * Allocate and free page tables.
 */

extern inline pgd_t *pgd_alloc (struct mm_struct *mm)
{
	return (pgd_t *)get_zeroed_page(GFP_KERNEL);
}

extern inline void pgd_free (pgd_t *pgd)
{
	free_page((unsigned long)pgd);
}

extern inline pte_t *pte_alloc_one_kernel(struct mm_struct *mm, unsigned long address)
{
  	pte_t *pte = (pte_t *)__get_free_page(GFP_KERNEL|__GFP_REPEAT);
	if (pte)
		clear_page(pte);
 	return pte;
}

extern inline struct page *pte_alloc_one(struct mm_struct *mm, unsigned long address)
{
	struct page *pte;
	pte = alloc_pages(GFP_KERNEL|__GFP_REPEAT, 0);
	if (pte)
		clear_page(page_address(pte));
	return pte;
}

extern inline void pte_free_kernel(pte_t *pte)
{
	free_page((unsigned long)pte);
}

extern inline void pte_free(struct page *pte)
{
	__free_page(pte);
}


#define __pte_free_tlb(tlb,pte) tlb_remove_page((tlb),(pte))

/*
 * We don't have any real pmd's, and this code never triggers because
 * the pgd will always be present..
 */

#define pmd_alloc_one(mm, addr)    ({ BUG(); ((pmd_t *)2); })
#define pmd_free(x)                do { } while (0)
#define __pmd_free_tlb(tlb,x)      do { } while (0)
#define pgd_populate(mm, pmd, pte) BUG()

#define check_pgt_cache()          do { } while (0)

#endif
