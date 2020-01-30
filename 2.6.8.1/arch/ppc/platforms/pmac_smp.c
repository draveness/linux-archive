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
#include <asm/residual.h>
#include <asm/machdep.h>
#include <asm/pmac_feature.h>
#include <asm/time.h>
#include <asm/open_pic.h>
#include <asm/cacheflush.h>
#include <asm/keylargo.h>

/*
 * Powersurge (old powermac SMP) support.
 */

extern void __secondary_start_psurge(void);
extern void __secondary_start_psurge2(void);	/* Temporary horrible hack */
extern void __secondary_start_psurge3(void);	/* Temporary horrible hack */

/* Addresses for powersurge registers */
#define HAMMERHEAD_BASE		0xf8000000
#define HHEAD_CONFIG		0x90
#define HHEAD_SEC_INTR		0xc0

/* register for interrupting the primary processor on the powersurge */
/* N.B. this is actually the ethernet ROM! */
#define PSURGE_PRI_INTR		0xf3019000

/* register for storing the start address for the secondary processor */
/* N.B. this is the PCI config space address register for the 1st bridge */
#define PSURGE_START		0xf2800000

/* Daystar/XLR8 4-CPU card */
#define PSURGE_QUAD_REG_ADDR	0xf8800000

#define PSURGE_QUAD_IRQ_SET	0
#define PSURGE_QUAD_IRQ_CLR	1
#define PSURGE_QUAD_IRQ_PRIMARY	2
#define PSURGE_QUAD_CKSTOP_CTL	3
#define PSURGE_QUAD_PRIMARY_ARB	4
#define PSURGE_QUAD_BOARD_ID	6
#define PSURGE_QUAD_WHICH_CPU	7
#define PSURGE_QUAD_CKSTOP_RDBK	8
#define PSURGE_QUAD_RESET_CTL	11

#define PSURGE_QUAD_OUT(r, v)	(out_8(quad_base + ((r) << 4) + 4, (v)))
#define PSURGE_QUAD_IN(r)	(in_8(quad_base + ((r) << 4) + 4) & 0x0f)
#define PSURGE_QUAD_BIS(r, v)	(PSURGE_QUAD_OUT((r), PSURGE_QUAD_IN(r) | (v)))
#define PSURGE_QUAD_BIC(r, v)	(PSURGE_QUAD_OUT((r), PSURGE_QUAD_IN(r) & ~(v)))

/* virtual addresses for the above */
static volatile u8 *hhead_base;
static volatile u8 *quad_base;
static volatile u32 *psurge_pri_intr;
static volatile u8 *psurge_sec_intr;
static volatile u32 *psurge_start;

/* values for psurge_type */
#define PSURGE_NONE		-1
#define PSURGE_DUAL		0
#define PSURGE_QUAD_OKEE	1
#define PSURGE_QUAD_COTTON	2
#define PSURGE_QUAD_ICEGRASS	3

/* what sort of powersurge board we have */
static int psurge_type = PSURGE_NONE;

/* L2 and L3 cache settings to pass from CPU0 to CPU1 */
volatile static long int core99_l2_cache;
volatile static long int core99_l3_cache;

/* Timebase freeze GPIO */
static unsigned int core99_tb_gpio;

/* Sync flag for HW tb sync */
static volatile int sec_tb_reset = 0;

static void __init core99_init_caches(int cpu)
{
	if (!(cur_cpu_spec[0]->cpu_features & CPU_FTR_L2CR))
		return;

	if (cpu == 0) {
		core99_l2_cache = _get_L2CR();
		printk("CPU0: L2CR is %lx\n", core99_l2_cache);
	} else {
		printk("CPU%d: L2CR was %lx\n", cpu, _get_L2CR());
		_set_L2CR(0);
		_set_L2CR(core99_l2_cache);
		printk("CPU%d: L2CR set to %lx\n", cpu, core99_l2_cache);
	}

	if (!(cur_cpu_spec[0]->cpu_features & CPU_FTR_L3CR))
		return;

	if (cpu == 0){
		core99_l3_cache = _get_L3CR();
		printk("CPU0: L3CR is %lx\n", core99_l3_cache);
	} else {
		printk("CPU%d: L3CR was %lx\n", cpu, _get_L3CR());
		_set_L3CR(0);
		_set_L3CR(core99_l3_cache);
		printk("CPU%d: L3CR set to %lx\n", cpu, core99_l3_cache);
	}
}

/*
 * Set and clear IPIs for powersurge.
 */
static inline void psurge_set_ipi(int cpu)
{
	if (psurge_type == PSURGE_NONE)
		return;
	if (cpu == 0)
		in_be32(psurge_pri_intr);
	else if (psurge_type == PSURGE_DUAL)
		out_8(psurge_sec_intr, 0);
	else
		PSURGE_QUAD_OUT(PSURGE_QUAD_IRQ_SET, 1 << cpu);
}

static inline void psurge_clr_ipi(int cpu)
{
	if (cpu > 0) {
		switch(psurge_type) {
		case PSURGE_DUAL:
			out_8(psurge_sec_intr, ~0);
		case PSURGE_NONE:
			break;
		default:
			PSURGE_QUAD_OUT(PSURGE_QUAD_IRQ_CLR, 1 << cpu);
		}
	}
}

/*
 * On powersurge (old SMP powermac architecture) we don't have
 * separate IPIs for separate messages like openpic does.  Instead
 * we have a bitmap for each processor, where a 1 bit means that
 * the corresponding message is pending for that processor.
 * Ideally each cpu's entry would be in a different cache line.
 *  -- paulus.
 */
static unsigned long psurge_smp_message[NR_CPUS];

void __pmac psurge_smp_message_recv(struct pt_regs *regs)
{
	int cpu = smp_processor_id();
	int msg;

	/* clear interrupt */
	psurge_clr_ipi(cpu);

	if (num_online_cpus() < 2)
		return;

	/* make sure there is a message there */
	for (msg = 0; msg < 4; msg++)
		if (test_and_clear_bit(msg, &psurge_smp_message[cpu]))
			smp_message_recv(msg, regs);
}

irqreturn_t __pmac psurge_primary_intr(int irq, void *d, struct pt_regs *regs)
{
	psurge_smp_message_recv(regs);
	return IRQ_HANDLED;
}

static void __pmac smp_psurge_message_pass(int target, int msg, unsigned long data,
					   int wait)
{
	int i;

	if (num_online_cpus() < 2)
		return;

	for (i = 0; i < NR_CPUS; i++) {
		if (!cpu_online(i))
			continue;
		if (target == MSG_ALL
		    || (target == MSG_ALL_BUT_SELF && i != smp_processor_id())
		    || target == i) {
			set_bit(msg, &psurge_smp_message[i]);
			psurge_set_ipi(i);
		}
	}
}

/*
 * Determine a quad card presence. We read the board ID register, we
 * force the data bus to change to something else, and we read it again.
 * It it's stable, then the register probably exist (ugh !)
 */
static int __init psurge_quad_probe(void)
{
	int type;
	unsigned int i;

	type = PSURGE_QUAD_IN(PSURGE_QUAD_BOARD_ID);
	if (type < PSURGE_QUAD_OKEE || type > PSURGE_QUAD_ICEGRASS
	    || type != PSURGE_QUAD_IN(PSURGE_QUAD_BOARD_ID))
		return PSURGE_DUAL;

	/* looks OK, try a slightly more rigorous test */
	/* bogus is not necessarily cacheline-aligned,
	   though I don't suppose that really matters.  -- paulus */
	for (i = 0; i < 100; i++) {
		volatile u32 bogus[8];
		bogus[(0+i)%8] = 0x00000000;
		bogus[(1+i)%8] = 0x55555555;
		bogus[(2+i)%8] = 0xFFFFFFFF;
		bogus[(3+i)%8] = 0xAAAAAAAA;
		bogus[(4+i)%8] = 0x33333333;
		bogus[(5+i)%8] = 0xCCCCCCCC;
		bogus[(6+i)%8] = 0xCCCCCCCC;
		bogus[(7+i)%8] = 0x33333333;
		wmb();
		asm volatile("dcbf 0,%0" : : "r" (bogus) : "memory");
		mb();
		if (type != PSURGE_QUAD_IN(PSURGE_QUAD_BOARD_ID))
			return PSURGE_DUAL;
	}
	return type;
}

static void __init psurge_quad_init(void)
{
	int procbits;

	if (ppc_md.progress) ppc_md.progress("psurge_quad_init", 0x351);
	procbits = ~PSURGE_QUAD_IN(PSURGE_QUAD_WHICH_CPU);
	if (psurge_type == PSURGE_QUAD_ICEGRASS)
		PSURGE_QUAD_BIS(PSURGE_QUAD_RESET_CTL, procbits);
	else
		PSURGE_QUAD_BIC(PSURGE_QUAD_CKSTOP_CTL, procbits);
	mdelay(33);
	out_8(psurge_sec_intr, ~0);
	PSURGE_QUAD_OUT(PSURGE_QUAD_IRQ_CLR, procbits);
	PSURGE_QUAD_BIS(PSURGE_QUAD_RESET_CTL, procbits);
	if (psurge_type != PSURGE_QUAD_ICEGRASS)
		PSURGE_QUAD_BIS(PSURGE_QUAD_CKSTOP_CTL, procbits);
	PSURGE_QUAD_BIC(PSURGE_QUAD_PRIMARY_ARB, procbits);
	mdelay(33);
	PSURGE_QUAD_BIC(PSURGE_QUAD_RESET_CTL, procbits);
	mdelay(33);
	PSURGE_QUAD_BIS(PSURGE_QUAD_PRIMARY_ARB, procbits);
	mdelay(33);
}

static int __init smp_psurge_probe(void)
{
	int i, ncpus;

	/* We don't do SMP on the PPC601 -- paulus */
	if (PVR_VER(mfspr(PVR)) == 1)
		return 1;

	/*
	 * The powersurge cpu board can be used in the generation
	 * of powermacs that have a socket for an upgradeable cpu card,
	 * including the 7500, 8500, 9500, 9600.
	 * The device tree doesn't tell you if you have 2 cpus because
	 * OF doesn't know anything about the 2nd processor.
	 * Instead we look for magic bits in magic registers,
	 * in the hammerhead memory controller in the case of the
	 * dual-cpu powersurge board.  -- paulus.
	 */
	if (find_devices("hammerhead") == NULL)
		return 1;

	hhead_base = ioremap(HAMMERHEAD_BASE, 0x800);
	quad_base = ioremap(PSURGE_QUAD_REG_ADDR, 1024);
	psurge_sec_intr = hhead_base + HHEAD_SEC_INTR;

	psurge_type = psurge_quad_probe();
	if (psurge_type != PSURGE_DUAL) {
		psurge_quad_init();
		/* All released cards using this HW design have 4 CPUs */
		ncpus = 4;
	} else {
		iounmap((void *) quad_base);
		if ((in_8(hhead_base + HHEAD_CONFIG) & 0x02) == 0) {
			/* not a dual-cpu card */
			iounmap((void *) hhead_base);
			psurge_type = PSURGE_NONE;
			return 1;
		}
		ncpus = 2;
	}

	psurge_start = ioremap(PSURGE_START, 4);
	psurge_pri_intr = ioremap(PSURGE_PRI_INTR, 4);

	/* this is not actually strictly necessary -- paulus. */
	for (i = 1; i < ncpus; ++i)
		smp_hw_index[i] = i;

	if (ppc_md.progress) ppc_md.progress("smp_psurge_probe - done", 0x352);

	return ncpus;
}

static void __init smp_psurge_kick_cpu(int nr)
{
	void (*start)(void) = __secondary_start_psurge;
	unsigned long a;

	/* may need to flush here if secondary bats aren't setup */
	for (a = KERNELBASE; a < KERNELBASE + 0x800000; a += 32)
		asm volatile("dcbf 0,%0" : : "r" (a) : "memory");
	asm volatile("sync");

	if (ppc_md.progress) ppc_md.progress("smp_psurge_kick_cpu", 0x353);

	/* setup entry point of secondary processor */
	switch (nr) {
	case 2:
		start = __secondary_start_psurge2;
		break;
	case 3:
		start = __secondary_start_psurge3;
		break;
	}

	out_be32(psurge_start, __pa(start));
	mb();

	psurge_set_ipi(nr);
	udelay(10);
	psurge_clr_ipi(nr);

	if (ppc_md.progress) ppc_md.progress("smp_psurge_kick_cpu - done", 0x354);
}

/*
 * With the dual-cpu powersurge board, the decrementers and timebases
 * of both cpus are frozen after the secondary cpu is started up,
 * until we give the secondary cpu another interrupt.  This routine
 * uses this to get the timebases synchronized.
 *  -- paulus.
 */
static void __init psurge_dual_sync_tb(int cpu_nr)
{
	int t;

	set_dec(tb_ticks_per_jiffy);
	set_tb(0, 0);
	last_jiffy_stamp(cpu_nr) = 0;

	if (cpu_nr > 0) {
		mb();
		sec_tb_reset = 1;
		return;
	}

	/* wait for the secondary to have reset its TB before proceeding */
	for (t = 10000000; t > 0 && !sec_tb_reset; --t)
		;

	/* now interrupt the secondary, starting both TBs */
	psurge_set_ipi(1);

	smp_tb_synchronized = 1;
}

static void __init smp_psurge_setup_cpu(int cpu_nr)
{

	if (cpu_nr == 0) {
		/* If we failed to start the second CPU, we should still
		 * send it an IPI to start the timebase & DEC or we might
		 * have them stuck.
		 */
		if (num_online_cpus() < 2) {
			if (psurge_type == PSURGE_DUAL)
				psurge_set_ipi(1);
			return;
		}
		/* reset the entry point so if we get another intr we won't
		 * try to startup again */
		out_be32(psurge_start, 0x100);
		if (request_irq(30, psurge_primary_intr, SA_INTERRUPT, "primary IPI", NULL))
			printk(KERN_ERR "Couldn't get primary IPI interrupt");
	}

	if (psurge_type == PSURGE_DUAL)
		psurge_dual_sync_tb(cpu_nr);
}

void __init smp_psurge_take_timebase(void)
{
	/* Dummy implementation */
}

void __init smp_psurge_give_timebase(void)
{
	/* Dummy implementation */
}

static int __init smp_core99_probe(void)
{
#ifdef CONFIG_6xx
	extern int powersave_nap;
#endif
	struct device_node *cpus, *firstcpu;
	int i, ncpus = 0, boot_cpu = -1;
	u32 *tbprop;

	if (ppc_md.progress) ppc_md.progress("smp_core99_probe", 0x345);
	cpus = firstcpu = find_type_devices("cpu");
	while(cpus != NULL) {
		u32 *regprop = (u32 *)get_property(cpus, "reg", NULL);
		char *stateprop = (char *)get_property(cpus, "state", NULL);
		if (regprop != NULL && stateprop != NULL &&
		    !strncmp(stateprop, "running", 7))
			boot_cpu = *regprop;
		++ncpus;
		cpus = cpus->next;
	}
	if (boot_cpu == -1)
		printk(KERN_WARNING "Couldn't detect boot CPU !\n");
	if (boot_cpu != 0)
		printk(KERN_WARNING "Boot CPU is %d, unsupported setup !\n", boot_cpu);

	if (machine_is_compatible("MacRISC4")) {
		extern struct smp_ops_t core99_smp_ops;

		core99_smp_ops.take_timebase = smp_generic_take_timebase;
		core99_smp_ops.give_timebase = smp_generic_give_timebase;
	} else {
		if (firstcpu != NULL)
			tbprop = (u32 *)get_property(firstcpu, "timebase-enable", NULL);
		if (tbprop)
			core99_tb_gpio = *tbprop;
		else
			core99_tb_gpio = KL_GPIO_TB_ENABLE;
	}

	if (ncpus > 1) {
		openpic_request_IPIs();
		for (i = 1; i < ncpus; ++i)
			smp_hw_index[i] = i;
#ifdef CONFIG_6xx
		powersave_nap = 0;
#endif
		core99_init_caches(0);
	}

	return ncpus;
}

static void __init smp_core99_kick_cpu(int nr)
{
	unsigned long save_vector, new_vector;
	unsigned long flags;

	volatile unsigned long *vector
		 = ((volatile unsigned long *)(KERNELBASE+0x100));
	if (nr < 1 || nr > 3)
		return;
	if (ppc_md.progress) ppc_md.progress("smp_core99_kick_cpu", 0x346);

	local_irq_save(flags);
	local_irq_disable();

	/* Save reset vector */
	save_vector = *vector;

	/* Setup fake reset vector that does	
	 *   b __secondary_start_psurge - KERNELBASE
	 */
	switch(nr) {
		case 1:
			new_vector = (unsigned long)__secondary_start_psurge;
			break;
		case 2:
			new_vector = (unsigned long)__secondary_start_psurge2;
			break;
		case 3:
			new_vector = (unsigned long)__secondary_start_psurge3;
			break;
	}
	*vector = 0x48000002 + new_vector - KERNELBASE;

	/* flush data cache and inval instruction cache */
	flush_icache_range((unsigned long) vector, (unsigned long) vector + 4);

	/* Put some life in our friend */
	pmac_call_feature(PMAC_FTR_RESET_CPU, NULL, nr, 0);

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
	/* Setup L2/L3 */
	if (cpu_nr != 0)
		core99_init_caches(cpu_nr);

	/* Setup openpic */
	do_openpic_setup_cpu();

	if (cpu_nr == 0) {
#ifdef CONFIG_POWER4
		extern void g5_phy_disable_cpu1(void);

		/* If we didn't start the second CPU, we must take
		 * it off the bus
		 */
		if (machine_is_compatible("MacRISC4") &&
		    num_online_cpus() < 2)		
			g5_phy_disable_cpu1();
#endif /* CONFIG_POWER4 */
		if (ppc_md.progress) ppc_md.progress("core99_setup_cpu 0 done", 0x349);
	}
}

void __init smp_core99_take_timebase(void)
{
	/* Secondary processor "takes" the timebase by freezing
	 * it, resetting its local TB and telling CPU 0 to go on
	 */
	pmac_call_feature(PMAC_FTR_WRITE_GPIO, NULL, core99_tb_gpio, 4);
	pmac_call_feature(PMAC_FTR_READ_GPIO, NULL, core99_tb_gpio, 0);
	mb();

	set_dec(tb_ticks_per_jiffy);
	set_tb(0, 0);
	last_jiffy_stamp(smp_processor_id()) = 0;

	mb();
       	sec_tb_reset = 1;
}

void __init smp_core99_give_timebase(void)
{
	unsigned int t;

	/* Primary processor waits for secondary to have frozen
	 * the timebase, resets local TB, and kick timebase again
	 */
	/* wait for the secondary to have reset its TB before proceeding */
	for (t = 1000; t > 0 && !sec_tb_reset; --t)
		udelay(1000);
	if (t == 0)
		printk(KERN_WARNING "Timeout waiting sync on second CPU\n");

       	set_dec(tb_ticks_per_jiffy);
	set_tb(0, 0);
	last_jiffy_stamp(smp_processor_id()) = 0;
	mb();

	/* Now, restart the timebase by leaving the GPIO to an open collector */
       	pmac_call_feature(PMAC_FTR_WRITE_GPIO, NULL, core99_tb_gpio, 0);
        pmac_call_feature(PMAC_FTR_READ_GPIO, NULL, core99_tb_gpio, 0);

	smp_tb_synchronized = 1;
}


/* PowerSurge-style Macs */
struct smp_ops_t psurge_smp_ops __pmacdata = {
	.message_pass	= smp_psurge_message_pass,
	.probe		= smp_psurge_probe,
	.kick_cpu	= smp_psurge_kick_cpu,
	.setup_cpu	= smp_psurge_setup_cpu,
	.give_timebase	= smp_psurge_give_timebase,
	.take_timebase	= smp_psurge_take_timebase,
};

/* Core99 Macs (dual G4s) */
struct smp_ops_t core99_smp_ops __pmacdata = {
	.message_pass	= smp_openpic_message_pass,
	.probe		= smp_core99_probe,
	.kick_cpu	= smp_core99_kick_cpu,
	.setup_cpu	= smp_core99_setup_cpu,
	.give_timebase	= smp_core99_give_timebase,
	.take_timebase	= smp_core99_take_timebase,
};
