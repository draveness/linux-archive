/* $Id: pbm.h,v 1.22 2000/03/25 05:18:30 davem Exp $
 * pbm.h: UltraSparc PCI controller software state.
 *
 * Copyright (C) 1997, 1998, 1999 David S. Miller (davem@redhat.com)
 */

#ifndef __SPARC64_PBM_H
#define __SPARC64_PBM_H

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>

#include <asm/io.h>
#include <asm/page.h>
#include <asm/oplib.h>

/* The abstraction used here is that there are PCI controllers,
 * each with one (Sabre) or two (PSYCHO/SCHIZO) PCI bus modules
 * underneath.  Each PCI controller has a single IOMMU shared
 * by the PCI bus modules underneath, and if a streaming buffer
 * is present, each PCI bus module has it's own. (ie. the IOMMU
 * is shared between PBMs, the STC is not)  Furthermore, each
 * PCI bus module controls it's own autonomous PCI bus.
 */

#define PBM_LOGCLUSTERS 3
#define PBM_NCLUSTERS (1 << PBM_LOGCLUSTERS)

struct pci_controller_info;

/* This contains the software state necessary to drive a PCI
 * controller's IOMMU.
 */
struct pci_iommu {
	/* This protects the controller's IOMMU and all
	 * streaming buffers underneath.
	 */
	spinlock_t	lock;

	/* Context allocator. */
	unsigned int	iommu_cur_ctx;

	/* IOMMU page table, a linear array of ioptes. */
	iopte_t		*page_table;		/* The page table itself. */
	int		page_table_sz_bits;	/* log2 of ow many pages does it map? */

	/* Base PCI memory space address where IOMMU mappings
	 * begin.
	 */
	u32		page_table_map_base;

	/* IOMMU Controller Registers */
	unsigned long	iommu_control;		/* IOMMU control register */
	unsigned long	iommu_tsbbase;		/* IOMMU page table base register */
	unsigned long	iommu_flush;		/* IOMMU page flush register */
	unsigned long	iommu_ctxflush;		/* IOMMU context flush register */

	/* This is a register in the PCI controller, which if
	 * read will have no side-effects but will guarentee
	 * completion of all previous writes into IOMMU/STC.
	 */
	unsigned long	write_complete_reg;

	/* The lowest used consistent mapping entry.  Since
	 * we allocate consistent maps out of cluster 0 this
	 * is relative to the beginning of closter 0.
	 */
	u32		lowest_consistent_map;

	/* If PBM_NCLUSTERS is ever decreased to 4 or lower,
	 * or if largest supported page_table_sz * 8K goes above
	 * 2GB, you must increase the size of the type of
	 * these counters.  You have been duly warned. -DaveM
	 */
	struct {
		u16	next;
		u16	flush;
	} alloc_info[PBM_NCLUSTERS];

	/* Here a PCI controller driver describes the areas of
	 * PCI memory space where DMA to/from physical memory
	 * are addressed.  Drivers interrogate the PCI layer
	 * if their device has addressing limitations.  They
	 * do so via pci_dma_supported, and pass in a mask of
	 * DMA address bits their device can actually drive.
	 *
	 * The test for being usable is:
	 * 	(device_mask & dma_addr_mask) == dma_addr_mask
	 */
	u32 dma_addr_mask;
};

/* This describes a PCI bus module's streaming buffer. */
struct pci_strbuf {
	int		strbuf_enabled;		/* Present and using it? */

	/* Streaming Buffer Control Registers */
	unsigned long	strbuf_control;		/* STC control register */
	unsigned long	strbuf_pflush;		/* STC page flush register */
	unsigned long	strbuf_fsync;		/* STC flush synchronization reg */
	unsigned long	strbuf_ctxflush;	/* STC context flush register */
	unsigned long	strbuf_ctxmatch_base;	/* STC context flush match reg */
	unsigned long	strbuf_flushflag_pa;	/* Physical address of flush flag */
	volatile unsigned long *strbuf_flushflag; /* The flush flag itself */

	/* And this is the actual flush flag area.
	 * We allocate extra because the chips require
	 * a 64-byte aligned area.
	 */
	volatile unsigned long	__flushflag_buf[(64 + (64 - 1)) / sizeof(long)];
};

#define PCI_STC_FLUSHFLAG_INIT(STC) \
	(*((STC)->strbuf_flushflag) = 0UL)
#define PCI_STC_FLUSHFLAG_SET(STC) \
	(*((STC)->strbuf_flushflag) != 0UL)

/* There can be quite a few ranges and interrupt maps on a PCI
 * segment.  Thus...
 */
#define PROM_PCIRNG_MAX		64
#define PROM_PCIIMAP_MAX	64

struct pci_pbm_info {
	/* PCI controller we sit under. */
	struct pci_controller_info	*parent;

	/* Name used for top-level resources. */
	char				name[64];

	/* OBP specific information. */
	int				prom_node;
	char				prom_name[64];
	struct linux_prom_pci_ranges	pbm_ranges[PROM_PCIRNG_MAX];
	int				num_pbm_ranges;
	struct linux_prom_pci_intmap	pbm_intmap[PROM_PCIIMAP_MAX];
	int				num_pbm_intmap;
	struct linux_prom_pci_intmask	pbm_intmask;

	/* PBM I/O and Memory space resources. */
	struct resource			io_space;
	struct resource			mem_space;

	/* State of 66MHz capabilities on this PBM. */
	int				is_66mhz_capable;
	int				all_devs_66mhz;

	/* This PBM's streaming buffer. */
	struct pci_strbuf		stc;

	/* Now things for the actual PCI bus probes. */
	unsigned int			pci_first_busno;
	unsigned int			pci_last_busno;
	struct pci_bus			*pci_bus;
};

struct pci_controller_info {
	/* List of all PCI controllers. */
	struct pci_controller_info	*next;

	/* Physical address base of controller registers
	 * and PCI config space.
	 */
	unsigned long			controller_regs;
	unsigned long			config_space;

	/* Opaque 32-bit system bus Port ID. */
	u32				portid;

	/* Each controller gets a unique index, used mostly for
	 * error logging purposes.
	 */
	int				index;

	/* The PCI bus modules controlled by us. */
	struct pci_pbm_info		pbm_A;
	struct pci_pbm_info		pbm_B;

	/* Operations which are controller specific. */
	void (*scan_bus)(struct pci_controller_info *);
	unsigned int (*irq_build)(struct pci_controller_info *, struct pci_dev *, unsigned int);
	void (*base_address_update)(struct pci_dev *, int);
	void (*resource_adjust)(struct pci_dev *, struct resource *, struct resource *);

	/* Now things for the actual PCI bus probes. */
	struct pci_ops			*pci_ops;
	unsigned int			pci_first_busno;
	unsigned int			pci_last_busno;

	/* IOMMU state shared by both PBM segments. */
	struct pci_iommu		iommu;

	void				*starfire_cookie;
};

/* PCI devices which are not bridges have this placed in their pci_dev
 * sysdata member.  This makes OBP aware PCI device drivers easier to
 * code.
 */
struct pcidev_cookie {
	struct pci_pbm_info		*pbm;
	char				prom_name[64];
	int				prom_node;
	struct linux_prom_pci_registers	prom_regs[PROMREG_MAX];
	int num_prom_regs;
	struct linux_prom_pci_registers prom_assignments[PROMREG_MAX];
	int num_prom_assignments;
};

/* Currently these are the same across all PCI controllers
 * we support.  Someday they may not be...
 */
#define PCI_IRQ_IGN	0x000007c0	/* Interrupt Group Number */
#define PCI_IRQ_INO	0x0000003f	/* Interrupt Number */

#endif /* !(__SPARC64_PBM_H) */
