/*
 * IA-64 Huge TLB Page Support for Kernel.
 *
 * Copyright (C) 2002-2004 Rohit Seth <rohit.seth@intel.com>
 * Copyright (C) 2003-2004 Ken Chen <kenneth.w.chen@intel.com>
 *
 * Sep, 2003: add numa support
 * Feb, 2004: dynamic hugetlb page size via boot parameter
 */

#include <linux/config.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <asm/mman.h>
#include <asm/pgalloc.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

unsigned int hpage_shift=HPAGE_SHIFT_DEFAULT;

static pte_t *
huge_pte_alloc (struct mm_struct *mm, unsigned long addr)
{
	unsigned long taddr = htlbpage_to_page(addr);
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte = NULL;

	pgd = pgd_offset(mm, taddr);
	pmd = pmd_alloc(mm, pgd, taddr);
	if (pmd)
		pte = pte_alloc_map(mm, pmd, taddr);
	return pte;
}

static pte_t *
huge_pte_offset (struct mm_struct *mm, unsigned long addr)
{
	unsigned long taddr = htlbpage_to_page(addr);
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte = NULL;

	pgd = pgd_offset(mm, taddr);
	if (pgd_present(*pgd)) {
		pmd = pmd_offset(pgd, taddr);
		if (pmd_present(*pmd))
			pte = pte_offset_map(pmd, taddr);
	}

	return pte;
}

#define mk_pte_huge(entry) { pte_val(entry) |= _PAGE_P; }

static void
set_huge_pte (struct mm_struct *mm, struct vm_area_struct *vma,
	      struct page *page, pte_t * page_table, int write_access)
{
	pte_t entry;

	mm->rss += (HPAGE_SIZE / PAGE_SIZE);
	if (write_access) {
		entry =
		    pte_mkwrite(pte_mkdirty(mk_pte(page, vma->vm_page_prot)));
	} else
		entry = pte_wrprotect(mk_pte(page, vma->vm_page_prot));
	entry = pte_mkyoung(entry);
	mk_pte_huge(entry);
	set_pte(page_table, entry);
	return;
}
/*
 * This function checks for proper alignment of input addr and len parameters.
 */
int is_aligned_hugepage_range(unsigned long addr, unsigned long len)
{
	if (len & ~HPAGE_MASK)
		return -EINVAL;
	if (addr & ~HPAGE_MASK)
		return -EINVAL;
	if (REGION_NUMBER(addr) != REGION_HPAGE)
		return -EINVAL;

	return 0;
}

int copy_hugetlb_page_range(struct mm_struct *dst, struct mm_struct *src,
			struct vm_area_struct *vma)
{
	pte_t *src_pte, *dst_pte, entry;
	struct page *ptepage;
	unsigned long addr = vma->vm_start;
	unsigned long end = vma->vm_end;

	while (addr < end) {
		dst_pte = huge_pte_alloc(dst, addr);
		if (!dst_pte)
			goto nomem;
		src_pte = huge_pte_offset(src, addr);
		entry = *src_pte;
		ptepage = pte_page(entry);
		get_page(ptepage);
		set_pte(dst_pte, entry);
		dst->rss += (HPAGE_SIZE / PAGE_SIZE);
		addr += HPAGE_SIZE;
	}
	return 0;
nomem:
	return -ENOMEM;
}

int
follow_hugetlb_page(struct mm_struct *mm, struct vm_area_struct *vma,
		    struct page **pages, struct vm_area_struct **vmas,
		    unsigned long *st, int *length, int i)
{
	pte_t *ptep, pte;
	unsigned long start = *st;
	unsigned long pstart;
	int len = *length;
	struct page *page;

	do {
		pstart = start & HPAGE_MASK;
		ptep = huge_pte_offset(mm, start);
		pte = *ptep;

back1:
		page = pte_page(pte);
		if (pages) {
			page += ((start & ~HPAGE_MASK) >> PAGE_SHIFT);
			get_page(page);
			pages[i] = page;
		}
		if (vmas)
			vmas[i] = vma;
		i++;
		len--;
		start += PAGE_SIZE;
		if (((start & HPAGE_MASK) == pstart) && len &&
				(start < vma->vm_end))
			goto back1;
	} while (len && start < vma->vm_end);
	*length = len;
	*st = start;
	return i;
}

struct page *follow_huge_addr(struct mm_struct *mm, unsigned long addr, int write)
{
	struct page *page;
	pte_t *ptep;

	if (REGION_NUMBER(addr) != REGION_HPAGE)
		return ERR_PTR(-EINVAL);

	ptep = huge_pte_offset(mm, addr);
	if (!ptep || pte_none(*ptep))
		return NULL;
	page = pte_page(*ptep);
	page += ((addr & ~HPAGE_MASK) >> PAGE_SHIFT);
	return page;
}
int pmd_huge(pmd_t pmd)
{
	return 0;
}
struct page *
follow_huge_pmd(struct mm_struct *mm, unsigned long address, pmd_t *pmd, int write)
{
	return NULL;
}

/*
 * Same as generic free_pgtables(), except constant PGDIR_* and pgd_offset
 * are hugetlb region specific.
 */
void hugetlb_free_pgtables(struct mmu_gather *tlb, struct vm_area_struct *prev,
	unsigned long start, unsigned long end)
{
	unsigned long first = start & HUGETLB_PGDIR_MASK;
	unsigned long last = end + HUGETLB_PGDIR_SIZE - 1;
	unsigned long start_index, end_index;
	struct mm_struct *mm = tlb->mm;

	if (!prev) {
		prev = mm->mmap;
		if (!prev)
			goto no_mmaps;
		if (prev->vm_end > start) {
			if (last > prev->vm_start)
				last = prev->vm_start;
			goto no_mmaps;
		}
	}
	for (;;) {
		struct vm_area_struct *next = prev->vm_next;

		if (next) {
			if (next->vm_start < start) {
				prev = next;
				continue;
			}
			if (last > next->vm_start)
				last = next->vm_start;
		}
		if (prev->vm_end > first)
			first = prev->vm_end + HUGETLB_PGDIR_SIZE - 1;
		break;
	}
no_mmaps:
	if (last < first)	/* for arches with discontiguous pgd indices */
		return;
	/*
	 * If the PGD bits are not consecutive in the virtual address, the
	 * old method of shifting the VA >> by PGDIR_SHIFT doesn't work.
	 */

	start_index = pgd_index(htlbpage_to_page(first));
	end_index = pgd_index(htlbpage_to_page(last));

	if (end_index > start_index) {
		clear_page_tables(tlb, start_index, end_index - start_index);
	}
}

void unmap_hugepage_range(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long address;
	pte_t *pte;
	struct page *page;

	BUG_ON(start & (HPAGE_SIZE - 1));
	BUG_ON(end & (HPAGE_SIZE - 1));

	for (address = start; address < end; address += HPAGE_SIZE) {
		pte = huge_pte_offset(mm, address);
		if (pte_none(*pte))
			continue;
		page = pte_page(*pte);
		put_page(page);
		pte_clear(pte);
	}
	mm->rss -= (end - start) >> PAGE_SHIFT;
	flush_tlb_range(vma, start, end);
}

int hugetlb_prefault(struct address_space *mapping, struct vm_area_struct *vma)
{
	struct mm_struct *mm = current->mm;
	unsigned long addr;
	int ret = 0;

	BUG_ON(vma->vm_start & ~HPAGE_MASK);
	BUG_ON(vma->vm_end & ~HPAGE_MASK);

	spin_lock(&mm->page_table_lock);
	for (addr = vma->vm_start; addr < vma->vm_end; addr += HPAGE_SIZE) {
		unsigned long idx;
		pte_t *pte = huge_pte_alloc(mm, addr);
		struct page *page;

		if (!pte) {
			ret = -ENOMEM;
			goto out;
		}
		if (!pte_none(*pte))
			continue;

		idx = ((addr - vma->vm_start) >> HPAGE_SHIFT)
			+ (vma->vm_pgoff >> (HPAGE_SHIFT - PAGE_SHIFT));
		page = find_get_page(mapping, idx);
		if (!page) {
			/* charge the fs quota first */
			if (hugetlb_get_quota(mapping)) {
				ret = -ENOMEM;
				goto out;
			}
			page = alloc_huge_page();
			if (!page) {
				hugetlb_put_quota(mapping);
				ret = -ENOMEM;
				goto out;
			}
			ret = add_to_page_cache(page, mapping, idx, GFP_ATOMIC);
			if (! ret) {
				unlock_page(page);
			} else {
				hugetlb_put_quota(mapping);
				page_cache_release(page);
				goto out;
			}
		}
		set_huge_pte(mm, vma, page, pte, vma->vm_flags & VM_WRITE);
	}
out:
	spin_unlock(&mm->page_table_lock);
	return ret;
}

unsigned long hugetlb_get_unmapped_area(struct file *file, unsigned long addr, unsigned long len,
		unsigned long pgoff, unsigned long flags)
{
	struct vm_area_struct *vmm;

	if (len > RGN_MAP_LIMIT)
		return -ENOMEM;
	if (len & ~HPAGE_MASK)
		return -EINVAL;
	/* This code assumes that REGION_HPAGE != 0. */
	if ((REGION_NUMBER(addr) != REGION_HPAGE) || (addr & (HPAGE_SIZE - 1)))
		addr = HPAGE_REGION_BASE;
	else
		addr = ALIGN(addr, HPAGE_SIZE);
	for (vmm = find_vma(current->mm, addr); ; vmm = vmm->vm_next) {
		/* At this point:  (!vmm || addr < vmm->vm_end). */
		if (REGION_OFFSET(addr) + len > RGN_MAP_LIMIT)
			return -ENOMEM;
		if (!vmm || (addr + len) <= vmm->vm_start)
			return addr;
		addr = ALIGN(vmm->vm_end, HPAGE_SIZE);
	}
}

static int __init hugetlb_setup_sz(char *str)
{
	u64 tr_pages;
	unsigned long long size;

	if (ia64_pal_vm_page_size(&tr_pages, NULL) != 0)
		/*
		 * shouldn't happen, but just in case.
		 */
		tr_pages = 0x15557000UL;

	size = memparse(str, &str);
	if (*str || (size & (size-1)) || !(tr_pages & size) ||
		size <= PAGE_SIZE ||
		size >= (1UL << PAGE_SHIFT << MAX_ORDER)) {
		printk(KERN_WARNING "Invalid huge page size specified\n");
		return 1;
	}

	hpage_shift = __ffs(size);
	/*
	 * boot cpu already executed ia64_mmu_init, and has HPAGE_SHIFT_DEFAULT
	 * override here with new page shift.
	 */
	ia64_set_rr(HPAGE_REGION_BASE, hpage_shift << 2);
	return 1;
}
__setup("hugepagesz=", hugetlb_setup_sz);
