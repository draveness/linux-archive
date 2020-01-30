/*
 * Support for periodic interrupts (100 per second) and for getting
 * the current time from the RTC on Power Macintoshes.
 *
 * We use the decrementer register for our periodic interrupts.
 *
 * Paul Mackerras	August 1996.
 * Copyright (C) 1996 Paul Mackerras.
 */
#include <linux/config.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/adb.h>
#include <linux/cuda.h>
#include <linux/pmu.h>

#include <asm/init.h>
#include <asm/prom.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/machdep.h>
#include <asm/hardirq.h>
#include <asm/time.h>
#include <asm/nvram.h>

extern rwlock_t xtime_lock;

/* Apparently the RTC stores seconds since 1 Jan 1904 */
#define RTC_OFFSET	2082844800

/*
 * Calibrate the decrementer frequency with the VIA timer 1.
 */
#define VIA_TIMER_FREQ_6	4700000	/* time 1 frequency * 6 */

/* VIA registers */
#define RS		0x200		/* skip between registers */
#define T1CL		(4*RS)		/* Timer 1 ctr/latch (low 8 bits) */
#define T1CH		(5*RS)		/* Timer 1 counter (high 8 bits) */
#define T1LL		(6*RS)		/* Timer 1 latch (low 8 bits) */
#define T1LH		(7*RS)		/* Timer 1 latch (high 8 bits) */
#define ACR		(11*RS)		/* Auxiliary control register */
#define IFR		(13*RS)		/* Interrupt flag register */

/* Bits in ACR */
#define T1MODE		0xc0		/* Timer 1 mode */
#define T1MODE_CONT	0x40		/*  continuous interrupts */

/* Bits in IFR and IER */
#define T1_INT		0x40		/* Timer 1 interrupt */

extern struct timezone sys_tz;

__init
long pmac_time_init(void)
{
#ifdef CONFIG_NVRAM
	s32 delta = 0;
	int dst;
	
	delta = ((s32)pmac_xpram_read(PMAC_XPRAM_MACHINE_LOC + 0x9)) << 16;
	delta |= ((s32)pmac_xpram_read(PMAC_XPRAM_MACHINE_LOC + 0xa)) << 8;
	delta |= pmac_xpram_read(PMAC_XPRAM_MACHINE_LOC + 0xb);
	if (delta & 0x00800000UL)
		delta |= 0xFF000000UL;
	dst = ((pmac_xpram_read(PMAC_XPRAM_MACHINE_LOC + 0x8) & 0x80) != 0);
	printk("GMT Delta read from XPRAM: %d minutes, DST: %s\n", delta/60,
		dst ? "on" : "off");
	return delta;
#else
	return 0;
#endif
}

__pmac
unsigned long pmac_get_rtc_time(void)
{
#if defined(CONFIG_ADB_CUDA) || defined(CONFIG_ADB_PMU)
	struct adb_request req;
	unsigned long now;
#endif

	/* Get the time from the RTC */
	switch (sys_ctrler) {
#ifdef CONFIG_ADB_CUDA
	case SYS_CTRLER_CUDA:
		if (cuda_request(&req, NULL, 2, CUDA_PACKET, CUDA_GET_TIME) < 0)
			return 0;
		while (!req.complete)
			cuda_poll();
		if (req.reply_len != 7)
			printk(KERN_ERR "pmac_get_rtc_time: got %d byte reply\n",
			       req.reply_len);
		now = (req.reply[3] << 24) + (req.reply[4] << 16)
			+ (req.reply[5] << 8) + req.reply[6];
		return now - RTC_OFFSET;
#endif /* CONFIG_ADB_CUDA */
#ifdef CONFIG_ADB_PMU
	case SYS_CTRLER_PMU:
		if (pmu_request(&req, NULL, 1, PMU_READ_RTC) < 0)
			return 0;
		while (!req.complete)
			pmu_poll();
		if (req.reply_len != 5)
			printk(KERN_ERR "pmac_get_rtc_time: got %d byte reply\n",
			       req.reply_len);
		now = (req.reply[1] << 24) + (req.reply[2] << 16)
			+ (req.reply[3] << 8) + req.reply[4];
		return now - RTC_OFFSET;
#endif /* CONFIG_ADB_PMU */
	default:
	}
	return 0;
}

int pmac_set_rtc_time(unsigned long nowtime)
{
#if defined(CONFIG_ADB_CUDA) || defined(CONFIG_ADB_PMU)
	struct adb_request req;
#endif

	nowtime += RTC_OFFSET;

	switch (sys_ctrler) {
#ifdef CONFIG_ADB_CUDA
	case SYS_CTRLER_CUDA:
		if (cuda_request(&req, NULL, 6, CUDA_PACKET, CUDA_SET_TIME,
				 nowtime >> 24, nowtime >> 16, nowtime >> 8, nowtime) < 0)
			return 0;
		while (!req.complete)
			cuda_poll();
//		if (req.reply_len != 7)
			printk(KERN_ERR "pmac_set_rtc_time: got %d byte reply\n",
			       req.reply_len);
		return 1;
#endif /* CONFIG_ADB_CUDA */
#ifdef CONFIG_ADB_PMU
	case SYS_CTRLER_PMU:
		if (pmu_request(&req, NULL, 5, PMU_SET_RTC,
				nowtime >> 24, nowtime >> 16, nowtime >> 8, nowtime) < 0)
			return 0;
		while (!req.complete)
			pmu_poll();
		if (req.reply_len != 0)
			printk(KERN_ERR "pmac_set_rtc_time: got %d byte reply\n",
			       req.reply_len);
		return 1;
#endif /* CONFIG_ADB_PMU */
	default:
		return 0;
	}
}

/*
 * Calibrate the decrementer register using VIA timer 1.
 * This is used both on powermacs and CHRP machines.
 */
int __init via_calibrate_decr(void)
{
	struct device_node *vias;
	volatile unsigned char *via;
	int count = VIA_TIMER_FREQ_6 / HZ;
	unsigned int dstart, dend;

	vias = find_devices("via-cuda");
	if (vias == 0)
		vias = find_devices("via-pmu");
	if (vias == 0)
		vias = find_devices("via");
	if (vias == 0 || vias->n_addrs == 0)
		return 0;
	via = (volatile unsigned char *)
		ioremap(vias->addrs[0].address, vias->addrs[0].size);

	/* set timer 1 for continuous interrupts */
	out_8(&via[ACR], (via[ACR] & ~T1MODE) | T1MODE_CONT);
	/* set the counter to a small value */
	out_8(&via[T1CH], 2);
	/* set the latch to `count' */
	out_8(&via[T1LL], count);
	out_8(&via[T1LH], count >> 8);
	/* wait until it hits 0 */
	while ((in_8(&via[IFR]) & T1_INT) == 0)
		;
	dstart = get_dec();
	/* clear the interrupt & wait until it hits 0 again */
	in_8(&via[T1CL]);
	while ((in_8(&via[IFR]) & T1_INT) == 0)
		;
	dend = get_dec();

	tb_ticks_per_jiffy = (dstart - dend) / 6;
	tb_to_us = mulhwu_scale_factor(dstart - dend, 60000);

	printk(KERN_INFO "via_calibrate_decr: ticks per jiffy = %u (%u ticks)\n",
	       tb_ticks_per_jiffy, dstart - dend);

	return 1;
}

#ifdef CONFIG_PMAC_PBOOK
/*
 * Reset the time after a sleep.
 */
static int time_sleep_notify(struct pmu_sleep_notifier *self, int when)
{
	static unsigned long time_diff;
	unsigned long flags;

	switch (when) {
	case PBOOK_SLEEP_NOW:
		read_lock_irqsave(&xtime_lock, flags);
		time_diff = xtime.tv_sec - pmac_get_rtc_time();
		read_unlock_irqrestore(&xtime_lock, flags);
		break;
	case PBOOK_WAKE:
		write_lock_irqsave(&xtime_lock, flags);
		xtime.tv_sec = pmac_get_rtc_time() + time_diff;
		set_dec(tb_ticks_per_jiffy);
		/* No currently-supported powerbook has a 601,
		   so use get_tbl, not native  */
		last_jiffy_stamp(0) = tb_last_stamp = get_tbl();
		xtime.tv_usec = 0;
		last_rtc_update = xtime.tv_sec;
		write_unlock_irqrestore(&xtime_lock, flags);
		break;
	}
	return PBOOK_SLEEP_OK;
}

static struct pmu_sleep_notifier time_sleep_notifier = {
	time_sleep_notify, SLEEP_LEVEL_MISC,
};
#endif /* CONFIG_PMAC_PBOOK */

/*
 * Query the OF and get the decr frequency.
 * This was taken from the pmac time_init() when merging the prep/pmac
 * time functions.
 */
void __init pmac_calibrate_decr(void)
{
	struct device_node *cpu;
	unsigned int freq, *fp;

#ifdef CONFIG_PMAC_PBOOK
	pmu_register_sleep_notifier(&time_sleep_notifier);
#endif /* CONFIG_PMAC_PBOOK */

	if (via_calibrate_decr())
		return;

	/*
	 * The cpu node should have a timebase-frequency property
	 * to tell us the rate at which the decrementer counts.
	 */
	cpu = find_type_devices("cpu");
	if (cpu == 0)
		panic("can't find cpu node in time_init");
	fp = (unsigned int *) get_property(cpu, "timebase-frequency", NULL);
	if (fp == 0)
		panic("can't get cpu timebase frequency");
	freq = *fp;
	printk("time_init: decrementer frequency = %u.%.6u MHz\n",
	       freq/1000000, freq%1000000);
	tb_ticks_per_jiffy = freq / HZ;
	tb_to_us = mulhwu_scale_factor(freq, 1000000);
}

