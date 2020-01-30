/*======================================================================

    A PCMCIA token-ring driver for IBM-based cards

    This driver supports the IBM PCMCIA Token-Ring Card.
    Written by Steve Kipisz, kipisz@vnet.ibm.com or
                             bungy@ibm.net

    Written 1995,1996.

    This code is based on pcnet_cs.c from David Hinds.
    
    V2.2.0 February 1999 - Mike Phillips phillim@amtrak.com

    Linux V2.2.x presented significant changes to the underlying
    ibmtr.c code.  Mainly the code became a lot more organized and
    modular.

    This caused the old PCMCIA Token Ring driver to give up and go 
    home early. Instead of just patching the old code to make it 
    work, the PCMCIA code has been streamlined, updated and possibly
    improved.

    This code now only contains code required for the Card Services.
    All we do here is set the card up enough so that the real ibmtr.c
    driver can find it and work with it properly.

    i.e. We set up the io port, irq, mmio memory and shared ram
    memory.  This enables ibmtr_probe in ibmtr.c to find the card and
    configure it as though it was a normal ISA and/or PnP card.

    CHANGES

    v2.2.5 April 1999 Mike Phillips (phillim@amtrak.com)
    Obscure bug fix, required changed to ibmtr.c not ibmtr_cs.c
    
    v2.2.7 May 1999 Mike Phillips (phillim@amtrak.com)
    Updated to version 2.2.7 to match the first version of the kernel
    that the modification to ibmtr.c were incorporated into.
    
    v2.2.17 July 2000 Burt Silverman (burts@us.ibm.com)
    Address translation feature of PCMCIA controller is usable so
    memory windows can be placed in High memory (meaning above
    0xFFFFF.)

======================================================================*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/module.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/trdevice.h>
#include <linux/ibmtr.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ds.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/system.h>

#define PCMCIA
#include "../tokenring/ibmtr.c"

#ifdef PCMCIA_DEBUG
static int pc_debug = PCMCIA_DEBUG;
MODULE_PARM(pc_debug, "i");
#define DEBUG(n, args...) if (pc_debug>(n)) printk(KERN_DEBUG args)
static char *version =
"ibmtr_cs.c 1.10   1996/01/06 05:19:00 (Steve Kipisz)\n"
"           2.2.7  1999/05/03 12:00:00 (Mike Phillips)\n"
"           2.4.2  2001/30/28 Midnight (Burt Silverman)\n";
#else
#define DEBUG(n, args...)
#endif

/*====================================================================*/

/* Parameters that can be set with 'insmod' */

/* Bit map of interrupts to choose from */
static u_int irq_mask = 0xdeb8;
static int irq_list[4] = { -1 };

/* MMIO base address */
static u_long mmiobase = 0xce000;

/* SRAM base address */
static u_long srambase = 0xd0000;

/* SRAM size 8,16,32,64 */
static u_long sramsize = 64;

/* Ringspeed 4,16 */
static int ringspeed = 16;

MODULE_PARM(irq_mask, "i");
MODULE_PARM(irq_list, "1-4i");
MODULE_PARM(mmiobase, "i");
MODULE_PARM(srambase, "i");
MODULE_PARM(sramsize, "i");
MODULE_PARM(ringspeed, "i");
MODULE_LICENSE("GPL");

/*====================================================================*/

static void ibmtr_config(dev_link_t *link);
static void ibmtr_hw_setup(struct net_device *dev, u_int mmiobase);
static void ibmtr_release(dev_link_t *link);
static int ibmtr_event(event_t event, int priority,
                       event_callback_args_t *args);

static dev_info_t dev_info = "ibmtr_cs";

static dev_link_t *ibmtr_attach(void);
static void ibmtr_detach(dev_link_t *);

static dev_link_t *dev_list;

extern int ibmtr_probe_card(struct net_device *dev);
extern irqreturn_t tok_interrupt (int irq, void *dev_id, struct pt_regs *regs);

/*====================================================================*/

typedef struct ibmtr_dev_t {
    dev_link_t		link;
    struct net_device	*dev;
    dev_node_t          node;
    window_handle_t     sram_win_handle;
    struct tok_info	*ti;
} ibmtr_dev_t;

static void netdev_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strcpy(info->driver, "ibmtr_cs");
}

static struct ethtool_ops netdev_ethtool_ops = {
	.get_drvinfo		= netdev_get_drvinfo,
};

/*======================================================================

    ibmtr_attach() creates an "instance" of the driver, allocating
    local data structures for one device.  The device is registered
    with Card Services.

======================================================================*/

static dev_link_t *ibmtr_attach(void)
{
    ibmtr_dev_t *info;
    dev_link_t *link;
    struct net_device *dev;
    client_reg_t client_reg;
    int i, ret;
    
    DEBUG(0, "ibmtr_attach()\n");

    /* Create new token-ring device */
    info = kmalloc(sizeof(*info), GFP_KERNEL); 
    if (!info) return NULL;
    memset(info,0,sizeof(*info));
    dev = alloc_trdev(sizeof(struct tok_info));
    if (!dev) { 
	kfree(info); 
	return NULL;
    } 

    link = &info->link;
    link->priv = info;
    info->ti = netdev_priv(dev);

    link->io.Attributes1 = IO_DATA_PATH_WIDTH_8;
    link->io.NumPorts1 = 4;
    link->io.IOAddrLines = 16;
    link->irq.Attributes = IRQ_TYPE_EXCLUSIVE | IRQ_HANDLE_PRESENT;
    link->irq.IRQInfo1 = IRQ_INFO2_VALID|IRQ_LEVEL_ID;
    if (irq_list[0] == -1)
	link->irq.IRQInfo2 = irq_mask;
    else
	for (i = 0; i < 4; i++)
	    link->irq.IRQInfo2 |= 1 << irq_list[i];
    link->irq.Handler = &tok_interrupt;
    link->conf.Attributes = CONF_ENABLE_IRQ;
    link->conf.Vcc = 50;
    link->conf.IntType = INT_MEMORY_AND_IO;
    link->conf.Present = PRESENT_OPTION;

    link->irq.Instance = info->dev = dev;
    
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
    client_reg.event_handler = &ibmtr_event;
    client_reg.Version = 0x0210;
    client_reg.event_callback_args.client_data = link;
    ret = pcmcia_register_client(&link->handle, &client_reg);
    if (ret != 0) {
        cs_error(link->handle, RegisterClient, ret);
	goto out_detach;
    }

out:
    return link;

out_detach:
    ibmtr_detach(link);
    link = NULL;
    goto out;
} /* ibmtr_attach */

/*======================================================================

    This deletes a driver "instance".  The device is de-registered
    with Card Services.  If it has been released, all local data
    structures are freed.  Otherwise, the structures will be freed
    when the device is released.

======================================================================*/

static void ibmtr_detach(dev_link_t *link)
{
    struct ibmtr_dev_t *info = link->priv;
    dev_link_t **linkp;
    struct net_device *dev;

    DEBUG(0, "ibmtr_detach(0x%p)\n", link);

    /* Locate device structure */
    for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
        if (*linkp == link) break;
    if (*linkp == NULL)
        return;

    dev = info->dev;

    if (link->dev)
	unregister_netdev(dev);

    {
	struct tok_info *ti = netdev_priv(dev);
	del_timer_sync(&(ti->tr_timer));
    }
    if (link->state & DEV_CONFIG)
        ibmtr_release(link);

    if (link->handle)
        pcmcia_deregister_client(link->handle);

    /* Unlink device structure, free bits */
    *linkp = link->next;
    free_netdev(dev);
    kfree(info); 
} /* ibmtr_detach */

/*======================================================================

    ibmtr_config() is scheduled to run after a CARD_INSERTION event
    is received, to configure the PCMCIA socket, and to make the
    token-ring device available to the system.

======================================================================*/

#define CS_CHECK(fn, ret) \
do { last_fn = (fn); if ((last_ret = (ret)) != 0) goto cs_failed; } while (0)

static void ibmtr_config(dev_link_t *link)
{
    client_handle_t handle = link->handle;
    ibmtr_dev_t *info = link->priv;
    struct net_device *dev = info->dev;
    struct tok_info *ti = netdev_priv(dev);
    tuple_t tuple;
    cisparse_t parse;
    win_req_t req;
    memreq_t mem;
    int i, last_ret, last_fn;
    u_char buf[64];

    DEBUG(0, "ibmtr_config(0x%p)\n", link);

    tuple.Attributes = 0;
    tuple.TupleData = buf;
    tuple.TupleDataMax = 64;
    tuple.TupleOffset = 0;
    tuple.DesiredTuple = CISTPL_CONFIG;
    CS_CHECK(GetFirstTuple, pcmcia_get_first_tuple(handle, &tuple));
    CS_CHECK(GetTupleData, pcmcia_get_tuple_data(handle, &tuple));
    CS_CHECK(ParseTuple, pcmcia_parse_tuple(handle, &tuple, &parse));
    link->conf.ConfigBase = parse.config.base;

    /* Configure card */
    link->state |= DEV_CONFIG;

    link->conf.ConfigIndex = 0x61;

    /* Determine if this is PRIMARY or ALTERNATE. */

    /* Try PRIMARY card at 0xA20-0xA23 */
    link->io.BasePort1 = 0xA20;
    i = pcmcia_request_io(link->handle, &link->io);
    if (i != CS_SUCCESS) {
	/* Couldn't get 0xA20-0xA23.  Try ALTERNATE at 0xA24-0xA27. */
	link->io.BasePort1 = 0xA24;
	CS_CHECK(RequestIO, pcmcia_request_io(link->handle, &link->io));
    }
    dev->base_addr = link->io.BasePort1;

    CS_CHECK(RequestIRQ, pcmcia_request_irq(link->handle, &link->irq));
    dev->irq = link->irq.AssignedIRQ;
    ti->irq = link->irq.AssignedIRQ;
    ti->global_int_enable=GLOBAL_INT_ENABLE+((dev->irq==9) ? 2 : dev->irq);

    /* Allocate the MMIO memory window */
    req.Attributes = WIN_DATA_WIDTH_16|WIN_MEMORY_TYPE_CM|WIN_ENABLE;
    req.Attributes |= WIN_USE_WAIT;
    req.Base = 0; 
    req.Size = 0x2000;
    req.AccessSpeed = 250;
    CS_CHECK(RequestWindow, pcmcia_request_window(&link->handle, &req, &link->win));

    mem.CardOffset = mmiobase;
    mem.Page = 0;
    CS_CHECK(MapMemPage, pcmcia_map_mem_page(link->win, &mem));
    ti->mmio = ioremap(req.Base, req.Size);

    /* Allocate the SRAM memory window */
    req.Attributes = WIN_DATA_WIDTH_16|WIN_MEMORY_TYPE_CM|WIN_ENABLE;
    req.Attributes |= WIN_USE_WAIT;
    req.Base = 0;
    req.Size = sramsize * 1024;
    req.AccessSpeed = 250;
    CS_CHECK(RequestWindow, pcmcia_request_window(&link->handle, &req, &info->sram_win_handle));

    mem.CardOffset = srambase;
    mem.Page = 0;
    CS_CHECK(MapMemPage, pcmcia_map_mem_page(info->sram_win_handle, &mem));

    ti->sram_base = mem.CardOffset >> 12;
    ti->sram_virt = (u_long)ioremap(req.Base, req.Size);

    CS_CHECK(RequestConfiguration, pcmcia_request_configuration(link->handle, &link->conf));

    /*  Set up the Token-Ring Controller Configuration Register and
        turn on the card.  Check the "Local Area Network Credit Card
        Adapters Technical Reference"  SC30-3585 for this info.  */
    ibmtr_hw_setup(dev, mmiobase);

    link->dev = &info->node;
    link->state &= ~DEV_CONFIG_PENDING;

    i = ibmtr_probe_card(dev);
    if (i != 0) {
	printk(KERN_NOTICE "ibmtr_cs: register_netdev() failed\n");
	link->dev = NULL;
	goto failed;
    }

    strcpy(info->node.dev_name, dev->name);

    printk(KERN_INFO "%s: port %#3lx, irq %d,",
           dev->name, dev->base_addr, dev->irq);
    printk (" mmio %#5lx,", (u_long)ti->mmio);
    printk (" sram %#5lx,", (u_long)ti->sram_base << 12);
    printk ("\n" KERN_INFO "  hwaddr=");
    for (i = 0; i < TR_ALEN; i++)
        printk("%02X", dev->dev_addr[i]);
    printk("\n");
    return;

cs_failed:
    cs_error(link->handle, last_fn, last_ret);
failed:
    ibmtr_release(link);
} /* ibmtr_config */

/*======================================================================

    After a card is removed, ibmtr_release() will unregister the net
    device, and release the PCMCIA configuration.  If the device is
    still open, this will be postponed until it is closed.

======================================================================*/

static void ibmtr_release(dev_link_t *link)
{
    ibmtr_dev_t *info = link->priv;
    struct net_device *dev = info->dev;

    DEBUG(0, "ibmtr_release(0x%p)\n", link);

    pcmcia_release_configuration(link->handle);
    pcmcia_release_io(link->handle, &link->io);
    pcmcia_release_irq(link->handle, &link->irq);
    if (link->win) {
	struct tok_info *ti = netdev_priv(dev);
	iounmap((void *)ti->mmio);
	pcmcia_release_window(link->win);
	pcmcia_release_window(info->sram_win_handle);
    }

    link->state &= ~DEV_CONFIG;
}

/*======================================================================

    The card status event handler.  Mostly, this schedules other
    stuff to run after an event is received.  A CARD_REMOVAL event
    also sets some flags to discourage the net drivers from trying
    to talk to the card any more.

======================================================================*/

static int ibmtr_event(event_t event, int priority,
                       event_callback_args_t *args)
{
    dev_link_t *link = args->client_data;
    ibmtr_dev_t *info = link->priv;
    struct net_device *dev = info->dev;

    DEBUG(1, "ibmtr_event(0x%06x)\n", event);

    switch (event) {
    case CS_EVENT_CARD_REMOVAL:
        link->state &= ~DEV_PRESENT;
        if (link->state & DEV_CONFIG) {
	    /* set flag to bypass normal interrupt code */
	    struct tok_info *priv = netdev_priv(dev);
	    priv->sram_virt |= 1;
	    netif_device_detach(dev);
        }
        break;
    case CS_EVENT_CARD_INSERTION:
        link->state |= DEV_PRESENT;
	ibmtr_config(link);
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
		ibmtr_probe(dev);	/* really? */
		netif_device_attach(dev);
            }
        }
        break;
    }
    return 0;
} /* ibmtr_event */

/*====================================================================*/

static void ibmtr_hw_setup(struct net_device *dev, u_int mmiobase)
{
    int i;

    /* Bizarre IBM behavior, there are 16 bits of information we
       need to set, but the card only allows us to send 4 bits at a 
       time.  For each byte sent to base_addr, bits 7-4 tell the
       card which part of the 16 bits we are setting, bits 3-0 contain 
       the actual information */

    /* First nibble provides 4 bits of mmio */
    i = (mmiobase >> 16) & 0x0F;
    outb(i, dev->base_addr);

    /* Second nibble provides 3 bits of mmio */
    i = 0x10 | ((mmiobase >> 12) & 0x0E);
    outb(i, dev->base_addr);

    /* Third nibble, hard-coded values */
    i = 0x26;
    outb(i, dev->base_addr);

    /* Fourth nibble sets shared ram page size */

    /* 8 = 00, 16 = 01, 32 = 10, 64 = 11 */          
    i = (sramsize >> 4) & 0x07;
    i = ((i == 4) ? 3 : i) << 2;
    i |= 0x30;

    if (ringspeed == 16)
	i |= 2;
    if (dev->base_addr == 0xA24)
	i |= 1;
    outb(i, dev->base_addr);

    /* 0x40 will release the card for use */
    outb(0x40, dev->base_addr);

    return;
}

static struct pcmcia_driver ibmtr_cs_driver = {
	.owner		= THIS_MODULE,
	.drv		= {
		.name	= "ibmtr_cs",
	},
	.attach		= ibmtr_attach,
	.detach		= ibmtr_detach,
};

static int __init init_ibmtr_cs(void)
{
	return pcmcia_register_driver(&ibmtr_cs_driver);
}

static void __exit exit_ibmtr_cs(void)
{
	pcmcia_unregister_driver(&ibmtr_cs_driver);
	while (dev_list != NULL)
		ibmtr_detach(dev_list);
}

module_init(init_ibmtr_cs);
module_exit(exit_ibmtr_cs);
