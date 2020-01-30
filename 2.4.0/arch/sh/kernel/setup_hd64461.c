/*
 *	$Id: setup_hd64461.c,v 1.1 2000/06/10 21:45:18 yaegashi Exp $
 *	Copyright (C) 2000 YAEGASHI Takeshi
 *	Hitachi HD64461 companion chip support
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/irq.h>

#include <asm/io.h>
#include <asm/irq.h>

#include <asm/hd64461.h>

static void disable_hd64461_irq(unsigned int irq)
{
	unsigned long flags;
	unsigned short nimr;
	unsigned short mask = 1 << (irq - HD64461_IRQBASE);

	save_and_cli(flags);
	nimr = inw(HD64461_NIMR);
	nimr |= mask;
	outw(nimr, HD64461_NIMR);
	restore_flags(flags);
}


static void enable_hd64461_irq(unsigned int irq)
{
	unsigned long flags;
	unsigned short nimr;
	unsigned short mask = 1 << (irq - HD64461_IRQBASE);

	save_and_cli(flags);
	nimr = inw(HD64461_NIMR);
	nimr &= ~mask;
	outw(nimr, HD64461_NIMR);
	restore_flags(flags);
}


static void mask_and_ack_hd64461(unsigned int irq)
{
	disable_hd64461_irq(irq);
#ifdef CONFIG_HD64461_ENABLER
	if (irq == HD64461_IRQBASE + 13)
		outb(0x00, HD64461_PCC1CSCR);
#endif
}


static void end_hd64461_irq(unsigned int irq)
{
	enable_hd64461_irq(irq);
}


static unsigned int startup_hd64461_irq(unsigned int irq)
{ 
	enable_hd64461_irq(irq);
	return 0;
}


static void shutdown_hd64461_irq(unsigned int irq)
{
	disable_hd64461_irq(irq);
}


static struct hw_interrupt_type hd64461_irq_type = {
	"HD64461-IRQ",
	startup_hd64461_irq,
	shutdown_hd64461_irq,
	enable_hd64461_irq,
	disable_hd64461_irq,
	mask_and_ack_hd64461,
	end_hd64461_irq
};


static void hd64461_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	printk(KERN_INFO
	       "HD64461: spurious interrupt, nirr: 0x%lx nimr: 0x%lx\n",
	       inw(HD64461_NIRR), inw(HD64461_NIMR));
}

int hd64461_irq_demux(int irq)
{
	if (irq == CONFIG_HD64461_IRQ) {
		unsigned short bit;
		unsigned short nirr = inw(HD64461_NIRR);
		unsigned short nimr = inw(HD64461_NIMR);
		nirr &= ~nimr;
		for (bit = 1, irq = 0; irq < 16; bit <<= 1, irq++)
			if (nirr & bit) break;
		if (irq == 16) irq = CONFIG_HD64461_IRQ;
		else irq += HD64461_IRQBASE;
	}
	return irq;
}

static struct irqaction irq0  = { hd64461_interrupt, SA_INTERRUPT, 0, "HD64461", NULL, NULL};


int __init setup_hd64461(void)
{
	int i;

	if (!MACH_HD64461)
		return 0;

	printk(KERN_INFO "HD64461 configured at 0x%x on irq %d(mapped into %d to %d)\n",
	       CONFIG_HD64461_IOBASE, CONFIG_HD64461_IRQ,
	       HD64461_IRQBASE, HD64461_IRQBASE+15);
#ifdef CONFIG_CPU_SUBTYPE_SH7709
	/* IRQ line for HD64461 should be set level trigger mode("10"). */
	/* And this should be done earlier than the kernel starts. */
	ctrl_outw(0x0200, INTC_ICR1); /* when connected to IRQ4. */
#endif
	outw(0xffff, HD64461_NIMR);

	for (i = HD64461_IRQBASE; i < HD64461_IRQBASE + 16; i++) {
		irq_desc[i].handler = &hd64461_irq_type;
	}

	setup_irq(CONFIG_HD64461_IRQ, &irq0);

#ifdef CONFIG_HD64461_ENABLER
	printk(KERN_INFO "HD64461: enabling PCMCIA devices\n");
	outb(0x04, HD64461_PCC1CSCIER);
	outb(0x00, HD64461_PCC1CSCR);
#endif

	return 0;
}

module_init(setup_hd64461);
