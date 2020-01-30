/*
 * arch/ppc64/kernel/pmac_iommu.c
 *
 * Copyright (C) 2004 Olof Johansson <olof@austin.ibm.com>, IBM Corporation
 *
 * Based on pSeries_iommu.c:
 * Copyright (C) 2001 Mike Corrigan & Dave Engebretsen, IBM Corporation
 * Copyright (C) 2004 Olof Johansson <olof@austin.ibm.com>, IBM Corporation
 *
 * Dynamic DMA mapping support, PowerMac G5 (DART)-specific parts.
 *
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/config.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/rtas.h>
#include <asm/ppcdebug.h>
#include <asm/iommu.h>
#include <asm/pci-bridge.h>
#include <asm/machdep.h>
#include <asm/abs_addr.h>
#include <asm/cacheflush.h>
#include "pci.h"


/* physical base of DART registers */
#define DART_BASE        0xf8033000UL

/* Offset from base to control register */
#define DARTCNTL   0
/* Offset from base to exception register */
#define DARTEXCP   0x10
/* Offset from base to TLB tag registers */
#define DARTTAG    0x1000


/* Control Register fields */

/* base address of table (pfn) */
#define DARTCNTL_BASE_MASK    0xfffff
#define DARTCNTL_BASE_SHIFT   12

#define DARTCNTL_FLUSHTLB     0x400
#define DARTCNTL_ENABLE       0x200

/* size of table in pages */
#define DARTCNTL_SIZE_MASK    0x1ff
#define DARTCNTL_SIZE_SHIFT   0

/* DART table fields */
#define DARTMAP_VALID   0x80000000
#define DARTMAP_RPNMASK 0x00ffffff

/* Physical base address and size of the DART table */
unsigned long dart_tablebase;
unsigned long dart_tablesize;

/* Virtual base address of the DART table */
static u32 *dart_vbase;

/* Mapped base address for the dart */
static unsigned int *dart; 

/* Dummy val that entries are set to when unused */
static unsigned int dart_emptyval;

static struct iommu_table iommu_table_pmac;
static int dart_dirty;

#define DBG(...)

static inline void dart_tlb_invalidate_all(void)
{
	unsigned long l = 0;
	unsigned int reg;
	unsigned long limit;

	DBG("dart: flush\n");

	/* To invalidate the DART, set the DARTCNTL_FLUSHTLB bit in the
	 * control register and wait for it to clear.
	 *
	 * Gotcha: Sometimes, the DART won't detect that the bit gets
	 * set. If so, clear it and set it again.
	 */ 

	limit = 0;

retry:
	reg = in_be32((unsigned int *)dart+DARTCNTL);
	reg |= DARTCNTL_FLUSHTLB;
	out_be32((unsigned int *)dart+DARTCNTL, reg);

	l = 0;
	while ((in_be32((unsigned int *)dart+DARTCNTL) & DARTCNTL_FLUSHTLB) &&
		l < (1L<<limit)) {
		l++;
	}
	if (l == (1L<<limit)) {
		if (limit < 4) {
			limit++;
		        reg = in_be32((unsigned int *)dart+DARTCNTL);
		        reg &= ~DARTCNTL_FLUSHTLB;
		        out_be32((unsigned int *)dart+DARTCNTL, reg);
			goto retry;
		} else
			panic("U3-DART: TLB did not flush after waiting a long "
			      "time. Buggy U3 ?");
	}
}

static void dart_flush(struct iommu_table *tbl)
{
	if (dart_dirty)
		dart_tlb_invalidate_all();
	dart_dirty = 0;
}

static void dart_build_pmac(struct iommu_table *tbl, long index, 
			    long npages, unsigned long uaddr,
			    enum dma_data_direction direction)
{
	unsigned int *dp;
	unsigned int rpn;

	DBG("dart: build at: %lx, %lx, addr: %x\n", index, npages, uaddr);

	dp = ((unsigned int*)tbl->it_base) + index;
	
	/* On pmac, all memory is contigous, so we can move this
	 * out of the loop.
	 */
	while (npages--) {
		rpn = virt_to_abs(uaddr) >> PAGE_SHIFT;

		*(dp++) = DARTMAP_VALID | (rpn & DARTMAP_RPNMASK);

		rpn++;
		uaddr += PAGE_SIZE;
	}

	dart_dirty = 1;
}


static void dart_free_pmac(struct iommu_table *tbl, long index, long npages)
{
	unsigned int *dp;
	
	/* We don't worry about flushing the TLB cache. The only drawback of
	 * not doing it is that we won't catch buggy device drivers doing
	 * bad DMAs, but then no 32-bit architecture ever does either.
	 */

	DBG("dart: free at: %lx, %lx\n", index, npages);

	dp  = ((unsigned int *)tbl->it_base) + index;
		
	while (npages--)
		*(dp++) = dart_emptyval;
}


static int dart_init(struct device_node *dart_node)
{
	unsigned int regword;
	unsigned int i;
	unsigned long tmp;
	struct page *p;

	if (dart_tablebase == 0 || dart_tablesize == 0) {
		printk(KERN_INFO "U3-DART: table not allocated, using direct DMA\n");
		return -ENODEV;
	}

	/* Make sure nothing from the DART range remains in the CPU cache
	 * from a previous mapping that existed before the kernel took
	 * over
	 */
	flush_dcache_phys_range(dart_tablebase, dart_tablebase + dart_tablesize);

	/* Allocate a spare page to map all invalid DART pages. We need to do
	 * that to work around what looks like a problem with the HT bridge
	 * prefetching into invalid pages and corrupting data
	 */
	tmp = __get_free_pages(GFP_ATOMIC, 1);
	if (tmp == 0)
		panic("U3-DART: Cannot allocate spare page !");
	dart_emptyval = DARTMAP_VALID |
		((virt_to_abs(tmp) >> PAGE_SHIFT) & DARTMAP_RPNMASK);

	/* Map in DART registers. FIXME: Use device node to get base address */
	dart = ioremap(DART_BASE, 0x7000);
	if (dart == NULL)
		panic("U3-DART: Cannot map registers !");

	/* Set initial control register contents: table base, 
	 * table size and enable bit
	 */
	regword = DARTCNTL_ENABLE | 
		((dart_tablebase >> PAGE_SHIFT) << DARTCNTL_BASE_SHIFT) |
		(((dart_tablesize >> PAGE_SHIFT) & DARTCNTL_SIZE_MASK)
				 << DARTCNTL_SIZE_SHIFT);
	p = virt_to_page(dart_tablebase);
	dart_vbase = ioremap(virt_to_abs(dart_tablebase), dart_tablesize);

	/* Fill initial table */
	for (i = 0; i < dart_tablesize/4; i++)
		dart_vbase[i] = dart_emptyval;

	/* Initialize DART with table base and enable it. */
	out_be32((unsigned int *)dart, regword);

	/* Invalidate DART to get rid of possible stale TLBs */
	dart_tlb_invalidate_all();

	iommu_table_pmac.it_busno = 0;
	
	/* Units of tce entries */
	iommu_table_pmac.it_offset = 0;
	
	/* Set the tce table size - measured in pages */
	iommu_table_pmac.it_size = dart_tablesize >> PAGE_SHIFT;

	/* Initialize the common IOMMU code */
	iommu_table_pmac.it_base = (unsigned long)dart_vbase;
	iommu_table_pmac.it_index = 0;
	iommu_table_pmac.it_blocksize = 1;
	iommu_table_pmac.it_entrysize = sizeof(u32);
	iommu_init_table(&iommu_table_pmac);

	/* Reserve the last page of the DART to avoid possible prefetch
	 * past the DART mapped area
	 */
	set_bit(iommu_table_pmac.it_mapsize - 1, iommu_table_pmac.it_map);

	printk(KERN_INFO "U3-DART IOMMU initialized\n");

	return 0;
}


void iommu_setup_pmac(void)
{
	struct pci_dev *dev = NULL;
	struct device_node *dn;

	/* Find the DART in the device-tree */
	dn = of_find_compatible_node(NULL, "dart", "u3-dart");
	if (dn == NULL)
		return;

	/* Setup low level TCE operations for the core IOMMU code */
	ppc_md.tce_build = dart_build_pmac;
	ppc_md.tce_free  = dart_free_pmac;
	ppc_md.tce_flush = dart_flush;

	/* Initialize the DART HW */
	if (dart_init(dn))
		return;

	/* Setup pci_dma ops */
	pci_iommu_init();

	/* We only have one iommu table on the mac for now, which makes
	 * things simple. Setup all PCI devices to point to this table
	 */
	while ((dev = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
		/* We must use pci_device_to_OF_node() to make sure that
		 * we get the real "final" pointer to the device in the
		 * pci_dev sysdata and not the temporary PHB one
		 */
		struct device_node *dn = pci_device_to_OF_node(dev);
		if (dn)
			dn->iommu_table = &iommu_table_pmac;
	}
}




