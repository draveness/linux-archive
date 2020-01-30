/*
 *  This program is free software; you can distribute it and/or modify it
 *  under the terms of the GNU General Public License (Version 2) as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Copyright (C) 2004, 05, 06 MIPS Technologies, Inc.
 *    Elizabeth Clarke (beth@mips.com)
 *    Ralf Baechle (ralf@linux-mips.org)
 * Copyright (C) 2006 Ralf Baechle (ralf@linux-mips.org)
 */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/compiler.h>

#include <asm/atomic.h>
#include <asm/cacheflush.h>
#include <asm/cpu.h>
#include <asm/processor.h>
#include <asm/system.h>
#include <asm/hardirq.h>
#include <asm/mmu_context.h>
#include <asm/smp.h>
#include <asm/time.h>
#include <asm/mipsregs.h>
#include <asm/mipsmtregs.h>
#include <asm/mips_mt.h>

#define MIPS_CPU_IPI_RESCHED_IRQ 0
#define MIPS_CPU_IPI_CALL_IRQ 1

static int cpu_ipi_resched_irq, cpu_ipi_call_irq;

#if 0
static void dump_mtregisters(int vpe, int tc)
{
	printk("vpe %d tc %d\n", vpe, tc);

	settc(tc);

	printk("  c0 status  0x%lx\n", read_vpe_c0_status());
	printk("  vpecontrol 0x%lx\n", read_vpe_c0_vpecontrol());
	printk("  vpeconf0    0x%lx\n", read_vpe_c0_vpeconf0());
	printk("  tcstatus 0x%lx\n", read_tc_c0_tcstatus());
	printk("  tcrestart 0x%lx\n", read_tc_c0_tcrestart());
	printk("  tcbind 0x%lx\n", read_tc_c0_tcbind());
	printk("  tchalt 0x%lx\n", read_tc_c0_tchalt());
}
#endif

void __init sanitize_tlb_entries(void)
{
	int i, tlbsiz;
	unsigned long mvpconf0, ncpu;

	if (!cpu_has_mipsmt)
		return;

	/* Enable VPC */
	set_c0_mvpcontrol(MVPCONTROL_VPC);

	back_to_back_c0_hazard();

	/* Disable TLB sharing */
	clear_c0_mvpcontrol(MVPCONTROL_STLB);

	mvpconf0 = read_c0_mvpconf0();

	printk(KERN_INFO "MVPConf0 0x%lx TLBS %lx PTLBE %ld\n", mvpconf0,
		   (mvpconf0 & MVPCONF0_TLBS) >> MVPCONF0_TLBS_SHIFT,
			   (mvpconf0 & MVPCONF0_PTLBE) >> MVPCONF0_PTLBE_SHIFT);

	tlbsiz = (mvpconf0 & MVPCONF0_PTLBE) >> MVPCONF0_PTLBE_SHIFT;
	ncpu = ((mvpconf0 & MVPCONF0_PVPE) >> MVPCONF0_PVPE_SHIFT) + 1;

	printk(" tlbsiz %d ncpu %ld\n", tlbsiz, ncpu);

	if (tlbsiz > 0) {
		/* share them out across the vpe's */
		tlbsiz /= ncpu;

		printk(KERN_INFO "setting Config1.MMU_size to %d\n", tlbsiz);

		for (i = 0; i < ncpu; i++) {
			settc(i);

			if (i == 0)
				write_c0_config1((read_c0_config1() & ~(0x3f << 25)) | (tlbsiz << 25));
			else
				write_vpe_c0_config1((read_vpe_c0_config1() & ~(0x3f << 25)) |
						   (tlbsiz << 25));
		}
	}

	clear_c0_mvpcontrol(MVPCONTROL_VPC);
}

static void ipi_resched_dispatch(void)
{
	do_IRQ(MIPS_CPU_IRQ_BASE + MIPS_CPU_IPI_RESCHED_IRQ);
}

static void ipi_call_dispatch(void)
{
	do_IRQ(MIPS_CPU_IRQ_BASE + MIPS_CPU_IPI_CALL_IRQ);
}

static irqreturn_t ipi_resched_interrupt(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

static irqreturn_t ipi_call_interrupt(int irq, void *dev_id)
{
	smp_call_function_interrupt();

	return IRQ_HANDLED;
}

static struct irqaction irq_resched = {
	.handler	= ipi_resched_interrupt,
	.flags		= IRQF_DISABLED|IRQF_PERCPU,
	.name		= "IPI_resched"
};

static struct irqaction irq_call = {
	.handler	= ipi_call_interrupt,
	.flags		= IRQF_DISABLED|IRQF_PERCPU,
	.name		= "IPI_call"
};

static void __init smp_copy_vpe_config(void)
{
	write_vpe_c0_status(
		(read_c0_status() & ~(ST0_IM | ST0_IE | ST0_KSU)) | ST0_CU0);

	/* set config to be the same as vpe0, particularly kseg0 coherency alg */
	write_vpe_c0_config( read_c0_config());

	/* make sure there are no software interrupts pending */
	write_vpe_c0_cause(0);

	/* Propagate Config7 */
	write_vpe_c0_config7(read_c0_config7());

	write_vpe_c0_count(read_c0_count());
}

static unsigned int __init smp_vpe_init(unsigned int tc, unsigned int mvpconf0,
	unsigned int ncpu)
{
	if (tc > ((mvpconf0 & MVPCONF0_PVPE) >> MVPCONF0_PVPE_SHIFT))
		return ncpu;

	/* Deactivate all but VPE 0 */
	if (tc != 0) {
		unsigned long tmp = read_vpe_c0_vpeconf0();

		tmp &= ~VPECONF0_VPA;

		/* master VPE */
		tmp |= VPECONF0_MVP;
		write_vpe_c0_vpeconf0(tmp);

		/* Record this as available CPU */
		cpu_set(tc, phys_cpu_present_map);
		__cpu_number_map[tc]	= ++ncpu;
		__cpu_logical_map[ncpu]	= tc;
	}

	/* Disable multi-threading with TC's */
	write_vpe_c0_vpecontrol(read_vpe_c0_vpecontrol() & ~VPECONTROL_TE);

	if (tc != 0)
		smp_copy_vpe_config();

	return ncpu;
}

static void __init smp_tc_init(unsigned int tc, unsigned int mvpconf0)
{
	unsigned long tmp;

	if (!tc)
		return;

	/* bind a TC to each VPE, May as well put all excess TC's
	   on the last VPE */
	if (tc >= (((mvpconf0 & MVPCONF0_PVPE) >> MVPCONF0_PVPE_SHIFT)+1))
		write_tc_c0_tcbind(read_tc_c0_tcbind() | ((mvpconf0 & MVPCONF0_PVPE) >> MVPCONF0_PVPE_SHIFT));
	else {
		write_tc_c0_tcbind(read_tc_c0_tcbind() | tc);

		/* and set XTC */
		write_vpe_c0_vpeconf0(read_vpe_c0_vpeconf0() | (tc << VPECONF0_XTC_SHIFT));
	}

	tmp = read_tc_c0_tcstatus();

	/* mark not allocated and not dynamically allocatable */
	tmp &= ~(TCSTATUS_A | TCSTATUS_DA);
	tmp |= TCSTATUS_IXMT;		/* interrupt exempt */
	write_tc_c0_tcstatus(tmp);

	write_tc_c0_tchalt(TCHALT_H);
}

/*
 * Common setup before any secondaries are started
 * Make sure all CPU's are in a sensible state before we boot any of the
 * secondarys
 */
void __init plat_smp_setup(void)
{
	unsigned int mvpconf0, ntc, tc, ncpu = 0;

#ifdef CONFIG_MIPS_MT_FPAFF
	/* If we have an FPU, enroll ourselves in the FPU-full mask */
	if (cpu_has_fpu)
		cpu_set(0, mt_fpu_cpumask);
#endif /* CONFIG_MIPS_MT_FPAFF */
	if (!cpu_has_mipsmt)
		return;

	/* disable MT so we can configure */
	dvpe();
	dmt();

	/* Put MVPE's into 'configuration state' */
	set_c0_mvpcontrol(MVPCONTROL_VPC);

	mvpconf0 = read_c0_mvpconf0();
	ntc = (mvpconf0 & MVPCONF0_PTC) >> MVPCONF0_PTC_SHIFT;

	/* we'll always have more TC's than VPE's, so loop setting everything
	   to a sensible state */
	for (tc = 0; tc <= ntc; tc++) {
		settc(tc);

		smp_tc_init(tc, mvpconf0);
		ncpu = smp_vpe_init(tc, mvpconf0, ncpu);
	}

	/* Release config state */
	clear_c0_mvpcontrol(MVPCONTROL_VPC);

	/* We'll wait until starting the secondaries before starting MVPE */

	printk(KERN_INFO "Detected %i available secondary CPU(s)\n", ncpu);
}

void __init plat_prepare_cpus(unsigned int max_cpus)
{
	mips_mt_set_cpuoptions();

	/* set up ipi interrupts */
	if (cpu_has_vint) {
		set_vi_handler(MIPS_CPU_IPI_RESCHED_IRQ, ipi_resched_dispatch);
		set_vi_handler(MIPS_CPU_IPI_CALL_IRQ, ipi_call_dispatch);
	}

	cpu_ipi_resched_irq = MIPS_CPU_IRQ_BASE + MIPS_CPU_IPI_RESCHED_IRQ;
	cpu_ipi_call_irq = MIPS_CPU_IRQ_BASE + MIPS_CPU_IPI_CALL_IRQ;

	setup_irq(cpu_ipi_resched_irq, &irq_resched);
	setup_irq(cpu_ipi_call_irq, &irq_call);

	set_irq_handler(cpu_ipi_resched_irq, handle_percpu_irq);
	set_irq_handler(cpu_ipi_call_irq, handle_percpu_irq);
}

/*
 * Setup the PC, SP, and GP of a secondary processor and start it
 * running!
 * smp_bootstrap is the place to resume from
 * __KSTK_TOS(idle) is apparently the stack pointer
 * (unsigned long)idle->thread_info the gp
 * assumes a 1:1 mapping of TC => VPE
 */
void __cpuinit prom_boot_secondary(int cpu, struct task_struct *idle)
{
	struct thread_info *gp = task_thread_info(idle);
	dvpe();
	set_c0_mvpcontrol(MVPCONTROL_VPC);

	settc(cpu);

	/* restart */
	write_tc_c0_tcrestart((unsigned long)&smp_bootstrap);

	/* enable the tc this vpe/cpu will be running */
	write_tc_c0_tcstatus((read_tc_c0_tcstatus() & ~TCSTATUS_IXMT) | TCSTATUS_A);

	write_tc_c0_tchalt(0);

	/* enable the VPE */
	write_vpe_c0_vpeconf0(read_vpe_c0_vpeconf0() | VPECONF0_VPA);

	/* stack pointer */
	write_tc_gpr_sp( __KSTK_TOS(idle));

	/* global pointer */
	write_tc_gpr_gp((unsigned long)gp);

	flush_icache_range((unsigned long)gp,
	                   (unsigned long)(gp + sizeof(struct thread_info)));

	/* finally out of configuration and into chaos */
	clear_c0_mvpcontrol(MVPCONTROL_VPC);

	evpe(EVPE_ENABLE);
}

void __cpuinit prom_init_secondary(void)
{
	/* Enable per-cpu interrupts */

	/* This is Malta specific: IPI,performance and timer inetrrupts */
	write_c0_status((read_c0_status() & ~ST0_IM ) |
	                (STATUSF_IP0 | STATUSF_IP1 | STATUSF_IP6 | STATUSF_IP7));
}

void __cpuinit prom_smp_finish(void)
{
	write_c0_compare(read_c0_count() + (8* mips_hpt_frequency/HZ));

#ifdef CONFIG_MIPS_MT_FPAFF
	/* If we have an FPU, enroll ourselves in the FPU-full mask */
	if (cpu_has_fpu)
		cpu_set(smp_processor_id(), mt_fpu_cpumask);
#endif /* CONFIG_MIPS_MT_FPAFF */

	local_irq_enable();
}

void prom_cpus_done(void)
{
}

void core_send_ipi(int cpu, unsigned int action)
{
	int i;
	unsigned long flags;
	int vpflags;

	local_irq_save (flags);

	vpflags = dvpe();	/* cant access the other CPU's registers whilst MVPE enabled */

	switch (action) {
	case SMP_CALL_FUNCTION:
		i = C_SW1;
		break;

	case SMP_RESCHEDULE_YOURSELF:
	default:
		i = C_SW0;
		break;
	}

	/* 1:1 mapping of vpe and tc... */
	settc(cpu);
	write_vpe_c0_cause(read_vpe_c0_cause() | i);
	evpe(vpflags);

	local_irq_restore(flags);
}
