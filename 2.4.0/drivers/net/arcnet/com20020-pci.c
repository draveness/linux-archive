/*
 * Linux ARCnet driver - COM20020 PCI support (Contemporary Controls PCI20)
 * 
 * Written 1994-1999 by Avery Pennarun,
 *    based on an ISA version by David Woodhouse.
 * Written 1999-2000 by Martin Mares <mj@suse.cz>.
 * Derived from skeleton.c by Donald Becker.
 *
 * Special thanks to Contemporary Controls, Inc. (www.ccontrols.com)
 *  for sponsoring the further development of this driver.
 *
 * **********************
 *
 * The original copyright of skeleton.c was as follows:
 *
 * skeleton.c Written 1993 by Donald Becker.
 * Copyright 1993 United States Government as represented by the
 * Director, National Security Agency.  This software may only be used
 * and distributed according to the terms of the GNU Public License as
 * modified by SRC, incorporated herein by reference.
 *
 * **********************
 *
 * For more details, see drivers/net/arcnet.c
 *
 * **********************
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/ioport.h>
#include <linux/malloc.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/arcdevice.h>
#include <linux/com20020.h>

#include <asm/io.h>


#define VERSION "arcnet: COM20020 PCI support\n"

/* Module parameters */

static int node = 0;
static char *device;		/* use eg. device="arc1" to change name */
static int timeout = 3;
static int backplane = 0;
static int clockp = 0;
static int clockm = 0;

MODULE_PARM(node, "i");
MODULE_PARM(device, "s");
MODULE_PARM(timeout, "i");
MODULE_PARM(backplane, "i");
MODULE_PARM(clockp, "i");
MODULE_PARM(clockm, "i");

static void com20020pci_open_close(struct net_device *dev, bool open)
{
	if (open)
		MOD_INC_USE_COUNT;
	else
		MOD_DEC_USE_COUNT;
}

static int __devinit com20020pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct net_device *dev;
	struct arcnet_local *lp;
	int ioaddr, err;

	if (pci_enable_device(pdev))
		return -EIO;
	dev = dev_alloc(device ? : "arc%d", &err);
	if (!dev)
		return err;
	lp = dev->priv = kmalloc(sizeof(struct arcnet_local), GFP_KERNEL);
	if (!lp)
		return -ENOMEM;
	memset(lp, 0, sizeof(struct arcnet_local));
	pdev->driver_data = dev;

	ioaddr = pci_resource_start(pdev, 2);
	dev->base_addr = ioaddr;
	dev->irq = pdev->irq;
	dev->dev_addr[0] = node;
	lp->card_name = pdev->name;
	lp->card_flags = id->driver_data;
	lp->backplane = backplane;
	lp->clockp = clockp & 7;
	lp->clockm = clockm & 3;
	lp->timeout = timeout;
	lp->hw.open_close_ll = com20020pci_open_close;

	if (check_region(ioaddr, ARCNET_TOTAL_SIZE)) {
		BUGMSG(D_INIT, "IO region %xh-%xh already allocated.\n",
		       ioaddr, ioaddr + ARCNET_TOTAL_SIZE - 1);
		return -EBUSY;
	}
	if (ASTATUS() == 0xFF) {
		BUGMSG(D_NORMAL, "IO address %Xh was reported by PCI BIOS, "
		       "but seems empty!\n", ioaddr);
		return -EIO;
	}
	if (com20020_check(dev))
		return -EIO;

	return com20020_found(dev, SA_SHIRQ);
}

static void __devexit com20020pci_remove(struct pci_dev *pdev)
{
	com20020_remove(pdev->driver_data);
}

static struct pci_device_id com20020pci_id_table[] __devinitdata = {
	{ 0x1571, 0xa001, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0x1571, 0xa002, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0x1571, 0xa003, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0x1571, 0xa004, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0x1571, 0xa005, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0x1571, 0xa006, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0x1571, 0xa007, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0x1571, 0xa008, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0x1571, 0xa009, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_IS_5MBIT },
	{ 0x1571, 0xa00a, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_IS_5MBIT },
	{ 0x1571, 0xa00b, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_IS_5MBIT },
	{ 0x1571, 0xa00c, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_IS_5MBIT },
	{ 0x1571, 0xa00d, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_IS_5MBIT },
	{ 0x1571, 0xa201, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_CAN_10MBIT },
	{ 0x1571, 0xa202, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_CAN_10MBIT },
	{ 0x1571, 0xa203, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_CAN_10MBIT },
	{ 0x1571, 0xa204, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_CAN_10MBIT },
	{ 0x1571, 0xa205, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_CAN_10MBIT },
	{ 0x1571, 0xa206, PCI_ANY_ID, PCI_ANY_ID, 0, 0, ARC_CAN_10MBIT },
	{0,}
};

MODULE_DEVICE_TABLE(pci, com20020pci_id_table);

static struct pci_driver com20020pci_driver = {
	name:		"com20020",
	id_table:	com20020pci_id_table,
	probe:		com20020pci_probe,
	remove:		com20020pci_remove
};

static int __init com20020pci_init(void)
{
	BUGLVL(D_NORMAL) printk(VERSION);
#ifndef MODULE
	arcnet_init();
#endif
	return pci_module_init(&com20020pci_driver);
}

static void __exit com20020pci_cleanup(void)
{
	pci_unregister_driver(&com20020pci_driver);
}

module_init(com20020pci_init)
module_exit(com20020pci_cleanup)
