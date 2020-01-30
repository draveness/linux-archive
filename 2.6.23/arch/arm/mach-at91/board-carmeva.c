/*
 * linux/arch/arm/mach-at91/board-carmeva.c
 *
 *  Copyright (c) 2005 Peer Georgi
 *  		       Conitec Datasystems
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

#include <linux/types.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <asm/hardware.h>
#include <asm/setup.h>
#include <asm/mach-types.h>
#include <asm/irq.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>

#include <asm/arch/board.h>
#include <asm/arch/gpio.h>

#include "generic.h"


/*
 * Serial port configuration.
 *    0 .. 3 = USART0 .. USART3
 *    4      = DBGU
 */
static struct at91_uart_config __initdata carmeva_uart_config = {
	.console_tty	= 0,				/* ttyS0 */
	.nr_tty		= 2,
	.tty_map	= { 4, 1, -1, -1, -1 }		/* ttyS0, ..., ttyS4 */
};

static void __init carmeva_map_io(void)
{
	/* Initialize processor: 20.000 MHz crystal */
	at91rm9200_initialize(20000000, AT91RM9200_BGA);

	/* Setup the serial ports and console */
	at91_init_serial(&carmeva_uart_config);
}

static void __init carmeva_init_irq(void)
{
	at91rm9200_init_interrupts(NULL);
}

static struct at91_eth_data __initdata carmeva_eth_data = {
	.phy_irq_pin	= AT91_PIN_PC4,
	.is_rmii	= 1,
};

static struct at91_usbh_data __initdata carmeva_usbh_data = {
	.ports		= 2,
};

static struct at91_udc_data __initdata carmeva_udc_data = {
	.vbus_pin	= AT91_PIN_PD12,
	.pullup_pin	= AT91_PIN_PD9,
};

/* FIXME: user dependant */
// static struct at91_cf_data __initdata carmeva_cf_data = {
//	.det_pin	= AT91_PIN_PB0,
//	.rst_pin	= AT91_PIN_PC5,
	// .irq_pin	= ... not connected
	// .vcc_pin	= ... always powered
// };

static struct at91_mmc_data __initdata carmeva_mmc_data = {
	.slot_b		= 0,
	.wire4		= 1,
	.det_pin	= AT91_PIN_PB10,
	.wp_pin		= AT91_PIN_PC14,
};

static struct spi_board_info carmeva_spi_devices[] = {
	{ /* DataFlash chip */
		.modalias = "mtd_dataflash",
		.chip_select  = 0,
		.max_speed_hz = 10 * 1000 * 1000,
	},
	{ /* User accessible spi - cs1 (250KHz) */
		.modalias = "spi-cs1",
		.chip_select  = 1,
		.max_speed_hz = 250 *  1000,
	},
	{ /* User accessible spi - cs2 (1MHz) */
		.modalias = "spi-cs2",
		.chip_select  = 2,
		.max_speed_hz = 1 * 1000 *  1000,
	},
	{ /* User accessible spi - cs3 (10MHz) */
		.modalias = "spi-cs3",
		.chip_select  = 3,
		.max_speed_hz = 10 * 1000 *  1000,
	},
};

static void __init carmeva_board_init(void)
{
	/* Serial */
	at91_add_device_serial();
	/* Ethernet */
	at91_add_device_eth(&carmeva_eth_data);
	/* USB Host */
	at91_add_device_usbh(&carmeva_usbh_data);
	/* USB Device */
	at91_add_device_udc(&carmeva_udc_data);
	/* I2C */
	at91_add_device_i2c();
	/* SPI */
	at91_add_device_spi(carmeva_spi_devices, ARRAY_SIZE(carmeva_spi_devices));
	/* Compact Flash */
//	at91_add_device_cf(&carmeva_cf_data);
	/* MMC */
	at91_add_device_mmc(0, &carmeva_mmc_data);
}

MACHINE_START(CARMEVA, "Carmeva")
	/* Maintainer: Conitec Datasystems */
	.phys_io	= AT91_BASE_SYS,
	.io_pg_offst	= (AT91_VA_BASE_SYS >> 18) & 0xfffc,
	.boot_params	= AT91_SDRAM_BASE + 0x100,
	.timer		= &at91rm9200_timer,
	.map_io		= carmeva_map_io,
	.init_irq	= carmeva_init_irq,
	.init_machine	= carmeva_board_init,
MACHINE_END
