/* $Id: boardergo.h,v 1.2 2000/11/13 22:51:47 kai Exp $

 * Linux driver for HYSDN cards, definitions for ergo type boards (buffers..).
 * written by Werner Cornelius (werner@titro.de) for Hypercope GmbH
 *
 * Copyright 1999  by Werner Cornelius (werner@titro.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


/************************************************/
/* defines for the dual port memory of the card */
/************************************************/
#define ERG_DPRAM_PAGE_SIZE 0x2000	/* DPRAM occupies a 8K page */
#define BOOT_IMG_SIZE 4096
#define ERG_DPRAM_FILL_SIZE (ERG_DPRAM_PAGE_SIZE - BOOT_IMG_SIZE)

#define ERG_TO_HY_BUF_SIZE  0x0E00	/* 3072 bytes buffer size to card */
#define ERG_TO_PC_BUF_SIZE  0x0E00	/* 3072 bytes to PC, too */

/* following DPRAM layout copied from OS2-driver boarderg.h */
typedef struct ErgDpram_tag {
/*0000 */ uchar ToHyBuf[ERG_TO_HY_BUF_SIZE];
/*0E00 */ uchar ToPcBuf[ERG_TO_PC_BUF_SIZE];

	/*1C00 */ uchar bSoftUart[SIZE_RSV_SOFT_UART];
	/* size 0x1B0 */

	/*1DB0 *//* tErrLogEntry */ uchar volatile ErrLogMsg[64];
	/* size 64 bytes */
	/*1DB0  ulong ulErrType;               */
	/*1DB4  ulong ulErrSubtype;            */
	/*1DB8  ulong ucTextSize;              */
	/*1DB9  ulong ucText[ERRLOG_TEXT_SIZE]; *//* ASCIIZ of len ucTextSize-1 */
	/*1DF0 */

/*1DF0 */ word volatile ToHyChannel;
/*1DF2 */ word volatile ToHySize;
	/*1DF4 */ uchar volatile ToHyFlag;
	/* !=0: msg for Hy waiting */
	/*1DF5 */ uchar volatile ToPcFlag;
	/* !=0: msg for PC waiting */
/*1DF6 */ word volatile ToPcChannel;
/*1DF8 */ word volatile ToPcSize;
	/*1DFA */ uchar bRes1DBA[0x1E00 - 0x1DFA];
	/* 6 bytes */

/*1E00 */ uchar bRestOfEntryTbl[0x1F00 - 0x1E00];
/*1F00 */ ulong TrapTable[62];
	/*1FF8 */ uchar bRes1FF8[0x1FFB - 0x1FF8];
	/* low part of reset vetor */
/*1FFB */ uchar ToPcIntMetro;
	/* notes:
	 * - metro has 32-bit boot ram - accessing
	 *   ToPcInt and ToHyInt would be the same;
	 *   so we moved ToPcInt to 1FFB.
	 *   Because on the PC side both vars are
	 *   readonly (reseting on int from E1 to PC),
	 *   we can read both vars on both cards
	 *   without destroying anything.
	 * - 1FFB is the high byte of the reset vector,
	 *   so E1 side should NOT change this byte
	 *   when writing!
	 */
/*1FFC */ uchar volatile ToHyNoDpramErrLog;
	/* note: ToHyNoDpramErrLog is used to inform
	 *       boot loader, not to use DPRAM based
	 *       ErrLog; when DOS driver is rewritten
	 *       this becomes obsolete
	 */
/*1FFD */ uchar bRes1FFD;
	/*1FFE */ uchar ToPcInt;
	/* E1_intclear; on CHAMP2: E1_intset   */
	/*1FFF */ uchar ToHyInt;
	/* E1_intset;   on CHAMP2: E1_intclear */
} tErgDpram;

/**********************************************/
/* PCI9050 controller local register offsets: */
/* copied from boarderg.c                     */
/**********************************************/
#define PCI9050_INTR_REG    0x4C	/* Interrupt register */
#define PCI9050_USER_IO     0x51	/* User I/O  register */

				    /* bitmask for PCI9050_INTR_REG: */
#define PCI9050_INTR_REG_EN1    0x01	/* 1= enable (def.), 0= disable */
#define PCI9050_INTR_REG_POL1   0x02	/* 1= active high (def.), 0= active low */
#define PCI9050_INTR_REG_STAT1  0x04	/* 1= intr. active, 0= intr. not active (def.) */
#define PCI9050_INTR_REG_ENPCI  0x40	/* 1= PCI interrupts enable (def.) */

				    /* bitmask for PCI9050_USER_IO: */
#define PCI9050_USER_IO_EN3     0x02	/* 1= disable      , 0= enable (def.) */
#define PCI9050_USER_IO_DIR3    0x04	/* 1= output (def.), 0= input         */
#define PCI9050_USER_IO_DAT3    0x08	/* 1= high (def.)  , 0= low           */

#define PCI9050_E1_RESET    (                     PCI9050_USER_IO_DIR3)		/* 0x04 */
#define PCI9050_E1_RUN      (PCI9050_USER_IO_DAT3|PCI9050_USER_IO_DIR3)		/* 0x0C */
