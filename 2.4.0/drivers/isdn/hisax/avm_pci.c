/* $Id: avm_pci.c,v 1.22.6.2 2000/11/29 16:00:14 kai Exp $
 *
 * avm_pci.c    low level stuff for AVM Fritz!PCI and ISA PnP isdn cards
 *              Thanks to AVM, Berlin for informations
 *
 * Author       Karsten Keil (keil@isdn4linux.de)
 *
 * This file is (c) under GNU PUBLIC LICENSE
 *
 */
#define __NO_VERSION__
#include <linux/config.h>
#include <linux/init.h>
#include "hisax.h"
#include "isac.h"
#include "isdnl1.h"
#include <linux/pci.h>
#include <linux/interrupt.h>

extern const char *CardType[];
static const char *avm_pci_rev = "$Revision: 1.22.6.2 $";

#define  AVM_FRITZ_PCI		1
#define  AVM_FRITZ_PNP		2

#define  HDLC_FIFO		0x0
#define  HDLC_STATUS		0x4

#define	 AVM_HDLC_1		0x00
#define	 AVM_HDLC_2		0x01
#define	 AVM_ISAC_FIFO		0x02
#define	 AVM_ISAC_REG_LOW	0x04
#define	 AVM_ISAC_REG_HIGH	0x06

#define  AVM_STATUS0_IRQ_ISAC	0x01
#define  AVM_STATUS0_IRQ_HDLC	0x02
#define  AVM_STATUS0_IRQ_TIMER	0x04
#define  AVM_STATUS0_IRQ_MASK	0x07

#define  AVM_STATUS0_RESET	0x01
#define  AVM_STATUS0_DIS_TIMER	0x02
#define  AVM_STATUS0_RES_TIMER	0x04
#define  AVM_STATUS0_ENA_IRQ	0x08
#define  AVM_STATUS0_TESTBIT	0x10

#define  AVM_STATUS1_INT_SEL	0x0f
#define  AVM_STATUS1_ENA_IOM	0x80

#define  HDLC_MODE_ITF_FLG	0x01
#define  HDLC_MODE_TRANS	0x02
#define  HDLC_MODE_CCR_7	0x04
#define  HDLC_MODE_CCR_16	0x08
#define  HDLC_MODE_TESTLOOP	0x80

#define  HDLC_INT_XPR		0x80
#define  HDLC_INT_XDU		0x40
#define  HDLC_INT_RPR		0x20
#define  HDLC_INT_MASK		0xE0

#define  HDLC_STAT_RME		0x01
#define  HDLC_STAT_RDO		0x10
#define  HDLC_STAT_CRCVFRRAB	0x0E
#define  HDLC_STAT_CRCVFR	0x06
#define  HDLC_STAT_RML_MASK	0x3f00

#define  HDLC_CMD_XRS		0x80
#define  HDLC_CMD_XME		0x01
#define  HDLC_CMD_RRS		0x20
#define  HDLC_CMD_XML_MASK	0x3f00


/* Interface functions */

static u_char
ReadISAC(struct IsdnCardState *cs, u_char offset)
{
	register u_char idx = (offset > 0x2f) ? AVM_ISAC_REG_HIGH : AVM_ISAC_REG_LOW;
	register u_char val;
	register long flags;

	save_flags(flags);
	cli();
	outb(idx, cs->hw.avm.cfg_reg + 4);
	val = inb(cs->hw.avm.isac + (offset & 0xf));
	restore_flags(flags);
	return (val);
}

static void
WriteISAC(struct IsdnCardState *cs, u_char offset, u_char value)
{
	register u_char idx = (offset > 0x2f) ? AVM_ISAC_REG_HIGH : AVM_ISAC_REG_LOW;
	register long flags;

	save_flags(flags);
	cli();
	outb(idx, cs->hw.avm.cfg_reg + 4);
	outb(value, cs->hw.avm.isac + (offset & 0xf));
	restore_flags(flags);
}

static void
ReadISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	outb(AVM_ISAC_FIFO, cs->hw.avm.cfg_reg + 4);
	insb(cs->hw.avm.isac, data, size);
}

static void
WriteISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	outb(AVM_ISAC_FIFO, cs->hw.avm.cfg_reg + 4);
	outsb(cs->hw.avm.isac, data, size);
}

static inline u_int
ReadHDLCPCI(struct IsdnCardState *cs, int chan, u_char offset)
{
	register u_int idx = chan ? AVM_HDLC_2 : AVM_HDLC_1;
	register u_int val;
	register long flags;

	save_flags(flags);
	cli();
	outl(idx, cs->hw.avm.cfg_reg + 4);
	val = inl(cs->hw.avm.isac + offset);
	restore_flags(flags);
	return (val);
}

static inline void
WriteHDLCPCI(struct IsdnCardState *cs, int chan, u_char offset, u_int value)
{
	register u_int idx = chan ? AVM_HDLC_2 : AVM_HDLC_1;
	register long flags;

	save_flags(flags);
	cli();
	outl(idx, cs->hw.avm.cfg_reg + 4);
	outl(value, cs->hw.avm.isac + offset);
	restore_flags(flags);
}

static inline u_char
ReadHDLCPnP(struct IsdnCardState *cs, int chan, u_char offset)
{
	register u_char idx = chan ? AVM_HDLC_2 : AVM_HDLC_1;
	register u_char val;
	register long flags;

	save_flags(flags);
	cli();
	outb(idx, cs->hw.avm.cfg_reg + 4);
	val = inb(cs->hw.avm.isac + offset);
	restore_flags(flags);
	return (val);
}

static inline void
WriteHDLCPnP(struct IsdnCardState *cs, int chan, u_char offset, u_char value)
{
	register u_char idx = chan ? AVM_HDLC_2 : AVM_HDLC_1;
	register long flags;

	save_flags(flags);
	cli();
	outb(idx, cs->hw.avm.cfg_reg + 4);
	outb(value, cs->hw.avm.isac + offset);
	restore_flags(flags);
}

static u_char
ReadHDLC_s(struct IsdnCardState *cs, int chan, u_char offset)
{
	return(0xff & ReadHDLCPCI(cs, chan, offset));
}

static void
WriteHDLC_s(struct IsdnCardState *cs, int chan, u_char offset, u_char value)
{
	WriteHDLCPCI(cs, chan, offset, value);
}

static inline
struct BCState *Sel_BCS(struct IsdnCardState *cs, int channel)
{
	if (cs->bcs[0].mode && (cs->bcs[0].channel == channel))
		return(&cs->bcs[0]);
	else if (cs->bcs[1].mode && (cs->bcs[1].channel == channel))
		return(&cs->bcs[1]);
	else
		return(NULL);
}

void inline
hdlc_sched_event(struct BCState *bcs, int event)
{
	bcs->event |= 1 << event;
	queue_task(&bcs->tqueue, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
}

void
write_ctrl(struct BCState *bcs, int which) {

	if (bcs->cs->debug & L1_DEB_HSCX)
		debugl1(bcs->cs, "hdlc %c wr%x ctrl %x",
			'A' + bcs->channel, which, bcs->hw.hdlc.ctrl.ctrl);
	if (bcs->cs->subtyp == AVM_FRITZ_PCI) {
		WriteHDLCPCI(bcs->cs, bcs->channel, HDLC_STATUS, bcs->hw.hdlc.ctrl.ctrl);
	} else {
		if (which & 4)
			WriteHDLCPnP(bcs->cs, bcs->channel, HDLC_STATUS + 2,
				bcs->hw.hdlc.ctrl.sr.mode);
		if (which & 2)
			WriteHDLCPnP(bcs->cs, bcs->channel, HDLC_STATUS + 1,
				bcs->hw.hdlc.ctrl.sr.xml);
		if (which & 1)
			WriteHDLCPnP(bcs->cs, bcs->channel, HDLC_STATUS,
				bcs->hw.hdlc.ctrl.sr.cmd);
	}
}

void
modehdlc(struct BCState *bcs, int mode, int bc)
{
	struct IsdnCardState *cs = bcs->cs;
	int hdlc = bcs->channel;

	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "hdlc %c mode %d --> %d ichan %d --> %d",
			'A' + hdlc, bcs->mode, mode, hdlc, bc);
	bcs->hw.hdlc.ctrl.ctrl = 0;
	switch (mode) {
		case (-1): /* used for init */
			bcs->mode = 1;
			bcs->channel = bc;
			bc = 0;
		case (L1_MODE_NULL):
			if (bcs->mode == L1_MODE_NULL)
				return;
			bcs->hw.hdlc.ctrl.sr.cmd  = HDLC_CMD_XRS | HDLC_CMD_RRS;
			bcs->hw.hdlc.ctrl.sr.mode = HDLC_MODE_TRANS;
			write_ctrl(bcs, 5);
			bcs->mode = L1_MODE_NULL;
			bcs->channel = bc;
			break;
		case (L1_MODE_TRANS):
			bcs->mode = mode;
			bcs->channel = bc;
			bcs->hw.hdlc.ctrl.sr.cmd  = HDLC_CMD_XRS | HDLC_CMD_RRS;
			bcs->hw.hdlc.ctrl.sr.mode = HDLC_MODE_TRANS;
			write_ctrl(bcs, 5);
			bcs->hw.hdlc.ctrl.sr.cmd = HDLC_CMD_XRS;
			write_ctrl(bcs, 1);
			bcs->hw.hdlc.ctrl.sr.cmd = 0;
			hdlc_sched_event(bcs, B_XMTBUFREADY);
			break;
		case (L1_MODE_HDLC):
			bcs->mode = mode;
			bcs->channel = bc;
			bcs->hw.hdlc.ctrl.sr.cmd  = HDLC_CMD_XRS | HDLC_CMD_RRS;
			bcs->hw.hdlc.ctrl.sr.mode = HDLC_MODE_ITF_FLG;
			write_ctrl(bcs, 5);
			bcs->hw.hdlc.ctrl.sr.cmd = HDLC_CMD_XRS;
			write_ctrl(bcs, 1);
			bcs->hw.hdlc.ctrl.sr.cmd = 0;
			hdlc_sched_event(bcs, B_XMTBUFREADY);
			break;
	}
}

static inline void
hdlc_empty_fifo(struct BCState *bcs, int count)
{
	register u_int *ptr;
	u_char *p;
	u_char idx = bcs->channel ? AVM_HDLC_2 : AVM_HDLC_1;
	int cnt=0;
	struct IsdnCardState *cs = bcs->cs;

	if ((cs->debug & L1_DEB_HSCX) && !(cs->debug & L1_DEB_HSCX_FIFO))
		debugl1(cs, "hdlc_empty_fifo %d", count);
	if (bcs->hw.hdlc.rcvidx + count > HSCX_BUFMAX) {
		if (cs->debug & L1_DEB_WARN)
			debugl1(cs, "hdlc_empty_fifo: incoming packet too large");
		return;
	}
	ptr = (u_int *) p = bcs->hw.hdlc.rcvbuf + bcs->hw.hdlc.rcvidx;
	bcs->hw.hdlc.rcvidx += count;
	if (cs->subtyp == AVM_FRITZ_PCI) {
		outl(idx, cs->hw.avm.cfg_reg + 4);
		while (cnt < count) {
#ifdef __powerpc__
#ifdef CONFIG_APUS
			*ptr++ = in_le32((unsigned *)(cs->hw.avm.isac +_IO_BASE));
#else
			*ptr++ = in_be32((unsigned *)(cs->hw.avm.isac +_IO_BASE));
#endif /* CONFIG_APUS */
#else
			*ptr++ = inl(cs->hw.avm.isac);
#endif /* __powerpc__ */
			cnt += 4;
		}
	} else {
		outb(idx, cs->hw.avm.cfg_reg + 4);
		while (cnt < count) {
			*p++ = inb(cs->hw.avm.isac);
			cnt++;
		}
	}
	if (cs->debug & L1_DEB_HSCX_FIFO) {
		char *t = bcs->blog;

		if (cs->subtyp == AVM_FRITZ_PNP)
			p = (u_char *) ptr;
		t += sprintf(t, "hdlc_empty_fifo %c cnt %d",
			     bcs->channel ? 'B' : 'A', count);
		QuickHex(t, p, count);
		debugl1(cs, bcs->blog);
	}
}

static inline void
hdlc_fill_fifo(struct BCState *bcs)
{
	struct IsdnCardState *cs = bcs->cs;
	int count, cnt =0;
	int fifo_size = 32;
	u_char *p;
	u_int *ptr;

	if ((cs->debug & L1_DEB_HSCX) && !(cs->debug & L1_DEB_HSCX_FIFO))
		debugl1(cs, "hdlc_fill_fifo");
	if (!bcs->tx_skb)
		return;
	if (bcs->tx_skb->len <= 0)
		return;

	bcs->hw.hdlc.ctrl.sr.cmd &= ~HDLC_CMD_XME;
	if (bcs->tx_skb->len > fifo_size) {
		count = fifo_size;
	} else {
		count = bcs->tx_skb->len;
		if (bcs->mode != L1_MODE_TRANS)
			bcs->hw.hdlc.ctrl.sr.cmd |= HDLC_CMD_XME;
	}
	if ((cs->debug & L1_DEB_HSCX) && !(cs->debug & L1_DEB_HSCX_FIFO))
		debugl1(cs, "hdlc_fill_fifo %d/%ld", count, bcs->tx_skb->len);
	ptr = (u_int *) p = bcs->tx_skb->data;
	skb_pull(bcs->tx_skb, count);
	bcs->tx_cnt -= count;
	bcs->hw.hdlc.count += count;
	bcs->hw.hdlc.ctrl.sr.xml = ((count == fifo_size) ? 0 : count);
	write_ctrl(bcs, 3);  /* sets the correct index too */
	if (cs->subtyp == AVM_FRITZ_PCI) {
		while (cnt<count) {
#ifdef __powerpc__
#ifdef CONFIG_APUS
			out_le32((unsigned *)(cs->hw.avm.isac +_IO_BASE), *ptr++);
#else
			out_be32((unsigned *)(cs->hw.avm.isac +_IO_BASE), *ptr++);
#endif /* CONFIG_APUS */
#else
			outl(*ptr++, cs->hw.avm.isac);
#endif /* __powerpc__ */
			cnt += 4;
		}
	} else {
		while (cnt<count) {
			outb(*p++, cs->hw.avm.isac);
			cnt++;
		}
	}
	if (cs->debug & L1_DEB_HSCX_FIFO) {
		char *t = bcs->blog;

		if (cs->subtyp == AVM_FRITZ_PNP)
			p = (u_char *) ptr;
		t += sprintf(t, "hdlc_fill_fifo %c cnt %d",
			     bcs->channel ? 'B' : 'A', count);
		QuickHex(t, p, count);
		debugl1(cs, bcs->blog);
	}
}

static void
fill_hdlc(struct BCState *bcs)
{
	long flags;
	save_flags(flags);
	cli();
	hdlc_fill_fifo(bcs);
	restore_flags(flags);
}

static inline void
HDLC_irq(struct BCState *bcs, u_int stat) {
	int len;
	struct sk_buff *skb;

	if (bcs->cs->debug & L1_DEB_HSCX)
		debugl1(bcs->cs, "ch%d stat %#x", bcs->channel, stat);
	if (stat & HDLC_INT_RPR) {
		if (stat & HDLC_STAT_RDO) {
			if (bcs->cs->debug & L1_DEB_HSCX)
				debugl1(bcs->cs, "RDO");
			else
				debugl1(bcs->cs, "ch%d stat %#x", bcs->channel, stat);
			bcs->hw.hdlc.ctrl.sr.xml = 0;
			bcs->hw.hdlc.ctrl.sr.cmd |= HDLC_CMD_RRS;
			write_ctrl(bcs, 1);
			bcs->hw.hdlc.ctrl.sr.cmd &= ~HDLC_CMD_RRS;
			write_ctrl(bcs, 1);
			bcs->hw.hdlc.rcvidx = 0;
		} else {
			if (!(len = (stat & HDLC_STAT_RML_MASK)>>8))
				len = 32;
			hdlc_empty_fifo(bcs, len);
			if ((stat & HDLC_STAT_RME) || (bcs->mode == L1_MODE_TRANS)) {
				if (((stat & HDLC_STAT_CRCVFRRAB)==HDLC_STAT_CRCVFR) ||
					(bcs->mode == L1_MODE_TRANS)) {
					if (!(skb = dev_alloc_skb(bcs->hw.hdlc.rcvidx)))
						printk(KERN_WARNING "HDLC: receive out of memory\n");
					else {
						memcpy(skb_put(skb, bcs->hw.hdlc.rcvidx),
							bcs->hw.hdlc.rcvbuf, bcs->hw.hdlc.rcvidx);
						skb_queue_tail(&bcs->rqueue, skb);
					}
					bcs->hw.hdlc.rcvidx = 0;
					hdlc_sched_event(bcs, B_RCVBUFREADY);
				} else {
					if (bcs->cs->debug & L1_DEB_HSCX)
						debugl1(bcs->cs, "invalid frame");
					else
						debugl1(bcs->cs, "ch%d invalid frame %#x", bcs->channel, stat);
					bcs->hw.hdlc.rcvidx = 0;
				}
			}
		}
	}
	if (stat & HDLC_INT_XDU) {
		/* Here we lost an TX interrupt, so
		 * restart transmitting the whole frame.
		 */
		if (bcs->tx_skb) {
			skb_push(bcs->tx_skb, bcs->hw.hdlc.count);
			bcs->tx_cnt += bcs->hw.hdlc.count;
			bcs->hw.hdlc.count = 0;
//			hdlc_sched_event(bcs, B_XMTBUFREADY);
			if (bcs->cs->debug & L1_DEB_WARN)
				debugl1(bcs->cs, "ch%d XDU", bcs->channel);
		} else if (bcs->cs->debug & L1_DEB_WARN)
			debugl1(bcs->cs, "ch%d XDU without skb", bcs->channel);
		bcs->hw.hdlc.ctrl.sr.xml = 0;
		bcs->hw.hdlc.ctrl.sr.cmd |= HDLC_CMD_XRS;
		write_ctrl(bcs, 1);
		bcs->hw.hdlc.ctrl.sr.cmd &= ~HDLC_CMD_XRS;
		write_ctrl(bcs, 1);
		hdlc_fill_fifo(bcs);
	} else if (stat & HDLC_INT_XPR) {
		if (bcs->tx_skb) {
			if (bcs->tx_skb->len) {
				hdlc_fill_fifo(bcs);
				return;
			} else {
				if (bcs->st->lli.l1writewakeup &&
					(PACKET_NOACK != bcs->tx_skb->pkt_type))
					bcs->st->lli.l1writewakeup(bcs->st, bcs->hw.hdlc.count);
				dev_kfree_skb_irq(bcs->tx_skb);
				bcs->hw.hdlc.count = 0;
				bcs->tx_skb = NULL;
			}
		}
		if ((bcs->tx_skb = skb_dequeue(&bcs->squeue))) {
			bcs->hw.hdlc.count = 0;
			test_and_set_bit(BC_FLG_BUSY, &bcs->Flag);
			hdlc_fill_fifo(bcs);
		} else {
			test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
			hdlc_sched_event(bcs, B_XMTBUFREADY);
		}
	}
}

inline void
HDLC_irq_main(struct IsdnCardState *cs)
{
	u_int stat;
	long  flags;
	struct BCState *bcs;

	save_flags(flags);
	cli();
	if (cs->subtyp == AVM_FRITZ_PCI) {
		stat = ReadHDLCPCI(cs, 0, HDLC_STATUS);
	} else {
		stat = ReadHDLCPnP(cs, 0, HDLC_STATUS);
		if (stat & HDLC_INT_RPR)
			stat |= (ReadHDLCPnP(cs, 0, HDLC_STATUS+1))<<8;
	}
	if (stat & HDLC_INT_MASK) {
		if (!(bcs = Sel_BCS(cs, 0))) {
			if (cs->debug)
				debugl1(cs, "hdlc spurious channel 0 IRQ");
		} else
			HDLC_irq(bcs, stat);
	}
	if (cs->subtyp == AVM_FRITZ_PCI) {
		stat = ReadHDLCPCI(cs, 1, HDLC_STATUS);
	} else {
		stat = ReadHDLCPnP(cs, 1, HDLC_STATUS);
		if (stat & HDLC_INT_RPR)
			stat |= (ReadHDLCPnP(cs, 1, HDLC_STATUS+1))<<8;
	}
	if (stat & HDLC_INT_MASK) {
		if (!(bcs = Sel_BCS(cs, 1))) {
			if (cs->debug)
				debugl1(cs, "hdlc spurious channel 1 IRQ");
		} else
			HDLC_irq(bcs, stat);
	}
	restore_flags(flags);
}

void
hdlc_l2l1(struct PStack *st, int pr, void *arg)
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
				st->l1.bcs->hw.hdlc.count = 0;
				restore_flags(flags);
				st->l1.bcs->cs->BC_Send_Data(st->l1.bcs);
			}
			break;
		case (PH_PULL | INDICATION):
			if (st->l1.bcs->tx_skb) {
				printk(KERN_WARNING "hdlc_l2l1: this shouldn't happen\n");
				break;
			}
			test_and_set_bit(BC_FLG_BUSY, &st->l1.bcs->Flag);
			st->l1.bcs->tx_skb = skb;
			st->l1.bcs->hw.hdlc.count = 0;
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
			modehdlc(st->l1.bcs, st->l1.mode, st->l1.bc);
			l1_msg_b(st, pr, arg);
			break;
		case (PH_DEACTIVATE | REQUEST):
			l1_msg_b(st, pr, arg);
			break;
		case (PH_DEACTIVATE | CONFIRM):
			test_and_clear_bit(BC_FLG_ACTIV, &st->l1.bcs->Flag);
			test_and_clear_bit(BC_FLG_BUSY, &st->l1.bcs->Flag);
			modehdlc(st->l1.bcs, 0, st->l1.bc);
			st->l1.l1l2(st, PH_DEACTIVATE | CONFIRM, NULL);
			break;
	}
}

void
close_hdlcstate(struct BCState *bcs)
{
	modehdlc(bcs, 0, 0);
	if (test_and_clear_bit(BC_FLG_INIT, &bcs->Flag)) {
		if (bcs->hw.hdlc.rcvbuf) {
			kfree(bcs->hw.hdlc.rcvbuf);
			bcs->hw.hdlc.rcvbuf = NULL;
		}
		if (bcs->blog) {
			kfree(bcs->blog);
			bcs->blog = NULL;
		}
		discard_queue(&bcs->rqueue);
		discard_queue(&bcs->squeue);
		if (bcs->tx_skb) {
			dev_kfree_skb_any(bcs->tx_skb);
			bcs->tx_skb = NULL;
			test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
		}
	}
}

int
open_hdlcstate(struct IsdnCardState *cs, struct BCState *bcs)
{
	if (!test_and_set_bit(BC_FLG_INIT, &bcs->Flag)) {
		if (!(bcs->hw.hdlc.rcvbuf = kmalloc(HSCX_BUFMAX, GFP_ATOMIC))) {
			printk(KERN_WARNING
			       "HiSax: No memory for hdlc.rcvbuf\n");
			return (1);
		}
		if (!(bcs->blog = kmalloc(MAX_BLOG_SPACE, GFP_ATOMIC))) {
			printk(KERN_WARNING
				"HiSax: No memory for bcs->blog\n");
			test_and_clear_bit(BC_FLG_INIT, &bcs->Flag);
			kfree(bcs->hw.hdlc.rcvbuf);
			bcs->hw.hdlc.rcvbuf = NULL;
			return (2);
		}
		skb_queue_head_init(&bcs->rqueue);
		skb_queue_head_init(&bcs->squeue);
	}
	bcs->tx_skb = NULL;
	test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
	bcs->event = 0;
	bcs->hw.hdlc.rcvidx = 0;
	bcs->tx_cnt = 0;
	return (0);
}

int
setstack_hdlc(struct PStack *st, struct BCState *bcs)
{
	bcs->channel = st->l1.bc;
	if (open_hdlcstate(st->l1.hardware, bcs))
		return (-1);
	st->l1.bcs = bcs;
	st->l2.l2l1 = hdlc_l2l1;
	setstack_manager(st);
	bcs->st = st;
	setstack_l1_B(st);
	return (0);
}

void __init
clear_pending_hdlc_ints(struct IsdnCardState *cs)
{
	u_int val;

	if (cs->subtyp == AVM_FRITZ_PCI) {
		val = ReadHDLCPCI(cs, 0, HDLC_STATUS);
		debugl1(cs, "HDLC 1 STA %x", val);
		val = ReadHDLCPCI(cs, 1, HDLC_STATUS);
		debugl1(cs, "HDLC 2 STA %x", val);
	} else {
		val = ReadHDLCPnP(cs, 0, HDLC_STATUS);
		debugl1(cs, "HDLC 1 STA %x", val);
		val = ReadHDLCPnP(cs, 0, HDLC_STATUS + 1);
		debugl1(cs, "HDLC 1 RML %x", val);
		val = ReadHDLCPnP(cs, 0, HDLC_STATUS + 2);
		debugl1(cs, "HDLC 1 MODE %x", val);
		val = ReadHDLCPnP(cs, 0, HDLC_STATUS + 3);
		debugl1(cs, "HDLC 1 VIN %x", val);
		val = ReadHDLCPnP(cs, 1, HDLC_STATUS);
		debugl1(cs, "HDLC 2 STA %x", val);
		val = ReadHDLCPnP(cs, 1, HDLC_STATUS + 1);
		debugl1(cs, "HDLC 2 RML %x", val);
		val = ReadHDLCPnP(cs, 1, HDLC_STATUS + 2);
		debugl1(cs, "HDLC 2 MODE %x", val);
		val = ReadHDLCPnP(cs, 1, HDLC_STATUS + 3);
		debugl1(cs, "HDLC 2 VIN %x", val);
	}
}

void __init
inithdlc(struct IsdnCardState *cs)
{
	cs->bcs[0].BC_SetStack = setstack_hdlc;
	cs->bcs[1].BC_SetStack = setstack_hdlc;
	cs->bcs[0].BC_Close = close_hdlcstate;
	cs->bcs[1].BC_Close = close_hdlcstate;
	modehdlc(cs->bcs, -1, 0);
	modehdlc(cs->bcs + 1, -1, 1);
}

static void
avm_pcipnp_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u_char val;
	u_char sval;

	if (!cs) {
		printk(KERN_WARNING "AVM PCI: Spurious interrupt!\n");
		return;
	}
	sval = inb(cs->hw.avm.cfg_reg + 2);
	if ((sval & AVM_STATUS0_IRQ_MASK) == AVM_STATUS0_IRQ_MASK)
		/* possible a shared  IRQ reqest */
		return;
	if (!(sval & AVM_STATUS0_IRQ_ISAC)) {
		val = ReadISAC(cs, ISAC_ISTA);
		isac_interrupt(cs, val);
	}
	if (!(sval & AVM_STATUS0_IRQ_HDLC)) {
		HDLC_irq_main(cs);
	}
	WriteISAC(cs, ISAC_MASK, 0xFF);
	WriteISAC(cs, ISAC_MASK, 0x0);
}

static void
reset_avmpcipnp(struct IsdnCardState *cs)
{
	long flags;

	printk(KERN_INFO "AVM PCI/PnP: reset\n");
	save_flags(flags);
	sti();
	outb(AVM_STATUS0_RESET | AVM_STATUS0_DIS_TIMER, cs->hw.avm.cfg_reg + 2);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000); /* Timeout 10ms */
	outb(AVM_STATUS0_DIS_TIMER | AVM_STATUS0_RES_TIMER | AVM_STATUS0_ENA_IRQ, cs->hw.avm.cfg_reg + 2);
	outb(AVM_STATUS1_ENA_IOM | cs->irq, cs->hw.avm.cfg_reg + 3);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000); /* Timeout 10ms */
	printk(KERN_INFO "AVM PCI/PnP: S1 %x\n", inb(cs->hw.avm.cfg_reg + 3));
}

static int
AVM_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			reset_avmpcipnp(cs);
			return(0);
		case CARD_RELEASE:
			outb(0, cs->hw.avm.cfg_reg + 2);
			release_region(cs->hw.avm.cfg_reg, 32);
			return(0);
		case CARD_INIT:
			clear_pending_isac_ints(cs);
			initisac(cs);
			clear_pending_hdlc_ints(cs);
			inithdlc(cs);
			outb(AVM_STATUS0_DIS_TIMER | AVM_STATUS0_RES_TIMER,
				cs->hw.avm.cfg_reg + 2);
			WriteISAC(cs, ISAC_MASK, 0);
			outb(AVM_STATUS0_DIS_TIMER | AVM_STATUS0_RES_TIMER |
				AVM_STATUS0_ENA_IRQ, cs->hw.avm.cfg_reg + 2);
			/* RESET Receiver and Transmitter */
			WriteISAC(cs, ISAC_CMDR, 0x41);
			return(0);
		case CARD_TEST:
			return(0);
	}
	return(0);
}

static struct pci_dev *dev_avm __initdata = NULL;

int __init
setup_avm_pcipnp(struct IsdnCard *card)
{
	u_int val, ver;
	struct IsdnCardState *cs = card->cs;
	char tmp[64];

	strcpy(tmp, avm_pci_rev);
	printk(KERN_INFO "HiSax: AVM PCI driver Rev. %s\n", HiSax_getrev(tmp));
	if (cs->typ != ISDN_CTYPE_FRITZPCI)
		return (0);
	if (card->para[1]) {
		cs->hw.avm.cfg_reg = card->para[1];
		cs->irq = card->para[0];
		cs->subtyp = AVM_FRITZ_PNP;
	} else {
#if CONFIG_PCI
		if (!pci_present()) {
			printk(KERN_ERR "FritzPCI: no PCI bus present\n");
			return(0);
		}
		if ((dev_avm = pci_find_device(PCI_VENDOR_ID_AVM,
			PCI_DEVICE_ID_AVM_A1,  dev_avm))) {
			cs->irq = dev_avm->irq;
			if (!cs->irq) {
				printk(KERN_ERR "FritzPCI: No IRQ for PCI card found\n");
				return(0);
			}
			if (pci_enable_device(dev_avm))
				return(0);
			cs->hw.avm.cfg_reg = pci_resource_start(dev_avm, 1);
			if (!cs->hw.avm.cfg_reg) {
				printk(KERN_ERR "FritzPCI: No IO-Adr for PCI card found\n");
				return(0);
			}
			cs->subtyp = AVM_FRITZ_PCI;
		} else {
			printk(KERN_WARNING "FritzPCI: No PCI card found\n");
			return(0);
		}
		cs->irq_flags |= SA_SHIRQ;
#else
		printk(KERN_WARNING "FritzPCI: NO_PCI_BIOS\n");
		return (0);
#endif /* CONFIG_PCI */
	}
	cs->hw.avm.isac = cs->hw.avm.cfg_reg + 0x10;
	if (check_region((cs->hw.avm.cfg_reg), 32)) {
		printk(KERN_WARNING
		       "HiSax: %s config port %x-%x already in use\n",
		       CardType[card->typ],
		       cs->hw.avm.cfg_reg,
		       cs->hw.avm.cfg_reg + 31);
		return (0);
	} else {
		request_region(cs->hw.avm.cfg_reg, 32,
			(cs->subtyp == AVM_FRITZ_PCI) ? "avm PCI" : "avm PnP");
	}
	switch (cs->subtyp) {
	  case AVM_FRITZ_PCI:
		val = inl(cs->hw.avm.cfg_reg);
		printk(KERN_INFO "AVM PCI: stat %#x\n", val);
		printk(KERN_INFO "AVM PCI: Class %X Rev %d\n",
			val & 0xff, (val>>8) & 0xff);
		cs->BC_Read_Reg = &ReadHDLC_s;
		cs->BC_Write_Reg = &WriteHDLC_s;
		break;
	  case AVM_FRITZ_PNP:
		val = inb(cs->hw.avm.cfg_reg);
		ver = inb(cs->hw.avm.cfg_reg + 1);
		printk(KERN_INFO "AVM PnP: Class %X Rev %d\n", val, ver);
		reset_avmpcipnp(cs);
		cs->BC_Read_Reg = &ReadHDLCPnP;
		cs->BC_Write_Reg = &WriteHDLCPnP;
		break;
	  default:
	  	printk(KERN_WARNING "AVM unknown subtype %d\n", cs->subtyp);
	  	return(0);
	}
	printk(KERN_INFO "HiSax: %s config irq:%d base:0x%X\n",
		(cs->subtyp == AVM_FRITZ_PCI) ? "AVM Fritz!PCI" : "AVM Fritz!PnP",
		cs->irq, cs->hw.avm.cfg_reg);

	cs->readisac = &ReadISAC;
	cs->writeisac = &WriteISAC;
	cs->readisacfifo = &ReadISACfifo;
	cs->writeisacfifo = &WriteISACfifo;
	cs->BC_Send_Data = &fill_hdlc;
	cs->cardmsg = &AVM_card_msg;
	cs->irq_func = &avm_pcipnp_interrupt;
	ISACVersion(cs, (cs->subtyp == AVM_FRITZ_PCI) ? "AVM PCI:" : "AVM PnP:");
	return (1);
}
