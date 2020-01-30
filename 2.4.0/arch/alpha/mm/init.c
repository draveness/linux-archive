/*
 *  linux/arch/alpha/mm/init.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 */

/* 2.3.x zone allocator, 1999 Andrea Arcangeli <andrea@suse.de> */

#include <linux/config.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/bootmem.h> /* max_low_pfn */
#include <linux/vmalloc.h>
#ifdef CONFIG_BLK_DEV_INITRD
#include <linux/blk.h>
#endif

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/hwrpb.h>
#include <asm/dma.h>
#include <asm/mmu_context.h>
#include <asm/console.h>

static unsigned long totalram_pages;

extern void die_if_kernel(char *,struct pt_regs *,long);

struct thread_struct original_pcb;

#ifndef CONFIG_SMP
struct pgtable_cache_struct quicklists;
#endif

void
__bad_pmd(pgd_t *pgd)
{
	printk("Bad pgd in pmd_alloc: %08lx\n", pgd_val(*pgd));
	pgd_set(pgd, BAD_PAGETABLE);
}

void
__bad_pte(pmd_t *pmd)
{
	printk("Bad pmd in pte_alloc: %08lx\n", pmd_val(*pmd));
	pmd_set(pmd, (pte_t *) BAD_PAGETABLE);
}

pgd_t *
get_pgd_slow(void)
{
	pgd_t *ret, *init;

	ret = (pgd_t *)__get_free_page(GFP_KERNEL);
	init = pgd_offset(&init_mm, 0UL);
	if (ret) {
		clear_page(ret);
#ifdef CONFIG_ALPHA_LARGE_VMALLOC
		memcpy (ret + USER_PTRS_PER_PGD, init + USER_PTRS_PER_PGD,
			(PTRS_PER_PGD - USER_PTRS_PER_PGD - 1)*sizeof(pgd_t));
#else
		pgd_val(ret[PTRS_PER_PGD-2]) = pgd_val(init[PTRS_PER_PGD-2]);
#endif

		/* The last PGD entry is the VPTB self-map.  */
		pgd_val(ret[PTRS_PER_PGD-1])
		  = pte_val(mk_pte(virt_to_page(ret), PAGE_KERNEL));
	}
	return ret;
}

pmd_t *
get_pmd_slow(pgd_t *pgd, unsigned long offset)
{
	pmd_t *pmd;

	pmd = (pmd_t *) __get_free_page(GFP_KERNEL);
	if (pgd_none(*pgd)) {
		if (pmd) {
			clear_page((void *)pmd);
			pgd_set(pgd, pmd);
			return pmd + offset;
		}
		pgd_set(pgd, BAD_PAGETABLE);
		return NULL;
	}
	free_page((unsigned long)pmd);
	if (pgd_bad(*pgd)) {
		__bad_pmd(pgd);
		return NULL;
	}
	return (pmd_t *) pgd_page(*pgd) + offset;
}

pte_t *
get_pte_slow(pmd_t *pmd, unsigned long offset)
{
	pte_t *pte;

	pte = (pte_t *) __get_free_page(GFP_KERNEL);
	if (pmd_none(*pmd)) {
		if (pte) {
			clear_page((void *)pte);
			pmd_set(pmd, pte);
			return pte + offset;
		}
		pmd_set(pmd, (pte_t *) BAD_PAGETABLE);
		return NULL;
	}
	free_page((unsigned long)pte);
	if (pmd_bad(*pmd)) {
		__bad_pte(pmd);
		return NULL;
	}
	return (pte_t *) pmd_page(*pmd) + offset;
}

int do_check_pgt_cache(int low, int high)
{
	int freed = 0;
        if(pgtable_cache_size > high) {
                do {
                        if(pgd_quicklist)
                                free_pgd_slow(get_pgd_fast()), freed++;
                        if(pmd_quicklist)
                                free_pmd_slow(get_pmd_fast()), freed++;
                        if(pte_quicklist)
                                free_pte_slow(get_pte_fast()), freed++;
                } while(pgtable_cache_size > low);
        }
        return freed;
}

/*
 * BAD_PAGE is the page that is used for page faults when linux
 * is out-of-memory. Older versions of linux just did a
 * do_exit(), but using this instead means there is less risk
 * for a process dying in kernel mode, possibly leaving an inode
 * unused etc..
 *
 * BAD_PAGETABLE is the accompanying page-table: it is initialized
 * to point to BAD_PAGE entries.
 *
 * ZERO_PAGE is a special page that is used for zero-initialized
 * data and COW.
 */
pmd_t *
__bad_pagetable(void)
{
	memset((void *) EMPTY_PGT, 0, PAGE_SIZE);
	return (pmd_t *) EMPTY_PGT;
}

pte_t
__bad_page(void)
{
	memset((void *) EMPTY_PGE, 0, PAGE_SIZE);
	return pte_mkdirty(mk_pte(virt_to_page(EMPTY_PGE), PAGE_SHARED));
}

void
show_mem(void)
{
	long i,free = 0,total = 0,reserved = 0;
	long shared = 0, cached = 0;

	printk("\nMem-info:\n");
	show_free_areas();
	printk("Free swap:       %6dkB\n",nr_swap_pages<<(PAGE_SHIFT-10));
	i = max_mapnr;
	while (i-- > 0) {
		total++;
		if (PageReserved(mem_map+i))
			reserved++;
		else if (PageSwapCache(mem_map+i))
			cached++;
		else if (!page_count(mem_map+i))
			free++;
		else
			shared += atomic_read(&mem_map[i].count) - 1;
	}
	printk("%ld pages of RAM\n",total);
	printk("%ld free pages\n",free);
	printk("%ld reserved pages\n",reserved);
	printk("%ld pages shared\n",shared);
	printk("%ld pages swap cached\n",cached);
	printk("%ld pages in page table cache\n",pgtable_cache_size);
	show_buffers();
}

static inline unsigned long
load_PCB(struct thread_struct * pcb)
{
	register unsigned long sp __asm__("$30");
	pcb->ksp = sp;
	return __reload_thread(pcb);
}

/* Set up initial PCB, VPTB, and other such nicities.  */

static inline void
switch_to_system_map(void)
{
	unsigned long newptbr;
	unsigned long original_pcb_ptr;

	/* Initialize the kernel's page tables.  Linux puts the vptb in
	   the last slot of the L1 page table.  */
	memset(swapper_pg_dir, 0, PAGE_SIZE);
	newptbr = ((unsigned long) swapper_pg_dir - PAGE_OFFSET) >> PAGE_SHIFT;
	pgd_val(swapper_pg_dir[1023]) =
		(newptbr << 32) | pgprot_val(PAGE_KERNEL);

	/* Set the vptb.  This is often done by the bootloader, but 
	   shouldn't be required.  */
	if (hwrpb->vptb != 0xfffffffe00000000) {
		wrvptptr(0xfffffffe00000000);
		hwrpb->vptb = 0xfffffffe00000000;
		hwrpb_update_checksum(hwrpb);
	}

	/* Also set up the real kernel PCB while we're at it.  */
	init_task.thread.ptbr = newptbr;
	init_task.thread.pal_flags = 1;	/* set FEN, clear everything else */
	init_task.thread.flags = 0;
	original_pcb_ptr = load_PCB(&init_task.thread);
	tbia();

	/* Save off the contents of the original PCB so that we can
	   restore the original console's page tables for a clean reboot.

	   Note that the PCB is supposed to be a physical address, but
	   since KSEG values also happen to work, folks get confused.
	   Check this here.  */

	if (original_pcb_ptr < PAGE_OFFSET) {
		original_pcb_ptr = (unsigned long)
			phys_to_virt(original_pcb_ptr);
	}
	original_pcb = *(struct thread_struct *) original_pcb_ptr;
}

int callback_init_done;

void * __init
callback_init(void * kernel_end)
{
	struct crb_struct * crb;
	pgd_t *pgd;
	pmd_t *pmd;
	void *two_pages;

	/* Starting at the HWRPB, locate the CRB. */
	crb = (struct crb_struct *)((char *)hwrpb + hwrpb->crb_offset);

	if (alpha_using_srm) {
		/* Tell the console whither it is to be remapped. */
		if (srm_fixup(VMALLOC_START, (unsigned long)hwrpb))
			__halt();		/* "We're boned."  --Bender */

		/* Edit the procedure descriptors for DISPATCH and FIXUP. */
		crb->dispatch_va = (struct procdesc_struct *)
			(VMALLOC_START + (unsigned long)crb->dispatch_va
			 - crb->map[0].va);
		crb->fixup_va = (struct procdesc_struct *)
			(VMALLOC_START + (unsigned long)crb->fixup_va
			 - crb->map[0].va);
	}

	switch_to_system_map();

	/* Allocate one PGD and one PMD.  In the case of SRM, we'll need
	   these to actually remap the console.  There is an assumption
	   here that only one of each is needed, and this allows for 8MB.
	   Currently (late 1999), big consoles are still under 4MB.

	   In the case of not SRM, but not CONFIG_ALPHA_LARGE_VMALLOC,
	   we need to allocate the PGD we use for vmalloc before we start
	   forking other tasks.  */

	two_pages = (void *)
	  (((unsigned long)kernel_end + ~PAGE_MASK) & PAGE_MASK);
	kernel_end = two_pages + 2*PAGE_SIZE;
	memset(two_pages, 0, 2*PAGE_SIZE);

	pgd = pgd_offset_k(VMALLOC_START);
	pgd_set(pgd, (pmd_t *)two_pages);
	pmd = pmd_offset(pgd, VMALLOC_START);
	pmd_set(pmd, (pte_t *)(two_pages + PAGE_SIZE));

	if (alpha_using_srm) {
		static struct vm_struct console_remap_vm;
		unsigned long vaddr = VMALLOC_START;
		long i, j;

		/* Set up the third level PTEs and update the virtual
		   addresses of the CRB entries.  */
		for (i = 0; i < crb->map_entries; ++i) {
			unsigned long paddr = crb->map[i].pa;
			crb->map[i].va = vaddr;
			for (j = 0; j < crb->map[i].count; ++j) {
				set_pte(pte_offset(pmd, vaddr),
					mk_pte_phys(paddr, PAGE_KERNEL));
				paddr += PAGE_SIZE;
				vaddr += PAGE_SIZE;
			}
		}

		/* Let vmalloc know that we've allocated some space.  */
		console_remap_vm.flags = VM_ALLOC;
		console_remap_vm.addr = VMALLOC_START;
		console_remap_vm.size = vaddr - VMALLOC_START;
		vmlist = &console_remap_vm;
	}

	callback_init_done = 1;
	return kernel_end;
}


/*
 * paging_init() sets up the memory map.
 */
void
paging_init(void)
{
	unsigned long zones_size[MAX_NR_ZONES] = {0, 0, 0};
	unsigned long dma_pfn, high_pfn;

	dma_pfn = virt_to_phys((char *)MAX_DMA_ADDRESS) >> PAGE_SHIFT;
	high_pfn = max_low_pfn;

#define ORDER_MASK (~((1L << (MAX_ORDER-1))-1))
#define ORDER_ALIGN(n)	(((n) +  ~ORDER_MASK) & ORDER_MASK)

	dma_pfn = ORDER_ALIGN(dma_pfn);
	high_pfn = ORDER_ALIGN(high_pfn);

#undef ORDER_MASK
#undef ORDER_ALIGN

	if (dma_pfn > high_pfn)
		zones_size[ZONE_DMA] = high_pfn;
	else {
		zones_size[ZONE_DMA] = dma_pfn;
		zones_size[ZONE_NORMAL] = high_pfn - dma_pfn;
	}

	/* Initialize mem_map[].  */
	free_area_init(zones_size);

	/* Initialize the kernel's ZERO_PGE. */
	memset((void *)ZERO_PGE, 0, PAGE_SIZE);
}

#if defined(CONFIG_ALPHA_GENERIC) || defined(CONFIG_ALPHA_SRM)
void
srm_paging_stop (void)
{
	/* Move the vptb back to where the SRM console expects it.  */
	swapper_pg_dir[1] = swapper_pg_dir[1023];
	tbia();
	wrvptptr(0x200000000);
	hwrpb->vptb = 0x200000000;
	hwrpb_update_checksum(hwrpb);

	/* Reload the page tables that the console had in use.  */
	load_PCB(&original_pcb);
	tbia();
}
#endif

static void __init
printk_memory_info(void)
{
	unsigned long codesize, reservedpages, datasize, initsize, tmp;
	extern int page_is_ram(unsigned long) __init;
	extern char _text, _etext, _data, _edata;
	extern char __init_begin, __init_end;

	/* printk all informations */
	reservedpages = 0;
	for (tmp = 0; tmp < max_low_pfn; tmp++)
		/*
		 * Only count reserved RAM pages
		 */
		if (page_is_ram(tmp) && PageReserved(mem_map+tmp))
			reservedpages++;

	codesize =  (unsigned long) &_etext - (unsigned long) &_text;
	datasize =  (unsigned long) &_edata - (unsigned long) &_data;
	initsize =  (unsigned long) &__init_end - (unsigned long) &__init_begin;

	printk("Memory: %luk/%luk available (%luk kernel code, %luk reserved, %luk data, %luk init)\n",
	       (unsigned long) nr_free_pages() << (PAGE_SHIFT-10),
	       max_mapnr << (PAGE_SHIFT-10),
	       codesize >> 10,
	       reservedpages << (PAGE_SHIFT-10),
	       datasize >> 10,
	       initsize >> 10);
}

void __init
mem_init(void)
{
	max_mapnr = num_physpages = max_low_pfn;
	totalram_pages += free_all_bootmem();
	high_memory = (void *) __va(max_low_pfn * PAGE_SIZE);

	printk_memory_info();
}

void
free_initmem (void)
{
	extern char __init_begin, __init_end;
	unsigned long addr;

	addr = (unsigned long)(&__init_begin);
	for (; addr < (unsigned long)(&__init_end); addr += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(addr));
		set_page_count(virt_to_page(addr), 1);
		free_page(addr);
		totalram_pages++;
	}
	printk ("Freeing unused kernel memory: %ldk freed\n",
		(&__init_end - &__init_begin) >> 10);
}

#ifdef CONFIG_BLK_DEV_INITRD
void
free_initrd_mem(unsigned long start, unsigned long end)
{
	for (; start < end; start += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(start));
		set_page_count(virt_to_page(start), 1);
		free_page(start);
		totalram_pages++;
	}
	printk ("Freeing initrd memory: %ldk freed\n", (end - start) >> 10);
}
#endif

void
si_meminfo(struct sysinfo *val)
{
	val->totalram = totalram_pages;
	val->sharedram = 0;
	val->freeram = nr_free_pages();
	val->bufferram = atomic_read(&buffermem_pages);
	val->totalhigh = 0;
	val->freehigh = 0;
	val->mem_unit = PAGE_SIZE;
}
