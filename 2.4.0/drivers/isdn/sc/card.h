/*
 *  $Id: card.h,v 1.1 1996/11/07 13:07:40 fritz Exp $
 *  Copyright (C) 1996  SpellCaster Telecommunications Inc.
 *
 *  card.h - Driver parameters for SpellCaster ISA ISDN adapters
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  For more information, please contact gpl-info@spellcast.com or write:
 *
 *     SpellCaster Telecommunications Inc.
 *     5621 Finch Avenue East, Unit #3
 *     Scarborough, Ontario  Canada
 *     M1B 2T9
 *     +1 (416) 297-8565
 *     +1 (416) 297-6433 Facsimile
 */

#ifndef CARD_H
#define CARD_H

/*
 * We need these if they're not already included
 */
#include <linux/timer.h>
#include <linux/isdnif.h>
#include "message.h"

/*
 * Amount of time to wait for a reset to complete
 */
#define CHECKRESET_TIME		milliseconds(4000)

/*
 * Amount of time between line status checks
 */
#define CHECKSTAT_TIME		milliseconds(8000)

/*
 * The maximum amount of time to wait for a message response
 * to arrive. Use exclusively by send_and_receive
 */
#define SAR_TIMEOUT		milliseconds(10000)

/*
 * Macro to determine is a card id is valid
 */
#define IS_VALID_CARD(x)	((x >= 0) && (x <= cinst))

/*
 * Per channel status and configuration
 */
typedef struct {
	int l2_proto;
	int l3_proto;
	char dn[50];
	unsigned long first_sendbuf;	/* Offset of first send buffer */
	unsigned int num_sendbufs;	/* Number of send buffers */
	unsigned int free_sendbufs;	/* Number of free sendbufs */
	unsigned int next_sendbuf;	/* Next sequential buffer */
	char eazlist[50];		/* Set with SETEAZ */
	char sillist[50];		/* Set with SETSIL */
	int eazclear;			/* Don't accept calls if TRUE */
} bchan;

/*
 * Everything you want to know about the adapter ...
 */
typedef struct {
	int model;
	int driverId;			/* LL Id */
	char devicename[20];		/* The device name */
	isdn_if *card;			/* ISDN4Linux structure */
	bchan *channel;			/* status of the B channels */
	char nChannels;			/* Number of channels */
	unsigned int interrupt;		/* Interrupt number */
	int iobase;			/* I/O Base address */
	int ioport[MAX_IO_REGS];	/* Index to I/O ports */
	int shmem_pgport;		/* port for the exp mem page reg. */
	int shmem_magic;		/* adapter magic number */
	unsigned int rambase;		/* Shared RAM base address */
	unsigned int ramsize;		/* Size of shared memory */
	RspMessage async_msg;		/* Async response message */
	int want_async_messages;	/* Snoop the Q ? */
	unsigned char seq_no;		/* Next send seq. number */
	struct timer_list reset_timer;	/* Check reset timer */
	struct timer_list stat_timer;	/* Check startproc timer */
	unsigned char nphystat;		/* Latest PhyStat info */
	unsigned char phystat;		/* Last PhyStat info */
	HWConfig_pl hwconfig;		/* Hardware config info */
	char load_ver[11];		/* CommManage Version string */
	char proc_ver[11];		/* CommEngine Version */
	int StartOnReset;		/* Indicates startproc after reset */
	int EngineUp;			/* Indicates CommEngine Up */
	int trace_mode;			/* Indicate if tracing is on */
} board;

#endif /* CARD_H */
