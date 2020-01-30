/*
 *  proteon.c: A network driver for Proteon ISA token ring cards.
 *
 *  Based on tmspci written 1999 by Adam Fritzler
 *  
 *  Written 2003 by Jochen Friedrich
 *
 *  This software may be used and distributed according to the terms
 *  of the GNU General Public License, incorporated herein by reference.
 *
 *  This driver module supports the following cards:
 *	- Proteon 1392, 1392+
 *
 *  Maintainer(s):
 *    AF        Adam Fritzler           mid@auk.cx
 *    JF	Jochen Friedrich	jochen@scram.de
 *
 *  Modification History:
 *	02-Jan-03	JF	Created
 *
 */
static const char version[] = "proteon.c: v1.00 02/01/2003 by Jochen Friedrich\n";

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/trdevice.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/pci.h>
#include <asm/dma.h>

#include "tms380tr.h"

#define PROTEON_IO_EXTENT 32

/* A zero-terminated list of I/O addresses to be probed. */
static unsigned int portlist[] __initdata = {
	0x0A20, 0x0E20, 0x1A20, 0x1E20, 0x2A20, 0x2E20, 0x3A20, 0x3E20,// Prot.
	0x4A20, 0x4E20, 0x5A20, 0x5E20, 0x6A20, 0x6E20, 0x7A20, 0x7E20,// Prot.
	0x8A20, 0x8E20, 0x9A20, 0x9E20, 0xAA20, 0xAE20, 0xBA20, 0xBE20,// Prot.
	0xCA20, 0xCE20, 0xDA20, 0xDE20, 0xEA20, 0xEE20, 0xFA20, 0xFE20,// Prot.
	0
};

/* A zero-terminated list of IRQs to be probed. */
static unsigned short irqlist[] = {
	7, 6, 5, 4, 3, 12, 11, 10, 9,
	0
};

/* A zero-terminated list of DMAs to be probed. */
static int dmalist[] __initdata = {
	5, 6, 7,
	0
};

static char cardname[] = "Proteon 1392\0";

struct net_device *proteon_probe(int unit);
static int proteon_open(struct net_device *dev);
static void proteon_read_eeprom(struct net_device *dev);
static unsigned short proteon_setnselout_pins(struct net_device *dev);

static unsigned short proteon_sifreadb(struct net_device *dev, unsigned short reg)
{
	return inb(dev->base_addr + reg);
}

static unsigned short proteon_sifreadw(struct net_device *dev, unsigned short reg)
{
	return inw(dev->base_addr + reg);
}

static void proteon_sifwriteb(struct net_device *dev, unsigned short val, unsigned short reg)
{
	outb(val, dev->base_addr + reg);
}

static void proteon_sifwritew(struct net_device *dev, unsigned short val, unsigned short reg)
{
	outw(val, dev->base_addr + reg);
}

static int __init proteon_probe1(struct net_device *dev, int ioaddr)
{
	unsigned char chk1, chk2;
	int i;

	if (!request_region(ioaddr, PROTEON_IO_EXTENT, cardname))
		return -ENODEV;
		

	chk1 = inb(ioaddr + 0x1f);      /* Get Proteon ID reg 1 */
	if (chk1 != 0x1f) 
		goto nodev;

	chk1 = inb(ioaddr + 0x1e) & 0x07;       /* Get Proteon ID reg 0 */
	for (i=0; i<16; i++) {
		chk2 = inb(ioaddr + 0x1e) & 0x07;
		if (((chk1 + 1) & 0x07) != chk2)
			goto nodev;
		chk1 = chk2;
	}

	dev->base_addr = ioaddr;
	return (0);
nodev:
	release_region(ioaddr, PROTEON_IO_EXTENT); 
	return -ENODEV;
}

static int __init setup_card(struct net_device *dev)
{
	struct net_local *tp;
        static int versionprinted;
	const unsigned *port;
	int j,err = 0;

	if (!dev)
		return -ENOMEM;

	SET_MODULE_OWNER(dev);
	if (dev->base_addr)	/* probe specific location */
		err = proteon_probe1(dev, dev->base_addr);
	else {
		for (port = portlist; *port; port++) {
			err = proteon_probe1(dev, *port);
			if (!err)
				break;
		}
	}
	if (err)
		goto out4;

	/* At this point we have found a valid card. */

	if (versionprinted++ == 0)
		printk(KERN_DEBUG "%s", version);

	err = -EIO;
	if (tmsdev_init(dev, ISA_MAX_ADDRESS, NULL))
		goto out4;

	dev->base_addr &= ~3; 
		
	proteon_read_eeprom(dev);

	printk(KERN_DEBUG "%s:    Ring Station Address: ", dev->name);
	printk("%2.2x", dev->dev_addr[0]);
	for (j = 1; j < 6; j++)
		printk(":%2.2x", dev->dev_addr[j]);
	printk("\n");
		
	tp = netdev_priv(dev);
	tp->setnselout = proteon_setnselout_pins;
		
	tp->sifreadb = proteon_sifreadb;
	tp->sifreadw = proteon_sifreadw;
	tp->sifwriteb = proteon_sifwriteb;
	tp->sifwritew = proteon_sifwritew;
	
	memcpy(tp->ProductID, cardname, PROD_ID_SIZE + 1);

	tp->tmspriv = NULL;

	dev->open = proteon_open;
	dev->stop = tms380tr_close;

	if (dev->irq == 0)
	{
		for(j = 0; irqlist[j] != 0; j++)
		{
			dev->irq = irqlist[j];
			if (!request_irq(dev->irq, tms380tr_interrupt, 0, 
				cardname, dev))
				break;
                }
		
                if(irqlist[j] == 0)
                {
                        printk(KERN_INFO "%s: AutoSelect no IRQ available\n", dev->name);
			goto out3;
		}
	}
	else
	{
		for(j = 0; irqlist[j] != 0; j++)
			if (irqlist[j] == dev->irq)
				break;
		if (irqlist[j] == 0)
		{
			printk(KERN_INFO "%s: Illegal IRQ %d specified\n",
				dev->name, dev->irq);
			goto out3;
		}
		if (request_irq(dev->irq, tms380tr_interrupt, 0, 
			cardname, dev))
		{
                        printk(KERN_INFO "%s: Selected IRQ %d not available\n", 
				dev->name, dev->irq);
			goto out3;
		}
	}

	if (dev->dma == 0)
	{
		for(j = 0; dmalist[j] != 0; j++)
		{
			dev->dma = dmalist[j];
                        if (!request_dma(dev->dma, cardname))
				break;
		}

		if(dmalist[j] == 0)
		{
			printk(KERN_INFO "%s: AutoSelect no DMA available\n", dev->name);
			goto out2;
		}
	}
	else
	{
		for(j = 0; dmalist[j] != 0; j++)
			if (dmalist[j] == dev->dma)
				break;
		if (dmalist[j] == 0)
		{
                        printk(KERN_INFO "%s: Illegal DMA %d specified\n", 
				dev->name, dev->dma);
			goto out2;
		}
		if (request_dma(dev->dma, cardname))
		{
                        printk(KERN_INFO "%s: Selected DMA %d not available\n", 
				dev->name, dev->dma);
			goto out2;
		}
	}

	printk(KERN_DEBUG "%s:    IO: %#4lx  IRQ: %d  DMA: %d\n",
	       dev->name, dev->base_addr, dev->irq, dev->dma);
		
	err = register_netdev(dev);
	if (err)
		goto out;

	return 0;
out:
	free_dma(dev->dma);
out2:
	free_irq(dev->irq, dev);
out3:
	tmsdev_term(dev);
out4:
	release_region(dev->base_addr, PROTEON_IO_EXTENT); 
	return err;
}

struct net_device * __init proteon_probe(int unit)
{
	struct net_device *dev = alloc_trdev(sizeof(struct net_local));
	int err = 0;

	if (!dev)
		return ERR_PTR(-ENOMEM);

	if (unit >= 0) {
		sprintf(dev->name, "tr%d", unit);
		netdev_boot_setup_check(dev);
	}

	err = setup_card(dev);
	if (err)
		goto out;

	return dev;

out:
	free_netdev(dev);
	return ERR_PTR(err);
}

/*
 * Reads MAC address from adapter RAM, which should've read it from
 * the onboard ROM.  
 *
 * Calling this on a board that does not support it can be a very
 * dangerous thing.  The Madge board, for instance, will lock your
 * machine hard when this is called.  Luckily, its supported in a
 * separate driver.  --ASF
 */
static void proteon_read_eeprom(struct net_device *dev)
{
	int i;
	
	/* Address: 0000:0000 */
	proteon_sifwritew(dev, 0, SIFADX);
	proteon_sifwritew(dev, 0, SIFADR);	
	
	/* Read six byte MAC address data */
	dev->addr_len = 6;
	for(i = 0; i < 6; i++)
		dev->dev_addr[i] = proteon_sifreadw(dev, SIFINC) >> 8;
}

unsigned short proteon_setnselout_pins(struct net_device *dev)
{
	return 0;
}

static int proteon_open(struct net_device *dev)
{  
	struct net_local *tp = netdev_priv(dev);
	unsigned short val = 0;
	int i;

	/* Proteon reset sequence */
	outb(0, dev->base_addr + 0x11);
	mdelay(20);
	outb(0x04, dev->base_addr + 0x11);
	mdelay(20);
	outb(0, dev->base_addr + 0x11);
	mdelay(100);

	/* set control/status reg */
	val = inb(dev->base_addr + 0x11);
	val |= 0x78;
	val &= 0xf9;
	if(tp->DataRate == SPEED_4)
		val |= 0x20;
	else
		val &= ~0x20;

	outb(val, dev->base_addr + 0x11);
	outb(0xff, dev->base_addr + 0x12);
	for(i = 0; irqlist[i] != 0; i++)
	{
		if(irqlist[i] == dev->irq)
			break;
	}
	val = i;
	i = (7 - dev->dma) << 4;
	val |= i;
	outb(val, dev->base_addr + 0x13);

	return tms380tr_open(dev);
}

#ifdef MODULE

#define ISATR_MAX_ADAPTERS 3

static int io[ISATR_MAX_ADAPTERS];
static int irq[ISATR_MAX_ADAPTERS];
static int dma[ISATR_MAX_ADAPTERS];

MODULE_LICENSE("GPL");

MODULE_PARM(io, "1-" __MODULE_STRING(ISATR_MAX_ADAPTERS) "i");
MODULE_PARM(irq, "1-" __MODULE_STRING(ISATR_MAX_ADAPTERS) "i");
MODULE_PARM(dma, "1-" __MODULE_STRING(ISATR_MAX_ADAPTERS) "i");

static struct net_device *proteon_dev[ISATR_MAX_ADAPTERS];

int init_module(void)
{
	struct net_device *dev;
	int i, num = 0, err = 0;

	for (i = 0; i < ISATR_MAX_ADAPTERS ; i++) {
		dev = alloc_trdev(sizeof(struct net_local));
		if (!dev)
			continue;

		dev->base_addr = io[i];
		dev->irq = irq[i];
		dev->dma = dma[i];
		err = setup_card(dev);
		if (!err) {
			proteon_dev[i] = dev;
			++num;
		} else {
			free_netdev(dev);
		}
	}

	printk(KERN_NOTICE "proteon.c: %d cards found.\n", num);
	/* Probe for cards. */
	if (num == 0) {
		printk(KERN_NOTICE "proteon.c: No cards found.\n");
		return (-ENODEV);
	}
	return (0);
}

void cleanup_module(void)
{
	int i;

	for (i = 0; i < ISATR_MAX_ADAPTERS ; i++) {
		struct net_device *dev = proteon_dev[i];
		
		if (!dev) 
			continue;
		
		unregister_netdev(dev);
		release_region(dev->base_addr, PROTEON_IO_EXTENT);
		free_irq(dev->irq, dev);
		free_dma(dev->dma);
		tmsdev_term(dev);
		free_netdev(dev);
	}
}
#endif /* MODULE */


/*
 * Local variables:
 *  compile-command: "gcc -DMODVERSIONS  -DMODULE -D__KERNEL__ -Wall -Wstrict-prototypes -O6 -fomit-frame-pointer -I/usr/src/linux/drivers/net/tokenring/ -c proteon.c"
 *  alt-compile-command: "gcc -DMODULE -D__KERNEL__ -Wall -Wstrict-prototypes -O6 -fomit-frame-pointer -I/usr/src/linux/drivers/net/tokenring/ -c proteon.c"
 *  c-set-style "K&R"
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
