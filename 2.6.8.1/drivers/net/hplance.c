/* hplance.c  : the  Linux/hp300/lance ethernet driver
 *
 * Copyright (C) 05/1998 Peter Maydell <pmaydell@chiark.greenend.org.uk>
 * Based on the Sun Lance driver and the NetBSD HP Lance driver
 * Uses the generic 7990.c LANCE code.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/errno.h>
/* Used for the temporal inet entries and routing */
#include <linux/socket.h>
#include <linux/route.h>
#include <linux/dio.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/pgtable.h>

#include "hplance.h"

/* We have 16834 bytes of RAM for the init block and buffers. This places
 * an upper limit on the number of buffers we can use. NetBSD uses 8 Rx
 * buffers and 2 Tx buffers.
 */
#define LANCE_LOG_TX_BUFFERS 1
#define LANCE_LOG_RX_BUFFERS 3

#include "7990.h"                                 /* use generic LANCE code */

/* Our private data structure */
struct hplance_private {
  struct lance_private lance;
  unsigned int scode;
  void *base;
};

/* function prototypes... This is easy because all the grot is in the
 * generic LANCE support. All we have to support is probing for boards,
 * plus board-specific init, open and close actions. 
 * Oh, and we need to tell the generic code how to read and write LANCE registers...
 */
static void hplance_init(struct net_device *dev, int scode);
static int hplance_open(struct net_device *dev);
static int hplance_close(struct net_device *dev);
static void hplance_writerap(void *priv, unsigned short value);
static void hplance_writerdp(void *priv, unsigned short value);
static unsigned short hplance_readrdp(void *priv);

#ifdef MODULE
static struct hplance_private *root_hplance_dev;
#endif

static void cleanup_card(struct net_device *dev)
{
        struct hplance_private *lp = netdev_priv(dev);
	dio_unconfig_board(lp->scode);
}

/* Find all the HP Lance boards and initialise them... */
struct net_device * __init hplance_probe(int unit)
{
	struct net_device *dev;

        if (!MACH_IS_HP300)
                return ERR_PTR(-ENODEV);

	dev = alloc_etherdev(sizeof(struct hplance_private));
	if (!dev)
		return ERR_PTR(-ENOMEM);

	if (unit >= 0) {
		sprintf(dev->name, "eth%d", unit);
		netdev_boot_setup_check(dev);
	}

	SET_MODULE_OWNER(dev);
        
        /* Isn't DIO nice? */
        for(;;)
        {
                int scode = dio_find(DIO_ID_LAN);
                                
                if (!scode)
                        break;
                
		dio_config_board(scode);
                hplance_init(dev, scode);
		if (!register_netdev(dev)) {
			struct hplance_private *lp = netdev_priv(dev);
			lp->next_module = root_hplance_dev;
			root_hplance_dev = lp;
			return dev;
		}
		cleanup_card(dev);
        }
	free_netdev(dev);
	return ERR_PTR(-ENODEV);
}

/* Initialise a single lance board at the given select code */
static void __init hplance_init(struct net_device *dev, int scode)
{
        const char *name = dio_scodetoname(scode);
        void *va = dio_scodetoviraddr(scode);
        struct hplance_private *lp;
        int i;
        
        printk("%s: %s; select code %d, addr", dev->name, name, scode);

        /* reset the board */
        out_8(va+DIO_IDOFF, 0xff);
        udelay(100);                              /* ariba! ariba! udelay! udelay! */

        /* Fill the dev fields */
        dev->base_addr = (unsigned long)va;
        dev->open = &hplance_open;
        dev->stop = &hplance_close;
        dev->hard_start_xmit = &lance_start_xmit;
        dev->get_stats = &lance_get_stats;
        dev->set_multicast_list = &lance_set_multicast;
        dev->dma = 0;
        
        for (i=0; i<6; i++)
        {
                /* The NVRAM holds our ethernet address, one nibble per byte,
                 * at bytes NVRAMOFF+1,3,5,7,9...
                 */
                dev->dev_addr[i] = ((in_8(va + HPLANCE_NVRAMOFF + i*4 + 1) & 0xF) << 4)
                        | (in_8(va + HPLANCE_NVRAMOFF + i*4 + 3) & 0xF);
                printk("%c%2.2x", i == 0 ? ' ' : ':', dev->dev_addr[i]);
        }
        
        lp = netdev_priv(dev);
        lp->lance.name = (char*)name;                   /* discards const, shut up gcc */
        lp->lance.ll = (struct lance_regs *)(va + HPLANCE_REGOFF);
        lp->lance.init_block = (struct lance_init_block *)(va + HPLANCE_MEMOFF); /* CPU addr */
        lp->lance.lance_init_block = 0;                 /* LANCE addr of same RAM */
        lp->lance.busmaster_regval = LE_C3_BSWP;        /* we're bigendian */
        lp->lance.irq = dio_scodetoipl(scode);
        lp->lance.writerap = hplance_writerap;
        lp->lance.writerdp = hplance_writerdp;
        lp->lance.readrdp = hplance_readrdp;
        lp->lance.lance_log_rx_bufs = LANCE_LOG_RX_BUFFERS;
        lp->lance.lance_log_tx_bufs = LANCE_LOG_TX_BUFFERS;
        lp->lance.rx_ring_mod_mask = RX_RING_MOD_MASK;
        lp->lance.tx_ring_mod_mask = TX_RING_MOD_MASK;
        lp->scode = scode;
	lp->base = va;
	printk(", irq %d\n", lp->lance.irq);
}

/* This is disgusting. We have to check the DIO status register for ack every
 * time we read or write the LANCE registers.
 */
static void hplance_writerap(void *priv, unsigned short value)
{
	struct hplance_private *lp = (struct hplance_private *)priv;
        struct hplance_reg *hpregs = (struct hplance_reg *)lp->base;
        do {
                lp->lance.ll->rap = value;
        } while ((hpregs->status & LE_ACK) == 0);
}

static void hplance_writerdp(void *priv, unsigned short value)
{
	struct hplance_private *lp = (struct hplance_private *)priv;
        struct hplance_reg *hpregs = (struct hplance_reg *)lp->base;
        do {
                lp->lance.ll->rdp = value;
        } while ((hpregs->status & LE_ACK) == 0);
}

static unsigned short hplance_readrdp(void *priv)
{
        unsigned short val;
	struct hplance_private *lp = (struct hplance_private *)priv;
        struct hplance_reg *hpregs = (struct hplance_reg *)lp->base;
        do {
                val = lp->lance.ll->rdp;
        } while ((hpregs->status & LE_ACK) == 0);
        return val;
}

static int hplance_open(struct net_device *dev)
{
        int status;
        struct hplance_private *lp = netdev_priv(dev);
        struct hplance_reg *hpregs = (struct hplance_reg *)lp->base;
        
        status = lance_open(dev);                 /* call generic lance open code */
        if (status)
                return status;
        /* enable interrupts at board level. */
        out_8(&(hpregs->status), LE_IE);

        return 0;
}

static int hplance_close(struct net_device *dev)
{
        struct hplance_private *lp = netdev_priv(dev);
        struct hplance_reg *hpregs = (struct hplance_reg *)lp->base;
        out_8(&(hpregs->status), 8);              /* disable interrupts at boardlevel */
        lance_close(dev);
        return 0;
}

#ifdef MODULE
MODULE_LICENSE("GPL");
int init_module(void)
{
	int found = 0;
	while (!IS_ERR(hplance_probe(-1)))
		found++;
	return found ? 0 : -ENODEV;
}

void cleanup_module(void)
{
        /* Walk the chain of devices, unregistering them */
        struct hplance_private *lp;
        while (root_hplance_dev) {
                lp = root_hplance_dev->next_module;
                unregister_netdev(root_lance_dev->dev);
                cleanup_card(root_lance_dev->dev);
                free_netdev(root_lance_dev->dev);
                root_lance_dev = lp;
        }
}

#endif /* MODULE */
