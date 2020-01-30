/* 
 * Copyright 2002 Andi Kleen, SuSE Labs. 
 * Thanks to Ben LaHaise for precious feedback.
 */ 

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <asm/processor.h>
#include <asm/tlbflush.h>

static spinlock_t cpa_lock = SPIN_LOCK_UNLOCKED;
static struct list_head df_list = LIST_HEAD_INIT(df_list);


pte_t *lookup_address(unsigned long address) 
{ 
	pgd_t *pgd = pgd_offset_k(address); 
	pmd_t *pmd;
	if (pgd_none(*pgd))
		return NULL;
	pmd = pmd_offset(pgd, address); 	       
	if (pmd_none(*pmd))
		return NULL;
	if (pmd_large(*pmd))
		return (pte_t *)pmd;
        return pte_offset_kernel(pmd, address);
} 

static struct page *split_large_page(unsigned long address, pgprot_t prot)
{ 
	int i; 
	unsigned long addr;
	struct page *base;
	pte_t *pbase;

	spin_unlock_irq(&cpa_lock);
	base = alloc_pages(GFP_KERNEL, 0);
	spin_lock_irq(&cpa_lock);
	if (!base) 
		return NULL;

	address = __pa(address);
	addr = address & LARGE_PAGE_MASK; 
	pbase = (pte_t *)page_address(base);
	for (i = 0; i < PTRS_PER_PTE; i++, addr += PAGE_SIZE) {
		pbase[i] = pfn_pte(addr >> PAGE_SHIFT, 
				   addr == address ? prot : PAGE_KERNEL);
	}
	return base;
} 

static void flush_kernel_map(void *dummy) 
{ 
	/* Could use CLFLUSH here if the CPU supports it (Hammer,P4) */
	if (boot_cpu_data.x86_model >= 4) 
		asm volatile("wbinvd":::"memory"); 
	/* Flush all to work around Errata in early athlons regarding 
	 * large page flushing. 
	 */
	__flush_tlb_all(); 	
}

static void set_pmd_pte(pte_t *kpte, unsigned long address, pte_t pte) 
{ 
	struct page *page;
	unsigned long flags;

	set_pte_atomic(kpte, pte); 	/* change init_mm */
	if (PTRS_PER_PMD > 1)
		return;

	spin_lock_irqsave(&pgd_lock, flags);
	for (page = pgd_list; page; page = (struct page *)page->index) {
		pgd_t *pgd;
		pmd_t *pmd;
		pgd = (pgd_t *)page_address(page) + pgd_index(address);
		pmd = pmd_offset(pgd, address);
		set_pte_atomic((pte_t *)pmd, pte);
	}
	spin_unlock_irqrestore(&pgd_lock, flags);
}

/* 
 * No more special protections in this 2/4MB area - revert to a
 * large page again. 
 */
static inline void revert_page(struct page *kpte_page, unsigned long address)
{
	pte_t *linear = (pte_t *) 
		pmd_offset(pgd_offset(&init_mm, address), address);
	set_pmd_pte(linear,  address,
		    pfn_pte((__pa(address) & LARGE_PAGE_MASK) >> PAGE_SHIFT,
			    PAGE_KERNEL_LARGE));
}

static int
__change_page_attr(struct page *page, pgprot_t prot)
{ 
	pte_t *kpte; 
	unsigned long address;
	struct page *kpte_page;

#ifdef CONFIG_HIGHMEM
	if (page >= highmem_start_page) 
		BUG(); 
#endif
	address = (unsigned long)page_address(page);

	kpte = lookup_address(address);
	if (!kpte)
		return -EINVAL;
	kpte_page = virt_to_page(((unsigned long)kpte) & PAGE_MASK);
	if (pgprot_val(prot) != pgprot_val(PAGE_KERNEL)) { 
		if ((pte_val(*kpte) & _PAGE_PSE) == 0) { 
			pte_t old = *kpte;
			pte_t standard = mk_pte(page, PAGE_KERNEL); 
			set_pte_atomic(kpte, mk_pte(page, prot)); 
			if (pte_same(old,standard))
				get_page(kpte_page);
		} else {
			struct page *split = split_large_page(address, prot); 
			if (!split)
				return -ENOMEM;
			get_page(kpte_page);
			set_pmd_pte(kpte,address,mk_pte(split, PAGE_KERNEL));
		}	
	} else if ((pte_val(*kpte) & _PAGE_PSE) == 0) { 
		set_pte_atomic(kpte, mk_pte(page, PAGE_KERNEL));
		__put_page(kpte_page);
	}

	if (cpu_has_pse && (page_count(kpte_page) == 1)) {
		list_add(&kpte_page->lru, &df_list);
		revert_page(kpte_page, address);
	} 
	return 0;
} 

static inline void flush_map(void)
{
	on_each_cpu(flush_kernel_map, NULL, 1, 1);
}

/*
 * Change the page attributes of an page in the linear mapping.
 *
 * This should be used when a page is mapped with a different caching policy
 * than write-back somewhere - some CPUs do not like it when mappings with
 * different caching policies exist. This changes the page attributes of the
 * in kernel linear mapping too.
 * 
 * The caller needs to ensure that there are no conflicting mappings elsewhere.
 * This function only deals with the kernel linear map.
 * 
 * Caller must call global_flush_tlb() after this.
 */
int change_page_attr(struct page *page, int numpages, pgprot_t prot)
{
	int err = 0; 
	int i; 
	unsigned long flags;

	spin_lock_irqsave(&cpa_lock, flags);
	for (i = 0; i < numpages; i++, page++) { 
		err = __change_page_attr(page, prot);
		if (err) 
			break; 
	} 	
	spin_unlock_irqrestore(&cpa_lock, flags);
	return err;
}

void global_flush_tlb(void)
{ 
	LIST_HEAD(l);
	struct list_head* n;

	BUG_ON(irqs_disabled());

	spin_lock_irq(&cpa_lock);
	list_splice_init(&df_list, &l);
	spin_unlock_irq(&cpa_lock);
	flush_map();
	n = l.next;
	while (n != &l) {
		struct page *pg = list_entry(n, struct page, lru);
		n = n->next;
		__free_page(pg);
	}
} 

#ifdef CONFIG_DEBUG_PAGEALLOC
void kernel_map_pages(struct page *page, int numpages, int enable)
{
	if (PageHighMem(page))
		return;
	/* the return value is ignored - the calls cannot fail,
	 * large pages are disabled at boot time.
	 */
	change_page_attr(page, numpages, enable ? PAGE_KERNEL : __pgprot(0));
	/* we should perform an IPI and flush all tlbs,
	 * but that can deadlock->flush only current cpu.
	 */
	__flush_tlb_all();
}
EXPORT_SYMBOL(kernel_map_pages);
#endif

EXPORT_SYMBOL(change_page_attr);
EXPORT_SYMBOL(global_flush_tlb);
