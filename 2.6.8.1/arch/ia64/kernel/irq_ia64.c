/*
 * linux/arch/ia64/kernel/irq.c
 *
 * Copyright (C) 1998-2001 Hewlett-Packard Co
 *	Stephane Eranian <eranian@hpl.hp.com>
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 *
 *  6/10/99: Updated to bring in sync with x86 version to facilitate
 *	     support for SMP and different interrupt controllers.
 *
 * 09/15/00 Goutham Rao <goutham.rao@intel.com> Implemented pci_irq_to_vector
 *                      PCI to vector allocation routine.
 * 04/14/2004 Ashok Raj <ashok.raj@intel.com>
 *						Added CPU Hotplug handling for IPF.
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/kernel_stat.h>
#include <linux/slab.h>
#include <linux/ptrace.h>
#include <linux/random.h>	/* for rand_initialize_irq() */
#include <linux/signal.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/threads.h>

#include <asm/bitops.h>
#include <asm/delay.h>
#include <asm/intrinsics.h>
#include <asm/io.h>
#include <asm/hw_irq.h>
#include <asm/machvec.h>
#include <asm/pgtable.h>
#include <asm/system.h>

#ifdef CONFIG_PERFMON
# include <asm/perfmon.h>
#endif

#define IRQ_DEBUG	0

/* default base addr of IPI table */
unsigned long ipi_base_addr = (__IA64_UNCACHED_OFFSET | IA64_IPI_DEFAULT_BASE_ADDR);

/*
 * Legacy IRQ to IA-64 vector translation table.
 */
__u8 isa_irq_to_vector_map[16] = {
	/* 8259 IRQ translation, first 16 entries */
	0x2f, 0x20, 0x2e, 0x2d, 0x2c, 0x2b, 0x2a, 0x29,
	0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21
};
EXPORT_SYMBOL(isa_irq_to_vector_map);

static inline void
irq_enter (void)
{
	preempt_count() += HARDIRQ_OFFSET;
}

static inline void
irq_exit (void)
{
	preempt_count() -= IRQ_EXIT_OFFSET;
	if (!in_interrupt() && local_softirq_pending())
		do_softirq();
	preempt_enable_no_resched();
}

int
assign_irq_vector (int irq)
{
	static int next_vector = IA64_FIRST_DEVICE_VECTOR;

	if (next_vector > IA64_LAST_DEVICE_VECTOR)
		/* XXX could look for sharable vectors instead of panic'ing... */
		panic("assign_irq_vector: out of interrupt vectors!");
	return next_vector++;
}

extern unsigned int do_IRQ(unsigned long irq, struct pt_regs *regs);

#ifdef CONFIG_SMP
#	define IS_RESCHEDULE(vec)	(vec == IA64_IPI_RESCHEDULE)
#else
#	define IS_RESCHEDULE(vec)	(0)
#endif
/*
 * That's where the IVT branches when we get an external
 * interrupt. This branches to the correct hardware IRQ handler via
 * function ptr.
 */
void
ia64_handle_irq (ia64_vector vector, struct pt_regs *regs)
{
	unsigned long saved_tpr;

#if IRQ_DEBUG
	{
		unsigned long bsp, sp;

		/*
		 * Note: if the interrupt happened while executing in
		 * the context switch routine (ia64_switch_to), we may
		 * get a spurious stack overflow here.  This is
		 * because the register and the memory stack are not
		 * switched atomically.
		 */
		bsp = ia64_getreg(_IA64_REG_AR_BSP);
		sp = ia64_getreg(_IA64_REG_AR_SP);

		if ((sp - bsp) < 1024) {
			static unsigned char count;
			static long last_time;

			if (jiffies - last_time > 5*HZ)
				count = 0;
			if (++count < 5) {
				last_time = jiffies;
				printk("ia64_handle_irq: DANGER: less than "
				       "1KB of free stack space!!\n"
				       "(bsp=0x%lx, sp=%lx)\n", bsp, sp);
			}
		}
	}
#endif /* IRQ_DEBUG */

	/*
	 * Always set TPR to limit maximum interrupt nesting depth to
	 * 16 (without this, it would be ~240, which could easily lead
	 * to kernel stack overflows).
	 */
	irq_enter();
	saved_tpr = ia64_getreg(_IA64_REG_CR_TPR);
	ia64_srlz_d();
	while (vector != IA64_SPURIOUS_INT_VECTOR) {
		if (!IS_RESCHEDULE(vector)) {
			ia64_setreg(_IA64_REG_CR_TPR, vector);
			ia64_srlz_d();

			do_IRQ(local_vector_to_irq(vector), regs);

			/*
			 * Disable interrupts and send EOI:
			 */
			local_irq_disable();
			ia64_setreg(_IA64_REG_CR_TPR, saved_tpr);
		}
		ia64_eoi();
		vector = ia64_get_ivr();
	}
	/*
	 * This must be done *after* the ia64_eoi().  For example, the keyboard softirq
	 * handler needs to be able to wait for further keyboard interrupts, which can't
	 * come through until ia64_eoi() has been done.
	 */
	irq_exit();
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * This function emulates a interrupt processing when a cpu is about to be
 * brought down.
 */
void ia64_process_pending_intr(void)
{
	ia64_vector vector;
	unsigned long saved_tpr;
	extern unsigned int vectors_in_migration[NR_IRQS];

	vector = ia64_get_ivr();

	 irq_enter();
	 saved_tpr = ia64_getreg(_IA64_REG_CR_TPR);
	 ia64_srlz_d();

	 /*
	  * Perform normal interrupt style processing
	  */
	while (vector != IA64_SPURIOUS_INT_VECTOR) {
		if (!IS_RESCHEDULE(vector)) {
			ia64_setreg(_IA64_REG_CR_TPR, vector);
			ia64_srlz_d();

			/*
			 * Now try calling normal ia64_handle_irq as it would have got called
			 * from a real intr handler. Try passing null for pt_regs, hopefully
			 * it will work. I hope it works!.
			 * Probably could shared code.
			 */
			vectors_in_migration[local_vector_to_irq(vector)]=0;
			do_IRQ(local_vector_to_irq(vector), NULL);

			/*
			 * Disable interrupts and send EOI
			 */
			local_irq_disable();
			ia64_setreg(_IA64_REG_CR_TPR, saved_tpr);
		}
		ia64_eoi();
		vector = ia64_get_ivr();
	}
	irq_exit();
}
#endif


#ifdef CONFIG_SMP
extern irqreturn_t handle_IPI (int irq, void *dev_id, struct pt_regs *regs);

static struct irqaction ipi_irqaction = {
	.handler =	handle_IPI,
	.flags =	SA_INTERRUPT,
	.name =		"IPI"
};
#endif

void
register_percpu_irq (ia64_vector vec, struct irqaction *action)
{
	irq_desc_t *desc;
	unsigned int irq;

	for (irq = 0; irq < NR_IRQS; ++irq)
		if (irq_to_vector(irq) == vec) {
			desc = irq_descp(irq);
			desc->status |= IRQ_PER_CPU;
			desc->handler = &irq_type_ia64_lsapic;
			if (action)
				setup_irq(irq, action);
		}
}

void __init
init_IRQ (void)
{
	register_percpu_irq(IA64_SPURIOUS_INT_VECTOR, NULL);
#ifdef CONFIG_SMP
	register_percpu_irq(IA64_IPI_VECTOR, &ipi_irqaction);
#endif
#ifdef CONFIG_PERFMON
	pfm_init_percpu();
#endif
	platform_irq_init();
}

void
ia64_send_ipi (int cpu, int vector, int delivery_mode, int redirect)
{
	unsigned long ipi_addr;
	unsigned long ipi_data;
	unsigned long phys_cpu_id;

#ifdef CONFIG_SMP
	phys_cpu_id = cpu_physical_id(cpu);
#else
	phys_cpu_id = (ia64_getreg(_IA64_REG_CR_LID) >> 16) & 0xffff;
#endif

	/*
	 * cpu number is in 8bit ID and 8bit EID
	 */

	ipi_data = (delivery_mode << 8) | (vector & 0xff);
	ipi_addr = ipi_base_addr | (phys_cpu_id << 4) | ((redirect & 1)  << 3);

	writeq(ipi_data, ipi_addr);
}
