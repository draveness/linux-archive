/* 3c574.c: A PCMCIA ethernet driver for the 3com 3c574 "RoadRunner".

	Written 1993-1998 by
	Donald Becker, becker@cesdis.gsfc.nasa.gov, (driver core) and
	David Hinds, dahinds@users.sourceforge.net (from his PC card code).

	This software may be used and distributed according to the terms of
	the GNU Public License, incorporated herein by reference.

	This driver derives from Donald Becker's 3c509 core, which has the
	following copyright:
	Copyright 1993 United States Government as represented by the
	Director, National Security Agency.

*/

/* Driver author info must always be in the binary.  Version too.. */
static const char *tc574_version =
"3c574_cs.c v1.08 9/24/98 Donald Becker/David Hinds, becker@cesdis.gsfc.nasa.gov.\n";

/*
				Theory of Operation

I. Board Compatibility

This device driver is designed for the 3Com 3c574 PC card Fast Ethernet
Adapter.

II. Board-specific settings

None -- PC cards are autoconfigured.

III. Driver operation

The 3c574 uses a Boomerang-style interface, without the bus-master capability.
See the Boomerang driver and documentation for most details.

IV. Notes and chip documentation.

Two added registers are used to enhance PIO performance, RunnerRdCtrl and
RunnerWrCtrl.  These are 11 bit down-counters that are preloaded with the
count of word (16 bits) reads or writes the driver is about to do to the Rx
or Tx FIFO.  The chip is then able to hide the internal-PCI-bus to PC-card
translation latency by buffering the I/O operations with an 8 word FIFO.
Note: No other chip accesses are permitted when this buffer is used.

A second enhancement is that both attribute and common memory space
0x0800-0x0fff can translated to the PIO FIFO.  Thus memory operations (faster
with *some* PCcard bridges) may be used instead of I/O operations.
This is enabled by setting the 0x10 bit in the PCMCIA LAN COR.

Some slow PC card bridges work better if they never see a WAIT signal.
This is configured by setting the 0x20 bit in the PCMCIA LAN COR.
Only do this after testing that it is reliable and improves performance.

The upper five bits of RunnerRdCtrl are used to window into PCcard
configuration space registers.  Window 0 is the regular Boomerang/Odie
register set, 1-5 are various PC card control registers, and 16-31 are
the (reversed!) CIS table.

A final note: writing the InternalConfig register in window 3 with an
invalid ramWidth is Very Bad.

V. References

http://cesdis.gsfc.nasa.gov/linux/misc/NWay.html
http://www.national.com/pf/DP/DP83840.html

Thanks to Terry Murphy of 3Com for providing development information for
earlier 3Com products.

*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/bitops.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/ioport.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/cisreg.h>
#include <pcmcia/ciscode.h>
#include <pcmcia/ds.h>
#include <pcmcia/mem_op.h>

/* A few values that may be tweaked. */
MODULE_PARM(irq_mask, "i");
MODULE_PARM(irq_list, "1-4i");
MODULE_PARM(max_interrupt_work, "i");
MODULE_PARM(full_duplex, "i");

/* Now-standard PC card module parameters. */
static u_int irq_mask = 0xdeb8;			/* IRQ3,4,5,7,9,10,11,12,14,15 */
static int irq_list[4] = { -1 };

/* Time in jiffies before concluding the transmitter is hung. */
#define TX_TIMEOUT  ((800*HZ)/1000)

/* Maximum events (Rx packets, etc.) to handle at each interrupt. */
static int max_interrupt_work = 32;

/* Force full duplex modes? */
static int full_duplex;

/* To minimize the size of the driver source and make the driver more
   readable not all constants are symbolically defined.
   You'll need the manual if you want to understand driver details anyway. */
/* Offsets from base I/O address. */
#define EL3_DATA	0x00
#define EL3_CMD		0x0e
#define EL3_STATUS	0x0e

#define EL3WINDOW(win_num) outw(SelectWindow + (win_num), ioaddr + EL3_CMD)

/* The top five bits written to EL3_CMD are a command, the lower
   11 bits are the parameter, if applicable. */
enum el3_cmds {
	TotalReset = 0<<11, SelectWindow = 1<<11, StartCoax = 2<<11,
	RxDisable = 3<<11, RxEnable = 4<<11, RxReset = 5<<11, RxDiscard = 8<<11,
	TxEnable = 9<<11, TxDisable = 10<<11, TxReset = 11<<11,
	FakeIntr = 12<<11, AckIntr = 13<<11, SetIntrEnb = 14<<11,
	SetStatusEnb = 15<<11, SetRxFilter = 16<<11, SetRxThreshold = 17<<11,
	SetTxThreshold = 18<<11, SetTxStart = 19<<11, StatsEnable = 21<<11,
	StatsDisable = 22<<11, StopCoax = 23<<11,
};

enum elxl_status {
	IntLatch = 0x0001, AdapterFailure = 0x0002, TxComplete = 0x0004,
	TxAvailable = 0x0008, RxComplete = 0x0010, RxEarly = 0x0020,
	IntReq = 0x0040, StatsFull = 0x0080, CmdBusy = 0x1000 };

/* The SetRxFilter command accepts the following classes: */
enum RxFilter {
	RxStation = 1, RxMulticast = 2, RxBroadcast = 4, RxProm = 8
};

enum Window0 {
	Wn0EepromCmd = 10, Wn0EepromData = 12, /* EEPROM command/address, data. */
	IntrStatus=0x0E,		/* Valid in all windows. */
};
/* These assumes the larger EEPROM. */
enum Win0_EEPROM_cmds {
	EEPROM_Read = 0x200, EEPROM_WRITE = 0x100, EEPROM_ERASE = 0x300,
	EEPROM_EWENB = 0x30,		/* Enable erasing/writing for 10 msec. */
	EEPROM_EWDIS = 0x00,		/* Disable EWENB before 10 msec timeout. */
};

/* Register window 1 offsets, the window used in normal operation.
   On the "Odie" this window is always mapped at offsets 0x10-0x1f.
   Except for TxFree, which is overlapped by RunnerWrCtrl. */
enum Window1 {
	TX_FIFO = 0x10,  RX_FIFO = 0x10,  RxErrors = 0x14,
	RxStatus = 0x18,  Timer=0x1A, TxStatus = 0x1B,
	TxFree = 0x0C, /* Remaining free bytes in Tx buffer. */
	RunnerRdCtrl = 0x16, RunnerWrCtrl = 0x1c,
};

enum Window3 {			/* Window 3: MAC/config bits. */
	Wn3_Config=0, Wn3_MAC_Ctrl=6, Wn3_Options=8,
};
union wn3_config {
	int i;
	struct w3_config_fields {
		unsigned int ram_size:3, ram_width:1, ram_speed:2, rom_size:2;
		int pad8:8;
		unsigned int ram_split:2, pad18:2, xcvr:3, pad21:1, autoselect:1;
		int pad24:7;
	} u;
};

enum Window4 {		/* Window 4: Xcvr/media bits. */
	Wn4_FIFODiag = 4, Wn4_NetDiag = 6, Wn4_PhysicalMgmt=8, Wn4_Media = 10,
};

#define MEDIA_TP	0x00C0	/* Enable link beat and jabber for 10baseT. */

struct el3_private {
	dev_link_t link;
	struct net_device dev;
	dev_node_t node;
	struct net_device_stats stats;
	u16 advertising, partner;			/* NWay media advertisement */
	unsigned char phys[2];				/* MII device addresses. */
	unsigned int
	  autoselect:1, default_media:3;	/* Read from the EEPROM/Wn3_Config. */
	/* for transceiver monitoring */
	struct timer_list media;
	u_short media_status;
	u_short fast_poll;
	u_long last_irq;
};

/* Set iff a MII transceiver on any interface requires mdio preamble.
   This only set with the original DP83840 on older 3c905 boards, so the extra
   code size of a per-interface flag is not worthwhile. */
static char mii_preamble_required = 0;

#ifdef PCMCIA_DEBUG
static int pc_debug = PCMCIA_DEBUG;
MODULE_PARM(pc_debug, "i");
#define DEBUG(n, args...) if (pc_debug>(n)) printk(KERN_DEBUG args)
static char *version =
"3c574_cs.c 1.000 1998/1/8 Donald Becker, becker@cesdis.gsfc.nasa.gov.\n";
#else
#define DEBUG(n, args...)
#endif

/* Index of functions. */

static void tc574_config(dev_link_t *link);
static void tc574_release(u_long arg);
static int tc574_event(event_t event, int priority,
					   event_callback_args_t *args);

static void mdio_sync(ioaddr_t ioaddr, int bits);
static int mdio_read(ioaddr_t ioaddr, int phy_id, int location);
static void mdio_write(ioaddr_t ioaddr, int phy_id, int location, int value);
static u_short read_eeprom(ioaddr_t ioaddr, int index);
static void wait_for_completion(struct net_device *dev, int cmd);

static void tc574_reset(struct net_device *dev);
static void media_check(u_long arg);
static int el3_open(struct net_device *dev);
static int el3_start_xmit(struct sk_buff *skb, struct net_device *dev);
static void el3_interrupt(int irq, void *dev_id, struct pt_regs *regs);
static void update_stats(struct net_device *dev);
static struct net_device_stats *el3_get_stats(struct net_device *dev);
static int el3_rx(struct net_device *dev, int worklimit);
static int el3_close(struct net_device *dev);
static void el3_tx_timeout(struct net_device *dev);
static int el3_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
static void set_rx_mode(struct net_device *dev);

static dev_info_t dev_info = "3c574_cs";

static dev_link_t *tc574_attach(void);
static void tc574_detach(dev_link_t *);

static dev_link_t *dev_list = NULL;

static void flush_stale_links(void)
{
	dev_link_t *link, *next;
	for (link = dev_list; link; link = next) {
		next = link->next;
	    if (link->state & DEV_STALE_LINK)
			tc574_detach(link);
    }
}

static void cs_error(client_handle_t handle, int func, int ret)
{
#if CS_RELEASE_CODE < 0x2911
    CardServices(ReportError, dev_info, (void *)func, (void *)ret);
#else
	error_info_t err = { func, ret };
	CardServices(ReportError, handle, &err);
#endif
}

/*
	tc574_attach() creates an "instance" of the driver, allocating
	local data structures for one device.  The device is registered
	with Card Services.
*/

static dev_link_t *tc574_attach(void)
{
	struct el3_private *lp;
	client_reg_t client_reg;
	dev_link_t *link;
	struct net_device *dev;
	int i, ret;

	DEBUG(0, "3c574_attach()\n");
	flush_stale_links();

	/* Create the PC card device object. */
	lp = kmalloc(sizeof(*lp), GFP_KERNEL);
	if (!lp) return NULL;
	memset(lp, 0, sizeof(*lp));
	link = &lp->link; dev = &lp->dev;
	link->priv = dev->priv = link->irq.Instance = lp;
	
	link->release.function = &tc574_release;
	link->release.data = (u_long)link;
	link->io.NumPorts1 = 32;
	link->io.Attributes1 = IO_DATA_PATH_WIDTH_16;
	link->irq.Attributes = IRQ_TYPE_EXCLUSIVE | IRQ_HANDLE_PRESENT;
	link->irq.IRQInfo1 = IRQ_INFO2_VALID|IRQ_LEVEL_ID;
	if (irq_list[0] == -1)
		link->irq.IRQInfo2 = irq_mask;
	else
		for (i = 0; i < 4; i++)
			link->irq.IRQInfo2 |= 1 << irq_list[i];
	link->irq.Handler = &el3_interrupt;
	link->conf.Attributes = CONF_ENABLE_IRQ;
	link->conf.Vcc = 50;
	link->conf.IntType = INT_MEMORY_AND_IO;
	link->conf.ConfigIndex = 1;
	link->conf.Present = PRESENT_OPTION;

	/* The EL3-specific entries in the device structure. */
	dev->hard_start_xmit = &el3_start_xmit;
	dev->get_stats = &el3_get_stats;
	dev->do_ioctl = &el3_ioctl;
	dev->set_multicast_list = &set_rx_mode;
	ether_setup(dev);
	dev->open = &el3_open;
	dev->stop = &el3_close;
	dev->tx_timeout = el3_tx_timeout;
	dev->watchdog_timeo = TX_TIMEOUT;

	/* Register with Card Services */
	link->next = dev_list;
	dev_list = link;
	client_reg.dev_info = &dev_info;
	client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
	client_reg.EventMask =
		CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
			CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
				CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
	client_reg.event_handler = &tc574_event;
	client_reg.Version = 0x0210;
	client_reg.event_callback_args.client_data = link;
	ret = CardServices(RegisterClient, &link->handle, &client_reg);
	if (ret != 0) {
		cs_error(link->handle, RegisterClient, ret);
		tc574_detach(link);
		return NULL;
	}

	return link;
} /* tc574_attach */

/*

	This deletes a driver "instance".  The device is de-registered
	with Card Services.  If it has been released, all local data
	structures are freed.  Otherwise, the structures will be freed
	when the device is released.

*/

static void tc574_detach(dev_link_t *link)
{
	struct el3_private *lp = link->priv;
	dev_link_t **linkp;

	DEBUG(0, "3c574_detach(0x%p)\n", link);

	/* Locate device structure */
	for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
		if (*linkp == link) break;
	if (*linkp == NULL)
	return;

	del_timer(&link->release);
	if (link->state & DEV_CONFIG) {
		tc574_release((u_long)link);
		if (link->state & DEV_STALE_CONFIG) {
			link->state |= DEV_STALE_LINK;
			return;
		}
	}

	if (link->handle)
		CardServices(DeregisterClient, link->handle);

	/* Unlink device structure, free bits */
	*linkp = link->next;
	if (link->dev)
		unregister_netdev(&lp->dev);
	kfree(lp);

} /* tc574_detach */

/*
	tc574_config() is scheduled to run after a CARD_INSERTION event
	is received, to configure the PCMCIA socket, and to make the
	ethernet device available to the system.
*/

#define CS_CHECK(fn, args...) \
while ((last_ret=CardServices(last_fn=(fn), args))!=0) goto cs_failed

static void tc574_config(dev_link_t *link)
{
	client_handle_t handle = link->handle;
	struct el3_private *lp = link->priv;
	struct net_device *dev = &lp->dev;
	tuple_t tuple;
	cisparse_t parse;
	u_short buf[32];
	int last_fn, last_ret, i, j;
	ioaddr_t ioaddr;
	u16 *phys_addr;
	char *cardname;

	phys_addr = (u16 *)dev->dev_addr;

	DEBUG(0, "3c574_config(0x%p)\n", link);

	tuple.Attributes = 0;
	tuple.DesiredTuple = CISTPL_CONFIG;
	CS_CHECK(GetFirstTuple, handle, &tuple);
	tuple.TupleData = (cisdata_t *)buf;
	tuple.TupleDataMax = 64;
	tuple.TupleOffset = 0;
	CS_CHECK(GetTupleData, handle, &tuple);
	CS_CHECK(ParseTuple, handle, &tuple, &parse);
	link->conf.ConfigBase = parse.config.base;
	link->conf.Present = parse.config.rmask[0];

	/* Configure card */
	link->state |= DEV_CONFIG;

	link->io.IOAddrLines = 16;
	for (i = j = 0; j < 0x400; j += 0x20) {
		link->io.BasePort1 = j ^ 0x300;
		i = CardServices(RequestIO, link->handle, &link->io);
		if (i == CS_SUCCESS) break;
	}
	if (i != CS_SUCCESS) {
		cs_error(link->handle, RequestIO, i);
		goto failed;
	}
	CS_CHECK(RequestIRQ, link->handle, &link->irq);
	CS_CHECK(RequestConfiguration, link->handle, &link->conf);

	dev->irq = link->irq.AssignedIRQ;
	dev->base_addr = link->io.BasePort1;

	if (register_netdev(dev) != 0) {
		printk(KERN_NOTICE "3c574_cs: register_netdev() failed\n");
		goto failed;
	}

	ioaddr = dev->base_addr;
	strcpy(lp->node.dev_name, dev->name);
	link->dev = &lp->node;
	link->state &= ~DEV_CONFIG_PENDING;

	/* The 3c574 normally uses an EEPROM for configuration info, including
	   the hardware address.  The future products may include a modem chip
	   and put the address in the CIS. */
	tuple.DesiredTuple = 0x88;
	if (CardServices(GetFirstTuple, handle, &tuple) == CS_SUCCESS) {
		CardServices(GetTupleData, handle, &tuple);
		for (i = 0; i < 3; i++)
			phys_addr[i] = htons(buf[i]);
	} else {
		EL3WINDOW(0);
		for (i = 0; i < 3; i++)
			phys_addr[i] = htons(read_eeprom(ioaddr, i + 10));
		if (phys_addr[0] == 0x6060) {
			printk(KERN_NOTICE "3c574_cs: IO port conflict at 0x%03lx"
				   "-0x%03lx\n", dev->base_addr, dev->base_addr+15);
			goto failed;
		}
	}
	tuple.DesiredTuple = CISTPL_VERS_1;
	if (CardServices(GetFirstTuple, handle, &tuple) == CS_SUCCESS &&
		CardServices(GetTupleData, handle, &tuple) == CS_SUCCESS &&
		CardServices(ParseTuple, handle, &tuple, &parse) == CS_SUCCESS) {
		cardname = parse.version_1.str + parse.version_1.ofs[1];
	} else
		cardname = "3Com 3c574";

	printk(KERN_INFO "%s: %s at io %#3lx, irq %d, hw_addr ",
		   dev->name, cardname, dev->base_addr, dev->irq);

	for (i = 0; i < 6; i++)
		printk("%02X%s", dev->dev_addr[i], ((i<5) ? ":" : ".\n"));

	{
		u_char mcr, *ram_split[] = {"5:3", "3:1", "1:1", "3:5"};
		union wn3_config config;
		outw(2<<11, ioaddr + RunnerRdCtrl);
		mcr = inb(ioaddr + 2);
		outw(0<<11, ioaddr + RunnerRdCtrl);
		printk(KERN_INFO "  ASIC rev %d,", mcr>>3);
		EL3WINDOW(3);
		config.i = inl(ioaddr + Wn3_Config);
		printk(" %dK FIFO split %s Rx:Tx, %sMII interface.\n",
			   8 << config.u.ram_size, ram_split[config.u.ram_split],
			   config.u.autoselect ? "autoselect " : "");
		lp->default_media = config.u.xcvr;
		lp->autoselect = config.u.autoselect;
	}

	{
		int phy, phy_idx = 0;
		
		/* Roadrunner only: Turn on the MII transceiver */
		outw(0x8040, ioaddr + Wn3_Options);
		mdelay(1);
		outw(0xc040, ioaddr + Wn3_Options);
		wait_for_completion(dev, TxReset);
		wait_for_completion(dev, RxReset);
		mdelay(1);
		outw(0x8040, ioaddr + Wn3_Options);
		
		EL3WINDOW(4);
		for (phy = 1; phy <= 32 && phy_idx < sizeof(lp->phys); phy++) {
			int mii_status;
			mdio_sync(ioaddr, 32);
			mii_status = mdio_read(ioaddr, phy & 0x1f, 1);
			if (mii_status != 0xffff) {
				lp->phys[phy_idx++] = phy & 0x1f;
				DEBUG(0, "  MII transceiver at index %d, status %x.\n",
					  phy, mii_status);
				if ((mii_status & 0x0040) == 0)
					mii_preamble_required = 1;
			}
		}
		if (phy_idx == 0) {
			printk(KERN_NOTICE "  No MII transceivers found!\n");
			goto failed;
		}
		i = mdio_read(ioaddr, lp->phys[0], 16) | 0x40;
		mdio_write(ioaddr, lp->phys[0], 16, i);
		lp->advertising = mdio_read(ioaddr, lp->phys[0], 4);
		if (full_duplex) {
			/* Only advertise the FD media types. */
			lp->advertising &= ~0x02a0;
			mdio_write(ioaddr, lp->phys[0], 4, lp->advertising);
		}
	}

	return;

cs_failed:
	cs_error(link->handle, last_fn, last_ret);
failed:
	tc574_release((u_long)link);
	return;

} /* tc574_config */

/*
	After a card is removed, tc574_release() will unregister the net
	device, and release the PCMCIA configuration.  If the device is
	still open, this will be postponed until it is closed.
*/

static void tc574_release(u_long arg)
{
	dev_link_t *link = (dev_link_t *)arg;

	DEBUG(0, "3c574_release(0x%p)\n", link);

	if (link->open) {
		DEBUG(1, "3c574_cs: release postponed, '%s' still open\n",
			  link->dev->dev_name);
		link->state |= DEV_STALE_CONFIG;
		return;
	}

	CardServices(ReleaseConfiguration, link->handle);
	CardServices(ReleaseIO, link->handle, &link->io);
	CardServices(ReleaseIRQ, link->handle, &link->irq);

	link->state &= ~DEV_CONFIG;

} /* tc574_release */

/*
	The card status event handler.  Mostly, this schedules other
	stuff to run after an event is received.  A CARD_REMOVAL event
	also sets some flags to discourage the net drivers from trying
	to talk to the card any more.
*/

static int tc574_event(event_t event, int priority,
					   event_callback_args_t *args)
{
	dev_link_t *link = args->client_data;
	struct el3_private *lp = link->priv;
	struct net_device *dev = &lp->dev;

	DEBUG(1, "3c574_event(0x%06x)\n", event);

	switch (event) {
	case CS_EVENT_CARD_REMOVAL:
		link->state &= ~DEV_PRESENT;
		if (link->state & DEV_CONFIG) {
			netif_device_detach(dev);
			mod_timer(&link->release, jiffies + HZ/20);
		}
		break;
	case CS_EVENT_CARD_INSERTION:
		link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
		tc574_config(link);
		break;
	case CS_EVENT_PM_SUSPEND:
		link->state |= DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_RESET_PHYSICAL:
		if (link->state & DEV_CONFIG) {
			if (link->open)
				netif_device_detach(dev);
			CardServices(ReleaseConfiguration, link->handle);
		}
		break;
	case CS_EVENT_PM_RESUME:
		link->state &= ~DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_CARD_RESET:
		if (link->state & DEV_CONFIG) {
			CardServices(RequestConfiguration, link->handle, &link->conf);
			if (link->open) {
				tc574_reset(dev);
				netif_device_attach(dev);
			}
		}
		break;
	}
	return 0;
} /* tc574_event */

static void dump_status(struct net_device *dev)
{
	ioaddr_t ioaddr = dev->base_addr;
	EL3WINDOW(1);
    printk(KERN_INFO "  irq status %04x, rx status %04x, tx status "
		   "%02x, tx free %04x\n", inw(ioaddr+EL3_STATUS),
		   inw(ioaddr+RxStatus), inb(ioaddr+TxStatus),
		   inw(ioaddr+TxFree));
	EL3WINDOW(4);
    printk(KERN_INFO "  diagnostics: fifo %04x net %04x ethernet %04x"
		   " media %04x\n", inw(ioaddr+0x04), inw(ioaddr+0x06),
		   inw(ioaddr+0x08), inw(ioaddr+0x0a));
	EL3WINDOW(1);
}

/*
  Use this for commands that may take time to finish
*/
static void wait_for_completion(struct net_device *dev, int cmd)
{
    int i = 1500;
    outw(cmd, dev->base_addr + EL3_CMD);
    while (--i > 0)
		if (!(inw(dev->base_addr + EL3_STATUS) & 0x1000)) break;
    if (i == 0)
		printk(KERN_NOTICE "%s: command 0x%04x did not complete!\n",
			   dev->name, cmd);
}

/* Read a word from the EEPROM using the regular EEPROM access register.
   Assume that we are in register window zero.
 */
static u_short read_eeprom(ioaddr_t ioaddr, int index)
{
	int timer;
	outw(EEPROM_Read + index, ioaddr + Wn0EepromCmd);
	/* Pause for at least 162 usec for the read to take place. */
	for (timer = 1620; timer >= 0; timer--) {
		if ((inw(ioaddr + Wn0EepromCmd) & 0x8000) == 0)
			break;
	}
	return inw(ioaddr + Wn0EepromData);
}

/* MII transceiver control section.
   Read and write the MII registers using software-generated serial
   MDIO protocol.  See the MII specifications or DP83840A data sheet
   for details.
   The maxium data clock rate is 2.5 Mhz.  The timing is easily met by the
   slow PC card interface. */

#define MDIO_SHIFT_CLK	0x01
#define MDIO_DIR_WRITE	0x04
#define MDIO_DATA_WRITE0 (0x00 | MDIO_DIR_WRITE)
#define MDIO_DATA_WRITE1 (0x02 | MDIO_DIR_WRITE)
#define MDIO_DATA_READ	0x02
#define MDIO_ENB_IN		0x00

/* Generate the preamble required for initial synchronization and
   a few older transceivers. */
static void mdio_sync(ioaddr_t ioaddr, int bits)
{
	int mdio_addr = ioaddr + Wn4_PhysicalMgmt;

	/* Establish sync by sending at least 32 logic ones. */
	while (-- bits >= 0) {
		outw(MDIO_DATA_WRITE1, mdio_addr);
		outw(MDIO_DATA_WRITE1 | MDIO_SHIFT_CLK, mdio_addr);
	}
}

static int mdio_read(ioaddr_t ioaddr, int phy_id, int location)
{
	int i;
	int read_cmd = (0xf6 << 10) | (phy_id << 5) | location;
	unsigned int retval = 0;
	int mdio_addr = ioaddr + Wn4_PhysicalMgmt;

	if (mii_preamble_required)
		mdio_sync(ioaddr, 32);

	/* Shift the read command bits out. */
	for (i = 14; i >= 0; i--) {
		int dataval = (read_cmd&(1<<i)) ? MDIO_DATA_WRITE1 : MDIO_DATA_WRITE0;
		outw(dataval, mdio_addr);
		outw(dataval | MDIO_SHIFT_CLK, mdio_addr);
	}
	/* Read the two transition, 16 data, and wire-idle bits. */
	for (i = 19; i > 0; i--) {
		outw(MDIO_ENB_IN, mdio_addr);
		retval = (retval << 1) | ((inw(mdio_addr) & MDIO_DATA_READ) ? 1 : 0);
		outw(MDIO_ENB_IN | MDIO_SHIFT_CLK, mdio_addr);
	}
	return (retval>>1) & 0xffff;
}

static void mdio_write(ioaddr_t ioaddr, int phy_id, int location, int value)
{
	int write_cmd = 0x50020000 | (phy_id << 23) | (location << 18) | value;
	int mdio_addr = ioaddr + Wn4_PhysicalMgmt;
	int i;

	if (mii_preamble_required)
		mdio_sync(ioaddr, 32);

	/* Shift the command bits out. */
	for (i = 31; i >= 0; i--) {
		int dataval = (write_cmd&(1<<i)) ? MDIO_DATA_WRITE1 : MDIO_DATA_WRITE0;
		outw(dataval, mdio_addr);
		outw(dataval | MDIO_SHIFT_CLK, mdio_addr);
	}
	/* Leave the interface idle. */
	for (i = 1; i >= 0; i--) {
		outw(MDIO_ENB_IN, mdio_addr);
		outw(MDIO_ENB_IN | MDIO_SHIFT_CLK, mdio_addr);
	}

	return;
}

/* Reset and restore all of the 3c574 registers. */
static void tc574_reset(struct net_device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;
	int i, ioaddr = dev->base_addr;

	wait_for_completion(dev, TotalReset|0x10);

	/* Clear any transactions in progress. */
	outw(0, ioaddr + RunnerWrCtrl);
	outw(0, ioaddr + RunnerRdCtrl);

	/* Set the station address and mask. */
	EL3WINDOW(2);
	for (i = 0; i < 6; i++)
		outb(dev->dev_addr[i], ioaddr + i);
	for (; i < 12; i+=2)
		outw(0, ioaddr + i);

	/* Reset config options */
	EL3WINDOW(3);
	outb((dev->mtu > 1500 ? 0x40 : 0), ioaddr + Wn3_MAC_Ctrl);
	outl((lp->autoselect ? 0x01000000 : 0) | 0x0062001b,
		 ioaddr + Wn3_Config);
	
	/* Roadrunner only: Turn on the MII transceiver. */
	outw(0x8040, ioaddr + Wn3_Options);
	mdelay(1);
	outw(0xc040, ioaddr + Wn3_Options);
	wait_for_completion(dev, TxReset);
	wait_for_completion(dev, RxReset);
	mdelay(1);
	outw(0x8040, ioaddr + Wn3_Options);

	/* Switch to the stats window, and clear all stats by reading. */
	outw(StatsDisable, ioaddr + EL3_CMD);
	EL3WINDOW(6);
	for (i = 0; i < 10; i++)
		inb(ioaddr + i);
	inw(ioaddr + 10);
	inw(ioaddr + 12);
	EL3WINDOW(4);
	inb(ioaddr + 12);
	inb(ioaddr + 13);

	/* .. enable any extra statistics bits.. */
	outw(0x0040, ioaddr + Wn4_NetDiag);
	/* .. re-sync MII and re-fill what NWay is advertising. */
	mdio_sync(ioaddr, 32);
	mdio_write(ioaddr, lp->phys[0], 4, lp->advertising);

	/* Switch to register set 1 for normal use, just for TxFree. */
	EL3WINDOW(1);

	set_rx_mode(dev);
	outw(StatsEnable, ioaddr + EL3_CMD); /* Turn on statistics. */
	outw(RxEnable, ioaddr + EL3_CMD); /* Enable the receiver. */
	outw(TxEnable, ioaddr + EL3_CMD); /* Enable transmitter. */
	/* Allow status bits to be seen. */
	outw(SetStatusEnb | 0xff, ioaddr + EL3_CMD);
	/* Ack all pending events, and set active indicator mask. */
	outw(AckIntr | IntLatch | TxAvailable | RxEarly | IntReq,
		 ioaddr + EL3_CMD);
	outw(SetIntrEnb | IntLatch | TxAvailable | RxComplete | StatsFull
		 | AdapterFailure | RxEarly, ioaddr + EL3_CMD);
}

static int el3_open(struct net_device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;
	dev_link_t *link = &lp->link;

	if (!DEV_OK(link))
		return -ENODEV;
	
	link->open++;
	MOD_INC_USE_COUNT;
	netif_start_queue(dev);
	
	tc574_reset(dev);
	lp->media.function = &media_check;
	lp->media.data = (u_long)lp;
	lp->media.expires = jiffies + HZ;
	add_timer(&lp->media);
	
	DEBUG(2, "%s: opened, status %4.4x.\n",
		  dev->name, inw(dev->base_addr + EL3_STATUS));
	
	return 0;
}

static void el3_tx_timeout(struct net_device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;
	ioaddr_t ioaddr = dev->base_addr;
	
	printk(KERN_NOTICE "%s: Transmit timed out!\n", dev->name);
	dump_status(dev);
	lp->stats.tx_errors++;
	dev->trans_start = jiffies;
	/* Issue TX_RESET and TX_START commands. */
	wait_for_completion(dev, TxReset);
	outw(TxEnable, ioaddr + EL3_CMD);
	netif_start_queue(dev);
}

static void pop_tx_status(struct net_device *dev)
{
    struct el3_private *lp = (struct el3_private *)dev->priv;
    ioaddr_t ioaddr = dev->base_addr;
    int i;
    
    /* Clear the Tx status stack. */
    for (i = 32; i > 0; i--) {
		u_char tx_status = inb(ioaddr + TxStatus);
		if (!(tx_status & 0x84)) break;
		/* reset transmitter on jabber error or underrun */
		if (tx_status & 0x30)
			wait_for_completion(dev, TxReset);
		if (tx_status & 0x38) {
			DEBUG(1, "%s: transmit error: status 0x%02x\n",
				  dev->name, tx_status);
			outw(TxEnable, ioaddr + EL3_CMD);
			lp->stats.tx_aborted_errors++;
		}
		outb(0x00, ioaddr + TxStatus); /* Pop the status stack. */
    }
}

static int el3_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	ioaddr_t ioaddr = dev->base_addr;

	DEBUG(3, "%s: el3_start_xmit(length = %ld) called, "
		  "status %4.4x.\n", dev->name, (long)skb->len,
		  inw(ioaddr + EL3_STATUS));

	outw(skb->len, ioaddr + TX_FIFO);
	outw(0, ioaddr + TX_FIFO);
	outsl(ioaddr + TX_FIFO, skb->data, (skb->len+3)>>2);

	dev->trans_start = jiffies;

	/* TxFree appears only in Window 1, not offset 0x1c. */
	if (inw(ioaddr + TxFree) <= 1536) {
		netif_stop_queue(dev);
		/* Interrupt us when the FIFO has room for max-sized packet. 
		   The threshold is in units of dwords. */
		outw(SetTxThreshold + (1536>>2), ioaddr + EL3_CMD);
	}

	dev_kfree_skb (skb);
	pop_tx_status(dev);

	return 0;
}

/* The EL3 interrupt handler. */
static void el3_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct el3_private *lp = dev_id;
	struct net_device *dev = &lp->dev;
	ioaddr_t ioaddr, status;
	int work_budget = max_interrupt_work;

	if (!netif_device_present(dev))
		return;
	ioaddr = dev->base_addr;

	DEBUG(3, "%s: interrupt, status %4.4x.\n",
		  dev->name, inw(ioaddr + EL3_STATUS));

	while ((status = inw(ioaddr + EL3_STATUS)) &
		   (IntLatch | RxComplete | RxEarly | StatsFull)) {
		if (!netif_device_present(dev) ||
			((status & 0xe000) != 0x2000)) {
			DEBUG(1, "%s: Interrupt from dead card\n", dev->name);
			break;
		}

		if (status & RxComplete)
			work_budget = el3_rx(dev, work_budget);

		if (status & TxAvailable) {
			DEBUG(3, "  TX room bit was handled.\n");
			/* There's room in the FIFO for a full-sized packet. */
			outw(AckIntr | TxAvailable, ioaddr + EL3_CMD);
			netif_wake_queue(dev);
		}

		if (status & TxComplete)
			pop_tx_status(dev);

		if (status & (AdapterFailure | RxEarly | StatsFull)) {
			/* Handle all uncommon interrupts. */
			if (status & StatsFull)
				update_stats(dev);
			if (status & RxEarly) {
				work_budget = el3_rx(dev, work_budget);
				outw(AckIntr | RxEarly, ioaddr + EL3_CMD);
			}
			if (status & AdapterFailure) {
				u16 fifo_diag;
				EL3WINDOW(4);
				fifo_diag = inw(ioaddr + Wn4_FIFODiag);
				EL3WINDOW(1);
				printk(KERN_NOTICE "%s: adapter failure, FIFO diagnostic"
					   " register %04x.\n", dev->name, fifo_diag);
				if (fifo_diag & 0x0400) {
					/* Tx overrun */
					wait_for_completion(dev, TxReset);
					outw(TxEnable, ioaddr + EL3_CMD);
				}
				if (fifo_diag & 0x2000) {
					/* Rx underrun */
					wait_for_completion(dev, RxReset);
					set_rx_mode(dev);
					outw(RxEnable, ioaddr + EL3_CMD);
				}
				outw(AckIntr | AdapterFailure, ioaddr + EL3_CMD);
			}
		}

		if (--work_budget < 0) {
			DEBUG(0, "%s: Too much work in interrupt, "
				  "status %4.4x.\n", dev->name, status);
			/* Clear all interrupts */
			outw(AckIntr | 0xFF, ioaddr + EL3_CMD);
			break;
		}
		/* Acknowledge the IRQ. */
		outw(AckIntr | IntReq | IntLatch, ioaddr + EL3_CMD);
	}

	DEBUG(3, "%s: exiting interrupt, status %4.4x.\n",
		  dev->name, inw(ioaddr + EL3_STATUS));
	return;
}

/*
    This timer serves two purposes: to check for missed interrupts
	(and as a last resort, poll the NIC for events), and to monitor
	the MII, reporting changes in cable status.
*/
static void media_check(u_long arg)
{
    struct el3_private *lp = (struct el3_private *)arg;
    struct net_device *dev = &lp->dev;
    ioaddr_t ioaddr = dev->base_addr;
    u_long flags;
	u_short /* cable, */ media, partner;

	if (!netif_device_present(dev))
		goto reschedule;
	
    /* Check for pending interrupt with expired latency timer: with
       this, we can limp along even if the interrupt is blocked */
    if ((inw(ioaddr + EL3_STATUS) & IntLatch) &&
		(inb(ioaddr + Timer) == 0xff)) {
		if (!lp->fast_poll)
			printk(KERN_INFO "%s: interrupt(s) dropped!\n", dev->name);
		el3_interrupt(dev->irq, lp, NULL);
		lp->fast_poll = HZ;
    }
    if (lp->fast_poll) {
		lp->fast_poll--;
		lp->media.expires = jiffies + 2;
		add_timer(&lp->media);
		return;
    }

	save_flags(flags);
	cli();
	EL3WINDOW(4);
	media = mdio_read(ioaddr, lp->phys[0], 1);
	partner = mdio_read(ioaddr, lp->phys[0], 5);
	EL3WINDOW(1);
	restore_flags(flags);

	if (media != lp->media_status) {
		if ((media ^ lp->media_status) & 0x0004)
			printk(KERN_INFO "%s: %s link beat\n", dev->name,
				   (lp->media_status & 0x0004) ? "lost" : "found");
		if ((media ^ lp->media_status) & 0x0020) {
			lp->partner = 0;
			if (lp->media_status & 0x0020) {
				printk(KERN_INFO "%s: autonegotiation restarted\n",
					   dev->name);
			} else if (partner) {
				partner &= lp->advertising;
				lp->partner = partner;
				printk(KERN_INFO "%s: autonegotiation complete: "
					   "%sbaseT-%cD selected\n", dev->name,
					   ((partner & 0x0180) ? "100" : "10"),
					   ((partner & 0x0140) ? 'F' : 'H'));
			} else {
				printk(KERN_INFO "%s: link partner did not autonegotiate\n",
					   dev->name);
			}

			EL3WINDOW(3);
			outb((partner & 0x0140 ? 0x20 : 0) |
				 (dev->mtu > 1500 ? 0x40 : 0), ioaddr + Wn3_MAC_Ctrl);
			EL3WINDOW(1);

		}
		if (media & 0x0010)
			printk(KERN_INFO "%s: remote fault detected\n",
				   dev->name);
		if (media & 0x0002)
			printk(KERN_INFO "%s: jabber detected\n", dev->name);
		lp->media_status = media;
	}

reschedule:
    lp->media.expires = jiffies + HZ;
    add_timer(&lp->media);
}

static struct net_device_stats *el3_get_stats(struct net_device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;

	if (netif_device_present(dev))
		update_stats(dev);
	return &lp->stats;
}

/*  Update statistics.
	Suprisingly this need not be run single-threaded, but it effectively is.
	The counters clear when read, so the adds must merely be atomic.
 */
static void update_stats(struct net_device *dev)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;
	ioaddr_t ioaddr = dev->base_addr;
	u8 rx, tx, up;

	DEBUG(2, "%s: updating the statistics.\n", dev->name);

	if (inw(ioaddr+EL3_STATUS) == 0xffff) /* No card. */
		return;

	/* Unlike the 3c509 we need not turn off stats updates while reading. */
	/* Switch to the stats window, and read everything. */
	EL3WINDOW(6);
	lp->stats.tx_carrier_errors 	+= inb(ioaddr + 0);
	lp->stats.tx_heartbeat_errors	+= inb(ioaddr + 1);
	/* Multiple collisions. */	   	inb(ioaddr + 2);
	lp->stats.collisions			+= inb(ioaddr + 3);
	lp->stats.tx_window_errors		+= inb(ioaddr + 4);
	lp->stats.rx_fifo_errors		+= inb(ioaddr + 5);
	lp->stats.tx_packets			+= inb(ioaddr + 6);
	up		 						 = inb(ioaddr + 9);
	lp->stats.tx_packets			+= (up&0x30) << 4;
	/* Rx packets   */				   inb(ioaddr + 7);
	/* Tx deferrals */				   inb(ioaddr + 8);
	rx		 						 = inw(ioaddr + 10);
	tx								 = inw(ioaddr + 12);

	EL3WINDOW(4);
	/* BadSSD */					   inb(ioaddr + 12);
	up								 = inb(ioaddr + 13);

	lp->stats.rx_bytes += rx + ((up & 0x0f) << 16);
	lp->stats.tx_bytes += tx + ((up & 0xf0) << 12);

	EL3WINDOW(1);
}

static int el3_rx(struct net_device *dev, int worklimit)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;
	ioaddr_t ioaddr = dev->base_addr;
	short rx_status;
	
	DEBUG(3, "%s: in rx_packet(), status %4.4x, rx_status %4.4x.\n",
		  dev->name, inw(ioaddr+EL3_STATUS), inw(ioaddr+RxStatus));
    while (!((rx_status = inw(ioaddr + RxStatus)) & 0x8000) &&
		   (--worklimit >= 0)) {
		if (rx_status & 0x4000) { /* Error, update stats. */
			short error = rx_status & 0x3800;
			lp->stats.rx_errors++;
			switch (error) {
			case 0x0000:	lp->stats.rx_over_errors++; break;
			case 0x0800:	lp->stats.rx_length_errors++; break;
			case 0x1000:	lp->stats.rx_frame_errors++; break;
			case 0x1800:	lp->stats.rx_length_errors++; break;
			case 0x2000:	lp->stats.rx_frame_errors++; break;
			case 0x2800:	lp->stats.rx_crc_errors++; break;
			}
		} else {
			short pkt_len = rx_status & 0x7ff;
			struct sk_buff *skb;

			skb = dev_alloc_skb(pkt_len+5);

			DEBUG(3, "  Receiving packet size %d status %4.4x.\n",
				  pkt_len, rx_status);
			if (skb != NULL) {
				skb->dev = dev;
				skb_reserve(skb, 2);

				insl(ioaddr+RX_FIFO, skb_put(skb, pkt_len),
						((pkt_len+3)>>2));

				skb->protocol = eth_type_trans(skb, dev);
				netif_rx(skb);
				lp->stats.rx_packets++;
			} else {
				DEBUG(1, "%s: couldn't allocate a sk_buff of"
					  " size %d.\n", dev->name, pkt_len);
				lp->stats.rx_dropped++;
			}
		}
		wait_for_completion(dev, RxDiscard);
	}

	return worklimit;
}

/* Provide ioctl() calls to examine the MII xcvr state. */
static int el3_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct el3_private *lp = (struct el3_private *)dev->priv;
	ioaddr_t ioaddr = dev->base_addr;
	u16 *data = (u16 *)&rq->ifr_data;
	int phy = lp->phys[0] & 0x1f;

	DEBUG(2, "%s: In ioct(%-.6s, %#4.4x) %4.4x %4.4x %4.4x %4.4x.\n",
		  dev->name, rq->ifr_ifrn.ifrn_name, cmd,
		  data[0], data[1], data[2], data[3]);

    switch(cmd) {
	case SIOCDEVPRIVATE:		/* Get the address of the PHY in use. */
		data[0] = phy;
	case SIOCDEVPRIVATE+1:		/* Read the specified MII register. */
		{
			int saved_window;
			long flags;

			save_flags(flags);
			cli();
			saved_window = inw(ioaddr + EL3_CMD) >> 13;
			EL3WINDOW(4);
			data[3] = mdio_read(ioaddr, data[0] & 0x1f, data[1] & 0x1f);
			EL3WINDOW(saved_window);
			restore_flags(flags);
			return 0;
		}
	case SIOCDEVPRIVATE+2:		/* Write the specified MII register */
		{
			int saved_window;
			long flags;

			if (!capable(CAP_NET_ADMIN))
				return -EPERM;
			save_flags(flags);
			cli();
			saved_window = inw(ioaddr + EL3_CMD) >> 13;
			EL3WINDOW(4);
			mdio_write(ioaddr, data[0] & 0x1f, data[1] & 0x1f, data[2]);
			EL3WINDOW(saved_window);
			restore_flags(flags);
			return 0;
		}
	default:
		return -EOPNOTSUPP;
	}
}

/* The Odie chip has a 64 bin multicast filter, but the bit layout is not
   documented.  Until it is we revert to receiving all multicast frames when
   any multicast reception is desired.
   Note: My other drivers emit a log message whenever promiscuous mode is
   entered to help detect password sniffers.  This is less desirable on
   typical PC card machines, so we omit the message.
   */

static void set_rx_mode(struct net_device *dev)
{
	ioaddr_t ioaddr = dev->base_addr;

	if (dev->flags & IFF_PROMISC)
		outw(SetRxFilter | RxStation | RxMulticast | RxBroadcast | RxProm,
			 ioaddr + EL3_CMD);
	else if (dev->mc_count || (dev->flags & IFF_ALLMULTI))
		outw(SetRxFilter|RxStation|RxMulticast|RxBroadcast, ioaddr + EL3_CMD);
	else
		outw(SetRxFilter | RxStation | RxBroadcast, ioaddr + EL3_CMD);
}

static int el3_close(struct net_device *dev)
{
	ioaddr_t ioaddr = dev->base_addr;
	struct el3_private *lp = dev->priv;
	dev_link_t *link = &lp->link;

	DEBUG(2, "%s: shutting down ethercard.\n", dev->name);
	
	if (DEV_OK(link)) {
		/* Turn off statistics ASAP.  We update lp->stats below. */
		outw(StatsDisable, ioaddr + EL3_CMD);
		
		/* Disable the receiver and transmitter. */
		outw(RxDisable, ioaddr + EL3_CMD);
		outw(TxDisable, ioaddr + EL3_CMD);
		
		/* Note: Switching to window 0 may disable the IRQ. */
		EL3WINDOW(0);
		
		update_stats(dev);
	}

	link->open--;
	netif_stop_queue(dev);
	del_timer(&lp->media);
	if (link->state & DEV_STALE_CONFIG)
		mod_timer(&link->release, jiffies + HZ/20);

	MOD_DEC_USE_COUNT;

	return 0;
}

static int __init init_3c574_cs(void)
{
	servinfo_t serv;

	/* Always emit the version, before any failure. */
	printk(KERN_INFO"%s", tc574_version);
	DEBUG(0, "%s\n", version);
	CardServices(GetCardServicesInfo, &serv);
	if (serv.Revision != CS_RELEASE_CODE) {
		printk(KERN_NOTICE "3c574_cs: Card Services release "
			   "does not match!\n");
		return -1;
	}
	register_pccard_driver(&dev_info, &tc574_attach, &tc574_detach);
	return 0;
}

static void __exit exit_3c574_cs(void)
{
	DEBUG(0, "3c574_cs: unloading\n");
	unregister_pccard_driver(&dev_info);
	while (dev_list != NULL)
		tc574_detach(dev_list);
}

module_init(init_3c574_cs);
module_exit(exit_3c574_cs);

/*
 * Local variables:
 *  compile-command: "make 3c574_cs.o"
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
