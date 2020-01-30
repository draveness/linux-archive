#ifndef __I386_SCHED_H
#define __I386_SCHED_H

#include <linux/config.h>
#include <asm/desc.h>
#include <asm/atomic.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

/*
 * Used for LDT copy/destruction.
 */
int init_new_context(struct task_struct *tsk, struct mm_struct *mm);
void destroy_context(struct mm_struct *mm);


static inline void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
#ifdef CONFIG_SMP
	unsigned cpu = smp_processor_id();
	if (cpu_tlbstate[cpu].state == TLBSTATE_OK)
		cpu_tlbstate[cpu].state = TLBSTATE_LAZY;	
#endif
}

static inline void switch_mm(struct mm_struct *prev,
			     struct mm_struct *next,
			     struct task_struct *tsk)
{
	int cpu = smp_processor_id();

	if (likely(prev != next)) {
		/* stop flush ipis for the previous mm */
		cpu_clear(cpu, prev->cpu_vm_mask);
#ifdef CONFIG_SMP
		cpu_tlbstate[cpu].state = TLBSTATE_OK;
		cpu_tlbstate[cpu].active_mm = next;
#endif
		cpu_set(cpu, next->cpu_vm_mask);

		/* Re-load page tables */
		load_cr3(next->pgd);

		/*
		 * load the LDT, if the LDT is different:
		 */
		if (unlikely(prev->context.ldt != next->context.ldt))
			load_LDT_nolock(&next->context, cpu);
	}
#ifdef CONFIG_SMP
	else {
		cpu_tlbstate[cpu].state = TLBSTATE_OK;
		BUG_ON(cpu_tlbstate[cpu].active_mm != next);

		if (!cpu_test_and_set(cpu, next->cpu_vm_mask)) {
			/* We were in lazy tlb mode and leave_mm disabled 
			 * tlb flush IPI delivery. We must reload %cr3.
			 */
			load_cr3(next->pgd);
			load_LDT_nolock(&next->context, cpu);
		}
	}
#endif
}

#define deactivate_mm(tsk, mm) \
	asm("movl %0,%%fs ; movl %0,%%gs": :"r" (0))

#define activate_mm(prev, next) \
	switch_mm((prev),(next),NULL)

#endif
