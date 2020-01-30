/*
 *  $Id: packet.c,v 1.5 1999/08/31 11:20:41 paul Exp $
 *  Copyright (C) 1996  SpellCaster Telecommunications Inc.
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

#define __NO_VERSION__
#include "includes.h"
#include "hardware.h"
#include "message.h"
#include "card.h"

extern board *adapter[];
extern unsigned int cinst;

extern int get_card_from_id(int);
extern int indicate_status(int, int,ulong, char*);
extern void *memcpy_toshmem(int, void *, const void *, size_t);
extern void *memcpy_fromshmem(int, void *, const void *, size_t);
extern int sendmessage(int, unsigned int, unsigned int, unsigned int,
                unsigned int, unsigned int, unsigned int, unsigned int *);

int sndpkt(int devId, int channel, struct sk_buff *data)
{
	LLData	ReqLnkWrite;
	int status;
	int card;
	unsigned long len;

	card = get_card_from_id(devId);

	if(!IS_VALID_CARD(card)) {
		pr_debug("invalid param: %d is not a valid card id\n", card);
		return -ENODEV;
	}

	pr_debug("%s: sndpkt: frst = 0x%x nxt = %d  f = %d n = %d\n",
		adapter[card]->devicename,
		adapter[card]->channel[channel].first_sendbuf,
		adapter[card]->channel[channel].next_sendbuf,
		adapter[card]->channel[channel].free_sendbufs,
		adapter[card]->channel[channel].num_sendbufs);

	if(!adapter[card]->channel[channel].free_sendbufs) {
		pr_debug("%s: out of TX buffers\n", adapter[card]->devicename);
		return -EINVAL;
	}

	if(data->len > BUFFER_SIZE) {
		pr_debug("%s: data overflows buffer size (data > buffer)\n", adapter[card]->devicename);
		return -EINVAL;
	}

	ReqLnkWrite.buff_offset = adapter[card]->channel[channel].next_sendbuf *
		BUFFER_SIZE + adapter[card]->channel[channel].first_sendbuf;
	ReqLnkWrite.msg_len = data->len; /* sk_buff size */
	pr_debug("%s: writing %d bytes to buffer offset 0x%x\n", adapter[card]->devicename,
			ReqLnkWrite.msg_len, ReqLnkWrite.buff_offset);
	memcpy_toshmem(card, (char *)ReqLnkWrite.buff_offset, data->data, ReqLnkWrite.msg_len);

	/*
	 * sendmessage
	 */
	pr_debug("%s: sndpkt size=%d, buf_offset=0x%x buf_indx=%d\n",
		adapter[card]->devicename,
		ReqLnkWrite.msg_len, ReqLnkWrite.buff_offset,
		adapter[card]->channel[channel].next_sendbuf);

	status = sendmessage(card, CEPID, ceReqTypeLnk, ceReqClass1, ceReqLnkWrite,
				channel+1, sizeof(LLData), (unsigned int*)&ReqLnkWrite);
	len = data->len;
	if(status) {
		pr_debug("%s: failed to send packet, status = %d\n", adapter[card]->devicename, status);
		return -1;
	}
	else {
		adapter[card]->channel[channel].free_sendbufs--;
		adapter[card]->channel[channel].next_sendbuf =
			++adapter[card]->channel[channel].next_sendbuf ==
			adapter[card]->channel[channel].num_sendbufs ? 0 :
			adapter[card]->channel[channel].next_sendbuf;
			pr_debug("%s: packet sent successfully\n", adapter[card]->devicename);
		dev_kfree_skb(data);
		indicate_status(card,ISDN_STAT_BSENT,channel, (char *)&len);
	}
	return len;
}

void rcvpkt(int card, RspMessage *rcvmsg)
{
	LLData newll;
	struct sk_buff *skb;

	if(!IS_VALID_CARD(card)) {
		pr_debug("invalid param: %d is not a valid card id\n", card);
		return;
	}

	switch(rcvmsg->rsp_status){
	case 0x01:
	case 0x02:
	case 0x70:
		pr_debug("%s: error status code: 0x%x\n", adapter[card]->devicename, rcvmsg->rsp_status);
		return;
	case 0x00: 
	    if (!(skb = dev_alloc_skb(rcvmsg->msg_data.response.msg_len))) {
			printk(KERN_WARNING "%s: rcvpkt out of memory, dropping packet\n",
				adapter[card]->devicename);
			return;
		}
		skb_put(skb, rcvmsg->msg_data.response.msg_len);
		pr_debug("%s: getting data from offset: 0x%x\n",
			adapter[card]->devicename,rcvmsg->msg_data.response.buff_offset);
		memcpy_fromshmem(card,
			skb_put(skb, rcvmsg->msg_data.response.msg_len),
		 	(char *)rcvmsg->msg_data.response.buff_offset,
			rcvmsg->msg_data.response.msg_len);
		adapter[card]->card->rcvcallb_skb(adapter[card]->driverId,
			rcvmsg->phy_link_no-1, skb);

	case 0x03:
		/*
	 	 * Recycle the buffer
	 	 */
		pr_debug("%s: buffer size : %d\n", adapter[card]->devicename, BUFFER_SIZE);
/*		memset_shmem(card, rcvmsg->msg_data.response.buff_offset, 0, BUFFER_SIZE); */
		newll.buff_offset = rcvmsg->msg_data.response.buff_offset;
		newll.msg_len = BUFFER_SIZE;
		pr_debug("%s: recycled buffer at offset 0x%x size %d\n", adapter[card]->devicename,
			newll.buff_offset, newll.msg_len);
		sendmessage(card, CEPID, ceReqTypeLnk, ceReqClass1, ceReqLnkRead,
			rcvmsg->phy_link_no, sizeof(LLData), (unsigned int *)&newll);
	}

}

int setup_buffers(int card, int c)
{
	unsigned int nBuffers, i, cBase;
	unsigned int buffer_size;
	LLData	RcvBuffOffset;

	if(!IS_VALID_CARD(card)) {
		pr_debug("invalid param: %d is not a valid card id\n", card);
		return -ENODEV;
	}

	/*
	 * Calculate the buffer offsets (send/recv/send/recv)
	 */
	pr_debug("%s: setting up channel buffer space in shared RAM\n", adapter[card]->devicename);
	buffer_size = BUFFER_SIZE;
	nBuffers = ((adapter[card]->ramsize - BUFFER_BASE) / buffer_size) / 2;
	nBuffers = nBuffers > BUFFERS_MAX ? BUFFERS_MAX : nBuffers;
	pr_debug("%s: calculating buffer space: %d buffers, %d big\n", adapter[card]->devicename,
		nBuffers, buffer_size);
	if(nBuffers < 2) {
		pr_debug("%s: not enough buffer space\n", adapter[card]->devicename);
		return -1;
	}
	cBase = (nBuffers * buffer_size) * (c - 1);
	pr_debug("%s: channel buffer offset from shared RAM: 0x%x\n", adapter[card]->devicename, cBase);
	adapter[card]->channel[c-1].first_sendbuf = BUFFER_BASE + cBase;
	adapter[card]->channel[c-1].num_sendbufs = nBuffers / 2;
	adapter[card]->channel[c-1].free_sendbufs = nBuffers / 2;
	adapter[card]->channel[c-1].next_sendbuf = 0;
	pr_debug("%s: send buffer setup complete: first=0x%x n=%d f=%d, nxt=%d\n",
				adapter[card]->devicename,
				adapter[card]->channel[c-1].first_sendbuf,
				adapter[card]->channel[c-1].num_sendbufs,
				adapter[card]->channel[c-1].free_sendbufs,
				adapter[card]->channel[c-1].next_sendbuf);

	/*
	 * Prep the receive buffers
	 */
	pr_debug("%s: adding %d RecvBuffers:\n", adapter[card]->devicename, nBuffers /2);
	for (i = 0 ; i < nBuffers / 2; i++) {
		RcvBuffOffset.buff_offset = 
			((adapter[card]->channel[c-1].first_sendbuf +
			(nBuffers / 2) * buffer_size) + (buffer_size * i));
		RcvBuffOffset.msg_len = buffer_size;
		pr_debug("%s: adding RcvBuffer #%d offset=0x%x sz=%d bufsz:%d\n",
				adapter[card]->devicename,
				i + 1, RcvBuffOffset.buff_offset, 
				RcvBuffOffset.msg_len,buffer_size);
		sendmessage(card, CEPID, ceReqTypeLnk, ceReqClass1, ceReqLnkRead,
				c, sizeof(LLData), (unsigned int *)&RcvBuffOffset);
	} 
	return 0;
}

int print_skb(int card,char *skb_p, int len){
	int i,data;
	pr_debug("%s: data at 0x%x len: 0x%x\n",adapter[card]->devicename,
			skb_p,len);
	for(i=1;i<=len;i++,skb_p++){
		data = (int) (0xff & (*skb_p));
		pr_debug("%s: data =  0x%x",adapter[card]->devicename,data);
		if(!(i%4))
			pr_debug(" ");
		if(!(i%32))
			pr_debug("\n");
	}
	pr_debug("\n");
	return 0;
}		

