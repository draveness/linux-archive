/*
 *	linux/arch/mips/dec/kn02-irq.c
 *
 *	DECstation 5000/200 (KN02) Control and Status Register
 *	interrupts.
 *
 *	Copyright (c) 2002, 2003  Maciej W. Rozycki
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/irq.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <asm/dec/kn02.h>


/*
 * Bits 7:0 of the Control Register are write-only -- the
 * corresponding bits of the Status Register have a different
 * meaning.  Hence we use a cache.  It speeds up things a bit
 * as well.
 *
 * There is no default value -- it has to be initialized.
 */
u32 cached_kn02_csr;
spinlock_t kn02_lock = SPIN_LOCK_UNLOCKED;


static int kn02_irq_base;


static inline void unmask_kn02_irq(unsigned int irq)
{
	volatile u32 *csr = (volatile u32 *)KN02_CSR_BASE;

	cached_kn02_csr |= (1 << (irq - kn02_irq_base + 16));
	*csr = cached_kn02_csr;
}

static inline void mask_kn02_irq(unsigned int irq)
{
	volatile u32 *csr = (volatile u32 *)KN02_CSR_BASE;

	cached_kn02_csr &= ~(1 << (irq - kn02_irq_base + 16));
	*csr = cached_kn02_csr;
}

static inline void enable_kn02_irq(unsigned int irq)
{
	unsigned long flags;

	spin_lock_irqsave(&kn02_lock, flags);
	unmask_kn02_irq(irq);
	spin_unlock_irqrestore(&kn02_lock, flags);
}

static inline void disable_kn02_irq(unsigned int irq)
{
	unsigned long flags;

	spin_lock_irqsave(&kn02_lock, flags);
	mask_kn02_irq(irq);
	spin_unlock_irqrestore(&kn02_lock, flags);
}


static unsigned int startup_kn02_irq(unsigned int irq)
{
	enable_kn02_irq(irq);
	return 0;
}

#define shutdown_kn02_irq disable_kn02_irq

static void ack_kn02_irq(unsigned int irq)
{
	spin_lock(&kn02_lock);
	mask_kn02_irq(irq);
	spin_unlock(&kn02_lock);
	iob();
}

static void end_kn02_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED | IRQ_INPROGRESS)))
		enable_kn02_irq(irq);
}

static struct hw_interrupt_type kn02_irq_type = {
	.typename = "KN02-CSR",
	.startup = startup_kn02_irq,
	.shutdown = shutdown_kn02_irq,
	.enable = enable_kn02_irq,
	.disable = disable_kn02_irq,
	.ack = ack_kn02_irq,
	.end = end_kn02_irq,
};


void __init init_kn02_irqs(int base)
{
	volatile u32 *csr = (volatile u32 *)KN02_CSR_BASE;
	unsigned long flags;
	int i;

	/* Mask interrupts. */
	spin_lock_irqsave(&kn02_lock, flags);
	cached_kn02_csr &= ~KN03_CSR_IOINTEN;
	*csr = cached_kn02_csr;
	iob();
	spin_unlock_irqrestore(&kn02_lock, flags);

	for (i = base; i < base + KN02_IRQ_LINES; i++) {
		irq_desc[i].status = IRQ_DISABLED;
		irq_desc[i].action = 0;
		irq_desc[i].depth = 1;
		irq_desc[i].handler = &kn02_irq_type;
	}

	kn02_irq_base = base;
}
