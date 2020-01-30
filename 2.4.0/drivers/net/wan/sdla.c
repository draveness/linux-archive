/*
 * SDLA		An implementation of a driver for the Sangoma S502/S508 series
 *		multi-protocol PC interface card.  Initial offering is with 
 *		the DLCI driver, providing Frame Relay support for linux.
 *
 *		Global definitions for the Frame relay interface.
 *
 * Version:	@(#)sdla.c   0.30	12 Sep 1996
 *
 * Credits:	Sangoma Technologies, for the use of 2 cards for an extended
 *			period of time.
 *		David Mandelstam <dm@sangoma.com> for getting me started on 
 *			this project, and incentive to complete it.
 *		Gene Kozen <74604.152@compuserve.com> for providing me with
 *			important information about the cards.
 *
 * Author:	Mike McLagan <mike.mclagan@linux.org>
 *
 * Changes:
 *		0.15	Mike McLagan	Improved error handling, packet dropping
 *		0.20	Mike McLagan	New transmit/receive flags for config
 *					If in FR mode, don't accept packets from
 *					non DLCI devices.
 *		0.25	Mike McLagan	Fixed problem with rejecting packets
 *					from non DLCI devices.
 *		0.30	Mike McLagan	Fixed kernel panic when used with modified
 *					ifconfig
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <linux/config.h> /* for CONFIG_DLCI_MAX */
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/init.h>

#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <asm/uaccess.h>

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/if_frad.h>

#include <linux/sdla.h>

static const char* version = "SDLA driver v0.30, 12 Sep 1996, mike.mclagan@linux.org";

static const char* devname = "sdla";

static unsigned int valid_port[] __initdata = { 0x250, 0x270, 0x280, 0x300, 0x350, 0x360, 0x380, 0x390};

static unsigned int valid_mem[]  __initdata = {
				    0xA0000, 0xA2000, 0xA4000, 0xA6000, 0xA8000, 0xAA000, 0xAC000, 0xAE000, 
                                    0xB0000, 0xB2000, 0xB4000, 0xB6000, 0xB8000, 0xBA000, 0xBC000, 0xBE000,
                                    0xC0000, 0xC2000, 0xC4000, 0xC6000, 0xC8000, 0xCA000, 0xCC000, 0xCE000,
                                    0xD0000, 0xD2000, 0xD4000, 0xD6000, 0xD8000, 0xDA000, 0xDC000, 0xDE000,
                                    0xE0000, 0xE2000, 0xE4000, 0xE6000, 0xE8000, 0xEA000, 0xEC000, 0xEE000}; 

/*********************************************************
 *
 * these are the core routines that access the card itself 
 *
 *********************************************************/

#define SDLA_WINDOW(dev,addr) outb((((addr) >> 13) & 0x1F), (dev)->base_addr + SDLA_REG_Z80_WINDOW)

static void sdla_read(struct net_device *dev, int addr, void *buf, short len)
{
	unsigned long flags;
	char          *temp, *base;
	int           offset, bytes;

	temp = buf;
	while(len)
	{	
		offset = addr & SDLA_ADDR_MASK;
		bytes = offset + len > SDLA_WINDOW_SIZE ? SDLA_WINDOW_SIZE - offset : len;
		base = (void *) (dev->mem_start + offset);

		save_flags(flags);
		cli();
		SDLA_WINDOW(dev, addr);
		memcpy(temp, base, bytes);
		restore_flags(flags);

		addr += bytes;
		temp += bytes;
		len  -= bytes;
	}  
}

static void sdla_write(struct net_device *dev, int addr, void *buf, short len)
{
	unsigned long flags;
	char          *temp, *base;
	int           offset, bytes;

	temp = buf;
	while(len)
	{
		offset = addr & SDLA_ADDR_MASK;
		bytes = offset + len > SDLA_WINDOW_SIZE ? SDLA_WINDOW_SIZE - offset : len;
		base = (void *) (dev->mem_start + offset);
		save_flags(flags);
		cli();
		SDLA_WINDOW(dev, addr);
		memcpy(base, temp, bytes);
		restore_flags(flags);
		addr += bytes;
		temp += bytes;
		len  -= bytes;
	}
}

static void sdla_clear(struct net_device *dev)
{
	unsigned long flags;
	char          *base;
	int           len, addr, bytes;

	len = 65536;	
	addr = 0;
	bytes = SDLA_WINDOW_SIZE;
	base = (void *) dev->mem_start;

	save_flags(flags);
	cli();
	while(len)
	{
		SDLA_WINDOW(dev, addr);
		memset(base, 0, bytes);

		addr += bytes;
		len  -= bytes;
	}
	restore_flags(flags);
}

static char sdla_byte(struct net_device *dev, int addr)
{
	unsigned long flags;
	char          byte, *temp;

	temp = (void *) (dev->mem_start + (addr & SDLA_ADDR_MASK));

	save_flags(flags);
	cli();
	SDLA_WINDOW(dev, addr);
	byte = *temp;
	restore_flags(flags);

	return(byte);
}

void sdla_stop(struct net_device *dev)
{
	struct frad_local *flp;

	flp = dev->priv;
	switch(flp->type)
	{
		case SDLA_S502A:
			outb(SDLA_S502A_HALT, dev->base_addr + SDLA_REG_CONTROL);
			flp->state = SDLA_HALT;
			break;
		case SDLA_S502E:
			outb(SDLA_HALT, dev->base_addr + SDLA_REG_Z80_CONTROL);
			outb(SDLA_S502E_ENABLE, dev->base_addr + SDLA_REG_CONTROL);
			flp->state = SDLA_S502E_ENABLE;
			break;
		case SDLA_S507:
			flp->state &= ~SDLA_CPUEN;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			break;
		case SDLA_S508:
			flp->state &= ~SDLA_CPUEN;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			break;
	}
}

void sdla_start(struct net_device *dev)
{
	struct frad_local *flp;

	flp = dev->priv;
	switch(flp->type)
	{
		case SDLA_S502A:
			outb(SDLA_S502A_NMI, dev->base_addr + SDLA_REG_CONTROL);
			outb(SDLA_S502A_START, dev->base_addr + SDLA_REG_CONTROL);
			flp->state = SDLA_S502A_START;
			break;
		case SDLA_S502E:
			outb(SDLA_S502E_CPUEN, dev->base_addr + SDLA_REG_Z80_CONTROL);
			outb(0x00, dev->base_addr + SDLA_REG_CONTROL);
			flp->state = 0;
			break;
		case SDLA_S507:
			flp->state |= SDLA_CPUEN;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			break;
		case SDLA_S508:
			flp->state |= SDLA_CPUEN;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			break;
	}
}

/****************************************************
 *
 * this is used for the S502A/E cards to determine
 * the speed of the onboard CPU.  Calibration is
 * necessary for the Frame Relay code uploaded 
 * later.  Incorrect results cause timing problems
 * with link checks & status messages
 *
 ***************************************************/

int sdla_z80_poll(struct net_device *dev, int z80_addr, int jiffs, char resp1, char resp2)
{
	unsigned long start, done, now;
	char          resp, *temp;

	start = now = jiffies;
	done = jiffies + jiffs;

	temp = (void *)dev->mem_start;
	temp += z80_addr & SDLA_ADDR_MASK;
	
	resp = ~resp1;
	while (time_before(jiffies, done) && (resp != resp1) && (!resp2 || (resp != resp2)))
	{
		if (jiffies != now)
		{
			SDLA_WINDOW(dev, z80_addr);
			now = jiffies;
			resp = *temp;
		}
	}
	return(time_before(jiffies, done) ? jiffies - start : -1);
}

/* constants for Z80 CPU speed */
#define Z80_READY 		'1'	/* Z80 is ready to begin */
#define LOADER_READY 		'2'	/* driver is ready to begin */
#define Z80_SCC_OK 		'3'	/* SCC is on board */
#define Z80_SCC_BAD	 	'4'	/* SCC was not found */

static int sdla_cpuspeed(struct net_device *dev, struct ifreq *ifr)
{
	int  jiffs;
	char data;

	sdla_start(dev);
	if (sdla_z80_poll(dev, 0, 3*HZ, Z80_READY, 0) < 0)
		return(-EIO);

	data = LOADER_READY;
	sdla_write(dev, 0, &data, 1);

	if ((jiffs = sdla_z80_poll(dev, 0, 8*HZ, Z80_SCC_OK, Z80_SCC_BAD)) < 0)
		return(-EIO);

	sdla_stop(dev);
	sdla_read(dev, 0, &data, 1);

	if (data == Z80_SCC_BAD)
	{
		printk("%s: SCC bad\n", dev->name);
		return(-EIO);
	}

	if (data != Z80_SCC_OK)
		return(-EINVAL);

	if (jiffs < 165)
		ifr->ifr_mtu = SDLA_CPU_16M;
	else if (jiffs < 220)
		ifr->ifr_mtu = SDLA_CPU_10M;
	else if (jiffs < 258)
		ifr->ifr_mtu = SDLA_CPU_8M;
	else if (jiffs < 357)
		ifr->ifr_mtu = SDLA_CPU_7M;
	else if (jiffs < 467)
		ifr->ifr_mtu = SDLA_CPU_5M;
	else
		ifr->ifr_mtu = SDLA_CPU_3M;
 
	return(0);
}

/************************************************
 *
 *  Direct interaction with the Frame Relay code 
 *  starts here.
 *
 ************************************************/

struct _dlci_stat 
{
	short dlci		__attribute__((packed));
	char  flags		__attribute__((packed));
};

struct _frad_stat 
{
	char    flags;
	struct _dlci_stat dlcis[SDLA_MAX_DLCI];
};

static void sdla_errors(struct net_device *dev, int cmd, int dlci, int ret, int len, void *data) 
{
	struct _dlci_stat *pstatus;
	short             *pdlci;
	int               i;
	char              *state, line[30];

	switch (ret)
	{
		case SDLA_RET_MODEM:
			state = data;
			if (*state & SDLA_MODEM_DCD_LOW)
				printk(KERN_INFO "%s: Modem DCD unexpectedly low!\n", dev->name);
			if (*state & SDLA_MODEM_CTS_LOW)
				printk(KERN_INFO "%s: Modem CTS unexpectedly low!\n", dev->name);
			/* I should probably do something about this! */
			break;

		case SDLA_RET_CHANNEL_OFF:
			printk(KERN_INFO "%s: Channel became inoperative!\n", dev->name);
			/* same here */
			break;

		case SDLA_RET_CHANNEL_ON:
			printk(KERN_INFO "%s: Channel became operative!\n", dev->name);
			/* same here */
			break;

		case SDLA_RET_DLCI_STATUS:
			printk(KERN_INFO "%s: Status change reported by Access Node.\n", dev->name);
			len /= sizeof(struct _dlci_stat);
			for(pstatus = data, i=0;i < len;i++,pstatus++)
			{
				if (pstatus->flags & SDLA_DLCI_NEW)
					state = "new";
				else if (pstatus->flags & SDLA_DLCI_DELETED)
					state = "deleted";
				else if (pstatus->flags & SDLA_DLCI_ACTIVE)
					state = "active";
				else
				{
					sprintf(line, "unknown status: %02X", pstatus->flags);
					state = line;
				}
				printk(KERN_INFO "%s: DLCI %i: %s.\n", dev->name, pstatus->dlci, state);
				/* same here */
			}
			break;

		case SDLA_RET_DLCI_UNKNOWN:
			printk(KERN_INFO "%s: Received unknown DLCIs:", dev->name);
			len /= sizeof(short);
			for(pdlci = data,i=0;i < len;i++,pdlci++)
				printk(" %i", *pdlci);
			printk("\n");
			break;

		case SDLA_RET_TIMEOUT:
			printk(KERN_ERR "%s: Command timed out!\n", dev->name);
			break;

		case SDLA_RET_BUF_OVERSIZE:
			printk(KERN_INFO "%s: Bc/CIR overflow, acceptable size is %i\n", dev->name, len);
			break;

		case SDLA_RET_BUF_TOO_BIG:
			printk(KERN_INFO "%s: Buffer size over specified max of %i\n", dev->name, len);
			break;

		case SDLA_RET_CHANNEL_INACTIVE:
		case SDLA_RET_DLCI_INACTIVE:
		case SDLA_RET_CIR_OVERFLOW:
		case SDLA_RET_NO_BUFS:
			if (cmd == SDLA_INFORMATION_WRITE)
				break;

		default: 
			printk(KERN_DEBUG "%s: Cmd 0x%2.2X generated return code 0x%2.2X\n", dev->name, cmd, ret);
			/* Further processing could be done here */
			break;
	}
}

static int sdla_cmd(struct net_device *dev, int cmd, short dlci, short flags, 
                        void *inbuf, short inlen, void *outbuf, short *outlen)
{
	static struct _frad_stat status;
	struct frad_local        *flp;
	struct sdla_cmd          *cmd_buf;
	unsigned long            pflags;
	int                      jiffs, ret, waiting, len;
	long                     window;

	flp = dev->priv;
	window = flp->type == SDLA_S508 ? SDLA_508_CMD_BUF : SDLA_502_CMD_BUF;
	cmd_buf = (struct sdla_cmd *)(dev->mem_start + (window & SDLA_ADDR_MASK));
	ret = 0;
	len = 0;
	jiffs = jiffies + HZ;  /* 1 second is plenty */
	save_flags(pflags);
	cli();
	SDLA_WINDOW(dev, window);
	cmd_buf->cmd = cmd;
	cmd_buf->dlci = dlci;
	cmd_buf->flags = flags;

	if (inbuf)
		memcpy(cmd_buf->data, inbuf, inlen);

	cmd_buf->length = inlen;

	cmd_buf->opp_flag = 1;
	restore_flags(pflags);

	waiting = 1;
	len = 0;
	while (waiting && time_before_eq(jiffies, jiffs))
	{
		if (waiting++ % 3) 
		{
			save_flags(pflags);
			cli();
			SDLA_WINDOW(dev, window);
			waiting = ((volatile int)(cmd_buf->opp_flag));
			restore_flags(pflags);
		}
	}
	
	if (!waiting)
	{
		save_flags(pflags);
		cli();
		SDLA_WINDOW(dev, window);
		ret = cmd_buf->retval;
		len = cmd_buf->length;
		if (outbuf && outlen)
		{
			*outlen = *outlen >= len ? len : *outlen;

			if (*outlen)
				memcpy(outbuf, cmd_buf->data, *outlen);
		}

		/* This is a local copy that's used for error handling */
		if (ret)
			memcpy(&status, cmd_buf->data, len > sizeof(status) ? sizeof(status) : len);

		restore_flags(pflags);
	}
	else
		ret = SDLA_RET_TIMEOUT;

	if (ret != SDLA_RET_OK)
	   	sdla_errors(dev, cmd, dlci, ret, len, &status);

	return(ret);
}

/***********************************************
 *
 * these functions are called by the DLCI driver 
 *
 ***********************************************/

static int sdla_reconfig(struct net_device *dev);

int sdla_activate(struct net_device *slave, struct net_device *master)
{
	struct frad_local *flp;
	int i;

	flp = slave->priv;

	for(i=0;i<CONFIG_DLCI_MAX;i++)
		if (flp->master[i] == master)
			break;

	if (i == CONFIG_DLCI_MAX)
		return(-ENODEV);

	flp->dlci[i] = abs(flp->dlci[i]);

	if (netif_running(slave) && (flp->config.station == FRAD_STATION_NODE))
		sdla_cmd(slave, SDLA_ACTIVATE_DLCI, 0, 0, &flp->dlci[i], sizeof(short), NULL, NULL);

	return(0);
}

int sdla_deactivate(struct net_device *slave, struct net_device *master)
{
	struct frad_local *flp;
	int               i;

	flp = slave->priv;

	for(i=0;i<CONFIG_DLCI_MAX;i++)
		if (flp->master[i] == master)
			break;

	if (i == CONFIG_DLCI_MAX)
		return(-ENODEV);

	flp->dlci[i] = -abs(flp->dlci[i]);

	if (netif_running(slave) && (flp->config.station == FRAD_STATION_NODE))
		sdla_cmd(slave, SDLA_DEACTIVATE_DLCI, 0, 0, &flp->dlci[i], sizeof(short), NULL, NULL);

	return(0);
}

int sdla_assoc(struct net_device *slave, struct net_device *master)
{
	struct frad_local *flp;
	int               i;

	if (master->type != ARPHRD_DLCI)
		return(-EINVAL);

	flp = slave->priv;

	for(i=0;i<CONFIG_DLCI_MAX;i++)
	{
		if (!flp->master[i])
			break;
		if (abs(flp->dlci[i]) == *(short *)(master->dev_addr))
			return(-EADDRINUSE);
	} 

	if (i == CONFIG_DLCI_MAX)
		return(-EMLINK);  /* #### Alan: Comments on this ?? */

	MOD_INC_USE_COUNT;

	flp->master[i] = master;
	flp->dlci[i] = -*(short *)(master->dev_addr);
	master->mtu = slave->mtu;

	if (netif_running(slave)) {
		if (flp->config.station == FRAD_STATION_CPE)
			sdla_reconfig(slave);
		else
			sdla_cmd(slave, SDLA_ADD_DLCI, 0, 0, master->dev_addr, sizeof(short), NULL, NULL);
	}

	return(0);
}

int sdla_deassoc(struct net_device *slave, struct net_device *master)
{
	struct frad_local *flp;
	int               i;

	flp = slave->priv;

	for(i=0;i<CONFIG_DLCI_MAX;i++)
		if (flp->master[i] == master)
			break;

	if (i == CONFIG_DLCI_MAX)
		return(-ENODEV);

	flp->master[i] = NULL;
	flp->dlci[i] = 0;

	MOD_DEC_USE_COUNT;

	if (netif_running(slave)) {
		if (flp->config.station == FRAD_STATION_CPE)
			sdla_reconfig(slave);
		else
			sdla_cmd(slave, SDLA_DELETE_DLCI, 0, 0, master->dev_addr, sizeof(short), NULL, NULL);
	}

	return(0);
}

int sdla_dlci_conf(struct net_device *slave, struct net_device *master, int get)
{
	struct frad_local *flp;
	struct dlci_local *dlp;
	int               i;
	short             len, ret;

	flp = slave->priv;

	for(i=0;i<CONFIG_DLCI_MAX;i++)
		if (flp->master[i] == master)
			break;

	if (i == CONFIG_DLCI_MAX)
		return(-ENODEV);

	dlp = master->priv;

	ret = SDLA_RET_OK;
	len = sizeof(struct dlci_conf);
	if (netif_running(slave)) {
		if (get)
			ret = sdla_cmd(slave, SDLA_READ_DLCI_CONFIGURATION, abs(flp->dlci[i]), 0,  
			            NULL, 0, &dlp->config, &len);
		else
			ret = sdla_cmd(slave, SDLA_SET_DLCI_CONFIGURATION, abs(flp->dlci[i]), 0,  
			            &dlp->config, sizeof(struct dlci_conf) - 4 * sizeof(short), NULL, NULL);
	}

	return(ret == SDLA_RET_OK ? 0 : -EIO);
}

/**************************
 *
 * now for the Linux driver 
 *
 **************************/

/* NOTE: the DLCI driver deals with freeing the SKB!! */
static int sdla_transmit(struct sk_buff *skb, struct net_device *dev)
{
	struct frad_local *flp;
	int               ret, addr, accept, i;
	short             size;
	unsigned long     flags;
	struct buf_entry  *pbuf;

	flp = dev->priv;
	ret = 0;
	accept = 1;

	netif_stop_queue(dev);

	/*
	 * stupid GateD insists on setting up the multicast router thru us
	 * and we're ill equipped to handle a non Frame Relay packet at this
	 * time!
	 */

	accept = 1;
	switch (dev->type)
	{
		case ARPHRD_FRAD:
			if (skb->dev->type != ARPHRD_DLCI)
			{
				printk(KERN_WARNING "%s: Non DLCI device, type %i, tried to send on FRAD module.\n", dev->name, skb->dev->type);
				accept = 0;
			}
			break;
		default:
			printk(KERN_WARNING "%s: unknown firmware type 0x%4.4X\n", dev->name, dev->type);
			accept = 0;
			break;
	}
	if (accept)
	{
		/* this is frame specific, but till there's a PPP module, it's the default */
		switch (flp->type)
		{
			case SDLA_S502A:
			case SDLA_S502E:
				ret = sdla_cmd(dev, SDLA_INFORMATION_WRITE, *(short *)(skb->dev->dev_addr), 0, skb->data, skb->len, NULL, NULL);
				break;
				case SDLA_S508:
				size = sizeof(addr);
				ret = sdla_cmd(dev, SDLA_INFORMATION_WRITE, *(short *)(skb->dev->dev_addr), 0, NULL, skb->len, &addr, &size);
				if (ret == SDLA_RET_OK)
				{
					save_flags(flags); 
					cli();
					SDLA_WINDOW(dev, addr);
					pbuf = (void *)(((int) dev->mem_start) + (addr & SDLA_ADDR_MASK));
						sdla_write(dev, pbuf->buf_addr, skb->data, skb->len);
						SDLA_WINDOW(dev, addr);
					pbuf->opp_flag = 1;
					restore_flags(flags);
				}
				break;
		}
		switch (ret)
		{
			case SDLA_RET_OK:
				flp->stats.tx_packets++;
				ret = DLCI_RET_OK;
				break;

			case SDLA_RET_CIR_OVERFLOW:
			case SDLA_RET_BUF_OVERSIZE:
			case SDLA_RET_NO_BUFS:
				flp->stats.tx_dropped++;
				ret = DLCI_RET_DROP;
				break;

			default:
				flp->stats.tx_errors++;
				ret = DLCI_RET_ERR;
				break;
		}
	}
	netif_wake_queue(dev);
	for(i=0;i<CONFIG_DLCI_MAX;i++)
	{
		if(flp->master[i]!=NULL)
			netif_wake_queue(flp->master[i]);
	}		
	return(ret);
}

static void sdla_receive(struct net_device *dev)
{
	struct net_device	  *master;
	struct frad_local *flp;
	struct dlci_local *dlp;
	struct sk_buff	 *skb;

	struct sdla_cmd	*cmd;
	struct buf_info	*pbufi;
	struct buf_entry  *pbuf;

	unsigned long	  flags;
	int               i, received, success, addr, buf_base, buf_top;
	short             dlci, len, len2, split;

	flp = dev->priv;
	success = 1;
	received = addr = buf_top = buf_base = 0;
	len = dlci = 0;
	skb = NULL;
	master = NULL;
	cmd = NULL;
	pbufi = NULL;
	pbuf = NULL;

	save_flags(flags);
	cli();

	switch (flp->type)
	{
		case SDLA_S502A:
		case SDLA_S502E:
			cmd = (void *) (dev->mem_start + (SDLA_502_RCV_BUF & SDLA_ADDR_MASK));
			SDLA_WINDOW(dev, SDLA_502_RCV_BUF);
			success = cmd->opp_flag;
			if (!success)
				break;

			dlci = cmd->dlci;
			len = cmd->length;
			break;

		case SDLA_S508:
			pbufi = (void *) (dev->mem_start + (SDLA_508_RXBUF_INFO & SDLA_ADDR_MASK));
			SDLA_WINDOW(dev, SDLA_508_RXBUF_INFO);
			pbuf = (void *) (dev->mem_start + ((pbufi->rse_base + flp->buffer * sizeof(struct buf_entry)) & SDLA_ADDR_MASK));
			success = pbuf->opp_flag;
			if (!success)
				break;

			buf_top = pbufi->buf_top;
			buf_base = pbufi->buf_base;
			dlci = pbuf->dlci;
			len = pbuf->length;
			addr = pbuf->buf_addr;
			break;
	}

	/* common code, find the DLCI and get the SKB */
	if (success)
	{
		for (i=0;i<CONFIG_DLCI_MAX;i++)
			if (flp->dlci[i] == dlci)
				break;

		if (i == CONFIG_DLCI_MAX)
		{
			printk(KERN_NOTICE "%s: Received packet from invalid DLCI %i, ignoring.", dev->name, dlci);
			flp->stats.rx_errors++;
			success = 0;
		}
	}

	if (success)
	{
		master = flp->master[i];
		skb = dev_alloc_skb(len + sizeof(struct frhdr));
		if (skb == NULL) 
		{
			printk(KERN_NOTICE "%s: Memory squeeze, dropping packet.\n", dev->name);
			flp->stats.rx_dropped++; 
			success = 0;
		}
		else
			skb_reserve(skb, sizeof(struct frhdr));
	}

	/* pick up the data */
	switch (flp->type)
	{
		case SDLA_S502A:
		case SDLA_S502E:
			if (success)
				sdla_read(dev, SDLA_502_RCV_BUF + SDLA_502_DATA_OFS, skb_put(skb,len), len);

			SDLA_WINDOW(dev, SDLA_502_RCV_BUF);
			cmd->opp_flag = 0;
			break;

		case SDLA_S508:
			if (success)
			{
				/* is this buffer split off the end of the internal ring buffer */
				split = addr + len > buf_top + 1 ? len - (buf_top - addr + 1) : 0;
				len2 = len - split;

				sdla_read(dev, addr, skb_put(skb, len2), len2);
				if (split)
					sdla_read(dev, buf_base, skb_put(skb, split), split);
			}

			/* increment the buffer we're looking at */
			SDLA_WINDOW(dev, SDLA_508_RXBUF_INFO);
			flp->buffer = (flp->buffer + 1) % pbufi->rse_num;
			pbuf->opp_flag = 0;
			break;
	}

	if (success)
	{
		flp->stats.rx_packets++;
		dlp = master->priv;
		(*dlp->receive)(skb, master);
	}

	restore_flags(flags);
}

static void sdla_isr(int irq, void *dev_id, struct pt_regs * regs)
{
	struct net_device     *dev;
	struct frad_local *flp;
	char              byte;

	dev = dev_id;

	if (dev == NULL)
	{
		printk(KERN_WARNING "sdla_isr(): irq %d for unknown device.\n", irq);
		return;
	}

	flp = dev->priv;

	if (!flp->initialized)
	{
		printk(KERN_WARNING "%s: irq %d for uninitialized device.\n", dev->name, irq);
		return;
	}

	byte = sdla_byte(dev, flp->type == SDLA_S508 ? SDLA_508_IRQ_INTERFACE : SDLA_502_IRQ_INTERFACE);
	switch (byte)
	{
		case SDLA_INTR_RX:
			sdla_receive(dev);
			break;

		/* the command will get an error return, which is processed above */
		case SDLA_INTR_MODEM:
		case SDLA_INTR_STATUS:
			sdla_cmd(dev, SDLA_READ_DLC_STATUS, 0, 0, NULL, 0, NULL, NULL);
			break;

		case SDLA_INTR_TX:
		case SDLA_INTR_COMPLETE:
		case SDLA_INTR_TIMER:
			printk(KERN_WARNING "%s: invalid irq flag 0x%02X.\n", dev->name, byte);
			break;
	}

	/* the S502E requires a manual acknowledgement of the interrupt */ 
	if (flp->type == SDLA_S502E)
	{
		flp->state &= ~SDLA_S502E_INTACK;
		outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
		flp->state |= SDLA_S502E_INTACK;
		outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
	}

	/* this clears the byte, informing the Z80 we're done */
	byte = 0;
	sdla_write(dev, flp->type == SDLA_S508 ? SDLA_508_IRQ_INTERFACE : SDLA_502_IRQ_INTERFACE, &byte, sizeof(byte));
}

static void sdla_poll(unsigned long device)
{
	struct net_device	  *dev;
	struct frad_local *flp;

	dev = (struct net_device *) device;
	flp = dev->priv;

	if (sdla_byte(dev, SDLA_502_RCV_BUF))
		sdla_receive(dev);

	flp->timer.expires = 1;
	add_timer(&flp->timer);
}

static int sdla_close(struct net_device *dev)
{
	struct frad_local *flp;
	struct intr_info  intr;
	int               len, i;
	short             dlcis[CONFIG_DLCI_MAX];

	flp = dev->priv;

	len = 0;
	for(i=0;i<CONFIG_DLCI_MAX;i++)
		if (flp->dlci[i])
			dlcis[len++] = abs(flp->dlci[i]);
	len *= 2;

	if (flp->config.station == FRAD_STATION_NODE)
	{
		for(i=0;i<CONFIG_DLCI_MAX;i++)
			if (flp->dlci[i] > 0) 
				sdla_cmd(dev, SDLA_DEACTIVATE_DLCI, 0, 0, dlcis, len, NULL, NULL);
		sdla_cmd(dev, SDLA_DELETE_DLCI, 0, 0, &flp->dlci[i], sizeof(flp->dlci[i]), NULL, NULL);
	}

	memset(&intr, 0, sizeof(intr));
	/* let's start up the reception */
	switch(flp->type)
	{
		case SDLA_S502A:
			del_timer(&flp->timer); 
			break;

		case SDLA_S502E:
			sdla_cmd(dev, SDLA_SET_IRQ_TRIGGER, 0, 0, &intr, sizeof(char) + sizeof(short), NULL, NULL);
			flp->state &= ~SDLA_S502E_INTACK;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			break;

		case SDLA_S507:
			break;

		case SDLA_S508:
			sdla_cmd(dev, SDLA_SET_IRQ_TRIGGER, 0, 0, &intr, sizeof(struct intr_info), NULL, NULL);
			flp->state &= ~SDLA_S508_INTEN;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			break;
	}

	sdla_cmd(dev, SDLA_DISABLE_COMMUNICATIONS, 0, 0, NULL, 0, NULL, NULL);

	netif_stop_queue(dev);
	
	MOD_DEC_USE_COUNT;

	return(0);
}

struct conf_data {
	struct frad_conf config;
	short            dlci[CONFIG_DLCI_MAX];
};

static int sdla_open(struct net_device *dev)
{
	struct frad_local *flp;
	struct dlci_local *dlp;
	struct conf_data  data;
	struct intr_info  intr;
	int               len, i;
	char              byte;

	flp = dev->priv;

	if (!flp->initialized)
		return(-EPERM);

	if (!flp->configured)
		return(-EPERM);

	/* time to send in the configuration */
	len = 0;
	for(i=0;i<CONFIG_DLCI_MAX;i++)
		if (flp->dlci[i])
			data.dlci[len++] = abs(flp->dlci[i]);
	len *= 2;

	memcpy(&data.config, &flp->config, sizeof(struct frad_conf));
	len += sizeof(struct frad_conf);

	sdla_cmd(dev, SDLA_DISABLE_COMMUNICATIONS, 0, 0, NULL, 0, NULL, NULL);
	sdla_cmd(dev, SDLA_SET_DLCI_CONFIGURATION, 0, 0, &data, len, NULL, NULL);

	if (flp->type == SDLA_S508)
		flp->buffer = 0;

	sdla_cmd(dev, SDLA_ENABLE_COMMUNICATIONS, 0, 0, NULL, 0, NULL, NULL);

	/* let's start up the reception */
	memset(&intr, 0, sizeof(intr));
	switch(flp->type)
	{
		case SDLA_S502A:
			flp->timer.expires = 1;
			add_timer(&flp->timer);
			break;

		case SDLA_S502E:
			flp->state |= SDLA_S502E_ENABLE;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			flp->state |= SDLA_S502E_INTACK;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			byte = 0;
			sdla_write(dev, SDLA_502_IRQ_INTERFACE, &byte, sizeof(byte));
			intr.flags = SDLA_INTR_RX | SDLA_INTR_STATUS | SDLA_INTR_MODEM;
			sdla_cmd(dev, SDLA_SET_IRQ_TRIGGER, 0, 0, &intr, sizeof(char) + sizeof(short), NULL, NULL);
			break;

		case SDLA_S507:
			break;

		case SDLA_S508:
			flp->state |= SDLA_S508_INTEN;
			outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);
			byte = 0;
			sdla_write(dev, SDLA_508_IRQ_INTERFACE, &byte, sizeof(byte));
			intr.flags = SDLA_INTR_RX | SDLA_INTR_STATUS | SDLA_INTR_MODEM;
			intr.irq = dev->irq;
			sdla_cmd(dev, SDLA_SET_IRQ_TRIGGER, 0, 0, &intr, sizeof(struct intr_info), NULL, NULL);
			break;
	}

	if (flp->config.station == FRAD_STATION_CPE)
	{
		byte = SDLA_ICS_STATUS_ENQ;
		sdla_cmd(dev, SDLA_ISSUE_IN_CHANNEL_SIGNAL, 0, 0, &byte, sizeof(byte), NULL, NULL);
	}
	else
	{
		sdla_cmd(dev, SDLA_ADD_DLCI, 0, 0, data.dlci, len - sizeof(struct frad_conf), NULL, NULL);
		for(i=0;i<CONFIG_DLCI_MAX;i++)
			if (flp->dlci[i] > 0)
				sdla_cmd(dev, SDLA_ACTIVATE_DLCI, 0, 0, &flp->dlci[i], 2*sizeof(flp->dlci[i]), NULL, NULL);
	}

	/* configure any specific DLCI settings */
	for(i=0;i<CONFIG_DLCI_MAX;i++)
		if (flp->dlci[i])
		{
			dlp = flp->master[i]->priv;
			if (dlp->configured)
				sdla_cmd(dev, SDLA_SET_DLCI_CONFIGURATION, abs(flp->dlci[i]), 0, &dlp->config, sizeof(struct dlci_conf), NULL, NULL);
		}

	netif_start_queue(dev);
	
	MOD_INC_USE_COUNT;

	return(0);
}

static int sdla_config(struct net_device *dev, struct frad_conf *conf, int get)
{
	struct frad_local *flp;
	struct conf_data  data;
	int               i;
	short             size;

	if (dev->type == 0xFFFF)
		return(-EUNATCH);

	flp = dev->priv;

	if (!get)
	{
		if (netif_running(dev))
			return(-EBUSY);

		if(copy_from_user(&data.config, conf, sizeof(struct frad_conf)))
			return -EFAULT;

		if (data.config.station & ~FRAD_STATION_NODE)
			return(-EINVAL);

		if (data.config.flags & ~FRAD_VALID_FLAGS)
			return(-EINVAL);

		if ((data.config.kbaud < 0) || 
			 ((data.config.kbaud > 128) && (flp->type != SDLA_S508)))
			return(-EINVAL);

		if (data.config.clocking & ~(FRAD_CLOCK_INT | SDLA_S508_PORT_RS232))
			return(-EINVAL);

		if ((data.config.mtu < 0) || (data.config.mtu > SDLA_MAX_MTU))
			return(-EINVAL);

		if ((data.config.T391 < 5) || (data.config.T391 > 30))
			return(-EINVAL);

		if ((data.config.T392 < 5) || (data.config.T392 > 30))
			return(-EINVAL);

		if ((data.config.N391 < 1) || (data.config.N391 > 255))
			return(-EINVAL);

		if ((data.config.N392 < 1) || (data.config.N392 > 10))
			return(-EINVAL);

		if ((data.config.N393 < 1) || (data.config.N393 > 10))
			return(-EINVAL);

		memcpy(&flp->config, &data.config, sizeof(struct frad_conf));
		flp->config.flags |= SDLA_DIRECT_RECV;

		if (flp->type == SDLA_S508)
			flp->config.flags |= SDLA_TX70_RX30;

		if (dev->mtu != flp->config.mtu)
		{
			/* this is required to change the MTU */
			dev->mtu = flp->config.mtu;
			for(i=0;i<CONFIG_DLCI_MAX;i++)
				if (flp->master[i])
					flp->master[i]->mtu = flp->config.mtu;
		}

		flp->config.mtu += sizeof(struct frhdr);

		/* off to the races! */
		if (!flp->configured)
			sdla_start(dev);

		flp->configured = 1;
	}
	else
	{
		/* no sense reading if the CPU isn't started */
		if (netif_running(dev))
		{
			size = sizeof(data);
			if (sdla_cmd(dev, SDLA_READ_DLCI_CONFIGURATION, 0, 0, NULL, 0, &data, &size) != SDLA_RET_OK)
				return(-EIO);
		}
		else
			if (flp->configured)
				memcpy(&data.config, &flp->config, sizeof(struct frad_conf));
			else
				memset(&data.config, 0, sizeof(struct frad_conf));

		memcpy(&flp->config, &data.config, sizeof(struct frad_conf));
		data.config.flags &= FRAD_VALID_FLAGS;
		data.config.mtu -= data.config.mtu > sizeof(struct frhdr) ? sizeof(struct frhdr) : data.config.mtu;
		return copy_to_user(conf, &data.config, sizeof(struct frad_conf))?-EFAULT:0;
	}

	return(0);
}

static int sdla_xfer(struct net_device *dev, struct sdla_mem *info, int read)
{
	struct sdla_mem mem;
	char	*temp;

	if(copy_from_user(&mem, info, sizeof(mem)))
		return -EFAULT;
		
	if (read)
	{	
		temp = kmalloc(mem.len, GFP_KERNEL);
		if (!temp)
			return(-ENOMEM);
		sdla_read(dev, mem.addr, temp, mem.len);
		if(copy_to_user(mem.data, temp, mem.len))
		{
			kfree(temp);
			return -EFAULT;
		}
		kfree(temp);
	}
	else
	{
		temp = kmalloc(mem.len, GFP_KERNEL);
		if (!temp)
			return(-ENOMEM);
		if(copy_from_user(temp, mem.data, mem.len))
		{
			kfree(temp);
			return -EFAULT;
		}
		sdla_write(dev, mem.addr, temp, mem.len);
		kfree(temp);
	}
	return(0);
}

static int sdla_reconfig(struct net_device *dev)
{
	struct frad_local *flp;
	struct conf_data  data;
	int               i, len;

	flp = dev->priv;

	len = 0;
	for(i=0;i<CONFIG_DLCI_MAX;i++)
		if (flp->dlci[i])
			data.dlci[len++] = flp->dlci[i];
	len *= 2;

	memcpy(&data, &flp->config, sizeof(struct frad_conf));
	len += sizeof(struct frad_conf);

	sdla_cmd(dev, SDLA_DISABLE_COMMUNICATIONS, 0, 0, NULL, 0, NULL, NULL);
	sdla_cmd(dev, SDLA_SET_DLCI_CONFIGURATION, 0, 0, &data, len, NULL, NULL);
	sdla_cmd(dev, SDLA_ENABLE_COMMUNICATIONS, 0, 0, NULL, 0, NULL, NULL);

	return(0);
}

static int sdla_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct frad_local *flp;

	if(!capable(CAP_NET_ADMIN))
		return -EPERM;
		
	flp = dev->priv;

	if (!flp->initialized)
		return(-EINVAL);

	switch (cmd)
	{
		case FRAD_GET_CONF:
		case FRAD_SET_CONF:
			return(sdla_config(dev, (struct frad_conf *)ifr->ifr_data, cmd == FRAD_GET_CONF));

		case SDLA_IDENTIFY:
			ifr->ifr_flags = flp->type;
			break;

		case SDLA_CPUSPEED:
			return(sdla_cpuspeed(dev, ifr)); 

/* ==========================================================
NOTE:  This is rather a useless action right now, as the
       current driver does not support protocols other than
       FR.  However, Sangoma has modules for a number of
       other protocols in the works.
============================================================*/
		case SDLA_PROTOCOL:
			if (flp->configured)
				return(-EALREADY);

			switch (ifr->ifr_flags)
			{
				case ARPHRD_FRAD:
					dev->type = ifr->ifr_flags;
					break;
				default:
					return(-ENOPROTOOPT);
			}
			break;

		case SDLA_CLEARMEM:
			sdla_clear(dev);
			break;

		case SDLA_WRITEMEM:
		case SDLA_READMEM:
			return(sdla_xfer(dev, (struct sdla_mem *)ifr->ifr_data, cmd == SDLA_READMEM));

		case SDLA_START:
			sdla_start(dev);
			break;

		case SDLA_STOP:
			sdla_stop(dev);
			break;

		default:
			return(-EOPNOTSUPP);
	}
	return(0);
}

int sdla_change_mtu(struct net_device *dev, int new_mtu)
{
	struct frad_local *flp;

	flp = dev->priv;

	if (netif_running(dev))
		return(-EBUSY);

	/* for now, you can't change the MTU! */
	return(-EOPNOTSUPP);
}

int sdla_set_config(struct net_device *dev, struct ifmap *map)
{
	struct frad_local *flp;
	int               i;
	char              byte;

	flp = dev->priv;

	if (flp->initialized)
		return(-EINVAL);

	for(i=0;i < sizeof(valid_port) / sizeof (int) ; i++)
		if (valid_port[i] == map->base_addr)
			break;   

	if (i == sizeof(valid_port) / sizeof(int))
		return(-EINVAL);

	dev->base_addr = map->base_addr;
	request_region(dev->base_addr, SDLA_IO_EXTENTS, dev->name);

	/* test for card types, S502A, S502E, S507, S508                 */
	/* these tests shut down the card completely, so clear the state */
	flp->type = SDLA_UNKNOWN;
	flp->state = 0;
   
	for(i=1;i<SDLA_IO_EXTENTS;i++)
		if (inb(dev->base_addr + i) != 0xFF)
			break;

	if (i == SDLA_IO_EXTENTS)
	{   
		outb(SDLA_HALT, dev->base_addr + SDLA_REG_Z80_CONTROL);
		if ((inb(dev->base_addr + SDLA_S502_STS) & 0x0F) == 0x08)
		{
			outb(SDLA_S502E_INTACK, dev->base_addr + SDLA_REG_CONTROL);
			if ((inb(dev->base_addr + SDLA_S502_STS) & 0x0F) == 0x0C)
			{
				outb(SDLA_HALT, dev->base_addr + SDLA_REG_CONTROL);
				flp->type = SDLA_S502E;
			}
		}
	}

	if (flp->type == SDLA_UNKNOWN)
	{
		for(byte=inb(dev->base_addr),i=0;i<SDLA_IO_EXTENTS;i++)
			if (inb(dev->base_addr + i) != byte)
				break;

		if (i == SDLA_IO_EXTENTS)
		{
			outb(SDLA_HALT, dev->base_addr + SDLA_REG_CONTROL);
			if ((inb(dev->base_addr + SDLA_S502_STS) & 0x7E) == 0x30)
			{
				outb(SDLA_S507_ENABLE, dev->base_addr + SDLA_REG_CONTROL);
				if ((inb(dev->base_addr + SDLA_S502_STS) & 0x7E) == 0x32)
				{
					outb(SDLA_HALT, dev->base_addr + SDLA_REG_CONTROL);
					flp->type = SDLA_S507;
				}
			}
		}
	}

	if (flp->type == SDLA_UNKNOWN)
	{
		outb(SDLA_HALT, dev->base_addr + SDLA_REG_CONTROL);
		if ((inb(dev->base_addr + SDLA_S508_STS) & 0x3F) == 0x00)
		{
			outb(SDLA_S508_INTEN, dev->base_addr + SDLA_REG_CONTROL);
			if ((inb(dev->base_addr + SDLA_S508_STS) & 0x3F) == 0x10)
			{
				outb(SDLA_HALT, dev->base_addr + SDLA_REG_CONTROL);
				flp->type = SDLA_S508;
			}
		}
	}

	if (flp->type == SDLA_UNKNOWN)
	{
		outb(SDLA_S502A_HALT, dev->base_addr + SDLA_REG_CONTROL);
		if (inb(dev->base_addr + SDLA_S502_STS) == 0x40)
		{
			outb(SDLA_S502A_START, dev->base_addr + SDLA_REG_CONTROL);
			if (inb(dev->base_addr + SDLA_S502_STS) == 0x40)
			{
				outb(SDLA_S502A_INTEN, dev->base_addr + SDLA_REG_CONTROL);
				if (inb(dev->base_addr + SDLA_S502_STS) == 0x44)
				{
					outb(SDLA_S502A_START, dev->base_addr + SDLA_REG_CONTROL);
					flp->type = SDLA_S502A;
				}
			}
		}
	}

	if (flp->type == SDLA_UNKNOWN)
	{
		printk(KERN_NOTICE "%s: Unknown card type\n", dev->name);
		return(-ENODEV);
	}

	switch(dev->base_addr)
	{
		case 0x270:
		case 0x280:
		case 0x380: 
		case 0x390:
			if ((flp->type != SDLA_S508) && (flp->type != SDLA_S507))
				return(-EINVAL);
	}

	switch (map->irq)
	{
		case 2:
			if (flp->type != SDLA_S502E)
				return(-EINVAL);
			break;

		case 10:
		case 11:
		case 12:
		case 15:
		case 4:
			if ((flp->type != SDLA_S508) && (flp->type != SDLA_S507))
				return(-EINVAL);

		case 3:
		case 5:
		case 7:
			if (flp->type == SDLA_S502A)
				return(-EINVAL);
			break;

		default:
			return(-EINVAL);
	}
	dev->irq = map->irq;

	if (request_irq(dev->irq, &sdla_isr, 0, dev->name, dev)) 
		return(-EAGAIN);

	if (flp->type == SDLA_S507)
	{
		switch(dev->irq)
		{
			case 3:
				flp->state = SDLA_S507_IRQ3;
				break;
			case 4:
				flp->state = SDLA_S507_IRQ4;
				break;
			case 5:
				flp->state = SDLA_S507_IRQ5;
				break;
			case 7:
				flp->state = SDLA_S507_IRQ7;
				break;
			case 10:
				flp->state = SDLA_S507_IRQ10;
				break;
			case 11:
				flp->state = SDLA_S507_IRQ11;
				break;
			case 12:
				flp->state = SDLA_S507_IRQ12;
				break;
			case 15:
				flp->state = SDLA_S507_IRQ15;
				break;
		}
	}

	for(i=0;i < sizeof(valid_mem) / sizeof (int) ; i++)
		if (valid_mem[i] == map->mem_start)
			break;   

	if (i == sizeof(valid_mem) / sizeof(int))
	/*
	 *	FIXME:
	 *	BUG BUG BUG: MUST RELEASE THE IRQ WE ALLOCATED IN
	 *	ALL THESE CASES
	 *
	 */
		return(-EINVAL);

	if ((flp->type == SDLA_S502A) && (((map->mem_start & 0xF000) >> 12) == 0x0E))
		return(-EINVAL);

	if ((flp->type != SDLA_S507) && ((map->mem_start >> 16) == 0x0B))
		return(-EINVAL);

	if ((flp->type == SDLA_S507) && ((map->mem_start >> 16) == 0x0D))
		return(-EINVAL);

	dev->mem_start = map->mem_start;
	dev->mem_end = dev->mem_start + 0x2000;

	byte = flp->type != SDLA_S508 ? SDLA_8K_WINDOW : 0;
	byte |= (map->mem_start & 0xF000) >> (12 + (flp->type == SDLA_S508 ? 1 : 0));
	switch(flp->type)
	{
		case SDLA_S502A:
		case SDLA_S502E:
			switch (map->mem_start >> 16)
			{
				case 0x0A:
					byte |= SDLA_S502_SEG_A;
					break;
				case 0x0C:
					byte |= SDLA_S502_SEG_C;
					break;
				case 0x0D:
					byte |= SDLA_S502_SEG_D;
					break;
				case 0x0E:
					byte |= SDLA_S502_SEG_E;
					break;
			}
			break;
		case SDLA_S507:
			switch (map->mem_start >> 16)
			{
				case 0x0A:
					byte |= SDLA_S507_SEG_A;
					break;
				case 0x0B:
					byte |= SDLA_S507_SEG_B;
					break;
				case 0x0C:
					byte |= SDLA_S507_SEG_C;
					break;
				case 0x0E:
					byte |= SDLA_S507_SEG_E;
					break;
			}
			break;
		case SDLA_S508:
			switch (map->mem_start >> 16)
			{
				case 0x0A:
					byte |= SDLA_S508_SEG_A;
					break;
				case 0x0C:
					byte |= SDLA_S508_SEG_C;
					break;
				case 0x0D:
					byte |= SDLA_S508_SEG_D;
					break;
				case 0x0E:
					byte |= SDLA_S508_SEG_E;
					break;
			}
			break;
	}

	/* set the memory bits, and enable access */
	outb(byte, dev->base_addr + SDLA_REG_PC_WINDOW);

	switch(flp->type)
	{
		case SDLA_S502E:
			flp->state = SDLA_S502E_ENABLE;
			break;
		case SDLA_S507:
			flp->state |= SDLA_MEMEN;
			break;
		case SDLA_S508:
			flp->state = SDLA_MEMEN;
			break;
	}
	outb(flp->state, dev->base_addr + SDLA_REG_CONTROL);

	flp->initialized = 1;
	return(0);
}
 
static struct net_device_stats *sdla_stats(struct net_device *dev)
{
	struct frad_local *flp;
	flp = dev->priv;

	return(&flp->stats);
}

int __init sdla_init(struct net_device *dev)
{
	struct frad_local *flp;

	/* allocate the private data structure */
	flp = kmalloc(sizeof(struct frad_local), GFP_KERNEL);
	if (!flp)
		return(-ENOMEM);

	memset(flp, 0, sizeof(struct frad_local));
	dev->priv = flp;

	dev->flags		= 0;
	dev->open		= sdla_open;
	dev->stop		= sdla_close;
	dev->do_ioctl		= sdla_ioctl;
	dev->set_config		= sdla_set_config;
	dev->get_stats		= sdla_stats;
	dev->hard_start_xmit	= sdla_transmit;
	dev->change_mtu		= sdla_change_mtu;

	dev->type		= 0xFFFF;
	dev->hard_header_len = 0;
	dev->addr_len		= 0;
	dev->mtu		= SDLA_MAX_MTU;

	dev_init_buffers(dev);
   
	flp->activate		= sdla_activate;
	flp->deactivate		= sdla_deactivate;
	flp->assoc		= sdla_assoc;
	flp->deassoc		= sdla_deassoc;
	flp->dlci_conf		= sdla_dlci_conf;

	init_timer(&flp->timer);
	flp->timer.expires	= 1;
	flp->timer.data		= (unsigned long) dev;
	flp->timer.function	= sdla_poll;

	return(0);
}

int __init sdla_c_setup(void)
{
	printk("%s.\n", version);
	register_frad(devname);
	return 0;
}

#ifdef MODULE
static struct net_device sdla0 = {"sdla0", 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, sdla_init};

int init_module(void)
{
	int result;

	sdla_c_setup();
	if ((result = register_netdev(&sdla0)) != 0)
		return result;
	return 0;
}

void cleanup_module(void)
{
	unregister_netdev(&sdla0);
	if (sdla0.priv)
		kfree(sdla0.priv);
	if (sdla0.irq)
		free_irq(sdla0.irq, &sdla0);
}
#endif /* MODULE */
