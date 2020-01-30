/* $Id: jade_irq.c,v 1.5 2000/11/19 17:02:48 kai Exp $
 *
 * jade_irq.c   Low level JADE IRQ stuff (derived from original hscx_irq.c)
 *
 * Author   Roland Klabunde (R.Klabunde@Berkom.de)
 *
 * This file is (c) under GNU PUBLIC LICENSE
 *
 */

static inline void
waitforCEC(struct IsdnCardState *cs, int jade, int reg)
{
  	int to = 50;
  	int mask = (reg == jade_HDLC_XCMD ? jadeSTAR_XCEC : jadeSTAR_RCEC);
  	while ((READJADE(cs, jade, jade_HDLC_STAR) & mask) && to) {
  		udelay(1);
  		to--;
  	}
  	if (!to)
  		printk(KERN_WARNING "HiSax: waitforCEC (jade) timeout\n");
}


static inline void
waitforXFW(struct IsdnCardState *cs, int jade)
{
  	/* Does not work on older jade versions, don't care */
}

static inline void
WriteJADECMDR(struct IsdnCardState *cs, int jade, int reg, u_char data)
{
	long flags;

	save_flags(flags);
	cli();
	waitforCEC(cs, jade, reg);
	WRITEJADE(cs, jade, reg, data);
	restore_flags(flags);
}



static void
jade_empty_fifo(struct BCState *bcs, int count)
{
	u_char *ptr;
	struct IsdnCardState *cs = bcs->cs;
	long flags;

	if ((cs->debug & L1_DEB_HSCX) && !(cs->debug & L1_DEB_HSCX_FIFO))
		debugl1(cs, "jade_empty_fifo");

	if (bcs->hw.hscx.rcvidx + count > HSCX_BUFMAX) {
		if (cs->debug & L1_DEB_WARN)
			debugl1(cs, "jade_empty_fifo: incoming packet too large");
		WriteJADECMDR(cs, bcs->hw.hscx.hscx, jade_HDLC_RCMD, jadeRCMD_RMC);
		bcs->hw.hscx.rcvidx = 0;
		return;
	}
	ptr = bcs->hw.hscx.rcvbuf + bcs->hw.hscx.rcvidx;
	bcs->hw.hscx.rcvidx += count;
	save_flags(flags);
	cli();
	READJADEFIFO(cs, bcs->hw.hscx.hscx, ptr, count);
	WriteJADECMDR(cs, bcs->hw.hscx.hscx, jade_HDLC_RCMD, jadeRCMD_RMC);
	restore_flags(flags);
	if (cs->debug & L1_DEB_HSCX_FIFO) {
		char *t = bcs->blog;

		t += sprintf(t, "jade_empty_fifo %c cnt %d",
			     bcs->hw.hscx.hscx ? 'B' : 'A', count);
		QuickHex(t, ptr, count);
		debugl1(cs, bcs->blog);
	}
}

static void
jade_fill_fifo(struct BCState *bcs)
{
	struct IsdnCardState *cs = bcs->cs;
	int more, count;
	int fifo_size = 32;
	u_char *ptr;
	long flags;

	if ((cs->debug & L1_DEB_HSCX) && !(cs->debug & L1_DEB_HSCX_FIFO))
		debugl1(cs, "jade_fill_fifo");

	if (!bcs->tx_skb)
		return;
	if (bcs->tx_skb->len <= 0)
		return;

	more = (bcs->mode == L1_MODE_TRANS) ? 1 : 0;
	if (bcs->tx_skb->len > fifo_size) {
		more = !0;
		count = fifo_size;
	} else
		count = bcs->tx_skb->len;

	waitforXFW(cs, bcs->hw.hscx.hscx);
	save_flags(flags);
	cli();
	ptr = bcs->tx_skb->data;
	skb_pull(bcs->tx_skb, count);
	bcs->tx_cnt -= count;
	bcs->hw.hscx.count += count;
	WRITEJADEFIFO(cs, bcs->hw.hscx.hscx, ptr, count);
	WriteJADECMDR(cs, bcs->hw.hscx.hscx, jade_HDLC_XCMD, more ? jadeXCMD_XF : (jadeXCMD_XF|jadeXCMD_XME));
	restore_flags(flags);
	if (cs->debug & L1_DEB_HSCX_FIFO) {
		char *t = bcs->blog;

		t += sprintf(t, "jade_fill_fifo %c cnt %d",
			     bcs->hw.hscx.hscx ? 'B' : 'A', count);
		QuickHex(t, ptr, count);
		debugl1(cs, bcs->blog);
	}
}


static inline void
jade_interrupt(struct IsdnCardState *cs, u_char val, u_char jade)
{
	u_char r;
	struct BCState *bcs = cs->bcs + jade;
	struct sk_buff *skb;
	int fifo_size = 32;
	int count;
	int i_jade = (int) jade; /* To satisfy the compiler */
	
	if (!test_bit(BC_FLG_INIT, &bcs->Flag))
		return;

	if (val & 0x80) {	/* RME */
		r = READJADE(cs, i_jade, jade_HDLC_RSTA);
		if ((r & 0xf0) != 0xa0) {
			if (!(r & 0x80))
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "JADE %s invalid frame", (jade ? "B":"A"));
			if ((r & 0x40) && bcs->mode)
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "JADE %c RDO mode=%d", 'A'+jade, bcs->mode);
			if (!(r & 0x20))
				if (cs->debug & L1_DEB_WARN)
					debugl1(cs, "JADE %c CRC error", 'A'+jade);
			WriteJADECMDR(cs, jade, jade_HDLC_RCMD, jadeRCMD_RMC);
		} else {
			count = READJADE(cs, i_jade, jade_HDLC_RBCL) & 0x1F;
			if (count == 0)
				count = fifo_size;
			jade_empty_fifo(bcs, count);
			if ((count = bcs->hw.hscx.rcvidx - 1) > 0) {
				if (cs->debug & L1_DEB_HSCX_FIFO)
					debugl1(cs, "HX Frame %d", count);
				if (!(skb = dev_alloc_skb(count)))
					printk(KERN_WARNING "JADE %s receive out of memory\n", (jade ? "B":"A"));
				else {
					memcpy(skb_put(skb, count), bcs->hw.hscx.rcvbuf, count);
					skb_queue_tail(&bcs->rqueue, skb);
				}
			}
		}
		bcs->hw.hscx.rcvidx = 0;
		jade_sched_event(bcs, B_RCVBUFREADY);
	}
	if (val & 0x40) {	/* RPF */
		jade_empty_fifo(bcs, fifo_size);
		if (bcs->mode == L1_MODE_TRANS) {
			/* receive audio data */
			if (!(skb = dev_alloc_skb(fifo_size)))
				printk(KERN_WARNING "HiSax: receive out of memory\n");
			else {
				memcpy(skb_put(skb, fifo_size), bcs->hw.hscx.rcvbuf, fifo_size);
				skb_queue_tail(&bcs->rqueue, skb);
			}
			bcs->hw.hscx.rcvidx = 0;
			jade_sched_event(bcs, B_RCVBUFREADY);
		}
	}
	if (val & 0x10) {	/* XPR */
		if (bcs->tx_skb) {
			if (bcs->tx_skb->len) {
				jade_fill_fifo(bcs);
				return;
			} else {
				if (bcs->st->lli.l1writewakeup &&
					(PACKET_NOACK != bcs->tx_skb->pkt_type))
					bcs->st->lli.l1writewakeup(bcs->st, bcs->hw.hscx.count);
				dev_kfree_skb_irq(bcs->tx_skb);
				bcs->hw.hscx.count = 0;
				bcs->tx_skb = NULL;
			}
		}
		if ((bcs->tx_skb = skb_dequeue(&bcs->squeue))) {
			bcs->hw.hscx.count = 0;
			test_and_set_bit(BC_FLG_BUSY, &bcs->Flag);
			jade_fill_fifo(bcs);
		} else {
			test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
			jade_sched_event(bcs, B_XMTBUFREADY);
		}
	}
}

static inline void
jade_int_main(struct IsdnCardState *cs, u_char val, int jade)
{
	struct BCState *bcs;
	bcs = cs->bcs + jade;
	
	if (val & jadeISR_RFO) {
		/* handled with RDO */
		val &= ~jadeISR_RFO;
	}
	if (val & jadeISR_XDU) {
		/* relevant in HDLC mode only */
		/* don't reset XPR here */
		if (bcs->mode == 1)
			jade_fill_fifo(bcs);
		else {
			/* Here we lost an TX interrupt, so
			   * restart transmitting the whole frame.
			 */
			if (bcs->tx_skb) {
			   	skb_push(bcs->tx_skb, bcs->hw.hscx.count);
				bcs->tx_cnt += bcs->hw.hscx.count;
				bcs->hw.hscx.count = 0;
			}
			WriteJADECMDR(cs, bcs->hw.hscx.hscx, jade_HDLC_XCMD, jadeXCMD_XRES);
			if (cs->debug & L1_DEB_WARN)
				debugl1(cs, "JADE %c EXIR %x Lost TX", 'A'+jade, val);
		}
	}
	if (val & (jadeISR_RME|jadeISR_RPF|jadeISR_XPR)) {
		if (cs->debug & L1_DEB_HSCX)
			debugl1(cs, "JADE %c interrupt %x", 'A'+jade, val);
		jade_interrupt(cs, val, jade);
	}
}
