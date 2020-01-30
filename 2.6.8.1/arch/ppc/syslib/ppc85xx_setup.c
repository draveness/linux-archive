/*
 * arch/ppc/syslib/ppc85xx_setup.c
 *
 * MPC85XX common board code
 *
 * Maintainer: Kumar Gala <kumar.gala@freescale.com>
 *
 * Copyright 2004 Freescale Semiconductor Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/serial.h>
#include <linux/tty.h>	/* for linux/serial_core.h */
#include <linux/serial_core.h>

#include <asm/prom.h>
#include <asm/time.h>
#include <asm/mpc85xx.h>
#include <asm/immap_85xx.h>
#include <asm/mmu.h>
#include <asm/ocp.h>
#include <asm/kgdb.h>

#include <syslib/ppc85xx_setup.h>

/* Return the amount of memory */
unsigned long __init
mpc85xx_find_end_of_memory(void)
{
        bd_t *binfo;

        binfo = (bd_t *) __res;

        return binfo->bi_memsize;
}

/* The decrementer counts at the system (internal) clock freq divided by 8 */
void __init
mpc85xx_calibrate_decr(void)
{
        bd_t *binfo = (bd_t *) __res;
        unsigned int freq, divisor;

        /* get the core frequency */
        freq = binfo->bi_busfreq;

        /* The timebase is updated every 8 bus clocks, HID0[SEL_TBCLK] = 0 */
        divisor = 8;
        tb_ticks_per_jiffy = freq / divisor / HZ;
        tb_to_us = mulhwu_scale_factor(freq / divisor, 1000000);

	/* Set the time base to zero */
	mtspr(SPRN_TBWL, 0);
	mtspr(SPRN_TBWU, 0);

	/* Clear any pending timer interrupts */
	mtspr(SPRN_TSR, TSR_ENW | TSR_WIS | TSR_DIS | TSR_FIS);

	/* Enable decrementer interrupt */
	mtspr(SPRN_TCR, TCR_DIE);
}

#ifdef CONFIG_SERIAL_8250
void __init
mpc85xx_early_serial_map(void)
{
	struct uart_port serial_req;
	bd_t *binfo = (bd_t *) __res;
	phys_addr_t duart_paddr = binfo->bi_immr_base + MPC85xx_UART0_OFFSET;

	/* Setup serial port access */
	memset(&serial_req, 0, sizeof (serial_req));
	serial_req.uartclk = binfo->bi_busfreq;
	serial_req.line = 0;
	serial_req.irq = MPC85xx_IRQ_DUART;
	serial_req.flags = ASYNC_BOOT_AUTOCONF | ASYNC_SKIP_TEST;
	serial_req.iotype = SERIAL_IO_MEM;
	serial_req.membase = ioremap(duart_paddr, MPC85xx_UART0_SIZE);
	serial_req.mapbase = duart_paddr;
	serial_req.regshift = 0;

#if defined(CONFIG_SERIAL_TEXT_DEBUG) || defined(CONFIG_KGDB)
	gen550_init(0, &serial_req);
#endif

	if (early_serial_setup(&serial_req) != 0)
		printk("Early serial init of port 0 failed\n");

	/* Assume early_serial_setup() doesn't modify serial_req */
	duart_paddr = binfo->bi_immr_base + MPC85xx_UART1_OFFSET;
	serial_req.line = 1;
	serial_req.mapbase = duart_paddr;
	serial_req.membase = ioremap(duart_paddr, MPC85xx_UART1_SIZE);

#if defined(CONFIG_SERIAL_TEXT_DEBUG) || defined(CONFIG_KGDB)
	gen550_init(1, &serial_req);
#endif

	if (early_serial_setup(&serial_req) != 0)
		printk("Early serial init of port 1 failed\n");
}
#endif

void
mpc85xx_restart(char *cmd)
{
	local_irq_disable();
	abort();
}

void
mpc85xx_power_off(void)
{
	local_irq_disable();
	for(;;);
}

void
mpc85xx_halt(void)
{
	local_irq_disable();
	for(;;);
}

#ifdef CONFIG_PCI
static void __init
mpc85xx_setup_pci1(struct pci_controller *hose)
{
	volatile struct ccsr_pci *pci;
	volatile struct ccsr_guts *guts;
	unsigned short temps;
	bd_t *binfo = (bd_t *) __res;

	pci = ioremap(binfo->bi_immr_base + MPC85xx_PCI1_OFFSET,
		    MPC85xx_PCI1_SIZE);

	guts = ioremap(binfo->bi_immr_base + MPC85xx_GUTS_OFFSET,
		    MPC85xx_GUTS_SIZE);

	early_read_config_word(hose, 0, 0, PCI_COMMAND, &temps);
	temps |= PCI_COMMAND_SERR | PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY;
	early_write_config_word(hose, 0, 0, PCI_COMMAND, temps);

#define PORDEVSR_PCI	(0x00800000)	/* PCI Mode */
	if (guts->pordevsr & PORDEVSR_PCI) {
 		early_write_config_byte(hose, 0, 0, PCI_LATENCY_TIMER, 0x80);
 	} else {
		/* PCI-X init */
		temps = PCI_X_CMD_MAX_SPLIT | PCI_X_CMD_MAX_READ
			| PCI_X_CMD_ERO | PCI_X_CMD_DPERR_E;
		early_write_config_word(hose, 0, 0, PCIX_COMMAND, temps);
	}

	/* Disable all windows (except powar0 since its ignored) */
	pci->powar1 = 0;
	pci->powar2 = 0;
	pci->powar3 = 0;
	pci->powar4 = 0;
	pci->piwar1 = 0;
	pci->piwar2 = 0;
	pci->piwar3 = 0;

	/* Setup Phys:PCI 1:1 outbound mem window @ MPC85XX_PCI1_LOWER_MEM */
	pci->potar1 = (MPC85XX_PCI1_LOWER_MEM >> 12) & 0x000fffff;
	pci->potear1 = 0x00000000;
	pci->powbar1 = (MPC85XX_PCI1_LOWER_MEM >> 12) & 0x000fffff;
	/* Enable, Mem R/W */
	pci->powar1 = 0x80044000 |
	   (__ilog2(MPC85XX_PCI1_UPPER_MEM - MPC85XX_PCI1_LOWER_MEM + 1) - 1);

	/* Setup outboud IO windows @ MPC85XX_PCI1_IO_BASE */
	pci->potar2 = 0x00000000;
	pci->potear2 = 0x00000000;
	pci->powbar2 = (MPC85XX_PCI1_IO_BASE >> 12) & 0x000fffff;
	/* Enable, IO R/W */
	pci->powar2 = 0x80088000 | (__ilog2(MPC85XX_PCI1_IO_SIZE) - 1);

	/* Setup 2G inbound Memory Window @ 0 */
	pci->pitar1 = 0x00000000;
	pci->piwbar1 = 0x00000000;
	pci->piwar1 = 0xa0f5501e;	/* Enable, Prefetch, Local
					   Mem, Snoop R/W, 2G */
}


extern int mpc85xx_map_irq(struct pci_dev *dev, unsigned char idsel, unsigned char pin);
extern int mpc85xx_exclude_device(u_char bus, u_char devfn);

#ifdef CONFIG_85xx_PCI2
static void __init
mpc85xx_setup_pci2(struct pci_controller *hose)
{
	volatile struct ccsr_pci *pci;
	unsigned short temps;
	bd_t *binfo = (bd_t *) __res;

	pci = ioremap(binfo->bi_immr_base + MPC85xx_PCI2_OFFSET,
		    MPC85xx_PCI2_SIZE);

	early_read_config_word(hose, hose->bus_offset, 0, PCI_COMMAND, &temps);
	temps |= PCI_COMMAND_SERR | PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY;
	early_write_config_word(hose, hose->bus_offset, 0, PCI_COMMAND, temps);
	early_write_config_byte(hose, hose->bus_offset, 0, PCI_LATENCY_TIMER, 0x80);

	/* Disable all windows (except powar0 since its ignored) */
	pci->powar1 = 0;
	pci->powar2 = 0;
	pci->powar3 = 0;
	pci->powar4 = 0;
	pci->piwar1 = 0;
	pci->piwar2 = 0;
	pci->piwar3 = 0;

	/* Setup Phys:PCI 1:1 outbound mem window @ MPC85XX_PCI2_LOWER_MEM */
	pci->potar1 = (MPC85XX_PCI2_LOWER_MEM >> 12) & 0x000fffff;
	pci->potear1 = 0x00000000;
	pci->powbar1 = (MPC85XX_PCI2_LOWER_MEM >> 12) & 0x000fffff;
	/* Enable, Mem R/W */
	pci->powar1 = 0x80044000 |
	   (__ilog2(MPC85XX_PCI1_UPPER_MEM - MPC85XX_PCI1_LOWER_MEM + 1) - 1);

	/* Setup outboud IO windows @ MPC85XX_PCI2_IO_BASE */
	pci->potar2 = 0x00000000;
	pci->potear2 = 0x00000000;
	pci->powbar2 = (MPC85XX_PCI2_IO_BASE >> 12) & 0x000fffff;
	/* Enable, IO R/W */
	pci->powar2 = 0x80088000 | (__ilog2(MPC85XX_PCI1_IO_SIZE) - 1);

	/* Setup 2G inbound Memory Window @ 0 */
	pci->pitar1 = 0x00000000;
	pci->piwbar1 = 0x00000000;
	pci->piwar1 = 0xa0f5501e;	/* Enable, Prefetch, Local
					   Mem, Snoop R/W, 2G */
}
#endif /* CONFIG_85xx_PCI2 */

void __init
mpc85xx_setup_hose(void)
{
	struct pci_controller *hose_a;
#ifdef CONFIG_85xx_PCI2
	struct pci_controller *hose_b;
#endif
	bd_t *binfo = (bd_t *) __res;

	hose_a = pcibios_alloc_controller();

	if (!hose_a)
		return;

	ppc_md.pci_swizzle = common_swizzle;
	ppc_md.pci_map_irq = mpc85xx_map_irq;

	hose_a->first_busno = 0;
	hose_a->bus_offset = 0;
	hose_a->last_busno = 0xff;

	setup_indirect_pci(hose_a, binfo->bi_immr_base + PCI1_CFG_ADDR_OFFSET,
			   binfo->bi_immr_base + PCI1_CFG_DATA_OFFSET);
	hose_a->set_cfg_type = 1;

	mpc85xx_setup_pci1(hose_a);

	hose_a->pci_mem_offset = MPC85XX_PCI1_MEM_OFFSET;
	hose_a->mem_space.start = MPC85XX_PCI1_LOWER_MEM;
	hose_a->mem_space.end = MPC85XX_PCI1_UPPER_MEM;

	hose_a->io_space.start = MPC85XX_PCI1_LOWER_IO;
	hose_a->io_space.end = MPC85XX_PCI1_UPPER_IO;
	hose_a->io_base_phys = MPC85XX_PCI1_IO_BASE;
#if CONFIG_85xx_PCI2
	isa_io_base =
		(unsigned long) ioremap(MPC85XX_PCI1_IO_BASE,
					MPC85XX_PCI1_IO_SIZE +
					MPC85XX_PCI2_IO_SIZE);
#else
	isa_io_base =
		(unsigned long) ioremap(MPC85XX_PCI1_IO_BASE,
					MPC85XX_PCI1_IO_SIZE);
#endif
	hose_a->io_base_virt = (void *) isa_io_base;

	/* setup resources */
	pci_init_resource(&hose_a->mem_resources[0],
			MPC85XX_PCI1_LOWER_MEM,
			MPC85XX_PCI1_UPPER_MEM,
			IORESOURCE_MEM, "PCI1 host bridge");

	pci_init_resource(&hose_a->io_resource,
			MPC85XX_PCI1_LOWER_IO,
			MPC85XX_PCI1_UPPER_IO,
			IORESOURCE_IO, "PCI1 host bridge");

	ppc_md.pci_exclude_device = mpc85xx_exclude_device;

	hose_a->last_busno = pciauto_bus_scan(hose_a, hose_a->first_busno);

#if CONFIG_85xx_PCI2
	hose_b = pcibios_alloc_controller();

	if (!hose_b)
		return;

	hose_b->bus_offset = hose_a->last_busno + 1;
	hose_b->first_busno = hose_a->last_busno + 1;
	hose_b->last_busno = 0xff;

	setup_indirect_pci(hose_b, binfo->bi_immr_base + PCI2_CFG_ADDR_OFFSET,
			   binfo->bi_immr_base + PCI2_CFG_DATA_OFFSET);
	hose_b->set_cfg_type = 1;

	mpc85xx_setup_pci2(hose_b);

	hose_b->pci_mem_offset = MPC85XX_PCI2_MEM_OFFSET;
	hose_b->mem_space.start = MPC85XX_PCI2_LOWER_MEM;
	hose_b->mem_space.end = MPC85XX_PCI2_UPPER_MEM;

	hose_b->io_space.start = MPC85XX_PCI2_LOWER_IO;
	hose_b->io_space.end = MPC85XX_PCI2_UPPER_IO;
	hose_b->io_base_phys = MPC85XX_PCI2_IO_BASE;
	hose_b->io_base_virt = (void *) isa_io_base + MPC85XX_PCI1_IO_SIZE;

	/* setup resources */
	pci_init_resource(&hose_b->mem_resources[0],
			MPC85XX_PCI2_LOWER_MEM,
			MPC85XX_PCI2_UPPER_MEM,
			IORESOURCE_MEM, "PCI2 host bridge");

	pci_init_resource(&hose_b->io_resource,
			MPC85XX_PCI2_LOWER_IO,
			MPC85XX_PCI2_UPPER_IO,
			IORESOURCE_IO, "PCI2 host bridge");

	hose_b->last_busno = pciauto_bus_scan(hose_b, hose_b->first_busno);
#endif
	return;
}
#endif /* CONFIG_PCI */


