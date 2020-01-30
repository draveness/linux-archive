/* stnic.c : A SH7750 specific part of driver for NS DP83902A ST-NIC.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1999 kaz Kojima
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/hitachi_se.h>
#include <asm/machvec.h>

#include "8390.h"

#define byte	unsigned char
#define half	unsigned short
#define word	unsigned int
#define vbyte	volatile unsigned char
#define vhalf	volatile unsigned short
#define vword	volatile unsigned int

#define STNIC_RUN	0x01	/* 1 == Run, 0 == reset. */

#define START_PG	0	/* First page of TX buffer */
#define STOP_PG		128	/* Last page +1 of RX ring */

/* Alias */
#define STNIC_CR	E8390_CMD
#define PG0_RSAR0	EN0_RSARLO
#define PG0_RSAR1	EN0_RSARHI
#define PG0_RBCR0	EN0_RCNTLO
#define PG0_RBCR1	EN0_RCNTHI

#define CR_RRD		E8390_RREAD
#define CR_RWR		E8390_RWRITE
#define CR_PG0		E8390_PAGE0
#define CR_STA		E8390_START
#define CR_RDMA		E8390_NODMA

/* FIXME! YOU MUST SET YOUR OWN ETHER ADDRESS.  */
static byte stnic_eadr[6] =
{0x00, 0xc0, 0x6e, 0x00, 0x00, 0x07};

static struct net_device *stnic_dev;

static int stnic_open (struct net_device *dev);
static int stnic_close (struct net_device *dev);
static void stnic_reset (struct net_device *dev);
static void stnic_get_hdr (struct net_device *dev, struct e8390_pkt_hdr *hdr,
			   int ring_page);
static void stnic_block_input (struct net_device *dev, int count,
			       struct sk_buff *skb , int ring_offset);
static void stnic_block_output (struct net_device *dev, int count,
				const unsigned char *buf, int start_page);

static void stnic_init (struct net_device *dev);

/* SH7750 specific read/write io. */
static inline void
STNIC_DELAY (void)
{
  vword trash;
  trash = *(vword *) 0xa0000000;
  trash = *(vword *) 0xa0000000;
}

static inline byte
STNIC_READ (int reg)
{
  byte val;

  val = (*(vhalf *) (PA_83902 + ((reg) << 1)) >> 8) & 0xff;
  STNIC_DELAY ();
  return val;
}

static inline void
STNIC_WRITE (int reg, byte val)
{
  *(vhalf *) (PA_83902 + ((reg) << 1)) = ((half) (val) << 8);
  STNIC_DELAY ();
}

int __init stnic_probe(void)
{
  struct net_device *dev;
  int i;

  /* If we are not running on a SolutionEngine, give up now */
  if (! MACH_SE)
    return -ENODEV;

  /* New style probing API */
  dev = init_etherdev (NULL, 0);
  if (!dev)
  	return -ENOMEM;
  SET_MODULE_OWNER(dev);
  stnic_dev = dev;

  /* Allocate dev->priv and fill in 8390 specific dev fields. */
  if (ethdev_init (dev))
    {
      printk ("Unable to get memory for dev->priv.\n");
      return -ENOMEM;
    }

  for (i = 0; i < ETHER_ADDR_LEN; i++)
    dev->dev_addr[i] = stnic_eadr[i];

  /* Set the base address to point to the NIC, not the "real" base! */
  dev->base_addr = 0x1000;
  dev->irq = IRQ_STNIC;
  dev->open = &stnic_open;
  dev->stop = &stnic_close;

  /* Snarf the interrupt now.  There's no point in waiting since we cannot
     share and the board will usually be enabled. */
  i = request_irq (dev->irq, ei_interrupt, 0, dev->name, dev);
  if (i)  {
      printk (" unable to get IRQ %d.\n", dev->irq);
      unregister_netdev(dev);
      kfree(dev->priv);
      kfree(dev);
      return i;
    }

  ei_status.name = dev->name;
  ei_status.word16 = 1;
  ei_status.tx_start_page = START_PG;
  ei_status.rx_start_page = START_PG + TX_PAGES;
  ei_status.stop_page = STOP_PG;

  ei_status.reset_8390 = &stnic_reset;
  ei_status.get_8390_hdr = &stnic_get_hdr;
  ei_status.block_input = &stnic_block_input;
  ei_status.block_output = &stnic_block_output;
  stnic_init (dev);

  printk (KERN_INFO "NS ST-NIC 83902A\n");

  return 0;
}

static int
stnic_open (struct net_device *dev)
{
#if 0
  printk ("stnic open\n");
#endif
  ei_open (dev);
  return 0;
}

static int
stnic_close (struct net_device *dev)
{
  ei_close (dev);
  return 0;
}

static void
stnic_reset (struct net_device *dev)
{
  *(vhalf *) PA_83902_RST = 0;
  udelay (5);
  if (ei_debug > 1)
    printk("8390 reset done (%ld).\n", jiffies);
  *(vhalf *) PA_83902_RST = ~0;
  udelay (5);
}

static void
stnic_get_hdr (struct net_device *dev, struct e8390_pkt_hdr *hdr,
	       int ring_page)
{
  half buf[2];

  STNIC_WRITE (PG0_RSAR0, 0);
  STNIC_WRITE (PG0_RSAR1, ring_page);
  STNIC_WRITE (PG0_RBCR0, 4);
  STNIC_WRITE (PG0_RBCR1, 0);
  STNIC_WRITE (STNIC_CR, CR_RRD | CR_PG0 | CR_STA);

  buf[0] = *(vhalf *) PA_83902_IF;
  STNIC_DELAY ();
  buf[1] = *(vhalf *) PA_83902_IF;
  STNIC_DELAY ();
  hdr->next = buf[0] >> 8;
  hdr->status = buf[0] & 0xff;
#ifdef __LITTLE_ENDIAN__
  hdr->count = buf[1];
#else
  hdr->count = ((buf[1] >> 8) & 0xff) | (buf[1] << 8);
#endif

  if (ei_debug > 1)
    printk ("ring %x status %02x next %02x count %04x.\n",
	    ring_page, hdr->status, hdr->next, hdr->count);

  STNIC_WRITE (STNIC_CR, CR_RDMA | CR_PG0 | CR_STA);
}

/* Block input and output, similar to the Crynwr packet driver. If you are
   porting to a new ethercard look at the packet driver source for hints.
   The HP LAN doesn't use shared memory -- we put the packet
   out through the "remote DMA" dataport. */

static void
stnic_block_input (struct net_device *dev, int length, struct sk_buff *skb,
		   int offset)
{
  char *buf = skb->data;
  half val;

  STNIC_WRITE (PG0_RSAR0, offset & 0xff);
  STNIC_WRITE (PG0_RSAR1, offset >> 8);
  STNIC_WRITE (PG0_RBCR0, length & 0xff);
  STNIC_WRITE (PG0_RBCR1, length >> 8);
  STNIC_WRITE (STNIC_CR, CR_RRD | CR_PG0 | CR_STA);

  if (length & 1)
    length++;

  while (length > 0)
    {
      val = *(vhalf *) PA_83902_IF;
#ifdef __LITTLE_ENDIAN__
      *buf++ = val & 0xff;
      *buf++ = val >> 8;
#else
      *buf++ = val >> 8;
      *buf++ = val & 0xff;
#endif
      STNIC_DELAY ();
      length -= sizeof (half);
    }

  STNIC_WRITE (STNIC_CR, CR_RDMA | CR_PG0 | CR_STA);
}

static void
stnic_block_output (struct net_device *dev, int length,
		    const unsigned char *buf, int output_page)
{
#if 0
  STNIC_WRITE (PG0_RBCR0, 1);
  STNIC_WRITE (STNIC_CR, CR_RRD | CR_PG0 | CR_STA);
#else  /* XXX: I don't know why but this works.  -- gniibe  */
  STNIC_WRITE (PG0_RBCR0, 0x42);
  STNIC_WRITE (PG0_RBCR1, 0x00);
  STNIC_WRITE (PG0_RBCR0, 0x42);
  STNIC_WRITE (PG0_RBCR1, 0x00);
  STNIC_WRITE (STNIC_CR, CR_RRD | CR_PG0 | CR_STA);
  STNIC_DELAY ();
#endif

  STNIC_WRITE (PG0_RSAR0, 0);
  STNIC_WRITE (PG0_RSAR1, output_page);
  STNIC_WRITE (PG0_RBCR0, length & 0xff);
  STNIC_WRITE (PG0_RBCR1, length >> 8);
  STNIC_WRITE (STNIC_CR, CR_RWR | CR_PG0 | CR_STA);

  if (length & 1)
    length++;

  while (length > 0)
    {
#ifdef __LITTLE_ENDIAN__
      *(vhalf *) PA_83902_IF = ((half) buf[1] << 8) | buf[0];
#else
      *(vhalf *) PA_83902_IF = ((half) buf[0] << 8) | buf[1];
#endif
      STNIC_DELAY ();
      buf += sizeof (half);
      length -= sizeof (half);
    }

  STNIC_WRITE (STNIC_CR, CR_RDMA | CR_PG0 | CR_STA);
}

/* This function resets the STNIC if something screws up.  */
static void
stnic_init (struct net_device *dev)
{
  stnic_reset (dev);
  NS8390_init (dev, 0);
  return;
}

/* Hardware interrupt handler.  */
extern void ei_interrupt (int irq, void *dev_id, struct pt_regs *regs);

void
do_stnic_intr (int irq, void *dev_id, struct pt_regs *regs)
{
  ei_interrupt (0, stnic_dev, regs);
}

module_init(stnic_probe);
/* No cleanup routine. */
