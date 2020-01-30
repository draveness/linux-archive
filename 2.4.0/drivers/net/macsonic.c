/*
 * macsonic.c
 *
 * (C) 1998 Alan Cox
 *
 * Debugging Andreas Ehliar, Michael Schmitz
 *
 * Based on code
 * (C) 1996 by Thomas Bogendoerfer (tsbogend@bigbug.franken.de)
 * 
 * This driver is based on work from Andreas Busse, but most of
 * the code is rewritten.
 * 
 * (C) 1995 by Andreas Busse (andy@waldorf-gmbh.de)
 *
 * A driver for the Mac onboard Sonic ethernet chip.
 *
 * 98/12/21 MSch: judged from tests on Q800, it's basically working, 
 *		  but eating up both receive and transmit resources
 *		  and duplicating packets. Needs more testing.
 *
 * 99/01/03 MSch: upgraded to version 0.92 of the core driver, fixed.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/ctype.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/nubus.h>
#include <asm/bootinfo.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/pgtable.h>
#include <asm/segment.h>
#include <asm/io.h>
#include <asm/hwtest.h>
#include <asm/dma.h>
#include <asm/macintosh.h>
#include <asm/macints.h>
#include <asm/mac_via.h>

#include <linux/errno.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/module.h>

#define SREGS_PAD(n)    u16 n;

#include "sonic.h"

static int sonic_debug = 0;
static int sonic_version_printed = 0;

extern int macsonic_probe(struct net_device* dev);
extern int mac_onboard_sonic_probe(struct net_device* dev);
extern int mac_nubus_sonic_probe(struct net_device* dev);

/* For onboard SONIC */
#define ONBOARD_SONIC_REGISTERS	0x50F0A000
#define ONBOARD_SONIC_PROM_BASE	0x50f08000

enum macsonic_type {
	MACSONIC_DUODOCK,
	MACSONIC_APPLE,
	MACSONIC_APPLE16,
	MACSONIC_DAYNA,
	MACSONIC_DAYNALINK
};

/* For the built-in SONIC in the Duo Dock */
#define DUODOCK_SONIC_REGISTERS 0xe10000
#define DUODOCK_SONIC_PROM_BASE 0xe12000

/* For Apple-style NuBus SONIC */
#define APPLE_SONIC_REGISTERS	0
#define APPLE_SONIC_PROM_BASE	0x40000

/* Daynalink LC SONIC */
#define DAYNALINK_PROM_BASE 0x400000

/* For Dayna-style NuBus SONIC (haven't seen one yet) */
#define DAYNA_SONIC_REGISTERS   0x180000
/* This is what OpenBSD says.  However, this is definitely in NuBus
   ROM space so we should be able to get it by walking the NuBus
   resource directories */
#define DAYNA_SONIC_MAC_ADDR	0xffe004

#define SONIC_READ_PROM(addr) readb(prom_addr+addr)

int __init macsonic_probe(struct net_device* dev)
{
	int rv;

	/* This will catch fatal stuff like -ENOMEM as well as success */
	if ((rv = mac_onboard_sonic_probe(dev)) != -ENODEV)
		return rv;
	return mac_nubus_sonic_probe(dev);
}

/*
 * For reversing the PROM address
 */

static unsigned char nibbletab[] = {0, 8, 4, 12, 2, 10, 6, 14,
				    1, 9, 5, 13, 3, 11, 7, 15};

static inline void bit_reverse_addr(unsigned char addr[6])
{
	int i;

	for(i = 0; i < 6; i++)
		addr[i] = ((nibbletab[addr[i] & 0xf] << 4) | 
			   nibbletab[(addr[i] >> 4) &0xf]);
}

int __init macsonic_init(struct net_device* dev)
{
	struct sonic_local* lp = (struct sonic_local *)dev->priv;
	int i;

	/* Allocate the entire chunk of memory for the descriptors.
           Note that this cannot cross a 64K boundary. */
	for (i = 0; i < 20; i++) {
		unsigned long desc_base, desc_top;
		if ((lp->sonic_desc = 
		     kmalloc(SIZEOF_SONIC_DESC
			     * SONIC_BUS_SCALE(lp->dma_bitmode), GFP_DMA)) == NULL) {
			printk(KERN_ERR "%s: couldn't allocate descriptor buffers\n", dev->name);
		}
		desc_base = (unsigned long) lp->sonic_desc;
		desc_top = desc_base + SIZEOF_SONIC_DESC * SONIC_BUS_SCALE(lp->dma_bitmode);
		if ((desc_top & 0xffff) >= (desc_base & 0xffff))
			break;
		/* Hmm. try again (FIXME: does this actually work?) */
		kfree(lp->sonic_desc);
		printk(KERN_DEBUG
		       "%s: didn't get continguous chunk [%08lx - %08lx], trying again\n",
		       dev->name, desc_base, desc_top);
	}

	if (lp->sonic_desc == NULL) {
		printk(KERN_ERR "%s: tried 20 times to allocate descriptor buffers, giving up.\n",
		       dev->name);
		return -ENOMEM;
	}		       

	/* Now set up the pointers to point to the appropriate places */
	lp->cda = lp->sonic_desc;
	lp->tda = lp->cda + (SIZEOF_SONIC_CDA * SONIC_BUS_SCALE(lp->dma_bitmode));
	lp->rda = lp->tda + (SIZEOF_SONIC_TD * SONIC_NUM_TDS
			     * SONIC_BUS_SCALE(lp->dma_bitmode));
	lp->rra = lp->rda + (SIZEOF_SONIC_RD * SONIC_NUM_RDS
			     * SONIC_BUS_SCALE(lp->dma_bitmode));

	/* FIXME, maybe we should use skbs */
	if ((lp->rba = (char *)
	     kmalloc(SONIC_NUM_RRS * SONIC_RBSIZE, GFP_DMA)) == NULL) {
		printk(KERN_ERR "%s: couldn't allocate receive buffers\n", dev->name);
		return -ENOMEM;
	}

	{
		int rs, ds;

		/* almost always 12*4096, but let's not take chances */
		rs = ((SONIC_NUM_RRS * SONIC_RBSIZE + 4095) / 4096) * 4096;
		/* almost always under a page, but let's not take chances */
		ds = ((SIZEOF_SONIC_DESC + 4095) / 4096) * 4096;
		kernel_set_cachemode(lp->rba, rs, IOMAP_NOCACHE_SER);
		kernel_set_cachemode(lp->sonic_desc, ds, IOMAP_NOCACHE_SER);
	}
	
#if 0
	flush_cache_all();
#endif

	dev->open = sonic_open;
	dev->stop = sonic_close;
	dev->hard_start_xmit = sonic_send_packet;
	dev->get_stats = sonic_get_stats;
	dev->set_multicast_list = &sonic_multicast_list;

	/*
	 * clear tally counter
	 */
	sonic_write(dev, SONIC_CRCT, 0xffff);
	sonic_write(dev, SONIC_FAET, 0xffff);
	sonic_write(dev, SONIC_MPT, 0xffff);

	/* Fill in the fields of the device structure with ethernet values. */
	ether_setup(dev);
	return 0;
}

int __init mac_onboard_sonic_ethernet_addr(struct net_device* dev)
{
	const int prom_addr = ONBOARD_SONIC_PROM_BASE;
	int i;

	/* On NuBus boards we can sometimes look in the ROM resources.
	   No such luck for comm-slot/onboard. */
	for(i = 0; i < 6; i++)
		dev->dev_addr[i] = SONIC_READ_PROM(i);

	/* Most of the time, the address is bit-reversed.  The NetBSD
	   source has a rather long and detailed historical account of
	   why this is so. */
	if (memcmp(dev->dev_addr, "\x08\x00\x07", 3) &&
	    memcmp(dev->dev_addr, "\x00\xA0\x40", 3) &&
	    memcmp(dev->dev_addr, "\x00\x05\x02", 3))
		bit_reverse_addr(dev->dev_addr);
	else
		return 0;

	/* If we still have what seems to be a bogus address, we'll
           look in the CAM.  The top entry should be ours. */
	/* Danger! This only works if MacOS has already initialized
           the card... */
	if (memcmp(dev->dev_addr, "\x08\x00\x07", 3) &&
	    memcmp(dev->dev_addr, "\x00\xA0\x40", 3) &&
	    memcmp(dev->dev_addr, "\x00\x05\x02", 3))
	{
		unsigned short val;

		printk(KERN_INFO "macsonic: PROM seems to be wrong, trying CAM entry 15\n");
		
		sonic_write(dev, SONIC_CMD, SONIC_CR_RST);
		sonic_write(dev, SONIC_CEP, 15);

		val = sonic_read(dev, SONIC_CAP2);
		dev->dev_addr[5] = val >> 8;
		dev->dev_addr[4] = val & 0xff;
		val = sonic_read(dev, SONIC_CAP1);
		dev->dev_addr[3] = val >> 8;
		dev->dev_addr[2] = val & 0xff;
		val = sonic_read(dev, SONIC_CAP0);
		dev->dev_addr[1] = val >> 8;
		dev->dev_addr[0] = val & 0xff;
		
		printk(KERN_INFO "HW Address from CAM 15: ");
		for (i = 0; i < 6; i++) {
			printk("%2.2x", dev->dev_addr[i]);
			if (i < 5)
				printk(":");
		}
		printk("\n");
	} else return 0;

	if (memcmp(dev->dev_addr, "\x08\x00\x07", 3) &&
	    memcmp(dev->dev_addr, "\x00\xA0\x40", 3) &&
	    memcmp(dev->dev_addr, "\x00\x05\x02", 3))
	{
		/*
		 * Still nonsense ... messed up someplace!
		 */
		printk(KERN_ERR "macsonic: ERROR (INVALID MAC)\n");
		return -EIO;
	} else return 0;
}

int __init mac_onboard_sonic_probe(struct net_device* dev)
{
	/* Bwahahaha */
	static int once_is_more_than_enough = 0;
	struct sonic_local* lp;
	int i;
	
	if (once_is_more_than_enough)
		return -ENODEV;
	once_is_more_than_enough = 1;

	if (!MACH_IS_MAC)
		return -ENODEV;

	printk(KERN_INFO "Checking for internal Macintosh ethernet (SONIC).. ");

	if (macintosh_config->ether_type != MAC_ETHER_SONIC)
	{
		printk("none.\n");
		return -ENODEV;
	}

	/* Bogus probing, on the models which may or may not have
	   Ethernet (BTW, the Ethernet *is* always at the same
	   address, and nothing else lives there, at least if Apple's
	   documentation is to be believed) */
	if (macintosh_config->ident == MAC_MODEL_Q630 ||
	    macintosh_config->ident == MAC_MODEL_P588 ||
	    macintosh_config->ident == MAC_MODEL_C610) {
		unsigned long flags;
		int card_present;

		save_flags(flags);
		cli();
		card_present = hwreg_present((void*)ONBOARD_SONIC_REGISTERS);
		restore_flags(flags);

		if (!card_present) {
			printk("none.\n");
			return -ENODEV;
		}
	}

	printk("yes\n");	

	if (dev) {
		dev = init_etherdev(dev, sizeof(struct sonic_local));
		/* methinks this will always be true but better safe than sorry */
		if (dev->priv == NULL)
			dev->priv = kmalloc(sizeof(struct sonic_local), GFP_KERNEL);
	} else {
		dev = init_etherdev(NULL, sizeof(struct sonic_local));
	}

	if (dev == NULL)
		return -ENOMEM;

	lp = (struct sonic_local*) dev->priv;
	memset(lp, 0, sizeof(struct sonic_local));
	/* Danger!  My arms are flailing wildly!  You *must* set this
           before using sonic_read() */

	dev->base_addr = ONBOARD_SONIC_REGISTERS;
	if (via_alt_mapping)
		dev->irq = IRQ_AUTO_3;
	else
		dev->irq = IRQ_NUBUS_9;

	if (!sonic_version_printed) {
		printk(KERN_INFO "%s", version);
		sonic_version_printed = 1;
	}
	printk(KERN_INFO "%s: onboard / comm-slot SONIC at 0x%08lx\n",
	       dev->name, dev->base_addr);

	/* Now do a song and dance routine in an attempt to determine
           the bus width */

	/* The PowerBook's SONIC is 16 bit always. */
	if (macintosh_config->ident == MAC_MODEL_PB520) {
		lp->reg_offset = 0;
		lp->dma_bitmode = 0;
	} else {
		/* Some of the comm-slot cards are 16 bit.  But some
                   of them are not.  The 32-bit cards use offset 2 and
                   pad with zeroes or sometimes ones (I think...)
                   Therefore, if we try offset 0 and get a silicon
                   revision of 0, we assume 16 bit. */
		int sr;

		/* Technically this is not necessary since we zeroed
                   it above */
		lp->reg_offset = 0;
		lp->dma_bitmode = 0;
		sr = sonic_read(dev, SONIC_SR);
		if (sr == 0 || sr == 0xffff) {
			lp->reg_offset = 2;
			/* 83932 is 0x0004, 83934 is 0x0100 or 0x0101 */
			sr = sonic_read(dev, SONIC_SR);
			lp->dma_bitmode = 1;
			
		}
		printk(KERN_INFO
		       "%s: revision 0x%04x, using %d bit DMA and register offset %d\n",
		       dev->name, sr, lp->dma_bitmode?32:16, lp->reg_offset);
	}


	/* Software reset, then initialize control registers. */
	sonic_write(dev, SONIC_CMD, SONIC_CR_RST);
	sonic_write(dev, SONIC_DCR, SONIC_DCR_BMS |
		    SONIC_DCR_RFT1 | SONIC_DCR_TFT0 | SONIC_DCR_EXBUS |
		    (lp->dma_bitmode ? SONIC_DCR_DW : 0));
	/* This *must* be written back to in order to restore the
           extended programmable output bits */
	sonic_write(dev, SONIC_DCR2, 0);

	/* Clear *and* disable interrupts to be on the safe side */
	sonic_write(dev, SONIC_ISR,0x7fff);
	sonic_write(dev, SONIC_IMR,0);

	/* Now look for the MAC address. */
	if (mac_onboard_sonic_ethernet_addr(dev) != 0)
		return -ENODEV;

	printk(KERN_INFO "MAC ");
	for (i = 0; i < 6; i++) {
		printk("%2.2x", dev->dev_addr[i]);
		if (i < 5)
			printk(":");
	}

	printk(" IRQ %d\n", dev->irq);

	/* Shared init code */
	return macsonic_init(dev);
}

int __init mac_nubus_sonic_ethernet_addr(struct net_device* dev,
					 unsigned long prom_addr,
					 int id)
{
	int i;
	for(i = 0; i < 6; i++)
		dev->dev_addr[i] = SONIC_READ_PROM(i);
	/* For now we are going to assume that they're all bit-reversed */
	bit_reverse_addr(dev->dev_addr);

	return 0;
}

int __init macsonic_ident(struct nubus_dev* ndev)
{
	if (ndev->dr_hw == NUBUS_DRHW_ASANTE_LC && 
	    ndev->dr_sw == NUBUS_DRSW_SONIC_LC)
		return MACSONIC_DAYNALINK;
	if (ndev->dr_hw == NUBUS_DRHW_SONIC &&
	    ndev->dr_sw == NUBUS_DRSW_APPLE) {
		/* There has to be a better way to do this... */
		if (strstr(ndev->board->name, "DuoDock"))
			return MACSONIC_DUODOCK;
		else
			return MACSONIC_APPLE;
	}
	return -1;
}

int __init mac_nubus_sonic_probe(struct net_device* dev)
{
	static int slots = 0;
	struct nubus_dev* ndev = NULL;
	struct sonic_local* lp;
	unsigned long base_addr, prom_addr;
	u16 sonic_dcr;
	int id;
	int i;
	int reg_offset, dma_bitmode;

	/* Find the first SONIC that hasn't been initialized already */
	while ((ndev = nubus_find_type(NUBUS_CAT_NETWORK,
				       NUBUS_TYPE_ETHERNET, ndev)) != NULL)
	{
		/* Have we seen it already? */
		if (slots & (1<<ndev->board->slot))
			continue;
		slots |= 1<<ndev->board->slot;

		/* Is it one of ours? */
		if ((id = macsonic_ident(ndev)) != -1)
			break;
	}

	if (ndev == NULL)
		return -ENODEV;

	switch (id) {
	case MACSONIC_DUODOCK:
		base_addr = ndev->board->slot_addr + DUODOCK_SONIC_REGISTERS;
		prom_addr = ndev->board->slot_addr + DUODOCK_SONIC_PROM_BASE;
		sonic_dcr = SONIC_DCR_EXBUS | SONIC_DCR_RFT0 | SONIC_DCR_RFT1
			| SONIC_DCR_TFT0;
		reg_offset = 2;
		dma_bitmode = 1;
		break;
	case MACSONIC_APPLE:
		base_addr = ndev->board->slot_addr + APPLE_SONIC_REGISTERS;
		prom_addr = ndev->board->slot_addr + APPLE_SONIC_PROM_BASE;
		sonic_dcr = SONIC_DCR_BMS | SONIC_DCR_RFT1 | SONIC_DCR_TFT0;
		reg_offset = 0;
		dma_bitmode = 1;
		break;
	case MACSONIC_APPLE16:
		base_addr = ndev->board->slot_addr + APPLE_SONIC_REGISTERS;
		prom_addr = ndev->board->slot_addr + APPLE_SONIC_PROM_BASE;
		sonic_dcr = SONIC_DCR_EXBUS
 			| SONIC_DCR_RFT1 | SONIC_DCR_TFT0
			| SONIC_DCR_PO1 | SONIC_DCR_BMS; 
		reg_offset = 0;
		dma_bitmode = 0;
		break;
	case MACSONIC_DAYNALINK:
		base_addr = ndev->board->slot_addr + APPLE_SONIC_REGISTERS;
		prom_addr = ndev->board->slot_addr + DAYNALINK_PROM_BASE;
		sonic_dcr = SONIC_DCR_RFT1 | SONIC_DCR_TFT0
			| SONIC_DCR_PO1 | SONIC_DCR_BMS; 
		reg_offset = 0;
		dma_bitmode = 0;
		break;
	case MACSONIC_DAYNA:
		base_addr = ndev->board->slot_addr + DAYNA_SONIC_REGISTERS;
		prom_addr = ndev->board->slot_addr + DAYNA_SONIC_MAC_ADDR;
		sonic_dcr = SONIC_DCR_BMS
			| SONIC_DCR_RFT1 | SONIC_DCR_TFT0 | SONIC_DCR_PO1;
		reg_offset = 0;
		dma_bitmode = 0;
		break;
	default:
		printk(KERN_ERR "macsonic: WTF, id is %d\n", id);
		return -ENODEV;
	}

	if (dev) {
		dev = init_etherdev(dev, sizeof(struct sonic_local));
		/* methinks this will always be true but better safe than sorry */
		if (dev->priv == NULL)
			dev->priv = kmalloc(sizeof(struct sonic_local), GFP_KERNEL);
	} else {
		dev = init_etherdev(NULL, sizeof(struct sonic_local));
	}

	if (dev == NULL)
		return -ENOMEM;

	lp = (struct sonic_local*) dev->priv;
	memset(lp, 0, sizeof(struct sonic_local));
	/* Danger!  My arms are flailing wildly!  You *must* set this
           before using sonic_read() */
	lp->reg_offset = reg_offset;
	lp->dma_bitmode = dma_bitmode;
	dev->base_addr = base_addr;
	dev->irq = SLOT2IRQ(ndev->board->slot);

	if (!sonic_version_printed) {
		printk(KERN_INFO "%s", version);
		sonic_version_printed = 1;
	}
	printk(KERN_INFO "%s: %s in slot %X\n",
	       dev->name, ndev->board->name, ndev->board->slot);
	printk(KERN_INFO "%s: revision 0x%04x, using %d bit DMA and register offset %d\n",
	       dev->name, sonic_read(dev, SONIC_SR), dma_bitmode?32:16, reg_offset);

	/* Software reset, then initialize control registers. */
	sonic_write(dev, SONIC_CMD, SONIC_CR_RST);
	sonic_write(dev, SONIC_DCR, sonic_dcr
		    | (dma_bitmode ? SONIC_DCR_DW : 0));

	/* Clear *and* disable interrupts to be on the safe side */
	sonic_write(dev, SONIC_ISR,0x7fff);
	sonic_write(dev, SONIC_IMR,0);

	/* Now look for the MAC address. */
	if (mac_nubus_sonic_ethernet_addr(dev, prom_addr, id) != 0)
		return -ENODEV;

	printk(KERN_INFO "MAC ");
	for (i = 0; i < 6; i++) {
		printk("%2.2x", dev->dev_addr[i]);
		if (i < 5)
			printk(":");
	}
	printk(" IRQ %d\n", dev->irq);

	/* Shared init code */
	return macsonic_init(dev);
}

#ifdef MODULE
static char namespace[16] = "";
static struct net_device dev_macsonic = {
        NULL,
        0, 0, 0, 0,
        0, 0,
        0, 0, 0, NULL, NULL };

MODULE_PARM(sonic_debug, "i");

EXPORT_NO_SYMBOLS;

int
init_module(void)
{
        dev_macsonic.name = namespace;
        dev_macsonic.init = macsonic_probe;

        if (register_netdev(&dev_macsonic) != 0) {
                printk(KERN_WARNING "macsonic.c: No card found\n");
                return -ENXIO;
        }
	return 0;
}

void
cleanup_module(void)
{
        if (dev_macsonic.priv != NULL) {
                unregister_netdev(&dev_macsonic);
                kfree(dev_macsonic.priv);
                dev_macsonic.priv = NULL;
        }
}
#endif /* MODULE */


#define vdma_alloc(foo, bar) ((u32)foo)
#define vdma_free(baz)
#define sonic_chiptomem(bat) (bat)
#define PHYSADDR(quux) (quux)

#include "sonic.c"

/*
 * Local variables:
 *  compile-command: "m68k-linux-gcc -D__KERNEL__ -I../../include -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -fno-strength-reduce -ffixed-a2 -DMODULE -DMODVERSIONS -include ../../include/linux/modversions.h   -c -o macsonic.o macsonic.c"
 *  version-control: t
 *  kept-new-versions: 5
 *  c-indent-level: 8
 *  tab-width: 8
 * End:
 *
 */
