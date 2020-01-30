#ifndef _ASM_IA64_PCI_H
#define _ASM_IA64_PCI_H

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/io.h>
#include <asm/scatterlist.h>

/*
 * Can be used to override the logic in pci_scan_bus for skipping already-configured bus
 * numbers - to be used for buggy BIOSes or architectures with incomplete PCI setup by the
 * loader.
 */
#define pcibios_assign_all_busses()     0
#define pcibios_scan_all_fns(a, b)	0

#define PCIBIOS_MIN_IO		0x1000
#define PCIBIOS_MIN_MEM		0x10000000

void pcibios_config_init(void);

struct pci_dev;

/*
 * PCI_DMA_BUS_IS_PHYS should be set to 1 if there is _necessarily_ a direct correspondence
 * between device bus addresses and CPU physical addresses.  Platforms with a hardware I/O
 * MMU _must_ turn this off to suppress the bounce buffer handling code in the block and
 * network device layers.  Platforms with separate bus address spaces _must_ turn this off
 * and provide a device DMA mapping implementation that takes care of the necessary
 * address translation.
 *
 * For now, the ia64 platforms which may have separate/multiple bus address spaces all
 * have I/O MMUs which support the merging of physically discontiguous buffers, so we can
 * use that as the sole factor to determine the setting of PCI_DMA_BUS_IS_PHYS.
 */
extern unsigned long ia64_max_iommu_merge_mask;
#define PCI_DMA_BUS_IS_PHYS	(ia64_max_iommu_merge_mask == ~0UL)

static inline void
pcibios_set_master (struct pci_dev *dev)
{
	/* No special bus mastering setup handling */
}

static inline void
pcibios_penalize_isa_irq (int irq)
{
	/* We don't do dynamic PCI IRQ allocation */
}

#define HAVE_ARCH_PCI_MWI 1
extern int pcibios_prep_mwi (struct pci_dev *);

#include <asm-generic/pci-dma-compat.h>

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

/* The ia64 platform always supports 64-bit addressing. */
#define pci_dac_dma_supported(pci_dev, mask)		(1)
#define pci_dac_page_to_dma(dev,pg,off,dir)		((dma_addr_t) page_to_bus(pg) + (off))
#define pci_dac_dma_to_page(dev,dma_addr)		(virt_to_page(bus_to_virt(dma_addr)))
#define pci_dac_dma_to_offset(dev,dma_addr)		offset_in_page(dma_addr)
#define pci_dac_dma_sync_single_for_cpu(dev,dma_addr,len,dir)	do { } while (0)
#define pci_dac_dma_sync_single_for_device(dev,dma_addr,len,dir)	do { mb(); } while (0)

#define sg_dma_len(sg)		((sg)->dma_length)
#define sg_dma_address(sg)	((sg)->dma_address)

#define HAVE_PCI_MMAP
extern int pci_mmap_page_range (struct pci_dev *dev, struct vm_area_struct *vma,
				enum pci_mmap_state mmap_state, int write_combine);

struct pci_window {
	struct resource resource;
	u64 offset;
};

struct pci_controller {
	void *acpi_handle;
	void *iommu;
	int segment;

	unsigned int windows;
	struct pci_window *window;

	void *platform_data;
};

#define PCI_CONTROLLER(busdev) ((struct pci_controller *) busdev->sysdata)
#define pci_domain_nr(busdev)    (PCI_CONTROLLER(busdev)->segment)

static inline int pci_name_bus(char *name, struct pci_bus *bus)
{
	if (pci_domain_nr(bus) == 0) {
		sprintf(name, "%02x", bus->number);
	} else {
		sprintf(name, "%04x:%02x", pci_domain_nr(bus), bus->number);
	}
	return 0;
}

static inline void pcibios_add_platform_entries(struct pci_dev *dev)
{
}

/* generic pci stuff */
#include <asm-generic/pci.h>

#endif /* _ASM_IA64_PCI_H */
