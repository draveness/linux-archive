/*
 * A RTC driver for the Simtek STK17TA8
 *
 * By Thomas Hommel <thomas.hommel@gefanuc.com>
 *
 * Based on the DS1553 driver from
 * Atsushi Nemoto <anemo@mba.ocn.ne.jp>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bcd.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/interrupt.h>
#include <linux/rtc.h>
#include <linux/platform_device.h>
#include <linux/io.h>

#define DRV_VERSION "0.1"

#define RTC_REG_SIZE		0x20000
#define RTC_OFFSET		0x1fff0

#define RTC_FLAGS		(RTC_OFFSET + 0)
#define RTC_CENTURY		(RTC_OFFSET + 1)
#define RTC_SECONDS_ALARM	(RTC_OFFSET + 2)
#define RTC_MINUTES_ALARM	(RTC_OFFSET + 3)
#define RTC_HOURS_ALARM		(RTC_OFFSET + 4)
#define RTC_DATE_ALARM		(RTC_OFFSET + 5)
#define RTC_INTERRUPTS		(RTC_OFFSET + 6)
#define RTC_WATCHDOG		(RTC_OFFSET + 7)
#define RTC_CALIBRATION		(RTC_OFFSET + 8)
#define RTC_SECONDS		(RTC_OFFSET + 9)
#define RTC_MINUTES		(RTC_OFFSET + 10)
#define RTC_HOURS		(RTC_OFFSET + 11)
#define RTC_DAY			(RTC_OFFSET + 12)
#define RTC_DATE		(RTC_OFFSET + 13)
#define RTC_MONTH		(RTC_OFFSET + 14)
#define RTC_YEAR		(RTC_OFFSET + 15)

#define RTC_SECONDS_MASK	0x7f
#define RTC_DAY_MASK		0x07
#define RTC_CAL_MASK		0x3f

/* Bits in the Calibration register */
#define RTC_STOP		0x80

/* Bits in the Flags register */
#define RTC_FLAGS_AF		0x40
#define RTC_FLAGS_PF		0x20
#define RTC_WRITE		0x02
#define RTC_READ		0x01

/* Bits in the Interrupts register */
#define RTC_INTS_AIE		0x40

struct rtc_plat_data {
	struct rtc_device *rtc;
	void __iomem *ioaddr;
	unsigned long baseaddr;
	unsigned long last_jiffies;
	int irq;
	unsigned int irqen;
	int alrm_sec;
	int alrm_min;
	int alrm_hour;
	int alrm_mday;
};

static int stk17ta8_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);
	void __iomem *ioaddr = pdata->ioaddr;
	u8 flags;

	flags = readb(pdata->ioaddr + RTC_FLAGS);
	writeb(flags | RTC_WRITE, pdata->ioaddr + RTC_FLAGS);

	writeb(BIN2BCD(tm->tm_year % 100), ioaddr + RTC_YEAR);
	writeb(BIN2BCD(tm->tm_mon + 1), ioaddr + RTC_MONTH);
	writeb(BIN2BCD(tm->tm_wday) & RTC_DAY_MASK, ioaddr + RTC_DAY);
	writeb(BIN2BCD(tm->tm_mday), ioaddr + RTC_DATE);
	writeb(BIN2BCD(tm->tm_hour), ioaddr + RTC_HOURS);
	writeb(BIN2BCD(tm->tm_min), ioaddr + RTC_MINUTES);
	writeb(BIN2BCD(tm->tm_sec) & RTC_SECONDS_MASK, ioaddr + RTC_SECONDS);
	writeb(BIN2BCD((tm->tm_year + 1900) / 100), ioaddr + RTC_CENTURY);

	writeb(flags & ~RTC_WRITE, pdata->ioaddr + RTC_FLAGS);
	return 0;
}

static int stk17ta8_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);
	void __iomem *ioaddr = pdata->ioaddr;
	unsigned int year, month, day, hour, minute, second, week;
	unsigned int century;
	u8 flags;

	/* give enough time to update RTC in case of continuous read */
	if (pdata->last_jiffies == jiffies)
		msleep(1);
	pdata->last_jiffies = jiffies;

	flags = readb(pdata->ioaddr + RTC_FLAGS);
	writeb(flags | RTC_READ, ioaddr + RTC_FLAGS);
	second = readb(ioaddr + RTC_SECONDS) & RTC_SECONDS_MASK;
	minute = readb(ioaddr + RTC_MINUTES);
	hour = readb(ioaddr + RTC_HOURS);
	day = readb(ioaddr + RTC_DATE);
	week = readb(ioaddr + RTC_DAY) & RTC_DAY_MASK;
	month = readb(ioaddr + RTC_MONTH);
	year = readb(ioaddr + RTC_YEAR);
	century = readb(ioaddr + RTC_CENTURY);
	writeb(flags & ~RTC_READ, ioaddr + RTC_FLAGS);
	tm->tm_sec = BCD2BIN(second);
	tm->tm_min = BCD2BIN(minute);
	tm->tm_hour = BCD2BIN(hour);
	tm->tm_mday = BCD2BIN(day);
	tm->tm_wday = BCD2BIN(week);
	tm->tm_mon = BCD2BIN(month) - 1;
	/* year is 1900 + tm->tm_year */
	tm->tm_year = BCD2BIN(year) + BCD2BIN(century) * 100 - 1900;

	if (rtc_valid_tm(tm) < 0) {
		dev_err(dev, "retrieved date/time is not valid.\n");
		rtc_time_to_tm(0, tm);
	}
	return 0;
}

static void stk17ta8_rtc_update_alarm(struct rtc_plat_data *pdata)
{
	void __iomem *ioaddr = pdata->ioaddr;
	unsigned long irqflags;
	u8 flags;

	spin_lock_irqsave(&pdata->rtc->irq_lock, irqflags);

	flags = readb(ioaddr + RTC_FLAGS);
	writeb(flags | RTC_WRITE, ioaddr + RTC_FLAGS);

	writeb(pdata->alrm_mday < 0 || (pdata->irqen & RTC_UF) ?
	       0x80 : BIN2BCD(pdata->alrm_mday),
	       ioaddr + RTC_DATE_ALARM);
	writeb(pdata->alrm_hour < 0 || (pdata->irqen & RTC_UF) ?
	       0x80 : BIN2BCD(pdata->alrm_hour),
	       ioaddr + RTC_HOURS_ALARM);
	writeb(pdata->alrm_min < 0 || (pdata->irqen & RTC_UF) ?
	       0x80 : BIN2BCD(pdata->alrm_min),
	       ioaddr + RTC_MINUTES_ALARM);
	writeb(pdata->alrm_sec < 0 || (pdata->irqen & RTC_UF) ?
	       0x80 : BIN2BCD(pdata->alrm_sec),
	       ioaddr + RTC_SECONDS_ALARM);
	writeb(pdata->irqen ? RTC_INTS_AIE : 0, ioaddr + RTC_INTERRUPTS);
	readb(ioaddr + RTC_FLAGS);	/* clear interrupts */
	writeb(flags & ~RTC_WRITE, ioaddr + RTC_FLAGS);
	spin_unlock_irqrestore(&pdata->rtc->irq_lock, irqflags);
}

static int stk17ta8_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);

	if (pdata->irq < 0)
		return -EINVAL;
	pdata->alrm_mday = alrm->time.tm_mday;
	pdata->alrm_hour = alrm->time.tm_hour;
	pdata->alrm_min = alrm->time.tm_min;
	pdata->alrm_sec = alrm->time.tm_sec;
	if (alrm->enabled)
		pdata->irqen |= RTC_AF;
	stk17ta8_rtc_update_alarm(pdata);
	return 0;
}

static int stk17ta8_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);

	if (pdata->irq < 0)
		return -EINVAL;
	alrm->time.tm_mday = pdata->alrm_mday < 0 ? 0 : pdata->alrm_mday;
	alrm->time.tm_hour = pdata->alrm_hour < 0 ? 0 : pdata->alrm_hour;
	alrm->time.tm_min = pdata->alrm_min < 0 ? 0 : pdata->alrm_min;
	alrm->time.tm_sec = pdata->alrm_sec < 0 ? 0 : pdata->alrm_sec;
	alrm->enabled = (pdata->irqen & RTC_AF) ? 1 : 0;
	return 0;
}

static irqreturn_t stk17ta8_rtc_interrupt(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);
	void __iomem *ioaddr = pdata->ioaddr;
	unsigned long events = RTC_IRQF;

	/* read and clear interrupt */
	if (!(readb(ioaddr + RTC_FLAGS) & RTC_FLAGS_AF))
		return IRQ_NONE;
	if (readb(ioaddr + RTC_SECONDS_ALARM) & 0x80)
		events |= RTC_UF;
	else
		events |= RTC_AF;
	rtc_update_irq(pdata->rtc, 1, events);
	return IRQ_HANDLED;
}

static void stk17ta8_rtc_release(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);

	if (pdata->irq >= 0) {
		pdata->irqen = 0;
		stk17ta8_rtc_update_alarm(pdata);
	}
}

static int stk17ta8_rtc_ioctl(struct device *dev, unsigned int cmd,
			    unsigned long arg)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);

	if (pdata->irq < 0)
		return -ENOIOCTLCMD; /* fall back into rtc-dev's emulation */
	switch (cmd) {
	case RTC_AIE_OFF:
		pdata->irqen &= ~RTC_AF;
		stk17ta8_rtc_update_alarm(pdata);
		break;
	case RTC_AIE_ON:
		pdata->irqen |= RTC_AF;
		stk17ta8_rtc_update_alarm(pdata);
		break;
	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

static const struct rtc_class_ops stk17ta8_rtc_ops = {
	.read_time	= stk17ta8_rtc_read_time,
	.set_time	= stk17ta8_rtc_set_time,
	.read_alarm	= stk17ta8_rtc_read_alarm,
	.set_alarm	= stk17ta8_rtc_set_alarm,
	.release	= stk17ta8_rtc_release,
	.ioctl		= stk17ta8_rtc_ioctl,
};

static ssize_t stk17ta8_nvram_read(struct kobject *kobj,
				 struct bin_attribute *attr, char *buf,
				 loff_t pos, size_t size)
{
	struct platform_device *pdev =
		to_platform_device(container_of(kobj, struct device, kobj));
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);
	void __iomem *ioaddr = pdata->ioaddr;
	ssize_t count;

	for (count = 0; size > 0 && pos < RTC_OFFSET; count++, size--)
		*buf++ = readb(ioaddr + pos++);
	return count;
}

static ssize_t stk17ta8_nvram_write(struct kobject *kobj,
				  struct bin_attribute *attr, char *buf,
				  loff_t pos, size_t size)
{
	struct platform_device *pdev =
		to_platform_device(container_of(kobj, struct device, kobj));
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);
	void __iomem *ioaddr = pdata->ioaddr;
	ssize_t count;

	for (count = 0; size > 0 && pos < RTC_OFFSET; count++, size--)
		writeb(*buf++, ioaddr + pos++);
	return count;
}

static struct bin_attribute stk17ta8_nvram_attr = {
	.attr = {
		.name = "nvram",
		.mode = S_IRUGO | S_IWUGO,
		.owner = THIS_MODULE,
	},
	.size = RTC_OFFSET,
	.read = stk17ta8_nvram_read,
	.write = stk17ta8_nvram_write,
};

static int __init stk17ta8_rtc_probe(struct platform_device *pdev)
{
	struct rtc_device *rtc;
	struct resource *res;
	unsigned int cal;
	unsigned int flags;
	struct rtc_plat_data *pdata;
	void __iomem *ioaddr = NULL;
	int ret = 0;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;
	pdata->irq = -1;
	if (!request_mem_region(res->start, RTC_REG_SIZE, pdev->name)) {
		ret = -EBUSY;
		goto out;
	}
	pdata->baseaddr = res->start;
	ioaddr = ioremap(pdata->baseaddr, RTC_REG_SIZE);
	if (!ioaddr) {
		ret = -ENOMEM;
		goto out;
	}
	pdata->ioaddr = ioaddr;
	pdata->irq = platform_get_irq(pdev, 0);

	/* turn RTC on if it was not on */
	cal = readb(ioaddr + RTC_CALIBRATION);
	if (cal & RTC_STOP) {
		cal &= RTC_CAL_MASK;
		flags = readb(ioaddr + RTC_FLAGS);
		writeb(flags | RTC_WRITE, ioaddr + RTC_FLAGS);
		writeb(cal, ioaddr + RTC_CALIBRATION);
		writeb(flags & ~RTC_WRITE, ioaddr + RTC_FLAGS);
	}
	if (readb(ioaddr + RTC_FLAGS) & RTC_FLAGS_PF)
		dev_warn(&pdev->dev, "voltage-low detected.\n");

	if (pdata->irq >= 0) {
		writeb(0, ioaddr + RTC_INTERRUPTS);
		if (request_irq(pdata->irq, stk17ta8_rtc_interrupt,
				IRQF_DISABLED | IRQF_SHARED,
				pdev->name, pdev) < 0) {
			dev_warn(&pdev->dev, "interrupt not available.\n");
			pdata->irq = -1;
		}
	}

	rtc = rtc_device_register(pdev->name, &pdev->dev,
				  &stk17ta8_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc)) {
		ret = PTR_ERR(rtc);
		goto out;
	}
	pdata->rtc = rtc;
	pdata->last_jiffies = jiffies;
	platform_set_drvdata(pdev, pdata);
	ret = sysfs_create_bin_file(&pdev->dev.kobj, &stk17ta8_nvram_attr);
	if (ret)
		goto out;
	return 0;
 out:
	if (pdata->rtc)
		rtc_device_unregister(pdata->rtc);
	if (pdata->irq >= 0)
		free_irq(pdata->irq, pdev);
	if (ioaddr)
		iounmap(ioaddr);
	if (pdata->baseaddr)
		release_mem_region(pdata->baseaddr, RTC_REG_SIZE);
	kfree(pdata);
	return ret;
}

static int __devexit stk17ta8_rtc_remove(struct platform_device *pdev)
{
	struct rtc_plat_data *pdata = platform_get_drvdata(pdev);

	sysfs_remove_bin_file(&pdev->dev.kobj, &stk17ta8_nvram_attr);
	rtc_device_unregister(pdata->rtc);
	if (pdata->irq >= 0) {
		writeb(0, pdata->ioaddr + RTC_INTERRUPTS);
		free_irq(pdata->irq, pdev);
	}
	iounmap(pdata->ioaddr);
	release_mem_region(pdata->baseaddr, RTC_REG_SIZE);
	kfree(pdata);
	return 0;
}

static struct platform_driver stk17ta8_rtc_driver = {
	.probe		= stk17ta8_rtc_probe,
	.remove		= __devexit_p(stk17ta8_rtc_remove),
	.driver		= {
		.name	= "stk17ta8",
		.owner	= THIS_MODULE,
	},
};

static __init int stk17ta8_init(void)
{
	return platform_driver_register(&stk17ta8_rtc_driver);
}

static __exit void stk17ta8_exit(void)
{
	return platform_driver_unregister(&stk17ta8_rtc_driver);
}

module_init(stk17ta8_init);
module_exit(stk17ta8_exit);

MODULE_AUTHOR("Thomas Hommel <thomas.hommel@gefanuc.com>");
MODULE_DESCRIPTION("Simtek STK17TA8 RTC driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
