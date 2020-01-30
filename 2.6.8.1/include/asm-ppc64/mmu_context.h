#ifndef __PPC64_MMU_CONTEXT_H
#define __PPC64_MMU_CONTEXT_H

#include <linux/config.h>
#include <linux/spinlock.h>	
#include <linux/kernel.h>	
#include <linux/mm.h>	
#include <asm/mmu.h>	
#include <asm/ppcdebug.h>	
#include <asm/cputable.h>

/*
 * Copyright (C) 2001 PPC 64 Team, IBM Corp
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/*
 * Every architecture must define this function. It's the fastest
 * way of searching a 140-bit bitmap where the first 100 bits are
 * unlikely to be set. It's guaranteed that at least one of the 140
 * bits is cleared.
 */
static inline int sched_find_first_bit(unsigned long *b)
{
	if (unlikely(b[0]))
		return __ffs(b[0]);
	if (unlikely(b[1]))
		return __ffs(b[1]) + 64;
	return __ffs(b[2]) + 128;
}

#define NO_CONTEXT		0
#define FIRST_USER_CONTEXT	0x10    /* First 16 reserved for kernel */
#define LAST_USER_CONTEXT	0x8000  /* Same as PID_MAX for now... */
#define NUM_USER_CONTEXT	(LAST_USER_CONTEXT-FIRST_USER_CONTEXT)

/* Choose whether we want to implement our context
 * number allocator as a LIFO or FIFO queue.
 */
#if 1
#define MMU_CONTEXT_LIFO
#else
#define MMU_CONTEXT_FIFO
#endif

struct mmu_context_queue_t {
	spinlock_t lock;
	long head;
	long tail;
	long size;
	mm_context_id_t elements[LAST_USER_CONTEXT];
};

extern struct mmu_context_queue_t mmu_context_queue;

static inline void
enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
}

/*
 * The context number queue has underflowed.
 * Meaning: we tried to push a context number that was freed
 * back onto the context queue and the queue was already full.
 */
static inline void
mmu_context_underflow(void)
{
	printk(KERN_DEBUG "mmu_context_underflow\n");
	panic("mmu_context_underflow");
}

/*
 * Set up the context for a new address space.
 */
static inline int
init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	long head;
	unsigned long flags;
	/* This does the right thing across a fork (I hope) */

	spin_lock_irqsave(&mmu_context_queue.lock, flags);

	if (mmu_context_queue.size <= 0) {
		spin_unlock_irqrestore(&mmu_context_queue.lock, flags);
		return -ENOMEM;
	}

	head = mmu_context_queue.head;
	mm->context.id = mmu_context_queue.elements[head];

	head = (head < LAST_USER_CONTEXT-1) ? head+1 : 0;
	mmu_context_queue.head = head;
	mmu_context_queue.size--;

	spin_unlock_irqrestore(&mmu_context_queue.lock, flags);

	return 0;
}

/*
 * We're finished using the context for an address space.
 */
static inline void
destroy_context(struct mm_struct *mm)
{
	long index;
	unsigned long flags;

	spin_lock_irqsave(&mmu_context_queue.lock, flags);

	if (mmu_context_queue.size >= NUM_USER_CONTEXT) {
		spin_unlock_irqrestore(&mmu_context_queue.lock, flags);
		mmu_context_underflow();
	}

#ifdef MMU_CONTEXT_LIFO
	index = mmu_context_queue.head;
	index = (index > 0) ? index-1 : LAST_USER_CONTEXT-1;
	mmu_context_queue.head = index;
#else
	index = mmu_context_queue.tail;
	index = (index < LAST_USER_CONTEXT-1) ? index+1 : 0;
	mmu_context_queue.tail = index;
#endif

	mmu_context_queue.size++;
	mmu_context_queue.elements[index] = mm->context.id;

	spin_unlock_irqrestore(&mmu_context_queue.lock, flags);
}

extern void flush_stab(struct task_struct *tsk, struct mm_struct *mm);
extern void switch_slb(struct task_struct *tsk, struct mm_struct *mm);

/*
 * switch_mm is the entry point called from the architecture independent
 * code in kernel/sched.c
 */
static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
			     struct task_struct *tsk)
{
#ifdef CONFIG_ALTIVEC
	asm volatile (
 BEGIN_FTR_SECTION
	"dssall;\n"
 END_FTR_SECTION_IFSET(CPU_FTR_ALTIVEC)
	 : : );
#endif /* CONFIG_ALTIVEC */

	if (!cpu_isset(smp_processor_id(), next->cpu_vm_mask))
		cpu_set(smp_processor_id(), next->cpu_vm_mask);

	/* No need to flush userspace segments if the mm doesnt change */
	if (prev == next)
		return;

	if (cur_cpu_spec->cpu_features & CPU_FTR_SLB)
		switch_slb(tsk, next);
	else
		flush_stab(tsk, next);
}

#define deactivate_mm(tsk,mm)	do { } while (0)

/*
 * After we have set current->mm to a new value, this activates
 * the context for the new mm so we see the new mappings.
 */
static inline void activate_mm(struct mm_struct *prev, struct mm_struct *next)
{
	unsigned long flags;

	local_irq_save(flags);
	switch_mm(prev, next, current);
	local_irq_restore(flags);
}

/* This is only valid for kernel (including vmalloc, imalloc and bolted) EA's
 */
static inline unsigned long
get_kernel_vsid( unsigned long ea )
{
	unsigned long ordinal, vsid;
	
	ordinal = (((ea >> 28) & 0x1fff) * LAST_USER_CONTEXT) | (ea >> 60);
	vsid = (ordinal * VSID_RANDOMIZER) & VSID_MASK;

#ifdef HTABSTRESS
	/* For debug, this path creates a very poor vsid distribuition.
	 * A user program can access virtual addresses in the form
	 * 0x0yyyyxxxx000 where yyyy = xxxx to cause multiple mappings
	 * to hash to the same page table group.
	 */
	ordinal = ((ea >> 28) & 0x1fff) | (ea >> 44);
	vsid = ordinal & VSID_MASK;
#endif /* HTABSTRESS */

	return vsid;
} 

/* This is only valid for user EA's (user EA's do not exceed 2^41 (EADDR_SIZE))
 */
static inline unsigned long
get_vsid( unsigned long context, unsigned long ea )
{
	unsigned long ordinal, vsid;

	ordinal = (((ea >> 28) & 0x1fff) * LAST_USER_CONTEXT) | context;
	vsid = (ordinal * VSID_RANDOMIZER) & VSID_MASK;

#ifdef HTABSTRESS
	/* See comment above. */
	ordinal = ((ea >> 28) & 0x1fff) | (context << 16);
	vsid = ordinal & VSID_MASK;
#endif /* HTABSTRESS */

	return vsid;
}

#endif /* __PPC64_MMU_CONTEXT_H */
