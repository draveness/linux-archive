/*
 *	x86 SMP booting functions
 *
 *	(c) 1995 Alan Cox, Building #3 <alan@redhat.com>
 *	(c) 1998, 1999, 2000 Ingo Molnar <mingo@redhat.com>
 *
 *	Much of the core SMP work is based on previous work by Thomas Radke, to
 *	whom a great many thanks are extended.
 *
 *	Thanks to Intel for making available several different Pentium,
 *	Pentium Pro and Pentium-II/Xeon MP machines.
 *	Original development of Linux SMP code supported by Caldera.
 *
 *	This code is released under the GNU General Public License version 2 or
 *	later.
 *
 *	Fixes
 *		Felix Koop	:	NR_CPUS used properly
 *		Jose Renau	:	Handle single CPU case.
 *		Alan Cox	:	By repeated request 8) - Total BogoMIP report.
 *		Greg Wright	:	Fix for kernel stacks panic.
 *		Erich Boleyn	:	MP v1.4 and additional changes.
 *	Matthias Sattler	:	Changes for 2.1 kernel map.
 *	Michel Lespinasse	:	Changes for 2.1 kernel map.
 *	Michael Chastain	:	Change trampoline.S to gnu as.
 *		Alan Cox	:	Dumb bug: 'B' step PPro's are fine
 *		Ingo Molnar	:	Added APIC timers, based on code
 *					from Jose Renau
 *		Ingo Molnar	:	various cleanups and rewrites
 *		Tigran Aivazian	:	fixed "0.00 in /proc/uptime on SMP" bug.
 *	Maciej W. Rozycki	:	Bits for genuine 82489DX APICs
 *		Martin J. Bligh	: 	Added support for multi-quad systems
 *		Dave Jones	:	Report invalid combinations of Athlon CPUs.
*		Rusty Russell	:	Hacked into shape for new "hotplug" boot process. */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/kernel.h>

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/smp_lock.h>
#include <linux/irq.h>
#include <linux/bootmem.h>

#include <linux/delay.h>
#include <linux/mc146818rtc.h>
#include <asm/tlbflush.h>
#include <asm/desc.h>
#include <asm/arch_hooks.h>

#include <mach_apic.h>
#include <mach_wakecpu.h>
#include <smpboot_hooks.h>

/* Set if we find a B stepping CPU */
static int __initdata smp_b_stepping;

/* Number of siblings per CPU package */
int smp_num_siblings = 1;
int phys_proc_id[NR_CPUS]; /* Package ID of each logical CPU */

/* bitmap of online cpus */
cpumask_t cpu_online_map;

static cpumask_t cpu_callin_map;
cpumask_t cpu_callout_map;
static cpumask_t smp_commenced_mask;

/* Per CPU bogomips and other parameters */
struct cpuinfo_x86 cpu_data[NR_CPUS] __cacheline_aligned;

/* Set when the idlers are all forked */
int smp_threads_ready;

/*
 * Trampoline 80x86 program as an array.
 */

extern unsigned char trampoline_data [];
extern unsigned char trampoline_end  [];
static unsigned char *trampoline_base;
static int trampoline_exec;

/*
 * Currently trivial. Write the real->protected mode
 * bootstrap into the page concerned. The caller
 * has made sure it's suitably aligned.
 */

static unsigned long __init setup_trampoline(void)
{
	memcpy(trampoline_base, trampoline_data, trampoline_end - trampoline_data);
	return virt_to_phys(trampoline_base);
}

/*
 * We are called very early to get the low memory for the
 * SMP bootup trampoline page.
 */
void __init smp_alloc_memory(void)
{
	trampoline_base = (void *) alloc_bootmem_low_pages(PAGE_SIZE);
	/*
	 * Has to be in very low memory so we can execute
	 * real-mode AP code.
	 */
	if (__pa(trampoline_base) >= 0x9F000)
		BUG();
	/*
	 * Make the SMP trampoline executable:
	 */
	trampoline_exec = set_kernel_exec((unsigned long)trampoline_base, 1);
}

/*
 * The bootstrap kernel entry code has set these up. Save them for
 * a given CPU
 */

static void __init smp_store_cpu_info(int id)
{
	struct cpuinfo_x86 *c = cpu_data + id;

	*c = boot_cpu_data;
	if (id!=0)
		identify_cpu(c);
	/*
	 * Mask B, Pentium, but not Pentium MMX
	 */
	if (c->x86_vendor == X86_VENDOR_INTEL &&
	    c->x86 == 5 &&
	    c->x86_mask >= 1 && c->x86_mask <= 4 &&
	    c->x86_model <= 3)
		/*
		 * Remember we have B step Pentia with bugs
		 */
		smp_b_stepping = 1;

	/*
	 * Certain Athlons might work (for various values of 'work') in SMP
	 * but they are not certified as MP capable.
	 */
	if ((c->x86_vendor == X86_VENDOR_AMD) && (c->x86 == 6)) {

		/* Athlon 660/661 is valid. */	
		if ((c->x86_model==6) && ((c->x86_mask==0) || (c->x86_mask==1)))
			goto valid_k7;

		/* Duron 670 is valid */
		if ((c->x86_model==7) && (c->x86_mask==0))
			goto valid_k7;

		/*
		 * Athlon 662, Duron 671, and Athlon >model 7 have capability bit.
		 * It's worth noting that the A5 stepping (662) of some Athlon XP's
		 * have the MP bit set.
		 * See http://www.heise.de/newsticker/data/jow-18.10.01-000 for more.
		 */
		if (((c->x86_model==6) && (c->x86_mask>=2)) ||
		    ((c->x86_model==7) && (c->x86_mask>=1)) ||
		     (c->x86_model> 7))
			if (cpu_has_mp)
				goto valid_k7;

		/* If we get here, it's not a certified SMP capable AMD system. */
		tainted |= TAINT_UNSAFE_SMP;
	}

valid_k7:
	;
}

/*
 * TSC synchronization.
 *
 * We first check whether all CPUs have their TSC's synchronized,
 * then we print a warning if not, and always resync.
 */

static atomic_t tsc_start_flag = ATOMIC_INIT(0);
static atomic_t tsc_count_start = ATOMIC_INIT(0);
static atomic_t tsc_count_stop = ATOMIC_INIT(0);
static unsigned long long tsc_values[NR_CPUS];

#define NR_LOOPS 5

/*
 * accurate 64-bit/32-bit division, expanded to 32-bit divisions and 64-bit
 * multiplication. Not terribly optimized but we need it at boot time only
 * anyway.
 *
 * result == a / b
 *	== (a1 + a2*(2^32)) / b
 *	== a1/b + a2*(2^32/b)
 *	== a1/b + a2*((2^32-1)/b) + a2/b + (a2*((2^32-1) % b))/b
 *		    ^---- (this multiplication can overflow)
 */

static unsigned long long __init div64 (unsigned long long a, unsigned long b0)
{
	unsigned int a1, a2;
	unsigned long long res;

	a1 = ((unsigned int*)&a)[0];
	a2 = ((unsigned int*)&a)[1];

	res = a1/b0 +
		(unsigned long long)a2 * (unsigned long long)(0xffffffff/b0) +
		a2 / b0 +
		(a2 * (0xffffffff % b0)) / b0;

	return res;
}

static void __init synchronize_tsc_bp (void)
{
	int i;
	unsigned long long t0;
	unsigned long long sum, avg;
	long long delta;
	unsigned long one_usec;
	int buggy = 0;

	printk("checking TSC synchronization across %u CPUs: ", num_booting_cpus());

	/* convert from kcyc/sec to cyc/usec */
	one_usec = cpu_khz / 1000;

	atomic_set(&tsc_start_flag, 1);
	wmb();

	/*
	 * We loop a few times to get a primed instruction cache,
	 * then the last pass is more or less synchronized and
	 * the BP and APs set their cycle counters to zero all at
	 * once. This reduces the chance of having random offsets
	 * between the processors, and guarantees that the maximum
	 * delay between the cycle counters is never bigger than
	 * the latency of information-passing (cachelines) between
	 * two CPUs.
	 */
	for (i = 0; i < NR_LOOPS; i++) {
		/*
		 * all APs synchronize but they loop on '== num_cpus'
		 */
		while (atomic_read(&tsc_count_start) != num_booting_cpus()-1)
			mb();
		atomic_set(&tsc_count_stop, 0);
		wmb();
		/*
		 * this lets the APs save their current TSC:
		 */
		atomic_inc(&tsc_count_start);

		rdtscll(tsc_values[smp_processor_id()]);
		/*
		 * We clear the TSC in the last loop:
		 */
		if (i == NR_LOOPS-1)
			write_tsc(0, 0);

		/*
		 * Wait for all APs to leave the synchronization point:
		 */
		while (atomic_read(&tsc_count_stop) != num_booting_cpus()-1)
			mb();
		atomic_set(&tsc_count_start, 0);
		wmb();
		atomic_inc(&tsc_count_stop);
	}

	sum = 0;
	for (i = 0; i < NR_CPUS; i++) {
		if (cpu_isset(i, cpu_callout_map)) {
			t0 = tsc_values[i];
			sum += t0;
		}
	}
	avg = div64(sum, num_booting_cpus());

	sum = 0;
	for (i = 0; i < NR_CPUS; i++) {
		if (!cpu_isset(i, cpu_callout_map))
			continue;
		delta = tsc_values[i] - avg;
		if (delta < 0)
			delta = -delta;
		/*
		 * We report bigger than 2 microseconds clock differences.
		 */
		if (delta > 2*one_usec) {
			long realdelta;
			if (!buggy) {
				buggy = 1;
				printk("\n");
			}
			realdelta = div64(delta, one_usec);
			if (tsc_values[i] < avg)
				realdelta = -realdelta;

			printk("BIOS BUG: CPU#%d improperly initialized, has %ld usecs TSC skew! FIXED.\n", i, realdelta);
		}

		sum += delta;
	}
	if (!buggy)
		printk("passed.\n");
		;
}

static void __init synchronize_tsc_ap (void)
{
	int i;

	/*
	 * Not every cpu is online at the time
	 * this gets called, so we first wait for the BP to
	 * finish SMP initialization:
	 */
	while (!atomic_read(&tsc_start_flag)) mb();

	for (i = 0; i < NR_LOOPS; i++) {
		atomic_inc(&tsc_count_start);
		while (atomic_read(&tsc_count_start) != num_booting_cpus())
			mb();

		rdtscll(tsc_values[smp_processor_id()]);
		if (i == NR_LOOPS-1)
			write_tsc(0, 0);

		atomic_inc(&tsc_count_stop);
		while (atomic_read(&tsc_count_stop) != num_booting_cpus()) mb();
	}
}
#undef NR_LOOPS

extern void calibrate_delay(void);

static atomic_t init_deasserted;

void __init smp_callin(void)
{
	int cpuid, phys_id;
	unsigned long timeout;

	/*
	 * If waken up by an INIT in an 82489DX configuration
	 * we may get here before an INIT-deassert IPI reaches
	 * our local APIC.  We have to wait for the IPI or we'll
	 * lock up on an APIC access.
	 */
	wait_for_init_deassert(&init_deasserted);

	/*
	 * (This works even if the APIC is not enabled.)
	 */
	phys_id = GET_APIC_ID(apic_read(APIC_ID));
	cpuid = smp_processor_id();
	if (cpu_isset(cpuid, cpu_callin_map)) {
		printk("huh, phys CPU#%d, CPU#%d already present??\n",
					phys_id, cpuid);
		BUG();
	}
	Dprintk("CPU#%d (phys ID: %d) waiting for CALLOUT\n", cpuid, phys_id);

	/*
	 * STARTUP IPIs are fragile beasts as they might sometimes
	 * trigger some glue motherboard logic. Complete APIC bus
	 * silence for 1 second, this overestimates the time the
	 * boot CPU is spending to send the up to 2 STARTUP IPIs
	 * by a factor of two. This should be enough.
	 */

	/*
	 * Waiting 2s total for startup (udelay is not yet working)
	 */
	timeout = jiffies + 2*HZ;
	while (time_before(jiffies, timeout)) {
		/*
		 * Has the boot CPU finished it's STARTUP sequence?
		 */
		if (cpu_isset(cpuid, cpu_callout_map))
			break;
		rep_nop();
	}

	if (!time_before(jiffies, timeout)) {
		printk("BUG: CPU%d started up but did not get a callout!\n",
			cpuid);
		BUG();
	}

	/*
	 * the boot CPU has finished the init stage and is spinning
	 * on callin_map until we finish. We are free to set up this
	 * CPU, first the APIC. (this is probably redundant on most
	 * boards)
	 */

	Dprintk("CALLIN, before setup_local_APIC().\n");
	smp_callin_clear_local_apic();
	setup_local_APIC();
	map_cpu_to_logical_apicid();

	local_irq_enable();

	/*
	 * Get our bogomips.
	 */
	calibrate_delay();
	Dprintk("Stack at about %p\n",&cpuid);

	/*
	 * Save our processor parameters
	 */
 	smp_store_cpu_info(cpuid);

	disable_APIC_timer();
	local_irq_disable();
	/*
	 * Allow the master to continue.
	 */
	cpu_set(cpuid, cpu_callin_map);

	/*
	 *      Synchronize the TSC with the BP
	 */
	if (cpu_has_tsc && cpu_khz)
		synchronize_tsc_ap();
}

int cpucount;

extern int cpu_idle(void);

/*
 * Activate a secondary processor.
 */
int __init start_secondary(void *unused)
{
	/*
	 * Dont put anything before smp_callin(), SMP
	 * booting is too fragile that we want to limit the
	 * things done here to the most necessary things.
	 */
	cpu_init();
	smp_callin();
	while (!cpu_isset(smp_processor_id(), smp_commenced_mask))
		rep_nop();
	setup_secondary_APIC_clock();
	if (nmi_watchdog == NMI_IO_APIC) {
		disable_8259A_irq(0);
		enable_NMI_through_LVT0(NULL);
		enable_8259A_irq(0);
	}
	enable_APIC_timer();
	/*
	 * low-memory mappings have been cleared, flush them from
	 * the local TLBs too.
	 */
	local_flush_tlb();
	cpu_set(smp_processor_id(), cpu_online_map);
	wmb();
	return cpu_idle();
}

/*
 * Everything has been set up for the secondary
 * CPUs - they just need to reload everything
 * from the task structure
 * This function must not return.
 */
void __init initialize_secondary(void)
{
	/*
	 * We don't actually need to load the full TSS,
	 * basically just the stack pointer and the eip.
	 */

	asm volatile(
		"movl %0,%%esp\n\t"
		"jmp *%1"
		:
		:"r" (current->thread.esp),"r" (current->thread.eip));
}

extern struct {
	void * esp;
	unsigned short ss;
} stack_start;

static struct task_struct * __init fork_by_hand(void)
{
	struct pt_regs regs;
	/*
	 * don't care about the eip and regs settings since
	 * we'll never reschedule the forked task.
	 */
	return copy_process(CLONE_VM|CLONE_IDLETASK, 0, &regs, 0, NULL, NULL);
}

#ifdef CONFIG_NUMA

/* which logical CPUs are on which nodes */
cpumask_t node_2_cpu_mask[MAX_NUMNODES] =
				{ [0 ... MAX_NUMNODES-1] = CPU_MASK_NONE };
/* which node each logical CPU is on */
int cpu_2_node[NR_CPUS] = { [0 ... NR_CPUS-1] = 0 };
EXPORT_SYMBOL(cpu_2_node);

/* set up a mapping between cpu and node. */
static inline void map_cpu_to_node(int cpu, int node)
{
	printk("Mapping cpu %d to node %d\n", cpu, node);
	cpu_set(cpu, node_2_cpu_mask[node]);
	cpu_2_node[cpu] = node;
}

/* undo a mapping between cpu and node. */
static inline void unmap_cpu_to_node(int cpu)
{
	int node;

	printk("Unmapping cpu %d from all nodes\n", cpu);
	for (node = 0; node < MAX_NUMNODES; node ++)
		cpu_clear(cpu, node_2_cpu_mask[node]);
	cpu_2_node[cpu] = 0;
}
#else /* !CONFIG_NUMA */

#define map_cpu_to_node(cpu, node)	({})
#define unmap_cpu_to_node(cpu)	({})

#endif /* CONFIG_NUMA */

u8 cpu_2_logical_apicid[NR_CPUS] = { [0 ... NR_CPUS-1] = BAD_APICID };

void map_cpu_to_logical_apicid(void)
{
	int cpu = smp_processor_id();
	int apicid = logical_smp_processor_id();

	cpu_2_logical_apicid[cpu] = apicid;
	map_cpu_to_node(cpu, apicid_to_node(apicid));
}

void unmap_cpu_to_logical_apicid(int cpu)
{
	cpu_2_logical_apicid[cpu] = BAD_APICID;
	unmap_cpu_to_node(cpu);
}

#if APIC_DEBUG
static inline void __inquire_remote_apic(int apicid)
{
	int i, regs[] = { APIC_ID >> 4, APIC_LVR >> 4, APIC_SPIV >> 4 };
	char *names[] = { "ID", "VERSION", "SPIV" };
	int timeout, status;

	printk("Inquiring remote APIC #%d...\n", apicid);

	for (i = 0; i < sizeof(regs) / sizeof(*regs); i++) {
		printk("... APIC #%d %s: ", apicid, names[i]);

		/*
		 * Wait for idle.
		 */
		apic_wait_icr_idle();

		apic_write_around(APIC_ICR2, SET_APIC_DEST_FIELD(apicid));
		apic_write_around(APIC_ICR, APIC_DM_REMRD | regs[i]);

		timeout = 0;
		do {
			udelay(100);
			status = apic_read(APIC_ICR) & APIC_ICR_RR_MASK;
		} while (status == APIC_ICR_RR_INPROG && timeout++ < 1000);

		switch (status) {
		case APIC_ICR_RR_VALID:
			status = apic_read(APIC_RRR);
			printk("%08x\n", status);
			break;
		default:
			printk("failed\n");
		}
	}
}
#endif

#ifdef WAKE_SECONDARY_VIA_NMI
/* 
 * Poke the other CPU in the eye via NMI to wake it up. Remember that the normal
 * INIT, INIT, STARTUP sequence will reset the chip hard for us, and this
 * won't ... remember to clear down the APIC, etc later.
 */
static int __init
wakeup_secondary_cpu(int logical_apicid, unsigned long start_eip)
{
	unsigned long send_status = 0, accept_status = 0;
	int timeout, maxlvt;

	/* Target chip */
	apic_write_around(APIC_ICR2, SET_APIC_DEST_FIELD(logical_apicid));

	/* Boot on the stack */
	/* Kick the second */
	apic_write_around(APIC_ICR, APIC_DM_NMI | APIC_DEST_LOGICAL);

	Dprintk("Waiting for send to finish...\n");
	timeout = 0;
	do {
		Dprintk("+");
		udelay(100);
		send_status = apic_read(APIC_ICR) & APIC_ICR_BUSY;
	} while (send_status && (timeout++ < 1000));

	/*
	 * Give the other CPU some time to accept the IPI.
	 */
	udelay(200);
	/*
	 * Due to the Pentium erratum 3AP.
	 */
	maxlvt = get_maxlvt();
	if (maxlvt > 3) {
		apic_read_around(APIC_SPIV);
		apic_write(APIC_ESR, 0);
	}
	accept_status = (apic_read(APIC_ESR) & 0xEF);
	Dprintk("NMI sent.\n");

	if (send_status)
		printk("APIC never delivered???\n");
	if (accept_status)
		printk("APIC delivery error (%lx).\n", accept_status);

	return (send_status | accept_status);
}
#endif	/* WAKE_SECONDARY_VIA_NMI */

#ifdef WAKE_SECONDARY_VIA_INIT
static int __init
wakeup_secondary_cpu(int phys_apicid, unsigned long start_eip)
{
	unsigned long send_status = 0, accept_status = 0;
	int maxlvt, timeout, num_starts, j;

	/*
	 * Be paranoid about clearing APIC errors.
	 */
	if (APIC_INTEGRATED(apic_version[phys_apicid])) {
		apic_read_around(APIC_SPIV);
		apic_write(APIC_ESR, 0);
		apic_read(APIC_ESR);
	}

	Dprintk("Asserting INIT.\n");

	/*
	 * Turn INIT on target chip
	 */
	apic_write_around(APIC_ICR2, SET_APIC_DEST_FIELD(phys_apicid));

	/*
	 * Send IPI
	 */
	apic_write_around(APIC_ICR, APIC_INT_LEVELTRIG | APIC_INT_ASSERT
				| APIC_DM_INIT);

	Dprintk("Waiting for send to finish...\n");
	timeout = 0;
	do {
		Dprintk("+");
		udelay(100);
		send_status = apic_read(APIC_ICR) & APIC_ICR_BUSY;
	} while (send_status && (timeout++ < 1000));

	mdelay(10);

	Dprintk("Deasserting INIT.\n");

	/* Target chip */
	apic_write_around(APIC_ICR2, SET_APIC_DEST_FIELD(phys_apicid));

	/* Send IPI */
	apic_write_around(APIC_ICR, APIC_INT_LEVELTRIG | APIC_DM_INIT);

	Dprintk("Waiting for send to finish...\n");
	timeout = 0;
	do {
		Dprintk("+");
		udelay(100);
		send_status = apic_read(APIC_ICR) & APIC_ICR_BUSY;
	} while (send_status && (timeout++ < 1000));

	atomic_set(&init_deasserted, 1);

	/*
	 * Should we send STARTUP IPIs ?
	 *
	 * Determine this based on the APIC version.
	 * If we don't have an integrated APIC, don't send the STARTUP IPIs.
	 */
	if (APIC_INTEGRATED(apic_version[phys_apicid]))
		num_starts = 2;
	else
		num_starts = 0;

	/*
	 * Run STARTUP IPI loop.
	 */
	Dprintk("#startup loops: %d.\n", num_starts);

	maxlvt = get_maxlvt();

	for (j = 1; j <= num_starts; j++) {
		Dprintk("Sending STARTUP #%d.\n",j);
		apic_read_around(APIC_SPIV);
		apic_write(APIC_ESR, 0);
		apic_read(APIC_ESR);
		Dprintk("After apic_write.\n");

		/*
		 * STARTUP IPI
		 */

		/* Target chip */
		apic_write_around(APIC_ICR2, SET_APIC_DEST_FIELD(phys_apicid));

		/* Boot on the stack */
		/* Kick the second */
		apic_write_around(APIC_ICR, APIC_DM_STARTUP
					| (start_eip >> 12));

		/*
		 * Give the other CPU some time to accept the IPI.
		 */
		udelay(300);

		Dprintk("Startup point 1.\n");

		Dprintk("Waiting for send to finish...\n");
		timeout = 0;
		do {
			Dprintk("+");
			udelay(100);
			send_status = apic_read(APIC_ICR) & APIC_ICR_BUSY;
		} while (send_status && (timeout++ < 1000));

		/*
		 * Give the other CPU some time to accept the IPI.
		 */
		udelay(200);
		/*
		 * Due to the Pentium erratum 3AP.
		 */
		if (maxlvt > 3) {
			apic_read_around(APIC_SPIV);
			apic_write(APIC_ESR, 0);
		}
		accept_status = (apic_read(APIC_ESR) & 0xEF);
		if (send_status || accept_status)
			break;
	}
	Dprintk("After Startup.\n");

	if (send_status)
		printk("APIC never delivered???\n");
	if (accept_status)
		printk("APIC delivery error (%lx).\n", accept_status);

	return (send_status | accept_status);
}
#endif	/* WAKE_SECONDARY_VIA_INIT */

extern cpumask_t cpu_initialized;

static int __init do_boot_cpu(int apicid)
/*
 * NOTE - on most systems this is a PHYSICAL apic ID, but on multiquad
 * (ie clustered apic addressing mode), this is a LOGICAL apic ID.
 * Returns zero if CPU booted OK, else error code from wakeup_secondary_cpu.
 */
{
	struct task_struct *idle;
	unsigned long boot_error;
	int timeout, cpu;
	unsigned long start_eip;
	unsigned short nmi_high = 0, nmi_low = 0;

	cpu = ++cpucount;
	/*
	 * We can't use kernel_thread since we must avoid to
	 * reschedule the child.
	 */
	idle = fork_by_hand();
	if (IS_ERR(idle))
		panic("failed fork for CPU %d", cpu);
	wake_up_forked_process(idle);

	/*
	 * We remove it from the pidhash and the runqueue
	 * once we got the process:
	 */
	init_idle(idle, cpu);

	idle->thread.eip = (unsigned long) start_secondary;

	unhash_process(idle);

	/* start_eip had better be page-aligned! */
	start_eip = setup_trampoline();

	/* So we see what's up   */
	printk("Booting processor %d/%d eip %lx\n", cpu, apicid, start_eip);
	/* Stack for startup_32 can be just as for start_secondary onwards */
	stack_start.esp = (void *) idle->thread.esp;

	irq_ctx_init(cpu);

	/*
	 * This grunge runs the startup process for
	 * the targeted processor.
	 */

	atomic_set(&init_deasserted, 0);

	Dprintk("Setting warm reset code and vector.\n");

	store_NMI_vector(&nmi_high, &nmi_low);

	smpboot_setup_warm_reset_vector(start_eip);

	/*
	 * Starting actual IPI sequence...
	 */
	boot_error = wakeup_secondary_cpu(apicid, start_eip);

	if (!boot_error) {
		/*
		 * allow APs to start initializing.
		 */
		Dprintk("Before Callout %d.\n", cpu);
		cpu_set(cpu, cpu_callout_map);
		Dprintk("After Callout %d.\n", cpu);

		/*
		 * Wait 5s total for a response
		 */
		for (timeout = 0; timeout < 50000; timeout++) {
			if (cpu_isset(cpu, cpu_callin_map))
				break;	/* It has booted */
			udelay(100);
		}

		if (cpu_isset(cpu, cpu_callin_map)) {
			/* number CPUs logically, starting from 1 (BSP is 0) */
			Dprintk("OK.\n");
			printk("CPU%d: ", cpu);
			print_cpu_info(&cpu_data[cpu]);
			Dprintk("CPU has booted.\n");
		} else {
			boot_error= 1;
			if (*((volatile unsigned char *)trampoline_base)
					== 0xA5)
				/* trampoline started but...? */
				printk("Stuck ??\n");
			else
				/* trampoline code not run */
				printk("Not responding.\n");
			inquire_remote_apic(apicid);
		}
	}
	if (boot_error) {
		/* Try to put things back the way they were before ... */
		unmap_cpu_to_logical_apicid(cpu);
		cpu_clear(cpu, cpu_callout_map); /* was set here (do_boot_cpu()) */
		cpu_clear(cpu, cpu_initialized); /* was set by cpu_init() */
		cpucount--;
	}

	/* mark "stuck" area as not stuck */
	*((volatile unsigned long *)trampoline_base) = 0;

	return boot_error;
}

cycles_t cacheflush_time;
unsigned long cache_decay_ticks;

static void smp_tune_scheduling (void)
{
	unsigned long cachesize;       /* kB   */
	unsigned long bandwidth = 350; /* MB/s */
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
		 * this basically disables processor-affinity
		 * scheduling on SMP without a TSC.
		 */
		cacheflush_time = 0;
		return;
	} else {
		cachesize = boot_cpu_data.x86_cache_size;
		if (cachesize == -1) {
			cachesize = 16; /* Pentiums, 2x8kB cache */
			bandwidth = 100;
		}

		cacheflush_time = (cpu_khz>>10) * (cachesize<<10) / bandwidth;
	}

	cache_decay_ticks = (long)cacheflush_time/cpu_khz + 1;

	printk("per-CPU timeslice cutoff: %ld.%02ld usecs.\n",
		(long)cacheflush_time/(cpu_khz/1000),
		((long)cacheflush_time*100/(cpu_khz/1000)) % 100);
	printk("task migration cache decay timeout: %ld msecs.\n",
		cache_decay_ticks);
}

/*
 * Cycle through the processors sending APIC IPIs to boot each.
 */

static int boot_cpu_logical_apicid;
/* Where the IO area was mapped on multiquad, always 0 otherwise */
void *xquad_portio;

cpumask_t cpu_sibling_map[NR_CPUS] __cacheline_aligned;

static void __init smp_boot_cpus(unsigned int max_cpus)
{
	int apicid, cpu, bit, kicked;
	unsigned long bogosum = 0;

	/*
	 * Setup boot CPU information
	 */
	smp_store_cpu_info(0); /* Final full version of the data */
	printk("CPU%d: ", 0);
	print_cpu_info(&cpu_data[0]);

	boot_cpu_physical_apicid = GET_APIC_ID(apic_read(APIC_ID));
	boot_cpu_logical_apicid = logical_smp_processor_id();

	current_thread_info()->cpu = 0;
	smp_tune_scheduling();
	cpus_clear(cpu_sibling_map[0]);
	cpu_set(0, cpu_sibling_map[0]);

	/*
	 * If we couldn't find an SMP configuration at boot time,
	 * get out of here now!
	 */
	if (!smp_found_config && !acpi_lapic) {
		printk(KERN_NOTICE "SMP motherboard not detected.\n");
		smpboot_clear_io_apic_irqs();
		phys_cpu_present_map = physid_mask_of_physid(0);
		if (APIC_init_uniprocessor())
			printk(KERN_NOTICE "Local APIC not detected."
					   " Using dummy APIC emulation.\n");
		map_cpu_to_logical_apicid();
		return;
	}

	/*
	 * Should not be necessary because the MP table should list the boot
	 * CPU too, but we do it for the sake of robustness anyway.
	 * Makes no sense to do this check in clustered apic mode, so skip it
	 */
	if (!check_phys_apicid_present(boot_cpu_physical_apicid)) {
		printk("weird, boot CPU (#%d) not listed by the BIOS.\n",
				boot_cpu_physical_apicid);
		physid_set(hard_smp_processor_id(), phys_cpu_present_map);
	}

	/*
	 * If we couldn't find a local APIC, then get out of here now!
	 */
	if (APIC_INTEGRATED(apic_version[boot_cpu_physical_apicid]) && !cpu_has_apic) {
		printk(KERN_ERR "BIOS bug, local APIC #%d not detected!...\n",
			boot_cpu_physical_apicid);
		printk(KERN_ERR "... forcing use of dummy APIC emulation. (tell your hw vendor)\n");
		smpboot_clear_io_apic_irqs();
		phys_cpu_present_map = physid_mask_of_physid(0);
		return;
	}

	verify_local_APIC();

	/*
	 * If SMP should be disabled, then really disable it!
	 */
	if (!max_cpus) {
		smp_found_config = 0;
		printk(KERN_INFO "SMP mode deactivated, forcing use of dummy APIC emulation.\n");
		smpboot_clear_io_apic_irqs();
		phys_cpu_present_map = physid_mask_of_physid(0);
		return;
	}

	connect_bsp_APIC();
	setup_local_APIC();
	map_cpu_to_logical_apicid();


	setup_portio_remap();

	/*
	 * Scan the CPU present map and fire up the other CPUs via do_boot_cpu
	 *
	 * In clustered apic mode, phys_cpu_present_map is a constructed thus:
	 * bits 0-3 are quad0, 4-7 are quad1, etc. A perverse twist on the 
	 * clustered apic ID.
	 */
	Dprintk("CPU present map: %lx\n", physids_coerce(phys_cpu_present_map));

	kicked = 1;
	for (bit = 0; kicked < NR_CPUS && bit < MAX_APICS; bit++) {
		apicid = cpu_present_to_apicid(bit);
		/*
		 * Don't even attempt to start the boot CPU!
		 */
		if ((apicid == boot_cpu_apicid) || (apicid == BAD_APICID))
			continue;

		if (!check_apicid_present(bit))
			continue;
		if (max_cpus <= cpucount+1)
			continue;

		if (do_boot_cpu(apicid))
			printk("CPU #%d not responding - cannot use it.\n",
								apicid);
		else
			++kicked;
	}

	/*
	 * Cleanup possible dangling ends...
	 */
	smpboot_restore_warm_reset_vector();

	/*
	 * Allow the user to impress friends.
	 */
	Dprintk("Before bogomips.\n");
	for (cpu = 0; cpu < NR_CPUS; cpu++)
		if (cpu_isset(cpu, cpu_callout_map))
			bogosum += cpu_data[cpu].loops_per_jiffy;
	printk(KERN_INFO
		"Total of %d processors activated (%lu.%02lu BogoMIPS).\n",
		cpucount+1,
		bogosum/(500000/HZ),
		(bogosum/(5000/HZ))%100);
	
	Dprintk("Before bogocount - setting activated=1.\n");

	if (smp_b_stepping)
		printk(KERN_WARNING "WARNING: SMP operation may be unreliable with B stepping processors.\n");

	/*
	 * Don't taint if we are running SMP kernel on a single non-MP
	 * approved Athlon
	 */
	if (tainted & TAINT_UNSAFE_SMP) {
		if (cpucount)
			printk (KERN_INFO "WARNING: This combination of AMD processors is not suitable for SMP.\n");
		else
			tainted &= ~TAINT_UNSAFE_SMP;
	}

	Dprintk("Boot done.\n");

	/*
	 * construct cpu_sibling_map[], so that we can tell sibling CPUs
	 * efficiently.
	 */
	for (cpu = 0; cpu < NR_CPUS; cpu++)
		cpus_clear(cpu_sibling_map[cpu]);

	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		int siblings = 0;
		int i;
		if (!cpu_isset(cpu, cpu_callout_map))
			continue;

		if (smp_num_siblings > 1) {
			for (i = 0; i < NR_CPUS; i++) {
				if (!cpu_isset(i, cpu_callout_map))
					continue;
				if (phys_proc_id[cpu] == phys_proc_id[i]) {
					siblings++;
					cpu_set(i, cpu_sibling_map[cpu]);
				}
			}
		} else {
			siblings++;
			cpu_set(cpu, cpu_sibling_map[cpu]);
		}

		if (siblings != smp_num_siblings)
			printk(KERN_WARNING "WARNING: %d siblings found for CPU%d, should be %d\n", siblings, cpu, smp_num_siblings);
	}

	if (nmi_watchdog == NMI_LOCAL_APIC)
		check_nmi_watchdog();

	smpboot_setup_io_apic();

	setup_boot_APIC_clock();

	/*
	 * Synchronize the TSC with the AP
	 */
	if (cpu_has_tsc && cpucount && cpu_khz)
		synchronize_tsc_bp();
}

#ifdef CONFIG_SCHED_SMT
#ifdef CONFIG_NUMA
static struct sched_group sched_group_cpus[NR_CPUS];
static struct sched_group sched_group_phys[NR_CPUS];
static struct sched_group sched_group_nodes[MAX_NUMNODES];
static DEFINE_PER_CPU(struct sched_domain, cpu_domains);
static DEFINE_PER_CPU(struct sched_domain, phys_domains);
static DEFINE_PER_CPU(struct sched_domain, node_domains);
__init void arch_init_sched_domains(void)
{
	int i;
	struct sched_group *first = NULL, *last = NULL;

	/* Set up domains */
	for_each_cpu(i) {
		struct sched_domain *cpu_domain = &per_cpu(cpu_domains, i);
		struct sched_domain *phys_domain = &per_cpu(phys_domains, i);
		struct sched_domain *node_domain = &per_cpu(node_domains, i);
		int node = cpu_to_node(i);
		cpumask_t nodemask = node_to_cpumask(node);

		*cpu_domain = SD_SIBLING_INIT;
		cpu_domain->span = cpu_sibling_map[i];
		cpu_domain->parent = phys_domain;
		cpu_domain->groups = &sched_group_cpus[i];

		*phys_domain = SD_CPU_INIT;
		phys_domain->span = nodemask;
		phys_domain->parent = node_domain;
		phys_domain->groups = &sched_group_phys[first_cpu(cpu_domain->span)];

		*node_domain = SD_NODE_INIT;
		node_domain->span = cpu_possible_map;
		node_domain->groups = &sched_group_nodes[cpu_to_node(i)];
	}

	/* Set up CPU (sibling) groups */
	for_each_cpu(i) {
		struct sched_domain *cpu_domain = &per_cpu(cpu_domains, i);
		int j;
		first = last = NULL;

		if (i != first_cpu(cpu_domain->span))
			continue;

		for_each_cpu_mask(j, cpu_domain->span) {
			struct sched_group *cpu = &sched_group_cpus[j];

			cpu->cpumask = CPU_MASK_NONE;
			cpu_set(j, cpu->cpumask);
			cpu->cpu_power = SCHED_LOAD_SCALE;

			if (!first)
				first = cpu;
			if (last)
				last->next = cpu;
			last = cpu;
		}
		last->next = first;
	}

	for (i = 0; i < MAX_NUMNODES; i++) {
		int j;
		cpumask_t nodemask;
		struct sched_group *node = &sched_group_nodes[i];
		cpumask_t node_cpumask = node_to_cpumask(i);

		cpus_and(nodemask, node_cpumask, cpu_possible_map);

		if (cpus_empty(nodemask))
			continue;

		first = last = NULL;
		/* Set up physical groups */
		for_each_cpu_mask(j, nodemask) {
			struct sched_domain *cpu_domain = &per_cpu(cpu_domains, j);
			struct sched_group *cpu = &sched_group_phys[j];

			if (j != first_cpu(cpu_domain->span))
				continue;

			cpu->cpumask = cpu_domain->span;
			/*
			 * Make each extra sibling increase power by 10% of
			 * the basic CPU. This is very arbitrary.
			 */
			cpu->cpu_power = SCHED_LOAD_SCALE + SCHED_LOAD_SCALE*(cpus_weight(cpu->cpumask)-1) / 10;
			node->cpu_power += cpu->cpu_power;

			if (!first)
				first = cpu;
			if (last)
				last->next = cpu;
			last = cpu;
		}
		last->next = first;
	}

	/* Set up nodes */
	first = last = NULL;
	for (i = 0; i < MAX_NUMNODES; i++) {
		struct sched_group *cpu = &sched_group_nodes[i];
		cpumask_t nodemask;
		cpumask_t node_cpumask = node_to_cpumask(i);

		cpus_and(nodemask, node_cpumask, cpu_possible_map);

		if (cpus_empty(nodemask))
			continue;

		cpu->cpumask = nodemask;
		/* ->cpu_power already setup */

		if (!first)
			first = cpu;
		if (last)
			last->next = cpu;
		last = cpu;
	}
	last->next = first;

	mb();
	for_each_cpu(i) {
		struct sched_domain *cpu_domain = &per_cpu(cpu_domains, i);
		cpu_attach_domain(cpu_domain, i);
	}
}
#else /* !CONFIG_NUMA */
static struct sched_group sched_group_cpus[NR_CPUS];
static struct sched_group sched_group_phys[NR_CPUS];
static DEFINE_PER_CPU(struct sched_domain, cpu_domains);
static DEFINE_PER_CPU(struct sched_domain, phys_domains);
__init void arch_init_sched_domains(void)
{
	int i;
	struct sched_group *first = NULL, *last = NULL;

	/* Set up domains */
	for_each_cpu(i) {
		struct sched_domain *cpu_domain = &per_cpu(cpu_domains, i);
		struct sched_domain *phys_domain = &per_cpu(phys_domains, i);

		*cpu_domain = SD_SIBLING_INIT;
		cpu_domain->span = cpu_sibling_map[i];
		cpu_domain->parent = phys_domain;
		cpu_domain->groups = &sched_group_cpus[i];

		*phys_domain = SD_CPU_INIT;
		phys_domain->span = cpu_possible_map;
		phys_domain->groups = &sched_group_phys[first_cpu(cpu_domain->span)];
	}

	/* Set up CPU (sibling) groups */
	for_each_cpu(i) {
		struct sched_domain *cpu_domain = &per_cpu(cpu_domains, i);
		int j;
		first = last = NULL;

		if (i != first_cpu(cpu_domain->span))
			continue;

		for_each_cpu_mask(j, cpu_domain->span) {
			struct sched_group *cpu = &sched_group_cpus[j];

			cpus_clear(cpu->cpumask);
			cpu_set(j, cpu->cpumask);
			cpu->cpu_power = SCHED_LOAD_SCALE;

			if (!first)
				first = cpu;
			if (last)
				last->next = cpu;
			last = cpu;
		}
		last->next = first;
	}

	first = last = NULL;
	/* Set up physical groups */
	for_each_cpu(i) {
		struct sched_domain *cpu_domain = &per_cpu(cpu_domains, i);
		struct sched_group *cpu = &sched_group_phys[i];

		if (i != first_cpu(cpu_domain->span))
			continue;

		cpu->cpumask = cpu_domain->span;
		/* See SMT+NUMA setup for comment */
		cpu->cpu_power = SCHED_LOAD_SCALE + SCHED_LOAD_SCALE*(cpus_weight(cpu->cpumask)-1) / 10;

		if (!first)
			first = cpu;
		if (last)
			last->next = cpu;
		last = cpu;
	}
	last->next = first;

	mb();
	for_each_cpu(i) {
		struct sched_domain *cpu_domain = &per_cpu(cpu_domains, i);
		cpu_attach_domain(cpu_domain, i);
	}
}
#endif /* CONFIG_NUMA */
#endif /* CONFIG_SCHED_SMT */

/* These are wrappers to interface to the new boot process.  Someone
   who understands all this stuff should rewrite it properly. --RR 15/Jul/02 */
void __init smp_prepare_cpus(unsigned int max_cpus)
{
	smp_boot_cpus(max_cpus);
}

void __devinit smp_prepare_boot_cpu(void)
{
	cpu_set(smp_processor_id(), cpu_online_map);
	cpu_set(smp_processor_id(), cpu_callout_map);
}

int __devinit __cpu_up(unsigned int cpu)
{
	/* This only works at boot for x86.  See "rewrite" above. */
	if (cpu_isset(cpu, smp_commenced_mask)) {
		local_irq_enable();
		return -ENOSYS;
	}

	/* In case one didn't come up */
	if (!cpu_isset(cpu, cpu_callin_map)) {
		local_irq_enable();
		return -EIO;
	}

	local_irq_enable();
	/* Unleash the CPU! */
	cpu_set(cpu, smp_commenced_mask);
	while (!cpu_isset(cpu, cpu_online_map))
		mb();
	return 0;
}

void __init smp_cpus_done(unsigned int max_cpus)
{
#ifdef CONFIG_X86_IO_APIC
	setup_ioapic_dest();
#endif
	zap_low_mappings();
	/*
	 * Disable executability of the SMP trampoline:
	 */
	set_kernel_exec((unsigned long)trampoline_base, trampoline_exec);
}

void __init smp_intr_init(void)
{
	/*
	 * IRQ0 must be given a fixed assignment and initialized,
	 * because it's used before the IO-APIC is set up.
	 */
	set_intr_gate(FIRST_DEVICE_VECTOR, interrupt[0]);

	/*
	 * The reschedule interrupt is a CPU-to-CPU reschedule-helper
	 * IPI, driven by wakeup.
	 */
	set_intr_gate(RESCHEDULE_VECTOR, reschedule_interrupt);

	/* IPI for invalidation */
	set_intr_gate(INVALIDATE_TLB_VECTOR, invalidate_interrupt);

	/* IPI for generic function call */
	set_intr_gate(CALL_FUNCTION_VECTOR, call_function_interrupt);
}
