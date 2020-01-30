#ifndef _ALPHA_HARDIRQ_H
#define _ALPHA_HARDIRQ_H

#include <linux/config.h>
#include <linux/threads.h>
#include <linux/cache.h>


/* entry.S is sensitive to the offsets of these fields */
typedef struct {
	unsigned long __softirq_pending;
	unsigned int __syscall_count;
	unsigned long idle_timestamp;
	struct task_struct * __ksoftirqd_task;
} ____cacheline_aligned irq_cpustat_t;

#include <linux/irq_cpustat.h>	/* Standard mappings for irq_cpustat_t above */

/*
 * We put the hardirq and softirq counter into the preemption
 * counter. The bitmask has the following meaning:
 *
 * - bits 0-7 are the preemption count (max preemption depth: 256)
 * - bits 8-15 are the softirq count (max # of softirqs: 256)
 * - bits 16-27 are the hardirq count (max # of hardirqs: 4096)
 *
 * - ( bit 30 is the PREEMPT_ACTIVE flag. )
 *
 * PREEMPT_MASK: 0x000000ff
 * SOFTIRQ_MASK: 0x0000ff00
 * HARDIRQ_MASK: 0x0fff0000
 */

#define PREEMPT_BITS	8
#define SOFTIRQ_BITS	8
#define HARDIRQ_BITS	12

#define PREEMPT_SHIFT	0
#define SOFTIRQ_SHIFT	(PREEMPT_SHIFT + PREEMPT_BITS)
#define HARDIRQ_SHIFT	(SOFTIRQ_SHIFT + SOFTIRQ_BITS)

#define __MASK(x)	((1UL << (x))-1)

#define PREEMPT_MASK	(__MASK(PREEMPT_BITS) << PREEMPT_SHIFT)
#define HARDIRQ_MASK	(__MASK(HARDIRQ_BITS) << HARDIRQ_SHIFT)
#define SOFTIRQ_MASK	(__MASK(SOFTIRQ_BITS) << SOFTIRQ_SHIFT)

#define hardirq_count()	(preempt_count() & HARDIRQ_MASK)
#define softirq_count()	(preempt_count() & SOFTIRQ_MASK)
#define irq_count()	(preempt_count() & (HARDIRQ_MASK | SOFTIRQ_MASK))

#define PREEMPT_OFFSET	(1UL << PREEMPT_SHIFT)
#define SOFTIRQ_OFFSET	(1UL << SOFTIRQ_SHIFT)
#define HARDIRQ_OFFSET	(1UL << HARDIRQ_SHIFT)

/*
 * The hardirq mask has to be large enough to have
 * space for potentially nestable IRQ sources in the system
 * to nest on a single CPU. On Alpha, interrupts are masked at the CPU
 * by IPL as well as at the system level. We only have 8 IPLs (UNIX PALcode)
 * so we really only have 8 nestable IRQs, but allow some overhead
 */
#if (1 << HARDIRQ_BITS) < 16
#error HARDIRQ_BITS is too low!
#endif

/*
 * Are we doing bottom half or hardware interrupt processing?
 * Are we in a softirq context? Interrupt context?
 */
#define in_irq()		(hardirq_count())
#define in_softirq()		(softirq_count())
#define in_interrupt()		(irq_count())


#define hardirq_trylock()	(!in_interrupt())
#define hardirq_endlock()	do { } while (0)

#define irq_enter()		(preempt_count() += HARDIRQ_OFFSET)


#ifdef CONFIG_PREEMPT
#define in_atomic()	(preempt_count() != kernel_locked())
# define IRQ_EXIT_OFFSET (HARDIRQ_OFFSET-1)
#else
#define in_atomic()	(preempt_count() != 0)
#define IRQ_EXIT_OFFSET HARDIRQ_OFFSET
# endif
#define irq_exit()						\
do {								\
		preempt_count() -= IRQ_EXIT_OFFSET;		\
		if (!in_interrupt() &&				\
		    softirq_pending(smp_processor_id()))	\
			do_softirq();				\
		preempt_enable_no_resched();			\
} while (0)

#ifndef CONFIG_SMP
# define synchronize_irq(irq)	barrier()
#else
  extern void synchronize_irq(unsigned int irq);
#endif /* CONFIG_SMP */

#endif /* _ALPHA_HARDIRQ_H */
