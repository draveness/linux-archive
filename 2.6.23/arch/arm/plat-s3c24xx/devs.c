/* linux/arch/arm/plat-s3c24xx/devs.c
 *
 * Copyright (c) 2004 Simtec Electronics
 *	Ben Dooks <ben@simtec.co.uk>
 *
 * Base S3C24XX platform device definitions
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
*/

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/init.h>
#include <linux/serial_core.h>
#include <linux/platform_device.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>
#include <asm/arch/fb.h>
#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/irq.h>

#include <asm/plat-s3c/regs-serial.h>
#include <asm/plat-s3c24xx/udc.h>

#include <asm/plat-s3c24xx/devs.h>
#include <asm/plat-s3c24xx/cpu.h>
#include <asm/plat-s3c24xx/regs-spi.h>

/* Serial port registrations */

static struct resource s3c2410_uart0_resource[] = {
	[0] = {
		.start = S3C2410_PA_UART0,
		.end   = S3C2410_PA_UART0 + 0x3fff,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_S3CUART_RX0,
		.end   = IRQ_S3CUART_ERR0,
		.flags = IORESOURCE_IRQ,
	}
};

static struct resource s3c2410_uart1_resource[] = {
	[0] = {
		.start = S3C2410_PA_UART1,
		.end   = S3C2410_PA_UART1 + 0x3fff,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_S3CUART_RX1,
		.end   = IRQ_S3CUART_ERR1,
		.flags = IORESOURCE_IRQ,
	}
};

static struct resource s3c2410_uart2_resource[] = {
	[0] = {
		.start = S3C2410_PA_UART2,
		.end   = S3C2410_PA_UART2 + 0x3fff,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_S3CUART_RX2,
		.end   = IRQ_S3CUART_ERR2,
		.flags = IORESOURCE_IRQ,
	}
};

struct s3c24xx_uart_resources s3c2410_uart_resources[] __initdata = {
	[0] = {
		.resources	= s3c2410_uart0_resource,
		.nr_resources	= ARRAY_SIZE(s3c2410_uart0_resource),
	},
	[1] = {
		.resources	= s3c2410_uart1_resource,
		.nr_resources	= ARRAY_SIZE(s3c2410_uart1_resource),
	},
	[2] = {
		.resources	= s3c2410_uart2_resource,
		.nr_resources	= ARRAY_SIZE(s3c2410_uart2_resource),
	},
};

/* yart devices */

static struct platform_device s3c24xx_uart_device0 = {
	.id		= 0,
};

static struct platform_device s3c24xx_uart_device1 = {
	.id		= 1,
};

static struct platform_device s3c24xx_uart_device2 = {
	.id		= 2,
};

struct platform_device *s3c24xx_uart_src[3] = {
	&s3c24xx_uart_device0,
	&s3c24xx_uart_device1,
	&s3c24xx_uart_device2,
};

struct platform_device *s3c24xx_uart_devs[3] = {
};

/* USB Host Controller */

static struct resource s3c_usb_resource[] = {
	[0] = {
		.start = S3C24XX_PA_USBHOST,
		.end   = S3C24XX_PA_USBHOST + S3C24XX_SZ_USBHOST - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_USBH,
		.end   = IRQ_USBH,
		.flags = IORESOURCE_IRQ,
	}
};

static u64 s3c_device_usb_dmamask = 0xffffffffUL;

struct platform_device s3c_device_usb = {
	.name		  = "s3c2410-ohci",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_usb_resource),
	.resource	  = s3c_usb_resource,
	.dev              = {
		.dma_mask = &s3c_device_usb_dmamask,
		.coherent_dma_mask = 0xffffffffUL
	}
};

EXPORT_SYMBOL(s3c_device_usb);

/* LCD Controller */

static struct resource s3c_lcd_resource[] = {
	[0] = {
		.start = S3C24XX_PA_LCD,
		.end   = S3C24XX_PA_LCD + S3C24XX_SZ_LCD - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_LCD,
		.end   = IRQ_LCD,
		.flags = IORESOURCE_IRQ,
	}

};

static u64 s3c_device_lcd_dmamask = 0xffffffffUL;

struct platform_device s3c_device_lcd = {
	.name		  = "s3c2410-lcd",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_lcd_resource),
	.resource	  = s3c_lcd_resource,
	.dev              = {
		.dma_mask		= &s3c_device_lcd_dmamask,
		.coherent_dma_mask	= 0xffffffffUL
	}
};

EXPORT_SYMBOL(s3c_device_lcd);

void __init s3c24xx_fb_set_platdata(struct s3c2410fb_mach_info *pd)
{
	struct s3c2410fb_mach_info *npd;

	npd = kmalloc(sizeof(*npd), GFP_KERNEL);
	if (npd) {
		memcpy(npd, pd, sizeof(*npd));
		s3c_device_lcd.dev.platform_data = npd;
	} else {
		printk(KERN_ERR "no memory for LCD platform data\n");
	}
}

/* NAND Controller */

static struct resource s3c_nand_resource[] = {
	[0] = {
		.start = S3C2410_PA_NAND,
		.end   = S3C2410_PA_NAND + S3C24XX_SZ_NAND - 1,
		.flags = IORESOURCE_MEM,
	}
};

struct platform_device s3c_device_nand = {
	.name		  = "s3c2410-nand",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_nand_resource),
	.resource	  = s3c_nand_resource,
};

EXPORT_SYMBOL(s3c_device_nand);

/* USB Device (Gadget)*/

static struct resource s3c_usbgadget_resource[] = {
	[0] = {
		.start = S3C24XX_PA_USBDEV,
		.end   = S3C24XX_PA_USBDEV + S3C24XX_SZ_USBDEV - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_USBD,
		.end   = IRQ_USBD,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_usbgadget = {
	.name		  = "s3c2410-usbgadget",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_usbgadget_resource),
	.resource	  = s3c_usbgadget_resource,
};

EXPORT_SYMBOL(s3c_device_usbgadget);

void __init s3c24xx_udc_set_platdata(struct s3c2410_udc_mach_info *pd)
{
	struct s3c2410_udc_mach_info *npd;

	npd = kmalloc(sizeof(*npd), GFP_KERNEL);
	if (npd) {
		memcpy(npd, pd, sizeof(*npd));
		s3c_device_usbgadget.dev.platform_data = npd;
	} else {
		printk(KERN_ERR "no memory for udc platform data\n");
	}
}


/* Watchdog */

static struct resource s3c_wdt_resource[] = {
	[0] = {
		.start = S3C24XX_PA_WATCHDOG,
		.end   = S3C24XX_PA_WATCHDOG + S3C24XX_SZ_WATCHDOG - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_WDT,
		.end   = IRQ_WDT,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_wdt = {
	.name		  = "s3c2410-wdt",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_wdt_resource),
	.resource	  = s3c_wdt_resource,
};

EXPORT_SYMBOL(s3c_device_wdt);

/* I2C */

static struct resource s3c_i2c_resource[] = {
	[0] = {
		.start = S3C24XX_PA_IIC,
		.end   = S3C24XX_PA_IIC + S3C24XX_SZ_IIC - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_IIC,
		.end   = IRQ_IIC,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_i2c = {
	.name		  = "s3c2410-i2c",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_i2c_resource),
	.resource	  = s3c_i2c_resource,
};

EXPORT_SYMBOL(s3c_device_i2c);

/* IIS */

static struct resource s3c_iis_resource[] = {
	[0] = {
		.start = S3C24XX_PA_IIS,
		.end   = S3C24XX_PA_IIS + S3C24XX_SZ_IIS -1,
		.flags = IORESOURCE_MEM,
	}
};

static u64 s3c_device_iis_dmamask = 0xffffffffUL;

struct platform_device s3c_device_iis = {
	.name		  = "s3c2410-iis",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_iis_resource),
	.resource	  = s3c_iis_resource,
	.dev              = {
		.dma_mask = &s3c_device_iis_dmamask,
		.coherent_dma_mask = 0xffffffffUL
	}
};

EXPORT_SYMBOL(s3c_device_iis);

/* RTC */

static struct resource s3c_rtc_resource[] = {
	[0] = {
		.start = S3C24XX_PA_RTC,
		.end   = S3C24XX_PA_RTC + 0xff,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_RTC,
		.end   = IRQ_RTC,
		.flags = IORESOURCE_IRQ,
	},
	[2] = {
		.start = IRQ_TICK,
		.end   = IRQ_TICK,
		.flags = IORESOURCE_IRQ
	}
};

struct platform_device s3c_device_rtc = {
	.name		  = "s3c2410-rtc",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_rtc_resource),
	.resource	  = s3c_rtc_resource,
};

EXPORT_SYMBOL(s3c_device_rtc);

/* ADC */

static struct resource s3c_adc_resource[] = {
	[0] = {
		.start = S3C24XX_PA_ADC,
		.end   = S3C24XX_PA_ADC + S3C24XX_SZ_ADC - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_TC,
		.end   = IRQ_TC,
		.flags = IORESOURCE_IRQ,
	},
	[2] = {
		.start = IRQ_ADC,
		.end   = IRQ_ADC,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_adc = {
	.name		  = "s3c2410-adc",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_adc_resource),
	.resource	  = s3c_adc_resource,
};

/* SDI */

static struct resource s3c_sdi_resource[] = {
	[0] = {
		.start = S3C2410_PA_SDI,
		.end   = S3C2410_PA_SDI + S3C24XX_SZ_SDI - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_SDI,
		.end   = IRQ_SDI,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_sdi = {
	.name		  = "s3c2410-sdi",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_sdi_resource),
	.resource	  = s3c_sdi_resource,
};

EXPORT_SYMBOL(s3c_device_sdi);

/* High-speed MMC/SD */

static struct resource s3c_hsmmc_resource[] = {
	[0] = {
		.start = S3C2443_PA_HSMMC,
		.end   = S3C2443_PA_HSMMC + S3C2443_SZ_HSMMC - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_S3C2443_HSMMC,
		.end   = IRQ_S3C2443_HSMMC,
		.flags = IORESOURCE_IRQ,
	}
};

static u64 s3c_device_hsmmc_dmamask = 0xffffffffUL;

struct platform_device s3c_device_hsmmc = {
	.name		  = "s3c-sdhci",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_hsmmc_resource),
	.resource	  = s3c_hsmmc_resource,
	.dev              = {
		.dma_mask = &s3c_device_hsmmc_dmamask,
		.coherent_dma_mask = 0xffffffffUL
	}
};



/* SPI (0) */

static struct resource s3c_spi0_resource[] = {
	[0] = {
		.start = S3C24XX_PA_SPI,
		.end   = S3C24XX_PA_SPI + 0x1f,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_SPI0,
		.end   = IRQ_SPI0,
		.flags = IORESOURCE_IRQ,
	}

};

static u64 s3c_device_spi0_dmamask = 0xffffffffUL;

struct platform_device s3c_device_spi0 = {
	.name		  = "s3c2410-spi",
	.id		  = 0,
	.num_resources	  = ARRAY_SIZE(s3c_spi0_resource),
	.resource	  = s3c_spi0_resource,
        .dev              = {
                .dma_mask = &s3c_device_spi0_dmamask,
                .coherent_dma_mask = 0xffffffffUL
        }
};

EXPORT_SYMBOL(s3c_device_spi0);

/* SPI (1) */

static struct resource s3c_spi1_resource[] = {
	[0] = {
		.start = S3C24XX_PA_SPI + S3C2410_SPI1,
		.end   = S3C24XX_PA_SPI + S3C2410_SPI1 + 0x1f,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_SPI1,
		.end   = IRQ_SPI1,
		.flags = IORESOURCE_IRQ,
	}

};

static u64 s3c_device_spi1_dmamask = 0xffffffffUL;

struct platform_device s3c_device_spi1 = {
	.name		  = "s3c2410-spi",
	.id		  = 1,
	.num_resources	  = ARRAY_SIZE(s3c_spi1_resource),
	.resource	  = s3c_spi1_resource,
        .dev              = {
                .dma_mask = &s3c_device_spi1_dmamask,
                .coherent_dma_mask = 0xffffffffUL
        }
};

EXPORT_SYMBOL(s3c_device_spi1);

/* pwm timer blocks */

static struct resource s3c_timer0_resource[] = {
	[0] = {
		.start = S3C24XX_PA_TIMER + 0x0C,
		.end   = S3C24XX_PA_TIMER + 0x0C + 0xB,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_TIMER0,
		.end   = IRQ_TIMER0,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_timer0 = {
	.name		  = "s3c2410-timer",
	.id		  = 0,
	.num_resources	  = ARRAY_SIZE(s3c_timer0_resource),
	.resource	  = s3c_timer0_resource,
};

EXPORT_SYMBOL(s3c_device_timer0);

/* timer 1 */

static struct resource s3c_timer1_resource[] = {
	[0] = {
		.start = S3C24XX_PA_TIMER + 0x18,
		.end   = S3C24XX_PA_TIMER + 0x23,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_TIMER1,
		.end   = IRQ_TIMER1,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_timer1 = {
	.name		  = "s3c2410-timer",
	.id		  = 1,
	.num_resources	  = ARRAY_SIZE(s3c_timer1_resource),
	.resource	  = s3c_timer1_resource,
};

EXPORT_SYMBOL(s3c_device_timer1);

/* timer 2 */

static struct resource s3c_timer2_resource[] = {
	[0] = {
		.start = S3C24XX_PA_TIMER + 0x24,
		.end   = S3C24XX_PA_TIMER + 0x2F,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_TIMER2,
		.end   = IRQ_TIMER2,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_timer2 = {
	.name		  = "s3c2410-timer",
	.id		  = 2,
	.num_resources	  = ARRAY_SIZE(s3c_timer2_resource),
	.resource	  = s3c_timer2_resource,
};

EXPORT_SYMBOL(s3c_device_timer2);

/* timer 3 */

static struct resource s3c_timer3_resource[] = {
	[0] = {
		.start = S3C24XX_PA_TIMER + 0x30,
		.end   = S3C24XX_PA_TIMER + 0x3B,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_TIMER3,
		.end   = IRQ_TIMER3,
		.flags = IORESOURCE_IRQ,
	}

};

struct platform_device s3c_device_timer3 = {
	.name		  = "s3c2410-timer",
	.id		  = 3,
	.num_resources	  = ARRAY_SIZE(s3c_timer3_resource),
	.resource	  = s3c_timer3_resource,
};

EXPORT_SYMBOL(s3c_device_timer3);

#ifdef CONFIG_CPU_S3C2440

/* Camif Controller */

static struct resource s3c_camif_resource[] = {
	[0] = {
		.start = S3C2440_PA_CAMIF,
		.end   = S3C2440_PA_CAMIF + S3C2440_SZ_CAMIF - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_CAM,
		.end   = IRQ_CAM,
		.flags = IORESOURCE_IRQ,
	}

};

static u64 s3c_device_camif_dmamask = 0xffffffffUL;

struct platform_device s3c_device_camif = {
	.name		  = "s3c2440-camif",
	.id		  = -1,
	.num_resources	  = ARRAY_SIZE(s3c_camif_resource),
	.resource	  = s3c_camif_resource,
	.dev              = {
		.dma_mask = &s3c_device_camif_dmamask,
		.coherent_dma_mask = 0xffffffffUL
	}
};

EXPORT_SYMBOL(s3c_device_camif);

#endif // CONFIG_CPU_S32440
