/* New Hydra driver using generic 8390 core */
/* Based on old hydra driver by Topi Kanerva (topi@susanna.oulu.fi) */

/* This file is subject to the terms and conditions of the GNU General      */
/* Public License.  See the file COPYING in the main directory of the       */
/* Linux distribution for more details.                                     */

/* Peter De Schrijver (p2@mind.be) */
/* Oldenburg 2000 */

/* The Amiganet is a Zorro-II board made by Hydra Systems. It contains a    */
/* NS8390 NIC (network interface controller) clone, 16 or 64K on-board RAM  */
/* and 10BASE-2 (thin coax) and AUI connectors.                             */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/malloc.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>

#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/irq.h>

#include <asm/amigaints.h>
#include <asm/amigahw.h>
#include <linux/zorro.h>

#include "8390.h"

#define NE_BASE		(dev->base_addr)
#define NE_CMD		(0x00*2)

#define NE_EN0_ISR      (0x07*2)
#define NE_EN0_DCFG     (0x0e*2)

#define NE_EN0_RSARLO   (0x08*2)
#define NE_EN0_RSARHI   (0x09*2)
#define NE_EN0_RCNTLO   (0x0a*2)
#define NE_EN0_RXCR     (0x0c*2)
#define NE_EN0_TXCR     (0x0d*2)
#define NE_EN0_RCNTHI   (0x0b*2)
#define NE_EN0_IMR      (0x0f*2)

#define NESM_START_PG   0x0    /* First page of TX buffer */
#define NESM_STOP_PG    0x40    /* Last page +1 of RX ring */

#define HYDRA_NIC_BASE 0xffe1
#define HYDRA_ADDRPROM 0xffc0
#define HYDRA_VERSION "v3.0alpha"

#define WORDSWAP(a)     ((((a)>>8)&0xff) | ((a)<<8))

#ifdef MODULE
static struct net_device *root_hydra_dev = NULL;
#endif

static int __init hydra_probe(void);
static int hydra_init(unsigned long board);
static int hydra_open(struct net_device *dev);
static int hydra_close(struct net_device *dev);
static void hydra_reset_8390(struct net_device *dev);
static void hydra_get_8390_hdr(struct net_device *dev,
			       struct e8390_pkt_hdr *hdr, int ring_page);
static void hydra_block_input(struct net_device *dev, int count,
			      struct sk_buff *skb, int ring_offset);
static void hydra_block_output(struct net_device *dev, int count,
			       const unsigned char *buf, int start_page);
static void __exit hydra_cleanup(void);

static int __init hydra_probe(void)
{
    struct zorro_dev *z = NULL;
    unsigned long board;
    int err = -ENODEV;

    if (load_8390_module("hydra.c"))
	return -ENOSYS;

    while ((z = zorro_find_device(ZORRO_PROD_HYDRA_SYSTEMS_AMIGANET, z))) {
	board = z->resource.start;
	if (!request_mem_region(board, 0x10000, "Hydra"))
	    continue;
	if ((err = hydra_init(ZTWO_VADDR(board)))) {
	    release_mem_region(board, 0x10000);
	    return err;
	}
	err = 0;
    }

    if (err == -ENODEV) {
	printk("No Hydra ethernet card found.\n");
	unload_8390_module();
    }
    return err;
}

int __init hydra_init(unsigned long board)
{
    struct net_device *dev;
    unsigned long ioaddr = board+HYDRA_NIC_BASE;
    const char *name = NULL;
    int start_page, stop_page;
    int j;

    static u32 hydra_offsets[16] = {
	0x00, 0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0e,
	0x10, 0x12, 0x14, 0x16, 0x18, 0x1a, 0x1c, 0x1e,
    };

    dev = init_etherdev(0, 0);
    if (!dev)
	return -ENOMEM;

    for(j = 0; j < ETHER_ADDR_LEN; j++)
	dev->dev_addr[j] = *((u8 *)(board + HYDRA_ADDRPROM + 2*j));

    /* We must set the 8390 for word mode. */
    writeb(0x4b, ioaddr + NE_EN0_DCFG);
    start_page = NESM_START_PG;
    stop_page = NESM_STOP_PG;

    dev->base_addr = ioaddr;
    dev->irq = IRQ_AMIGA_PORTS;

    /* Install the Interrupt handler */
    if (request_irq(IRQ_AMIGA_PORTS, ei_interrupt, SA_SHIRQ, "Hydra Ethernet",
		    dev))
	return -EAGAIN;

    /* Allocate dev->priv and fill in 8390 specific dev fields. */
    if (ethdev_init(dev)) {
	printk("Unable to get memory for dev->priv.\n");
	return -ENOMEM;
    }

    name = "NE2000";

    printk("%s: hydra at 0x%08lx, address %02x:%02x:%02x:%02x:%02x:%02x (hydra.c " HYDRA_VERSION ")\n", dev->name, ZTWO_PADDR(board),
	dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
	dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5]);

    ei_status.name = name;
    ei_status.tx_start_page = start_page;
    ei_status.stop_page = stop_page;
    ei_status.word16 = 1;
    ei_status.bigendian = 1;

    ei_status.rx_start_page = start_page + TX_PAGES;

    ei_status.reset_8390 = &hydra_reset_8390;
    ei_status.block_input = &hydra_block_input;
    ei_status.block_output = &hydra_block_output;
    ei_status.get_8390_hdr = &hydra_get_8390_hdr;
    ei_status.reg_offset = hydra_offsets;
    dev->open = &hydra_open;
    dev->stop = &hydra_close;
#ifdef MODULE
    ei_status.priv = (unsigned long)root_hydra_dev;
    root_hydra_dev = dev;
#endif
    NS8390_init(dev, 0);
    return 0;
}

static int hydra_open(struct net_device *dev)
{
    ei_open(dev);
    MOD_INC_USE_COUNT;
    return 0;
}

static int hydra_close(struct net_device *dev)
{
    if (ei_debug > 1)
	printk("%s: Shutting down ethercard.\n", dev->name);
    ei_close(dev);
    MOD_DEC_USE_COUNT;
    return 0;
}

static void hydra_reset_8390(struct net_device *dev)
{
    printk("Hydra hw reset not there\n");
}

static void hydra_get_8390_hdr(struct net_device *dev,
			       struct e8390_pkt_hdr *hdr, int ring_page)
{
    int nic_base = dev->base_addr;
    short *ptrs;
    unsigned long hdr_start= (nic_base-HYDRA_NIC_BASE) +
			     ((ring_page - NESM_START_PG)<<8);
    ptrs = (short *)hdr;

    *(ptrs++) = readw(hdr_start);
    *((short *)hdr) = WORDSWAP(*((short *)hdr));
    hdr_start += 2;
    *(ptrs++) = readw(hdr_start);
    *((short *)hdr+1) = WORDSWAP(*((short *)hdr+1));
}

static void hydra_block_input(struct net_device *dev, int count,
			      struct sk_buff *skb, int ring_offset)
{
    unsigned long nic_base = dev->base_addr;
    unsigned long mem_base = nic_base - HYDRA_NIC_BASE;
    unsigned long xfer_start = mem_base + ring_offset - (NESM_START_PG<<8);

    if (count&1)
	count++;

    if (xfer_start+count >  mem_base + (NESM_STOP_PG<<8)) {
	int semi_count = (mem_base + (NESM_STOP_PG<<8)) - xfer_start;

	memcpy_fromio(skb->data,xfer_start,semi_count);
	count -= semi_count;
	memcpy_fromio(skb->data+semi_count, mem_base, count);
    } else
	memcpy_fromio(skb->data, xfer_start,count);

}

static void hydra_block_output(struct net_device *dev, int count,
			       const unsigned char *buf, int start_page)
{
    unsigned long nic_base = dev->base_addr;
    unsigned long mem_base = nic_base - HYDRA_NIC_BASE;

    if (count&1)
	count++;

    memcpy_toio(mem_base+((start_page - NESM_START_PG)<<8), buf, count);
}

static void __exit hydra_cleanup(void)
{
#ifdef MODULE
    struct net_device *dev, *next;

    while ((dev = root_hydra_dev)) {
	next = (struct net_device *)(ei_status.priv);
	unregister_netdev(dev);
	free_irq(IRQ_AMIGA_PORTS, dev);
	release_mem_region(ZTWO_PADDR(dev->base_addr)-HYDRA_NIC_BASE, 0x10000);
	kfree(dev);
	root_hydra_dev = next;
    }
    unload_8390_module();
#endif
}

module_init(hydra_probe);
module_exit(hydra_cleanup);
