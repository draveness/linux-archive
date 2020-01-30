/*
 *	linux/arch/alpha/kernel/smp.c
 *
 *      2001-07-09 Phil Ezolt (Phillip.Ezolt@compaq.com)
 *            Renamed modified smp_call_function to smp_call_function_on_cpu()
 *            Created an function that conforms to the old calling convention
 *            of smp_call_function().
 *
 *            This is helpful for DCPI.
 *
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/threads.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/irq.h>
#include <linux/cache.h>

#include <asm/hwrpb.h>
#include <asm/ptrace.h>
#include <asm/atomic.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/bitops.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/hardirq.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>

#include "proto.h"
#include "irq_impl.h"


#define DEBUG_SMP 0
#if DEBUG_SMP
#define DBGS(args)	printk args
#else
#define DBGS(args)
#endif

/* A collection of per-processor data.  */
struct cpuinfo_alpha cpu_data[NR_CPUS];

/* A collection of single bit ipi messages.  */
static struct {
	unsigned long bits ____cacheline_aligned;
} ipi_data[NR_CPUS] __cacheline_aligned;

enum ipi_message_type {
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CPU_STOP,
};

/* Set to a secondary's cpuid when it comes online.  */
static int smp_secondary_alive __initdata = 0;

/* Which cpus ids came online.  */
cpumask_t cpu_present_mask;
cpumask_t cpu_online_map;

EXPORT_SYMBOL(cpu_online_map);

/* cpus reported in the hwrpb */
static unsigned long hwrpb_cpu_present_mask __initdata = 0;

int smp_num_probed;		/* Internal processor count */
int smp_num_cpus = 1;		/* Number that came online.  */
cycles_t cacheflush_time;
unsigned long cache_decay_ticks;

extern void calibrate_delay(void);



/*
 * Called by both boot and secondaries to move global data into
 *  per-processor storage.
 */
static inline void __init
smp_store_cpu_info(int cpuid)
{
	cpu_data[cpuid].loops_per_jiffy = loops_per_jiffy;
	cpu_data[cpuid].last_asn = ASN_FIRST_VERSION;
	cpu_data[cpuid].need_new_asn = 0;
	cpu_data[cpuid].asn_lock = 0;
}

/*
 * Ideally sets up per-cpu profiling hooks.  Doesn't do much now...
 */
static inline void __init
smp_setup_percpu_timer(int cpuid)
{
	cpu_data[cpuid].prof_counter = 1;
	cpu_data[cpuid].prof_multiplier = 1;
}

static void __init
wait_boot_cpu_to_stop(int cpuid)
{
	unsigned long stop = jiffies + 10*HZ;

	while (time_before(jiffies, stop)) {
	        if (!smp_secondary_alive)
			return;
		barrier();
	}

	printk("wait_boot_cpu_to_stop: FAILED on CPU %d, hanging now\n", cpuid);
	for (;;)
		barrier();
}

/*
 * Where secondaries begin a life of C.
 */
void __init
smp_callin(void)
{
	int cpuid = hard_smp_processor_id();

	if (cpu_test_and_set(cpuid, cpu_online_map)) {
		printk("??, cpu 0x%x already present??\n", cpuid);
		BUG();
	}

	/* Turn on machine checks.  */
	wrmces(7);

	/* Set trap vectors.  */
	trap_init();

	/* Set interrupt vector.  */
	wrent(entInt, 0);

	/* Get our local ticker going. */
	smp_setup_percpu_timer(cpuid);

	/* Call platform-specific callin, if specified */
	if (alpha_mv.smp_callin) alpha_mv.smp_callin();

	/* All kernel threads share the same mm context.  */
	atomic_inc(&init_mm.mm_count);
	current->active_mm = &init_mm;

	/* Must have completely accurate bogos.  */
	local_irq_enable();

	/* Wait boot CPU to stop with irq enabled before running
	   calibrate_delay. */
	wait_boot_cpu_to_stop(cpuid);
	mb();
	calibrate_delay();

	smp_store_cpu_info(cpuid);
	/* Allow master to continue only after we written loops_per_jiffy.  */
	wmb();
	smp_secondary_alive = 1;

	DBGS(("smp_callin: commencing CPU %d current %p active_mm %p\n",
	      cpuid, current, current->active_mm));

	/* Do nothing.  */
	cpu_idle();
}


/*
 * Rough estimation for SMP scheduling, this is the number of cycles it
 * takes for a fully memory-limited process to flush the SMP-local cache.
 *
 * We are not told how much cache there is, so we have to guess.
 */
static void __init
smp_tune_scheduling (int cpuid)
{
	struct percpu_struct *cpu;
	unsigned long on_chip_cache;	/* kB */
	unsigned long freq;		/* Hz */
	unsigned long bandwidth = 350;	/* MB/s */

	cpu = (struct percpu_struct*)((char*)hwrpb + hwrpb->processor_offset
				      + cpuid * hwrpb->processor_size);
	switch (cpu->type)
	{
	case EV45_CPU:
		on_chip_cache = 16 + 16;
		break;

	case EV5_CPU:
	case EV56_CPU:
		on_chip_cache = 8 + 8 + 96;
		break;

	case PCA56_CPU:
		on_chip_cache = 16 + 8;
		break;

	case EV6_CPU:
	case EV67_CPU:
	default:
		on_chip_cache = 64 + 64;
		break;
	}

	freq = hwrpb->cycle_freq ? : est_cycle_freq;

	cacheflush_time = (freq / 1000000) * (on_chip_cache << 10) / bandwidth;
	cache_decay_ticks = cacheflush_time / (freq / 1000) * HZ / 1000;

	printk("per-CPU timeslice cutoff: %ld.%02ld usecs.\n",
	       cacheflush_time/(freq/1000000),
	       (cacheflush_time*100/(freq/1000000)) % 100);
	printk("task migration cache decay timeout: %ld msecs.\n",
	       (cache_decay_ticks + 1) * 1000 / HZ);
}

/* Wait until hwrpb->txrdy is clear for cpu.  Return -1 on timeout.  */
static int __init
wait_for_txrdy (unsigned long cpumask)
{
	unsigned long timeout;

	if (!(hwrpb->txrdy & cpumask))
		return 0;

	timeout = jiffies + 10*HZ;
	while (time_before(jiffies, timeout)) {
		if (!(hwrpb->txrdy & cpumask))
			return 0;
		udelay(10);
		barrier();
	}

	return -1;
}

/*
 * Send a message to a secondary's console.  "START" is one such
 * interesting message.  ;-)
 */
static void __init
send_secondary_console_msg(char *str, int cpuid)
{
	struct percpu_struct *cpu;
	register char *cp1, *cp2;
	unsigned long cpumask;
	size_t len;

	cpu = (struct percpu_struct *)
		((char*)hwrpb
		 + hwrpb->processor_offset
		 + cpuid * hwrpb->processor_size);

	cpumask = (1UL << cpuid);
	if (wait_for_txrdy(cpumask))
		goto timeout;

	cp2 = str;
	len = strlen(cp2);
	*(unsigned int *)&cpu->ipc_buffer[0] = len;
	cp1 = (char *) &cpu->ipc_buffer[1];
	memcpy(cp1, cp2, len);

	/* atomic test and set */
	wmb();
	set_bit(cpuid, &hwrpb->rxrdy);

	if (wait_for_txrdy(cpumask))
		goto timeout;
	return;

 timeout:
	printk("Processor %x not ready\n", cpuid);
}

/*
 * A secondary console wants to send a message.  Receive it.
 */
static void
recv_secondary_console_msg(void)
{
	int mycpu, i, cnt;
	unsigned long txrdy = hwrpb->txrdy;
	char *cp1, *cp2, buf[80];
	struct percpu_struct *cpu;

	DBGS(("recv_secondary_console_msg: TXRDY 0x%lx.\n", txrdy));

	mycpu = hard_smp_processor_id();

	for (i = 0; i < NR_CPUS; i++) {
		if (!(txrdy & (1UL << i)))
			continue;

		DBGS(("recv_secondary_console_msg: "
		      "TXRDY contains CPU %d.\n", i));

		cpu = (struct percpu_struct *)
		  ((char*)hwrpb
		   + hwrpb->processor_offset
		   + i * hwrpb->processor_size);

 		DBGS(("recv_secondary_console_msg: on %d from %d"
		      " HALT_REASON 0x%lx FLAGS 0x%lx\n",
		      mycpu, i, cpu->halt_reason, cpu->flags));

		cnt = cpu->ipc_buffer[0] >> 32;
		if (cnt <= 0 || cnt >= 80)
			strcpy(buf, "<<< BOGUS MSG >>>");
		else {
			cp1 = (char *) &cpu->ipc_buffer[11];
			cp2 = buf;
			strcpy(cp2, cp1);
			
			while ((cp2 = strchr(cp2, '\r')) != 0) {
				*cp2 = ' ';
				if (cp2[1] == '\n')
					cp2[1] = ' ';
			}
		}

		DBGS((KERN_INFO "recv_secondary_console_msg: on %d "
		      "message is '%s'\n", mycpu, buf));
	}

	hwrpb->txrdy = 0;
}

/*
 * Convince the console to have a secondary cpu begin execution.
 */
static int __init
secondary_cpu_start(int cpuid, struct task_struct *idle)
{
	struct percpu_struct *cpu;
	struct pcb_struct *hwpcb, *ipcb;
	unsigned long timeout;
	  
	cpu = (struct percpu_struct *)
		((char*)hwrpb
		 + hwrpb->processor_offset
		 + cpuid * hwrpb->processor_size);
	hwpcb = (struct pcb_struct *) cpu->hwpcb;
	ipcb = &idle->thread_info->pcb;

	/* Initialize the CPU's HWPCB to something just good enough for
	   us to get started.  Immediately after starting, we'll swpctx
	   to the target idle task's pcb.  Reuse the stack in the mean
	   time.  Precalculate the target PCBB.  */
	hwpcb->ksp = (unsigned long)ipcb + sizeof(union thread_union) - 16;
	hwpcb->usp = 0;
	hwpcb->ptbr = ipcb->ptbr;
	hwpcb->pcc = 0;
	hwpcb->asn = 0;
	hwpcb->unique = virt_to_phys(ipcb);
	hwpcb->flags = ipcb->flags;
	hwpcb->res1 = hwpcb->res2 = 0;

#if 0
	DBGS(("KSP 0x%lx PTBR 0x%lx VPTBR 0x%lx UNIQUE 0x%lx\n",
	      hwpcb->ksp, hwpcb->ptbr, hwrpb->vptb, hwpcb->unique));
#endif
	DBGS(("Starting secondary cpu %d: state 0x%lx pal_flags 0x%lx\n",
	      cpuid, idle->state, ipcb->flags));

	/* Setup HWRPB fields that SRM uses to activate secondary CPU */
	hwrpb->CPU_restart = __smp_callin;
	hwrpb->CPU_restart_data = (unsigned long) __smp_callin;

	/* Recalculate and update the HWRPB checksum */
	hwrpb_update_checksum(hwrpb);

	/*
	 * Send a "start" command to the specified processor.
	 */

	/* SRM III 3.4.1.3 */
	cpu->flags |= 0x22;	/* turn on Context Valid and Restart Capable */
	cpu->flags &= ~1;	/* turn off Bootstrap In Progress */
	wmb();

	send_secondary_console_msg("START\r\n", cpuid);

	/* Wait 10 seconds for an ACK from the console.  */
	timeout = jiffies + 10*HZ;
	while (time_before(jiffies, timeout)) {
		if (cpu->flags & 1)
			goto started;
		udelay(10);
		barrier();
	}
	printk(KERN_ERR "SMP: Processor %d failed to start.\n", cpuid);
	return -1;

 started:
	DBGS(("secondary_cpu_start: SUCCESS for CPU %d!!!\n", cpuid));
	return 0;
}

static struct task_struct * __init
fork_by_hand(void)
{
	/* Don't care about the contents of regs since we'll never
	   reschedule the forked task. */
	struct pt_regs regs;
	return copy_process(CLONE_VM|CLONE_IDLETASK, 0, &regs, 0, NULL, NULL);
}

/*
 * Bring one cpu online.
 */
static int __init
smp_boot_one_cpu(int cpuid)
{
	struct task_struct *idle;
	unsigned long timeout;

	/* Cook up an idler for this guy.  Note that the address we
	   give to kernel_thread is irrelevant -- it's going to start
	   where HWRPB.CPU_restart says to start.  But this gets all
	   the other task-y sort of data structures set up like we
	   wish.  We can't use kernel_thread since we must avoid
	   rescheduling the child.  */
	idle = fork_by_hand();
	if (IS_ERR(idle))
		panic("failed fork for CPU %d", cpuid);

	wake_up_forked_process(idle);

	init_idle(idle, cpuid);
	unhash_process(idle);

	DBGS(("smp_boot_one_cpu: CPU %d state 0x%lx flags 0x%lx\n",
	      cpuid, idle->state, idle->flags));

	/* Signal the secondary to wait a moment.  */
	smp_secondary_alive = -1;

	/* Whirrr, whirrr, whirrrrrrrrr... */
	if (secondary_cpu_start(cpuid, idle))
		return -1;

	/* Notify the secondary CPU it can run calibrate_delay.  */
	mb();
	smp_secondary_alive = 0;

	/* We've been acked by the console; wait one second for
	   the task to start up for real.  */
	timeout = jiffies + 1*HZ;
	while (time_before(jiffies, timeout)) {
		if (smp_secondary_alive == 1)
			goto alive;
		udelay(10);
		barrier();
	}

	/* We failed to boot the CPU.  */

	printk(KERN_ERR "SMP: Processor %d is stuck.\n", cpuid);
	return -1;

 alive:
	/* Another "Red Snapper". */
	return 0;
}

/*
 * Called from setup_arch.  Detect an SMP system and which processors
 * are present.
 */
void __init
setup_smp(void)
{
	struct percpu_struct *cpubase, *cpu;
	unsigned long i;

	if (boot_cpuid != 0) {
		printk(KERN_WARNING "SMP: Booting off cpu %d instead of 0?\n",
		       boot_cpuid);
	}

	if (hwrpb->nr_processors > 1) {
		int boot_cpu_palrev;

		DBGS(("setup_smp: nr_processors %ld\n",
		      hwrpb->nr_processors));

		cpubase = (struct percpu_struct *)
			((char*)hwrpb + hwrpb->processor_offset);
		boot_cpu_palrev = cpubase->pal_revision;

		for (i = 0; i < hwrpb->nr_processors; i++) {
			cpu = (struct percpu_struct *)
				((char *)cpubase + i*hwrpb->processor_size);
			if ((cpu->flags & 0x1cc) == 0x1cc) {
				smp_num_probed++;
				/* Assume here that "whami" == index */
				hwrpb_cpu_present_mask |= (1UL << i);
				cpu->pal_revision = boot_cpu_palrev;
			}

			DBGS(("setup_smp: CPU %d: flags 0x%lx type 0x%lx\n",
			      i, cpu->flags, cpu->type));
			DBGS(("setup_smp: CPU %d: PAL rev 0x%lx\n",
			      i, cpu->pal_revision));
		}
	} else {
		smp_num_probed = 1;
		hwrpb_cpu_present_mask = (1UL << boot_cpuid);
	}
	cpu_present_mask = cpumask_of_cpu(boot_cpuid);

	printk(KERN_INFO "SMP: %d CPUs probed -- cpu_present_mask = %lx\n",
	       smp_num_probed, hwrpb_cpu_present_mask);
}

/*
 * Called by smp_init prepare the secondaries
 */
void __init
smp_prepare_cpus(unsigned int max_cpus)
{
	int cpu_count, i;

	/* Take care of some initial bookkeeping.  */
	memset(ipi_data, 0, sizeof(ipi_data));

	current_thread_info()->cpu = boot_cpuid;

	smp_store_cpu_info(boot_cpuid);
	smp_tune_scheduling(boot_cpuid);
	smp_setup_percpu_timer(boot_cpuid);

	/* Nothing to do on a UP box, or when told not to.  */
	if (smp_num_probed == 1 || max_cpus == 0) {
		cpu_present_mask = cpumask_of_cpu(boot_cpuid);
		printk(KERN_INFO "SMP mode deactivated.\n");
		return;
	}

	printk(KERN_INFO "SMP starting up secondaries.\n");

	cpu_count = 1;
	for (i = 0; (i < NR_CPUS) && (cpu_count < max_cpus); i++) {
		if (i == boot_cpuid)
			continue;

		if (((hwrpb_cpu_present_mask >> i) & 1) == 0)
			continue;

		cpu_set(i, cpu_possible_map);
		cpu_count++;
	}

	smp_num_cpus = cpu_count;
}

void __devinit
smp_prepare_boot_cpu(void)
{
	/*
	 * Mark the boot cpu (current cpu) as both present and online
	 */ 
	cpu_set(smp_processor_id(), cpu_present_mask);
	cpu_set(smp_processor_id(), cpu_online_map);
}

int __devinit
__cpu_up(unsigned int cpu)
{
	smp_boot_one_cpu(cpu);

	return cpu_online(cpu) ? 0 : -ENOSYS;
}

void __init
smp_cpus_done(unsigned int max_cpus)
{
	int cpu;
	unsigned long bogosum = 0;

	for(cpu = 0; cpu < NR_CPUS; cpu++) 
		if (cpu_online(cpu))
			bogosum += cpu_data[cpu].loops_per_jiffy;
	
	printk(KERN_INFO "SMP: Total of %d processors activated "
	       "(%lu.%02lu BogoMIPS).\n",
	       num_online_cpus(), 
	       (bogosum + 2500) / (500000/HZ),
	       ((bogosum + 2500) / (5000/HZ)) % 100);
}


void
smp_percpu_timer_interrupt(struct pt_regs *regs)
{
	int cpu = smp_processor_id();
	unsigned long user = user_mode(regs);
	struct cpuinfo_alpha *data = &cpu_data[cpu];

	/* Record kernel PC.  */
	if (!user)
		alpha_do_profile(regs->pc);

	if (!--data->prof_counter) {
		/* We need to make like a normal interrupt -- otherwise
		   timer interrupts ignore the global interrupt lock,
		   which would be a Bad Thing.  */
		irq_enter();

		update_process_times(user);

		data->prof_counter = data->prof_multiplier;

		irq_exit();
	}
}

int __init
setup_profiling_timer(unsigned int multiplier)
{
	return -EINVAL;
}


static void
send_ipi_message(cpumask_t to_whom, enum ipi_message_type operation)
{
	int i;

	mb();
	for_each_cpu_mask(i, to_whom)
		set_bit(operation, &ipi_data[i].bits);

	mb();
	for_each_cpu_mask(i, to_whom)
		wripir(i);
}

/* Structure and data for smp_call_function.  This is designed to 
   minimize static memory requirements.  Plus it looks cleaner.  */

struct smp_call_struct {
	void (*func) (void *info);
	void *info;
	long wait;
	atomic_t unstarted_count;
	atomic_t unfinished_count;
};

static struct smp_call_struct *smp_call_function_data;

/* Atomicly drop data into a shared pointer.  The pointer is free if
   it is initially locked.  If retry, spin until free.  */

static int
pointer_lock (void *lock, void *data, int retry)
{
	void *old, *tmp;

	mb();
 again:
	/* Compare and swap with zero.  */
	asm volatile (
	"1:	ldq_l	%0,%1\n"
	"	mov	%3,%2\n"
	"	bne	%0,2f\n"
	"	stq_c	%2,%1\n"
	"	beq	%2,1b\n"
	"2:"
	: "=&r"(old), "=m"(*(void **)lock), "=&r"(tmp)
	: "r"(data)
	: "memory");

	if (old == 0)
		return 0;
	if (! retry)
		return -EBUSY;

	while (*(void **)lock)
		barrier();
	goto again;
}

void
handle_ipi(struct pt_regs *regs)
{
	int this_cpu = smp_processor_id();
	unsigned long *pending_ipis = &ipi_data[this_cpu].bits;
	unsigned long ops;

#if 0
	DBGS(("handle_ipi: on CPU %d ops 0x%lx PC 0x%lx\n",
	      this_cpu, *pending_ipis, regs->pc));
#endif

	mb();	/* Order interrupt and bit testing. */
	while ((ops = xchg(pending_ipis, 0)) != 0) {
	  mb();	/* Order bit clearing and data access. */
	  do {
		unsigned long which;

		which = ops & -ops;
		ops &= ~which;
		which = __ffs(which);

		switch (which) {
		case IPI_RESCHEDULE:
			/* Reschedule callback.  Everything to be done
			   is done by the interrupt return path.  */
			break;

		case IPI_CALL_FUNC:
		    {
			struct smp_call_struct *data;
			void (*func)(void *info);
			void *info;
			int wait;

			data = smp_call_function_data;
			func = data->func;
			info = data->info;
			wait = data->wait;

			/* Notify the sending CPU that the data has been
			   received, and execution is about to begin.  */
			mb();
			atomic_dec (&data->unstarted_count);

			/* At this point the structure may be gone unless
			   wait is true.  */
			(*func)(info);

			/* Notify the sending CPU that the task is done.  */
			mb();
			if (wait) atomic_dec (&data->unfinished_count);
			break;
		    }

		case IPI_CPU_STOP:
			halt();

		default:
			printk(KERN_CRIT "Unknown IPI on CPU %d: %lu\n",
			       this_cpu, which);
			break;
		}
	  } while (ops);

	  mb();	/* Order data access and bit testing. */
	}

	cpu_data[this_cpu].ipi_count++;

	if (hwrpb->txrdy)
		recv_secondary_console_msg();
}

void
smp_send_reschedule(int cpu)
{
#ifdef DEBUG_IPI_MSG
	if (cpu == hard_smp_processor_id())
		printk(KERN_WARNING
		       "smp_send_reschedule: Sending IPI to self.\n");
#endif
	send_ipi_message(cpumask_of_cpu(cpu), IPI_RESCHEDULE);
}

void
smp_send_stop(void)
{
	cpumask_t to_whom = cpu_possible_map;
	cpu_clear(smp_processor_id(), to_whom);
#ifdef DEBUG_IPI_MSG
	if (hard_smp_processor_id() != boot_cpu_id)
		printk(KERN_WARNING "smp_send_stop: Not on boot cpu.\n");
#endif
	send_ipi_message(to_whom, IPI_CPU_STOP);
}

/*
 * Run a function on all other CPUs.
 *  <func>	The function to run. This must be fast and non-blocking.
 *  <info>	An arbitrary pointer to pass to the function.
 *  <retry>	If true, keep retrying until ready.
 *  <wait>	If true, wait until function has completed on other CPUs.
 *  [RETURNS]   0 on success, else a negative status code.
 *
 * Does not return until remote CPUs are nearly ready to execute <func>
 * or are or have executed.
 * You must not call this function with disabled interrupts or from a
 * hardware interrupt handler or from a bottom half handler.
 */

int
smp_call_function_on_cpu (void (*func) (void *info), void *info, int retry,
			  int wait, cpumask_t to_whom)
{
	struct smp_call_struct data;
	unsigned long timeout;
	int num_cpus_to_call;
	
	/* Can deadlock when called with interrupts disabled */
	WARN_ON(irqs_disabled());

	data.func = func;
	data.info = info;
	data.wait = wait;

	cpu_clear(smp_processor_id(), to_whom);
	num_cpus_to_call = cpus_weight(to_whom);

	atomic_set(&data.unstarted_count, num_cpus_to_call);
	atomic_set(&data.unfinished_count, num_cpus_to_call);

	/* Acquire the smp_call_function_data mutex.  */
	if (pointer_lock(&smp_call_function_data, &data, retry))
		return -EBUSY;

	/* Send a message to the requested CPUs.  */
	send_ipi_message(to_whom, IPI_CALL_FUNC);

	/* Wait for a minimal response.  */
	timeout = jiffies + HZ;
	while (atomic_read (&data.unstarted_count) > 0
	       && time_before (jiffies, timeout))
		barrier();

	/* If there's no response yet, log a message but allow a longer
	 * timeout period -- if we get a response this time, log
	 * a message saying when we got it.. 
	 */
	if (atomic_read(&data.unstarted_count) > 0) {
		long start_time = jiffies;
		printk(KERN_ERR "%s: initial timeout -- trying long wait\n",
		       __FUNCTION__);
		timeout = jiffies + 30 * HZ;
		while (atomic_read(&data.unstarted_count) > 0
		       && time_before(jiffies, timeout))
			barrier();
		if (atomic_read(&data.unstarted_count) <= 0) {
			long delta = jiffies - start_time;
			printk(KERN_ERR 
			       "%s: response %ld.%ld seconds into long wait\n",
			       __FUNCTION__, delta / HZ,
			       (100 * (delta - ((delta / HZ) * HZ))) / HZ);
		}
	}

	/* We either got one or timed out -- clear the lock. */
	mb();
	smp_call_function_data = NULL;

	/* 
	 * If after both the initial and long timeout periods we still don't
	 * have a response, something is very wrong...
	 */
	BUG_ON(atomic_read (&data.unstarted_count) > 0);

	/* Wait for a complete response, if needed.  */
	if (wait) {
		while (atomic_read (&data.unfinished_count) > 0)
			barrier();
	}

	return 0;
}

int
smp_call_function (void (*func) (void *info), void *info, int retry, int wait)
{
	return smp_call_function_on_cpu (func, info, retry, wait,
					 cpu_online_map);
}

static void
ipi_imb(void *ignored)
{
	imb();
}

void
smp_imb(void)
{
	/* Must wait other processors to flush their icache before continue. */
	if (on_each_cpu(ipi_imb, NULL, 1, 1))
		printk(KERN_CRIT "smp_imb: timed out\n");
}

static void
ipi_flush_tlb_all(void *ignored)
{
	tbia();
}

void
flush_tlb_all(void)
{
	/* Although we don't have any data to pass, we do want to
	   synchronize with the other processors.  */
	if (on_each_cpu(ipi_flush_tlb_all, NULL, 1, 1)) {
		printk(KERN_CRIT "flush_tlb_all: timed out\n");
	}
}

#define asn_locked() (cpu_data[smp_processor_id()].asn_lock)

static void
ipi_flush_tlb_mm(void *x)
{
	struct mm_struct *mm = (struct mm_struct *) x;
	if (mm == current->active_mm && !asn_locked())
		flush_tlb_current(mm);
	else
		flush_tlb_other(mm);
}

void
flush_tlb_mm(struct mm_struct *mm)
{
	preempt_disable();

	if (mm == current->active_mm) {
		flush_tlb_current(mm);
		if (atomic_read(&mm->mm_users) <= 1) {
			int cpu, this_cpu = smp_processor_id();
			for (cpu = 0; cpu < NR_CPUS; cpu++) {
				if (!cpu_online(cpu) || cpu == this_cpu)
					continue;
				if (mm->context[cpu])
					mm->context[cpu] = 0;
			}
			preempt_enable();
			return;
		}
	}

	if (smp_call_function(ipi_flush_tlb_mm, mm, 1, 1)) {
		printk(KERN_CRIT "flush_tlb_mm: timed out\n");
	}

	preempt_enable();
}

struct flush_tlb_page_struct {
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned long addr;
};

static void
ipi_flush_tlb_page(void *x)
{
	struct flush_tlb_page_struct *data = (struct flush_tlb_page_struct *)x;
	struct mm_struct * mm = data->mm;

	if (mm == current->active_mm && !asn_locked())
		flush_tlb_current_page(mm, data->vma, data->addr);
	else
		flush_tlb_other(mm);
}

void
flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	struct flush_tlb_page_struct data;
	struct mm_struct *mm = vma->vm_mm;

	preempt_disable();

	if (mm == current->active_mm) {
		flush_tlb_current_page(mm, vma, addr);
		if (atomic_read(&mm->mm_users) <= 1) {
			int cpu, this_cpu = smp_processor_id();
			for (cpu = 0; cpu < NR_CPUS; cpu++) {
				if (!cpu_online(cpu) || cpu == this_cpu)
					continue;
				if (mm->context[cpu])
					mm->context[cpu] = 0;
			}
			preempt_enable();
			return;
		}
	}

	data.vma = vma;
	data.mm = mm;
	data.addr = addr;

	if (smp_call_function(ipi_flush_tlb_page, &data, 1, 1)) {
		printk(KERN_CRIT "flush_tlb_page: timed out\n");
	}

	preempt_enable();
}

void
flush_tlb_range(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	/* On the Alpha we always flush the whole user tlb.  */
	flush_tlb_mm(vma->vm_mm);
}

static void
ipi_flush_icache_page(void *x)
{
	struct mm_struct *mm = (struct mm_struct *) x;
	if (mm == current->active_mm && !asn_locked())
		__load_new_mm_context(mm);
	else
		flush_tlb_other(mm);
}

void
flush_icache_user_range(struct vm_area_struct *vma, struct page *page,
			unsigned long addr, int len)
{
	struct mm_struct *mm = vma->vm_mm;

	if ((vma->vm_flags & VM_EXEC) == 0)
		return;

	preempt_disable();

	if (mm == current->active_mm) {
		__load_new_mm_context(mm);
		if (atomic_read(&mm->mm_users) <= 1) {
			int cpu, this_cpu = smp_processor_id();
			for (cpu = 0; cpu < NR_CPUS; cpu++) {
				if (!cpu_online(cpu) || cpu == this_cpu)
					continue;
				if (mm->context[cpu])
					mm->context[cpu] = 0;
			}
			preempt_enable();
			return;
		}
	}

	if (smp_call_function(ipi_flush_icache_page, mm, 1, 1)) {
		printk(KERN_CRIT "flush_icache_page: timed out\n");
	}

	preempt_enable();
}

#ifdef CONFIG_DEBUG_SPINLOCK
void
_raw_spin_unlock(spinlock_t * lock)
{
	mb();
	lock->lock = 0;

	lock->on_cpu = -1;
	lock->previous = NULL;
	lock->task = NULL;
	lock->base_file = "none";
	lock->line_no = 0;
}

void
debug_spin_lock(spinlock_t * lock, const char *base_file, int line_no)
{
	long tmp;
	long stuck;
	void *inline_pc = __builtin_return_address(0);
	unsigned long started = jiffies;
	int printed = 0;
	int cpu = smp_processor_id();

	stuck = 1L << 30;
 try_again:

	/* Use sub-sections to put the actual loop at the end
	   of this object file's text section so as to perfect
	   branch prediction.  */
	__asm__ __volatile__(
	"1:	ldl_l	%0,%1\n"
	"	subq	%2,1,%2\n"
	"	blbs	%0,2f\n"
	"	or	%0,1,%0\n"
	"	stl_c	%0,%1\n"
	"	beq	%0,3f\n"
	"4:	mb\n"
	".subsection 2\n"
	"2:	ldl	%0,%1\n"
	"	subq	%2,1,%2\n"
	"3:	blt	%2,4b\n"
	"	blbs	%0,2b\n"
	"	br	1b\n"
	".previous"
	: "=r" (tmp), "=m" (lock->lock), "=r" (stuck)
	: "1" (lock->lock), "2" (stuck) : "memory");

	if (stuck < 0) {
		printk(KERN_WARNING
		       "%s:%d spinlock stuck in %s at %p(%d)"
		       " owner %s at %p(%d) %s:%d\n",
		       base_file, line_no,
		       current->comm, inline_pc, cpu,
		       lock->task->comm, lock->previous,
		       lock->on_cpu, lock->base_file, lock->line_no);
		stuck = 1L << 36;
		printed = 1;
		goto try_again;
	}

	/* Exiting.  Got the lock.  */
	lock->on_cpu = cpu;
	lock->previous = inline_pc;
	lock->task = current;
	lock->base_file = base_file;
	lock->line_no = line_no;

	if (printed) {
		printk(KERN_WARNING
		       "%s:%d spinlock grabbed in %s at %p(%d) %ld ticks\n",
		       base_file, line_no, current->comm, inline_pc,
		       cpu, jiffies - started);
	}
}

int
debug_spin_trylock(spinlock_t * lock, const char *base_file, int line_no)
{
	int ret;
	if ((ret = !test_and_set_bit(0, lock))) {
		lock->on_cpu = smp_processor_id();
		lock->previous = __builtin_return_address(0);
		lock->task = current;
	} else {
		lock->base_file = base_file;
		lock->line_no = line_no;
	}
	return ret;
}
#endif /* CONFIG_DEBUG_SPINLOCK */

#ifdef CONFIG_DEBUG_RWLOCK
void _raw_write_lock(rwlock_t * lock)
{
	long regx, regy;
	int stuck_lock, stuck_reader;
	void *inline_pc = __builtin_return_address(0);

 try_again:

	stuck_lock = 1<<30;
	stuck_reader = 1<<30;

	__asm__ __volatile__(
	"1:	ldl_l	%1,%0\n"
	"	blbs	%1,6f\n"
	"	blt	%1,8f\n"
	"	mov	1,%1\n"
	"	stl_c	%1,%0\n"
	"	beq	%1,6f\n"
	"4:	mb\n"
	".subsection 2\n"
	"6:	blt	%3,4b	# debug\n"
	"	subl	%3,1,%3	# debug\n"
	"	ldl	%1,%0\n"
	"	blbs	%1,6b\n"
	"8:	blt	%4,4b	# debug\n"
	"	subl	%4,1,%4	# debug\n"
	"	ldl	%1,%0\n"
	"	blt	%1,8b\n"
	"	br	1b\n"
	".previous"
	: "=m" (*(volatile int *)lock), "=&r" (regx), "=&r" (regy),
	  "=&r" (stuck_lock), "=&r" (stuck_reader)
	: "0" (*(volatile int *)lock), "3" (stuck_lock), "4" (stuck_reader) : "memory");

	if (stuck_lock < 0) {
		printk(KERN_WARNING "write_lock stuck at %p\n", inline_pc);
		goto try_again;
	}
	if (stuck_reader < 0) {
		printk(KERN_WARNING "write_lock stuck on readers at %p\n",
		       inline_pc);
		goto try_again;
	}
}

void _raw_read_lock(rwlock_t * lock)
{
	long regx;
	int stuck_lock;
	void *inline_pc = __builtin_return_address(0);

 try_again:

	stuck_lock = 1<<30;

	__asm__ __volatile__(
	"1:	ldl_l	%1,%0;"
	"	blbs	%1,6f;"
	"	subl	%1,2,%1;"
	"	stl_c	%1,%0;"
	"	beq	%1,6f;"
	"4:	mb\n"
	".subsection 2\n"
	"6:	ldl	%1,%0;"
	"	blt	%2,4b	# debug\n"
	"	subl	%2,1,%2	# debug\n"
	"	blbs	%1,6b;"
	"	br	1b\n"
	".previous"
	: "=m" (*(volatile int *)lock), "=&r" (regx), "=&r" (stuck_lock)
	: "0" (*(volatile int *)lock), "2" (stuck_lock) : "memory");

	if (stuck_lock < 0) {
		printk(KERN_WARNING "read_lock stuck at %p\n", inline_pc);
		goto try_again;
	}
}
#endif /* CONFIG_DEBUG_RWLOCK */
