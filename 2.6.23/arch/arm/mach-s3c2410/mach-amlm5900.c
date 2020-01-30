/* linux/arch/arm/mach-s3c2410/mach-amlm5900.c
 *
 * linux/arch/arm/mach-s3c2410/mach-amlm5900.c
 *
 * Copyright (c) 2006 American Microsystems Limited
 *	David Anders <danders@amltd.com>

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 * @History:
 * derived from linux/arch/arm/mach-s3c2410/mach-bast.c, written by
 * Ben Dooks <ben@simtec.co.uk>
 *
 ***********************************************************************/

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/serial_core.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>
#include <asm/mach/flash.h>

#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/mach-types.h>
#include <asm/arch/fb.h>

#include <asm/plat-s3c/regs-serial.h>
#include <asm/arch/regs-lcd.h>
#include <asm/arch/regs-gpio.h>

#include <asm/plat-s3c24xx/devs.h>
#include <asm/plat-s3c24xx/cpu.h>

#ifdef CONFIG_MTD_PARTITIONS

#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/map.h>
#include <linux/mtd/physmap.h>

static struct resource amlm5900_nor_resource = {
		.start = 0x00000000,
		.end   = 0x01000000 - 1,
		.flags = IORESOURCE_MEM,
};



static struct mtd_partition amlm5900_mtd_partitions[] = {
	{
		.name		= "System",
		.size		= 0x240000,
		.offset		= 0,
		.mask_flags 	= MTD_WRITEABLE,  /* force read-only */
	}, {
		.name		= "Kernel",
		.size		= 0x100000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "Ramdisk",
		.size		= 0x300000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "JFFS2",
		.size		= 0x9A0000,
		.offset		= MTDPART_OFS_APPEND,
	}, {
		.name		= "Settings",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	}
};

static struct physmap_flash_data amlm5900_flash_data = {
	.width		= 2,
	.parts		= amlm5900_mtd_partitions,
	.nr_parts	= ARRAY_SIZE(amlm5900_mtd_partitions),
};

static struct platform_device amlm5900_device_nor = {
	.name		= "physmap-flash",
	.id		= 0,
	.dev = {
			.platform_data = &amlm5900_flash_data,
		},
	.num_resources	= 1,
	.resource	= &amlm5900_nor_resource,
};
#endif

static struct map_desc amlm5900_iodesc[] __initdata = {
};

#define UCON S3C2410_UCON_DEFAULT
#define ULCON S3C2410_LCON_CS8 | S3C2410_LCON_PNONE | S3C2410_LCON_STOPB
#define UFCON S3C2410_UFCON_RXTRIG8 | S3C2410_UFCON_FIFOMODE

static struct s3c2410_uartcfg amlm5900_uartcfgs[] = {
	[0] = {
		.hwport	     = 0,
		.flags	     = 0,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	},
	[1] = {
		.hwport	     = 1,
		.flags	     = 0,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	},
	[2] = {
		.hwport	     = 2,
		.flags	     = 0,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	}
};


static struct platform_device *amlm5900_devices[] __initdata = {
#ifdef CONFIG_FB_S3C2410
	&s3c_device_lcd,
#endif
	&s3c_device_adc,
	&s3c_device_wdt,
	&s3c_device_i2c,
	&s3c_device_usb,
 	&s3c_device_rtc,
	&s3c_device_usbgadget,
        &s3c_device_sdi,
#ifdef CONFIG_MTD_PARTITIONS
	&amlm5900_device_nor,
#endif
};

static void __init amlm5900_map_io(void)
{
	s3c24xx_init_io(amlm5900_iodesc, ARRAY_SIZE(amlm5900_iodesc));
	s3c24xx_init_clocks(0);
	s3c24xx_init_uarts(amlm5900_uartcfgs, ARRAY_SIZE(amlm5900_uartcfgs));
}

#ifdef CONFIG_FB_S3C2410
static struct s3c2410fb_mach_info __initdata amlm5900_lcd_info = {
	.width		= 160,
	.height		= 160,

/* commented out until stn patch is submitted
*	.type		= S3C2410_LCDCON1_STN4,
*/
	.gpccon =	0xaaaaaaaa,
	.gpccon_mask =	0xffffffff,
	.gpcup =	0x0000ffff,
	.gpcup_mask =	0xffffffff,

	.gpdcon =	0xaaaaaaaa,
	.gpdcon_mask =	0xffffffff,
	.gpdup =	0x0000ffff,
	.gpdup_mask =	0xffffffff,

	.xres		= {
		.min		= 160,
		.max		= 160,
		.defval		= 160,
	},

	.yres		= {
		.min		= 160,
		.max	        = 160,
		.defval		= 160,
	},

	.bpp		= {
		.min		= 4,
		.max		= 4,
		.defval		= 4,
	},

	.regs		= {
		.lcdcon1	= 0x00008225,
		.lcdcon2	= 0x0027c000,
		.lcdcon3	= 0x00182708,
		.lcdcon4	= 0x00000002,
		.lcdcon5	= 0x00000001,
	}
};
#endif

static irqreturn_t
amlm5900_wake_interrupt(int irq, void *ignored)
{
	return IRQ_HANDLED;
}

static void amlm5900_init_pm(void)
{
	int ret = 0;

	ret = request_irq(IRQ_EINT9, &amlm5900_wake_interrupt,
				IRQF_TRIGGER_RISING | IRQF_SHARED,
				"amlm5900_wakeup", &amlm5900_wake_interrupt);
	if (ret != 0) {
		printk(KERN_ERR "AML-M5900: no wakeup irq, %d?\n", ret);
	} else {
		enable_irq_wake(IRQ_EINT9);
		/* configure the suspend/resume status pin */
		s3c2410_gpio_cfgpin(S3C2410_GPF2, S3C2410_GPF2_OUTP);
		s3c2410_gpio_pullup(S3C2410_GPF2, 0);
	}
}
static void __init amlm5900_init(void)
{
	amlm5900_init_pm();
#ifdef CONFIG_FB_S3C2410
	s3c24xx_fb_set_platdata(&amlm5900_lcd_info);
#endif
	platform_add_devices(amlm5900_devices, ARRAY_SIZE(amlm5900_devices));
}

MACHINE_START(AML_M5900, "AML_M5900")
	.phys_io	= S3C2410_PA_UART,
	.io_pg_offst	= (((u32)S3C24XX_VA_UART) >> 18) & 0xfffc,
	.boot_params	= S3C2410_SDRAM_PA + 0x100,
	.map_io		= amlm5900_map_io,
	.init_irq	= s3c24xx_init_irq,
	.init_machine	= amlm5900_init,
	.timer		= &s3c24xx_timer,
MACHINE_END
