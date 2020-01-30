/*
 * Generic heartbeat driver for regular LED banks
 *
 * Copyright (C) 2007  Paul Mundt
 *
 * Most SH reference boards include a number of individual LEDs that can
 * be independently controlled (either via a pre-defined hardware
 * function or via the LED class, if desired -- the hardware tends to
 * encapsulate some of the same "triggers" that the LED class supports,
 * so there's not too much value in it).
 *
 * Additionally, most of these boards also have a LED bank that we've
 * traditionally used for strobing the load average. This use case is
 * handled by this driver, rather than giving each LED bit position its
 * own struct device.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/io.h>

#define DRV_NAME "heartbeat"
#define DRV_VERSION "0.1.0"

struct heartbeat_data {
	void __iomem *base;
	unsigned char bit_pos[8];
	struct timer_list timer;
};

static void heartbeat_timer(unsigned long data)
{
	struct heartbeat_data *hd = (struct heartbeat_data *)data;
	static unsigned bit = 0, up = 1;

	ctrl_outw(1 << hd->bit_pos[bit], (unsigned long)hd->base);
	bit += up;
	if ((bit == 0) || (bit == ARRAY_SIZE(hd->bit_pos)-1))
		up = -up;

	mod_timer(&hd->timer, jiffies + (110 - ((300 << FSHIFT) /
			((avenrun[0] / 5) + (3 << FSHIFT)))));
}

static int heartbeat_drv_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct heartbeat_data *hd;

	if (unlikely(pdev->num_resources != 1)) {
		dev_err(&pdev->dev, "invalid number of resources\n");
		return -EINVAL;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (unlikely(res == NULL)) {
		dev_err(&pdev->dev, "invalid resource\n");
		return -EINVAL;
	}

	hd = kmalloc(sizeof(struct heartbeat_data), GFP_KERNEL);
	if (unlikely(!hd))
		return -ENOMEM;

	if (pdev->dev.platform_data) {
		memcpy(hd->bit_pos, pdev->dev.platform_data,
		       ARRAY_SIZE(hd->bit_pos));
	} else {
		int i;

		for (i = 0; i < ARRAY_SIZE(hd->bit_pos); i++)
			hd->bit_pos[i] = i;
	}

	hd->base = (void __iomem *)(unsigned long)res->start;

	setup_timer(&hd->timer, heartbeat_timer, (unsigned long)hd);
	platform_set_drvdata(pdev, hd);

	return mod_timer(&hd->timer, jiffies + 1);
}

static int heartbeat_drv_remove(struct platform_device *pdev)
{
	struct heartbeat_data *hd = platform_get_drvdata(pdev);

	del_timer_sync(&hd->timer);

	platform_set_drvdata(pdev, NULL);

	kfree(hd);

	return 0;
}

static struct platform_driver heartbeat_driver = {
	.probe		= heartbeat_drv_probe,
	.remove		= heartbeat_drv_remove,
	.driver		= {
		.name	= DRV_NAME,
	},
};

static int __init heartbeat_init(void)
{
	printk(KERN_NOTICE DRV_NAME ": version %s loaded\n", DRV_VERSION);
	return platform_driver_register(&heartbeat_driver);
}

static void __exit heartbeat_exit(void)
{
	platform_driver_unregister(&heartbeat_driver);
}
module_init(heartbeat_init);
module_exit(heartbeat_exit);

MODULE_VERSION(DRV_VERSION);
MODULE_AUTHOR("Paul Mundt");
MODULE_LICENSE("GPLv2");
