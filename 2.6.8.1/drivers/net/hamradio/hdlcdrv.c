/*****************************************************************************/

/*
 *	hdlcdrv.c  -- HDLC packet radio network driver.
 *
 *	Copyright (C) 1996-2000  Thomas Sailer (sailer@ife.ee.ethz.ch)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Please note that the GPL allows you to use the driver, NOT the radio.
 *  In order to use the radio, you need a license from the communications
 *  authority of your country.
 *
 *  The driver was derived from Donald Beckers skeleton.c
 *	Written 1993-94 by Donald Becker.
 *
 *  History:
 *   0.1  21.09.1996  Started
 *        18.10.1996  Changed to new user space access routines 
 *                    (copy_{to,from}_user)
 *   0.2  21.11.1996  various small changes
 *   0.3  03.03.1997  fixed (hopefully) IP not working with ax.25 as a module
 *   0.4  16.04.1997  init code/data tagged
 *   0.5  30.07.1997  made HDLC buffers bigger (solves a problem with the
 *                    soundmodem driver)
 *   0.6  05.04.1998  add spinlocks
 *   0.7  03.08.1999  removed some old compatibility cruft
 *   0.8  12.02.2000  adapted to softnet driver interface
 */

/*****************************************************************************/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/if.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <asm/bitops.h>
#include <asm/uaccess.h>

#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/hdlcdrv.h>
/* prototypes for ax25_encapsulate and ax25_rebuild_header */
#include <net/ax25.h> 

/* make genksyms happy */
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/crc-ccitt.h>

/* --------------------------------------------------------------------- */

/*
 * The name of the card. Is used for messages and in the requests for
 * io regions, irqs and dma channels
 */

static char ax25_bcast[AX25_ADDR_LEN] =
{'Q' << 1, 'S' << 1, 'T' << 1, ' ' << 1, ' ' << 1, ' ' << 1, '0' << 1};
static char ax25_nocall[AX25_ADDR_LEN] =
{'L' << 1, 'I' << 1, 'N' << 1, 'U' << 1, 'X' << 1, ' ' << 1, '1' << 1};

/* --------------------------------------------------------------------- */

#define KISS_VERBOSE

/* --------------------------------------------------------------------- */

#define PARAM_TXDELAY   1
#define PARAM_PERSIST   2
#define PARAM_SLOTTIME  3
#define PARAM_TXTAIL    4
#define PARAM_FULLDUP   5
#define PARAM_HARDWARE  6
#define PARAM_RETURN    255

/* --------------------------------------------------------------------- */
/*
 * the CRC routines are stolen from WAMPES
 * by Dieter Deyke
 */


/*---------------------------------------------------------------------------*/

static inline void append_crc_ccitt(unsigned char *buffer, int len)
{
 	unsigned int crc = crc_ccitt(0xffff, buffer, len) ^ 0xffff;
	*buffer++ = crc;
	*buffer++ = crc >> 8;
}

/*---------------------------------------------------------------------------*/

static inline int check_crc_ccitt(const unsigned char *buf, int cnt)
{
	return (crc_ccitt(0xffff, buf, cnt) & 0xffff) == 0xf0b8;
}

/*---------------------------------------------------------------------------*/

#if 0
static int calc_crc_ccitt(const unsigned char *buf, int cnt)
{
	unsigned int crc = 0xffff;

	for (; cnt > 0; cnt--)
		crc = (crc >> 8) ^ crc_ccitt_table[(crc ^ *buf++) & 0xff];
	crc ^= 0xffff;
	return (crc & 0xffff);
}
#endif

/* ---------------------------------------------------------------------- */

#define tenms_to_2flags(s,tenms) ((tenms * s->par.bitrate) / 100 / 16)

/* ---------------------------------------------------------------------- */
/*
 * The HDLC routines
 */

static int hdlc_rx_add_bytes(struct hdlcdrv_state *s, unsigned int bits, 
			     int num)
{
	int added = 0;
	
	while (s->hdlcrx.rx_state && num >= 8) {
		if (s->hdlcrx.len >= sizeof(s->hdlcrx.buffer)) {
			s->hdlcrx.rx_state = 0;
			return 0;
		}
		*s->hdlcrx.bp++ = bits >> (32-num);
		s->hdlcrx.len++;
		num -= 8;
		added += 8;
	}
	return added;
}

static void hdlc_rx_flag(struct net_device *dev, struct hdlcdrv_state *s)
{
	struct sk_buff *skb;
	int pkt_len;
	unsigned char *cp;

	if (s->hdlcrx.len < 4) 
		return;
	if (!check_crc_ccitt(s->hdlcrx.buffer, s->hdlcrx.len)) 
		return;
	pkt_len = s->hdlcrx.len - 2 + 1; /* KISS kludge */
	if (!(skb = dev_alloc_skb(pkt_len))) {
		printk("%s: memory squeeze, dropping packet\n", dev->name);
		s->stats.rx_dropped++;
		return;
	}
	skb->dev = dev;
	cp = skb_put(skb, pkt_len);
	*cp++ = 0; /* KISS kludge */
	memcpy(cp, s->hdlcrx.buffer, pkt_len - 1);
	skb->protocol = htons(ETH_P_AX25);
	skb->mac.raw = skb->data;
	netif_rx(skb);
	dev->last_rx = jiffies;
	s->stats.rx_packets++;
}

void hdlcdrv_receiver(struct net_device *dev, struct hdlcdrv_state *s)
{
	int i;
	unsigned int mask1, mask2, mask3, mask4, mask5, mask6, word;
	
	if (!s || s->magic != HDLCDRV_MAGIC) 
		return;
	if (test_and_set_bit(0, &s->hdlcrx.in_hdlc_rx))
		return;

	while (!hdlcdrv_hbuf_empty(&s->hdlcrx.hbuf)) {
		word = hdlcdrv_hbuf_get(&s->hdlcrx.hbuf);	

#ifdef HDLCDRV_DEBUG
		hdlcdrv_add_bitbuffer_word(&s->bitbuf_hdlc, word);
#endif /* HDLCDRV_DEBUG */
	       	s->hdlcrx.bitstream >>= 16;
		s->hdlcrx.bitstream |= word << 16;
		s->hdlcrx.bitbuf >>= 16;
		s->hdlcrx.bitbuf |= word << 16;
		s->hdlcrx.numbits += 16;
		for(i = 15, mask1 = 0x1fc00, mask2 = 0x1fe00, mask3 = 0x0fc00,
		    mask4 = 0x1f800, mask5 = 0xf800, mask6 = 0xffff; 
		    i >= 0; 
		    i--, mask1 <<= 1, mask2 <<= 1, mask3 <<= 1, mask4 <<= 1, 
		    mask5 <<= 1, mask6 = (mask6 << 1) | 1) {
			if ((s->hdlcrx.bitstream & mask1) == mask1)
				s->hdlcrx.rx_state = 0; /* abort received */
			else if ((s->hdlcrx.bitstream & mask2) == mask3) {
				/* flag received */
				if (s->hdlcrx.rx_state) {
					hdlc_rx_add_bytes(s, s->hdlcrx.bitbuf 
							  << (8+i),
							  s->hdlcrx.numbits
							  -8-i);
					hdlc_rx_flag(dev, s);
				}
				s->hdlcrx.len = 0;
				s->hdlcrx.bp = s->hdlcrx.buffer;
				s->hdlcrx.rx_state = 1;
				s->hdlcrx.numbits = i;
			} else if ((s->hdlcrx.bitstream & mask4) == mask5) {
				/* stuffed bit */
				s->hdlcrx.numbits--;
				s->hdlcrx.bitbuf = (s->hdlcrx.bitbuf & (~mask6)) |
					((s->hdlcrx.bitbuf & mask6) << 1);
			}
		}
		s->hdlcrx.numbits -= hdlc_rx_add_bytes(s, s->hdlcrx.bitbuf,
						       s->hdlcrx.numbits);
	}
	clear_bit(0, &s->hdlcrx.in_hdlc_rx);
}

/* ---------------------------------------------------------------------- */

static inline void do_kiss_params(struct hdlcdrv_state *s,
				  unsigned char *data, unsigned long len)
{

#ifdef KISS_VERBOSE
#define PKP(a,b) printk(KERN_INFO "hdlcdrv.c: channel params: " a "\n", b)
#else /* KISS_VERBOSE */	      
#define PKP(a,b) 
#endif /* KISS_VERBOSE */	      

	if (len < 2)
		return;
	switch(data[0]) {
	case PARAM_TXDELAY:
		s->ch_params.tx_delay = data[1];
		PKP("TX delay = %ums", 10 * s->ch_params.tx_delay);
		break;
	case PARAM_PERSIST:   
		s->ch_params.ppersist = data[1];
		PKP("p persistence = %u", s->ch_params.ppersist);
		break;
	case PARAM_SLOTTIME:  
		s->ch_params.slottime = data[1];
		PKP("slot time = %ums", s->ch_params.slottime);
		break;
	case PARAM_TXTAIL:    
		s->ch_params.tx_tail = data[1];
		PKP("TX tail = %ums", s->ch_params.tx_tail);
		break;
	case PARAM_FULLDUP:   
		s->ch_params.fulldup = !!data[1];
		PKP("%s duplex", s->ch_params.fulldup ? "full" : "half");
		break;
	default:
		break;
	}
#undef PKP
}

/* ---------------------------------------------------------------------- */

void hdlcdrv_transmitter(struct net_device *dev, struct hdlcdrv_state *s)
{
	unsigned int mask1, mask2, mask3;
	int i;
	struct sk_buff *skb;
	int pkt_len;

	if (!s || s->magic != HDLCDRV_MAGIC) 
		return;
	if (test_and_set_bit(0, &s->hdlctx.in_hdlc_tx))
		return;
	for (;;) {
		if (s->hdlctx.numbits >= 16) {
			if (hdlcdrv_hbuf_full(&s->hdlctx.hbuf)) {
				clear_bit(0, &s->hdlctx.in_hdlc_tx);
				return;
			}
			hdlcdrv_hbuf_put(&s->hdlctx.hbuf, s->hdlctx.bitbuf);
			s->hdlctx.bitbuf >>= 16;
			s->hdlctx.numbits -= 16;
		}
		switch (s->hdlctx.tx_state) {
		default:
			clear_bit(0, &s->hdlctx.in_hdlc_tx);
			return;
		case 0:
		case 1:
			if (s->hdlctx.numflags) {
				s->hdlctx.numflags--;
				s->hdlctx.bitbuf |= 
					0x7e7e << s->hdlctx.numbits;
				s->hdlctx.numbits += 16;
				break;
			}
			if (s->hdlctx.tx_state == 1) {
				clear_bit(0, &s->hdlctx.in_hdlc_tx);
				return;
			}
			if (!(skb = s->skb)) {
				int flgs = tenms_to_2flags(s, s->ch_params.tx_tail);
				if (flgs < 2)
					flgs = 2;
				s->hdlctx.tx_state = 1;
				s->hdlctx.numflags = flgs;
				break;
			}
			s->skb = NULL;
			netif_wake_queue(dev);
			pkt_len = skb->len-1; /* strip KISS byte */
			if (pkt_len >= HDLCDRV_MAXFLEN || pkt_len < 2) {
				s->hdlctx.tx_state = 0;
				s->hdlctx.numflags = 1;
				dev_kfree_skb_irq(skb);
				break;
			}
			memcpy(s->hdlctx.buffer, skb->data+1, pkt_len);
			dev_kfree_skb_irq(skb);
			s->hdlctx.bp = s->hdlctx.buffer;
			append_crc_ccitt(s->hdlctx.buffer, pkt_len);
			s->hdlctx.len = pkt_len+2; /* the appended CRC */
			s->hdlctx.tx_state = 2;
			s->hdlctx.bitstream = 0;
			s->stats.tx_packets++;
			break;
		case 2:
			if (!s->hdlctx.len) {
				s->hdlctx.tx_state = 0;
				s->hdlctx.numflags = 1;
				break;
			}
			s->hdlctx.len--;
			s->hdlctx.bitbuf |= *s->hdlctx.bp <<
				s->hdlctx.numbits;
			s->hdlctx.bitstream >>= 8;
			s->hdlctx.bitstream |= (*s->hdlctx.bp++) << 16;
			mask1 = 0x1f000;
			mask2 = 0x10000;
			mask3 = 0xffffffff >> (31-s->hdlctx.numbits);
			s->hdlctx.numbits += 8;
			for(i = 0; i < 8; i++, mask1 <<= 1, mask2 <<= 1, 
			    mask3 = (mask3 << 1) | 1) {
				if ((s->hdlctx.bitstream & mask1) != mask1) 
					continue;
				s->hdlctx.bitstream &= ~mask2;
				s->hdlctx.bitbuf = 
					(s->hdlctx.bitbuf & mask3) |
						((s->hdlctx.bitbuf & 
						 (~mask3)) << 1);
				s->hdlctx.numbits++;
				mask3 = (mask3 << 1) | 1;
			}
			break;
		}
	}
}

/* ---------------------------------------------------------------------- */

static void start_tx(struct net_device *dev, struct hdlcdrv_state *s)
{
	s->hdlctx.tx_state = 0;
	s->hdlctx.numflags = tenms_to_2flags(s, s->ch_params.tx_delay);
	s->hdlctx.bitbuf = s->hdlctx.bitstream = s->hdlctx.numbits = 0;
	hdlcdrv_transmitter(dev, s);
	s->hdlctx.ptt = 1;
	s->ptt_keyed++;
}

/* ---------------------------------------------------------------------- */

static unsigned short random_seed;

static inline unsigned short random_num(void)
{
	random_seed = 28629 * random_seed + 157;
	return random_seed;
}

/* ---------------------------------------------------------------------- */

void hdlcdrv_arbitrate(struct net_device *dev, struct hdlcdrv_state *s)
{
	if (!s || s->magic != HDLCDRV_MAGIC || s->hdlctx.ptt || !s->skb) 
		return;
	if (s->ch_params.fulldup) {
		start_tx(dev, s);
		return;
	}
	if (s->hdlcrx.dcd) {
		s->hdlctx.slotcnt = s->ch_params.slottime;
		return;
	}
	if ((--s->hdlctx.slotcnt) > 0)
		return;
	s->hdlctx.slotcnt = s->ch_params.slottime;
	if ((random_num() % 256) > s->ch_params.ppersist)
		return;
	start_tx(dev, s);
}

/* --------------------------------------------------------------------- */
/*
 * ===================== network driver interface =========================
 */

static inline int hdlcdrv_paranoia_check(struct net_device *dev,
					const char *routine)
{
	if (!dev || !dev->priv || 
	    ((struct hdlcdrv_state *)dev->priv)->magic != HDLCDRV_MAGIC) {
		printk(KERN_ERR "hdlcdrv: bad magic number for hdlcdrv_state "
		       "struct in routine %s\n", routine);
		return 1;
	}
	return 0;
}

/* --------------------------------------------------------------------- */

static int hdlcdrv_send_packet(struct sk_buff *skb, struct net_device *dev)
{
	struct hdlcdrv_state *sm;

	if (hdlcdrv_paranoia_check(dev, "hdlcdrv_send_packet"))
		return 0;
	sm = (struct hdlcdrv_state *)dev->priv;
	if (skb->data[0] != 0) {
		do_kiss_params(sm, skb->data, skb->len);
		dev_kfree_skb(skb);
		return 0;
	}
	if (sm->skb)
		return -1;
	netif_stop_queue(dev);
	sm->skb = skb;
	return 0;
}

/* --------------------------------------------------------------------- */

static int hdlcdrv_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *sa = (struct sockaddr *)addr;

	/* addr is an AX.25 shifted ASCII mac address */
	memcpy(dev->dev_addr, sa->sa_data, dev->addr_len); 
	return 0;                                         
}

/* --------------------------------------------------------------------- */

static struct net_device_stats *hdlcdrv_get_stats(struct net_device *dev)
{
	struct hdlcdrv_state *sm;

	if (hdlcdrv_paranoia_check(dev, "hdlcdrv_get_stats"))
		return NULL;
	sm = (struct hdlcdrv_state *)dev->priv;
	/* 
	 * Get the current statistics.  This may be called with the
	 * card open or closed. 
	 */
	return &sm->stats;
}

/* --------------------------------------------------------------------- */
/*
 * Open/initialize the board. This is called (in the current kernel)
 * sometime after booting when the 'ifconfig' program is run.
 *
 * This routine should set everything up anew at each open, even
 * registers that "should" only need to be set once at boot, so that
 * there is non-reboot way to recover if something goes wrong.
 */

static int hdlcdrv_open(struct net_device *dev)
{
	struct hdlcdrv_state *s;
	int i;

	if (hdlcdrv_paranoia_check(dev, "hdlcdrv_open"))
		return -EINVAL;
	s = (struct hdlcdrv_state *)dev->priv;

	if (!s->ops || !s->ops->open)
		return -ENODEV;

	/*
	 * initialise some variables
	 */
	s->opened = 1;
	s->hdlcrx.hbuf.rd = s->hdlcrx.hbuf.wr = 0;
	s->hdlcrx.in_hdlc_rx = 0;
	s->hdlcrx.rx_state = 0;
	
	s->hdlctx.hbuf.rd = s->hdlctx.hbuf.wr = 0;
	s->hdlctx.in_hdlc_tx = 0;
	s->hdlctx.tx_state = 1;
	s->hdlctx.numflags = 0;
	s->hdlctx.bitstream = s->hdlctx.bitbuf = s->hdlctx.numbits = 0;
	s->hdlctx.ptt = 0;
	s->hdlctx.slotcnt = s->ch_params.slottime;
	s->hdlctx.calibrate = 0;

	i = s->ops->open(dev);
	if (i)
		return i;
	netif_start_queue(dev);
	return 0;
}

/* --------------------------------------------------------------------- */
/* 
 * The inverse routine to hdlcdrv_open(). 
 */

static int hdlcdrv_close(struct net_device *dev)
{
	struct hdlcdrv_state *s;
	int i = 0;

	if (hdlcdrv_paranoia_check(dev, "hdlcdrv_close"))
		return -EINVAL;
	s = (struct hdlcdrv_state *)dev->priv;

	netif_stop_queue(dev);

	if (s->ops && s->ops->close)
		i = s->ops->close(dev);
	if (s->skb)
		dev_kfree_skb(s->skb);
	s->skb = NULL;
	s->opened = 0;
	return i;
}

/* --------------------------------------------------------------------- */

static int hdlcdrv_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct hdlcdrv_state *s;
	struct hdlcdrv_ioctl bi;
		
	if (hdlcdrv_paranoia_check(dev, "hdlcdrv_ioctl"))
		return -EINVAL;
	s = (struct hdlcdrv_state *)dev->priv;

	if (cmd != SIOCDEVPRIVATE) {
		if (s->ops && s->ops->ioctl)
			return s->ops->ioctl(dev, ifr, &bi, cmd);
		return -ENOIOCTLCMD;
	}
	if (copy_from_user(&bi, ifr->ifr_data, sizeof(bi)))
		return -EFAULT;

	switch (bi.cmd) {
	default:
		if (s->ops && s->ops->ioctl)
			return s->ops->ioctl(dev, ifr, &bi, cmd);
		return -ENOIOCTLCMD;

	case HDLCDRVCTL_GETCHANNELPAR:
		bi.data.cp.tx_delay = s->ch_params.tx_delay;
		bi.data.cp.tx_tail = s->ch_params.tx_tail;
		bi.data.cp.slottime = s->ch_params.slottime;
		bi.data.cp.ppersist = s->ch_params.ppersist;
		bi.data.cp.fulldup = s->ch_params.fulldup;
		break;

	case HDLCDRVCTL_SETCHANNELPAR:
		if (!capable(CAP_NET_ADMIN))
			return -EACCES;
		s->ch_params.tx_delay = bi.data.cp.tx_delay;
		s->ch_params.tx_tail = bi.data.cp.tx_tail;
		s->ch_params.slottime = bi.data.cp.slottime;
		s->ch_params.ppersist = bi.data.cp.ppersist;
		s->ch_params.fulldup = bi.data.cp.fulldup;
		s->hdlctx.slotcnt = 1;
		return 0;
		
	case HDLCDRVCTL_GETMODEMPAR:
		bi.data.mp.iobase = dev->base_addr;
		bi.data.mp.irq = dev->irq;
		bi.data.mp.dma = dev->dma;
		bi.data.mp.dma2 = s->ptt_out.dma2;
		bi.data.mp.seriobase = s->ptt_out.seriobase;
		bi.data.mp.pariobase = s->ptt_out.pariobase;
		bi.data.mp.midiiobase = s->ptt_out.midiiobase;
		break;

	case HDLCDRVCTL_SETMODEMPAR:
		if ((!capable(CAP_SYS_RAWIO)) || netif_running(dev))
			return -EACCES;
		dev->base_addr = bi.data.mp.iobase;
		dev->irq = bi.data.mp.irq;
		dev->dma = bi.data.mp.dma;
		s->ptt_out.dma2 = bi.data.mp.dma2;
		s->ptt_out.seriobase = bi.data.mp.seriobase;
		s->ptt_out.pariobase = bi.data.mp.pariobase;
		s->ptt_out.midiiobase = bi.data.mp.midiiobase;
		return 0;	
	
	case HDLCDRVCTL_GETSTAT:
		bi.data.cs.ptt = hdlcdrv_ptt(s);
		bi.data.cs.dcd = s->hdlcrx.dcd;
		bi.data.cs.ptt_keyed = s->ptt_keyed;
		bi.data.cs.tx_packets = s->stats.tx_packets;
		bi.data.cs.tx_errors = s->stats.tx_errors;
		bi.data.cs.rx_packets = s->stats.rx_packets;
		bi.data.cs.rx_errors = s->stats.rx_errors;
		break;		

	case HDLCDRVCTL_OLDGETSTAT:
		bi.data.ocs.ptt = hdlcdrv_ptt(s);
		bi.data.ocs.dcd = s->hdlcrx.dcd;
		bi.data.ocs.ptt_keyed = s->ptt_keyed;
		break;		

	case HDLCDRVCTL_CALIBRATE:
		if(!capable(CAP_SYS_RAWIO))
			return -EPERM;
		s->hdlctx.calibrate = bi.data.calibrate * s->par.bitrate / 16;
		return 0;

	case HDLCDRVCTL_GETSAMPLES:
#ifndef HDLCDRV_DEBUG
		return -EPERM;
#else /* HDLCDRV_DEBUG */
		if (s->bitbuf_channel.rd == s->bitbuf_channel.wr) 
			return -EAGAIN;
		bi.data.bits = 
			s->bitbuf_channel.buffer[s->bitbuf_channel.rd];
		s->bitbuf_channel.rd = (s->bitbuf_channel.rd+1) %
			sizeof(s->bitbuf_channel.buffer);
		break;
#endif /* HDLCDRV_DEBUG */
				
	case HDLCDRVCTL_GETBITS:
#ifndef HDLCDRV_DEBUG
		return -EPERM;
#else /* HDLCDRV_DEBUG */
		if (s->bitbuf_hdlc.rd == s->bitbuf_hdlc.wr) 
			return -EAGAIN;
		bi.data.bits = 
			s->bitbuf_hdlc.buffer[s->bitbuf_hdlc.rd];
		s->bitbuf_hdlc.rd = (s->bitbuf_hdlc.rd+1) %
			sizeof(s->bitbuf_hdlc.buffer);
		break;		
#endif /* HDLCDRV_DEBUG */

	case HDLCDRVCTL_DRIVERNAME:
		if (s->ops && s->ops->drvname) {
			strncpy(bi.data.drivername, s->ops->drvname, 
				sizeof(bi.data.drivername));
			break;
		}
		bi.data.drivername[0] = '\0';
		break;
		
	}
	if (copy_to_user(ifr->ifr_data, &bi, sizeof(bi)))
		return -EFAULT;
	return 0;

}

/* --------------------------------------------------------------------- */

/*
 * Initialize fields in hdlcdrv
 */
static void hdlcdrv_setup(struct net_device *dev)
{
	static const struct hdlcdrv_channel_params dflt_ch_params = { 
		20, 2, 10, 40, 0 
	};
	struct hdlcdrv_state *s = dev->priv;

	/*
	 * initialize the hdlcdrv_state struct
	 */
	s->ch_params = dflt_ch_params;
	s->ptt_keyed = 0;

	spin_lock_init(&s->hdlcrx.hbuf.lock);
	s->hdlcrx.hbuf.rd = s->hdlcrx.hbuf.wr = 0;
	s->hdlcrx.in_hdlc_rx = 0;
	s->hdlcrx.rx_state = 0;
	
	spin_lock_init(&s->hdlctx.hbuf.lock);
	s->hdlctx.hbuf.rd = s->hdlctx.hbuf.wr = 0;
	s->hdlctx.in_hdlc_tx = 0;
	s->hdlctx.tx_state = 1;
	s->hdlctx.numflags = 0;
	s->hdlctx.bitstream = s->hdlctx.bitbuf = s->hdlctx.numbits = 0;
	s->hdlctx.ptt = 0;
	s->hdlctx.slotcnt = s->ch_params.slottime;
	s->hdlctx.calibrate = 0;

#ifdef HDLCDRV_DEBUG
	s->bitbuf_channel.rd = s->bitbuf_channel.wr = 0;
	s->bitbuf_channel.shreg = 0x80;

	s->bitbuf_hdlc.rd = s->bitbuf_hdlc.wr = 0;
	s->bitbuf_hdlc.shreg = 0x80;
#endif /* HDLCDRV_DEBUG */

	/*
	 * initialize the device struct
	 */
	dev->open = hdlcdrv_open;
	dev->stop = hdlcdrv_close;
	dev->do_ioctl = hdlcdrv_ioctl;
	dev->hard_start_xmit = hdlcdrv_send_packet;
	dev->get_stats = hdlcdrv_get_stats;

	/* Fill in the fields of the device structure */

	s->skb = NULL;
	
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
	dev->hard_header = ax25_encapsulate;
	dev->rebuild_header = ax25_rebuild_header;
#else /* CONFIG_AX25 || CONFIG_AX25_MODULE */
	dev->hard_header = NULL;
	dev->rebuild_header = NULL;
#endif /* CONFIG_AX25 || CONFIG_AX25_MODULE */
	dev->set_mac_address = hdlcdrv_set_mac_address;
	
	dev->type = ARPHRD_AX25;           /* AF_AX25 device */
	dev->hard_header_len = AX25_MAX_HEADER_LEN + AX25_BPQ_HEADER_LEN;
	dev->mtu = AX25_DEF_PACLEN;        /* eth_mtu is the default */
	dev->addr_len = AX25_ADDR_LEN;     /* sizeof an ax.25 address */
	memcpy(dev->broadcast, ax25_bcast, AX25_ADDR_LEN);
	memcpy(dev->dev_addr, ax25_nocall, AX25_ADDR_LEN);
	dev->tx_queue_len = 16;
}

/* --------------------------------------------------------------------- */
struct net_device *hdlcdrv_register(const struct hdlcdrv_ops *ops,
				    unsigned int privsize, const char *ifname,
				    unsigned int baseaddr, unsigned int irq, 
				    unsigned int dma) 
{
	struct net_device *dev;
	struct hdlcdrv_state *s;
	int err;

	BUG_ON(ops == NULL);

	if (privsize < sizeof(struct hdlcdrv_state))
		privsize = sizeof(struct hdlcdrv_state);

	dev = alloc_netdev(privsize, ifname, hdlcdrv_setup);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	/*
	 * initialize part of the hdlcdrv_state struct
	 */
	s = dev->priv;
	s->magic = HDLCDRV_MAGIC;
	s->ops = ops;
	dev->base_addr = baseaddr;
	dev->irq = irq;
	dev->dma = dma;

	err = register_netdev(dev);
	if (err < 0) {
		printk(KERN_WARNING "hdlcdrv: cannot register net "
		       "device %s\n", dev->name);
		free_netdev(dev);
		dev = ERR_PTR(err);
	}
	return dev;
}

/* --------------------------------------------------------------------- */

void hdlcdrv_unregister(struct net_device *dev) 
{
	struct hdlcdrv_state *s = dev->priv;

	BUG_ON(s->magic != HDLCDRV_MAGIC);

	if (s->opened && s->ops->close)
		s->ops->close(dev);
	unregister_netdev(dev);
	
	free_netdev(dev);
}

/* --------------------------------------------------------------------- */

EXPORT_SYMBOL(hdlcdrv_receiver);
EXPORT_SYMBOL(hdlcdrv_transmitter);
EXPORT_SYMBOL(hdlcdrv_arbitrate);
EXPORT_SYMBOL(hdlcdrv_register);
EXPORT_SYMBOL(hdlcdrv_unregister);

/* --------------------------------------------------------------------- */

static int __init hdlcdrv_init_driver(void)
{
	printk(KERN_INFO "hdlcdrv: (C) 1996-2000 Thomas Sailer HB9JNX/AE4WA\n");
	printk(KERN_INFO "hdlcdrv: version 0.8 compiled " __TIME__ " " __DATE__ "\n");
	return 0;
}

/* --------------------------------------------------------------------- */

static void __exit hdlcdrv_cleanup_driver(void)
{
	printk(KERN_INFO "hdlcdrv: cleanup\n");
}

/* --------------------------------------------------------------------- */

MODULE_AUTHOR("Thomas M. Sailer, sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu");
MODULE_DESCRIPTION("Packet Radio network interface HDLC encoder/decoder");
MODULE_LICENSE("GPL");
module_init(hdlcdrv_init_driver);
module_exit(hdlcdrv_cleanup_driver);

/* --------------------------------------------------------------------- */
