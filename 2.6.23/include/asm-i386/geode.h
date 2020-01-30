/*
 * AMD Geode definitions
 * Copyright (C) 2006, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef _ASM_GEODE_H_
#define _ASM_GEODE_H_

#include <asm/processor.h>
#include <linux/io.h>

/* Generic southbridge functions */

#define GEODE_DEV_PMS 0
#define GEODE_DEV_ACPI 1
#define GEODE_DEV_GPIO 2
#define GEODE_DEV_MFGPT 3

extern int geode_get_dev_base(unsigned int dev);

/* Useful macros */
#define geode_pms_base()	geode_get_dev_base(GEODE_DEV_PMS)
#define geode_acpi_base()	geode_get_dev_base(GEODE_DEV_ACPI)
#define geode_gpio_base()	geode_get_dev_base(GEODE_DEV_GPIO)
#define geode_mfgpt_base()	geode_get_dev_base(GEODE_DEV_MFGPT)

/* MSRS */

#define GX_GLCP_SYS_RSTPLL	0x4C000014

#define MSR_LBAR_SMB		0x5140000B
#define MSR_LBAR_GPIO		0x5140000C
#define MSR_LBAR_MFGPT		0x5140000D
#define MSR_LBAR_ACPI		0x5140000E
#define MSR_LBAR_PMS		0x5140000F

#define MSR_PIC_YSEL_LOW	0x51400020
#define MSR_PIC_YSEL_HIGH	0x51400021
#define MSR_PIC_ZSEL_LOW	0x51400022
#define MSR_PIC_ZSEL_HIGH	0x51400023

#define MFGPT_IRQ_MSR		0x51400028
#define MFGPT_NR_MSR		0x51400029

/* Resource Sizes */

#define LBAR_GPIO_SIZE		0xFF
#define LBAR_MFGPT_SIZE		0x40
#define LBAR_ACPI_SIZE		0x40
#define LBAR_PMS_SIZE		0x80

/* ACPI registers (PMS block) */

/*
 * PM1_EN is only valid when VSA is enabled for 16 bit reads.
 * When VSA is not enabled, *always* read both PM1_STS and PM1_EN
 * with a 32 bit read at offset 0x0
 */

#define PM1_STS			0x00
#define PM1_EN			0x02
#define PM1_CNT			0x08
#define PM2_CNT			0x0C
#define PM_TMR			0x10
#define PM_GPE0_STS		0x18
#define PM_GPE0_EN		0x1C

/* PMC registers (PMS block) */

#define PM_SSD			0x00
#define PM_SCXA			0x04
#define PM_SCYA			0x08
#define PM_OUT_SLPCTL		0x0C
#define PM_SCLK			0x10
#define PM_SED			0x1
#define PM_SCXD			0x18
#define PM_SCYD			0x1C
#define PM_IN_SLPCTL		0x20
#define PM_WKD			0x30
#define PM_WKXD			0x34
#define PM_RD			0x38
#define PM_WKXA			0x3C
#define PM_FSD			0x40
#define PM_TSD			0x44
#define PM_PSD			0x48
#define PM_NWKD			0x4C
#define PM_AWKD			0x50
#define PM_SSC			0x54

/* GPIO */

#define GPIO_OUTPUT_VAL		0x00
#define GPIO_OUTPUT_ENABLE	0x04
#define GPIO_OUTPUT_OPEN_DRAIN	0x08
#define GPIO_OUTPUT_INVERT	0x0C
#define GPIO_OUTPUT_AUX1	0x10
#define GPIO_OUTPUT_AUX2	0x14
#define GPIO_PULL_UP		0x18
#define GPIO_PULL_DOWN		0x1C
#define GPIO_INPUT_ENABLE	0x20
#define GPIO_INPUT_INVERT	0x24
#define GPIO_INPUT_FILTER	0x28
#define GPIO_INPUT_EVENT_COUNT	0x2C
#define GPIO_READ_BACK		0x30
#define GPIO_INPUT_AUX1		0x34
#define GPIO_EVENTS_ENABLE	0x38
#define GPIO_LOCK_ENABLE	0x3C
#define GPIO_POSITIVE_EDGE_EN	0x40
#define GPIO_NEGATIVE_EDGE_EN	0x44
#define GPIO_POSITIVE_EDGE_STS	0x48
#define GPIO_NEGATIVE_EDGE_STS	0x4C

#define GPIO_MAP_X		0xE0
#define GPIO_MAP_Y		0xE4
#define GPIO_MAP_Z		0xE8
#define GPIO_MAP_W		0xEC

extern void geode_gpio_set(unsigned int, unsigned int);
extern void geode_gpio_clear(unsigned int, unsigned int);
extern int geode_gpio_isset(unsigned int, unsigned int);
extern void geode_gpio_setup_event(unsigned int, int, int);
extern void geode_gpio_set_irq(unsigned int, unsigned int);

static inline void geode_gpio_event_irq(unsigned int gpio, int pair)
{
	geode_gpio_setup_event(gpio, pair, 0);
}

static inline void geode_gpio_event_pme(unsigned int gpio, int pair)
{
	geode_gpio_setup_event(gpio, pair, 1);
}

/* Specific geode tests */

static inline int is_geode_gx(void)
{
	return ((boot_cpu_data.x86_vendor == X86_VENDOR_NSC) &&
		(boot_cpu_data.x86 == 5) &&
		(boot_cpu_data.x86_model == 5));
}

static inline int is_geode_lx(void)
{
	return ((boot_cpu_data.x86_vendor == X86_VENDOR_AMD) &&
		(boot_cpu_data.x86 == 5) &&
		(boot_cpu_data.x86_model == 10));
}

static inline int is_geode(void)
{
	return (is_geode_gx() || is_geode_lx());
}

#endif
