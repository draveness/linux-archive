/* $Id: ds1286.c,v 1.6 1999/10/09 00:01:31 ralf Exp $
 *
 *	Real Time Clock interface for Linux	
 *
 *	Copyright (C) 1998, 1999 Ralf Baechle
 *	
 *	Based on code written by Paul Gortmaker.
 *
 *	This driver allows use of the real time clock (built into
 *	nearly all computers) from user space. It exports the /dev/rtc
 *	interface supporting various ioctl() and also the /proc/rtc
 *	pseudo-file for status information.
 *
 *	The ioctls can be used to set the interrupt behaviour and
 *	generation rate from the RTC via IRQ 8. Then the /dev/rtc
 *	interface can be used to make use of these timer interrupts,
 *	be they interval or alarm based.
 *
 *	The /dev/rtc interface will block on reads until an interrupt
 *	has been received. If a RTC interrupt has already happened,
 *	it will output an unsigned long and then block. The output value
 *	contains the interrupt status in the low byte and the number of
 *	interrupts since the last read in the remaining high bytes. The 
 *	/dev/rtc interface can also be used with the select(2) call.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/malloc.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/rtc.h>
#include <linux/spinlock.h>
#include <linux/smp_lock.h>

#include <asm/ds1286.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#define DS1286_VERSION		"1.0"

/*
 *	We sponge a minor off of the misc major. No need slurping
 *	up another valuable major dev number for this. If you add
 *	an ioctl, make sure you don't conflict with SPARC's RTC
 *	ioctls.
 */

static DECLARE_WAIT_QUEUE_HEAD(ds1286_wait);

static long long ds1286_llseek(struct file *file, loff_t offset, int origin);

static ssize_t ds1286_read(struct file *file, char *buf,
			size_t count, loff_t *ppos);

static int ds1286_ioctl(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg);

static unsigned int ds1286_poll(struct file *file, poll_table *wait);

void get_rtc_time (struct rtc_time *rtc_tm);
void get_rtc_alm_time (struct rtc_time *alm_tm);

void set_rtc_irq_bit(unsigned char bit);
void clear_rtc_irq_bit(unsigned char bit);

static inline unsigned char ds1286_is_updating(void);

static spinlock_t ds1286_lock = SPIN_LOCK_UNLOCKED;

/*
 *	Bits in rtc_status. (7 bits of room for future expansion)
 */

#define RTC_IS_OPEN		0x01	/* means /dev/rtc is in use	*/
#define RTC_TIMER_ON		0x02	/* missed irq timer active	*/

unsigned char ds1286_status = 0;	/* bitmapped status byte.	*/
unsigned long ds1286_freq = 0;		/* Current periodic IRQ rate	*/
unsigned long ds1286_irq_data = 0;	/* our output to the world	*/

unsigned char days_in_mo[] = 
{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/*
 *	A very tiny interrupt handler. It runs with SA_INTERRUPT set,
 *	so that there is no possibility of conflicting with the
 *	set_rtc_mmss() call that happens during some timer interrupts.
 *	(See ./arch/XXXX/kernel/time.c for the set_rtc_mmss() function.)
 */

/*
 *	Now all the various file operations that we export.
 */

static long long ds1286_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}

static ssize_t ds1286_read(struct file *file, char *buf,
                           size_t count, loff_t *ppos)
{
	DECLARE_WAITQUEUE(wait, current);
	unsigned long data;
	ssize_t retval;
	
	if (count < sizeof(unsigned long))
		return -EINVAL;

	add_wait_queue(&ds1286_wait, &wait);

	current->state = TASK_INTERRUPTIBLE;
		
	while ((data = xchg(&ds1286_irq_data, 0)) == 0) {
		if (file->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			goto out;
		}
		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			goto out;
		}
		schedule();
	}

	retval = put_user(data, (unsigned long *)buf); 
	if (!retval)
		retval = sizeof(unsigned long); 
 out:
	current->state = TASK_RUNNING;
	remove_wait_queue(&ds1286_wait, &wait);

	return retval;
}

static int ds1286_ioctl(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg)
{

	struct rtc_time wtime; 

	switch (cmd) {
	case RTC_AIE_OFF:	/* Mask alarm int. enab. bit	*/
	{
		unsigned int flags;
		unsigned char val;

		if (!capable(CAP_SYS_TIME))
			return -EACCES;

		spin_lock_irqsave(&ds1286_lock, flags);
		val = CMOS_READ(RTC_CMD);
		val |=  RTC_TDM;
		CMOS_WRITE(val, RTC_CMD);
		spin_unlock_irqrestore(&ds1286_lock, flags);

		return 0;
	}
	case RTC_AIE_ON:	/* Allow alarm interrupts.	*/
	{
		unsigned int flags;
		unsigned char val;

		if (!capable(CAP_SYS_TIME))
			return -EACCES;

		spin_lock_irqsave(&ds1286_lock, flags);
		val = CMOS_READ(RTC_CMD);
		val &=  ~RTC_TDM;
		CMOS_WRITE(val, RTC_CMD);
		spin_unlock_irqrestore(&ds1286_lock, flags);

		return 0;
	}
	case RTC_WIE_OFF:	/* Mask watchdog int. enab. bit	*/
	{
		unsigned int flags;
		unsigned char val;

		if (!capable(CAP_SYS_TIME))
			return -EACCES;

		spin_lock_irqsave(&ds1286_lock, flags);
		val = CMOS_READ(RTC_CMD);
		val |= RTC_WAM;
		CMOS_WRITE(val, RTC_CMD);
		spin_unlock_irqrestore(&ds1286_lock, flags);

		return 0;
	}
	case RTC_WIE_ON:	/* Allow watchdog interrupts.	*/
	{
		unsigned int flags;
		unsigned char val;

		if (!capable(CAP_SYS_TIME))
			return -EACCES;

		spin_lock_irqsave(&ds1286_lock, flags);
		val = CMOS_READ(RTC_CMD);
		val &= ~RTC_WAM;
		CMOS_WRITE(val, RTC_CMD);
		spin_unlock_irqrestore(&ds1286_lock, flags);

		return 0;
	}
	case RTC_ALM_READ:	/* Read the present alarm time */
	{
		/*
		 * This returns a struct rtc_time. Reading >= 0xc0
		 * means "don't care" or "match all". Only the tm_hour,
		 * tm_min, and tm_sec values are filled in.
		 */

		get_rtc_alm_time(&wtime);
		break; 
	}
	case RTC_ALM_SET:	/* Store a time into the alarm */
	{
		/*
		 * This expects a struct rtc_time. Writing 0xff means
		 * "don't care" or "match all". Only the tm_hour,
		 * tm_min and tm_sec are used.
		 */
		unsigned char hrs, min, sec;
		struct rtc_time alm_tm;

		if (!capable(CAP_SYS_TIME))
			return -EACCES;

		if (copy_from_user(&alm_tm, (struct rtc_time*)arg,
				   sizeof(struct rtc_time)))
			return -EFAULT;

		hrs = alm_tm.tm_hour;
		min = alm_tm.tm_min;

		if (hrs >= 24)
			hrs = 0xff;

		if (min >= 60)
			min = 0xff;

		BIN_TO_BCD(sec);
		BIN_TO_BCD(min);
		BIN_TO_BCD(hrs);

		spin_lock(&ds1286_lock);
		CMOS_WRITE(hrs, RTC_HOURS_ALARM);
		CMOS_WRITE(min, RTC_MINUTES_ALARM);
		spin_unlock(&ds1286_lock);

		return 0;
	}
	case RTC_RD_TIME:	/* Read the time/date from RTC	*/
	{
		get_rtc_time(&wtime);
		break;
	}
	case RTC_SET_TIME:	/* Set the RTC */
	{
		struct rtc_time rtc_tm;
		unsigned char mon, day, hrs, min, sec, leap_yr;
		unsigned char save_control;
		unsigned int yrs, flags;

		if (!capable(CAP_SYS_TIME))
			return -EACCES;

		if (copy_from_user(&rtc_tm, (struct rtc_time*)arg,
				   sizeof(struct rtc_time)))
			return -EFAULT;

		yrs = rtc_tm.tm_year + 1900;
		mon = rtc_tm.tm_mon + 1;   /* tm_mon starts at zero */
		day = rtc_tm.tm_mday;
		hrs = rtc_tm.tm_hour;
		min = rtc_tm.tm_min;
		sec = rtc_tm.tm_sec;

		if (yrs < 1970)
			return -EINVAL;

		leap_yr = ((!(yrs % 4) && (yrs % 100)) || !(yrs % 400));

		if ((mon > 12) || (day == 0))
			return -EINVAL;

		if (day > (days_in_mo[mon] + ((mon == 2) && leap_yr)))
			return -EINVAL;

		if ((hrs >= 24) || (min >= 60) || (sec >= 60))
			return -EINVAL;

		if ((yrs -= 1940) > 255)    /* They are unsigned */
			return -EINVAL;

		if (yrs >= 100)
			yrs -= 100;

		BIN_TO_BCD(sec);
		BIN_TO_BCD(min);
		BIN_TO_BCD(hrs);
		BIN_TO_BCD(day);
		BIN_TO_BCD(mon);
		BIN_TO_BCD(yrs);

		spin_lock_irqsave(&ds1286_lock, flags);
		save_control = CMOS_READ(RTC_CMD);
		CMOS_WRITE((save_control|RTC_TE), RTC_CMD);

		CMOS_WRITE(yrs, RTC_YEAR);
		CMOS_WRITE(mon, RTC_MONTH);
		CMOS_WRITE(day, RTC_DATE);
		CMOS_WRITE(hrs, RTC_HOURS);
		CMOS_WRITE(min, RTC_MINUTES);
		CMOS_WRITE(sec, RTC_SECONDS);
		CMOS_WRITE(0, RTC_HUNDREDTH_SECOND);

		CMOS_WRITE(save_control, RTC_CMD);
		spin_unlock_irqrestore(&ds1286_lock, flags);

		return 0;
	}
	default:
		return -EINVAL;
	}
	return copy_to_user((void *)arg, &wtime, sizeof wtime) ? -EFAULT : 0;
}

/*
 *	We enforce only one user at a time here with the open/close.
 *	Also clear the previous interrupt data on an open, and clean
 *	up things on a close.
 */

static int ds1286_open(struct inode *inode, struct file *file)
{
	if(ds1286_status & RTC_IS_OPEN)
		return -EBUSY;

	ds1286_status |= RTC_IS_OPEN;
	ds1286_irq_data = 0;
	return 0;
}

static int ds1286_release(struct inode *inode, struct file *file)
{
	lock_kernel();
	ds1286_status &= ~RTC_IS_OPEN;
	unlock_kernel();
	return 0;
}

static unsigned int ds1286_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &ds1286_wait, wait);
	if (ds1286_irq_data != 0)
		return POLLIN | POLLRDNORM;
	return 0;
}

/*
 *	The various file operations we support.
 */

static struct file_operations ds1286_fops = {
	llseek:		ds1286_llseek,
	read:		ds1286_read,
	poll:		ds1286_poll,
	ioctl:		ds1286_ioctl,
	open:		ds1286_open,
	release:	ds1286_release,
};

static struct miscdevice ds1286_dev=
{
	RTC_MINOR,
	"rtc",
	&ds1286_fops
};

int __init ds1286_init(void)
{
	printk(KERN_INFO "DS1286 Real Time Clock Driver v%s\n", DS1286_VERSION);
	misc_register(&ds1286_dev);

	return 0;
}

static char *days[] = {
	"***", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/*
 *	Info exported via "/proc/rtc".
 */
int get_ds1286_status(char *buf)
{
	char *p, *s;
	struct rtc_time tm;
	unsigned char hundredth, month, cmd, amode;

	p = buf;

	get_rtc_time(&tm);
	hundredth = CMOS_READ(RTC_HUNDREDTH_SECOND);
	hundredth = BCD_TO_BIN(hundredth);

	p += sprintf(p,
	             "rtc_time\t: %02d:%02d:%02d.%02d\n"
	             "rtc_date\t: %04d-%02d-%02d\n",
		     tm.tm_hour, tm.tm_min, tm.tm_sec, hundredth,
		     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

	/*
	 * We implicitly assume 24hr mode here. Alarm values >= 0xc0 will
	 * match any value for that particular field. Values that are
	 * greater than a valid time, but less than 0xc0 shouldn't appear.
	 */
	get_rtc_alm_time(&tm);
	p += sprintf(p, "alarm\t\t: %s ", days[tm.tm_wday]);
	if (tm.tm_hour <= 24)
		p += sprintf(p, "%02d:", tm.tm_hour);
	else
		p += sprintf(p, "**:");

	if (tm.tm_min <= 59)
		p += sprintf(p, "%02d\n", tm.tm_min);
	else
		p += sprintf(p, "**\n");

	month = CMOS_READ(RTC_MONTH);
	p += sprintf(p,
	             "oscillator\t: %s\n"
	             "square_wave\t: %s\n",
	             (month & RTC_EOSC) ? "disabled" : "enabled",
	             (month & RTC_ESQW) ? "disabled" : "enabled");

	amode = ((CMOS_READ(RTC_MINUTES_ALARM) & 0x80) >> 5) |
	        ((CMOS_READ(RTC_HOURS_ALARM) & 0x80) >> 6) |
	        ((CMOS_READ(RTC_DAY_ALARM) & 0x80) >> 7);
	if (amode == 7)      s = "each minute";
	else if (amode == 3) s = "minutes match";
	else if (amode == 1) s = "hours and minutes match";
	else if (amode == 0) s = "days, hours and minutes match";
	else                 s = "invalid";
	p += sprintf(p, "alarm_mode\t: %s\n", s);

	cmd = CMOS_READ(RTC_CMD);
	p += sprintf(p,
	             "alarm_enable\t: %s\n"
	             "wdog_alarm\t: %s\n"
	             "alarm_mask\t: %s\n"
	             "wdog_alarm_mask\t: %s\n"
	             "interrupt_mode\t: %s\n"
	             "INTB_mode\t: %s_active\n"
	             "interrupt_pins\t: %s\n",
		     (cmd & RTC_TDF) ? "yes" : "no",
		     (cmd & RTC_WAF) ? "yes" : "no",
		     (cmd & RTC_TDM) ? "disabled" : "enabled",
		     (cmd & RTC_WAM) ? "disabled" : "enabled",
		     (cmd & RTC_PU_LVL) ? "pulse" : "level",
		     (cmd & RTC_IBH_LO) ? "low" : "high",
	             (cmd & RTC_IPSW) ? "unswapped" : "swapped");

	return  p - buf;
}

/*
 * Returns true if a clock update is in progress
 */
static inline unsigned char ds1286_is_updating(void)
{
	return CMOS_READ(RTC_CMD) & RTC_TE;
}

void get_rtc_time(struct rtc_time *rtc_tm)
{
	unsigned long uip_watchdog = jiffies;
	unsigned char save_control;
	unsigned int flags;

	/*
	 * read RTC once any update in progress is done. The update
	 * can take just over 2ms. We wait 10 to 20ms. There is no need to
	 * to poll-wait (up to 1s - eeccch) for the falling edge of RTC_UIP.
	 * If you need to know *exactly* when a second has started, enable
	 * periodic update complete interrupts, (via ioctl) and then 
	 * immediately read /dev/rtc which will block until you get the IRQ.
	 * Once the read clears, read the RTC time (again via ioctl). Easy.
	 */

	if (ds1286_is_updating() != 0)
		while (jiffies - uip_watchdog < 2*HZ/100)
			barrier();

	/*
	 * Only the values that we read from the RTC are set. We leave
	 * tm_wday, tm_yday and tm_isdst untouched. Even though the
	 * RTC has RTC_DAY_OF_WEEK, we ignore it, as it is only updated
	 * by the RTC when initially set to a non-zero value.
	 */
	spin_lock_irqsave(&ds1286_lock, flags);
	save_control = CMOS_READ(RTC_CMD);
	CMOS_WRITE((save_control|RTC_TE), RTC_CMD);

	rtc_tm->tm_sec = CMOS_READ(RTC_SECONDS);
	rtc_tm->tm_min = CMOS_READ(RTC_MINUTES);
	rtc_tm->tm_hour = CMOS_READ(RTC_HOURS) & 0x1f;
	rtc_tm->tm_mday = CMOS_READ(RTC_DATE);
	rtc_tm->tm_mon = CMOS_READ(RTC_MONTH) & 0x1f;
	rtc_tm->tm_year = CMOS_READ(RTC_YEAR);

	CMOS_WRITE(save_control, RTC_CMD);
	spin_unlock_irqrestore(&ds1286_lock, flags);

	BCD_TO_BIN(rtc_tm->tm_sec);
	BCD_TO_BIN(rtc_tm->tm_min);
	BCD_TO_BIN(rtc_tm->tm_hour);
	BCD_TO_BIN(rtc_tm->tm_mday);
	BCD_TO_BIN(rtc_tm->tm_mon);
	BCD_TO_BIN(rtc_tm->tm_year);

	/*
	 * Account for differences between how the RTC uses the values
	 * and how they are defined in a struct rtc_time;
	 */
	if (rtc_tm->tm_year < 45)
		rtc_tm->tm_year += 30;
	if ((rtc_tm->tm_year += 40) < 70)
		rtc_tm->tm_year += 100;

	rtc_tm->tm_mon--;
}

void get_rtc_alm_time(struct rtc_time *alm_tm)
{
	unsigned char cmd;
	unsigned int flags;

	/*
	 * Only the values that we read from the RTC are set. That
	 * means only tm_wday, tm_hour, tm_min.
	 */
	spin_lock_irqsave(&ds1286_lock, flags);
	alm_tm->tm_min = CMOS_READ(RTC_MINUTES_ALARM) & 0x7f;
	alm_tm->tm_hour = CMOS_READ(RTC_HOURS_ALARM)  & 0x1f;
	alm_tm->tm_wday = CMOS_READ(RTC_DAY_ALARM)    & 0x07;
	cmd = CMOS_READ(RTC_CMD);
	spin_unlock_irqrestore(&ds1286_lock, flags);

	BCD_TO_BIN(alm_tm->tm_min);
	BCD_TO_BIN(alm_tm->tm_hour);
	alm_tm->tm_sec = 0;
}
