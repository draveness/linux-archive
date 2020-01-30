/*
 *
 *    Copyright (c) 1999 Grant Erickson <grant@lcse.umn.edu>
 *
 *    Module name: oak.h
 *
 *    Description:
 *	Macros, definitions, and data structures specific to the IBM PowerPC
 *      403G{A,B,C,CX} "Oak" evaluation board. Anything specific to the pro-
 *      cessor itself is defined elsewhere.
 *
 */

#ifdef __KERNEL__
#ifndef	__OAK_H__
#define	__OAK_H__

#include <asm/irq.h>


#ifdef __cplusplus
extern "C" {
#endif

#define _IO_BASE	0
#define _ISA_MEM_BASE	0
#define PCI_DRAM_OFFSET	0

/* Memory map for the "Oak" evaluation board */

#define	PPC403SPU_IO_BASE      	0x40000000	/* 403 On-chip serial port */
#define	PPC403SPU_IO_SIZE      	0x00000008
#define	OAKSERIAL_IO_BASE	0x7E000000	/* NS16550DV serial port */
#define	OAKSERIAL_IO_SIZE	0x00000008
#define	OAKNET_IO_BASE		0xF4000000	/* NS83902AV Ethernet */
#define	OAKNET_IO_SIZE		0x00000040
#define	OAKPROM_IO_BASE		0xFFFE0000	/* AMD 29F010 Flash ROM */
#define	OAKPROM_IO_SIZE		0x00020000


/* Interrupt assignments fixed by the hardware implementation */

#define	PPC403SPU_RX_INT	AIC_INT4
#define	PPC403SPU_TX_INT	AIC_INT5
#define	OAKNET_INT		AIC_INT27
#define	OAKSERIAL_INT		AIC_INT28

/*
 * Data structure defining board information maintained by the boot
 * ROM on IBM's "Oak" evaluation board. An effort has been made to
 * keep the field names consistent with the 8xx 'bd_t' board info
 * structures.
 */

typedef struct board_info {
	unsigned char	 bi_s_version[4];	/* Version of this structure */
	unsigned char	 bi_r_version[30];	/* Version of the IBM ROM */
	unsigned int	 bi_memsize;		/* DRAM installed, in bytes */
	unsigned char	 bi_enetaddr[6];	/* Ethernet MAC address */
	unsigned int	 bi_intfreq;		/* Processor speed, in Hz */
	unsigned int	 bi_busfreq;		/* Bus speed, in Hz */
} bd_t;


#ifdef __cplusplus
}
#endif

#endif /* __OAK_H__ */
#endif /* __KERNEL__ */
