/*
 * SH7780 Setup
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

static struct resource rtc_resources[] = {
	[0] = {
		.start	= 0xffe80000,
		.end	= 0xffe80000 + 0x58 - 1,
		.flags	= IORESOURCE_IO,
	},
	[1] = {
		/* Period IRQ */
		.start	= 21,
		.flags	= IORESOURCE_IRQ,
	},
	[2] = {
		/* Carry IRQ */
		.start	= 22,
		.flags	= IORESOURCE_IRQ,
	},
	[3] = {
		/* Alarm IRQ */
		.start	= 20,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device rtc_device = {
	.name		= "sh-rtc",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(rtc_resources),
	.resource	= rtc_resources,
};

static struct plat_sci_port sci_platform_data[] = {
	{
		.mapbase	= 0xffe00000,
		.flags		= UPF_BOOT_AUTOCONF,
		.type		= PORT_SCIF,
		.irqs		= { 40, 41, 43, 42 },
	}, {
		.mapbase	= 0xffe10000,
		.flags		= UPF_BOOT_AUTOCONF,
		.type		= PORT_SCIF,
		.irqs		= { 76, 77, 79, 78 },
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

static struct platform_device *sh7780_devices[] __initdata = {
	&rtc_device,
	&sci_device,
};

static int __init sh7780_devices_setup(void)
{
	return platform_add_devices(sh7780_devices,
				    ARRAY_SIZE(sh7780_devices));
}
__initcall(sh7780_devices_setup);

enum {
	UNUSED = 0,

	/* interrupt sources */

	IRL_LLLL, IRL_LLLH, IRL_LLHL, IRL_LLHH,
	IRL_LHLL, IRL_LHLH, IRL_LHHL, IRL_LHHH,
	IRL_HLLL, IRL_HLLH, IRL_HLHL, IRL_HLHH,
	IRL_HHLL, IRL_HHLH, IRL_HHHL,

	IRQ0, IRQ1, IRQ2, IRQ3, IRQ4, IRQ5, IRQ6, IRQ7,
	RTC_ATI, RTC_PRI, RTC_CUI,
	WDT,
	TMU0, TMU1, TMU2, TMU2_TICPI,
	HUDI,
	DMAC0_DMINT0, DMAC0_DMINT1, DMAC0_DMINT2, DMAC0_DMINT3, DMAC0_DMAE,
	SCIF0_ERI, SCIF0_RXI, SCIF0_BRI, SCIF0_TXI,
	DMAC0_DMINT4, DMAC0_DMINT5, DMAC1_DMINT6, DMAC1_DMINT7,
	CMT, HAC,
	PCISERR, PCIINTA, PCIINTB, PCIINTC, PCIINTD,
	PCIERR, PCIPWD3, PCIPWD2, PCIPWD1, PCIPWD0,
	SCIF1_ERI, SCIF1_RXI, SCIF1_BRI, SCIF1_TXI,
	SIOF, HSPI,
	MMCIF_FSTAT, MMCIF_TRAN, MMCIF_ERR, MMCIF_FRDY,
	DMAC1_DMINT8, DMAC1_DMINT9, DMAC1_DMINT10, DMAC1_DMINT11,
	TMU3, TMU4, TMU5,
	SSI,
	FLCTL_FLSTE, FLCTL_FLEND, FLCTL_FLTRQ0, FLCTL_FLTRQ1,
	GPIOI0, GPIOI1, GPIOI2, GPIOI3,

	/* interrupt groups */

	RTC, TMU012, DMAC0, SCIF0, DMAC45, DMAC1,
	PCIC5, SCIF1, MMCIF, TMU345, FLCTL, GPIO,
};

static struct intc_vect vectors[] = {
	INTC_VECT(RTC_ATI, 0x480), INTC_VECT(RTC_PRI, 0x4a0),
	INTC_VECT(RTC_CUI, 0x4c0),
	INTC_VECT(WDT, 0x560),
	INTC_VECT(TMU0, 0x580), INTC_VECT(TMU1, 0x5a0),
	INTC_VECT(TMU2, 0x5c0), INTC_VECT(TMU2_TICPI, 0x5e0),
	INTC_VECT(HUDI, 0x600),
	INTC_VECT(DMAC0_DMINT0, 0x640), INTC_VECT(DMAC0_DMINT1, 0x660),
	INTC_VECT(DMAC0_DMINT2, 0x680), INTC_VECT(DMAC0_DMINT3, 0x6a0),
	INTC_VECT(DMAC0_DMAE, 0x6c0),
	INTC_VECT(SCIF0_ERI, 0x700), INTC_VECT(SCIF0_RXI, 0x720),
	INTC_VECT(SCIF0_BRI, 0x740), INTC_VECT(SCIF0_TXI, 0x760),
	INTC_VECT(DMAC0_DMINT4, 0x780), INTC_VECT(DMAC0_DMINT5, 0x7a0),
	INTC_VECT(DMAC1_DMINT6, 0x7c0), INTC_VECT(DMAC1_DMINT7, 0x7e0),
	INTC_VECT(CMT, 0x900), INTC_VECT(HAC, 0x980),
	INTC_VECT(PCISERR, 0xa00), INTC_VECT(PCIINTA, 0xa20),
	INTC_VECT(PCIINTB, 0xa40), INTC_VECT(PCIINTC, 0xa60),
	INTC_VECT(PCIINTD, 0xa80), INTC_VECT(PCIERR, 0xaa0),
	INTC_VECT(PCIPWD3, 0xac0), INTC_VECT(PCIPWD2, 0xae0),
	INTC_VECT(PCIPWD1, 0xb00), INTC_VECT(PCIPWD0, 0xb20),
	INTC_VECT(SCIF1_ERI, 0xb80), INTC_VECT(SCIF1_RXI, 0xba0),
	INTC_VECT(SCIF1_BRI, 0xbc0), INTC_VECT(SCIF1_TXI, 0xbe0),
	INTC_VECT(SIOF, 0xc00), INTC_VECT(HSPI, 0xc80),
	INTC_VECT(MMCIF_FSTAT, 0xd00), INTC_VECT(MMCIF_TRAN, 0xd20),
	INTC_VECT(MMCIF_ERR, 0xd40), INTC_VECT(MMCIF_FRDY, 0xd60),
	INTC_VECT(DMAC1_DMINT8, 0xd80), INTC_VECT(DMAC1_DMINT9, 0xda0),
	INTC_VECT(DMAC1_DMINT10, 0xdc0), INTC_VECT(DMAC1_DMINT11, 0xde0),
	INTC_VECT(TMU3, 0xe00), INTC_VECT(TMU4, 0xe20),
	INTC_VECT(TMU5, 0xe40),
	INTC_VECT(SSI, 0xe80),
	INTC_VECT(FLCTL_FLSTE, 0xf00), INTC_VECT(FLCTL_FLEND, 0xf20),
	INTC_VECT(FLCTL_FLTRQ0, 0xf40), INTC_VECT(FLCTL_FLTRQ1, 0xf60),
	INTC_VECT(GPIOI0, 0xf80), INTC_VECT(GPIOI1, 0xfa0),
	INTC_VECT(GPIOI2, 0xfc0), INTC_VECT(GPIOI3, 0xfe0),
};

static struct intc_group groups[] = {
	INTC_GROUP(RTC, RTC_ATI, RTC_PRI, RTC_CUI),
	INTC_GROUP(TMU012, TMU0, TMU1, TMU2, TMU2_TICPI),
	INTC_GROUP(DMAC0, DMAC0_DMINT0, DMAC0_DMINT1, DMAC0_DMINT2,
		   DMAC0_DMINT3, DMAC0_DMINT4, DMAC0_DMINT5, DMAC0_DMAE),
	INTC_GROUP(SCIF0, SCIF0_ERI, SCIF0_RXI, SCIF0_BRI, SCIF0_TXI),
	INTC_GROUP(DMAC1, DMAC1_DMINT6, DMAC1_DMINT7, DMAC1_DMINT8,
		   DMAC1_DMINT9, DMAC1_DMINT10, DMAC1_DMINT11),
	INTC_GROUP(PCIC5, PCIERR, PCIPWD3, PCIPWD2, PCIPWD1, PCIPWD0),
	INTC_GROUP(SCIF1, SCIF1_ERI, SCIF1_RXI, SCIF1_BRI, SCIF1_TXI),
	INTC_GROUP(MMCIF, MMCIF_FSTAT, MMCIF_TRAN, MMCIF_ERR, MMCIF_FRDY),
	INTC_GROUP(TMU345, TMU3, TMU4, TMU5),
	INTC_GROUP(FLCTL, FLCTL_FLSTE, FLCTL_FLEND,
		   FLCTL_FLTRQ0, FLCTL_FLTRQ1),
	INTC_GROUP(GPIO, GPIOI0, GPIOI1, GPIOI2, GPIOI3),
};

static struct intc_prio priorities[] = {
	INTC_PRIO(SCIF0, 3),
	INTC_PRIO(SCIF1, 3),
};

static struct intc_mask_reg mask_registers[] = {
	{ 0xffd40038, 0xffd4003c, 32, /* INT2MSKR / INT2MSKCR */
	  { 0, 0, 0, 0, 0, 0, GPIO, FLCTL,
	    SSI, MMCIF, HSPI, SIOF, PCIC5, PCIINTD, PCIINTC, PCIINTB,
	    PCIINTA, PCISERR, HAC, CMT, 0, 0, DMAC1, DMAC0,
	    HUDI, 0, WDT, SCIF1, SCIF0, RTC, TMU345, TMU012 } },
};

static struct intc_prio_reg prio_registers[] = {
	{ 0xffd40000, 32, 8, /* INT2PRI0 */ { TMU0, TMU1, TMU2, TMU2_TICPI } },
	{ 0xffd40004, 32, 8, /* INT2PRI1 */ { TMU3, TMU4, TMU5, RTC } },
	{ 0xffd40008, 32, 8, /* INT2PRI2 */ { SCIF0, SCIF1, WDT } },
	{ 0xffd4000c, 32, 8, /* INT2PRI3 */ { HUDI, DMAC0, DMAC1 } },
	{ 0xffd40010, 32, 8, /* INT2PRI4 */ { CMT, HAC, PCISERR, PCIINTA, } },
	{ 0xffd40014, 32, 8, /* INT2PRI5 */ { PCIINTB, PCIINTC,
					      PCIINTD, PCIC5 } },
	{ 0xffd40018, 32, 8, /* INT2PRI6 */ { SIOF, HSPI, MMCIF, SSI } },
	{ 0xffd4001c, 32, 8, /* INT2PRI7 */ { FLCTL, GPIO } },
};

static DECLARE_INTC_DESC(intc_desc, "sh7780", vectors, groups, priorities,
			 mask_registers, prio_registers, NULL);

/* Support for external interrupt pins in IRQ mode */

static struct intc_vect irq_vectors[] = {
	INTC_VECT(IRQ0, 0x240), INTC_VECT(IRQ1, 0x280),
	INTC_VECT(IRQ2, 0x2c0), INTC_VECT(IRQ3, 0x300),
	INTC_VECT(IRQ4, 0x340), INTC_VECT(IRQ5, 0x380),
	INTC_VECT(IRQ6, 0x3c0), INTC_VECT(IRQ7, 0x200),
};

static struct intc_mask_reg irq_mask_registers[] = {
	{ 0xffd00044, 0xffd00064, 32, /* INTMSK0 / INTMSKCLR0 */
	  { IRQ0, IRQ1, IRQ2, IRQ3, IRQ4, IRQ5, IRQ6, IRQ7 } },
};

static struct intc_prio_reg irq_prio_registers[] = {
	{ 0xffd00010, 32, 4, /* INTPRI */ { IRQ0, IRQ1, IRQ2, IRQ3,
					    IRQ4, IRQ5, IRQ6, IRQ7 } },
};

static struct intc_sense_reg irq_sense_registers[] = {
	{ 0xffd0001c, 32, 2, /* ICR1 */   { IRQ0, IRQ1, IRQ2, IRQ3,
					    IRQ4, IRQ5, IRQ6, IRQ7 } },
};

static DECLARE_INTC_DESC(intc_irq_desc, "sh7780-irq", irq_vectors,
			 NULL, NULL, irq_mask_registers, irq_prio_registers,
			 irq_sense_registers);

/* External interrupt pins in IRL mode */

static struct intc_vect irl_vectors[] = {
	INTC_VECT(IRL_LLLL, 0x200), INTC_VECT(IRL_LLLH, 0x220),
	INTC_VECT(IRL_LLHL, 0x240), INTC_VECT(IRL_LLHH, 0x260),
	INTC_VECT(IRL_LHLL, 0x280), INTC_VECT(IRL_LHLH, 0x2a0),
	INTC_VECT(IRL_LHHL, 0x2c0), INTC_VECT(IRL_LHHH, 0x2e0),
	INTC_VECT(IRL_HLLL, 0x300), INTC_VECT(IRL_HLLH, 0x320),
	INTC_VECT(IRL_HLHL, 0x340), INTC_VECT(IRL_HLHH, 0x360),
	INTC_VECT(IRL_HHLL, 0x380), INTC_VECT(IRL_HHLH, 0x3a0),
	INTC_VECT(IRL_HHHL, 0x3c0),
};

static struct intc_mask_reg irl3210_mask_registers[] = {
	{ 0xffd00080, 0xffd00084, 32, /* INTMSK2 / INTMSKCLR2 */
	  { IRL_LLLL, IRL_LLLH, IRL_LLHL, IRL_LLHH,
	    IRL_LHLL, IRL_LHLH, IRL_LHHL, IRL_LHHH,
	    IRL_HLLL, IRL_HLLH, IRL_HLHL, IRL_HLHH,
	    IRL_HHLL, IRL_HHLH, IRL_HHHL, } },
};

static struct intc_mask_reg irl7654_mask_registers[] = {
	{ 0xffd00080, 0xffd00084, 32, /* INTMSK2 / INTMSKCLR2 */
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    IRL_LLLL, IRL_LLLH, IRL_LLHL, IRL_LLHH,
	    IRL_LHLL, IRL_LHLH, IRL_LHHL, IRL_LHHH,
	    IRL_HLLL, IRL_HLLH, IRL_HLHL, IRL_HLHH,
	    IRL_HHLL, IRL_HHLH, IRL_HHHL, } },
};

static DECLARE_INTC_DESC(intc_irl7654_desc, "sh7780-irl7654", irl_vectors,
			 NULL, NULL, irl7654_mask_registers, NULL, NULL);

static DECLARE_INTC_DESC(intc_irl3210_desc, "sh7780-irl3210", irl_vectors,
			 NULL, NULL, irl3210_mask_registers, NULL, NULL);

void __init plat_irq_setup(void)
{
	register_intc_controller(&intc_desc);
}

void __init plat_irq_setup_pins(int mode)
{
	switch (mode) {
	case IRQ_MODE_IRQ:
		register_intc_controller(&intc_irq_desc);
		break;
	case IRQ_MODE_IRL7654:
		register_intc_controller(&intc_irl7654_desc);
		break;
	case IRQ_MODE_IRL3210:
		register_intc_controller(&intc_irl3210_desc);
		break;
	default:
		BUG();
	}
}
