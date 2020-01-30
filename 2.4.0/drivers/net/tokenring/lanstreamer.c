/*
 *   lanstreamer.c -- driver for the IBM Auto LANStreamer PCI Adapter
 *
 *  Written By: Mike Sullivan, IBM Corporation
 *
 *  Copyright (C) 1999 IBM Corporation
 *
 *  Linux driver for IBM PCI tokenring cards based on the LanStreamer MPC
 *  chipset. 
 *
 *  This driver is based on the olympic driver for IBM PCI TokenRing cards (Pit/Pit-Phy/Olympic
 *  chipsets) written  by:
 *      1999 Peter De Schrijver All Rights Reserved
 *	1999 Mike Phillips (phillim@amtrak.com)
 *
 *  Base Driver Skeleton:
 *      Written 1993-94 by Donald Becker.
 *
 *      Copyright 1993 United States Government as represented by the
 *      Director, National Security Agency.
 *
 * This program is free software; you can redistribute it and/or modify      
 * it under the terms of the GNU General Public License as published by      
 * the Free Software Foundation; either version 2 of the License, or         
 * (at your option) any later version.                                       
 *                                                                           
 * This program is distributed in the hope that it will be useful,           
 * but WITHOUT ANY WARRANTY; without even the implied warranty of            
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             
 * GNU General Public License for more details.                              
 *                                                                           
 * NO WARRANTY                                                               
 * THE PROGRAM IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OR        
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED INCLUDING, WITHOUT      
 * LIMITATION, ANY WARRANTIES OR CONDITIONS OF TITLE, NON-INFRINGEMENT,      
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Each Recipient is    
 * solely responsible for determining the appropriateness of using and       
 * distributing the Program and assumes all risks associated with its        
 * exercise of rights under this Agreement, including but not limited to     
 * the risks and costs of program errors, damage to or loss of data,         
 * programs or equipment, and unavailability or interruption of operations.  
 *                                                                           
 * DISCLAIMER OF LIABILITY                                                   
 * NEITHER RECIPIENT NOR ANY CONTRIBUTORS SHALL HAVE ANY LIABILITY FOR ANY   
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL        
 * DAMAGES (INCLUDING WITHOUT LIMITATION LOST PROFITS), HOWEVER CAUSED AND   
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR     
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE    
 * USE OR DISTRIBUTION OF THE PROGRAM OR THE EXERCISE OF ANY RIGHTS GRANTED  
 * HEREUNDER, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGES             
 *                                                                           
 * You should have received a copy of the GNU General Public License         
 * along with this program; if not, write to the Free Software               
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 *                                                                           
 * 
 *  12/10/99 - Alpha Release 0.1.0
 *            First release to the public
 *  03/03/00 - Merged to kernel, indented -kr -i8 -bri0, fixed some missing
 *		malloc free checks, reviewed code. <alan@redhat.com>
 *  03/13/00 - Added spinlocks for smp
 *  
 *  To Do:
 *
 *  1) Test Network Monitor Mode
 *  2) Add auto reset logic on adapter errors
 *  3) Test with varying options
 *
 *  If Problems do Occur
 *  Most problems can be rectified by either closing and opening the interface
 *  (ifconfig down and up) or rmmod and insmod'ing the driver (a bit difficult
 *  if compiled into the kernel).
 */

/* Change STREAMER_DEBUG to 1 to get verbose, and I mean really verbose, messages */

#define STREAMER_DEBUG 0
#define STREAMER_DEBUG_PACKETS 0

/* Change STREAMER_NETWORK_MONITOR to receive mac frames through the arb channel.
 * Will also create a /proc/net/streamer_tr entry if proc_fs is compiled into the
 * kernel.
 * Intended to be used to create a ring-error reporting network module 
 * i.e. it will give you the source address of beaconers on the ring 
 */

#define STREAMER_NETWORK_MONITOR 0

#include <linux/config.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/in.h>
#include <linux/ioport.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/ptrace.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/trdevice.h>
#include <linux/stddef.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <net/checksum.h>

#include <asm/io.h>
#include <asm/system.h>
#include <asm/bitops.h>

#include "lanstreamer.h"

/* I've got to put some intelligence into the version number so that Peter and I know
 * which version of the code somebody has got. 
 * Version Number = a.b.c.d  where a.b.c is the level of code and d is the latest author.
 * So 0.0.1.pds = Peter, 0.0.1.mlp = Mike
 * 
 * Official releases will only have an a.b.c version number format.
 */

static char *version = "LanStreamer.c v0.3.1 03/13/99 - Mike Sullivan";

static char *open_maj_error[] = {
	"No error", "Lobe Media Test", "Physical Insertion",
	"Address Verification", "Neighbor Notification (Ring Poll)",
	"Request Parameters", "FDX Registration Request",
	"FDX Lobe Media Test", "FDX Duplicate Address Check",
	"Unknown stage"
};

static char *open_min_error[] = {
	"No error", "Function Failure", "Signal Lost", "Wire Fault",
	"Ring Speed Mismatch", "Timeout", "Ring Failure", "Ring Beaconing",
	"Duplicate Node Address", "Request Parameters", "Remove Received",
	"Reserved", "Reserved", "No Monitor Detected for RPL",
	"Monitor Contention failer for RPL", "FDX Protocol Error"
};

/* Module paramters */

/* Ring Speed 0,4,16
 * 0 = Autosense         
 * 4,16 = Selected speed only, no autosense
 * This allows the card to be the first on the ring
 * and become the active monitor.
 *
 * WARNING: Some hubs will allow you to insert
 * at the wrong speed
 */

static int ringspeed[STREAMER_MAX_ADAPTERS] = { 0, };

MODULE_PARM(ringspeed, "1-" __MODULE_STRING(STREAMER_MAX_ADAPTERS) "i");

/* Packet buffer size */

static int pkt_buf_sz[STREAMER_MAX_ADAPTERS] = { 0, };

MODULE_PARM(pkt_buf_sz, "1-" __MODULE_STRING(STREAMER_MAX_ADAPTERS) "i");

/* Message Level */

static int message_level[STREAMER_MAX_ADAPTERS] = { 1, };

MODULE_PARM(message_level,
	    "1-" __MODULE_STRING(STREAMER_MAX_ADAPTERS) "i");

static int streamer_scan(struct net_device *dev);
static int streamer_init(struct net_device *dev);
static int streamer_open(struct net_device *dev);
static int streamer_xmit(struct sk_buff *skb, struct net_device *dev);
static int streamer_close(struct net_device *dev);
static void streamer_set_rx_mode(struct net_device *dev);
static void streamer_interrupt(int irq, void *dev_id,
			       struct pt_regs *regs);
static struct net_device_stats *streamer_get_stats(struct net_device *dev);
static int streamer_set_mac_address(struct net_device *dev, void *addr);
static void streamer_arb_cmd(struct net_device *dev);
static int streamer_change_mtu(struct net_device *dev, int mtu);
static void streamer_srb_bh(struct net_device *dev);
static void streamer_asb_bh(struct net_device *dev);
#if STREAMER_NETWORK_MONITOR
#ifdef CONFIG_PROC_FS
static int sprintf_info(char *buffer, struct net_device *dev);
#endif
#endif

int __init streamer_probe(struct net_device *dev)
{
	int cards_found;

	cards_found = streamer_scan(dev);
	return cards_found ? 0 : -ENODEV;
}

static int __init streamer_scan(struct net_device *dev)
{
	struct pci_dev *pci_device = NULL;
	struct streamer_private *streamer_priv;
	int card_no = 0;
	if (pci_present()) 
	{
		while ((pci_device = pci_find_device(PCI_VENDOR_ID_IBM,	PCI_DEVICE_ID_IBM_TR, pci_device))) 
		{
			if (pci_enable_device(pci_device))
				continue;
			pci_set_master(pci_device);

			/* Check to see if io has been allocated, if so, we've already done this card,
			   so continue on the card discovery loop  */

			if (check_region(pci_resource_start(pci_device,0), STREAMER_IO_SPACE)) 
			{
				card_no++;
				continue;
			}

			streamer_priv = kmalloc(sizeof(struct streamer_private), GFP_KERNEL);
			if(streamer_priv==NULL)
			{
				printk(KERN_ERR "lanstreamer: out of memory.\n");
				break;
			}
			memset(streamer_priv, 0, sizeof(struct streamer_private));
			init_waitqueue_head(&streamer_priv->srb_wait);
			init_waitqueue_head(&streamer_priv->trb_wait);
#ifndef MODULE
			dev = init_trdev(dev, 0);
			if(dev==NULL)
			{
				kfree(streamer_priv);
				printk(KERN_ERR "lanstreamer: out of memory.\n");
				break;
			}
#endif
			dev->priv = (void *) streamer_priv;
#if STREAMER_DEBUG
			printk("pci_device: %p, dev:%p, dev->priv: %p\n",
			       pci_device, dev, dev->priv);
#endif
			dev->irq = pci_device->irq;
			dev->base_addr = pci_resource_start(pci_device, 0);
			dev->init = &streamer_init;
			streamer_priv->streamer_card_name = (char *)pci_device->resource[0].name;
			streamer_priv->streamer_mmio = 
				ioremap(pci_resource_start(pci_device, 1), 256);

			if ((pkt_buf_sz[card_no] < 100) || (pkt_buf_sz[card_no] > 18000))
				streamer_priv->pkt_buf_sz = PKT_BUF_SZ;
			else
				streamer_priv->pkt_buf_sz = pkt_buf_sz[card_no];

			streamer_priv->streamer_ring_speed = ringspeed[card_no];
			streamer_priv->streamer_message_level = message_level[card_no];

			if (streamer_init(dev) == -1) {
				unregister_netdevice(dev);
				kfree(dev->priv);
				return 0;
			}

			dev->open = &streamer_open;
			dev->hard_start_xmit = &streamer_xmit;
			dev->change_mtu = &streamer_change_mtu;

			dev->stop = &streamer_close;
			dev->do_ioctl = NULL;
			dev->set_multicast_list = &streamer_set_rx_mode;
			dev->get_stats = &streamer_get_stats;
			dev->set_mac_address = &streamer_set_mac_address;
			return 1;
		}
	}
	return 0;
}


static int streamer_reset(struct net_device *dev)
{
	struct streamer_private *streamer_priv;
	__u8 *streamer_mmio;
	unsigned long t;
	unsigned int uaa_addr;
	struct sk_buff *skb = 0;
	__u16 misr;

	streamer_priv = (struct streamer_private *) dev->priv;
	streamer_mmio = streamer_priv->streamer_mmio;

	writew(readw(streamer_mmio + BCTL) | BCTL_SOFTRESET, streamer_mmio + BCTL);
	t = jiffies;
	/* Hold soft reset bit for a while */
	current->state = TASK_UNINTERRUPTIBLE;
	schedule_timeout(HZ);
	
	writew(readw(streamer_mmio + BCTL) & ~BCTL_SOFTRESET,
	       streamer_mmio + BCTL);

#if STREAMER_DEBUG
	printk("BCTL: %x\n", readw(streamer_mmio + BCTL));
	printk("GPR: %x\n", readw(streamer_mmio + GPR));
	printk("SISRMASK: %x\n", readw(streamer_mmio + SISR_MASK));
#endif

	if (streamer_priv->streamer_ring_speed == 0) {	/* Autosense */
		writew(readw(streamer_mmio + GPR) | GPR_AUTOSENSE,
		       streamer_mmio + GPR);
		if (streamer_priv->streamer_message_level)
			printk(KERN_INFO "%s: Ringspeed autosense mode on\n",
			       dev->name);
	} else if (streamer_priv->streamer_ring_speed == 16) {
		if (streamer_priv->streamer_message_level)
			printk(KERN_INFO "%s: Trying to open at 16 Mbps as requested\n",
			       dev->name);
		writew(GPR_16MBPS, streamer_mmio + GPR);
	} else if (streamer_priv->streamer_ring_speed == 4) {
		if (streamer_priv->streamer_message_level)
			printk(KERN_INFO "%s: Trying to open at 4 Mbps as requested\n",
			       dev->name);
		writew(0, streamer_mmio + GPR);
	}

	skb = dev_alloc_skb(streamer_priv->pkt_buf_sz);
	if (!skb) {
		printk(KERN_INFO "%s: skb allocation for diagnostics failed...proceeding\n",
		       dev->name);
	} else {
	        struct streamer_rx_desc *rx_ring;
                u8 *data;

		rx_ring=(struct streamer_rx_desc *)skb->data;
		data=((u8 *)skb->data)+sizeof(struct streamer_rx_desc);
		rx_ring->forward=0;
		rx_ring->status=0;
		rx_ring->buffer=virt_to_bus(data);
		rx_ring->framelen_buflen=512; 
		writel(virt_to_bus(rx_ring),streamer_mmio+RXBDA);
	}

#if STREAMER_DEBUG
	printk("GPR = %x\n", readw(streamer_mmio + GPR));
#endif
	/* start solo init */
	writew(SISR_MI, streamer_mmio + SISR_MASK_SUM);

	while (!((readw(streamer_mmio + SISR)) & SISR_SRB_REPLY)) {
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ/10);
		if (jiffies - t > 40 * HZ) {
			printk(KERN_ERR
			       "IBM PCI tokenring card not responding\n");
			release_region(dev->base_addr, STREAMER_IO_SPACE);
			return -1;
		}
	}
	writew(~SISR_SRB_REPLY, streamer_mmio + SISR_RUM);
	misr = readw(streamer_mmio + MISR_RUM);
	writew(~misr, streamer_mmio + MISR_RUM);

	if (skb)
		dev_kfree_skb(skb);	/* release skb used for diagnostics */

#if STREAMER_DEBUG
	printk("LAPWWO: %x, LAPA: %x LAPE:  %x\n",
	       readw(streamer_mmio + LAPWWO), readw(streamer_mmio + LAPA),
	       readw(streamer_mmio + LAPE));
#endif

#if STREAMER_DEBUG
	{
		int i;
		writew(readw(streamer_mmio + LAPWWO),
		       streamer_mmio + LAPA);
		printk("initialization response srb dump: ");
		for (i = 0; i < 10; i++)
			printk("%x:",
			       ntohs(readw(streamer_mmio + LAPDINC)));
		printk("\n");
	}
#endif

	writew(readw(streamer_mmio + LAPWWO) + 6, streamer_mmio + LAPA);
	if (readw(streamer_mmio + LAPD)) {
		printk(KERN_INFO "tokenring card intialization failed. errorcode : %x\n",
		       ntohs(readw(streamer_mmio + LAPD)));
		release_region(dev->base_addr, STREAMER_IO_SPACE);
		return -1;
	}

	writew(readw(streamer_mmio + LAPWWO) + 8, streamer_mmio + LAPA);
	uaa_addr = ntohs(readw(streamer_mmio + LAPDINC));
	readw(streamer_mmio + LAPDINC);	/* skip over Level.Addr field */
	streamer_priv->streamer_addr_table_addr = ntohs(readw(streamer_mmio + LAPDINC));
	streamer_priv->streamer_parms_addr = ntohs(readw(streamer_mmio + LAPDINC));

#if STREAMER_DEBUG
	printk("UAA resides at %x\n", uaa_addr);
#endif

	/* setup uaa area for access with LAPD */
	{
		int i;
		__u16 addr;
		writew(uaa_addr, streamer_mmio + LAPA);
		for (i = 0; i < 6; i += 2) {
		        addr=ntohs(readw(streamer_mmio+LAPDINC));
			dev->dev_addr[i]= (addr >> 8) & 0xff;
			dev->dev_addr[i+1]= addr & 0xff;
		}
#if STREAMER_DEBUG
		printk("Adapter address: ");
		for (i = 0; i < 6; i++) {
			printk("%02x:", dev->dev_addr[i]);
		}
		printk("\n");
#endif
	}
	return 0;
}

static int __init streamer_init(struct net_device *dev) 
{
        struct streamer_private *streamer_priv;
	__u8 *streamer_mmio;
	int rc;
  
	streamer_priv=(struct streamer_private *)dev->priv;
	streamer_mmio=streamer_priv->streamer_mmio;

	spin_lock_init(&streamer_priv->streamer_lock);
  
	printk("%s \n", version);
	printk("%s: %s. I/O at %hx, MMIO at %p, using irq %d\n",dev->name,
	       streamer_priv->streamer_card_name,
	       (unsigned int) dev->base_addr,
	       streamer_priv->streamer_mmio, 
	       dev->irq);

	request_region(dev->base_addr, STREAMER_IO_SPACE, "streamer");

	rc=streamer_reset(dev);
	return rc;
}
  


static int streamer_open(struct net_device *dev)
{
	struct streamer_private *streamer_priv = (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	unsigned long flags;
	char open_error[255];
	int i, open_finished = 1;
	__u16 srb_word;
	__u16 srb_open;
	int rc;

	if (readw(streamer_mmio+BMCTL_SUM) & BMCTL_RX_ENABLED) {
	        rc=streamer_reset(dev);
	}

	if (request_irq(dev->irq, &streamer_interrupt, SA_SHIRQ, "streamer", dev)) {
		return -EAGAIN;
	}
#if STREAMER_DEBUG
	printk("BMCTL: %x\n", readw(streamer_mmio + BMCTL_SUM));
	printk("pending ints: %x\n", readw(streamer_mmio + SISR));
#endif

	writew(SISR_MI | SISR_SRB_REPLY, streamer_mmio + SISR_MASK);	/* more ints later, doesn't stop arb cmd interrupt */
	writew(LISR_LIE, streamer_mmio + LISR);	/* more ints later */

	/* adapter is closed, so SRB is pointed to by LAPWWO */
	writew(readw(streamer_mmio + LAPWWO), streamer_mmio + LAPA);

#if STREAMER_DEBUG
	printk("LAPWWO: %x, LAPA: %x\n", readw(streamer_mmio + LAPWWO),
	       readw(streamer_mmio + LAPA));
	printk("LAPE: %x\n", readw(streamer_mmio + LAPE));
	printk("SISR Mask = %04x\n", readw(streamer_mmio + SISR_MASK));
#endif
	do {
		int i;

		save_flags(flags);
		cli();
		for (i = 0; i < SRB_COMMAND_SIZE; i += 2) {
			writew(0, streamer_mmio + LAPDINC);
		}

		writew(readw(streamer_mmio+LAPWWO),streamer_mmio+LAPA);
		writew(htons(SRB_OPEN_ADAPTER<<8),streamer_mmio+LAPDINC) ; 	/* open */
		writew(htons(STREAMER_CLEAR_RET_CODE<<8),streamer_mmio+LAPDINC);
		writew(STREAMER_CLEAR_RET_CODE, streamer_mmio + LAPDINC);

		writew(readw(streamer_mmio + LAPWWO) + 8, streamer_mmio + LAPA);
#if STREAMER_NETWORK_MONITOR
		/* If Network Monitor, instruct card to copy MAC frames through the ARB */
		writew(htons(OPEN_ADAPTER_ENABLE_FDX | OPEN_ADAPTER_PASS_ADC_MAC | OPEN_ADAPTER_PASS_ATT_MAC | OPEN_ADAPTER_PASS_BEACON), streamer_mmio + LAPDINC);	/* offset 8 word contains open options */
#else
		writew(htons(OPEN_ADAPTER_ENABLE_FDX), streamer_mmio + LAPDINC);	/* Offset 8 word contains Open.Options */
#endif

		if (streamer_priv->streamer_laa[0]) {
			writew(readw(streamer_mmio + LAPWWO) + 12, streamer_mmio + LAPA);
			writew(htons((streamer_priv->streamer_laa[0] << 8) | 
				     streamer_priv->streamer_laa[1]),streamer_mmio+LAPDINC);
			writew(htons((streamer_priv->streamer_laa[2] << 8) | 
				     streamer_priv->streamer_laa[3]),streamer_mmio+LAPDINC);
			writew(htons((streamer_priv->streamer_laa[4] << 8) | 
				     streamer_priv->streamer_laa[5]),streamer_mmio+LAPDINC);
			memcpy(dev->dev_addr, streamer_priv->streamer_laa, dev->addr_len);
		}

		/* save off srb open offset */
		srb_open = readw(streamer_mmio + LAPWWO);
#if STREAMER_DEBUG
		writew(readw(streamer_mmio + LAPWWO),
		       streamer_mmio + LAPA);
		printk("srb open request: \n");
		for (i = 0; i < 16; i++) {
			printk("%x:", ntohs(readw(streamer_mmio + LAPDINC)));
		}
		printk("\n");
#endif

		streamer_priv->srb_queued = 1;

		/* signal solo that SRB command has been issued */
		writew(LISR_SRB_CMD, streamer_mmio + LISR_SUM);

		while (streamer_priv->srb_queued) {
			interruptible_sleep_on_timeout(&streamer_priv->srb_wait, 5 * HZ);
			if (signal_pending(current)) {
				printk(KERN_WARNING "%s: SRB timed out.\n", dev->name);
				printk(KERN_WARNING "SISR=%x MISR=%x, LISR=%x\n",
				       readw(streamer_mmio + SISR),
				       readw(streamer_mmio + MISR_RUM),
				       readw(streamer_mmio + LISR));
				streamer_priv->srb_queued = 0;
				break;
			}
		}
		restore_flags(flags);

#if STREAMER_DEBUG
		printk("SISR_MASK: %x\n", readw(streamer_mmio + SISR_MASK));
		printk("srb open response:\n");
		writew(srb_open, streamer_mmio + LAPA);
		for (i = 0; i < 10; i++) {
			printk("%x:",
			       ntohs(readw(streamer_mmio + LAPDINC)));
		}
#endif

		/* If we get the same return response as we set, the interrupt wasn't raised and the open
		 * timed out.
		 */
		writew(srb_open + 2, streamer_mmio + LAPA);
		srb_word = ntohs(readw(streamer_mmio + LAPD)) & 0xFF;
		if (srb_word == STREAMER_CLEAR_RET_CODE) {
			printk(KERN_WARNING "%s: Adapter Open time out or error.\n",
			       dev->name);
			return -EIO;
		}

		if (srb_word != 0) {
			if (srb_word == 0x07) {
				if (!streamer_priv->streamer_ring_speed && open_finished) {	/* Autosense , first time around */
					printk(KERN_WARNING "%s: Retrying at different ring speed \n",
					       dev->name);
					open_finished = 0;
				} else {
					__u16 error_code;

					writew(srb_open + 6, streamer_mmio + LAPA);
					error_code = ntohs(readw(streamer_mmio + LAPD));
					strcpy(open_error, open_maj_error[(error_code & 0xf0) >> 4]);
					strcat(open_error, " - ");
					strcat(open_error, open_min_error[(error_code & 0x0f)]);

					if (!streamer_priv->streamer_ring_speed
					    && ((error_code & 0x0f) == 0x0d)) 
					{
						printk(KERN_WARNING "%s: Tried to autosense ring speed with no monitors present\n", dev->name);
						printk(KERN_WARNING "%s: Please try again with a specified ring speed \n", dev->name);
						free_irq(dev->irq, dev);
						return -EIO;
					}

					printk(KERN_WARNING "%s: %s\n",
					       dev->name, open_error);
					free_irq(dev->irq, dev);
					return -EIO;

				}	/* if autosense && open_finished */
			} else {
				printk(KERN_WARNING "%s: Bad OPEN response: %x\n",
				       dev->name, srb_word);
				free_irq(dev->irq, dev);
				return -EIO;
			}
		} else
			open_finished = 1;
	} while (!(open_finished));	/* Will only loop if ring speed mismatch re-open attempted && autosense is on */

	writew(srb_open + 18, streamer_mmio + LAPA);
	srb_word=ntohs(readw(streamer_mmio+LAPD)) >> 8;
	if (srb_word & (1 << 3))
		if (streamer_priv->streamer_message_level)
			printk(KERN_INFO "%s: Opened in FDX Mode\n", dev->name);

	if (srb_word & 1)
		streamer_priv->streamer_ring_speed = 16;
	else
		streamer_priv->streamer_ring_speed = 4;

	if (streamer_priv->streamer_message_level)
		printk(KERN_INFO "%s: Opened in %d Mbps mode\n", 
			dev->name,
			streamer_priv->streamer_ring_speed);

	writew(srb_open + 8, streamer_mmio + LAPA);
	streamer_priv->asb = ntohs(readw(streamer_mmio + LAPDINC));
	streamer_priv->srb = ntohs(readw(streamer_mmio + LAPDINC));
	streamer_priv->arb = ntohs(readw(streamer_mmio + LAPDINC));
	readw(streamer_mmio + LAPDINC);	/* offset 14 word is rsvd */
	streamer_priv->trb = ntohs(readw(streamer_mmio + LAPDINC));

	streamer_priv->streamer_receive_options = 0x00;
	streamer_priv->streamer_copy_all_options = 0;

	/* setup rx ring */
	/* enable rx channel */
	writew(~BMCTL_RX_DIS, streamer_mmio + BMCTL_RUM);

	/* setup rx descriptors */
	streamer_priv->streamer_rx_ring=
	    kmalloc( sizeof(struct streamer_rx_desc)*
		     STREAMER_RX_RING_SIZE,GFP_KERNEL);
	if (!streamer_priv->streamer_rx_ring) {
	    printk(KERN_WARNING "%s ALLOC of streamer rx ring FAILED!!\n",dev->name);
	    return -EIO;
	}

	for (i = 0; i < STREAMER_RX_RING_SIZE; i++) {
		struct sk_buff *skb;

		skb = dev_alloc_skb(streamer_priv->pkt_buf_sz);
		if (skb == NULL)
			break;

		skb->dev = dev;

		streamer_priv->streamer_rx_ring[i].forward = virt_to_bus(&streamer_priv->streamer_rx_ring[i + 1]);
		streamer_priv->streamer_rx_ring[i].status = 0;
		streamer_priv->streamer_rx_ring[i].buffer = virt_to_bus(skb->data);
		streamer_priv->streamer_rx_ring[i].framelen_buflen = streamer_priv->pkt_buf_sz;
		streamer_priv->rx_ring_skb[i] = skb;
	}
	streamer_priv->streamer_rx_ring[STREAMER_RX_RING_SIZE - 1].forward =
				virt_to_bus(&streamer_priv->streamer_rx_ring[0]);

	if (i == 0) {
		printk(KERN_WARNING "%s: Not enough memory to allocate rx buffers. Adapter disabled\n", dev->name);
		free_irq(dev->irq, dev);
		return -EIO;
	}

	streamer_priv->rx_ring_last_received = STREAMER_RX_RING_SIZE - 1;	/* last processed rx status */

	writel(virt_to_bus(&streamer_priv->streamer_rx_ring[0]), streamer_mmio + RXBDA);
	writel(virt_to_bus(&streamer_priv->streamer_rx_ring[STREAMER_RX_RING_SIZE - 1]), streamer_mmio + RXLBDA);

	/* set bus master interrupt event mask */
	writew(MISR_RX_NOBUF | MISR_RX_EOF, streamer_mmio + MISR_MASK);


	/* setup tx ring */
	streamer_priv->streamer_tx_ring=kmalloc(sizeof(struct streamer_tx_desc)*
						STREAMER_TX_RING_SIZE,GFP_KERNEL);
	if (!streamer_priv->streamer_tx_ring) {
	    printk(KERN_WARNING "%s ALLOC of streamer_tx_ring FAILED\n",dev->name);
	    return -EIO;
	}

	writew(~BMCTL_TX2_DIS, streamer_mmio + BMCTL_RUM);	/* Enables TX channel 2 */
	for (i = 0; i < STREAMER_TX_RING_SIZE; i++) {
		streamer_priv->streamer_tx_ring[i].forward = virt_to_bus(&streamer_priv->streamer_tx_ring[i + 1]);
		streamer_priv->streamer_tx_ring[i].status = 0;
		streamer_priv->streamer_tx_ring[i].bufcnt_framelen = 0;
		streamer_priv->streamer_tx_ring[i].buffer = 0;
		streamer_priv->streamer_tx_ring[i].buflen = 0;
	}
	streamer_priv->streamer_tx_ring[STREAMER_TX_RING_SIZE - 1].forward =
					virt_to_bus(&streamer_priv->streamer_tx_ring[0]);;

	streamer_priv->free_tx_ring_entries = STREAMER_TX_RING_SIZE;
	streamer_priv->tx_ring_free = 0;	/* next entry in tx ring to use */
	streamer_priv->tx_ring_last_status = STREAMER_TX_RING_SIZE - 1;

	/* set Busmaster interrupt event mask (handle receives on interrupt only */
	writew(MISR_TX2_EOF | MISR_RX_NOBUF | MISR_RX_EOF, streamer_mmio + MISR_MASK);
	/* set system event interrupt mask */
	writew(SISR_ADAPTER_CHECK | SISR_ARB_CMD | SISR_TRB_REPLY | SISR_ASB_FREE, streamer_mmio + SISR_MASK_SUM);

#if STREAMER_DEBUG
	printk("BMCTL: %x\n", readw(streamer_mmio + BMCTL_SUM));
	printk("SISR MASK: %x\n", readw(streamer_mmio + SISR_MASK));
#endif

#if STREAMER_NETWORK_MONITOR

	writew(streamer_priv->streamer_addr_table_addr, streamer_mmio + LAPA);
	printk("%s: Node Address: %04x:%04x:%04x\n", dev->name,
		ntohs(readw(streamer_mmio + LAPDINC)),
		ntohs(readw(streamer_mmio + LAPDINC)),
		ntohs(readw(streamer_mmio + LAPDINC)));
	readw(streamer_mmio + LAPDINC);
	readw(streamer_mmio + LAPDINC);
	printk("%s: Functional Address: %04x:%04x\n", dev->name,
		ntohs(readw(streamer_mmio + LAPDINC)),
		ntohs(readw(streamer_mmio + LAPDINC)));

	writew(streamer_priv->streamer_parms_addr + 4,
		streamer_mmio + LAPA);
	printk("%s: NAUN Address: %04x:%04x:%04x\n", dev->name,
		ntohs(readw(streamer_mmio + LAPDINC)),
		ntohs(readw(streamer_mmio + LAPDINC)),
		ntohs(readw(streamer_mmio + LAPDINC)));
#endif

	netif_start_queue(dev);
	MOD_INC_USE_COUNT;
	return 0;
}

/*
 *	When we enter the rx routine we do not know how many frames have been 
 *	queued on the rx channel.  Therefore we start at the next rx status
 *	position and travel around the receive ring until we have completed
 *	all the frames.
 *
 *	This means that we may process the frame before we receive the end
 *	of frame interrupt. This is why we always test the status instead
 *	of blindly processing the next frame.
 *	
 */
static void streamer_rx(struct net_device *dev)
{
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	struct streamer_rx_desc *rx_desc;
	int rx_ring_last_received, length, frame_length, buffer_cnt = 0;
	struct sk_buff *skb, *skb2;

	/* setup the next rx descriptor to be received */
	rx_desc = &streamer_priv->streamer_rx_ring[(streamer_priv->rx_ring_last_received + 1) & (STREAMER_RX_RING_SIZE - 1)];
	rx_ring_last_received = streamer_priv->rx_ring_last_received;

	while (rx_desc->status & 0x01000000) {	/* While processed descriptors are available */
		if (rx_ring_last_received != streamer_priv->rx_ring_last_received) 
		{
			printk(KERN_WARNING "RX Error 1 rx_ring_last_received not the same %x %x\n",
				rx_ring_last_received, streamer_priv->rx_ring_last_received);
		}
		streamer_priv->rx_ring_last_received = (streamer_priv->rx_ring_last_received + 1) & (STREAMER_RX_RING_SIZE - 1);
		rx_ring_last_received = streamer_priv->rx_ring_last_received;

		length = rx_desc->framelen_buflen & 0xffff;	/* buffer length */
		frame_length = (rx_desc->framelen_buflen >> 16) & 0xffff;

		if (rx_desc->status & 0x7E830000) {	/* errors */
			if (streamer_priv->streamer_message_level) {
				printk(KERN_WARNING "%s: Rx Error %x \n",
				       dev->name, rx_desc->status);
			}
		} else {	/* received without errors */
			if (rx_desc->status & 0x80000000) {	/* frame complete */
				buffer_cnt = 1;
				skb = dev_alloc_skb(streamer_priv->pkt_buf_sz);
			} else {
				skb = dev_alloc_skb(frame_length);
			}

			if (skb == NULL) 
			{
				printk(KERN_WARNING "%s: Not enough memory to copy packet to upper layers. \n",	dev->name);
				streamer_priv->streamer_stats.rx_dropped++;
			} else {	/* we allocated an skb OK */
				skb->dev = dev;

				if (buffer_cnt == 1) {
					skb2 = streamer_priv->rx_ring_skb[rx_ring_last_received];
#if STREAMER_DEBUG_PACKETS
					{
						int i;
						printk("streamer_rx packet print: skb->data2 %p  skb->head %p\n", skb2->data, skb2->head);
						for (i = 0; i < frame_length; i++) 
						{
							printk("%x:", skb2->data[i]);
							if (((i + 1) % 16) == 0)
								printk("\n");
						}
						printk("\n");
					}
#endif
					skb_put(skb2, length);
					skb2->protocol = tr_type_trans(skb2, dev);
					/* recycle this descriptor */
					streamer_priv->streamer_rx_ring[rx_ring_last_received].status = 0;
					streamer_priv->streamer_rx_ring[rx_ring_last_received].framelen_buflen = streamer_priv->pkt_buf_sz;
					streamer_priv->streamer_rx_ring[rx_ring_last_received].buffer = virt_to_bus(skb->data);
					streamer_priv-> rx_ring_skb[rx_ring_last_received] = skb;
					/* place recycled descriptor back on the adapter */
					writel(virt_to_bus(&streamer_priv->streamer_rx_ring[rx_ring_last_received]),streamer_mmio + RXLBDA);
					/* pass the received skb up to the protocol */
					netif_rx(skb2);
				} else {
					do {	/* Walk the buffers */
						memcpy(skb_put(skb, length),bus_to_virt(rx_desc->buffer), length);	/* copy this fragment */
						streamer_priv->streamer_rx_ring[rx_ring_last_received].status = 0;
						streamer_priv->streamer_rx_ring[rx_ring_last_received].framelen_buflen = streamer_priv->pkt_buf_sz;
						
						/* give descriptor back to the adapter */
						writel(virt_to_bus(&streamer_priv->streamer_rx_ring[rx_ring_last_received]), streamer_mmio + RXLBDA);

						if (rx_desc->status & 0x80000000)
							break;	/* this descriptor completes the frame */

						/* else get the next pending descriptor */
						if (rx_ring_last_received!= streamer_priv->rx_ring_last_received)
						{
							printk("RX Error rx_ring_last_received not the same %x %x\n",
								rx_ring_last_received,
								streamer_priv->rx_ring_last_received);
						}
						rx_desc = &streamer_priv->streamer_rx_ring[(streamer_priv->rx_ring_last_received+1) & (STREAMER_RX_RING_SIZE-1)];

						length = rx_desc->framelen_buflen & 0xffff;	/* buffer length */
						streamer_priv->rx_ring_last_received =	(streamer_priv->rx_ring_last_received+1) & (STREAMER_RX_RING_SIZE - 1);
						rx_ring_last_received = streamer_priv->rx_ring_last_received;
					} while (1);

					skb->protocol = tr_type_trans(skb, dev);
					/* send up to the protocol */
					netif_rx(skb);
				}
				streamer_priv->streamer_stats.rx_packets++;
				streamer_priv->streamer_stats.rx_bytes += length;
			}	/* if skb == null */
		}		/* end received without errors */

		/* try the next one */
		rx_desc = &streamer_priv->streamer_rx_ring[(rx_ring_last_received + 1) & (STREAMER_RX_RING_SIZE - 1)];
	}			/* end for all completed rx descriptors */
}

static void streamer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct net_device *dev = (struct net_device *) dev_id;
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	__u16 sisr;
	__u16 misr;
	__u16 sisrmask;

	sisrmask = SISR_MI;
	writew(~sisrmask, streamer_mmio + SISR_MASK_RUM);
	sisr = readw(streamer_mmio + SISR);
	writew(~sisr, streamer_mmio + SISR_RUM);
	misr = readw(streamer_mmio + MISR_RUM);
	writew(~misr, streamer_mmio + MISR_RUM);

	if (!sisr) 
	{		/* Interrupt isn't for us */
  	        writew(~misr,streamer_mmio+MISR_RUM);
		return;
	}

	spin_lock(&streamer_priv->streamer_lock);

	if ((sisr & (SISR_SRB_REPLY | SISR_ADAPTER_CHECK | SISR_ASB_FREE | SISR_ARB_CMD | SISR_TRB_REPLY))
	    || (misr & (MISR_TX2_EOF | MISR_RX_NOBUF | MISR_RX_EOF))) {
		if (sisr & SISR_SRB_REPLY) {
			if (streamer_priv->srb_queued == 1) {
				wake_up_interruptible(&streamer_priv->srb_wait);
			} else if (streamer_priv->srb_queued == 2) {
				streamer_srb_bh(dev);
			}
			streamer_priv->srb_queued = 0;
		}
		/* SISR_SRB_REPLY */
		if (misr & MISR_TX2_EOF) {
			while (streamer_priv->streamer_tx_ring[(streamer_priv->tx_ring_last_status + 1) & (STREAMER_TX_RING_SIZE - 1)].status) 
			{
				streamer_priv->tx_ring_last_status = (streamer_priv->tx_ring_last_status + 1) & (STREAMER_TX_RING_SIZE - 1);
				streamer_priv->free_tx_ring_entries++;
				streamer_priv->streamer_stats.tx_bytes += streamer_priv->tx_ring_skb[streamer_priv->tx_ring_last_status]->len;
				streamer_priv->streamer_stats.tx_packets++;
				dev_kfree_skb_irq(streamer_priv->tx_ring_skb[streamer_priv->tx_ring_last_status]);
				streamer_priv-> streamer_tx_ring[streamer_priv->tx_ring_last_status].buffer = 0xdeadbeef;
				streamer_priv->streamer_tx_ring[streamer_priv->tx_ring_last_status].status = 0;
				streamer_priv->streamer_tx_ring[streamer_priv->tx_ring_last_status].bufcnt_framelen = 0;
				streamer_priv->streamer_tx_ring[streamer_priv->tx_ring_last_status].buflen = 0;
			}
			netif_wake_queue(dev);
		}

		if (misr & MISR_RX_EOF) {
			streamer_rx(dev);
		}
		/* MISR_RX_EOF */
		if (sisr & SISR_ADAPTER_CHECK) {
			printk(KERN_WARNING "%s: Adapter Check Interrupt Raised, 8 bytes of information follow:\n", dev->name);
			writel(readl(streamer_mmio + LAPWWO), streamer_mmio + LAPA);
			printk(KERN_WARNING "%s: Words %x:%x:%x:%x:\n",
			       dev->name, readw(streamer_mmio + LAPDINC),
			       ntohs(readw(streamer_mmio + LAPDINC)),
			       ntohs(readw(streamer_mmio + LAPDINC)),
			       ntohs(readw(streamer_mmio + LAPDINC)));
			free_irq(dev->irq, dev);
		}

		/* SISR_ADAPTER_CHECK */
		if (sisr & SISR_ASB_FREE) {
			/* Wake up anything that is waiting for the asb response */
			if (streamer_priv->asb_queued) {
				streamer_asb_bh(dev);
			}
		}
		/* SISR_ASB_FREE */
		if (sisr & SISR_ARB_CMD) {
			streamer_arb_cmd(dev);
		}
		/* SISR_ARB_CMD */
		if (sisr & SISR_TRB_REPLY) {
			/* Wake up anything that is waiting for the trb response */
			if (streamer_priv->trb_queued) {
				wake_up_interruptible(&streamer_priv->
						      trb_wait);
			}
			streamer_priv->trb_queued = 0;
		}
		/* SISR_TRB_REPLY */
		if (misr & MISR_RX_NOBUF) {
			/* According to the documentation, we don't have to do anything, but trapping it keeps it out of
			   /var/log/messages.  */
		}		/* SISR_RX_NOBUF */
	} else {
		printk(KERN_WARNING "%s: Unexpected interrupt: %x\n",
		       dev->name, sisr);
		printk(KERN_WARNING "%s: SISR_MASK: %x\n", dev->name,
		       readw(streamer_mmio + SISR_MASK));
	}			/* One if the interrupts we want */

	writew(SISR_MI, streamer_mmio + SISR_MASK_SUM);
	spin_unlock(&streamer_priv->streamer_lock) ; 
}

static int streamer_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	unsigned long flags ;

	spin_lock_irqsave(&streamer_priv->streamer_lock, flags);
	netif_stop_queue(dev);

	if (streamer_priv->free_tx_ring_entries) {
		streamer_priv->streamer_tx_ring[streamer_priv->tx_ring_free].status = 0;
		streamer_priv->streamer_tx_ring[streamer_priv->tx_ring_free].bufcnt_framelen = 0x00010000 | skb->len;
		streamer_priv->streamer_tx_ring[streamer_priv->tx_ring_free].buffer = virt_to_bus(skb->data);
		streamer_priv->streamer_tx_ring[streamer_priv->tx_ring_free].buflen = skb->len;
		streamer_priv->tx_ring_skb[streamer_priv->tx_ring_free] = skb;
		streamer_priv->free_tx_ring_entries--;
#if STREAMER_DEBUG_PACKETS
		{
			int i;
			printk("streamer_xmit packet print:\n");
			for (i = 0; i < skb->len; i++) {
				printk("%x:", skb->data[i]);
				if (((i + 1) % 16) == 0)
					printk("\n");
			}
			printk("\n");
		}
#endif

		writel(virt_to_bus (&streamer_priv->streamer_tx_ring[streamer_priv->tx_ring_free]),streamer_mmio + TX2LFDA);

		streamer_priv->tx_ring_free = (streamer_priv->tx_ring_free + 1) & (STREAMER_TX_RING_SIZE - 1);
		netif_wake_queue(dev);
		spin_unlock_irqrestore(&streamer_priv->streamer_lock,flags);
		return 0;
	} else {
	        spin_unlock_irqrestore(&streamer_priv->streamer_lock,flags);
		return 1;
	}
}


static int streamer_close(struct net_device *dev)
{
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	unsigned long flags;
	int i;

	netif_stop_queue(dev);
	writew(streamer_priv->srb, streamer_mmio + LAPA);
	writew(htons(SRB_CLOSE_ADAPTER << 8),streamer_mmio+LAPDINC);
	writew(htons(STREAMER_CLEAR_RET_CODE << 8), streamer_mmio+LAPDINC);

	save_flags(flags);
	cli();

	streamer_priv->srb_queued = 1;
	writew(LISR_SRB_CMD, streamer_mmio + LISR_SUM);

	while (streamer_priv->srb_queued) 
	{
		interruptible_sleep_on_timeout(&streamer_priv->srb_wait,
					       jiffies + 60 * HZ);
		if (signal_pending(current)) 
		{
			printk(KERN_WARNING "%s: SRB timed out.\n", dev->name);
			printk(KERN_WARNING "SISR=%x MISR=%x LISR=%x\n",
			       readw(streamer_mmio + SISR),
			       readw(streamer_mmio + MISR_RUM),
			       readw(streamer_mmio + LISR));
			streamer_priv->srb_queued = 0;
			break;
		}
	}

	restore_flags(flags);
	streamer_priv->rx_ring_last_received = (streamer_priv->rx_ring_last_received + 1) & (STREAMER_RX_RING_SIZE - 1);

	for (i = 0; i < STREAMER_RX_RING_SIZE; i++) {
	        if (streamer_priv->rx_ring_skb[streamer_priv->rx_ring_last_received]) {
		        dev_kfree_skb(streamer_priv->rx_ring_skb[streamer_priv->rx_ring_last_received]);
		} 
		streamer_priv->rx_ring_last_received = (streamer_priv->rx_ring_last_received + 1) & (STREAMER_RX_RING_SIZE - 1);
	}

	/* reset tx/rx fifo's and busmaster logic */

	/* TBD. Add graceful way to reset the LLC channel without doing a soft reset. 
	   writel(readl(streamer_mmio+BCTL)|(3<<13),streamer_mmio+BCTL);
	   udelay(1);
	   writel(readl(streamer_mmio+BCTL)&~(3<<13),streamer_mmio+BCTL);
	 */

#if STREAMER_DEBUG
	writew(streamer_priv->srb, streamer_mmio + LAPA);
	printk("srb): ");
	for (i = 0; i < 2; i++) {
		printk("%x ", ntohs(readw(streamer_mmio + LAPDINC)));
	}
	printk("\n");
#endif
	free_irq(dev->irq, dev);

	MOD_DEC_USE_COUNT;
	return 0;
}

static void streamer_set_rx_mode(struct net_device *dev)
{
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	__u8 options = 0;
	struct dev_mc_list *dmi;
	unsigned char dev_mc_address[5];
	int i;

	writel(streamer_priv->srb, streamer_mmio + LAPA);
	options = streamer_priv->streamer_copy_all_options;

	if (dev->flags & IFF_PROMISC)
		options |= (3 << 5);	/* All LLC and MAC frames, all through the main rx channel */
	else
		options &= ~(3 << 5);

	/* Only issue the srb if there is a change in options */

	if ((options ^ streamer_priv->streamer_copy_all_options)) 
	{
		/* Now to issue the srb command to alter the copy.all.options */
		writew(htons(SRB_MODIFY_RECEIVE_OPTIONS << 8), streamer_mmio+LAPDINC);
		writew(htons(STREAMER_CLEAR_RET_CODE << 8), streamer_mmio+LAPDINC);
		writew(htons((streamer_priv->streamer_receive_options << 8) | options),streamer_mmio+LAPDINC);
		writew(htons(0x4a41),streamer_mmio+LAPDINC);
		writew(htons(0x4d45),streamer_mmio+LAPDINC);
		writew(htons(0x5320),streamer_mmio+LAPDINC);
		writew(0x2020, streamer_mmio + LAPDINC);

		streamer_priv->srb_queued = 2;	/* Can't sleep, use srb_bh */

		writel(LISR_SRB_CMD, streamer_mmio + LISR_SUM);

		streamer_priv->streamer_copy_all_options = options;
		return;
	}

	/* Set the functional addresses we need for multicast */
	writel(streamer_priv->srb,streamer_mmio+LAPA);
	dev_mc_address[0] = dev_mc_address[1] = dev_mc_address[2] = dev_mc_address[3] = 0 ; 
  
	for (i=0,dmi=dev->mc_list;i < dev->mc_count; i++,dmi = dmi->next) 
	{ 
   	        dev_mc_address[0] |= dmi->dmi_addr[2] ; 
		dev_mc_address[1] |= dmi->dmi_addr[3] ; 
		dev_mc_address[2] |= dmi->dmi_addr[4] ; 
		dev_mc_address[3] |= dmi->dmi_addr[5] ; 
	}
  
	writew(htons(SRB_SET_FUNC_ADDRESS << 8),streamer_mmio+LAPDINC);
	writew(htons(STREAMER_CLEAR_RET_CODE << 8), streamer_mmio+LAPDINC);
	writew(0,streamer_mmio+LAPDINC);
	writew(htons( (dev_mc_address[0] << 8) | dev_mc_address[1]),streamer_mmio+LAPDINC);
	writew(htons( (dev_mc_address[2] << 8) | dev_mc_address[3]),streamer_mmio+LAPDINC);
	streamer_priv->srb_queued = 2 ; 
	writel(LISR_SRB_CMD,streamer_mmio+LISR_SUM);
}

static void streamer_srb_bh(struct net_device *dev)
{
	struct streamer_private *streamer_priv = (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	__u16 srb_word;

	writew(streamer_priv->srb, streamer_mmio + LAPA);
	srb_word=ntohs(readw(streamer_mmio+LAPDINC)) >> 8;

	switch (srb_word) {

		/* SRB_MODIFY_RECEIVE_OPTIONS i.e. set_multicast_list options (promiscuous) 
		 * At some point we should do something if we get an error, such as
		 * resetting the IFF_PROMISC flag in dev
		 */

	case SRB_MODIFY_RECEIVE_OPTIONS:
	        srb_word=ntohs(readw(streamer_mmio+LAPDINC)) >> 8;

		switch (srb_word) {
		case 0x01:
			printk(KERN_WARNING "%s: Unrecognized srb command\n", dev->name);
			break;
		case 0x04:
			printk(KERN_WARNING "%s: Adapter must be open for this operation, doh!!\n", dev->name);
			break;
		default:
			if (streamer_priv->streamer_message_level)
				printk(KERN_WARNING "%s: Receive Options Modified to %x,%x\n",
				       dev->name,
				       streamer_priv->streamer_copy_all_options,
				       streamer_priv->streamer_receive_options);
			break;
		}		/* switch srb[2] */
		break;


		/* SRB_SET_GROUP_ADDRESS - Multicast group setting 
		 */
	case SRB_SET_GROUP_ADDRESS:
	        srb_word=ntohs(readw(streamer_mmio+LAPDINC)) >> 8;
		switch (srb_word) {
		case 0x00:
		        break;
		case 0x01:
			printk(KERN_WARNING "%s: Unrecognized srb command \n",dev->name);
			break;
		case 0x04:
			printk(KERN_WARNING "%s: Adapter must be open for this operation, doh!!\n", dev->name);
			break;
		case 0x3c:
			printk(KERN_WARNING "%s: Group/Functional address indicator bits not set correctly\n", dev->name);
			break;
		case 0x3e:	/* If we ever implement individual multicast addresses, will need to deal with this */
			printk(KERN_WARNING "%s: Group address registers full\n", dev->name);
			break;
		case 0x55:
			printk(KERN_INFO "%s: Group Address already set.\n", dev->name);
			break;
		default:
			break;
		}		/* switch srb[2] */
		break;


		/* SRB_RESET_GROUP_ADDRESS - Remove a multicast address from group list
		 */
	case SRB_RESET_GROUP_ADDRESS:
	        srb_word=ntohs(readw(streamer_mmio+LAPDINC)) >> 8;
		switch (srb_word) {
		case 0x00:
		        break;
		case 0x01:
			printk(KERN_WARNING "%s: Unrecognized srb command \n", dev->name);
			break;
		case 0x04:
			printk(KERN_WARNING "%s: Adapter must be open for this operation, doh!!\n", dev->name);
			break;
		case 0x39:	/* Must deal with this if individual multicast addresses used */
			printk(KERN_INFO "%s: Group address not found \n", dev->name);
			break;
		default:
			break;
		}		/* switch srb[2] */
		break;


		/* SRB_SET_FUNC_ADDRESS - Called by the set_rx_mode 
		 */

	case SRB_SET_FUNC_ADDRESS:
	        srb_word=ntohs(readw(streamer_mmio+LAPDINC)) >> 8;
		switch (srb_word) {
		case 0x00:
			if (streamer_priv->streamer_message_level)
				printk(KERN_INFO "%s: Functional Address Mask Set \n", dev->name);
			break;
		case 0x01:
			printk(KERN_WARNING "%s: Unrecognized srb command \n", dev->name);
			break;
		case 0x04:
			printk(KERN_WARNING "%s: Adapter must be open for this operation, doh!!\n", dev->name);
			break;
		default:
			break;
		}		/* switch srb[2] */
		break;

		/* SRB_READ_LOG - Read and reset the adapter error counters
		 */

	case SRB_READ_LOG:
	        srb_word=ntohs(readw(streamer_mmio+LAPDINC)) >> 8;
		switch (srb_word) {
		case 0x00:
			{
				int i;
				if (streamer_priv->streamer_message_level)
					printk(KERN_INFO "%s: Read Log command complete\n", dev->name);
				printk("Read Log statistics: ");
				writew(streamer_priv->srb + 6,
				       streamer_mmio + LAPA);
				for (i = 0; i < 5; i++) {
					printk("%x:", ntohs(readw(streamer_mmio + LAPDINC)));
				}
				printk("\n");
			}
			break;
		case 0x01:
			printk(KERN_WARNING "%s: Unrecognized srb command \n", dev->name);
			break;
		case 0x04:
			printk(KERN_WARNING "%s: Adapter must be open for this operation, doh!!\n", dev->name);
			break;

		}		/* switch srb[2] */
		break;

		/* SRB_READ_SR_COUNTERS - Read and reset the source routing bridge related counters */

	case SRB_READ_SR_COUNTERS:
	        srb_word=ntohs(readw(streamer_mmio+LAPDINC)) >> 8;
		switch (srb_word) {
		case 0x00:
			if (streamer_priv->streamer_message_level)
				printk(KERN_INFO "%s: Read Source Routing Counters issued\n", dev->name);
			break;
		case 0x01:
			printk(KERN_WARNING "%s: Unrecognized srb command \n", dev->name);
			break;
		case 0x04:
			printk(KERN_WARNING "%s: Adapter must be open for this operation, doh!!\n", dev->name);
			break;
		default:
			break;
		}		/* switch srb[2] */
		break;

	default:
		printk(KERN_WARNING "%s: Unrecognized srb bh return value.\n", dev->name);
		break;
	}			/* switch srb[0] */
}

static struct net_device_stats *streamer_get_stats(struct net_device *dev)
{
	struct streamer_private *streamer_priv;
	streamer_priv = (struct streamer_private *) dev->priv;
	return (struct net_device_stats *) &streamer_priv->streamer_stats;
}

static int streamer_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *saddr = addr;
	struct streamer_private *streamer_priv = (struct streamer_private *) dev->priv;

	if (netif_running(dev)) 
	{
		printk(KERN_WARNING "%s: Cannot set mac/laa address while card is open\n", dev->name);
		return -EIO;
	}

	memcpy(streamer_priv->streamer_laa, saddr->sa_data, dev->addr_len);

	if (streamer_priv->streamer_message_level) {
		printk(KERN_INFO "%s: MAC/LAA Set to  = %x.%x.%x.%x.%x.%x\n",
		       dev->name, streamer_priv->streamer_laa[0],
		       streamer_priv->streamer_laa[1],
		       streamer_priv->streamer_laa[2],
		       streamer_priv->streamer_laa[3],
		       streamer_priv->streamer_laa[4],
		       streamer_priv->streamer_laa[5]);
	}
	return 0;
}

static void streamer_arb_cmd(struct net_device *dev)
{
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	__u8 header_len;
	__u16 frame_len, buffer_len;
	struct sk_buff *mac_frame;
	__u8 frame_data[256];
	__u16 buff_off;
	__u16 lan_status = 0, lan_status_diff;	/* Initialize to stop compiler warning */
	__u8 fdx_prot_error;
	__u16 next_ptr;
	__u16 arb_word;

#if STREAMER_NETWORK_MONITOR
	struct trh_hdr *mac_hdr;
#endif

	writew(streamer_priv->arb, streamer_mmio + LAPA);
	arb_word=ntohs(readw(streamer_mmio+LAPD)) >> 8;
	
	if (arb_word == ARB_RECEIVE_DATA) {	/* Receive.data, MAC frames */
		writew(streamer_priv->arb + 6, streamer_mmio + LAPA);
		streamer_priv->mac_rx_buffer = buff_off = ntohs(readw(streamer_mmio + LAPDINC));
		header_len=ntohs(readw(streamer_mmio+LAPDINC)) >> 8; /* 802.5 Token-Ring Header Length */
		frame_len = ntohs(readw(streamer_mmio + LAPDINC));

#if STREAMER_DEBUG
		{
			int i;
			__u16 next;
			__u8 status;
			__u16 len;

			writew(ntohs(buff_off), streamer_mmio + LAPA);	/*setup window to frame data */
			next = htons(readw(streamer_mmio + LAPDINC));
			status =
			    ntohs(readw(streamer_mmio + LAPDINC)) & 0xff;
			len = ntohs(readw(streamer_mmio + LAPDINC));

			/* print out 1st 14 bytes of frame data */
			for (i = 0; i < 7; i++) {
				printk("Loc %d = %04x\n", i,
				       ntohs(readw
					     (streamer_mmio + LAPDINC)));
			}

			printk("next %04x, fs %02x, len %04x \n", next,
			       status, len);
		}
#endif
		mac_frame = dev_alloc_skb(frame_len);

		/* Walk the buffer chain, creating the frame */

		do {
			int i;
			__u16 rx_word;

			writew(htons(buff_off), streamer_mmio + LAPA);	/* setup window to frame data */
			next_ptr = ntohs(readw(streamer_mmio + LAPDINC));
			readw(streamer_mmio + LAPDINC);	/* read thru status word */
			buffer_len = ntohs(readw(streamer_mmio + LAPDINC));

			if (buffer_len > 256)
				break;

			i = 0;
			while (i < buffer_len) {
				rx_word=ntohs(readw(streamer_mmio+LAPDINC));
				frame_data[i]=rx_word >> 8;
				frame_data[i+1]=rx_word & 0xff;
				i += 2;
			}

			memcpy_fromio(skb_put(mac_frame, buffer_len),
				      frame_data, buffer_len);
		} while (next_ptr && (buff_off = next_ptr));

#if STREAMER_NETWORK_MONITOR
		printk(KERN_WARNING "%s: Received MAC Frame, details: \n",
		       dev->name);
		mac_hdr = (struct trh_hdr *) mac_frame->data;
		printk(KERN_WARNING
		       "%s: MAC Frame Dest. Addr: %02x:%02x:%02x:%02x:%02x:%02x \n",
		       dev->name, mac_hdr->daddr[0], mac_hdr->daddr[1],
		       mac_hdr->daddr[2], mac_hdr->daddr[3],
		       mac_hdr->daddr[4], mac_hdr->daddr[5]);
		printk(KERN_WARNING
		       "%s: MAC Frame Srce. Addr: %02x:%02x:%02x:%02x:%02x:%02x \n",
		       dev->name, mac_hdr->saddr[0], mac_hdr->saddr[1],
		       mac_hdr->saddr[2], mac_hdr->saddr[3],
		       mac_hdr->saddr[4], mac_hdr->saddr[5]);
#endif
		mac_frame->dev = dev;
		mac_frame->protocol = tr_type_trans(mac_frame, dev);
		netif_rx(mac_frame);

		/* Now tell the card we have dealt with the received frame */

		/* Set LISR Bit 1 */
		writel(LISR_ARB_FREE, streamer_priv->streamer_mmio + LISR_SUM);

		/* Is the ASB free ? */

		if (!(readl(streamer_priv->streamer_mmio + SISR) & SISR_ASB_FREE)) 
		{
			streamer_priv->asb_queued = 1;
			writel(LISR_ASB_FREE_REQ, streamer_priv->streamer_mmio + LISR_SUM);
			return;
			/* Drop out and wait for the bottom half to be run */
		}


		writew(streamer_priv->asb, streamer_mmio + LAPA);
		writew(htons(ASB_RECEIVE_DATA << 8), streamer_mmio+LAPDINC);
		writew(htons(STREAMER_CLEAR_RET_CODE << 8), streamer_mmio+LAPDINC);
		writew(0, streamer_mmio + LAPDINC);
		writew(htons(streamer_priv->mac_rx_buffer), streamer_mmio + LAPD);

		writel(LISR_ASB_REPLY | LISR_ASB_FREE_REQ, streamer_priv->streamer_mmio + LISR_SUM);

		streamer_priv->asb_queued = 2;
		return;

	} else if (arb_word == ARB_LAN_CHANGE_STATUS) {	/* Lan.change.status */
		writew(streamer_priv->arb + 6, streamer_mmio + LAPA);
		lan_status = ntohs(readw(streamer_mmio + LAPDINC));
		fdx_prot_error = ntohs(readw(streamer_mmio+LAPD)) >> 8;
		
		/* Issue ARB Free */
		writew(LISR_ARB_FREE, streamer_priv->streamer_mmio + LISR_SUM);

		lan_status_diff = (streamer_priv->streamer_lan_status ^ lan_status) & 
		    lan_status; 

		if (lan_status_diff & (LSC_LWF | LSC_ARW | LSC_FPE | LSC_RR)) 
		{
			if (lan_status_diff & LSC_LWF)
				printk(KERN_WARNING "%s: Short circuit detected on the lobe\n", dev->name);
			if (lan_status_diff & LSC_ARW)
				printk(KERN_WARNING "%s: Auto removal error\n", dev->name);
			if (lan_status_diff & LSC_FPE)
				printk(KERN_WARNING "%s: FDX Protocol Error\n", dev->name);
			if (lan_status_diff & LSC_RR)
				printk(KERN_WARNING "%s: Force remove MAC frame received\n", dev->name);

			/* Adapter has been closed by the hardware */

			/* reset tx/rx fifo's and busmaster logic */

			/* @TBD. no llc reset on autostreamer writel(readl(streamer_mmio+BCTL)|(3<<13),streamer_mmio+BCTL);
			   udelay(1);
			   writel(readl(streamer_mmio+BCTL)&~(3<<13),streamer_mmio+BCTL); */
			netif_stop_queue(dev);
			free_irq(dev->irq, dev);

			printk(KERN_WARNING "%s: Adapter has been closed \n", dev->name);

		}
		/* If serious error */
		if (streamer_priv->streamer_message_level) {
			if (lan_status_diff & LSC_SIG_LOSS)
				printk(KERN_WARNING "%s: No receive signal detected \n", dev->name);
			if (lan_status_diff & LSC_HARD_ERR) 
				printk(KERN_INFO "%s: Beaconing \n", dev->name);
			if (lan_status_diff & LSC_SOFT_ERR)
				printk(KERN_WARNING "%s: Adapter transmitted Soft Error Report Mac Frame \n", dev->name);
			if (lan_status_diff & LSC_TRAN_BCN)
				printk(KERN_INFO "%s: We are tranmitting the beacon, aaah\n", dev->name);
			if (lan_status_diff & LSC_SS)
				printk(KERN_INFO "%s: Single Station on the ring \n", dev->name);
			if (lan_status_diff & LSC_RING_REC)
				printk(KERN_INFO "%s: Ring recovery ongoing\n", dev->name);
			if (lan_status_diff & LSC_FDX_MODE)
				printk(KERN_INFO "%s: Operating in FDX mode\n", dev->name);
		}

		if (lan_status_diff & LSC_CO) {
			if (streamer_priv->streamer_message_level)
				printk(KERN_INFO "%s: Counter Overflow \n", dev->name);

			/* Issue READ.LOG command */

			writew(streamer_priv->srb, streamer_mmio + LAPA);
			writew(htons(SRB_READ_LOG << 8),streamer_mmio+LAPDINC);
			writew(htons(STREAMER_CLEAR_RET_CODE << 8), streamer_mmio+LAPDINC);
			writew(0, streamer_mmio + LAPDINC);
			streamer_priv->srb_queued = 2;	/* Can't sleep, use srb_bh */

			writew(LISR_SRB_CMD, streamer_mmio + LISR_SUM);
		}

		if (lan_status_diff & LSC_SR_CO) {
			if (streamer_priv->streamer_message_level)
				printk(KERN_INFO "%s: Source routing counters overflow\n", dev->name);

			/* Issue a READ.SR.COUNTERS */
			writew(streamer_priv->srb, streamer_mmio + LAPA);
			writew(htons(SRB_READ_SR_COUNTERS << 8),
			       streamer_mmio+LAPDINC);
			writew(htons(STREAMER_CLEAR_RET_CODE << 8),
			       streamer_mmio+LAPDINC);
			streamer_priv->srb_queued = 2;	/* Can't sleep, use srb_bh */
			writew(LISR_SRB_CMD, streamer_mmio + LISR_SUM);

		}
		streamer_priv->streamer_lan_status = lan_status;
	} /* Lan.change.status */
	else
		printk(KERN_WARNING "%s: Unknown arb command \n", dev->name);
}

static void streamer_asb_bh(struct net_device *dev)
{
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;

	if (streamer_priv->asb_queued == 1) 
	{
		/* Dropped through the first time */

		writew(streamer_priv->asb, streamer_mmio + LAPA);
		writew(htons(ASB_RECEIVE_DATA << 8),streamer_mmio+LAPDINC);
		writew(htons(STREAMER_CLEAR_RET_CODE << 8), streamer_mmio+LAPDINC);
		writew(0, streamer_mmio + LAPDINC);
		writew(htons(streamer_priv->mac_rx_buffer), streamer_mmio + LAPD);

		writel(LISR_ASB_REPLY | LISR_ASB_FREE_REQ, streamer_priv->streamer_mmio + LISR_SUM);
		streamer_priv->asb_queued = 2;

		return;
	}

	if (streamer_priv->asb_queued == 2) {
		__u8 rc;
		writew(streamer_priv->asb + 2, streamer_mmio + LAPA);
		rc=ntohs(readw(streamer_mmio+LAPD)) >> 8;
		switch (rc) {
		case 0x01:
			printk(KERN_WARNING "%s: Unrecognized command code \n", dev->name);
			break;
		case 0x26:
			printk(KERN_WARNING "%s: Unrecognized buffer address \n", dev->name);
			break;
		case 0xFF:
			/* Valid response, everything should be ok again */
			break;
		default:
			printk(KERN_WARNING "%s: Invalid return code in asb\n", dev->name);
			break;
		}
	}
	streamer_priv->asb_queued = 0;
}

static int streamer_change_mtu(struct net_device *dev, int mtu)
{
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u16 max_mtu;

	if (streamer_priv->streamer_ring_speed == 4)
		max_mtu = 4500;
	else
		max_mtu = 18000;

	if (mtu > max_mtu)
		return -EINVAL;
	if (mtu < 100)
		return -EINVAL;

	dev->mtu = mtu;
	streamer_priv->pkt_buf_sz = mtu + TR_HLEN;

	return 0;
}

#if STREAMER_NETWORK_MONITOR
#ifdef CONFIG_PROC_FS
static int streamer_proc_info(char *buffer, char **start, off_t offset,
			      int length, int *eof, void *data)
{
	struct pci_dev *pci_device = NULL;
	int len = 0;
	off_t begin = 0;
	off_t pos = 0;
	int size;

	struct device *dev;


	size = sprintf(buffer, "IBM LanStreamer/MPC Chipset Token Ring Adapters\n");

	pos += size;
	len += size;

	while ((pci_device = pci_find_device(PCI_VENDOR_ID_IBM, PCI_DEVICE_ID_IBM_TR, pci_device))) 
	{

		for (dev = dev_base; dev != NULL; dev = dev->next) 
		{
		        if (dev->base_addr == pci_device->resource[0].start) 
			{       /* Yep, a Streamer device */
				size = sprintf_info(buffer + len, dev);
				len += size;
				pos = begin + len;

				if (pos < offset) {
					len = 0;
					begin = pos;
				}
				if (pos > offset + length)
					break;
			}	/* if */
		}		/* for */
	}			/* While */

	*start = buffer + (offset - begin);	/* Start of wanted data */
	len -= (offset - begin);	/* Start slop */
	if (len > length)
		len = length;	/* Ending slop */
	return len;
}

static int sprintf_info(char *buffer, struct net_device *dev)
{
	struct streamer_private *streamer_priv =
	    (struct streamer_private *) dev->priv;
	__u8 *streamer_mmio = streamer_priv->streamer_mmio;
	struct streamer_adapter_addr_table sat;
	struct streamer_parameters_table spt;
	int size = 0;
	int i;

	writew(streamer_priv->streamer_addr_table_addr, streamer_mmio + LAPA);
	for (i = 0; i < 14; i += 2) {
		__u16 io_word;
		__u8 *datap = (__u8 *) & sat;
		io_word=ntohs(readw(streamer_mmio+LAPDINC));
		datap[size]=io_word >> 8;
		datap[size+1]=io_word & 0xff;
	}
	writew(streamer_priv->streamer_parms_addr, streamer_mmio + LAPA);
	for (i = 0; i < 68; i += 2) {
		__u16 io_word;
		__u8 *datap = (__u8 *) & spt;
		io_word=ntohs(readw(streamer_mmio+LAPDINC));
		datap[size]=io_word >> 8;
		datap[size+1]=io_word & 0xff;
	}


	size = sprintf(buffer, "\n%6s: Adapter Address   : Node Address      : Functional Addr\n", dev->name);

	size += sprintf(buffer + size,
		    "%6s: %02x:%02x:%02x:%02x:%02x:%02x : %02x:%02x:%02x:%02x:%02x:%02x : %02x:%02x:%02x:%02x\n",
		    dev->name, dev->dev_addr[0], dev->dev_addr[1],
		    dev->dev_addr[2], dev->dev_addr[3], dev->dev_addr[4],
		    dev->dev_addr[5], sat.node_addr[0], sat.node_addr[1],
		    sat.node_addr[2], sat.node_addr[3], sat.node_addr[4],
		    sat.node_addr[5], sat.func_addr[0], sat.func_addr[1],
		    sat.func_addr[2], sat.func_addr[3]);

	size += sprintf(buffer + size, "\n%6s: Token Ring Parameters Table:\n", dev->name);

	size += sprintf(buffer + size, "%6s: Physical Addr : Up Node Address   : Poll Address      : AccPri : Auth Src : Att Code :\n", dev->name);

	size += sprintf(buffer + size,
		    "%6s: %02x:%02x:%02x:%02x   : %02x:%02x:%02x:%02x:%02x:%02x : %02x:%02x:%02x:%02x:%02x:%02x : %04x   : %04x     :  %04x    :\n",
		    dev->name, spt.phys_addr[0], spt.phys_addr[1],
		    spt.phys_addr[2], spt.phys_addr[3],
		    spt.up_node_addr[0], spt.up_node_addr[1],
		    spt.up_node_addr[2], spt.up_node_addr[3],
		    spt.up_node_addr[4], spt.up_node_addr[4],
		    spt.poll_addr[0], spt.poll_addr[1], spt.poll_addr[2],
		    spt.poll_addr[3], spt.poll_addr[4], spt.poll_addr[5],
		    ntohs(spt.acc_priority), ntohs(spt.auth_source_class),
		    ntohs(spt.att_code));

	size += sprintf(buffer + size, "%6s: Source Address    : Bcn T : Maj. V : Lan St : Lcl Rg : Mon Err : Frame Correl : \n", dev->name);

	size += sprintf(buffer + size,
		    "%6s: %02x:%02x:%02x:%02x:%02x:%02x : %04x  : %04x   : %04x   : %04x   : %04x    :     %04x     : \n",
		    dev->name, spt.source_addr[0], spt.source_addr[1],
		    spt.source_addr[2], spt.source_addr[3],
		    spt.source_addr[4], spt.source_addr[5],
		    ntohs(spt.beacon_type), ntohs(spt.major_vector),
		    ntohs(spt.lan_status), ntohs(spt.local_ring),
		    ntohs(spt.mon_error), ntohs(spt.frame_correl));

	size += sprintf(buffer + size, "%6s: Beacon Details :  Tx  :  Rx  : NAUN Node Address : NAUN Node Phys : \n",
		    dev->name);

	size += sprintf(buffer + size,
		    "%6s:                :  %02x  :  %02x  : %02x:%02x:%02x:%02x:%02x:%02x : %02x:%02x:%02x:%02x    : \n",
		    dev->name, ntohs(spt.beacon_transmit),
		    ntohs(spt.beacon_receive), spt.beacon_naun[0],
		    spt.beacon_naun[1], spt.beacon_naun[2],
		    spt.beacon_naun[3], spt.beacon_naun[4],
		    spt.beacon_naun[5], spt.beacon_phys[0],
		    spt.beacon_phys[1], spt.beacon_phys[2],
		    spt.beacon_phys[3]);
	return size;
}
#endif
#endif

#ifdef MODULE

static struct net_device *dev_streamer[STREAMER_MAX_ADAPTERS];

int init_module(void)
{
	int i;

#if STREAMER_NETWORK_MONITOR
#ifdef CONFIG_PROC_FS
	create_proc_read_entry("net/streamer_tr",0,0,streamer_proc_info,NULL); 
#endif
#endif
	for (i = 0; (i < STREAMER_MAX_ADAPTERS); i++) 
	{
		dev_streamer[i] = NULL;
		dev_streamer[i] = init_trdev(dev_streamer[i], 0);
		if (dev_streamer[i] == NULL)
			return -ENOMEM;

		dev_streamer[i]->init = &streamer_probe;

		if (register_trdev(dev_streamer[i]) != 0) {
			kfree(dev_streamer[i]);
			dev_streamer[i] = NULL;
			if (i == 0) 
			{
				printk(KERN_INFO "Streamer: No IBM LanStreamer PCI Token Ring cards found in system.\n");
				return -EIO;
			} else {
				printk(KERN_INFO "Streamer: %d IBM LanStreamer PCI Token Ring card(s) found in system.\n", i);
				return 0;
			}
		}
	}

	return 0;
}

void cleanup_module(void)
{
	int i;
	struct streamer_private *streamer_priv;

	for (i = 0; i < STREAMER_MAX_ADAPTERS; i++)
		if (dev_streamer[i]) {
			unregister_trdev(dev_streamer[i]);
			release_region(dev_streamer[i]->base_addr, STREAMER_IO_SPACE);
			streamer_priv=(struct streamer_private *)dev_streamer[i]->priv;
			kfree(streamer_priv->streamer_rx_ring);
			kfree(streamer_priv->streamer_tx_ring);
			kfree(dev_streamer[i]->priv);
			kfree(dev_streamer[i]);
			dev_streamer[i] = NULL;
		}
#if STREAMER_NETWORK_MONITOR
#ifdef CONFIG_PROC_FS
	remove_proc_entry("net/streamer_tr", NULL);
#endif
#endif
}
#endif				/* MODULE */
