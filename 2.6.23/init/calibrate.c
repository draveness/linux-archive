/* calibrate.c: default delay calibration
 *
 * Excised from init/main.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/init.h>

#include <asm/timex.h>

static unsigned long preset_lpj;
static int __init lpj_setup(char *str)
{
	preset_lpj = simple_strtoul(str,NULL,0);
	return 1;
}

__setup("lpj=", lpj_setup);

#ifdef ARCH_HAS_READ_CURRENT_TIMER

/* This routine uses the read_current_timer() routine and gets the
 * loops per jiffy directly, instead of guessing it using delay().
 * Also, this code tries to handle non-maskable asynchronous events
 * (like SMIs)
 */
#define DELAY_CALIBRATION_TICKS			((HZ < 100) ? 1 : (HZ/100))
#define MAX_DIRECT_CALIBRATION_RETRIES		5

static unsigned long __devinit calibrate_delay_direct(void)
{
	unsigned long pre_start, start, post_start;
	unsigned long pre_end, end, post_end;
	unsigned long start_jiffies;
	unsigned long tsc_rate_min, tsc_rate_max;
	unsigned long good_tsc_sum = 0;
	unsigned long good_tsc_count = 0;
	int i;

	if (read_current_timer(&pre_start) < 0 )
		return 0;

	/*
	 * A simple loop like
	 *	while ( jiffies < start_jiffies+1)
	 *		start = read_current_timer();
	 * will not do. As we don't really know whether jiffy switch
	 * happened first or timer_value was read first. And some asynchronous
	 * event can happen between these two events introducing errors in lpj.
	 *
	 * So, we do
	 * 1. pre_start <- When we are sure that jiffy switch hasn't happened
	 * 2. check jiffy switch
	 * 3. start <- timer value before or after jiffy switch
	 * 4. post_start <- When we are sure that jiffy switch has happened
	 *
	 * Note, we don't know anything about order of 2 and 3.
	 * Now, by looking at post_start and pre_start difference, we can
	 * check whether any asynchronous event happened or not
	 */

	for (i = 0; i < MAX_DIRECT_CALIBRATION_RETRIES; i++) {
		pre_start = 0;
		read_current_timer(&start);
		start_jiffies = jiffies;
		while (jiffies <= (start_jiffies + 1)) {
			pre_start = start;
			read_current_timer(&start);
		}
		read_current_timer(&post_start);

		pre_end = 0;
		end = post_start;
		while (jiffies <=
		       (start_jiffies + 1 + DELAY_CALIBRATION_TICKS)) {
			pre_end = end;
			read_current_timer(&end);
		}
		read_current_timer(&post_end);

		tsc_rate_max = (post_end - pre_start) / DELAY_CALIBRATION_TICKS;
		tsc_rate_min = (pre_end - post_start) / DELAY_CALIBRATION_TICKS;

		/*
	 	 * If the upper limit and lower limit of the tsc_rate is
		 * >= 12.5% apart, redo calibration.
		 */
		if (pre_start != 0 && pre_end != 0 &&
		    (tsc_rate_max - tsc_rate_min) < (tsc_rate_max >> 3)) {
			good_tsc_count++;
			good_tsc_sum += tsc_rate_max;
		}
	}

	if (good_tsc_count)
		return (good_tsc_sum/good_tsc_count);

	printk(KERN_WARNING "calibrate_delay_direct() failed to get a good "
	       "estimate for loops_per_jiffy.\nProbably due to long platform interrupts. Consider using \"lpj=\" boot option.\n");
	return 0;
}
#else
static unsigned long __devinit calibrate_delay_direct(void) {return 0;}
#endif

/*
 * This is the number of bits of precision for the loops_per_jiffy.  Each
 * bit takes on average 1.5/HZ seconds.  This (like the original) is a little
 * better than 1%
 */
#define LPS_PREC 8

void __devinit calibrate_delay(void)
{
	unsigned long ticks, loopbit;
	int lps_precision = LPS_PREC;

	if (preset_lpj) {
		loops_per_jiffy = preset_lpj;
		printk("Calibrating delay loop (skipped)... "
			"%lu.%02lu BogoMIPS preset\n",
			loops_per_jiffy/(500000/HZ),
			(loops_per_jiffy/(5000/HZ)) % 100);
	} else if ((loops_per_jiffy = calibrate_delay_direct()) != 0) {
		printk("Calibrating delay using timer specific routine.. ");
		printk("%lu.%02lu BogoMIPS (lpj=%lu)\n",
			loops_per_jiffy/(500000/HZ),
			(loops_per_jiffy/(5000/HZ)) % 100,
			loops_per_jiffy);
	} else {
		loops_per_jiffy = (1<<12);

		printk(KERN_DEBUG "Calibrating delay loop... ");
		while ((loops_per_jiffy <<= 1) != 0) {
			/* wait for "start of" clock tick */
			ticks = jiffies;
			while (ticks == jiffies)
				/* nothing */;
			/* Go .. */
			ticks = jiffies;
			__delay(loops_per_jiffy);
			ticks = jiffies - ticks;
			if (ticks)
				break;
		}

		/*
		 * Do a binary approximation to get loops_per_jiffy set to
		 * equal one clock (up to lps_precision bits)
		 */
		loops_per_jiffy >>= 1;
		loopbit = loops_per_jiffy;
		while (lps_precision-- && (loopbit >>= 1)) {
			loops_per_jiffy |= loopbit;
			ticks = jiffies;
			while (ticks == jiffies)
				/* nothing */;
			ticks = jiffies;
			__delay(loops_per_jiffy);
			if (jiffies != ticks)	/* longer than 1 tick */
				loops_per_jiffy &= ~loopbit;
		}

		/* Round the value and print it */
		printk("%lu.%02lu BogoMIPS (lpj=%lu)\n",
			loops_per_jiffy/(500000/HZ),
			(loops_per_jiffy/(5000/HZ)) % 100,
			loops_per_jiffy);
	}

}
