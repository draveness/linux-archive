/*
 *	linux/arch/alpha/kernel/core_cia.c
 *
 * Written by David A Rusling (david.rusling@reo.mts.dec.com).
 * December 1995.
 *
 *	Copyright (C) 1995  David A Rusling
 *	Copyright (C) 1997, 1998  Jay Estabrook
 *	Copyright (C) 1998, 1999, 2000  Richard Henderson
 *
 * Code common to all CIA core logic chips.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/init.h>

#include <asm/system.h>
#include <asm/ptrace.h>
#include <asm/hwrpb.h>

#define __EXTERN_INLINE inline
#include <asm/io.h>
#include <asm/core_cia.h>
#undef __EXTERN_INLINE

#include <linux/bootmem.h>

#include "proto.h"
#include "pci_impl.h"


/*
 * NOTE: Herein lie back-to-back mb instructions.  They are magic. 
 * One plausible explanation is that the i/o controller does not properly
 * handle the system transaction.  Another involves timing.  Ho hum.
 */

#define DEBUG_CONFIG 0
#if DEBUG_CONFIG
# define DBGC(args)	printk args
#else
# define DBGC(args)
#endif

#define vip	volatile int  *

/*
 * Given a bus, device, and function number, compute resulting
 * configuration space address.  It is therefore not safe to have
 * concurrent invocations to configuration space access routines, but
 * there really shouldn't be any need for this.
 *
 * Type 0:
 *
 *  3 3|3 3 2 2|2 2 2 2|2 2 2 2|1 1 1 1|1 1 1 1|1 1 
 *  3 2|1 0 9 8|7 6 5 4|3 2 1 0|9 8 7 6|5 4 3 2|1 0 9 8|7 6 5 4|3 2 1 0
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | | |D|D|D|D|D|D|D|D|D|D|D|D|D|D|D|D|D|D|D|D|D|F|F|F|R|R|R|R|R|R|0|0|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *	31:11	Device select bit.
 * 	10:8	Function number
 * 	 7:2	Register number
 *
 * Type 1:
 *
 *  3 3|3 3 2 2|2 2 2 2|2 2 2 2|1 1 1 1|1 1 1 1|1 1 
 *  3 2|1 0 9 8|7 6 5 4|3 2 1 0|9 8 7 6|5 4 3 2|1 0 9 8|7 6 5 4|3 2 1 0
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | | | | | | | | | | |B|B|B|B|B|B|B|B|D|D|D|D|D|F|F|F|R|R|R|R|R|R|0|1|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *	31:24	reserved
 *	23:16	bus number (8 bits = 128 possible buses)
 *	15:11	Device number (5 bits)
 *	10:8	function number
 *	 7:2	register number
 *  
 * Notes:
 *	The function number selects which function of a multi-function device 
 *	(e.g., SCSI and Ethernet).
 * 
 *	The register selects a DWORD (32 bit) register offset.  Hence it
 *	doesn't get shifted by 2 bits as we want to "drop" the bottom two
 *	bits.
 */

static int
mk_conf_addr(struct pci_dev *dev, int where, unsigned long *pci_addr,
	     unsigned char *type1)
{
	u8 bus = dev->bus->number;
	u8 device_fn = dev->devfn;

	*type1 = (bus != 0);
	*pci_addr = (bus << 16) | (device_fn << 8) | where;

	DBGC(("mk_conf_addr(bus=%d ,device_fn=0x%x, where=0x%x,"
	      " returning address 0x%p\n"
	      bus, device_fn, where, *pci_addr));

	return 0;
}

static unsigned int
conf_read(unsigned long addr, unsigned char type1)
{
	unsigned long flags;
	int stat0, value;
	int cia_cfg = 0;

	DBGC(("conf_read(addr=0x%lx, type1=%d) ", addr, type1));
	__save_and_cli(flags);

	/* Reset status register to avoid losing errors.  */
	stat0 = *(vip)CIA_IOC_CIA_ERR;
	*(vip)CIA_IOC_CIA_ERR = stat0;
	mb();
	*(vip)CIA_IOC_CIA_ERR; /* re-read to force write */

	/* If Type1 access, must set CIA CFG. */
	if (type1) {
		cia_cfg = *(vip)CIA_IOC_CFG;
		*(vip)CIA_IOC_CFG = (cia_cfg & ~3) | 1;
		mb();
		*(vip)CIA_IOC_CFG;
	}

	mb();
	draina();
	mcheck_expected(0) = 1;
	mcheck_taken(0) = 0;
	mb();

	/* Access configuration space.  */
	value = *(vip)addr;
	mb();
	mb();  /* magic */
	if (mcheck_taken(0)) {
		mcheck_taken(0) = 0;
		value = 0xffffffff;
		mb();
	}
	mcheck_expected(0) = 0;
	mb();

	/* If Type1 access, must reset IOC CFG so normal IO space ops work.  */
	if (type1) {
		*(vip)CIA_IOC_CFG = cia_cfg;
		mb();
		*(vip)CIA_IOC_CFG;
	}

	__restore_flags(flags);
	DBGC(("done\n"));

	return value;
}

static void
conf_write(unsigned long addr, unsigned int value, unsigned char type1)
{
	unsigned long flags;
	int stat0, cia_cfg = 0;

	DBGC(("conf_write(addr=0x%lx, type1=%d) ", addr, type1));
	__save_and_cli(flags);

	/* Reset status register to avoid losing errors.  */
	stat0 = *(vip)CIA_IOC_CIA_ERR;
	*(vip)CIA_IOC_CIA_ERR = stat0;
	mb();
	*(vip)CIA_IOC_CIA_ERR; /* re-read to force write */

	/* If Type1 access, must set CIA CFG.  */
	if (type1) {
		cia_cfg = *(vip)CIA_IOC_CFG;
		*(vip)CIA_IOC_CFG = (cia_cfg & ~3) | 1;
		mb();
		*(vip)CIA_IOC_CFG;
	}

	mb();
	draina();
	mcheck_expected(0) = 1;
	mcheck_taken(0) = 0;
	mb();

	/* Access configuration space.  */
	*(vip)addr = value;
	mb();
	*(vip)addr; /* read back to force the write */

	mcheck_expected(0) = 0;
	mb();

	/* If Type1 access, must reset IOC CFG so normal IO space ops work.  */
	if (type1) {
		*(vip)CIA_IOC_CFG = cia_cfg;
		mb();
		*(vip)CIA_IOC_CFG;
	}

	__restore_flags(flags);
	DBGC(("done\n"));
}

static int
cia_read_config_byte(struct pci_dev *dev, int where, u8 *value)
{
	unsigned long addr, pci_addr;
	unsigned char type1;

	if (mk_conf_addr(dev, where, &pci_addr, &type1))
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = (pci_addr << 5) + 0x00 + CIA_CONF;
	*value = conf_read(addr, type1) >> ((where & 3) * 8);
	return PCIBIOS_SUCCESSFUL;
}

static int 
cia_read_config_word(struct pci_dev *dev, int where, u16 *value)
{
	unsigned long addr, pci_addr;
	unsigned char type1;

	if (mk_conf_addr(dev, where, &pci_addr, &type1))
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = (pci_addr << 5) + 0x08 + CIA_CONF;
	*value = conf_read(addr, type1) >> ((where & 3) * 8);
	return PCIBIOS_SUCCESSFUL;
}

static int 
cia_read_config_dword(struct pci_dev *dev, int where, u32 *value)
{
	unsigned long addr, pci_addr;
	unsigned char type1;

	if (mk_conf_addr(dev, where, &pci_addr, &type1))
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = (pci_addr << 5) + 0x18 + CIA_CONF;
	*value = conf_read(addr, type1);
	return PCIBIOS_SUCCESSFUL;
}

static int 
cia_write_config(struct pci_dev *dev, int where, u32 value, long mask)
{
	unsigned long addr, pci_addr;
	unsigned char type1;

	if (mk_conf_addr(dev, where, &pci_addr, &type1))
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = (pci_addr << 5) + mask + CIA_CONF;
	conf_write(addr, value << ((where & 3) * 8), type1);
	return PCIBIOS_SUCCESSFUL;
}

static int
cia_write_config_byte(struct pci_dev *dev, int where, u8 value)
{
	return cia_write_config(dev, where, value, 0x00);
}

static int 
cia_write_config_word(struct pci_dev *dev, int where, u16 value)
{
	return cia_write_config(dev, where, value, 0x08);
}

static int 
cia_write_config_dword(struct pci_dev *dev, int where, u32 value)
{
	return cia_write_config(dev, where, value, 0x18);
}

struct pci_ops cia_pci_ops = 
{
	read_byte:	cia_read_config_byte,
	read_word:	cia_read_config_word,
	read_dword:	cia_read_config_dword,
	write_byte:	cia_write_config_byte,
	write_word:	cia_write_config_word,
	write_dword:	cia_write_config_dword
};

/*
 * CIA Pass 1 and PYXIS Pass 1 and 2 have a broken scatter-gather tlb.
 * It cannot be invalidated.  Rather than hard code the pass numbers,
 * actually try the tbia to see if it works.
 */

void
cia_pci_tbi(struct pci_controler *hose, dma_addr_t start, dma_addr_t end)
{
	wmb();
	*(vip)CIA_IOC_PCI_TBIA = 3;	/* Flush all locked and unlocked.  */
	mb();
	*(vip)CIA_IOC_PCI_TBIA;
}

/*
 * Fixup attempt number 1.
 *
 * Write zeros directly into the tag registers.
 */

static void
cia_pci_tbi_try1(struct pci_controler *hose,
		 dma_addr_t start, dma_addr_t end)
{
	wmb();
	*(vip)CIA_IOC_TB_TAGn(0) = 0;
	*(vip)CIA_IOC_TB_TAGn(1) = 0;
	*(vip)CIA_IOC_TB_TAGn(2) = 0;
	*(vip)CIA_IOC_TB_TAGn(3) = 0;
	*(vip)CIA_IOC_TB_TAGn(4) = 0;
	*(vip)CIA_IOC_TB_TAGn(5) = 0;
	*(vip)CIA_IOC_TB_TAGn(6) = 0;
	*(vip)CIA_IOC_TB_TAGn(7) = 0;
	mb();
	*(vip)CIA_IOC_TB_TAGn(0);
}

#if 0
/*
 * Fixup attempt number 2.  This is the method NT and NetBSD use.
 *
 * Allocate mappings, and put the chip into DMA loopback mode to read a
 * garbage page.  This works by causing TLB misses, causing old entries to
 * be purged to make room for the new entries coming in for the garbage page.
 */

#define CIA_BROKEN_TBI_TRY2_BASE	0xE0000000

static void __init
cia_enable_broken_tbi_try2(void)
{
	unsigned long *ppte, pte;
	long i;

	ppte = __alloc_bootmem(PAGE_SIZE, 32768, 0);
	pte = (virt_to_phys(ppte) >> (PAGE_SHIFT - 1)) | 1;

	for (i = 0; i < PAGE_SIZE / sizeof(unsigned long); ++i)
		ppte[i] = pte;

	*(vip)CIA_IOC_PCI_W3_BASE = CIA_BROKEN_TBI_TRY2_BASE | 3;
	*(vip)CIA_IOC_PCI_W3_MASK = (PAGE_SIZE - 1) & 0xfff00000;
	*(vip)CIA_IOC_PCI_T3_BASE = virt_to_phys(ppte) >> 2;
}

static void
cia_pci_tbi_try2(struct pci_controler *hose,
		 dma_addr_t start, dma_addr_t end)
{
	unsigned long flags;
	unsigned long bus_addr;
	int ctrl;
	long i;

	__save_and_cli(flags);

	/* Put the chip into PCI loopback mode.  */
	mb();
	ctrl = *(vip)CIA_IOC_CIA_CTRL;
	*(vip)CIA_IOC_CIA_CTRL = ctrl | CIA_CTRL_PCI_LOOP_EN;
	mb();
	*(vip)CIA_IOC_CIA_CTRL;
	mb();

	/* Read from PCI dense memory space at TBI_ADDR, skipping 32k on
	   each read.  This forces SG TLB misses.  NetBSD claims that the
	   TLB entries are not quite LRU, meaning that we need to read more
	   times than there are actual tags.  The 2117x docs claim strict
	   round-robin.  Oh well, we've come this far...  */

	bus_addr = cia_ioremap(CIA_BROKEN_TBI_TRY2_BASE);
	for (i = 0; i < 12; ++i, bus_addr += 32768)
		cia_readl(bus_addr);

	/* Restore normal PCI operation.  */
	mb();
	*(vip)CIA_IOC_CIA_CTRL = ctrl;
	mb();
	*(vip)CIA_IOC_CIA_CTRL;
	mb();

	__restore_flags(flags);
}
#endif

static void __init
verify_tb_operation(void)
{
	static int page[PAGE_SIZE/4]
		__attribute__((aligned(PAGE_SIZE)))
		__initdata = { 0 };

	struct pci_iommu_arena *arena = pci_isa_hose->sg_isa;
	int ctrl, addr0, tag0, pte0, data0;
	int temp;

	/* Put the chip into PCI loopback mode.  */
	mb();
	ctrl = *(vip)CIA_IOC_CIA_CTRL;
	*(vip)CIA_IOC_CIA_CTRL = ctrl | CIA_CTRL_PCI_LOOP_EN;
	mb();
	*(vip)CIA_IOC_CIA_CTRL;
	mb();

	/* Write a valid entry directly into the TLB registers.  */

	addr0 = arena->dma_base;
	tag0 = addr0 | 1;
	pte0 = (virt_to_phys(page) >> (PAGE_SHIFT - 1)) | 1;

	*(vip)CIA_IOC_TB_TAGn(0) = tag0;
	*(vip)CIA_IOC_TB_TAGn(1) = 0;
	*(vip)CIA_IOC_TB_TAGn(2) = 0;
	*(vip)CIA_IOC_TB_TAGn(3) = 0;
	*(vip)CIA_IOC_TB_TAGn(4) = 0;
	*(vip)CIA_IOC_TB_TAGn(5) = 0;
	*(vip)CIA_IOC_TB_TAGn(6) = 0;
	*(vip)CIA_IOC_TB_TAGn(7) = 0;
	*(vip)CIA_IOC_TBn_PAGEm(0,0) = pte0;
	*(vip)CIA_IOC_TBn_PAGEm(0,1) = 0;
	*(vip)CIA_IOC_TBn_PAGEm(0,2) = 0;
	*(vip)CIA_IOC_TBn_PAGEm(0,3) = 0;
	mb();

	/* First, verify we can read back what we've written.  If
	   this fails, we can't be sure of any of the other testing
	   we're going to do, so bail.  */
	/* ??? Actually, we could do the work with machine checks.
	   By passing this register update test, we pretty much
	   guarantee that cia_pci_tbi_try1 works.  If this test
	   fails, cia_pci_tbi_try2 might still work.  */

	temp = *(vip)CIA_IOC_TB_TAGn(0);
	if (temp != tag0) {
		printk("pci: failed tb register update test "
		       "(tag0 %#x != %#x)\n", temp, tag0);
		goto failed;
	}
	temp = *(vip)CIA_IOC_TB_TAGn(1);
	if (temp != 0) {
		printk("pci: failed tb register update test "
		       "(tag1 %#x != 0)\n", temp);
		goto failed;
	}
	temp = *(vip)CIA_IOC_TBn_PAGEm(0,0);
	if (temp != pte0) {
		printk("pci: failed tb register update test "
		       "(pte0 %#x != %#x)\n", temp, pte0);
		goto failed;
	}
	printk("pci: passed tb register update test\n");

	/* Second, verify we can actually do I/O through this entry.  */

	data0 = 0xdeadbeef;
	page[0] = data0;
	mcheck_expected(0) = 1;
	mcheck_taken(0) = 0;
	mb();
	temp = cia_readl(cia_ioremap(addr0));
	mb();
	mcheck_expected(0) = 0;
	mb();
	if (mcheck_taken(0)) {
		printk("pci: failed sg loopback i/o read test (mcheck)\n");
		goto failed;
	}
	if (temp != data0) {
		printk("pci: failed sg loopback i/o read test "
		       "(%#x != %#x)\n", temp, data0);
		goto failed;
	}
	printk("pci: passed sg loopback i/o read test\n");

	/* Third, try to invalidate the TLB.  */

	cia_pci_tbi(arena->hose, 0, -1);
	temp = *(vip)CIA_IOC_TB_TAGn(0);
	if (temp & 1) {
		cia_pci_tbi_try1(arena->hose, 0, -1);
	
		temp = *(vip)CIA_IOC_TB_TAGn(0);
		if (temp & 1) {
			printk("pci: failed tbia test; "
			       "no usable workaround\n");
			goto failed;
		}

		alpha_mv.mv_pci_tbi = cia_pci_tbi_try1;
		printk("pci: failed tbia test; workaround 1 succeeded\n");
	} else {
		printk("pci: passed tbia test\n");
	}

	/* Fourth, verify the TLB snoops the EV5's caches when
	   doing a tlb fill.  */

	data0 = 0x5adda15e;
	page[0] = data0;
	arena->ptes[4] = pte0;
	mcheck_expected(0) = 1;
	mcheck_taken(0) = 0;
	mb();
	temp = cia_readl(cia_ioremap(addr0 + 4*PAGE_SIZE));
	mb();
	mcheck_expected(0) = 0;
	mb();
	if (mcheck_taken(0)) {
		printk("pci: failed pte write cache snoop test (mcheck)\n");
		goto failed;
	}
	if (temp != data0) {
		printk("pci: failed pte write cache snoop test "
		       "(%#x != %#x)\n", temp, data0);
		goto failed;
	}
	printk("pci: passed pte write cache snoop test\n");

	/* Fifth, verify that a previously invalid PTE entry gets
	   filled from the page table.  */

	data0 = 0xabcdef12;
	page[0] = data0;
	arena->ptes[5] = pte0;
	mcheck_expected(0) = 1;
	mcheck_taken(0) = 0;
	mb();
	temp = cia_readl(cia_ioremap(addr0 + 5*PAGE_SIZE));
	mb();
	mcheck_expected(0) = 0;
	mb();
	if (mcheck_taken(0)) {
		printk("pci: failed valid tag invalid pte reload test "
		       "(mcheck; workaround available)\n");
		/* Work around this bug by aligning new allocations
		   on 4 page boundaries.  */
		arena->align_entry = 4;
	} else if (temp != data0) {
		printk("pci: failed valid tag invalid pte reload test "
		       "(%#x != %#x)\n", temp, data0);
		goto failed;
	} else {
		printk("pci: passed valid tag invalid pte reload test\n");
	}

	/* Sixth, verify machine checks are working.  Test invalid
	   pte under the same valid tag as we used above.  */

	mcheck_expected(0) = 1;
	mcheck_taken(0) = 0;
	mb();
	temp = cia_readl(cia_ioremap(addr0 + 6*PAGE_SIZE));
	mb();
	mcheck_expected(0) = 0;
	mb();
	printk("pci: %s pci machine check test\n",
	       mcheck_taken(0) ? "passed" : "failed");

	/* Clean up after the tests.  */
	arena->ptes[4] = 0;
	arena->ptes[5] = 0;
	alpha_mv.mv_pci_tbi(arena->hose, 0, -1);

exit:
	/* Restore normal PCI operation.  */
	mb();
	*(vip)CIA_IOC_CIA_CTRL = ctrl;
	mb();
	*(vip)CIA_IOC_CIA_CTRL;
	mb();
	return;

failed:
	printk("pci: disabling sg translation window\n");
	*(vip)CIA_IOC_PCI_W0_BASE = 0;
	alpha_mv.mv_pci_tbi = NULL;
	goto exit;
}

static void __init
do_init_arch(int is_pyxis)
{
	struct pci_controler *hose;
	int temp;
	int cia_rev;

	cia_rev = *(vip)CIA_IOC_CIA_REV & CIA_REV_MASK;
	printk("pci: cia revision %d%s\n",
	       cia_rev, is_pyxis ? " (pyxis)" : "");

	/* Set up error reporting.  */
	temp = *(vip)CIA_IOC_ERR_MASK;
	temp &= ~(CIA_ERR_CPU_PE | CIA_ERR_MEM_NEM | CIA_ERR_PA_PTE_INV
		  | CIA_ERR_RCVD_MAS_ABT | CIA_ERR_RCVD_TAR_ABT);
	*(vip)CIA_IOC_ERR_MASK = temp;

	/* Clear all currently pending errors.  */
	temp = *(vip)CIA_IOC_CIA_ERR;
	*(vip)CIA_IOC_CIA_ERR = temp;

	/* Turn on mchecks.  */
	temp = *(vip)CIA_IOC_CIA_CTRL;
	temp |= CIA_CTRL_FILL_ERR_EN | CIA_CTRL_MCHK_ERR_EN;
	*(vip)CIA_IOC_CIA_CTRL = temp;

	/* Clear the CFG register, which gets used for PCI config space
	   accesses.  That is the way we want to use it, and we do not
	   want to depend on what ARC or SRM might have left behind.  */
	*(vip)CIA_IOC_CFG = 0;
 
	/* Zero the HAEs.  */
	*(vip)CIA_IOC_HAE_MEM = 0;
	*(vip)CIA_IOC_HAE_IO = 0;

	/* For PYXIS, we always use BWX bus and i/o accesses.  To that end,
	   make sure they're enabled on the controler.  */
	if (is_pyxis) {
		temp = *(vip)CIA_IOC_CIA_CNFG;
		temp |= CIA_CNFG_IOA_BWEN;
		*(vip)CIA_IOC_CIA_CNFG = temp;
	}

	/* Syncronize with all previous changes.  */
	mb();
	*(vip)CIA_IOC_CIA_REV;

	/*
	 * Create our single hose.
	 */

	pci_isa_hose = hose = alloc_pci_controler();
	hose->io_space = &ioport_resource;
	hose->mem_space = &iomem_resource;
	hose->index = 0;

	if (! is_pyxis) {
		struct resource *hae_mem = alloc_resource();
		hose->mem_space = hae_mem;

		hae_mem->start = 0;
		hae_mem->end = CIA_MEM_R1_MASK;
		hae_mem->name = pci_hae0_name;
		hae_mem->flags = IORESOURCE_MEM;

		if (request_resource(&iomem_resource, hae_mem) < 0)
			printk(KERN_ERR "Failed to request HAE_MEM\n");

		hose->sparse_mem_base = CIA_SPARSE_MEM - IDENT_ADDR;
		hose->dense_mem_base = CIA_DENSE_MEM - IDENT_ADDR;
		hose->sparse_io_base = CIA_IO - IDENT_ADDR;
		hose->dense_io_base = 0;
	} else {
		hose->sparse_mem_base = 0;
		hose->dense_mem_base = CIA_BW_MEM - IDENT_ADDR;
		hose->sparse_io_base = 0;
		hose->dense_io_base = CIA_BW_IO - IDENT_ADDR;
	}

	/*
	 * Set up the PCI to main memory translation windows.
	 *
	 * Window 0 is scatter-gather 8MB at 8MB (for isa)
	 * Window 1 is direct access 1GB at 1GB
	 * Window 2 is direct access 1GB at 2GB
	 *
	 * We must actually use 2 windows to direct-map the 2GB space,
	 * because of an idiot-syncrasy of the CYPRESS chip used on 
	 * many PYXIS systems.  It may respond to a PCI bus address in
	 * the last 1MB of the 4GB address range.
	 *
	 * ??? NetBSD hints that page tables must be aligned to 32K,
	 * possibly due to a hardware bug.  This is over-aligned
	 * from the 8K alignment one would expect for an 8MB window. 
	 * No description of what revisions affected.
	 */

	hose->sg_pci = NULL;
	hose->sg_isa = iommu_arena_new(hose, 0x00800000, 0x00800000, 32768);
	__direct_map_base = 0x40000000;
	__direct_map_size = 0x80000000;

	*(vip)CIA_IOC_PCI_W0_BASE = hose->sg_isa->dma_base | 3;
	*(vip)CIA_IOC_PCI_W0_MASK = (hose->sg_isa->size - 1) & 0xfff00000;
	*(vip)CIA_IOC_PCI_T0_BASE = virt_to_phys(hose->sg_isa->ptes) >> 2;

	*(vip)CIA_IOC_PCI_W1_BASE = 0x40000000 | 1;
	*(vip)CIA_IOC_PCI_W1_MASK = (0x40000000 - 1) & 0xfff00000;
	*(vip)CIA_IOC_PCI_T1_BASE = 0 >> 2;

	*(vip)CIA_IOC_PCI_W2_BASE = 0x80000000 | 1;
	*(vip)CIA_IOC_PCI_W2_MASK = (0x40000000 - 1) & 0xfff00000;
	*(vip)CIA_IOC_PCI_T2_BASE = 0x40000000 >> 2;

	*(vip)CIA_IOC_PCI_W3_BASE = 0;
}

void __init
cia_init_arch(void)
{
	do_init_arch(0);
}

void __init
pyxis_init_arch(void)
{
	/* On pyxis machines we can precisely calculate the
	   CPU clock frequency using pyxis real time counter.
	   It's especially useful for SX164 with broken RTC.

	   Both CPU and chipset are driven by the single 16.666M
	   or 16.667M crystal oscillator. PYXIS_RT_COUNT clock is
	   66.66 MHz. -ink */

	unsigned int cc0, cc1;
	unsigned long pyxis_cc;

	__asm__ __volatile__ ("rpcc %0" : "=r"(cc0));
	pyxis_cc = *(vulp)PYXIS_RT_COUNT;
	do { } while(*(vulp)PYXIS_RT_COUNT - pyxis_cc < 4096);
	__asm__ __volatile__ ("rpcc %0" : "=r"(cc1));
	cc1 -= cc0;
	hwrpb->cycle_freq = ((cc1 >> 11) * 100000000UL) / 3;
	hwrpb_update_checksum(hwrpb);

	do_init_arch(1);
}

void __init
cia_init_pci(void)
{
	/* Must delay this from init_arch, as we need machine checks.  */
	verify_tb_operation();
	common_init_pci();
}

static inline void
cia_pci_clr_err(void)
{
	int jd;

	jd = *(vip)CIA_IOC_CIA_ERR;
	*(vip)CIA_IOC_CIA_ERR = jd;
	mb();
	*(vip)CIA_IOC_CIA_ERR;		/* re-read to force write.  */
}

static void
cia_decode_pci_error(struct el_CIA_sysdata_mcheck *cia, const char *msg)
{
	static const char * const pci_cmd_desc[16] = {
		"Interrupt Acknowledge", "Special Cycle", "I/O Read",
		"I/O Write", "Reserved 0x4", "Reserved 0x5", "Memory Read",
		"Memory Write", "Reserved 0x8", "Reserved 0x9",
		"Configuration Read", "Configuration Write",
		"Memory Read Multiple", "Dual Address Cycle",
		"Memory Read Line", "Memory Write and Invalidate"
	};

	if (cia->cia_err & (CIA_ERR_COR_ERR
			    | CIA_ERR_UN_COR_ERR
			    | CIA_ERR_MEM_NEM
			    | CIA_ERR_PA_PTE_INV)) {
		static const char * const window_desc[6] = {
			"No window active", "Window 0 hit", "Window 1 hit",
			"Window 2 hit", "Window 3 hit", "Monster window hit"
		};

		const char *window;
		const char *cmd;
		unsigned long addr, tmp;
		int lock, dac;
	
		cmd = pci_cmd_desc[cia->pci_err0 & 0x7];
		lock = (cia->pci_err0 >> 4) & 1;
		dac = (cia->pci_err0 >> 5) & 1;

		tmp = (cia->pci_err0 >> 8) & 0x1F;
		tmp = ffs(tmp);
		window = window_desc[tmp];

		addr = cia->pci_err1;
		if (dac) {
			tmp = *(vip)CIA_IOC_PCI_W_DAC & 0xFFUL;
			addr |= tmp << 32;
		}

		printk(KERN_CRIT "CIA machine check: %s\n", msg);
		printk(KERN_CRIT "  DMA command: %s\n", cmd);
		printk(KERN_CRIT "  PCI address: %#010lx\n", addr);
		printk(KERN_CRIT "  %s, Lock: %d, DAC: %d\n",
		       window, lock, dac);
	} else if (cia->cia_err & (CIA_ERR_PERR
				   | CIA_ERR_PCI_ADDR_PE
				   | CIA_ERR_RCVD_MAS_ABT
				   | CIA_ERR_RCVD_TAR_ABT
				   | CIA_ERR_IOA_TIMEOUT)) {
		static const char * const master_st_desc[16] = {
			"Idle", "Drive bus", "Address step cycle",
			"Address cycle", "Data cycle", "Last read data cycle",
			"Last write data cycle", "Read stop cycle",
			"Write stop cycle", "Read turnaround cycle",
			"Write turnaround cycle", "Reserved 0xB",
			"Reserved 0xC", "Reserved 0xD", "Reserved 0xE",
			"Unknown state"
		};
		static const char * const target_st_desc[16] = {
			"Idle", "Busy", "Read data cycle", "Write data cycle",
			"Read stop cycle", "Write stop cycle",
			"Read turnaround cycle", "Write turnaround cycle",
			"Read wait cycle", "Write wait cycle",
			"Reserved 0xA", "Reserved 0xB", "Reserved 0xC",
			"Reserved 0xD", "Reserved 0xE", "Unknown state"
		};

		const char *cmd;
		const char *master, *target;
		unsigned long addr, tmp;
		int dac;

		master = master_st_desc[(cia->pci_err0 >> 16) & 0xF];
		target = target_st_desc[(cia->pci_err0 >> 20) & 0xF];
		cmd = pci_cmd_desc[(cia->pci_err0 >> 24) & 0xF];
		dac = (cia->pci_err0 >> 28) & 1;

		addr = cia->pci_err2;
		if (dac) {
			tmp = *(volatile int *)CIA_IOC_PCI_W_DAC & 0xFFUL;
			addr |= tmp << 32;
		}

		printk(KERN_CRIT "CIA machine check: %s\n", msg);
		printk(KERN_CRIT "  PCI command: %s\n", cmd);
		printk(KERN_CRIT "  Master state: %s, Target state: %s\n",
		       master, target);
		printk(KERN_CRIT "  PCI address: %#010lx, DAC: %d\n",
		       addr, dac);
	} else {
		printk(KERN_CRIT "CIA machine check: %s\n", msg);
		printk(KERN_CRIT "  Unknown PCI error\n");
		printk(KERN_CRIT "  PCI_ERR0 = %#08lx", cia->pci_err0);
		printk(KERN_CRIT "  PCI_ERR1 = %#08lx", cia->pci_err1);
		printk(KERN_CRIT "  PCI_ERR2 = %#08lx", cia->pci_err2);
	}
}

static void
cia_decode_mem_error(struct el_CIA_sysdata_mcheck *cia, const char *msg)
{
	unsigned long mem_port_addr;
	unsigned long mem_port_mask;
	const char *mem_port_cmd;
	const char *seq_state;
	const char *set_select;
	unsigned long tmp;

	/* If this is a DMA command, also decode the PCI bits.  */
	if ((cia->mem_err1 >> 20) & 1)
		cia_decode_pci_error(cia, msg);
	else
		printk(KERN_CRIT "CIA machine check: %s\n", msg);

	mem_port_addr = cia->mem_err0 & 0xfffffff0;
	mem_port_addr |= (cia->mem_err1 & 0x83UL) << 32;

	mem_port_mask = (cia->mem_err1 >> 12) & 0xF;

	tmp = (cia->mem_err1 >> 8) & 0xF;
	tmp |= ((cia->mem_err1 >> 20) & 1) << 4;
	if ((tmp & 0x1E) == 0x06)
		mem_port_cmd = "WRITE BLOCK or WRITE BLOCK LOCK";
	else if ((tmp & 0x1C) == 0x08)
		mem_port_cmd = "READ MISS or READ MISS MODIFY";
	else if (tmp == 0x1C)
		mem_port_cmd = "BC VICTIM";
	else if ((tmp & 0x1E) == 0x0E)
		mem_port_cmd = "READ MISS MODIFY";
	else if ((tmp & 0x1C) == 0x18)
		mem_port_cmd = "DMA READ or DMA READ MODIFY";
	else if ((tmp & 0x1E) == 0x12)
		mem_port_cmd = "DMA WRITE";
	else
		mem_port_cmd = "Unknown";

	tmp = (cia->mem_err1 >> 16) & 0xF;
	switch (tmp) {
	case 0x0:
		seq_state = "Idle";
		break;
	case 0x1:
		seq_state = "DMA READ or DMA WRITE";
		break;
	case 0x2: case 0x3:
		seq_state = "READ MISS (or READ MISS MODIFY) with victim";
		break;
	case 0x4: case 0x5: case 0x6:
		seq_state = "READ MISS (or READ MISS MODIFY) with no victim";
		break;
	case 0x8: case 0x9: case 0xB:
		seq_state = "Refresh";
		break;
	case 0xC:
		seq_state = "Idle, waiting for DMA pending read";
		break;
	case 0xE: case 0xF:
		seq_state = "Idle, ras precharge";
		break;
	default:
		seq_state = "Unknown";
		break;
	}

	tmp = (cia->mem_err1 >> 24) & 0x1F;
	switch (tmp) {
	case 0x00: set_select = "Set 0 selected"; break;
	case 0x01: set_select = "Set 1 selected"; break;
	case 0x02: set_select = "Set 2 selected"; break;
	case 0x03: set_select = "Set 3 selected"; break;
	case 0x04: set_select = "Set 4 selected"; break;
	case 0x05: set_select = "Set 5 selected"; break;
	case 0x06: set_select = "Set 6 selected"; break;
	case 0x07: set_select = "Set 7 selected"; break;
	case 0x08: set_select = "Set 8 selected"; break;
	case 0x09: set_select = "Set 9 selected"; break;
	case 0x0A: set_select = "Set A selected"; break;
	case 0x0B: set_select = "Set B selected"; break;
	case 0x0C: set_select = "Set C selected"; break;
	case 0x0D: set_select = "Set D selected"; break;
	case 0x0E: set_select = "Set E selected"; break;
	case 0x0F: set_select = "Set F selected"; break;
	case 0x10: set_select = "No set selected"; break;
	case 0x1F: set_select = "Refresh cycle"; break;
	default:   set_select = "Unknown"; break;
	}

	printk(KERN_CRIT "  Memory port command: %s\n", mem_port_cmd);
	printk(KERN_CRIT "  Memory port address: %#010lx, mask: %#lx\n",
	       mem_port_addr, mem_port_mask);
	printk(KERN_CRIT "  Memory sequencer state: %s\n", seq_state);
	printk(KERN_CRIT "  Memory set: %s\n", set_select);
}

static void
cia_decode_ecc_error(struct el_CIA_sysdata_mcheck *cia, const char *msg)
{
	long syn;
	long i;
	const char *fmt;

	cia_decode_mem_error(cia, msg);

	syn = cia->cia_syn & 0xff;
	if (syn == (syn & -syn)) {
		fmt = KERN_CRIT "  ECC syndrome %#x -- check bit %d\n";
		i = ffs(syn) - 1;
	} else {
		static unsigned char const data_bit[64] = {
			0xCE, 0xCB, 0xD3, 0xD5,
			0xD6, 0xD9, 0xDA, 0xDC,
			0x23, 0x25, 0x26, 0x29,
			0x2A, 0x2C, 0x31, 0x34,
			0x0E, 0x0B, 0x13, 0x15,
			0x16, 0x19, 0x1A, 0x1C,
			0xE3, 0xE5, 0xE6, 0xE9,
			0xEA, 0xEC, 0xF1, 0xF4,
			0x4F, 0x4A, 0x52, 0x54,
			0x57, 0x58, 0x5B, 0x5D,
			0xA2, 0xA4, 0xA7, 0xA8,
			0xAB, 0xAD, 0xB0, 0xB5,
			0x8F, 0x8A, 0x92, 0x94,
			0x97, 0x98, 0x9B, 0x9D,
			0x62, 0x64, 0x67, 0x68,
			0x6B, 0x6D, 0x70, 0x75
		};

		for (i = 0; i < 64; ++i)
			if (data_bit[i] == syn)
				break;

		if (i < 64)
			fmt = KERN_CRIT "  ECC syndrome %#x -- data bit %d\n";
		else
			fmt = KERN_CRIT "  ECC syndrome %#x -- unknown bit\n";
	}

	printk (fmt, syn, i);
}

static void
cia_decode_parity_error(struct el_CIA_sysdata_mcheck *cia)
{
	static const char * const cmd_desc[16] = {
		"NOP", "LOCK", "FETCH", "FETCH_M", "MEMORY BARRIER",
		"SET DIRTY", "WRITE BLOCK", "WRITE BLOCK LOCK",
		"READ MISS0", "READ MISS1", "READ MISS MOD0",
		"READ MISS MOD1", "BCACHE VICTIM", "Spare",
		"READ MISS MOD STC0", "READ MISS MOD STC1"
	};

	unsigned long addr;
	unsigned long mask;
	const char *cmd;
	int par;

	addr = cia->cpu_err0 & 0xfffffff0;
	addr |= (cia->cpu_err1 & 0x83UL) << 32;
	cmd = cmd_desc[(cia->cpu_err1 >> 8) & 0xF];
	mask = (cia->cpu_err1 >> 12) & 0xF;
	par = (cia->cpu_err1 >> 21) & 1;

	printk(KERN_CRIT "CIA machine check: System bus parity error\n");
	printk(KERN_CRIT "  Command: %s, Parity bit: %d\n", cmd, par);
	printk(KERN_CRIT "  Address: %#010lx, Mask: %#lx\n", addr, mask);
}

static int
cia_decode_mchk(unsigned long la_ptr)
{
	struct el_common *com;
	struct el_CIA_sysdata_mcheck *cia;
	int which;

	com = (void *)la_ptr;
	cia = (void *)(la_ptr + com->sys_offset);

	if ((cia->cia_err & CIA_ERR_VALID) == 0)
		return 0;

	which = cia->cia_err & 0xfff;
	switch (ffs(which) - 1) {
	case 0: /* CIA_ERR_COR_ERR */
		cia_decode_ecc_error(cia, "Corrected ECC error");
		break;
	case 1: /* CIA_ERR_UN_COR_ERR */
		cia_decode_ecc_error(cia, "Uncorrected ECC error");
		break;
	case 2: /* CIA_ERR_CPU_PE */
		cia_decode_parity_error(cia);
		break;
	case 3: /* CIA_ERR_MEM_NEM */
		cia_decode_mem_error(cia, "Access to nonexistent memory");
		break;
	case 4: /* CIA_ERR_PCI_SERR */
		cia_decode_pci_error(cia, "PCI bus system error");
		break;
	case 5: /* CIA_ERR_PERR */
		cia_decode_pci_error(cia, "PCI data parity error");
		break;
	case 6: /* CIA_ERR_PCI_ADDR_PE */
		cia_decode_pci_error(cia, "PCI address parity error");
		break;
	case 7: /* CIA_ERR_RCVD_MAS_ABT */
		cia_decode_pci_error(cia, "PCI master abort");
		break;
	case 8: /* CIA_ERR_RCVD_TAR_ABT */
		cia_decode_pci_error(cia, "PCI target abort");
		break;
	case 9: /* CIA_ERR_PA_PTE_INV */
		cia_decode_pci_error(cia, "PCI invalid PTE");
		break;
	case 10: /* CIA_ERR_FROM_WRT_ERR */
		cia_decode_mem_error(cia, "Write to flash ROM attempted");
		break;
	case 11: /* CIA_ERR_IOA_TIMEOUT */
		cia_decode_pci_error(cia, "I/O timeout");
		break;
	}

	if (cia->cia_err & CIA_ERR_LOST_CORR_ERR)
		printk(KERN_CRIT "CIA lost machine check: "
		       "Correctable ECC error\n");
	if (cia->cia_err & CIA_ERR_LOST_UN_CORR_ERR)
		printk(KERN_CRIT "CIA lost machine check: "
		       "Uncorrectable ECC error\n");
	if (cia->cia_err & CIA_ERR_LOST_CPU_PE)
		printk(KERN_CRIT "CIA lost machine check: "
		       "System bus parity error\n");
	if (cia->cia_err & CIA_ERR_LOST_MEM_NEM)
		printk(KERN_CRIT "CIA lost machine check: "
		       "Access to nonexistent memory\n");
	if (cia->cia_err & CIA_ERR_LOST_PERR)
		printk(KERN_CRIT "CIA lost machine check: "
		       "PCI data parity error\n");
	if (cia->cia_err & CIA_ERR_LOST_PCI_ADDR_PE)
		printk(KERN_CRIT "CIA lost machine check: "
		       "PCI address parity error\n");
	if (cia->cia_err & CIA_ERR_LOST_RCVD_MAS_ABT)
		printk(KERN_CRIT "CIA lost machine check: "
		       "PCI master abort\n");
	if (cia->cia_err & CIA_ERR_LOST_RCVD_TAR_ABT)
		printk(KERN_CRIT "CIA lost machine check: "
		       "PCI target abort\n");
	if (cia->cia_err & CIA_ERR_LOST_PA_PTE_INV)
		printk(KERN_CRIT "CIA lost machine check: "
		       "PCI invalid PTE\n");
	if (cia->cia_err & CIA_ERR_LOST_FROM_WRT_ERR)
		printk(KERN_CRIT "CIA lost machine check: "
		       "Write to flash ROM attempted\n");
	if (cia->cia_err & CIA_ERR_LOST_IOA_TIMEOUT)
		printk(KERN_CRIT "CIA lost machine check: "
		       "I/O timeout\n");

	return 1;
}

void
cia_machine_check(unsigned long vector, unsigned long la_ptr,
		  struct pt_regs * regs)
{
	int expected;

	/* Clear the error before any reporting.  */
	mb();
	mb();  /* magic */
	draina();
	cia_pci_clr_err();
	wrmces(rdmces());	/* reset machine check pending flag.  */
	mb();

	expected = mcheck_expected(0);
	if (!expected && vector == 0x660)
		expected = cia_decode_mchk(la_ptr);
	process_mcheck_info(vector, la_ptr, regs, "CIA", expected);
}
