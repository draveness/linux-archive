/* $Id: sbus.h,v 1.22 2000/02/18 13:50:50 davem Exp $
 * sbus.h:  Defines for the Sun SBus.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 */

#ifndef _SPARC_SBUS_H
#define _SPARC_SBUS_H

#include <linux/ioport.h>

#include <asm/oplib.h>
#include <asm/iommu.h>
#include <asm/scatterlist.h>

/* We scan which devices are on the SBus using the PROM node device
 * tree.  SBus devices are described in two different ways.  You can
 * either get an absolute address at which to access the device, or
 * you can get a SBus 'slot' number and an offset within that slot.
 */

/* The base address at which to calculate device OBIO addresses. */
#define SUN_SBUS_BVADDR        0xf8000000
#define SBUS_OFF_MASK          0x01ffffff

/* These routines are used to calculate device address from slot
 * numbers + offsets, and vice versa.
 */

extern __inline__ unsigned long sbus_devaddr(int slotnum, unsigned long offset)
{
  return (unsigned long) (SUN_SBUS_BVADDR+((slotnum)<<25)+(offset));
}

extern __inline__ int sbus_dev_slot(unsigned long dev_addr)
{
  return (int) (((dev_addr)-SUN_SBUS_BVADDR)>>25);
}

struct sbus_bus;

/* Linux SBUS device tables */
struct sbus_dev {
	struct sbus_bus	*bus;       /* Back ptr to sbus */
	struct sbus_dev	*next;      /* next device on this SBus or null */
	struct sbus_dev	*child;     /* For ledma and espdma on sun4m */
	struct sbus_dev	*parent;    /* Parent device if not toplevel */
	int prom_node;              /* PROM device tree node for this device */
	char prom_name[64];         /* PROM device name */
	int slot;

	struct resource resource[PROMREG_MAX];

	struct linux_prom_registers reg_addrs[PROMREG_MAX];
	int num_registers, ranges_applied;

	struct linux_prom_ranges device_ranges[PROMREG_MAX];
	int num_device_ranges;

	unsigned int irqs[4];
	int num_irqs;
};

/* This struct describes the SBus(s) found on this machine. */
struct sbus_bus {
	void			*iommu;		/* Opaque IOMMU cookie */
	struct sbus_dev		*devices;	/* Link to devices on this SBus */
	struct sbus_bus		*next;		/* next SBus, if more than one SBus */
	int			prom_node;	/* PROM device tree node for this SBus */
	char			prom_name[64];  /* Usually "sbus" or "sbi" */
	int			clock_freq;

	struct linux_prom_ranges sbus_ranges[PROMREG_MAX];
	int num_sbus_ranges;

	int devid;
	int board;
};

extern struct sbus_bus *sbus_root;

extern __inline__ int
sbus_is_slave(struct sbus_dev *dev)
{
	/* XXX Have to write this for sun4c's */
	return 0;
}

/* Device probing routines could find these handy */
#define for_each_sbus(bus) \
        for((bus) = sbus_root; (bus); (bus)=(bus)->next)

#define for_each_sbusdev(device, bus) \
        for((device) = (bus)->devices; (device); (device)=(device)->next)
        
#define for_all_sbusdev(device, bus) \
	for((bus) = sbus_root, ((device) = (bus) ? (bus)->devices : 0); (bus); (device)=((device)->next ? (device)->next : ((bus) = (bus)->next, (bus) ? (bus)->devices : 0)))

/* Driver DVMA interfaces. */
#define sbus_can_dma_64bit(sdev)	(0) /* actually, sparc_cpu_model==sun4d */
#define sbus_can_burst64(sdev)		(0) /* actually, sparc_cpu_model==sun4d */
extern void sbus_set_sbus64(struct sbus_dev *, int);

/* These yield IOMMU mappings in consistent mode. */
extern void *sbus_alloc_consistent(struct sbus_dev *, long, u32 *dma_addrp);
extern void sbus_free_consistent(struct sbus_dev *, long, void *, u32);

#define SBUS_DMA_BIDIRECTIONAL	0
#define SBUS_DMA_TODEVICE	1
#define SBUS_DMA_FROMDEVICE	2
#define	SBUS_DMA_NONE		3

/* All the rest use streaming mode mappings. */
extern u32 sbus_map_single(struct sbus_dev *, void *, long, int);
extern void sbus_unmap_single(struct sbus_dev *, u32, long, int);
extern int sbus_map_sg(struct sbus_dev *, struct scatterlist *, int, int);
extern void sbus_unmap_sg(struct sbus_dev *, struct scatterlist *, int, int);

/* Finally, allow explicit synchronization of streamable mappings. */
extern void sbus_dma_sync_single(struct sbus_dev *, u32, long, int);
extern void sbus_dma_sync_sg(struct sbus_dev *, struct scatterlist *, int, int);

#endif /* !(_SPARC_SBUS_H) */
