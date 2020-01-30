
/*
 *
 * Copyright (C) Eicon Technology Corporation, 2000.
 *
 * This source file is supplied for the exclusive use with Eicon
 * Technology Corporation's range of DIVA Server Adapters.
 *
 * Eicon File Revision :    1.2  
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY OF ANY KIND WHATSOEVER INCLUDING ANY 
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


/*
 * Unix Eicon active card driver
 * XLOG related functions
 */

#include "sys.h"
#include "idi.h"
#include "pc.h"
#include "pc_maint.h"
#include "divalog.h"

#include "adapter.h"
#include "uxio.h"

/*
 * convert/copy XLOG info into a KLOG entry
 */

static
void	xlog_to_klog(byte *b, int size, int card_num)

{
	typedef struct
	{
		word	code;
		word	time_hi;
		word	time_lo;
		word	xcode;
		byte	data[2];
	} card_xlog_t;

	card_xlog_t	*x;

	klog_t		klog;

	x = (card_xlog_t *) b;

	bzero(&klog, sizeof(klog));

	klog.time_stamp = (dword) x->time_hi;
	klog.time_stamp = (klog.time_stamp << 16) | (dword) x->time_lo;

	klog.length = size > sizeof(klog.buffer) ? sizeof(klog.buffer) : size;

	klog.card = card_num;
	if (x->code == 1)
	{
		klog.type = KLOG_XTXT_MSG;
		klog.code = 0;
		bcopy(&x->xcode, klog.buffer, klog.length);
	}
	else if (x->code == 2)
	{
		klog.type = KLOG_XLOG_MSG;
		klog.code = x->xcode;
		bcopy(&x->data, klog.buffer, klog.length);
	}
	else
	{
		char	*c; int i;
		klog.type = KLOG_TEXT_MSG;
		klog.code = 0;
		c = "divas: invalid xlog message code from card";
		i = 0;
		while (*c)
		{
			klog.buffer[i] = *c;
			c++;
			i++;
		}
		klog.buffer[i] = *c;
	}

    /* send to the log driver and return */

    DivasLogAdd(&klog, sizeof(klog));

	return;
}

/*
 * send an XLOG request down to specified card
 * if response available from previous request then read it
 * if not then just send down new request, ready for next time
 */

void	DivasXlogReq(int card_num)

{
	card_t				*card;
	ADAPTER 			*a;

	if ((card_num < 0) || (card_num > DivasCardNext))
	{
		DPRINTF(("xlog: invalid card number"));
		return;
	}

	card = &DivasCards[card_num];

	if (DivasXlogRetrieve(card))
	{
		return;
	}

	/* send down request for next time */

	a = &card->a;

	a->ram_out(a, (word *) (card->xlog_offset + 1), 0);
	a->ram_out(a, (word *) (dword) (card->xlog_offset), DO_LOG);

	return;
}

/*
 * retrieve XLOG request from specified card
 * returns non-zero if new request sent to card
 */

int		DivasXlogRetrieve(card_t *card)

{
	ADAPTER 			*a;
	struct mi_pc_maint	pcm;

	a = &card->a;

	/* get status of last request */

	pcm.rc = a->ram_in(a, (word *)(card->xlog_offset + 1));

	/* if nothing there from previous request, send down a new one */

	if (pcm.rc == OK)
	{
		/* read in response */

		a->ram_in_buffer(a, (word *) (dword) card->xlog_offset, &pcm, sizeof(pcm)); 

		xlog_to_klog((byte *) &pcm.data, sizeof(pcm.data), 
						(int) (card - DivasCards));
	}

	/* if any response received from card, re-send request */

	if (pcm.rc)
	{
		a->ram_out(a, (word *) (card->xlog_offset + 1), 0);
		a->ram_out(a, (word *) (dword) (card->xlog_offset), DO_LOG);

		return 1;
	} 

	return 0;
}
