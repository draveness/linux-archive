#ifndef __ASM_PARISC_PCI_H
#define __ASM_PARISC_PCI_H

#include <linux/config.h>
#include <asm/scatterlist.h>

/*
** HP PCI platforms generally support multiple bus adapters.
**    (workstations 1-~4, servers 2-~32)
**
** Newer platforms number the busses across PCI bus adapters *sparsely*.
** E.g. 0, 8, 16, ...
**
** Under a PCI bus, most HP platforms support PPBs up to two or three
** levels deep. See "Bit3" product line. 
*/
#define PCI_MAX_BUSSES	256

/*
** pci_hba_data (aka H2P_OBJECT in HP/UX)
**
** This is the "common" or "base" data structure which HBA drivers
** (eg Dino or LBA) are required to place at the top of their own
** platform_data structure.  I've heard this called "C inheritance" too.
**
** Data needed by pcibios layer belongs here.
*/
struct pci_hba_data {
	unsigned long	base_addr;	/* aka Host Physical Address */
	const struct parisc_device *dev; /* device from PA bus walk */
	struct pci_bus *hba_bus;	/* primary PCI bus below HBA */
	int		hba_num;	/* I/O port space access "key" */
	struct resource bus_num;	/* PCI bus numbers */
	struct resource io_space;	/* PIOP */
	struct resource lmmio_space;	/* bus addresses < 4Gb */
	struct resource elmmio_space;	/* additional bus addresses < 4Gb */
	struct resource gmmio_space;	/* bus addresses > 4Gb */
	/* NOTE: Dino code assumes it can use *all* of the lmmio_space,
	 * elmmio_space and gmmio_space as a contiguous array of
	 * resources.  This #define represents the array size */
	#define DINO_MAX_LMMIO_RESOURCES	3

	unsigned long   lmmio_space_offset;  /* CPU view - PCI view */
	void *          iommu;          /* IOMMU this device is under */
	/* REVISIT - spinlock to protect resources? */
};

#define HBA_DATA(d)		((struct pci_hba_data *) (d))

/* 
** We support 2^16 I/O ports per HBA.  These are set up in the form
** 0xbbxxxx, where bb is the bus number and xxxx is the I/O port
** space address.
*/
#define HBA_PORT_SPACE_BITS	16

#define HBA_PORT_BASE(h)	((h) << HBA_PORT_SPACE_BITS)
#define HBA_PORT_SPACE_SIZE	(1UL << HBA_PORT_SPACE_BITS)

#define PCI_PORT_HBA(a)		((a) >> HBA_PORT_SPACE_BITS)
#define PCI_PORT_ADDR(a)	((a) & (HBA_PORT_SPACE_SIZE - 1))

/*
** Convert between PCI (IO_VIEW) addresses and processor (PA_VIEW) addresses.
** Note that we currently support only LMMIO.
*/
#define PCI_BUS_ADDR(hba,a)	((a) - hba->lmmio_space_offset)
#define PCI_HOST_ADDR(hba,a)	((a) + hba->lmmio_space_offset)

/*
** KLUGE: linux/pci.h include asm/pci.h BEFORE declaring struct pci_bus
** (This eliminates some of the warnings).
*/
struct pci_bus;
struct pci_dev;

/* The PCI address space does equal the physical memory
 * address space.  The networking and block device layers use
 * this boolean for bounce buffer decisions.
 */
#define PCI_DMA_BUS_IS_PHYS     (1)

/*
** Most PCI devices (eg Tulip, NCR720) also export the same registers
** to both MMIO and I/O port space.  Due to poor performance of I/O Port
** access under HP PCI bus adapters, strongly reccomend use of MMIO
** address space.
**
** While I'm at it more PA programming notes:
**
** 1) MMIO stores (writes) are posted operations. This means the processor
**    gets an "ACK" before the write actually gets to the device. A read
**    to the same device (or typically the bus adapter above it) will
**    force in-flight write transaction(s) out to the targeted device
**    before the read can complete.
**
** 2) The Programmed I/O (PIO) data may not always be strongly ordered with
**    respect to DMA on all platforms. Ie PIO data can reach the processor
**    before in-flight DMA reaches memory. Since most SMP PA platforms
**    are I/O coherent, it generally doesn't matter...but sometimes
**    it does.
**
** I've helped device driver writers debug both types of problems.
*/
struct pci_port_ops {
	  u8 (*inb)  (struct pci_hba_data *hba, u16 port);
	 u16 (*inw)  (struct pci_hba_data *hba, u16 port);
	 u32 (*inl)  (struct pci_hba_data *hba, u16 port);
	void (*outb) (struct pci_hba_data *hba, u16 port,  u8 data);
	void (*outw) (struct pci_hba_data *hba, u16 port, u16 data);
	void (*outl) (struct pci_hba_data *hba, u16 port, u32 data);
};


struct pci_bios_ops {
	void (*init)(void);
	void (*fixup_bus)(struct pci_bus *bus);
};

/* pci_unmap_{single,page} is not a nop, thus... */
#define DECLARE_PCI_UNMAP_ADDR(ADDR_NAME)	\
	dma_addr_t ADDR_NAME;
#define DECLARE_PCI_UNMAP_LEN(LEN_NAME)		\
	__u32 LEN_NAME;
#define pci_unmap_addr(PTR, ADDR_NAME)			\
	((PTR)->ADDR_NAME)
#define pci_unmap_addr_set(PTR, ADDR_NAME, VAL)		\
	(((PTR)->ADDR_NAME) = (VAL))
#define pci_unmap_len(PTR, LEN_NAME)			\
	((PTR)->LEN_NAME)
#define pci_unmap_len_set(PTR, LEN_NAME, VAL)		\
	(((PTR)->LEN_NAME) = (VAL))

/*
** Stuff declared in arch/parisc/kernel/pci.c
*/
extern struct pci_port_ops *pci_port;
extern struct pci_bios_ops *pci_bios;
extern int pci_post_reset_delay;	/* delay after de-asserting #RESET */
extern int pci_hba_count;
extern struct pci_hba_data *parisc_pci_hba[];

#ifdef CONFIG_PCI
extern void pcibios_register_hba(struct pci_hba_data *);
extern void pcibios_set_master(struct pci_dev *);
#else
extern inline void pcibios_register_hba(struct pci_hba_data *x)
{
}
#endif

/*
** used by drivers/pci/pci.c:pci_do_scan_bus()
**   0 == check if bridge is numbered before re-numbering.
**   1 == pci_do_scan_bus() should automatically number all PCI-PCI bridges.
**
** REVISIT:
**   To date, only alpha sets this to one. We'll need to set this
**   to zero for legacy platforms and one for PAT platforms.
*/
#define pcibios_assign_all_busses()     (pdc_type == PDC_TYPE_PAT)
#define pcibios_scan_all_fns(a, b)	0

#define PCIBIOS_MIN_IO          0x10
#define PCIBIOS_MIN_MEM         0x1000 /* NBPG - but pci/setup-res.c dies */

/* Don't support DAC yet. */
#define pci_dac_dma_supported(pci_dev, mask)   (0)

/* export the pci_ DMA API in terms of the dma_ one */
#include <asm-generic/pci-dma-compat.h>

extern void
pcibios_resource_to_bus(struct pci_dev *dev, struct pci_bus_region *region,
			 struct resource *res);

static inline void pcibios_add_platform_entries(struct pci_dev *dev)
{
}

#endif /* __ASM_PARISC_PCI_H */
