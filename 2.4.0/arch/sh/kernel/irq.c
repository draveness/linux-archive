/* $Id: irq.c,v 1.12 2000/03/06 14:07:50 gniibe Exp $
 *
 * linux/arch/sh/kernel/irq.c
 *
 *	Copyright (C) 1992, 1998 Linus Torvalds, Ingo Molnar
 *
 *
 * SuperH version:  Copyright (C) 1999  Niibe Yutaka
 */

/*
 * IRQs are in fact implemented a bit like signal handlers for the kernel.
 * Naturally it's not a 1:1 relation, but there are similarities.
 */

#include <linux/config.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/kernel_stat.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/timex.h>
#include <linux/malloc.h>
#include <linux/random.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/init.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/bitops.h>
#include <asm/smp.h>
#include <asm/pgalloc.h>
#include <asm/delay.h>
#include <asm/irq.h>
#include <linux/irq.h>

/*
 * Micro-access to controllers is serialized over the whole
 * system. We never hold this lock when we call the actual
 * IRQ handler.
 */
spinlock_t irq_controller_lock = SPIN_LOCK_UNLOCKED;
/*
 * Controller mappings for all interrupt sources:
 */
irq_desc_t irq_desc[NR_IRQS] __cacheline_aligned =
				{ [0 ... NR_IRQS-1] = { 0, &no_irq_type, }};

/*
 * Special irq handlers.
 */

void no_action(int cpl, void *dev_id, struct pt_regs *regs) { }

/*
 * Generic no controller code
 */

static void enable_none(unsigned int irq) { }
static unsigned int startup_none(unsigned int irq) { return 0; }
static void disable_none(unsigned int irq) { }
static void ack_none(unsigned int irq)
{
/*
 * 'what should we do if we get a hw irq event on an illegal vector'.
 * each architecture has to answer this themselves, it doesnt deserve
 * a generic callback i think.
 */
	printk("unexpected IRQ trap at vector %02x\n", irq);
}

/* startup is the same as "enable", shutdown is same as "disable" */
#define shutdown_none	disable_none
#define end_none	enable_none

struct hw_interrupt_type no_irq_type = {
	"none",
	startup_none,
	shutdown_none,
	enable_none,
	disable_none,
	ack_none,
	end_none
};

/*
 * Generic, controller-independent functions:
 */

#if defined(CONFIG_PROC_FS)
int get_irq_list(char *buf)
{
	int i, j;
	struct irqaction * action;
	char *p = buf;

	p += sprintf(p, "           ");
	for (j=0; j<smp_num_cpus; j++)
		p += sprintf(p, "CPU%d       ",j);
	*p++ = '\n';

	for (i = 0 ; i < ACTUAL_NR_IRQS ; i++) {
		action = irq_desc[i].action;
		if (!action) 
			continue;
		p += sprintf(p, "%3d: ",i);
		p += sprintf(p, "%10u ", kstat_irqs(i));
		p += sprintf(p, " %14s", irq_desc[i].handler->typename);
		p += sprintf(p, "  %s", action->name);

		for (action=action->next; action; action = action->next)
			p += sprintf(p, ", %s", action->name);
		*p++ = '\n';
	}
	return p - buf;
}
#endif

/*
 * This should really return information about whether
 * we should do bottom half handling etc. Right now we
 * end up _always_ checking the bottom half, which is a
 * waste of time and is not what some drivers would
 * prefer.
 */
int handle_IRQ_event(unsigned int irq, struct pt_regs * regs, struct irqaction * action)
{
	int status;
	int cpu = smp_processor_id();

	irq_enter(cpu, irq);

	status = 1;	/* Force the "do bottom halves" bit */

	if (!(action->flags & SA_INTERRUPT))
		__sti();

	do {
		status |= action->flags;
		action->handler(irq, action->dev_id, regs);
		action = action->next;
	} while (action);
	if (status & SA_SAMPLE_RANDOM)
		add_interrupt_randomness(irq);
	__cli();

	irq_exit(cpu, irq);

	return status;
}

/*
 * Generic enable/disable code: this just calls
 * down into the PIC-specific version for the actual
 * hardware disable after having gotten the irq
 * controller lock. 
 */
void disable_irq_nosync(unsigned int irq)
{
	unsigned long flags;

	spin_lock_irqsave(&irq_controller_lock, flags);
	if (!irq_desc[irq].depth++) {
		irq_desc[irq].status |= IRQ_DISABLED;
		irq_desc[irq].handler->disable(irq);
	}
	spin_unlock_irqrestore(&irq_controller_lock, flags);
}

/*
 * Synchronous version of the above, making sure the IRQ is
 * no longer running on any other IRQ..
 */
void disable_irq(unsigned int irq)
{
	disable_irq_nosync(irq);

	if (!local_irq_count(smp_processor_id())) {
		do {
			barrier();
		} while (irq_desc[irq].status & IRQ_INPROGRESS);
	}
}

void enable_irq(unsigned int irq)
{
	unsigned long flags;

	spin_lock_irqsave(&irq_controller_lock, flags);
	switch (irq_desc[irq].depth) {
	case 1: {
		unsigned int status = irq_desc[irq].status & ~IRQ_DISABLED;
		irq_desc[irq].status = status;
		if ((status & (IRQ_PENDING | IRQ_REPLAY)) == IRQ_PENDING) {
			irq_desc[irq].status = status | IRQ_REPLAY;
			hw_resend_irq(irq_desc[irq].handler,irq);
		}
		irq_desc[irq].handler->enable(irq);
		/* fall-through */
	}
	default:
		irq_desc[irq].depth--;
		break;
	case 0:
		printk("enable_irq() unbalanced from %p\n",
		       __builtin_return_address(0));
	}
	spin_unlock_irqrestore(&irq_controller_lock, flags);
}

/*
 * do_IRQ handles all normal device IRQ's.
 */
asmlinkage int do_IRQ(unsigned long r4, unsigned long r5,
		      unsigned long r6, unsigned long r7,
		      struct pt_regs regs)
{	
	/* 
	 * We ack quickly, we don't want the irq controller
	 * thinking we're snobs just because some other CPU has
	 * disabled global interrupts (we have already done the
	 * INT_ACK cycles, it's too late to try to pretend to the
	 * controller that we aren't taking the interrupt).
	 *
	 * 0 return value means that this irq is already being
	 * handled by some other CPU. (or is disabled)
	 */
	int irq;
	int cpu = smp_processor_id();
	irq_desc_t *desc;
	struct irqaction * action;
	unsigned int status;

	/* Get IRQ number */
	asm volatile("stc	$r2_bank, %0\n\t"
		     "shlr2	%0\n\t"
		     "shlr2	%0\n\t"
		     "shlr	%0\n\t"
		     "add	#-16, %0\n\t"
		     :"=z" (irq));
	irq = irq_demux(irq);

	kstat.irqs[cpu][irq]++;
	desc = irq_desc + irq;
	spin_lock(&irq_controller_lock);
	desc->handler->ack(irq);
	/*
	   REPLAY is when Linux resends an IRQ that was dropped earlier
	   WAITING is used by probe to mark irqs that are being tested
	   */
	status = desc->status & ~(IRQ_REPLAY | IRQ_WAITING);
	status |= IRQ_PENDING; /* we _want_ to handle it */

	/*
	 * If the IRQ is disabled for whatever reason, we cannot
	 * use the action we have.
	 */
	action = NULL;
	if (!(status & (IRQ_DISABLED | IRQ_INPROGRESS))) {
		action = desc->action;
		status &= ~IRQ_PENDING; /* we commit to handling */
		status |= IRQ_INPROGRESS; /* we are handling it */
	}
	desc->status = status;
	spin_unlock(&irq_controller_lock);

	/*
	 * If there is no IRQ handler or it was disabled, exit early.
	   Since we set PENDING, if another processor is handling
	   a different instance of this same irq, the other processor
	   will take care of it.
	 */
	if (!action)
		return 1;

	/*
	 * Edge triggered interrupts need to remember
	 * pending events.
	 * This applies to any hw interrupts that allow a second
	 * instance of the same irq to arrive while we are in do_IRQ
	 * or in the handler. But the code here only handles the _second_
	 * instance of the irq, not the third or fourth. So it is mostly
	 * useful for irq hardware that does not mask cleanly in an
	 * SMP environment.
	 */
	for (;;) {
		handle_IRQ_event(irq, &regs, action);
		spin_lock(&irq_controller_lock);

		if (!(desc->status & IRQ_PENDING))
			break;
		desc->status &= ~IRQ_PENDING;
		spin_unlock(&irq_controller_lock);
	}
	desc->status &= ~IRQ_INPROGRESS;
	if (!(desc->status & IRQ_DISABLED))
		desc->handler->end(irq);
	spin_unlock(&irq_controller_lock);

#if 0
	__sti();
#endif
	if (softirq_active(cpu)&softirq_mask(cpu))
		do_softirq();
	return 1;
}

int request_irq(unsigned int irq, 
		void (*handler)(int, void *, struct pt_regs *),
		unsigned long irqflags, 
		const char * devname,
		void *dev_id)
{
	int retval;
	struct irqaction * action;

	if (irq >= ACTUAL_NR_IRQS)
		return -EINVAL;
	if (!handler)
		return -EINVAL;

	action = (struct irqaction *)
			kmalloc(sizeof(struct irqaction), GFP_KERNEL);
	if (!action)
		return -ENOMEM;

	action->handler = handler;
	action->flags = irqflags;
	action->mask = 0;
	action->name = devname;
	action->next = NULL;
	action->dev_id = dev_id;

	retval = setup_irq(irq, action);
	if (retval)
		kfree(action);
	return retval;
}

void free_irq(unsigned int irq, void *dev_id)
{
	struct irqaction **p;
	unsigned long flags;

	if (irq >= ACTUAL_NR_IRQS)
		return;

	spin_lock_irqsave(&irq_controller_lock,flags);
	p = &irq_desc[irq].action;
	for (;;) {
		struct irqaction * action = *p;
		if (action) {
			struct irqaction **pp = p;
			p = &action->next;
			if (action->dev_id != dev_id)
				continue;

			/* Found it - now remove it from the list of entries */
			*pp = action->next;
			if (!irq_desc[irq].action) {
				irq_desc[irq].status |= IRQ_DISABLED;
				irq_desc[irq].handler->shutdown(irq);
			}
			spin_unlock_irqrestore(&irq_controller_lock,flags);
			kfree(action);
			return;
		}
		printk("Trying to free free IRQ%d\n",irq);
		spin_unlock_irqrestore(&irq_controller_lock,flags);
		return;
	}
}

/*
 * IRQ autodetection code..
 *
 * This depends on the fact that any interrupt that
 * comes in on to an unassigned handler will get stuck
 * with "IRQ_WAITING" cleared and the interrupt
 * disabled.
 */
unsigned long probe_irq_on(void)
{
	unsigned int i;
	unsigned long delay;
	unsigned long val;

	/*
	 * first, enable any unassigned irqs
	 */
	spin_lock_irq(&irq_controller_lock);
	for (i = NR_IRQS-1; i > 0; i--) {
		if (!irq_desc[i].action) {
			irq_desc[i].status |= IRQ_AUTODETECT | IRQ_WAITING;
			if (irq_desc[i].handler->startup(i))
				irq_desc[i].status |= IRQ_PENDING;
		}
	}
	spin_unlock_irq(&irq_controller_lock);

	/*
	 * Wait for spurious interrupts to trigger
	 */
	for (delay = jiffies + HZ/10; time_after(delay, jiffies); )
		/* about 100ms delay */ synchronize_irq();

	/*
	 * Now filter out any obviously spurious interrupts
	 */
	val = 0;
	spin_lock_irq(&irq_controller_lock);
	for (i=0; i<NR_IRQS; i++) {
		unsigned int status = irq_desc[i].status;

		if (!(status & IRQ_AUTODETECT))
			continue;

		/* It triggered already - consider it spurious. */
		if (!(status & IRQ_WAITING)) {
			irq_desc[i].status = status & ~IRQ_AUTODETECT;
			irq_desc[i].handler->shutdown(i);
		}

		if (i < 32)
			val |= 1 << i;
	}
	spin_unlock_irq(&irq_controller_lock);

	return val;
}

int probe_irq_off(unsigned long val)
{
	int i, irq_found, nr_irqs;

	nr_irqs = 0;
	irq_found = 0;
	spin_lock_irq(&irq_controller_lock);
	for (i=0; i<NR_IRQS; i++) {
		unsigned int status = irq_desc[i].status;

		if (!(status & IRQ_AUTODETECT))
			continue;

		if (!(status & IRQ_WAITING)) {
			if (!nr_irqs)
				irq_found = i;
			nr_irqs++;
		}
		irq_desc[i].status = status & ~IRQ_AUTODETECT;
		irq_desc[i].handler->shutdown(i);
	}
	spin_unlock_irq(&irq_controller_lock);

	if (nr_irqs > 1)
		irq_found = -irq_found;
	return irq_found;
}

int setup_irq(unsigned int irq, struct irqaction * new)
{
	int shared = 0;
	struct irqaction *old, **p;
	unsigned long flags;

	/*
	 * Some drivers like serial.c use request_irq() heavily,
	 * so we have to be careful not to interfere with a
	 * running system.
	 */
	if (new->flags & SA_SAMPLE_RANDOM) {
		/*
		 * This function might sleep, we want to call it first,
		 * outside of the atomic block.
		 * Yes, this might clear the entropy pool if the wrong
		 * driver is attempted to be loaded, without actually
		 * installing a new handler, but is this really a problem,
		 * only the sysadmin is able to do this.
		 */
		rand_initialize_irq(irq);
	}

	/*
	 * The following block of code has to be executed atomically
	 */
	spin_lock_irqsave(&irq_controller_lock,flags);
	p = &irq_desc[irq].action;
	if ((old = *p) != NULL) {
		/* Can't share interrupts unless both agree to */
		if (!(old->flags & new->flags & SA_SHIRQ)) {
			spin_unlock_irqrestore(&irq_controller_lock,flags);
			return -EBUSY;
		}

		/* add new interrupt at end of irq queue */
		do {
			p = &old->next;
			old = *p;
		} while (old);
		shared = 1;
	}

	*p = new;

	if (!shared) {
		irq_desc[irq].depth = 0;
		irq_desc[irq].status &= ~IRQ_DISABLED;
		irq_desc[irq].handler->startup(irq);
	}
	spin_unlock_irqrestore(&irq_controller_lock,flags);
	return 0;
}

#if defined(CONFIG_PROC_FS) && defined(CONFIG_SYSCTL)

void init_irq_proc(void)
{
}
#endif
