/* $Id: s0box.c,v 2.4 2000/11/24 17:05:38 kai Exp $
 *
 * s0box.c      low level stuff for Creatix S0BOX
 *
 * Author       S0BOX specific stuff: Enrik Berkhan (enrik@starfleet.inka.de)
 *
 * This file is (c) under GNU PUBLIC LICENSE
 *
 */
#define __NO_VERSION__
#include <linux/init.h>
#include "hisax.h"
#include "isac.h"
#include "hscx.h"
#include "isdnl1.h"

extern const char *CardType[];
const char *s0box_revision = "$Revision: 2.4 $";

static inline void
writereg(unsigned int padr, signed int addr, u_char off, u_char val) {
	unsigned long flags;

	save_flags(flags);
	cli();
	outb_p(0x1c,padr+2);
	outb_p(0x14,padr+2);
	outb_p((addr+off)&0x7f,padr);
	outb_p(0x16,padr+2);
	outb_p(val,padr);
	outb_p(0x17,padr+2);
	outb_p(0x14,padr+2);
	outb_p(0x1c,padr+2);
	restore_flags(flags);
}

static u_char nibtab[] = { 1, 9, 5, 0xd, 3, 0xb, 7, 0xf,
			 0, 0, 0, 0, 0, 0, 0, 0,
			 0, 8, 4, 0xc, 2, 0xa, 6, 0xe } ;

static inline u_char
readreg(unsigned int padr, signed int addr, u_char off) {
	register u_char n1, n2;
	unsigned long flags;

	save_flags(flags);
	cli();
	outb_p(0x1c,padr+2);
	outb_p(0x14,padr+2);
	outb_p((addr+off)|0x80,padr);
	outb_p(0x16,padr+2);
	outb_p(0x17,padr+2);
	n1 = (inb_p(padr+1) >> 3) & 0x17;
	outb_p(0x16,padr+2);
	n2 = (inb_p(padr+1) >> 3) & 0x17;
	outb_p(0x14,padr+2);
	outb_p(0x1c,padr+2);
	restore_flags(flags);
	return nibtab[n1] | (nibtab[n2] << 4);
}

static inline void
read_fifo(unsigned int padr, signed int adr, u_char * data, int size)
{
	int i;
	register u_char n1, n2;
	
	outb_p(0x1c, padr+2);
	outb_p(0x14, padr+2);
	outb_p(adr|0x80, padr);
	outb_p(0x16, padr+2);
	for (i=0; i<size; i++) {
		outb_p(0x17, padr+2);
		n1 = (inb_p(padr+1) >> 3) & 0x17;
		outb_p(0x16,padr+2);
		n2 = (inb_p(padr+1) >> 3) & 0x17;
		*(data++)=nibtab[n1] | (nibtab[n2] << 4);
	}
	outb_p(0x14,padr+2);
	outb_p(0x1c,padr+2);
	return;
}

static inline void
write_fifo(unsigned int padr, signed int adr, u_char * data, int size)
{
	int i;
	outb_p(0x1c, padr+2);
	outb_p(0x14, padr+2);
	outb_p(adr&0x7f, padr);
	for (i=0; i<size; i++) {
		outb_p(0x16, padr+2);
		outb_p(*(data++), padr);
		outb_p(0x17, padr+2);
	}
	outb_p(0x14,padr+2);
	outb_p(0x1c,padr+2);
	return;
}

/* Interface functions */

static u_char
ReadISAC(struct IsdnCardState *cs, u_char offset)
{
	return (readreg(cs->hw.teles3.cfg_reg, cs->hw.teles3.isac, offset));
}

static void
WriteISAC(struct IsdnCardState *cs, u_char offset, u_char value)
{
	writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.isac, offset, value);
}

static void
ReadISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	read_fifo(cs->hw.teles3.cfg_reg, cs->hw.teles3.isacfifo, data, size);
}

static void
WriteISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	write_fifo(cs->hw.teles3.cfg_reg, cs->hw.teles3.isacfifo, data, size);
}

static u_char
ReadHSCX(struct IsdnCardState *cs, int hscx, u_char offset)
{
	return (readreg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[hscx], offset));
}

static void
WriteHSCX(struct IsdnCardState *cs, int hscx, u_char offset, u_char value)
{
	writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[hscx], offset, value);
}

/*
 * fast interrupt HSCX stuff goes here
 */

#define READHSCX(cs, nr, reg) readreg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[nr], reg)
#define WRITEHSCX(cs, nr, reg, data) writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[nr], reg, data)
#define READHSCXFIFO(cs, nr, ptr, cnt) read_fifo(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscxfifo[nr], ptr, cnt)
#define WRITEHSCXFIFO(cs, nr, ptr, cnt) write_fifo(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscxfifo[nr], ptr, cnt)

#include "hscx_irq.c"

static void
s0box_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
#define MAXCOUNT 5
	struct IsdnCardState *cs = dev_id;
	u_char val;
	int count = 0;

	if (!cs) {
		printk(KERN_WARNING "Teles: Spurious interrupt!\n");
		return;
	}
	val = readreg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[1], HSCX_ISTA);
      Start_HSCX:
	if (val)
		hscx_int_main(cs, val);
	val = readreg(cs->hw.teles3.cfg_reg, cs->hw.teles3.isac, ISAC_ISTA);
      Start_ISAC:
	if (val)
		isac_interrupt(cs, val);
	count++;
	val = readreg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[1], HSCX_ISTA);
	if (val && count < MAXCOUNT) {
		if (cs->debug & L1_DEB_HSCX)
			debugl1(cs, "HSCX IntStat after IntRoutine");
		goto Start_HSCX;
	}
	val = readreg(cs->hw.teles3.cfg_reg, cs->hw.teles3.isac, ISAC_ISTA);
	if (val && count < MAXCOUNT) {
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "ISAC IntStat after IntRoutine");
		goto Start_ISAC;
	}
	if (count >= MAXCOUNT)
		printk(KERN_WARNING "S0Box: more than %d loops in s0box_interrupt\n", count);
	writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[0], HSCX_MASK, 0xFF);
	writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[1], HSCX_MASK, 0xFF);
	writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.isac, ISAC_MASK, 0xFF);
	writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.isac, ISAC_MASK, 0x0);
	writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[0], HSCX_MASK, 0x0);
	writereg(cs->hw.teles3.cfg_reg, cs->hw.teles3.hscx[1], HSCX_MASK, 0x0);
}

void
release_io_s0box(struct IsdnCardState *cs)
{
	release_region(cs->hw.teles3.cfg_reg, 8);
}

static int
S0Box_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			break;
		case CARD_RELEASE:
			release_io_s0box(cs);
			break;
		case CARD_INIT:
			inithscxisac(cs, 3);
			break;
		case CARD_TEST:
			break;
	}
	return(0);
}

int __init
setup_s0box(struct IsdnCard *card)
{
	struct IsdnCardState *cs = card->cs;
	char tmp[64];

	strcpy(tmp, s0box_revision);
	printk(KERN_INFO "HiSax: S0Box IO driver Rev. %s\n", HiSax_getrev(tmp));
	if (cs->typ != ISDN_CTYPE_S0BOX)
		return (0);

	cs->hw.teles3.cfg_reg = card->para[1];
	cs->hw.teles3.hscx[0] = -0x20;
	cs->hw.teles3.hscx[1] = 0x0;
	cs->hw.teles3.isac = 0x20;
	cs->hw.teles3.isacfifo = cs->hw.teles3.isac + 0x3e;
	cs->hw.teles3.hscxfifo[0] = cs->hw.teles3.hscx[0] + 0x3e;
	cs->hw.teles3.hscxfifo[1] = cs->hw.teles3.hscx[1] + 0x3e;
	cs->irq = card->para[0];
	if (check_region(cs->hw.teles3.cfg_reg,8)) {
		printk(KERN_WARNING
		       "HiSax: %s ports %x-%x already in use\n",
		       CardType[cs->typ],
                       cs->hw.teles3.cfg_reg,
                       cs->hw.teles3.cfg_reg + 7);
		return 0;
	} else
		request_region(cs->hw.teles3.cfg_reg, 8, "S0Box parallel I/O");
	printk(KERN_INFO
	       "HiSax: %s config irq:%d isac:0x%x  cfg:0x%x\n",
	       CardType[cs->typ], cs->irq,
	       cs->hw.teles3.isac, cs->hw.teles3.cfg_reg);
	printk(KERN_INFO
	       "HiSax: hscx A:0x%x  hscx B:0x%x\n",
	       cs->hw.teles3.hscx[0], cs->hw.teles3.hscx[1]);
	cs->readisac = &ReadISAC;
	cs->writeisac = &WriteISAC;
	cs->readisacfifo = &ReadISACfifo;
	cs->writeisacfifo = &WriteISACfifo;
	cs->BC_Read_Reg = &ReadHSCX;
	cs->BC_Write_Reg = &WriteHSCX;
	cs->BC_Send_Data = &hscx_fill_fifo;
	cs->cardmsg = &S0Box_card_msg;
	cs->irq_func = &s0box_interrupt;
	ISACVersion(cs, "S0Box:");
	if (HscxVersion(cs, "S0Box:")) {
		printk(KERN_WARNING
		       "S0Box: wrong HSCX versions check IO address\n");
		release_io_s0box(cs);
		return (0);
	}
	return (1);
}
