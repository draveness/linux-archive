/*
 *  PowerPC version 
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *
 *  Modifications by Paul Mackerras (PowerMac) (paulus@cs.anu.edu.au)
 *  and Cort Dougan (PReP) (cort@cs.nmt.edu)
 *    Copyright (C) 1996 Paul Mackerras
 *  Amiga/APUS changes by Jesper Skov (jskov@cygnus.co.uk).
 *
 *  Derived from "arch/i386/mm/init.c"
 *    Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Dave Engebretsen <engebret@us.ibm.com>
 *      Rework for PPC64 port.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 */

#include <linux/config.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/stddef.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/bootmem.h>
#include <linux/highmem.h>

#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/abs_addr.h>
#include <asm/prom.h>
#include <asm/lmb.h>
#include <asm/rtas.h>
#include <asm/io.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <asm/mmu.h>
#include <asm/uaccess.h>
#include <asm/smp.h>
#include <asm/machdep.h>
#include <asm/tlb.h>
#include <asm/naca.h>
#include <asm/eeh.h>
#include <asm/processor.h>
#include <asm/mmzone.h>
#include <asm/cputable.h>
#include <asm/ppcdebug.h>
#include <asm/sections.h>
#include <asm/system.h>
#include <asm/iommu.h>
#include <asm/abs_addr.h>


struct mmu_context_queue_t mmu_context_queue;
int mem_init_done;
unsigned long ioremap_bot = IMALLOC_BASE;
static unsigned long phbs_io_bot = PHBS_IO_BASE;

extern pgd_t swapper_pg_dir[];
extern struct task_struct *current_set[NR_CPUS];

extern pgd_t ioremap_dir[];
pgd_t * ioremap_pgd = (pgd_t *)&ioremap_dir;

unsigned long klimit = (unsigned long)_end;

unsigned long _SDR1=0;
unsigned long _ASR=0;

/* max amount of RAM to use */
unsigned long __max_memory;

/* info on what we think the IO hole is */
unsigned long 	io_hole_start;
unsigned long	io_hole_size;
unsigned long	top_of_ram;

void show_mem(void)
{
	unsigned long total = 0, reserved = 0;
	unsigned long shared = 0, cached = 0;
	struct page *page;
	pg_data_t *pgdat;
	unsigned long i;

	printk("Mem-info:\n");
	show_free_areas();
	printk("Free swap:       %6ldkB\n", nr_swap_pages<<(PAGE_SHIFT-10));
	for_each_pgdat(pgdat) {
		for (i = 0; i < pgdat->node_spanned_pages; i++) {
			page = pgdat->node_mem_map + i;
			total++;
			if (PageReserved(page))
				reserved++;
			else if (PageSwapCache(page))
				cached++;
			else if (page_count(page))
				shared += page_count(page) - 1;
		}
	}
	printk("%ld pages of RAM\n", total);
	printk("%ld reserved pages\n", reserved);
	printk("%ld pages shared\n", shared);
	printk("%ld pages swap cached\n", cached);
}

#ifdef CONFIG_PPC_ISERIES

void *ioremap(unsigned long addr, unsigned long size)
{
	return (void *)addr;
}

extern void *__ioremap(unsigned long addr, unsigned long size,
		       unsigned long flags)
{
	return (void *)addr;
}

void iounmap(void *addr)
{
	return;
}

#else

/*
 * map_io_page currently only called by __ioremap
 * map_io_page adds an entry to the ioremap page table
 * and adds an entry to the HPT, possibly bolting it
 */
static void map_io_page(unsigned long ea, unsigned long pa, int flags)
{
	pgd_t *pgdp;
	pmd_t *pmdp;
	pte_t *ptep;
	unsigned long vsid;

	if (mem_init_done) {
		spin_lock(&ioremap_mm.page_table_lock);
		pgdp = pgd_offset_i(ea);
		pmdp = pmd_alloc(&ioremap_mm, pgdp, ea);
		ptep = pte_alloc_kernel(&ioremap_mm, pmdp, ea);

		pa = abs_to_phys(pa);
		set_pte(ptep, pfn_pte(pa >> PAGE_SHIFT, __pgprot(flags)));
		spin_unlock(&ioremap_mm.page_table_lock);
	} else {
		unsigned long va, vpn, hash, hpteg;

		/*
		 * If the mm subsystem is not fully up, we cannot create a
		 * linux page table entry for this mapping.  Simply bolt an
		 * entry in the hardware page table.
		 */
		vsid = get_kernel_vsid(ea);
		va = (vsid << 28) | (ea & 0xFFFFFFF);
		vpn = va >> PAGE_SHIFT;

		hash = hpt_hash(vpn, 0);

		hpteg = ((hash & htab_data.htab_hash_mask)*HPTES_PER_GROUP);

		/* Panic if a pte grpup is full */
		if (ppc_md.hpte_insert(hpteg, va, pa >> PAGE_SHIFT, 0,
				       _PAGE_NO_CACHE|_PAGE_GUARDED|PP_RWXX,
				       1, 0) == -1) {
			panic("map_io_page: could not insert mapping");
		}
	}
}


static void * __ioremap_com(unsigned long addr, unsigned long pa,
			    unsigned long ea, unsigned long size,
			    unsigned long flags)
{
	unsigned long i;

	if ((flags & _PAGE_PRESENT) == 0)
		flags |= pgprot_val(PAGE_KERNEL);
	if (flags & (_PAGE_NO_CACHE | _PAGE_WRITETHRU))
		flags |= _PAGE_GUARDED;

	for (i = 0; i < size; i += PAGE_SIZE) {
		map_io_page(ea+i, pa+i, flags);
	}

	return (void *) (ea + (addr & ~PAGE_MASK));
}


void *
ioremap(unsigned long addr, unsigned long size)
{
	void *ret = __ioremap(addr, size, _PAGE_NO_CACHE);
	if(mem_init_done)
		return eeh_ioremap(addr, ret);	/* may remap the addr */
	return ret;
}

void *
__ioremap(unsigned long addr, unsigned long size, unsigned long flags)
{
	unsigned long pa, ea;

	/*
	 * Choose an address to map it to.
	 * Once the imalloc system is running, we use it.
	 * Before that, we map using addresses going
	 * up from ioremap_bot.  imalloc will use
	 * the addresses from ioremap_bot through
	 * IMALLOC_END (0xE000001fffffffff)
	 * 
	 */
	pa = addr & PAGE_MASK;
	size = PAGE_ALIGN(addr + size) - pa;

	if (size == 0)
		return NULL;

	if (mem_init_done) {
		struct vm_struct *area;
		area = im_get_free_area(size);
		if (area == NULL)
			return NULL;
		ea = (unsigned long)(area->addr);
	} else {
		ea = ioremap_bot;
		ioremap_bot += size;
	}

	return __ioremap_com(addr, pa, ea, size, flags);
}

#define IS_PAGE_ALIGNED(_val) ((_val) == ((_val) & PAGE_MASK))

int __ioremap_explicit(unsigned long pa, unsigned long ea,
		       unsigned long size, unsigned long flags)
{
	struct vm_struct *area;
	
	/* For now, require page-aligned values for pa, ea, and size */
	if (!IS_PAGE_ALIGNED(pa) || !IS_PAGE_ALIGNED(ea) ||
	    !IS_PAGE_ALIGNED(size)) {
		printk(KERN_ERR	"unaligned value in %s\n", __FUNCTION__);
		return 1;
	}
	
	if (!mem_init_done) {
		/* Two things to consider in this case:
		 * 1) No records will be kept (imalloc, etc) that the region
		 *    has been remapped
		 * 2) It won't be easy to iounmap() the region later (because
		 *    of 1)
		 */
		;
	} else {
		area = im_get_area(ea, size, IM_REGION_UNUSED|IM_REGION_SUBSET);
		if (area == NULL) {
			printk(KERN_ERR "could not obtain imalloc area for ea 0x%lx\n", ea);
			return 1;
		}
		if (ea != (unsigned long) area->addr) {
			printk(KERN_ERR "unexpected addr return from im_get_area\n");
			return 1;
		}
	}
	
	if (__ioremap_com(pa, pa, ea, size, flags) != (void *) ea) {
		printk(KERN_ERR "__ioremap_com() returned unexpected addr\n");
		return 1;
	}

	return 0;
}

static void unmap_im_area_pte(pmd_t *pmd, unsigned long address,
				  unsigned long size)
{
	unsigned long end;
	pte_t *pte;

	if (pmd_none(*pmd))
		return;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return;
	}

	pte = pte_offset_kernel(pmd, address);
	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;

	do {
		pte_t page;
		page = ptep_get_and_clear(pte);
		address += PAGE_SIZE;
		pte++;
		if (pte_none(page))
			continue;
		if (pte_present(page))
			continue;
		printk(KERN_CRIT "Whee.. Swapped out page in kernel page table\n");
	} while (address < end);
}

static void unmap_im_area_pmd(pgd_t *dir, unsigned long address,
				  unsigned long size)
{
	unsigned long end;
	pmd_t *pmd;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		pgd_ERROR(*dir);
		pgd_clear(dir);
		return;
	}

	pmd = pmd_offset(dir, address);
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;

	do {
		unmap_im_area_pte(pmd, address, end - address);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
}

/*  
 * Unmap an IO region and remove it from imalloc'd list.
 * Access to IO memory should be serialized by driver.
 * This code is modeled after vmalloc code - unmap_vm_area()
 *
 * XXX	what about calls before mem_init_done (ie python_countermeasures())	
 */
void iounmap(void *addr)
{
	unsigned long address, start, end, size;
	struct mm_struct *mm;
	pgd_t *dir;

	if (!mem_init_done) {
		return;
	}
	
	/* addr could be in EEH or IO region, map it to IO region regardless.
	 */
	addr = (void *) (IO_TOKEN_TO_ADDR(addr) & PAGE_MASK);
	
	if ((size = im_free(addr)) == 0) {
		return;
	}

	address = (unsigned long)addr; 
	start = address;
	end = address + size;

	mm = &ioremap_mm;
	spin_lock(&mm->page_table_lock);

	dir = pgd_offset_i(address);
	flush_cache_vunmap(address, end);
	do {
		unmap_im_area_pmd(dir, address, end - address);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	} while (address && (address < end));
	flush_tlb_kernel_range(start, end);

	spin_unlock(&mm->page_table_lock);
	return;
}

int iounmap_explicit(void *addr, unsigned long size)
{
	struct vm_struct *area;
	
	/* addr could be in EEH or IO region, map it to IO region regardless.
	 */
	addr = (void *) (IO_TOKEN_TO_ADDR(addr) & PAGE_MASK);

	/* Verify that the region either exists or is a subset of an existing
	 * region.  In the latter case, split the parent region to create 
	 * the exact region 
	 */
	area = im_get_area((unsigned long) addr, size, 
			    IM_REGION_EXISTS | IM_REGION_SUBSET);
	if (area == NULL) {
		printk(KERN_ERR "%s() cannot unmap nonexistent range 0x%lx\n",
				__FUNCTION__, (unsigned long) addr);
		return 1;
	}

	iounmap(area->addr);
	return 0;
}

#endif

void free_initmem(void)
{
	unsigned long addr;

	addr = (unsigned long)__init_begin;
	for (; addr < (unsigned long)__init_end; addr += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(addr));
		set_page_count(virt_to_page(addr), 1);
		free_page(addr);
		totalram_pages++;
	}
	printk ("Freeing unused kernel memory: %luk freed\n",
		((unsigned long)__init_end - (unsigned long)__init_begin) >> 10);
}

#ifdef CONFIG_BLK_DEV_INITRD
void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (start < end)
		printk ("Freeing initrd memory: %ldk freed\n", (end - start) >> 10);
	for (; start < end; start += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(start));
		set_page_count(virt_to_page(start), 1);
		free_page(start);
		totalram_pages++;
	}
}
#endif

/*
 * Do very early mm setup.
 */
void __init mm_init_ppc64(void)
{
	unsigned long i;

	ppc64_boot_msg(0x100, "MM Init");

	/* Reserve all contexts < FIRST_USER_CONTEXT for kernel use.
	 * The range of contexts [FIRST_USER_CONTEXT, NUM_USER_CONTEXT)
	 * are stored on a stack/queue for easy allocation and deallocation.
	 */
	mmu_context_queue.lock = SPIN_LOCK_UNLOCKED;
	mmu_context_queue.head = 0;
	mmu_context_queue.tail = NUM_USER_CONTEXT-1;
	mmu_context_queue.size = NUM_USER_CONTEXT;
	for (i = 0; i < NUM_USER_CONTEXT; i++)
		mmu_context_queue.elements[i] = i + FIRST_USER_CONTEXT;

	/* This is the story of the IO hole... please, keep seated,
	 * unfortunately, we are out of oxygen masks at the moment.
	 * So we need some rough way to tell where your big IO hole
	 * is. On pmac, it's between 2G and 4G, on POWER3, it's around
	 * that area as well, on POWER4 we don't have one, etc...
	 * We need that to implement something approx. decent for
	 * page_is_ram() so that /dev/mem doesn't map cacheable IO space
	 * when XFree resquest some IO regions witout using O_SYNC, we
	 * also need that as a "hint" when sizing the TCE table on POWER3
	 * So far, the simplest way that seem work well enough for us it
	 * to just assume that the first discontinuity in our physical
	 * RAM layout is the IO hole. That may not be correct in the future
	 * (and isn't on iSeries but then we don't care ;)
	 */
	top_of_ram = lmb_end_of_DRAM();

#ifndef CONFIG_PPC_ISERIES
	for (i = 1; i < lmb.memory.cnt; i++) {
		unsigned long base, prevbase, prevsize;

		prevbase = lmb.memory.region[i-1].physbase;
		prevsize = lmb.memory.region[i-1].size;
		base = lmb.memory.region[i].physbase;
		if (base > (prevbase + prevsize)) {
			io_hole_start = prevbase + prevsize;
			io_hole_size = base  - (prevbase + prevsize);
			break;
		}
	}
#endif /* CONFIG_PPC_ISERIES */
	if (io_hole_start)
		printk("IO Hole assumed to be %lx -> %lx\n",
		       io_hole_start, io_hole_start + io_hole_size - 1);

	ppc64_boot_msg(0x100, "MM Init Done");
}


/*
 * This is called by /dev/mem to know if a given address has to
 * be mapped non-cacheable or not
 */
int page_is_ram(unsigned long physaddr)
{
#ifdef CONFIG_PPC_ISERIES
	return 1;
#endif
	if (physaddr >= top_of_ram)
		return 0;
	return io_hole_start == 0 ||  physaddr < io_hole_start ||
		physaddr >= (io_hole_start + io_hole_size);
}


/*
 * Initialize the bootmem system and give it all the memory we
 * have available.
 */
#ifndef CONFIG_DISCONTIGMEM
void __init do_init_bootmem(void)
{
	unsigned long i;
	unsigned long start, bootmap_pages;
	unsigned long total_pages = lmb_end_of_DRAM() >> PAGE_SHIFT;
	int boot_mapsize;

	/*
	 * Find an area to use for the bootmem bitmap.  Calculate the size of
	 * bitmap required as (Total Memory) / PAGE_SIZE / BITS_PER_BYTE.
	 * Add 1 additional page in case the address isn't page-aligned.
	 */
	bootmap_pages = bootmem_bootmap_pages(total_pages);

	start = abs_to_phys(lmb_alloc(bootmap_pages<<PAGE_SHIFT, PAGE_SIZE));
	BUG_ON(!start);

	boot_mapsize = init_bootmem(start >> PAGE_SHIFT, total_pages);

	max_pfn = max_low_pfn;

	/* add all physical memory to the bootmem map. Also find the first */
	for (i=0; i < lmb.memory.cnt; i++) {
		unsigned long physbase, size;

		physbase = lmb.memory.region[i].physbase;
		size = lmb.memory.region[i].size;
		free_bootmem(physbase, size);
	}

	/* reserve the sections we're already using */
	for (i=0; i < lmb.reserved.cnt; i++) {
		unsigned long physbase = lmb.reserved.region[i].physbase;
		unsigned long size = lmb.reserved.region[i].size;

		reserve_bootmem(physbase, size);
	}
}

/*
 * paging_init() sets up the page tables - in fact we've already done this.
 */
void __init paging_init(void)
{
	unsigned long zones_size[MAX_NR_ZONES];
	unsigned long zholes_size[MAX_NR_ZONES];
	unsigned long total_ram = lmb_phys_mem_size();

	printk(KERN_INFO "Top of RAM: 0x%lx, Total RAM: 0x%lx\n",
	       top_of_ram, total_ram);
	printk(KERN_INFO "Memory hole size: %ldMB\n",
	       (top_of_ram - total_ram) >> 20);
	/*
	 * All pages are DMA-able so we put them all in the DMA zone.
	 */
	memset(zones_size, 0, sizeof(zones_size));
	memset(zholes_size, 0, sizeof(zholes_size));

	zones_size[ZONE_DMA] = top_of_ram >> PAGE_SHIFT;
	zholes_size[ZONE_DMA] = (top_of_ram - total_ram) >> PAGE_SHIFT;

	free_area_init_node(0, &contig_page_data, NULL, zones_size,
			    __pa(PAGE_OFFSET) >> PAGE_SHIFT, zholes_size);
	mem_map = contig_page_data.node_mem_map;
}
#endif /* CONFIG_DISCONTIGMEM */

static struct kcore_list kcore_vmem;

static int __init setup_kcore(void)
{
	int i;

	for (i=0; i < lmb.memory.cnt; i++) {
		unsigned long physbase, size;
		struct kcore_list *kcore_mem;

		physbase = lmb.memory.region[i].physbase;
		size = lmb.memory.region[i].size;

		/* GFP_ATOMIC to avoid might_sleep warnings during boot */
		kcore_mem = kmalloc(sizeof(struct kcore_list), GFP_ATOMIC);
		if (!kcore_mem)
			panic("mem_init: kmalloc failed\n");

		kclist_add(kcore_mem, __va(physbase), size);
	}

	kclist_add(&kcore_vmem, (void *)VMALLOC_START, VMALLOC_END-VMALLOC_START);

	return 0;
}
module_init(setup_kcore);

void __init mem_init(void)
{
#ifndef CONFIG_DISCONTIGMEM
	unsigned long addr;
#endif
	int codepages = 0;
	int datapages = 0;
	int initpages = 0;

	num_physpages = max_low_pfn;	/* RAM is assumed contiguous */
	high_memory = (void *) __va(max_low_pfn * PAGE_SIZE);

#ifdef CONFIG_DISCONTIGMEM
{
	int nid;

        for (nid = 0; nid < numnodes; nid++) {
		if (node_data[nid].node_spanned_pages != 0) {
			printk("freeing bootmem node %x\n", nid);
			totalram_pages +=
				free_all_bootmem_node(NODE_DATA(nid));
		}
	}

	printk("Memory: %luk available (%dk kernel code, %dk data, %dk init) [%08lx,%08lx]\n",
	       (unsigned long)nr_free_pages()<< (PAGE_SHIFT-10),
	       codepages<< (PAGE_SHIFT-10), datapages<< (PAGE_SHIFT-10),
	       initpages<< (PAGE_SHIFT-10),
	       PAGE_OFFSET, (unsigned long)__va(lmb_end_of_DRAM()));
}
#else
	max_mapnr = num_physpages;

	totalram_pages += free_all_bootmem();

	for (addr = KERNELBASE; addr < (unsigned long)__va(lmb_end_of_DRAM());
	     addr += PAGE_SIZE) {
		if (!PageReserved(virt_to_page(addr)))
			continue;
		if (addr < (unsigned long)_etext)
			codepages++;

		else if (addr >= (unsigned long)__init_begin
			 && addr < (unsigned long)__init_end)
			initpages++;
		else if (addr < klimit)
			datapages++;
	}

	printk("Memory: %luk available (%dk kernel code, %dk data, %dk init) [%08lx,%08lx]\n",
	       (unsigned long)nr_free_pages()<< (PAGE_SHIFT-10),
	       codepages<< (PAGE_SHIFT-10), datapages<< (PAGE_SHIFT-10),
	       initpages<< (PAGE_SHIFT-10),
	       PAGE_OFFSET, (unsigned long)__va(lmb_end_of_DRAM()));
#endif
	mem_init_done = 1;

#ifdef CONFIG_PPC_ISERIES
	iommu_vio_init();
#endif
}

/*
 * This is called when a page has been modified by the kernel.
 * It just marks the page as not i-cache clean.  We do the i-cache
 * flush later when the page is given to a user process, if necessary.
 */
void flush_dcache_page(struct page *page)
{
	if (cur_cpu_spec->cpu_features & CPU_FTR_COHERENT_ICACHE)
		return;
	/* avoid an atomic op if possible */
	if (test_bit(PG_arch_1, &page->flags))
		clear_bit(PG_arch_1, &page->flags);
}

void clear_user_page(void *page, unsigned long vaddr, struct page *pg)
{
	clear_page(page);

	if (cur_cpu_spec->cpu_features & CPU_FTR_COHERENT_ICACHE)
		return;
	/*
	 * We shouldnt have to do this, but some versions of glibc
	 * require it (ld.so assumes zero filled pages are icache clean)
	 * - Anton
	 */

	/* avoid an atomic op if possible */
	if (test_bit(PG_arch_1, &pg->flags))
		clear_bit(PG_arch_1, &pg->flags);
}

void copy_user_page(void *vto, void *vfrom, unsigned long vaddr,
		    struct page *pg)
{
	copy_page(vto, vfrom);

	/*
	 * We should be able to use the following optimisation, however
	 * there are two problems.
	 * Firstly a bug in some versions of binutils meant PLT sections
	 * were not marked executable.
	 * Secondly the first word in the GOT section is blrl, used
	 * to establish the GOT address. Until recently the GOT was
	 * not marked executable.
	 * - Anton
	 */
#if 0
	if (!vma->vm_file && ((vma->vm_flags & VM_EXEC) == 0))
		return;
#endif

	if (cur_cpu_spec->cpu_features & CPU_FTR_COHERENT_ICACHE)
		return;

	/* avoid an atomic op if possible */
	if (test_bit(PG_arch_1, &pg->flags))
		clear_bit(PG_arch_1, &pg->flags);
}

void flush_icache_user_range(struct vm_area_struct *vma, struct page *page,
			     unsigned long addr, int len)
{
	unsigned long maddr;

	maddr = (unsigned long)page_address(page) + (addr & ~PAGE_MASK);
	flush_icache_range(maddr, maddr + len);
}

/*
 * This is called at the end of handling a user page fault, when the
 * fault has been handled by updating a PTE in the linux page tables.
 * We use it to preload an HPTE into the hash table corresponding to
 * the updated linux PTE.
 * 
 * This must always be called with the mm->page_table_lock held
 */
void update_mmu_cache(struct vm_area_struct *vma, unsigned long ea,
		      pte_t pte)
{
	unsigned long vsid;
	void *pgdir;
	pte_t *ptep;
	int local = 0;
	cpumask_t tmp;
	unsigned long flags;

	/* handle i-cache coherency */
	if (!(cur_cpu_spec->cpu_features & CPU_FTR_COHERENT_ICACHE) &&
	    !(cur_cpu_spec->cpu_features & CPU_FTR_NOEXECUTE)) {
		unsigned long pfn = pte_pfn(pte);
		if (pfn_valid(pfn)) {
			struct page *page = pfn_to_page(pfn);
			if (!PageReserved(page)
			    && !test_bit(PG_arch_1, &page->flags)) {
				__flush_dcache_icache(page_address(page));
				set_bit(PG_arch_1, &page->flags);
			}
		}
	}

	/* We only want HPTEs for linux PTEs that have _PAGE_ACCESSED set */
	if (!pte_young(pte))
		return;

	pgdir = vma->vm_mm->pgd;
	if (pgdir == NULL)
		return;

	ptep = find_linux_pte(pgdir, ea);
	if (!ptep)
		return;

	vsid = get_vsid(vma->vm_mm->context.id, ea);

	local_irq_save(flags);
	tmp = cpumask_of_cpu(smp_processor_id());
	if (cpus_equal(vma->vm_mm->cpu_vm_mask, tmp))
		local = 1;

	__hash_page(ea, pte_val(pte) & (_PAGE_USER|_PAGE_RW), vsid, ptep,
		    0x300, local);
	local_irq_restore(flags);
}

void * reserve_phb_iospace(unsigned long size)
{
	void *virt_addr;
		
	if (phbs_io_bot >= IMALLOC_BASE) 
		panic("reserve_phb_iospace(): phb io space overflow\n");
			
	virt_addr = (void *) phbs_io_bot;
	phbs_io_bot += size;

	return virt_addr;
}

kmem_cache_t *zero_cache;

static void zero_ctor(void *pte, kmem_cache_t *cache, unsigned long flags)
{
	memset(pte, 0, PAGE_SIZE);
}

void pgtable_cache_init(void)
{
	zero_cache = kmem_cache_create("zero",
				PAGE_SIZE,
				0,
				SLAB_HWCACHE_ALIGN | SLAB_MUST_HWCACHE_ALIGN,
				zero_ctor,
				NULL);
	if (!zero_cache)
		panic("pgtable_cache_init(): could not create zero_cache!\n");
}
