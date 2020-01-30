/*=============================================================================
 *
 * A  PCMCIA client driver for the Raylink wireless LAN card.
 * The starting point for this module was the skeleton.c in the
 * PCMCIA 2.9.12 package written by David Hinds, dhinds@allegro.stanford.edu
 *
 *
 * Copyright (c) 1998  Corey Thomas (corey@world.std.com)
 *
 * This driver is free software; you can redistribute it and/or modify
 * it under the terms of version 2 only of the GNU General Public License as 
 * published by the Free Software Foundation.
 *
 * It is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 *
 * Changes:
 * Arnaldo Carvalho de Melo <acme@conectiva.com.br> - 08/08/2000
 * - reorganize kmallocs in ray_attach, checking all for failure
 *   and releasing the previous allocations if one fails
 *
 * 
=============================================================================*/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/ptrace.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/system.h>
#include <asm/byteorder.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/ioport.h>
#include <linux/skbuff.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/cisreg.h>
#include <pcmcia/ds.h>
#include <pcmcia/mem_op.h>

#ifdef CONFIG_NET_PCMCIA_RADIO
#include <linux/wireless.h>

/* Warning : these stuff will slow down the driver... */
#define WIRELESS_SPY		/* Enable spying addresses */
/* Definitions we need for spy */
typedef struct iw_statistics	iw_stats;
typedef struct iw_quality	iw_qual;
typedef u_char	mac_addr[ETH_ALEN];	/* Hardware address */
#endif	/* CONFIG_NET_PCMCIA_RADIO */

#include "rayctl.h"
#include "ray_cs.h"

/* All the PCMCIA modules use PCMCIA_DEBUG to control debugging.  If
   you do not define PCMCIA_DEBUG at all, all the debug code will be
   left out.  If you compile with PCMCIA_DEBUG=0, the debug code will
   be present but disabled -- but it can then be enabled for specific
   modules at load time with a 'pc_debug=#' option to insmod.
*/

#ifdef RAYLINK_DEBUG
#define PCMCIA_DEBUG RAYLINK_DEBUG
#endif
#ifdef PCMCIA_DEBUG
static int ray_debug = 0;
static int pc_debug = PCMCIA_DEBUG;
MODULE_PARM(pc_debug, "i");
/* #define DEBUG(n, args...) if (pc_debug>(n)) printk(KERN_DEBUG args); */
#define DEBUG(n, args...) if (pc_debug>(n)) printk(args);
#else
#define DEBUG(n, args...)
#endif
/** Prototypes based on PCMCIA skeleton driver *******************************/
static void ray_config(dev_link_t *link);
static void ray_release(u_long arg);
static int ray_event(event_t event, int priority, event_callback_args_t *args);
static dev_link_t *ray_attach(void);
static void ray_detach(dev_link_t *);

/***** Prototypes indicated by device structure ******************************/
static int ray_dev_close(struct net_device *dev);
static int ray_dev_config(struct net_device *dev, struct ifmap *map);
static struct net_device_stats *ray_get_stats(struct net_device *dev);
static int ray_dev_init(struct net_device *dev);
static int ray_dev_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd);
static int ray_open(struct net_device *dev);
static int ray_dev_start_xmit(struct sk_buff *skb, struct net_device *dev);
static void set_multicast_list(struct net_device *dev);
static void ray_update_multi_list(struct net_device *dev, int all);
static int translate_frame(ray_dev_t *local, struct tx_msg *ptx,
                unsigned char *data, int len);
static void ray_build_header(ray_dev_t *local, struct tx_msg *ptx, UCHAR msg_type,
                unsigned char *data);
static void untranslate(ray_dev_t *local, struct sk_buff *skb, int len);
#if WIRELESS_EXT > 7	/* If wireless extension exist in the kernel */
static iw_stats * ray_get_wireless_stats(struct net_device *	dev);
#endif	/* WIRELESS_EXT > 7 */

/***** Prototypes for raylink functions **************************************/
static int asc_to_int(char a);
static void authenticate(ray_dev_t *local);
static int build_auth_frame(ray_dev_t *local, UCHAR *dest, int auth_type);
static void authenticate_timeout(u_long);
static int get_free_ccs(ray_dev_t *local);
static int get_free_tx_ccs(ray_dev_t *local);
static void init_startup_params(ray_dev_t *local);
static int parse_addr(char *in_str, UCHAR *out);
static int ray_hw_xmit(unsigned char* data, int len, struct net_device* dev, UCHAR type);
static int ray_init(struct net_device *dev);
static int interrupt_ecf(ray_dev_t *local, int ccs);
static void ray_reset(struct net_device *dev);
static void ray_update_parm(struct net_device *dev, UCHAR objid, UCHAR *value, int len);
static void verify_dl_startup(u_long);

/* Prototypes for interrpt time functions **********************************/
static void ray_interrupt (int reg, void *dev_id, struct pt_regs *regs);
static void clear_interrupt(ray_dev_t *local);
static void rx_deauthenticate(ray_dev_t *local, struct rcs *prcs, 
                       unsigned int pkt_addr, int rx_len);
static int copy_from_rx_buff(ray_dev_t *local, UCHAR *dest, int pkt_addr, int len);
static void ray_rx(struct net_device *dev, ray_dev_t *local, struct rcs *prcs);
static void release_frag_chain(ray_dev_t *local, struct rcs *prcs);
static void rx_authenticate(ray_dev_t *local, struct rcs *prcs,
                     unsigned int pkt_addr, int rx_len);
static void rx_data(struct net_device *dev, struct rcs *prcs, unsigned int pkt_addr, 
             int rx_len);
static void associate(ray_dev_t *local);

/* Card command functions */
static int dl_startup_params(struct net_device *dev);
static void join_net(u_long local);
static void start_net(u_long local);
/* void start_net(ray_dev_t *local); */

/* Create symbol table for registering with kernel in init_module */
EXPORT_SYMBOL(ray_dev_ioctl);
EXPORT_SYMBOL(ray_rx);

/*===========================================================================*/
/* Parameters that can be set with 'insmod' */
/* Bit map of interrupts to choose from */
/* This means pick from 15, 14, 12, 11, 10, 9, 7, 5, 4, and 3 */
static u_long irq_mask = 0xdeb8;

/* ADHOC=0, Infrastructure=1 */
static int net_type = ADHOC;

/* Hop dwell time in Kus (1024 us units defined by 802.11) */
static int hop_dwell = 128;

/* Beacon period in Kus */
static int beacon_period = 256;

/* power save mode (0 = off, 1 = save power) */
static int psm = 0;

/* String for network's Extended Service Set ID. 32 Characters max */
static char *essid = NULL;

/* Default to encapsulation unless translation requested */
static int translate = 1;

static int country = USA;

static int sniffer = 0;

static int bc = 0;

/* 48 bit physical card address if overriding card's real physical
 * address is required.  Since IEEE 802.11 addresses are 48 bits
 * like ethernet, an int can't be used, so a string is used. To
 * allow use of addresses starting with a decimal digit, the first
 * character must be a letter and will be ignored. This letter is
 * followed by up to 12 hex digits which are the address.  If less
 * than 12 digits are used, the address will be left filled with 0's.
 * Note that bit 0 of the first byte is the broadcast bit, and evil
 * things will happen if it is not 0 in a card address.
 */
static char *phy_addr = NULL;


/* The dev_info variable is the "key" that is used to match up this
   device driver with appropriate cards, through the card configuration
   database.
*/
static dev_info_t dev_info = "ray_cs";

/* A linked list of "instances" of the ray device.  Each actual
   PCMCIA card corresponds to one device instance, and is described
   by one dev_link_t structure (defined in ds.h).
*/
static dev_link_t *dev_list = NULL;

/* A dev_link_t structure has fields for most things that are needed
   to keep track of a socket, but there will usually be some device
   specific information that also needs to be kept track of.  The
   'priv' pointer in a dev_link_t structure can be used to point to
   a device-specific private data structure, like this.
*/
static unsigned int ray_mem_speed = 500;

MODULE_AUTHOR("Corey Thomas <corey@world.std.com>");
MODULE_DESCRIPTION("Raylink/WebGear wireless LAN driver");
MODULE_PARM(irq_mask,"i");
MODULE_PARM(net_type,"i");
MODULE_PARM(hop_dwell,"i");
MODULE_PARM(beacon_period,"i");
MODULE_PARM(psm,"i");
MODULE_PARM(essid,"s");
MODULE_PARM(translate,"i");
MODULE_PARM(country,"i");
MODULE_PARM(sniffer,"i");
MODULE_PARM(bc,"i");
MODULE_PARM(phy_addr,"s");
MODULE_PARM(ray_mem_speed, "i");

static UCHAR b5_default_startup_parms[] = {
    0,   0,                         /* Adhoc station */
   'L','I','N','U','X', 0,  0,  0,  /* 32 char ESSID */
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    1,  0,                          /* Active scan, CA Mode */
    0,  0,  0,  0,  0,  0,          /* No default MAC addr  */
    0x7f, 0xff,                     /* Frag threshold */
    0x00, 0x80,                     /* Hop time 128 Kus*/
    0x01, 0x00,                     /* Beacon period 256 Kus */
    0x01, 0x07, 0xa3,               /* DTIM, retries, ack timeout*/
    0x1d, 0x82, 0x4e,               /* SIFS, DIFS, PIFS */
    0x7f, 0xff,                     /* RTS threshold */
    0x04, 0xe2, 0x38, 0xA4,         /* scan_dwell, max_scan_dwell */
    0x05,                           /* assoc resp timeout thresh */
    0x08, 0x02, 0x08,               /* adhoc, infra, super cycle max*/
    0,                              /* Promiscuous mode */
    0x0c, 0x0bd,                    /* Unique word */
    0x32,                           /* Slot time */
    0xff, 0xff,                     /* roam-low snr, low snr count */
    0x05, 0xff,                     /* Infra, adhoc missed bcn thresh */
    0x01, 0x0b, 0x4f,               /* USA, hop pattern, hop pat length */
/* b4 - b5 differences start here */
    0x00, 0x3f,                     /* CW max */
    0x00, 0x0f,                     /* CW min */
    0x04, 0x08,                     /* Noise gain, limit offset */
    0x28, 0x28,                     /* det rssi, med busy offsets */
    7,                              /* det sync thresh */
    0, 2, 2,                        /* test mode, min, max */
    0,                              /* allow broadcast SSID probe resp */
    0, 0,                           /* privacy must start, can join */
    2, 0, 0, 0, 0, 0, 0, 0          /* basic rate set */
};

static UCHAR b4_default_startup_parms[] = {
    0,   0,                         /* Adhoc station */
   'L','I','N','U','X', 0,  0,  0,  /* 32 char ESSID */
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    1,  0,                          /* Active scan, CA Mode */
    0,  0,  0,  0,  0,  0,          /* No default MAC addr  */
    0x7f, 0xff,                     /* Frag threshold */
    0x02, 0x00,                     /* Hop time */
    0x00, 0x01,                     /* Beacon period */
    0x01, 0x07, 0xa3,               /* DTIM, retries, ack timeout*/
    0x1d, 0x82, 0xce,               /* SIFS, DIFS, PIFS */
    0x7f, 0xff,                     /* RTS threshold */
    0xfb, 0x1e, 0xc7, 0x5c,         /* scan_dwell, max_scan_dwell */
    0x05,                           /* assoc resp timeout thresh */
    0x04, 0x02, 0x4,                /* adhoc, infra, super cycle max*/
    0,                              /* Promiscuous mode */
    0x0c, 0x0bd,                    /* Unique word */
    0x4e,                           /* Slot time (TBD seems wrong)*/
    0xff, 0xff,                     /* roam-low snr, low snr count */
    0x05, 0xff,                     /* Infra, adhoc missed bcn thresh */
    0x01, 0x0b, 0x4e,               /* USA, hop pattern, hop pat length */
/* b4 - b5 differences start here */
    0x3f, 0x0f,                     /* CW max, min */
    0x04, 0x08,                     /* Noise gain, limit offset */
    0x28, 0x28,                     /* det rssi, med busy offsets */
    7,                              /* det sync thresh */
    0, 2, 2                         /* test mode, min, max*/
};
/*===========================================================================*/
static unsigned char eth2_llc[] = {0xaa, 0xaa, 3, 0, 0, 0};

static char hop_pattern_length[] = { 1,
	     USA_HOP_MOD,             EUROPE_HOP_MOD,
	     JAPAN_HOP_MOD,           KOREA_HOP_MOD,
	     SPAIN_HOP_MOD,           FRANCE_HOP_MOD,
	     ISRAEL_HOP_MOD,          AUSTRALIA_HOP_MOD,
	     JAPAN_TEST_HOP_MOD
};

static char rcsid[] = "Raylink/WebGear wireless LAN - Corey <Thomas corey@world.std.com>";

/*===========================================================================*/
static void cs_error(client_handle_t handle, int func, int ret)
{
    error_info_t err = { func, ret };
    pcmcia_report_error(handle, &err);
}
/*======================================================================

    This bit of code is used to avoid unregistering network devices
    at inappropriate times.  2.2 and later kernels are fairly picky
    about when this can happen.
    
======================================================================*/

static void flush_stale_links(void)
{
    dev_link_t *link, *next;
    for (link = dev_list; link; link = next) {
	next = link->next;
	if (link->state & DEV_STALE_LINK)
	    ray_detach(link);
    }
}

/*=============================================================================
    ray_attach() creates an "instance" of the driver, allocating
    local data structures for one device.  The device is registered
    with Card Services.
    The dev_link structure is initialized, but we don't actually
    configure the card at this point -- we wait until we receive a
    card insertion event.
=============================================================================*/
static dev_link_t *ray_attach(void)
{
    client_reg_t client_reg;
    dev_link_t *link;
    ray_dev_t *local;
    int ret;
    struct net_device *dev;
    
    DEBUG(1, "ray_attach()\n");
    flush_stale_links();

    /* Initialize the dev_link_t structure */
    link = kmalloc(sizeof(struct dev_link_t), GFP_KERNEL);

    if (!link)
	    return NULL;

    /* Allocate space for private device-specific data */
    dev = kmalloc(sizeof(struct net_device), GFP_KERNEL);

    if (!dev)
	    goto fail_alloc_dev;

    local = kmalloc(sizeof(ray_dev_t), GFP_KERNEL);

    if (!local)
	    goto fail_alloc_local;

    memset(link, 0, sizeof(struct dev_link_t));
    memset(dev, 0, sizeof(struct net_device));
    memset(local, 0, sizeof(ray_dev_t));

    link->release.function = &ray_release;
    link->release.data = (u_long)link;

    /* The io structure describes IO port mapping. None used here */
    link->io.NumPorts1 = 0;
    link->io.Attributes1 = IO_DATA_PATH_WIDTH_8;
    link->io.IOAddrLines = 5;

    /* Interrupt setup. For PCMCIA, driver takes what's given */
    link->irq.Attributes = IRQ_TYPE_EXCLUSIVE | IRQ_HANDLE_PRESENT;
    link->irq.IRQInfo1 = IRQ_INFO2_VALID | IRQ_LEVEL_ID;
    link->irq.IRQInfo2 = irq_mask;
    link->irq.Handler = &ray_interrupt;

    /* General socket configuration */
    link->conf.Attributes = CONF_ENABLE_IRQ;
    link->conf.Vcc = 50;
    link->conf.IntType = INT_MEMORY_AND_IO;
    link->conf.ConfigIndex = 1;
    link->conf.Present = PRESENT_OPTION;

    link->priv = dev;
    link->irq.Instance = dev;
    
    dev->priv = local;
    local->finder = link;
    local->card_status = CARD_INSERTED;
    local->authentication_state = UNAUTHENTICATED;
    local->num_multi = 0;
    DEBUG(2,"ray_attach link = %p,  dev = %p,  local = %p, intr = %p\n",
          link,dev,local,&ray_interrupt);

    /* Raylink entries in the device structure */
    dev->hard_start_xmit = &ray_dev_start_xmit;
    dev->set_config = &ray_dev_config;
    dev->get_stats  = &ray_get_stats;
    dev->do_ioctl = &ray_dev_ioctl;
#if WIRELESS_EXT > 7	/* If wireless extension exist in the kernel */
    dev->get_wireless_stats = ray_get_wireless_stats;
#endif

    dev->set_multicast_list = &set_multicast_list;

    DEBUG(2,"ray_cs ray_attach calling ether_setup.)\n");
    ether_setup(dev);
    dev->init = &ray_dev_init;
    dev->open = &ray_open;
    dev->stop = &ray_dev_close;
    netif_stop_queue(dev);

    /* Register with Card Services */
    link->next = dev_list;
    dev_list = link;
    client_reg.dev_info = &dev_info;
    client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
    client_reg.EventMask =
        CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
        CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
        CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
    client_reg.event_handler = &ray_event;
    client_reg.Version = 0x0210;
    client_reg.event_callback_args.client_data = link;

    DEBUG(2,"ray_cs ray_attach calling CardServices(RegisterClient...)\n");

    init_timer(&local->timer);

    ret = pcmcia_register_client(&link->handle, &client_reg);
    if (ret != 0) {
        printk("ray_cs ray_attach RegisterClient unhappy - detaching\n");
        cs_error(link->handle, RegisterClient, ret);
        ray_detach(link);
        return NULL;
    }
    DEBUG(2,"ray_cs ray_attach ending\n");
    return link;

fail_alloc_local:
    kfree(dev);
fail_alloc_dev:
    kfree(link);
    return NULL;
} /* ray_attach */
/*=============================================================================
    This deletes a driver "instance".  The device is de-registered
    with Card Services.  If it has been released, all local data
    structures are freed.  Otherwise, the structures will be freed
    when the device is released.
=============================================================================*/
static void ray_detach(dev_link_t *link)
{
    dev_link_t **linkp;

    DEBUG(1, "ray_detach(0x%p)\n", link);
    
    /* Locate device structure */
    for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
        if (*linkp == link) break;
    if (*linkp == NULL)
        return;

    /* If the device is currently configured and active, we won't
      actually delete it yet.  Instead, it is marked so that when
      the release() function is called, that will trigger a proper
      detach().
    */
    del_timer(&link->release);
    if (link->state & DEV_CONFIG) {
        ray_release((u_long)link);
        if(link->state & DEV_STALE_CONFIG) {
            link->state |= DEV_STALE_LINK;
            return;
        }
    }

    /* Break the link with Card Services */
    if (link->handle)
        pcmcia_deregister_client(link->handle);
    
    /* Unlink device structure, free pieces */
    *linkp = link->next;
    if (link->priv) {
        struct net_device *dev = link->priv;
	if (link->dev) unregister_netdev(dev);
        if (dev->priv)
            kfree(dev->priv);
        kfree(link->priv);
    }
    kfree(link);
    DEBUG(2,"ray_cs ray_detach ending\n");
} /* ray_detach */
/*=============================================================================
    ray_config() is run after a CARD_INSERTION event
    is received, to configure the PCMCIA socket, and to make the
    ethernet device available to the system.
=============================================================================*/
#define CS_CHECK(fn, args...) \
while ((last_ret=fn(args))!=0) goto cs_failed
#define MAX_TUPLE_SIZE 128
static void ray_config(dev_link_t *link)
{
    client_handle_t handle = link->handle;
    tuple_t tuple;
    cisparse_t parse;
    int last_fn = 0, last_ret = 0;
    int i;
    u_char buf[MAX_TUPLE_SIZE];
    win_req_t req;
    memreq_t mem;
    struct net_device *dev = (struct net_device *)link->priv;
    ray_dev_t *local = (ray_dev_t *)dev->priv;

    DEBUG(1, "ray_config(0x%p)\n", link);

    /* This reads the card's CONFIG tuple to find its configuration regs */
    tuple.DesiredTuple = CISTPL_CONFIG;
    CS_CHECK(pcmcia_get_first_tuple, handle, &tuple);
    tuple.TupleData = buf;
    tuple.TupleDataMax = MAX_TUPLE_SIZE;
    tuple.TupleOffset = 0;
    CS_CHECK(pcmcia_get_tuple_data, handle, &tuple);
    CS_CHECK(pcmcia_parse_tuple, handle, &tuple, &parse);
    link->conf.ConfigBase = parse.config.base;
    link->conf.Present = parse.config.rmask[0];

    /* Determine card type and firmware version */
    buf[0] = buf[MAX_TUPLE_SIZE - 1] = 0;
    tuple.DesiredTuple = CISTPL_VERS_1;
    CS_CHECK(pcmcia_get_first_tuple, handle, &tuple);
    tuple.TupleData = buf;
    tuple.TupleDataMax = MAX_TUPLE_SIZE;
    tuple.TupleOffset = 2;
    CS_CHECK(pcmcia_get_tuple_data, handle, &tuple);

    for (i=0; i<tuple.TupleDataLen - 4; i++) 
        if (buf[i] == 0) buf[i] = ' ';
    printk(KERN_INFO "ray_cs Detected: %s\n",buf);

    /* Configure card */
    link->state |= DEV_CONFIG;

    /* Now allocate an interrupt line.  Note that this does not
       actually assign a handler to the interrupt.
    */
    CS_CHECK(pcmcia_request_irq, link->handle, &link->irq);
    dev->irq = link->irq.AssignedIRQ;
    
    /* This actually configures the PCMCIA socket -- setting up
       the I/O windows and the interrupt mapping.
    */
    CS_CHECK(pcmcia_request_configuration, link->handle, &link->conf);

/*** Set up 32k window for shared memory (transmit and control) ************/
    req.Attributes = WIN_DATA_WIDTH_8 | WIN_MEMORY_TYPE_CM | WIN_ENABLE | WIN_USE_WAIT;
    req.Base = 0;
    req.Size = 0x8000;
    req.AccessSpeed = ray_mem_speed;
    CS_CHECK(pcmcia_request_window, &link->handle, &req, &link->win);
    mem.CardOffset = 0x0000; mem.Page = 0;
    CS_CHECK(pcmcia_map_mem_page, link->win, &mem);
    local->sram = (UCHAR *)(ioremap(req.Base,req.Size));

/*** Set up 16k window for shared memory (receive buffer) ***************/
    req.Attributes = WIN_DATA_WIDTH_8 | WIN_MEMORY_TYPE_CM | WIN_ENABLE | WIN_USE_WAIT;
    req.Base = 0;
    req.Size = 0x4000;
    req.AccessSpeed = ray_mem_speed;
    CS_CHECK(pcmcia_request_window, &link->handle, &req, &local->rmem_handle);
    mem.CardOffset = 0x8000; mem.Page = 0;
    CS_CHECK(pcmcia_map_mem_page, local->rmem_handle, &mem);
    local->rmem = (UCHAR *)(ioremap(req.Base,req.Size));

/*** Set up window for attribute memory ***********************************/
    req.Attributes = WIN_DATA_WIDTH_8 | WIN_MEMORY_TYPE_AM | WIN_ENABLE | WIN_USE_WAIT;
    req.Base = 0;
    req.Size = 0x1000;
    req.AccessSpeed = ray_mem_speed;
    CS_CHECK(pcmcia_request_window, &link->handle, &req, &local->amem_handle);
    mem.CardOffset = 0x0000; mem.Page = 0;
    CS_CHECK(pcmcia_map_mem_page, local->amem_handle, &mem);
    local->amem = (UCHAR *)(ioremap(req.Base,req.Size));

    DEBUG(3,"ray_config sram=%p\n",local->sram);
    DEBUG(3,"ray_config rmem=%p\n",local->rmem);
    DEBUG(3,"ray_config amem=%p\n",local->amem);
    if (ray_init(dev) < 0) {
        ray_release((u_long)link);
        return;
    }

    i = register_netdev(dev);
    if (i != 0) {
        printk("ray_config register_netdev() failed\n");
        ray_release((u_long)link);
        return;
    }

    strcpy(local->node.dev_name, dev->name);
    link->dev = &local->node;

    link->state &= ~DEV_CONFIG_PENDING;
    printk(KERN_INFO "%s: RayLink, irq %d, hw_addr ",
       dev->name, dev->irq);
    for (i = 0; i < 6; i++)
    printk("%02X%s", dev->dev_addr[i], ((i<5) ? ":" : "\n"));

    return;

cs_failed:
    cs_error(link->handle, last_fn, last_ret);

    ray_release((u_long)link);
} /* ray_config */
/*===========================================================================*/
static int ray_init(struct net_device *dev)
{
    int i;
    UCHAR *p;
    struct ccs *pccs;
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    dev_link_t *link = local->finder;
    DEBUG(1, "ray_init(0x%p)\n", dev);
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(0,"ray_init - device not present\n");
        return -1;
    }

    local->net_type = net_type;
    local->sta_type = TYPE_STA;

    /* Copy the startup results to local memory */
    memcpy_fromio(&local->startup_res, local->sram + ECF_TO_HOST_BASE,\
           sizeof(struct startup_res_6));

    /* Check Power up test status and get mac address from card */
    if (local->startup_res.startup_word != 0x80) {
    printk(KERN_INFO "ray_init ERROR card status = %2x\n",
           local->startup_res.startup_word);
        local->card_status = CARD_INIT_ERROR;
        return -1;
    }

    local->fw_ver = local->startup_res.firmware_version[0];
    local->fw_bld = local->startup_res.firmware_version[1];
    local->fw_var = local->startup_res.firmware_version[2];
    DEBUG(1,"ray_init firmware version %d.%d \n",local->fw_ver, local->fw_bld);

    local->tib_length = 0x20;
    if ((local->fw_ver == 5) && (local->fw_bld >= 30))
        local->tib_length = local->startup_res.tib_length;
    DEBUG(2,"ray_init tib_length = 0x%02x\n", local->tib_length);
    /* Initialize CCS's to buffer free state */
    pccs = (struct ccs *)(local->sram + CCS_BASE);
    for (i=0;  i<NUMBER_OF_CCS;  i++) {
        writeb(CCS_BUFFER_FREE, &(pccs++)->buffer_status);
    }
    init_startup_params(local);

    /* copy mac address to startup parameters */
    if (parse_addr(phy_addr, local->sparm.b4.a_mac_addr))
    {
        p = local->sparm.b4.a_mac_addr;
    }
    else
    {
        memcpy(&local->sparm.b4.a_mac_addr,
               &local->startup_res.station_addr, ADDRLEN);
        p = local->sparm.b4.a_mac_addr;
    }

    clear_interrupt(local); /* Clear any interrupt from the card */
    local->card_status = CARD_AWAITING_PARAM;
    DEBUG(2,"ray_init ending\n");
    return 0;
} /* ray_init */
/*===========================================================================*/
/* Download startup parameters to the card and command it to read them       */
static int dl_startup_params(struct net_device *dev)
{
    int ccsindex;
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    struct ccs *pccs;
    dev_link_t *link = local->finder;

    DEBUG(1,"dl_startup_params entered\n");
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs dl_startup_params - device not present\n");
        return -1;
    }
    
    /* Copy parameters to host to ECF area */
    if (local->fw_ver == 0x55) 
        memcpy_toio(local->sram + HOST_TO_ECF_BASE, &local->sparm.b4,
               sizeof(struct b4_startup_params));
    else
        memcpy_toio(local->sram + HOST_TO_ECF_BASE, &local->sparm.b5,
               sizeof(struct b5_startup_params));

    
    /* Fill in the CCS fields for the ECF */
    if ((ccsindex = get_free_ccs(local)) < 0) return -1;
    local->dl_param_ccs = ccsindex;
    pccs = ((struct ccs *)(local->sram + CCS_BASE)) + ccsindex;
    writeb(CCS_DOWNLOAD_STARTUP_PARAMS, &pccs->cmd);
    DEBUG(2,"dl_startup_params start ccsindex = %d\n", local->dl_param_ccs);
    /* Interrupt the firmware to process the command */
    if (interrupt_ecf(local, ccsindex)) {
        printk(KERN_INFO "ray dl_startup_params failed - "
           "ECF not ready for intr\n");
        local->card_status = CARD_DL_PARAM_ERROR;
        writeb(CCS_BUFFER_FREE, &(pccs++)->buffer_status);
        return -2;
    }
    local->card_status = CARD_DL_PARAM;
    /* Start kernel timer to wait for dl startup to complete. */
    local->timer.expires = jiffies + HZ/2;
    local->timer.data = (long)local;
    local->timer.function = &verify_dl_startup;
    add_timer(&local->timer);
    DEBUG(2,"ray_cs dl_startup_params started timer for verify_dl_startup\n");
    return 0;
} /* dl_startup_params */
/*===========================================================================*/
static void init_startup_params(ray_dev_t *local)
{
    int i; 

    if (country > JAPAN_TEST) country = USA;
    else
        if (country < USA) country = USA;
    /* structure for hop time and beacon period is defined here using 
     * New 802.11D6.1 format.  Card firmware is still using old format
     * until version 6.
     *    Before                    After
     *    a_hop_time ms byte        a_hop_time ms byte
     *    a_hop_time 2s byte        a_hop_time ls byte
     *    a_hop_time ls byte        a_beacon_period ms byte
     *    a_beacon_period           a_beacon_period ls byte
     *
     *    a_hop_time = uS           a_hop_time = KuS
     *    a_beacon_period = hops    a_beacon_period = KuS
     */                             /* 64ms = 010000 */
    if (local->fw_ver == 0x55)  {
        memcpy((UCHAR *)&local->sparm.b4, b4_default_startup_parms, 
               sizeof(struct b4_startup_params));
        /* Translate sane kus input values to old build 4/5 format */
        /* i = hop time in uS truncated to 3 bytes */
        i = (hop_dwell * 1024) & 0xffffff;
        local->sparm.b4.a_hop_time[0] = (i >> 16) & 0xff;
        local->sparm.b4.a_hop_time[1] = (i >> 8) & 0xff;
        local->sparm.b4.a_beacon_period[0] = 0;
        local->sparm.b4.a_beacon_period[1] =
            ((beacon_period/hop_dwell) - 1) & 0xff;
        local->sparm.b4.a_curr_country_code = country;
        local->sparm.b4.a_hop_pattern_length = 
            hop_pattern_length[(int)country] - 1;
        if (bc)
        {
            local->sparm.b4.a_ack_timeout = 0x50;
            local->sparm.b4.a_sifs = 0x3f;
        }
    }
    else {    /* Version 5 uses real kus values */
        memcpy((UCHAR *)&local->sparm.b5, b5_default_startup_parms, 
               sizeof(struct b5_startup_params));

        local->sparm.b5.a_hop_time[0] = (hop_dwell >> 8) & 0xff;
        local->sparm.b5.a_hop_time[1] = hop_dwell & 0xff;
        local->sparm.b5.a_beacon_period[0] = (beacon_period >> 8) & 0xff;
        local->sparm.b5.a_beacon_period[1] = beacon_period & 0xff;
        if (psm)
            local->sparm.b5.a_power_mgt_state = 1;
        local->sparm.b5.a_curr_country_code = country;
        local->sparm.b5.a_hop_pattern_length = 
            hop_pattern_length[(int)country];
    }
    
    local->sparm.b4.a_network_type = net_type & 0x01;
    local->sparm.b4.a_acting_as_ap_status = TYPE_STA;

    if (essid != NULL)
        strncpy(local->sparm.b4.a_current_ess_id, essid, ESSID_SIZE);
} /* init_startup_params */ 
/*===========================================================================*/
static void verify_dl_startup(u_long data)
{
    ray_dev_t *local = (ray_dev_t *)data;
    struct ccs *pccs = ((struct ccs *)(local->sram + CCS_BASE)) + local->dl_param_ccs;
    UCHAR status;
    dev_link_t *link = local->finder;

    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs verify_dl_startup - device not present\n");
        return;
    }
#ifdef PCMCIA_DEBUG
    if (pc_debug > 2) {
    int i;
    printk(KERN_DEBUG "verify_dl_startup parameters sent via ccs %d:\n",
           local->dl_param_ccs);
        for (i=0; i<sizeof(struct b5_startup_params); i++) {
            printk(" %2x", (unsigned int) readb(local->sram + HOST_TO_ECF_BASE + i));
        }
    printk("\n");
    }
#endif

    status = readb(&pccs->buffer_status);
    if (status!= CCS_BUFFER_FREE)
    {
        printk(KERN_INFO "Download startup params failed.  Status = %d\n",
           status);
        local->card_status = CARD_DL_PARAM_ERROR;
        return;
    }
    if (local->sparm.b4.a_network_type == ADHOC)
        start_net((u_long)local);
    else
        join_net((u_long)local);

    return;
} /* end verify_dl_startup */
/*===========================================================================*/
/* Command card to start a network */
static void start_net(u_long data)
{
    ray_dev_t *local = (ray_dev_t *)data;
    struct ccs *pccs;
    int ccsindex;
    dev_link_t *link = local->finder;
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs start_net - device not present\n");
        return;
    }
    /* Fill in the CCS fields for the ECF */
    if ((ccsindex = get_free_ccs(local)) < 0) return;
    pccs = ((struct ccs *)(local->sram + CCS_BASE)) + ccsindex;
    writeb(CCS_START_NETWORK, &pccs->cmd);
    writeb(0, &pccs->var.start_network.update_param);
    /* Interrupt the firmware to process the command */
    if (interrupt_ecf(local, ccsindex)) {
        DEBUG(1,"ray start net failed - card not ready for intr\n");
        writeb(CCS_BUFFER_FREE, &(pccs++)->buffer_status);
        return;
    }
    local->card_status = CARD_DOING_ACQ;
    return;
} /* end start_net */
/*===========================================================================*/
/* Command card to join a network */
static void join_net(u_long data)
{
    ray_dev_t *local = (ray_dev_t *)data;

    struct ccs *pccs;
    int ccsindex;
    dev_link_t *link = local->finder;
    
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs join_net - device not present\n");
        return;
    }
    /* Fill in the CCS fields for the ECF */
    if ((ccsindex = get_free_ccs(local)) < 0) return;
    pccs = ((struct ccs *)(local->sram + CCS_BASE)) + ccsindex;
    writeb(CCS_JOIN_NETWORK, &pccs->cmd);
    writeb(0, &pccs->var.join_network.update_param);
    writeb(0, &pccs->var.join_network.net_initiated);
    /* Interrupt the firmware to process the command */
    if (interrupt_ecf(local, ccsindex)) {
        DEBUG(1,"ray join net failed - card not ready for intr\n");
        writeb(CCS_BUFFER_FREE, &(pccs++)->buffer_status);
        return;
    }
    local->card_status = CARD_DOING_ACQ;
    return;
}
/*============================================================================
    After a card is removed, ray_release() will unregister the net
    device, and release the PCMCIA configuration.  If the device is
    still open, this will be postponed until it is closed.
=============================================================================*/
static void ray_release(u_long arg)
{
    dev_link_t *link = (dev_link_t *)arg;
    struct net_device *dev = link->priv; 
    ray_dev_t *local = dev->priv;
    int i;
    
    DEBUG(1, "ray_release(0x%p)\n", link);
    /* If the device is currently in use, we won't release until it
      is actually closed.
    */
    if (link->open) {
        DEBUG(1, "ray_cs: release postponed, '%s' still open\n",
              link->dev->dev_name);
        link->state |= DEV_STALE_CONFIG;
        return;
    }
    del_timer(&local->timer);
    link->state &= ~DEV_CONFIG;

    iounmap(local->sram);
    iounmap(local->rmem);
    iounmap(local->amem);
    /* Do bother checking to see if these succeed or not */
    i = pcmcia_release_window(link->win);
    if ( i != CS_SUCCESS ) DEBUG(0,"ReleaseWindow(link->win) ret = %x\n",i);
    i = pcmcia_release_window(local->amem_handle);
    if ( i != CS_SUCCESS ) DEBUG(0,"ReleaseWindow(local->amem) ret = %x\n",i);
    i = pcmcia_release_window(local->rmem_handle);
    if ( i != CS_SUCCESS ) DEBUG(0,"ReleaseWindow(local->rmem) ret = %x\n",i);
    i = pcmcia_release_configuration(link->handle);
    if ( i != CS_SUCCESS ) DEBUG(0,"ReleaseConfiguration ret = %x\n",i);
    i = pcmcia_release_irq(link->handle, &link->irq);
    if ( i != CS_SUCCESS ) DEBUG(0,"ReleaseIRQ ret = %x\n",i);

    DEBUG(2,"ray_release ending\n");
} /* ray_release */
/*=============================================================================
    The card status event handler.  Mostly, this schedules other
    stuff to run after an event is received.  A CARD_REMOVAL event
    also sets some flags to discourage the net drivers from trying
    to talk to the card any more.

    When a CARD_REMOVAL event is received, we immediately set a flag
    to block future accesses to this device.  All the functions that
    actually access the device should check this flag to make sure
    the card is still present.
=============================================================================*/
static int ray_event(event_t event, int priority,
                     event_callback_args_t *args)
{
    dev_link_t *link = args->client_data;
    struct net_device *dev = link->priv;
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    DEBUG(1, "ray_event(0x%06x)\n", event);
    
    switch (event) {
    case CS_EVENT_CARD_REMOVAL:
        link->state &= ~DEV_PRESENT;
        netif_device_detach(dev);
        if (link->state & DEV_CONFIG) {
            mod_timer(&link->release, jiffies + HZ/20);
            del_timer(&local->timer);
        }
        break;
    case CS_EVENT_CARD_INSERTION:
        link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
        ray_config(link);
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
                ray_reset(dev);
		netif_device_attach(dev);
            }
        }
        break;
    }
    return 0;
    DEBUG(2,"ray_event ending\n");
} /* ray_event */
/*===========================================================================*/
int ray_dev_init(struct net_device *dev)
{
    int i;
    ray_dev_t *local = dev->priv;
    dev_link_t *link = local->finder;

    DEBUG(1,"ray_dev_init(dev=%p)\n",dev);
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_dev_init - device not present\n");
        return -1;
    }
    /* Download startup parameters */
    if ( (i = dl_startup_params(dev)) < 0)
    {
        printk(KERN_INFO "ray_dev_init dl_startup_params failed - "
           "returns 0x%x\n",i);
        return -1;
    }
    
    /* copy mac and broadcast addresses to linux device */
    memcpy(&dev->dev_addr, &local->sparm.b4.a_mac_addr, ADDRLEN);
    memset(dev->broadcast, 0xff, ETH_ALEN);

    DEBUG(2,"ray_dev_init ending\n");
    return 0;
}
/*===========================================================================*/
static int ray_dev_config(struct net_device *dev, struct ifmap *map)
{
    ray_dev_t *local = dev->priv;
    dev_link_t *link = local->finder;
    /* Dummy routine to satisfy device structure */
    DEBUG(1,"ray_dev_config(dev=%p,ifmap=%p)\n",dev,map);
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_dev_config - device not present\n");
        return -1;
    }

    return 0;
}
/*===========================================================================*/
static int ray_dev_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    ray_dev_t *local = dev->priv;
    dev_link_t *link = local->finder;
    short length;

    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_dev_start_xmit - device not present\n");
        return -1;
    }
    DEBUG(3,"ray_dev_start_xmit(skb=%p, dev=%p)\n",skb,dev);
    if (local->authentication_state == NEED_TO_AUTH) {
        DEBUG(0,"ray_cs Sending authentication request.\n");
        if (!build_auth_frame (local, local->auth_id, OPEN_AUTH_REQUEST)) {
            local->authentication_state = AUTHENTICATED;
            netif_stop_queue(dev);
            return 1;
        }
    }

    length = ETH_ZLEN < skb->len ? skb->len : ETH_ZLEN;
    switch (ray_hw_xmit( skb->data, length, dev, DATA_TYPE)) {
        case XMIT_NO_CCS:
        case XMIT_NEED_AUTH:
	    netif_stop_queue(dev);
            return 1;
        case XMIT_NO_INTR:
        case XMIT_MSG_BAD:
        case XMIT_OK:
        default:
            dev->trans_start = jiffies;
            dev_kfree_skb(skb);
            return 0;
    }
    return 0;
} /* ray_dev_start_xmit */
/*===========================================================================*/
static int ray_hw_xmit(unsigned char* data, int len, struct net_device* dev, 
                UCHAR msg_type)
{
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    struct ccs *pccs;
    int ccsindex;
    int offset;
    struct tx_msg *ptx; /* Address of xmit buffer in PC space */
    short int addr;     /* Address of xmit buffer in card space */
    
    DEBUG(3,"ray_hw_xmit(data=%p, len=%d, dev=%p)\n",data,len,dev);
    if (len + TX_HEADER_LENGTH > TX_BUF_SIZE)
    {
        printk(KERN_INFO "ray_hw_xmit packet too large: %d bytes\n",len);
        return XMIT_MSG_BAD;
    }
	switch (ccsindex = get_free_tx_ccs(local)) {
	case ECCSBUSY:
		DEBUG(2,"ray_hw_xmit tx_ccs table busy\n");
	case ECCSFULL:
        DEBUG(2,"ray_hw_xmit No free tx ccs\n");
	case ECARDGONE:
	netif_stop_queue(dev);
        return XMIT_NO_CCS;
	default:
		break;
	}
    addr = TX_BUF_BASE + (ccsindex << 11);

    if (msg_type == DATA_TYPE) {
        local->stats.tx_bytes += len;
        local->stats.tx_packets++;
    }

    ptx = (struct tx_msg *)(local->sram + addr);

    ray_build_header(local, ptx, msg_type, data);
    if (translate) {
        offset = translate_frame(local, ptx, data, len);
    }
    else { /* Encapsulate frame */
        /* TBD TIB length will move address of ptx->var */
        memcpy_toio(&ptx->var, data, len);
        offset = 0;
    }

    /* fill in the CCS */
    pccs = ((struct ccs *)(local->sram + CCS_BASE)) + ccsindex;
    len += TX_HEADER_LENGTH + offset;
    writeb(CCS_TX_REQUEST, &pccs->cmd);
    writeb(addr >> 8, &pccs->var.tx_request.tx_data_ptr[0]);
    writeb(local->tib_length, &pccs->var.tx_request.tx_data_ptr[1]);
    writeb(len >> 8, &pccs->var.tx_request.tx_data_length[0]);
    writeb(len & 0xff, &pccs->var.tx_request.tx_data_length[1]);
/* TBD still need psm_cam? */
    writeb(PSM_CAM, &pccs->var.tx_request.pow_sav_mode);
    writeb(local->net_default_tx_rate, &pccs->var.tx_request.tx_rate);
    writeb(0, &pccs->var.tx_request.antenna);
    DEBUG(3,"ray_hw_xmit default_tx_rate = 0x%x\n",\
          local->net_default_tx_rate);

    /* Interrupt the firmware to process the command */
    if (interrupt_ecf(local, ccsindex)) {
        DEBUG(2,"ray_hw_xmit failed - ECF not ready for intr\n");
/* TBD very inefficient to copy packet to buffer, and then not
   send it, but the alternative is to queue the messages and that
   won't be done for a while.  Maybe set tbusy until a CCS is free?
*/
        writeb(CCS_BUFFER_FREE, &pccs->buffer_status);
        return XMIT_NO_INTR;
    }
    return XMIT_OK;
} /* end ray_hw_xmit */
/*===========================================================================*/
static int translate_frame(ray_dev_t *local, struct tx_msg *ptx, unsigned char *data,
                    int len)
{
    unsigned short int proto = ((struct ethhdr *)data)->h_proto;
    if (ntohs(proto) >= 1536) { /* DIX II ethernet frame */
        DEBUG(3,"ray_cs translate_frame DIX II\n");
        /* Copy LLC header to card buffer */
        memcpy_toio((UCHAR *)&ptx->var, eth2_llc, sizeof(eth2_llc));
        memcpy_toio( ((UCHAR *)&ptx->var) + sizeof(eth2_llc), (UCHAR *)&proto, 2);
        if ((proto == 0xf380) || (proto == 0x3781)) {
            /* This is the selective translation table, only 2 entries */
            writeb(0xf8, (UCHAR *) &((struct snaphdr_t *)ptx->var)->org[3]);
        }
        /* Copy body of ethernet packet without ethernet header */
        memcpy_toio((UCHAR *)&ptx->var + sizeof(struct snaphdr_t), \
                    data + ETH_HLEN,  len - ETH_HLEN);
        return (int) sizeof(struct snaphdr_t) - ETH_HLEN;
    }
    else { /* already  802 type, and proto is length */
        DEBUG(3,"ray_cs translate_frame 802\n");
        if (proto == 0xffff) { /* evil netware IPX 802.3 without LLC */
        DEBUG(3,"ray_cs translate_frame evil IPX\n");
            memcpy_toio((UCHAR *)&ptx->var, data + ETH_HLEN,  len - ETH_HLEN);
            return 0 - ETH_HLEN;
        }
        memcpy_toio((UCHAR *)&ptx->var, data + ETH_HLEN,  len - ETH_HLEN);
        return 0 - ETH_HLEN;
    }
    /* TBD do other frame types */
} /* end translate_frame */
/*===========================================================================*/
static void ray_build_header(ray_dev_t *local, struct tx_msg *ptx, UCHAR msg_type,
                unsigned char *data)
{
    writeb(PROTOCOL_VER | msg_type, &ptx->mac.frame_ctl_1);
/*** IEEE 802.11 Address field assignments *************
                TODS FROMDS   addr_1     addr_2          addr_3   addr_4
Adhoc           0    0        dest       src (terminal)  BSSID    N/A
AP to Terminal  0    1        dest       AP(BSSID)       source   N/A
Terminal to AP  1    0        AP(BSSID)  src (terminal)  dest     N/A
AP to AP        1    1        dest AP    src AP          dest     source      
*******************************************************/
    if (local->net_type == ADHOC) {   
        writeb(0, &ptx->mac.frame_ctl_2);
        memcpy_toio(ptx->mac.addr_1, ((struct ethhdr *)data)->h_dest, 2 * ADDRLEN);
        memcpy_toio(ptx->mac.addr_3, local->bss_id, ADDRLEN);
    }
    else /* infrastructure */
    {
        if (local->sparm.b4.a_acting_as_ap_status)
        {
            writeb(FC2_FROM_DS, &ptx->mac.frame_ctl_2);
            memcpy_toio(ptx->mac.addr_1, ((struct ethhdr *)data)->h_dest, ADDRLEN);
            memcpy_toio(ptx->mac.addr_2, local->bss_id, 6);
            memcpy_toio(ptx->mac.addr_3, ((struct ethhdr *)data)->h_source, ADDRLEN);
        }
        else /* Terminal */
        {
            writeb(FC2_TO_DS, &ptx->mac.frame_ctl_2);
            memcpy_toio(ptx->mac.addr_1, local->bss_id, ADDRLEN);
            memcpy_toio(ptx->mac.addr_2, ((struct ethhdr *)data)->h_source, ADDRLEN);
            memcpy_toio(ptx->mac.addr_3, ((struct ethhdr *)data)->h_dest, ADDRLEN);
        }
    }
} /* end encapsulate_frame */
/*===========================================================================*/
static int ray_dev_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    dev_link_t *link = local->finder;
    int err = 0;
#if WIRELESS_EXT > 7
    struct iwreq *wrq = (struct iwreq *) ifr;
#endif	/* WIRELESS_EXT > 7 */

    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_dev_ioctl - device not present\n");
        return -1;
    }
    DEBUG(2,"ray_cs IOCTL dev=%p, ifr=%p, cmd = 0x%x\n",dev,ifr,cmd);
    /* Validate the command */
    switch (cmd)
    {
#if WIRELESS_EXT > 7
      /* --------------- WIRELESS EXTENSIONS --------------- */
      /* Get name */
    case SIOCGIWNAME:
      strcpy(wrq->u.name, "IEEE 802.11-FH");
      break;

      /* Get frequency/channel */
    case SIOCGIWFREQ:
      wrq->u.freq.m = local->sparm.b5.a_hop_pattern;
      wrq->u.freq.e = 0;
      break;

      /* Get current network name (ESSID) */
    case SIOCGIWESSID:
      if (wrq->u.data.pointer)
	{
	  char essid[IW_ESSID_MAX_SIZE + 1];
	  /* Get the essid that was set */
	  memcpy(essid, local->sparm.b5.a_current_ess_id,
		 IW_ESSID_MAX_SIZE);
	  essid[IW_ESSID_MAX_SIZE] = '\0';

	  /* Push it out ! */
	  wrq->u.data.length = strlen(essid) + 1;
	  wrq->u.data.flags = 1; /* active */
	  copy_to_user(wrq->u.data.pointer, essid, sizeof(essid));
	}
      break;

      /* Get current Access Point (BSSID in our case) */
    case SIOCGIWAP:
      memcpy(wrq->u.ap_addr.sa_data, local->bss_id, ETH_ALEN);
      wrq->u.ap_addr.sa_family = ARPHRD_ETHER;
      break;

      /* Get the current bit-rate */
    case SIOCGIWRATE:
      if(local->net_default_tx_rate == 3)
	wrq->u.bitrate.value = 2000000;		/* Hum... */
      else
	wrq->u.bitrate.value = local->net_default_tx_rate * 500000;
      wrq->u.bitrate.fixed = 0;		/* We are in auto mode */
      break;

      /* Set the desired bit-rate */
    case SIOCSIWRATE:
      /* Check if rate is in range */
      if((wrq->u.bitrate.value != 1000000) &&
	 (wrq->u.bitrate.value != 2000000))
	{
	  err = -EINVAL;
	  break;
	}
      /* Hack for 1.5 Mb/s instead of 2 Mb/s */
      if((local->fw_ver == 0x55) &&		/* Please check */
	 (wrq->u.bitrate.value == 2000000))
	local->net_default_tx_rate = 3;
      else
	local->net_default_tx_rate = wrq->u.bitrate.value/500000;
      break;

      /* Get the current RTS threshold */
    case SIOCGIWRTS:
      wrq->u.rts.value = (local->sparm.b5.a_rts_threshold[0] << 8)
	+ local->sparm.b5.a_rts_threshold[1];
#if WIRELESS_EXT > 8
      wrq->u.rts.disabled = (wrq->u.rts.value == 32767);
#endif /* WIRELESS_EXT > 8 */
      wrq->u.rts.fixed = 1;
      break;

      /* Get the current fragmentation threshold */
    case SIOCGIWFRAG:
      wrq->u.frag.value = (local->sparm.b5.a_frag_threshold[0] << 8)
	+ local->sparm.b5.a_frag_threshold[1];
#if WIRELESS_EXT > 8
      wrq->u.frag.disabled = (wrq->u.frag.value == 32767);
#endif /* WIRELESS_EXT > 8 */
      wrq->u.frag.fixed = 1;
      break;
#endif	/* WIRELESS_EXT > 7 */
#if WIRELESS_EXT > 8

      /* Get the current mode of operation */
    case SIOCGIWMODE:
      if(local->sparm.b5.a_network_type)
	wrq->u.mode = IW_MODE_INFRA;
      else
	wrq->u.mode = IW_MODE_ADHOC;
      break;
#endif /* WIRELESS_EXT > 8 */
#if WIRELESS_EXT > 7
      /* ------------------ IWSPY SUPPORT ------------------ */
      /* Define the range (variations) of above parameters */
    case SIOCGIWRANGE:
      /* Basic checking... */
      if(wrq->u.data.pointer != (caddr_t) 0)
	{
	  struct iw_range	range;
	  memset((char *) &range, 0, sizeof(struct iw_range));

	  /* Set the length (useless : its constant...) */
	  wrq->u.data.length = sizeof(struct iw_range);

	  /* Set information in the range struct */
	  range.throughput = 1.1 * 1000 * 1000;	/* Put the right number here */
	  range.num_channels = hop_pattern_length[(int)country]; 
	  range.num_frequency = 0;
	  range.max_qual.qual = 0;
	  range.max_qual.level = 255;	/* What's the correct value ? */
	  range.max_qual.noise = 255;	/* Idem */
	  range.num_bitrates = 2;
	  range.bitrate[0] = 1000000;	/* 1 Mb/s */
	  range.bitrate[1] = 2000000;	/* 2 Mb/s */

	  /* Copy structure to the user buffer */
	  if(copy_to_user(wrq->u.data.pointer, &range,
			  sizeof(struct iw_range)))
	    err = -EFAULT;
	}
      break;

#ifdef WIRELESS_SPY
      /* Set addresses to spy */
    case SIOCSIWSPY:
      /* Check the number of addresses */
      if(wrq->u.data.length > IW_MAX_SPY)
	{
	  err = -E2BIG;
	  break;
	}
      local->spy_number = wrq->u.data.length;

      /* If there is some addresses to copy */
      if(local->spy_number > 0)
	{
	  struct sockaddr	address[IW_MAX_SPY];
	  int			i;

	  /* Copy addresses to the driver */
	  if(copy_from_user(address, wrq->u.data.pointer,
			    sizeof(struct sockaddr) * local->spy_number))
	    {
	      err = -EFAULT;
	      break;
	    }

	  /* Copy addresses to the lp structure */
	  for(i = 0; i < local->spy_number; i++)
	    memcpy(local->spy_address[i], address[i].sa_data, ETH_ALEN);

	  /* Reset structure... */
	  memset(local->spy_stat, 0x00, sizeof(iw_qual) * IW_MAX_SPY);

#ifdef DEBUG_IOCTL_INFO
	  printk(KERN_DEBUG "SetSpy - Set of new addresses is :\n");
	  for(i = 0; i < local->spy_number; i++)
	    printk(KERN_DEBUG "%02X:%02X:%02X:%02X:%02X:%02X\n",
		   local->spy_address[i][0],
		   local->spy_address[i][1],
		   local->spy_address[i][2],
		   local->spy_address[i][3],
		   local->spy_address[i][4],
		   local->spy_address[i][5]);
#endif	/* DEBUG_IOCTL_INFO */
	}
      break;

      /* Get the spy list and spy stats */
    case SIOCGIWSPY:
      /* Set the number of addresses */
      wrq->u.data.length = local->spy_number;

      /* If the user want to have the addresses back... */
      if((local->spy_number > 0) && (wrq->u.data.pointer != (caddr_t) 0))
	{
	  struct sockaddr	address[IW_MAX_SPY];
	  int			i;

	  /* Copy addresses from the lp structure */
	  for(i = 0; i < local->spy_number; i++)
	    {
	      memcpy(address[i].sa_data, local->spy_address[i], ETH_ALEN);
	      address[i].sa_family = ARPHRD_ETHER;
	    }

	  /* Copy addresses to the user buffer */
	  if(copy_to_user(wrq->u.data.pointer, address,
		       sizeof(struct sockaddr) * local->spy_number))
	    {
	      err = -EFAULT;
	      break;
	    }

	  /* Copy stats to the user buffer (just after) */
	  if(copy_to_user(wrq->u.data.pointer +
		       (sizeof(struct sockaddr) * local->spy_number),
		       local->spy_stat, sizeof(iw_qual) * local->spy_number))
	    {
	      err = -EFAULT;
	      break;
	    }

	  /* Reset updated flags */
	  for(i = 0; i < local->spy_number; i++)
	    local->spy_stat[i].updated = 0x0;
	}	/* if(pointer != NULL) */

      break;
#endif	/* WIRELESS_SPY */

      /* ------------------ PRIVATE IOCTL ------------------ */
#define SIOCSIPFRAMING	SIOCDEVPRIVATE		/* Set framing mode */
#define SIOCGIPFRAMING	SIOCDEVPRIVATE + 1	/* Get framing mode */
#define SIOCGIPCOUNTRY	SIOCDEVPRIVATE + 3	/* Get country code */
    case SIOCSIPFRAMING:
      if(!capable(CAP_NET_ADMIN))	/* For private IOCTLs, we need to check permissions */
	{
	  err = -EPERM;
	  break;
	}
      translate = *(wrq->u.name);	/* Set framing mode */
      break;
    case SIOCGIPFRAMING:
      *(wrq->u.name) = translate;
      break;
    case SIOCGIPCOUNTRY:
      *(wrq->u.name) = country;
      break;
    case SIOCGIWPRIV:
      /* Export our "private" intercace */
      if(wrq->u.data.pointer != (caddr_t) 0)
	{
	  struct iw_priv_args	priv[] =
	  {	/* cmd,		set_args,	get_args,	name */
	    { SIOCSIPFRAMING, IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1, 0, "set_framing" },
	    { SIOCGIPFRAMING, 0, IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1, "get_framing" },
	    { SIOCGIPCOUNTRY, 0, IW_PRIV_TYPE_BYTE | IW_PRIV_SIZE_FIXED | 1, "get_country" },
	  };
	  /* Set the number of ioctl available */
	  wrq->u.data.length = 3;
	  /* Copy structure to the user buffer */
	  if(copy_to_user(wrq->u.data.pointer, (u_char *) priv,
		       sizeof(priv)))
	    err = -EFAULT;
	}
      break;
#endif	/* WIRELESS_EXT > 7 */


        default:
            DEBUG(0,"ray_dev_ioctl cmd = 0x%x\n", cmd);
            err = -EOPNOTSUPP;
    }
    return err;
} /* end ray_dev_ioctl */
/*===========================================================================*/
#if WIRELESS_EXT > 7	/* If wireless extension exist in the kernel */
static iw_stats * ray_get_wireless_stats(struct net_device *	dev)
{
  ray_dev_t *	local = (ray_dev_t *) dev->priv;
  dev_link_t *link = local->finder;
  struct status *p = (struct status *)(local->sram + STATUS_BASE);

  if(local == (ray_dev_t *) NULL)
    return (iw_stats *) NULL;

  local->wstats.status = local->card_status;
#ifdef WIRELESS_SPY
  if((local->spy_number > 0) && (local->sparm.b5.a_network_type == 0))
    {
      /* Get it from the first node in spy list */
      local->wstats.qual.qual = local->spy_stat[0].qual;
      local->wstats.qual.level = local->spy_stat[0].level;
      local->wstats.qual.noise = local->spy_stat[0].noise;
      local->wstats.qual.updated = local->spy_stat[0].updated;
    }
#endif /* WIRELESS_SPY */

  if((link->state & DEV_PRESENT)) {
    local->wstats.qual.noise = readb(&p->rxnoise);
    local->wstats.qual.updated |= 4;
  }

  return &local->wstats;
} /* end ray_get_wireless_stats */
#endif	/* WIRELESS_EXT > 7 */
/*===========================================================================*/
static int ray_open(struct net_device *dev)
{
    dev_link_t *link;
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    
    MOD_INC_USE_COUNT;

    DEBUG(1, "ray_open('%s')\n", dev->name);

    for (link = dev_list; link; link = link->next)
        if (link->priv == dev) break;
    if (!DEV_OK(link)) {
        MOD_DEC_USE_COUNT;
        return -ENODEV;
    }

    if (link->open == 0) local->num_multi = 0;
    link->open++;

    if (sniffer) netif_stop_queue(dev);
    else         netif_start_queue(dev);

    DEBUG(2,"ray_open ending\n");
    return 0;
} /* end ray_open */
/*===========================================================================*/
static int ray_dev_close(struct net_device *dev)
{
    dev_link_t *link;

    DEBUG(1, "ray_dev_close('%s')\n", dev->name);

    for (link = dev_list; link; link = link->next)
        if (link->priv == dev) break;
    if (link == NULL)
        return -ENODEV;

    link->open--;
    netif_stop_queue(dev);
    if (link->state & DEV_STALE_CONFIG)
	mod_timer(&link->release, jiffies + HZ/20);

    MOD_DEC_USE_COUNT;

    return 0;
} /* end ray_dev_close */
/*===========================================================================*/
static void ray_reset(struct net_device *dev) {
    DEBUG(1,"ray_reset entered\n");
    return;
}
/*===========================================================================*/
/* Cause a firmware interrupt if it is ready for one                         */
/* Return nonzero if not ready                                               */
static int interrupt_ecf(ray_dev_t *local, int ccs)
{
    int i = 50;
    dev_link_t *link = local->finder;

    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs interrupt_ecf - device not present\n");
        return -1;
    }
    DEBUG(2,"interrupt_ecf(local=%p, ccs = 0x%x\n",local,ccs);

    while ( i && 
            (readb(local->amem + CIS_OFFSET + ECF_INTR_OFFSET) & ECF_INTR_SET))
        i--;
    if (i == 0) {
        DEBUG(2,"ray_cs interrupt_ecf card not ready for interrupt\n");
        return -1;
    }
	/* Fill the mailbox, then kick the card */
    writeb(ccs, local->sram + SCB_BASE);
    writeb(ECF_INTR_SET, local->amem + CIS_OFFSET + ECF_INTR_OFFSET);
    return 0;
} /* interrupt_ecf */
/*===========================================================================*/
/* Get next free transmit CCS                                                */
/* Return - index of current tx ccs                                          */
static int get_free_tx_ccs(ray_dev_t *local)
{
    int i;
    struct ccs *pccs = (struct ccs *)(local->sram + CCS_BASE);
    dev_link_t *link = local->finder;

    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs get_free_tx_ccs - device not present\n");
        return ECARDGONE;
    }

    if (test_and_set_bit(0,&local->tx_ccs_lock)) {
        DEBUG(1,"ray_cs tx_ccs_lock busy\n");
        return ECCSBUSY;
    } 

    for (i=0; i < NUMBER_OF_TX_CCS; i++) {
        if (readb(&(pccs+i)->buffer_status) == CCS_BUFFER_FREE) {
            writeb(CCS_BUFFER_BUSY, &(pccs+i)->buffer_status);
            writeb(CCS_END_LIST, &(pccs+i)->link);
			local->tx_ccs_lock = 0;
            return i;
        }
    }
	local->tx_ccs_lock = 0;
    DEBUG(2,"ray_cs ERROR no free tx CCS for raylink card\n");
    return ECCSFULL;
} /* get_free_tx_ccs */
/*===========================================================================*/
/* Get next free CCS                                                         */
/* Return - index of current ccs                                             */
static int get_free_ccs(ray_dev_t *local)
{
    int i;
    struct ccs *pccs = (struct ccs *)(local->sram + CCS_BASE);
    dev_link_t *link = local->finder;

    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs get_free_ccs - device not present\n");
        return ECARDGONE;
    }
    if (test_and_set_bit(0,&local->ccs_lock)) {
        DEBUG(1,"ray_cs ccs_lock busy\n");
        return ECCSBUSY;
    } 

    for (i = NUMBER_OF_TX_CCS; i < NUMBER_OF_CCS; i++) {
        if (readb(&(pccs+i)->buffer_status) == CCS_BUFFER_FREE) {
            writeb(CCS_BUFFER_BUSY, &(pccs+i)->buffer_status);
            writeb(CCS_END_LIST, &(pccs+i)->link);
			local->ccs_lock = 0;
            return i;
        }
    }
	local->ccs_lock = 0;
    DEBUG(1,"ray_cs ERROR no free CCS for raylink card\n");
    return ECCSFULL;
} /* get_free_ccs */
/*===========================================================================*/
static void authenticate_timeout(u_long data)
{
    ray_dev_t *local = (ray_dev_t *)data;
    del_timer(&local->timer);
    printk(KERN_INFO "ray_cs Authentication with access point failed"
       " - timeout\n");
    join_net((u_long)local);
}
/*===========================================================================*/
static int asc_to_int(char a)
{
    if (a < '0') return -1;
    if (a <= '9') return (a - '0');
    if (a < 'A') return -1;
    if (a <= 'F') return (10 + a - 'A');
    if (a < 'a') return -1;
    if (a <= 'f') return (10 + a - 'a');
    return -1;
}
/*===========================================================================*/
static int parse_addr(char *in_str, UCHAR *out)
{
    int len;
    int i,j,k;
    int status;
    
    if (in_str == NULL) return 0;
    if ((len = strlen(in_str)) < 2) return 0;
    memset(out, 0, ADDRLEN);

    status = 1;
    j = len - 1;
    if (j > 12) j = 12;
    i = 5;
    
    while (j > 0)
    {
        if ((k = asc_to_int(in_str[j--])) != -1) out[i] = k;
        else return 0;

        if (j == 0) break;
        if ((k = asc_to_int(in_str[j--])) != -1) out[i] += k << 4;
        else return 0;
        if (!i--) break;
    }
    return status;
}
/*===========================================================================*/
static struct net_device_stats *ray_get_stats(struct net_device *dev)
{
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    dev_link_t *link = local->finder;
    struct status *p = (struct status *)(local->sram + STATUS_BASE);
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs net_device_stats - device not present\n");
        return &local->stats;
    }
    if (readb(&p->mrx_overflow_for_host))
    {
        local->stats.rx_over_errors += ntohs(readb(&p->mrx_overflow));
        writeb(0,&p->mrx_overflow);
        writeb(0,&p->mrx_overflow_for_host);
    }
    if (readb(&p->mrx_checksum_error_for_host))
    {
        local->stats.rx_crc_errors += ntohs(readb(&p->mrx_checksum_error));
        writeb(0,&p->mrx_checksum_error);
        writeb(0,&p->mrx_checksum_error_for_host);
    }
    if (readb(&p->rx_hec_error_for_host))
    {
        local->stats.rx_frame_errors += ntohs(readb(&p->rx_hec_error));
        writeb(0,&p->rx_hec_error);
        writeb(0,&p->rx_hec_error_for_host);
    }
    return &local->stats;
}
/*===========================================================================*/
static void ray_update_parm(struct net_device *dev, UCHAR objid, UCHAR *value, int len)
{
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    dev_link_t *link = local->finder;
    int ccsindex;
    int i;
    struct ccs *pccs;

    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_update_parm - device not present\n");
        return;
    }

    if ((ccsindex = get_free_ccs(local)) < 0)
    {
        DEBUG(0,"ray_update_parm - No free ccs\n");
        return;
    }
    pccs = ((struct ccs *)(local->sram + CCS_BASE)) + ccsindex;
    writeb(CCS_UPDATE_PARAMS, &pccs->cmd);
    writeb(objid, &pccs->var.update_param.object_id);
    writeb(1, &pccs->var.update_param.number_objects);
    writeb(0, &pccs->var.update_param.failure_cause);
    for (i=0; i<len; i++) {
        writeb(value[i], local->sram + HOST_TO_ECF_BASE);
    }
    /* Interrupt the firmware to process the command */
    if (interrupt_ecf(local, ccsindex)) {
        DEBUG(0,"ray_cs associate failed - ECF not ready for intr\n");
        writeb(CCS_BUFFER_FREE, &(pccs++)->buffer_status);
    }
}
/*===========================================================================*/
static void ray_update_multi_list(struct net_device *dev, int all)
{
    struct dev_mc_list *dmi, **dmip;
    int ccsindex;
    struct ccs *pccs;
    int i = 0;
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    dev_link_t *link = local->finder;
    UCHAR *p = local->sram + HOST_TO_ECF_BASE;

    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_update_multi_list - device not present\n");
        return;
    }
    else 
        DEBUG(2,"ray_update_multi_list(%p)\n",dev);
    if ((ccsindex = get_free_ccs(local)) < 0)
    {
        DEBUG(1,"ray_update_multi - No free ccs\n");
        return;
    }
    pccs = ((struct ccs *)(local->sram + CCS_BASE)) + ccsindex;
    writeb(CCS_UPDATE_MULTICAST_LIST, &pccs->cmd);

    if (all) {
        writeb(0xff, &pccs->var);
        local->num_multi = 0xff;
    }
    else {
        /* Copy the kernel's list of MC addresses to card */
        for (dmip=&dev->mc_list; (dmi=*dmip)!=NULL; dmip=&dmi->next) {
            memcpy_toio(p, dmi->dmi_addr, ETH_ALEN);
            DEBUG(1,"ray_update_multi add addr %02x%02x%02x%02x%02x%02x\n",dmi->dmi_addr[0],dmi->dmi_addr[1],dmi->dmi_addr[2],dmi->dmi_addr[3],dmi->dmi_addr[4],dmi->dmi_addr[5]);
            p += ETH_ALEN;
            i++;
        }
        if (i > 256/ADDRLEN) i = 256/ADDRLEN;
        writeb((UCHAR)i, &pccs->var);
        DEBUG(1,"ray_cs update_multi %d addresses in list\n", i);
        /* Interrupt the firmware to process the command */
        local->num_multi = i;
    }
    if (interrupt_ecf(local, ccsindex)) {
        DEBUG(1,"ray_cs update_multi failed - ECF not ready for intr\n");
        writeb(CCS_BUFFER_FREE, &(pccs++)->buffer_status);
    }
} /* end ray_update_multi_list */
/*===========================================================================*/
static void set_multicast_list(struct net_device *dev)
{
    ray_dev_t *local = (ray_dev_t *)dev->priv;
    UCHAR promisc;

    DEBUG(2,"ray_cs set_multicast_list(%p)\n",dev);

    if (dev->flags & IFF_PROMISC)
    {
        if (local->sparm.b5.a_promiscuous_mode == 0) {
            DEBUG(1,"ray_cs set_multicast_list promisc on\n");
            local->sparm.b5.a_promiscuous_mode = 1;
            promisc = 1;
            ray_update_parm(dev,  OBJID_promiscuous_mode, \
                            &promisc, sizeof(promisc));
        }
    }
    else {
        if (local->sparm.b5.a_promiscuous_mode == 1) {
            DEBUG(1,"ray_cs set_multicast_list promisc off\n");
            local->sparm.b5.a_promiscuous_mode = 0;
            promisc = 0;
            ray_update_parm(dev,  OBJID_promiscuous_mode, \
                            &promisc, sizeof(promisc));
        }
    }

    if (dev->flags & IFF_ALLMULTI) ray_update_multi_list(dev, 1);
    else
    {
        if (local->num_multi != dev->mc_count) ray_update_multi_list(dev, 0);
    }
} /* end set_multicast_list */
/*=============================================================================
 * All routines below here are run at interrupt time.
=============================================================================*/
static void ray_interrupt(int irq, void *dev_id, struct pt_regs * regs)
{
    struct net_device *dev = (struct net_device *)dev_id;
    dev_link_t *link;
    ray_dev_t *local;
    struct ccs *pccs;
    struct rcs *prcs;
    UCHAR rcsindex;
    UCHAR tmp;
    UCHAR cmd;
    UCHAR status;

    if (dev == NULL) /* Note that we want interrupts with dev->start == 0 */
    return;

    DEBUG(4,"ray_cs: interrupt for *dev=%p\n",dev);

    local = (ray_dev_t *)dev->priv;
    link = (dev_link_t *)local->finder;
    if ( ! (link->state & DEV_PRESENT) || link->state & DEV_SUSPEND ) {
        DEBUG(2,"ray_cs interrupt from device not present or suspended.\n");
        return;
    }
    rcsindex = readb(&((struct scb *)(local->sram))->rcs_index);

    if (rcsindex >= (NUMBER_OF_CCS + NUMBER_OF_RCS))
    {
        DEBUG(1,"ray_cs interrupt bad rcsindex = 0x%x\n",rcsindex);
        clear_interrupt(local);
        return;
    }
    if (rcsindex < NUMBER_OF_CCS) /* If it's a returned CCS */
    {
        pccs = ((struct ccs *) (local->sram + CCS_BASE)) + rcsindex;
        cmd = readb(&pccs->cmd);
        status = readb(&pccs->buffer_status);
        switch (cmd)
        {
        case CCS_DOWNLOAD_STARTUP_PARAMS: /* Happens in firmware someday */
            del_timer(&local->timer);
            if (status == CCS_COMMAND_COMPLETE) {
                DEBUG(1,"ray_cs interrupt download_startup_parameters OK\n");
            }
            else {
                DEBUG(1,"ray_cs interrupt download_startup_parameters fail\n");
            }
            break;
        case CCS_UPDATE_PARAMS:
            DEBUG(1,"ray_cs interrupt update params done\n");
            if (status != CCS_COMMAND_COMPLETE) {
                tmp = readb(&pccs->var.update_param.failure_cause);
            DEBUG(0,"ray_cs interrupt update params failed - reason %d\n",tmp);
            }
            break;
        case CCS_REPORT_PARAMS:
            DEBUG(1,"ray_cs interrupt report params done\n");
            break;
        case CCS_UPDATE_MULTICAST_LIST: /* Note that this CCS isn't returned */
            DEBUG(1,"ray_cs interrupt CCS Update Multicast List done\n");
            break;
        case CCS_UPDATE_POWER_SAVINGS_MODE:
            DEBUG(1,"ray_cs interrupt update power save mode done\n");
            break;
        case CCS_START_NETWORK:
        case CCS_JOIN_NETWORK:
            if (status == CCS_COMMAND_COMPLETE) {
                if (readb(&pccs->var.start_network.net_initiated) == 1) {
                    DEBUG(0,"ray_cs interrupt network \"%s\" started\n",\
                          local->sparm.b4.a_current_ess_id);
                }
                else {
                    DEBUG(0,"ray_cs interrupt network \"%s\" joined\n",\
                          local->sparm.b4.a_current_ess_id);
                }
                memcpy_fromio(&local->bss_id,pccs->var.start_network.bssid,ADDRLEN);

                if (local->fw_ver == 0x55) local->net_default_tx_rate = 3;
                else local->net_default_tx_rate = 
                         readb(&pccs->var.start_network.net_default_tx_rate);
                local->encryption = readb(&pccs->var.start_network.encryption);
                if (!sniffer && (local->net_type == INFRA)
                    && !(local->sparm.b4.a_acting_as_ap_status)) {
                    authenticate(local);
                }
                local->card_status = CARD_ACQ_COMPLETE;
            }
            else {
                local->card_status = CARD_ACQ_FAILED;

                del_timer(&local->timer);
                local->timer.expires = jiffies + HZ*5;
                local->timer.data = (long)local;
                if (status == CCS_START_NETWORK) {
                    DEBUG(0,"ray_cs interrupt network \"%s\" start failed\n",\
                          local->sparm.b4.a_current_ess_id);
                    local->timer.function = &start_net;
                }
                else {
                    DEBUG(0,"ray_cs interrupt network \"%s\" join failed\n",\
                          local->sparm.b4.a_current_ess_id);
                    local->timer.function = &join_net;
                }
                add_timer(&local->timer);
            }
            break;
        case CCS_START_ASSOCIATION:
            if (status == CCS_COMMAND_COMPLETE) {
                local->card_status = CARD_ASSOC_COMPLETE;
                DEBUG(0,"ray_cs association successful\n");
            }
            else
            {
                DEBUG(0,"ray_cs association failed,\n");
                local->card_status = CARD_ASSOC_FAILED;
                join_net((u_long)local);
            }
            break;
        case CCS_TX_REQUEST:
            if (status == CCS_COMMAND_COMPLETE) {
                DEBUG(3,"ray_cs interrupt tx request complete\n");
            }
            else {
                DEBUG(1,"ray_cs interrupt tx request failed\n");
            }
            if (!sniffer) netif_start_queue(dev);
            netif_wake_queue(dev);
            break;
        case CCS_TEST_MEMORY:
            DEBUG(1,"ray_cs interrupt mem test done\n");
            break;
        case CCS_SHUTDOWN:
            DEBUG(1,"ray_cs interrupt Unexpected CCS returned - Shutdown\n");
            break;
        case CCS_DUMP_MEMORY:
            DEBUG(1,"ray_cs interrupt dump memory done\n");
            break;
        case CCS_START_TIMER:
            DEBUG(2,"ray_cs interrupt DING - raylink timer expired\n");
            break;
        default:
            DEBUG(1,"ray_cs interrupt Unexpected CCS 0x%x returned 0x%x\n",\
                  rcsindex, cmd);
        }
        writeb(CCS_BUFFER_FREE, &pccs->buffer_status);
    }
    else /* It's an RCS */
    {
        prcs = ((struct rcs *)(local->sram + CCS_BASE)) + rcsindex;
    
        switch (readb(&prcs->interrupt_id))
        {
        case PROCESS_RX_PACKET:
            ray_rx(dev, local, prcs);
            break;
        case REJOIN_NET_COMPLETE:
            DEBUG(1,"ray_cs interrupt rejoin net complete\n");
            local->card_status = CARD_ACQ_COMPLETE;
            /* do we need to clear tx buffers CCS's? */
            if (local->sparm.b4.a_network_type == ADHOC) {
                if (!sniffer) netif_start_queue(dev);
            }
            else {
                memcpy_fromio(&local->bss_id, prcs->var.rejoin_net_complete.bssid, ADDRLEN);
                DEBUG(1,"ray_cs new BSSID = %02x%02x%02x%02x%02x%02x\n",\
                      local->bss_id[0], local->bss_id[1], local->bss_id[2],\
                      local->bss_id[3], local->bss_id[4], local->bss_id[5]);
                if (!sniffer) authenticate(local);
            }
            break;
        case ROAMING_INITIATED:
            DEBUG(1,"ray_cs interrupt roaming initiated\n"); 
            netif_stop_queue(dev);
            local->card_status = CARD_DOING_ACQ;
            break;
        case JAPAN_CALL_SIGN_RXD:
            DEBUG(1,"ray_cs interrupt japan call sign rx\n");
            break;
        default:
            DEBUG(1,"ray_cs Unexpected interrupt for RCS 0x%x cmd = 0x%x\n",\
                  rcsindex, (unsigned int) readb(&prcs->interrupt_id));
            break;
        }
        writeb(CCS_BUFFER_FREE, &prcs->buffer_status);
    }
    clear_interrupt(local);
} /* ray_interrupt */
/*===========================================================================*/
static void ray_rx(struct net_device *dev, ray_dev_t *local, struct rcs *prcs)
{
    int rx_len;
    unsigned int pkt_addr;
    UCHAR *pmsg;
    DEBUG(4,"ray_rx process rx packet\n");

    /* Calculate address of packet within Rx buffer */
    pkt_addr = ((readb(&prcs->var.rx_packet.rx_data_ptr[0]) << 8)
                + readb(&prcs->var.rx_packet.rx_data_ptr[1])) & RX_BUFF_END;
    /* Length of first packet fragment */
    rx_len = (readb(&prcs->var.rx_packet.rx_data_length[0]) << 8)
        + readb(&prcs->var.rx_packet.rx_data_length[1]);

    local->last_rsl = readb(&prcs->var.rx_packet.rx_sig_lev);
    pmsg = local->rmem + pkt_addr;
    switch(readb(pmsg))
    {
    case DATA_TYPE:
        DEBUG(4,"ray_rx data type\n");
        rx_data(dev, prcs, pkt_addr, rx_len);
        break;
    case AUTHENTIC_TYPE:
        DEBUG(4,"ray_rx authentic type\n");
        if (sniffer) rx_data(dev, prcs, pkt_addr, rx_len);
        else rx_authenticate(local, prcs, pkt_addr, rx_len);
        break;
    case DEAUTHENTIC_TYPE:
        DEBUG(4,"ray_rx deauth type\n");
        if (sniffer) rx_data(dev, prcs, pkt_addr, rx_len);
        else rx_deauthenticate(local, prcs, pkt_addr, rx_len);
        break;
    case NULL_MSG_TYPE:
        DEBUG(3,"ray_cs rx NULL msg\n");
        break;
    case BEACON_TYPE:
        DEBUG(4,"ray_rx beacon type\n");
        if (sniffer) rx_data(dev, prcs, pkt_addr, rx_len);

        copy_from_rx_buff(local, (UCHAR *)&local->last_bcn, pkt_addr, 
                          rx_len < sizeof(struct beacon_rx) ? 
                          rx_len : sizeof(struct beacon_rx));

	local->beacon_rxed = 1;
        /* Get the statistics so the card counters never overflow */
        ray_get_stats(dev);
            break;
    default:
        DEBUG(0,"ray_cs unknown pkt type %2x\n", (unsigned int) readb(pmsg));
        break;
    }

} /* end ray_rx */
/*===========================================================================*/
static void rx_data(struct net_device *dev, struct rcs *prcs, unsigned int pkt_addr, 
             int rx_len)
{
    struct sk_buff *skb = NULL;
    struct rcs *prcslink = prcs;
    ray_dev_t *local = dev->priv;
    UCHAR *rx_ptr;
    int total_len;
    int tmp;
#ifdef WIRELESS_SPY
    int siglev = local->last_rsl;
    u_char linksrcaddr[ETH_ALEN];	/* Other end of the wireless link */
#endif

    if (!sniffer) {
        if (translate) {
/* TBD length needs fixing for translated header */
            if (rx_len < (ETH_HLEN + RX_MAC_HEADER_LENGTH) ||
                rx_len > (dev->mtu + RX_MAC_HEADER_LENGTH + ETH_HLEN + FCS_LEN)) 
            {
                DEBUG(0,"ray_cs invalid packet length %d received \n",rx_len);
                return;
            }
        }
        else /* encapsulated ethernet */ {
            if (rx_len < (ETH_HLEN + RX_MAC_HEADER_LENGTH) ||
                rx_len > (dev->mtu + RX_MAC_HEADER_LENGTH + ETH_HLEN + FCS_LEN))
            {
                DEBUG(0,"ray_cs invalid packet length %d received \n",rx_len);
                return;
            }
        }
    }
    DEBUG(4,"ray_cs rx_data packet\n");
    /* If fragmented packet, verify sizes of fragments add up */
    if (readb(&prcs->var.rx_packet.next_frag_rcs_index) != 0xFF) {
        DEBUG(1,"ray_cs rx'ed fragment\n");
        tmp = (readb(&prcs->var.rx_packet.totalpacketlength[0]) << 8)
            +  readb(&prcs->var.rx_packet.totalpacketlength[1]);
        total_len = tmp;
        prcslink = prcs;
        do {
            tmp -= (readb(&prcslink->var.rx_packet.rx_data_length[0]) << 8)
                +   readb(&prcslink->var.rx_packet.rx_data_length[1]);
            if (readb(&prcslink->var.rx_packet.next_frag_rcs_index) == 0xFF
                || tmp < 0) break;
            prcslink = ((struct rcs *)(local->sram + CCS_BASE))
                + readb(&prcslink->link_field);
        } while (1);

        if (tmp < 0)
        {
            DEBUG(0,"ray_cs rx_data fragment lengths don't add up\n");
            local->stats.rx_dropped++; 
            release_frag_chain(local, prcs);
            return;
        }
    }
    else { /* Single unfragmented packet */
        total_len = rx_len;
    }

    skb = dev_alloc_skb( total_len+5 );
    if (skb == NULL)
    {
        DEBUG(0,"ray_cs rx_data could not allocate skb\n");
        local->stats.rx_dropped++; 
        if (readb(&prcs->var.rx_packet.next_frag_rcs_index) != 0xFF)
            release_frag_chain(local, prcs);
        return;
    }
    skb_reserve( skb, 2);   /* Align IP on 16 byte (TBD check this)*/
    skb->dev = dev;

    DEBUG(4,"ray_cs rx_data total_len = %x, rx_len = %x\n",total_len,rx_len);

/************************/
    /* Reserve enough room for the whole damn packet. */
    rx_ptr = skb_put( skb, total_len);
    /* Copy the whole packet to sk_buff */
    rx_ptr += copy_from_rx_buff(local, rx_ptr, pkt_addr & RX_BUFF_END, rx_len);
    /* Get source address */
#ifdef WIRELESS_SPY
    memcpy(linksrcaddr, ((struct mac_header *)skb->data)->addr_2, ETH_ALEN);
#endif
    /* Now, deal with encapsulation/translation/sniffer */
    if (!sniffer) {
        if (!translate) { 
            /* Encapsulated ethernet, so just lop off 802.11 MAC header */
/* TBD reserve            skb_reserve( skb, RX_MAC_HEADER_LENGTH); */
            skb_pull( skb, RX_MAC_HEADER_LENGTH);
        }
        else {
            /* Do translation */
            untranslate(local, skb, total_len);
        }
    }
    else 
    {  /* sniffer mode, so just pass whole packet */  };

/************************/
    /* Now pick up the rest of the fragments if any */
    tmp = 17; 
    if (readb(&prcs->var.rx_packet.next_frag_rcs_index) != 0xFF) {
        prcslink = prcs;
        DEBUG(1,"ray_cs rx_data in fragment loop\n");
        do {
            prcslink = ((struct rcs *)(local->sram + CCS_BASE))
                + readb(&prcslink->var.rx_packet.next_frag_rcs_index);
            rx_len = (( readb(&prcslink->var.rx_packet.rx_data_length[0]) << 8)
                      + readb(&prcslink->var.rx_packet.rx_data_length[1]))
                & RX_BUFF_END;
            pkt_addr = (( readb(&prcslink->var.rx_packet.rx_data_ptr[0]) << 8)
                        + readb(&prcslink->var.rx_packet.rx_data_ptr[1]))
                & RX_BUFF_END;

            rx_ptr += copy_from_rx_buff(local, rx_ptr, pkt_addr, rx_len);

        } while (tmp-- && 
                 readb(&prcslink->var.rx_packet.next_frag_rcs_index) != 0xFF);
        release_frag_chain(local, prcs);
    }

    skb->protocol = eth_type_trans(skb,dev);
    netif_rx(skb);

    local->stats.rx_packets++;
    local->stats.rx_bytes += skb->len;

    /* Gather signal strength per address */
#ifdef WIRELESS_SPY
    /* For the Access Point or the node having started the ad-hoc net
     * note : ad-hoc work only in some specific configurations, but we
     * kludge in ray_get_wireless_stats... */
    if(!memcmp(linksrcaddr, local->bss_id, ETH_ALEN))
      {
	/* Update statistics */
	/*local->wstats.qual.qual = none ? */
	local->wstats.qual.level = siglev;
	/*local->wstats.qual.noise = none ? */
	local->wstats.qual.updated = 0x2;
      }
    /* Now, for the addresses in the spy list */
    {
      int	i;
      /* Look all addresses */
      for(i = 0; i < local->spy_number; i++)
	/* If match */
	if(!memcmp(linksrcaddr, local->spy_address[i], ETH_ALEN))
	  {
	    /* Update statistics */
	    /*local->spy_stat[i].qual = none ? */
	    local->spy_stat[i].level = siglev;
	    /*local->spy_stat[i].noise = none ? */
	    local->spy_stat[i].updated = 0x2;
	  }
    }
#endif	/* WIRELESS_SPY */
} /* end rx_data */
/*===========================================================================*/
static void untranslate(ray_dev_t *local, struct sk_buff *skb, int len)
{
    snaphdr_t *psnap = (snaphdr_t *)(skb->data + RX_MAC_HEADER_LENGTH);
    struct mac_header *pmac = (struct mac_header *)skb->data;
    unsigned short type = *(unsigned short *)psnap->ethertype;
    unsigned int xsap = *(unsigned int *)psnap & 0x00ffffff;
    unsigned int org = (*(unsigned int *)psnap->org) & 0x00ffffff;
    int delta;
    struct ethhdr *peth;
    UCHAR srcaddr[ADDRLEN];
    UCHAR destaddr[ADDRLEN];

    if (pmac->frame_ctl_2 & FC2_FROM_DS) {
	if (pmac->frame_ctl_2 & FC2_TO_DS) { /* AP to AP */
	    memcpy(destaddr, pmac->addr_3, ADDRLEN);
	    memcpy(srcaddr, ((unsigned char *)pmac->addr_3) + ADDRLEN, ADDRLEN);
	} else { /* AP to terminal */
	    memcpy(destaddr, pmac->addr_1, ADDRLEN);
	    memcpy(srcaddr, pmac->addr_3, ADDRLEN); 
	}
    } else { /* Terminal to AP */
	if (pmac->frame_ctl_2 & FC2_TO_DS) {
	    memcpy(destaddr, pmac->addr_3, ADDRLEN);
	    memcpy(srcaddr, pmac->addr_2, ADDRLEN); 
	} else { /* Adhoc */
	    memcpy(destaddr, pmac->addr_1, ADDRLEN);
	    memcpy(srcaddr, pmac->addr_2, ADDRLEN); 
	}
    }

#ifdef PCMCIA_DEBUG
    if (pc_debug > 3) {
    int i;
    printk(KERN_DEBUG "skb->data before untranslate");
    for (i=0;i<64;i++) 
        printk("%02x ",skb->data[i]);
    printk("\n" KERN_DEBUG "type = %08x, xsap = %08x, org = %08x\n",
           type,xsap,org);
    printk(KERN_DEBUG "untranslate skb->data = %p\n",skb->data);
    }
#endif

    if ( xsap != SNAP_ID) {
        /* not a snap type so leave it alone */
        DEBUG(3,"ray_cs untranslate NOT SNAP %x\n", *(unsigned int *)psnap & 0x00ffffff);

        delta = RX_MAC_HEADER_LENGTH - ETH_HLEN;
        peth = (struct ethhdr *)(skb->data + delta);
        peth->h_proto = htons(len - RX_MAC_HEADER_LENGTH);
    }
    else { /* Its a SNAP */
        if (org == BRIDGE_ENCAP) { /* EtherII and nuke the LLC  */
        DEBUG(3,"ray_cs untranslate Bridge encap\n");
            delta = RX_MAC_HEADER_LENGTH 
                + sizeof(struct snaphdr_t) - ETH_HLEN;
            peth = (struct ethhdr *)(skb->data + delta);
            peth->h_proto = type;
        }
        else {
            if (org == RFC1042_ENCAP) {
                switch (type) {
                case RAY_IPX_TYPE:
                case APPLEARP_TYPE:
                    DEBUG(3,"ray_cs untranslate RFC IPX/AARP\n");
                    delta = RX_MAC_HEADER_LENGTH - ETH_HLEN;
                    peth = (struct ethhdr *)(skb->data + delta);
                    peth->h_proto = htons(len - RX_MAC_HEADER_LENGTH);
                    break;
                default:
                    DEBUG(3,"ray_cs untranslate RFC default\n");
                    delta = RX_MAC_HEADER_LENGTH + 
                        sizeof(struct snaphdr_t) - ETH_HLEN;
                    peth = (struct ethhdr *)(skb->data + delta);
                    peth->h_proto = type;
                    break;
                }
            }
            else {
                printk("ray_cs untranslate very confused by packet\n");
                delta = RX_MAC_HEADER_LENGTH - ETH_HLEN;
                peth = (struct ethhdr *)(skb->data + delta);
                peth->h_proto = type;
            }
        }
    }
/* TBD reserve  skb_reserve(skb, delta); */
    skb_pull(skb, delta);
    DEBUG(3,"untranslate after skb_pull(%d), skb->data = %p\n",delta,skb->data);
    memcpy(peth->h_dest, destaddr, ADDRLEN);
    memcpy(peth->h_source, srcaddr, ADDRLEN);
#ifdef PCMCIA_DEBUG
    if (pc_debug > 3) {
    int i;
    printk(KERN_DEBUG "skb->data after untranslate:");
    for (i=0;i<64;i++)
        printk("%02x ",skb->data[i]);
    printk("\n");
    }
#endif
} /* end untranslate */
/*===========================================================================*/
/* Copy data from circular receive buffer to PC memory.
 * dest     = destination address in PC memory
 * pkt_addr = source address in receive buffer
 * len      = length of packet to copy
 */
static int copy_from_rx_buff(ray_dev_t *local, UCHAR *dest, int pkt_addr, int length)
{
    int wrap_bytes = (pkt_addr + length) - (RX_BUFF_END + 1);
    if (wrap_bytes <= 0)
    {
        memcpy_fromio(dest,local->rmem + pkt_addr,length);
    }
    else /* Packet wrapped in circular buffer */
    {
        memcpy_fromio(dest,local->rmem+pkt_addr,length - wrap_bytes);
        memcpy_fromio(dest + length - wrap_bytes, local->rmem, wrap_bytes);
    }
    return length;
}
/*===========================================================================*/
static void release_frag_chain(ray_dev_t *local, struct rcs* prcs)
{
    struct rcs *prcslink = prcs;
    int tmp = 17;
    unsigned rcsindex = readb(&prcs->var.rx_packet.next_frag_rcs_index);

    while (tmp--) {
        writeb(CCS_BUFFER_FREE, &prcslink->buffer_status);
        if (rcsindex >= (NUMBER_OF_CCS + NUMBER_OF_RCS)) {
            DEBUG(1,"ray_cs interrupt bad rcsindex = 0x%x\n",rcsindex);
            break;      
        }   
        prcslink = ((struct rcs *)(local->sram + CCS_BASE)) + rcsindex;
        rcsindex = readb(&prcslink->var.rx_packet.next_frag_rcs_index);
    }
    writeb(CCS_BUFFER_FREE, &prcslink->buffer_status);
}
/*===========================================================================*/
static void authenticate(ray_dev_t *local)
{
    dev_link_t *link = local->finder;
    DEBUG(0,"ray_cs Starting authentication.\n");
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs authenticate - device not present\n");
        return;
    }

    del_timer(&local->timer);
    if (build_auth_frame(local, local->bss_id, OPEN_AUTH_REQUEST)) {
        local->timer.function = &join_net;
    }
    else {
        local->timer.function = &authenticate_timeout;
    }
    local->timer.expires = jiffies + HZ*2;
    local->timer.data = (long)local;
    add_timer(&local->timer);
    local->authentication_state = AWAITING_RESPONSE;
} /* end authenticate */
/*===========================================================================*/
static void rx_authenticate(ray_dev_t *local, struct rcs *prcs,
                     unsigned int pkt_addr, int rx_len)
{
    UCHAR buff[256];
    struct rx_msg *msg = (struct rx_msg *)buff;
    
    del_timer(&local->timer);

    copy_from_rx_buff(local, buff, pkt_addr, rx_len & 0xff);
    /* if we are trying to get authenticated */
    if (local->sparm.b4.a_network_type == ADHOC) {
        DEBUG(1,"ray_cs rx_auth var= %02x %02x %02x %02x %02x %02x\n", msg->var[0],msg->var[1],msg->var[2],msg->var[3],msg->var[4],msg->var[5]);
        if (msg->var[2] == 1) {
                    DEBUG(0,"ray_cs Sending authentication response.\n");
                    if (!build_auth_frame (local, msg->mac.addr_2, OPEN_AUTH_RESPONSE)) {
                        local->authentication_state = NEED_TO_AUTH;
                        memcpy(local->auth_id, msg->mac.addr_2, ADDRLEN);
                    }
        }
    }
    else /* Infrastructure network */
    {
        if (local->authentication_state == AWAITING_RESPONSE) {
            /* Verify authentication sequence #2 and success */
            if (msg->var[2] == 2) {
                if ((msg->var[3] | msg->var[4]) == 0) {
                    DEBUG(1,"Authentication successful\n");
                    local->card_status = CARD_AUTH_COMPLETE;
                    associate(local);
                    local->authentication_state = AUTHENTICATED;
                }
                else {
                    DEBUG(0,"Authentication refused\n");
                    local->card_status = CARD_AUTH_REFUSED;
                    join_net((u_long)local);
                    local->authentication_state = UNAUTHENTICATED;
                }
            }
        }
    }

} /* end rx_authenticate */
/*===========================================================================*/
static void associate(ray_dev_t *local)
{
    struct ccs *pccs;
    dev_link_t *link = local->finder;
    struct net_device *dev = link->priv;
    int ccsindex;
    if (!(link->state & DEV_PRESENT)) {
        DEBUG(2,"ray_cs associate - device not present\n");
        return;
    }
    /* If no tx buffers available, return*/
    if ((ccsindex = get_free_ccs(local)) < 0)
    {
/* TBD should never be here but... what if we are? */
        DEBUG(1,"ray_cs associate - No free ccs\n");
        return;
    }
    DEBUG(1,"ray_cs Starting association with access point\n");
    pccs = ((struct ccs *)(local->sram + CCS_BASE)) + ccsindex;
    /* fill in the CCS */
    writeb(CCS_START_ASSOCIATION, &pccs->cmd);
    /* Interrupt the firmware to process the command */
    if (interrupt_ecf(local, ccsindex)) {
        DEBUG(1,"ray_cs associate failed - ECF not ready for intr\n");
        writeb(CCS_BUFFER_FREE, &(pccs++)->buffer_status);

        del_timer(&local->timer);
        local->timer.expires = jiffies + HZ*2;
        local->timer.data = (long)local;
        local->timer.function = &join_net;
        add_timer(&local->timer);
        local->card_status = CARD_ASSOC_FAILED;
        return;
    }
    if (!sniffer) netif_start_queue(dev);

} /* end associate */
/*===========================================================================*/
static void rx_deauthenticate(ray_dev_t *local, struct rcs *prcs, 
                       unsigned int pkt_addr, int rx_len)
{
/*  UCHAR buff[256];
    struct rx_msg *msg = (struct rx_msg *)buff;
*/
    DEBUG(0,"Deauthentication frame received\n");
    local->authentication_state = UNAUTHENTICATED;
    /* Need to reauthenticate or rejoin depending on reason code */
/*  copy_from_rx_buff(local, buff, pkt_addr, rx_len & 0xff);
 */
}
/*===========================================================================*/
static void clear_interrupt(ray_dev_t *local)
{
    writeb(0, local->amem + CIS_OFFSET + HCS_INTR_OFFSET);
}
/*===========================================================================*/
#ifdef CONFIG_PROC_FS
#define MAXDATA (PAGE_SIZE - 80)

static char *card_status[] = {
    "Card inserted - uninitialized",     /* 0 */
    "Card not downloaded",               /* 1 */
    "Waiting for download parameters",   /* 2 */
    "Card doing acquisition",            /* 3 */
    "Acquisition complete",              /* 4 */
    "Authentication complete",           /* 5 */
    "Association complete",              /* 6 */
    "???", "???", "???", "???",          /* 7 8 9 10 undefined */
    "Card init error",                   /* 11 */
    "Download parameters error",         /* 12 */
    "???",                               /* 13 */
    "Acquisition failed",                /* 14 */
    "Authentication refused",            /* 15 */
    "Association failed"                 /* 16 */
};

static char *nettype[] = {"Adhoc", "Infra "};
static char *framing[] = {"Encapsulation", "Translation"}
;
/*===========================================================================*/
static int ray_cs_proc_read(char *buf, char **start, off_t offset, int len)
{
/* Print current values which are not available via other means
 * eg ifconfig 
 */
    int i;
    dev_link_t *link;
    struct net_device *dev;
    ray_dev_t *local;
    UCHAR *p;
    struct freq_hop_element *pfh;
    UCHAR c[33];

    link = dev_list;
    if (!link)
    	return 0;
    dev = (struct net_device *)link->priv;
    if (!dev)
    	return 0;
    local = (ray_dev_t *)dev->priv;
    if (!local)
    	return 0;

    len = 0;

    len += sprintf(buf + len, "Raylink Wireless LAN driver status\n");
    len += sprintf(buf + len, "%s\n", rcsid);
    /* build 4 does not report version, and field is 0x55 after memtest */
    len += sprintf(buf + len, "Firmware version     = ");
    if (local->fw_ver == 0x55)
        len += sprintf(buf + len, "4 - Use dump_cis for more details\n");
    else
        len += sprintf(buf + len, "%2d.%02d.%02d\n",
                   local->fw_ver, local->fw_bld, local->fw_var);

    for (i=0; i<32; i++) c[i] = local->sparm.b5.a_current_ess_id[i];
    c[32] = 0;
    len += sprintf(buf + len, "%s network ESSID = \"%s\"\n", 
                   nettype[local->sparm.b5.a_network_type], c);

    p = local->bss_id;
    len += sprintf(buf + len, 
                   "BSSID                = %02x:%02x:%02x:%02x:%02x:%02x\n",
                   p[0],p[1],p[2],p[3],p[4],p[5]);

    len += sprintf(buf + len, "Country code         = %d\n", 
                   local->sparm.b5.a_curr_country_code);

    i = local->card_status;
    if (i < 0) i = 10;
    if (i > 16) i = 10;
    len += sprintf(buf + len, "Card status          = %s\n", card_status[i]);

    len += sprintf(buf + len, "Framing mode         = %s\n",framing[translate]);

    len += sprintf(buf + len, "Last pkt signal lvl  = %d\n", local->last_rsl);

    if (local->beacon_rxed) {
	/* Pull some fields out of last beacon received */
	len += sprintf(buf + len, "Beacon Interval      = %d Kus\n", 
		       local->last_bcn.beacon_intvl[0]
		       + 256 * local->last_bcn.beacon_intvl[1]);
    
    p = local->last_bcn.elements;
    if (p[0] == C_ESSID_ELEMENT_ID) p += p[1] + 2;
    else {
        len += sprintf(buf + len, "Parse beacon failed at essid element id = %d\n",p[0]);
        return len;
    }

    if (p[0] == C_SUPPORTED_RATES_ELEMENT_ID) {
        len += sprintf(buf + len, "Supported rate codes = ");
        for (i=2; i<p[1] + 2; i++) 
            len += sprintf(buf + len, "0x%02x ", p[i]);
        len += sprintf(buf + len, "\n");
        p += p[1] + 2;
    }
    else {
        len += sprintf(buf + len, "Parse beacon failed at rates element\n");
        return len;
    }

	if (p[0] == C_FH_PARAM_SET_ELEMENT_ID) {
	    pfh = (struct freq_hop_element *)p;
	    len += sprintf(buf + len, "Hop dwell            = %d Kus\n",
			   pfh->dwell_time[0] + 256 * pfh->dwell_time[1]);
	    len += sprintf(buf + len, "Hop set              = %d \n", pfh->hop_set);
	    len += sprintf(buf + len, "Hop pattern          = %d \n", pfh->hop_pattern);
	    len += sprintf(buf + len, "Hop index            = %d \n", pfh->hop_index);
	    p += p[1] + 2;
	}
	else {
	    len += sprintf(buf + len, "Parse beacon failed at FH param element\n");
	    return len;
	}
    } else {
	len += sprintf(buf + len, "No beacons received\n");
    }
    return len;
}

#endif
/*===========================================================================*/
static int build_auth_frame(ray_dev_t *local, UCHAR *dest, int auth_type)
{
    int addr;
    struct ccs *pccs;
    struct tx_msg *ptx;
    int ccsindex;

    /* If no tx buffers available, return */
    if ((ccsindex = get_free_tx_ccs(local)) < 0)
    {
        DEBUG(1,"ray_cs send authenticate - No free tx ccs\n");
        return -1;
    }

    pccs = ((struct ccs *)(local->sram + CCS_BASE)) + ccsindex;

    /* Address in card space */
    addr = TX_BUF_BASE + (ccsindex << 11);
    /* fill in the CCS */
    writeb(CCS_TX_REQUEST, &pccs->cmd);
    writeb(addr >> 8, pccs->var.tx_request.tx_data_ptr);
    writeb(0x20, pccs->var.tx_request.tx_data_ptr + 1);
    writeb(TX_AUTHENTICATE_LENGTH_MSB, pccs->var.tx_request.tx_data_length);
    writeb(TX_AUTHENTICATE_LENGTH_LSB,pccs->var.tx_request.tx_data_length + 1);
    writeb(0, &pccs->var.tx_request.pow_sav_mode);

    ptx = (struct tx_msg *)(local->sram + addr);
    /* fill in the mac header */
    writeb(PROTOCOL_VER | AUTHENTIC_TYPE, &ptx->mac.frame_ctl_1);
    writeb(0, &ptx->mac.frame_ctl_2);

    memcpy_toio(ptx->mac.addr_1, dest, ADDRLEN);
    memcpy_toio(ptx->mac.addr_2, local->sparm.b4.a_mac_addr, ADDRLEN);
    memcpy_toio(ptx->mac.addr_3, local->bss_id, ADDRLEN);

    /* Fill in msg body with protocol 00 00, sequence 01 00 ,status 00 00 */
    memset_io(ptx->var, 0, 6);
    writeb(auth_type & 0xff, ptx->var + 2);

    /* Interrupt the firmware to process the command */
    if (interrupt_ecf(local, ccsindex)) {
        DEBUG(1,"ray_cs send authentication request failed - ECF not ready for intr\n");
        writeb(CCS_BUFFER_FREE, &(pccs++)->buffer_status);
        return -1;
    }
    return 0;
} /* End build_auth_frame */

/*===========================================================================*/
#ifdef CONFIG_PROC_FS
static void raycs_write(const char *name, write_proc_t *w, void *data)
{
	struct proc_dir_entry * entry = create_proc_entry(name, S_IFREG | S_IWUSR, NULL);
	if (entry) {
		entry->write_proc = w;
		entry->data = data;
	}
}

static int write_essid(struct file *file, const char *buffer, unsigned long count, void *data)
{
	static char proc_essid[33];
	int len = count;

	if (len > 32)
		len = 32;
	memset(proc_essid, 0, 33);
	if (copy_from_user(proc_essid, buffer, len))
		return -EFAULT;
	essid = proc_essid;
	return count;
}

static int write_int(struct file *file, const char *buffer, unsigned long count, void *data)
{
	static char proc_number[10];
	char *p;
	int nr, len;

	if (!count)
		return 0;

	if (count > 9)
		return -EINVAL;
	if (copy_from_user(proc_number, buffer, count))
		return -EFAULT;
	p = proc_number;
	nr = 0;
	len = count;
	do {
		unsigned int c = *p - '0';
		if (c > 9)
			return -EINVAL;
		nr = nr*10 + c;
		p++;
	} while (--len);
	*(int *)data = nr;
	return count;
}
#endif

static int __init init_ray_cs(void)
{
    int rc;
    
    DEBUG(1, "%s\n", rcsid);
    rc = register_pcmcia_driver(&dev_info, &ray_attach, &ray_detach);
    DEBUG(1, "raylink init_module register_pcmcia_driver returns 0x%x\n",rc);

#ifdef CONFIG_PROC_FS
    proc_mkdir("driver/ray_cs", 0);

    create_proc_info_entry("driver/ray_cs/ray_cs", 0, NULL, &ray_cs_proc_read);
    raycs_write("driver/ray_cs/essid", write_essid, NULL);
    raycs_write("driver/ray_cs/net_type", write_int, &net_type);
    raycs_write("driver/ray_cs/translate", write_int, &translate);
#endif
    if (translate != 0) translate = 1;
    return 0;
} /* init_ray_cs */

/*===========================================================================*/

static void __exit exit_ray_cs(void)
{
    DEBUG(0, "ray_cs: cleanup_module\n");


#ifdef CONFIG_PROC_FS
    remove_proc_entry("ray_cs", proc_root_driver);
#endif

    unregister_pcmcia_driver(&dev_info);
    while (dev_list != NULL)
        ray_detach(dev_list);

#ifdef CONFIG_PROC_FS
    remove_proc_entry("driver/ray_cs/ray_cs", NULL);
    remove_proc_entry("driver/ray_cs/essid", NULL);
    remove_proc_entry("driver/ray_cs/net_type", NULL);
    remove_proc_entry("driver/ray_cs/translate", NULL);
    remove_proc_entry("driver/ray_cs", NULL);
#endif
} /* exit_ray_cs */

module_init(init_ray_cs);
module_exit(exit_ray_cs);

/*===========================================================================*/
