/*
	lne390.c

	Linux driver for Mylex LNE390 EISA Network Adapter

	Copyright (C) 1996-1998, Paul Gortmaker.

	This software may be used and distributed according to the terms
	of the GNU Public License, incorporated herein by reference.

	Information and Code Sources:

	1) Based upon framework of es3210 driver.
	2) The existing myriad of other Linux 8390 drivers by Donald Becker.
	3) Russ Nelson's asm packet driver provided additional info.
	4) Info for getting IRQ and sh-mem gleaned from the EISA cfg files.

	The LNE390 is an EISA shared memory NS8390 implementation. Note
	that all memory copies to/from the board must be 32bit transfers.
	There are two versions of the card: the lne390a and the lne390b.
	Going by the EISA cfg files, the "a" has jumpers to select between
	BNC/AUI, but the "b" also has RJ-45 and selection is via the SCU.
	The shared memory address selection is also slightly different.
	Note that shared memory address > 1MB are supported with this driver.

	You can try <http://www.mylex.com> if you want more info, as I've
	never even seen one of these cards.  :)

	Arnaldo Carvalho de Melo <acme@conectiva.com.br> - 2000/09/01
	- get rid of check_region
	- no need to check if dev == NULL in lne390_probe1
*/

static const char *version =
	"lne390.c: Driver revision v0.99.1, 01/09/2000\n";

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/system.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include "8390.h"

int lne390_probe(struct net_device *dev);
static int lne390_probe1(struct net_device *dev, int ioaddr);

static int lne390_open(struct net_device *dev);
static int lne390_close(struct net_device *dev);

static void lne390_reset_8390(struct net_device *dev);

static void lne390_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr, int ring_page);
static void lne390_block_input(struct net_device *dev, int count, struct sk_buff *skb, int ring_offset);
static void lne390_block_output(struct net_device *dev, int count, const unsigned char *buf, const int start_page);

#define LNE390_START_PG		0x00    /* First page of TX buffer	*/
#define LNE390_STOP_PG		0x80    /* Last page +1 of RX ring	*/

#define LNE390_ID_PORT		0xc80	/* Same for all EISA cards 	*/
#define LNE390_IO_EXTENT	0x20
#define LNE390_SA_PROM		0x16	/* Start of e'net addr.		*/
#define LNE390_RESET_PORT	0xc84	/* From the pkt driver source	*/
#define LNE390_NIC_OFFSET	0x00	/* Hello, the 8390 is *here*	*/

#define LNE390_ADDR0		0x00	/* 3 byte vendor prefix		*/
#define LNE390_ADDR1		0x80
#define LNE390_ADDR2		0xe5

#define LNE390_ID0	0x10009835	/* 0x3598 = 01101 01100 11000 = mlx */
#define LNE390_ID1	0x11009835	/* above is the 390A, this is 390B  */

#define LNE390_CFG1		0xc84	/* NB: 0xc84 is also "reset" port. */
#define LNE390_CFG2		0xc90

/*
 *	You can OR any of the following bits together and assign it
 *	to LNE390_DEBUG to get verbose driver info during operation.
 *	Currently only the probe one is implemented.
 */

#define LNE390_D_PROBE	0x01
#define LNE390_D_RX_PKT	0x02
#define LNE390_D_TX_PKT	0x04
#define LNE390_D_IRQ	0x08

#define LNE390_DEBUG	0

static unsigned char irq_map[] __initdata = {15, 12, 11, 10, 9, 7, 5, 3};
static unsigned int shmem_mapA[] __initdata = {0xff, 0xfe, 0xfd, 0xfff, 0xffe, 0xffc, 0x0d, 0x0};
static unsigned int shmem_mapB[] __initdata = {0xff, 0xfe, 0x0e, 0xfff, 0xffe, 0xffc, 0x0d, 0x0};

/*
 *	Probe for the card. The best way is to read the EISA ID if it
 *	is known. Then we can check the prefix of the station address
 *	PROM for a match against the value assigned to Mylex.
 */

int __init lne390_probe(struct net_device *dev)
{
	unsigned short ioaddr = dev->base_addr;
	int ret;

	SET_MODULE_OWNER(dev);

	if (ioaddr > 0x1ff) {		/* Check a single specified location. */
		if (!request_region(ioaddr, LNE390_IO_EXTENT, dev->name))
			return -EBUSY;
		ret = lne390_probe1(dev, ioaddr);
		if (ret)
			release_region(ioaddr, LNE390_IO_EXTENT);
		return ret;
	}
	else if (ioaddr > 0)		/* Don't probe at all. */
		return -ENXIO;

	if (!EISA_bus) {
#if LNE390_DEBUG & LNE390_D_PROBE
		printk("lne390-debug: Not an EISA bus. Not probing high ports.\n");
#endif
		return -ENXIO;
	}

	/* EISA spec allows for up to 16 slots, but 8 is typical. */
	for (ioaddr = 0x1000; ioaddr < 0x9000; ioaddr += 0x1000) {
		if (!request_region(ioaddr, LNE390_IO_EXTENT, dev->name))
			continue;
		if (lne390_probe1(dev, ioaddr) == 0)
			return 0;
		release_region(ioaddr, LNE390_IO_EXTENT);
	}

	return -ENODEV;
}

static int __init lne390_probe1(struct net_device *dev, int ioaddr)
{
	int i, revision, ret;
	unsigned long eisa_id;

	if (inb_p(ioaddr + LNE390_ID_PORT) == 0xff) return -ENODEV;

#if LNE390_DEBUG & LNE390_D_PROBE
	printk("lne390-debug: probe at %#x, ID %#8x\n", ioaddr, inl(ioaddr + LNE390_ID_PORT));
	printk("lne390-debug: config regs: %#x %#x\n",
		inb(ioaddr + LNE390_CFG1), inb(ioaddr + LNE390_CFG2));
#endif


/*	Check the EISA ID of the card. */
	eisa_id = inl(ioaddr + LNE390_ID_PORT);
	if ((eisa_id != LNE390_ID0) && (eisa_id != LNE390_ID1)) {
		return -ENODEV;
	}

	revision = (eisa_id >> 24) & 0x01;	/* 0 = rev A, 1 rev B */
	
#if 0
/*	Check the Mylex vendor ID as well. Not really required. */
	if (inb(ioaddr + LNE390_SA_PROM + 0) != LNE390_ADDR0
		|| inb(ioaddr + LNE390_SA_PROM + 1) != LNE390_ADDR1
		|| inb(ioaddr + LNE390_SA_PROM + 2) != LNE390_ADDR2 ) {
		printk("lne390.c: card not found");
		for(i = 0; i < ETHER_ADDR_LEN; i++)
			printk(" %02x", inb(ioaddr + LNE390_SA_PROM + i));
		printk(" (invalid prefix).\n");
		return -ENODEV;
	}
#endif
	/* Allocate dev->priv and fill in 8390 specific dev fields. */
	if (ethdev_init(dev)) {
		printk ("lne390.c: unable to allocate memory for dev->priv!\n");
		return -ENOMEM;
	}

	printk("lne390.c: LNE390%X in EISA slot %d, address", 0xa+revision, ioaddr/0x1000);
	for(i = 0; i < ETHER_ADDR_LEN; i++)
		printk(" %02x", (dev->dev_addr[i] = inb(ioaddr + LNE390_SA_PROM + i)));
	printk(".\nlne390.c: ");

	/* Snarf the interrupt now. CFG file has them all listed as `edge' with share=NO */
	if (dev->irq == 0) {
		unsigned char irq_reg = inb(ioaddr + LNE390_CFG2) >> 3;
		dev->irq = irq_map[irq_reg & 0x07];
		printk("using");
	} else {
		/* This is useless unless we reprogram the card here too */
		if (dev->irq == 2) dev->irq = 9;	/* Doh! */
		printk("assigning");
	}
	printk(" IRQ %d,", dev->irq);

	if ((ret = request_irq(dev->irq, ei_interrupt, 0, dev->name, dev))) {
		printk (" unable to get IRQ %d.\n", dev->irq);
		kfree(dev->priv);
		dev->priv = NULL;
		return ret;
	}

	if (dev->mem_start == 0) {
		unsigned char mem_reg = inb(ioaddr + LNE390_CFG2) & 0x07;

		if (revision)	/* LNE390B */
			dev->mem_start = shmem_mapB[mem_reg] * 0x10000;
		else		/* LNE390A */
			dev->mem_start = shmem_mapA[mem_reg] * 0x10000;
		printk(" using ");
	} else {
		/* Should check for value in shmem_map and reprogram the card to use it */
		dev->mem_start &= 0xfff0000;
		printk(" assigning ");
	}

	printk("%dkB memory at physical address %#lx\n",
			LNE390_STOP_PG/4, dev->mem_start);

	/*
	   BEWARE!! Some dain-bramaged EISA SCUs will allow you to put
	   the card mem within the region covered by `normal' RAM  !!!
	*/
	if (dev->mem_start > 1024*1024) {	/* phys addr > 1MB */
		if (dev->mem_start < virt_to_bus(high_memory)) {
			printk(KERN_CRIT "lne390.c: Card RAM overlaps with normal memory!!!\n");
			printk(KERN_CRIT "lne390.c: Use EISA SCU to set card memory below 1MB,\n");
			printk(KERN_CRIT "lne390.c: or to an address above 0x%lx.\n", virt_to_bus(high_memory));
			printk(KERN_CRIT "lne390.c: Driver NOT installed.\n");
			ret = -EINVAL;
			goto cleanup;
		}
		dev->mem_start = (unsigned long)ioremap(dev->mem_start, LNE390_STOP_PG*0x100);
		if (dev->mem_start == 0) {
			printk(KERN_ERR "lne390.c: Unable to remap card memory above 1MB !!\n");
			printk(KERN_ERR "lne390.c: Try using EISA SCU to set memory below 1MB.\n");
			printk(KERN_ERR "lne390.c: Driver NOT installed.\n");
			ret = -EAGAIN;
			goto cleanup;
		}
		ei_status.reg0 = 1;	/* Use as remap flag */
		printk("lne390.c: remapped %dkB card memory to virtual address %#lx\n",
				LNE390_STOP_PG/4, dev->mem_start);
	}

	dev->mem_end = dev->rmem_end = dev->mem_start
		+ (LNE390_STOP_PG - LNE390_START_PG)*256;
	dev->rmem_start = dev->mem_start + TX_PAGES*256;

	/* The 8390 offset is zero for the LNE390 */
	dev->base_addr = ioaddr;

	ei_status.name = "LNE390";
	ei_status.tx_start_page = LNE390_START_PG;
	ei_status.rx_start_page = LNE390_START_PG + TX_PAGES;
	ei_status.stop_page = LNE390_STOP_PG;
	ei_status.word16 = 1;

	if (ei_debug > 0)
		printk(version);

	ei_status.reset_8390 = &lne390_reset_8390;
	ei_status.block_input = &lne390_block_input;
	ei_status.block_output = &lne390_block_output;
	ei_status.get_8390_hdr = &lne390_get_8390_hdr;

	dev->open = &lne390_open;
	dev->stop = &lne390_close;
	NS8390_init(dev, 0);
	return 0;
cleanup:
	free_irq(dev->irq, dev);
	kfree(dev->priv);
	dev->priv = NULL;
	return ret;
}

/*
 *	Reset as per the packet driver method. Judging by the EISA cfg
 *	file, this just toggles the "Board Enable" bits (bit 2 and 0).
 */

static void lne390_reset_8390(struct net_device *dev)
{
	unsigned short ioaddr = dev->base_addr;

	outb(0x04, ioaddr + LNE390_RESET_PORT);
	if (ei_debug > 1) printk("%s: resetting the LNE390...", dev->name);

	mdelay(2);

	ei_status.txing = 0;
	outb(0x01, ioaddr + LNE390_RESET_PORT);
	if (ei_debug > 1) printk("reset done\n");

	return;
}

/*
 *	Note: In the following three functions is the implicit assumption
 *	that the associated memcpy will only use "rep; movsl" as long as
 *	we keep the counts as some multiple of doublewords. This is a
 *	requirement of the hardware, and also prevents us from using
 *	eth_io_copy_and_sum() since we can't guarantee it will limit
 *	itself to doubleword access.
 */

/*
 *	Grab the 8390 specific header. Similar to the block_input routine, but
 *	we don't need to be concerned with ring wrap as the header will be at
 *	the start of a page, so we optimize accordingly. (A single doubleword.)
 */

static void
lne390_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr, int ring_page)
{
	unsigned long hdr_start = dev->mem_start + ((ring_page - LNE390_START_PG)<<8);
	isa_memcpy_fromio(hdr, hdr_start, sizeof(struct e8390_pkt_hdr));
	hdr->count = (hdr->count + 3) & ~3;     /* Round up allocation. */
}

/*	
 *	Block input and output are easy on shared memory ethercards, the only
 *	complication is when the ring buffer wraps. The count will already
 *	be rounded up to a doubleword value via lne390_get_8390_hdr() above.
 */

static void lne390_block_input(struct net_device *dev, int count, struct sk_buff *skb,
						  int ring_offset)
{
	unsigned long xfer_start = dev->mem_start + ring_offset - (LNE390_START_PG<<8);

	if (xfer_start + count > dev->rmem_end) {
		/* Packet wraps over end of ring buffer. */
		int semi_count = dev->rmem_end - xfer_start;
		isa_memcpy_fromio(skb->data, xfer_start, semi_count);
		count -= semi_count;
		isa_memcpy_fromio(skb->data + semi_count, dev->rmem_start, count);
	} else {
		/* Packet is in one chunk. */
		isa_memcpy_fromio(skb->data, xfer_start, count);
	}
}

static void lne390_block_output(struct net_device *dev, int count,
				const unsigned char *buf, int start_page)
{
	unsigned long shmem = dev->mem_start + ((start_page - LNE390_START_PG)<<8);

	count = (count + 3) & ~3;     /* Round up to doubleword */
	isa_memcpy_toio(shmem, buf, count);
}

static int lne390_open(struct net_device *dev)
{
	ei_open(dev);
	return 0;
}

static int lne390_close(struct net_device *dev)
{

	if (ei_debug > 1)
		printk("%s: Shutting down ethercard.\n", dev->name);

	ei_close(dev);
	return 0;
}

#ifdef MODULE
#define MAX_LNE_CARDS	4	/* Max number of LNE390 cards per module */
static struct net_device dev_lne[MAX_LNE_CARDS];
static int io[MAX_LNE_CARDS];
static int irq[MAX_LNE_CARDS];
static int mem[MAX_LNE_CARDS];

MODULE_PARM(io, "1-" __MODULE_STRING(MAX_LNE_CARDS) "i");
MODULE_PARM(irq, "1-" __MODULE_STRING(MAX_LNE_CARDS) "i");
MODULE_PARM(mem, "1-" __MODULE_STRING(MAX_LNE_CARDS) "i");

int init_module(void)
{
	int this_dev, found = 0;

	for (this_dev = 0; this_dev < MAX_LNE_CARDS; this_dev++) {
		struct net_device *dev = &dev_lne[this_dev];
		dev->irq = irq[this_dev];
		dev->base_addr = io[this_dev];
		dev->mem_start = mem[this_dev];
		dev->init = lne390_probe;
		/* Default is to only install one card. */
		if (io[this_dev] == 0 && this_dev != 0) break;
		if (register_netdev(dev) != 0) {
			printk(KERN_WARNING "lne390.c: No LNE390 card found (i/o = 0x%x).\n", io[this_dev]);
			if (found != 0) {	/* Got at least one. */
				return 0;
			}
			return -ENXIO;
		}
		found++;
	}
	return 0;
}

void cleanup_module(void)
{
	int this_dev;

	for (this_dev = 0; this_dev < MAX_LNE_CARDS; this_dev++) {
		struct net_device *dev = &dev_lne[this_dev];
		if (dev->priv != NULL) {
			void *priv = dev->priv;
			free_irq(dev->irq, dev);
			release_region(dev->base_addr, LNE390_IO_EXTENT);
			if (ei_status.reg0)
				iounmap((void *)dev->mem_start);
			unregister_netdev(dev);
			kfree(priv);
		}
	}
}
#endif /* MODULE */

