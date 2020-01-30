/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Copyright (C) 2000, 2001 Kanoj Sarcar
 * Copyright (C) 2000, 2001 Ralf Baechle
 * Copyright (C) 2000, 2001 Silicon Graphics, Inc.
 * Copyright (C) 2000, 2001, 2003 Broadcom Corporation
 */
#include <linux/config.h>
#include <linux/cache.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/threads.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/sched.h>
#include <linux/cpumask.h>

#include <asm/atomic.h>
#include <asm/cpu.h>
#include <asm/processor.h>
#include <asm/system.h>
#include <asm/hardirq.h>
#include <asm/mmu_context.h>
#include <asm/smp.h>

cpumask_t phys_cpu_present_map;		/* Bitmask of available CPUs */
volatile cpumask_t cpu_callin_map;	/* Bitmask of started secondaries */
cpumask_t cpu_online_map;		/* Bitmask of currently online CPUs */
int __cpu_number_map[NR_CPUS];		/* Map physical to logical */
int __cpu_logical_map[NR_CPUS];		/* Map logical to physical */

EXPORT_SYMBOL(cpu_online_map);

cycles_t cacheflush_time;
unsigned long cache_decay_ticks;

static void smp_tune_scheduling (void)
{
	struct cache_desc *cd = &current_cpu_data.scache;
	unsigned long cachesize;       /* kB   */
	unsigned long bandwidth = 350; /* MB/s */
	unsigned long cpu_khz;

	/*
	 * Crude estimate until we actually meassure ...
	 */
	cpu_khz = loops_per_jiffy * 2 * HZ / 1000;

	/*
	 * Rough estimation for SMP scheduling, this is the number of
	 * cycles it takes for a fully memory-limited process to flush
	 * the SMP-local cache.
	 *
	 * (For a P5 this pretty much means we will choose another idle
	 *  CPU almost always at wakeup time (this is due to the small
	 *  L1 cache), on PIIs it's around 50-100 usecs, depending on
	 *  the cache size)
	 */
	if (!cpu_khz) {
		/*
		 * This basically disables processor-affinity scheduling on SMP
		 * without a cycle counter.  Currently all SMP capable MIPS
		 * processors have a cycle counter.
		 */
		cacheflush_time = 0;
		return;
	}

	cachesize = cd->linesz * cd->sets * cd->ways;
	cacheflush_time = (cpu_khz>>10) * (cachesize<<10) / bandwidth;
	cache_decay_ticks = (long)cacheflush_time/cpu_khz * HZ / 1000;

	printk("per-CPU timeslice cutoff: %ld.%02ld usecs.\n",
		(long)cacheflush_time/(cpu_khz/1000),
		((long)cacheflush_time*100/(cpu_khz/1000)) % 100);
	printk("task migration cache decay timeout: %ld msecs.\n",
		(cache_decay_ticks + 1) * 1000 / HZ);
}

extern void __init calibrate_delay(void);

/*
 * First C code run on the secondary CPUs after being started up by
 * the master.
 */
asmlinkage void start_secondary(void)
{
	unsigned int cpu = smp_processor_id();

	cpu_probe();
	cpu_report();
	per_cpu_trap_init();
	prom_init_secondary();

	/*
	 * XXX parity protection should be folded in here when it's converted
	 * to an option instead of something based on .cputype
	 */

	calibrate_delay();
	cpu_data[cpu].udelay_val = loops_per_jiffy;

	prom_smp_finish();

	cpu_set(cpu, cpu_callin_map);

	cpu_idle();
}

spinlock_t smp_call_lock = SPIN_LOCK_UNLOCKED;

struct call_data_struct *call_data;

/*
 * Run a function on all other CPUs.
 *  <func>      The function to run. This must be fast and non-blocking.
 *  <info>      An arbitrary pointer to pass to the function.
 *  <retry>     If true, keep retrying until ready.
 *  <wait>      If true, wait until function has completed on other CPUs.
 *  [RETURNS]   0 on success, else a negative status code.
 *
 * Does not return until remote CPUs are nearly ready to execute <func>
 * or are or have executed.
 *
 * You must not call this function with disabled interrupts or from a
 * hardware interrupt handler or from a bottom half handler.
 */
int smp_call_function (void (*func) (void *info), void *info, int retry,
								int wait)
{
	struct call_data_struct data;
	int i, cpus = num_online_cpus() - 1;
	int cpu = smp_processor_id();

	if (!cpus)
		return 0;

	/* Can deadlock when called with interrupts disabled */
	WARN_ON(irqs_disabled());

	data.func = func;
	data.info = info;
	atomic_set(&data.started, 0);
	data.wait = wait;
	if (wait)
		atomic_set(&data.finished, 0);

	spin_lock(&smp_call_lock);
	call_data = &data;
	mb();

	/* Send a message to all other CPUs and wait for them to respond */
	for (i = 0; i < NR_CPUS; i++)
		if (cpu_online(i) && i != cpu)
			core_send_ipi(i, SMP_CALL_FUNCTION);

	/* Wait for response */
	/* FIXME: lock-up detection, backtrace on lock-up */
	while (atomic_read(&data.started) != cpus)
		barrier();

	if (wait)
		while (atomic_read(&data.finished) != cpus)
			barrier();
	spin_unlock(&smp_call_lock);

	return 0;
}

void smp_call_function_interrupt(void)
{
	void (*func) (void *info) = call_data->func;
	void *info = call_data->info;
	int wait = call_data->wait;

	/*
	 * Notify initiating CPU that I've grabbed the data and am
	 * about to execute the function.
	 */
	mb();
	atomic_inc(&call_data->started);

	/*
	 * At this point the info structure may be out of scope unless wait==1.
	 */
	irq_enter();
	(*func)(info);
	irq_exit();

	if (wait) {
		mb();
		atomic_inc(&call_data->finished);
	}
}

static void stop_this_cpu(void *dummy)
{
	/*
	 * Remove this CPU:
	 */
	cpu_clear(smp_processor_id(), cpu_online_map);
	local_irq_enable();	/* May need to service _machine_restart IPI */
	for (;;);		/* Wait if available. */
}

void smp_send_stop(void)
{
	smp_call_function(stop_this_cpu, NULL, 1, 0);
}

void __init smp_cpus_done(unsigned int max_cpus)
{
	prom_cpus_done();
}

/* called from main before smp_init() */
void __init smp_prepare_cpus(unsigned int max_cpus)
{
	cpu_data[0].udelay_val = loops_per_jiffy;
	init_new_context(current, &init_mm);
	current_thread_info()->cpu = 0;
	smp_tune_scheduling();
	prom_build_cpu_map();
	prom_prepare_cpus(max_cpus);
}

/* preload SMP state for boot cpu */
void __devinit smp_prepare_boot_cpu(void)
{
	/*
	 * This assumes that bootup is always handled by the processor
	 * with the logic and physical number 0.
	 */
	__cpu_number_map[0] = 0;
	__cpu_logical_map[0] = 0;
	cpu_set(0, phys_cpu_present_map);
	cpu_set(0, cpu_online_map);
	cpu_set(0, cpu_callin_map);
}

static struct task_struct * __init fork_by_hand(void)
{
	struct pt_regs regs;
	/*
	 * don't care about the eip and regs settings since
	 * we'll never reschedule the forked task.
	 */
	return copy_process(CLONE_VM|CLONE_IDLETASK, 0, &regs, 0, NULL, NULL);
}

/*
 * Startup the CPU with this logical number
 */
static int __init do_boot_cpu(int cpu)
{
	struct task_struct *idle;

	/*
	 * The following code is purely to make sure
	 * Linux can schedule processes on this slave.
	 */
	idle = fork_by_hand();
	if (IS_ERR(idle))
		panic("failed fork for CPU %d\n", cpu);

	wake_up_forked_process(idle);

	/*
	 * We remove it from the pidhash and the runqueue once we've
	 * got the process:
	 */
	init_idle(idle, cpu);

	unhash_process(idle);

	prom_boot_secondary(cpu, idle);

	/* XXXKW timeout */
	while (!cpu_isset(cpu, cpu_callin_map))
		udelay(100);

	cpu_set(cpu, cpu_online_map);

	return 0;
}

/*
 * Called once for each "cpu_possible(cpu)".  Needs to spin up the cpu
 * and keep control until "cpu_online(cpu)" is set.  Note: cpu is
 * physical, not logical.
 */
int __devinit __cpu_up(unsigned int cpu)
{
	int ret;

	/* Processor goes to start_secondary(), sets online flag */
	ret = do_boot_cpu(cpu);
	if (ret < 0)
		return ret;

	return 0;
}

/* Not really SMP stuff ... */
int setup_profiling_timer(unsigned int multiplier)
{
	return 0;
}

static void flush_tlb_all_ipi(void *info)
{
	local_flush_tlb_all();
}

void flush_tlb_all(void)
{
	on_each_cpu(flush_tlb_all_ipi, 0, 1, 1);
}

static void flush_tlb_mm_ipi(void *mm)
{
	local_flush_tlb_mm((struct mm_struct *)mm);
}

/*
 * The following tlb flush calls are invoked when old translations are
 * being torn down, or pte attributes are changing. For single threaded
 * address spaces, a new context is obtained on the current cpu, and tlb
 * context on other cpus are invalidated to force a new context allocation
 * at switch_mm time, should the mm ever be used on other cpus. For
 * multithreaded address spaces, intercpu interrupts have to be sent.
 * Another case where intercpu interrupts are required is when the target
 * mm might be active on another cpu (eg debuggers doing the flushes on
 * behalf of debugees, kswapd stealing pages from another process etc).
 * Kanoj 07/00.
 */

void flush_tlb_mm(struct mm_struct *mm)
{
	preempt_disable();

	if ((atomic_read(&mm->mm_users) != 1) || (current->mm != mm)) {
		smp_call_function(flush_tlb_mm_ipi, (void *)mm, 1, 1);
	} else {
		int i;
		for (i = 0; i < num_online_cpus(); i++)
			if (smp_processor_id() != i)
				cpu_context(i, mm) = 0;
	}
	local_flush_tlb_mm(mm);

	preempt_enable();
}

struct flush_tlb_data {
	struct vm_area_struct *vma;
	unsigned long addr1;
	unsigned long addr2;
};

static void flush_tlb_range_ipi(void *info)
{
	struct flush_tlb_data *fd = (struct flush_tlb_data *)info;

	local_flush_tlb_range(fd->vma, fd->addr1, fd->addr2);
}

void flush_tlb_range(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;

	preempt_disable();
	if ((atomic_read(&mm->mm_users) != 1) || (current->mm != mm)) {
		struct flush_tlb_data fd;

		fd.vma = vma;
		fd.addr1 = start;
		fd.addr2 = end;
		smp_call_function(flush_tlb_range_ipi, (void *)&fd, 1, 1);
	} else {
		int i;
		for (i = 0; i < num_online_cpus(); i++)
			if (smp_processor_id() != i)
				cpu_context(i, mm) = 0;
	}
	local_flush_tlb_range(vma, start, end);
	preempt_enable();
}

static void flush_tlb_kernel_range_ipi(void *info)
{
	struct flush_tlb_data *fd = (struct flush_tlb_data *)info;

	local_flush_tlb_kernel_range(fd->addr1, fd->addr2);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	struct flush_tlb_data fd;

	fd.addr1 = start;
	fd.addr2 = end;
	on_each_cpu(flush_tlb_kernel_range_ipi, (void *)&fd, 1, 1);
}

static void flush_tlb_page_ipi(void *info)
{
	struct flush_tlb_data *fd = (struct flush_tlb_data *)info;

	local_flush_tlb_page(fd->vma, fd->addr1);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long page)
{
	preempt_disable();
	if ((atomic_read(&vma->vm_mm->mm_users) != 1) || (current->mm != vma->vm_mm)) {
		struct flush_tlb_data fd;

		fd.vma = vma;
		fd.addr1 = page;
		smp_call_function(flush_tlb_page_ipi, (void *)&fd, 1, 1);
	} else {
		int i;
		for (i = 0; i < num_online_cpus(); i++)
			if (smp_processor_id() != i)
				cpu_context(i, vma->vm_mm) = 0;
	}
	local_flush_tlb_page(vma, page);
	preempt_enable();
}

static void flush_tlb_one_ipi(void *info)
{
	unsigned long vaddr = (unsigned long) info;

	local_flush_tlb_one(vaddr);
}

void flush_tlb_one(unsigned long vaddr)
{
	smp_call_function(flush_tlb_one_ipi, (void *) vaddr, 1, 1);
	local_flush_tlb_one(vaddr);
}

EXPORT_SYMBOL(flush_tlb_page);
EXPORT_SYMBOL(flush_tlb_one);
EXPORT_SYMBOL(cpu_data);
EXPORT_SYMBOL(synchronize_irq);
