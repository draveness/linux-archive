/* $Id: hfcscard.c,v 1.8 2000/11/24 17:05:37 kai Exp $
 *
 * hfcscard.c     low level stuff for hfcs based cards (Teles3c, ACER P10)
 *
 * Author     Karsten Keil (keil@isdn4linux.de)
 *
 * This file is (c) under GNU PUBLIC LICENSE
 *
 */

#define __NO_VERSION__
#include <linux/init.h>
#include "hisax.h"
#include "hfc_2bds0.h"
#include "isdnl1.h"

extern const char *CardType[];

static const char *hfcs_revision = "$Revision: 1.8 $";

static void
hfcs_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u_char val, stat;

	if (!cs) {
		printk(KERN_WARNING "HFCS: Spurious interrupt!\n");
		return;
	}
	if ((HFCD_ANYINT | HFCD_BUSY_NBUSY) & 
		(stat = cs->BC_Read_Reg(cs, HFCD_DATA, HFCD_STAT))) {
		val = cs->BC_Read_Reg(cs, HFCD_DATA, HFCD_INT_S1);
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "HFCS: stat(%02x) s1(%02x)", stat, val);
		hfc2bds0_interrupt(cs, val);
	} else {
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "HFCS: irq_no_irq stat(%02x)", stat);
	}
}

static void
hfcs_Timer(struct IsdnCardState *cs)
{
	cs->hw.hfcD.timer.expires = jiffies + 75;
	/* WD RESET */
/*	WriteReg(cs, HFCD_DATA, HFCD_CTMT, cs->hw.hfcD.ctmt | 0x80);
	add_timer(&cs->hw.hfcD.timer);
*/
}

void
release_io_hfcs(struct IsdnCardState *cs)
{
	release2bds0(cs);
	del_timer(&cs->hw.hfcD.timer);
	if (cs->hw.hfcD.addr)
		release_region(cs->hw.hfcD.addr, 2);
}

static void
reset_hfcs(struct IsdnCardState *cs)
{
	long flags;

	printk(KERN_INFO "HFCS: resetting card\n");
	cs->hw.hfcD.cirm = HFCD_RESET;
	if (cs->typ == ISDN_CTYPE_TELES3C)
		cs->hw.hfcD.cirm |= HFCD_MEM8K;
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_CIRM, cs->hw.hfcD.cirm);	/* Reset On */
	save_flags(flags);
	sti();
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((30*HZ)/1000);
	cs->hw.hfcD.cirm = 0;
	if (cs->typ == ISDN_CTYPE_TELES3C)
		cs->hw.hfcD.cirm |= HFCD_MEM8K;
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_CIRM, cs->hw.hfcD.cirm);	/* Reset Off */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000);
	if (cs->typ == ISDN_CTYPE_TELES3C)
		cs->hw.hfcD.cirm |= HFCD_INTB;
	else if (cs->typ == ISDN_CTYPE_ACERP10)
		cs->hw.hfcD.cirm |= HFCD_INTA;
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_CIRM, cs->hw.hfcD.cirm);
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_CLKDEL, 0x0e);
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_TEST, HFCD_AUTO_AWAKE); /* S/T Auto awake */
	cs->hw.hfcD.ctmt = HFCD_TIM25 | HFCD_AUTO_TIMER;
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_CTMT, cs->hw.hfcD.ctmt);
	cs->hw.hfcD.int_m2 = HFCD_IRQ_ENABLE;
	cs->hw.hfcD.int_m1 = HFCD_INTS_B1TRANS | HFCD_INTS_B2TRANS |
		HFCD_INTS_DTRANS | HFCD_INTS_B1REC | HFCD_INTS_B2REC |
		HFCD_INTS_DREC | HFCD_INTS_L1STATE;
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_INT_M1, cs->hw.hfcD.int_m1);
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_INT_M2, cs->hw.hfcD.int_m2);
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_STATES, HFCD_LOAD_STATE | 2); /* HFC ST 2 */
	udelay(10);
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_STATES, 2); /* HFC ST 2 */
	cs->hw.hfcD.mst_m = HFCD_MASTER;
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_MST_MODE, cs->hw.hfcD.mst_m); /* HFC Master */
	cs->hw.hfcD.sctrl = 0;
	cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_SCTRL, cs->hw.hfcD.sctrl);
	restore_flags(flags);
}

static int
hfcs_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	long flags;

	if (cs->debug & L1_DEB_ISAC)
		debugl1(cs, "HFCS: card_msg %x", mt);
	switch (mt) {
		case CARD_RESET:
			reset_hfcs(cs);
			return(0);
		case CARD_RELEASE:
			release_io_hfcs(cs);
			return(0);
		case CARD_INIT:
			cs->hw.hfcD.timer.expires = jiffies + 75;
			add_timer(&cs->hw.hfcD.timer);
			init2bds0(cs);
			save_flags(flags);
			sti();
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout((80*HZ)/1000);
			cs->hw.hfcD.ctmt |= HFCD_TIM800;
			cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_CTMT, cs->hw.hfcD.ctmt); 
			cs->BC_Write_Reg(cs, HFCD_DATA, HFCD_MST_MODE, cs->hw.hfcD.mst_m);
			restore_flags(flags);
			return(0);
		case CARD_TEST:
			return(0);
	}
	return(0);
}

int __init
setup_hfcs(struct IsdnCard *card)
{
	struct IsdnCardState *cs = card->cs;
	char tmp[64];

	strcpy(tmp, hfcs_revision);
	printk(KERN_INFO "HiSax: HFC-S driver Rev. %s\n", HiSax_getrev(tmp));
	cs->hw.hfcD.addr = card->para[1] & 0xfffe;
	cs->irq = card->para[0];
	cs->hw.hfcD.cip = 0;
	cs->hw.hfcD.int_s1 = 0;
	cs->hw.hfcD.send = NULL;
	cs->bcs[0].hw.hfc.send = NULL;
	cs->bcs[1].hw.hfc.send = NULL;
	cs->hw.hfcD.dfifosize = 512;
	cs->dc.hfcd.ph_state = 0;
	cs->hw.hfcD.fifo = 255;
	if (cs->typ == ISDN_CTYPE_TELES3C) {
		cs->hw.hfcD.bfifosize = 1024 + 512;
	} else if (cs->typ == ISDN_CTYPE_ACERP10) {
		cs->hw.hfcD.bfifosize = 7*1024 + 512;
	} else
		return (0);
	if (check_region((cs->hw.hfcD.addr), 2)) {
		printk(KERN_WARNING
		       "HiSax: %s config port %x-%x already in use\n",
		       CardType[card->typ],
		       cs->hw.hfcD.addr,
		       cs->hw.hfcD.addr + 2);
		return (0);
	} else {
		request_region(cs->hw.hfcD.addr, 2, "HFCS isdn");
	}
	printk(KERN_INFO
	       "HFCS: defined at 0x%x IRQ %d HZ %d\n",
	       cs->hw.hfcD.addr,
	       cs->irq, HZ);
	if (cs->typ == ISDN_CTYPE_TELES3C) {
		/* Teles 16.3c IO ADR is 0x200 | YY0U (YY Bit 15/14 address) */
		outb(0x00, cs->hw.hfcD.addr);
		outb(0x56, cs->hw.hfcD.addr | 1);
	} else if (cs->typ == ISDN_CTYPE_ACERP10) {
		/* Acer P10 IO ADR is 0x300 */
		outb(0x00, cs->hw.hfcD.addr);
		outb(0x57, cs->hw.hfcD.addr | 1);
	}
	set_cs_func(cs);
	cs->hw.hfcD.timer.function = (void *) hfcs_Timer;
	cs->hw.hfcD.timer.data = (long) cs;
	init_timer(&cs->hw.hfcD.timer);
	reset_hfcs(cs);
	cs->cardmsg = &hfcs_card_msg;
	cs->irq_func = &hfcs_interrupt;
	return (1);
}
