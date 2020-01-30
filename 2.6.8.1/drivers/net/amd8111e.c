
/* Advanced  Micro Devices Inc. AMD8111E Linux Network Driver 
 * Copyright (C) 2004 Advanced Micro Devices 
 *
 * 
 * Copyright 2001,2002 Jeff Garzik <jgarzik@mandrakesoft.com> [ 8139cp.c,tg3.c ]
 * Copyright (C) 2001, 2002 David S. Miller (davem@redhat.com)[ tg3.c]
 * Copyright 1996-1999 Thomas Bogendoerfer [ pcnet32.c ]
 * Derived from the lance driver written 1993,1994,1995 by Donald Becker.
 * Copyright 1993 United States Government as represented by the
 *	Director, National Security Agency.[ pcnet32.c ]
 * Carsten Langgaard, carstenl@mips.com [ pcnet32.c ]
 * Copyright (C) 2000 MIPS Technologies, Inc.  All rights reserved.
 *
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 
 * USA
  
Module Name:

	amd8111e.c

Abstract:
	
 	 AMD8111 based 10/100 Ethernet Controller Driver. 

Environment:

	Kernel Mode

Revision History:
 	3.0.0
	   Initial Revision.
	3.0.1
	 1. Dynamic interrupt coalescing.
	 2. Removed prev_stats.
	 3. MII support.
	 4. Dynamic IPG support
	3.0.2  05/29/2003
	 1. Bug fix: Fixed failure to send jumbo packets larger than 4k.
	 2. Bug fix: Fixed VLAN support failure.
	 3. Bug fix: Fixed receive interrupt coalescing bug.
	 4. Dynamic IPG support is disabled by default.
	3.0.3 06/05/2003
	 1. Bug fix: Fixed failure to close the interface if SMP is enabled.
	3.0.4 12/09/2003
	 1. Added set_mac_address routine for bonding driver support.
	 2. Tested the driver for bonding support
	 3. Bug fix: Fixed mismach in actual receive buffer lenth and lenth 
	    indicated to the h/w.
	 4. Modified amd8111e_rx() routine to receive all the received packets 
	    in the first interrupt.
	 5. Bug fix: Corrected  rx_errors  reported in get_stats() function.
	3.0.5 03/22/2004
	 1. Added NAPI support  

*/


#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/if_vlan.h>
#include <linux/ctype.h>	
#include <linux/crc32.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>

#if defined(CONFIG_VLAN_8021Q) || defined(CONFIG_VLAN_8021Q_MODULE)
#define AMD8111E_VLAN_TAG_USED 1
#else
#define AMD8111E_VLAN_TAG_USED 0
#endif

#include "amd8111e.h"
#define MODULE_NAME	"amd8111e"
#define MODULE_VERS	"3.0.5"
MODULE_AUTHOR("Advanced Micro Devices, Inc.");
MODULE_DESCRIPTION ("AMD8111 based 10/100 Ethernet Controller. Driver Version 3.0.3");
MODULE_LICENSE("GPL");
MODULE_PARM(speed_duplex, "1-" __MODULE_STRING (MAX_UNITS) "i");
MODULE_PARM_DESC(speed_duplex, "Set device speed and duplex modes, 0: Auto Negotitate, 1: 10Mbps Half Duplex, 2: 10Mbps Full Duplex, 3: 100Mbps Half Duplex, 4: 100Mbps Full Duplex");
MODULE_PARM(coalesce, "1-" __MODULE_STRING(MAX_UNITS) "i");
MODULE_PARM_DESC(coalesce, "Enable or Disable interrupt coalescing, 1: Enable, 0: Disable");
MODULE_PARM(dynamic_ipg, "1-" __MODULE_STRING(MAX_UNITS) "i");
MODULE_PARM_DESC(dynamic_ipg, "Enable or Disable dynamic IPG, 1: Enable, 0: Disable");

static struct pci_device_id amd8111e_pci_tbl[] = {
		
	{ PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD8111E_7462,
	 PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0UL },
	{ 0, }

};
/* 
This function will read the PHY registers.
*/
static int amd8111e_read_phy(struct amd8111e_priv* lp, int phy_id, int reg, u32* val)
{
	void * mmio = lp->mmio;
	unsigned int reg_val;
	unsigned int repeat= REPEAT_CNT;

	reg_val = readl(mmio + PHY_ACCESS);
	while (reg_val & PHY_CMD_ACTIVE)
		reg_val = readl( mmio + PHY_ACCESS );

	writel( PHY_RD_CMD | ((phy_id & 0x1f) << 21) |
			   ((reg & 0x1f) << 16),  mmio +PHY_ACCESS);
	do{
		reg_val = readl(mmio + PHY_ACCESS);
		udelay(30);  /* It takes 30 us to read/write data */
	} while (--repeat && (reg_val & PHY_CMD_ACTIVE));
	if(reg_val & PHY_RD_ERR)
		goto err_phy_read;
	
	*val = reg_val & 0xffff;
	return 0;
err_phy_read:	
	*val = 0;
	return -EINVAL;
	
}

/* 
This function will write into PHY registers. 
*/
static int amd8111e_write_phy(struct amd8111e_priv* lp,int phy_id, int reg, u32 val)
{
	unsigned int repeat = REPEAT_CNT
	void * mmio = lp->mmio;
	unsigned int reg_val;

	reg_val = readl(mmio + PHY_ACCESS);
	while (reg_val & PHY_CMD_ACTIVE)
		reg_val = readl( mmio + PHY_ACCESS );

	writel( PHY_WR_CMD | ((phy_id & 0x1f) << 21) |
			   ((reg & 0x1f) << 16)|val, mmio + PHY_ACCESS);

	do{
		reg_val = readl(mmio + PHY_ACCESS);
		udelay(30);  /* It takes 30 us to read/write the data */
	} while (--repeat && (reg_val & PHY_CMD_ACTIVE));
	
	if(reg_val & PHY_RD_ERR)
		goto err_phy_write;
	
	return 0;

err_phy_write:	
	return -EINVAL;
	
}
/* 
This is the mii register read function provided to the mii interface.
*/ 
static int amd8111e_mdio_read(struct net_device * dev, int phy_id, int reg_num)
{
	struct amd8111e_priv* lp = netdev_priv(dev);
	unsigned int reg_val;

	amd8111e_read_phy(lp,phy_id,reg_num,&reg_val);
	return reg_val;
	
}

/* 
This is the mii register write function provided to the mii interface.
*/ 
static void amd8111e_mdio_write(struct net_device * dev, int phy_id, int reg_num, int val)
{
	struct amd8111e_priv* lp = netdev_priv(dev);

	amd8111e_write_phy(lp, phy_id, reg_num, val);
}

/*
This function will set PHY speed. During initialization sets the original speed to 100 full.
*/
static void amd8111e_set_ext_phy(struct net_device *dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	u32 bmcr,advert,tmp;
	
	/* Determine mii register values to set the speed */
	advert = amd8111e_mdio_read(dev, PHY_ID, MII_ADVERTISE);
	tmp = advert & ~(ADVERTISE_ALL | ADVERTISE_100BASE4);
	switch (lp->ext_phy_option){

		default:
		case SPEED_AUTONEG: /* advertise all values */
			tmp |= ( ADVERTISE_10HALF|ADVERTISE_10FULL|
				ADVERTISE_100HALF|ADVERTISE_100FULL) ;
			break;
		case SPEED10_HALF:
			tmp |= ADVERTISE_10HALF;
			break;
		case SPEED10_FULL:
			tmp |= ADVERTISE_10FULL;
			break;
		case SPEED100_HALF: 
			tmp |= ADVERTISE_100HALF;
			break;
		case SPEED100_FULL:
			tmp |= ADVERTISE_100FULL;
			break;
	}

	if(advert != tmp)
		amd8111e_mdio_write(dev, PHY_ID, MII_ADVERTISE, tmp);
	/* Restart auto negotiation */
	bmcr = amd8111e_mdio_read(dev, PHY_ID, MII_BMCR);
	bmcr |= (BMCR_ANENABLE | BMCR_ANRESTART);
	amd8111e_mdio_write(dev, PHY_ID, MII_BMCR, bmcr);

}

/* 
This function will unmap skb->data space and will free 
all transmit and receive skbuffs.
*/
static int amd8111e_free_skbs(struct net_device *dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	struct sk_buff* rx_skbuff;
	int i;

	/* Freeing transmit skbs */
	for(i = 0; i < NUM_TX_BUFFERS; i++){
		if(lp->tx_skbuff[i]){
			pci_unmap_single(lp->pci_dev,lp->tx_dma_addr[i],					lp->tx_skbuff[i]->len,PCI_DMA_TODEVICE);
			dev_kfree_skb (lp->tx_skbuff[i]);
			lp->tx_skbuff[i] = NULL;
			lp->tx_dma_addr[i] = 0;
		}
	}
	/* Freeing previously allocated receive buffers */
	for (i = 0; i < NUM_RX_BUFFERS; i++){
		rx_skbuff = lp->rx_skbuff[i];
		if(rx_skbuff != NULL){
			pci_unmap_single(lp->pci_dev,lp->rx_dma_addr[i],
				  lp->rx_buff_len - 2,PCI_DMA_FROMDEVICE);
			dev_kfree_skb(lp->rx_skbuff[i]);
			lp->rx_skbuff[i] = NULL;
			lp->rx_dma_addr[i] = 0;
		}
	}
	
	return 0;
}

/*
This will set the receive buffer length corresponding to the mtu size of networkinterface.
*/
static inline void amd8111e_set_rx_buff_len(struct net_device* dev)
{
	struct amd8111e_priv* lp = netdev_priv(dev);
	unsigned int mtu = dev->mtu;
	
	if (mtu > ETH_DATA_LEN){
		/* MTU + ethernet header + FCS
		+ optional VLAN tag + skb reserve space 2 */

		lp->rx_buff_len = mtu + ETH_HLEN + 10;
		lp->options |= OPTION_JUMBO_ENABLE;
	} else{
		lp->rx_buff_len = PKT_BUFF_SZ;
		lp->options &= ~OPTION_JUMBO_ENABLE;
	}
}

/* 
This function will free all the previously allocated buffers, determine new receive buffer length  and will allocate new receive buffers. This function also allocates and initializes both the transmitter and receive hardware descriptors.
 */
static int amd8111e_init_ring(struct net_device *dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	int i;

	lp->rx_idx = lp->tx_idx = 0;
	lp->tx_complete_idx = 0;
	lp->tx_ring_idx = 0;
	

	if(lp->opened)
		/* Free previously allocated transmit and receive skbs */
		amd8111e_free_skbs(dev);	

	else{
		 /* allocate the tx and rx descriptors */
	     	if((lp->tx_ring = pci_alloc_consistent(lp->pci_dev, 
			sizeof(struct amd8111e_tx_dr)*NUM_TX_RING_DR,
			&lp->tx_ring_dma_addr)) == NULL)
		
			goto err_no_mem;
	
	     	if((lp->rx_ring = pci_alloc_consistent(lp->pci_dev, 
			sizeof(struct amd8111e_rx_dr)*NUM_RX_RING_DR,
			&lp->rx_ring_dma_addr)) == NULL)
		
			goto err_free_tx_ring;

	}
	/* Set new receive buff size */
	amd8111e_set_rx_buff_len(dev);

	/* Allocating receive  skbs */
	for (i = 0; i < NUM_RX_BUFFERS; i++) {

		if (!(lp->rx_skbuff[i] = dev_alloc_skb(lp->rx_buff_len))) {
				/* Release previos allocated skbs */
				for(--i; i >= 0 ;i--)
					dev_kfree_skb(lp->rx_skbuff[i]);
				goto err_free_rx_ring;
		}
		skb_reserve(lp->rx_skbuff[i],2);
	}
        /* Initilaizing receive descriptors */
	for (i = 0; i < NUM_RX_BUFFERS; i++) {
		lp->rx_dma_addr[i] = pci_map_single(lp->pci_dev, 
			lp->rx_skbuff[i]->data,lp->rx_buff_len-2, PCI_DMA_FROMDEVICE);

		lp->rx_ring[i].buff_phy_addr = cpu_to_le32(lp->rx_dma_addr[i]);
		lp->rx_ring[i].buff_count = cpu_to_le16(lp->rx_buff_len-2);
		lp->rx_ring[i].rx_flags = cpu_to_le16(OWN_BIT);
	}

	/* Initializing transmit descriptors */
	for (i = 0; i < NUM_TX_RING_DR; i++) {
		lp->tx_ring[i].buff_phy_addr = 0;
		lp->tx_ring[i].tx_flags = 0;
		lp->tx_ring[i].buff_count = 0;
	}

	return 0;

err_free_rx_ring:
	
	pci_free_consistent(lp->pci_dev, 
		sizeof(struct amd8111e_rx_dr)*NUM_RX_RING_DR,lp->rx_ring,
		lp->rx_ring_dma_addr);

err_free_tx_ring:
	
	pci_free_consistent(lp->pci_dev,
		 sizeof(struct amd8111e_tx_dr)*NUM_TX_RING_DR,lp->tx_ring, 
		 lp->tx_ring_dma_addr);

err_no_mem:
	return -ENOMEM;
}
/* This function will set the interrupt coalescing according to the input arguments */
static int amd8111e_set_coalesce(struct net_device * dev, enum coal_mode cmod)
{
	unsigned int timeout;
	unsigned int event_count;

	struct amd8111e_priv *lp = netdev_priv(dev);
	void* mmio = lp->mmio;
	struct amd8111e_coalesce_conf * coal_conf = &lp->coal_conf;


	switch(cmod)
	{
		case RX_INTR_COAL :
			timeout = coal_conf->rx_timeout;
			event_count = coal_conf->rx_event_count;
			if( timeout > MAX_TIMEOUT || 
					event_count > MAX_EVENT_COUNT ) 
			return -EINVAL;

			timeout = timeout * DELAY_TIMER_CONV; 
			writel(VAL0|STINTEN, mmio+INTEN0);
			writel((u32)DLY_INT_A_R0|( event_count<< 16 )|timeout,
							mmio+DLY_INT_A);
			break;

		case TX_INTR_COAL :
			timeout = coal_conf->tx_timeout;
			event_count = coal_conf->tx_event_count;
			if( timeout > MAX_TIMEOUT || 
					event_count > MAX_EVENT_COUNT ) 
			return -EINVAL;

		   
			timeout = timeout * DELAY_TIMER_CONV; 
			writel(VAL0|STINTEN,mmio+INTEN0);
			writel((u32)DLY_INT_B_T0|( event_count<< 16 )|timeout,
							 mmio+DLY_INT_B);
			break;

		case DISABLE_COAL:
			writel(0,mmio+STVAL);
			writel(STINTEN, mmio+INTEN0);
			writel(0, mmio +DLY_INT_B);
			writel(0, mmio+DLY_INT_A);
			break;
		 case ENABLE_COAL: 
		       /* Start the timer */
			writel((u32)SOFT_TIMER_FREQ, mmio+STVAL); /*  0.5 sec */
			writel(VAL0|STINTEN, mmio+INTEN0);
			break;
		default:
			break;

   }
	return 0;

}

/* 
This function initializes the device registers  and starts the device.  
*/
static int amd8111e_restart(struct net_device *dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	void * mmio = lp->mmio;
	int i,reg_val;

	/* stop the chip */
	 writel(RUN, mmio + CMD0);

	if(amd8111e_init_ring(dev))
		return -ENOMEM;

	/* enable the port manager and set auto negotiation always */
	writel((u32) VAL1|EN_PMGR, mmio + CMD3 );
	writel((u32)XPHYANE|XPHYRST , mmio + CTRL2); 
	
	amd8111e_set_ext_phy(dev);

	/* set control registers */
	reg_val = readl(mmio + CTRL1);
	reg_val &= ~XMTSP_MASK;
	writel( reg_val| XMTSP_128 | CACHE_ALIGN, mmio + CTRL1 );

	/* enable interrupt */
	writel( APINT5EN | APINT4EN | APINT3EN | APINT2EN | APINT1EN | 
		APINT0EN | MIIPDTINTEN | MCCIINTEN | MCCINTEN | MREINTEN |
		SPNDINTEN | MPINTEN | SINTEN | STINTEN, mmio + INTEN0);

	writel(VAL3 | LCINTEN | VAL1 | TINTEN0 | VAL0 | RINTEN0, mmio + INTEN0);

	/* initialize tx and rx ring base addresses */
	writel((u32)lp->tx_ring_dma_addr,mmio + XMT_RING_BASE_ADDR0);
	writel((u32)lp->rx_ring_dma_addr,mmio+ RCV_RING_BASE_ADDR0);

	writew((u32)NUM_TX_RING_DR, mmio + XMT_RING_LEN0);
	writew((u16)NUM_RX_RING_DR, mmio + RCV_RING_LEN0);
	
	/* set default IPG to 96 */
	writew((u32)DEFAULT_IPG,mmio+IPG);
	writew((u32)(DEFAULT_IPG-IFS1_DELTA), mmio + IFS1); 

	if(lp->options & OPTION_JUMBO_ENABLE){
		writel((u32)VAL2|JUMBO, mmio + CMD3);
		/* Reset REX_UFLO */
		writel( REX_UFLO, mmio + CMD2);
		/* Should not set REX_UFLO for jumbo frames */
		writel( VAL0 | APAD_XMT|REX_RTRY , mmio + CMD2);
	}else{
		writel( VAL0 | APAD_XMT | REX_RTRY|REX_UFLO, mmio + CMD2);
		writel((u32)JUMBO, mmio + CMD3);
	}

#if AMD8111E_VLAN_TAG_USED
	writel((u32) VAL2|VSIZE|VL_TAG_DEL, mmio + CMD3);
#endif
	writel( VAL0 | APAD_XMT | REX_RTRY, mmio + CMD2 );
	
	/* Setting the MAC address to the device */
	for(i = 0; i < ETH_ADDR_LEN; i++)
		writeb( dev->dev_addr[i], mmio + PADR + i ); 

	/* Enable interrupt coalesce */
	if(lp->options & OPTION_INTR_COAL_ENABLE){
		printk(KERN_INFO "%s: Interrupt Coalescing Enabled.\n",
								dev->name);
		amd8111e_set_coalesce(dev,ENABLE_COAL);
	}
	
	/* set RUN bit to start the chip */
	writel(VAL2 | RDMD0, mmio + CMD0);
	writel(VAL0 | INTREN | RUN, mmio + CMD0);
	
	/* To avoid PCI posting bug */
	readl(mmio+CMD0);
	return 0;
}
/* 
This function clears necessary the device registers. 
*/	
static void amd8111e_init_hw_default( struct amd8111e_priv* lp)
{
	unsigned int reg_val;
	unsigned int logic_filter[2] ={0,};
	void * mmio = lp->mmio;


        /* stop the chip */
	writel(RUN, mmio + CMD0);

	/* AUTOPOLL0 Register *//*TBD default value is 8100 in FPS */
	writew( 0x8101, mmio + AUTOPOLL0);

	/* Clear RCV_RING_BASE_ADDR */
	writel(0, mmio + RCV_RING_BASE_ADDR0);

	/* Clear XMT_RING_BASE_ADDR */
	writel(0, mmio + XMT_RING_BASE_ADDR0);
	writel(0, mmio + XMT_RING_BASE_ADDR1);
	writel(0, mmio + XMT_RING_BASE_ADDR2);
	writel(0, mmio + XMT_RING_BASE_ADDR3);

	/* Clear CMD0  */
	writel(CMD0_CLEAR,mmio + CMD0);
	
	/* Clear CMD2 */
	writel(CMD2_CLEAR, mmio +CMD2);

	/* Clear CMD7 */
	writel(CMD7_CLEAR , mmio + CMD7);

	/* Clear DLY_INT_A and DLY_INT_B */
	writel(0x0, mmio + DLY_INT_A);
	writel(0x0, mmio + DLY_INT_B);

	/* Clear FLOW_CONTROL */
	writel(0x0, mmio + FLOW_CONTROL);

	/* Clear INT0  write 1 to clear register */
	reg_val = readl(mmio + INT0);
	writel(reg_val, mmio + INT0);

	/* Clear STVAL */
	writel(0x0, mmio + STVAL);

	/* Clear INTEN0 */
	writel( INTEN0_CLEAR, mmio + INTEN0);

	/* Clear LADRF */
	writel(0x0 , mmio + LADRF);

	/* Set SRAM_SIZE & SRAM_BOUNDARY registers  */
	writel( 0x80010,mmio + SRAM_SIZE);

	/* Clear RCV_RING0_LEN */
	writel(0x0, mmio +  RCV_RING_LEN0);

	/* Clear XMT_RING0/1/2/3_LEN */
	writel(0x0, mmio +  XMT_RING_LEN0);
	writel(0x0, mmio +  XMT_RING_LEN1);
	writel(0x0, mmio +  XMT_RING_LEN2);
	writel(0x0, mmio +  XMT_RING_LEN3);

	/* Clear XMT_RING_LIMIT */
	writel(0x0, mmio + XMT_RING_LIMIT);

	/* Clear MIB */
	writew(MIB_CLEAR, mmio + MIB_ADDR);

	/* Clear LARF */
	amd8111e_writeq(*(u64*)logic_filter,mmio+LADRF);

	/* SRAM_SIZE register */
	reg_val = readl(mmio + SRAM_SIZE);
	
	if(lp->options & OPTION_JUMBO_ENABLE)
		writel( VAL2|JUMBO, mmio + CMD3);
#if AMD8111E_VLAN_TAG_USED
	writel(VAL2|VSIZE|VL_TAG_DEL, mmio + CMD3 );
#endif
	/* Set default value to CTRL1 Register */
	writel(CTRL1_DEFAULT, mmio + CTRL1);

	/* To avoid PCI posting bug */
	readl(mmio + CMD2);

}

/* 
This function disables the interrupt and clears all the pending 
interrupts in INT0
 */
static void amd8111e_disable_interrupt(struct amd8111e_priv* lp)
{	
	u32 intr0;

	/* Disable interrupt */
	writel(INTREN, lp->mmio + CMD0);
	
	/* Clear INT0 */
	intr0 = readl(lp->mmio + INT0);
	writel(intr0, lp->mmio + INT0);
	
	/* To avoid PCI posting bug */
	readl(lp->mmio + INT0);

}

/*
This function stops the chip. 
*/
static void amd8111e_stop_chip(struct amd8111e_priv* lp)
{
	writel(RUN, lp->mmio + CMD0);
	
	/* To avoid PCI posting bug */
	readl(lp->mmio + CMD0);
}

/* 
This function frees the  transmiter and receiver descriptor rings.
*/
static void amd8111e_free_ring(struct amd8111e_priv* lp)
{	

	/* Free transmit and receive skbs */
	amd8111e_free_skbs(lp->amd8111e_net_dev);

	/* Free transmit and receive descriptor rings */
	if(lp->rx_ring){
		pci_free_consistent(lp->pci_dev, 
			sizeof(struct amd8111e_rx_dr)*NUM_RX_RING_DR,
			lp->rx_ring, lp->rx_ring_dma_addr);
		lp->rx_ring = NULL;
	}
	
	if(lp->tx_ring){
		pci_free_consistent(lp->pci_dev, 
			sizeof(struct amd8111e_tx_dr)*NUM_TX_RING_DR,
			lp->tx_ring, lp->tx_ring_dma_addr);

		lp->tx_ring = NULL;
	}

}
#if AMD8111E_VLAN_TAG_USED	
/* 
This is the receive indication function for packets with vlan tag.
*/	
static int amd8111e_vlan_rx(struct amd8111e_priv *lp, struct sk_buff *skb, u16 vlan_tag)
{
#ifdef CONFIG_AMD8111E_NAPI
	return vlan_hwaccel_receive_skb(skb, lp->vlgrp,vlan_tag);
#else
	return vlan_hwaccel_rx(skb, lp->vlgrp, vlan_tag);
#endif /* CONFIG_AMD8111E_NAPI */
}
#endif

/*
This function will free all the transmit skbs that are actually transmitted by the device. It will check the ownership of the skb before freeing the skb. 
*/
static int amd8111e_tx(struct net_device *dev)
{
	struct amd8111e_priv* lp = netdev_priv(dev);
	int tx_index = lp->tx_complete_idx & TX_RING_DR_MOD_MASK;
	int status;
	/* Complete all the transmit packet */
	while (lp->tx_complete_idx != lp->tx_idx){
		tx_index =  lp->tx_complete_idx & TX_RING_DR_MOD_MASK;
		status = le16_to_cpu(lp->tx_ring[tx_index].tx_flags);

		if(status & OWN_BIT)
			break;	/* It still hasn't been Txed */

		lp->tx_ring[tx_index].buff_phy_addr = 0;

		/* We must free the original skb */
		if (lp->tx_skbuff[tx_index]) {
			pci_unmap_single(lp->pci_dev, lp->tx_dma_addr[tx_index],
				  	lp->tx_skbuff[tx_index]->len,
					PCI_DMA_TODEVICE);
			dev_kfree_skb_irq (lp->tx_skbuff[tx_index]);
			lp->tx_skbuff[tx_index] = NULL;
			lp->tx_dma_addr[tx_index] = 0;
		}
		lp->tx_complete_idx++;
		/*COAL update tx coalescing parameters */
		lp->coal_conf.tx_packets++;
		lp->coal_conf.tx_bytes += lp->tx_ring[tx_index].buff_count;	

		if (netif_queue_stopped(dev) &&
			lp->tx_complete_idx > lp->tx_idx - NUM_TX_BUFFERS +2){
			/* The ring is no longer full, clear tbusy. */
			/* lp->tx_full = 0; */
			netif_wake_queue (dev);
		}
	}
	return 0;
}

#if CONFIG_AMD8111E_NAPI 
/* This function handles the driver receive operation in polling mode */
static int amd8111e_rx_poll(struct net_device *dev, int * budget)
{
	struct amd8111e_priv *lp = dev->priv;
	int rx_index = lp->rx_idx & RX_RING_DR_MOD_MASK;
	void * mmio = lp->mmio;
	struct sk_buff *skb,*new_skb;
	int min_pkt_len, status;
	unsigned int intr0;
	int num_rx_pkt = 0;
	/*int max_rx_pkt = NUM_RX_BUFFERS;*/
	short pkt_len;
#if AMD8111E_VLAN_TAG_USED		
	short vtag;
#endif
	int rx_pkt_limit = dev->quota;
	
	do{   
		/* process receive packets until we use the quota*/
		/* If we own the next entry, it's a new packet. Send it up. */
		while(!(lp->rx_ring[rx_index].rx_flags & OWN_BIT)){
	       
			/* check if err summary bit is set */ 
			if(le16_to_cpu(lp->rx_ring[rx_index].rx_flags) 
								& ERR_BIT){
			/* 
			 * There is a tricky error noted by John Murphy,
			 * <murf@perftech.com> to Russ Nelson: Even with
			 * full-sized * buffers it's possible for a  
			 * jabber packet to use two buffers, with only 
			 * the last correctly noting the error.
			 */

			/* reseting flags */
			lp->rx_ring[rx_index].rx_flags &=RESET_RX_FLAGS;
			goto err_next_pkt;

			}
			/* check for STP and ENP */
		status = le16_to_cpu(lp->rx_ring[rx_index].rx_flags);
		if(!((status & STP_BIT) && (status & ENP_BIT))){
			/* reseting flags */
			lp->rx_ring[rx_index].rx_flags &=RESET_RX_FLAGS;
			goto err_next_pkt;
		}
		pkt_len = le16_to_cpu(lp->rx_ring[rx_index].msg_count) - 4;

#if AMD8111E_VLAN_TAG_USED		
		vtag = le16_to_cpu(lp->rx_ring[rx_index].rx_flags) & TT_MASK;
		/*MAC will strip vlan tag*/ 
		if(lp->vlgrp != NULL && vtag !=0)
			min_pkt_len =MIN_PKT_LEN - 4;
		else
#endif
			min_pkt_len =MIN_PKT_LEN;

		if (pkt_len < min_pkt_len) {
			lp->rx_ring[rx_index].rx_flags &= RESET_RX_FLAGS;
			lp->drv_rx_errors++;
			goto err_next_pkt;
		}
		if(--rx_pkt_limit < 0)
			goto rx_not_empty;
		if(!(new_skb = dev_alloc_skb(lp->rx_buff_len))){
			/* if allocation fail, 
				ignore that pkt and go to next one */
			lp->rx_ring[rx_index].rx_flags &= RESET_RX_FLAGS;
			lp->drv_rx_errors++;
			goto err_next_pkt;
		}
		
		skb_reserve(new_skb, 2);
		skb = lp->rx_skbuff[rx_index];
		pci_unmap_single(lp->pci_dev,lp->rx_dma_addr[rx_index],
			lp->rx_buff_len-2, PCI_DMA_FROMDEVICE);
		skb_put(skb, pkt_len);
		skb->dev = dev;
		lp->rx_skbuff[rx_index] = new_skb;
		new_skb->dev = dev;
		lp->rx_dma_addr[rx_index] = pci_map_single(lp->pci_dev,
			new_skb->data, lp->rx_buff_len-2,PCI_DMA_FROMDEVICE);
	
		skb->protocol = eth_type_trans(skb, dev);

#if AMD8111E_VLAN_TAG_USED		
		
		vtag = lp->rx_ring[rx_index].rx_flags & TT_MASK;
		if(lp->vlgrp != NULL && (vtag == TT_VLAN_TAGGED)){
			amd8111e_vlan_rx(lp, skb,
				    lp->rx_ring[rx_index].tag_ctrl_info);
		} else
#endif
			
			netif_receive_skb(skb);
		/*COAL update rx coalescing parameters*/
		lp->coal_conf.rx_packets++;
		lp->coal_conf.rx_bytes += pkt_len;	
		num_rx_pkt++;
		dev->last_rx = jiffies;
	
err_next_pkt:	
		lp->rx_ring[rx_index].buff_phy_addr
			 = cpu_to_le32(lp->rx_dma_addr[rx_index]);
		lp->rx_ring[rx_index].buff_count = 
				cpu_to_le16(lp->rx_buff_len-2);
		lp->rx_ring[rx_index].rx_flags |= cpu_to_le16(OWN_BIT);
		rx_index = (++lp->rx_idx) & RX_RING_DR_MOD_MASK;
	}
	/* Check the interrupt status register for more packets in the 
	mean time. Process them since we have not used up our quota.*/

	intr0 = readl(mmio + INT0);
	/*Ack receive packets */
	writel(intr0 & RINT0,mmio + INT0);

	}while(intr0 & RINT0);

	/* Receive descriptor is empty now */
	dev->quota -= num_rx_pkt;
	*budget -= num_rx_pkt;
	netif_rx_complete(dev);
	/* enable receive interrupt */
	writel(VAL0|RINTEN0, mmio + INTEN0);
	writel(VAL2 | RDMD0, mmio + CMD0);
	return 0;
rx_not_empty:
	/* Do not call a netif_rx_complete */
	dev->quota -= num_rx_pkt;	
	*budget -= num_rx_pkt;
	return 1;

	
}

#else
/* 
This function will check the ownership of receive buffers and descriptors. It will indicate to kernel up to half the number of maximum receive buffers in the descriptor ring, in a single receive interrupt. It will also replenish the descriptors with new skbs.
*/
static int amd8111e_rx(struct net_device *dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	struct sk_buff *skb,*new_skb;
	int rx_index = lp->rx_idx & RX_RING_DR_MOD_MASK;
	int min_pkt_len, status;
	int num_rx_pkt = 0;
	int max_rx_pkt = NUM_RX_BUFFERS;
	short pkt_len;
#if AMD8111E_VLAN_TAG_USED		
	short vtag;
#endif
	
	/* If we own the next entry, it's a new packet. Send it up. */
	while(++num_rx_pkt <= max_rx_pkt){
		if(lp->rx_ring[rx_index].rx_flags & OWN_BIT)
			return 0;
	       
		/* check if err summary bit is set */ 
		if(le16_to_cpu(lp->rx_ring[rx_index].rx_flags) & ERR_BIT){
			/* 
			 * There is a tricky error noted by John Murphy,
			 * <murf@perftech.com> to Russ Nelson: Even with full-sized
			 * buffers it's possible for a jabber packet to use two
			 * buffers, with only the last correctly noting the error.			 */
			/* reseting flags */
			lp->rx_ring[rx_index].rx_flags &= RESET_RX_FLAGS;
			goto err_next_pkt;
		}
		/* check for STP and ENP */
		status = le16_to_cpu(lp->rx_ring[rx_index].rx_flags);
		if(!((status & STP_BIT) && (status & ENP_BIT))){
			/* reseting flags */
			lp->rx_ring[rx_index].rx_flags &= RESET_RX_FLAGS;
			goto err_next_pkt;
		}
		pkt_len = le16_to_cpu(lp->rx_ring[rx_index].msg_count) - 4;

#if AMD8111E_VLAN_TAG_USED		
		vtag = le16_to_cpu(lp->rx_ring[rx_index].rx_flags) & TT_MASK;
		/*MAC will strip vlan tag*/ 
		if(lp->vlgrp != NULL && vtag !=0)
			min_pkt_len =MIN_PKT_LEN - 4;
		else
#endif
			min_pkt_len =MIN_PKT_LEN;

		if (pkt_len < min_pkt_len) {
			lp->rx_ring[rx_index].rx_flags &= RESET_RX_FLAGS;
			lp->drv_rx_errors++;
			goto err_next_pkt;
		}
		if(!(new_skb = dev_alloc_skb(lp->rx_buff_len))){
			/* if allocation fail, 
				ignore that pkt and go to next one */
			lp->rx_ring[rx_index].rx_flags &= RESET_RX_FLAGS;
			lp->drv_rx_errors++;
			goto err_next_pkt;
		}
		
		skb_reserve(new_skb, 2);
		skb = lp->rx_skbuff[rx_index];
		pci_unmap_single(lp->pci_dev,lp->rx_dma_addr[rx_index],
			lp->rx_buff_len-2, PCI_DMA_FROMDEVICE);
		skb_put(skb, pkt_len);
		skb->dev = dev;
		lp->rx_skbuff[rx_index] = new_skb;
		new_skb->dev = dev;
		lp->rx_dma_addr[rx_index] = pci_map_single(lp->pci_dev,
			new_skb->data, lp->rx_buff_len-2,PCI_DMA_FROMDEVICE);
	
		skb->protocol = eth_type_trans(skb, dev);

#if AMD8111E_VLAN_TAG_USED		
		
		vtag = lp->rx_ring[rx_index].rx_flags & TT_MASK;
		if(lp->vlgrp != NULL && (vtag == TT_VLAN_TAGGED)){
			amd8111e_vlan_rx(lp, skb,
				    lp->rx_ring[rx_index].tag_ctrl_info);
		} else
#endif
			
			netif_rx (skb);
			/*COAL update rx coalescing parameters*/
			lp->coal_conf.rx_packets++;
			lp->coal_conf.rx_bytes += pkt_len;	

			dev->last_rx = jiffies;
	
err_next_pkt:
		lp->rx_ring[rx_index].buff_phy_addr
			 = cpu_to_le32(lp->rx_dma_addr[rx_index]);
		lp->rx_ring[rx_index].buff_count = 
				cpu_to_le16(lp->rx_buff_len-2);
		lp->rx_ring[rx_index].rx_flags |= cpu_to_le16(OWN_BIT);
		rx_index = (++lp->rx_idx) & RX_RING_DR_MOD_MASK;
	}

	return 0;
}
#endif /* CONFIG_AMD8111E_NAPI */
/* 
This function will indicate the link status to the kernel.
*/
static int amd8111e_link_change(struct net_device* dev)
{	
	struct amd8111e_priv *lp = netdev_priv(dev);
	int status0,speed;

	/* read the link change */
     	status0 = readl(lp->mmio + STAT0);
	
	if(status0 & LINK_STATS){
		if(status0 & AUTONEG_COMPLETE)
			lp->link_config.autoneg = AUTONEG_ENABLE;
		else 
			lp->link_config.autoneg = AUTONEG_DISABLE;

		if(status0 & FULL_DPLX)
			lp->link_config.duplex = DUPLEX_FULL;
		else 
			lp->link_config.duplex = DUPLEX_HALF;
		speed = (status0 & SPEED_MASK) >> 7;
		if(speed == PHY_SPEED_10)
			lp->link_config.speed = SPEED_10;
		else if(speed == PHY_SPEED_100)
			lp->link_config.speed = SPEED_100;

		printk(KERN_INFO "%s: Link is Up. Speed is %s Mbps %s Duplex\n",			dev->name,
		       (lp->link_config.speed == SPEED_100) ? "100": "10", 
		       (lp->link_config.duplex == DUPLEX_FULL)? "Full": "Half"); 
		netif_carrier_on(dev);
	}
	else{	
		lp->link_config.speed = SPEED_INVALID;
		lp->link_config.duplex = DUPLEX_INVALID;
		lp->link_config.autoneg = AUTONEG_INVALID;
		printk(KERN_INFO "%s: Link is Down.\n",dev->name);
		netif_carrier_off(dev);
	}
		
	return 0;
}
/*
This function reads the mib counters. 	 
*/
static int amd8111e_read_mib(void* mmio, u8 MIB_COUNTER)
{
	unsigned int  status;
	unsigned  int data;
	unsigned int repeat = REPEAT_CNT;

	writew( MIB_RD_CMD | MIB_COUNTER, mmio + MIB_ADDR);
	do {
		status = readw(mmio + MIB_ADDR);
		udelay(2);	/* controller takes MAX 2 us to get mib data */
	}
	while (--repeat && (status & MIB_CMD_ACTIVE));

	data = readl(mmio + MIB_DATA);
	return data;
}

/*
This function reads the mib registers and returns the hardware statistics. It  updates previous internal driver statistics with new values.
*/ 
static struct net_device_stats *amd8111e_get_stats(struct net_device * dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	void * mmio = lp->mmio;
	unsigned long flags;
	/* struct net_device_stats *prev_stats = &lp->prev_stats; */
	struct net_device_stats* new_stats = &lp->stats;
	
	if(!lp->opened)
		return &lp->stats;	
	spin_lock_irqsave (&lp->lock, flags);

	/* stats.rx_packets */
	new_stats->rx_packets = amd8111e_read_mib(mmio, rcv_broadcast_pkts)+
				amd8111e_read_mib(mmio, rcv_multicast_pkts)+
				amd8111e_read_mib(mmio, rcv_unicast_pkts);

	/* stats.tx_packets */
	new_stats->tx_packets = amd8111e_read_mib(mmio, xmt_packets);

	/*stats.rx_bytes */
	new_stats->rx_bytes = amd8111e_read_mib(mmio, rcv_octets);

	/* stats.tx_bytes */
	new_stats->tx_bytes = amd8111e_read_mib(mmio, xmt_octets);

	/* stats.rx_errors */
	/* hw errors + errors driver reported */
	new_stats->rx_errors = amd8111e_read_mib(mmio, rcv_undersize_pkts)+
				amd8111e_read_mib(mmio, rcv_fragments)+
				amd8111e_read_mib(mmio, rcv_jabbers)+
				amd8111e_read_mib(mmio, rcv_alignment_errors)+
				amd8111e_read_mib(mmio, rcv_fcs_errors)+
				amd8111e_read_mib(mmio, rcv_miss_pkts)+
				lp->drv_rx_errors;

	/* stats.tx_errors */
	new_stats->tx_errors = amd8111e_read_mib(mmio, xmt_underrun_pkts);

	/* stats.rx_dropped*/
	new_stats->rx_dropped = amd8111e_read_mib(mmio, rcv_miss_pkts);

	/* stats.tx_dropped*/
	new_stats->tx_dropped = amd8111e_read_mib(mmio,  xmt_underrun_pkts);

	/* stats.multicast*/
	new_stats->multicast = amd8111e_read_mib(mmio, rcv_multicast_pkts);

	/* stats.collisions*/
	new_stats->collisions = amd8111e_read_mib(mmio, xmt_collisions);

	/* stats.rx_length_errors*/
	new_stats->rx_length_errors = 
		amd8111e_read_mib(mmio, rcv_undersize_pkts)+
		amd8111e_read_mib(mmio, rcv_oversize_pkts);

	/* stats.rx_over_errors*/
	new_stats->rx_over_errors = amd8111e_read_mib(mmio, rcv_miss_pkts);

	/* stats.rx_crc_errors*/
	new_stats->rx_crc_errors = amd8111e_read_mib(mmio, rcv_fcs_errors);

	/* stats.rx_frame_errors*/
	new_stats->rx_frame_errors =
		amd8111e_read_mib(mmio, rcv_alignment_errors);

	/* stats.rx_fifo_errors */
	new_stats->rx_fifo_errors = amd8111e_read_mib(mmio, rcv_miss_pkts);

	/* stats.rx_missed_errors */
	new_stats->rx_missed_errors = amd8111e_read_mib(mmio, rcv_miss_pkts);

	/* stats.tx_aborted_errors*/
	new_stats->tx_aborted_errors = 
		amd8111e_read_mib(mmio, xmt_excessive_collision);

	/* stats.tx_carrier_errors*/
	new_stats->tx_carrier_errors = 
		amd8111e_read_mib(mmio, xmt_loss_carrier);

	/* stats.tx_fifo_errors*/
	new_stats->tx_fifo_errors = amd8111e_read_mib(mmio, xmt_underrun_pkts);

	/* stats.tx_window_errors*/
	new_stats->tx_window_errors =
		amd8111e_read_mib(mmio, xmt_late_collision);

	/* Reset the mibs for collecting new statistics */
	/* writew(MIB_CLEAR, mmio + MIB_ADDR);*/
		
	spin_unlock_irqrestore (&lp->lock, flags);

	return new_stats;
}
/* This function recalculate the interupt coalescing  mode on every interrupt 
according to the datarate and the packet rate.
*/
static int amd8111e_calc_coalesce(struct net_device *dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	struct amd8111e_coalesce_conf * coal_conf = &lp->coal_conf;
	int tx_pkt_rate;
	int rx_pkt_rate;
	int tx_data_rate;
	int rx_data_rate;
	int rx_pkt_size;
	int tx_pkt_size;

	tx_pkt_rate = coal_conf->tx_packets - coal_conf->tx_prev_packets;
	coal_conf->tx_prev_packets =  coal_conf->tx_packets;
	
	tx_data_rate = coal_conf->tx_bytes - coal_conf->tx_prev_bytes;
	coal_conf->tx_prev_bytes =  coal_conf->tx_bytes;
	
	rx_pkt_rate = coal_conf->rx_packets - coal_conf->rx_prev_packets;
	coal_conf->rx_prev_packets =  coal_conf->rx_packets;
	
	rx_data_rate = coal_conf->rx_bytes - coal_conf->rx_prev_bytes;
	coal_conf->rx_prev_bytes =  coal_conf->rx_bytes;
	
	if(rx_pkt_rate < 800){
		if(coal_conf->rx_coal_type != NO_COALESCE){
			
			coal_conf->rx_timeout = 0x0;
			coal_conf->rx_event_count = 0;
			amd8111e_set_coalesce(dev,RX_INTR_COAL);
			coal_conf->rx_coal_type = NO_COALESCE;
		}
	}
	else{
	
		rx_pkt_size = rx_data_rate/rx_pkt_rate;
		if (rx_pkt_size < 128){
			if(coal_conf->rx_coal_type != NO_COALESCE){
			
				coal_conf->rx_timeout = 0;
				coal_conf->rx_event_count = 0;
				amd8111e_set_coalesce(dev,RX_INTR_COAL);
				coal_conf->rx_coal_type = NO_COALESCE;
			}

		}
		else if ( (rx_pkt_size >= 128) && (rx_pkt_size < 512) ){
	
			if(coal_conf->rx_coal_type !=  LOW_COALESCE){
				coal_conf->rx_timeout = 1;
				coal_conf->rx_event_count = 4;
				amd8111e_set_coalesce(dev,RX_INTR_COAL);
				coal_conf->rx_coal_type = LOW_COALESCE;
			}
		}
		else if ((rx_pkt_size >= 512) && (rx_pkt_size < 1024)){
			
			if(coal_conf->rx_coal_type !=  MEDIUM_COALESCE){
				coal_conf->rx_timeout = 1;
				coal_conf->rx_event_count = 4;
				amd8111e_set_coalesce(dev,RX_INTR_COAL);
				coal_conf->rx_coal_type = MEDIUM_COALESCE;
			}		
				
		}
		else if(rx_pkt_size >= 1024){
			if(coal_conf->rx_coal_type !=  HIGH_COALESCE){
				coal_conf->rx_timeout = 2;
				coal_conf->rx_event_count = 3;
				amd8111e_set_coalesce(dev,RX_INTR_COAL);
				coal_conf->rx_coal_type = HIGH_COALESCE;
			}		
		}
	}
    	/* NOW FOR TX INTR COALESC */
	if(tx_pkt_rate < 800){
		if(coal_conf->tx_coal_type != NO_COALESCE){
			
			coal_conf->tx_timeout = 0x0;
			coal_conf->tx_event_count = 0;
			amd8111e_set_coalesce(dev,TX_INTR_COAL);
			coal_conf->tx_coal_type = NO_COALESCE;
		}
	}
	else{
	
		tx_pkt_size = tx_data_rate/tx_pkt_rate;
		if (tx_pkt_size < 128){
		
			if(coal_conf->tx_coal_type != NO_COALESCE){
			
				coal_conf->tx_timeout = 0;
				coal_conf->tx_event_count = 0;
				amd8111e_set_coalesce(dev,TX_INTR_COAL);
				coal_conf->tx_coal_type = NO_COALESCE;
			}

		}
		else if ( (tx_pkt_size >= 128) && (tx_pkt_size < 512) ){
	
			if(coal_conf->tx_coal_type !=  LOW_COALESCE){
				coal_conf->tx_timeout = 1;
				coal_conf->tx_event_count = 2;
				amd8111e_set_coalesce(dev,TX_INTR_COAL);
				coal_conf->tx_coal_type = LOW_COALESCE;

			}
		}
		else if ((tx_pkt_size >= 512) && (tx_pkt_size < 1024)){
			
			if(coal_conf->tx_coal_type !=  MEDIUM_COALESCE){
				coal_conf->tx_timeout = 2;
				coal_conf->tx_event_count = 5;
				amd8111e_set_coalesce(dev,TX_INTR_COAL);
				coal_conf->tx_coal_type = MEDIUM_COALESCE;
			}		
				
		}
		else if(tx_pkt_size >= 1024){
			if (tx_pkt_size >= 1024){
				if(coal_conf->tx_coal_type !=  HIGH_COALESCE){
					coal_conf->tx_timeout = 4;
					coal_conf->tx_event_count = 8;
					amd8111e_set_coalesce(dev,TX_INTR_COAL);
					coal_conf->tx_coal_type = HIGH_COALESCE;
				}		
			}
		}
	}
	return 0;

}
/*
This is device interrupt function. It handles transmit, receive,link change and hardware timer interrupts.
*/
static irqreturn_t amd8111e_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{

	struct net_device * dev = (struct net_device *) dev_id;
	struct amd8111e_priv *lp = netdev_priv(dev);
	void * mmio = lp->mmio;
	unsigned int intr0;
	unsigned int handled = 1;

	if(dev == NULL)
		return IRQ_NONE;

	if (regs) spin_lock (&lp->lock);
	/* disabling interrupt */
	writel(INTREN, mmio + CMD0);

	/* Read interrupt status */
	intr0 = readl(mmio + INT0);

	/* Process all the INT event until INTR bit is clear. */

	if (!(intr0 & INTR)){
		handled = 0;
		goto err_no_interrupt;
	}
		 
	/* Current driver processes 4 interrupts : RINT,TINT,LCINT,STINT */
	writel(intr0, mmio + INT0);

	/* Check if Receive Interrupt has occurred. */
#if CONFIG_AMD8111E_NAPI
	if(intr0 & RINT0){
		if(netif_rx_schedule_prep(dev)){
			/* Disable receive interupts */
			writel(RINTEN0, mmio + INTEN0);
			/* Schedule a polling routine */
			__netif_rx_schedule(dev);
		}
		else {
			printk("************Driver bug! \
				interrupt while in poll\n");
			/* Fix by disabling interrupts */
			writel(RINT0, mmio + INT0);
		}
	}
#else
	if(intr0 & RINT0){
		amd8111e_rx(dev);
		writel(VAL2 | RDMD0, mmio + CMD0);
	}
#endif /* CONFIG_AMD8111E_NAPI */
	/* Check if  Transmit Interrupt has occurred. */
	if(intr0 & TINT0)
		amd8111e_tx(dev);
		
	/* Check if  Link Change Interrupt has occurred. */
	if (intr0 & LCINT)
		amd8111e_link_change(dev);

	/* Check if Hardware Timer Interrupt has occurred. */
	if (intr0 & STINT)
		amd8111e_calc_coalesce(dev);

err_no_interrupt:
	writel( VAL0 | INTREN,mmio + CMD0);
	
	if (regs) spin_unlock(&lp->lock);
	
	return IRQ_RETVAL(handled);
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void amd8111e_poll(struct net_device *dev)
{ 
	unsigned long flags;
	local_save_flags(flags); 
	local_irq_disable();
	amd8111e_interrupt(0, dev, NULL);
	local_irq_restore(flags); 
} 
#endif


/*
This function closes the network interface and updates the statistics so that most recent statistics will be available after the interface is down.
*/
static int amd8111e_close(struct net_device * dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	netif_stop_queue(dev);
	
	spin_lock_irq(&lp->lock);
	
	amd8111e_disable_interrupt(lp);
	amd8111e_stop_chip(lp);
	amd8111e_free_ring(lp);
	
	netif_carrier_off(lp->amd8111e_net_dev);

	/* Delete ipg timer */
	if(lp->options & OPTION_DYN_IPG_ENABLE)	        
		del_timer_sync(&lp->ipg_data.ipg_timer);

	spin_unlock_irq(&lp->lock);
	free_irq(dev->irq, dev);
	
	/* Update the statistics before closing */
	amd8111e_get_stats(dev);
	lp->opened = 0;
	return 0;
}
/* This function opens new interface.It requests irq for the device, initializes the device,buffers and descriptors, and starts the device. 
*/
static int amd8111e_open(struct net_device * dev )
{
	struct amd8111e_priv *lp = netdev_priv(dev);

	if(dev->irq ==0 || request_irq(dev->irq, amd8111e_interrupt, SA_SHIRQ,
					 dev->name, dev)) 
		return -EAGAIN;

	spin_lock_irq(&lp->lock);

	amd8111e_init_hw_default(lp);

	if(amd8111e_restart(dev)){
		spin_unlock_irq(&lp->lock);
		return -ENOMEM;
	}
	/* Start ipg timer */
	if(lp->options & OPTION_DYN_IPG_ENABLE){	        
		add_timer(&lp->ipg_data.ipg_timer);
		printk(KERN_INFO "%s: Dynamic IPG Enabled.\n",dev->name);
	}

	lp->opened = 1;

	spin_unlock_irq(&lp->lock);

	netif_start_queue(dev);

	return 0;		
}
/* 
This function checks if there is any transmit  descriptors available to queue more packet.
*/
static int amd8111e_tx_queue_avail(struct amd8111e_priv* lp )
{	
	int tx_index = lp->tx_idx & TX_BUFF_MOD_MASK;
	if(lp->tx_skbuff[tx_index] != 0)
		return -1;
	else
		return 0;
	
}
/* 
This function will queue the transmit packets to the descriptors and will trigger the send operation. It also initializes the transmit descriptors with buffer physical address, byte count, ownership to hardware etc.
*/

static int amd8111e_start_xmit(struct sk_buff *skb, struct net_device * dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	int tx_index;
	unsigned long flags;

	spin_lock_irqsave(&lp->lock, flags);

	tx_index = lp->tx_idx & TX_RING_DR_MOD_MASK;

	lp->tx_ring[tx_index].buff_count = cpu_to_le16(skb->len);

	lp->tx_skbuff[tx_index] = skb;
	lp->tx_ring[tx_index].tx_flags = 0;

#if AMD8111E_VLAN_TAG_USED
	if((lp->vlgrp != NULL) && vlan_tx_tag_present(skb)){
		lp->tx_ring[tx_index].tag_ctrl_cmd |= 
				cpu_to_le32(TCC_VLAN_INSERT);	
		lp->tx_ring[tx_index].tag_ctrl_info = 
				cpu_to_le16(vlan_tx_tag_get(skb));

	}
#endif
	lp->tx_dma_addr[tx_index] =
	    pci_map_single(lp->pci_dev, skb->data, skb->len, PCI_DMA_TODEVICE);
	lp->tx_ring[tx_index].buff_phy_addr =
	    (u32) cpu_to_le32(lp->tx_dma_addr[tx_index]);

	/*  Set FCS and LTINT bits */
	lp->tx_ring[tx_index].tx_flags |=
	    cpu_to_le16(OWN_BIT | STP_BIT | ENP_BIT|ADD_FCS_BIT|LTINT_BIT);

	lp->tx_idx++;

	/* Trigger an immediate send poll. */
	writel( VAL1 | TDMD0, lp->mmio + CMD0);
	writel( VAL2 | RDMD0,lp->mmio + CMD0);

	dev->trans_start = jiffies;

	if(amd8111e_tx_queue_avail(lp) < 0){
		netif_stop_queue(dev);
	}
	spin_unlock_irqrestore(&lp->lock, flags);
	return 0;
}
/*
This function returns all the memory mapped registers of the device.
*/
static char* amd8111e_read_regs(struct amd8111e_priv* lp)
{    	
	void * mmio = lp->mmio;
        u32 * reg_buff;

     	reg_buff = kmalloc( AMD8111E_REG_DUMP_LEN,GFP_KERNEL);
	if(NULL == reg_buff)
		return NULL;

	/* Read only necessary registers */
	reg_buff[0] = readl(mmio + XMT_RING_BASE_ADDR0);
	reg_buff[1] = readl(mmio + XMT_RING_LEN0);
	reg_buff[2] = readl(mmio + RCV_RING_BASE_ADDR0);
	reg_buff[3] = readl(mmio + RCV_RING_LEN0);
	reg_buff[4] = readl(mmio + CMD0);
	reg_buff[5] = readl(mmio + CMD2);
	reg_buff[6] = readl(mmio + CMD3);
	reg_buff[7] = readl(mmio + CMD7);
	reg_buff[8] = readl(mmio + INT0);
	reg_buff[9] = readl(mmio + INTEN0);
	reg_buff[10] = readl(mmio + LADRF);
	reg_buff[11] = readl(mmio + LADRF+4);
	reg_buff[12] = readl(mmio + STAT0);

	return (char *)reg_buff;
}
/*
amd8111e crc generator implementation is different from the kernel
ether_crc() function.
*/
int amd8111e_ether_crc(int len, char* mac_addr)
{
	int i,byte;
	unsigned char octet;
	u32 crc= INITCRC;

	for(byte=0; byte < len; byte++){
		octet = mac_addr[byte];
		for( i=0;i < 8; i++){
			/*If the next bit form the input stream is 1,subtract				 the divisor (CRC32) from the dividend(crc).*/
			if( (octet & 0x1) ^ (crc & 0x1) ){
				crc >>= 1;
				crc ^= CRC32;
			}
			else
				crc >>= 1;
			
			octet >>= 1;
		}
	}	
	return crc; 
}
/*
This function sets promiscuos mode, all-multi mode or the multicast address 
list to the device.
*/
static void amd8111e_set_multicast_list(struct net_device *dev)
{
	struct dev_mc_list* mc_ptr;
	struct amd8111e_priv *lp = netdev_priv(dev);
	u32 mc_filter[2] ;
	int i,bit_num;
	if(dev->flags & IFF_PROMISC){
		printk(KERN_INFO "%s: Setting  promiscuous mode.\n",dev->name);
		writel( VAL2 | PROM, lp->mmio + CMD2);
		return;
	}
	else
		writel( PROM, lp->mmio + CMD2);
	if(dev->flags & IFF_ALLMULTI || dev->mc_count > MAX_FILTER_SIZE){
		/* get all multicast packet */
		mc_filter[1] = mc_filter[0] = 0xffffffff;
		lp->mc_list = dev->mc_list;
		lp->options |= OPTION_MULTICAST_ENABLE;
		amd8111e_writeq(*(u64*)mc_filter,lp->mmio + LADRF);
		return;
	}
	if( dev->mc_count == 0 ){
		/* get only own packets */
		mc_filter[1] = mc_filter[0] = 0;
		lp->mc_list = NULL;
		lp->options &= ~OPTION_MULTICAST_ENABLE;
		amd8111e_writeq(*(u64*)mc_filter,lp->mmio + LADRF);
		/* disable promiscous mode */
		writel(PROM, lp->mmio + CMD2);
		return;
	}
	/* load all the multicast addresses in the logic filter */
	lp->options |= OPTION_MULTICAST_ENABLE;
	lp->mc_list = dev->mc_list;
	mc_filter[1] = mc_filter[0] = 0;
	for (i = 0, mc_ptr = dev->mc_list; mc_ptr && i < dev->mc_count;
		     i++, mc_ptr = mc_ptr->next) {
		bit_num = ( amd8111e_ether_crc(ETH_ALEN,mc_ptr->dmi_addr)							 >> 26 ) & 0x3f;
		mc_filter[bit_num >> 5] |= 1 << (bit_num & 31);
	}	
	amd8111e_writeq(*(u64*)mc_filter,lp->mmio+ LADRF);

	/* To eliminate PCI posting bug */
	readl(lp->mmio + CMD2);

}

/*
This function handles all the  ethtool ioctls. It gives driver info, gets/sets driver speed, gets memory mapped register values, forces auto negotiation, sets/gets WOL options for ethtool application. 
*/
	
static int amd8111e_ethtool_ioctl(struct net_device* dev, void __user *useraddr)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	struct pci_dev *pci_dev = lp->pci_dev;
	u32 ethcmd;
	
	if( useraddr == NULL) 
		return -EINVAL;
	if(copy_from_user (&ethcmd, useraddr, sizeof (ethcmd)))
		return -EFAULT;
	
	switch(ethcmd){
	
	case ETHTOOL_GDRVINFO:{
		struct ethtool_drvinfo info = { ETHTOOL_GDRVINFO };
		strcpy (info.driver, MODULE_NAME);
		strcpy (info.version, MODULE_VERS);
		memset(&info.fw_version, 0, sizeof(info.fw_version));
		sprintf(info.fw_version,"%u",chip_version);
		strcpy (info.bus_info, pci_name(pci_dev));
		info.eedump_len = 0;
		info.regdump_len = AMD8111E_REG_DUMP_LEN;
		if (copy_to_user (useraddr, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	/* get settings */
	case ETHTOOL_GSET: {
		struct ethtool_cmd ecmd = { ETHTOOL_GSET };
		spin_lock_irq(&lp->lock);
		mii_ethtool_gset(&lp->mii_if, &ecmd);
		spin_unlock_irq(&lp->lock);
		if (copy_to_user(useraddr, &ecmd, sizeof(ecmd)))
			return -EFAULT;
		return 0;
	}
	/* set settings */
	case ETHTOOL_SSET: {
		int r;
		struct ethtool_cmd ecmd;
		if (copy_from_user(&ecmd, useraddr, sizeof(ecmd)))
			return -EFAULT;

		spin_lock_irq(&lp->lock);
		r = mii_ethtool_sset(&lp->mii_if, &ecmd);
		spin_unlock_irq(&lp->lock);
		return r;
	}
	case ETHTOOL_GREGS: {
		struct ethtool_regs regs;
		u8 *regbuf;
		int ret;

		if (copy_from_user(&regs, useraddr, sizeof(regs)))
			return -EFAULT;
		if (regs.len > AMD8111E_REG_DUMP_LEN)
			regs.len = AMD8111E_REG_DUMP_LEN;
		regs.version = 0;
		if (copy_to_user(useraddr, &regs, sizeof(regs)))
			return -EFAULT;

		regbuf = amd8111e_read_regs(lp);
		if (!regbuf)
			return -ENOMEM;

		useraddr += offsetof(struct ethtool_regs, data);
		ret = 0;
		if (copy_to_user(useraddr, regbuf, regs.len))
			ret = -EFAULT;
		kfree(regbuf);
		return ret;
	}
	/* restart autonegotiation */
	case ETHTOOL_NWAY_RST: {
		return mii_nway_restart(&lp->mii_if);
	}
	/* get link status */
	case ETHTOOL_GLINK: {
		struct ethtool_value val = {ETHTOOL_GLINK};
		val.data = mii_link_ok(&lp->mii_if);
		if (copy_to_user(useraddr, &val, sizeof(val)))
			return -EFAULT;
		return 0;
	}
	case ETHTOOL_GWOL: {
		struct ethtool_wolinfo wol_info = { ETHTOOL_GWOL };

		wol_info.supported = WAKE_MAGIC|WAKE_PHY;
		wol_info.wolopts = 0;
		if (lp->options & OPTION_WOL_ENABLE)
			wol_info.wolopts = WAKE_MAGIC;
		memset(&wol_info.sopass, 0, sizeof(wol_info.sopass));
		if (copy_to_user(useraddr, &wol_info, sizeof(wol_info)))
			return -EFAULT;
		return 0;
	}
	case ETHTOOL_SWOL: {
		struct ethtool_wolinfo wol_info;

		if (copy_from_user(&wol_info, useraddr, sizeof(wol_info)))
			return -EFAULT;
		if (wol_info.wolopts & ~(WAKE_MAGIC |WAKE_PHY))
			return -EINVAL;
		spin_lock_irq(&lp->lock);
		if(wol_info.wolopts & WAKE_MAGIC)
			lp->options |= 
				(OPTION_WOL_ENABLE | OPTION_WAKE_MAGIC_ENABLE);
		else if(wol_info.wolopts & WAKE_PHY)
			lp->options |= 
				(OPTION_WOL_ENABLE | OPTION_WAKE_PHY_ENABLE);
		else
			lp->options &= ~OPTION_WOL_ENABLE; 
		spin_unlock_irq(&lp->lock);
		return 0;
	}
	
	default:
		break;
	}
		return -EOPNOTSUPP;
}
static int amd8111e_ioctl(struct net_device * dev , struct ifreq *ifr, int cmd)
{
	struct mii_ioctl_data *data = if_mii(ifr);
	struct amd8111e_priv *lp = netdev_priv(dev);
	int err;
	u32 mii_regval;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	switch(cmd) {
	case SIOCETHTOOL:
		return amd8111e_ethtool_ioctl(dev, ifr->ifr_data);
	case SIOCGMIIPHY:
		data->phy_id = PHY_ID;

	/* fallthru */
	case SIOCGMIIREG: 

		spin_lock_irq(&lp->lock);
		err = amd8111e_read_phy(lp, data->phy_id,
			data->reg_num & PHY_REG_ADDR_MASK, &mii_regval);
		spin_unlock_irq(&lp->lock);

		data->val_out = mii_regval;
		return err;

	case SIOCSMIIREG:

		spin_lock_irq(&lp->lock);
		err = amd8111e_write_phy(lp, data->phy_id,
			data->reg_num & PHY_REG_ADDR_MASK, data->val_in);
		spin_unlock_irq(&lp->lock);

		return err;

	default:
		/* do nothing */
		break;
	}
	return -EOPNOTSUPP;
}
static int amd8111e_set_mac_address(struct net_device *dev, void *p)
{
	struct amd8111e_priv *lp = dev->priv;
	int i;
	struct sockaddr *addr = p;

	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	spin_lock_irq(&lp->lock);
	/* Setting the MAC address to the device */
	for(i = 0; i < ETH_ADDR_LEN; i++)
		writeb( dev->dev_addr[i], lp->mmio + PADR + i ); 
		
	spin_unlock_irq(&lp->lock);

	return 0;
}

/* 
This function changes the mtu of the device. It restarts the device  to initialize the descriptor with new receive buffers.
*/  
int amd8111e_change_mtu(struct net_device *dev, int new_mtu)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	int err;

	if ((new_mtu < AMD8111E_MIN_MTU) || (new_mtu > AMD8111E_MAX_MTU))
		return -EINVAL;

	if (!netif_running(dev)) {
		/* new_mtu will be used
		   when device starts netxt time */ 
		dev->mtu = new_mtu;
		return 0;
	}

	spin_lock_irq(&lp->lock);

        /* stop the chip */
	writel(RUN, lp->mmio + CMD0);

	dev->mtu = new_mtu;

	err = amd8111e_restart(dev);
	spin_unlock_irq(&lp->lock);
	if(!err)
		netif_start_queue(dev);
	return err;
}

#if AMD8111E_VLAN_TAG_USED
static void amd8111e_vlan_rx_register(struct net_device *dev, struct vlan_group *grp)
{
	struct  amd8111e_priv *lp = netdev_priv(dev);
	spin_lock_irq(&lp->lock);
	lp->vlgrp = grp;
	spin_unlock_irq(&lp->lock);
}
	
static void amd8111e_vlan_rx_kill_vid(struct net_device *dev, unsigned short vid)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	spin_lock_irq(&lp->lock);
	if (lp->vlgrp)
		lp->vlgrp->vlan_devices[vid] = NULL;
	spin_unlock_irq(&lp->lock);
}
#endif
static int amd8111e_enable_magicpkt(struct amd8111e_priv* lp)
{
	writel( VAL1|MPPLBA, lp->mmio + CMD3);
	writel( VAL0|MPEN_SW, lp->mmio + CMD7);

	/* To eliminate PCI posting bug */
	readl(lp->mmio + CMD7);
	return 0;
}

static int amd8111e_enable_link_change(struct amd8111e_priv* lp)
{

	/* Adapter is already stoped/suspended/interrupt-disabled */
	writel(VAL0|LCMODE_SW,lp->mmio + CMD7);
	
	/* To eliminate PCI posting bug */
	readl(lp->mmio + CMD7);
	return 0;
}	
/* This function is called when a packet transmission fails to complete within a  resonable period, on the assumption that an interrupts have been failed or the  interface is locked up. This function will reinitialize the hardware */

static void amd8111e_tx_timeout(struct net_device *dev)
{
	struct amd8111e_priv* lp = netdev_priv(dev);
	int err;

	printk(KERN_ERR "%s: transmit timed out, resetting\n",
	 					      dev->name);
	spin_lock_irq(&lp->lock);
	err = amd8111e_restart(dev);
	spin_unlock_irq(&lp->lock);
	if(!err)
		netif_wake_queue(dev);
}
static int amd8111e_suspend(struct pci_dev *pci_dev, u32 state)
{	
	struct net_device *dev = pci_get_drvdata(pci_dev);
	struct amd8111e_priv *lp = netdev_priv(dev);
	
	if (!netif_running(dev))
		return 0;

	/* disable the interrupt */
	spin_lock_irq(&lp->lock);
	amd8111e_disable_interrupt(lp);
	spin_unlock_irq(&lp->lock);

	netif_device_detach(dev);
	
	/* stop chip */
	spin_lock_irq(&lp->lock);
	if(lp->options & OPTION_DYN_IPG_ENABLE)	        
		del_timer_sync(&lp->ipg_data.ipg_timer);
	amd8111e_stop_chip(lp);
	spin_unlock_irq(&lp->lock);

	if(lp->options & OPTION_WOL_ENABLE){
		 /* enable wol */
		if(lp->options & OPTION_WAKE_MAGIC_ENABLE)
			amd8111e_enable_magicpkt(lp);	
		if(lp->options & OPTION_WAKE_PHY_ENABLE)
			amd8111e_enable_link_change(lp);	
		
		pci_enable_wake(pci_dev, 3, 1);
		pci_enable_wake(pci_dev, 4, 1); /* D3 cold */

	}
	else{		
		pci_enable_wake(pci_dev, 3, 0);
		pci_enable_wake(pci_dev, 4, 0); /* 4 == D3 cold */
	}
	
	pci_save_state(pci_dev, lp->pm_state);
	pci_set_power_state(pci_dev, 3);

	return 0;
}
static int amd8111e_resume(struct pci_dev *pci_dev)
{
	struct net_device *dev = pci_get_drvdata(pci_dev);
	struct amd8111e_priv *lp = netdev_priv(dev);
	
	if (!netif_running(dev))
		return 0;

	pci_set_power_state(pci_dev, 0);
	pci_restore_state(pci_dev, lp->pm_state);

	pci_enable_wake(pci_dev, 3, 0);
	pci_enable_wake(pci_dev, 4, 0); /* D3 cold */

	netif_device_attach(dev);

	spin_lock_irq(&lp->lock);
	amd8111e_restart(dev);
	/* Restart ipg timer */
	if(lp->options & OPTION_DYN_IPG_ENABLE)	        
		mod_timer(&lp->ipg_data.ipg_timer, 
				jiffies + IPG_CONVERGE_JIFFIES);
	spin_unlock_irq(&lp->lock);

	return 0;
}


static void __devexit amd8111e_remove_one(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	if (dev) {
		unregister_netdev(dev);
		iounmap((void *) ((struct amd8111e_priv *)(dev->priv))->mmio);
		free_netdev(dev);
		pci_release_regions(pdev);
		pci_disable_device(pdev);
		pci_set_drvdata(pdev, NULL);
	}
}
static void amd8111e_config_ipg(struct net_device* dev)
{
	struct amd8111e_priv *lp = netdev_priv(dev);
	struct ipg_info* ipg_data = &lp->ipg_data;
	void * mmio = lp->mmio;
	unsigned int prev_col_cnt = ipg_data->col_cnt;
	unsigned int total_col_cnt;
	unsigned int tmp_ipg;
	
	if(lp->link_config.duplex == DUPLEX_FULL){
		ipg_data->ipg = DEFAULT_IPG;
		return;
	}

	if(ipg_data->ipg_state == SSTATE){
		
		if(ipg_data->timer_tick == IPG_STABLE_TIME){
			
			ipg_data->timer_tick = 0;
			ipg_data->ipg = MIN_IPG - IPG_STEP;
			ipg_data->current_ipg = MIN_IPG;
			ipg_data->diff_col_cnt = 0xFFFFFFFF;
			ipg_data->ipg_state = CSTATE;
		}
		else
			ipg_data->timer_tick++;
	}

	if(ipg_data->ipg_state == CSTATE){
		
		/* Get the current collision count */

		total_col_cnt = ipg_data->col_cnt = 
				amd8111e_read_mib(mmio, xmt_collisions);

		if ((total_col_cnt - prev_col_cnt) < 
				(ipg_data->diff_col_cnt)){
			
			ipg_data->diff_col_cnt =
				total_col_cnt - prev_col_cnt ;

			ipg_data->ipg = ipg_data->current_ipg;
		}

		ipg_data->current_ipg += IPG_STEP;

		if (ipg_data->current_ipg <= MAX_IPG)
			tmp_ipg = ipg_data->current_ipg;
		else{
			tmp_ipg = ipg_data->ipg;
			ipg_data->ipg_state = SSTATE;
		}
		writew((u32)tmp_ipg, mmio + IPG); 
		writew((u32)(tmp_ipg - IFS1_DELTA), mmio + IFS1); 
	}
	 mod_timer(&lp->ipg_data.ipg_timer, jiffies + IPG_CONVERGE_JIFFIES);
	return;

}

static int __devinit amd8111e_probe_one(struct pci_dev *pdev,
				  const struct pci_device_id *ent)
{
	int err,i,pm_cap;
	unsigned long reg_addr,reg_len;
	struct amd8111e_priv* lp;
	struct net_device* dev;

	err = pci_enable_device(pdev);
	if(err){
		printk(KERN_ERR "amd8111e: Cannot enable new PCI device,"
			"exiting.\n");
		return err;
	}

	if(!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)){
		printk(KERN_ERR "amd8111e: Cannot find PCI base address"
		       "exiting.\n");
		err = -ENODEV;
		goto err_disable_pdev;
	}

	err = pci_request_regions(pdev, MODULE_NAME);
	if(err){
		printk(KERN_ERR "amd8111e: Cannot obtain PCI resources, "
		       "exiting.\n");
		goto err_disable_pdev;
	}

	pci_set_master(pdev);

	/* Find power-management capability. */
	if((pm_cap = pci_find_capability(pdev, PCI_CAP_ID_PM))==0){
		printk(KERN_ERR "amd8111e: No Power Management capability, "
		       "exiting.\n");
		goto err_free_reg;
	}

	/* Initialize DMA */
	if(!pci_dma_supported(pdev, 0xffffffff)){
		printk(KERN_ERR "amd8111e: DMA not supported,"
			"exiting.\n");
		goto  err_free_reg;
	} else
		pdev->dma_mask = 0xffffffff;
	
	reg_addr = pci_resource_start(pdev, 0);
	reg_len = pci_resource_len(pdev, 0);

	dev = alloc_etherdev(sizeof(struct amd8111e_priv));
	if (!dev) {
		printk(KERN_ERR "amd8111e: Etherdev alloc failed, exiting.\n");
		err = -ENOMEM;
		goto err_free_reg;
	}

	SET_MODULE_OWNER(dev);
	SET_NETDEV_DEV(dev, &pdev->dev);

#if AMD8111E_VLAN_TAG_USED
	dev->features |= NETIF_F_HW_VLAN_TX | NETIF_F_HW_VLAN_RX ;
	dev->vlan_rx_register =amd8111e_vlan_rx_register;
	dev->vlan_rx_kill_vid = amd8111e_vlan_rx_kill_vid;
#endif	
	
	lp = netdev_priv(dev);
	lp->pci_dev = pdev;
	lp->amd8111e_net_dev = dev;
	lp->pm_cap = pm_cap;

	/* setting mii default values */
	lp->mii_if.dev = dev;
	lp->mii_if.mdio_read = amd8111e_mdio_read;
	lp->mii_if.mdio_write = amd8111e_mdio_write;
	lp->mii_if.phy_id = PHY_ID;

	spin_lock_init(&lp->lock);

	lp->mmio = ioremap(reg_addr, reg_len);
	if (lp->mmio == 0) {
		printk(KERN_ERR "amd8111e: Cannot map device registers, "
		       "exiting\n");
		err = -ENOMEM;
		goto err_free_dev;
	}
	
	/* Initializing MAC address */
	for(i = 0; i < ETH_ADDR_LEN; i++)
			dev->dev_addr[i] =readb(lp->mmio + PADR + i);
	
	/* Setting user defined parametrs */
	lp->ext_phy_option = speed_duplex[card_idx];
	if(coalesce[card_idx])
		lp->options |= OPTION_INTR_COAL_ENABLE;		
	if(dynamic_ipg[card_idx++])
		lp->options |= OPTION_DYN_IPG_ENABLE;	        	

	/* Initialize driver entry points */
	dev->open = amd8111e_open;
	dev->hard_start_xmit = amd8111e_start_xmit;
	dev->stop = amd8111e_close;
	dev->get_stats = amd8111e_get_stats;
	dev->set_multicast_list = amd8111e_set_multicast_list;
	dev->set_mac_address = amd8111e_set_mac_address;
	dev->do_ioctl = amd8111e_ioctl;
	dev->change_mtu = amd8111e_change_mtu;
	dev->irq =pdev->irq;
	dev->tx_timeout = amd8111e_tx_timeout; 
	dev->watchdog_timeo = AMD8111E_TX_TIMEOUT; 
#ifdef CONFIG_AMD8111E_NAPI
	dev->poll = amd8111e_rx_poll;
	dev->weight = 32;
#endif
#ifdef CONFIG_NET_POLL_CONTROLLER
	dev->poll_controller = amd8111e_poll; 
#endif

#if AMD8111E_VLAN_TAG_USED
	dev->features |= NETIF_F_HW_VLAN_TX | NETIF_F_HW_VLAN_RX;
	dev->vlan_rx_register =amd8111e_vlan_rx_register;
	dev->vlan_rx_kill_vid = amd8111e_vlan_rx_kill_vid;
#endif	
	
	/* Set receive buffer length and set jumbo option*/
	amd8111e_set_rx_buff_len(dev);


	err = register_netdev(dev);
	if (err) {
		printk(KERN_ERR "amd8111e: Cannot register net device, "
		       "exiting.\n");
		goto err_iounmap;
	}

	pci_set_drvdata(pdev, dev);
	
	/* Initialize software ipg timer */
	if(lp->options & OPTION_DYN_IPG_ENABLE){	        
		init_timer(&lp->ipg_data.ipg_timer);
		lp->ipg_data.ipg_timer.data = (unsigned long) dev;
		lp->ipg_data.ipg_timer.function = (void *)&amd8111e_config_ipg;
		lp->ipg_data.ipg_timer.expires = jiffies + 
						 IPG_CONVERGE_JIFFIES;
		lp->ipg_data.ipg = DEFAULT_IPG;
		lp->ipg_data.ipg_state = CSTATE;
	};

	/*  display driver and device information */

    	chip_version = (readl(lp->mmio + CHIPID) & 0xf0000000)>>28;
    	printk(KERN_INFO "%s: AMD-8111e Driver Version: %s\n",								 dev->name,MODULE_VERS);
    	printk(KERN_INFO "%s: [ Rev %x ] PCI 10/100BaseT Ethernet ",							dev->name, chip_version);
    	for (i = 0; i < 6; i++)
		printk("%2.2x%c",dev->dev_addr[i],i == 5 ? ' ' : ':');
    	printk( "\n");	
    	return 0;
err_iounmap:
	iounmap((void *) lp->mmio);

err_free_dev:
	free_netdev(dev);

err_free_reg:
	pci_release_regions(pdev);

err_disable_pdev:
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	return err;

}

static struct pci_driver amd8111e_driver = {
	.name   	= MODULE_NAME,
	.id_table	= amd8111e_pci_tbl,
	.probe		= amd8111e_probe_one,
	.remove		= __devexit_p(amd8111e_remove_one),
	.suspend	= amd8111e_suspend,
	.resume		= amd8111e_resume
};

static int __init amd8111e_init(void)
{
	return pci_module_init(&amd8111e_driver);
}

static void __exit amd8111e_cleanup(void)
{
	pci_unregister_driver(&amd8111e_driver);
}

module_init(amd8111e_init);
module_exit(amd8111e_cleanup);
