/*
 * SH7770 Setup
 *
 *  Copyright (C) 2006  Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/serial.h>
#include <asm/sci.h>

static struct plat_sci_port sci_platform_data[] = {
	{
		.mapbase	= 0xff923000,
		.flags		= UPF_BOOT_AUTOCONF,
		.type		= PORT_SCIF,
		.irqs		= { 61, 61, 61, 61 },
	}, {
		.mapbase	= 0xff924000,
		.flags		= UPF_BOOT_AUTOCONF,
		.type		= PORT_SCIF,
		.irqs		= { 62, 62, 62, 62 },
	}, {
		.mapbase	= 0xff925000,
		.flags		= UPF_BOOT_AUTOCONF,
		.type		= PORT_SCIF,
		.irqs		= { 63, 63, 63, 63 },
	}, {
		.flags = 0,
	}
};

static struct platform_device sci_device = {
	.name		= "sh-sci",
	.id		= -1,
	.dev		= {
		.platform_data	= sci_platform_data,
	},
};

static struct platform_device *sh7770_devices[] __initdata = {
	&sci_device,
};

static int __init sh7770_devices_setup(void)
{
	return platform_add_devices(sh7770_devices,
				    ARRAY_SIZE(sh7770_devices));
}
__initcall(sh7770_devices_setup);
