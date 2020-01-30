/*
	es3210.c

	Linux driver for Racal-Interlan ES3210 EISA Network Adapter

	Copyright (C) 1996, Paul Gortmaker.

	This software may be used and distributed according to the terms
	of the GNU Public License, incorporated herein by reference.

	Information and Code Sources:

	1) The existing myriad of Linux 8390 drivers written by Donald Becker.

	2) Once again Russ Nelson's asm packet driver provided additional info.

	3) Info for getting IRQ and sh-mem gleaned from the EISA cfg files.
	   Too bad it doesn't work -- see below.

	The ES3210 is an EISA shared memory NS8390 implementation. Note
	that all memory copies to/from the board must be 32bit transfers.
	Which rules out using eth_io_copy_and_sum() in this driver.

	Apparently there are two slightly different revisions of the
	card, since there are two distinct EISA cfg files (!rii0101.cfg
	and !rii0102.cfg) One has media select in the cfg file and the
	other doesn't. Hopefully this will work with either.

	That is about all I can tell you about it, having never actually
	even seen one of these cards. :)  Try http://www.interlan.com
	if you want more info.

	Thanks go to Mark Salazar for testing v0.02 of this driver.

	Bugs, to-fix, etc:

	1) The EISA cfg ports that are *supposed* to have the IRQ and shared
	   mem values just read 0xff all the time. Hrrmpf. Apparently the
	   same happens with the packet driver as the code for reading
	   these registers is disabled there. In the meantime, boot with:
	   ether=<IRQ>,0,0x<shared_mem_addr>,eth0 to override the IRQ and
	   shared memory detection. (The i/o port detection is okay.)

	2) Module support currently untested. Probably works though.

*/

static const char *version =
	"es3210.c: Driver revision v0.03, 14/09/96\n";

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/system.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include "8390.h"

int es_probe(struct net_device *dev);
static int es_probe1(struct net_device *dev, int ioaddr);

static int es_open(struct net_device *dev);
static int es_close(struct net_device *dev);

static void es_reset_8390(struct net_device *dev);

static void es_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr, int ring_page);
static void es_block_input(struct net_device *dev, int count, struct sk_buff *skb, int ring_offset);
static void es_block_output(struct net_device *dev, int count, const unsigned char *buf, int start_page);

#define ES_START_PG	0x00    /* First page of TX buffer		*/
#define ES_STOP_PG	0x40    /* Last page +1 of RX ring		*/

#define ES_IO_EXTENT	0x37	/* The cfg file says 0xc90 -> 0xcc7	*/
#define ES_ID_PORT	0xc80	/* Same for all EISA cards 		*/
#define ES_SA_PROM	0xc90	/* Start of e'net addr.			*/
#define ES_RESET_PORT	0xc84	/* From the packet driver source	*/
#define ES_NIC_OFFSET	0xca0	/* Hello, the 8390 is *here*		*/

#define ES_ADDR0	0x02	/* 3 byte vendor prefix			*/
#define ES_ADDR1	0x07
#define ES_ADDR2	0x01

/*
 * Two card revisions. EISA ID's are always rev. minor, rev. major,, and
 * then the three vendor letters stored in 5 bits each, with an "a" = 1.
 * For eg: "rii" = 10010 01001 01001 = 0x4929, which is how the EISA
 * config utility determines automagically what config file(s) to use.
 */
#define ES_EISA_ID1	0x01012949	/* !rii0101.cfg 		*/
#define ES_EISA_ID2	0x02012949	/* !rii0102.cfg 		*/

#define ES_CFG1		0xcc0	/* IOPORT(1) --> IOPORT(6) in cfg file	*/
#define ES_CFG2		0xcc1
#define ES_CFG3		0xcc2
#define ES_CFG4		0xcc3
#define ES_CFG5		0xcc4
#define ES_CFG6		0xc84	/* NB: 0xc84 is also "reset" port.	*/

/*
 *	You can OR any of the following bits together and assign it
 *	to ES_DEBUG to get verbose driver info during operation.
 *	Some of these don't do anything yet.
 */

#define ES_D_PROBE	0x01
#define ES_D_RX_PKT	0x02
#define ES_D_TX_PKT	0x04
#define ED_D_IRQ	0x08

#define ES_DEBUG	0

static unsigned char lo_irq_map[] __initdata = {3, 4, 5, 6, 7, 9, 10};
static unsigned char hi_irq_map[] __initdata = {11, 12, 0, 14, 0, 0, 0, 15};

/*
 *	Probe for the card. The best way is to read the EISA ID if it
 *	is known. Then we check the prefix of the station address
 *	PROM for a match against the Racal-Interlan assigned value.
 */

int __init es_probe(struct net_device *dev)
{
	unsigned short ioaddr = dev->base_addr;

	SET_MODULE_OWNER(dev);

	if (ioaddr > 0x1ff)		/* Check a single specified location. */
		return es_probe1(dev, ioaddr);
	else if (ioaddr > 0)		/* Don't probe at all. */
		return -ENXIO;

	if (!EISA_bus) {
#if ES_DEBUG & ES_D_PROBE
		printk("es3210.c: Not EISA bus. Not probing high ports.\n");
#endif
		return -ENXIO;
	}

	/* EISA spec allows for up to 16 slots, but 8 is typical. */
	for (ioaddr = 0x1000; ioaddr < 0x9000; ioaddr += 0x1000)
		if (es_probe1(dev, ioaddr) == 0)
			return 0;

	return -ENODEV;
}

static int __init es_probe1(struct net_device *dev, int ioaddr)
{
	int i, retval;
	unsigned long eisa_id;

	if (!request_region(ioaddr + ES_SA_PROM, ES_IO_EXTENT, "es3210"))
		return -ENODEV;

#if ES_DEBUG & ES_D_PROBE
	printk("es3210.c: probe at %#x, ID %#8x\n", ioaddr, inl(ioaddr + ES_ID_PORT));
	printk("es3210.c: config regs: %#x %#x %#x %#x %#x %#x\n",
		inb(ioaddr + ES_CFG1), inb(ioaddr + ES_CFG2), inb(ioaddr + ES_CFG3),
		inb(ioaddr + ES_CFG4), inb(ioaddr + ES_CFG5), inb(ioaddr + ES_CFG6));
#endif


/*	Check the EISA ID of the card. */
	eisa_id = inl(ioaddr + ES_ID_PORT);
	if ((eisa_id != ES_EISA_ID1) && (eisa_id != ES_EISA_ID2)) {
		retval = -ENODEV;
		goto out;
	}

/*	Check the Racal vendor ID as well. */
	if (inb(ioaddr + ES_SA_PROM + 0) != ES_ADDR0
		|| inb(ioaddr + ES_SA_PROM + 1) != ES_ADDR1
		|| inb(ioaddr + ES_SA_PROM + 2) != ES_ADDR2 ) {
		printk("es3210.c: card not found");
		for(i = 0; i < ETHER_ADDR_LEN; i++)
			printk(" %02x", inb(ioaddr + ES_SA_PROM + i));
		printk(" (invalid prefix).\n");
		retval = -ENODEV;
		goto out;
	}

	printk("es3210.c: ES3210 rev. %ld at %#x, node", eisa_id>>24, ioaddr);
	for(i = 0; i < ETHER_ADDR_LEN; i++)
		printk(" %02x", (dev->dev_addr[i] = inb(ioaddr + ES_SA_PROM + i)));

	/* Snarf the interrupt now. */
	if (dev->irq == 0) {
		unsigned char hi_irq = inb(ioaddr + ES_CFG2) & 0x07;
		unsigned char lo_irq = inb(ioaddr + ES_CFG1) & 0xfe;

		if (hi_irq != 0) {
			dev->irq = hi_irq_map[hi_irq - 1];
		} else {
			int i = 0;
			while (lo_irq > (1<<i)) i++;
			dev->irq = lo_irq_map[i];
		}
		printk(" using IRQ %d", dev->irq);
#if ES_DEBUG & ES_D_PROBE
		printk("es3210.c: hi_irq %#x, lo_irq %#x, dev->irq = %d\n",
					hi_irq, lo_irq, dev->irq);
#endif
	} else {
		if (dev->irq == 2)
			dev->irq = 9;			/* Doh! */
		printk(" assigning IRQ %d", dev->irq);
	}

	if (request_irq(dev->irq, ei_interrupt, 0, "es3210", dev)) {
		printk (" unable to get IRQ %d.\n", dev->irq);
		retval = -EAGAIN;
		goto out;
	}

	if (dev->mem_start == 0) {
		unsigned char mem_enabled = inb(ioaddr + ES_CFG2) & 0xc0;
		unsigned char mem_bits = inb(ioaddr + ES_CFG3) & 0x07;

		if (mem_enabled != 0x80) {
			printk(" shared mem disabled - giving up\n");
			retval = -ENXIO;
			goto out1;
		}
		dev->mem_start = 0xC0000 + mem_bits*0x4000;
		printk(" using ");
	} else {
		printk(" assigning ");
	}

	dev->mem_end = dev->rmem_end = dev->mem_start
		+ (ES_STOP_PG - ES_START_PG)*256;
	dev->rmem_start = dev->mem_start + TX_PAGES*256;

	printk("mem %#lx-%#lx\n", dev->mem_start, dev->mem_end-1);

	/* Allocate dev->priv and fill in 8390 specific dev fields. */
	if (ethdev_init(dev)) {
		printk (" unable to allocate memory for dev->priv.\n");
		retval = -ENOMEM;
		goto out1;
	}

#if ES_DEBUG & ES_D_PROBE
	if (inb(ioaddr + ES_CFG5))
		printk("es3210: Warning - DMA channel enabled, but not used here.\n");
#endif
	/* Note, point at the 8390, and not the card... */
	dev->base_addr = ioaddr + ES_NIC_OFFSET;

	ei_status.name = "ES3210";
	ei_status.tx_start_page = ES_START_PG;
	ei_status.rx_start_page = ES_START_PG + TX_PAGES;
	ei_status.stop_page = ES_STOP_PG;
	ei_status.word16 = 1;

	if (ei_debug > 0)
		printk(version);

	ei_status.reset_8390 = &es_reset_8390;
	ei_status.block_input = &es_block_input;
	ei_status.block_output = &es_block_output;
	ei_status.get_8390_hdr = &es_get_8390_hdr;

	dev->open = &es_open;
	dev->stop = &es_close;
	NS8390_init(dev, 0);
	return 0;
out1:
	free_irq(dev->irq, dev);
out:
	release_region(ioaddr + ES_SA_PROM, ES_IO_EXTENT);
	return retval;
}

/*
 *	Reset as per the packet driver method. Judging by the EISA cfg
 *	file, this just toggles the "Board Enable" bits (bit 2 and 0).
 */

static void es_reset_8390(struct net_device *dev)
{
	unsigned short ioaddr = dev->base_addr;
	unsigned long end;

	outb(0x04, ioaddr + ES_RESET_PORT);
	if (ei_debug > 1) printk("%s: resetting the ES3210...", dev->name);

	end = jiffies + 2*HZ/100;
        while ((signed)(end - jiffies) > 0) continue;

	ei_status.txing = 0;
	outb(0x01, ioaddr + ES_RESET_PORT);
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
es_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr, int ring_page)
{
	unsigned long hdr_start = dev->mem_start + ((ring_page - ES_START_PG)<<8);
	isa_memcpy_fromio(hdr, hdr_start, sizeof(struct e8390_pkt_hdr));
	hdr->count = (hdr->count + 3) & ~3;     /* Round up allocation. */
}

/*
 *	Block input and output are easy on shared memory ethercards, the only
 *	complication is when the ring buffer wraps. The count will already
 *	be rounded up to a doubleword value via es_get_8390_hdr() above.
 */

static void es_block_input(struct net_device *dev, int count, struct sk_buff *skb,
						  int ring_offset)
{
	unsigned long xfer_start = dev->mem_start + ring_offset - (ES_START_PG<<8);

	if (xfer_start + count > dev->rmem_end) {
		/* Packet wraps over end of ring buffer. */
		int semi_count = dev->rmem_end - xfer_start;
		isa_memcpy_fromio(skb->data, xfer_start, semi_count);
		count -= semi_count;
		isa_memcpy_fromio(skb->data + semi_count, dev->rmem_start, count);
	} else {
		/* Packet is in one chunk. */
		isa_eth_io_copy_and_sum(skb, xfer_start, count, 0);
	}
}

static void es_block_output(struct net_device *dev, int count,
				const unsigned char *buf, int start_page)
{
	unsigned long shmem = dev->mem_start + ((start_page - ES_START_PG)<<8);

	count = (count + 3) & ~3;     /* Round up to doubleword */
	isa_memcpy_toio(shmem, buf, count);
}

static int es_open(struct net_device *dev)
{
	ei_open(dev);
	return 0;
}

static int es_close(struct net_device *dev)
{

	if (ei_debug > 1)
		printk("%s: Shutting down ethercard.\n", dev->name);

	ei_close(dev);
	return 0;
}

#ifdef MODULE
#define MAX_ES_CARDS	4	/* Max number of ES3210 cards per module */
#define NAMELEN		8	/* # of chars for storing dev->name */
static struct net_device dev_es3210[MAX_ES_CARDS];
static int io[MAX_ES_CARDS];
static int irq[MAX_ES_CARDS];
static int mem[MAX_ES_CARDS];

MODULE_PARM(io, "1-" __MODULE_STRING(MAX_ES_CARDS) "i");
MODULE_PARM(irq, "1-" __MODULE_STRING(MAX_ES_CARDS) "i");
MODULE_PARM(mem, "1-" __MODULE_STRING(MAX_ES_CARDS) "i");

int
init_module(void)
{
	int this_dev, found = 0;

	for (this_dev = 0; this_dev < MAX_ES_CARDS; this_dev++) {
		struct net_device *dev = &dev_es3210[this_dev];
		dev->irq = irq[this_dev];
		dev->base_addr = io[this_dev];
		dev->mem_start = mem[this_dev];		/* Currently ignored by driver */
		dev->init = es_probe;
		/* Default is to only install one card. */
		if (io[this_dev] == 0 && this_dev != 0) break;
		if (register_netdev(dev) != 0) {
			printk(KERN_WARNING "es3210.c: No es3210 card found (i/o = 0x%x).\n", io[this_dev]);
			if (found != 0) {	/* Got at least one. */
				return 0;
			}
			return -ENXIO;
		}
		found++;
	}
	return 0;
}

void
cleanup_module(void)
{
	int this_dev;

	for (this_dev = 0; this_dev < MAX_ES_CARDS; this_dev++) {
		struct net_device *dev = &dev_es3210[this_dev];
		if (dev->priv != NULL) {
			void *priv = dev->priv;
			free_irq(dev->irq, dev);
			release_region(dev->base_addr, ES_IO_EXTENT);
			unregister_netdev(dev);
			kfree(priv);
		}
	}
}
#endif /* MODULE */
