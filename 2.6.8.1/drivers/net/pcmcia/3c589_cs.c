/*======================================================================

    A PCMCIA ethernet driver for the 3com 3c589 card.
    
    Copyright (C) 1999 David A. Hinds -- dahinds@users.sourceforge.net

    3c589_cs.c 1.162 2001/10/13 00:08:50

    The network driver code is based on Donald Becker's 3c589 code:
    
    Written 1994 by Donald Becker.
    Copyright 1993 United States Government as represented by the
    Director, National Security Agency.  This software may be used and
    distributed according to the terms of the GNU General Public License,
    incorporated herein by reference.
    Donald Becker may be reached at becker@scyld.com
    
    Updated for 2.5.x by Alan Cox <alan@redhat.com>

======================================================================*/

#define DRV_NAME	"3c589_cs"
#define DRV_VERSION	"1.162-ac"

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
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

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/bitops.h>

/* To minimize the size of the driver source I only define operating
   constants if they are used several times.  You'll need the manual
   if you want to understand driver details. */
/* Offsets from base I/O address. */
#define EL3_DATA	0x00
#define EL3_TIMER	0x0a
#define EL3_CMD		0x0e
#define EL3_STATUS	0x0e

#define EEPROM_READ	0x0080
#define EEPROM_BUSY	0x8000

#define EL3WINDOW(win_num) outw(SelectWindow + (win_num), ioaddr + EL3_CMD)

/* The top five bits written to EL3_CMD are a command, the lower
   11 bits are the parameter, if applicable. */
enum c509cmd {
    TotalReset = 0<<11, SelectWindow = 1<<11, StartCoax = 2<<11,
    RxDisable = 3<<11, RxEnable = 4<<11, RxReset = 5<<11, RxDiscard = 8<<11,
    TxEnable = 9<<11, TxDisable = 10<<11, TxReset = 11<<11,
    FakeIntr = 12<<11, AckIntr = 13<<11, SetIntrEnb = 14<<11,
    SetStatusEnb = 15<<11, SetRxFilter = 16<<11, SetRxThreshold = 17<<11,
    SetTxThreshold = 18<<11, SetTxStart = 19<<11, StatsEnable = 21<<11,
    StatsDisable = 22<<11, StopCoax = 23<<11,
};

enum c509status {
    IntLatch = 0x0001, AdapterFailure = 0x0002, TxComplete = 0x0004,
    TxAvailable = 0x0008, RxComplete = 0x0010, RxEarly = 0x0020,
    IntReq = 0x0040, StatsFull = 0x0080, CmdBusy = 0x1000
};

/* The SetRxFilter command accepts the following classes: */
enum RxFilter {
    RxStation = 1, RxMulticast = 2, RxBroadcast = 4, RxProm = 8
};

/* Register window 1 offsets, the window used in normal operation. */
#define TX_FIFO		0x00
#define RX_FIFO		0x00
#define RX_STATUS 	0x08
#define TX_STATUS 	0x0B
#define TX_FREE		0x0C	/* Remaining free bytes in Tx buffer. */

#define WN0_IRQ		0x08	/* Window 0: Set IRQ line in bits 12-15. */
#define WN4_MEDIA	0x0A	/* Window 4: Various transcvr/media bits. */
#define MEDIA_TP	0x00C0	/* Enable link beat and jabber for 10baseT. */
#define MEDIA_LED	0x0001	/* Enable link light on 3C589E cards. */

/* Time in jiffies before concluding Tx hung */
#define TX_TIMEOUT	((400*HZ)/1000)

struct el3_private {
    dev_link_t		link;
    dev_node_t 		node;
    struct net_device_stats stats;
    /* For transceiver monitoring */
    struct timer_list	media;
    u16			media_status;
    u16			fast_poll;
    unsigned long	last_irq;
    spinlock_t		lock;
};

static char *if_names[] = { "auto", "10baseT", "10base2", "AUI" };

/*====================================================================*/

/* Module parameters */

MODULE_AUTHOR("David Hinds <dahinds@users.sourceforge.net>");
MODULE_DESCRIPTION("3Com 3c589 series PCMCIA ethernet driver");
MODULE_LICENSE("GPL");

#define INT_MODULE_PARM(n, v) static int n = v; MODULE_PARM(n, "i")

/* Special hook for setting if_port when module is loaded */
INT_MODULE_PARM(if_port, 0);

/* Bit map of interrupts to choose from */
INT_MODULE_PARM(irq_mask, 0xdeb8);
static int irq_list[4] = { -1 };
MODULE_PARM(irq_list, "1-4i");

#ifdef PCMCIA_DEBUG
INT_MODULE_PARM(pc_debug, PCMCIA_DEBUG);
#define DEBUG(n, args...) if (pc_debug>(n)) printk(KERN_DEBUG args)
static char *version =
DRV_NAME ".c " DRV_VERSION " 2001/10/13 00:08:50 (David Hinds)";
#else
#define DEBUG(n, args...)
#endif

/*====================================================================*/

static void tc589_config(dev_link_t *link);
static void tc589_release(dev_link_t *link);
static int tc589_event(event_t event, int priority,
		       event_callback_args_t *args);

static u16 read_eeprom(ioaddr_t ioaddr, int index);
static void tc589_reset(struct net_device *dev);
static void media_check(unsigned long arg);
static int el3_config(struct net_device *dev, struct ifmap *map);
static int el3_open(struct net_device *dev);
static int el3_start_xmit(struct sk_buff *skb, struct net_device *dev);
static irqreturn_t el3_interrupt(int irq, void *dev_id, struct pt_regs *regs);
static void update_stats(struct net_device *dev);
static struct net_device_stats *el3_get_stats(struct net_device *dev);
static int el3_rx(struct net_device *dev);
static int el3_close(struct net_device *dev);
static void el3_tx_timeout(struct net_device *dev);
static void set_multicast_list(struct net_device *dev);
static struct ethtool_ops netdev_ethtool_ops;

static dev_info_t dev_info = "3c589_cs";

static dev_link_t *tc589_attach(void);
static void tc589_detach(dev_link_t *);

static dev_link_t *dev_list;

/*======================================================================

    tc589_attach() creates an "instance" of the driver, allocating
    local data structures for one device.  The device is registered
    with Card Services.

======================================================================*/

static dev_link_t *tc589_attach(void)
{
    struct el3_private *lp;
    client_reg_t client_reg;
    dev_link_t *link;
    struct net_device *dev;
    int i, ret;

    DEBUG(0, "3c589_attach()\n");
    
    /* Create new ethernet device */
    dev = alloc_etherdev(sizeof(struct el3_private));
    if (!dev)
	 return NULL;
    lp = netdev_priv(dev);
    link = &lp->link;
    link->priv = dev;

    spin_lock_init(&lp->lock);
    link->io.NumPorts1 = 16;
    link->io.Attributes1 = IO_DATA_PATH_WIDTH_16;
    link->irq.Attributes = IRQ_TYPE_EXCLUSIVE | IRQ_HANDLE_PRESENT;
    link->irq.IRQInfo1 = IRQ_INFO2_VALID|IRQ_LEVEL_ID;
    if (irq_list[0] == -1)
	link->irq.IRQInfo2 = irq_mask;
    else
	for (i = 0; i < 4; i++)
	    link->irq.IRQInfo2 |= 1 << irq_list[i];
    link->irq.Handler = &el3_interrupt;
    link->irq.Instance = dev;
    link->conf.Attributes = CONF_ENABLE_IRQ;
    link->conf.Vcc = 50;
    link->conf.IntType = INT_MEMORY_AND_IO;
    link->conf.ConfigIndex = 1;
    link->conf.Present = PRESENT_OPTION;
    
    /* The EL3-specific entries in the device structure. */
    SET_MODULE_OWNER(dev);
    dev->hard_start_xmit = &el3_start_xmit;
    dev->set_config = &el3_config;
    dev->get_stats = &el3_get_stats;
    dev->set_multicast_list = &set_multicast_list;
    dev->open = &el3_open;
    dev->stop = &el3_close;
#ifdef HAVE_TX_TIMEOUT
    dev->tx_timeout = el3_tx_timeout;
    dev->watchdog_timeo = TX_TIMEOUT;
#endif
    SET_ETHTOOL_OPS(dev, &netdev_ethtool_ops);

    /* Register with Card Services */
    link->next = dev_list;
    dev_list = link;
    client_reg.dev_info = &dev_info;
    client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
    client_reg.EventMask =
	CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
	CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
	CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
    client_reg.event_handler = &tc589_event;
    client_reg.Version = 0x0210;
    client_reg.event_callback_args.client_data = link;
    ret = pcmcia_register_client(&link->handle, &client_reg);
    if (ret != 0) {
	cs_error(link->handle, RegisterClient, ret);
	tc589_detach(link);
	return NULL;
    }
    
    return link;
} /* tc589_attach */

/*======================================================================

    This deletes a driver "instance".  The device is de-registered
    with Card Services.  If it has been released, all local data
    structures are freed.  Otherwise, the structures will be freed
    when the device is released.

======================================================================*/

static void tc589_detach(dev_link_t *link)
{
    struct net_device *dev = link->priv;
    dev_link_t **linkp;
    
    DEBUG(0, "3c589_detach(0x%p)\n", link);
    
    /* Locate device structure */
    for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
	if (*linkp == link) break;
    if (*linkp == NULL)
	return;

    if (link->dev)
	unregister_netdev(dev);

    if (link->state & DEV_CONFIG)
	tc589_release(link);
    
    if (link->handle)
	pcmcia_deregister_client(link->handle);
    
    /* Unlink device structure, free bits */
    *linkp = link->next;
    free_netdev(dev);
} /* tc589_detach */

/*======================================================================

    tc589_config() is scheduled to run after a CARD_INSERTION event
    is received, to configure the PCMCIA socket, and to make the
    ethernet device available to the system.
    
======================================================================*/

#define CS_CHECK(fn, ret) \
do { last_fn = (fn); if ((last_ret = (ret)) != 0) goto cs_failed; } while (0)

static void tc589_config(dev_link_t *link)
{
    client_handle_t handle = link->handle;
    struct net_device *dev = link->priv;
    struct el3_private *lp = netdev_priv(dev);
    tuple_t tuple;
    cisparse_t parse;
    u16 buf[32], *phys_addr;
    int last_fn, last_ret, i, j, multi = 0, fifo;
    ioaddr_t ioaddr;
    char *ram_split[] = {"5:3", "3:1", "1:1", "3:5"};
    
    DEBUG(0, "3c589_config(0x%p)\n", link);

    phys_addr = (u16 *)dev->dev_addr;
    tuple.Attributes = 0;
    tuple.DesiredTuple = CISTPL_CONFIG;
    CS_CHECK(GetFirstTuple, pcmcia_get_first_tuple(handle, &tuple));
    tuple.TupleData = (cisdata_t *)buf;
    tuple.TupleDataMax = sizeof(buf);
    tuple.TupleOffset = 0;
    CS_CHECK(GetTupleData, pcmcia_get_tuple_data(handle, &tuple));
    CS_CHECK(ParseTuple, pcmcia_parse_tuple(handle, &tuple, &parse));
    link->conf.ConfigBase = parse.config.base;
    link->conf.Present = parse.config.rmask[0];
    
    /* Is this a 3c562? */
    tuple.DesiredTuple = CISTPL_MANFID;
    tuple.Attributes = TUPLE_RETURN_COMMON;
    if ((pcmcia_get_first_tuple(handle, &tuple) == CS_SUCCESS) &&
	(pcmcia_get_tuple_data(handle, &tuple) == CS_SUCCESS)) {
	if (le16_to_cpu(buf[0]) != MANFID_3COM)
	    printk(KERN_INFO "3c589_cs: hmmm, is this really a "
		   "3Com card??\n");
	multi = (le16_to_cpu(buf[1]) == PRODID_3COM_3C562);
    }
    
    /* Configure card */
    link->state |= DEV_CONFIG;

    /* For the 3c562, the base address must be xx00-xx7f */
    link->io.IOAddrLines = 16;
    for (i = j = 0; j < 0x400; j += 0x10) {
	if (multi && (j & 0x80)) continue;
	link->io.BasePort1 = j ^ 0x300;
	i = pcmcia_request_io(link->handle, &link->io);
	if (i == CS_SUCCESS) break;
    }
    if (i != CS_SUCCESS) {
	cs_error(link->handle, RequestIO, i);
	goto failed;
    }
    CS_CHECK(RequestIRQ, pcmcia_request_irq(link->handle, &link->irq));
    CS_CHECK(RequestConfiguration, pcmcia_request_configuration(link->handle, &link->conf));
	
    dev->irq = link->irq.AssignedIRQ;
    dev->base_addr = link->io.BasePort1;
    ioaddr = dev->base_addr;
    EL3WINDOW(0);

    /* The 3c589 has an extra EEPROM for configuration info, including
       the hardware address.  The 3c562 puts the address in the CIS. */
    tuple.DesiredTuple = 0x88;
    if (pcmcia_get_first_tuple(handle, &tuple) == CS_SUCCESS) {
	pcmcia_get_tuple_data(handle, &tuple);
	for (i = 0; i < 3; i++)
	    phys_addr[i] = htons(buf[i]);
    } else {
	for (i = 0; i < 3; i++)
	    phys_addr[i] = htons(read_eeprom(ioaddr, i));
	if (phys_addr[0] == 0x6060) {
	    printk(KERN_ERR "3c589_cs: IO port conflict at 0x%03lx"
		   "-0x%03lx\n", dev->base_addr, dev->base_addr+15);
	    goto failed;
	}
    }

    /* The address and resource configuration register aren't loaded from
       the EEPROM and *must* be set to 0 and IRQ3 for the PCMCIA version. */
    outw(0x3f00, ioaddr + 8);
    fifo = inl(ioaddr);

    /* The if_port symbol can be set when the module is loaded */
    if ((if_port >= 0) && (if_port <= 3))
	dev->if_port = if_port;
    else
	printk(KERN_ERR "3c589_cs: invalid if_port requested\n");
    
    link->dev = &lp->node;
    link->state &= ~DEV_CONFIG_PENDING;

    if (register_netdev(dev) != 0) {
	printk(KERN_ERR "3c589_cs: register_netdev() failed\n");
	link->dev = NULL;
	goto failed;
    }

    strcpy(lp->node.dev_name, dev->name);

    printk(KERN_INFO "%s: 3Com 3c%s, io %#3lx, irq %d, hw_addr ",
	   dev->name, (multi ? "562" : "589"), dev->base_addr,
	   dev->irq);
    for (i = 0; i < 6; i++)
	printk("%02X%s", dev->dev_addr[i], ((i<5) ? ":" : "\n"));
    printk(KERN_INFO "  %dK FIFO split %s Rx:Tx, %s xcvr\n",
	   (fifo & 7) ? 32 : 8, ram_split[(fifo >> 16) & 3],
	   if_names[dev->if_port]);
    return;

cs_failed:
    cs_error(link->handle, last_fn, last_ret);
failed:
    tc589_release(link);
    return;
    
} /* tc589_config */

/*======================================================================

    After a card is removed, tc589_release() will unregister the net
    device, and release the PCMCIA configuration.  If the device is
    still open, this will be postponed until it is closed.
    
======================================================================*/

static void tc589_release(dev_link_t *link)
{
    DEBUG(0, "3c589_release(0x%p)\n", link);
    
    pcmcia_release_configuration(link->handle);
    pcmcia_release_io(link->handle, &link->io);
    pcmcia_release_irq(link->handle, &link->irq);
    
    link->state &= ~DEV_CONFIG;
}

/*======================================================================

    The card status event handler.  Mostly, this schedules other
    stuff to run after an event is received.  A CARD_REMOVAL event
    also sets some flags to discourage the net drivers from trying
    to talk to the card any more.
    
======================================================================*/

static int tc589_event(event_t event, int priority,
		       event_callback_args_t *args)
{
    dev_link_t *link = args->client_data;
    struct net_device *dev = link->priv;
    
    DEBUG(1, "3c589_event(0x%06x)\n", event);
    
    switch (event) {
    case CS_EVENT_CARD_REMOVAL:
	link->state &= ~DEV_PRESENT;
	if (link->state & DEV_CONFIG)
	    netif_device_detach(dev);
	break;
    case CS_EVENT_CARD_INSERTION:
	link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
	tc589_config(link);
	break;
    case CS_EVENT_PM_SUSPEND:
	link->state |= DEV_SUSPEND;
	/* Fall through... */
    case CS_EVENT_RESET_PHYSICAL:
	if (link->state & DEV_CONFIG) {
	    if (link->open)
		netif_device_detach(dev);
	    pcmcia_release_configuration(link->handle);
	}
	break;
    case CS_EVENT_PM_RESUME:
	link->state &= ~DEV_SUSPEND;
	/* Fall through... */
    case CS_EVENT_CARD_RESET:
	if (link->state & DEV_CONFIG) {
	    pcmcia_request_configuration(link->handle, &link->conf);
	    if (link->open) {
		tc589_reset(dev);
		netif_device_attach(dev);
	    }
	}
	break;
    }
    return 0;
} /* tc589_event */

/*====================================================================*/

/*
  Use this for commands that may take time to finish
*/
static void tc589_wait_for_completion(struct net_device *dev, int cmd)
{
    int i = 100;
    outw(cmd, dev->base_addr + EL3_CMD);
    while (--i > 0)
	if (!(inw(dev->base_addr + EL3_STATUS) & 0x1000)) break;
    if (i == 0)
	printk(KERN_WARNING "%s: command 0x%04x did not complete!\n",
	       dev->name, cmd);
}

/*
  Read a word from the EEPROM using the regular EEPROM access register.
  Assume that we are in register window zero.
*/
static u16 read_eeprom(ioaddr_t ioaddr, int index)
{
    int i;
    outw(EEPROM_READ + index, ioaddr + 10);
    /* Reading the eeprom takes 162 us */
    for (i = 1620; i >= 0; i--)
	if ((inw(ioaddr + 10) & EEPROM_BUSY) == 0)
	    break;
    return inw(ioaddr + 12);
}

/*
  Set transceiver type, perhaps to something other than what the user
  specified in dev->if_port.
*/
static void tc589_set_xcvr(struct net_device *dev, int if_port)
{
    struct el3_private *lp = netdev_priv(dev);
    ioaddr_t ioaddr = dev->base_addr;
    
    EL3WINDOW(0);
    switch (if_port) {
    case 0: case 1: outw(0, ioaddr + 6); break;
    case 2: outw(3<<14, ioaddr + 6); break;
    case 3: outw(1<<14, ioaddr + 6); break;
    }
    /* On PCMCIA, this just turns on the LED */
    outw((if_port == 2) ? StartCoax : StopCoax, ioaddr + EL3_CMD);
    /* 10baseT interface, enable link beat and jabber check. */
    EL3WINDOW(4);
    outw(MEDIA_LED | ((if_port < 2) ? MEDIA_TP : 0), ioaddr + WN4_MEDIA);
    EL3WINDOW(1);
    if (if_port == 2)
	lp->media_status = ((dev->if_port == 0) ? 0x8000 : 0x4000);
    else
	lp->media_status = ((dev->if_port == 0) ? 0x4010 : 0x8800);
}

static void dump_status(struct net_device *dev)
{
    ioaddr_t ioaddr = dev->base_addr;
    EL3WINDOW(1);
    printk(KERN_INFO "  irq status %04x, rx status %04x, tx status "
	   "%02x  tx free %04x\n", inw(ioaddr+EL3_STATUS),
	   inw(ioaddr+RX_STATUS), inb(ioaddr+TX_STATUS),
	   inw(ioaddr+TX_FREE));
    EL3WINDOW(4);
    printk(KERN_INFO "  diagnostics: fifo %04x net %04x ethernet %04x"
	   " media %04x\n", inw(ioaddr+0x04), inw(ioaddr+0x06),
	   inw(ioaddr+0x08), inw(ioaddr+0x0a));
    EL3WINDOW(1);
}

/* Reset and restore all of the 3c589 registers. */
static void tc589_reset(struct net_device *dev)
{
    ioaddr_t ioaddr = dev->base_addr;
    int i;
    
    EL3WINDOW(0);
    outw(0x0001, ioaddr + 4);			/* Activate board. */ 
    outw(0x3f00, ioaddr + 8);			/* Set the IRQ line. */
    
    /* Set the station address in window 2. */
    EL3WINDOW(2);
    for (i = 0; i < 6; i++)
	outb(dev->dev_addr[i], ioaddr + i);

    tc589_set_xcvr(dev, dev->if_port);
    
    /* Switch to the stats window, and clear all stats by reading. */
    outw(StatsDisable, ioaddr + EL3_CMD);
    EL3WINDOW(6);
    for (i = 0; i < 9; i++)
	inb(ioaddr+i);
    inw(ioaddr + 10);
    inw(ioaddr + 12);
    
    /* Switch to register set 1 for normal use. */
    EL3WINDOW(1);

    /* Accept b-cast and phys addr only. */
    outw(SetRxFilter | RxStation | RxBroadcast, ioaddr + EL3_CMD);
    outw(StatsEnable, ioaddr + EL3_CMD); /* Turn on statistics. */
    outw(RxEnable, ioaddr + EL3_CMD); /* Enable the receiver. */
    outw(TxEnable, ioaddr + EL3_CMD); /* Enable transmitter. */
    /* Allow status bits to be seen. */
    outw(SetStatusEnb | 0xff, ioaddr + EL3_CMD);
    /* Ack all pending events, and set active indicator mask. */
    outw(AckIntr | IntLatch | TxAvailable | RxEarly | IntReq,
	 ioaddr + EL3_CMD);
    outw(SetIntrEnb | IntLatch | TxAvailable | RxComplete | StatsFull
	 | AdapterFailure, ioaddr + EL3_CMD);
}

static void netdev_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strcpy(info->driver, DRV_NAME);
	strcpy(info->version, DRV_VERSION);
	sprintf(info->bus_info, "PCMCIA 0x%lx", dev->base_addr);
}

#ifdef PCMCIA_DEBUG
static u32 netdev_get_msglevel(struct net_device *dev)
{
	return pc_debug;
}

static void netdev_set_msglevel(struct net_device *dev, u32 level)
{
	pc_debug = level;
}
#endif /* PCMCIA_DEBUG */

static struct ethtool_ops netdev_ethtool_ops = {
	.get_drvinfo		= netdev_get_drvinfo,
#ifdef PCMCIA_DEBUG
	.get_msglevel		= netdev_get_msglevel,
	.set_msglevel		= netdev_set_msglevel,
#endif /* PCMCIA_DEBUG */
};

static int el3_config(struct net_device *dev, struct ifmap *map)
{
    if ((map->port != (u_char)(-1)) && (map->port != dev->if_port)) {
	if (map->port <= 3) {
	    dev->if_port = map->port;
	    printk(KERN_INFO "%s: switched to %s port\n",
		   dev->name, if_names[dev->if_port]);
	    tc589_set_xcvr(dev, dev->if_port);
	} else
	    return -EINVAL;
    }
    return 0;
}

static int el3_open(struct net_device *dev)
{
    struct el3_private *lp = netdev_priv(dev);
    dev_link_t *link = &lp->link;
    
    if (!DEV_OK(link))
	return -ENODEV;

    link->open++;
    netif_start_queue(dev);
    
    tc589_reset(dev);
    init_timer(&lp->media);
    lp->media.function = &media_check;
    lp->media.data = (unsigned long) dev;
    lp->media.expires = jiffies + HZ;
    add_timer(&lp->media);

    DEBUG(1, "%s: opened, status %4.4x.\n",
	  dev->name, inw(dev->base_addr + EL3_STATUS));
    
    return 0;
}

static void el3_tx_timeout(struct net_device *dev)
{
    struct el3_private *lp = netdev_priv(dev);
    ioaddr_t ioaddr = dev->base_addr;
    
    printk(KERN_WARNING "%s: Transmit timed out!\n", dev->name);
    dump_status(dev);
    lp->stats.tx_errors++;
    dev->trans_start = jiffies;
    /* Issue TX_RESET and TX_START commands. */
    tc589_wait_for_completion(dev, TxReset);
    outw(TxEnable, ioaddr + EL3_CMD);
    netif_wake_queue(dev);
}

static void pop_tx_status(struct net_device *dev)
{
    struct el3_private *lp = netdev_priv(dev);
    ioaddr_t ioaddr = dev->base_addr;
    int i;
    
    /* Clear the Tx status stack. */
    for (i = 32; i > 0; i--) {
	u_char tx_status = inb(ioaddr + TX_STATUS);
	if (!(tx_status & 0x84)) break;
	/* reset transmitter on jabber error or underrun */
	if (tx_status & 0x30)
	    tc589_wait_for_completion(dev, TxReset);
	if (tx_status & 0x38) {
	    DEBUG(1, "%s: transmit error: status 0x%02x\n",
		  dev->name, tx_status);
	    outw(TxEnable, ioaddr + EL3_CMD);
	    lp->stats.tx_aborted_errors++;
	}
	outb(0x00, ioaddr + TX_STATUS); /* Pop the status stack. */
    }
}

static int el3_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    ioaddr_t ioaddr = dev->base_addr;
    struct el3_private *priv = netdev_priv(dev);

    DEBUG(3, "%s: el3_start_xmit(length = %ld) called, "
	  "status %4.4x.\n", dev->name, (long)skb->len,
	  inw(ioaddr + EL3_STATUS));

    priv->stats.tx_bytes += skb->len;

    /* Put out the doubleword header... */
    outw(skb->len, ioaddr + TX_FIFO);
    outw(0x00, ioaddr + TX_FIFO);
    /* ... and the packet rounded to a doubleword. */
    outsl(ioaddr + TX_FIFO, skb->data, (skb->len + 3) >> 2);

    dev->trans_start = jiffies;
    if (inw(ioaddr + TX_FREE) <= 1536) {
	netif_stop_queue(dev);
	/* Interrupt us when the FIFO has room for max-sized packet. */
	outw(SetTxThreshold + 1536, ioaddr + EL3_CMD);
    }

    dev_kfree_skb(skb);
    pop_tx_status(dev);
    
    return 0;
}

/* The EL3 interrupt handler. */
static irqreturn_t el3_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    struct net_device *dev = (struct net_device *) dev_id;
    struct el3_private *lp = netdev_priv(dev);
    ioaddr_t ioaddr, status;
    int i = 0, handled = 1;
    
    if (!netif_device_present(dev))
	return IRQ_NONE;

    ioaddr = dev->base_addr;

    DEBUG(3, "%s: interrupt, status %4.4x.\n",
	  dev->name, inw(ioaddr + EL3_STATUS));

    spin_lock(&lp->lock);    
    while ((status = inw(ioaddr + EL3_STATUS)) &
	(IntLatch | RxComplete | StatsFull)) {
	if ((status & 0xe000) != 0x2000) {
	    DEBUG(1, "%s: interrupt from dead card\n", dev->name);
	    handled = 0;
	    break;
	}
	
	if (status & RxComplete)
	    el3_rx(dev);
	
	if (status & TxAvailable) {
	    DEBUG(3, "    TX room bit was handled.\n");
	    /* There's room in the FIFO for a full-sized packet. */
	    outw(AckIntr | TxAvailable, ioaddr + EL3_CMD);
	    netif_wake_queue(dev);
	}
	
	if (status & TxComplete)
	    pop_tx_status(dev);

	if (status & (AdapterFailure | RxEarly | StatsFull)) {
	    /* Handle all uncommon interrupts. */
	    if (status & StatsFull)		/* Empty statistics. */
		update_stats(dev);
	    if (status & RxEarly) {		/* Rx early is unused. */
		el3_rx(dev);
		outw(AckIntr | RxEarly, ioaddr + EL3_CMD);
	    }
	    if (status & AdapterFailure) {
		u16 fifo_diag;
		EL3WINDOW(4);
		fifo_diag = inw(ioaddr + 4);
		EL3WINDOW(1);
		printk(KERN_WARNING "%s: adapter failure, FIFO diagnostic"
		       " register %04x.\n", dev->name, fifo_diag);
		if (fifo_diag & 0x0400) {
		    /* Tx overrun */
		    tc589_wait_for_completion(dev, TxReset);
		    outw(TxEnable, ioaddr + EL3_CMD);
		}
		if (fifo_diag & 0x2000) {
		    /* Rx underrun */
		    tc589_wait_for_completion(dev, RxReset);
		    set_multicast_list(dev);
		    outw(RxEnable, ioaddr + EL3_CMD);
		}
		outw(AckIntr | AdapterFailure, ioaddr + EL3_CMD);
	    }
	}
	
	if (++i > 10) {
	    printk(KERN_ERR "%s: infinite loop in interrupt, "
		   "status %4.4x.\n", dev->name, status);
	    /* Clear all interrupts */
	    outw(AckIntr | 0xFF, ioaddr + EL3_CMD);
	    break;
	}
	/* Acknowledge the IRQ. */
	outw(AckIntr | IntReq | IntLatch, ioaddr + EL3_CMD);
    }

    lp->last_irq = jiffies;
    spin_unlock(&lp->lock);    
    DEBUG(3, "%s: exiting interrupt, status %4.4x.\n",
	  dev->name, inw(ioaddr + EL3_STATUS));
    return IRQ_RETVAL(handled);
}

static void media_check(unsigned long arg)
{
    struct net_device *dev = (struct net_device *)(arg);
    struct el3_private *lp = netdev_priv(dev);
    ioaddr_t ioaddr = dev->base_addr;
    u16 media, errs;
    unsigned long flags;

    if (!netif_device_present(dev)) goto reschedule;

    EL3WINDOW(1);
    /* Check for pending interrupt with expired latency timer: with
       this, we can limp along even if the interrupt is blocked */
    if ((inw(ioaddr + EL3_STATUS) & IntLatch) &&
	(inb(ioaddr + EL3_TIMER) == 0xff)) {
	if (!lp->fast_poll)
	    printk(KERN_WARNING "%s: interrupt(s) dropped!\n", dev->name);
	el3_interrupt(dev->irq, lp, NULL);
	lp->fast_poll = HZ;
    }
    if (lp->fast_poll) {
	lp->fast_poll--;
	lp->media.expires = jiffies + HZ/100;
	add_timer(&lp->media);
	return;
    }

    /* lp->lock guards the EL3 window. Window should always be 1 except
       when the lock is held */
    spin_lock_irqsave(&lp->lock, flags);    
    EL3WINDOW(4);
    media = inw(ioaddr+WN4_MEDIA) & 0xc810;

    /* Ignore collisions unless we've had no irq's recently */
    if (jiffies - lp->last_irq < HZ) {
	media &= ~0x0010;
    } else {
	/* Try harder to detect carrier errors */
	EL3WINDOW(6);
	outw(StatsDisable, ioaddr + EL3_CMD);
	errs = inb(ioaddr + 0);
	outw(StatsEnable, ioaddr + EL3_CMD);
	lp->stats.tx_carrier_errors += errs;
	if (errs || (lp->media_status & 0x0010)) media |= 0x0010;
    }

    if (media != lp->media_status) {
	if ((media & lp->media_status & 0x8000) &&
	    ((lp->media_status ^ media) & 0x0800))
	    printk(KERN_INFO "%s: %s link beat\n", dev->name,
		   (lp->media_status & 0x0800 ? "lost" : "found"));
	else if ((media & lp->media_status & 0x4000) &&
		 ((lp->media_status ^ media) & 0x0010))
	    printk(KERN_INFO "%s: coax cable %s\n", dev->name,
		   (lp->media_status & 0x0010 ? "ok" : "problem"));
	if (dev->if_port == 0) {
	    if (media & 0x8000) {
		if (media & 0x0800)
		    printk(KERN_INFO "%s: flipped to 10baseT\n",
			   dev->name);
		else
		    tc589_set_xcvr(dev, 2);
	    } else if (media & 0x4000) {
		if (media & 0x0010)
		    tc589_set_xcvr(dev, 1);
		else
		    printk(KERN_INFO "%s: flipped to 10base2\n",
			   dev->name);
	    }
	}
	lp->media_status = media;
    }
    
    EL3WINDOW(1);
    spin_unlock_irqrestore(&lp->lock, flags);    

reschedule:
    lp->media.expires = jiffies + HZ;
    add_timer(&lp->media);
}

static struct net_device_stats *el3_get_stats(struct net_device *dev)
{
    struct el3_private *lp = netdev_priv(dev);
    unsigned long flags;
    dev_link_t *link = &lp->link;

    if (DEV_OK(link)) {
    	spin_lock_irqsave(&lp->lock, flags);
	update_stats(dev);
	spin_unlock_irqrestore(&lp->lock, flags);
    }
    return &lp->stats;
}

/*
  Update statistics.  We change to register window 6, so this should be run
  single-threaded if the device is active. This is expected to be a rare
  operation, and it's simpler for the rest of the driver to assume that
  window 1 is always valid rather than use a special window-state variable.
  
  Caller must hold the lock for this
*/
static void update_stats(struct net_device *dev)
{
    struct el3_private *lp = netdev_priv(dev);
    ioaddr_t ioaddr = dev->base_addr;

    DEBUG(2, "%s: updating the statistics.\n", dev->name);
    /* Turn off statistics updates while reading. */
    outw(StatsDisable, ioaddr + EL3_CMD);
    /* Switch to the stats window, and read everything. */
    EL3WINDOW(6);
    lp->stats.tx_carrier_errors 	+= inb(ioaddr + 0);
    lp->stats.tx_heartbeat_errors	+= inb(ioaddr + 1);
    /* Multiple collisions. */	   	inb(ioaddr + 2);
    lp->stats.collisions		+= inb(ioaddr + 3);
    lp->stats.tx_window_errors		+= inb(ioaddr + 4);
    lp->stats.rx_fifo_errors		+= inb(ioaddr + 5);
    lp->stats.tx_packets		+= inb(ioaddr + 6);
    /* Rx packets   */			inb(ioaddr + 7);
    /* Tx deferrals */			inb(ioaddr + 8);
    /* Rx octets */			inw(ioaddr + 10);
    /* Tx octets */			inw(ioaddr + 12);
    
    /* Back to window 1, and turn statistics back on. */
    EL3WINDOW(1);
    outw(StatsEnable, ioaddr + EL3_CMD);
}

static int el3_rx(struct net_device *dev)
{
    struct el3_private *lp = netdev_priv(dev);
    ioaddr_t ioaddr = dev->base_addr;
    int worklimit = 32;
    short rx_status;
    
    DEBUG(3, "%s: in rx_packet(), status %4.4x, rx_status %4.4x.\n",
	  dev->name, inw(ioaddr+EL3_STATUS), inw(ioaddr+RX_STATUS));
    while (!((rx_status = inw(ioaddr + RX_STATUS)) & 0x8000) &&
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
	    
	    DEBUG(3, "    Receiving packet size %d status %4.4x.\n",
		  pkt_len, rx_status);
	    if (skb != NULL) {
		skb->dev = dev;
		skb_reserve(skb, 2);
		insl(ioaddr+RX_FIFO, skb_put(skb, pkt_len),
			(pkt_len+3)>>2);
		skb->protocol = eth_type_trans(skb, dev);
		netif_rx(skb);
		dev->last_rx = jiffies;
		lp->stats.rx_packets++;
		lp->stats.rx_bytes += pkt_len;
	    } else {
		DEBUG(1, "%s: couldn't allocate a sk_buff of"
		      " size %d.\n", dev->name, pkt_len);
		lp->stats.rx_dropped++;
	    }
	}
	/* Pop the top of the Rx FIFO */
	tc589_wait_for_completion(dev, RxDiscard);
    }
    if (worklimit == 0)
	printk(KERN_WARNING "%s: too much work in el3_rx!\n", dev->name);
    return 0;
}

static void set_multicast_list(struct net_device *dev)
{
    struct el3_private *lp = netdev_priv(dev);
    dev_link_t *link = &lp->link;
    ioaddr_t ioaddr = dev->base_addr;
    u16 opts = SetRxFilter | RxStation | RxBroadcast;

    if (!(DEV_OK(link))) return;
    if (dev->flags & IFF_PROMISC)
	opts |= RxMulticast | RxProm;
    else if (dev->mc_count || (dev->flags & IFF_ALLMULTI))
	opts |= RxMulticast;
    outw(opts, ioaddr + EL3_CMD);
}

static int el3_close(struct net_device *dev)
{
    struct el3_private *lp = netdev_priv(dev);
    dev_link_t *link = &lp->link;
    ioaddr_t ioaddr = dev->base_addr;
    
    DEBUG(1, "%s: shutting down ethercard.\n", dev->name);

    if (DEV_OK(link)) {
	/* Turn off statistics ASAP.  We update lp->stats below. */
	outw(StatsDisable, ioaddr + EL3_CMD);
	
	/* Disable the receiver and transmitter. */
	outw(RxDisable, ioaddr + EL3_CMD);
	outw(TxDisable, ioaddr + EL3_CMD);
	
	if (dev->if_port == 2)
	    /* Turn off thinnet power.  Green! */
	    outw(StopCoax, ioaddr + EL3_CMD);
	else if (dev->if_port == 1) {
	    /* Disable link beat and jabber */
	    EL3WINDOW(4);
	    outw(0, ioaddr + WN4_MEDIA);
	}
	
	/* Switching back to window 0 disables the IRQ. */
	EL3WINDOW(0);
	/* But we explicitly zero the IRQ line select anyway. */
	outw(0x0f00, ioaddr + WN0_IRQ);
        
	/* Check if the card still exists */
	if ((inw(ioaddr+EL3_STATUS) & 0xe000) == 0x2000)
	    update_stats(dev);
    }

    link->open--;
    netif_stop_queue(dev);
    del_timer_sync(&lp->media);
    
    return 0;
}

static struct pcmcia_driver tc589_driver = {
	.owner		= THIS_MODULE,
	.drv		= {
		.name	= "3c589_cs",
	},
	.attach		= tc589_attach,
	.detach		= tc589_detach,
};

static int __init init_tc589(void)
{
	return pcmcia_register_driver(&tc589_driver);
}

static void __exit exit_tc589(void)
{
	pcmcia_unregister_driver(&tc589_driver);
	while (dev_list != NULL)
		tc589_detach(dev_list);
}

module_init(init_tc589);
module_exit(exit_tc589);
