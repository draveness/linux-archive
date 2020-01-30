/*
 *  linux/arch/m68k/amiga/cia.c - CIA support
 *
 *  Copyright (C) 1996 Roman Zippel
 *
 *  The concept of some functions bases on the original Amiga OS function
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/kernel_stat.h>
#include <linux/init.h>

#include <asm/irq.h>
#include <asm/amigahw.h>
#include <asm/amigaints.h>

struct ciabase {
	volatile struct CIA *cia;
	u_char icr_mask, icr_data;
	u_short int_mask;
	int handler_irq, cia_irq, server_irq;
	char *name;
	struct irq_server server;
	irq_handler_t irq_list[CIA_IRQS];
} ciaa_base = {
	&ciaa, 0, 0, IF_PORTS,
	IRQ_AMIGA_AUTO_2, IRQ_AMIGA_CIAA,
	IRQ_AMIGA_PORTS,
	"CIAA handler", {0, 0}
}, ciab_base = {
	&ciab, 0, 0, IF_EXTER,
	IRQ_AMIGA_AUTO_6, IRQ_AMIGA_CIAB,
	IRQ_AMIGA_EXTER,
	"CIAB handler", {0, 0}
};

#define CIA_SET_BASE_ADJUST_IRQ(base, irq)	\
do {						\
	if (irq >= IRQ_AMIGA_CIAB) {		\
		base = &ciab_base;		\
		irq -= IRQ_AMIGA_CIAB;		\
	} else {				\
		base = &ciaa_base;		\
		irq -= IRQ_AMIGA_CIAA;		\
	}					\
} while (0)

/*
 *  Cause or clear CIA interrupts, return old interrupt status.
 */

static unsigned char cia_set_irq_private(struct ciabase *base,
					 unsigned char mask)
{
	u_char old;

	old = (base->icr_data |= base->cia->icr);
	if (mask & CIA_ICR_SETCLR)
		base->icr_data |= mask;
	else
		base->icr_data &= ~mask;
	if (base->icr_data & base->icr_mask)
		custom.intreq = IF_SETCLR | base->int_mask;
	return old & base->icr_mask;
}

unsigned char cia_set_irq(unsigned int irq, int set)
{
	struct ciabase *base;
	unsigned char mask;

	if (irq >= IRQ_AMIGA_CIAB)
		mask = (1 << (irq - IRQ_AMIGA_CIAB));
	else
		mask = (1 << (irq - IRQ_AMIGA_CIAA));
	mask |= (set) ? CIA_ICR_SETCLR : 0;

	CIA_SET_BASE_ADJUST_IRQ(base, irq);

	return cia_set_irq_private(base, mask);
}

unsigned char cia_get_irq_mask(unsigned int irq)
{
	struct ciabase *base;

	CIA_SET_BASE_ADJUST_IRQ(base, irq);

	return base->cia->icr;
}

/*
 *  Enable or disable CIA interrupts, return old interrupt mask,
 *  interrupts will only be enabled if a handler exists
 */

static unsigned char cia_able_irq_private(struct ciabase *base,
					  unsigned char mask)
{
	u_char old, tmp;
	int i;

	old = base->icr_mask;
	base->icr_data |= base->cia->icr;
	base->cia->icr = mask;
	if (mask & CIA_ICR_SETCLR)
		base->icr_mask |= mask;
	else
		base->icr_mask &= ~mask;
	base->icr_mask &= CIA_ICR_ALL;
	for (i = 0, tmp = 1; i < CIA_IRQS; i++, tmp <<= 1) {
		if ((tmp & base->icr_mask) && !base->irq_list[i].handler) {
			base->icr_mask &= ~tmp;
			base->cia->icr = tmp;
		}
	}
	if (base->icr_data & base->icr_mask)
		custom.intreq = IF_SETCLR | base->int_mask;
	return old;
}

unsigned char cia_able_irq(unsigned int irq, int enable)
{
	struct ciabase *base;
	unsigned char mask;

	if (irq >= IRQ_AMIGA_CIAB)
		mask = (1 << (irq - IRQ_AMIGA_CIAB));
	else
		mask = (1 << (irq - IRQ_AMIGA_CIAA));
	mask |= (enable) ? CIA_ICR_SETCLR : 0;

	CIA_SET_BASE_ADJUST_IRQ(base, irq);

	return cia_able_irq_private(base, mask);
}

int cia_request_irq(unsigned int irq,
                    void (*handler)(int, void *, struct pt_regs *),
                    unsigned long flags, const char *devname, void *dev_id)
{
	u_char mask;
	struct ciabase *base;

	CIA_SET_BASE_ADJUST_IRQ(base, irq);

	base->irq_list[irq].handler = handler;
	base->irq_list[irq].flags   = flags;
	base->irq_list[irq].dev_id  = dev_id;
	base->irq_list[irq].devname = devname;

	/* enable the interrupt */
	mask = 1 << irq;
	cia_set_irq_private(base, mask);
	cia_able_irq_private(base, CIA_ICR_SETCLR | mask);
	return 0;
}

void cia_free_irq(unsigned int irq, void *dev_id)
{
	struct ciabase *base;

	CIA_SET_BASE_ADJUST_IRQ(base, irq);

	if (base->irq_list[irq].dev_id != dev_id)
		printk("%s: removing probably wrong IRQ %i from %s\n",
		       __FUNCTION__, base->cia_irq + irq,
		       base->irq_list[irq].devname);

	base->irq_list[irq].handler = NULL;
	base->irq_list[irq].flags   = 0;

	cia_able_irq_private(base, 1 << irq);
}

static void cia_handler(int irq, void *dev_id, struct pt_regs *fp)
{
	struct ciabase *base = (struct ciabase *)dev_id;
	int mach_irq, i;
	unsigned char ints;

	mach_irq = base->cia_irq;
	irq = SYS_IRQS + mach_irq;
	ints = cia_set_irq_private(base, CIA_ICR_ALL);
	custom.intreq = base->int_mask;
	for (i = 0; i < CIA_IRQS; i++, irq++, mach_irq++) {
		if (ints & 1) {
			kstat.irqs[0][irq]++;
			base->irq_list[i].handler(mach_irq, base->irq_list[i].dev_id, fp);
		}
		ints >>= 1;
	}
	amiga_do_irq_list(base->server_irq, fp, &base->server);
}

void __init cia_init_IRQ(struct ciabase *base)
{
	int i;

	/* init isr handlers */
	for (i = 0; i < CIA_IRQS; i++) {
		base->irq_list[i].handler = NULL;
		base->irq_list[i].flags   = 0;
	}

	/* clear any pending interrupt and turn off all interrupts */
	cia_set_irq_private(base, CIA_ICR_ALL);
	cia_able_irq_private(base, CIA_ICR_ALL);

	/* install CIA handler */
	request_irq(base->handler_irq, cia_handler, 0, base->name, base);

	custom.intena = IF_SETCLR | base->int_mask;
}

int cia_get_irq_list(struct ciabase *base, char *buf)
{
	int i, j, len = 0;

	j = base->cia_irq;
	for (i = 0; i < CIA_IRQS; i++) {
		len += sprintf(buf+len, "cia  %2d: %10d ", j + i,
			       kstat.irqs[0][SYS_IRQS + j + i]);
			len += sprintf(buf+len, "  ");
		len += sprintf(buf+len, "%s\n", base->irq_list[i].devname);
	}
	return len;
}
