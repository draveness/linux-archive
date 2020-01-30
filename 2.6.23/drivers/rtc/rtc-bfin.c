/*
 * Blackfin On-Chip Real Time Clock Driver
 *  Supports BF53[123]/BF53[467]/BF54[2489]
 *
 * Copyright 2004-2007 Analog Devices Inc.
 *
 * Enter bugs at http://blackfin.uclinux.org/
 *
 * Licensed under the GPL-2 or later.
 */

/* The biggest issue we deal with in this driver is that register writes are
 * synced to the RTC frequency of 1Hz.  So if you write to a register and
 * attempt to write again before the first write has completed, the new write
 * is simply discarded.  This can easily be troublesome if userspace disables
 * one event (say periodic) and then right after enables an event (say alarm).
 * Since all events are maintained in the same interrupt mask register, if
 * we wrote to it to disable the first event and then wrote to it again to
 * enable the second event, that second event would not be enabled as the
 * write would be discarded and things quickly fall apart.
 *
 * To keep this delay from significantly degrading performance (we, in theory,
 * would have to sleep for up to 1 second everytime we wanted to write a
 * register), we only check the write pending status before we start to issue
 * a new write.  We bank on the idea that it doesnt matter when the sync
 * happens so long as we don't attempt another write before it does.  The only
 * time userspace would take this penalty is when they try and do multiple
 * operations right after another ... but in this case, they need to take the
 * sync penalty, so we should be OK.
 *
 * Also note that the RTC_ISTAT register does not suffer this penalty; its
 * writes to clear status registers complete immediately.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/delay.h>

#include <asm/blackfin.h>

#define stamp(fmt, args...) pr_debug("%s:%i: " fmt "\n", __FUNCTION__, __LINE__, ## args)
#define stampit() stamp("here i am")

struct bfin_rtc {
	struct rtc_device *rtc_dev;
	struct rtc_time rtc_alarm;
	spinlock_t lock;
};

/* Bit values for the ISTAT / ICTL registers */
#define RTC_ISTAT_WRITE_COMPLETE  0x8000
#define RTC_ISTAT_WRITE_PENDING   0x4000
#define RTC_ISTAT_ALARM_DAY       0x0040
#define RTC_ISTAT_24HR            0x0020
#define RTC_ISTAT_HOUR            0x0010
#define RTC_ISTAT_MIN             0x0008
#define RTC_ISTAT_SEC             0x0004
#define RTC_ISTAT_ALARM           0x0002
#define RTC_ISTAT_STOPWATCH       0x0001

/* Shift values for RTC_STAT register */
#define DAY_BITS_OFF    17
#define HOUR_BITS_OFF   12
#define MIN_BITS_OFF    6
#define SEC_BITS_OFF    0

/* Some helper functions to convert between the common RTC notion of time
 * and the internal Blackfin notion that is stored in 32bits.
 */
static inline u32 rtc_time_to_bfin(unsigned long now)
{
	u32 sec  = (now % 60);
	u32 min  = (now % (60 * 60)) / 60;
	u32 hour = (now % (60 * 60 * 24)) / (60 * 60);
	u32 days = (now / (60 * 60 * 24));
	return (sec  << SEC_BITS_OFF) +
	       (min  << MIN_BITS_OFF) +
	       (hour << HOUR_BITS_OFF) +
	       (days << DAY_BITS_OFF);
}
static inline unsigned long rtc_bfin_to_time(u32 rtc_bfin)
{
	return (((rtc_bfin >> SEC_BITS_OFF)  & 0x003F)) +
	       (((rtc_bfin >> MIN_BITS_OFF)  & 0x003F) * 60) +
	       (((rtc_bfin >> HOUR_BITS_OFF) & 0x001F) * 60 * 60) +
	       (((rtc_bfin >> DAY_BITS_OFF)  & 0x7FFF) * 60 * 60 * 24);
}
static inline void rtc_bfin_to_tm(u32 rtc_bfin, struct rtc_time *tm)
{
	rtc_time_to_tm(rtc_bfin_to_time(rtc_bfin), tm);
}

/* Wait for the previous write to a RTC register to complete.
 * Unfortunately, we can't sleep here as that introduces a race condition when
 * turning on interrupt events.  Consider this:
 *  - process sets alarm
 *  - process enables alarm
 *  - process sleeps while waiting for rtc write to sync
 *  - interrupt fires while process is sleeping
 *  - interrupt acks the event by writing to ISTAT
 *  - interrupt sets the WRITE PENDING bit
 *  - interrupt handler finishes
 *  - process wakes up, sees WRITE PENDING bit set, goes to sleep
 *  - interrupt fires while process is sleeping
 * If anyone can point out the obvious solution here, i'm listening :).  This
 * shouldn't be an issue on an SMP or preempt system as this function should
 * only be called with the rtc lock held.
 */
static void rtc_bfin_sync_pending(void)
{
	stampit();
	while (!(bfin_read_RTC_ISTAT() & RTC_ISTAT_WRITE_COMPLETE)) {
		if (!(bfin_read_RTC_ISTAT() & RTC_ISTAT_WRITE_PENDING))
			break;
	}
	bfin_write_RTC_ISTAT(RTC_ISTAT_WRITE_COMPLETE);
}

static void rtc_bfin_reset(struct bfin_rtc *rtc)
{
	/* Initialize the RTC. Enable pre-scaler to scale RTC clock
	 * to 1Hz and clear interrupt/status registers. */
	spin_lock_irq(&rtc->lock);
	rtc_bfin_sync_pending();
	bfin_write_RTC_PREN(0x1);
	bfin_write_RTC_ICTL(0);
	bfin_write_RTC_SWCNT(0);
	bfin_write_RTC_ALARM(0);
	bfin_write_RTC_ISTAT(0xFFFF);
	spin_unlock_irq(&rtc->lock);
}

static irqreturn_t bfin_rtc_interrupt(int irq, void *dev_id)
{
	struct platform_device *pdev = to_platform_device(dev_id);
	struct bfin_rtc *rtc = platform_get_drvdata(pdev);
	unsigned long events = 0;
	u16 rtc_istat;

	stampit();

	spin_lock_irq(&rtc->lock);

	rtc_istat = bfin_read_RTC_ISTAT();

	if (rtc_istat & (RTC_ISTAT_ALARM | RTC_ISTAT_ALARM_DAY)) {
		bfin_write_RTC_ISTAT(RTC_ISTAT_ALARM | RTC_ISTAT_ALARM_DAY);
		events |= RTC_AF | RTC_IRQF;
	}

	if (rtc_istat & RTC_ISTAT_STOPWATCH) {
		bfin_write_RTC_ISTAT(RTC_ISTAT_STOPWATCH);
		events |= RTC_PF | RTC_IRQF;
		bfin_write_RTC_SWCNT(rtc->rtc_dev->irq_freq);
	}

	if (rtc_istat & RTC_ISTAT_SEC) {
		bfin_write_RTC_ISTAT(RTC_ISTAT_SEC);
		events |= RTC_UF | RTC_IRQF;
	}

	rtc_update_irq(rtc->rtc_dev, 1, events);

	spin_unlock_irq(&rtc->lock);

	return IRQ_HANDLED;
}

static int bfin_rtc_open(struct device *dev)
{
	struct bfin_rtc *rtc = dev_get_drvdata(dev);
	int ret;

	stampit();

	ret = request_irq(IRQ_RTC, bfin_rtc_interrupt, IRQF_DISABLED, "rtc-bfin", dev);
	if (unlikely(ret)) {
		dev_err(dev, "request RTC IRQ failed with %d\n", ret);
		return ret;
	}

	rtc_bfin_reset(rtc);

	return ret;
}

static void bfin_rtc_release(struct device *dev)
{
	struct bfin_rtc *rtc = dev_get_drvdata(dev);
	stampit();
	rtc_bfin_reset(rtc);
	free_irq(IRQ_RTC, dev);
}

static int bfin_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	struct bfin_rtc *rtc = dev_get_drvdata(dev);

	stampit();

	switch (cmd) {
	case RTC_PIE_ON:
		stampit();
		spin_lock_irq(&rtc->lock);
		rtc_bfin_sync_pending();
		bfin_write_RTC_ISTAT(RTC_ISTAT_STOPWATCH);
		bfin_write_RTC_SWCNT(rtc->rtc_dev->irq_freq);
		bfin_write_RTC_ICTL(bfin_read_RTC_ICTL() | RTC_ISTAT_STOPWATCH);
		spin_unlock_irq(&rtc->lock);
		return 0;
	case RTC_PIE_OFF:
		stampit();
		spin_lock_irq(&rtc->lock);
		rtc_bfin_sync_pending();
		bfin_write_RTC_SWCNT(0);
		bfin_write_RTC_ICTL(bfin_read_RTC_ICTL() & ~RTC_ISTAT_STOPWATCH);
		spin_unlock_irq(&rtc->lock);
		return 0;

	case RTC_UIE_ON:
		stampit();
		spin_lock_irq(&rtc->lock);
		rtc_bfin_sync_pending();
		bfin_write_RTC_ISTAT(RTC_ISTAT_SEC);
		bfin_write_RTC_ICTL(bfin_read_RTC_ICTL() | RTC_ISTAT_SEC);
		spin_unlock_irq(&rtc->lock);
		return 0;
	case RTC_UIE_OFF:
		stampit();
		spin_lock_irq(&rtc->lock);
		rtc_bfin_sync_pending();
		bfin_write_RTC_ICTL(bfin_read_RTC_ICTL() & ~RTC_ISTAT_SEC);
		spin_unlock_irq(&rtc->lock);
		return 0;

	case RTC_AIE_ON: {
		unsigned long rtc_alarm;
		u16 which_alarm;
		int ret = 0;

		stampit();

		spin_lock_irq(&rtc->lock);

		rtc_bfin_sync_pending();
		if (rtc->rtc_alarm.tm_yday == -1) {
			struct rtc_time now;
			rtc_bfin_to_tm(bfin_read_RTC_STAT(), &now);
			now.tm_sec = rtc->rtc_alarm.tm_sec;
			now.tm_min = rtc->rtc_alarm.tm_min;
			now.tm_hour = rtc->rtc_alarm.tm_hour;
			ret = rtc_tm_to_time(&now, &rtc_alarm);
			which_alarm = RTC_ISTAT_ALARM;
		} else {
			ret = rtc_tm_to_time(&rtc->rtc_alarm, &rtc_alarm);
			which_alarm = RTC_ISTAT_ALARM_DAY;
		}
		if (ret == 0) {
			bfin_write_RTC_ISTAT(which_alarm);
			bfin_write_RTC_ALARM(rtc_time_to_bfin(rtc_alarm));
			bfin_write_RTC_ICTL(bfin_read_RTC_ICTL() | which_alarm);
		}

		spin_unlock_irq(&rtc->lock);

		return ret;
	}
	case RTC_AIE_OFF:
		stampit();
		spin_lock_irq(&rtc->lock);
		rtc_bfin_sync_pending();
		bfin_write_RTC_ICTL(bfin_read_RTC_ICTL() & ~(RTC_ISTAT_ALARM | RTC_ISTAT_ALARM_DAY));
		spin_unlock_irq(&rtc->lock);
		return 0;
	}

	return -ENOIOCTLCMD;
}

static int bfin_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct bfin_rtc *rtc = dev_get_drvdata(dev);

	stampit();

	spin_lock_irq(&rtc->lock);
	rtc_bfin_sync_pending();
	rtc_bfin_to_tm(bfin_read_RTC_STAT(), tm);
	spin_unlock_irq(&rtc->lock);

	return 0;
}

static int bfin_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct bfin_rtc *rtc = dev_get_drvdata(dev);
	int ret;
	unsigned long now;

	stampit();

	spin_lock_irq(&rtc->lock);

	ret = rtc_tm_to_time(tm, &now);
	if (ret == 0) {
		rtc_bfin_sync_pending();
		bfin_write_RTC_STAT(rtc_time_to_bfin(now));
	}

	spin_unlock_irq(&rtc->lock);

	return ret;
}

static int bfin_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct bfin_rtc *rtc = dev_get_drvdata(dev);
	stampit();
	memcpy(&alrm->time, &rtc->rtc_alarm, sizeof(struct rtc_time));
	alrm->pending = !!(bfin_read_RTC_ICTL() & (RTC_ISTAT_ALARM | RTC_ISTAT_ALARM_DAY));
	return 0;
}

static int bfin_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct bfin_rtc *rtc = dev_get_drvdata(dev);
	stampit();
	memcpy(&rtc->rtc_alarm, &alrm->time, sizeof(struct rtc_time));
	return 0;
}

static int bfin_rtc_proc(struct device *dev, struct seq_file *seq)
{
#define yesno(x) (x ? "yes" : "no")
	u16 ictl = bfin_read_RTC_ICTL();
	stampit();
	seq_printf(seq, "alarm_IRQ\t: %s\n", yesno(ictl & RTC_ISTAT_ALARM));
	seq_printf(seq, "wkalarm_IRQ\t: %s\n", yesno(ictl & RTC_ISTAT_ALARM_DAY));
	seq_printf(seq, "seconds_IRQ\t: %s\n", yesno(ictl & RTC_ISTAT_SEC));
	seq_printf(seq, "periodic_IRQ\t: %s\n", yesno(ictl & RTC_ISTAT_STOPWATCH));
#ifdef DEBUG
	seq_printf(seq, "RTC_STAT\t: 0x%08X\n", bfin_read_RTC_STAT());
	seq_printf(seq, "RTC_ICTL\t: 0x%04X\n", bfin_read_RTC_ICTL());
	seq_printf(seq, "RTC_ISTAT\t: 0x%04X\n", bfin_read_RTC_ISTAT());
	seq_printf(seq, "RTC_SWCNT\t: 0x%04X\n", bfin_read_RTC_SWCNT());
	seq_printf(seq, "RTC_ALARM\t: 0x%08X\n", bfin_read_RTC_ALARM());
	seq_printf(seq, "RTC_PREN\t: 0x%04X\n", bfin_read_RTC_PREN());
#endif
	return 0;
}

static int bfin_irq_set_freq(struct device *dev, int freq)
{
	struct bfin_rtc *rtc = dev_get_drvdata(dev);
	stampit();
	rtc->rtc_dev->irq_freq = freq;
	return 0;
}

static struct rtc_class_ops bfin_rtc_ops = {
	.open          = bfin_rtc_open,
	.release       = bfin_rtc_release,
	.ioctl         = bfin_rtc_ioctl,
	.read_time     = bfin_rtc_read_time,
	.set_time      = bfin_rtc_set_time,
	.read_alarm    = bfin_rtc_read_alarm,
	.set_alarm     = bfin_rtc_set_alarm,
	.proc          = bfin_rtc_proc,
	.irq_set_freq  = bfin_irq_set_freq,
};

static int __devinit bfin_rtc_probe(struct platform_device *pdev)
{
	struct bfin_rtc *rtc;
	int ret = 0;

	stampit();

	rtc = kzalloc(sizeof(*rtc), GFP_KERNEL);
	if (unlikely(!rtc))
		return -ENOMEM;

	spin_lock_init(&rtc->lock);

	rtc->rtc_dev = rtc_device_register(pdev->name, &pdev->dev, &bfin_rtc_ops, THIS_MODULE);
	if (unlikely(IS_ERR(rtc))) {
		ret = PTR_ERR(rtc->rtc_dev);
		goto err;
	}
	rtc->rtc_dev->irq_freq = 0;
	rtc->rtc_dev->max_user_freq = (2 << 16); /* stopwatch is an unsigned 16 bit reg */

	platform_set_drvdata(pdev, rtc);

	return 0;

err:
	kfree(rtc);
	return ret;
}

static int __devexit bfin_rtc_remove(struct platform_device *pdev)
{
	struct bfin_rtc *rtc = platform_get_drvdata(pdev);

	rtc_device_unregister(rtc->rtc_dev);
	platform_set_drvdata(pdev, NULL);
	kfree(rtc);

	return 0;
}

static struct platform_driver bfin_rtc_driver = {
	.driver		= {
		.name	= "rtc-bfin",
		.owner	= THIS_MODULE,
	},
	.probe		= bfin_rtc_probe,
	.remove		= __devexit_p(bfin_rtc_remove),
};

static int __init bfin_rtc_init(void)
{
	stampit();
	return platform_driver_register(&bfin_rtc_driver);
}

static void __exit bfin_rtc_exit(void)
{
	platform_driver_unregister(&bfin_rtc_driver);
}

module_init(bfin_rtc_init);
module_exit(bfin_rtc_exit);

MODULE_DESCRIPTION("Blackfin On-Chip Real Time Clock Driver");
MODULE_AUTHOR("Mike Frysinger <vapier@gentoo.org>");
MODULE_LICENSE("GPL");
