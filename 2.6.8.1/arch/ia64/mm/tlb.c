/*
 * TLB support routines.
 *
 * Copyright (C) 1998-2001, 2003 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 *
 * 08/02/00 A. Mallick <asit.k.mallick@intel.com>
 *		Modified RID allocation for SMP
 *          Goutham Rao <goutham.rao@intel.com>
 *              IPI based ptc implementation and A-step IPI implementation.
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/mm.h>

#include <asm/delay.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/pal.h>
#include <asm/tlbflush.h>

static struct {
	unsigned long mask;	/* mask of supported purge page-sizes */
	unsigned long max_bits;	/* log2() of largest supported purge page-size */
} purge;

struct ia64_ctx ia64_ctx = {
	.lock =		SPIN_LOCK_UNLOCKED,
	.next =		1,
	.limit =	(1 << 15) - 1,		/* start out with the safe (architected) limit */
	.max_ctx =	~0U
};

DEFINE_PER_CPU(u8, ia64_need_tlb_flush);

/*
 * Acquire the ia64_ctx.lock before calling this function!
 */
void
wrap_mmu_context (struct mm_struct *mm)
{
	unsigned long tsk_context, max_ctx = ia64_ctx.max_ctx;
	struct task_struct *tsk;
	int i;

	if (ia64_ctx.next > max_ctx)
		ia64_ctx.next = 300;	/* skip daemons */
	ia64_ctx.limit = max_ctx + 1;

	/*
	 * Scan all the task's mm->context and set proper safe range
	 */

	read_lock(&tasklist_lock);
  repeat:
	for_each_process(tsk) {
		if (!tsk->mm)
			continue;
		tsk_context = tsk->mm->context;
		if (tsk_context == ia64_ctx.next) {
			if (++ia64_ctx.next >= ia64_ctx.limit) {
				/* empty range: reset the range limit and start over */
				if (ia64_ctx.next > max_ctx)
					ia64_ctx.next = 300;
				ia64_ctx.limit = max_ctx + 1;
				goto repeat;
			}
		}
		if ((tsk_context > ia64_ctx.next) && (tsk_context < ia64_ctx.limit))
			ia64_ctx.limit = tsk_context;
	}
	read_unlock(&tasklist_lock);
	/* can't call flush_tlb_all() here because of race condition with O(1) scheduler [EF] */
	{
		int cpu = get_cpu(); /* prevent preemption/migration */
		for (i = 0; i < NR_CPUS; ++i)
			if (cpu_online(i) && (i != cpu))
				per_cpu(ia64_need_tlb_flush, i) = 1;
		put_cpu();
	}
	local_flush_tlb_all();
}

void
ia64_global_tlb_purge (unsigned long start, unsigned long end, unsigned long nbits)
{
	static spinlock_t ptcg_lock = SPIN_LOCK_UNLOCKED;

	/* HW requires global serialization of ptc.ga.  */
	spin_lock(&ptcg_lock);
	{
		do {
			/*
			 * Flush ALAT entries also.
			 */
			ia64_ptcga(start, (nbits<<2));
			ia64_srlz_i();
			start += (1UL << nbits);
		} while (start < end);
	}
	spin_unlock(&ptcg_lock);
}

void
local_flush_tlb_all (void)
{
	unsigned long i, j, flags, count0, count1, stride0, stride1, addr;

	addr    = local_cpu_data->ptce_base;
	count0  = local_cpu_data->ptce_count[0];
	count1  = local_cpu_data->ptce_count[1];
	stride0 = local_cpu_data->ptce_stride[0];
	stride1 = local_cpu_data->ptce_stride[1];

	local_irq_save(flags);
	for (i = 0; i < count0; ++i) {
		for (j = 0; j < count1; ++j) {
			ia64_ptce(addr);
			addr += stride1;
		}
		addr += stride0;
	}
	local_irq_restore(flags);
	ia64_srlz_i();			/* srlz.i implies srlz.d */
}
EXPORT_SYMBOL(local_flush_tlb_all);

void
flush_tlb_range (struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long size = end - start;
	unsigned long nbits;

	if (mm != current->active_mm) {
		/* this does happen, but perhaps it's not worth optimizing for? */
#ifdef CONFIG_SMP
		flush_tlb_all();
#else
		mm->context = 0;
#endif
		return;
	}

	nbits = ia64_fls(size + 0xfff);
	while (unlikely (((1UL << nbits) & purge.mask) == 0) && (nbits < purge.max_bits))
		++nbits;
	if (nbits > purge.max_bits)
		nbits = purge.max_bits;
	start &= ~((1UL << nbits) - 1);

# ifdef CONFIG_SMP
	platform_global_tlb_purge(start, end, nbits);
# else
	do {
		ia64_ptcl(start, (nbits<<2));
		start += (1UL << nbits);
	} while (start < end);
# endif

	ia64_srlz_i();			/* srlz.i implies srlz.d */
}
EXPORT_SYMBOL(flush_tlb_range);

void __devinit
ia64_tlb_init (void)
{
	ia64_ptce_info_t ptce_info;
	unsigned long tr_pgbits;
	long status;

	if ((status = ia64_pal_vm_page_size(&tr_pgbits, &purge.mask)) != 0) {
		printk(KERN_ERR "PAL_VM_PAGE_SIZE failed with status=%ld;"
		       "defaulting to architected purge page-sizes.\n", status);
		purge.mask = 0x115557000;
	}
	purge.max_bits = ia64_fls(purge.mask);

	ia64_get_ptce(&ptce_info);
	local_cpu_data->ptce_base = ptce_info.base;
	local_cpu_data->ptce_count[0] = ptce_info.count[0];
	local_cpu_data->ptce_count[1] = ptce_info.count[1];
	local_cpu_data->ptce_stride[0] = ptce_info.stride[0];
	local_cpu_data->ptce_stride[1] = ptce_info.stride[1];

	local_flush_tlb_all();		/* nuke left overs from bootstrapping... */
}
