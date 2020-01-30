/*
 *  fixup-tb0219.c, The TANBAC TB0219 specific PCI fixups.
 *
 *  Copyright (C) 2003  Megasolution Inc. <matsu@megasolution.jp>
 *  Copyright (C) 2004  Yoichi Yuasa <yuasa@hh.iij4u.or.jp>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/init.h>
#include <linux/pci.h>

#include <asm/vr41xx/tb0219.h>

int __init pcibios_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	int irq = -1;

	switch (slot) {
	case 12:
		vr41xx_set_irq_trigger(TB0219_PCI_SLOT1_PIN,
				       TRIGGER_LEVEL,
				       SIGNAL_THROUGH);
		vr41xx_set_irq_level(TB0219_PCI_SLOT1_PIN,
				     LEVEL_LOW);
		irq = TB0219_PCI_SLOT1_IRQ;
		break;
	case 13:
		vr41xx_set_irq_trigger(TB0219_PCI_SLOT2_PIN,
				       TRIGGER_LEVEL,
				       SIGNAL_THROUGH);
		vr41xx_set_irq_level(TB0219_PCI_SLOT2_PIN,
				     LEVEL_LOW);
		irq = TB0219_PCI_SLOT2_IRQ;
		break;
	case 14:
		vr41xx_set_irq_trigger(TB0219_PCI_SLOT3_PIN,
				       TRIGGER_LEVEL,
				       SIGNAL_THROUGH);
		vr41xx_set_irq_level(TB0219_PCI_SLOT3_PIN,
				     LEVEL_LOW);
		irq = TB0219_PCI_SLOT3_IRQ;
		break;
	default:
		break;
	}

	return irq;
}

struct pci_fixup pcibios_fixups[] __initdata = {
	{	.pass = 0,	},
};
