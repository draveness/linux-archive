/*
 * ULI M1575 setup code - specific to Freescale boards
 *
 * Copyright 2007 Freescale Semiconductor Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/mc146818rtc.h>

#include <asm/system.h>
#include <asm/pci-bridge.h>

#define ULI_PIRQA	0x08
#define ULI_PIRQB	0x09
#define ULI_PIRQC	0x0a
#define ULI_PIRQD	0x0b
#define ULI_PIRQE	0x0c
#define ULI_PIRQF	0x0d
#define ULI_PIRQG	0x0e

#define ULI_8259_NONE	0x00
#define ULI_8259_IRQ1	0x08
#define ULI_8259_IRQ3	0x02
#define ULI_8259_IRQ4	0x04
#define ULI_8259_IRQ5	0x05
#define ULI_8259_IRQ6	0x07
#define ULI_8259_IRQ7	0x06
#define ULI_8259_IRQ9	0x01
#define ULI_8259_IRQ10	0x03
#define ULI_8259_IRQ11	0x09
#define ULI_8259_IRQ12	0x0b
#define ULI_8259_IRQ14	0x0d
#define ULI_8259_IRQ15	0x0f

u8 uli_pirq_to_irq[8] = {
	ULI_8259_IRQ9,		/* PIRQA */
	ULI_8259_IRQ10,		/* PIRQB */
	ULI_8259_IRQ11,		/* PIRQC */
	ULI_8259_IRQ12,		/* PIRQD */
	ULI_8259_IRQ5,		/* PIRQE */
	ULI_8259_IRQ6,		/* PIRQF */
	ULI_8259_IRQ7,		/* PIRQG */
	ULI_8259_NONE,		/* PIRQH */
};

/* set in board code if you want this quirks to do something */
int uses_fsl_uli_m1575;

/* Bridge */
static void __devinit early_uli5249(struct pci_dev *dev)
{
	unsigned char temp;

	if (!uses_fsl_uli_m1575)
		return;

	pci_write_config_word(dev, PCI_COMMAND, PCI_COMMAND_IO |
		 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

	/* read/write lock */
	pci_read_config_byte(dev, 0x7c, &temp);
	pci_write_config_byte(dev, 0x7c, 0x80);

	/* set as P2P bridge */
	pci_write_config_byte(dev, PCI_CLASS_PROG, 0x01);
	dev->class |= 0x1;

	/* restore lock */
	pci_write_config_byte(dev, 0x7c, temp);
}


static void __devinit quirk_uli1575(struct pci_dev *dev)
{
	int i;

	if (!uses_fsl_uli_m1575)
		return;

	/*
	 * ULI1575 interrupts route setup
	 */

	/* ULI1575 IRQ mapping conf register maps PIRQx to IRQn */
	for (i = 0; i < 4; i++) {
		u8 val = uli_pirq_to_irq[i*2] | (uli_pirq_to_irq[i*2+1] << 4);
		pci_write_config_byte(dev, 0x48 + i, val);
	}

	/* USB 1.1 OHCI controller 1: dev 28, func 0 - IRQ12 */
	pci_write_config_byte(dev, 0x86, ULI_PIRQD);

	/* USB 1.1 OHCI controller 2: dev 28, func 1 - IRQ9 */
	pci_write_config_byte(dev, 0x87, ULI_PIRQA);

	/* USB 1.1 OHCI controller 3: dev 28, func 2 - IRQ10 */
	pci_write_config_byte(dev, 0x88, ULI_PIRQB);

	/* Lan controller: dev 27, func 0 - IRQ6 */
	pci_write_config_byte(dev, 0x89, ULI_PIRQF);

	/* AC97 Audio controller: dev 29, func 0 - IRQ6 */
	pci_write_config_byte(dev, 0x8a, ULI_PIRQF);

	/* Modem controller: dev 29, func 1 - IRQ6 */
	pci_write_config_byte(dev, 0x8b, ULI_PIRQF);

	/* HD Audio controller: dev 29, func 2 - IRQ6 */
	pci_write_config_byte(dev, 0x8c, ULI_PIRQF);

	/* SATA controller: dev 31, func 1 - IRQ5 */
	pci_write_config_byte(dev, 0x8d, ULI_PIRQE);

	/* SMB interrupt: dev 30, func 1 - IRQ7 */
	pci_write_config_byte(dev, 0x8e, ULI_PIRQG);

	/* PMU ACPI SCI interrupt: dev 30, func 2 - IRQ7 */
	pci_write_config_byte(dev, 0x8f, ULI_PIRQG);

	/* USB 2.0 controller: dev 28, func 3 */
	pci_write_config_byte(dev, 0x74, ULI_8259_IRQ11);

	/* Primary PATA IDE IRQ: 14
	 * Secondary PATA IDE IRQ: 15
	 */
	pci_write_config_byte(dev, 0x44, 0x30 | ULI_8259_IRQ14);
	pci_write_config_byte(dev, 0x75, ULI_8259_IRQ15);
}

static void __devinit quirk_final_uli1575(struct pci_dev *dev)
{
	/* Set i8259 interrupt trigger
	 * IRQ 3:  Level
	 * IRQ 4:  Level
	 * IRQ 5:  Level
	 * IRQ 6:  Level
	 * IRQ 7:  Level
	 * IRQ 9:  Level
	 * IRQ 10: Level
	 * IRQ 11: Level
	 * IRQ 12: Level
	 * IRQ 14: Edge
	 * IRQ 15: Edge
	 */
	if (!uses_fsl_uli_m1575)
		return;

	outb(0xfa, 0x4d0);
	outb(0x1e, 0x4d1);

	/* setup RTC */
	CMOS_WRITE(RTC_SET, RTC_CONTROL);
	CMOS_WRITE(RTC_24H, RTC_CONTROL);

	/* ensure month, date, and week alarm fields are ignored */
	CMOS_WRITE(0, RTC_VALID);

	outb_p(0x7c, 0x72);
	outb_p(RTC_ALARM_DONT_CARE, 0x73);

	outb_p(0x7d, 0x72);
	outb_p(RTC_ALARM_DONT_CARE, 0x73);
}

/* SATA */
static void __devinit quirk_uli5288(struct pci_dev *dev)
{
	unsigned char c;
	unsigned int d;

	if (!uses_fsl_uli_m1575)
		return;

	/* read/write lock */
	pci_read_config_byte(dev, 0x83, &c);
	pci_write_config_byte(dev, 0x83, c|0x80);

	pci_read_config_dword(dev, PCI_CLASS_REVISION, &d);
	d = (d & 0xff) | (PCI_CLASS_STORAGE_SATA_AHCI << 8);
	pci_write_config_dword(dev, PCI_CLASS_REVISION, d);

	/* restore lock */
	pci_write_config_byte(dev, 0x83, c);

	/* disable emulated PATA mode enabled */
	pci_read_config_byte(dev, 0x84, &c);
	pci_write_config_byte(dev, 0x84, c & ~0x01);
}

/* PATA */
static void __devinit quirk_uli5229(struct pci_dev *dev)
{
	unsigned short temp;

	if (!uses_fsl_uli_m1575)
		return;

	pci_write_config_word(dev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE |
		PCI_COMMAND_MASTER | PCI_COMMAND_IO);

	/* Enable Native IRQ 14/15 */
	pci_read_config_word(dev, 0x4a, &temp);
	pci_write_config_word(dev, 0x4a, temp | 0x1000);
}

/* We have to do a dummy read on the P2P for the RTC to work, WTF */
static void __devinit quirk_final_uli5249(struct pci_dev *dev)
{
	int i;
	u8 *dummy;
	struct pci_bus *bus = dev->bus;

	for (i = 0; i < PCI_BUS_NUM_RESOURCES; i++) {
		if ((bus->resource[i]) &&
			(bus->resource[i]->flags & IORESOURCE_MEM)) {
			dummy = ioremap(bus->resource[i]->start, 0x4);
			if (dummy) {
				in_8(dummy);
				iounmap(dummy);
			}
			break;
		}
	}
}

DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_AL, 0x5249, early_uli5249);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AL, 0x1575, quirk_uli1575);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AL, 0x5288, quirk_uli5288);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AL, 0x5229, quirk_uli5229);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AL, 0x5249, quirk_final_uli5249);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AL, 0x1575, quirk_final_uli1575);

int uli_exclude_device(struct pci_controller *hose,
			u_char bus, u_char devfn)
{
	if (bus == (hose->first_busno + 2)) {
		/* exclude Modem controller */
		if ((PCI_SLOT(devfn) == 29) && (PCI_FUNC(devfn) == 1))
			return PCIBIOS_DEVICE_NOT_FOUND;

		/* exclude HD Audio controller */
		if ((PCI_SLOT(devfn) == 29) && (PCI_FUNC(devfn) == 2))
			return PCIBIOS_DEVICE_NOT_FOUND;
	}

	return PCIBIOS_SUCCESSFUL;
}
