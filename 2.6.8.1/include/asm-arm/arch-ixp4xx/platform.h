/*
 * include/asm-arm/arch-ixp4xx/platform.h
 *
 * Constants and functions that are useful to IXP4xx platform-specific code
 * and device drivers.
 *
 * Copyright (C) 2004 MontaVista Software, Inc.
 */

#ifndef __ASM_ARCH_HARDWARE_H__
#error "Do not include this directly, instead #include <asm/hardware.h>"
#endif

#ifndef __ASSEMBLY__

#include <asm/types.h>

/*
 * Expansion bus memory regions
 */
#define IXP4XX_EXP_BUS_BASE_PHYS	(0x50000000)

#define	IXP4XX_EXP_BUS_CSX_REGION_SIZE	(0x01000000)

#define IXP4XX_EXP_BUS_CS0_BASE_PHYS	(IXP4XX_EXP_BUS_BASE_PHYS + 0x00000000)
#define IXP4XX_EXP_BUS_CS1_BASE_PHYS	(IXP4XX_EXP_BUS_BASE_PHYS + 0x01000000)
#define IXP4XX_EXP_BUS_CS2_BASE_PHYS	(IXP4XX_EXP_BUS_BASE_PHYS + 0x02000000)
#define IXP4XX_EXP_BUS_CS3_BASE_PHYS	(IXP4XX_EXP_BUS_BASE_PHYS + 0x03000000)
#define IXP4XX_EXP_BUS_CS4_BASE_PHYS	(IXP4XX_EXP_BUS_BASE_PHYS + 0x04000000)
#define IXP4XX_EXP_BUS_CS5_BASE_PHYS	(IXP4XX_EXP_BUS_BASE_PHYS + 0x05000000)
#define IXP4XX_EXP_BUS_CS6_BASE_PHYS	(IXP4XX_EXP_BUS_BASE_PHYS + 0x06000000)
#define IXP4XX_EXP_BUS_CS7_BASE_PHYS	(IXP4XX_EXP_BUS_BASE_PHYS + 0x07000000)

#define IXP4XX_FLASH_WRITABLE	(0x2)
#define IXP4XX_FLASH_DEFAULT	(0xbcd23c40)
#define IXP4XX_FLASH_WRITE	(0xbcd23c42)

/*
 * Clock Speed Definitions.
 */
#define IXP4XX_PERIPHERAL_BUS_CLOCK 	(66) /* 66Mhzi APB BUS   */ 
#define IXP4XX_UART_XTAL        	14745600

/*
 * The IXP4xx chips do not have an I2C unit, so GPIO lines are just
 * used to 
 * Used as platform_data to provide GPIO pin information to the ixp42x
 * I2C driver.
 */
struct ixp4xx_i2c_pins {
	unsigned long sda_pin;
	unsigned long scl_pin;
};


/*
 * Functions used by platform-level setup code
 */
extern void ixp4xx_map_io(void);
extern void ixp4xx_init_irq(void);
extern void ixp4xx_init_time(void);
extern void ixp4xx_pci_preinit(void);
struct pci_sys_data;
extern int ixp4xx_setup(int nr, struct pci_sys_data *sys);
extern struct pci_bus *ixp4xx_scan_bus(int nr, struct pci_sys_data *sys);

/*
 * GPIO-functions
 */
/*
 * The following converted to the real HW bits the gpio_line_config
 */
/* GPIO pin types */
#define IXP4XX_GPIO_OUT 		0x1
#define IXP4XX_GPIO_IN  		0x2

#define IXP4XX_GPIO_INTSTYLE_MASK	0x7C  /* Bits [6:2] define interrupt style */

/* 
 * GPIO interrupt types.
 */
#define IXP4XX_GPIO_ACTIVE_HIGH		0x4 /* Default */
#define IXP4XX_GPIO_ACTIVE_LOW		0x8
#define IXP4XX_GPIO_RISING_EDGE		0x10
#define IXP4XX_GPIO_FALLING_EDGE 	0x20
#define IXP4XX_GPIO_TRANSITIONAL 	0x40

/* GPIO signal types */
#define IXP4XX_GPIO_LOW			0
#define IXP4XX_GPIO_HIGH		1

/* GPIO Clocks */
#define IXP4XX_GPIO_CLK_0		14
#define IXP4XX_GPIO_CLK_1		15

extern void gpio_line_config(u8 line, u32 style);

static inline void gpio_line_get(u8 line, int *value)
{
	*value = (*IXP4XX_GPIO_GPINR >> line) & 0x1;
}

static inline void gpio_line_set(u8 line, int value)
{
	if (value == IXP4XX_GPIO_HIGH)
	    *IXP4XX_GPIO_GPOUTR |= (1 << line);
	else if (value == IXP4XX_GPIO_LOW)
	    *IXP4XX_GPIO_GPOUTR &= ~(1 << line);
}

static inline void gpio_line_isr_clear(u8 line)
{
	*IXP4XX_GPIO_GPISR = (1 << line);
}

#endif // __ASSEMBLY__

