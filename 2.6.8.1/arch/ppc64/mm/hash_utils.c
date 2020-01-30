/*
 * PowerPC64 port by Mike Corrigan and Dave Engebretsen
 *   {mikejc|engebret}@us.ibm.com
 *
 *    Copyright (c) 2000 Mike Corrigan <mikejc@us.ibm.com>
 *
 * SMP scalability work:
 *    Copyright (C) 2001 Anton Blanchard <anton@au.ibm.com>, IBM
 * 
 *    Module name: htab.c
 *
 *    Description:
 *      PowerPC Hashed Page Table functions
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/sysctl.h>
#include <linux/ctype.h>
#include <linux/cache.h>
#include <linux/init.h>

#include <asm/ppcdebug.h>
#include <asm/processor.h>
#include <asm/pgtable.h>
#include <asm/mmu.h>
#include <asm/mmu_context.h>
#include <asm/page.h>
#include <asm/types.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/naca.h>
#include <asm/machdep.h>
#include <asm/lmb.h>
#include <asm/abs_addr.h>
#include <asm/tlbflush.h>
#include <asm/io.h>
#include <asm/eeh.h>
#include <asm/tlb.h>
#include <asm/cacheflush.h>
#include <asm/cputable.h>
#include <asm/abs_addr.h>

/*
 * Note:  pte   --> Linux PTE
 *        HPTE  --> PowerPC Hashed Page Table Entry
 *
 * Execution context:
 *   htab_initialize is called with the MMU off (of course), but
 *   the kernel has been copied down to zero so it can directly
 *   reference global data.  At this point it is very difficult
 *   to print debug info.
 *
 */

#ifdef CONFIG_PMAC_DART
extern unsigned long dart_tablebase;
#endif /* CONFIG_PMAC_DART */

HTAB htab_data = {NULL, 0, 0, 0, 0};

extern unsigned long _SDR1;

#define KB (1024)
#define MB (1024*KB)

static inline void loop_forever(void)
{
	volatile unsigned long x = 1;
	for(;x;x|=1)
		;
}

#ifdef CONFIG_PPC_PSERIES
static inline void create_pte_mapping(unsigned long start, unsigned long end,
				      unsigned long mode, int large)
{
	unsigned long addr;
	unsigned int step;

	if (large)
		step = 16*MB;
	else
		step = 4*KB;

	for (addr = start; addr < end; addr += step) {
		unsigned long vpn, hash, hpteg;
		unsigned long vsid = get_kernel_vsid(addr);
		unsigned long va = (vsid << 28) | (addr & 0xfffffff);
		int ret;

		if (large)
			vpn = va >> LARGE_PAGE_SHIFT;
		else
			vpn = va >> PAGE_SHIFT;

		hash = hpt_hash(vpn, large);

		hpteg = ((hash & htab_data.htab_hash_mask)*HPTES_PER_GROUP);

		if (systemcfg->platform == PLATFORM_PSERIES_LPAR)
			ret = pSeries_lpar_hpte_insert(hpteg, va,
				virt_to_abs(addr) >> PAGE_SHIFT,
				0, mode, 1, large);
		else
			ret = pSeries_hpte_insert(hpteg, va,
				virt_to_abs(addr) >> PAGE_SHIFT,
				0, mode, 1, large);

		if (ret == -1) {
			ppc64_terminate_msg(0x20, "create_pte_mapping");
			loop_forever();
		}
	}
}

void __init htab_initialize(void)
{
	unsigned long table, htab_size_bytes;
	unsigned long pteg_count;
	unsigned long mode_rw;
	int i, use_largepages = 0;

	/*
	 * Calculate the required size of the htab.  We want the number of
	 * PTEGs to equal one half the number of real pages.
	 */ 
	htab_size_bytes = 1UL << naca->pftSize;
	pteg_count = htab_size_bytes >> 7;

	/* For debug, make the HTAB 1/8 as big as it normally would be. */
	ifppcdebug(PPCDBG_HTABSIZE) {
		pteg_count >>= 3;
		htab_size_bytes = pteg_count << 7;
	}

	htab_data.htab_num_ptegs = pteg_count;
	htab_data.htab_hash_mask = pteg_count - 1;

	if (systemcfg->platform == PLATFORM_PSERIES ||
	    systemcfg->platform == PLATFORM_POWERMAC) {
		/* Find storage for the HPT.  Must be contiguous in
		 * the absolute address space.
		 */
		table = lmb_alloc(htab_size_bytes, htab_size_bytes);
		if ( !table ) {
			ppc64_terminate_msg(0x20, "hpt space");
			loop_forever();
		}
		htab_data.htab = abs_to_virt(table);

		/* htab absolute addr + encoded htabsize */
		_SDR1 = table + __ilog2(pteg_count) - 11;

		/* Initialize the HPT with no entries */
		memset((void *)table, 0, htab_size_bytes);
	} else {
		/* Using a hypervisor which owns the htab */
		htab_data.htab = NULL;
		_SDR1 = 0; 
	}

	mode_rw = _PAGE_ACCESSED | _PAGE_COHERENT | PP_RWXX;

	/* On U3 based machines, we need to reserve the DART area and
	 * _NOT_ map it to avoid cache paradoxes as it's remapped non
	 * cacheable later on
	 */
	if (cur_cpu_spec->cpu_features & CPU_FTR_16M_PAGE)
		use_largepages = 1;

	/* add all physical memory to the bootmem map */
	for (i=0; i < lmb.memory.cnt; i++) {
		unsigned long base, size;

		base = lmb.memory.region[i].physbase + KERNELBASE;
		size = lmb.memory.region[i].size;

#ifdef CONFIG_PMAC_DART
		/* Do not map the DART space. Fortunately, it will be aligned
		 * in such a way that it will not cross two lmb regions and will
		 * fit within a single 16Mb page.
		 * The DART space is assumed to be a full 16Mb region even if we
		 * only use 2Mb of that space. We will use more of it later for
		 * AGP GART. We have to use a full 16Mb large page.
		 */
		if (dart_tablebase != 0 && dart_tablebase >= base
		    && dart_tablebase < (base + size)) {
			if (base != dart_tablebase)
				create_pte_mapping(base, dart_tablebase, mode_rw,
						   use_largepages);
			if ((base + size) > (dart_tablebase + 16*MB))
				create_pte_mapping(dart_tablebase + 16*MB, base + size,
						   mode_rw, use_largepages);
			continue;
		}
#endif /* CONFIG_PMAC_DART */
		create_pte_mapping(base, base + size, mode_rw, use_largepages);
	}
}
#undef KB
#undef MB
#endif

/*
 * Called by asm hashtable.S for doing lazy icache flush
 */
unsigned int hash_page_do_lazy_icache(unsigned int pp, pte_t pte, int trap)
{
	struct page *page;

#define PPC64_HWNOEXEC (1 << 2)

	if (!pfn_valid(pte_pfn(pte)))
		return pp;

	page = pte_page(pte);

	/* page is dirty */
	if (!test_bit(PG_arch_1, &page->flags) && !PageReserved(page)) {
		if (trap == 0x400) {
			__flush_dcache_icache(page_address(page));
			set_bit(PG_arch_1, &page->flags);
		} else
			pp |= PPC64_HWNOEXEC;
	}
	return pp;
}

/*
 * Called by asm hashtable.S in case of critical insert failure
 */
void htab_insert_failure(void)
{
	panic("hash_page: pte_insert failed\n");
}

int hash_page(unsigned long ea, unsigned long access, unsigned long trap)
{
	void *pgdir;
	unsigned long vsid;
	struct mm_struct *mm;
	pte_t *ptep;
	int ret;
	int user_region = 0;
	int local = 0;
	cpumask_t tmp;

	/* Check for invalid addresses. */
	if (!IS_VALID_EA(ea))
		return 1;

 	switch (REGION_ID(ea)) {
	case USER_REGION_ID:
		user_region = 1;
		mm = current->mm;
		if (mm == NULL)
			return 1;

		vsid = get_vsid(mm->context.id, ea);
		break;
	case IO_REGION_ID:
		mm = &ioremap_mm;
		vsid = get_kernel_vsid(ea);
		break;
	case VMALLOC_REGION_ID:
		mm = &init_mm;
		vsid = get_kernel_vsid(ea);
		break;
#if 0
	case EEH_REGION_ID:
		/*
		 * Should only be hit if there is an access to MMIO space
		 * which is protected by EEH.
		 * Send the problem up to do_page_fault 
		 */
	case KERNEL_REGION_ID:
		/*
		 * Should never get here - entire 0xC0... region is bolted.
		 * Send the problem up to do_page_fault 
		 */
#endif
	default:
		/* Not a valid range
		 * Send the problem up to do_page_fault 
		 */
		return 1;
		break;
	}

	pgdir = mm->pgd;

	if (pgdir == NULL)
		return 1;

	tmp = cpumask_of_cpu(smp_processor_id());
	if (user_region && cpus_equal(mm->cpu_vm_mask, tmp))
		local = 1;

	/* Is this a huge page ? */
	if (unlikely(in_hugepage_area(mm->context, ea)))
		ret = hash_huge_page(mm, access, ea, vsid, local);
	else {
		ptep = find_linux_pte(pgdir, ea);
		if (ptep == NULL)
			return 1;
		ret = __hash_page(ea, access, vsid, ptep, trap, local);
	}

	return ret;
}

void flush_hash_page(unsigned long context, unsigned long ea, pte_t pte,
		     int local)
{
	unsigned long vsid, vpn, va, hash, secondary, slot;

	/* XXX fix for large ptes */
	unsigned long large = 0;

	if ((ea >= USER_START) && (ea <= USER_END))
		vsid = get_vsid(context, ea);
	else
		vsid = get_kernel_vsid(ea);

	va = (vsid << 28) | (ea & 0x0fffffff);
	if (large)
		vpn = va >> LARGE_PAGE_SHIFT;
	else
		vpn = va >> PAGE_SHIFT;
	hash = hpt_hash(vpn, large);
	secondary = (pte_val(pte) & _PAGE_SECONDARY) >> 15;
	if (secondary)
		hash = ~hash;
	slot = (hash & htab_data.htab_hash_mask) * HPTES_PER_GROUP;
	slot += (pte_val(pte) & _PAGE_GROUP_IX) >> 12;

	ppc_md.hpte_invalidate(slot, va, large, local);
}

void flush_hash_range(unsigned long context, unsigned long number, int local)
{
	if (ppc_md.flush_hash_range) {
		ppc_md.flush_hash_range(context, number, local);
	} else {
		int i;
		struct ppc64_tlb_batch *batch = &__get_cpu_var(ppc64_tlb_batch);

		for (i = 0; i < number; i++)
			flush_hash_page(context, batch->addr[i], batch->pte[i],
					local);
	}
}

static inline void make_bl(unsigned int *insn_addr, void *func)
{
	unsigned long funcp = *((unsigned long *)func);
	int offset = funcp - (unsigned long)insn_addr;

	*insn_addr = (unsigned int)(0x48000001 | (offset & 0x03fffffc));
	flush_icache_range((unsigned long)insn_addr, 4+
			   (unsigned long)insn_addr);
}

void __init htab_finish_init(void)
{
	extern unsigned int *htab_call_hpte_insert1;
	extern unsigned int *htab_call_hpte_insert2;
	extern unsigned int *htab_call_hpte_remove;
	extern unsigned int *htab_call_hpte_updatepp;

	make_bl(htab_call_hpte_insert1, ppc_md.hpte_insert);
	make_bl(htab_call_hpte_insert2, ppc_md.hpte_insert);
	make_bl(htab_call_hpte_remove, ppc_md.hpte_remove);
	make_bl(htab_call_hpte_updatepp, ppc_md.hpte_updatepp);
}
