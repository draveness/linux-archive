/* tulip_core.c: A DEC 21040-family ethernet driver for Linux. */

/*
	Maintained by Jeff Garzik <jgarzik@mandrakesoft.com>
	Copyright 2000  The Linux Kernel Team
	Written/copyright 1994-1999 by Donald Becker.

	This software may be used and distributed according to the terms
	of the GNU Public License, incorporated herein by reference.

	Please read Documentation/networking/tulip.txt for more
	information.

	For this specific driver variant please use linux-kernel for
	bug reports.

	Additional information available at
	http://cesdis.gsfc.nasa.gov/linux/drivers/tulip.html

*/

#include <linux/module.h>
#include "tulip.h"
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <asm/unaligned.h>

static char version[] __devinitdata =
	"Linux Tulip driver version 0.9.13 (January 2, 2001)\n";


/* A few user-configurable values. */

/* Maximum events (Rx packets, etc.) to handle at each interrupt. */
static unsigned int max_interrupt_work = 25;

#define MAX_UNITS 8
/* Used to pass the full-duplex flag, etc. */
static int full_duplex[MAX_UNITS];
static int options[MAX_UNITS];
static int mtu[MAX_UNITS];			/* Jumbo MTU for interfaces. */

/*  The possible media types that can be set in options[] are: */
const char * const medianame[] = {
	"10baseT", "10base2", "AUI", "100baseTx",
	"10baseT-FD", "100baseTx-FD", "100baseT4", "100baseFx",
	"100baseFx-FD", "MII 10baseT", "MII 10baseT-FD", "MII",
	"10baseT(forced)", "MII 100baseTx", "MII 100baseTx-FD", "MII 100baseT4",
};

/* Set the copy breakpoint for the copy-only-tiny-buffer Rx structure. */
#if defined(__alpha__) || defined(__arm__) || defined(__hppa__) \
	|| defined(__sparc_) || defined(__ia64__)
static int rx_copybreak = 1518;
#else
static int rx_copybreak = 100;
#endif

/*
  Set the bus performance register.
	Typical: Set 16 longword cache alignment, no burst limit.
	Cache alignment bits 15:14	     Burst length 13:8
		0000	No alignment  0x00000000 unlimited		0800 8 longwords
		4000	8  longwords		0100 1 longword		1000 16 longwords
		8000	16 longwords		0200 2 longwords	2000 32 longwords
		C000	32  longwords		0400 4 longwords
	Warning: many older 486 systems are broken and require setting 0x00A04800
	   8 longword cache alignment, 8 longword burst.
	ToDo: Non-Intel setting could be better.
*/

#if defined(__alpha__) || defined(__ia64__)
static int csr0 = 0x01A00000 | 0xE000;
#elif defined(__i386__) || defined(__powerpc__)
static int csr0 = 0x01A00000 | 0x8000;
#elif defined(__sparc__) || defined(__hppa__)
/* The UltraSparc PCI controllers will disconnect at every 64-byte
 * crossing anyways so it makes no sense to tell Tulip to burst
 * any more than that.
 */
static int csr0 = 0x01A00000 | 0x9000;
#elif defined(__arm__)
static int csr0 = 0x01A00000 | 0x4800;
#else
#warning Processor architecture undefined!
static int csr0 = 0x00A00000 | 0x4800;
#endif

/* Operational parameters that usually are not changed. */
/* Time in jiffies before concluding the transmitter is hung. */
#define TX_TIMEOUT  (4*HZ)


MODULE_AUTHOR("The Linux Kernel Team");
MODULE_DESCRIPTION("Digital 21*4* Tulip ethernet driver");
MODULE_PARM(tulip_debug, "i");
MODULE_PARM(max_interrupt_work, "i");
MODULE_PARM(rx_copybreak, "i");
MODULE_PARM(csr0, "i");
MODULE_PARM(options, "1-" __MODULE_STRING(MAX_UNITS) "i");
MODULE_PARM(full_duplex, "1-" __MODULE_STRING(MAX_UNITS) "i");

#define TULIP_MODULE_NAME "tulip"
#define PFX TULIP_MODULE_NAME ": "

#ifdef TULIP_DEBUG
int tulip_debug = TULIP_DEBUG;
#else
int tulip_debug = 1;
#endif



/*
 * This table use during operation for capabilities and media timer.
 *
 * It is indexed via the values in 'enum chips'
 */

struct tulip_chip_table tulip_tbl[] = {
  { "Digital DC21040 Tulip", 128, 0x0001ebef, 0, tulip_timer },
  { "Digital DC21041 Tulip", 128, 0x0001ebef,
	HAS_MEDIA_TABLE | HAS_NWAY, tulip_timer },
  { "Digital DS21140 Tulip", 128, 0x0001ebef,
	HAS_MII | HAS_MEDIA_TABLE | CSR12_IN_SROM, tulip_timer },
  { "Digital DS21143 Tulip", 128, 0x0801fbff,
	HAS_MII | HAS_MEDIA_TABLE | ALWAYS_CHECK_MII | HAS_ACPI | HAS_NWAY
	| HAS_INTR_MITIGATION, t21142_timer },
  { "Lite-On 82c168 PNIC", 256, 0x0001ebef,
	HAS_MII | HAS_PNICNWAY, pnic_timer },
  { "Macronix 98713 PMAC", 128, 0x0001ebef,
	HAS_MII | HAS_MEDIA_TABLE | CSR12_IN_SROM, mxic_timer },
  { "Macronix 98715 PMAC", 256, 0x0001ebef,
	HAS_MEDIA_TABLE, mxic_timer },
  { "Macronix 98725 PMAC", 256, 0x0001ebef,
	HAS_MEDIA_TABLE, mxic_timer },
  { "ASIX AX88140", 128, 0x0001fbff,
	HAS_MII | HAS_MEDIA_TABLE | CSR12_IN_SROM | MC_HASH_ONLY | IS_ASIX, tulip_timer },
  { "Lite-On PNIC-II", 256, 0x0801fbff,
	HAS_MII | HAS_NWAY | HAS_8023X, t21142_timer },
  { "ADMtek Comet", 256, 0x0001abef,
	MC_HASH_ONLY, comet_timer },
  { "Compex 9881 PMAC", 128, 0x0001ebef,
	HAS_MII | HAS_MEDIA_TABLE | CSR12_IN_SROM, mxic_timer },
  { "Intel DS21145 Tulip", 128, 0x0801fbff,
	HAS_MII | HAS_MEDIA_TABLE | ALWAYS_CHECK_MII | HAS_ACPI | HAS_NWAY,
	t21142_timer },
  { "Davicom DM9102/DM9102A", 128, 0x0001ebef,
	HAS_MII | HAS_MEDIA_TABLE | CSR12_IN_SROM | HAS_ACPI,
	tulip_timer },
  {0},
};


static struct pci_device_id tulip_pci_tbl[] __devinitdata = {
	{ 0x1011, 0x0002, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DC21040 },
	{ 0x1011, 0x0014, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DC21041 },
	{ 0x1011, 0x0009, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DC21140 },
	{ 0x1011, 0x0019, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DC21143 },
	{ 0x11AD, 0x0002, PCI_ANY_ID, PCI_ANY_ID, 0, 0, LC82C168 },
	{ 0x10d9, 0x0512, PCI_ANY_ID, PCI_ANY_ID, 0, 0, MX98713 },
	{ 0x10d9, 0x0531, PCI_ANY_ID, PCI_ANY_ID, 0, 0, MX98715 },
/*	{ 0x10d9, 0x0531, PCI_ANY_ID, PCI_ANY_ID, 0, 0, MX98725 },*/
	{ 0x125B, 0x1400, PCI_ANY_ID, PCI_ANY_ID, 0, 0, AX88140 },
	{ 0x11AD, 0xc115, PCI_ANY_ID, PCI_ANY_ID, 0, 0, PNIC2 },
	{ 0x1317, 0x0981, PCI_ANY_ID, PCI_ANY_ID, 0, 0, COMET },
	{ 0x1317, 0x0985, PCI_ANY_ID, PCI_ANY_ID, 0, 0, COMET },
	{ 0x1317, 0x1985, PCI_ANY_ID, PCI_ANY_ID, 0, 0, COMET },
	{ 0x13D1, 0xAB02, PCI_ANY_ID, PCI_ANY_ID, 0, 0, COMET },
	{ 0x13D1, 0xAB03, PCI_ANY_ID, PCI_ANY_ID, 0, 0, COMET },
	{ 0x104A, 0x0981, PCI_ANY_ID, PCI_ANY_ID, 0, 0, COMET },
	{ 0x104A, 0x2774, PCI_ANY_ID, PCI_ANY_ID, 0, 0, COMET },
	{ 0x11F6, 0x9881, PCI_ANY_ID, PCI_ANY_ID, 0, 0, COMPEX9881 },
	{ 0x8086, 0x0039, PCI_ANY_ID, PCI_ANY_ID, 0, 0, I21145 },
	{ 0x1282, 0x9100, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DM910X },
	{ 0x1282, 0x9102, PCI_ANY_ID, PCI_ANY_ID, 0, 0, DM910X },
	{ 0x1113, 0x1217, PCI_ANY_ID, PCI_ANY_ID, 0, 0, MX98715 },
	{0, }
};
MODULE_DEVICE_TABLE(pci, tulip_pci_tbl);


/* A full-duplex map for media types. */
const char tulip_media_cap[] =
{0,0,0,16,  3,19,16,24,  27,4,7,5, 0,20,23,20 };
u8 t21040_csr13[] = {2,0x0C,8,4,  4,0,0,0, 0,0,0,0, 4,0,0,0};

/* 21041 transceiver register settings: 10-T, 10-2, AUI, 10-T, 10T-FD*/
u16 t21041_csr13[] = {
	csr13_mask_10bt,		/* 10-T */
	csr13_mask_auibnc,		/* 10-2 */
	csr13_mask_auibnc,		/* AUI */
	csr13_mask_10bt,		/* 10-T */
	csr13_mask_10bt,		/* 10T-FD */
};
u16 t21041_csr14[] = { 0xFFFF, 0xF7FD, 0xF7FD, 0x7F3F, 0x7F3D, };
u16 t21041_csr15[] = { 0x0008, 0x0006, 0x000E, 0x0008, 0x0008, };


static void tulip_tx_timeout(struct net_device *dev);
static void tulip_init_ring(struct net_device *dev);
static int tulip_start_xmit(struct sk_buff *skb, struct net_device *dev);
static int tulip_open(struct net_device *dev);
static int tulip_close(struct net_device *dev);
static void tulip_up(struct net_device *dev);
static void tulip_down(struct net_device *dev);
static struct net_device_stats *tulip_get_stats(struct net_device *dev);
static int private_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
static void set_rx_mode(struct net_device *dev);



static void tulip_up(struct net_device *dev)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	long ioaddr = dev->base_addr;
	int next_tick = 3*HZ;
	int i;

	DPRINTK("ENTER\n");

	/* Wake the chip from sleep/snooze mode. */
	if (tp->flags & HAS_ACPI)
		pci_write_config_dword(tp->pdev, 0x40, 0);

	/* On some chip revs we must set the MII/SYM port before the reset!? */
	if (tp->mii_cnt  ||  (tp->mtable  &&  tp->mtable->has_mii))
		tulip_outl_csr (tp, 0x00040000, CSR6);

	/* Reset the chip, holding bit 0 set at least 50 PCI cycles. */
	outl(0x00000001, ioaddr + CSR0);
	udelay(100);

	/* Deassert reset.
	   Wait the specified 50 PCI cycles after a reset by initializing
	   Tx and Rx queues and the address filter list. */
	outl(tp->csr0, ioaddr + CSR0);
	udelay(100);

	if (tulip_debug > 1)
		printk(KERN_DEBUG "%s: tulip_up(), irq==%d.\n", dev->name, dev->irq);

	if (tp->flags & MC_HASH_ONLY) {
		u32 addr_low = cpu_to_le32(get_unaligned((u32 *)dev->dev_addr));
		u32 addr_high = cpu_to_le32(get_unaligned((u16 *)(dev->dev_addr+4)));
		if (tp->chip_id == AX88140) {
			outl(0, ioaddr + CSR13);
			outl(addr_low,  ioaddr + CSR14);
			outl(1, ioaddr + CSR13);
			outl(addr_high, ioaddr + CSR14);
		} else if (tp->chip_id == COMET) {
			outl(addr_low,  ioaddr + 0xA4);
			outl(addr_high, ioaddr + 0xA8);
			outl(0, ioaddr + 0xAC);
			outl(0, ioaddr + 0xB0);
		}
	} else {
		/* This is set_rx_mode(), but without starting the transmitter. */
		u16 *eaddrs = (u16 *)dev->dev_addr;
		u16 *setup_frm = &tp->setup_frame[15*6];
		dma_addr_t mapping;

		/* 21140 bug: you must add the broadcast address. */
		memset(tp->setup_frame, 0xff, sizeof(tp->setup_frame));
		/* Fill the final entry of the table with our physical address. */
		*setup_frm++ = eaddrs[0]; *setup_frm++ = eaddrs[0];
		*setup_frm++ = eaddrs[1]; *setup_frm++ = eaddrs[1];
		*setup_frm++ = eaddrs[2]; *setup_frm++ = eaddrs[2];

		mapping = pci_map_single(tp->pdev, tp->setup_frame,
					 sizeof(tp->setup_frame),
					 PCI_DMA_TODEVICE);
		tp->tx_buffers[0].skb = NULL;
		tp->tx_buffers[0].mapping = mapping;

		/* Put the setup frame on the Tx list. */
		tp->tx_ring[0].length = cpu_to_le32(0x08000000 | 192);
		tp->tx_ring[0].buffer1 = cpu_to_le32(mapping);
		tp->tx_ring[0].status = cpu_to_le32(DescOwned);

		tp->cur_tx++;
	}

	outl(tp->rx_ring_dma, ioaddr + CSR3);
	outl(tp->tx_ring_dma, ioaddr + CSR4);

	tp->saved_if_port = dev->if_port;
	if (dev->if_port == 0)
		dev->if_port = tp->default_port;

	/* Allow selecting a default media. */
	i = 0;
	if (tp->mtable == NULL)
		goto media_picked;
	if (dev->if_port) {
		int looking_for = tulip_media_cap[dev->if_port] & MediaIsMII ? 11 :
			(dev->if_port == 12 ? 0 : dev->if_port);
		for (i = 0; i < tp->mtable->leafcount; i++)
			if (tp->mtable->mleaf[i].media == looking_for) {
				printk(KERN_INFO "%s: Using user-specified media %s.\n",
					   dev->name, medianame[dev->if_port]);
				goto media_picked;
			}
	}
	if ((tp->mtable->defaultmedia & 0x0800) == 0) {
		int looking_for = tp->mtable->defaultmedia & 15;
		for (i = 0; i < tp->mtable->leafcount; i++)
			if (tp->mtable->mleaf[i].media == looking_for) {
				printk(KERN_INFO "%s: Using EEPROM-set media %s.\n",
					   dev->name, medianame[looking_for]);
				goto media_picked;
			}
	}
	/* Start sensing first non-full-duplex media. */
	for (i = tp->mtable->leafcount - 1;
		 (tulip_media_cap[tp->mtable->mleaf[i].media] & MediaAlwaysFD) && i > 0; i--)
		;
media_picked:

	tp->csr6 = 0;
	tp->cur_index = i;
	tp->nwayset = 0;
	if (dev->if_port == 0  && tp->chip_id == DC21041)
		tp->nway = 1;

	if (dev->if_port == 0  &&  tp->chip_id == DC21142) {
		if (tp->mii_cnt) {
			tulip_select_media(dev, 1);
			if (tulip_debug > 1)
				printk(KERN_INFO "%s: Using MII transceiver %d, status "
					   "%4.4x.\n",
					   dev->name, tp->phys[0], tulip_mdio_read(dev, tp->phys[0], 1));
			tulip_outl_csr(tp, csr6_mask_defstate, CSR6);
			tp->csr6 = csr6_mask_hdcap;
			dev->if_port = 11;
			outl(0x0000, ioaddr + CSR13);
			outl(0x0000, ioaddr + CSR14);
		} else
			t21142_start_nway(dev);
	} else if (tp->chip_id == PNIC2) {
		t21142_start_nway(dev);
	} else if (tp->chip_id == LC82C168  &&  ! tp->medialock) {
		if (tp->mii_cnt) {
			dev->if_port = 11;
			tp->csr6 = 0x814C0000 | (tp->full_duplex ? 0x0200 : 0);
			outl(0x0001, ioaddr + CSR15);
		} else if (inl(ioaddr + CSR5) & TPLnkPass)
			pnic_do_nway(dev);
		else {
			/* Start with 10mbps to do autonegotiation. */
			outl(0x32, ioaddr + CSR12);
			tp->csr6 = 0x00420000;
			outl(0x0001B078, ioaddr + 0xB8);
			outl(0x0201B078, ioaddr + 0xB8);
			next_tick = 1*HZ;
		}
	} else if ((tp->chip_id == MX98713 || tp->chip_id == COMPEX9881)
			   && ! tp->medialock) {
		dev->if_port = 0;
		tp->csr6 = 0x01880000 | (tp->full_duplex ? 0x0200 : 0);
		outl(0x0f370000 | inw(ioaddr + 0x80), ioaddr + 0x80);
	} else if (tp->chip_id == MX98715 || tp->chip_id == MX98725) {
		/* Provided by BOLO, Macronix - 12/10/1998. */
		dev->if_port = 0;
		tp->csr6 = 0x01a80200;
		outl(0x0f370000 | inw(ioaddr + 0x80), ioaddr + 0x80);
		outl(0x11000 | inw(ioaddr + 0xa0), ioaddr + 0xa0);
	} else if (tp->chip_id == DC21143  &&
			   tulip_media_cap[dev->if_port] & MediaIsMII) {
		/* We must reset the media CSRs when we force-select MII mode. */
		outl(0x0000, ioaddr + CSR13);
		outl(0x0000, ioaddr + CSR14);
		outl(0x0008, ioaddr + CSR15);
	} else if (tp->chip_id == COMET) {
		dev->if_port = 0;
		tp->csr6 = 0x00040000;
	} else if (tp->chip_id == AX88140) {
		tp->csr6 = tp->mii_cnt ? 0x00040100 : 0x00000100;
	} else
		tulip_select_media(dev, 1);

	/* Start the chip's Tx to process setup frame. */
	tulip_outl_csr(tp, tp->csr6, CSR6);
	tulip_outl_csr(tp, tp->csr6 | csr6_st, CSR6);

	/* Enable interrupts by setting the interrupt mask. */
	outl(tulip_tbl[tp->chip_id].valid_intrs, ioaddr + CSR5);
	outl(tulip_tbl[tp->chip_id].valid_intrs, ioaddr + CSR7);
	tulip_outl_csr(tp, tp->csr6 | csr6_st | csr6_sr, CSR6);
	outl(0, ioaddr + CSR2);		/* Rx poll demand */

	if (tulip_debug > 2) {
		printk(KERN_DEBUG "%s: Done tulip_open(), CSR0 %8.8x, CSR5 %8.8x CSR6 %8.8x.\n",
			   dev->name, inl(ioaddr + CSR0), inl(ioaddr + CSR5),
			   inl(ioaddr + CSR6));
	}
	/* Set the timer to switch to check for link beat and perhaps switch
	   to an alternate media type. */
	init_timer(&tp->timer);
	tp->timer.expires = RUN_AT(next_tick);
	tp->timer.data = (unsigned long)dev;
	tp->timer.function = tulip_tbl[tp->chip_id].media_timer;
	add_timer(&tp->timer);
}


static int
tulip_open(struct net_device *dev)
{
	int retval;
	MOD_INC_USE_COUNT;

	if ((retval = request_irq(dev->irq, &tulip_interrupt, SA_SHIRQ, dev->name, dev))) {
		MOD_DEC_USE_COUNT;
		return retval;
	}

	tulip_init_ring (dev);

	tulip_up (dev);

	netif_start_queue (dev);

	return 0;
}


static void tulip_tx_timeout(struct net_device *dev)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	long ioaddr = dev->base_addr;
	unsigned long flags;

	DPRINTK("ENTER\n");

	spin_lock_irqsave (&tp->lock, flags);

	if (tulip_media_cap[dev->if_port] & MediaIsMII) {
		/* Do nothing -- the media monitor should handle this. */
		if (tulip_debug > 1)
			printk(KERN_WARNING "%s: Transmit timeout using MII device.\n",
				   dev->name);
	} else if (tp->chip_id == DC21040) {
		if ( !tp->medialock  &&  inl(ioaddr + CSR12) & 0x0002) {
			dev->if_port = (dev->if_port == 2 ? 0 : 2);
			printk(KERN_INFO "%s: 21040 transmit timed out, switching to "
				   "%s.\n",
				   dev->name, medianame[dev->if_port]);
			tulip_select_media(dev, 0);
		}
		goto out;
	} else if (tp->chip_id == DC21041) {
		int csr12 = inl(ioaddr + CSR12);

		printk(KERN_WARNING "%s: 21041 transmit timed out, status %8.8x, "
			   "CSR12 %8.8x, CSR13 %8.8x, CSR14 %8.8x, resetting...\n",
			   dev->name, inl(ioaddr + CSR5), csr12,
			   inl(ioaddr + CSR13), inl(ioaddr + CSR14));
		tp->mediasense = 1;
		if ( ! tp->medialock) {
			if (dev->if_port == 1 || dev->if_port == 2)
				if (csr12 & 0x0004) {
					dev->if_port = 2 - dev->if_port;
				} else
					dev->if_port = 0;
			else
				dev->if_port = 1;
			tulip_select_media(dev, 0);
		}
	} else if (tp->chip_id == DC21140 || tp->chip_id == DC21142
			   || tp->chip_id == MX98713 || tp->chip_id == COMPEX9881
			   || tp->chip_id == DM910X) {
		printk(KERN_WARNING "%s: 21140 transmit timed out, status %8.8x, "
			   "SIA %8.8x %8.8x %8.8x %8.8x, resetting...\n",
			   dev->name, inl(ioaddr + CSR5), inl(ioaddr + CSR12),
			   inl(ioaddr + CSR13), inl(ioaddr + CSR14), inl(ioaddr + CSR15));
		if ( ! tp->medialock  &&  tp->mtable) {
			do
				--tp->cur_index;
			while (tp->cur_index >= 0
				   && (tulip_media_cap[tp->mtable->mleaf[tp->cur_index].media]
					   & MediaIsFD));
			if (--tp->cur_index < 0) {
				/* We start again, but should instead look for default. */
				tp->cur_index = tp->mtable->leafcount - 1;
			}
			tulip_select_media(dev, 0);
			printk(KERN_WARNING "%s: transmit timed out, switching to %s "
				   "media.\n", dev->name, medianame[dev->if_port]);
		}
	} else {
		printk(KERN_WARNING "%s: Transmit timed out, status %8.8x, CSR12 "
			   "%8.8x, resetting...\n",
			   dev->name, inl(ioaddr + CSR5), inl(ioaddr + CSR12));
		dev->if_port = 0;
	}

#if defined(way_too_many_messages)
	if (tulip_debug > 3) {
		int i;
		for (i = 0; i < RX_RING_SIZE; i++) {
			u8 *buf = (u8 *)(tp->rx_ring[i].buffer1);
			int j;
			printk(KERN_DEBUG "%2d: %8.8x %8.8x %8.8x %8.8x  "
				   "%2.2x %2.2x %2.2x.\n",
				   i, (unsigned int)tp->rx_ring[i].status,
				   (unsigned int)tp->rx_ring[i].length,
				   (unsigned int)tp->rx_ring[i].buffer1,
				   (unsigned int)tp->rx_ring[i].buffer2,
				   buf[0], buf[1], buf[2]);
			for (j = 0; buf[j] != 0xee && j < 1600; j++)
				if (j < 100) printk(" %2.2x", buf[j]);
			printk(" j=%d.\n", j);
		}
		printk(KERN_DEBUG "  Rx ring %8.8x: ", (int)tp->rx_ring);
		for (i = 0; i < RX_RING_SIZE; i++)
			printk(" %8.8x", (unsigned int)tp->rx_ring[i].status);
		printk("\n" KERN_DEBUG "  Tx ring %8.8x: ", (int)tp->tx_ring);
		for (i = 0; i < TX_RING_SIZE; i++)
			printk(" %8.8x", (unsigned int)tp->tx_ring[i].status);
		printk("\n");
	}
#endif

	/* Stop and restart the chip's Tx processes . */
	tulip_restart_rxtx(tp, tp->csr6);
	/* Trigger an immediate transmit demand. */
	outl(0, ioaddr + CSR1);

	tp->stats.tx_errors++;

out:
	dev->trans_start = jiffies;
	spin_unlock_irqrestore (&tp->lock, flags);
}


/* Initialize the Rx and Tx rings, along with various 'dev' bits. */
static void tulip_init_ring(struct net_device *dev)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	int i;

	DPRINTK("ENTER\n");

	tp->cur_rx = tp->cur_tx = 0;
	tp->dirty_rx = tp->dirty_tx = 0;
	tp->susp_rx = 0;
	tp->ttimer = 0;
	tp->nir = 0;

	for (i = 0; i < RX_RING_SIZE; i++) {
		tp->rx_ring[i].status = 0x00000000;
		tp->rx_ring[i].length = cpu_to_le32(PKT_BUF_SZ);
		tp->rx_ring[i].buffer2 = cpu_to_le32(tp->rx_ring_dma + sizeof(struct tulip_rx_desc) * (i + 1));
		tp->rx_buffers[i].skb = NULL;
		tp->rx_buffers[i].mapping = 0;
	}
	/* Mark the last entry as wrapping the ring. */
	tp->rx_ring[i-1].length = cpu_to_le32(PKT_BUF_SZ | DESC_RING_WRAP);
	tp->rx_ring[i-1].buffer2 = cpu_to_le32(tp->rx_ring_dma);

	for (i = 0; i < RX_RING_SIZE; i++) {
		dma_addr_t mapping;

		/* Note the receive buffer must be longword aligned.
		   dev_alloc_skb() provides 16 byte alignment.  But do *not*
		   use skb_reserve() to align the IP header! */
		struct sk_buff *skb = dev_alloc_skb(PKT_BUF_SZ);
		tp->rx_buffers[i].skb = skb;
		if (skb == NULL)
			break;
		mapping = pci_map_single(tp->pdev, skb->tail,
					 PKT_BUF_SZ, PCI_DMA_FROMDEVICE);
		tp->rx_buffers[i].mapping = mapping;
		skb->dev = dev;			/* Mark as being used by this device. */
		tp->rx_ring[i].status = cpu_to_le32(DescOwned);	/* Owned by Tulip chip */
		tp->rx_ring[i].buffer1 = cpu_to_le32(mapping);
	}
	tp->dirty_rx = (unsigned int)(i - RX_RING_SIZE);

	/* The Tx buffer descriptor is filled in as needed, but we
	   do need to clear the ownership bit. */
	for (i = 0; i < TX_RING_SIZE; i++) {
		tp->tx_buffers[i].skb = NULL;
		tp->tx_buffers[i].mapping = 0;
		tp->tx_ring[i].status = 0x00000000;
		tp->tx_ring[i].buffer2 = cpu_to_le32(tp->tx_ring_dma + sizeof(struct tulip_tx_desc) * (i + 1));
	}
	tp->tx_ring[i-1].buffer2 = cpu_to_le32(tp->tx_ring_dma);
}

static int
tulip_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	int entry;
	u32 flag;
	dma_addr_t mapping;

	spin_lock_irq(&tp->lock);

	/* Calculate the next Tx descriptor entry. */
	entry = tp->cur_tx % TX_RING_SIZE;

	tp->tx_buffers[entry].skb = skb;
	mapping = pci_map_single(tp->pdev, skb->data,
				 skb->len, PCI_DMA_TODEVICE);
	tp->tx_buffers[entry].mapping = mapping;
	tp->tx_ring[entry].buffer1 = cpu_to_le32(mapping);

	if (tp->cur_tx - tp->dirty_tx < TX_RING_SIZE/2) {/* Typical path */
		flag = 0x60000000; /* No interrupt */
	} else if (tp->cur_tx - tp->dirty_tx == TX_RING_SIZE/2) {
		flag = 0xe0000000; /* Tx-done intr. */
	} else if (tp->cur_tx - tp->dirty_tx < TX_RING_SIZE - 2) {
		flag = 0x60000000; /* No Tx-done intr. */
	} else {		/* Leave room for set_rx_mode() to fill entries. */
		flag = 0xe0000000; /* Tx-done intr. */
		netif_stop_queue(dev);
	}
	if (entry == TX_RING_SIZE-1)
		flag = 0xe0000000 | DESC_RING_WRAP;

	tp->tx_ring[entry].length = cpu_to_le32(skb->len | flag);
	/* if we were using Transmit Automatic Polling, we would need a
	 * wmb() here. */
	tp->tx_ring[entry].status = cpu_to_le32(DescOwned);
	wmb();

	tp->cur_tx++;

	/* Trigger an immediate transmit demand. */
	outl(0, dev->base_addr + CSR1);

	spin_unlock_irq(&tp->lock);

	dev->trans_start = jiffies;

	return 0;
}

static void tulip_down (struct net_device *dev)
{
	long ioaddr = dev->base_addr;
	struct tulip_private *tp = (struct tulip_private *) dev->priv;
	unsigned long flags;

	del_timer_sync (&tp->timer);

	spin_lock_irqsave (&tp->lock, flags);

	/* Disable interrupts by clearing the interrupt mask. */
	outl (0x00000000, ioaddr + CSR7);

	/* Stop the Tx and Rx processes. */
	tulip_stop_rxtx(tp, inl(ioaddr + CSR6));

	/* 21040 -- Leave the card in 10baseT state. */
	if (tp->chip_id == DC21040)
		outl (0x00000004, ioaddr + CSR13);

	if (inl (ioaddr + CSR6) != 0xffffffff)
		tp->stats.rx_missed_errors += inl (ioaddr + CSR8) & 0xffff;

	spin_unlock_irqrestore (&tp->lock, flags);

	dev->if_port = tp->saved_if_port;

	/* Leave the driver in snooze, not sleep, mode. */
	if (tp->flags & HAS_ACPI)
		pci_write_config_dword (tp->pdev, 0x40, 0x40000000);
}


static int tulip_close (struct net_device *dev)
{
	long ioaddr = dev->base_addr;
	struct tulip_private *tp = (struct tulip_private *) dev->priv;
	int i;

	netif_stop_queue (dev);

	tulip_down (dev);

	if (tulip_debug > 1)
		printk (KERN_DEBUG "%s: Shutting down ethercard, status was %2.2x.\n",
			dev->name, inl (ioaddr + CSR5));

	free_irq (dev->irq, dev);

	/* Free all the skbuffs in the Rx queue. */
	for (i = 0; i < RX_RING_SIZE; i++) {
		struct sk_buff *skb = tp->rx_buffers[i].skb;
		dma_addr_t mapping = tp->rx_buffers[i].mapping;

		tp->rx_buffers[i].skb = NULL;
		tp->rx_buffers[i].mapping = 0;

		tp->rx_ring[i].status = 0;	/* Not owned by Tulip chip. */
		tp->rx_ring[i].length = 0;
		tp->rx_ring[i].buffer1 = 0xBADF00D0;	/* An invalid address. */
		if (skb) {
			pci_unmap_single(tp->pdev, mapping, PKT_BUF_SZ,
					 PCI_DMA_FROMDEVICE);
			dev_kfree_skb (skb);
		}
	}
	for (i = 0; i < TX_RING_SIZE; i++) {
		struct sk_buff *skb = tp->tx_buffers[i].skb;

		if (skb != NULL) {
			pci_unmap_single(tp->pdev, tp->tx_buffers[i].mapping,
					 skb->len, PCI_DMA_TODEVICE);
			dev_kfree_skb (skb);
		}
		tp->tx_buffers[i].skb = NULL;
		tp->tx_buffers[i].mapping = 0;
	}

	MOD_DEC_USE_COUNT;

	return 0;
}

static struct net_device_stats *tulip_get_stats(struct net_device *dev)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	long ioaddr = dev->base_addr;

	if (netif_running(dev)) {
		unsigned long flags;

		spin_lock_irqsave (&tp->lock, flags);

		tp->stats.rx_missed_errors += inl(ioaddr + CSR8) & 0xffff;

		spin_unlock_irqrestore(&tp->lock, flags);
	}

	return &tp->stats;
}


/* Provide ioctl() calls to examine the MII xcvr state. */
static int private_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	long ioaddr = dev->base_addr;
	u16 *data = (u16 *)&rq->ifr_data;
	int phy = tp->phys[0] & 0x1f;
	long flags;

	switch(cmd) {
	case SIOCDEVPRIVATE:		/* Get the address of the PHY in use. */
		if (tp->mii_cnt)
			data[0] = phy;
		else if (tp->flags & HAS_NWAY)
			data[0] = 32;
		else if (tp->chip_id == COMET)
			data[0] = 1;
		else
			return -ENODEV;
	case SIOCDEVPRIVATE+1:		/* Read the specified MII register. */
		if (data[0] == 32  &&  (tp->flags & HAS_NWAY)) {
			int csr12 = inl(ioaddr + CSR12);
			int csr14 = inl(ioaddr + CSR14);
			switch (data[1]) {
			case 0: {
				data[3] = (csr14<<5) & 0x1000;
				break; }
			case 1:
				data[3] = 0x7848 + ((csr12&0x7000) == 0x5000 ? 0x20 : 0)
					+ (csr12&0x06 ? 0x04 : 0);
				break;
			case 4: {
				data[3] = ((csr14>>9)&0x07C0) +
					((inl(ioaddr + CSR6)>>3)&0x0040) + ((csr14>>1)&0x20) + 1;
				break;
			}
			case 5: data[3] = csr12 >> 16; break;
			default: data[3] = 0; break;
			}
		} else {
			spin_lock_irqsave (&tp->lock, flags);
			data[3] = tulip_mdio_read(dev, data[0] & 0x1f, data[1] & 0x1f);
			spin_unlock_irqrestore (&tp->lock, flags);
		}
		return 0;
	case SIOCDEVPRIVATE+2:		/* Write the specified MII register */
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		if (data[0] == 32  &&  (tp->flags & HAS_NWAY)) {
			if (data[1] == 5)
				tp->to_advertise = data[2];
		} else {
			spin_lock_irqsave (&tp->lock, flags);
			tulip_mdio_write(dev, data[0] & 0x1f, data[1] & 0x1f, data[2]);
			spin_unlock_irqrestore(&tp->lock, flags);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}

	return -EOPNOTSUPP;
}


/* Set or clear the multicast filter for this adaptor.
   Note that we only use exclusion around actually queueing the
   new frame, not around filling tp->setup_frame.  This is non-deterministic
   when re-entered but still correct. */

/* The little-endian AUTODIN32 ethernet CRC calculation.
   N.B. Do not use for bulk data, use a table-based routine instead.
   This is common code and should be moved to net/core/crc.c */
static unsigned const ethernet_polynomial_le = 0xedb88320U;
static inline u32 ether_crc_le(int length, unsigned char *data)
{
	u32 crc = 0xffffffff;	/* Initial value. */
	while(--length >= 0) {
		unsigned char current_octet = *data++;
		int bit;
		for (bit = 8; --bit >= 0; current_octet >>= 1) {
			if ((crc ^ current_octet) & 1) {
				crc >>= 1;
				crc ^= ethernet_polynomial_le;
			} else
				crc >>= 1;
		}
	}
	return crc;
}
static unsigned const ethernet_polynomial = 0x04c11db7U;
static inline u32 ether_crc(int length, unsigned char *data)
{
    int crc = -1;

    while(--length >= 0) {
		unsigned char current_octet = *data++;
		int bit;
		for (bit = 0; bit < 8; bit++, current_octet >>= 1)
			crc = (crc << 1) ^
				((crc < 0) ^ (current_octet & 1) ? ethernet_polynomial : 0);
    }
    return crc;
}

#undef set_bit_le
#define set_bit_le(i,p) do { ((char *)(p))[(i)/8] |= (1<<((i)%8)); } while(0)

static void build_setup_frame_hash(u16 *setup_frm, struct net_device *dev)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	u16 hash_table[32];
	struct dev_mc_list *mclist;
	int i;
	u16 *eaddrs;

	memset(hash_table, 0, sizeof(hash_table));
	set_bit_le(255, hash_table); 			/* Broadcast entry */
	/* This should work on big-endian machines as well. */
	for (i = 0, mclist = dev->mc_list; mclist && i < dev->mc_count;
	     i++, mclist = mclist->next) {
		int index = ether_crc_le(ETH_ALEN, mclist->dmi_addr) & 0x1ff;

		set_bit_le(index, hash_table);

		for (i = 0; i < 32; i++) {
			*setup_frm++ = hash_table[i];
			*setup_frm++ = hash_table[i];
		}
		setup_frm = &tp->setup_frame[13*6];
	}

	/* Fill the final entry with our physical address. */
	eaddrs = (u16 *)dev->dev_addr;
	*setup_frm++ = eaddrs[0]; *setup_frm++ = eaddrs[0];
	*setup_frm++ = eaddrs[1]; *setup_frm++ = eaddrs[1];
	*setup_frm++ = eaddrs[2]; *setup_frm++ = eaddrs[2];
}

static void build_setup_frame_perfect(u16 *setup_frm, struct net_device *dev)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	struct dev_mc_list *mclist;
	int i;
	u16 *eaddrs;

	/* We have <= 14 addresses so we can use the wonderful
	   16 address perfect filtering of the Tulip. */
	for (i = 0, mclist = dev->mc_list; i < dev->mc_count;
	     i++, mclist = mclist->next) {
		eaddrs = (u16 *)mclist->dmi_addr;
		*setup_frm++ = *eaddrs; *setup_frm++ = *eaddrs++;
		*setup_frm++ = *eaddrs; *setup_frm++ = *eaddrs++;
		*setup_frm++ = *eaddrs; *setup_frm++ = *eaddrs++;
	}
	/* Fill the unused entries with the broadcast address. */
	memset(setup_frm, 0xff, (15-i)*12);
	setup_frm = &tp->setup_frame[15*6];

	/* Fill the final entry with our physical address. */
	eaddrs = (u16 *)dev->dev_addr;
	*setup_frm++ = eaddrs[0]; *setup_frm++ = eaddrs[0];
	*setup_frm++ = eaddrs[1]; *setup_frm++ = eaddrs[1];
	*setup_frm++ = eaddrs[2]; *setup_frm++ = eaddrs[2];
}


static void set_rx_mode(struct net_device *dev)
{
	struct tulip_private *tp = (struct tulip_private *)dev->priv;
	long ioaddr = dev->base_addr;
	int csr6;

	DPRINTK("ENTER\n");

	csr6 = inl(ioaddr + CSR6) & ~0x00D5;

	tp->csr6 &= ~0x00D5;
	if (dev->flags & IFF_PROMISC) {			/* Set promiscuous. */
		tp->csr6 |= 0x00C0;
		csr6 |= 0x00C0;
		/* Unconditionally log net taps. */
		printk(KERN_INFO "%s: Promiscuous mode enabled.\n", dev->name);
	} else if ((dev->mc_count > 1000)  ||  (dev->flags & IFF_ALLMULTI)) {
		/* Too many to filter well -- accept all multicasts. */
		tp->csr6 |= 0x0080;
		csr6 |= 0x0080;
	} else	if (tp->flags & MC_HASH_ONLY) {
		/* Some work-alikes have only a 64-entry hash filter table. */
		/* Should verify correctness on big-endian/__powerpc__ */
		struct dev_mc_list *mclist;
		int i;
		u32 mc_filter[2];		 /* Multicast hash filter */
		if (dev->mc_count > 64) {		/* Arbitrary non-effective limit. */
			tp->csr6 |= 0x0080;
			csr6 |= 0x0080;
		} else {
			mc_filter[1] = mc_filter[0] = 0;
			for (i = 0, mclist = dev->mc_list; mclist && i < dev->mc_count;
				 i++, mclist = mclist->next)
				set_bit(ether_crc(ETH_ALEN, mclist->dmi_addr)>>26, mc_filter);

			if (tp->chip_id == AX88140) {
				outl(2, ioaddr + CSR13);
				outl(mc_filter[0], ioaddr + CSR14);
				outl(3, ioaddr + CSR13);
				outl(mc_filter[1], ioaddr + CSR14);
			} else if (tp->chip_id == COMET) { /* Has a simple hash filter. */
				outl(mc_filter[0], ioaddr + 0xAC);
				outl(mc_filter[1], ioaddr + 0xB0);
			}
		}
	} else {
		unsigned long flags;

		/* Note that only the low-address shortword of setup_frame is valid!
		   The values are doubled for big-endian architectures. */
		if (dev->mc_count > 14) { /* Must use a multicast hash table. */
			build_setup_frame_hash(tp->setup_frame, dev);
		} else {
			build_setup_frame_perfect(tp->setup_frame, dev);
		}

		spin_lock_irqsave(&tp->lock, flags);

		if (tp->cur_tx - tp->dirty_tx > TX_RING_SIZE - 2) {
			/* Same setup recently queued, we need not add it. */
		} else {
			u32 tx_flags = 0x08000000 | 192;
			unsigned int entry;
			int dummy = -1;

			/* Now add this frame to the Tx list. */

			entry = tp->cur_tx++ % TX_RING_SIZE;

			if (entry != 0) {
				/* Avoid a chip errata by prefixing a dummy entry. */
				tp->tx_buffers[entry].skb = NULL;
				tp->tx_buffers[entry].mapping = 0;
				tp->tx_ring[entry].length =
					(entry == TX_RING_SIZE-1) ? cpu_to_le32(DESC_RING_WRAP) : 0;
				tp->tx_ring[entry].buffer1 = 0;
				/* Must set DescOwned later to avoid race with chip */
				dummy = entry;
				entry = tp->cur_tx++ % TX_RING_SIZE;
			}

			tp->tx_buffers[entry].skb = NULL;
			tp->tx_buffers[entry].mapping =
				pci_map_single(tp->pdev, tp->setup_frame,
					       sizeof(tp->setup_frame),
					       PCI_DMA_TODEVICE);
			/* Put the setup frame on the Tx list. */
			if (entry == TX_RING_SIZE-1)
				tx_flags |= DESC_RING_WRAP;		/* Wrap ring. */
			tp->tx_ring[entry].length = cpu_to_le32(tx_flags);
			tp->tx_ring[entry].buffer1 =
				cpu_to_le32(tp->tx_buffers[entry].mapping);
			tp->tx_ring[entry].status = cpu_to_le32(DescOwned);
			if (dummy >= 0)
				tp->tx_ring[dummy].status = cpu_to_le32(DescOwned);
			if (tp->cur_tx - tp->dirty_tx >= TX_RING_SIZE - 2)
				netif_stop_queue(dev);

			/* Trigger an immediate transmit demand. */
			outl(0, ioaddr + CSR1);
		}

		spin_unlock_irqrestore(&tp->lock, flags);
	}

	/* Can someone explain to me what the OR here is supposed to accomplish???? */
	tulip_outl_csr(tp, csr6 | 0x0000, CSR6);
}


static int __devinit tulip_init_one (struct pci_dev *pdev,
				     const struct pci_device_id *ent)
{
	static int did_version;			/* Already printed version info. */
	struct tulip_private *tp;
	/* See note below on the multiport cards. */
	static unsigned char last_phys_addr[6] = {0x00, 'L', 'i', 'n', 'u', 'x'};
	static int last_irq;
	static int multiport_cnt;		/* For four-port boards w/one EEPROM */
	u8 chip_rev;
	int i, irq;
	unsigned short sum;
	u8 ee_data[EEPROM_SIZE];
	struct net_device *dev;
	long ioaddr;
	static int board_idx = -1;
	int chip_idx = ent->driver_data;

	board_idx++;

	if (tulip_debug > 0  &&  did_version++ == 0)
		printk (KERN_INFO "%s", version);

	/*
	 *	Lan media wire a tulip chip to a wan interface. Needs a very
	 *	different driver (lmc driver)
	 */
	 
        if( pdev->subsystem_vendor == 0x1376 ){
		printk (KERN_ERR PFX "skipping LMC card.\n");
		return -ENODEV;
	}
	
	/*
	 *	Early DM9100's need software CRC and the DMFE driver
	 */
	 
	if (pdev->vendor == 0x1282 && pdev->device == 0x9100)
	{
		u32 dev_rev;
		/* Read Chip revision */
		pci_read_config_dword(pdev, PCI_REVISION_ID, &dev_rev);
		if(dev_rev < 0x02000030)
		{
			printk(KERN_ERR PFX "skipping early DM9100 with Crc bug (use dmfe)\n");
			return -ENODEV;
		}
	}
		
	/*
	 *	Looks for early PCI chipsets where people report hangs 
	 *	without the workarounds being on.
	 */
	 
	/* Intel Saturn. Switch to 8 long words burst, 8 long word cache aligned 
	   Aries might need this too. The Saturn errata are not pretty reading but
	   thankfully its an old 486 chipset.
	*/
	
	if (pci_find_device(PCI_VENDOR_ID_INTEL, 0x0483, NULL))
		csr0 = 0x00A04800;
	/* The dreaded SiS496 486 chipset. Same workaround as above. */
	if (pci_find_device(PCI_VENDOR_ID_SI, 0x0496, NULL))
		csr0 = 0x00A04800;
		
	/*
	 *	And back to business
	 */
 
	ioaddr = pci_resource_start (pdev, 0);
	irq = pdev->irq;

	/* init_etherdev ensures aligned and zeroed private structures */
	dev = init_etherdev (NULL, sizeof (*tp));
	if (!dev) {
		printk (KERN_ERR PFX "ether device alloc failed, aborting\n");
		return -ENOMEM;
	}

	if (pci_resource_len (pdev, 0) < tulip_tbl[chip_idx].io_size) {
		printk (KERN_ERR PFX "%s: I/O region (0x%lx@0x%lx) too small, "
			"aborting\n", dev->name, pci_resource_len (pdev, 0),
			pci_resource_start (pdev, 0));
		goto err_out_free_netdev;
	}

	/* grab all resources from both PIO and MMIO regions, as we
	 * don't want anyone else messing around with our hardware */
	if (!request_region (pci_resource_start (pdev, 0),
			     pci_resource_len (pdev, 0),
			     dev->name)) {
		printk (KERN_ERR PFX "%s: I/O region (0x%lx@0x%lx) unavailable, "
			"aborting\n", dev->name, pci_resource_len (pdev, 0),
			pci_resource_start (pdev, 0));
		goto err_out_free_netdev;
	}
	if (!request_mem_region (pci_resource_start (pdev, 1),
				 pci_resource_len (pdev, 1),
				 dev->name)) {
		printk (KERN_ERR PFX "%s: MMIO region (0x%lx@0x%lx) unavailable, "
			"aborting\n", dev->name, pci_resource_len (pdev, 1),
			pci_resource_start (pdev, 1));
		goto err_out_free_pio_res;
	}

	if (pci_enable_device(pdev)) {
		printk (KERN_ERR PFX "%s: Cannot enable PCI device, aborting\n",
			dev->name);
		goto err_out_free_mmio_res;
	}

	pci_set_master(pdev);

	pci_read_config_byte (pdev, PCI_REVISION_ID, &chip_rev);

	/*
	 * initialize priviate data structure 'tp'
	 * it is zeroed and aligned in init_etherdev
	 */
	tp = dev->priv;

	tp->rx_ring = pci_alloc_consistent(pdev,
					   sizeof(struct tulip_rx_desc) * RX_RING_SIZE +
					   sizeof(struct tulip_tx_desc) * TX_RING_SIZE,
					   &tp->rx_ring_dma);
	if (!tp->rx_ring)
		goto err_out_free_mmio_res;
	tp->tx_ring = (struct tulip_tx_desc *)(tp->rx_ring + RX_RING_SIZE);
	tp->tx_ring_dma = tp->rx_ring_dma + sizeof(struct tulip_rx_desc) * RX_RING_SIZE;

	tp->chip_id = chip_idx;
	tp->flags = tulip_tbl[chip_idx].flags;
	tp->pdev = pdev;
	tp->base_addr = ioaddr;
	tp->revision = chip_rev;
	tp->csr0 = csr0;
	spin_lock_init(&tp->lock);

	dev->base_addr = ioaddr;
	dev->irq = irq;
	pci_set_drvdata(pdev, dev);

#ifdef TULIP_FULL_DUPLEX
	tp->full_duplex = 1;
	tp->full_duplex_lock = 1;
#endif
#ifdef TULIP_DEFAULT_MEDIA
	tp->default_port = TULIP_DEFAULT_MEDIA;
#endif
#ifdef TULIP_NO_MEDIA_SWITCH
	tp->medialock = 1;
#endif

	printk(KERN_INFO "%s: %s rev %d at %#3lx,",
		   dev->name, tulip_tbl[chip_idx].chip_name, chip_rev, ioaddr);

	/* bugfix: the ASIX must have a burst limit or horrible things happen. */
	if (chip_idx == AX88140) {
		if ((tp->csr0 & 0x3f00) == 0)
			tp->csr0 |= 0x2000;
	}
	else if (chip_idx == DC21143  &&  chip_rev == 65)
		tp->csr0 &= ~0x01000000;

	/* Stop the chip's Tx and Rx processes. */
	tulip_stop_rxtx(tp, inl(ioaddr + CSR6));

	/* Clear the missed-packet counter. */
	inl(ioaddr + CSR8);

	if (chip_idx == DC21041) {
		if (inl(ioaddr + CSR9) & 0x8000) {
			printk(" 21040 compatible mode,");
			chip_idx = DC21040;
		} else {
			printk(" 21041 mode,");
		}
	}

	/* The station address ROM is read byte serially.  The register must
	   be polled, waiting for the value to be read bit serially from the
	   EEPROM.
	   */
	sum = 0;
	if (chip_idx == DC21040) {
		outl(0, ioaddr + CSR9);		/* Reset the pointer with a dummy write. */
		for (i = 0; i < 6; i++) {
			int value, boguscnt = 100000;
			do
				value = inl(ioaddr + CSR9);
			while (value < 0  && --boguscnt > 0);
			dev->dev_addr[i] = value;
			sum += value & 0xff;
		}
	} else if (chip_idx == LC82C168) {
		for (i = 0; i < 3; i++) {
			int value, boguscnt = 100000;
			outl(0x600 | i, ioaddr + 0x98);
			do
				value = inl(ioaddr + CSR9);
			while (value < 0  && --boguscnt > 0);
			put_unaligned(le16_to_cpu(value), ((u16*)dev->dev_addr) + i);
			sum += value & 0xffff;
		}
	} else if (chip_idx == COMET) {
		/* No need to read the EEPROM. */
		put_unaligned(inl(ioaddr + 0xA4), (u32 *)dev->dev_addr);
		put_unaligned(inl(ioaddr + 0xA8), (u16 *)(dev->dev_addr + 4));
		for (i = 0; i < 6; i ++)
			sum += dev->dev_addr[i];
	} else {
		/* A serial EEPROM interface, we read now and sort it out later. */
		int sa_offset = 0;
		int ee_addr_size = tulip_read_eeprom(ioaddr, 0xff, 8) & 0x40000 ? 8 : 6;

		for (i = 0; i < sizeof(ee_data)/2; i++)
			((u16 *)ee_data)[i] =
				le16_to_cpu(tulip_read_eeprom(ioaddr, i, ee_addr_size));

		/* DEC now has a specification (see Notes) but early board makers
		   just put the address in the first EEPROM locations. */
		/* This does  memcmp(eedata, eedata+16, 8) */
		for (i = 0; i < 8; i ++)
			if (ee_data[i] != ee_data[16+i])
				sa_offset = 20;
		if (ee_data[0] == 0xff  &&  ee_data[1] == 0xff &&  ee_data[2] == 0) {
			sa_offset = 2;		/* Grrr, damn Matrox boards. */
			multiport_cnt = 4;
		}
		for (i = 0; i < 6; i ++) {
			dev->dev_addr[i] = ee_data[i + sa_offset];
			sum += ee_data[i + sa_offset];
		}
	}
	/* Lite-On boards have the address byte-swapped. */
	if ((dev->dev_addr[0] == 0xA0  ||  dev->dev_addr[0] == 0xC0)
		&&  dev->dev_addr[1] == 0x00)
		for (i = 0; i < 6; i+=2) {
			char tmp = dev->dev_addr[i];
			dev->dev_addr[i] = dev->dev_addr[i+1];
			dev->dev_addr[i+1] = tmp;
		}
	/* On the Zynx 315 Etherarray and other multiport boards only the
	   first Tulip has an EEPROM.
	   The addresses of the subsequent ports are derived from the first.
	   Many PCI BIOSes also incorrectly report the IRQ line, so we correct
	   that here as well. */
	if (sum == 0  || sum == 6*0xff) {
		printk(" EEPROM not present,");
		for (i = 0; i < 5; i++)
			dev->dev_addr[i] = last_phys_addr[i];
		dev->dev_addr[i] = last_phys_addr[i] + 1;
#if defined(__i386__)		/* Patch up x86 BIOS bug. */
		if (last_irq)
			irq = last_irq;
#endif
	}

	for (i = 0; i < 6; i++)
		printk("%c%2.2X", i ? ':' : ' ', last_phys_addr[i] = dev->dev_addr[i]);
	printk(", IRQ %d.\n", irq);
	last_irq = irq;

	/* The lower four bits are the media type. */
	if (board_idx >= 0  &&  board_idx < MAX_UNITS) {
		tp->default_port = options[board_idx] & 15;
		if ((options[board_idx] & 0x90) || full_duplex[board_idx] > 0)
			tp->full_duplex = 1;
		if (mtu[board_idx] > 0)
			dev->mtu = mtu[board_idx];
	}
	if (dev->mem_start)
		tp->default_port = dev->mem_start;
	if (tp->default_port) {
		tp->medialock = 1;
		if (tulip_media_cap[tp->default_port] & MediaAlwaysFD)
			tp->full_duplex = 1;
	}
	if (tp->full_duplex)
		tp->full_duplex_lock = 1;

	if (tulip_media_cap[tp->default_port] & MediaIsMII) {
		u16 media2advert[] = { 0x20, 0x40, 0x03e0, 0x60, 0x80, 0x100, 0x200 };
		tp->to_advertise = media2advert[tp->default_port - 9];
	} else if (tp->flags & HAS_8023X)
		tp->to_advertise = 0x05e1;
	else
		tp->to_advertise = 0x01e1;

	/* This is logically part of _init_one(), but too complex to write inline. */
	if (tp->flags & HAS_MEDIA_TABLE) {
		memcpy(tp->eeprom, ee_data, sizeof(tp->eeprom));
		tulip_parse_eeprom(dev);
	}

	if ((tp->flags & ALWAYS_CHECK_MII) ||
		(tp->mtable  &&  tp->mtable->has_mii) ||
		( ! tp->mtable  &&  (tp->flags & HAS_MII))) {
		int phy, phy_idx;
		if (tp->mtable  &&  tp->mtable->has_mii) {
			for (i = 0; i < tp->mtable->leafcount; i++)
				if (tp->mtable->mleaf[i].media == 11) {
					tp->cur_index = i;
					tp->saved_if_port = dev->if_port;
					tulip_select_media(dev, 1);
					dev->if_port = tp->saved_if_port;
					break;
				}
		}
		/* Find the connected MII xcvrs.
		   Doing this in open() would allow detecting external xcvrs later,
		   but takes much time. */
		for (phy = 0, phy_idx = 0; phy < 32 && phy_idx < sizeof(tp->phys);
			 phy++) {
			int mii_status = tulip_mdio_read(dev, phy, 1);
			if ((mii_status & 0x8301) == 0x8001 ||
				((mii_status & 0x8000) == 0  && (mii_status & 0x7800) != 0)) {
				int mii_reg0 = tulip_mdio_read(dev, phy, 0);
				int mii_advert = tulip_mdio_read(dev, phy, 4);
				int reg4 = ((mii_status>>6) & tp->to_advertise) | 1;
				tp->phys[phy_idx] = phy;
				tp->advertising[phy_idx++] = reg4;
				printk(KERN_INFO "%s:  MII transceiver #%d "
					   "config %4.4x status %4.4x advertising %4.4x.\n",
					   dev->name, phy, mii_reg0, mii_status, mii_advert);
				/* Fixup for DLink with miswired PHY. */
				if (mii_advert != reg4) {
					printk(KERN_DEBUG "%s:  Advertising %4.4x on PHY %d,"
						   " previously advertising %4.4x.\n",
						   dev->name, reg4, phy, mii_advert);
					printk(KERN_DEBUG "%s:  Advertising %4.4x (to advertise"
						   " is %4.4x).\n",
						   dev->name, reg4, tp->to_advertise);
					tulip_mdio_write(dev, phy, 4, reg4);
				}
				/* Enable autonegotiation: some boards default to off. */
				tulip_mdio_write(dev, phy, 0, mii_reg0 |
						   (tp->full_duplex ? 0x1100 : 0x1000) |
						   (tulip_media_cap[tp->default_port]&MediaIs100 ? 0x2000:0));
			}
		}
		tp->mii_cnt = phy_idx;
		if (tp->mtable  &&  tp->mtable->has_mii  &&  phy_idx == 0) {
			printk(KERN_INFO "%s: ***WARNING***: No MII transceiver found!\n",
				   dev->name);
			tp->phys[0] = 1;
		}
	}

	/* The Tulip-specific entries in the device structure. */
	dev->open = tulip_open;
	dev->hard_start_xmit = tulip_start_xmit;
	dev->tx_timeout = tulip_tx_timeout;
	dev->watchdog_timeo = TX_TIMEOUT;
	dev->stop = tulip_close;
	dev->get_stats = tulip_get_stats;
	dev->do_ioctl = private_ioctl;
	dev->set_multicast_list = set_rx_mode;

	if ((tp->flags & HAS_NWAY)  || tp->chip_id == DC21041)
		tp->link_change = t21142_lnk_change;
	else if (tp->flags & HAS_PNICNWAY)
		tp->link_change = pnic_lnk_change;

	/* Reset the xcvr interface and turn on heartbeat. */
	switch (chip_idx) {
	case DC21041:
		tp->to_advertise = 0x0061;
		outl(0x00000000, ioaddr + CSR13);
		outl(0xFFFFFFFF, ioaddr + CSR14);
		outl(0x00000008, ioaddr + CSR15); /* Listen on AUI also. */
		tulip_outl_csr(tp, inl(ioaddr + CSR6) | csr6_fd, CSR6);
		outl(0x0000EF05, ioaddr + CSR13);
		break;
	case DC21040:
		outl(0x00000000, ioaddr + CSR13);
		outl(0x00000004, ioaddr + CSR13);
		break;
	case DC21140:
	case DM910X:
	default:
		if (tp->mtable)
			outl(tp->mtable->csr12dir | 0x100, ioaddr + CSR12);
		break;
	case DC21142:
	case PNIC2:
		if (tp->mii_cnt  ||  tulip_media_cap[dev->if_port] & MediaIsMII) {
			tulip_outl_csr(tp, csr6_mask_defstate, CSR6);
			outl(0x0000, ioaddr + CSR13);
			outl(0x0000, ioaddr + CSR14);
			tulip_outl_csr(tp, csr6_mask_hdcap, CSR6);
		} else
			t21142_start_nway(dev);
		break;
	case LC82C168:
		if ( ! tp->mii_cnt) {
			tp->nway = 1;
			tp->nwayset = 0;
			tulip_outl_csr(tp, csr6_ttm | csr6_ca, CSR6);
			outl(0x30, ioaddr + CSR12);
			tulip_outl_csr(tp, 0x0001F078, CSR6);
			tulip_outl_csr(tp, 0x0201F078, CSR6); /* Turn on autonegotiation. */
		}
		break;
	case MX98713:
	case COMPEX9881:
		tulip_outl_csr(tp, 0x00000000, CSR6);
		outl(0x000711C0, ioaddr + CSR14); /* Turn on NWay. */
		outl(0x00000001, ioaddr + CSR13);
		break;
	case MX98715:
	case MX98725:
		tulip_outl_csr(tp, 0x01a80000, CSR6);
		outl(0xFFFFFFFF, ioaddr + CSR14);
		outl(0x00001000, ioaddr + CSR12);
		break;
	case COMET:
		/* No initialization necessary. */
		break;
	}

	/* put the chip in snooze mode until opened */
	if (tulip_tbl[chip_idx].flags & HAS_ACPI)
		pci_write_config_dword(pdev, 0x40, 0x40000000);

	return 0;

err_out_free_mmio_res:
	release_mem_region (pci_resource_start (pdev, 1),
			    pci_resource_len (pdev, 1));
err_out_free_pio_res:
	release_region (pci_resource_start (pdev, 0),
			pci_resource_len (pdev, 0));
err_out_free_netdev:
	unregister_netdev (dev);
	kfree (dev);
	return -ENODEV;
}


static void tulip_suspend (struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);

	if (dev && netif_device_present (dev)) {
		netif_device_detach (dev);
		tulip_down (dev);
	}
//	pci_set_power_state(pdev, 3);
}


static void tulip_resume(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);

	pci_enable_device(pdev);
	if (dev && !netif_device_present (dev)) {
		tulip_up (dev);
		netif_device_attach (dev);
	}
}


static void __devexit tulip_remove_one (struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);

	if (dev) {
		struct tulip_private *tp = (struct tulip_private *)dev->priv;
		pci_free_consistent(pdev,
				    sizeof(struct tulip_rx_desc) * RX_RING_SIZE +
				    sizeof(struct tulip_tx_desc) * TX_RING_SIZE,
				    tp->rx_ring,
				    tp->rx_ring_dma);
		unregister_netdev(dev);
		release_mem_region (pci_resource_start (pdev, 1),
				    pci_resource_len (pdev, 1));
		release_region (pci_resource_start (pdev, 0),
				pci_resource_len (pdev, 0));
		kfree(dev);

		pci_set_drvdata(pdev, NULL);
	}
}


static struct pci_driver tulip_driver = {
	name:		TULIP_MODULE_NAME,
	id_table:	tulip_pci_tbl,
	probe:		tulip_init_one,
	remove:		tulip_remove_one,
	suspend:	tulip_suspend,
	resume:		tulip_resume,
};


static int __init tulip_init (void)
{
	/* copy module parms into globals */
	tulip_rx_copybreak = rx_copybreak;
	tulip_max_interrupt_work = max_interrupt_work;

	/* probe for and init boards */
	return pci_module_init (&tulip_driver);
}


static void __exit tulip_cleanup (void)
{
	pci_unregister_driver (&tulip_driver);
}


module_init(tulip_init);
module_exit(tulip_cleanup);
