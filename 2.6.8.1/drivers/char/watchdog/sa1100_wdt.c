/*
 *	Watchdog driver for the SA11x0/PXA2xx
 *
 *      (c) Copyright 2000 Oleg Drokin <green@crimea.edu>
 *          Based on SoftDog driver by Alan Cox <alan@redhat.com>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *	Neither Oleg Drokin nor iXcelerator.com admit liability nor provide
 *	warranty for any of this software. This material is provided
 *	"AS-IS" and at no charge.
 *
 *	(c) Copyright 2000           Oleg Drokin <green@crimea.edu>
 *
 *      27/11/2000 Initial release
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <linux/init.h>

#include <asm/hardware.h>
#include <asm/bitops.h>
#include <asm/uaccess.h>

#define OSCR_FREQ		3686400
#define SA1100_CLOSE_MAGIC	(0x5afc4453)

static unsigned long sa1100wdt_users;
static int expect_close;
static int pre_margin;
static int boot_status;
#ifdef CONFIG_WATCHDOG_NOWAYOUT
static int nowayout = 1;
#else
static int nowayout = 0;
#endif

/*
 *	Allow only one person to hold it open
 */
static int sa1100dog_open(struct inode *inode, struct file *file)
{
	nonseekable_open(inode, file);
	if (test_and_set_bit(1,&sa1100wdt_users))
		return -EBUSY;

	/* Activate SA1100 Watchdog timer */
	OSMR3 = OSCR + pre_margin;
	OSSR = OSSR_M3;
	OWER = OWER_WME;
	OIER |= OIER_E3;
	return 0;
}

/*
 *	Shut off the timer.
 * 	Lock it in if it's a module and we defined ...NOWAYOUT
 *	Oddly, the watchdog can only be enabled, but we can turn off
 *	the interrupt, which appears to prevent the watchdog timing out.
 */
static int sa1100dog_release(struct inode *inode, struct file *file)
{
	OSMR3 = OSCR + pre_margin;

	if (expect_close == SA1100_CLOSE_MAGIC) {
		OIER &= ~OIER_E3;
	} else {
		printk(KERN_CRIT "WATCHDOG: WDT device closed unexpectedly.  WDT will not stop!\n");
	}

	clear_bit(1, &sa1100wdt_users);
	expect_close = 0;

	return 0;
}

static ssize_t sa1100dog_write(struct file *file, const char *data, size_t len, loff_t *ppos)
{
	if (len) {
		if (!nowayout) {
			size_t i;

			expect_close = 0;

			for (i = 0; i != len; i++) {
				char c;

				if (get_user(c, data + i))
					return -EFAULT;
				if (c == 'V')
					expect_close = SA1100_CLOSE_MAGIC;
			}
		}
		/* Refresh OSMR3 timer. */
		OSMR3 = OSCR + pre_margin;
	}

	return len;
}

static struct watchdog_info ident = {
	.options	= WDIOF_CARDRESET | WDIOF_MAGICCLOSE |
			  WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.identity	= "SA1100 Watchdog",
};

static int sa1100dog_ioctl(struct inode *inode, struct file *file,
	unsigned int cmd, unsigned long arg)
{
	int ret = -ENOIOCTLCMD;
	int time;

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		ret = copy_to_user((struct watchdog_info *)arg, &ident,
				   sizeof(ident)) ? -EFAULT : 0;
		break;

	case WDIOC_GETSTATUS:
		ret = put_user(0, (int *)arg);
		break;

	case WDIOC_GETBOOTSTATUS:
		ret = put_user(boot_status, (int *)arg);
		break;

	case WDIOC_SETTIMEOUT:
		ret = get_user(time, (int *)arg);
		if (ret)
			break;

		if (time <= 0 || time > 255) {
			ret = -EINVAL;
			break;
		}

		pre_margin = OSCR_FREQ * time;
		OSMR3 = OSCR + pre_margin;
		/*fall through*/

	case WDIOC_GETTIMEOUT:
		ret = put_user(pre_margin / OSCR_FREQ, (int *)arg);
		break;

	case WDIOC_KEEPALIVE:
		OSMR3 = OSCR + pre_margin;
		ret = 0;
		break;
	}
	return ret;
}

static struct file_operations sa1100dog_fops =
{
	.owner		= THIS_MODULE,
	.write		= sa1100dog_write,
	.ioctl		= sa1100dog_ioctl,
	.open		= sa1100dog_open,
	.release	= sa1100dog_release,
};

static struct miscdevice sa1100dog_miscdev =
{
	.minor		= WATCHDOG_MINOR,
	.name		= "SA1100/PXA2xx watchdog",
	.fops		= &sa1100dog_fops,
};

static int margin __initdata = 60;		/* (secs) Default is 1 minute */

static int __init sa1100dog_init(void)
{
	int ret;

	/*
	 * Read the reset status, and save it for later.  If
	 * we suspend, RCSR will be cleared, and the watchdog
	 * reset reason will be lost.
	 */
	boot_status = (RCSR & RCSR_WDR) ? WDIOF_CARDRESET : 0;
	pre_margin = OSCR_FREQ * margin;

	ret = misc_register(&sa1100dog_miscdev);
	if (ret == 0)
		printk("SA1100/PXA2xx Watchdog Timer: timer margin %d sec\n",
		       margin);

	return ret;
}

static void __exit sa1100dog_exit(void)
{
	misc_deregister(&sa1100dog_miscdev);
}

module_init(sa1100dog_init);
module_exit(sa1100dog_exit);

MODULE_AUTHOR("Oleg Drokin <green@crimea.edu>");
MODULE_DESCRIPTION("SA1100/PXA2xx Watchdog");

module_param(margin, int, 0);
MODULE_PARM_DESC(margin, "Watchdog margin in seconds (default 60s)");

module_param(nowayout, int, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started");

MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(WATCHDOG_MINOR);
