/* $Id: teles0.c,v 2.13 2000/11/24 17:05:38 kai Exp $
 *
 * teles0.c     low level stuff for Teles Memory IO isdn cards
 *              based on the teles driver from Jan den Ouden
 *
 * Author       Karsten Keil (keil@isdn4linux.de)
 *
 * Thanks to    Jan den Ouden
 *              Fritz Elfert
 *              Beat Doebeli
 *
 * This file is (c) under GNU PUBLIC LICENSE
 *
 */
#define __NO_VERSION__
#include <linux/init.h>
#include "hisax.h"
#include "isdnl1.h"
#include "isac.h"
#include "hscx.h"

extern const char *CardType[];

const char *teles0_revision = "$Revision: 2.13 $";

#define TELES_IOMEM_SIZE	0x400
#define byteout(addr,val) outb(val,addr)
#define bytein(addr) inb(addr)

static inline u_char
readisac(unsigned long adr, u_char off)
{
	return readb(adr + ((off & 1) ? 0x2ff : 0x100) + off);
}

static inline void
writeisac(unsigned long adr, u_char off, u_char data)
{
	writeb(data, adr + ((off & 1) ? 0x2ff : 0x100) + off); mb();
}


static inline u_char
readhscx(unsigned long adr, int hscx, u_char off)
{
	return readb(adr + (hscx ? 0x1c0 : 0x180) +
		     ((off & 1) ? 0x1ff : 0) + off);
}

static inline void
writehscx(unsigned long adr, int hscx, u_char off, u_char data)
{
	writeb(data, adr + (hscx ? 0x1c0 : 0x180) +
	       ((off & 1) ? 0x1ff : 0) + off); mb();
}

static inline void
read_fifo_isac(unsigned long adr, u_char * data, int size)
{
	register int i;
	register u_char *ad = (u_char *)adr + 0x100;
	for (i = 0; i < size; i++)
		data[i] = readb(ad);
}

static inline void
write_fifo_isac(unsigned long adr, u_char * data, int size)
{
	register int i;
	register u_char *ad = (u_char *)adr + 0x100;
	for (i = 0; i < size; i++) {
		writeb(data[i], ad); mb();
	}
}

static inline void
read_fifo_hscx(unsigned long adr, int hscx, u_char * data, int size)
{
	register int i;
	register u_char *ad = (u_char *) (adr + (hscx ? 0x1c0 : 0x180));
	for (i = 0; i < size; i++)
		data[i] = readb(ad);
}

static inline void
write_fifo_hscx(unsigned long adr, int hscx, u_char * data, int size)
{
	int i;
	register u_char *ad = (u_char *) (adr + (hscx ? 0x1c0 : 0x180));
	for (i = 0; i < size; i++) {
		writeb(data[i], ad); mb();
	}
}

/* Interface functions */

static u_char
ReadISAC(struct IsdnCardState *cs, u_char offset)
{
	return (readisac(cs->hw.teles0.membase, offset));
}

static void
WriteISAC(struct IsdnCardState *cs, u_char offset, u_char value)
{
	writeisac(cs->hw.teles0.membase, offset, value);
}

static void
ReadISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	read_fifo_isac(cs->hw.teles0.membase, data, size);
}

static void
WriteISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	write_fifo_isac(cs->hw.teles0.membase, data, size);
}

static u_char
ReadHSCX(struct IsdnCardState *cs, int hscx, u_char offset)
{
	return (readhscx(cs->hw.teles0.membase, hscx, offset));
}

static void
WriteHSCX(struct IsdnCardState *cs, int hscx, u_char offset, u_char value)
{
	writehscx(cs->hw.teles0.membase, hscx, offset, value);
}

/*
 * fast interrupt HSCX stuff goes here
 */

#define READHSCX(cs, nr, reg) readhscx(cs->hw.teles0.membase, nr, reg)
#define WRITEHSCX(cs, nr, reg, data) writehscx(cs->hw.teles0.membase, nr, reg, data)
#define READHSCXFIFO(cs, nr, ptr, cnt) read_fifo_hscx(cs->hw.teles0.membase, nr, ptr, cnt)
#define WRITEHSCXFIFO(cs, nr, ptr, cnt) write_fifo_hscx(cs->hw.teles0.membase, nr, ptr, cnt)

#include "hscx_irq.c"

static void
teles0_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u_char val;
	int count = 0;

	if (!cs) {
		printk(KERN_WARNING "Teles0: Spurious interrupt!\n");
		return;
	}
	val = readhscx(cs->hw.teles0.membase, 1, HSCX_ISTA);
      Start_HSCX:
	if (val)
		hscx_int_main(cs, val);
	val = readisac(cs->hw.teles0.membase, ISAC_ISTA);
      Start_ISAC:
	if (val)
		isac_interrupt(cs, val);
	count++;
	val = readhscx(cs->hw.teles0.membase, 1, HSCX_ISTA);
	if (val && count < 5) {
		if (cs->debug & L1_DEB_HSCX)
			debugl1(cs, "HSCX IntStat after IntRoutine");
		goto Start_HSCX;
	}
	val = readisac(cs->hw.teles0.membase, ISAC_ISTA);
	if (val && count < 5) {
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "ISAC IntStat after IntRoutine");
		goto Start_ISAC;
	}
	writehscx(cs->hw.teles0.membase, 0, HSCX_MASK, 0xFF);
	writehscx(cs->hw.teles0.membase, 1, HSCX_MASK, 0xFF);
	writeisac(cs->hw.teles0.membase, ISAC_MASK, 0xFF);
	writeisac(cs->hw.teles0.membase, ISAC_MASK, 0x0);
	writehscx(cs->hw.teles0.membase, 0, HSCX_MASK, 0x0);
	writehscx(cs->hw.teles0.membase, 1, HSCX_MASK, 0x0);
}

void
release_io_teles0(struct IsdnCardState *cs)
{
	if (cs->hw.teles0.cfg_reg)
		release_region(cs->hw.teles0.cfg_reg, 8);
	iounmap((unsigned char *)cs->hw.teles0.membase);
	release_mem_region(cs->hw.teles0.phymem, TELES_IOMEM_SIZE);
}

static int
reset_teles0(struct IsdnCardState *cs)
{
	u_char cfval;
	long flags;

	save_flags(flags);
	sti();
	if (cs->hw.teles0.cfg_reg) {
		switch (cs->irq) {
			case 2:
			case 9:
				cfval = 0x00;
				break;
			case 3:
				cfval = 0x02;
				break;
			case 4:
				cfval = 0x04;
				break;
			case 5:
				cfval = 0x06;
				break;
			case 10:
				cfval = 0x08;
				break;
			case 11:
				cfval = 0x0A;
				break;
			case 12:
				cfval = 0x0C;
				break;
			case 15:
				cfval = 0x0E;
				break;
			default:
				return(1);
		}
		cfval |= ((cs->hw.teles0.phymem >> 9) & 0xF0);
		byteout(cs->hw.teles0.cfg_reg + 4, cfval);
		HZDELAY(HZ / 10 + 1);
		byteout(cs->hw.teles0.cfg_reg + 4, cfval | 1);
		HZDELAY(HZ / 10 + 1);
	}
	writeb(0, cs->hw.teles0.membase + 0x80); mb();
	HZDELAY(HZ / 5 + 1);
	writeb(1, cs->hw.teles0.membase + 0x80); mb();
	HZDELAY(HZ / 5 + 1);
	restore_flags(flags);
	return(0);
}

static int
Teles_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			reset_teles0(cs);
			return(0);
		case CARD_RELEASE:
			release_io_teles0(cs);
			return(0);
		case CARD_INIT:
			inithscxisac(cs, 3);
			return(0);
		case CARD_TEST:
			return(0);
	}
	return(0);
}

int __init
setup_teles0(struct IsdnCard *card)
{
	u_char val;
	struct IsdnCardState *cs = card->cs;
	char tmp[64];

	strcpy(tmp, teles0_revision);
	printk(KERN_INFO "HiSax: Teles 8.0/16.0 driver Rev. %s\n", HiSax_getrev(tmp));
	if ((cs->typ != ISDN_CTYPE_16_0) && (cs->typ != ISDN_CTYPE_8_0))
		return (0);

	if (cs->typ == ISDN_CTYPE_16_0)
		cs->hw.teles0.cfg_reg = card->para[2];
	else			/* 8.0 */
		cs->hw.teles0.cfg_reg = 0;

	if (card->para[1] < 0x10000) {
		card->para[1] <<= 4;
		printk(KERN_INFO
		   "Teles0: membase configured DOSish, assuming 0x%lx\n",
		       (unsigned long) card->para[1]);
	}
	cs->irq = card->para[0];
	if (cs->hw.teles0.cfg_reg) {
		if (check_region(cs->hw.teles0.cfg_reg, 8)) {
			printk(KERN_WARNING
			  "HiSax: %s config port %x-%x already in use\n",
			       CardType[card->typ],
			       cs->hw.teles0.cfg_reg,
			       cs->hw.teles0.cfg_reg + 8);
			return (0);
		} else {
			request_region(cs->hw.teles0.cfg_reg, 8, "teles cfg");
		}
	}
	if (cs->hw.teles0.cfg_reg) {
		if ((val = bytein(cs->hw.teles0.cfg_reg + 0)) != 0x51) {
			printk(KERN_WARNING "Teles0: 16.0 Byte at %x is %x\n",
			       cs->hw.teles0.cfg_reg + 0, val);
			release_region(cs->hw.teles0.cfg_reg, 8);
			return (0);
		}
		if ((val = bytein(cs->hw.teles0.cfg_reg + 1)) != 0x93) {
			printk(KERN_WARNING "Teles0: 16.0 Byte at %x is %x\n",
			       cs->hw.teles0.cfg_reg + 1, val);
			release_region(cs->hw.teles0.cfg_reg, 8);
			return (0);
		}
		val = bytein(cs->hw.teles0.cfg_reg + 2);	/* 0x1e=without AB
								   * 0x1f=with AB
								   * 0x1c 16.3 ???
								 */
		if (val != 0x1e && val != 0x1f) {
			printk(KERN_WARNING "Teles0: 16.0 Byte at %x is %x\n",
			       cs->hw.teles0.cfg_reg + 2, val);
			release_region(cs->hw.teles0.cfg_reg, 8);
			return (0);
		}
	}
	/* 16.0 and 8.0 designed for IOM1 */
	test_and_set_bit(HW_IOM1, &cs->HW_Flags);
	cs->hw.teles0.phymem = card->para[1];
	if (check_mem_region(cs->hw.teles0.phymem, TELES_IOMEM_SIZE)) {
		printk(KERN_WARNING
			"HiSax: %s memory region %lx-%lx already in use\n",
			CardType[card->typ],
			cs->hw.teles0.phymem,
			cs->hw.teles0.phymem + TELES_IOMEM_SIZE);
		if (cs->hw.teles0.cfg_reg)
			release_region(cs->hw.teles0.cfg_reg, 8);
		return (0);
	} else {
		request_mem_region(cs->hw.teles0.phymem, TELES_IOMEM_SIZE,
			"teles iomem");
	}
	cs->hw.teles0.membase =
		(unsigned long) ioremap(cs->hw.teles0.phymem, TELES_IOMEM_SIZE);
	printk(KERN_INFO
	       "HiSax: %s config irq:%d mem:0x%lX cfg:0x%X\n",
	       CardType[cs->typ], cs->irq,
	       cs->hw.teles0.membase, cs->hw.teles0.cfg_reg);
	if (reset_teles0(cs)) {
		printk(KERN_WARNING "Teles0: wrong IRQ\n");
		release_io_teles0(cs);
		return (0);
	}
	cs->readisac = &ReadISAC;
	cs->writeisac = &WriteISAC;
	cs->readisacfifo = &ReadISACfifo;
	cs->writeisacfifo = &WriteISACfifo;
	cs->BC_Read_Reg = &ReadHSCX;
	cs->BC_Write_Reg = &WriteHSCX;
	cs->BC_Send_Data = &hscx_fill_fifo;
	cs->cardmsg = &Teles_card_msg;
	cs->irq_func = &teles0_interrupt;
	ISACVersion(cs, "Teles0:");
	if (HscxVersion(cs, "Teles0:")) {
		printk(KERN_WARNING
		 "Teles0: wrong HSCX versions check IO/MEM addresses\n");
		release_io_teles0(cs);
		return (0);
	}
	return (1);
}
