/*
 * A collection of structures, addresses, and values associated with
 * the Motorola MBX boards.  This was originally created for the
 * MBX860, and probably needs revisions for other boards (like the 821).
 * When this file gets out of control, we can split it up into more
 * meaningful pieces.
 *
 * Copyright (c) 1997 Dan Malek (dmalek@jlc.net)
 */
#ifdef __KERNEL__
#ifndef __MACH_MBX_DEFS
#define __MACH_MBX_DEFS

/* A Board Information structure that is given to a program when
 * EPPC-Bug starts it up.
 */
typedef struct bd_info {
	unsigned int	bi_tag;		/* Should be 0x42444944 "BDID" */
	unsigned int	bi_size;	/* Size of this structure */
	unsigned int	bi_revision;	/* revision of this structure */
	unsigned int	bi_bdate;	/* EPPCbug date, i.e. 0x11061997 */
	unsigned int	bi_memstart;	/* Memory start address */
	unsigned int	bi_memsize;	/* Memory (end) size in bytes */
	unsigned int	bi_intfreq;	/* Internal Freq, in Hz */
	unsigned int	bi_busfreq;	/* Bus Freq, in Hz */
	unsigned int	bi_clun;	/* Boot device controller */
	unsigned int	bi_dlun;	/* Boot device logical dev */
	unsigned int	bi_baudrate;	/* ...to be like everyone else */
} bd_t;

/* Memory map for the MBX as configured by EPPC-Bug.  We could reprogram
 * The SIU and PCI bridge, and try to use larger MMU pages, but the
 * performance gain is not measureable and it certainly complicates the
 * generic MMU model.
 *
 * In a effort to minimize memory usage for embedded applications, any
 * PCI driver or ISA driver must request or map the region required by
 * the device.  For convenience (and since we can map up to 4 Mbytes with
 * a single page table page), the MMU initialization will map the
 * NVRAM, Status/Control registers, CPM Dual Port RAM, and the PCI
 * Bridge CSRs 1:1 into the kernel address space.
 */
#define PCI_ISA_IO_ADDR		((unsigned)0x80000000)
#define PCI_ISA_IO_SIZE		((uint)(512 * 1024 * 1024))
#define PCI_IDE_ADDR		((unsigned)0x81000000)
#define PCI_ISA_MEM_ADDR	((unsigned)0xc0000000)
#define PCI_ISA_MEM_SIZE	((uint)(512 * 1024 * 1024))
#define PCMCIA_MEM_ADDR		((uint)0xe0000000)
#define PCMCIA_MEM_SIZE		((uint)(64 * 1024 * 1024))
#define PCMCIA_DMA_ADDR		((uint)0xe4000000)
#define PCMCIA_DMA_SIZE		((uint)(64 * 1024 * 1024))
#define PCMCIA_ATTRB_ADDR	((uint)0xe8000000)
#define PCMCIA_ATTRB_SIZE	((uint)(64 * 1024 * 1024))
#define PCMCIA_IO_ADDR		((uint)0xec000000)
#define PCMCIA_IO_SIZE		((uint)(64 * 1024 * 1024))
#define NVRAM_ADDR		((uint)0xfa000000)
#define NVRAM_SIZE		((uint)(1 * 1024 * 1024))
#define MBX_CSR_ADDR		((uint)0xfa100000)
#define MBX_CSR_SIZE		((uint)(1 * 1024 * 1024))
#define IMAP_ADDR		((uint)0xfa200000)
#define IMAP_SIZE		((uint)(64 * 1024))
#define PCI_CSR_ADDR		((uint)0xfa210000)
#define PCI_CSR_SIZE		((uint)(64 * 1024))

/* Map additional physical space into well known virtual addresses.  Due
 * to virtual address mapping, these physical addresses are not accessible
 * in a 1:1 virtual to physical mapping.
 */
#define ISA_IO_VIRT_ADDR	((uint)0xfa220000)
#define ISA_IO_VIRT_SIZE	((uint)64 * 1024)

/* Interrupt assignments.
 * These are defined (and fixed) by the MBX hardware implementation.
 */
#define POWER_FAIL_INT	SIU_IRQ0	/* Power fail */
#define TEMP_HILO_INT	SIU_IRQ1	/* Temperature sensor */
#define QSPAN_INT	SIU_IRQ2	/* PCI Bridge (DMA CTLR?) */
#define ISA_BRIDGE_INT	SIU_IRQ3	/* All those PC things */
#define COMM_L_INT	SIU_IRQ6	/* MBX Comm expansion connector pin */
#define STOP_ABRT_INT	SIU_IRQ7	/* Stop/Abort header pin */

/* The MBX uses the 8259.
*/
#define NR_8259_INTS	16

/* Generic 8xx type
*/
#define _MACH_8xx (_MACH_mbx)
#endif
#endif /* __KERNEL__ */
