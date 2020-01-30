/* orinoco_pci.c 0.13e
 * 
 * Driver for Prism II devices that have a direct PCI interface
 * (i.e., not in a Pcmcia or PLX bridge)
 *
 * Specifically here we're talking about the Linksys WMP11
 *
 * Some of this code is borrowed from orinoco_plx.c
 *	Copyright (C) 2001 Daniel Barlow <dan@telent.net>
 * Some of this code is "inspired" by linux-wlan-ng-0.1.10, but nothing
 * has been copied from it. linux-wlan-ng-0.1.10 is originally :
 *	Copyright (C) 1999 AbsoluteValue Systems, Inc.  All Rights Reserved.
 * This file originally written by:
 *	Copyright (C) 2001 Jean Tourrilhes <jt@hpl.hp.com>
 * And is now maintained by:
 *	Copyright (C) 2002 David Gibson, IBM Corporation <herme@gibson.dropbear.id.au>
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License
 * at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and
 * limitations under the License.
 *
 * Alternatively, the contents of this file may be used under the
 * terms of the GNU General Public License version 2 (the "GPL"), in
 * which case the provisions of the GPL are applicable instead of the
 * above.  If you wish to allow the use of your version of this file
 * only under the terms of the GPL and not to allow others to use your
 * version of this file under the MPL, indicate your decision by
 * deleting the provisions above and replace them with the notice and
 * other provisions required by the GPL.  If you do not delete the
 * provisions above, a recipient may use your version of this file
 * under either the MPL or the GPL.
 */

/*
 * Theory of operation...
 * -------------------
 * Maybe you had a look in orinoco_plx. Well, this is totally different...
 *
 * The card contains only one PCI region, which contains all the usual
 * hermes registers.
 *
 * The driver will memory map this region in normal memory. Because
 * the hermes registers are mapped in normal memory and not in ISA I/O
 * post space, we can't use the usual inw/outw macros and we need to
 * use readw/writew.
 * This slight difference force us to compile our own version of
 * hermes.c with the register access macro changed. That's a bit
 * hackish but works fine.
 *
 * Note that the PCI region is pretty big (4K). That's much more than
 * the usual set of hermes register (0x0 -> 0x3E). I've got a strong
 * suspicion that the whole memory space of the adapter is in fact in
 * this region. Accessing directly the adapter memory instead of going
 * through the usual register would speed up significantely the
 * operations...
 *
 * Finally, the card looks like this :
-----------------------
  Bus  0, device  14, function  0:
    Network controller: PCI device 1260:3873 (Harris Semiconductor) (rev 1).
      IRQ 11.
      Master Capable.  Latency=248.  
      Prefetchable 32 bit memory at 0xffbcc000 [0xffbccfff].
-----------------------
00:0e.0 Network controller: Harris Semiconductor: Unknown device 3873 (rev 01)
        Subsystem: Unknown device 1737:3874
        Control: I/O+ Mem+ BusMaster+ SpecCycle- MemWINV- VGASnoop- ParErr- Stepping- SERR- FastB2B-
        Status: Cap+ 66Mhz- UDF- FastB2B+ ParErr- DEVSEL=medium >TAbort- <TAbort- <MAbort- >SERR- <PERR-
        Latency: 248 set, cache line size 08
        Interrupt: pin A routed to IRQ 11
        Region 0: Memory at ffbcc000 (32-bit, prefetchable) [size=4K]
        Capabilities: [dc] Power Management version 2
                Flags: PMEClk- AuxPwr- DSI- D1+ D2+ PME+
                Status: D0 PME-Enable- DSel=0 DScale=0 PME-
-----------------------
 *
 * That's all..
 *
 * Jean II
 */

#include <linux/config.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/ioport.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/wireless.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/fcntl.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/system.h>

#include "hermes.h"
#include "orinoco.h"

/* All the magic there is from wlan-ng */
/* Magic offset of the reset register of the PCI card */
#define HERMES_PCI_COR		(0x26)
/* Magic bitmask to reset the card */
#define HERMES_PCI_COR_MASK	(0x0080)
/* Magic timeouts for doing the reset.
 * Those times are straight from wlan-ng, and it is claimed that they
 * are necessary. Alan will kill me. Take your time and grab a coffee. */
#define HERMES_PCI_COR_ONT	(250)		/* ms */
#define HERMES_PCI_COR_OFFT	(500)		/* ms */
#define HERMES_PCI_COR_BUSYT	(500)		/* ms */

/*
 * Do a soft reset of the PCI card using the Configuration Option Register
 * We need this to get going...
 * This is the part of the code that is strongly inspired from wlan-ng
 *
 * Note : This code is done with irq enabled. This mean that many
 * interrupts will occur while we are there. This is why we use the
 * jiffies to regulate time instead of a straight mdelay(). Usually we
 * need only around 245 iteration of the loop to do 250 ms delay.
 *
 * Note bis : Don't try to access HERMES_CMD during the reset phase.
 * It just won't work !
 */
static int
orinoco_pci_cor_reset(struct orinoco_private *priv)
{
	hermes_t *hw = &priv->hw;
	unsigned long	timeout;
	u16	reg;

	/* Assert the reset until the card notice */
	hermes_write_regn(hw, PCI_COR, HERMES_PCI_COR_MASK);
	printk(KERN_NOTICE "Reset done");
	timeout = jiffies + (HERMES_PCI_COR_ONT * HZ / 1000);
	while(time_before(jiffies, timeout)) {
		printk(".");
		mdelay(1);
	}
	printk(";\n");
	//mdelay(HERMES_PCI_COR_ONT);

	/* Give time for the card to recover from this hard effort */
	hermes_write_regn(hw, PCI_COR, 0x0000);
	printk(KERN_NOTICE "Clear Reset");
	timeout = jiffies + (HERMES_PCI_COR_OFFT * HZ / 1000);
	while(time_before(jiffies, timeout)) {
		printk(".");
		mdelay(1);
	}
	printk(";\n");
	//mdelay(HERMES_PCI_COR_OFFT);

	/* The card is ready when it's no longer busy */
	timeout = jiffies + (HERMES_PCI_COR_BUSYT * HZ / 1000);
	reg = hermes_read_regn(hw, CMD);
	while (time_before(jiffies, timeout) && (reg & HERMES_CMD_BUSY)) {
		mdelay(1);
		reg = hermes_read_regn(hw, CMD);
	}
	/* Did we timeout ? */
	if(time_after_eq(jiffies, timeout)) {
		printk(KERN_ERR "orinoco_pci: Busy timeout\n");
		return -ETIMEDOUT;
	}
	printk(KERN_NOTICE "pci_cor : reg = 0x%X - %lX - %lX\n", reg, timeout, jiffies);

	return 0;
}

/*
 * Initialise a card. Mostly similar to PLX code.
 */
static int orinoco_pci_init_one(struct pci_dev *pdev,
				const struct pci_device_id *ent)
{
	int err = 0;
	unsigned long pci_iorange;
	u16 *pci_ioaddr = NULL;
	unsigned long pci_iolen;
	struct orinoco_private *priv = NULL;
	struct net_device *dev = NULL;

	err = pci_enable_device(pdev);
	if (err)
		return -EIO;

	/* Resource 0 is mapped to the hermes registers */
	pci_iorange = pci_resource_start(pdev, 0);
	pci_iolen = pci_resource_len(pdev, 0);
	pci_ioaddr = ioremap(pci_iorange, pci_iolen);
	if (! pci_iorange)
		goto fail;

	/* Usual setup of structures */
	dev = alloc_orinocodev(0, NULL);
	if (! dev) {
		err = -ENOMEM;
		goto fail;
	}
	priv = dev->priv;

	dev->base_addr = (unsigned long) pci_ioaddr;
	dev->mem_start = pci_iorange;
	dev->mem_end = pci_iorange + pci_iolen - 1;

	SET_MODULE_OWNER(dev);
	SET_NETDEV_DEV(dev, &pdev->dev);

	printk(KERN_DEBUG
	       "Detected Orinoco/Prism2 PCI device at %s, mem:0x%lX to 0x%lX -> 0x%p, irq:%d\n",
	       pci_name(pdev), dev->mem_start, dev->mem_end, pci_ioaddr, pdev->irq);

	hermes_struct_init(&priv->hw, dev->base_addr,
			   HERMES_MEM, HERMES_32BIT_REGSPACING);
	pci_set_drvdata(pdev, dev);

	err = request_irq(pdev->irq, orinoco_interrupt, SA_SHIRQ,
			  dev->name, dev);
	if (err) {
		printk(KERN_ERR "orinoco_pci: Error allocating IRQ %d.\n",
		       pdev->irq);
		err = -EBUSY;
		goto fail;
	}
	dev->irq = pdev->irq;
	/* Perform a COR reset to start the card */
	if(orinoco_pci_cor_reset(priv) != 0) {
		printk(KERN_ERR "%s: Failed to start the card\n", dev->name);
		err = -ETIMEDOUT;
		goto fail;
	}

	/* Override the normal firmware detection - the Prism 2.5 PCI
	 * cards look like Lucent firmware but are actually Intersil */
	priv->firmware_type = FIRMWARE_TYPE_INTERSIL;

	err = register_netdev(dev);
	if (err) {
		printk(KERN_ERR "%s: Failed to register net device\n", dev->name);
		goto fail;
	}

        return 0;               /* succeeded */
 fail:
	if (dev) {
		if (dev->irq)
			free_irq(dev->irq, dev);

		free_netdev(dev);
	}

	if (pci_ioaddr)
		iounmap(pci_ioaddr);

	pci_disable_device(pdev);

	return err;
}

static void __devexit orinoco_pci_remove_one(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct orinoco_private *priv = dev->priv;

	unregister_netdev(dev);

        if (dev->irq)
		free_irq(dev->irq, dev);

	if (priv->hw.iobase)
		iounmap((unsigned char *) priv->hw.iobase);

	pci_set_drvdata(pdev, NULL);
	free_netdev(dev);

	pci_disable_device(pdev);
}

static int orinoco_pci_suspend(struct pci_dev *pdev, u32 state)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct orinoco_private *priv = dev->priv;
	unsigned long flags;
	int err;
	
	printk(KERN_DEBUG "%s: Orinoco-PCI entering sleep mode (state=%d)\n",
	       dev->name, state);

	err = orinoco_lock(priv, &flags);
	if (err) {
		printk(KERN_ERR "%s: hw_unavailable on orinoco_pci_suspend\n",
		       dev->name);
		return err;
	}

	err = __orinoco_down(dev);
	if (err)
		printk(KERN_WARNING "%s: orinoco_pci_suspend(): Error %d downing interface\n",
		       dev->name, err);
	
	netif_device_detach(dev);

	priv->hw_unavailable++;
	
	orinoco_unlock(priv, &flags);

	return 0;
}

static int orinoco_pci_resume(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct orinoco_private *priv = dev->priv;
	unsigned long flags;
	int err;

	printk(KERN_DEBUG "%s: Orinoco-PCI waking up\n", dev->name);

	err = orinoco_reinit_firmware(dev);
	if (err) {
		printk(KERN_ERR "%s: Error %d re-initializing firmware on orinoco_pci_resume()\n",
		       dev->name, err);
		return err;
	}

	spin_lock_irqsave(&priv->lock, flags);

	netif_device_attach(dev);

	priv->hw_unavailable--;

	if (priv->open && (! priv->hw_unavailable)) {
		err = __orinoco_up(dev);
		if (err)
			printk(KERN_ERR "%s: Error %d restarting card on orinoco_pci_resume()\n",
			       dev->name, err);
	}
	
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static struct pci_device_id orinoco_pci_pci_id_table[] = {
	{0x1260, 0x3872, PCI_ANY_ID, PCI_ANY_ID,},
	{0x1260, 0x3873, PCI_ANY_ID, PCI_ANY_ID,},
	{0,},
};

MODULE_DEVICE_TABLE(pci, orinoco_pci_pci_id_table);

static struct pci_driver orinoco_pci_driver = {
	.name		= "orinoco_pci",
	.id_table	= orinoco_pci_pci_id_table,
	.probe		= orinoco_pci_init_one,
	.remove		= __devexit_p(orinoco_pci_remove_one),
	.suspend	= orinoco_pci_suspend,
	.resume		= orinoco_pci_resume,
};

static char version[] __initdata = "orinoco_pci.c 0.13e (David Gibson <hermes@gibson.dropbear.id.au> & Jean Tourrilhes <jt@hpl.hp.com>)";
MODULE_AUTHOR("David Gibson <hermes@gibson.dropbear.id.au>");
MODULE_DESCRIPTION("Driver for wireless LAN cards using direct PCI interface");
MODULE_LICENSE("Dual MPL/GPL");

static int __init orinoco_pci_init(void)
{
	printk(KERN_DEBUG "%s\n", version);
	return pci_module_init(&orinoco_pci_driver);
}

void __exit orinoco_pci_exit(void)
{
	pci_unregister_driver(&orinoco_pci_driver);
}

module_init(orinoco_pci_init);
module_exit(orinoco_pci_exit);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
