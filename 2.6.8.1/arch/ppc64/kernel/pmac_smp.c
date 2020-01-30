/*
 * SMP support for power macintosh.
 *
 * We support both the old "powersurge" SMP architecture
 * and the current Core99 (G4 PowerMac) machines.
 *
 * Note that we don't support the very first rev. of
 * Apple/DayStar 2 CPUs board, the one with the funky
 * watchdog. Hopefully, none of these should be there except
 * maybe internally to Apple. I should probably still add some
 * code to detect this card though and disable SMP. --BenH.
 *
 * Support Macintosh G4 SMP by Troy Benjegerdes (hozer@drgw.net)
 * and Ben Herrenschmidt <benh@kernel.crashing.org>.
 *
 * Support for DayStar quad CPU cards
 * Copyright (C) XLR8, Inc. 1994-2000
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
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
#include <linux/errno.h>

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
#include <asm/machdep.h>
#include <asm/pmac_feature.h>
#include <asm/time.h>
#include <asm/cacheflush.h>
#include <asm/keylargo.h>

#include "open_pic.h"

extern void pmac_secondary_start_1(void);
extern void pmac_secondary_start_2(void);
extern void pmac_secondary_start_3(void);

extern void smp_openpic_message_pass(int target, int msg);

extern struct smp_ops_t *smp_ops;

static int __init smp_core99_probe(void)
{
	struct device_node *cpus;
	int ncpus = 1;

	/* Maybe use systemconfiguration here ? */
	if (ppc_md.progress) ppc_md.progress("smp_core99_probe", 0x345);
	cpus = find_type_devices("cpu");
	if (cpus == NULL)
		return 0;

       	while ((cpus = cpus->next) != NULL)
	       	++ncpus;

	printk(KERN_INFO "PowerMac SMP probe found %d cpus\n", ncpus);

	if (ncpus > 1)
		openpic_request_IPIs();

	return ncpus;
}

static void __init smp_core99_kick_cpu(int nr)
{
	int save_vector;
	unsigned long new_vector;
	unsigned long flags;
	volatile unsigned int *vector
		 = ((volatile unsigned int *)(KERNELBASE+0x100));

	if (nr < 1 || nr > 3)
		return;
	if (ppc_md.progress) ppc_md.progress("smp_core99_kick_cpu", 0x346);

	local_irq_save(flags);
	local_irq_disable();

	/* Save reset vector */
	save_vector = *vector;

	/* Setup fake reset vector that does	
	 *   b .pmac_secondary_start - KERNELBASE
	 */
	switch(nr) {
		case 1:
			new_vector = (unsigned long)pmac_secondary_start_1;
			break;
		case 2:
			new_vector = (unsigned long)pmac_secondary_start_2;
			break;
		case 3:
			new_vector = (unsigned long)pmac_secondary_start_3;
			break;
	}
	*vector = 0x48000002 + (new_vector - KERNELBASE);

	/* flush data cache and inval instruction cache */
	flush_icache_range((unsigned long) vector, (unsigned long) vector + 4);

	/* Put some life in our friend */
	pmac_call_feature(PMAC_FTR_RESET_CPU, NULL, nr, 0);
	paca[nr].cpu_start = 1;

	/* FIXME: We wait a bit for the CPU to take the exception, I should
	 * instead wait for the entry code to set something for me. Well,
	 * ideally, all that crap will be done in prom.c and the CPU left
	 * in a RAM-based wait loop like CHRP.
	 */
	mdelay(1);

	/* Restore our exception vector */
	*vector = save_vector;
	flush_icache_range((unsigned long) vector, (unsigned long) vector + 4);

	local_irq_restore(flags);
	if (ppc_md.progress) ppc_md.progress("smp_core99_kick_cpu done", 0x347);
}

static void __init smp_core99_setup_cpu(int cpu_nr)
{
	/* Setup openpic */
	do_openpic_setup_cpu();

	if (cpu_nr == 0) {
		extern void g5_phy_disable_cpu1(void);

		/* If we didn't start the second CPU, we must take
		 * it off the bus
		 */
		if (num_online_cpus() < 2)		
			g5_phy_disable_cpu1();
		if (ppc_md.progress) ppc_md.progress("core99_setup_cpu 0 done", 0x349);
	}
}

extern void smp_generic_give_timebase(void);
extern void smp_generic_take_timebase(void);

struct smp_ops_t core99_smp_ops __pmacdata = {
	.message_pass	= smp_openpic_message_pass,
	.probe		= smp_core99_probe,
	.kick_cpu	= smp_core99_kick_cpu,
	.setup_cpu	= smp_core99_setup_cpu,
	.give_timebase	= smp_generic_give_timebase,
	.take_timebase	= smp_generic_take_timebase,
};

void __init pmac_setup_smp(void)
{
	smp_ops = &core99_smp_ops;
}
