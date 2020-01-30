/* $Id: pci_sabre.c,v 1.20 2000/06/26 19:40:27 davem Exp $
 * pci_sabre.c: Sabre specific PCI controller support.
 *
 * Copyright (C) 1997, 1998, 1999 David S. Miller (davem@caipfs.rutgers.edu)
 * Copyright (C) 1998, 1999 Eddie C. Dost   (ecd@skynet.be)
 * Copyright (C) 1999 Jakub Jelinek   (jakub@redhat.com)
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/malloc.h>

#include <asm/apb.h>
#include <asm/pbm.h>
#include <asm/iommu.h>
#include <asm/irq.h>

#include "pci_impl.h"

/* All SABRE registers are 64-bits.  The following accessor
 * routines are how they are accessed.  The REG parameter
 * is a physical address.
 */
#define sabre_read(__reg) \
({	u64 __ret; \
	__asm__ __volatile__("ldxa [%1] %2, %0" \
			     : "=r" (__ret) \
			     : "r" (__reg), "i" (ASI_PHYS_BYPASS_EC_E) \
			     : "memory"); \
	__ret; \
})
#define sabre_write(__reg, __val) \
	__asm__ __volatile__("stxa %0, [%1] %2" \
			     : /* no outputs */ \
			     : "r" (__val), "r" (__reg), \
			       "i" (ASI_PHYS_BYPASS_EC_E))

/* SABRE PCI controller register offsets and definitions. */
#define SABRE_UE_AFSR		0x0030UL
#define  SABRE_UEAFSR_PDRD	 0x4000000000000000UL	/* Primary PCI DMA Read */
#define  SABRE_UEAFSR_PDWR	 0x2000000000000000UL	/* Primary PCI DMA Write */
#define  SABRE_UEAFSR_SDRD	 0x0800000000000000UL	/* Secondary PCI DMA Read */
#define  SABRE_UEAFSR_SDWR	 0x0400000000000000UL	/* Secondary PCI DMA Write */
#define  SABRE_UEAFSR_SDTE	 0x0200000000000000UL	/* Secondary DMA Translation Error */
#define  SABRE_UEAFSR_PDTE	 0x0100000000000000UL	/* Primary DMA Translation Error */
#define  SABRE_UEAFSR_BMSK	 0x0000ffff00000000UL	/* Bytemask */
#define  SABRE_UEAFSR_OFF	 0x00000000e0000000UL	/* Offset (AFAR bits [5:3] */
#define  SABRE_UEAFSR_BLK	 0x0000000000800000UL	/* Was block operation */
#define SABRE_UECE_AFAR		0x0038UL
#define SABRE_CE_AFSR		0x0040UL
#define  SABRE_CEAFSR_PDRD	 0x4000000000000000UL	/* Primary PCI DMA Read */
#define  SABRE_CEAFSR_PDWR	 0x2000000000000000UL	/* Primary PCI DMA Write */
#define  SABRE_CEAFSR_SDRD	 0x0800000000000000UL	/* Secondary PCI DMA Read */
#define  SABRE_CEAFSR_SDWR	 0x0400000000000000UL	/* Secondary PCI DMA Write */
#define  SABRE_CEAFSR_ESYND	 0x00ff000000000000UL	/* ECC Syndrome */
#define  SABRE_CEAFSR_BMSK	 0x0000ffff00000000UL	/* Bytemask */
#define  SABRE_CEAFSR_OFF	 0x00000000e0000000UL	/* Offset */
#define  SABRE_CEAFSR_BLK	 0x0000000000800000UL	/* Was block operation */
#define SABRE_UECE_AFAR_ALIAS	0x0048UL	/* Aliases to 0x0038 */
#define SABRE_IOMMU_CONTROL	0x0200UL
#define  SABRE_IOMMUCTRL_ERRSTS	 0x0000000006000000UL	/* Error status bits */
#define  SABRE_IOMMUCTRL_ERR	 0x0000000001000000UL	/* Error present in IOTLB */
#define  SABRE_IOMMUCTRL_LCKEN	 0x0000000000800000UL	/* IOTLB lock enable */
#define  SABRE_IOMMUCTRL_LCKPTR	 0x0000000000780000UL	/* IOTLB lock pointer */
#define  SABRE_IOMMUCTRL_TSBSZ	 0x0000000000070000UL	/* TSB Size */
#define  SABRE_IOMMU_TSBSZ_1K   0x0000000000000000
#define  SABRE_IOMMU_TSBSZ_2K   0x0000000000010000
#define  SABRE_IOMMU_TSBSZ_4K   0x0000000000020000
#define  SABRE_IOMMU_TSBSZ_8K   0x0000000000030000
#define  SABRE_IOMMU_TSBSZ_16K  0x0000000000040000
#define  SABRE_IOMMU_TSBSZ_32K  0x0000000000050000
#define  SABRE_IOMMU_TSBSZ_64K  0x0000000000060000
#define  SABRE_IOMMU_TSBSZ_128K 0x0000000000070000
#define  SABRE_IOMMUCTRL_TBWSZ	 0x0000000000000004UL	/* TSB assumed page size */
#define  SABRE_IOMMUCTRL_DENAB	 0x0000000000000002UL	/* Diagnostic Mode Enable */
#define  SABRE_IOMMUCTRL_ENAB	 0x0000000000000001UL	/* IOMMU Enable */
#define SABRE_IOMMU_TSBBASE	0x0208UL
#define SABRE_IOMMU_FLUSH	0x0210UL
#define SABRE_IMAP_A_SLOT0	0x0c00UL
#define SABRE_IMAP_B_SLOT0	0x0c20UL
#define SABRE_IMAP_SCSI		0x1000UL
#define SABRE_IMAP_ETH		0x1008UL
#define SABRE_IMAP_BPP		0x1010UL
#define SABRE_IMAP_AU_REC	0x1018UL
#define SABRE_IMAP_AU_PLAY	0x1020UL
#define SABRE_IMAP_PFAIL	0x1028UL
#define SABRE_IMAP_KMS		0x1030UL
#define SABRE_IMAP_FLPY		0x1038UL
#define SABRE_IMAP_SHW		0x1040UL
#define SABRE_IMAP_KBD		0x1048UL
#define SABRE_IMAP_MS		0x1050UL
#define SABRE_IMAP_SER		0x1058UL
#define SABRE_IMAP_UE		0x1070UL
#define SABRE_IMAP_CE		0x1078UL
#define SABRE_IMAP_PCIERR	0x1080UL
#define SABRE_IMAP_GFX		0x1098UL
#define SABRE_IMAP_EUPA		0x10a0UL
#define SABRE_ICLR_A_SLOT0	0x1400UL
#define SABRE_ICLR_B_SLOT0	0x1480UL
#define SABRE_ICLR_SCSI		0x1800UL
#define SABRE_ICLR_ETH		0x1808UL
#define SABRE_ICLR_BPP		0x1810UL
#define SABRE_ICLR_AU_REC	0x1818UL
#define SABRE_ICLR_AU_PLAY	0x1820UL
#define SABRE_ICLR_PFAIL	0x1828UL
#define SABRE_ICLR_KMS		0x1830UL
#define SABRE_ICLR_FLPY		0x1838UL
#define SABRE_ICLR_SHW		0x1840UL
#define SABRE_ICLR_KBD		0x1848UL
#define SABRE_ICLR_MS		0x1850UL
#define SABRE_ICLR_SER		0x1858UL
#define SABRE_ICLR_UE		0x1870UL
#define SABRE_ICLR_CE		0x1878UL
#define SABRE_ICLR_PCIERR	0x1880UL
#define SABRE_WRSYNC		0x1c20UL
#define SABRE_PCICTRL		0x2000UL
#define  SABRE_PCICTRL_MRLEN	 0x0000001000000000UL	/* Use MemoryReadLine for block loads/stores */
#define  SABRE_PCICTRL_SERR	 0x0000000400000000UL	/* Set when SERR asserted on PCI bus */
#define  SABRE_PCICTRL_ARBPARK	 0x0000000000200000UL	/* Bus Parking 0=Ultra-IIi 1=prev-bus-owner */
#define  SABRE_PCICTRL_CPUPRIO	 0x0000000000100000UL	/* Ultra-IIi granted every other bus cycle */
#define  SABRE_PCICTRL_ARBPRIO	 0x00000000000f0000UL	/* Slot which is granted every other bus cycle */
#define  SABRE_PCICTRL_ERREN	 0x0000000000000100UL	/* PCI Error Interrupt Enable */
#define  SABRE_PCICTRL_RTRYWE	 0x0000000000000080UL	/* DMA Flow Control 0=wait-if-possible 1=retry */
#define  SABRE_PCICTRL_AEN	 0x000000000000000fUL	/* Slot PCI arbitration enables */
#define SABRE_PIOAFSR		0x2010UL
#define  SABRE_PIOAFSR_PMA	 0x8000000000000000UL	/* Primary Master Abort */
#define  SABRE_PIOAFSR_PTA	 0x4000000000000000UL	/* Primary Target Abort */
#define  SABRE_PIOAFSR_PRTRY	 0x2000000000000000UL	/* Primary Excessive Retries */
#define  SABRE_PIOAFSR_PPERR	 0x1000000000000000UL	/* Primary Parity Error */
#define  SABRE_PIOAFSR_SMA	 0x0800000000000000UL	/* Secondary Master Abort */
#define  SABRE_PIOAFSR_STA	 0x0400000000000000UL	/* Secondary Target Abort */
#define  SABRE_PIOAFSR_SRTRY	 0x0200000000000000UL	/* Secondary Excessive Retries */
#define  SABRE_PIOAFSR_SPERR	 0x0100000000000000UL	/* Secondary Parity Error */
#define  SABRE_PIOAFSR_BMSK	 0x0000ffff00000000UL	/* Byte Mask */
#define  SABRE_PIOAFSR_BLK	 0x0000000080000000UL	/* Was Block Operation */
#define SABRE_PIOAFAR		0x2018UL
#define SABRE_PCIDIAG		0x2020UL
#define  SABRE_PCIDIAG_DRTRY	 0x0000000000000040UL	/* Disable PIO Retry Limit */
#define  SABRE_PCIDIAG_IPAPAR	 0x0000000000000008UL	/* Invert PIO Address Parity */
#define  SABRE_PCIDIAG_IPDPAR	 0x0000000000000004UL	/* Invert PIO Data Parity */
#define  SABRE_PCIDIAG_IDDPAR	 0x0000000000000002UL	/* Invert DMA Data Parity */
#define  SABRE_PCIDIAG_ELPBK	 0x0000000000000001UL	/* Loopback Enable - not supported */
#define SABRE_PCITASR		0x2028UL
#define  SABRE_PCITASR_EF	 0x0000000000000080UL	/* Respond to 0xe0000000-0xffffffff */
#define  SABRE_PCITASR_CD	 0x0000000000000040UL	/* Respond to 0xc0000000-0xdfffffff */
#define  SABRE_PCITASR_AB	 0x0000000000000020UL	/* Respond to 0xa0000000-0xbfffffff */
#define  SABRE_PCITASR_89	 0x0000000000000010UL	/* Respond to 0x80000000-0x9fffffff */
#define  SABRE_PCITASR_67	 0x0000000000000008UL	/* Respond to 0x60000000-0x7fffffff */
#define  SABRE_PCITASR_45	 0x0000000000000004UL	/* Respond to 0x40000000-0x5fffffff */
#define  SABRE_PCITASR_23	 0x0000000000000002UL	/* Respond to 0x20000000-0x3fffffff */
#define  SABRE_PCITASR_01	 0x0000000000000001UL	/* Respond to 0x00000000-0x1fffffff */
#define SABRE_PIOBUF_DIAG	0x5000UL
#define SABRE_DMABUF_DIAGLO	0x5100UL
#define SABRE_DMABUF_DIAGHI	0x51c0UL
#define SABRE_IMAP_GFX_ALIAS	0x6000UL	/* Aliases to 0x1098 */
#define SABRE_IMAP_EUPA_ALIAS	0x8000UL	/* Aliases to 0x10a0 */
#define SABRE_IOMMU_VADIAG	0xa400UL
#define SABRE_IOMMU_TCDIAG	0xa408UL
#define SABRE_IOMMU_TAG		0xa580UL
#define  SABRE_IOMMUTAG_ERRSTS	 0x0000000001800000UL	/* Error status bits */
#define  SABRE_IOMMUTAG_ERR	 0x0000000000400000UL	/* Error present */
#define  SABRE_IOMMUTAG_WRITE	 0x0000000000200000UL	/* Page is writable */
#define  SABRE_IOMMUTAG_STREAM	 0x0000000000100000UL	/* Streamable bit - unused */
#define  SABRE_IOMMUTAG_SIZE	 0x0000000000080000UL	/* 0=8k 1=16k */
#define  SABRE_IOMMUTAG_VPN	 0x000000000007ffffUL	/* Virtual Page Number [31:13] */
#define SABRE_IOMMU_DATA	0xa600UL
#define SABRE_IOMMUDATA_VALID	 0x0000000040000000UL	/* Valid */
#define SABRE_IOMMUDATA_USED	 0x0000000020000000UL	/* Used (for LRU algorithm) */
#define SABRE_IOMMUDATA_CACHE	 0x0000000010000000UL	/* Cacheable */
#define SABRE_IOMMUDATA_PPN	 0x00000000001fffffUL	/* Physical Page Number [33:13] */
#define SABRE_PCI_IRQSTATE	0xa800UL
#define SABRE_OBIO_IRQSTATE	0xa808UL
#define SABRE_FFBCFG		0xf000UL
#define  SABRE_FFBCFG_SPRQS	 0x000000000f000000	/* Slave P_RQST queue size */
#define  SABRE_FFBCFG_ONEREAD	 0x0000000000004000	/* Slave supports one outstanding read */
#define SABRE_MCCTRL0		0xf010UL
#define  SABRE_MCCTRL0_RENAB	 0x0000000080000000	/* Refresh Enable */
#define  SABRE_MCCTRL0_EENAB	 0x0000000010000000	/* Enable all ECC functions */
#define  SABRE_MCCTRL0_11BIT	 0x0000000000001000	/* Enable 11-bit column addressing */
#define  SABRE_MCCTRL0_DPP	 0x0000000000000f00	/* DIMM Pair Present Bits */
#define  SABRE_MCCTRL0_RINTVL	 0x00000000000000ff	/* Refresh Interval */
#define SABRE_MCCTRL1		0xf018UL
#define  SABRE_MCCTRL1_AMDC	 0x0000000038000000	/* Advance Memdata Clock */
#define  SABRE_MCCTRL1_ARDC	 0x0000000007000000	/* Advance DRAM Read Data Clock */
#define  SABRE_MCCTRL1_CSR	 0x0000000000e00000	/* CAS to RAS delay for CBR refresh */
#define  SABRE_MCCTRL1_CASRW	 0x00000000001c0000	/* CAS length for read/write */
#define  SABRE_MCCTRL1_RCD	 0x0000000000038000	/* RAS to CAS delay */
#define  SABRE_MCCTRL1_CP	 0x0000000000007000	/* CAS Precharge */
#define  SABRE_MCCTRL1_RP	 0x0000000000000e00	/* RAS Precharge */
#define  SABRE_MCCTRL1_RAS	 0x00000000000001c0	/* Length of RAS for refresh */
#define  SABRE_MCCTRL1_CASRW2	 0x0000000000000038	/* Must be same as CASRW */
#define  SABRE_MCCTRL1_RSC	 0x0000000000000007	/* RAS after CAS hold time */
#define SABRE_RESETCTRL		0xf020UL

#define SABRE_CONFIGSPACE	0x001000000UL
#define SABRE_IOSPACE		0x002000000UL
#define SABRE_IOSPACE_SIZE	0x00000ffffUL
#define SABRE_MEMSPACE		0x100000000UL
#define SABRE_MEMSPACE_SIZE	0x07fffffffUL

/* UltraSparc-IIi Programmer's Manual, page 325, PCI
 * configuration space address format:
 * 
 *  32             24 23 16 15    11 10       8 7   2  1 0
 * ---------------------------------------------------------
 * |0 0 0 0 0 0 0 0 1| bus | device | function | reg | 0 0 |
 * ---------------------------------------------------------
 */
#define SABRE_CONFIG_BASE(PBM)	\
	((PBM)->parent->config_space | (1UL << 24))
#define SABRE_CONFIG_ENCODE(BUS, DEVFN, REG)	\
	(((unsigned long)(BUS)   << 16) |	\
	 ((unsigned long)(DEVFN) << 8)  |	\
	 ((unsigned long)(REG)))

static void *sabre_pci_config_mkaddr(struct pci_pbm_info *pbm,
				     unsigned char bus,
				     unsigned int devfn,
				     int where)
{
	if (!pbm)
		return NULL;
	return (void *)
		(SABRE_CONFIG_BASE(pbm) |
		 SABRE_CONFIG_ENCODE(bus, devfn, where));
}

static int sabre_out_of_range(unsigned char devfn)
{
	return (((PCI_SLOT(devfn) == 0) && (PCI_FUNC(devfn) > 0)) ||
		((PCI_SLOT(devfn) == 1) && (PCI_FUNC(devfn) > 1)) ||
		(PCI_SLOT(devfn) > 1));
}

static int __sabre_out_of_range(struct pci_pbm_info *pbm,
				unsigned char bus,
				unsigned char devfn)
{
	return ((pbm->parent == 0) ||
		((pbm == &pbm->parent->pbm_B) &&
		 (bus == pbm->pci_first_busno) &&
		 PCI_SLOT(devfn) > 8) ||
		((pbm == &pbm->parent->pbm_A) &&
		 (bus == pbm->pci_first_busno) &&
		 PCI_SLOT(devfn) > 8));
}

static int __sabre_read_byte(struct pci_dev *dev, int where, u8 *value)
{
	struct pci_pbm_info *pbm = pci_bus2pbm[dev->bus->number];
	unsigned char bus = dev->bus->number;
	unsigned int devfn = dev->devfn;
	u8 *addr;

	*value = 0xff;
	addr = sabre_pci_config_mkaddr(pbm, bus, devfn, where);
	if (!addr)
		return PCIBIOS_SUCCESSFUL;

	if (__sabre_out_of_range(pbm, bus, devfn))
		return PCIBIOS_SUCCESSFUL;
	pci_config_read8(addr, value);
	return PCIBIOS_SUCCESSFUL;
}

static int __sabre_read_word(struct pci_dev *dev, int where, u16 *value)
{
	struct pci_pbm_info *pbm = pci_bus2pbm[dev->bus->number];
	unsigned char bus = dev->bus->number;
	unsigned int devfn = dev->devfn;
	u16 *addr;

	*value = 0xffff;
	addr = sabre_pci_config_mkaddr(pbm, bus, devfn, where);
	if (!addr)
		return PCIBIOS_SUCCESSFUL;

	if (__sabre_out_of_range(pbm, bus, devfn))
		return PCIBIOS_SUCCESSFUL;

	if (where & 0x01) {
		printk("pcibios_read_config_word: misaligned reg [%x]\n",
		       where);
		return PCIBIOS_SUCCESSFUL;
	}
	pci_config_read16(addr, value);
	return PCIBIOS_SUCCESSFUL;
}

static int __sabre_read_dword(struct pci_dev *dev, int where, u32 *value)
{
	struct pci_pbm_info *pbm = pci_bus2pbm[dev->bus->number];
	unsigned char bus = dev->bus->number;
	unsigned int devfn = dev->devfn;
	u32 *addr;

	*value = 0xffffffff;
	addr = sabre_pci_config_mkaddr(pbm, bus, devfn, where);
	if (!addr)
		return PCIBIOS_SUCCESSFUL;

	if (__sabre_out_of_range(pbm, bus, devfn))
		return PCIBIOS_SUCCESSFUL;

	if (where & 0x03) {
		printk("pcibios_read_config_dword: misaligned reg [%x]\n",
		       where);
		return PCIBIOS_SUCCESSFUL;
	}
	pci_config_read32(addr, value);
	return PCIBIOS_SUCCESSFUL;
}

static int sabre_read_byte(struct pci_dev *dev, int where, u8 *value)
{
	if (dev->bus->number)
		return __sabre_read_byte(dev, where, value);

	if (sabre_out_of_range(dev->devfn)) {
		*value = 0xff;
		return PCIBIOS_SUCCESSFUL;
	}

	if (where < 8) {
		u16 tmp;

		__sabre_read_word(dev, where & ~1, &tmp);
		if (where & 1)
			*value = tmp >> 8;
		else
			*value = tmp & 0xff;
		return PCIBIOS_SUCCESSFUL;
	} else
		return __sabre_read_byte(dev, where, value);
}

static int sabre_read_word(struct pci_dev *dev, int where, u16 *value)
{
	if (dev->bus->number)
		return __sabre_read_word(dev, where, value);

	if (sabre_out_of_range(dev->devfn)) {
		*value = 0xffff;
		return PCIBIOS_SUCCESSFUL;
	}

	if (where < 8)
		return __sabre_read_word(dev, where, value);
	else {
		u8 tmp;

		__sabre_read_byte(dev, where, &tmp);
		*value = tmp;
		__sabre_read_byte(dev, where + 1, &tmp);
		*value |= tmp << 8;
		return PCIBIOS_SUCCESSFUL;
	}
}

static int sabre_read_dword(struct pci_dev *dev, int where, u32 *value)
{
	u16 tmp;

	if (dev->bus->number)
		return __sabre_read_dword(dev, where, value);

	if (sabre_out_of_range(dev->devfn)) {
		*value = 0xffffffff;
		return PCIBIOS_SUCCESSFUL;
	}

	sabre_read_word(dev, where, &tmp);
	*value = tmp;
	sabre_read_word(dev, where + 2, &tmp);
	*value |= tmp << 16;
	return PCIBIOS_SUCCESSFUL;
}

static int __sabre_write_byte(struct pci_dev *dev, int where, u8 value)
{
	struct pci_pbm_info *pbm = pci_bus2pbm[dev->bus->number];
	unsigned char bus = dev->bus->number;
	unsigned int devfn = dev->devfn;
	u8 *addr;

	addr = sabre_pci_config_mkaddr(pbm, bus, devfn, where);
	if (!addr)
		return PCIBIOS_SUCCESSFUL;

	if (__sabre_out_of_range(pbm, bus, devfn))
		return PCIBIOS_SUCCESSFUL;
	pci_config_write8(addr, value);
	return PCIBIOS_SUCCESSFUL;
}

static int __sabre_write_word(struct pci_dev *dev, int where, u16 value)
{
	struct pci_pbm_info *pbm = pci_bus2pbm[dev->bus->number];
	unsigned char bus = dev->bus->number;
	unsigned int devfn = dev->devfn;
	u16 *addr;

	addr = sabre_pci_config_mkaddr(pbm, bus, devfn, where);
	if (!addr)
		return PCIBIOS_SUCCESSFUL;

	if (__sabre_out_of_range(pbm, bus, devfn))
		return PCIBIOS_SUCCESSFUL;

	if (where & 0x01) {
		printk("pcibios_write_config_word: misaligned reg [%x]\n",
		       where);
		return PCIBIOS_SUCCESSFUL;
	}
	pci_config_write16(addr, value);
	return PCIBIOS_SUCCESSFUL;
}

static int __sabre_write_dword(struct pci_dev *dev, int where, u32 value)
{
	struct pci_pbm_info *pbm = pci_bus2pbm[dev->bus->number];
	unsigned char bus = dev->bus->number;
	unsigned int devfn = dev->devfn;
	u32 *addr;

	addr = sabre_pci_config_mkaddr(pbm, bus, devfn, where);
	if (!addr)
		return PCIBIOS_SUCCESSFUL;

	if (__sabre_out_of_range(pbm, bus, devfn))
		return PCIBIOS_SUCCESSFUL;

	if (where & 0x03) {
		printk("pcibios_write_config_dword: misaligned reg [%x]\n",
		       where);
		return PCIBIOS_SUCCESSFUL;
	}
	pci_config_write32(addr, value);
	return PCIBIOS_SUCCESSFUL;
}

static int sabre_write_byte(struct pci_dev *dev, int where, u8 value)
{
	if (dev->bus->number)
		return __sabre_write_byte(dev, where, value);

	if (sabre_out_of_range(dev->devfn))
		return PCIBIOS_SUCCESSFUL;

	if (where < 8) {
		u16 tmp;

		__sabre_read_word(dev, where & ~1, &tmp);
		if (where & 1) {
			value &= 0x00ff;
			value |= tmp << 8;
		} else {
			value &= 0xff00;
			value |= tmp;
		}
		return __sabre_write_word(dev, where & ~1, tmp);
	} else
		return __sabre_write_byte(dev, where, value);
}

static int sabre_write_word(struct pci_dev *dev, int where, u16 value)
{
	if (dev->bus->number)
		return __sabre_write_word(dev, where, value);

	if (sabre_out_of_range(dev->devfn))
		return PCIBIOS_SUCCESSFUL;

	if (where < 8)
		return __sabre_write_word(dev, where, value);
	else {
		__sabre_write_byte(dev, where, value & 0xff);
		__sabre_write_byte(dev, where + 1, value >> 8);
		return PCIBIOS_SUCCESSFUL;
	}
}

static int sabre_write_dword(struct pci_dev *dev, int where, u32 value)
{
	if (dev->bus->number)
		return __sabre_write_dword(dev, where, value);

	if (sabre_out_of_range(dev->devfn))
		return PCIBIOS_SUCCESSFUL;

	sabre_write_word(dev, where, value & 0xffff);
	sabre_write_word(dev, where + 2, value >> 16);
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops sabre_ops = {
	sabre_read_byte,
	sabre_read_word,
	sabre_read_dword,
	sabre_write_byte,
	sabre_write_word,
	sabre_write_dword
};

static unsigned long sabre_pcislot_imap_offset(unsigned long ino)
{
	unsigned int bus =  (ino & 0x10) >> 4;
	unsigned int slot = (ino & 0x0c) >> 2;

	if (bus == 0)
		return SABRE_IMAP_A_SLOT0 + (slot * 8);
	else
		return SABRE_IMAP_B_SLOT0 + (slot * 8);
}

static unsigned long __onboard_imap_off[] = {
/*0x20*/	SABRE_IMAP_SCSI,
/*0x21*/	SABRE_IMAP_ETH,
/*0x22*/	SABRE_IMAP_BPP,
/*0x23*/	SABRE_IMAP_AU_REC,
/*0x24*/	SABRE_IMAP_AU_PLAY,
/*0x25*/	SABRE_IMAP_PFAIL,
/*0x26*/	SABRE_IMAP_KMS,
/*0x27*/	SABRE_IMAP_FLPY,
/*0x28*/	SABRE_IMAP_SHW,
/*0x29*/	SABRE_IMAP_KBD,
/*0x2a*/	SABRE_IMAP_MS,
/*0x2b*/	SABRE_IMAP_SER,
/*0x2c*/	0 /* reserved */,
/*0x2d*/	0 /* reserved */,
/*0x2e*/	SABRE_IMAP_UE,
/*0x2f*/	SABRE_IMAP_CE,
/*0x30*/	SABRE_IMAP_PCIERR,
};
#define SABRE_ONBOARD_IRQ_BASE		0x20
#define SABRE_ONBOARD_IRQ_LAST		0x30
#define sabre_onboard_imap_offset(__ino) \
	__onboard_imap_off[(__ino) - SABRE_ONBOARD_IRQ_BASE]

#define sabre_iclr_offset(ino)					      \
	((ino & 0x20) ? (SABRE_ICLR_SCSI + (((ino) & 0x1f) << 3)) :  \
			(SABRE_ICLR_A_SLOT0 + (((ino) & 0x1f)<<3)))

/* PCI SABRE INO number to Sparc PIL level. */
static unsigned char sabre_pil_table[] = {
/*0x00*/0, 0, 0, 0,	/* PCI A slot 0  Int A, B, C, D */
/*0x04*/0, 0, 0, 0,	/* PCI A slot 1  Int A, B, C, D */
/*0x08*/0, 0, 0, 0,	/* PCI A slot 2  Int A, B, C, D */
/*0x0c*/0, 0, 0, 0,	/* PCI A slot 3  Int A, B, C, D */
/*0x10*/0, 0, 0, 0,	/* PCI B slot 0  Int A, B, C, D */
/*0x14*/0, 0, 0, 0,	/* PCI B slot 1  Int A, B, C, D */
/*0x18*/0, 0, 0, 0,	/* PCI B slot 2  Int A, B, C, D */
/*0x1c*/0, 0, 0, 0,	/* PCI B slot 3  Int A, B, C, D */
/*0x20*/3,		/* SCSI				*/
/*0x21*/5,		/* Ethernet			*/
/*0x22*/8,		/* Parallel Port		*/
/*0x23*/13,		/* Audio Record			*/
/*0x24*/14,		/* Audio Playback		*/
/*0x25*/15,		/* PowerFail			*/
/*0x26*/3,		/* second SCSI			*/
/*0x27*/11,		/* Floppy			*/
/*0x28*/2,		/* Spare Hardware		*/
/*0x29*/9,		/* Keyboard			*/
/*0x2a*/4,		/* Mouse			*/
/*0x2b*/12,		/* Serial			*/
/*0x2c*/10,		/* Timer 0			*/
/*0x2d*/11,		/* Timer 1			*/
/*0x2e*/15,		/* Uncorrectable ECC		*/
/*0x2f*/15,		/* Correctable ECC		*/
/*0x30*/15,		/* PCI Bus A Error		*/
/*0x31*/15,		/* PCI Bus B Error		*/
/*0x32*/1,		/* Power Management		*/
};

static int __init sabre_ino_to_pil(struct pci_dev *pdev, unsigned int ino)
{
	int ret;

	ret = sabre_pil_table[ino];
	if (ret == 0 && pdev == NULL) {
		ret = 1;
	} else if (ret == 0) {
		switch ((pdev->class >> 16) & 0x0f) {
		case PCI_BASE_CLASS_STORAGE:
			ret = 4;

		case PCI_BASE_CLASS_NETWORK:
			ret = 6;

		case PCI_BASE_CLASS_DISPLAY:
			ret = 9;

		case PCI_BASE_CLASS_MULTIMEDIA:
		case PCI_BASE_CLASS_MEMORY:
		case PCI_BASE_CLASS_BRIDGE:
			ret = 10;

		default:
			ret = 1;
		};
	}
	return ret;
}

static unsigned int __init sabre_irq_build(struct pci_controller_info *p,
					   struct pci_dev *pdev,
					   unsigned int ino)
{
	struct ino_bucket *bucket;
	unsigned long imap, iclr;
	unsigned long imap_off, iclr_off;
	int pil, inofixup = 0;

	ino &= PCI_IRQ_INO;
	if (ino < SABRE_ONBOARD_IRQ_BASE) {
		/* PCI slot */
		imap_off = sabre_pcislot_imap_offset(ino);
	} else {
		/* onboard device */
		if (ino > SABRE_ONBOARD_IRQ_LAST) {
			prom_printf("sabre_irq_build: Wacky INO [%x]\n", ino);
			prom_halt();
		}
		imap_off = sabre_onboard_imap_offset(ino);
	}

	/* Now build the IRQ bucket. */
	pil = sabre_ino_to_pil(pdev, ino);
	imap = p->controller_regs + imap_off;
	imap += 4;

	iclr_off = sabre_iclr_offset(ino);
	iclr = p->controller_regs + iclr_off;
	iclr += 4;

	if ((ino & 0x20) == 0)
		inofixup = ino & 0x03;

	bucket = __bucket(build_irq(pil, inofixup, iclr, imap));
	bucket->flags |= IBF_PCI;

	if (pdev) {
		struct pcidev_cookie *pcp = pdev->sysdata;

		/* When a device lives behind a bridge deeper in the
		 * PCI bus topology than APB, a special sequence must
		 * run to make sure all pending DMA transfers at the
		 * time of IRQ delivery are visible in the coherency
		 * domain by the cpu.  This sequence is to perform
		 * a read on the far side of the non-APB bridge, then
		 * perform a read of Sabre's DMA write-sync register.
		 *
		 * Currently, the PCI_CONFIG register for the device
		 * is used for this read from the far side of the bridge.
		 */
		if (pdev->bus->number != pcp->pbm->pci_first_busno) {
			bucket->flags |= IBF_DMA_SYNC;
			bucket->synctab_ent = dma_sync_reg_table_entry++;
			dma_sync_reg_table[bucket->synctab_ent] =
				(unsigned long) sabre_pci_config_mkaddr(
					pcp->pbm,
					pdev->bus->number, pdev->devfn, PCI_COMMAND);
		}
	}
	return __irq(bucket);
}

/* SABRE error handling support. */
static void sabre_check_iommu_error(struct pci_controller_info *p,
				    unsigned long afsr,
				    unsigned long afar)
{
	unsigned long iommu_tag[16];
	unsigned long iommu_data[16];
	unsigned long flags;
	u64 control;
	int i;

	spin_lock_irqsave(&p->iommu.lock, flags);
	control = sabre_read(p->iommu.iommu_control);
	if (control & SABRE_IOMMUCTRL_ERR) {
		char *type_string;

		/* Clear the error encountered bit.
		 * NOTE: On Sabre this is write 1 to clear,
		 *       which is different from Psycho.
		 */
		sabre_write(p->iommu.iommu_control, control);
		switch((control & SABRE_IOMMUCTRL_ERRSTS) >> 25UL) {
		case 1:
			type_string = "Invalid Error";
			break;
		case 3:
			type_string = "ECC Error";
			break;
		default:
			type_string = "Unknown";
			break;
		};
		printk("SABRE%d: IOMMU Error, type[%s]\n",
		       p->index, type_string);

		/* Enter diagnostic mode and probe for error'd
		 * entries in the IOTLB.
		 */
		control &= ~(SABRE_IOMMUCTRL_ERRSTS | SABRE_IOMMUCTRL_ERR);
		sabre_write(p->iommu.iommu_control,
			    (control | SABRE_IOMMUCTRL_DENAB));
		for (i = 0; i < 16; i++) {
			unsigned long base = p->controller_regs;

			iommu_tag[i] =
				sabre_read(base + SABRE_IOMMU_TAG + (i * 8UL));
			iommu_data[i] =
				sabre_read(base + SABRE_IOMMU_DATA + (i * 8UL));
			sabre_write(base + SABRE_IOMMU_TAG + (i * 8UL), 0);
			sabre_write(base + SABRE_IOMMU_DATA + (i * 8UL), 0);
		}
		sabre_write(p->iommu.iommu_control, control);

		for (i = 0; i < 16; i++) {
			unsigned long tag, data;

			tag = iommu_tag[i];
			if (!(tag & SABRE_IOMMUTAG_ERR))
				continue;

			data = iommu_data[i];
			switch((tag & SABRE_IOMMUTAG_ERRSTS) >> 23UL) {
			case 1:
				type_string = "Invalid Error";
				break;
			case 3:
				type_string = "ECC Error";
				break;
			default:
				type_string = "Unknown";
				break;
			};
			printk("SABRE%d: IOMMU TAG(%d)[RAW(%016lx)error(%s)wr(%d)sz(%dK)vpg(%08lx)]\n",
			       p->index, i, tag, type_string,
			       ((tag & SABRE_IOMMUTAG_WRITE) ? 1 : 0),
			       ((tag & SABRE_IOMMUTAG_SIZE) ? 64 : 8),
			       ((tag & SABRE_IOMMUTAG_VPN) << PAGE_SHIFT));
			printk("SABRE%d: IOMMU DATA(%d)[RAW(%016lx)valid(%d)used(%d)cache(%d)ppg(%016lx)\n",
			       p->index, i, data,
			       ((data & SABRE_IOMMUDATA_VALID) ? 1 : 0),
			       ((data & SABRE_IOMMUDATA_USED) ? 1 : 0),
			       ((data & SABRE_IOMMUDATA_CACHE) ? 1 : 0),
			       ((data & SABRE_IOMMUDATA_PPN) << PAGE_SHIFT));
		}
	}
	spin_unlock_irqrestore(&p->iommu.lock, flags);
}

static void sabre_ue_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct pci_controller_info *p = dev_id;
	unsigned long afsr_reg = p->controller_regs + SABRE_UE_AFSR;
	unsigned long afar_reg = p->controller_regs + SABRE_UECE_AFAR;
	unsigned long afsr, afar, error_bits;
	int reported;

	/* Latch uncorrectable error status. */
	afar = sabre_read(afar_reg);
	afsr = sabre_read(afsr_reg);

	/* Clear the primary/secondary error status bits. */
	error_bits = afsr &
		(SABRE_UEAFSR_PDRD | SABRE_UEAFSR_PDWR |
		 SABRE_UEAFSR_SDRD | SABRE_UEAFSR_SDWR |
		 SABRE_UEAFSR_SDTE | SABRE_UEAFSR_PDTE);
	sabre_write(afsr_reg, error_bits);

	/* Log the error. */
	printk("SABRE%d: Uncorrectable Error, primary error type[%s%s]\n",
	       p->index,
	       ((error_bits & SABRE_UEAFSR_PDRD) ?
		"DMA Read" :
		((error_bits & SABRE_UEAFSR_PDWR) ?
		 "DMA Write" : "???")),
	       ((error_bits & SABRE_UEAFSR_PDTE) ?
		":Translation Error" : ""));
	printk("SABRE%d: bytemask[%04lx] dword_offset[%lx] was_block(%d)\n",
	       p->index,
	       (afsr & SABRE_UEAFSR_BMSK) >> 32UL,
	       (afsr & SABRE_UEAFSR_OFF) >> 29UL,
	       ((afsr & SABRE_UEAFSR_BLK) ? 1 : 0));
	printk("SABRE%d: UE AFAR [%016lx]\n", p->index, afar);
	printk("SABRE%d: UE Secondary errors [", p->index);
	reported = 0;
	if (afsr & SABRE_UEAFSR_SDRD) {
		reported++;
		printk("(DMA Read)");
	}
	if (afsr & SABRE_UEAFSR_SDWR) {
		reported++;
		printk("(DMA Write)");
	}
	if (afsr & SABRE_UEAFSR_SDTE) {
		reported++;
		printk("(Translation Error)");
	}
	if (!reported)
		printk("(none)");
	printk("]\n");

	/* Interrogate IOMMU for error status. */
	sabre_check_iommu_error(p, afsr, afar);
}

static void sabre_ce_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct pci_controller_info *p = dev_id;
	unsigned long afsr_reg = p->controller_regs + SABRE_CE_AFSR;
	unsigned long afar_reg = p->controller_regs + SABRE_UECE_AFAR;
	unsigned long afsr, afar, error_bits;
	int reported;

	/* Latch error status. */
	afar = sabre_read(afar_reg);
	afsr = sabre_read(afsr_reg);

	/* Clear primary/secondary error status bits. */
	error_bits = afsr &
		(SABRE_CEAFSR_PDRD | SABRE_CEAFSR_PDWR |
		 SABRE_CEAFSR_SDRD | SABRE_CEAFSR_SDWR);
	sabre_write(afsr_reg, error_bits);

	/* Log the error. */
	printk("SABRE%d: Correctable Error, primary error type[%s]\n",
	       p->index,
	       ((error_bits & SABRE_CEAFSR_PDRD) ?
		"DMA Read" :
		((error_bits & SABRE_CEAFSR_PDWR) ?
		 "DMA Write" : "???")));

	/* XXX Use syndrome and afar to print out module string just like
	 * XXX UDB CE trap handler does... -DaveM
	 */
	printk("SABRE%d: syndrome[%02lx] bytemask[%04lx] dword_offset[%lx] "
	       "was_block(%d)\n",
	       p->index,
	       (afsr & SABRE_CEAFSR_ESYND) >> 48UL,
	       (afsr & SABRE_CEAFSR_BMSK) >> 32UL,
	       (afsr & SABRE_CEAFSR_OFF) >> 29UL,
	       ((afsr & SABRE_CEAFSR_BLK) ? 1 : 0));
	printk("SABRE%d: CE AFAR [%016lx]\n", p->index, afar);
	printk("SABRE%d: CE Secondary errors [", p->index);
	reported = 0;
	if (afsr & SABRE_CEAFSR_SDRD) {
		reported++;
		printk("(DMA Read)");
	}
	if (afsr & SABRE_CEAFSR_SDWR) {
		reported++;
		printk("(DMA Write)");
	}
	if (!reported)
		printk("(none)");
	printk("]\n");
}

static void sabre_pcierr_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct pci_controller_info *p = dev_id;
	unsigned long afsr_reg, afar_reg;
	unsigned long afsr, afar, error_bits;
	int reported;

	afsr_reg = p->controller_regs + SABRE_PIOAFSR;
	afar_reg = p->controller_regs + SABRE_PIOAFAR;

	/* Latch error status. */
	afar = sabre_read(afar_reg);
	afsr = sabre_read(afsr_reg);

	/* Clear primary/secondary error status bits. */
	error_bits = afsr &
		(SABRE_PIOAFSR_PMA | SABRE_PIOAFSR_PTA |
		 SABRE_PIOAFSR_PRTRY | SABRE_PIOAFSR_PPERR |
		 SABRE_PIOAFSR_SMA | SABRE_PIOAFSR_STA |
		 SABRE_PIOAFSR_SRTRY | SABRE_PIOAFSR_SPERR);
	sabre_write(afsr_reg, error_bits);

	/* Log the error. */
	printk("SABRE%d: PCI Error, primary error type[%s]\n",
	       p->index,
	       (((error_bits & SABRE_PIOAFSR_PMA) ?
		 "Master Abort" :
		 ((error_bits & SABRE_PIOAFSR_PTA) ?
		  "Target Abort" :
		  ((error_bits & SABRE_PIOAFSR_PRTRY) ?
		   "Excessive Retries" :
		   ((error_bits & SABRE_PIOAFSR_PPERR) ?
		    "Parity Error" : "???"))))));
	printk("SABRE%d: bytemask[%04lx] was_block(%d)\n",
	       p->index,
	       (afsr & SABRE_PIOAFSR_BMSK) >> 32UL,
	       (afsr & SABRE_PIOAFSR_BLK) ? 1 : 0);
	printk("SABRE%d: PCI AFAR [%016lx]\n", p->index, afar);
	printk("SABRE%d: PCI Secondary errors [", p->index);
	reported = 0;
	if (afsr & SABRE_PIOAFSR_SMA) {
		reported++;
		printk("(Master Abort)");
	}
	if (afsr & SABRE_PIOAFSR_STA) {
		reported++;
		printk("(Target Abort)");
	}
	if (afsr & SABRE_PIOAFSR_SRTRY) {
		reported++;
		printk("(Excessive Retries)");
	}
	if (afsr & SABRE_PIOAFSR_SPERR) {
		reported++;
		printk("(Parity Error)");
	}
	if (!reported)
		printk("(none)");
	printk("]\n");

	/* For the error types shown, scan both PCI buses for devices
	 * which have logged that error type.
	 */

	/* If we see a Target Abort, this could be the result of an
	 * IOMMU translation error of some sort.  It is extremely
	 * useful to log this information as usually it indicates
	 * a bug in the IOMMU support code or a PCI device driver.
	 */
	if (error_bits & (SABRE_PIOAFSR_PTA | SABRE_PIOAFSR_STA)) {
		sabre_check_iommu_error(p, afsr, afar);
		pci_scan_for_target_abort(p, &p->pbm_A, p->pbm_A.pci_bus);
		pci_scan_for_target_abort(p, &p->pbm_B, p->pbm_B.pci_bus);
	}
	if (error_bits & (SABRE_PIOAFSR_PMA | SABRE_PIOAFSR_SMA)) {
		pci_scan_for_master_abort(p, &p->pbm_A, p->pbm_A.pci_bus);
		pci_scan_for_master_abort(p, &p->pbm_B, p->pbm_B.pci_bus);
	}
	/* For excessive retries, SABRE/PBM will abort the device
	 * and there is no way to specifically check for excessive
	 * retries in the config space status registers.  So what
	 * we hope is that we'll catch it via the master/target
	 * abort events.
	 */

	if (error_bits & (SABRE_PIOAFSR_PPERR | SABRE_PIOAFSR_SPERR)) {
		pci_scan_for_parity_error(p, &p->pbm_A, p->pbm_A.pci_bus);
		pci_scan_for_parity_error(p, &p->pbm_B, p->pbm_B.pci_bus);
	}
}

/* XXX What about PowerFail/PowerManagement??? -DaveM */
#define SABRE_UE_INO		0x2e
#define SABRE_CE_INO		0x2f
#define SABRE_PCIERR_INO	0x30
static void __init sabre_register_error_handlers(struct pci_controller_info *p)
{
	unsigned long base = p->controller_regs;
	unsigned long irq, portid = p->portid;
	u64 tmp;

	/* We clear the error bits in the appropriate AFSR before
	 * registering the handler so that we don't get spurious
	 * interrupts.
	 */
	sabre_write(base + SABRE_UE_AFSR,
		    (SABRE_UEAFSR_PDRD | SABRE_UEAFSR_PDWR |
		     SABRE_UEAFSR_SDRD | SABRE_UEAFSR_SDWR |
		     SABRE_UEAFSR_SDTE | SABRE_UEAFSR_PDTE));
	irq = sabre_irq_build(p, NULL, (portid << 6) | SABRE_UE_INO);
	if (request_irq(irq, sabre_ue_intr,
			SA_SHIRQ, "SABRE UE", p) < 0) {
		prom_printf("SABRE%d: Cannot register UE interrupt.\n",
			    p->index);
		prom_halt();
	}

	sabre_write(base + SABRE_CE_AFSR,
		    (SABRE_CEAFSR_PDRD | SABRE_CEAFSR_PDWR |
		     SABRE_CEAFSR_SDRD | SABRE_CEAFSR_SDWR));
	irq = sabre_irq_build(p, NULL, (portid << 6) | SABRE_CE_INO);
	if (request_irq(irq, sabre_ce_intr,
			SA_SHIRQ, "SABRE CE", p) < 0) {
		prom_printf("SABRE%d: Cannot register CE interrupt.\n",
			    p->index);
		prom_halt();
	}

	irq = sabre_irq_build(p, NULL, (portid << 6) | SABRE_PCIERR_INO);
	if (request_irq(irq, sabre_pcierr_intr,
			SA_SHIRQ, "SABRE PCIERR", p) < 0) {
		prom_printf("SABRE%d: Cannot register PciERR interrupt.\n",
			    p->index);
		prom_halt();
	}

	tmp = sabre_read(base + SABRE_PCICTRL);
	tmp |= SABRE_PCICTRL_ERREN;
	sabre_write(base + SABRE_PCICTRL, tmp);
}

static void __init sabre_resource_adjust(struct pci_dev *pdev,
					 struct resource *res,
					 struct resource *root)
{
	struct pcidev_cookie *pcp = pdev->sysdata;
	struct pci_controller_info *p = pcp->pbm->parent;
	unsigned long base;

	if (res->flags & IORESOURCE_IO)
		base = p->controller_regs + SABRE_IOSPACE;
	else
		base = p->controller_regs + SABRE_MEMSPACE;

	res->start += base;
	res->end += base;
}

static void __init sabre_base_address_update(struct pci_dev *pdev, int resource)
{
	struct pcidev_cookie *pcp = pdev->sysdata;
	struct pci_pbm_info *pbm = pcp->pbm;
	struct pci_controller_info *p = pbm->parent;
	struct resource *res;
	unsigned long base;
	u32 reg;
	int where, size, is_64bit;

	res = &pdev->resource[resource];
	where = PCI_BASE_ADDRESS_0 + (resource * 4);

	is_64bit = 0;
	if (res->flags & IORESOURCE_IO)
		base = p->controller_regs + SABRE_IOSPACE;
	else {
		base = p->controller_regs + SABRE_MEMSPACE;
		if ((res->flags & PCI_BASE_ADDRESS_MEM_TYPE_MASK)
		    == PCI_BASE_ADDRESS_MEM_TYPE_64)
			is_64bit = 1;
	}

	size = res->end - res->start;
	pci_read_config_dword(pdev, where, &reg);
	reg = ((reg & size) |
	       (((u32)(res->start - base)) & ~size));
	pci_write_config_dword(pdev, where, reg);

	/* This knows that the upper 32-bits of the address
	 * must be zero.  Our PCI common layer enforces this.
	 */
	if (is_64bit)
		pci_write_config_dword(pdev, where + 4, 0);
}

static void __init apb_init(struct pci_controller_info *p, struct pci_bus *sabre_bus)
{
	struct list_head *walk = &sabre_bus->devices;

	for (walk = walk->next; walk != &sabre_bus->devices; walk = walk->next) {
		struct pci_dev *pdev = pci_dev_b(walk);

		if (pdev->vendor == PCI_VENDOR_ID_SUN &&
		    pdev->device == PCI_DEVICE_ID_SUN_SIMBA) {
			u16 word;

			sabre_read_word(pdev, PCI_COMMAND, &word);
			word |= PCI_COMMAND_SERR | PCI_COMMAND_PARITY |
				PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY |
				PCI_COMMAND_IO;
			sabre_write_word(pdev, PCI_COMMAND, word);

			/* Status register bits are "write 1 to clear". */
			sabre_write_word(pdev, PCI_STATUS, 0xffff);
			sabre_write_word(pdev, PCI_SEC_STATUS, 0xffff);

			/* Use a primary/seconday latency timer value
			 * of 64.
			 */
			sabre_write_byte(pdev, PCI_LATENCY_TIMER, 64);
			sabre_write_byte(pdev, PCI_SEC_LATENCY_TIMER, 64);

			/* Enable reporting/forwarding of master aborts,
			 * parity, and SERR.
			 */
			sabre_write_byte(pdev, PCI_BRIDGE_CONTROL,
					 (PCI_BRIDGE_CTL_PARITY |
					  PCI_BRIDGE_CTL_SERR |
					  PCI_BRIDGE_CTL_MASTER_ABORT));
		}
	}
}

static void __init sabre_scan_bus(struct pci_controller_info *p)
{
	static int once = 0;
	struct pci_bus *sabre_bus;
	struct list_head *walk;

	/* The APB bridge speaks to the Sabre host PCI bridge
	 * at 66Mhz, but the front side of APB runs at 33Mhz
	 * for both segments.
	 */
	p->pbm_A.is_66mhz_capable = 0;
	p->pbm_B.is_66mhz_capable = 0;

	/* Unlike for PSYCHO, we can only have one SABRE
	 * in a system.  Having multiple SABREs is thus
	 * and error, and as a consequence we do not need
	 * to do any bus renumbering but we do have to have
	 * the pci_bus2pbm array setup properly.
	 *
	 * Also note that the SABRE host bridge is hardwired
	 * to live at bus 0.
	 */
	if (once != 0) {
		prom_printf("SABRE: Multiple controllers unsupported.\n");
		prom_halt();
	}
	once++;

	/* The pci_bus2pbm table has already been setup in sabre_init. */
	sabre_bus = pci_scan_bus(p->pci_first_busno,
				 p->pci_ops,
				 &p->pbm_A);
	apb_init(p, sabre_bus);

	walk = &sabre_bus->children;
	for (walk = walk->next; walk != &sabre_bus->children; walk = walk->next) {
		struct pci_bus *pbus = pci_bus_b(walk);
		struct pci_pbm_info *pbm;

		if (pbus->number == p->pbm_A.pci_first_busno) {
			pbm = &p->pbm_A;
		} else if (pbus->number == p->pbm_B.pci_first_busno) {
			pbm = &p->pbm_B;
		} else
			continue;

		pbus->sysdata = pbm;
		pbm->pci_bus = pbus;
		pci_fill_in_pbm_cookies(pbus, pbm, pbm->prom_node);
		pci_record_assignments(pbm, pbus);
		pci_assign_unassigned(pbm, pbus);
		pci_fixup_irq(pbm, pbus);
		pci_determine_66mhz_disposition(pbm, pbus);
		pci_setup_busmastering(pbm, pbus);
	}

	sabre_register_error_handlers(p);
}

static void __init sabre_iommu_init(struct pci_controller_info *p,
				    int tsbsize, unsigned long dvma_offset,
				    u32 dma_mask)
{
	unsigned long tsbbase, i, order;
	u64 control;

	/* Setup initial software IOMMU state. */
	spin_lock_init(&p->iommu.lock);
	p->iommu.iommu_cur_ctx = 0;

	/* Register addresses. */
	p->iommu.iommu_control  = p->controller_regs + SABRE_IOMMU_CONTROL;
	p->iommu.iommu_tsbbase  = p->controller_regs + SABRE_IOMMU_TSBBASE;
	p->iommu.iommu_flush    = p->controller_regs + SABRE_IOMMU_FLUSH;
	p->iommu.write_complete_reg = p->controller_regs + SABRE_WRSYNC;
	/* Sabre's IOMMU lacks ctx flushing. */
	p->iommu.iommu_ctxflush = 0;
                                        
	/* Invalidate TLB Entries. */
	control = sabre_read(p->controller_regs + SABRE_IOMMU_CONTROL);
	control |= SABRE_IOMMUCTRL_DENAB;
	sabre_write(p->controller_regs + SABRE_IOMMU_CONTROL, control);

	for(i = 0; i < 16; i++) {
		sabre_write(p->controller_regs + SABRE_IOMMU_TAG + (i * 8UL), 0);
		sabre_write(p->controller_regs + SABRE_IOMMU_DATA + (i * 8UL), 0);
	}

	/* Leave diag mode enabled for full-flushing done
	 * in pci_iommu.c
	 */

	tsbbase = __get_free_pages(GFP_KERNEL, order = get_order(tsbsize * 1024 * 8));
	if (!tsbbase) {
		prom_printf("SABRE_IOMMU: Error, gfp(tsb) failed.\n");
		prom_halt();
	}
	p->iommu.page_table = (iopte_t *)tsbbase;
	p->iommu.page_table_map_base = dvma_offset;
	p->iommu.dma_addr_mask = dma_mask;
	memset((char *)tsbbase, 0, PAGE_SIZE << order);

	sabre_write(p->controller_regs + SABRE_IOMMU_TSBBASE, __pa(tsbbase));

	control = sabre_read(p->controller_regs + SABRE_IOMMU_CONTROL);
	control &= ~(SABRE_IOMMUCTRL_TSBSZ | SABRE_IOMMUCTRL_TBWSZ);
	control |= SABRE_IOMMUCTRL_ENAB;
	switch(tsbsize) {
	case 64:
		control |= SABRE_IOMMU_TSBSZ_64K;
		p->iommu.page_table_sz_bits = 16;
		break;
	case 128:
		control |= SABRE_IOMMU_TSBSZ_128K;
		p->iommu.page_table_sz_bits = 17;
		break;
	default:
		prom_printf("iommu_init: Illegal TSB size %d\n", tsbsize);
		prom_halt();
		break;
	}
	sabre_write(p->controller_regs + SABRE_IOMMU_CONTROL, control);

	/* We start with no consistent mappings. */
	p->iommu.lowest_consistent_map =
		1 << (p->iommu.page_table_sz_bits - PBM_LOGCLUSTERS);

	for (i = 0; i < PBM_NCLUSTERS; i++) {
		p->iommu.alloc_info[i].flush = 0;
		p->iommu.alloc_info[i].next = 0;
	}
}

static void __init pbm_register_toplevel_resources(struct pci_controller_info *p,
						   struct pci_pbm_info *pbm)
{
	char *name = pbm->name;
	unsigned long ibase = p->controller_regs + SABRE_IOSPACE;
	unsigned long mbase = p->controller_regs + SABRE_MEMSPACE;
	unsigned int devfn;
	unsigned long first, last, i;
	u8 *addr, map;

	sprintf(name, "SABRE%d PBM%c",
		p->index,
		(pbm == &p->pbm_A ? 'A' : 'B'));
	pbm->io_space.name = pbm->mem_space.name = name;

	devfn = PCI_DEVFN(1, (pbm == &p->pbm_A) ? 0 : 1);
	addr = sabre_pci_config_mkaddr(pbm, 0, devfn, APB_IO_ADDRESS_MAP);
	map = 0;
	pci_config_read8(addr, &map);

	first = 8;
	last = 0;
	for (i = 0; i < 8; i++) {
		if ((map & (1 << i)) != 0) {
			if (first > i)
				first = i;
			if (last < i)
				last = i;
		}
	}
	pbm->io_space.start = ibase + (first << 21UL);
	pbm->io_space.end   = ibase + (last << 21UL) + ((1 << 21UL) - 1);
	pbm->io_space.flags = IORESOURCE_IO;

	addr = sabre_pci_config_mkaddr(pbm, 0, devfn, APB_MEM_ADDRESS_MAP);
	map = 0;
	pci_config_read8(addr, &map);

	first = 8;
	last = 0;
	for (i = 0; i < 8; i++) {
		if ((map & (1 << i)) != 0) {
			if (first > i)
				first = i;
			if (last < i)
				last = i;
		}
	}
	pbm->mem_space.start = mbase + (first << 29UL);
	pbm->mem_space.end   = mbase + (last << 29UL) + ((1 << 29UL) - 1);
	pbm->mem_space.flags = IORESOURCE_MEM;

	if (request_resource(&ioport_resource, &pbm->io_space) < 0) {
		prom_printf("Cannot register PBM-%c's IO space.\n",
			    (pbm == &p->pbm_A ? 'A' : 'B'));
		prom_halt();
	}
	if (request_resource(&iomem_resource, &pbm->mem_space) < 0) {
		prom_printf("Cannot register PBM-%c's MEM space.\n",
			    (pbm == &p->pbm_A ? 'A' : 'B'));
		prom_halt();
	}
}

static void __init sabre_pbm_init(struct pci_controller_info *p, int sabre_node)
{
	char namebuf[128];
	u32 busrange[2];
	int node;

	node = prom_getchild(sabre_node);
	while ((node = prom_searchsiblings(node, "pci")) != 0) {
		struct pci_pbm_info *pbm;
		int err;

		err = prom_getproperty(node, "model", namebuf, sizeof(namebuf));
		if ((err <= 0) || strncmp(namebuf, "SUNW,simba", err))
			goto next_pci;

		err = prom_getproperty(node, "bus-range",
				       (char *)&busrange[0], sizeof(busrange));
		if (err == 0 || err == -1) {
			prom_printf("APB: Error, cannot get PCI bus-range.\n");
			prom_halt();
		}

		if (busrange[0] == 1)
			pbm = &p->pbm_B;
		else
			pbm = &p->pbm_A;
		pbm->parent = p;
		pbm->prom_node = node;
		pbm->pci_first_busno = busrange[0];
		pbm->pci_last_busno = busrange[1];
		for (err = pbm->pci_first_busno;
		     err <= pbm->pci_last_busno;
		     err++)
			pci_bus2pbm[err] = pbm;


		prom_getstring(node, "name", pbm->prom_name, sizeof(pbm->prom_name));
		err = prom_getproperty(node, "ranges",
				       (char *)pbm->pbm_ranges,
				       sizeof(pbm->pbm_ranges));
		if (err != -1)
			pbm->num_pbm_ranges =
				(err / sizeof(struct linux_prom_pci_ranges));
		else
			pbm->num_pbm_ranges = 0;

		err = prom_getproperty(node, "interrupt-map",
				       (char *)pbm->pbm_intmap,
				       sizeof(pbm->pbm_intmap));
		if (err != -1) {
			pbm->num_pbm_intmap = (err / sizeof(struct linux_prom_pci_intmap));
			err = prom_getproperty(node, "interrupt-map-mask",
					       (char *)&pbm->pbm_intmask,
					       sizeof(pbm->pbm_intmask));
			if (err == -1) {
				prom_printf("APB: Fatal error, no interrupt-map-mask.\n");
				prom_halt();
			}
		} else {
			pbm->num_pbm_intmap = 0;
			memset(&pbm->pbm_intmask, 0, sizeof(pbm->pbm_intmask));
		}

		pbm_register_toplevel_resources(p, pbm);

	next_pci:
		node = prom_getsibling(node);
		if (!node)
			break;
	}
}

void __init sabre_init(int pnode)
{
	struct linux_prom64_registers pr_regs[2];
	struct pci_controller_info *p;
	unsigned long flags;
	int tsbsize, err;
	u32 busrange[2];
	u32 vdma[2];
	u32 upa_portid, dma_mask;
	int bus;

	p = kmalloc(sizeof(*p), GFP_ATOMIC);
	if (!p) {
		prom_printf("SABRE: Error, kmalloc(pci_controller_info) failed.\n");
		prom_halt();
	}

	upa_portid = prom_getintdefault(pnode, "upa-portid", 0xff);

	memset(p, 0, sizeof(*p));

	spin_lock_irqsave(&pci_controller_lock, flags);
	p->next = pci_controller_root;
	pci_controller_root = p;
	spin_unlock_irqrestore(&pci_controller_lock, flags);

	p->portid = upa_portid;
	p->index = pci_num_controllers++;
	p->scan_bus = sabre_scan_bus;
	p->irq_build = sabre_irq_build;
	p->base_address_update = sabre_base_address_update;
	p->resource_adjust = sabre_resource_adjust;
	p->pci_ops = &sabre_ops;

	/*
	 * Map in SABRE register set and report the presence of this SABRE.
	 */
	err = prom_getproperty(pnode, "reg",
			       (char *)&pr_regs[0], sizeof(pr_regs));
	if(err == 0 || err == -1) {
		prom_printf("SABRE: Error, cannot get U2P registers "
			    "from PROM.\n");
		prom_halt();
	}

	/*
	 * First REG in property is base of entire SABRE register space.
	 */
	p->controller_regs = pr_regs[0].phys_addr;
	pci_dma_wsync = p->controller_regs + SABRE_WRSYNC;

	printk("PCI: Found SABRE, main regs at %016lx, wsync at %016lx\n",
	       p->controller_regs, pci_dma_wsync);

	/* Error interrupts are enabled later after the bus scan. */
	sabre_write(p->controller_regs + SABRE_PCICTRL,
		    (SABRE_PCICTRL_MRLEN   | SABRE_PCICTRL_SERR |
		     SABRE_PCICTRL_ARBPARK | SABRE_PCICTRL_AEN));

	/* Now map in PCI config space for entire SABRE. */
	p->config_space = p->controller_regs + SABRE_CONFIGSPACE;
	printk("SABRE: PCI config space at %016lx\n", p->config_space);

	err = prom_getproperty(pnode, "virtual-dma",
			       (char *)&vdma[0], sizeof(vdma));
	if(err == 0 || err == -1) {
		prom_printf("SABRE: Error, cannot get virtual-dma property "
			    "from PROM.\n");
		prom_halt();
	}

	dma_mask = vdma[0];
	switch(vdma[1]) {
		case 0x20000000:
			dma_mask |= 0x1fffffff;
			tsbsize = 64;
			break;
		case 0x40000000:
			dma_mask |= 0x3fffffff;
			tsbsize = 128;
			break;

		case 0x80000000:
			dma_mask |= 0x7fffffff;
			tsbsize = 128;
			break;
		default:
			prom_printf("SABRE: strange virtual-dma size.\n");
			prom_halt();
	}

	sabre_iommu_init(p, tsbsize, vdma[0], dma_mask);

	printk("SABRE: DVMA at %08x [%08x]\n", vdma[0], vdma[1]);

	err = prom_getproperty(pnode, "bus-range",
				       (char *)&busrange[0], sizeof(busrange));
	if(err == 0 || err == -1) {
		prom_printf("SABRE: Error, cannot get PCI bus-range "
			    " from PROM.\n");
		prom_halt();
	}

	p->pci_first_busno = busrange[0];
	p->pci_last_busno = busrange[1];

	/*
	 * Handle config space reads through any Simba on APB.
	 */
	for (bus = p->pci_first_busno; bus <= p->pci_last_busno; bus++)
		pci_bus2pbm[bus] = &p->pbm_A;

	/*
	 * Look for APB underneath.
	 */
	sabre_pbm_init(p, pnode);
}
