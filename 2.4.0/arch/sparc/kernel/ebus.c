/* $Id: ebus.c,v 1.15 2000/11/08 05:06:21 davem Exp $
 * ebus.c: PCI to EBus bridge device.
 *
 * Copyright (C) 1997  Eddie C. Dost  (ecd@skynet.be)
 *
 * Adopted for sparc by V. Roganov and G. Raiko.
 * Fixes for different platforms by Pete Zaitcev.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/malloc.h>
#include <linux/string.h>

#include <asm/system.h>
#include <asm/page.h>
#include <asm/pbm.h>
#include <asm/ebus.h>
#include <asm/io.h>
#include <asm/oplib.h>
#include <asm/bpp.h>

struct linux_ebus *ebus_chain = 0;

#ifdef CONFIG_SUN_AUXIO
extern void auxio_probe(void);
#endif

/* We are together with pcic.c under CONFIG_PCI. */
extern unsigned int pcic_pin_to_irq(unsigned int, char *name);

/*
 * IRQ Blacklist
 * Here we list PROMs and systems that are known to supply crap as IRQ numbers.
 */
struct ebus_device_irq {
	char *name;
	unsigned int pin;
};

struct ebus_system_entry {
	char *esname;
	struct ebus_device_irq *ipt;
};

static struct ebus_device_irq je1_1[] = {
	{ "8042",		 3 },
	{ "SUNW,CS4231",	 0 },
	{ "parallel",		 0 },
	{ "se",			 2 },
	{ 0, 0 }
};

/*
 * Gleb's JE1 supplied reasonable pin numbers, but mine did not (OBP 2.32).
 * Blacklist the sucker... Note that Gleb's system will work.
 */
static struct ebus_system_entry ebus_blacklist[] = {
	{ "SUNW,JavaEngine1", je1_1 },
	{ 0, 0 }
};

static struct ebus_device_irq *ebus_blackp = NULL;

/*
 */
static inline unsigned long ebus_alloc(size_t size)
{
	return (unsigned long)kmalloc(size, GFP_ATOMIC);
}

/*
 */
int __init ebus_blacklist_irq(char *name)
{
	struct ebus_device_irq *dp;

	if ((dp = ebus_blackp) != NULL) {
		for (; dp->name != NULL; dp++) {
			if (strcmp(name, dp->name) == 0) {
				return pcic_pin_to_irq(dp->pin, name);
			}
		}
	}
	return 0;
}

void __init fill_ebus_child(int node, struct linux_prom_registers *preg,
				struct linux_ebus_child *dev)
{
	int regs[PROMREG_MAX];
	int irqs[PROMREG_MAX];
	char lbuf[128];
	int i, len;

	dev->prom_node = node;
	prom_getstring(node, "name", lbuf, sizeof(lbuf));
	strcpy(dev->prom_name, lbuf);

	len = prom_getproperty(node, "reg", (void *)regs, sizeof(regs));
	if (len == -1) len = 0;
	dev->num_addrs = len / sizeof(regs[0]);

	for (i = 0; i < dev->num_addrs; i++) {
		if (regs[i] >= dev->parent->num_addrs) {
			prom_printf("UGH: property for %s was %d, need < %d\n",
				    dev->prom_name, len, dev->parent->num_addrs);
			panic(__FUNCTION__);
		}
		dev->resource[i].start = dev->parent->resource[regs[i]].start; /* XXX resource */
	}

	if ((dev->irqs[0] = ebus_blacklist_irq(dev->prom_name)) != 0) {
		dev->num_irqs = 1;
	} else if ((len = prom_getproperty(node, "interrupts",
	    (char *)&irqs, sizeof(irqs))) == -1 || len == 0) {
		dev->num_irqs = 0;
		dev->irqs[0] = 0;
		if (dev->parent->num_irqs != 0) {
			dev->num_irqs = 1;
			dev->irqs[0] = dev->parent->irqs[0];
/* P3 */ /* printk("EBUS: dev %s irq %d from parent\n", dev->prom_name, dev->irqs[0]); */
		}
	} else {
		dev->num_irqs = len / sizeof(irqs[0]);
		if (irqs[0] == 0 || irqs[0] >= 8) {
			/*
			 * XXX Zero is a valid pin number...
			 * This works as long as Ebus is not wired to INTA#.
			 */
			printk("EBUS: %s got bad irq %d from PROM\n",
			    dev->prom_name, irqs[0]);
			dev->num_irqs = 0;
			dev->irqs[0] = 0;
		} else {
			dev->irqs[0] = pcic_pin_to_irq(irqs[0], dev->prom_name);
		}
	}
}

void __init fill_ebus_device(int node, struct linux_ebus_device *dev)
{
	struct linux_prom_registers regs[PROMREG_MAX];
	struct linux_ebus_child *child;
	int irqs[PROMINTR_MAX];
	char lbuf[128];
	int i, n, len;
	unsigned long baseaddr;

	dev->prom_node = node;
	prom_getstring(node, "name", lbuf, sizeof(lbuf));
	strcpy(dev->prom_name, lbuf);

	len = prom_getproperty(node, "reg", (void *)regs, sizeof(regs));
	if (len % sizeof(struct linux_prom_registers)) {
		prom_printf("UGH: proplen for %s was %d, need multiple of %d\n",
			    dev->prom_name, len,
			    (int)sizeof(struct linux_prom_registers));
		panic(__FUNCTION__);
	}
	dev->num_addrs = len / sizeof(struct linux_prom_registers);

	for (i = 0; i < dev->num_addrs; i++) {
		/*
		 * XXX Collect JE-1 PROM
		 * 
		 * Example - JS-E with 3.11:
		 *  /ebus
		 *      regs 
		 *        0x00000000, 0x0, 0x00000000, 0x0, 0x00000000,
		 *        0x82000010, 0x0, 0xf0000000, 0x0, 0x01000000,
		 *        0x82000014, 0x0, 0x38800000, 0x0, 0x00800000,
		 *      ranges
		 *        0x00, 0x00000000, 0x02000010, 0x0, 0x0, 0x01000000,
		 *        0x01, 0x01000000, 0x02000014, 0x0, 0x0, 0x00800000,
		 *  /ebus/8042
		 *      regs
		 *        0x00000001, 0x00300060, 0x00000008,
		 *        0x00000001, 0x00300060, 0x00000008,
		 */
		n = regs[i].which_io;
		if (n >= 4) {
			/* XXX This is copied from old JE-1 by Gleb. */
			n = (regs[i].which_io - 0x10) >> 2;
		} else {
			;
		}

/*
 * XXX Now as we have regions, why don't we make an on-demand allocation...
 */
		dev->resource[i].start = 0;
		if ((baseaddr = dev->bus->self->resource[n].start +
		    regs[i].phys_addr) != 0) {
			/* dev->resource[i].name = dev->prom_name; */
			if ((baseaddr = (unsigned long) ioremap(baseaddr,
			    regs[i].reg_size)) == 0) {
				panic("ebus: unable to remap dev %s",
				    dev->prom_name);
			}
		}
		dev->resource[i].start = baseaddr;	/* XXX Unaligned */
	}

	if ((dev->irqs[0] = ebus_blacklist_irq(dev->prom_name)) != 0) {
		dev->num_irqs = 1;
	} else if ((len = prom_getproperty(node, "interrupts",
	    (char *)&irqs, sizeof(irqs))) == -1 || len == 0) {
		dev->num_irqs = 0;
		if ((dev->irqs[0] = dev->bus->self->irq) != 0) {
			 dev->num_irqs = 1;
/* P3 */ /* printk("EBUS: child %s irq %d from parent\n", dev->prom_name, dev->irqs[0]); */
		}
	} else {
		dev->num_irqs = 1;  /* dev->num_irqs = len / sizeof(irqs[0]); */
		if (irqs[0] == 0 || irqs[0] >= 8) {
			/* See above for the parent. XXX */
			printk("EBUS: %s got bad irq %d from PROM\n",
			    dev->prom_name, irqs[0]);
			dev->num_irqs = 0;
			dev->irqs[0] = 0;
		} else {
			dev->irqs[0] = pcic_pin_to_irq(irqs[0], dev->prom_name);
		}
	}

	if ((node = prom_getchild(node))) {
		dev->children = (struct linux_ebus_child *)
			ebus_alloc(sizeof(struct linux_ebus_child));

		child = dev->children;
		child->next = 0;
		child->parent = dev;
		child->bus = dev->bus;
		fill_ebus_child(node, &regs[0], child);

		while ((node = prom_getsibling(node))) {
			child->next = (struct linux_ebus_child *)
				ebus_alloc(sizeof(struct linux_ebus_child));

			child = child->next;
			child->next = 0;
			child->parent = dev;
			child->bus = dev->bus;
			fill_ebus_child(node, &regs[0], child);
		}
	}
}

void __init ebus_init(void)
{
	struct linux_prom_pci_registers regs[PROMREG_MAX];
	struct linux_pbm_info *pbm;
	struct linux_ebus_device *dev;
	struct linux_ebus *ebus;
	struct ebus_system_entry *sp;
	struct pci_dev *pdev;
	struct pcidev_cookie *cookie;
	char lbuf[128];
	unsigned long addr, *base;
	unsigned short pci_command;
	int nd, len, ebusnd;
	int reg, nreg;
	int num_ebus = 0;

	if (!pci_present())
		return;

	prom_getstring(prom_root_node, "name", lbuf, sizeof(lbuf));
	for (sp = ebus_blacklist; sp->esname != NULL; sp++) {
		if (strcmp(lbuf, sp->esname) == 0) {
			ebus_blackp = sp->ipt;
			break;
		}
	}

	pdev = pci_find_device(PCI_VENDOR_ID_SUN, PCI_DEVICE_ID_SUN_EBUS, 0);
	if (!pdev) {
		return;
	}
	cookie = pdev->sysdata;
	ebusnd = cookie->prom_node;

	ebus_chain = ebus = (struct linux_ebus *)
			ebus_alloc(sizeof(struct linux_ebus));
	ebus->next = 0;

	while (ebusnd) {

		prom_getstring(ebusnd, "name", lbuf, sizeof(lbuf));
		ebus->prom_node = ebusnd;
		strcpy(ebus->prom_name, lbuf);
		ebus->self = pdev;
		ebus->parent = pbm = cookie->pbm;

		/* Enable BUS Master. */
		pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
		pci_command |= PCI_COMMAND_MASTER;
		pci_write_config_word(pdev, PCI_COMMAND, pci_command);

		len = prom_getproperty(ebusnd, "reg", (void *)regs,
				       sizeof(regs));
		if (len == 0 || len == -1) {
			prom_printf("%s: can't find reg property\n",
				    __FUNCTION__);
			prom_halt();
		}
		nreg = len / sizeof(struct linux_prom_pci_registers);

		base = &ebus->self->resource[0].start;
		for (reg = 0; reg < nreg; reg++) {
			if (!(regs[reg].which_io & 0x03000000))
				continue;

			addr = regs[reg].phys_lo;
			*base++ = addr;
		}

		nd = prom_getchild(ebusnd);
		if (!nd)
			goto next_ebus;

		ebus->devices = (struct linux_ebus_device *)
				ebus_alloc(sizeof(struct linux_ebus_device));

		dev = ebus->devices;
		dev->next = 0;
		dev->children = 0;
		dev->bus = ebus;
		fill_ebus_device(nd, dev);

		while ((nd = prom_getsibling(nd))) {
			dev->next = (struct linux_ebus_device *)
				ebus_alloc(sizeof(struct linux_ebus_device));

			dev = dev->next;
			dev->next = 0;
			dev->children = 0;
			dev->bus = ebus;
			fill_ebus_device(nd, dev);
		}

	next_ebus:
		pdev = pci_find_device(PCI_VENDOR_ID_SUN,
				       PCI_DEVICE_ID_SUN_EBUS, pdev);
		if (!pdev)
			break;

		cookie = pdev->sysdata;
		ebusnd = cookie->prom_node;

		ebus->next = (struct linux_ebus *)
			ebus_alloc(sizeof(struct linux_ebus));
		ebus = ebus->next;
		ebus->next = 0;
		++num_ebus;
	}

#ifdef CONFIG_SUN_AUXIO
	auxio_probe();
#endif
}
