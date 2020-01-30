/*
 * Smp support for CHRP machines.
 *
 * Written by Cort Dougan (cort@cs.nmt.edu) borrowing a great
 * deal of code from the sparc and intel versions.
 *
 * Copyright (C) 1999 Cort Dougan <cort@cs.nmt.edu>
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>

#include <asm/ptrace.h>
#include <asm/atomic.h>
#include <asm/irq.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/hardirq.h>
#include <asm/sections.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/smp.h>
#include <asm/residual.h>
#include <asm/time.h>
#include <asm/open_pic.h>

extern unsigned long smp_chrp_cpu_nr;

static int __init
smp_chrp_probe(void)
{
	if (smp_chrp_cpu_nr > 1)
		openpic_request_IPIs();

	return smp_chrp_cpu_nr;
}

static void __devinit
smp_chrp_kick_cpu(int nr)
{
	*(unsigned long *)KERNELBASE = nr;
	asm volatile("dcbf 0,%0"::"r"(KERNELBASE):"memory");
}

static void __devinit
smp_chrp_setup_cpu(int cpu_nr)
{
	if (OpenPIC_Addr)
		do_openpic_setup_cpu();
}

static spinlock_t timebase_lock = SPIN_LOCK_UNLOCKED;
static unsigned int timebase_upper = 0, timebase_lower = 0;

void __devinit
smp_chrp_give_timebase(void)
{
	spin_lock(&timebase_lock);
	call_rtas("freeze-time-base", 0, 1, NULL);
	timebase_upper = get_tbu();
	timebase_lower = get_tbl();
	spin_unlock(&timebase_lock);

	while (timebase_upper || timebase_lower)
		barrier();
	call_rtas("thaw-time-base", 0, 1, NULL);
}

void __devinit
smp_chrp_take_timebase(void)
{
	while (!(timebase_upper || timebase_lower))
		barrier();
	spin_lock(&timebase_lock);
	set_tb(timebase_upper, timebase_lower);
	timebase_upper = 0;
	timebase_lower = 0;
	spin_unlock(&timebase_lock);
	printk("CPU %i taken timebase\n", smp_processor_id());
}

/* CHRP with openpic */
struct smp_ops_t chrp_smp_ops __chrpdata = {
	.message_pass = smp_openpic_message_pass,
	.probe = smp_chrp_probe,
	.kick_cpu = smp_chrp_kick_cpu,
	.setup_cpu = smp_chrp_setup_cpu,
	.give_timebase = smp_chrp_give_timebase,
	.take_timebase = smp_chrp_take_timebase,
};
