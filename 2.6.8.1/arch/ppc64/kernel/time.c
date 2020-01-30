/*
 * 
 * Common time routines among all ppc machines.
 *
 * Written by Cort Dougan (cort@cs.nmt.edu) to merge
 * Paul Mackerras' version and mine for PReP and Pmac.
 * MPC8xx/MBX changes by Dan Malek (dmalek@jlc.net).
 * Converted for 64-bit by Mike Corrigan (mikejc@us.ibm.com)
 *
 * First round of bugfixes by Gabriel Paubert (paubert@iram.es)
 * to make clock more stable (2.4.0-test5). The only thing
 * that this code assumes is that the timebases have been synchronized
 * by firmware on SMP and are never stopped (never do sleep
 * on SMP then, nap and doze are OK).
 * 
 * Speeded up do_gettimeofday by getting rid of references to
 * xtime (which required locks for consistency). (mikejc@us.ibm.com)
 *
 * TODO (not necessarily in this file):
 * - improve precision and reproducibility of timebase frequency
 * measurement at boot time. (for iSeries, we calibrate the timebase
 * against the Titan chip's clock.)
 * - for astronomical applications: add a new function to get
 * non ambiguous timestamps even around leap seconds. This needs
 * a new timestamp format and a good name.
 *
 * 1997-09-10  Updated NTP code according to technical memorandum Jan '96
 *             "A Kernel Model for Precision Timekeeping" by Dave Mills
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
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
#include <linux/timex.h>
#include <linux/kernel_stat.h>
#include <linux/mc146818rtc.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/profile.h>

#include <asm/segment.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/nvram.h>
#include <asm/cache.h>
#include <asm/machdep.h>
#ifdef CONFIG_PPC_ISERIES
#include <asm/iSeries/ItLpQueue.h>
#include <asm/iSeries/HvCallXm.h>
#endif
#include <asm/uaccess.h>
#include <asm/time.h>
#include <asm/ppcdebug.h>
#include <asm/prom.h>
#include <asm/sections.h>

void smp_local_timer_interrupt(struct pt_regs *);

u64 jiffies_64 __cacheline_aligned_in_smp = INITIAL_JIFFIES;

EXPORT_SYMBOL(jiffies_64);

/* keep track of when we need to update the rtc */
time_t last_rtc_update;
extern int piranha_simulator;
#ifdef CONFIG_PPC_ISERIES
unsigned long iSeries_recal_titan = 0;
unsigned long iSeries_recal_tb = 0; 
static unsigned long first_settimeofday = 1;
#endif

#define XSEC_PER_SEC (1024*1024)

unsigned long tb_ticks_per_jiffy;
unsigned long tb_ticks_per_usec;
unsigned long tb_ticks_per_sec;
unsigned long next_xtime_sync_tb;
unsigned long xtime_sync_interval;
unsigned long tb_to_xs;
unsigned      tb_to_us;
unsigned long processor_freq;
spinlock_t rtc_lock = SPIN_LOCK_UNLOCKED;

unsigned long tb_to_ns_scale;
unsigned long tb_to_ns_shift;

struct gettimeofday_struct do_gtod;

extern unsigned long wall_jiffies;
extern unsigned long lpevent_count;
extern int smp_tb_synchronized;

void ppc_adjtimex(void);

static unsigned adjusting_time = 0;

/*
 * The profiling function is SMP safe. (nothing can mess
 * around with "current", and the profiling counters are
 * updated with atomic operations). This is especially
 * useful with a profiling multiplier != 1
 */
static inline void ppc64_do_profile(struct pt_regs *regs)
{
	unsigned long nip;
	extern unsigned long prof_cpu_mask;

	profile_hook(regs);

	if (user_mode(regs))
		return;

	if (!prof_buffer)
		return;

	nip = instruction_pointer(regs);

	/*
	 * Only measure the CPUs specified by /proc/irq/prof_cpu_mask.
	 * (default is all CPUs.)
	 */
	if (!((1<<smp_processor_id()) & prof_cpu_mask))
		return;

	nip -= (unsigned long)_stext;
	nip >>= prof_shift;
	/*
	 * Don't ignore out-of-bounds EIP values silently,
	 * put them into the last histogram slot, so if
	 * present, they will show up as a sharp peak.
	 */
	if (nip > prof_len-1)
		nip = prof_len-1;
	atomic_inc((atomic_t *)&prof_buffer[nip]);
}

static __inline__ void timer_check_rtc(void)
{
        /*
         * update the rtc when needed, this should be performed on the
         * right fraction of a second. Half or full second ?
         * Full second works on mk48t59 clocks, others need testing.
         * Note that this update is basically only used through 
         * the adjtimex system calls. Setting the HW clock in
         * any other way is a /dev/rtc and userland business.
         * This is still wrong by -0.5/+1.5 jiffies because of the
         * timer interrupt resolution and possible delay, but here we 
         * hit a quantization limit which can only be solved by higher
         * resolution timers and decoupling time management from timer
         * interrupts. This is also wrong on the clocks
         * which require being written at the half second boundary.
         * We should have an rtc call that only sets the minutes and
         * seconds like on Intel to avoid problems with non UTC clocks.
         */
        if ( (time_status & STA_UNSYNC) == 0 &&
             xtime.tv_sec - last_rtc_update >= 659 &&
             abs((xtime.tv_nsec/1000) - (1000000-1000000/HZ)) < 500000/HZ &&
             jiffies - wall_jiffies == 1) {
	    struct rtc_time tm;
	    to_tm(xtime.tv_sec+1, &tm);
	    tm.tm_year -= 1900;
	    tm.tm_mon -= 1;
            if (ppc_md.set_rtc_time(&tm) == 0)
                last_rtc_update = xtime.tv_sec+1;
            else
                /* Try again one minute later */
                last_rtc_update += 60;
        }
}

/* Synchronize xtime with do_gettimeofday */ 

static __inline__ void timer_sync_xtime( unsigned long cur_tb )
{
	struct timeval my_tv;

	if ( cur_tb > next_xtime_sync_tb ) {
		next_xtime_sync_tb = cur_tb + xtime_sync_interval;
		do_gettimeofday( &my_tv );
		if ( xtime.tv_sec <= my_tv.tv_sec ) {
			xtime.tv_sec = my_tv.tv_sec;
			xtime.tv_nsec = my_tv.tv_usec * 1000;
		}
	}
}

#ifdef CONFIG_PPC_ISERIES

/* 
 * This function recalibrates the timebase based on the 49-bit time-of-day
 * value in the Titan chip.  The Titan is much more accurate than the value
 * returned by the service processor for the timebase frequency.  
 */

static void iSeries_tb_recal(void)
{
	struct div_result divres;
	unsigned long titan, tb;
	tb = get_tb();
	titan = HvCallXm_loadTod();
	if ( iSeries_recal_titan ) {
		unsigned long tb_ticks = tb - iSeries_recal_tb;
		unsigned long titan_usec = (titan - iSeries_recal_titan) >> 12;
		unsigned long new_tb_ticks_per_sec   = (tb_ticks * USEC_PER_SEC)/titan_usec;
		unsigned long new_tb_ticks_per_jiffy = (new_tb_ticks_per_sec+(HZ/2))/HZ;
		long tick_diff = new_tb_ticks_per_jiffy - tb_ticks_per_jiffy;
		char sign = '+';		
		/* make sure tb_ticks_per_sec and tb_ticks_per_jiffy are consistent */
		new_tb_ticks_per_sec = new_tb_ticks_per_jiffy * HZ;

		if ( tick_diff < 0 ) {
			tick_diff = -tick_diff;
			sign = '-';
		}
		if ( tick_diff ) {
			if ( tick_diff < tb_ticks_per_jiffy/25 ) {
				printk( "Titan recalibrate: new tb_ticks_per_jiffy = %lu (%c%ld)\n",
						new_tb_ticks_per_jiffy, sign, tick_diff );
				tb_ticks_per_jiffy = new_tb_ticks_per_jiffy;
				tb_ticks_per_sec   = new_tb_ticks_per_sec;
				div128_by_32( XSEC_PER_SEC, 0, tb_ticks_per_sec, &divres );
				do_gtod.tb_ticks_per_sec = tb_ticks_per_sec;
				tb_to_xs = divres.result_low;
				do_gtod.varp->tb_to_xs = tb_to_xs;
			}
			else {
				printk( "Titan recalibrate: FAILED (difference > 4 percent)\n"
					"                   new tb_ticks_per_jiffy = %lu\n"
					"                   old tb_ticks_per_jiffy = %lu\n",
					new_tb_ticks_per_jiffy, tb_ticks_per_jiffy );
			}
		}
	}
	iSeries_recal_titan = titan;
	iSeries_recal_tb = tb;
}
#endif

/*
 * For iSeries shared processors, we have to let the hypervisor
 * set the hardware decrementer.  We set a virtual decrementer
 * in the ItLpPaca and call the hypervisor if the virtual
 * decrementer is less than the current value in the hardware
 * decrementer. (almost always the new decrementer value will
 * be greater than the current hardware decementer so the hypervisor
 * call will not be needed)
 */

unsigned long tb_last_stamp __cacheline_aligned_in_smp;

/*
 * timer_interrupt - gets called when the decrementer overflows,
 * with interrupts disabled.
 */
int timer_interrupt(struct pt_regs * regs)
{
	int next_dec;
	unsigned long cur_tb;
	struct paca_struct *lpaca = get_paca();
	unsigned long cpu = smp_processor_id();

	irq_enter();

#ifndef CONFIG_PPC_ISERIES
	ppc64_do_profile(regs);
#endif

	lpaca->lppaca.xIntDword.xFields.xDecrInt = 0;

	while (lpaca->next_jiffy_update_tb <= (cur_tb = get_tb())) {

#ifdef CONFIG_SMP
		smp_local_timer_interrupt(regs);
#endif
		if (cpu == boot_cpuid) {
			write_seqlock(&xtime_lock);
			tb_last_stamp = lpaca->next_jiffy_update_tb;
			do_timer(regs);
			timer_sync_xtime( cur_tb );
			timer_check_rtc();
			write_sequnlock(&xtime_lock);
			if ( adjusting_time && (time_adjust == 0) )
				ppc_adjtimex();
		}
		lpaca->next_jiffy_update_tb += tb_ticks_per_jiffy;
	}
	
	next_dec = lpaca->next_jiffy_update_tb - cur_tb;
	if (next_dec > lpaca->default_decr)
        	next_dec = lpaca->default_decr;
	set_dec(next_dec);

#ifdef CONFIG_PPC_ISERIES
	{
		struct ItLpQueue *lpq = lpaca->lpqueue_ptr;
		if (lpq && ItLpQueue_isLpIntPending(lpq))
			lpevent_count += ItLpQueue_process(lpq, regs);
	}
#endif

	irq_exit();

	return 1;
}

/*
 * Scheduler clock - returns current time in nanosec units.
 *
 * Note: mulhdu(a, b) (multiply high double unsigned) returns
 * the high 64 bits of a * b, i.e. (a * b) >> 64, where a and b
 * are 64-bit unsigned numbers.
 */
unsigned long long sched_clock(void)
{
	return mulhdu(get_tb(), tb_to_ns_scale) << tb_to_ns_shift;
}

/*
 * This version of gettimeofday has microsecond resolution.
 */
void do_gettimeofday(struct timeval *tv)
{
        unsigned long sec, usec, tb_ticks;
	unsigned long xsec, tb_xsec;
	struct gettimeofday_vars * temp_varp;
	unsigned long temp_tb_to_xs, temp_stamp_xsec;

	/* These calculations are faster (gets rid of divides)
	 * if done in units of 1/2^20 rather than microseconds.
	 * The conversion to microseconds at the end is done
	 * without a divide (and in fact, without a multiply) */
	tb_ticks = get_tb() - do_gtod.tb_orig_stamp;
	temp_varp = do_gtod.varp;
	temp_tb_to_xs = temp_varp->tb_to_xs;
	temp_stamp_xsec = temp_varp->stamp_xsec;
	tb_xsec = mulhdu( tb_ticks, temp_tb_to_xs );
	xsec = temp_stamp_xsec + tb_xsec;
	sec = xsec / XSEC_PER_SEC;
	xsec -= sec * XSEC_PER_SEC;
	usec = (xsec * USEC_PER_SEC)/XSEC_PER_SEC;

        tv->tv_sec = sec;
        tv->tv_usec = usec;
}

EXPORT_SYMBOL(do_gettimeofday);

int do_settimeofday(struct timespec *tv)
{
	time_t wtm_sec, new_sec = tv->tv_sec;
	long wtm_nsec, new_nsec = tv->tv_nsec;
	unsigned long flags;
	unsigned long delta_xsec;
	long int tb_delta;
	unsigned long new_xsec;

	if ((unsigned long)tv->tv_nsec >= NSEC_PER_SEC)
		return -EINVAL;

	write_seqlock_irqsave(&xtime_lock, flags);
	/* Updating the RTC is not the job of this code. If the time is
	 * stepped under NTP, the RTC will be update after STA_UNSYNC
	 * is cleared. Tool like clock/hwclock either copy the RTC
	 * to the system time, in which case there is no point in writing
	 * to the RTC again, or write to the RTC but then they don't call
	 * settimeofday to perform this operation.
	 */
#ifdef CONFIG_PPC_ISERIES
	if ( first_settimeofday ) {
		iSeries_tb_recal();
		first_settimeofday = 0;
	}
#endif
	tb_delta = tb_ticks_since(tb_last_stamp);
	tb_delta += (jiffies - wall_jiffies) * tb_ticks_per_jiffy;

	new_nsec -= tb_delta / tb_ticks_per_usec / 1000;

	wtm_sec  = wall_to_monotonic.tv_sec + (xtime.tv_sec - new_sec);
	wtm_nsec = wall_to_monotonic.tv_nsec + (xtime.tv_nsec - new_nsec);

 	set_normalized_timespec(&xtime, new_sec, new_nsec);
	set_normalized_timespec(&wall_to_monotonic, wtm_sec, wtm_nsec);

	/* In case of a large backwards jump in time with NTP, we want the 
	 * clock to be updated as soon as the PLL is again in lock.
	 */
	last_rtc_update = new_sec - 658;

	time_adjust = 0;                /* stop active adjtime() */
	time_status |= STA_UNSYNC;
	time_maxerror = NTP_PHASE_LIMIT;
	time_esterror = NTP_PHASE_LIMIT;

	delta_xsec = mulhdu( (tb_last_stamp-do_gtod.tb_orig_stamp), do_gtod.varp->tb_to_xs );
	new_xsec = (new_nsec * XSEC_PER_SEC) / NSEC_PER_SEC;
	new_xsec += new_sec * XSEC_PER_SEC;
	if ( new_xsec > delta_xsec ) {
		do_gtod.varp->stamp_xsec = new_xsec - delta_xsec;
	}
	else {
		/* This is only for the case where the user is setting the time
		 * way back to a time such that the boot time would have been
		 * before 1970 ... eg. we booted ten days ago, and we are setting
		 * the time to Jan 5, 1970 */
		do_gtod.varp->stamp_xsec = new_xsec;
		do_gtod.tb_orig_stamp = tb_last_stamp;
	}

	write_sequnlock_irqrestore(&xtime_lock, flags);
	clock_was_set();
	return 0;
}

EXPORT_SYMBOL(do_settimeofday);

/*
 * This function is a copy of the architecture independent function
 * but which calls do_settimeofday rather than setting the xtime
 * fields itself.  This way, the fields which are used for 
 * do_settimeofday get updated too.
 */
long ppc64_sys32_stime(int __user * tptr)
{
	int value;
	struct timespec myTimeval;

	if (!capable(CAP_SYS_TIME))
		return -EPERM;

	if (get_user(value, tptr))
		return -EFAULT;

	myTimeval.tv_sec = value;
	myTimeval.tv_nsec = 0;

	do_settimeofday(&myTimeval);

	return 0;
}

/*
 * This function is a copy of the architecture independent function
 * but which calls do_settimeofday rather than setting the xtime
 * fields itself.  This way, the fields which are used for 
 * do_settimeofday get updated too.
 */
long ppc64_sys_stime(long __user * tptr)
{
	long value;
	struct timespec myTimeval;

	if (!capable(CAP_SYS_TIME))
		return -EPERM;

	if (get_user(value, tptr))
		return -EFAULT;

	myTimeval.tv_sec = value;
	myTimeval.tv_nsec = 0;

	do_settimeofday(&myTimeval);

	return 0;
}

void __init time_init(void)
{
	/* This function is only called on the boot processor */
	unsigned long flags;
	struct rtc_time tm;
	struct div_result res;
	unsigned long scale, shift;

	ppc_md.calibrate_decr();

	/*
	 * Compute scale factor for sched_clock.
	 * The calibrate_decr() function has set tb_ticks_per_sec,
	 * which is the timebase frequency.
	 * We compute 1e9 * 2^64 / tb_ticks_per_sec and interpret
	 * the 128-bit result as a 64.64 fixed-point number.
	 * We then shift that number right until it is less than 1.0,
	 * giving us the scale factor and shift count to use in
	 * sched_clock().
	 */
	div128_by_32(1000000000, 0, tb_ticks_per_sec, &res);
	scale = res.result_low;
	for (shift = 0; res.result_high != 0; ++shift) {
		scale = (scale >> 1) | (res.result_high << 63);
		res.result_high >>= 1;
	}
	tb_to_ns_scale = scale;
	tb_to_ns_shift = shift;

#ifdef CONFIG_PPC_ISERIES
	if (!piranha_simulator)
#endif
		ppc_md.get_boot_time(&tm);

	write_seqlock_irqsave(&xtime_lock, flags);
	xtime.tv_sec = mktime(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			      tm.tm_hour, tm.tm_min, tm.tm_sec);
	tb_last_stamp = get_tb();
	do_gtod.tb_orig_stamp = tb_last_stamp;
	do_gtod.varp = &do_gtod.vars[0];
	do_gtod.var_idx = 0;
	do_gtod.varp->stamp_xsec = xtime.tv_sec * XSEC_PER_SEC;
	do_gtod.tb_ticks_per_sec = tb_ticks_per_sec;
	do_gtod.varp->tb_to_xs = tb_to_xs;
	do_gtod.tb_to_us = tb_to_us;

	xtime_sync_interval = tb_ticks_per_sec - (tb_ticks_per_sec/8);
	next_xtime_sync_tb = tb_last_stamp + xtime_sync_interval;

	time_freq = 0;

	xtime.tv_nsec = 0;
	last_rtc_update = xtime.tv_sec;
	set_normalized_timespec(&wall_to_monotonic,
	                        -xtime.tv_sec, -xtime.tv_nsec);
	write_sequnlock_irqrestore(&xtime_lock, flags);

	/* Not exact, but the timer interrupt takes care of this */
	set_dec(tb_ticks_per_jiffy);
}

/* 
 * After adjtimex is called, adjust the conversion of tb ticks
 * to microseconds to keep do_gettimeofday synchronized 
 * with ntpd.
 *
 * Use the time_adjust, time_freq and time_offset computed by adjtimex to 
 * adjust the frequency.
 */

/* #define DEBUG_PPC_ADJTIMEX 1 */

void ppc_adjtimex(void)
{
	unsigned long den, new_tb_ticks_per_sec, tb_ticks, old_xsec, new_tb_to_xs, new_xsec, new_stamp_xsec;
	unsigned long tb_ticks_per_sec_delta;
	long delta_freq, ltemp;
	struct div_result divres; 
	unsigned long flags;
	struct gettimeofday_vars * temp_varp;
	unsigned temp_idx;
	long singleshot_ppm = 0;

	/* Compute parts per million frequency adjustment to accomplish the time adjustment
	   implied by time_offset to be applied over the elapsed time indicated by time_constant.
	   Use SHIFT_USEC to get it into the same units as time_freq. */
	if ( time_offset < 0 ) {
		ltemp = -time_offset;
		ltemp <<= SHIFT_USEC - SHIFT_UPDATE;
		ltemp >>= SHIFT_KG + time_constant;
		ltemp = -ltemp;
	}
	else {
		ltemp = time_offset;
		ltemp <<= SHIFT_USEC - SHIFT_UPDATE;
		ltemp >>= SHIFT_KG + time_constant;
	}
	
	/* If there is a single shot time adjustment in progress */
	if ( time_adjust ) {
#ifdef DEBUG_PPC_ADJTIMEX
		printk("ppc_adjtimex: ");
		if ( adjusting_time == 0 )
			printk("starting ");
		printk("single shot time_adjust = %ld\n", time_adjust);
#endif	
	
		adjusting_time = 1;
		
		/* Compute parts per million frequency adjustment to match time_adjust */
		singleshot_ppm = tickadj * HZ;	
		/*
		 * The adjustment should be tickadj*HZ to match the code in
		 * linux/kernel/timer.c, but experiments show that this is too
		 * large. 3/4 of tickadj*HZ seems about right
		 */
		singleshot_ppm -= singleshot_ppm / 4;
		/* Use SHIFT_USEC to get it into the same units as time_freq */	
		singleshot_ppm <<= SHIFT_USEC;
		if ( time_adjust < 0 )
			singleshot_ppm = -singleshot_ppm;
	}
	else {
#ifdef DEBUG_PPC_ADJTIMEX
		if ( adjusting_time )
			printk("ppc_adjtimex: ending single shot time_adjust\n");
#endif
		adjusting_time = 0;
	}
	
	/* Add up all of the frequency adjustments */
	delta_freq = time_freq + ltemp + singleshot_ppm;
	
	/* Compute a new value for tb_ticks_per_sec based on the frequency adjustment */
	den = 1000000 * (1 << (SHIFT_USEC - 8));
	if ( delta_freq < 0 ) {
		tb_ticks_per_sec_delta = ( tb_ticks_per_sec * ( (-delta_freq) >> (SHIFT_USEC - 8))) / den;
		new_tb_ticks_per_sec = tb_ticks_per_sec + tb_ticks_per_sec_delta;
	}
	else {
		tb_ticks_per_sec_delta = ( tb_ticks_per_sec * ( delta_freq >> (SHIFT_USEC - 8))) / den;
		new_tb_ticks_per_sec = tb_ticks_per_sec - tb_ticks_per_sec_delta;
	}
	
#ifdef DEBUG_PPC_ADJTIMEX
	printk("ppc_adjtimex: ltemp = %ld, time_freq = %ld, singleshot_ppm = %ld\n", ltemp, time_freq, singleshot_ppm);
	printk("ppc_adjtimex: tb_ticks_per_sec - base = %ld  new = %ld\n", tb_ticks_per_sec, new_tb_ticks_per_sec);
#endif
				
	/* Compute a new value of tb_to_xs (used to convert tb to microseconds and a new value of 
	   stamp_xsec which is the time (in 1/2^20 second units) corresponding to tb_orig_stamp.  This 
	   new value of stamp_xsec compensates for the change in frequency (implied by the new tb_to_xs)
	   which guarantees that the current time remains the same */ 
	tb_ticks = get_tb() - do_gtod.tb_orig_stamp;
	div128_by_32( 1024*1024, 0, new_tb_ticks_per_sec, &divres );
	new_tb_to_xs = divres.result_low;
	new_xsec = mulhdu( tb_ticks, new_tb_to_xs );

	write_seqlock_irqsave( &xtime_lock, flags );
	old_xsec = mulhdu( tb_ticks, do_gtod.varp->tb_to_xs );
	new_stamp_xsec = do_gtod.varp->stamp_xsec + old_xsec - new_xsec;

	/* There are two copies of tb_to_xs and stamp_xsec so that no lock is needed to access and use these
	   values in do_gettimeofday.  We alternate the copies and as long as a reasonable time elapses between
	   changes, there will never be inconsistent values.  ntpd has a minimum of one minute between updates */

	if (do_gtod.var_idx == 0) {
		temp_varp = &do_gtod.vars[1];
		temp_idx  = 1;
	}
	else {
		temp_varp = &do_gtod.vars[0];
		temp_idx  = 0;
	}
	temp_varp->tb_to_xs = new_tb_to_xs;
	temp_varp->stamp_xsec = new_stamp_xsec;
	mb();
	do_gtod.varp = temp_varp;
	do_gtod.var_idx = temp_idx;

	write_sequnlock_irqrestore( &xtime_lock, flags );

}


#define TICK_SIZE tick
#define FEBRUARY	2
#define	STARTOFTIME	1970
#define SECDAY		86400L
#define SECYR		(SECDAY * 365)
#define	leapyear(year)		((year) % 4 == 0)
#define	days_in_year(a) 	(leapyear(a) ? 366 : 365)
#define	days_in_month(a) 	(month_days[(a) - 1])

static int month_days[12] = {
	31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/*
 * This only works for the Gregorian calendar - i.e. after 1752 (in the UK)
 */
void GregorianDay(struct rtc_time * tm)
{
	int leapsToDate;
	int lastYear;
	int day;
	int MonthOffset[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

	lastYear=tm->tm_year-1;

	/*
	 * Number of leap corrections to apply up to end of last year
	 */
	leapsToDate = lastYear/4 - lastYear/100 + lastYear/400;

	/*
	 * This year is a leap year if it is divisible by 4 except when it is
	 * divisible by 100 unless it is divisible by 400
	 *
	 * e.g. 1904 was a leap year, 1900 was not, 1996 is, and 2000 will be
	 */
	if((tm->tm_year%4==0) &&
	   ((tm->tm_year%100!=0) || (tm->tm_year%400==0)) &&
	   (tm->tm_mon>2))
	{
		/*
		 * We are past Feb. 29 in a leap year
		 */
		day=1;
	}
	else
	{
		day=0;
	}

	day += lastYear*365 + leapsToDate + MonthOffset[tm->tm_mon-1] +
		   tm->tm_mday;

	tm->tm_wday=day%7;
}

void to_tm(int tim, struct rtc_time * tm)
{
	register int    i;
	register long   hms, day;

	day = tim / SECDAY;
	hms = tim % SECDAY;

	/* Hours, minutes, seconds are easy */
	tm->tm_hour = hms / 3600;
	tm->tm_min = (hms % 3600) / 60;
	tm->tm_sec = (hms % 3600) % 60;

	/* Number of years in days */
	for (i = STARTOFTIME; day >= days_in_year(i); i++)
		day -= days_in_year(i);
	tm->tm_year = i;

	/* Number of months in days left */
	if (leapyear(tm->tm_year))
		days_in_month(FEBRUARY) = 29;
	for (i = 1; day >= days_in_month(i); i++)
		day -= days_in_month(i);
	days_in_month(FEBRUARY) = 28;
	tm->tm_mon = i;

	/* Days are what is left over (+1) from all that. */
	tm->tm_mday = day + 1;

	/*
	 * Determine the day of week
	 */
	GregorianDay(tm);
}

/* Auxiliary function to compute scaling factors */
/* Actually the choice of a timebase running at 1/4 the of the bus
 * frequency giving resolution of a few tens of nanoseconds is quite nice.
 * It makes this computation very precise (27-28 bits typically) which
 * is optimistic considering the stability of most processor clock
 * oscillators and the precision with which the timebase frequency
 * is measured but does not harm.
 */
unsigned mulhwu_scale_factor(unsigned inscale, unsigned outscale) {
        unsigned mlt=0, tmp, err;
        /* No concern for performance, it's done once: use a stupid
         * but safe and compact method to find the multiplier.
         */
  
        for (tmp = 1U<<31; tmp != 0; tmp >>= 1) {
                if (mulhwu(inscale, mlt|tmp) < outscale) mlt|=tmp;
        }
  
        /* We might still be off by 1 for the best approximation.
         * A side effect of this is that if outscale is too large
         * the returned value will be zero.
         * Many corner cases have been checked and seem to work,
         * some might have been forgotten in the test however.
         */
  
        err = inscale*(mlt+1);
        if (err <= inscale/2) mlt++;
        return mlt;
  }

/*
 * Divide a 128-bit dividend by a 32-bit divisor, leaving a 128 bit
 * result.
 */

void div128_by_32( unsigned long dividend_high, unsigned long dividend_low,
		   unsigned divisor, struct div_result *dr )
{
	unsigned long a,b,c,d, w,x,y,z, ra,rb,rc;

	a = dividend_high >> 32;
	b = dividend_high & 0xffffffff;
	c = dividend_low >> 32;
	d = dividend_low & 0xffffffff;

	w = a/divisor;
	ra = (a - (w * divisor)) << 32;

	x = (ra + b)/divisor;
	rb = ((ra + b) - (x * divisor)) << 32;

	y = (rb + c)/divisor;
	rc = ((rb + b) - (y * divisor)) << 32;

	z = (rc + d)/divisor;

	dr->result_high = (w << 32) + x;
	dr->result_low  = (y << 32) + z;

}

