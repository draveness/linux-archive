/* $Id: io-unit.c,v 1.22 2000/08/09 00:00:15 davem Exp $
 * io-unit.c:  IO-UNIT specific routines for memory management.
 *
 * Copyright (C) 1997,1998 Jakub Jelinek    (jj@sunsite.mff.cuni.cz)
 */
 
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/malloc.h>
#include <linux/spinlock.h>
#include <asm/scatterlist.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/sbus.h>
#include <asm/io.h>
#include <asm/io-unit.h>
#include <asm/mxcc.h>
#include <asm/bitops.h>

/* #define IOUNIT_DEBUG */
#ifdef IOUNIT_DEBUG
#define IOD(x) printk(x)
#else
#define IOD(x) do { } while (0)
#endif

#define IOPERM        (IOUPTE_CACHE | IOUPTE_WRITE | IOUPTE_VALID)
#define MKIOPTE(phys) __iopte((((phys)>>4) & IOUPTE_PAGE) | IOPERM)

void __init
iounit_init(int sbi_node, int io_node, struct sbus_bus *sbus)
{
	iopte_t *xpt, *xptend;
	struct iounit_struct *iounit;
	struct linux_prom_registers iommu_promregs[PROMREG_MAX];
	struct resource r;

	iounit = kmalloc(sizeof(struct iounit_struct), GFP_ATOMIC);

	memset(iounit, 0, sizeof(*iounit));
	iounit->limit[0] = IOUNIT_BMAP1_START;
	iounit->limit[1] = IOUNIT_BMAP2_START;
	iounit->limit[2] = IOUNIT_BMAPM_START;
	iounit->limit[3] = IOUNIT_BMAPM_END;
	iounit->rotor[1] = IOUNIT_BMAP2_START;
	iounit->rotor[2] = IOUNIT_BMAPM_START;

	prom_getproperty(sbi_node, "reg", (void *) iommu_promregs,
			 sizeof(iommu_promregs));
	prom_apply_generic_ranges(io_node, 0, iommu_promregs, 3);
	memset(&r, 0, sizeof(r));
	r.flags = iommu_promregs[2].which_io;
	r.start = iommu_promregs[2].phys_addr;
	xpt = (iopte_t *) sbus_ioremap(&r, 0, PAGE_SIZE * 16, "XPT");
	if(!xpt) panic("Cannot map External Page Table.");
	
	sbus->iommu = (struct iommu_struct *)iounit;
	iounit->page_table = xpt;
	
	for (xptend = iounit->page_table + (16 * PAGE_SIZE) / sizeof(iopte_t);
	     xpt < xptend;)
	     	*xpt++ = 0;
}

/* One has to hold iounit->lock to call this */
static unsigned long iounit_get_area(struct iounit_struct *iounit, unsigned long vaddr, int size)
{
	int i, j, k, npages;
	unsigned long rotor, scan, limit;
	iopte_t iopte;

        npages = ((vaddr & ~PAGE_MASK) + size + (PAGE_SIZE-1)) >> PAGE_SHIFT;

	/* A tiny bit of magic ingredience :) */
	switch (npages) {
	case 1: i = 0x0231; break;
	case 2: i = 0x0132; break;
	default: i = 0x0213; break;
	}
	
	IOD(("iounit_get_area(%08lx,%d[%d])=", vaddr, size, npages));
	
next:	j = (i & 15);
	rotor = iounit->rotor[j - 1];
	limit = iounit->limit[j];
	scan = rotor;
nexti:	scan = find_next_zero_bit(iounit->bmap, limit, scan);
	if (scan + npages > limit) {
		if (limit != rotor) {
			limit = rotor;
			scan = iounit->limit[j - 1];
			goto nexti;
		}
		i >>= 4;
		if (!(i & 15))
			panic("iounit_get_area: Couldn't find free iopte slots for (%08lx,%d)\n", vaddr, size);
		goto next;
	}
	for (k = 1, scan++; k < npages; k++)
		if (test_bit(scan++, iounit->bmap))
			goto nexti;
	iounit->rotor[j - 1] = (scan < limit) ? scan : iounit->limit[j - 1];
	scan -= npages;
	iopte = MKIOPTE(__pa(vaddr & PAGE_MASK));
	vaddr = IOUNIT_DMA_BASE + (scan << PAGE_SHIFT) + (vaddr & ~PAGE_MASK);
	for (k = 0; k < npages; k++, iopte = __iopte(iopte_val(iopte) + 0x100), scan++) {
		set_bit(scan, iounit->bmap);
		iounit->page_table[scan] = iopte;
	}
	IOD(("%08lx\n", vaddr));
	return vaddr;
}

static __u32 iounit_get_scsi_one(char *vaddr, unsigned long len, struct sbus_bus *sbus)
{
	unsigned long ret, flags;
	struct iounit_struct *iounit = (struct iounit_struct *)sbus->iommu;
	
	spin_lock_irqsave(&iounit->lock, flags);
	ret = iounit_get_area(iounit, (unsigned long)vaddr, len);
	spin_unlock_irqrestore(&iounit->lock, flags);
	return ret;
}

static void iounit_get_scsi_sgl(struct scatterlist *sg, int sz, struct sbus_bus *sbus)
{
	unsigned long flags;
	struct iounit_struct *iounit = (struct iounit_struct *)sbus->iommu;

	/* FIXME: Cache some resolved pages - often several sg entries are to the same page */
	spin_lock_irqsave(&iounit->lock, flags);
	for (; sz >= 0; sz--) {
		sg[sz].dvma_address = iounit_get_area(iounit, (unsigned long)sg[sz].address, sg[sz].length);
		sg[sz].dvma_length = sg[sz].length;
	}
	spin_unlock_irqrestore(&iounit->lock, flags);
}

static void iounit_release_scsi_one(__u32 vaddr, unsigned long len, struct sbus_bus *sbus)
{
	unsigned long flags;
	struct iounit_struct *iounit = (struct iounit_struct *)sbus->iommu;
	
	spin_lock_irqsave(&iounit->lock, flags);
	len = ((vaddr & ~PAGE_MASK) + len + (PAGE_SIZE-1)) >> PAGE_SHIFT;
	vaddr = (vaddr - IOUNIT_DMA_BASE) >> PAGE_SHIFT;
	IOD(("iounit_release %08lx-%08lx\n", (long)vaddr, (long)len+vaddr));
	for (len += vaddr; vaddr < len; vaddr++)
		clear_bit(vaddr, iounit->bmap);
	spin_unlock_irqrestore(&iounit->lock, flags);
}

static void iounit_release_scsi_sgl(struct scatterlist *sg, int sz, struct sbus_bus *sbus)
{
	unsigned long flags;
	unsigned long vaddr, len;
	struct iounit_struct *iounit = (struct iounit_struct *)sbus->iommu;

	spin_lock_irqsave(&iounit->lock, flags);
	for (; sz >= 0; sz--) {
		len = ((sg[sz].dvma_address & ~PAGE_MASK) + sg[sz].length + (PAGE_SIZE-1)) >> PAGE_SHIFT;
		vaddr = (sg[sz].dvma_address - IOUNIT_DMA_BASE) >> PAGE_SHIFT;
		IOD(("iounit_release %08lx-%08lx\n", (long)vaddr, (long)len+vaddr));
		for (len += vaddr; vaddr < len; vaddr++)
			clear_bit(vaddr, iounit->bmap);
	}
	spin_unlock_irqrestore(&iounit->lock, flags);
}

#ifdef CONFIG_SBUS
static void iounit_map_dma_area(unsigned long va, __u32 addr, int len)
{
	unsigned long page, end;
	pgprot_t dvma_prot;
	iopte_t *iopte;
	struct sbus_bus *sbus;

	dvma_prot = __pgprot(SRMMU_CACHE | SRMMU_ET_PTE | SRMMU_PRIV);
	end = PAGE_ALIGN((addr + len));
	while(addr < end) {
		page = va;
		{
			pgd_t *pgdp;
			pmd_t *pmdp;
			pte_t *ptep;
			long i;

			pgdp = pgd_offset(init_task.mm, addr);
			pmdp = pmd_offset(pgdp, addr);
			ptep = pte_offset(pmdp, addr);

			set_pte(ptep, pte_val(mk_pte(virt_to_page(page), dvma_prot)));
			
			i = ((addr - IOUNIT_DMA_BASE) >> PAGE_SHIFT);

			for_each_sbus(sbus) {
				struct iounit_struct *iounit = (struct iounit_struct *)sbus->iommu;

				iopte = (iopte_t *)(iounit->page_table + i);
				*iopte = __iopte(MKIOPTE(__pa(page)));
			}
		}
		addr += PAGE_SIZE;
		va += PAGE_SIZE;
	}
	flush_cache_all();
	flush_tlb_all();
}

static void iounit_unmap_dma_area(unsigned long addr, int len)
{
	/* XXX Somebody please fill this in */
}

/* XXX We do not pass sbus device here, bad. */
static unsigned long iounit_translate_dvma(unsigned long addr)
{
	struct sbus_bus *sbus = sbus_root;	/* They are all the same */
	struct iounit_struct *iounit = (struct iounit_struct *)sbus->iommu;
	int i;
	iopte_t *iopte;

	i = ((addr - IOUNIT_DMA_BASE) >> PAGE_SHIFT);
	iopte = (iopte_t *)(iounit->page_table + i);
	return (iopte_val(*iopte) & 0xFFFFFFF0) << 4; /* XXX sun4d guru, help */
}
#endif

static char *iounit_lockarea(char *vaddr, unsigned long len)
{
/* FIXME: Write this */
	return vaddr;
}

static void iounit_unlockarea(char *vaddr, unsigned long len)
{
/* FIXME: Write this */
}

void __init ld_mmu_iounit(void)
{
	BTFIXUPSET_CALL(mmu_lockarea, iounit_lockarea, BTFIXUPCALL_RETO0);
	BTFIXUPSET_CALL(mmu_unlockarea, iounit_unlockarea, BTFIXUPCALL_NOP);

	BTFIXUPSET_CALL(mmu_get_scsi_one, iounit_get_scsi_one, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(mmu_get_scsi_sgl, iounit_get_scsi_sgl, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(mmu_release_scsi_one, iounit_release_scsi_one, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(mmu_release_scsi_sgl, iounit_release_scsi_sgl, BTFIXUPCALL_NORM);

#ifdef CONFIG_SBUS
	BTFIXUPSET_CALL(mmu_map_dma_area, iounit_map_dma_area, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(mmu_unmap_dma_area, iounit_unmap_dma_area, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(mmu_translate_dvma, iounit_translate_dvma, BTFIXUPCALL_NORM);
#endif
}

__u32 iounit_map_dma_init(struct sbus_bus *sbus, int size)
{
	int i, j, k, npages;
	unsigned long rotor, scan, limit;
	unsigned long flags;
	__u32 ret;
	struct iounit_struct *iounit = (struct iounit_struct *)sbus->iommu;

        npages = (size + (PAGE_SIZE-1)) >> PAGE_SHIFT;
	i = 0x0213;
	spin_lock_irqsave(&iounit->lock, flags);
next:	j = (i & 15);
	rotor = iounit->rotor[j - 1];
	limit = iounit->limit[j];
	scan = rotor;
nexti:	scan = find_next_zero_bit(iounit->bmap, limit, scan);
	if (scan + npages > limit) {
		if (limit != rotor) {
			limit = rotor;
			scan = iounit->limit[j - 1];
			goto nexti;
		}
		i >>= 4;
		if (!(i & 15))
			panic("iounit_map_dma_init: Couldn't find free iopte slots for %d bytes\n", size);
		goto next;
	}
	for (k = 1, scan++; k < npages; k++)
		if (test_bit(scan++, iounit->bmap))
			goto nexti;
	iounit->rotor[j - 1] = (scan < limit) ? scan : iounit->limit[j - 1];
	scan -= npages;
	ret = IOUNIT_DMA_BASE + (scan << PAGE_SHIFT);
	for (k = 0; k < npages; k++, scan++)
		set_bit(scan, iounit->bmap);
	spin_unlock_irqrestore(&iounit->lock, flags);
	return ret;
}

__u32 iounit_map_dma_page(__u32 vaddr, void *addr, struct sbus_bus *sbus)
{
	int scan = (vaddr - IOUNIT_DMA_BASE) >> PAGE_SHIFT;
	struct iounit_struct *iounit = (struct iounit_struct *)sbus->iommu;
	
	iounit->page_table[scan] = MKIOPTE(__pa(((unsigned long)addr) & PAGE_MASK));
	return vaddr + (((unsigned long)addr) & ~PAGE_MASK);
}
