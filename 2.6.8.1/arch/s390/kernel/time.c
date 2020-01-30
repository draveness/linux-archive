/*
 *  arch/s390/kernel/time.c
 *    Time of day based timer functions.
 *
 *  S390 version
 *    Copyright (C) 1999 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Hartmut Penner (hp@de.ibm.com),
 *               Martin Schwidefsky (schwidefsky@de.ibm.com),
 *               Denis Joseph Barrow (djbarrow@de.ibm.com,barrow_dj@yahoo.com)
 *
 *  Derived from "arch/i386/kernel/time.c"
 *    Copyright (C) 1991, 1992, 1995  Linus Torvalds
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/profile.h>
#include <linux/timex.h>
#include <linux/notifier.h>

#include <asm/uaccess.h>
#include <asm/delay.h>
#include <asm/s390_ext.h>
#include <asm/div64.h>
#include <asm/irq.h>
#include <asm/timer.h>

/* change this if you have some constant time drift */
#define USECS_PER_JIFFY     ((unsigned long) 1000000/HZ)
#define CLK_TICKS_PER_JIFFY ((unsigned long) USECS_PER_JIFFY << 12)

/*
 * Create a small time difference between the timer interrupts
 * on the different cpus to avoid lock contention.
 */
#define CPU_DEVIATION       (smp_processor_id() << 12)

#define TICK_SIZE tick

u64 jiffies_64 = INITIAL_JIFFIES;

EXPORT_SYMBOL(jiffies_64);

static ext_int_info_t ext_int_info_cc;
static u64 init_timer_cc;
static u64 jiffies_timer_cc;
static u64 xtime_cc;

extern unsigned long wall_jiffies;

/*
 * Scheduler clock - returns current time in nanosec units.
 */
unsigned long long sched_clock(void)
{
	return ((get_clock() - jiffies_timer_cc) * 1000) >> 12;
}

void tod_to_timeval(__u64 todval, struct timespec *xtime)
{
	unsigned long long sec;

	sec = todval >> 12;
	do_div(sec, 1000000);
	xtime->tv_sec = sec;
	todval -= (sec * 1000000) << 12;
	xtime->tv_nsec = ((todval * 1000) >> 12);
}

static inline unsigned long do_gettimeoffset(void) 
{
	__u64 now;

        now = (get_clock() - jiffies_timer_cc) >> 12;
	/* We require the offset from the latest update of xtime */
	now -= (__u64) wall_jiffies*USECS_PER_JIFFY;
	return (unsigned long) now;
}

/*
 * This version of gettimeofday has microsecond resolution.
 */
void do_gettimeofday(struct timeval *tv)
{
	unsigned long flags;
	unsigned long seq;
	unsigned long usec, sec;

	do {
		seq = read_seqbegin_irqsave(&xtime_lock, flags);

		sec = xtime.tv_sec;
		usec = xtime.tv_nsec / 1000 + do_gettimeoffset();
	} while (read_seqretry_irqrestore(&xtime_lock, seq, flags));

	while (usec >= 1000000) {
		usec -= 1000000;
		sec++;
	}

	tv->tv_sec = sec;
	tv->tv_usec = usec;
}

EXPORT_SYMBOL(do_gettimeofday);

int do_settimeofday(struct timespec *tv)
{
	time_t wtm_sec, sec = tv->tv_sec;
	long wtm_nsec, nsec = tv->tv_nsec;

	if ((unsigned long)tv->tv_nsec >= NSEC_PER_SEC)
		return -EINVAL;

	write_seqlock_irq(&xtime_lock);
	/* This is revolting. We need to set the xtime.tv_nsec
	 * correctly. However, the value in this location is
	 * is value at the last tick.
	 * Discover what correction gettimeofday
	 * would have done, and then undo it!
	 */
	nsec -= do_gettimeoffset() * 1000;

	wtm_sec  = wall_to_monotonic.tv_sec + (xtime.tv_sec - sec);
	wtm_nsec = wall_to_monotonic.tv_nsec + (xtime.tv_nsec - nsec);

	set_normalized_timespec(&xtime, sec, nsec);
	set_normalized_timespec(&wall_to_monotonic, wtm_sec, wtm_nsec);

	time_adjust = 0;		/* stop active adjtime() */
	time_status |= STA_UNSYNC;
	time_maxerror = NTP_PHASE_LIMIT;
	time_esterror = NTP_PHASE_LIMIT;
	write_sequnlock_irq(&xtime_lock);
	clock_was_set();
	return 0;
}

EXPORT_SYMBOL(do_settimeofday);

#ifndef CONFIG_ARCH_S390X

static inline __u32
__calculate_ticks(__u64 elapsed)
{
	register_pair rp;

	rp.pair = elapsed >> 1;
	asm ("dr %0,%1" : "+d" (rp) : "d" (CLK_TICKS_PER_JIFFY >> 1));
	return rp.subreg.odd;
}

#else /* CONFIG_ARCH_S390X */

static inline __u32
__calculate_ticks(__u64 elapsed)
{
	return elapsed / CLK_TICKS_PER_JIFFY;
}

#endif /* CONFIG_ARCH_S390X */


#ifdef CONFIG_PROFILING
extern char _stext, _etext;

/*
 * The profiling function is SMP safe. (nothing can mess
 * around with "current", and the profiling counters are
 * updated with atomic operations). This is especially
 * useful with a profiling multiplier != 1
 */
static inline void s390_do_profile(struct pt_regs * regs)
{
	unsigned long eip;
	extern cpumask_t prof_cpu_mask;

	profile_hook(regs);

	if (user_mode(regs))
		return;

	if (!prof_buffer)
		return;

	eip = instruction_pointer(regs);

	/*
	 * Only measure the CPUs specified by /proc/irq/prof_cpu_mask.
	 * (default is all CPUs.)
	 */
	if (!cpu_isset(smp_processor_id(), prof_cpu_mask))
		return;

	eip -= (unsigned long) &_stext;
	eip >>= prof_shift;
	/*
	 * Don't ignore out-of-bounds EIP values silently,
	 * put them into the last histogram slot, so if
	 * present, they will show up as a sharp peak.
	 */
	if (eip > prof_len-1)
		eip = prof_len-1;
	atomic_inc((atomic_t *)&prof_buffer[eip]);
}
#else
#define s390_do_profile(regs)  do { ; } while(0)
#endif /* CONFIG_PROFILING */


/*
 * timer_interrupt() needs to keep up the real-time clock,
 * as well as call the "do_timer()" routine every clocktick
 */
void account_ticks(struct pt_regs *regs)
{
	__u64 tmp;
	__u32 ticks;

	/* Calculate how many ticks have passed. */
	if (S390_lowcore.int_clock < S390_lowcore.jiffy_timer)
		return;
	tmp = S390_lowcore.int_clock - S390_lowcore.jiffy_timer;
	if (tmp >= 2*CLK_TICKS_PER_JIFFY) {  /* more than two ticks ? */
		ticks = __calculate_ticks(tmp) + 1;
		S390_lowcore.jiffy_timer +=
			CLK_TICKS_PER_JIFFY * (__u64) ticks;
	} else if (tmp >= CLK_TICKS_PER_JIFFY) {
		ticks = 2;
		S390_lowcore.jiffy_timer += 2*CLK_TICKS_PER_JIFFY;
	} else {
		ticks = 1;
		S390_lowcore.jiffy_timer += CLK_TICKS_PER_JIFFY;
	}

	/* set clock comparator for next tick */
	tmp = S390_lowcore.jiffy_timer + CPU_DEVIATION;
        asm volatile ("SCKC %0" : : "m" (tmp));

#ifdef CONFIG_SMP
	/*
	 * Do not rely on the boot cpu to do the calls to do_timer.
	 * Spread it over all cpus instead.
	 */
	write_seqlock(&xtime_lock);
	if (S390_lowcore.jiffy_timer > xtime_cc) {
		__u32 xticks;

		tmp = S390_lowcore.jiffy_timer - xtime_cc;
		if (tmp >= 2*CLK_TICKS_PER_JIFFY) {
			xticks = __calculate_ticks(tmp);
			xtime_cc += (__u64) xticks * CLK_TICKS_PER_JIFFY;
		} else {
			xticks = 1;
			xtime_cc += CLK_TICKS_PER_JIFFY;
		}
		while (xticks--)
			do_timer(regs);
	}
	write_sequnlock(&xtime_lock);
	while (ticks--)
		update_process_times(user_mode(regs));
#else
	while (ticks--)
		do_timer(regs);
#endif
	s390_do_profile(regs);
}

#ifdef CONFIG_NO_IDLE_HZ

#ifdef CONFIG_NO_IDLE_HZ_INIT
int sysctl_hz_timer = 0;
#else
int sysctl_hz_timer = 1;
#endif

/*
 * Stop the HZ tick on the current CPU.
 * Only cpu_idle may call this function.
 */
static inline void stop_hz_timer(void)
{
	__u64 timer;

	if (sysctl_hz_timer != 0)
		return;

	/*
	 * Leave the clock comparator set up for the next timer
	 * tick if either rcu or a softirq is pending.
	 */
	if (rcu_pending(smp_processor_id()) || local_softirq_pending())
		return;

	/*
	 * This cpu is going really idle. Set up the clock comparator
	 * for the next event.
	 */
	cpu_set(smp_processor_id(), nohz_cpu_mask);
	timer = (__u64) (next_timer_interrupt() - jiffies) + jiffies_64;
	timer = jiffies_timer_cc + timer * CLK_TICKS_PER_JIFFY;
	asm volatile ("SCKC %0" : : "m" (timer));
}

/*
 * Start the HZ tick on the current CPU.
 * Only cpu_idle may call this function.
 */
static inline void start_hz_timer(void)
{
	if (!cpu_isset(smp_processor_id(), nohz_cpu_mask))
		return;
	account_ticks(__KSTK_PTREGS(current));
	cpu_clear(smp_processor_id(), nohz_cpu_mask);
}

static int nohz_idle_notify(struct notifier_block *self,
			    unsigned long action, void *hcpu)
{
	switch (action) {
	case CPU_IDLE:
		stop_hz_timer();
		break;
	case CPU_NOT_IDLE:
		start_hz_timer();
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block nohz_idle_nb = {
	.notifier_call = nohz_idle_notify,
};

void __init nohz_init(void)
{
	if (register_idle_notifier(&nohz_idle_nb))
		panic("Couldn't register idle notifier");
}

#endif

/*
 * Start the clock comparator on the current CPU.
 */
void init_cpu_timer(void)
{
	unsigned long cr0;
	__u64 timer;

	timer = jiffies_timer_cc + jiffies_64 * CLK_TICKS_PER_JIFFY;
	S390_lowcore.jiffy_timer = timer + CLK_TICKS_PER_JIFFY;
	timer += CLK_TICKS_PER_JIFFY + CPU_DEVIATION;
	asm volatile ("SCKC %0" : : "m" (timer));
        /* allow clock comparator timer interrupt */
	__ctl_store(cr0, 0, 0);
        cr0 |= 0x800;
	__ctl_load(cr0, 0, 0);
}

extern void vtime_init(void);

/*
 * Initialize the TOD clock and the CPU timer of
 * the boot cpu.
 */
void __init time_init(void)
{
	__u64 set_time_cc;
	int cc;

        /* kick the TOD clock */
        asm volatile ("STCK 0(%1)\n\t"
                      "IPM  %0\n\t"
                      "SRL  %0,28" : "=r" (cc) : "a" (&init_timer_cc) 
				   : "memory", "cc");
        switch (cc) {
        case 0: /* clock in set state: all is fine */
                break;
        case 1: /* clock in non-set state: FIXME */
                printk("time_init: TOD clock in non-set state\n");
                break;
        case 2: /* clock in error state: FIXME */
                printk("time_init: TOD clock in error state\n");
                break;
        case 3: /* clock in stopped or not-operational state: FIXME */
                printk("time_init: TOD clock stopped/non-operational\n");
                break;
        }
	jiffies_timer_cc = init_timer_cc - jiffies_64 * CLK_TICKS_PER_JIFFY;

	/* set xtime */
	xtime_cc = init_timer_cc + CLK_TICKS_PER_JIFFY;
	set_time_cc = init_timer_cc - 0x8126d60e46000000LL +
		(0x3c26700LL*1000000*4096);
        tod_to_timeval(set_time_cc, &xtime);
        set_normalized_timespec(&wall_to_monotonic,
                                -xtime.tv_sec, -xtime.tv_nsec);

	/* request the clock comparator external interrupt */
        if (register_early_external_interrupt(0x1004, 0,
					      &ext_int_info_cc) != 0)
                panic("Couldn't request external interrupt 0x1004");

        init_cpu_timer();

#ifdef CONFIG_NO_IDLE_HZ
	nohz_init();
#endif

#ifdef CONFIG_VIRT_TIMER
	vtime_init();
#endif
}

