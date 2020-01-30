/* $Id: isar.c,v 1.17 2000/11/24 17:05:37 kai Exp $
 *
 * isar.c   ISAR (Siemens PSB 7110) specific routines
 *
 * Author       Karsten Keil (keil@isdn4linux.de)
 *
 * This file is (c) under GNU PUBLIC LICENSE
 *
 */

#define __NO_VERSION__
#include <linux/init.h>
#include "hisax.h"
#include "isar.h"
#include "isdnl1.h"
#include <linux/interrupt.h>

#define DBG_LOADFIRM	0
#define DUMP_MBOXFRAME	2

#define MIN(a,b) ((a<b)?a:b)

#define DLE	0x10
#define ETX	0x03


const u_char faxmodulation_s[] = "3,24,48,72,73,74,96,97,98,121,122,145,146"; 
const u_char faxmodulation[] = {3,24,48,72,73,74,96,97,98,121,122,145,146}; 
#define FAXMODCNT 13

void isar_setup(struct IsdnCardState *cs);
static void isar_pump_cmd(struct BCState *bcs, u_char cmd, u_char para);
static inline void ll_deliver_faxstat(struct BCState *bcs, u_char status);

static inline int
waitforHIA(struct IsdnCardState *cs, int timeout)
{

	while ((cs->BC_Read_Reg(cs, 0, ISAR_HIA) & 1) && timeout) {
		udelay(1);
		timeout--;
	}
	if (!timeout)
		printk(KERN_WARNING "HiSax: ISAR waitforHIA timeout\n");
	return(timeout);
}


int
sendmsg(struct IsdnCardState *cs, u_char his, u_char creg, u_char len,
	u_char *msg)
{
	long flags;
	int i;
	
	if (!waitforHIA(cs, 4000))
		return(0);
#if DUMP_MBOXFRAME
	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "sendmsg(%02x,%02x,%d)", his, creg, len);
#endif
	save_flags(flags);
	cli();
	cs->BC_Write_Reg(cs, 0, ISAR_CTRL_H, creg);
	cs->BC_Write_Reg(cs, 0, ISAR_CTRL_L, len);
	cs->BC_Write_Reg(cs, 0, ISAR_WADR, 0);
	if (msg && len) {
		cs->BC_Write_Reg(cs, 1, ISAR_MBOX, msg[0]);
		for (i=1; i<len; i++)
			cs->BC_Write_Reg(cs, 2, ISAR_MBOX, msg[i]);
#if DUMP_MBOXFRAME>1
		if (cs->debug & L1_DEB_HSCX_FIFO) {
			char tmp[256], *t;
			
			i = len;
			while (i>0) {
				t = tmp;
				t += sprintf(t, "sendmbox cnt %d", len);
				QuickHex(t, &msg[len-i], (i>64) ? 64:i);
				debugl1(cs, tmp);
				i -= 64;
			}
		}
#endif
	}
	cs->BC_Write_Reg(cs, 1, ISAR_HIS, his);
	restore_flags(flags);
	waitforHIA(cs, 10000);
	return(1);
}

/* Call only with IRQ disabled !!! */
inline void
rcv_mbox(struct IsdnCardState *cs, struct isar_reg *ireg, u_char *msg)
{
	int i;

	cs->BC_Write_Reg(cs, 1, ISAR_RADR, 0);
	if (msg && ireg->clsb) {
		msg[0] = cs->BC_Read_Reg(cs, 1, ISAR_MBOX);
		for (i=1; i < ireg->clsb; i++)
			 msg[i] = cs->BC_Read_Reg(cs, 2, ISAR_MBOX);
#if DUMP_MBOXFRAME>1
		if (cs->debug & L1_DEB_HSCX_FIFO) {
			char tmp[256], *t;
			
			i = ireg->clsb;
			while (i>0) {
				t = tmp;
				t += sprintf(t, "rcv_mbox cnt %d", ireg->clsb);
				QuickHex(t, &msg[ireg->clsb-i], (i>64) ? 64:i);
				debugl1(cs, tmp);
				i -= 64;
			}
		}
#endif
	}
	cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
}

/* Call only with IRQ disabled !!! */
inline void
get_irq_infos(struct IsdnCardState *cs, struct isar_reg *ireg)
{
	ireg->iis = cs->BC_Read_Reg(cs, 1, ISAR_IIS);
	ireg->cmsb = cs->BC_Read_Reg(cs, 1, ISAR_CTRL_H);
	ireg->clsb = cs->BC_Read_Reg(cs, 1, ISAR_CTRL_L);
#if DUMP_MBOXFRAME
	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "rcv_mbox(%02x,%02x,%d)", ireg->iis, ireg->cmsb,
			ireg->clsb);
#endif
}

int
waitrecmsg(struct IsdnCardState *cs, u_char *len,
	u_char *msg, int maxdelay)
{
	int timeout = 0;
	long flags;
	struct isar_reg *ir = cs->bcs[0].hw.isar.reg;
	
	
	while((!(cs->BC_Read_Reg(cs, 0, ISAR_IRQBIT) & ISAR_IRQSTA)) &&
		(timeout++ < maxdelay))
		udelay(1);
	if (timeout >= maxdelay) {
		printk(KERN_WARNING"isar recmsg IRQSTA timeout\n");
		return(0);
	}
	save_flags(flags);
	cli();
	get_irq_infos(cs, ir);
	rcv_mbox(cs, ir, msg);
	*len = ir->clsb;
	restore_flags(flags);
	return(1);
}

int
ISARVersion(struct IsdnCardState *cs, char *s)
{
	int ver;
	u_char msg[] = ISAR_MSG_HWVER;
	u_char tmp[64];
	u_char len;
	int debug;

	cs->cardmsg(cs, CARD_RESET,  NULL);
	/* disable ISAR IRQ */
	cs->BC_Write_Reg(cs, 0, ISAR_IRQBIT, 0);
	debug = cs->debug;
	cs->debug &= ~(L1_DEB_HSCX | L1_DEB_HSCX_FIFO);
	if (!sendmsg(cs, ISAR_HIS_VNR, 0, 3, msg))
		return(-1);
	if (!waitrecmsg(cs, &len, tmp, 100000))
		 return(-2);
	cs->debug = debug;
	if (cs->bcs[0].hw.isar.reg->iis == ISAR_IIS_VNR) {
		if (len == 1) {
			ver = tmp[0] & 0xf;
			printk(KERN_INFO "%s ISAR version %d\n", s, ver);
			return(ver);
		}
		return(-3);
	}
	return(-4);
}

int
isar_load_firmware(struct IsdnCardState *cs, u_char *buf)
{
	int ret, size, cnt, debug;
	u_char len, nom, noc;
	u_short sadr, left, *sp;
	u_char *p = buf;
	u_char *msg, *tmpmsg, *mp, tmp[64];
	long flags;
	struct isar_reg *ireg = cs->bcs[0].hw.isar.reg;
	
	struct {u_short sadr;
		u_short len;
		u_short d_key;
	} blk_head;
		
#define	BLK_HEAD_SIZE 6
	if (1 != (ret = ISARVersion(cs, "Testing"))) {
		printk(KERN_ERR"isar_load_firmware wrong isar version %d\n", ret);
		return(1);
	}
	debug = cs->debug;
#if DBG_LOADFIRM<2
	cs->debug &= ~(L1_DEB_HSCX | L1_DEB_HSCX_FIFO);
#endif
	printk(KERN_DEBUG"isar_load_firmware buf %#lx\n", (u_long)buf);
	if ((ret = verify_area(VERIFY_READ, (void *) p, sizeof(int)))) {
		printk(KERN_ERR"isar_load_firmware verify_area ret %d\n", ret);
		return ret;
	}
	if ((ret = copy_from_user(&size, p, sizeof(int)))) {
		printk(KERN_ERR"isar_load_firmware copy_from_user ret %d\n", ret);
		return ret;
	}
	p += sizeof(int);
	printk(KERN_DEBUG"isar_load_firmware size: %d\n", size);
	if ((ret = verify_area(VERIFY_READ, (void *) p, size))) {
		printk(KERN_ERR"isar_load_firmware verify_area ret %d\n", ret);
		return ret;
	}
	cnt = 0;
	/* disable ISAR IRQ */
	cs->BC_Write_Reg(cs, 0, ISAR_IRQBIT, 0);
	if (!(msg = kmalloc(256, GFP_KERNEL))) {
		printk(KERN_ERR"isar_load_firmware no buffer\n");
		return (1);
	}
	if (!(tmpmsg = kmalloc(256, GFP_KERNEL))) {
		printk(KERN_ERR"isar_load_firmware no tmp buffer\n");
		kfree(msg);
		return (1);
	}
	while (cnt < size) {
		if ((ret = copy_from_user(&blk_head, p, BLK_HEAD_SIZE))) {
			printk(KERN_ERR"isar_load_firmware copy_from_user ret %d\n", ret);
			goto reterror;
		}
#ifdef __BIG_ENDIAN
		sadr = (blk_head.sadr & 0xff)*256 + blk_head.sadr/256;
		blk_head.sadr = sadr;
		sadr = (blk_head.len & 0xff)*256 + blk_head.len/256;
		blk_head.len = sadr;
		sadr = (blk_head.d_key & 0xff)*256 + blk_head.d_key/256;
		blk_head.d_key = sadr;
#endif /* __BIG_ENDIAN */
		cnt += BLK_HEAD_SIZE;
		p += BLK_HEAD_SIZE;
		printk(KERN_DEBUG"isar firmware block (%#x,%5d,%#x)\n",
			blk_head.sadr, blk_head.len, blk_head.d_key & 0xff);
		sadr = blk_head.sadr;
		left = blk_head.len;
		if (!sendmsg(cs, ISAR_HIS_DKEY, blk_head.d_key & 0xff, 0, NULL)) {
			printk(KERN_ERR"isar sendmsg dkey failed\n");
			ret = 1;goto reterror;
		}
		if (!waitrecmsg(cs, &len, tmp, 100000)) {
			printk(KERN_ERR"isar waitrecmsg dkey failed\n");
			ret = 1;goto reterror;
		}
		if ((ireg->iis != ISAR_IIS_DKEY) || ireg->cmsb || len) {
			printk(KERN_ERR"isar wrong dkey response (%x,%x,%x)\n",
				ireg->iis, ireg->cmsb, len);
			ret = 1;goto reterror;
		}
		while (left>0) {
			noc = MIN(126, left);
			nom = 2*noc;
			mp  = msg;
			*mp++ = sadr / 256;
			*mp++ = sadr % 256;
			left -= noc;
			*mp++ = noc;
			if ((ret = copy_from_user(tmpmsg, p, nom))) {
				printk(KERN_ERR"isar_load_firmware copy_from_user ret %d\n", ret);
				goto reterror;
			}
			p += nom;
			cnt += nom;
			nom += 3;
			sp = (u_short *)tmpmsg;
#if DBG_LOADFIRM
			printk(KERN_DEBUG"isar: load %3d words at %04x\n",
				 noc, sadr);
#endif
			sadr += noc;
			while(noc) {
#ifdef __BIG_ENDIAN
				*mp++ = *sp % 256;
				*mp++ = *sp / 256;
#else
				*mp++ = *sp / 256;
				*mp++ = *sp % 256;
#endif /* __BIG_ENDIAN */
				sp++;
				noc--;
			}
			if (!sendmsg(cs, ISAR_HIS_FIRM, 0, nom, msg)) {
				printk(KERN_ERR"isar sendmsg prog failed\n");
				ret = 1;goto reterror;
			}
			if (!waitrecmsg(cs, &len, tmp, 100000)) {
				printk(KERN_ERR"isar waitrecmsg prog failed\n");
				ret = 1;goto reterror;
			}
			if ((ireg->iis != ISAR_IIS_FIRM) || ireg->cmsb || len) {
				printk(KERN_ERR"isar wrong prog response (%x,%x,%x)\n",
					ireg->iis, ireg->cmsb, len);
				ret = 1;goto reterror;
			}
		}
		printk(KERN_DEBUG"isar firmware block %5d words loaded\n",
			blk_head.len);
	}
	/* 10ms delay */
	cnt = 10;
	while (cnt--)
		udelay(1000);
	msg[0] = 0xff;
	msg[1] = 0xfe;
	ireg->bstat = 0;
	if (!sendmsg(cs, ISAR_HIS_STDSP, 0, 2, msg)) {
		printk(KERN_ERR"isar sendmsg start dsp failed\n");
		ret = 1;goto reterror;
	}
	if (!waitrecmsg(cs, &len, tmp, 100000)) {
		printk(KERN_ERR"isar waitrecmsg start dsp failed\n");
		ret = 1;goto reterror;
	}
	if ((ireg->iis != ISAR_IIS_STDSP) || ireg->cmsb || len) {
		printk(KERN_ERR"isar wrong start dsp response (%x,%x,%x)\n",
			ireg->iis, ireg->cmsb, len);
		ret = 1;goto reterror;
	} else
		printk(KERN_DEBUG"isar start dsp success\n");
	/* NORMAL mode entered */
	/* Enable IRQs of ISAR */
	cs->BC_Write_Reg(cs, 0, ISAR_IRQBIT, ISAR_IRQSTA);
	save_flags(flags);
	sti();
	cnt = 1000; /* max 1s */
	while ((!ireg->bstat) && cnt) {
		udelay(1000);
		cnt--;
	}
	if (!cnt) {
		printk(KERN_ERR"isar no general status event received\n");
		ret = 1;goto reterrflg;
	} else {
		printk(KERN_DEBUG"isar general status event %x\n",
			ireg->bstat);
	}
	/* 10ms delay */
	cnt = 10;
	while (cnt--)
		udelay(1000);
	ireg->iis = 0;
	if (!sendmsg(cs, ISAR_HIS_DIAG, ISAR_CTRL_STST, 0, NULL)) {
		printk(KERN_ERR"isar sendmsg self tst failed\n");
		ret = 1;goto reterrflg;
	}
	cnt = 10000; /* max 100 ms */
	while ((ireg->iis != ISAR_IIS_DIAG) && cnt) {
		udelay(10);
		cnt--;
	}
	udelay(1000);
	if (!cnt) {
		printk(KERN_ERR"isar no self tst response\n");
		ret = 1;goto reterrflg;
	}
	if ((ireg->cmsb == ISAR_CTRL_STST) && (ireg->clsb == 1)
		&& (ireg->par[0] == 0)) {
		printk(KERN_DEBUG"isar selftest OK\n");
	} else {
		printk(KERN_DEBUG"isar selftest not OK %x/%x/%x\n",
			ireg->cmsb, ireg->clsb, ireg->par[0]);
		ret = 1;goto reterror;
	}
	ireg->iis = 0;
	if (!sendmsg(cs, ISAR_HIS_DIAG, ISAR_CTRL_SWVER, 0, NULL)) {
		printk(KERN_ERR"isar RQST SVN failed\n");
		ret = 1;goto reterror;
	}
	cnt = 30000; /* max 300 ms */
	while ((ireg->iis != ISAR_IIS_DIAG) && cnt) {
		udelay(10);
		cnt--;
	}
	udelay(1000);
	if (!cnt) {
		printk(KERN_ERR"isar no SVN response\n");
		ret = 1;goto reterrflg;
	} else {
		if ((ireg->cmsb == ISAR_CTRL_SWVER) && (ireg->clsb == 1))
			printk(KERN_DEBUG"isar software version %#x\n",
				ireg->par[0]);
		else {
			printk(KERN_ERR"isar wrong swver response (%x,%x) cnt(%d)\n",
				ireg->cmsb, ireg->clsb, cnt);
			ret = 1;goto reterrflg;
		}
	}
	cs->debug = debug;
	isar_setup(cs);
	ret = 0;
reterrflg:
	restore_flags(flags);
reterror:
	cs->debug = debug;
	if (ret)
		/* disable ISAR IRQ */
		cs->BC_Write_Reg(cs, 0, ISAR_IRQBIT, 0);
	kfree(msg);
	kfree(tmpmsg);
	return(ret);
}

extern void BChannel_bh(struct BCState *);
#define B_LL_NOCARRIER	8
#define B_LL_CONNECT	9
#define B_LL_OK		10

static void
isar_bh(struct BCState *bcs)
{
	BChannel_bh(bcs);
	if (test_and_clear_bit(B_LL_NOCARRIER, &bcs->event))
		ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_NOCARR);
	if (test_and_clear_bit(B_LL_CONNECT, &bcs->event))
		ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_CONNECT);
	if (test_and_clear_bit(B_LL_OK, &bcs->event))
		ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_OK);
}

static void
isar_sched_event(struct BCState *bcs, int event)
{
	bcs->event |= 1 << event;
	queue_task(&bcs->tqueue, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
}

static inline void
send_DLE_ETX(struct BCState *bcs)
{
	u_char dleetx[2] = {DLE,ETX};
	struct sk_buff *skb;
	
	if ((skb = dev_alloc_skb(2))) {
		memcpy(skb_put(skb, 2), dleetx, 2);
		skb_queue_tail(&bcs->rqueue, skb);
		isar_sched_event(bcs, B_RCVBUFREADY);
	} else {
		printk(KERN_WARNING "HiSax: skb out of memory\n");
	}
}

static inline int
dle_count(unsigned char *buf, int len)
{
	int count = 0;

	while (len--)
		if (*buf++ == DLE)
			count++;
	return count;
}

static inline void
insert_dle(unsigned char *dest, unsigned char *src, int count) {
	/* <DLE> in input stream have to be flagged as <DLE><DLE> */
	while (count--) {
		*dest++ = *src;
		if (*src++ == DLE)
			*dest++ = DLE;
	}
}
 
static inline void
isar_rcv_frame(struct IsdnCardState *cs, struct BCState *bcs)
{
	u_char *ptr;
	struct sk_buff *skb;
	struct isar_reg *ireg = bcs->hw.isar.reg;
	
	if (!ireg->clsb) {
		debugl1(cs, "isar zero len frame");
		cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
		return;
	}
	switch (bcs->mode) {
	case L1_MODE_NULL:
		debugl1(cs, "isar mode 0 spurious IIS_RDATA %x/%x/%x",
			ireg->iis, ireg->cmsb, ireg->clsb);
		printk(KERN_WARNING"isar mode 0 spurious IIS_RDATA %x/%x/%x\n",
			ireg->iis, ireg->cmsb, ireg->clsb);
		cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
		break;
	case L1_MODE_TRANS:
	case L1_MODE_V32:
		if ((skb = dev_alloc_skb(ireg->clsb))) {
			rcv_mbox(cs, ireg, (u_char *)skb_put(skb, ireg->clsb));
			skb_queue_tail(&bcs->rqueue, skb);
			isar_sched_event(bcs, B_RCVBUFREADY);
		} else {
			printk(KERN_WARNING "HiSax: skb out of memory\n");
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
		}
		break;
	case L1_MODE_HDLC:
		if ((bcs->hw.isar.rcvidx + ireg->clsb) > HSCX_BUFMAX) {
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "isar_rcv_frame: incoming packet too large");
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			bcs->hw.isar.rcvidx = 0;
		} else if (ireg->cmsb & HDLC_ERROR) {
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "isar frame error %x len %d",
					ireg->cmsb, ireg->clsb);
#ifdef ERROR_STATISTIC
			if (ireg->cmsb & HDLC_ERR_RER)
				bcs->err_inv++;
			if (ireg->cmsb & HDLC_ERR_CER)
				bcs->err_crc++;
#endif
			bcs->hw.isar.rcvidx = 0;
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
		} else {
			if (ireg->cmsb & HDLC_FSD)
				bcs->hw.isar.rcvidx = 0;
			ptr = bcs->hw.isar.rcvbuf + bcs->hw.isar.rcvidx;
			bcs->hw.isar.rcvidx += ireg->clsb;
			rcv_mbox(cs, ireg, ptr);
			if (ireg->cmsb & HDLC_FED) {
				if (bcs->hw.isar.rcvidx < 3) { /* last 2 bytes are the FCS */
					if (cs->debug & L1_DEB_WARN)
						debugl1(cs, "isar frame to short %d",
							bcs->hw.isar.rcvidx);
				} else if (!(skb = dev_alloc_skb(bcs->hw.isar.rcvidx-2))) {
					printk(KERN_WARNING "ISAR: receive out of memory\n");
				} else {
					memcpy(skb_put(skb, bcs->hw.isar.rcvidx-2),
						bcs->hw.isar.rcvbuf, bcs->hw.isar.rcvidx-2);
					skb_queue_tail(&bcs->rqueue, skb);
					isar_sched_event(bcs, B_RCVBUFREADY);
				}
				bcs->hw.isar.rcvidx = 0;
			}
		}
		break;
	case L1_MODE_FAX:
		if (bcs->hw.isar.state != STFAX_ACTIV) {
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "isar_rcv_frame: not ACTIV");
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			bcs->hw.isar.rcvidx = 0;
			break;
		}
		if (bcs->hw.isar.cmd == PCTRL_CMD_FRM) {
			rcv_mbox(cs, ireg, bcs->hw.isar.rcvbuf);
			bcs->hw.isar.rcvidx = ireg->clsb +
				dle_count(bcs->hw.isar.rcvbuf, ireg->clsb);
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "isar_rcv_frame: raw(%d) dle(%d)",
					ireg->clsb, bcs->hw.isar.rcvidx);
			if ((skb = dev_alloc_skb(bcs->hw.isar.rcvidx))) {
				insert_dle((u_char *)skb_put(skb, bcs->hw.isar.rcvidx),
					bcs->hw.isar.rcvbuf, ireg->clsb);
				skb_queue_tail(&bcs->rqueue, skb);
				isar_sched_event(bcs, B_RCVBUFREADY);
				if (ireg->cmsb & SART_NMD) { /* ABORT */
					if (cs->debug & L1_DEB_WARN)
						debugl1(cs, "isar_rcv_frame: no more data");
					cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
					bcs->hw.isar.rcvidx = 0;
					send_DLE_ETX(bcs);
					sendmsg(cs, SET_DPS(bcs->hw.isar.dpath) |
						ISAR_HIS_PUMPCTRL, PCTRL_CMD_ESC,
						0, NULL);
					bcs->hw.isar.state = STFAX_ESCAPE;
					isar_sched_event(bcs, B_LL_NOCARRIER);
				}
			} else {
				printk(KERN_WARNING "HiSax: skb out of memory\n");
				cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			}
			break;
		}
		if (bcs->hw.isar.cmd != PCTRL_CMD_FRH) {
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "isar_rcv_frame: unknown fax mode %x",
					bcs->hw.isar.cmd);
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			bcs->hw.isar.rcvidx = 0;
			break;
		}
		/* PCTRL_CMD_FRH */
		if ((bcs->hw.isar.rcvidx + ireg->clsb) > HSCX_BUFMAX) {
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "isar_rcv_frame: incoming packet too large");
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			bcs->hw.isar.rcvidx = 0;
		} else if (ireg->cmsb & HDLC_ERROR) {
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "isar frame error %x len %d",
					ireg->cmsb, ireg->clsb);
			bcs->hw.isar.rcvidx = 0;
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
		} else {
			if (ireg->cmsb & HDLC_FSD)
				bcs->hw.isar.rcvidx = 0;
			ptr = bcs->hw.isar.rcvbuf + bcs->hw.isar.rcvidx;
			bcs->hw.isar.rcvidx += ireg->clsb;
			rcv_mbox(cs, ireg, ptr);
			if (ireg->cmsb & HDLC_FED) {
				int len = bcs->hw.isar.rcvidx +
					dle_count(bcs->hw.isar.rcvbuf, bcs->hw.isar.rcvidx);
				if (bcs->hw.isar.rcvidx < 3) { /* last 2 bytes are the FCS */
					if (cs->debug & L1_DEB_WARN)
						debugl1(cs, "isar frame to short %d",
							bcs->hw.isar.rcvidx);
				} else if (!(skb = dev_alloc_skb(bcs->hw.isar.rcvidx))) {
					printk(KERN_WARNING "ISAR: receive out of memory\n");
				} else {
					insert_dle((u_char *)skb_put(skb, len),
						bcs->hw.isar.rcvbuf,
						bcs->hw.isar.rcvidx);
					skb_queue_tail(&bcs->rqueue, skb);
					isar_sched_event(bcs, B_RCVBUFREADY);
					send_DLE_ETX(bcs);
					isar_sched_event(bcs, B_LL_OK);
				}
				bcs->hw.isar.rcvidx = 0;
			}
		}
		if (ireg->cmsb & SART_NMD) { /* ABORT */
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "isar_rcv_frame: no more data");
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			bcs->hw.isar.rcvidx = 0;
			send_DLE_ETX(bcs);
			sendmsg(cs, SET_DPS(bcs->hw.isar.dpath) |
				ISAR_HIS_PUMPCTRL, PCTRL_CMD_ESC, 0, NULL);
			bcs->hw.isar.state = STFAX_ESCAPE;
			isar_sched_event(bcs, B_LL_NOCARRIER);
		}
		break;
	default:
		printk(KERN_ERR"isar_rcv_frame mode (%x)error\n", bcs->mode);
		cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
		break;
	}
}

void
isar_fill_fifo(struct BCState *bcs)
{
	struct IsdnCardState *cs = bcs->cs;
	int count;
	u_char msb;
	u_char *ptr;
	long flags;

	if ((cs->debug & L1_DEB_HSCX) && !(cs->debug & L1_DEB_HSCX_FIFO))
		debugl1(cs, "isar_fill_fifo");
	if (!bcs->tx_skb)
		return;
	if (bcs->tx_skb->len <= 0)
		return;
	if (!(bcs->hw.isar.reg->bstat & 
		(bcs->hw.isar.dpath == 1 ? BSTAT_RDM1 : BSTAT_RDM2)))
		return;
	if (bcs->tx_skb->len > bcs->hw.isar.mml) {
		msb = 0;
		count = bcs->hw.isar.mml;
	} else {
		count = bcs->tx_skb->len;
		msb = HDLC_FED;
	}
	save_flags(flags);
	cli();
	ptr = bcs->tx_skb->data;
	if (!bcs->hw.isar.txcnt) {
		msb |= HDLC_FST;
		if ((bcs->mode == L1_MODE_FAX) &&
			(bcs->hw.isar.cmd == PCTRL_CMD_FTH)) {
			if (bcs->tx_skb->len > 1) {
				if ((ptr[0]== 0xff) && (ptr[1] == 0x13))
					/* last frame */
					test_and_set_bit(BC_FLG_LASTDATA,
						&bcs->Flag);
			}  
		}
	}
	skb_pull(bcs->tx_skb, count);
	bcs->tx_cnt -= count;
	bcs->hw.isar.txcnt += count;
	switch (bcs->mode) {
		case L1_MODE_NULL:
			printk(KERN_ERR"isar_fill_fifo wrong mode 0\n");
			break;
		case L1_MODE_TRANS:
		case L1_MODE_V32:
			sendmsg(cs, SET_DPS(bcs->hw.isar.dpath) | ISAR_HIS_SDATA,
				0, count, ptr);
			break;
		case L1_MODE_HDLC:
			sendmsg(cs, SET_DPS(bcs->hw.isar.dpath) | ISAR_HIS_SDATA,
				msb, count, ptr);
			break;
		case L1_MODE_FAX:
			if (bcs->hw.isar.state != STFAX_ACTIV) {
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "isar_fill_fifo: not ACTIV");
			} else if (bcs->hw.isar.cmd == PCTRL_CMD_FTH) { 
				sendmsg(cs, SET_DPS(bcs->hw.isar.dpath) | ISAR_HIS_SDATA,
					msb, count, ptr);
			} else if (bcs->hw.isar.cmd == PCTRL_CMD_FTM) {
				sendmsg(cs, SET_DPS(bcs->hw.isar.dpath) | ISAR_HIS_SDATA,
					0, count, ptr);
			} else {
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "isar_fill_fifo: not FTH/FTM");
			}
			break;
		default:
			if (cs->debug)
				debugl1(cs, "isar_fill_fifo mode(%x) error", bcs->mode);
			printk(KERN_ERR"isar_fill_fifo mode(%x) error\n", bcs->mode);
			break;
	}
	restore_flags(flags);
}

inline
struct BCState *sel_bcs_isar(struct IsdnCardState *cs, u_char dpath)
{
	if ((!dpath) || (dpath == 3))
		return(NULL);
	if (cs->bcs[0].hw.isar.dpath == dpath)
		return(&cs->bcs[0]);
	if (cs->bcs[1].hw.isar.dpath == dpath)
		return(&cs->bcs[1]);
	return(NULL);
}

inline void
send_frames(struct BCState *bcs)
{
	if (bcs->tx_skb) {
		if (bcs->tx_skb->len) {
			isar_fill_fifo(bcs);
			return;
		} else {
			if (bcs->st->lli.l1writewakeup &&
				(PACKET_NOACK != bcs->tx_skb->pkt_type))
					bcs->st->lli.l1writewakeup(bcs->st, bcs->hw.isar.txcnt);
			if (bcs->mode == L1_MODE_FAX) {
				if (bcs->hw.isar.cmd == PCTRL_CMD_FTH) {
					if (test_bit(BC_FLG_LASTDATA, &bcs->Flag)) {
						test_and_set_bit(BC_FLG_NMD_DATA, &bcs->Flag);
					}
				} else if (bcs->hw.isar.cmd == PCTRL_CMD_FTM) {
					if (test_bit(BC_FLG_DLEETX, &bcs->Flag)) {
						test_and_set_bit(BC_FLG_LASTDATA, &bcs->Flag);
						test_and_set_bit(BC_FLG_NMD_DATA, &bcs->Flag);
					}
				}
			}
			dev_kfree_skb_any(bcs->tx_skb);
			bcs->hw.isar.txcnt = 0; 
			bcs->tx_skb = NULL;
		}
	}
	if ((bcs->tx_skb = skb_dequeue(&bcs->squeue))) {
		bcs->hw.isar.txcnt = 0;
		test_and_set_bit(BC_FLG_BUSY, &bcs->Flag);
		isar_fill_fifo(bcs);
	} else {
		if (test_and_clear_bit(BC_FLG_DLEETX, &bcs->Flag)) {
			if (test_and_clear_bit(BC_FLG_LASTDATA, &bcs->Flag)) {
				if (test_and_clear_bit(BC_FLG_NMD_DATA, &bcs->Flag)) {
					u_char dummy = 0;
					sendmsg(bcs->cs, SET_DPS(bcs->hw.isar.dpath) |
						ISAR_HIS_SDATA, 0x01, 1, &dummy);
				}
				test_and_set_bit(BC_FLG_LL_OK, &bcs->Flag);
			} else {
				isar_sched_event(bcs, B_LL_CONNECT);
			}
		}
		test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
		isar_sched_event(bcs, B_XMTBUFREADY);
	}
}

inline void
check_send(struct IsdnCardState *cs, u_char rdm)
{
	struct BCState *bcs;
	
	if (rdm & BSTAT_RDM1) {
		if ((bcs = sel_bcs_isar(cs, 1))) {
			if (bcs->mode) {
				send_frames(bcs);
			}
		}
	}
	if (rdm & BSTAT_RDM2) {
		if ((bcs = sel_bcs_isar(cs, 2))) {
			if (bcs->mode) {
				send_frames(bcs);
			}
		}
	}
	
}

const char *dmril[] = {"NO SPEED", "1200/75", "NODEF2", "75/1200", "NODEF4",
			"300", "600", "1200", "2400", "4800", "7200",
			"9600nt", "9600t", "12000", "14400", "WRONG"};
const char *dmrim[] = {"NO MOD", "NO DEF", "V32/V32b", "V22", "V21",
			"Bell103", "V23", "Bell202", "V17", "V29", "V27ter"};

static void
isar_pump_status_rsp(struct BCState *bcs, struct isar_reg *ireg) {
	struct IsdnCardState *cs = bcs->cs;
	u_char ril = ireg->par[0];
	u_char rim;

	if (!test_and_clear_bit(ISAR_RATE_REQ, &bcs->hw.isar.reg->Flags))
		return; 
	if (ril > 14) {
		if (cs->debug & L1_DEB_WARN)
			debugl1(cs, "wrong pstrsp ril=%d",ril);
		ril = 15;
	}
	switch(ireg->par[1]) {
		case 0:
			rim = 0;
			break;
		case 0x20:
			rim = 2;
			break;
		case 0x40:
			rim = 3;
			break;
		case 0x41:
			rim = 4;
			break;
		case 0x51:
			rim = 5;
			break;
		case 0x61:
			rim = 6;
			break;
		case 0x71:
			rim = 7;
			break;
		case 0x82:
			rim = 8;
			break;
		case 0x92:
			rim = 9;
			break;
		case 0xa2:
			rim = 10;
			break;
		default:
			rim = 1;
			break;
	}
	sprintf(bcs->hw.isar.conmsg,"%s %s", dmril[ril], dmrim[rim]);
	bcs->conmsg = bcs->hw.isar.conmsg;
	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "pump strsp %s", bcs->conmsg);
}

static void
isar_pump_statev_modem(struct BCState *bcs, u_char devt) {
	struct IsdnCardState *cs = bcs->cs;
	u_char dps = SET_DPS(bcs->hw.isar.dpath);

	switch(devt) {
		case PSEV_10MS_TIMER:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev TIMER");
			break;
		case PSEV_CON_ON:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev CONNECT");
			l1_msg_b(bcs->st, PH_ACTIVATE | REQUEST, NULL);
			break;
		case PSEV_CON_OFF:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev NO CONNECT");
			sendmsg(cs, dps | ISAR_HIS_PSTREQ, 0, 0, NULL);
			l1_msg_b(bcs->st, PH_DEACTIVATE | REQUEST, NULL);
			break;
		case PSEV_V24_OFF:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev V24 OFF");
			break;
		case PSEV_CTS_ON:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev CTS ON");
			break;
		case PSEV_CTS_OFF:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev CTS OFF");
			break;
		case PSEV_DCD_ON:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev CARRIER ON");
			test_and_set_bit(ISAR_RATE_REQ, &bcs->hw.isar.reg->Flags); 
			sendmsg(cs, dps | ISAR_HIS_PSTREQ, 0, 0, NULL);
			break;
		case PSEV_DCD_OFF:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev CARRIER OFF");
			break;
		case PSEV_DSR_ON:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev DSR ON");
			break;
		case PSEV_DSR_OFF:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev DSR_OFF");
			break;
		case PSEV_REM_RET:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev REMOTE RETRAIN");
			break;
		case PSEV_REM_REN:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev REMOTE RENEGOTIATE");
			break;
		case PSEV_GSTN_CLR:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev GSTN CLEAR", devt);
			break;
		default:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "unknown pump stev %x", devt);
			break;
	}
}

static inline void
ll_deliver_faxstat(struct BCState *bcs, u_char status)
{
        isdn_ctrl ic;
	struct Channel *chanp = (struct Channel *) bcs->st->lli.userdata;
 
	if (bcs->cs->debug & L1_DEB_HSCX)
		debugl1(bcs->cs, "HL->LL FAXIND %x", status);
	ic.driver = bcs->cs->myid;
	ic.command = ISDN_STAT_FAXIND;
	ic.arg = chanp->chan;
	ic.parm.aux.cmd = status;
	bcs->cs->iif.statcallb(&ic);
}

static void
isar_pump_statev_fax(struct BCState *bcs, u_char devt) {
	struct IsdnCardState *cs = bcs->cs;
	u_char dps = SET_DPS(bcs->hw.isar.dpath);
	u_char p1;

	switch(devt) {
		case PSEV_10MS_TIMER:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev TIMER");
			break;
		case PSEV_RSP_READY:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev RSP_READY");
			bcs->hw.isar.state = STFAX_READY;
			l1_msg_b(bcs->st, PH_ACTIVATE | REQUEST, NULL);
			if (test_bit(BC_FLG_ORIG, &bcs->Flag)) {
				isar_pump_cmd(bcs, ISDN_FAX_CLASS1_FRH, 3);
			} else {
				isar_pump_cmd(bcs, ISDN_FAX_CLASS1_FTH, 3);
			}
			break;
		case PSEV_LINE_TX_H:
			if (bcs->hw.isar.state == STFAX_LINE) {
				if (cs->debug & L1_DEB_HSCX)
					debugl1(cs, "pump stev LINE_TX_H");
				bcs->hw.isar.state = STFAX_CONT;
				sendmsg(cs, dps | ISAR_HIS_PUMPCTRL, PCTRL_CMD_CONT, 0, NULL);
			} else {
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "pump stev LINE_TX_H wrong st %x",
						bcs->hw.isar.state);
			}
			break;
		case PSEV_LINE_RX_H:
			if (bcs->hw.isar.state == STFAX_LINE) {
				if (cs->debug & L1_DEB_HSCX)
					debugl1(cs, "pump stev LINE_RX_H");
				bcs->hw.isar.state = STFAX_CONT;
				sendmsg(cs, dps | ISAR_HIS_PUMPCTRL, PCTRL_CMD_CONT, 0, NULL);
			} else {
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "pump stev LINE_RX_H wrong st %x",
						bcs->hw.isar.state);
			}
			break;
		case PSEV_LINE_TX_B:
			if (bcs->hw.isar.state == STFAX_LINE) {
				if (cs->debug & L1_DEB_HSCX)
					debugl1(cs, "pump stev LINE_TX_B");
				bcs->hw.isar.state = STFAX_CONT;
				sendmsg(cs, dps | ISAR_HIS_PUMPCTRL, PCTRL_CMD_CONT, 0, NULL);
			} else {
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "pump stev LINE_TX_B wrong st %x",
						bcs->hw.isar.state);
			}
			break;
		case PSEV_LINE_RX_B:
			if (bcs->hw.isar.state == STFAX_LINE) {
				if (cs->debug & L1_DEB_HSCX)
					debugl1(cs, "pump stev LINE_RX_B");
				bcs->hw.isar.state = STFAX_CONT;
				sendmsg(cs, dps | ISAR_HIS_PUMPCTRL, PCTRL_CMD_CONT, 0, NULL);
			} else {
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "pump stev LINE_RX_B wrong st %x",
						bcs->hw.isar.state);
			}
			break;
		case PSEV_RSP_CONN:
			if (bcs->hw.isar.state == STFAX_CONT) {
				if (cs->debug & L1_DEB_HSCX)
					debugl1(cs, "pump stev RSP_CONN");
				bcs->hw.isar.state = STFAX_ACTIV;
				test_and_set_bit(ISAR_RATE_REQ, &bcs->hw.isar.reg->Flags);
				sendmsg(cs, dps | ISAR_HIS_PSTREQ, 0, 0, NULL);
				if (bcs->hw.isar.cmd == PCTRL_CMD_FTH) {
					/* 1s Flags before data */
					if (test_and_set_bit(BC_FLG_FTI_RUN, &bcs->Flag))
						del_timer(&bcs->hw.isar.ftimer);
					/* 1000 ms */
					bcs->hw.isar.ftimer.expires =
						jiffies + ((1000 * HZ)/1000);
					test_and_set_bit(BC_FLG_LL_CONN,
						&bcs->Flag);
					add_timer(&bcs->hw.isar.ftimer);
				} else {
					isar_sched_event(bcs, B_LL_CONNECT);
				}
			} else {
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "pump stev RSP_CONN wrong st %x",
						bcs->hw.isar.state);
			}
			break;
		case PSEV_FLAGS_DET:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev FLAGS_DET");
			break;
		case PSEV_RSP_DISC:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev RSP_DISC");
			if (bcs->hw.isar.state == STFAX_ESCAPE) {
				switch(bcs->hw.isar.newcmd) {
					case 0:
						bcs->hw.isar.state = STFAX_READY;
						break;
					case PCTRL_CMD_FTH:
					case PCTRL_CMD_FTM:
						p1 = 10;
						sendmsg(cs, dps | ISAR_HIS_PUMPCTRL,
							PCTRL_CMD_SILON, 1, &p1);
						bcs->hw.isar.state = STFAX_SILDET;
						break;
					case PCTRL_CMD_FRH:
					case PCTRL_CMD_FRM:
						p1 = bcs->hw.isar.mod = bcs->hw.isar.newmod;
						bcs->hw.isar.newmod = 0;
						bcs->hw.isar.cmd = bcs->hw.isar.newcmd;
						bcs->hw.isar.newcmd = 0;
						sendmsg(cs, dps | ISAR_HIS_PUMPCTRL,
							bcs->hw.isar.cmd, 1, &p1);
						bcs->hw.isar.state = STFAX_LINE;
						bcs->hw.isar.try_mod = 3;
						break;
					default:
						if (cs->debug & L1_DEB_HSCX)
							debugl1(cs, "RSP_DISC unknown newcmd %x", bcs->hw.isar.newcmd);
						break;
				}
			} else if (bcs->hw.isar.state == STFAX_ACTIV) {
				if (test_and_clear_bit(BC_FLG_LL_OK, &bcs->Flag)) {
					isar_sched_event(bcs, B_LL_OK);
				} else if (bcs->hw.isar.cmd == PCTRL_CMD_FRM) {
					send_DLE_ETX(bcs);
					isar_sched_event(bcs, B_LL_NOCARRIER);
				} else {
					ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_FCERROR);
				}
				bcs->hw.isar.state = STFAX_READY;
			} else {
				bcs->hw.isar.state = STFAX_READY;
				ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_FCERROR);
			}
			break;
		case PSEV_RSP_SILDET:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev RSP_SILDET");
			if (bcs->hw.isar.state == STFAX_SILDET) {
				p1 = bcs->hw.isar.mod = bcs->hw.isar.newmod;
				bcs->hw.isar.newmod = 0;
				bcs->hw.isar.cmd = bcs->hw.isar.newcmd;
				bcs->hw.isar.newcmd = 0;
				sendmsg(cs, dps | ISAR_HIS_PUMPCTRL,
					bcs->hw.isar.cmd, 1, &p1);
				bcs->hw.isar.state = STFAX_LINE;
				bcs->hw.isar.try_mod = 3;
			}
			break;
		case PSEV_RSP_SILOFF:
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev RSP_SILOFF");
			break;
		case PSEV_RSP_FCERR:
			if (bcs->hw.isar.state == STFAX_LINE) {
				if (cs->debug & L1_DEB_HSCX)
					debugl1(cs, "pump stev RSP_FCERR try %d",
						bcs->hw.isar.try_mod);
				if (bcs->hw.isar.try_mod--) {
					sendmsg(cs, dps | ISAR_HIS_PUMPCTRL,
						bcs->hw.isar.cmd, 1,
						&bcs->hw.isar.mod);
					break;
				}
			}
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "pump stev RSP_FCERR");
			bcs->hw.isar.state = STFAX_ESCAPE;
			sendmsg(cs, dps | ISAR_HIS_PUMPCTRL, PCTRL_CMD_ESC, 0, NULL);
			ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_FCERROR);
			break;
		default:
			break;
	}
}

static char debbuf[128];

void
isar_int_main(struct IsdnCardState *cs)
{
	long flags;
	struct isar_reg *ireg = cs->bcs[0].hw.isar.reg;
	struct BCState *bcs;

	save_flags(flags);
	cli();
	get_irq_infos(cs, ireg);
	switch (ireg->iis & ISAR_IIS_MSCMSD) {
		case ISAR_IIS_RDATA:
			if ((bcs = sel_bcs_isar(cs, ireg->iis >> 6))) {
				isar_rcv_frame(cs, bcs);
			} else {
				debugl1(cs, "isar spurious IIS_RDATA %x/%x/%x",
					ireg->iis, ireg->cmsb, ireg->clsb);
				cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			}
			break;
		case ISAR_IIS_GSTEV:
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			ireg->bstat |= ireg->cmsb;
			check_send(cs, ireg->cmsb);
			break;
		case ISAR_IIS_BSTEV:
#ifdef ERROR_STATISTIC
			if ((bcs = sel_bcs_isar(cs, ireg->iis >> 6))) {
				if (ireg->cmsb == BSTEV_TBO)
					bcs->err_tx++;
				if (ireg->cmsb == BSTEV_RBO)
					bcs->err_rdo++;
			}
#endif
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "Buffer STEV dpath%d msb(%x)",
					ireg->iis>>6, ireg->cmsb);
			cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			break;
		case ISAR_IIS_PSTEV:
			if ((bcs = sel_bcs_isar(cs, ireg->iis >> 6))) {
				rcv_mbox(cs, ireg, (u_char *)ireg->par);
				if (bcs->mode == L1_MODE_V32) {
					isar_pump_statev_modem(bcs, ireg->cmsb);
				} else if (bcs->mode == L1_MODE_FAX) {
					isar_pump_statev_fax(bcs, ireg->cmsb);
				} else {
					if (cs->debug & L1_DEB_WARN)
						debugl1(cs, "isar IIS_PSTEV pmode %d stat %x",
							bcs->mode, ireg->cmsb);
				}
			} else {
				debugl1(cs, "isar spurious IIS_PSTEV %x/%x/%x",
					ireg->iis, ireg->cmsb, ireg->clsb);
				cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			}
			break;
		case ISAR_IIS_PSTRSP:
			if ((bcs = sel_bcs_isar(cs, ireg->iis >> 6))) {
				rcv_mbox(cs, ireg, (u_char *)ireg->par);
				isar_pump_status_rsp(bcs, ireg);
			} else {
				debugl1(cs, "isar spurious IIS_PSTRSP %x/%x/%x",
					ireg->iis, ireg->cmsb, ireg->clsb);
				cs->BC_Write_Reg(cs, 1, ISAR_IIA, 0);
			}
			break;
		case ISAR_IIS_DIAG:
		case ISAR_IIS_BSTRSP:
		case ISAR_IIS_IOM2RSP:
			rcv_mbox(cs, ireg, (u_char *)ireg->par);
			if ((cs->debug & (L1_DEB_HSCX | L1_DEB_HSCX_FIFO))
				== L1_DEB_HSCX) {
				u_char *tp=debbuf;

				tp += sprintf(debbuf, "msg iis(%x) msb(%x)",
					ireg->iis, ireg->cmsb);
				QuickHex(tp, (u_char *)ireg->par, ireg->clsb);
				debugl1(cs, debbuf);
			}
			break;
		case ISAR_IIS_INVMSG:
			rcv_mbox(cs, ireg, debbuf);
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "invalid msg his:%x",
					ireg->cmsb);
			break;
		default:
			rcv_mbox(cs, ireg, debbuf);
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "unhandled msg iis(%x) ctrl(%x/%x)",
					ireg->iis, ireg->cmsb, ireg->clsb);
			break;
	}
	restore_flags(flags);
}

static void
ftimer_handler(struct BCState *bcs) {
	if (bcs->cs->debug)
		debugl1(bcs->cs, "ftimer flags %04x",
			bcs->Flag);
	test_and_clear_bit(BC_FLG_FTI_RUN, &bcs->Flag);
	if (test_and_clear_bit(BC_FLG_LL_CONN, &bcs->Flag)) {
		isar_sched_event(bcs, B_LL_CONNECT);
	}
}

static void
setup_pump(struct BCState *bcs) {
	struct IsdnCardState *cs = bcs->cs;
	u_char dps = SET_DPS(bcs->hw.isar.dpath);
	u_char ctrl, param[6];

	switch (bcs->mode) {
		case L1_MODE_NULL:
		case L1_MODE_TRANS:
		case L1_MODE_HDLC:
			sendmsg(cs, dps | ISAR_HIS_PUMPCFG, PMOD_BYPASS, 0, NULL);
			break;
		case L1_MODE_V32:
			ctrl = PMOD_DATAMODEM;
			if (test_bit(BC_FLG_ORIG, &bcs->Flag)) {
				ctrl |= PCTRL_ORIG;
				param[5] = PV32P6_CTN;
			} else {
				param[5] = PV32P6_ATN;
			}
			param[0] = 6; /* 6 db */
			param[1] = PV32P2_V23R | PV32P2_V22A | PV32P2_V22B |
				   PV32P2_V22C | PV32P2_V21 | PV32P2_BEL; 
			param[2] = PV32P3_AMOD | PV32P3_V32B | PV32P3_V23B;
			param[3] = PV32P4_UT144;
			param[4] = PV32P5_UT144;
			sendmsg(cs, dps | ISAR_HIS_PUMPCFG, ctrl, 6, param);
			break;
		case L1_MODE_FAX:
			ctrl = PMOD_FAX;
			if (test_bit(BC_FLG_ORIG, &bcs->Flag)) {
				ctrl |= PCTRL_ORIG;
				param[1] = PFAXP2_CTN;
			} else {
				param[1] = PFAXP2_ATN;
			}
			param[0] = 6; /* 6 db */
			sendmsg(cs, dps | ISAR_HIS_PUMPCFG, ctrl, 2, param);
			bcs->hw.isar.state = STFAX_NULL;
			bcs->hw.isar.newcmd = 0;
			bcs->hw.isar.newmod = 0;
			test_and_set_bit(BC_FLG_FTI_RUN, &bcs->Flag);
			break;
	}
	udelay(1000);
	sendmsg(cs, dps | ISAR_HIS_PSTREQ, 0, 0, NULL);
	udelay(1000);
}

static void
setup_sart(struct BCState *bcs) {
	struct IsdnCardState *cs = bcs->cs;
	u_char dps = SET_DPS(bcs->hw.isar.dpath);
	u_char ctrl, param[2];
	
	switch (bcs->mode) {
		case L1_MODE_NULL:
			sendmsg(cs, dps | ISAR_HIS_SARTCFG, SMODE_DISABLE, 0,
				NULL);
			break;
		case L1_MODE_TRANS:
			sendmsg(cs, dps | ISAR_HIS_SARTCFG, SMODE_BINARY, 2,
				"\0\0");
			break;
		case L1_MODE_HDLC:
		case L1_MODE_FAX:
			param[0] = 0;
			sendmsg(cs, dps | ISAR_HIS_SARTCFG, SMODE_HDLC, 1,
				param);
			break;
		case L1_MODE_V32:
			ctrl = SMODE_V14 | SCTRL_HDMC_BOTH;
			param[0] = S_P1_CHS_8;
			param[1] = S_P2_BFT_DEF;
			sendmsg(cs, dps | ISAR_HIS_SARTCFG, ctrl, 2,
				param);
			break;
	}
	udelay(1000);
	sendmsg(cs, dps | ISAR_HIS_BSTREQ, 0, 0, NULL);
	udelay(1000);
}

static void
setup_iom2(struct BCState *bcs) {
	struct IsdnCardState *cs = bcs->cs;
	u_char dps = SET_DPS(bcs->hw.isar.dpath);
	u_char cmsb = IOM_CTRL_ENA, msg[5] = {IOM_P1_TXD,0,0,0,0};
	
	if (bcs->channel)
		msg[1] = msg[3] = 1;
	switch (bcs->mode) {
		case L1_MODE_NULL:
			cmsb = 0;
			/* dummy slot */
			msg[1] = msg[3] = bcs->hw.isar.dpath + 2;
			break;
		case L1_MODE_TRANS:
		case L1_MODE_HDLC:
			break;
		case L1_MODE_V32:
		case L1_MODE_FAX:
			cmsb |= IOM_CTRL_ALAW | IOM_CTRL_RCV;
			break;
	}
	sendmsg(cs, dps | ISAR_HIS_IOM2CFG, cmsb, 5, msg);
	udelay(1000);
	sendmsg(cs, dps | ISAR_HIS_IOM2REQ, 0, 0, NULL);
	udelay(1000);
}

int
modeisar(struct BCState *bcs, int mode, int bc)
{
	struct IsdnCardState *cs = bcs->cs;

	/* Here we are selecting the best datapath for requested mode */
	if(bcs->mode == L1_MODE_NULL) { /* New Setup */
		bcs->channel = bc;
		switch (mode) {
			case L1_MODE_NULL: /* init */
				if (!bcs->hw.isar.dpath)
					/* no init for dpath 0 */
					return(0);
				break;
			case L1_MODE_TRANS:
			case L1_MODE_HDLC:
				/* best is datapath 2 */
				if (!test_and_set_bit(ISAR_DP2_USE, 
					&bcs->hw.isar.reg->Flags))
					bcs->hw.isar.dpath = 2;
				else if (!test_and_set_bit(ISAR_DP1_USE,
					&bcs->hw.isar.reg->Flags))
					bcs->hw.isar.dpath = 1;
				else {
					printk(KERN_WARNING"isar modeisar both pathes in use\n");
					return(1);
				}
				break;
			case L1_MODE_V32:
			case L1_MODE_FAX:
				/* only datapath 1 */
				if (!test_and_set_bit(ISAR_DP1_USE, 
					&bcs->hw.isar.reg->Flags))
					bcs->hw.isar.dpath = 1;
				else {
					printk(KERN_WARNING"isar modeisar analog funktions only with DP1\n");
					debugl1(cs, "isar modeisar analog funktions only with DP1");
					return(1);
				}
				break;
		}
	}
	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "isar dp%d mode %d->%d ichan %d",
			bcs->hw.isar.dpath, bcs->mode, mode, bc);
	bcs->mode = mode;
	setup_pump(bcs);
	setup_iom2(bcs);
	setup_sart(bcs);
	if (bcs->mode == L1_MODE_NULL) {
		/* Clear resources */
		if (bcs->hw.isar.dpath == 1)
			test_and_clear_bit(ISAR_DP1_USE, &bcs->hw.isar.reg->Flags);
		else if (bcs->hw.isar.dpath == 2)
			test_and_clear_bit(ISAR_DP2_USE, &bcs->hw.isar.reg->Flags);
		bcs->hw.isar.dpath = 0;
	}
	return(0);
}

static void
isar_pump_cmd(struct BCState *bcs, u_char cmd, u_char para) 
{
	struct IsdnCardState *cs = bcs->cs;
	u_char dps = SET_DPS(bcs->hw.isar.dpath);
	u_char ctrl = 0, nom = 0, p1 = 0;

	switch(cmd) {
		case ISDN_FAX_CLASS1_FTM:
			if (bcs->hw.isar.state == STFAX_READY) {
				p1 = para;
				ctrl = PCTRL_CMD_FTM;
				nom = 1;
				bcs->hw.isar.state = STFAX_LINE;
				bcs->hw.isar.cmd = ctrl;
				bcs->hw.isar.mod = para;
				bcs->hw.isar.newmod = 0;
				bcs->hw.isar.newcmd = 0;
				bcs->hw.isar.try_mod = 3; 
			} else if ((bcs->hw.isar.state == STFAX_ACTIV) &&
				(bcs->hw.isar.cmd == PCTRL_CMD_FTM) &&
				(bcs->hw.isar.mod == para)) {
				ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_CONNECT);
			} else {
				bcs->hw.isar.newmod = para;
				bcs->hw.isar.newcmd = PCTRL_CMD_FTM;
				nom = 0;
				ctrl = PCTRL_CMD_ESC;
				bcs->hw.isar.state = STFAX_ESCAPE; 
			}
			break;
		case ISDN_FAX_CLASS1_FTH:
			if (bcs->hw.isar.state == STFAX_READY) {
				p1 = para;
				ctrl = PCTRL_CMD_FTH;
				nom = 1;
				bcs->hw.isar.state = STFAX_LINE;
				bcs->hw.isar.cmd = ctrl;
				bcs->hw.isar.mod = para;
				bcs->hw.isar.newmod = 0;
				bcs->hw.isar.newcmd = 0;
				bcs->hw.isar.try_mod = 3; 
			} else if ((bcs->hw.isar.state == STFAX_ACTIV) &&
				(bcs->hw.isar.cmd == PCTRL_CMD_FTH) &&
				(bcs->hw.isar.mod == para)) {
				ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_CONNECT);
			} else {
				bcs->hw.isar.newmod = para;
				bcs->hw.isar.newcmd = PCTRL_CMD_FTH;
				nom = 0;
				ctrl = PCTRL_CMD_ESC;
				bcs->hw.isar.state = STFAX_ESCAPE; 
			}
			break;
		case ISDN_FAX_CLASS1_FRM:
			if (bcs->hw.isar.state == STFAX_READY) {
				p1 = para;
				ctrl = PCTRL_CMD_FRM;
				nom = 1;
				bcs->hw.isar.state = STFAX_LINE;
				bcs->hw.isar.cmd = ctrl;
				bcs->hw.isar.mod = para;
				bcs->hw.isar.newmod = 0;
				bcs->hw.isar.newcmd = 0;
				bcs->hw.isar.try_mod = 3; 
			} else if ((bcs->hw.isar.state == STFAX_ACTIV) &&
				(bcs->hw.isar.cmd == PCTRL_CMD_FRM) &&
				(bcs->hw.isar.mod == para)) {
				ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_CONNECT);
			} else {
				bcs->hw.isar.newmod = para;
				bcs->hw.isar.newcmd = PCTRL_CMD_FRM;
				nom = 0;
				ctrl = PCTRL_CMD_ESC;
				bcs->hw.isar.state = STFAX_ESCAPE; 
			}
			break;
		case ISDN_FAX_CLASS1_FRH:
			if (bcs->hw.isar.state == STFAX_READY) {
				p1 = para;
				ctrl = PCTRL_CMD_FRH;
				nom = 1;
				bcs->hw.isar.state = STFAX_LINE;
				bcs->hw.isar.cmd = ctrl;
				bcs->hw.isar.mod = para;
				bcs->hw.isar.newmod = 0;
				bcs->hw.isar.newcmd = 0;
				bcs->hw.isar.try_mod = 3; 
			} else if ((bcs->hw.isar.state == STFAX_ACTIV) &&
				(bcs->hw.isar.cmd == PCTRL_CMD_FRH) &&
				(bcs->hw.isar.mod == para)) {
				ll_deliver_faxstat(bcs, ISDN_FAX_CLASS1_CONNECT);
			} else {
				bcs->hw.isar.newmod = para;
				bcs->hw.isar.newcmd = PCTRL_CMD_FRH;
				nom = 0;
				ctrl = PCTRL_CMD_ESC;
				bcs->hw.isar.state = STFAX_ESCAPE; 
			}
			break;
	}
	if (ctrl)
		sendmsg(cs, dps | ISAR_HIS_PUMPCTRL, ctrl, nom, &p1);
}

void
isar_setup(struct IsdnCardState *cs)
{
	u_char msg;
	int i;
	
	/* Dpath 1, 2 */
	msg = 61;
	for (i=0; i<2; i++) {
		/* Buffer Config */
		sendmsg(cs, (i ? ISAR_HIS_DPS2 : ISAR_HIS_DPS1) |
			ISAR_HIS_P12CFG, 4, 1, &msg);
		cs->bcs[i].hw.isar.mml = msg;
		cs->bcs[i].mode = 0;
		cs->bcs[i].hw.isar.dpath = i + 1;
		modeisar(&cs->bcs[i], 0, 0);
		cs->bcs[i].tqueue.routine = (void *) (void *) isar_bh;
	}
}

void
isar_l2l1(struct PStack *st, int pr, void *arg)
{
	struct sk_buff *skb = arg;
	long flags;

	switch (pr) {
		case (PH_DATA | REQUEST):
			save_flags(flags);
			cli();
			if (st->l1.bcs->tx_skb) {
				skb_queue_tail(&st->l1.bcs->squeue, skb);
				restore_flags(flags);
			} else {
				st->l1.bcs->tx_skb = skb;
				test_and_set_bit(BC_FLG_BUSY, &st->l1.bcs->Flag);
				if (st->l1.bcs->cs->debug & L1_DEB_HSCX)
					debugl1(st->l1.bcs->cs, "DRQ set BC_FLG_BUSY");
				st->l1.bcs->hw.isar.txcnt = 0;
				restore_flags(flags);
				st->l1.bcs->cs->BC_Send_Data(st->l1.bcs);
			}
			break;
		case (PH_PULL | INDICATION):
			if (st->l1.bcs->tx_skb) {
				printk(KERN_WARNING "isar_l2l1: this shouldn't happen\n");
				break;
			}
			test_and_set_bit(BC_FLG_BUSY, &st->l1.bcs->Flag);
			if (st->l1.bcs->cs->debug & L1_DEB_HSCX)
				debugl1(st->l1.bcs->cs, "PUI set BC_FLG_BUSY");
			st->l1.bcs->tx_skb = skb;
			st->l1.bcs->hw.isar.txcnt = 0;
			st->l1.bcs->cs->BC_Send_Data(st->l1.bcs);
			break;
		case (PH_PULL | REQUEST):
			if (!st->l1.bcs->tx_skb) {
				test_and_clear_bit(FLG_L1_PULL_REQ, &st->l1.Flags);
				st->l1.l1l2(st, PH_PULL | CONFIRM, NULL);
			} else
				test_and_set_bit(FLG_L1_PULL_REQ, &st->l1.Flags);
			break;
		case (PH_ACTIVATE | REQUEST):
			test_and_set_bit(BC_FLG_ACTIV, &st->l1.bcs->Flag);
			st->l1.bcs->hw.isar.conmsg[0] = 0;
			if (test_bit(FLG_ORIG, &st->l2.flag))
				test_and_set_bit(BC_FLG_ORIG, &st->l1.bcs->Flag);
			else
				test_and_clear_bit(BC_FLG_ORIG, &st->l1.bcs->Flag);
			switch(st->l1.mode) {
				case L1_MODE_TRANS:
				case L1_MODE_HDLC:
					if (modeisar(st->l1.bcs, st->l1.mode, st->l1.bc))
						l1_msg_b(st, PH_DEACTIVATE | REQUEST, arg);
					else
						l1_msg_b(st, PH_ACTIVATE | REQUEST, arg);
					break;
				case L1_MODE_V32:
				case L1_MODE_FAX:
					if (modeisar(st->l1.bcs, st->l1.mode, st->l1.bc))
						l1_msg_b(st, PH_DEACTIVATE | REQUEST, arg);
					break;
			}
			break;
		case (PH_DEACTIVATE | REQUEST):
			l1_msg_b(st, pr, arg);
			break;
		case (PH_DEACTIVATE | CONFIRM):
			test_and_clear_bit(BC_FLG_ACTIV, &st->l1.bcs->Flag);
			test_and_clear_bit(BC_FLG_BUSY, &st->l1.bcs->Flag);
			if (st->l1.bcs->cs->debug & L1_DEB_HSCX)
				debugl1(st->l1.bcs->cs, "PDAC clear BC_FLG_BUSY");
			modeisar(st->l1.bcs, 0, st->l1.bc);
			st->l1.l1l2(st, PH_DEACTIVATE | CONFIRM, NULL);
			break;
	}
}

void
close_isarstate(struct BCState *bcs)
{
	modeisar(bcs, 0, bcs->channel);
	if (test_and_clear_bit(BC_FLG_INIT, &bcs->Flag)) {
		if (bcs->hw.isar.rcvbuf) {
			kfree(bcs->hw.isar.rcvbuf);
			bcs->hw.isar.rcvbuf = NULL;
		}
		discard_queue(&bcs->rqueue);
		discard_queue(&bcs->squeue);
		if (bcs->tx_skb) {
			dev_kfree_skb_any(bcs->tx_skb);
			bcs->tx_skb = NULL;
			test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
			if (bcs->cs->debug & L1_DEB_HSCX)
				debugl1(bcs->cs, "closeisar clear BC_FLG_BUSY");
		}
	}
	del_timer(&bcs->hw.isar.ftimer);
}

int
open_isarstate(struct IsdnCardState *cs, struct BCState *bcs)
{
	if (!test_and_set_bit(BC_FLG_INIT, &bcs->Flag)) {
		if (!(bcs->hw.isar.rcvbuf = kmalloc(HSCX_BUFMAX, GFP_ATOMIC))) {
			printk(KERN_WARNING
			       "HiSax: No memory for isar.rcvbuf\n");
			return (1);
		}
		skb_queue_head_init(&bcs->rqueue);
		skb_queue_head_init(&bcs->squeue);
	}
	bcs->tx_skb = NULL;
	test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "openisar clear BC_FLG_BUSY");
	bcs->event = 0;
	bcs->hw.isar.rcvidx = 0;
	bcs->tx_cnt = 0;
	bcs->hw.isar.ftimer.function = (void *) ftimer_handler;
	bcs->hw.isar.ftimer.data = (long) bcs;
	init_timer(&bcs->hw.isar.ftimer);
	return (0);
}

int
setstack_isar(struct PStack *st, struct BCState *bcs)
{
	bcs->channel = st->l1.bc;
	if (open_isarstate(st->l1.hardware, bcs))
		return (-1);
	st->l1.bcs = bcs;
	st->l2.l2l1 = isar_l2l1;
	setstack_manager(st);
	bcs->st = st;
	setstack_l1_B(st);
	return (0);
}

int
isar_auxcmd(struct IsdnCardState *cs, isdn_ctrl *ic) {
	u_long adr;
	int features, i;
	struct BCState *bcs;

	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "isar_auxcmd cmd/ch %x/%d", ic->command, ic->arg);
	switch (ic->command) {
		case (ISDN_CMD_FAXCMD):
			bcs = cs->channel[ic->arg].bcs;
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "isar_auxcmd cmd/subcmd %d/%d",
					ic->parm.aux.cmd, ic->parm.aux.subcmd);
			switch(ic->parm.aux.cmd) {
				case ISDN_FAX_CLASS1_CTRL:
					if (ic->parm.aux.subcmd == ETX)
						test_and_set_bit(BC_FLG_DLEETX,
							&bcs->Flag);
					break;
				case ISDN_FAX_CLASS1_FRM:
				case ISDN_FAX_CLASS1_FRH:
				case ISDN_FAX_CLASS1_FTM:
				case ISDN_FAX_CLASS1_FTH:
					if (ic->parm.aux.subcmd == AT_QUERY) {
						sprintf(ic->parm.aux.para,
							"%d", bcs->hw.isar.mod);
						ic->command = ISDN_STAT_FAXIND;
						ic->parm.aux.cmd = ISDN_FAX_CLASS1_QUERY;
						cs->iif.statcallb(ic);
						return(0);
					} else if (ic->parm.aux.subcmd == AT_EQ_QUERY) {
						strcpy(ic->parm.aux.para, faxmodulation_s);
						ic->command = ISDN_STAT_FAXIND;
						ic->parm.aux.cmd = ISDN_FAX_CLASS1_QUERY;
						cs->iif.statcallb(ic);
						return(0);
					} else if (ic->parm.aux.subcmd == AT_EQ_VALUE) {
						for(i=0;i<FAXMODCNT;i++)
							if (faxmodulation[i]==ic->parm.aux.para[0])
								break;
						if ((FAXMODCNT > i) && 
							test_bit(BC_FLG_INIT, &bcs->Flag)) {
							isar_pump_cmd(bcs,
								ic->parm.aux.cmd,
								ic->parm.aux.para[0]);
							return(0);
						}
					}
					/* wrong modulation or not activ */
					/* fall through */
				default:
					ic->command = ISDN_STAT_FAXIND;
					ic->parm.aux.cmd = ISDN_FAX_CLASS1_ERROR;
					cs->iif.statcallb(ic);
			}
			break;
		case (ISDN_CMD_IOCTL):
			switch (ic->arg) {
				case (9): /* load firmware */
					features = ISDN_FEATURE_L2_MODEM |
						ISDN_FEATURE_L2_FAX |
						ISDN_FEATURE_L3_FCLASS1;
					memcpy(&adr, ic->parm.num, sizeof(ulong));
					if (isar_load_firmware(cs, (u_char *)adr))
						return(1);
					else 
						ll_run(cs, features);
					break;
				default:
					printk(KERN_DEBUG "HiSax: invalid ioctl %d\n",
					       (int) ic->arg);
					return(-EINVAL);
			}
			break;
		default:
			return(-EINVAL);
	}
	return(0);
}

void __devinit
initisar(struct IsdnCardState *cs)
{
	cs->bcs[0].BC_SetStack = setstack_isar;
	cs->bcs[1].BC_SetStack = setstack_isar;
	cs->bcs[0].BC_Close = close_isarstate;
	cs->bcs[1].BC_Close = close_isarstate;
}
