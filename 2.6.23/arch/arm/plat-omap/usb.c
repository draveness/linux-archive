/*
 * arch/arm/plat-omap/usb.c -- platform level USB initialization
 *
 * Copyright (C) 2004 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#undef	DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/usb/otg.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/system.h>
#include <asm/hardware.h>

#include <asm/arch/mux.h>
#include <asm/arch/usb.h>
#include <asm/arch/board.h>

#ifdef CONFIG_ARCH_OMAP1

#define INT_USB_IRQ_GEN		IH2_BASE + 20
#define INT_USB_IRQ_NISO	IH2_BASE + 30
#define INT_USB_IRQ_ISO		IH2_BASE + 29
#define INT_USB_IRQ_HGEN	INT_USB_HHC_1
#define INT_USB_IRQ_OTG		IH2_BASE + 8

#else

#define INT_USB_IRQ_GEN		INT_24XX_USB_IRQ_GEN
#define INT_USB_IRQ_NISO	INT_24XX_USB_IRQ_NISO
#define INT_USB_IRQ_ISO		INT_24XX_USB_IRQ_ISO
#define INT_USB_IRQ_HGEN	INT_24XX_USB_IRQ_HGEN
#define INT_USB_IRQ_OTG		INT_24XX_USB_IRQ_OTG

#endif


/* These routines should handle the standard chip-specific modes
 * for usb0/1/2 ports, covering basic mux and transceiver setup.
 *
 * Some board-*.c files will need to set up additional mux options,
 * like for suspend handling, vbus sensing, GPIOs, and the D+ pullup.
 */

/* TESTED ON:
 *  - 1611B H2 (with usb1 mini-AB) using standard Mini-B or OTG cables
 *  - 5912 OSK OHCI (with usb0 standard-A), standard A-to-B cables
 *  - 5912 OSK UDC, with *nonstandard* A-to-A cable
 *  - 1510 Innovator UDC with bundled usb0 cable
 *  - 1510 Innovator OHCI with bundled usb1/usb2 cable
 *  - 1510 Innovator OHCI with custom usb0 cable, feeding 5V VBUS
 *  - 1710 custom development board using alternate pin group
 *  - 1710 H3 (with usb1 mini-AB) using standard Mini-B or OTG cables
 */

/*-------------------------------------------------------------------------*/

#ifdef	CONFIG_ARCH_OMAP_OTG

static struct otg_transceiver *xceiv;

/**
 * otg_get_transceiver - find the (single) OTG transceiver driver
 *
 * Returns the transceiver driver, after getting a refcount to it; or
 * null if there is no such transceiver.  The caller is responsible for
 * releasing that count.
 */
struct otg_transceiver *otg_get_transceiver(void)
{
	if (xceiv)
		get_device(xceiv->dev);
	return xceiv;
}
EXPORT_SYMBOL(otg_get_transceiver);

int otg_set_transceiver(struct otg_transceiver *x)
{
	if (xceiv && x)
		return -EBUSY;
	xceiv = x;
	return 0;
}
EXPORT_SYMBOL(otg_set_transceiver);

#endif

/*-------------------------------------------------------------------------*/

#if defined(CONFIG_ARCH_OMAP_OTG) || defined(CONFIG_ARCH_OMAP15XX)

static u32 __init omap_usb0_init(unsigned nwires, unsigned is_device)
{
	u32	syscon1 = 0;

	if (cpu_is_omap24xx())
		CONTROL_DEVCONF_REG &= ~USBT0WRMODEI(USB_BIDIR_TLL);

	if (nwires == 0) {
		if (cpu_class_is_omap1() && !cpu_is_omap15xx()) {
			/* pulldown D+/D- */
			USB_TRANSCEIVER_CTRL_REG &= ~(3 << 1);
		}
		return 0;
	}

	if (is_device) {
		if (cpu_is_omap24xx())
			omap_cfg_reg(J20_24XX_USB0_PUEN);
		else
			omap_cfg_reg(W4_USB_PUEN);
	}

	/* internal transceiver (unavailable on 17xx, 24xx) */
	if (!cpu_class_is_omap2() && nwires == 2) {
		// omap_cfg_reg(P9_USB_DP);
		// omap_cfg_reg(R8_USB_DM);

		if (cpu_is_omap15xx()) {
			/* This works on 1510-Innovator */
			return 0;
		}

		/* NOTES:
		 *  - peripheral should configure VBUS detection!
		 *  - only peripherals may use the internal D+/D- pulldowns
		 *  - OTG support on this port not yet written
		 */

		USB_TRANSCEIVER_CTRL_REG &= ~(7 << 4);
		if (!is_device)
			USB_TRANSCEIVER_CTRL_REG |= (3 << 1);

		return 3 << 16;
	}

	/* alternate pin config, external transceiver */
	if (cpu_is_omap15xx()) {
		printk(KERN_ERR "no usb0 alt pin config on 15xx\n");
		return 0;
	}

	if (cpu_is_omap24xx()) {
		omap_cfg_reg(K18_24XX_USB0_DAT);
		omap_cfg_reg(K19_24XX_USB0_TXEN);
		omap_cfg_reg(J14_24XX_USB0_SE0);
		if (nwires != 3)
			omap_cfg_reg(J18_24XX_USB0_RCV);
	} else {
		omap_cfg_reg(V6_USB0_TXD);
		omap_cfg_reg(W9_USB0_TXEN);
		omap_cfg_reg(W5_USB0_SE0);
		if (nwires != 3)
			omap_cfg_reg(Y5_USB0_RCV);
	}

	/* NOTE:  SPEED and SUSP aren't configured here.  OTG hosts
	 * may be able to use I2C requests to set those bits along
	 * with VBUS switching and overcurrent detection.
	 */

	if (cpu_class_is_omap1() && nwires != 6)
		USB_TRANSCEIVER_CTRL_REG &= ~CONF_USB2_UNI_R;

	switch (nwires) {
	case 3:
		syscon1 = 2;
		if (cpu_is_omap24xx())
			CONTROL_DEVCONF_REG |= USBT0WRMODEI(USB_BIDIR);
		break;
	case 4:
		syscon1 = 1;
		if (cpu_is_omap24xx())
			CONTROL_DEVCONF_REG |= USBT0WRMODEI(USB_BIDIR);
		break;
	case 6:
		syscon1 = 3;
		if (cpu_is_omap24xx()) {
			omap_cfg_reg(J19_24XX_USB0_VP);
			omap_cfg_reg(K20_24XX_USB0_VM);
			CONTROL_DEVCONF_REG |= USBT0WRMODEI(USB_UNIDIR);
		} else {
			omap_cfg_reg(AA9_USB0_VP);
			omap_cfg_reg(R9_USB0_VM);
			USB_TRANSCEIVER_CTRL_REG |= CONF_USB2_UNI_R;
		}
		break;
	default:
		printk(KERN_ERR "illegal usb%d %d-wire transceiver\n",
			0, nwires);
	}
	return syscon1 << 16;
}

static u32 __init omap_usb1_init(unsigned nwires)
{
	u32	syscon1 = 0;

	if (cpu_class_is_omap1() && !cpu_is_omap15xx() && nwires != 6)
		USB_TRANSCEIVER_CTRL_REG &= ~CONF_USB1_UNI_R;
	if (cpu_is_omap24xx())
		CONTROL_DEVCONF_REG &= ~USBT1WRMODEI(USB_BIDIR_TLL);

	if (nwires == 0)
		return 0;

	/* external transceiver */
	if (cpu_class_is_omap1()) {
		omap_cfg_reg(USB1_TXD);
		omap_cfg_reg(USB1_TXEN);
		if (nwires != 3)
			omap_cfg_reg(USB1_RCV);
	}

	if (cpu_is_omap15xx()) {
		omap_cfg_reg(USB1_SEO);
		omap_cfg_reg(USB1_SPEED);
		// SUSP
	} else if (cpu_is_omap1610() || cpu_is_omap5912()) {
		omap_cfg_reg(W13_1610_USB1_SE0);
		omap_cfg_reg(R13_1610_USB1_SPEED);
		// SUSP
	} else if (cpu_is_omap1710()) {
		omap_cfg_reg(R13_1710_USB1_SE0);
		// SUSP
	} else if (cpu_is_omap24xx()) {
		/* NOTE:  board-specific code must set up pin muxing for usb1,
		 * since each signal could come out on either of two balls.
		 */
	} else {
		pr_debug("usb%d cpu unrecognized\n", 1);
		return 0;
	}

	switch (nwires) {
	case 2:
		if (!cpu_is_omap24xx())
			goto bad;
		/* NOTE: board-specific code must override this setting if
		 * this TLL link is not using DP/DM
		 */
		syscon1 = 1;
		CONTROL_DEVCONF_REG |= USBT1WRMODEI(USB_BIDIR_TLL);
		break;
	case 3:
		syscon1 = 2;
		if (cpu_is_omap24xx())
			CONTROL_DEVCONF_REG |= USBT1WRMODEI(USB_BIDIR);
		break;
	case 4:
		syscon1 = 1;
		if (cpu_is_omap24xx())
			CONTROL_DEVCONF_REG |= USBT1WRMODEI(USB_BIDIR);
		break;
	case 6:
		if (cpu_is_omap24xx())
			goto bad;
		syscon1 = 3;
		omap_cfg_reg(USB1_VP);
		omap_cfg_reg(USB1_VM);
		if (!cpu_is_omap15xx())
			USB_TRANSCEIVER_CTRL_REG |= CONF_USB1_UNI_R;
		break;
	default:
bad:
		printk(KERN_ERR "illegal usb%d %d-wire transceiver\n",
			1, nwires);
	}
	return syscon1 << 20;
}

static u32 __init omap_usb2_init(unsigned nwires, unsigned alt_pingroup)
{
	u32	syscon1 = 0;

	if (cpu_is_omap24xx()) {
		CONTROL_DEVCONF_REG &= ~(USBT2WRMODEI(USB_BIDIR_TLL)
					| USBT2TLL5PI);
		alt_pingroup = 0;
	}

	/* NOTE omap1 erratum: must leave USB2_UNI_R set if usb0 in use */
	if (alt_pingroup || nwires == 0)
		return 0;

	if (cpu_class_is_omap1() && !cpu_is_omap15xx() && nwires != 6)
		USB_TRANSCEIVER_CTRL_REG &= ~CONF_USB2_UNI_R;

	/* external transceiver */
	if (cpu_is_omap15xx()) {
		omap_cfg_reg(USB2_TXD);
		omap_cfg_reg(USB2_TXEN);
		omap_cfg_reg(USB2_SEO);
		if (nwires != 3)
			omap_cfg_reg(USB2_RCV);
		/* there is no USB2_SPEED */
	} else if (cpu_is_omap16xx()) {
		omap_cfg_reg(V6_USB2_TXD);
		omap_cfg_reg(W9_USB2_TXEN);
		omap_cfg_reg(W5_USB2_SE0);
		if (nwires != 3)
			omap_cfg_reg(Y5_USB2_RCV);
		// FIXME omap_cfg_reg(USB2_SPEED);
	} else if (cpu_is_omap24xx()) {
		omap_cfg_reg(Y11_24XX_USB2_DAT);
		omap_cfg_reg(AA10_24XX_USB2_SE0);
		if (nwires > 2)
			omap_cfg_reg(AA12_24XX_USB2_TXEN);
		if (nwires > 3)
			omap_cfg_reg(AA6_24XX_USB2_RCV);
	} else {
		pr_debug("usb%d cpu unrecognized\n", 1);
		return 0;
	}
	// if (cpu_class_is_omap1()) omap_cfg_reg(USB2_SUSP);

	switch (nwires) {
	case 2:
		if (!cpu_is_omap24xx())
			goto bad;
		/* NOTE: board-specific code must override this setting if
		 * this TLL link is not using DP/DM
		 */
		syscon1 = 1;
		CONTROL_DEVCONF_REG |= USBT2WRMODEI(USB_BIDIR_TLL);
		break;
	case 3:
		syscon1 = 2;
		if (cpu_is_omap24xx())
			CONTROL_DEVCONF_REG |= USBT2WRMODEI(USB_BIDIR);
		break;
	case 4:
		syscon1 = 1;
		if (cpu_is_omap24xx())
			CONTROL_DEVCONF_REG |= USBT2WRMODEI(USB_BIDIR);
		break;
	case 5:
		if (!cpu_is_omap24xx())
			goto bad;
		omap_cfg_reg(AA4_24XX_USB2_TLLSE0);
		/* NOTE: board-specific code must override this setting if
		 * this TLL link is not using DP/DM.  Something must also
		 * set up OTG_SYSCON2.HMC_TLL{ATTACH,SPEED}
		 */
		syscon1 = 3;
		CONTROL_DEVCONF_REG |= USBT2WRMODEI(USB_UNIDIR_TLL)
					| USBT2TLL5PI;
		break;
	case 6:
		if (cpu_is_omap24xx())
			goto bad;
		syscon1 = 3;
		if (cpu_is_omap15xx()) {
			omap_cfg_reg(USB2_VP);
			omap_cfg_reg(USB2_VM);
		} else {
			omap_cfg_reg(AA9_USB2_VP);
			omap_cfg_reg(R9_USB2_VM);
			USB_TRANSCEIVER_CTRL_REG |= CONF_USB2_UNI_R;
		}
		break;
	default:
bad:
		printk(KERN_ERR "illegal usb%d %d-wire transceiver\n",
			2, nwires);
	}
	return syscon1 << 24;
}

#endif

/*-------------------------------------------------------------------------*/

#if	defined(CONFIG_USB_GADGET_OMAP) || \
	defined(CONFIG_USB_OHCI_HCD) || defined(CONFIG_USB_OHCI_HCD_MODULE) || \
	(defined(CONFIG_USB_OTG) && defined(CONFIG_ARCH_OMAP_OTG))
static void usb_release(struct device *dev)
{
	/* normally not freed */
}
#endif

#ifdef	CONFIG_USB_GADGET_OMAP

static struct resource udc_resources[] = {
	/* order is significant! */
	{		/* registers */
		.start		= UDC_BASE,
		.end		= UDC_BASE + 0xff,
		.flags		= IORESOURCE_MEM,
	}, {		/* general IRQ */
		.start		= INT_USB_IRQ_GEN,
		.flags		= IORESOURCE_IRQ,
	}, {		/* PIO IRQ */
		.start		= INT_USB_IRQ_NISO,
		.flags		= IORESOURCE_IRQ,
	}, {		/* SOF IRQ */
		.start		= INT_USB_IRQ_ISO,
		.flags		= IORESOURCE_IRQ,
	},
};

static u64 udc_dmamask = ~(u32)0;

static struct platform_device udc_device = {
	.name		= "omap_udc",
	.id		= -1,
	.dev = {
		.release		= usb_release,
		.dma_mask		= &udc_dmamask,
		.coherent_dma_mask	= 0xffffffff,
	},
	.num_resources	= ARRAY_SIZE(udc_resources),
	.resource	= udc_resources,
};

#endif

#if	defined(CONFIG_USB_OHCI_HCD) || defined(CONFIG_USB_OHCI_HCD_MODULE)

/* The dmamask must be set for OHCI to work */
static u64 ohci_dmamask = ~(u32)0;

static struct resource ohci_resources[] = {
	{
		.start	= OMAP_OHCI_BASE,
		.end	= OMAP_OHCI_BASE + 0xff,
		.flags	= IORESOURCE_MEM,
	},
	{
		.start	= INT_USB_IRQ_HGEN,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device ohci_device = {
	.name			= "ohci",
	.id			= -1,
	.dev = {
		.release		= usb_release,
		.dma_mask		= &ohci_dmamask,
		.coherent_dma_mask	= 0xffffffff,
	},
	.num_resources	= ARRAY_SIZE(ohci_resources),
	.resource		= ohci_resources,
};

#endif

#if	defined(CONFIG_USB_OTG) && defined(CONFIG_ARCH_OMAP_OTG)

static struct resource otg_resources[] = {
	/* order is significant! */
	{
		.start		= OTG_BASE,
		.end		= OTG_BASE + 0xff,
		.flags		= IORESOURCE_MEM,
	}, {
		.start		= INT_USB_IRQ_OTG,
		.flags		= IORESOURCE_IRQ,
	},
};

static struct platform_device otg_device = {
	.name		= "omap_otg",
	.id		= -1,
	.dev = {
		.release		= usb_release,
	},
	.num_resources	= ARRAY_SIZE(otg_resources),
	.resource	= otg_resources,
};

#endif

/*-------------------------------------------------------------------------*/

#define ULPD_CLOCK_CTRL_REG	__REG16(ULPD_CLOCK_CTRL)
#define ULPD_SOFT_REQ_REG	__REG16(ULPD_SOFT_REQ)


// FIXME correct answer depends on hmc_mode,
// as does (on omap1) any nonzero value for config->otg port number
#ifdef	CONFIG_USB_GADGET_OMAP
#define	is_usb0_device(config)	1
#else
#define	is_usb0_device(config)	0
#endif

/*-------------------------------------------------------------------------*/

#ifdef	CONFIG_ARCH_OMAP_OTG

void __init
omap_otg_init(struct omap_usb_config *config)
{
	u32		syscon = OTG_SYSCON_1_REG & 0xffff;
	int		status;
	int		alt_pingroup = 0;

	/* NOTE:  no bus or clock setup (yet?) */

	syscon = OTG_SYSCON_1_REG & 0xffff;
	if (!(syscon & OTG_RESET_DONE))
		pr_debug("USB resets not complete?\n");

	// OTG_IRQ_EN_REG = 0;

	/* pin muxing and transceiver pinouts */
	if (config->pins[0] > 2)	/* alt pingroup 2 */
		alt_pingroup = 1;
	syscon |= omap_usb0_init(config->pins[0], is_usb0_device(config));
	syscon |= omap_usb1_init(config->pins[1]);
	syscon |= omap_usb2_init(config->pins[2], alt_pingroup);
	pr_debug("OTG_SYSCON_1_REG = %08x\n", syscon);
	OTG_SYSCON_1_REG = syscon;

	syscon = config->hmc_mode;
	syscon |= USBX_SYNCHRO | (4 << 16) /* B_ASE0_BRST */;
#ifdef	CONFIG_USB_OTG
	if (config->otg)
		syscon |= OTG_EN;
#endif
	if (cpu_class_is_omap1())
		pr_debug("USB_TRANSCEIVER_CTRL_REG = %03x\n", USB_TRANSCEIVER_CTRL_REG);
	pr_debug("OTG_SYSCON_2_REG = %08x\n", syscon);
	OTG_SYSCON_2_REG = syscon;

	printk("USB: hmc %d", config->hmc_mode);
	if (!alt_pingroup)
		printk(", usb2 alt %d wires", config->pins[2]);
	else if (config->pins[0])
		printk(", usb0 %d wires%s", config->pins[0],
			is_usb0_device(config) ? " (dev)" : "");
	if (config->pins[1])
		printk(", usb1 %d wires", config->pins[1]);
	if (!alt_pingroup && config->pins[2])
		printk(", usb2 %d wires", config->pins[2]);
	if (config->otg)
		printk(", Mini-AB on usb%d", config->otg - 1);
	printk("\n");

	if (cpu_class_is_omap1()) {
		/* leave USB clocks/controllers off until needed */
		ULPD_SOFT_REQ_REG &= ~SOFT_USB_CLK_REQ;
		ULPD_CLOCK_CTRL_REG &= ~USB_MCLK_EN;
		ULPD_CLOCK_CTRL_REG |= DIS_USB_PVCI_CLK;
	}
	syscon = OTG_SYSCON_1_REG;
	syscon |= HST_IDLE_EN|DEV_IDLE_EN|OTG_IDLE_EN;

#ifdef	CONFIG_USB_GADGET_OMAP
	if (config->otg || config->register_dev) {
		syscon &= ~DEV_IDLE_EN;
		udc_device.dev.platform_data = config;
		/* FIXME patch IRQ numbers for omap730 */
		status = platform_device_register(&udc_device);
		if (status)
			pr_debug("can't register UDC device, %d\n", status);
	}
#endif

#if	defined(CONFIG_USB_OHCI_HCD) || defined(CONFIG_USB_OHCI_HCD_MODULE)
	if (config->otg || config->register_host) {
		syscon &= ~HST_IDLE_EN;
		ohci_device.dev.platform_data = config;
		if (cpu_is_omap730())
			ohci_resources[1].start = INT_730_USB_HHC_1;
		status = platform_device_register(&ohci_device);
		if (status)
			pr_debug("can't register OHCI device, %d\n", status);
	}
#endif

#ifdef	CONFIG_USB_OTG
	if (config->otg) {
		syscon &= ~OTG_IDLE_EN;
		otg_device.dev.platform_data = config;
		if (cpu_is_omap730())
			otg_resources[1].start = INT_730_USB_OTG;
		status = platform_device_register(&otg_device);
		if (status)
			pr_debug("can't register OTG device, %d\n", status);
	}
#endif
	pr_debug("OTG_SYSCON_1_REG = %08x\n", syscon);
	OTG_SYSCON_1_REG = syscon;

	status = 0;
}

#else
static inline void omap_otg_init(struct omap_usb_config *config) {}
#endif

/*-------------------------------------------------------------------------*/

#ifdef	CONFIG_ARCH_OMAP15XX

#define ULPD_DPLL_CTRL_REG	__REG16(ULPD_DPLL_CTRL)
#define DPLL_IOB		(1 << 13)
#define DPLL_PLL_ENABLE		(1 << 4)
#define DPLL_LOCK		(1 << 0)

#define ULPD_APLL_CTRL_REG	__REG16(ULPD_APLL_CTRL)
#define APLL_NDPLL_SWITCH	(1 << 0)


static void __init omap_1510_usb_init(struct omap_usb_config *config)
{
	unsigned int val;

	omap_usb0_init(config->pins[0], is_usb0_device(config));
	omap_usb1_init(config->pins[1]);
	omap_usb2_init(config->pins[2], 0);

	val = omap_readl(MOD_CONF_CTRL_0) & ~(0x3f << 1);
	val |= (config->hmc_mode << 1);
	omap_writel(val, MOD_CONF_CTRL_0);

	printk("USB: hmc %d", config->hmc_mode);
	if (config->pins[0])
		printk(", usb0 %d wires%s", config->pins[0],
			is_usb0_device(config) ? " (dev)" : "");
	if (config->pins[1])
		printk(", usb1 %d wires", config->pins[1]);
	if (config->pins[2])
		printk(", usb2 %d wires", config->pins[2]);
	printk("\n");

	/* use DPLL for 48 MHz function clock */
	pr_debug("APLL %04x DPLL %04x REQ %04x\n", ULPD_APLL_CTRL_REG,
			ULPD_DPLL_CTRL_REG, ULPD_SOFT_REQ_REG);
	ULPD_APLL_CTRL_REG &= ~APLL_NDPLL_SWITCH;
	ULPD_DPLL_CTRL_REG |= DPLL_IOB | DPLL_PLL_ENABLE;
	ULPD_SOFT_REQ_REG |= SOFT_UDC_REQ | SOFT_DPLL_REQ;
	while (!(ULPD_DPLL_CTRL_REG & DPLL_LOCK))
		cpu_relax();

#ifdef	CONFIG_USB_GADGET_OMAP
	if (config->register_dev) {
		int status;

		udc_device.dev.platform_data = config;
		status = platform_device_register(&udc_device);
		if (status)
			pr_debug("can't register UDC device, %d\n", status);
		/* udc driver gates 48MHz by D+ pullup */
	}
#endif

#if	defined(CONFIG_USB_OHCI_HCD) || defined(CONFIG_USB_OHCI_HCD_MODULE)
	if (config->register_host) {
		int status;

		ohci_device.dev.platform_data = config;
		status = platform_device_register(&ohci_device);
		if (status)
			pr_debug("can't register OHCI device, %d\n", status);
		/* hcd explicitly gates 48MHz */
	}
#endif
}

#else
static inline void omap_1510_usb_init(struct omap_usb_config *config) {}
#endif

/*-------------------------------------------------------------------------*/

static struct omap_usb_config platform_data;

static int __init
omap_usb_init(void)
{
	const struct omap_usb_config *config;

	config = omap_get_config(OMAP_TAG_USB, struct omap_usb_config);
	if (config == NULL) {
		printk(KERN_ERR "USB: No board-specific "
				"platform config found\n");
		return -ENODEV;
	}
	platform_data = *config;

	if (cpu_is_omap730() || cpu_is_omap16xx() || cpu_is_omap24xx())
		omap_otg_init(&platform_data);
	else if (cpu_is_omap15xx())
		omap_1510_usb_init(&platform_data);
	else {
		printk(KERN_ERR "USB: No init for your chip yet\n");
		return -ENODEV;
	}
	return 0;
}

subsys_initcall(omap_usb_init);
