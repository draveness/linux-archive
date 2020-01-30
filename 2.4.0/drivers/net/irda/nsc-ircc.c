/*********************************************************************
 *                
 * Filename:      nsc-ircc.c
 * Version:       1.0
 * Description:   Driver for the NSC PC'108 and PC'338 IrDA chipsets
 * Status:        Stable.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Sat Nov  7 21:43:15 1998
 * Modified at:   Wed Mar  1 11:29:34 2000
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * 
 *     Copyright (c) 1998-2000 Dag Brattli <dagb@cs.uit.no>
 *     Copyright (c) 1998 Lichen Wang, <lwang@actisys.com>
 *     Copyright (c) 1998 Actisys Corp., www.actisys.com
 *     All Rights Reserved
 *      
 *     This program is free software; you can redistribute it and/or 
 *     modify it under the terms of the GNU General Public License as 
 *     published by the Free Software Foundation; either version 2 of 
 *     the License, or (at your option) any later version.
 *  
 *     Neither Dag Brattli nor University of Troms� admit liability nor
 *     provide warranty for any of this software. This material is 
 *     provided "AS-IS" and at no charge.
 *
 *     Notice that all functions that needs to access the chip in _any_
 *     way, must save BSR register on entry, and restore it on exit. 
 *     It is _very_ important to follow this policy!
 *
 *         __u8 bank;
 *     
 *         bank = inb(iobase+BSR);
 *  
 *         do_your_stuff_here();
 *
 *         outb(bank, iobase+BSR);
 *
 *    If you find bugs in this file, its very likely that the same bug
 *    will also be in w83977af_ir.c since the implementations are quite
 *    similar.
 *     
 ********************************************************************/

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/malloc.h>
#include <linux/init.h>
#include <linux/rtnetlink.h>

#include <asm/io.h>
#include <asm/dma.h>
#include <asm/byteorder.h>

#include <linux/pm.h>

#include <net/irda/wrapper.h>
#include <net/irda/irda.h>
#include <net/irda/irmod.h>
#include <net/irda/irlap_frame.h>
#include <net/irda/irda_device.h>

#include <net/irda/nsc-ircc.h>

#define CHIP_IO_EXTENT 8
#define BROKEN_DONGLE_ID

static char *driver_name = "nsc-ircc";

/* Module parameters */
static int qos_mtt_bits = 0x07;  /* 1 ms or more */
static int dongle_id;

/* Use BIOS settions by default, but user may supply module parameters */
static unsigned int io[]  = { ~0, ~0, ~0, ~0 };
static unsigned int irq[] = { 0, 0, 0, 0, 0 };
static unsigned int dma[] = { 0, 0, 0, 0, 0 };

static int nsc_ircc_probe_108(nsc_chip_t *chip, chipio_t *info);
static int nsc_ircc_probe_338(nsc_chip_t *chip, chipio_t *info);
static int nsc_ircc_init_108(nsc_chip_t *chip, chipio_t *info);
static int nsc_ircc_init_338(nsc_chip_t *chip, chipio_t *info);

/* These are the known NSC chips */
static nsc_chip_t chips[] = {
	{ "PC87108", { 0x150, 0x398, 0xea }, 0x05, 0x10, 0xf0, 
	  nsc_ircc_probe_108, nsc_ircc_init_108 },
	{ "PC87338", { 0x398, 0x15c, 0x2e }, 0x08, 0xb0, 0xf0, 
	  nsc_ircc_probe_338, nsc_ircc_init_338 },
	{ NULL }
};

/* Max 4 instances for now */
static struct nsc_ircc_cb *dev_self[] = { NULL, NULL, NULL, NULL };

static char *dongle_types[] = {
	"Differential serial interface",
	"Differential serial interface",
	"Reserved",
	"Reserved",
	"Sharp RY5HD01",
	"Reserved",
	"Single-ended serial interface",
	"Consumer-IR only",
	"HP HSDL-2300, HP HSDL-3600/HSDL-3610",
	"IBM31T1100 or Temic TFDS6000/TFDS6500",
	"Reserved",
	"Reserved",
	"HP HSDL-1100/HSDL-2100",
	"HP HSDL-1100/HSDL-2100"
	"Supports SIR Mode only",
	"No dongle connected",
};

/* Some prototypes */
static int  nsc_ircc_open(int i, chipio_t *info);
#ifdef MODULE
static int  nsc_ircc_close(struct nsc_ircc_cb *self);
#endif /* MODULE */
static int  nsc_ircc_setup(chipio_t *info);
static void nsc_ircc_pio_receive(struct nsc_ircc_cb *self);
static int  nsc_ircc_dma_receive(struct nsc_ircc_cb *self); 
static int  nsc_ircc_dma_receive_complete(struct nsc_ircc_cb *self, int iobase);
static int  nsc_ircc_hard_xmit_sir(struct sk_buff *skb, struct net_device *dev);
static int  nsc_ircc_hard_xmit_fir(struct sk_buff *skb, struct net_device *dev);
static int  nsc_ircc_pio_write(int iobase, __u8 *buf, int len, int fifo_size);
static void nsc_ircc_dma_xmit(struct nsc_ircc_cb *self, int iobase);
static void nsc_ircc_change_speed(struct nsc_ircc_cb *self, __u32 baud);
static void nsc_ircc_interrupt(int irq, void *dev_id, struct pt_regs *regs);
static int  nsc_ircc_is_receiving(struct nsc_ircc_cb *self);
static int  nsc_ircc_read_dongle_id (int iobase);
static void nsc_ircc_init_dongle_interface (int iobase, int dongle_id);

static int  nsc_ircc_net_init(struct net_device *dev);
static int  nsc_ircc_net_open(struct net_device *dev);
static int  nsc_ircc_net_close(struct net_device *dev);
static int  nsc_ircc_net_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
static struct net_device_stats *nsc_ircc_net_get_stats(struct net_device *dev);
static int nsc_ircc_pmproc(struct pm_dev *dev, pm_request_t rqst, void *data);

/*
 * Function nsc_ircc_init ()
 *
 *    Initialize chip. Just try to find out how many chips we are dealing with
 *    and where they are
 */
int __init nsc_ircc_init(void)
{
	chipio_t info;
	nsc_chip_t *chip;
	int ret = -ENODEV;
	int cfg_base;
	int cfg, id;
	int reg;
	int i = 0;

	/* Probe for all the NSC chipsets we know about */
	for (chip=chips; chip->name ; chip++,i++) {
		IRDA_DEBUG(2, __FUNCTION__"(), Probing for %s ...\n", 
			   chip->name);
		
		/* Try all config registers for this chip */
		for (cfg=0; cfg<3; cfg++) {
			cfg_base = chip->cfg[cfg];
			if (!cfg_base)
				continue;
			
			memset(&info, 0, sizeof(chipio_t));
			info.cfg_base = cfg_base;
			info.fir_base = io[i];
			info.dma = dma[i];
			info.irq = irq[i];

			/* Read index register */
			reg = inb(cfg_base);
			if (reg == 0xff) {
				IRDA_DEBUG(2, __FUNCTION__ 
					   "() no chip at 0x%03x\n", cfg_base);
				continue;
			}
			
			/* Read chip identification register */
			outb(chip->cid_index, cfg_base);
			id = inb(cfg_base+1);
			if ((id & chip->cid_mask) == chip->cid_value) {
				IRDA_DEBUG(2, __FUNCTION__ 
					   "() Found %s chip, revision=%d\n",
					   chip->name, id & ~chip->cid_mask);
				/* 
				 * If the user supplies the base address, then
				 * we init the chip, if not we probe the values
				 * set by the BIOS
				 */				
				if (io[i] < 2000) {
					chip->init(chip, &info);
				} else
					chip->probe(chip, &info);

				if (nsc_ircc_open(i, &info) == 0)
					ret = 0;
				i++;
			} else {
				IRDA_DEBUG(2, __FUNCTION__ 
					   "(), Wrong chip id=0x%02x\n", id);
			}
		} 
		
	}

	return ret;
}

/*
 * Function nsc_ircc_cleanup ()
 *
 *    Close all configured chips
 *
 */
#ifdef MODULE
static void nsc_ircc_cleanup(void)
{
	int i;

	pm_unregister_all(nsc_ircc_pmproc);

	for (i=0; i < 4; i++) {
		if (dev_self[i])
			nsc_ircc_close(dev_self[i]);
	}
}
#endif /* MODULE */

/*
 * Function nsc_ircc_open (iobase, irq)
 *
 *    Open driver instance
 *
 */
static int nsc_ircc_open(int i, chipio_t *info)
{
	struct net_device *dev;
	struct nsc_ircc_cb *self;
        struct pm_dev *pmdev;
	int ret;
	int err;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	if ((nsc_ircc_setup(info)) == -1)
		return -1;

	/* Allocate new instance of the driver */
	self = kmalloc(sizeof(struct nsc_ircc_cb), GFP_KERNEL);
	if (self == NULL) {
		ERROR(__FUNCTION__ "(), can't allocate memory for "
		       "control block!\n");
		return -ENOMEM;
	}
	memset(self, 0, sizeof(struct nsc_ircc_cb));
	spin_lock_init(&self->lock);
   
	/* Need to store self somewhere */
	dev_self[i] = self;
	self->index = i;

	/* Initialize IO */
	self->io.cfg_base  = info->cfg_base;
	self->io.fir_base  = info->fir_base;
        self->io.irq       = info->irq;
        self->io.fir_ext   = CHIP_IO_EXTENT;
        self->io.dma       = info->dma;
        self->io.fifo_size = 32;
	
	/* Reserve the ioports that we need */
	ret = check_region(self->io.fir_base, self->io.fir_ext);
	if (ret < 0) { 
		WARNING(__FUNCTION__ "(), can't get iobase of 0x%03x\n",
			self->io.fir_base);
		dev_self[i] = NULL;
		kfree(self);
		return -ENODEV;
	}
	request_region(self->io.fir_base, self->io.fir_ext, driver_name);

	/* Initialize QoS for this device */
	irda_init_max_qos_capabilies(&self->qos);
	
	/* The only value we must override it the baudrate */
	self->qos.baud_rate.bits = IR_9600|IR_19200|IR_38400|IR_57600|
		IR_115200|IR_576000|IR_1152000 |(IR_4000000 << 8);
	
	self->qos.min_turn_time.bits = qos_mtt_bits;
	irda_qos_bits_to_value(&self->qos);
	
	self->flags = IFF_FIR|IFF_MIR|IFF_SIR|IFF_DMA|IFF_PIO|IFF_DONGLE;

	/* Max DMA buffer size needed = (data_size + 6) * (window_size) + 6; */
	self->rx_buff.truesize = 14384; 
	self->tx_buff.truesize = 14384;

	/* Allocate memory if needed */
	self->rx_buff.head = (__u8 *) kmalloc(self->rx_buff.truesize,
					      GFP_KERNEL|GFP_DMA);
	if (self->rx_buff.head == NULL) {
		kfree(self);
		return -ENOMEM;
	}
	memset(self->rx_buff.head, 0, self->rx_buff.truesize);
	
	self->tx_buff.head = (__u8 *) kmalloc(self->tx_buff.truesize, 
					      GFP_KERNEL|GFP_DMA);
	if (self->tx_buff.head == NULL) {
		kfree(self);
		kfree(self->rx_buff.head);
		return -ENOMEM;
	}
	memset(self->tx_buff.head, 0, self->tx_buff.truesize);

	self->rx_buff.in_frame = FALSE;
	self->rx_buff.state = OUTSIDE_FRAME;
	self->tx_buff.data = self->tx_buff.head;
	self->rx_buff.data = self->rx_buff.head;
	
	/* Reset Tx queue info */
	self->tx_fifo.len = self->tx_fifo.ptr = self->tx_fifo.free = 0;
	self->tx_fifo.tail = self->tx_buff.head;

	if (!(dev = dev_alloc("irda%d", &err))) {
		ERROR(__FUNCTION__ "(), dev_alloc() failed!\n");
		return -ENOMEM;
	}

	dev->priv = (void *) self;
	self->netdev = dev;

	/* Override the network functions we need to use */
	dev->init            = nsc_ircc_net_init;
	dev->hard_start_xmit = nsc_ircc_hard_xmit_sir;
	dev->open            = nsc_ircc_net_open;
	dev->stop            = nsc_ircc_net_close;
	dev->do_ioctl        = nsc_ircc_net_ioctl;
	dev->get_stats	     = nsc_ircc_net_get_stats;

	rtnl_lock();
	err = register_netdevice(dev);
	rtnl_unlock();
	if (err) {
		ERROR(__FUNCTION__ "(), register_netdev() failed!\n");
		return -1;
	}
	MESSAGE("IrDA: Registered device %s\n", dev->name);

	/* Check if user has supplied the dongle id or not */
	if (!dongle_id) {
		dongle_id = nsc_ircc_read_dongle_id(self->io.fir_base);
		
		MESSAGE("%s, Found dongle: %s\n", driver_name,
			dongle_types[dongle_id]);
	} else {
		MESSAGE("%s, Using dongle: %s\n", driver_name,
			dongle_types[dongle_id]);
	}
	
	self->io.dongle_id = dongle_id;
	nsc_ircc_init_dongle_interface(self->io.fir_base, dongle_id);

        pmdev = pm_register(PM_SYS_DEV, PM_SYS_IRDA, nsc_ircc_pmproc);
        if (pmdev)
                pmdev->data = self;

	return 0;
}

#ifdef MODULE
/*
 * Function nsc_ircc_close (self)
 *
 *    Close driver instance
 *
 */
static int nsc_ircc_close(struct nsc_ircc_cb *self)
{
	int iobase;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -1;);

        iobase = self->io.fir_base;

	/* Remove netdevice */
	if (self->netdev) {
		rtnl_lock();
		unregister_netdevice(self->netdev);
		rtnl_unlock();
	}

	/* Release the PORT that this driver is using */
	IRDA_DEBUG(4, __FUNCTION__ "(), Releasing Region %03x\n", 
		   self->io.fir_base);
	release_region(self->io.fir_base, self->io.fir_ext);

	if (self->tx_buff.head)
		kfree(self->tx_buff.head);
	
	if (self->rx_buff.head)
		kfree(self->rx_buff.head);

	dev_self[self->index] = NULL;
	kfree(self);
	
	return 0;
}
#endif /* MODULE */

/*
 * Function nsc_ircc_init_108 (iobase, cfg_base, irq, dma)
 *
 *    Initialize the NSC '108 chip
 *
 */
static int nsc_ircc_init_108(nsc_chip_t *chip, chipio_t *info)
{
	int cfg_base = info->cfg_base;
	__u8 temp=0;

	outb(2, cfg_base);      /* Mode Control Register (MCTL) */
	outb(0x00, cfg_base+1); /* Disable device */
	
	/* Base Address and Interrupt Control Register (BAIC) */
	outb(0, cfg_base);
	switch (info->fir_base) {
	case 0x3e8: outb(0x14, cfg_base+1); break;
	case 0x2e8: outb(0x15, cfg_base+1); break;
	case 0x3f8: outb(0x16, cfg_base+1); break;
	case 0x2f8: outb(0x17, cfg_base+1); break;
	default: ERROR(__FUNCTION__ "(), invalid base_address");
	}
	
	/* Control Signal Routing Register (CSRT) */
	switch (info->irq) {
	case 3:  temp = 0x01; break;
	case 4:  temp = 0x02; break;
	case 5:  temp = 0x03; break;
	case 7:  temp = 0x04; break;
	case 9:  temp = 0x05; break;
	case 11: temp = 0x06; break;
	case 15: temp = 0x07; break;
	default: ERROR(__FUNCTION__ "(), invalid irq");
	}
	outb(1, cfg_base);
	
	switch (info->dma) {	
	case 0: outb(0x08+temp, cfg_base+1); break;
	case 1: outb(0x10+temp, cfg_base+1); break;
	case 3: outb(0x18+temp, cfg_base+1); break;
	default: ERROR(__FUNCTION__ "(), invalid dma");
	}
	
	outb(2, cfg_base);      /* Mode Control Register (MCTL) */
	outb(0x03, cfg_base+1); /* Enable device */

	return 0;
}

/*
 * Function nsc_ircc_probe_108 (chip, info)
 *
 *    
 *
 */
static int nsc_ircc_probe_108(nsc_chip_t *chip, chipio_t *info) 
{
	int cfg_base = info->cfg_base;
	int reg;

	/* Read address and interrupt control register (BAIC) */
	outb(CFG_BAIC, cfg_base);
	reg = inb(cfg_base+1);
	
	switch (reg & 0x03) {
	case 0:
		info->fir_base = 0x3e8;
		break;
	case 1:
		info->fir_base = 0x2e8;
		break;
	case 2:
		info->fir_base = 0x3f8;
		break;
	case 3:
		info->fir_base = 0x2f8;
		break;
	}
	info->sir_base = info->fir_base;
	IRDA_DEBUG(2, __FUNCTION__ "(), probing fir_base=0x%03x\n", 
		   info->fir_base);

	/* Read control signals routing register (CSRT) */
	outb(CFG_CSRT, cfg_base);
	reg = inb(cfg_base+1);

	switch (reg & 0x07) {
	case 0:
		info->irq = -1;
		break;
	case 1:
		info->irq = 3;
		break;
	case 2:
		info->irq = 4;
		break;
	case 3:
		info->irq = 5;
		break;
	case 4:
		info->irq = 7;
		break;
	case 5:
		info->irq = 9;
		break;
	case 6:
		info->irq = 11;
		break;
	case 7:
		info->irq = 15;
		break;
	}
	IRDA_DEBUG(2, __FUNCTION__ "(), probing irq=%d\n", info->irq);

	/* Currently we only read Rx DMA but it will also be used for Tx */
	switch ((reg >> 3) & 0x03) {
	case 0:
		info->dma = -1;
		break;
	case 1:
		info->dma = 0;
		break;
	case 2:
		info->dma = 1;
		break;
	case 3:
		info->dma = 3;
		break;
	}
	IRDA_DEBUG(2, __FUNCTION__ "(), probing dma=%d\n", info->dma);

	/* Read mode control register (MCTL) */
	outb(CFG_MCTL, cfg_base);
	reg = inb(cfg_base+1);

	info->enabled = reg & 0x01;
	info->suspended = !((reg >> 1) & 0x01);

	return 0;
}

/*
 * Function nsc_ircc_init_338 (chip, info)
 *
 *    Initialize the NSC '338 chip. Remember that the 87338 needs two 
 *    consecutive writes to the data registers while CPU interrupts are
 *    disabled. The 97338 does not require this, but shouldn't be any
 *    harm if we do it anyway.
 */
static int nsc_ircc_init_338(nsc_chip_t *chip, chipio_t *info) 
{
	/* No init yet */
	
	return 0;
}

/*
 * Function nsc_ircc_probe_338 (chip, info)
 *
 *    
 *
 */
static int nsc_ircc_probe_338(nsc_chip_t *chip, chipio_t *info) 
{
	int cfg_base = info->cfg_base;
	int reg, com = 0;
	int pnp;

	/* Read funtion enable register (FER) */
	outb(CFG_FER, cfg_base);
	reg = inb(cfg_base+1);

	info->enabled = (reg >> 2) & 0x01;

	/* Check if we are in Legacy or PnP mode */
	outb(CFG_PNP0, cfg_base);
	reg = inb(cfg_base+1);
	
	pnp = (reg >> 4) & 0x01;
	if (pnp) {
		IRDA_DEBUG(2, "(), Chip is in PnP mode\n");
		outb(0x46, cfg_base);
		reg = (inb(cfg_base+1) & 0xfe) << 2;

		outb(0x47, cfg_base);
		reg |= ((inb(cfg_base+1) & 0xfc) << 8);

		info->fir_base = reg;
	} else {
		/* Read function address register (FAR) */
		outb(CFG_FAR, cfg_base);
		reg = inb(cfg_base+1);
		
		switch ((reg >> 4) & 0x03) {
		case 0:
			info->fir_base = 0x3f8;
			break;
		case 1:
			info->fir_base = 0x2f8;
			break;
		case 2:
			com = 3;
			break;
		case 3:
			com = 4;
			break;
		}
		
		if (com) {
			switch ((reg >> 6) & 0x03) {
			case 0:
				if (com == 3)
					info->fir_base = 0x3e8;
				else
					info->fir_base = 0x2e8;
				break;
			case 1:
				if (com == 3)
					info->fir_base = 0x338;
				else
					info->fir_base = 0x238;
				break;
			case 2:
				if (com == 3)
					info->fir_base = 0x2e8;
				else
					info->fir_base = 0x2e0;
				break;
			case 3:
				if (com == 3)
					info->fir_base = 0x220;
				else
					info->fir_base = 0x228;
				break;
			}
		}
	}
	info->sir_base = info->fir_base;

	/* Read PnP register 1 (PNP1) */
	outb(CFG_PNP1, cfg_base);
	reg = inb(cfg_base+1);
	
	info->irq = reg >> 4;
	
	/* Read PnP register 3 (PNP3) */
	outb(CFG_PNP3, cfg_base);
	reg = inb(cfg_base+1);

	info->dma = (reg & 0x07) - 1;

	/* Read power and test register (PTR) */
	outb(CFG_PTR, cfg_base);
	reg = inb(cfg_base+1);

	info->suspended = reg & 0x01;

	return 0;
}

/*
 * Function nsc_ircc_setup (info)
 *
 *    Returns non-negative on success.
 *
 */
static int nsc_ircc_setup(chipio_t *info)
{
	int version;
	int iobase = info->fir_base;

	/* Read the Module ID */
	switch_bank(iobase, BANK3);
	version = inb(iobase+MID);

	/* Should be 0x2? */
	if (0x20 != (version & 0xf0)) {
		ERROR("%s, Wrong chip version %02x\n", driver_name, version);
		return -1;
	}
	MESSAGE("%s, Found chip at base=0x%03x\n", driver_name, 
		info->cfg_base);

	/* Switch to advanced mode */
	switch_bank(iobase, BANK2);
	outb(ECR1_EXT_SL, iobase+ECR1);
	switch_bank(iobase, BANK0);
	
	/* Set FIFO threshold to TX17, RX16, reset and enable FIFO's */
	switch_bank(iobase, BANK0);
	outb(FCR_RXTH|FCR_TXTH|FCR_TXSR|FCR_RXSR|FCR_FIFO_EN, iobase+FCR);

	outb(0x03, iobase+LCR); 	/* 8 bit word length */
	outb(MCR_SIR, iobase+MCR); 	/* Start at SIR-mode, also clears LSR*/

	/* Set FIFO size to 32 */
	switch_bank(iobase, BANK2);
	outb(EXCR2_RFSIZ|EXCR2_TFSIZ, iobase+EXCR2);

	/* IRCR2: FEND_MD is not set */
	switch_bank(iobase, BANK5);
 	outb(0x02, iobase+4);

	/* Make sure that some defaults are OK */
	switch_bank(iobase, BANK6);
	outb(0x20, iobase+0); /* Set 32 bits FIR CRC */
	outb(0x0a, iobase+1); /* Set MIR pulse width */
	outb(0x0d, iobase+2); /* Set SIR pulse width to 1.6us */
	outb(0x2a, iobase+4); /* Set beginning frag, and preamble length */

	MESSAGE("%s, driver loaded (Dag Brattli)\n", driver_name);

	/* Enable receive interrupts */
	switch_bank(iobase, BANK0);
	outb(IER_RXHDL_IE, iobase+IER);

	return 0;
}

/*
 * Function nsc_ircc_read_dongle_id (void)
 *
 * Try to read dongle indentification. This procedure needs to be executed
 * once after power-on/reset. It also needs to be used whenever you suspect
 * that the user may have plugged/unplugged the IrDA Dongle.
 */
static int nsc_ircc_read_dongle_id (int iobase)
{
	int dongle_id;
	__u8 bank;

	bank = inb(iobase+BSR);

	/* Select Bank 7 */
	switch_bank(iobase, BANK7);
	
	/* IRCFG4: IRSL0_DS and IRSL21_DS are cleared */
	outb(0x00, iobase+7);
	
	/* ID0, 1, and 2 are pulled up/down very slowly */
	udelay(50);
	
	/* IRCFG1: read the ID bits */
	dongle_id = inb(iobase+4) & 0x0f;

#ifdef BROKEN_DONGLE_ID
	if (dongle_id == 0x0a)
		dongle_id = 0x09;
#endif	
	/* Go back to  bank 0 before returning */
	switch_bank(iobase, BANK0);

	outb(bank, iobase+BSR);

	return dongle_id;
}

/*
 * Function nsc_ircc_init_dongle_interface (iobase, dongle_id)
 *
 *     This function initializes the dongle for the transceiver that is
 *     used. This procedure needs to be executed once after
 *     power-on/reset. It also needs to be used whenever you suspect that
 *     the dongle is changed. 
 */
static void nsc_ircc_init_dongle_interface (int iobase, int dongle_id)
{
	int bank;

	/* Save current bank */
	bank = inb(iobase+BSR);

	/* Select Bank 7 */
	switch_bank(iobase, BANK7);
	
	/* IRCFG4: set according to dongle_id */
	switch (dongle_id) {
	case 0x00: /* same as */
	case 0x01: /* Differential serial interface */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x02: /* same as */
	case 0x03: /* Reserved */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x04: /* Sharp RY5HD01 */
		break;
	case 0x05: /* Reserved, but this is what the Thinkpad reports */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x06: /* Single-ended serial interface */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x07: /* Consumer-IR only */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s is not for IrDA mode\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x08: /* HP HSDL-2300, HP HSDL-3600/HSDL-3610 */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s\n",
			   dongle_types[dongle_id]);
		break;
	case 0x09: /* IBM31T1100 or Temic TFDS6000/TFDS6500 */
		outb(0x28, iobase+7); /* Set irsl[0-2] as output */
		break;
	case 0x0A: /* same as */
	case 0x0B: /* Reserved */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x0C: /* same as */
	case 0x0D: /* HP HSDL-1100/HSDL-2100 */
		/* 
		 * Set irsl0 as input, irsl[1-2] as output, and separate 
		 * inputs are used for SIR and MIR/FIR 
		 */
		outb(0x48, iobase+7); 
		break;
	case 0x0E: /* Supports SIR Mode only */
		outb(0x28, iobase+7); /* Set irsl[0-2] as output */
		break;
	case 0x0F: /* No dongle connected */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s\n",
			   dongle_types[dongle_id]); 

		switch_bank(iobase, BANK0);
		outb(0x62, iobase+MCR);
		break;
	default: 
		IRDA_DEBUG(0, __FUNCTION__ "(), invalid dongle_id %#x", 
			   dongle_id);
	}
	
	/* IRCFG1: IRSL1 and 2 are set to IrDA mode */
	outb(0x00, iobase+4);

	/* Restore bank register */
	outb(bank, iobase+BSR);
	
} /* set_up_dongle_interface */

/*
 * Function nsc_ircc_change_dongle_speed (iobase, speed, dongle_id)
 *
 *    Change speed of the attach dongle
 *
 */
static void nsc_ircc_change_dongle_speed(int iobase, int speed, int dongle_id)
{
	unsigned long flags;
	__u8 bank;

	/* Save current bank */
	bank = inb(iobase+BSR);

	/* Select Bank 7 */
	switch_bank(iobase, BANK7);
	
	/* IRCFG1: set according to dongle_id */
	switch (dongle_id) {
	case 0x00: /* same as */
	case 0x01: /* Differential serial interface */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x02: /* same as */
	case 0x03: /* Reserved */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x04: /* Sharp RY5HD01 */
		break;
	case 0x05: /* Reserved */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x06: /* Single-ended serial interface */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x07: /* Consumer-IR only */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s is not for IrDA mode\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x08: /* HP HSDL-2300, HP HSDL-3600/HSDL-3610 */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s\n", 
			   dongle_types[dongle_id]); 
		outb(0x00, iobase+4);
		if (speed > 115200)
			outb(0x01, iobase+4);
		break;
	case 0x09: /* IBM31T1100 or Temic TFDS6000/TFDS6500 */
		outb(0x01, iobase+4);

		if (speed == 4000000) {
			save_flags(flags);
			cli();
			outb(0x81, iobase+4);
			outb(0x80, iobase+4);
			restore_flags(flags);
		} else
			outb(0x00, iobase+4);
		break;
	case 0x0A: /* same as */
	case 0x0B: /* Reserved */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s not defined by irda yet\n",
			   dongle_types[dongle_id]); 
		break;
	case 0x0C: /* same as */
	case 0x0D: /* HP HSDL-1100/HSDL-2100 */
		break;
	case 0x0E: /* Supports SIR Mode only */
		break;
	case 0x0F: /* No dongle connected */
		IRDA_DEBUG(0, __FUNCTION__ "(), %s is not for IrDA mode\n",
			   dongle_types[dongle_id]);

		switch_bank(iobase, BANK0); 
		outb(0x62, iobase+MCR);
		break;
	default: 
		IRDA_DEBUG(0, __FUNCTION__ "(), invalid data_rate\n");
	}
	/* Restore bank register */
	outb(bank, iobase+BSR);
}

/*
 * Function nsc_ircc_change_speed (self, baud)
 *
 *    Change the speed of the device
 *
 */
static void nsc_ircc_change_speed(struct nsc_ircc_cb *self, __u32 speed)
{
	struct net_device *dev = self->netdev;
	__u8 mcr = MCR_SIR;
	int iobase; 
	__u8 bank;

	IRDA_DEBUG(2, __FUNCTION__ "(), speed=%d\n", speed);

	ASSERT(self != NULL, return;);

	iobase = self->io.fir_base;

	/* Update accounting for new speed */
	self->io.speed = speed;

	/* Save current bank */
	bank = inb(iobase+BSR);

	/* Disable interrupts */
	switch_bank(iobase, BANK0);
	outb(0, iobase+IER);

	/* Select Bank 2 */
	switch_bank(iobase, BANK2);

	outb(0x00, iobase+BGDH);
	switch (speed) {
	case 9600:   outb(0x0c, iobase+BGDL); break;
	case 19200:  outb(0x06, iobase+BGDL); break;
	case 38400:  outb(0x03, iobase+BGDL); break;
	case 57600:  outb(0x02, iobase+BGDL); break;
	case 115200: outb(0x01, iobase+BGDL); break;
	case 576000:
		switch_bank(iobase, BANK5);
		
		/* IRCR2: MDRS is set */
		outb(inb(iobase+4) | 0x04, iobase+4);
	       
		mcr = MCR_MIR;
		IRDA_DEBUG(0, __FUNCTION__ "(), handling baud of 576000\n");
		break;
	case 1152000:
		mcr = MCR_MIR;
		IRDA_DEBUG(0, __FUNCTION__ "(), handling baud of 1152000\n");
		break;
	case 4000000:
		mcr = MCR_FIR;
		IRDA_DEBUG(0, __FUNCTION__ "(), handling baud of 4000000\n");
		break;
	default:
		mcr = MCR_FIR;
		IRDA_DEBUG(0, __FUNCTION__ "(), unknown baud rate of %d\n", 
			   speed);
		break;
	}

	/* Set appropriate speed mode */
	switch_bank(iobase, BANK0);
	outb(mcr | MCR_TX_DFR, iobase+MCR);

	/* Give some hits to the transceiver */
	nsc_ircc_change_dongle_speed(iobase, speed, self->io.dongle_id);

	/* Set FIFO threshold to TX17, RX16 */
	switch_bank(iobase, BANK0);
	outb(0x00, iobase+FCR);
	outb(FCR_FIFO_EN, iobase+FCR);
	outb(FCR_RXTH|     /* Set Rx FIFO threshold */
	     FCR_TXTH|     /* Set Tx FIFO threshold */
	     FCR_TXSR|     /* Reset Tx FIFO */
	     FCR_RXSR|     /* Reset Rx FIFO */
	     FCR_FIFO_EN,  /* Enable FIFOs */
	     iobase+FCR);
	
	/* Set FIFO size to 32 */
	switch_bank(iobase, BANK2);
	outb(EXCR2_RFSIZ|EXCR2_TFSIZ, iobase+EXCR2);
	
	/* Enable some interrupts so we can receive frames */
	switch_bank(iobase, BANK0); 
	if (speed > 115200) {
		/* Install FIR xmit handler */
		dev->hard_start_xmit = nsc_ircc_hard_xmit_fir;
		outb(IER_SFIF_IE, iobase+IER);
		nsc_ircc_dma_receive(self);
	} else {
		/* Install SIR xmit handler */
		dev->hard_start_xmit = nsc_ircc_hard_xmit_sir;
		outb(IER_RXHDL_IE, iobase+IER);
	}
    	
	/* Restore BSR */
	outb(bank, iobase+BSR);
	netif_wake_queue(dev);
	
}

/*
 * Function nsc_ircc_hard_xmit (skb, dev)
 *
 *    Transmit the frame!
 *
 */
static int nsc_ircc_hard_xmit_sir(struct sk_buff *skb, struct net_device *dev)
{
	struct nsc_ircc_cb *self;
	unsigned long flags;
	int iobase;
	__u32 speed;
	__u8 bank;
	
	self = (struct nsc_ircc_cb *) dev->priv;

	ASSERT(self != NULL, return 0;);

	iobase = self->io.fir_base;

	netif_stop_queue(dev);
		
	/* Check if we need to change the speed */
	if ((speed = irda_get_speed(skb)) != self->io.speed) {
		/* Check for empty frame */
		if (!skb->len) {
			nsc_ircc_change_speed(self, speed); 
			return 0;
		} else
			self->new_speed = speed;
	}

	spin_lock_irqsave(&self->lock, flags);
	
	/* Save current bank */
	bank = inb(iobase+BSR);
	
	self->tx_buff.data = self->tx_buff.head;
	
	self->tx_buff.len = async_wrap_skb(skb, self->tx_buff.data, 
					   self->tx_buff.truesize);

	self->stats.tx_bytes += self->tx_buff.len;
	
	/* Add interrupt on tx low level (will fire immediately) */
	switch_bank(iobase, BANK0);
	outb(IER_TXLDL_IE, iobase+IER);
	
	/* Restore bank register */
	outb(bank, iobase+BSR);

	spin_unlock_irqrestore(&self->lock, flags);

	dev_kfree_skb(skb);

	return 0;
}

static int nsc_ircc_hard_xmit_fir(struct sk_buff *skb, struct net_device *dev)
{
	struct nsc_ircc_cb *self;
	unsigned long flags;
	int iobase;
	__u32 speed;
	__u8 bank;
	int mtt, diff;
	
	self = (struct nsc_ircc_cb *) dev->priv;
	iobase = self->io.fir_base;

	netif_stop_queue(dev);
	
	/* Check if we need to change the speed */
	if ((speed = irda_get_speed(skb)) != self->io.speed) {
		/* Check for empty frame */
		if (!skb->len) {
			nsc_ircc_change_speed(self, speed); 
			return 0;
		} else
			self->new_speed = speed;
	}

	spin_lock_irqsave(&self->lock, flags);

	/* Save current bank */
	bank = inb(iobase+BSR);

	/* Register and copy this frame to DMA memory */
	self->tx_fifo.queue[self->tx_fifo.free].start = self->tx_fifo.tail;
	self->tx_fifo.queue[self->tx_fifo.free].len = skb->len;
	self->tx_fifo.tail += skb->len;

	self->stats.tx_bytes += skb->len;

	memcpy(self->tx_fifo.queue[self->tx_fifo.free].start, skb->data, 
	       skb->len);
	
	self->tx_fifo.len++;
	self->tx_fifo.free++;

	/* Start transmit only if there is currently no transmit going on */
	if (self->tx_fifo.len == 1) {
		/* Check if we must wait the min turn time or not */
		mtt = irda_get_mtt(skb);
		if (mtt) {
			/* Check how much time we have used already */
			get_fast_time(&self->now);
			diff = self->now.tv_usec - self->stamp.tv_usec;
			if (diff < 0) 
				diff += 1000000;
			
			/* Check if the mtt is larger than the time we have
			 * already used by all the protocol processing
			 */
			if (mtt > diff) {
				mtt -= diff;

				/* 
				 * Use timer if delay larger than 125 us, and
				 * use udelay for smaller values which should
				 * be acceptable
				 */
				if (mtt > 125) {
					/* Adjust for timer resolution */
					mtt = mtt / 125;
					
					/* Setup timer */
					switch_bank(iobase, BANK4);
					outb(mtt & 0xff, iobase+TMRL);
					outb((mtt >> 8) & 0x0f, iobase+TMRH);
					
					/* Start timer */
					outb(IRCR1_TMR_EN, iobase+IRCR1);
					self->io.direction = IO_XMIT;
					
					/* Enable timer interrupt */
					switch_bank(iobase, BANK0);
					outb(IER_TMR_IE, iobase+IER);
					
					/* Timer will take care of the rest */
					goto out; 
				} else
					udelay(mtt);
			}
		}		
		/* Enable DMA interrupt */
		switch_bank(iobase, BANK0);
		outb(IER_DMA_IE, iobase+IER);

		/* Transmit frame */
		nsc_ircc_dma_xmit(self, iobase);
	}
 out:
	/* Not busy transmitting anymore if window is not full */
	if (self->tx_fifo.free < MAX_TX_WINDOW)
		netif_wake_queue(self->netdev);

	/* Restore bank register */
	outb(bank, iobase+BSR);

	spin_unlock_irqrestore(&self->lock, flags);
	dev_kfree_skb(skb);

	return 0;
}

/*
 * Function nsc_ircc_dma_xmit (self, iobase)
 *
 *    Transmit data using DMA
 *
 */
static void nsc_ircc_dma_xmit(struct nsc_ircc_cb *self, int iobase)
{
	int bsr;

	/* Save current bank */
	bsr = inb(iobase+BSR);

	/* Disable DMA */
	switch_bank(iobase, BANK0);
	outb(inb(iobase+MCR) & ~MCR_DMA_EN, iobase+MCR);
	
	self->io.direction = IO_XMIT;
	
	/* Choose transmit DMA channel  */ 
	switch_bank(iobase, BANK2);
	outb(ECR1_DMASWP|ECR1_DMANF|ECR1_EXT_SL, iobase+ECR1);
	
	setup_dma(self->io.dma, 
		  self->tx_fifo.queue[self->tx_fifo.ptr].start, 
		  self->tx_fifo.queue[self->tx_fifo.ptr].len, 
		  DMA_TX_MODE);

	/* Enable DMA and SIR interaction pulse */
 	switch_bank(iobase, BANK0);	
	outb(inb(iobase+MCR)|MCR_TX_DFR|MCR_DMA_EN|MCR_IR_PLS, iobase+MCR);

	/* Restore bank register */
	outb(bsr, iobase+BSR);
}

/*
 * Function nsc_ircc_pio_xmit (self, iobase)
 *
 *    Transmit data using PIO. Returns the number of bytes that actually
 *    got transfered
 *
 */
static int nsc_ircc_pio_write(int iobase, __u8 *buf, int len, int fifo_size)
{
	int actual = 0;
	__u8 bank;
	
	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	/* Save current bank */
	bank = inb(iobase+BSR);

	switch_bank(iobase, BANK0);
	if (!(inb_p(iobase+LSR) & LSR_TXEMP)) {
		IRDA_DEBUG(4, __FUNCTION__ 
			   "(), warning, FIFO not empty yet!\n");

		/* FIFO may still be filled to the Tx interrupt threshold */
		fifo_size -= 17;
	}

	/* Fill FIFO with current frame */
	while ((fifo_size-- > 0) && (actual < len)) {
		/* Transmit next byte */
		outb(buf[actual++], iobase+TXD);
	}
        
	IRDA_DEBUG(4, __FUNCTION__ "(), fifo_size %d ; %d sent of %d\n", 
		   fifo_size, actual, len);
	
	/* Restore bank */
	outb(bank, iobase+BSR);

	return actual;
}

/*
 * Function nsc_ircc_dma_xmit_complete (self)
 *
 *    The transfer of a frame in finished. This function will only be called 
 *    by the interrupt handler
 *
 */
static int nsc_ircc_dma_xmit_complete(struct nsc_ircc_cb *self)
{
	int iobase;
	__u8 bank;
	int ret = TRUE;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	iobase = self->io.fir_base;

	/* Save current bank */
	bank = inb(iobase+BSR);

	/* Disable DMA */
	switch_bank(iobase, BANK0);
        outb(inb(iobase+MCR) & ~MCR_DMA_EN, iobase+MCR);
	
	/* Check for underrrun! */
	if (inb(iobase+ASCR) & ASCR_TXUR) {
		self->stats.tx_errors++;
		self->stats.tx_fifo_errors++;
		
		/* Clear bit, by writing 1 into it */
		outb(ASCR_TXUR, iobase+ASCR);
	} else {
		self->stats.tx_packets++;
	}

	/* Check if we need to change the speed */
	if (self->new_speed) {
		nsc_ircc_change_speed(self, self->new_speed);
		self->new_speed = 0;
	}

	/* Finished with this frame, so prepare for next */
	self->tx_fifo.ptr++;
	self->tx_fifo.len--;

	/* Any frames to be sent back-to-back? */
	if (self->tx_fifo.len) {
		nsc_ircc_dma_xmit(self, iobase);
		
		/* Not finished yet! */
		ret = FALSE;
	} else {
		/* Reset Tx FIFO info */
		self->tx_fifo.len = self->tx_fifo.ptr = self->tx_fifo.free = 0;
		self->tx_fifo.tail = self->tx_buff.head;
	}

	/* Make sure we have room for more frames */
	if (self->tx_fifo.free < MAX_TX_WINDOW) {
		/* Not busy transmitting anymore */
		/* Tell the network layer, that we can accept more frames */
		netif_wake_queue(self->netdev);
	}

	/* Restore bank */
	outb(bank, iobase+BSR);
	
	return ret;
}

/*
 * Function nsc_ircc_dma_receive (self)
 *
 *    Get ready for receiving a frame. The device will initiate a DMA
 *    if it starts to receive a frame.
 *
 */
static int nsc_ircc_dma_receive(struct nsc_ircc_cb *self) 
{
	int iobase;
	__u8 bsr;

	iobase = self->io.fir_base;

	/* Reset Tx FIFO info */
	self->tx_fifo.len = self->tx_fifo.ptr = self->tx_fifo.free = 0;
	self->tx_fifo.tail = self->tx_buff.head;

	/* Save current bank */
	bsr = inb(iobase+BSR);

	/* Disable DMA */
	switch_bank(iobase, BANK0);
	outb(inb(iobase+MCR) & ~MCR_DMA_EN, iobase+MCR);

	/* Choose DMA Rx, DMA Fairness, and Advanced mode */
	switch_bank(iobase, BANK2);
	outb(ECR1_DMANF|ECR1_EXT_SL, iobase+ECR1);

	self->io.direction = IO_RECV;
	self->rx_buff.data = self->rx_buff.head;
	
	/* Reset Rx FIFO. This will also flush the ST_FIFO */
	switch_bank(iobase, BANK0);
	outb(FCR_RXSR|FCR_FIFO_EN, iobase+FCR);

	self->st_fifo.len = self->st_fifo.pending_bytes = 0;
	self->st_fifo.tail = self->st_fifo.head = 0;
	
	setup_dma(self->io.dma, self->rx_buff.data, self->rx_buff.truesize, 
		  DMA_RX_MODE);

	/* Enable DMA */
	switch_bank(iobase, BANK0);
	outb(inb(iobase+MCR)|MCR_DMA_EN, iobase+MCR);

	/* Restore bank register */
	outb(bsr, iobase+BSR);
	
	return 0;
}

/*
 * Function nsc_ircc_dma_receive_complete (self)
 *
 *    Finished with receiving frames
 *
 *    
 */
static int nsc_ircc_dma_receive_complete(struct nsc_ircc_cb *self, int iobase)
{
	struct st_fifo *st_fifo;
	struct sk_buff *skb;
	__u8 status;
	__u8 bank;
	int len;

	st_fifo = &self->st_fifo;

	/* Save current bank */
	bank = inb(iobase+BSR);
	
	/* Read all entries in status FIFO */
	switch_bank(iobase, BANK5);
	while ((status = inb(iobase+FRM_ST)) & FRM_ST_VLD) {
		/* We must empty the status FIFO no matter what */
		len = inb(iobase+RFLFL) | ((inb(iobase+RFLFH) & 0x1f) << 8);

		if (st_fifo->tail >= MAX_RX_WINDOW) {
			IRDA_DEBUG(0, __FUNCTION__ "(), window is full!\n");
			continue;
		}
			
		st_fifo->entries[st_fifo->tail].status = status;
		st_fifo->entries[st_fifo->tail].len = len;
		st_fifo->pending_bytes += len;
		st_fifo->tail++;
		st_fifo->len++;
	}
	/* Try to process all entries in status FIFO */
	while (st_fifo->len > 0) {
		/* Get first entry */
		status = st_fifo->entries[st_fifo->head].status;
		len    = st_fifo->entries[st_fifo->head].len;
		st_fifo->pending_bytes -= len;
		st_fifo->head++;
		st_fifo->len--;

		/* Check for errors */
		if (status & FRM_ST_ERR_MSK) {
			if (status & FRM_ST_LOST_FR) {
				/* Add number of lost frames to stats */
				self->stats.rx_errors += len;	
			} else {
				/* Skip frame */
				self->stats.rx_errors++;
				
				self->rx_buff.data += len;
			
				if (status & FRM_ST_MAX_LEN)
					self->stats.rx_length_errors++;
				
				if (status & FRM_ST_PHY_ERR) 
					self->stats.rx_frame_errors++;
				
				if (status & FRM_ST_BAD_CRC) 
					self->stats.rx_crc_errors++;
			}
			/* The errors below can be reported in both cases */
			if (status & FRM_ST_OVR1)
				self->stats.rx_fifo_errors++;		       
			
			if (status & FRM_ST_OVR2)
				self->stats.rx_fifo_errors++;
		} else {
			/*  
			 * First we must make sure that the frame we
			 * want to deliver is all in main memory. If we
			 * cannot tell, then we check if the Rx FIFO is
			 * empty. If not then we will have to take a nap
			 * and try again later.  
			 */
			if (st_fifo->pending_bytes < self->io.fifo_size) {
				switch_bank(iobase, BANK0);
				if (inb(iobase+LSR) & LSR_RXDA) {
					/* Put this entry back in fifo */
					st_fifo->head--;
					st_fifo->len++;
					st_fifo->pending_bytes += len;
					st_fifo->entries[st_fifo->head].status = status;
					st_fifo->entries[st_fifo->head].len = len;
					/*  
					 * DMA not finished yet, so try again 
					 * later, set timer value, resolution 
					 * 125 us 
					 */
					switch_bank(iobase, BANK4);
					outb(0x02, iobase+TMRL); /* x 125 us */
					outb(0x00, iobase+TMRH);

					/* Start timer */
					outb(IRCR1_TMR_EN, iobase+IRCR1);

					/* Restore bank register */
					outb(bank, iobase+BSR);
					
					return FALSE; /* I'll be back! */
				}
			}

			/* 
			 * Remember the time we received this frame, so we can
			 * reduce the min turn time a bit since we will know
			 * how much time we have used for protocol processing
			 */
			get_fast_time(&self->stamp);

			skb = dev_alloc_skb(len+1);
			if (skb == NULL)  {
				WARNING(__FUNCTION__ "(), memory squeeze, "
					"dropping frame.\n");
				self->stats.rx_dropped++;

				/* Restore bank register */
				outb(bank, iobase+BSR);

				return FALSE;
			}
			
			/* Make sure IP header gets aligned */
			skb_reserve(skb, 1); 

			/* Copy frame without CRC */
			if (self->io.speed < 4000000) {
				skb_put(skb, len-2);
				memcpy(skb->data, self->rx_buff.data, len-2);
			} else {
				skb_put(skb, len-4);
				memcpy(skb->data, self->rx_buff.data, len-4);
			}

			/* Move to next frame */
			self->rx_buff.data += len;
			self->stats.rx_bytes += len;
			self->stats.rx_packets++;

			skb->dev = self->netdev;
			skb->mac.raw  = skb->data;
			skb->protocol = htons(ETH_P_IRDA);
			netif_rx(skb);
		}
	}
	/* Restore bank register */
	outb(bank, iobase+BSR);

	return TRUE;
}

/*
 * Function nsc_ircc_pio_receive (self)
 *
 *    Receive all data in receiver FIFO
 *
 */
static void nsc_ircc_pio_receive(struct nsc_ircc_cb *self) 
{
	__u8 byte;
	int iobase;

	iobase = self->io.fir_base;
	
	/*  Receive all characters in Rx FIFO */
	do {
		byte = inb(iobase+RXD);
		async_unwrap_char(self->netdev, &self->stats, &self->rx_buff, 
				  byte);
	} while (inb(iobase+LSR) & LSR_RXDA); /* Data available */	
}

/*
 * Function nsc_ircc_sir_interrupt (self, eir)
 *
 *    Handle SIR interrupt
 *
 */
static void nsc_ircc_sir_interrupt(struct nsc_ircc_cb *self, int eir)
{
	int actual;

	/* Check if transmit FIFO is low on data */
	if (eir & EIR_TXLDL_EV) {
		/* Write data left in transmit buffer */
		actual = nsc_ircc_pio_write(self->io.fir_base, 
					   self->tx_buff.data, 
					   self->tx_buff.len, 
					   self->io.fifo_size);
		self->tx_buff.data += actual;
		self->tx_buff.len  -= actual;
		
		self->io.direction = IO_XMIT;

		/* Check if finished */
		if (self->tx_buff.len > 0)
			self->ier = IER_TXLDL_IE;
		else { 

			self->stats.tx_packets++;
			netif_wake_queue(self->netdev);
			self->ier = IER_TXEMP_IE;
		}
			
	}
	/* Check if transmission has completed */
	if (eir & EIR_TXEMP_EV) {
		/* Check if we need to change the speed? */
		if (self->new_speed) {
			IRDA_DEBUG(2, __FUNCTION__ "(), Changing speed!\n");
			nsc_ircc_change_speed(self, self->new_speed);
			self->new_speed = 0;

			/* Check if we are going to FIR */
			if (self->io.speed > 115200) {
				/* Should wait for status FIFO interrupt */
				self->ier = IER_SFIF_IE;

				/* No need to do anymore SIR stuff */
				return;
			}
		}
		/* Turn around and get ready to receive some data */
		self->io.direction = IO_RECV;
		self->ier = IER_RXHDL_IE;
	}

	/* Rx FIFO threshold or timeout */
	if (eir & EIR_RXHDL_EV) {
		nsc_ircc_pio_receive(self);

		/* Keep receiving */
		self->ier = IER_RXHDL_IE;
	}
}

/*
 * Function nsc_ircc_fir_interrupt (self, eir)
 *
 *    Handle MIR/FIR interrupt
 *
 */
static void nsc_ircc_fir_interrupt(struct nsc_ircc_cb *self, int iobase, 
				   int eir)
{
	__u8 bank;

	bank = inb(iobase+BSR);
	
	/* Status FIFO event*/
	if (eir & EIR_SFIF_EV) {
		/* Check if DMA has finished */
		if (nsc_ircc_dma_receive_complete(self, iobase)) {
			/* Wait for next status FIFO interrupt */
			self->ier = IER_SFIF_IE;
		} else {
			self->ier = IER_SFIF_IE | IER_TMR_IE;
		}
	} else if (eir & EIR_TMR_EV) { /* Timer finished */
		/* Disable timer */
		switch_bank(iobase, BANK4);
		outb(0, iobase+IRCR1);

		/* Clear timer event */
		switch_bank(iobase, BANK0);
		outb(ASCR_CTE, iobase+ASCR);

		/* Check if this is a Tx timer interrupt */
		if (self->io.direction == IO_XMIT) {
			nsc_ircc_dma_xmit(self, iobase);

			/* Interrupt on DMA */
			self->ier = IER_DMA_IE;
		} else {
			/* Check (again) if DMA has finished */
			if (nsc_ircc_dma_receive_complete(self, iobase)) {
				self->ier = IER_SFIF_IE;
			} else {
				self->ier = IER_SFIF_IE | IER_TMR_IE;
			}
		}
	} else if (eir & EIR_DMA_EV) {
		/* Finished with all transmissions? */
		if (nsc_ircc_dma_xmit_complete(self)) {		
			/* Check if there are more frames to be transmitted */
			if (irda_device_txqueue_empty(self->netdev)) {
				/* Prepare for receive */
				nsc_ircc_dma_receive(self);
			
				self->ier = IER_SFIF_IE;
			}
		} else {
			/*  Not finished yet, so interrupt on DMA again */
			self->ier = IER_DMA_IE;
		}
	}
	outb(bank, iobase+BSR);
}

/*
 * Function nsc_ircc_interrupt (irq, dev_id, regs)
 *
 *    An interrupt from the chip has arrived. Time to do some work
 *
 */
static void nsc_ircc_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct net_device *dev = (struct net_device *) dev_id;
	struct nsc_ircc_cb *self;
	__u8 bsr, eir;
	int iobase;

	if (!dev) {
		WARNING("%s: irq %d for unknown device.\n", driver_name, irq);
		return;
	}
	self = (struct nsc_ircc_cb *) dev->priv;

	spin_lock(&self->lock);	

	iobase = self->io.fir_base;

	bsr = inb(iobase+BSR); 	/* Save current bank */

	switch_bank(iobase, BANK0);	
	self->ier = inb(iobase+IER); 
	eir = inb(iobase+EIR) & self->ier; /* Mask out the interesting ones */ 

	outb(0, iobase+IER); /* Disable interrupts */
	
	if (eir) {
		/* Dispatch interrupt handler for the current speed */
		if (self->io.speed > 115200)
			nsc_ircc_fir_interrupt(self, iobase, eir);
		else
			nsc_ircc_sir_interrupt(self, eir);
	}
	
	outb(self->ier, iobase+IER); /* Restore interrupts */
	outb(bsr, iobase+BSR);       /* Restore bank register */

	spin_unlock(&self->lock);
}

/*
 * Function nsc_ircc_is_receiving (self)
 *
 *    Return TRUE is we are currently receiving a frame
 *
 */
static int nsc_ircc_is_receiving(struct nsc_ircc_cb *self)
{
	unsigned long flags;
	int status = FALSE;
	int iobase;
	__u8 bank;

	ASSERT(self != NULL, return FALSE;);

	spin_lock_irqsave(&self->lock, flags);

	if (self->io.speed > 115200) {
		iobase = self->io.fir_base;

		/* Check if rx FIFO is not empty */
		bank = inb(iobase+BSR);
		switch_bank(iobase, BANK2);
		if ((inb(iobase+RXFLV) & 0x3f) != 0) {
			/* We are receiving something */
			status =  TRUE;
		}
		outb(bank, iobase+BSR);
	} else 
		status = (self->rx_buff.state != OUTSIDE_FRAME);
	
	spin_unlock_irqrestore(&self->lock, flags);

	return status;
}

/*
 * Function nsc_ircc_net_init (dev)
 *
 *    Initialize network device
 *
 */
static int nsc_ircc_net_init(struct net_device *dev)
{
	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	/* Setup to be a normal IrDA network device driver */
	irda_device_setup(dev);

	/* Insert overrides below this line! */

	return 0;
}

/*
 * Function nsc_ircc_net_open (dev)
 *
 *    Start the device
 *
 */
static int nsc_ircc_net_open(struct net_device *dev)
{
	struct nsc_ircc_cb *self;
	int iobase;
	__u8 bank;
	
	IRDA_DEBUG(4, __FUNCTION__ "()\n");
	
	ASSERT(dev != NULL, return -1;);
	self = (struct nsc_ircc_cb *) dev->priv;
	
	ASSERT(self != NULL, return 0;);
	
	iobase = self->io.fir_base;
	
	if (request_irq(self->io.irq, nsc_ircc_interrupt, 0, dev->name, dev)) {
		WARNING("%s, unable to allocate irq=%d\n", driver_name, 
			self->io.irq);
		return -EAGAIN;
	}
	/*
	 * Always allocate the DMA channel after the IRQ, and clean up on 
	 * failure.
	 */
	if (request_dma(self->io.dma, dev->name)) {
		WARNING("%s, unable to allocate dma=%d\n", driver_name, 
			self->io.dma);
		free_irq(self->io.irq, self);
		return -EAGAIN;
	}
	
	/* Save current bank */
	bank = inb(iobase+BSR);
	
	/* turn on interrupts */
	switch_bank(iobase, BANK0);
	outb(IER_LS_IE | IER_RXHDL_IE, iobase+IER);

	/* Restore bank register */
	outb(bank, iobase+BSR);

	/* Ready to play! */

	netif_start_queue(dev);
	
	/* 
	 * Open new IrLAP layer instance, now that everything should be
	 * initialized properly 
	 */
	self->irlap = irlap_open(dev, &self->qos);

	MOD_INC_USE_COUNT;

	return 0;
}

/*
 * Function nsc_ircc_net_close (dev)
 *
 *    Stop the device
 *
 */
static int nsc_ircc_net_close(struct net_device *dev)
{
	struct nsc_ircc_cb *self;
	int iobase;
	__u8 bank;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");
	
	ASSERT(dev != NULL, return -1;);

	self = (struct nsc_ircc_cb *) dev->priv;
	ASSERT(self != NULL, return 0;);

	/* Stop device */
	netif_stop_queue(dev);
	
	/* Stop and remove instance of IrLAP */
	if (self->irlap)
		irlap_close(self->irlap);
	self->irlap = NULL;
	
	iobase = self->io.fir_base;

	disable_dma(self->io.dma);

	/* Save current bank */
	bank = inb(iobase+BSR);

	/* Disable interrupts */
	switch_bank(iobase, BANK0);
	outb(0, iobase+IER); 
       
	free_irq(self->io.irq, dev);
	free_dma(self->io.dma);

	/* Restore bank register */
	outb(bank, iobase+BSR);

	MOD_DEC_USE_COUNT;

	return 0;
}

/*
 * Function nsc_ircc_net_ioctl (dev, rq, cmd)
 *
 *    Process IOCTL commands for this device
 *
 */
static int nsc_ircc_net_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct if_irda_req *irq = (struct if_irda_req *) rq;
	struct nsc_ircc_cb *self;
	unsigned long flags;
	int ret = 0;

	ASSERT(dev != NULL, return -1;);

	self = dev->priv;

	ASSERT(self != NULL, return -1;);

	IRDA_DEBUG(2, __FUNCTION__ "(), %s, (cmd=0x%X)\n", dev->name, cmd);
	
	/* Disable interrupts & save flags */
	save_flags(flags);
	cli();
	
	switch (cmd) {
	case SIOCSBANDWIDTH: /* Set bandwidth */
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		nsc_ircc_change_speed(self, irq->ifr_baudrate);
		break;
	case SIOCSMEDIABUSY: /* Set media busy */
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		irda_device_set_media_busy(self->netdev, TRUE);
		break;
	case SIOCGRECEIVING: /* Check if we are receiving right now */
		irq->ifr_receiving = nsc_ircc_is_receiving(self);
		break;
	default:
		ret = -EOPNOTSUPP;
	}
	
	restore_flags(flags);
	
	return ret;
}

static struct net_device_stats *nsc_ircc_net_get_stats(struct net_device *dev)
{
	struct nsc_ircc_cb *self = (struct nsc_ircc_cb *) dev->priv;
	
	return &self->stats;
}

static void nsc_ircc_suspend(struct nsc_ircc_cb *self)
{
	MESSAGE("%s, Suspending\n", driver_name);

	if (self->io.suspended)
		return;

	nsc_ircc_net_close(self->netdev);

	self->io.suspended = 1;
}

static void nsc_ircc_wakeup(struct nsc_ircc_cb *self)
{
	int iobase;

	if (!self->io.suspended)
		return;

	iobase = self->io.fir_base;

	/* Switch to advanced mode */
	switch_bank(iobase, BANK2);
	outb(ECR1_EXT_SL, iobase+ECR1);
	switch_bank(iobase, BANK0);

	nsc_ircc_net_open(self->netdev);
	
	MESSAGE("%s, Waking up\n", driver_name);

	self->io.suspended = 0;
}

static int nsc_ircc_pmproc(struct pm_dev *dev, pm_request_t rqst, void *data)
{
        struct nsc_ircc_cb *self = (struct nsc_ircc_cb*) dev->data;
        if (self) {
                switch (rqst) {
                case PM_SUSPEND:
                        nsc_ircc_suspend(self);
                        break;
                case PM_RESUME:
                        nsc_ircc_wakeup(self);
                        break;
                }
        }
	return 0;
}

#ifdef MODULE
MODULE_AUTHOR("Dag Brattli <dagb@cs.uit.no>");
MODULE_DESCRIPTION("NSC IrDA Device Driver");

MODULE_PARM(qos_mtt_bits, "i");
MODULE_PARM_DESC(qos_mtt_bits, "Minimum Turn Time");
MODULE_PARM(io,  "1-4i");
MODULE_PARM_DESC(io, "Base I/O addresses");
MODULE_PARM(irq, "1-4i");
MODULE_PARM_DESC(irq, "IRQ lines");
MODULE_PARM(dma, "1-4i");
MODULE_PARM_DESC(dma, "DMA channels");
MODULE_PARM(dongle_id, "i");
MODULE_PARM_DESC(dongle_id, "Type-id of used dongle");

int init_module(void)
{
	return nsc_ircc_init();
}

void cleanup_module(void)
{
	nsc_ircc_cleanup();
}
#endif /* MODULE */

