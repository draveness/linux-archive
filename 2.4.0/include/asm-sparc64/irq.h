/* $Id: irq.h,v 1.19 2000/06/26 19:40:27 davem Exp $
 * irq.h: IRQ registers on the 64-bit Sparc.
 *
 * Copyright (C) 1996 David S. Miller (davem@caip.rutgers.edu)
 * Copyright (C) 1998 Jakub Jelinek (jj@ultra.linux.cz)
 */

#ifndef _SPARC64_IRQ_H
#define _SPARC64_IRQ_H

#include <linux/config.h>
#include <linux/linkage.h>
#include <linux/kernel.h>

struct devid_cookie {
	int dummy;
};

/* You should not mess with this directly. That's the job of irq.c.
 *
 * If you make changes here, please update hand coded assembler of
 * SBUS/floppy interrupt handler in entry.S -DaveM
 *
 * This is currently one DCACHE line, two buckets per L2 cache
 * line.  Keep this in mind please.
 */
struct ino_bucket {
	/* Next handler in per-CPU PIL worklist.  We know that
	 * bucket pointers have the high 32-bits clear, so to
	 * save space we only store the bits we need.
	 */
/*0x00*/unsigned int irq_chain;

	/* PIL to schedule this IVEC at. */
/*0x04*/unsigned char pil;

	/* If an IVEC arrives while irq_info is NULL, we
	 * set this to notify request_irq() about the event.
	 */
/*0x05*/unsigned char pending;

	/* Miscellaneous flags. */
/*0x06*/unsigned char flags;

	/* This is used to deal with IBF_DMA_SYNC on
	 * Sabre systems.
	 */
/*0x07*/unsigned char synctab_ent;

	/* Reference to handler for this IRQ.  If this is
	 * non-NULL this means it is active and should be
	 * serviced.  Else the pending member is set to one
	 * and later registry of the interrupt checks for
	 * this condition.
	 *
	 * Normally this is just an irq_action structure.
	 * But, on PCI, if multiple interrupt sources behind
	 * a bridge have multiple interrupt sources that share
	 * the same INO bucket, this points to an array of
	 * pointers to four IRQ action structures.
	 */
/*0x08*/void *irq_info;

	/* Sun5 Interrupt Clear Register. */
/*0x10*/unsigned long iclr;

	/* Sun5 Interrupt Mapping Register. */
/*0x18*/unsigned long imap;

};

#ifdef CONFIG_PCI
extern unsigned long pci_dma_wsync;
extern unsigned long dma_sync_reg_table[256];
extern unsigned char dma_sync_reg_table_entry;
#endif

/* IMAP/ICLR register defines */
#define IMAP_VALID		0x80000000	/* IRQ Enabled		*/
#define IMAP_TID		0x7c000000	/* UPA TargetID		*/
#define IMAP_IGN		0x000007c0	/* IRQ Group Number	*/
#define IMAP_INO		0x0000003f	/* IRQ Number		*/
#define IMAP_INR		0x000007ff	/* Full interrupt number*/

#define ICLR_IDLE		0x00000000	/* Idle state		*/
#define ICLR_TRANSMIT		0x00000001	/* Transmit state	*/
#define ICLR_PENDING		0x00000003	/* Pending state	*/

/* Only 8-bits are available, be careful.  -DaveM */
#define IBF_DMA_SYNC	0x01	/* DMA synchronization behind PCI bridge needed. */
#define IBF_PCI		0x02	/* Indicates PSYCHO/SABRE/SCHIZO PCI interrupt.	 */
#define IBF_ACTIVE	0x04	/* This interrupt is active and has a handler.	 */
#define IBF_MULTI	0x08	/* On PCI, indicates shared bucket.		 */

#define NUM_IVECS	8192
extern struct ino_bucket ivector_table[NUM_IVECS];

#define __irq_ino(irq) \
        (((struct ino_bucket *)(unsigned long)(irq)) - &ivector_table[0])
#define __irq_pil(irq) ((struct ino_bucket *)(unsigned long)(irq))->pil
#define __bucket(irq) ((struct ino_bucket *)(unsigned long)(irq))
#define __irq(bucket) ((unsigned int)(unsigned long)(bucket))

static __inline__ char *__irq_itoa(unsigned int irq)
{
	static char buff[16];

	sprintf(buff, "%d,%x", __irq_pil(irq), (unsigned int)__irq_ino(irq));
	return buff;
}

#define NR_IRQS    15

extern void disable_irq(unsigned int);
#define disable_irq_nosync disable_irq
extern void enable_irq(unsigned int);
extern void init_timers(void (*lvl10_irq)(int, void *, struct pt_regs *),
			unsigned long *);
extern unsigned int build_irq(int pil, int inofixup, unsigned long iclr, unsigned long imap);
extern unsigned int sbus_build_irq(void *sbus, unsigned int ino);
extern unsigned int psycho_build_irq(void *psycho, int imap_off, int ino, int need_dma_sync);

#ifdef CONFIG_SMP
extern void set_cpu_int(int, int);
extern void clear_cpu_int(int, int);
extern void set_irq_udt(int);
#endif

extern int request_fast_irq(unsigned int irq,
			    void (*handler)(int, void *, struct pt_regs *),
			    unsigned long flags, __const__ char *devname,
			    void *dev_id);

extern __inline__ void set_softint(unsigned long bits)
{
	__asm__ __volatile__("wr	%0, 0x0, %%set_softint"
			     : /* No outputs */
			     : "r" (bits));
}

extern __inline__ void clear_softint(unsigned long bits)
{
	__asm__ __volatile__("wr	%0, 0x0, %%clear_softint"
			     : /* No outputs */
			     : "r" (bits));
}

extern __inline__ unsigned long get_softint(void)
{
	unsigned long retval;

	__asm__ __volatile__("rd	%%softint, %0"
			     : "=r" (retval));
	return retval;
}

#endif
