/*********************************************************************
 *                
 * Filename:      irlap_frame.c
 * Version:       1.0
 * Description:   Build and transmit IrLAP frames
 * Status:        Stable
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Tue Aug 19 10:27:26 1997
 * Modified at:   Wed Jan  5 08:59:04 2000
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * 
 *     Copyright (c) 1998-2000 Dag Brattli <dagb@cs.uit.no>, 
 *     All Rights Reserved.
 *     
 *     This program is free software; you can redistribute it and/or 
 *     modify it under the terms of the GNU General Public License as 
 *     published by the Free Software Foundation; either version 2 of 
 *     the License, or (at your option) any later version.
 *
 *     Neither Dag Brattli nor University of Troms� admit liability nor
 *     provide warranty for any of this software. This material is 
 *     provided "AS-IS" and at no charge.
 *
 ********************************************************************/

#include <linux/skbuff.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/irda.h>
 
#include <net/pkt_sched.h>
#include <net/sock.h>
 
#include <asm/byteorder.h>

#include <net/irda/irda.h>
#include <net/irda/irda_device.h>
#include <net/irda/irlap.h>
#include <net/irda/wrapper.h>
#include <net/irda/timer.h>
#include <net/irda/irlap_frame.h>
#include <net/irda/qos.h>

/*
 * Function irlap_insert_info (self, skb)
 *
 *    Insert minimum turnaround time and speed information into the skb. We 
 *    need to do this since it's per packet relevant information. Safe to
 *    have this function inlined since it's only called from one place
 */
static inline void irlap_insert_info(struct irlap_cb *self, 
				     struct sk_buff *skb)
{
	struct irda_skb_cb *cb = (struct irda_skb_cb *) skb->cb;

	/*  
	 * Insert MTT (min. turn time) and speed into skb, so that the
	 * device driver knows which settings to use 
	 */
	cb->magic = LAP_MAGIC;
	cb->mtt = self->mtt_required;
	cb->speed = self->speed;

	/* Reset */
	self->mtt_required = 0;
	
	/* 
	 * Delay equals negotiated BOFs count, plus the number of BOFs to 
	 * force the negotiated minimum turnaround time 
	 */
	cb->xbofs = self->bofs_count;
	cb->xbofs_delay = self->xbofs_delay;
	
	/* Reset XBOF's delay (used only for getting min turn time) */
	self->xbofs_delay = 0;
}

/*
 * Function irlap_queue_xmit (self, skb)
 *
 *    A little wrapper for dev_queue_xmit, so we can insert some common
 *    code into it.
 */
void irlap_queue_xmit(struct irlap_cb *self, struct sk_buff *skb)
{
	/* Some common init stuff */
	skb->dev = self->netdev;
	skb->h.raw = skb->nh.raw = skb->mac.raw = skb->data;
 	skb->protocol = htons(ETH_P_IRDA);
	skb->priority = TC_PRIO_BESTEFFORT;

	irlap_insert_info(self, skb);

	dev_queue_xmit(skb);
}

/*
 * Function irlap_send_snrm_cmd (void)
 *
 *    Transmits a connect SNRM command frame
 */
void irlap_send_snrm_frame(struct irlap_cb *self, struct qos_info *qos) 
{
	struct sk_buff *skb;
	struct snrm_frame *frame;
	int ret;

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);

	/* Allocate frame */
	skb = dev_alloc_skb(64);
	if (!skb)
		return;

	frame = (struct snrm_frame *) skb_put(skb, 2); 

	/* Insert connection address field */
	if (qos)
		frame->caddr = CMD_FRAME | CBROADCAST;
	else
		frame->caddr = CMD_FRAME | self->caddr;

	/* Insert control field */
 	frame->control = SNRM_CMD | PF_BIT;
	
	/*
	 *  If we are establishing a connection then insert QoS paramerters 
	 */
	if (qos) {
		skb_put(skb, 9); /* 21 left */
		frame->saddr = cpu_to_le32(self->saddr);
		frame->daddr = cpu_to_le32(self->daddr);

		frame->ncaddr = self->caddr;
				
		ret = irlap_insert_qos_negotiation_params(self, skb);
		if (ret < 0) {
			dev_kfree_skb(skb);
			return;
		}
	}
	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_recv_snrm_cmd (skb, info)
 *
 *    Received SNRM (Set Normal Response Mode) command frame
 *
 */
static void irlap_recv_snrm_cmd(struct irlap_cb *self, struct sk_buff *skb, 
				struct irlap_info *info) 
{
	struct snrm_frame *frame;

	frame = (struct snrm_frame *) skb->data;
	
	if (skb->len >= sizeof(struct snrm_frame)) {
		/* Copy the new connection address */
		info->caddr = frame->ncaddr;

		/* Check if the new connection address is valid */
		if ((info->caddr == 0x00) || (info->caddr == 0xfe)) {
			IRDA_DEBUG(3, __FUNCTION__ 
			      "(), invalid connection address!\n");
			return;
		}
		
		/* Copy peer device address */
		info->daddr = le32_to_cpu(frame->saddr);
		info->saddr = le32_to_cpu(frame->daddr);
		
		/* Only accept if addressed directly to us */
		if (info->saddr != self->saddr) {
			IRDA_DEBUG(2, __FUNCTION__ "(), not addressed to us!\n");
			return;
		}
		irlap_do_event(self, RECV_SNRM_CMD, skb, info);
	} else {
		/* Signal that this SNRM frame does not contain and I-field */
		irlap_do_event(self, RECV_SNRM_CMD, skb, NULL);
	}
}

/*
 * Function irlap_send_ua_response_frame (qos)
 *
 *    Send UA (Unnumbered Acknowledgement) frame
 *
 */
void irlap_send_ua_response_frame(struct irlap_cb *self, struct qos_info *qos)
{
	struct sk_buff *skb;
	struct ua_frame *frame;
	int ret;
	
	IRDA_DEBUG(2, __FUNCTION__ "() <%ld>\n", jiffies);
	
	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);

	skb = NULL;

	/* Allocate frame */
	skb = dev_alloc_skb(64);
	if (!skb)
		return;

	frame = (struct ua_frame *) skb_put(skb, 10);
	
	/* Build UA response */
	frame->caddr = self->caddr;
 	frame->control = UA_RSP | PF_BIT;

	frame->saddr = cpu_to_le32(self->saddr);
	frame->daddr = cpu_to_le32(self->daddr);

	/* Should we send QoS negotiation parameters? */
	if (qos) {
		ret = irlap_insert_qos_negotiation_params(self, skb);
		if (ret < 0) {
			dev_kfree_skb(skb);
			return;
		}
	}

	irlap_queue_xmit(self, skb);
}


/*
 * Function irlap_send_dm_frame (void)
 *
 *    Send disconnected mode (DM) frame
 *
 */
void irlap_send_dm_frame( struct irlap_cb *self)
{
	struct sk_buff *skb = NULL;
	__u8 *frame;
	
	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);

	skb = dev_alloc_skb(32);
	if (!skb)
		return;

	frame = skb_put( skb, 2);
	
	if (self->state == LAP_NDM)
		frame[0] = CBROADCAST;
	else
		frame[0] = self->caddr;

	frame[1] = DM_RSP | PF_BIT;

	irlap_queue_xmit(self, skb);	
}

/*
 * Function irlap_send_disc_frame (void)
 *
 *    Send disconnect (DISC) frame
 *
 */
void irlap_send_disc_frame(struct irlap_cb *self) 
{
	struct sk_buff *skb = NULL;
	__u8 *frame;
	
	IRDA_DEBUG(3, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);

	skb = dev_alloc_skb(16);
	if (!skb)
		return;

	frame = skb_put(skb, 2);
	
	frame[0] = self->caddr | CMD_FRAME;
	frame[1] = DISC_CMD | PF_BIT;

	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_send_discovery_xid_frame (S, s, command)
 *
 *    Build and transmit a XID (eXchange station IDentifier) discovery
 *    frame. 
 */
void irlap_send_discovery_xid_frame(struct irlap_cb *self, int S, __u8 s, 
				    __u8 command, discovery_t *discovery) 
{
	struct sk_buff *skb = NULL;
	struct xid_frame *frame;
	__u32 bcast = BROADCAST;
	__u8 *info;

 	IRDA_DEBUG(4, __FUNCTION__ "(), s=%d, S=%d, command=%d\n", s, S, 
		   command);

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);
	ASSERT(discovery != NULL, return;);

	skb = dev_alloc_skb(64);
	if (!skb)
		return;

	skb_put(skb, 14);
	frame = (struct xid_frame *) skb->data;

	if (command) {
		frame->caddr = CBROADCAST | CMD_FRAME;
		frame->control =  XID_CMD | PF_BIT;
	} else {
		frame->caddr = CBROADCAST;
		frame->control =  XID_RSP | PF_BIT;
	}
	frame->ident = XID_FORMAT;

	frame->saddr = cpu_to_le32(self->saddr);

	if (command)
		frame->daddr = cpu_to_le32(bcast);
	else
		frame->daddr = cpu_to_le32(discovery->daddr);
	
	switch (S) {
	case 1:
		frame->flags = 0x00;
		break;
	case 6:
		frame->flags = 0x01;
		break;
	case 8:
		frame->flags = 0x02;
		break;
	case 16:
		frame->flags = 0x03;
		break;
	default:
		frame->flags = 0x02;
		break;
	}

	frame->slotnr = s; 
	frame->version = 0x00;

	/*  
	 *  Provide info for final slot only in commands, and for all
	 *  responses. Send the second byte of the hint only if the
	 *  EXTENSION bit is set in the first byte.
	 */
	if (!command || (frame->slotnr == 0xff)) {
		int len;

		if (discovery->hints.byte[0] & HINT_EXTENSION) {
			info = skb_put(skb, 2);		
			info[0] = discovery->hints.byte[0];
			info[1] = discovery->hints.byte[1];
		} else {
			info = skb_put(skb, 1);
			info[0] = discovery->hints.byte[0];
		}
		info = skb_put(skb, 1);
		info[0] = discovery->charset;

		len = IRDA_MIN(discovery->name_len, skb_tailroom(skb));
		info = skb_put(skb, len);
		memcpy(info, discovery->nickname, len);
	} 
	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_recv_discovery_xid_rsp (skb, info)
 *
 *    Received a XID discovery response
 *
 */
static void irlap_recv_discovery_xid_rsp(struct irlap_cb *self, 
					 struct sk_buff *skb, 
					 struct irlap_info *info) 
{
	struct xid_frame *xid;
	discovery_t *discovery = NULL;
	__u8 *discovery_info;
	char *text;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);

	xid = (struct xid_frame *) skb->data;

	info->daddr = le32_to_cpu(xid->saddr);
	info->saddr = le32_to_cpu(xid->daddr);

	/* Make sure frame is addressed to us */
	if ((info->saddr != self->saddr) && (info->saddr != BROADCAST)) {
		IRDA_DEBUG(0, __FUNCTION__ 
			   "(), frame is not addressed to us!\n");
		return;
	}

	if ((discovery = kmalloc(sizeof(discovery_t), GFP_ATOMIC)) == NULL) {
		WARNING(__FUNCTION__ "(), kmalloc failed!\n");
		return;
	}
	memset(discovery, 0, sizeof(discovery_t));

	discovery->daddr = info->daddr;
	discovery->saddr = self->saddr;
	discovery->timestamp = jiffies;

	IRDA_DEBUG(4, __FUNCTION__ "(), daddr=%08x\n", discovery->daddr);

	discovery_info = skb_pull(skb, sizeof(struct xid_frame));

	/* Get info returned from peer */
	discovery->hints.byte[0] = discovery_info[0];
	if (discovery_info[0] & HINT_EXTENSION) {
		IRDA_DEBUG(4, "EXTENSION\n");
		discovery->hints.byte[1] = discovery_info[1];
		discovery->charset = discovery_info[2];
		text = (char *) &discovery_info[3];
	} else {
		discovery->hints.byte[1] = 0;
		discovery->charset = discovery_info[1];
		text = (char *) &discovery_info[2];
	}
	/* 
	 *  Terminate info string, should be safe since this is where the 
	 *  FCS bytes resides.
	 */
	skb->data[skb->len] = '\0'; 
	strncpy(discovery->nickname, text, NICKNAME_MAX_LEN);
	discovery->name_len = strlen(discovery->nickname);

	info->discovery = discovery;

	irlap_do_event(self, RECV_DISCOVERY_XID_RSP, skb, info);
}

/*
 * Function irlap_recv_discovery_xid_cmd (skb, info)
 *
 *    Received a XID discovery command
 *
 */
static void irlap_recv_discovery_xid_cmd(struct irlap_cb *self, 
					 struct sk_buff *skb, 
					 struct irlap_info *info) 
{
	struct xid_frame *xid;
	discovery_t *discovery = NULL;
	__u8 *discovery_info;
	char *text;

	xid = (struct xid_frame *) skb->data;

	info->daddr = le32_to_cpu(xid->saddr);
	info->saddr = le32_to_cpu(xid->daddr);

	/* Make sure frame is addressed to us */
	if ((info->saddr != self->saddr) && (info->saddr != BROADCAST)) {
		IRDA_DEBUG(0, __FUNCTION__ 
			   "(), frame is not addressed to us!\n");
		return;
	}

	switch (xid->flags & 0x03) {
	case 0x00:
		info->S = 1;
		break;
	case 0x01:
		info->S = 6;
		break;
	case 0x02:
		info->S = 8;
		break;
	case 0x03:
		info->S = 16;
		break;
	default:
		/* Error!! */
		dev_kfree_skb(skb);
		return;
	}
	info->s = xid->slotnr;
	
	discovery_info = skb_pull(skb, sizeof(struct xid_frame));

	/* 
	 *  Check if last frame 
	 */
	if (info->s == 0xff) {
		/* Check if things are sane at this point... */
		if((discovery_info == NULL) || (skb->len < 3)) {
			ERROR(__FUNCTION__ "(), discovery frame to short!\n");
			return;
		}

		/*
		 *  We now have some discovery info to deliver!
		 */
		discovery = kmalloc(sizeof(discovery_t), GFP_ATOMIC);
		if (!discovery) {
			WARNING(__FUNCTION__ "(), unable to malloc!\n");
			return;
		}
	      
		discovery->daddr = info->daddr;
		discovery->saddr = self->saddr;
		discovery->timestamp = jiffies;

		discovery->hints.byte[0] = discovery_info[0];
		if (discovery_info[0] & HINT_EXTENSION) {
			discovery->hints.byte[1] = discovery_info[1];
			discovery->charset = discovery_info[2];
			text = (char *) &discovery_info[3];
		} else {
			discovery->hints.byte[1] = 0;
			discovery->charset = discovery_info[1];
			text = (char *) &discovery_info[2];
		}
		/* 
		 *  Terminate string, should be safe since this is where the 
		 *  FCS bytes resides.
		 */
		skb->data[skb->len] = '\0'; 
		strncpy(discovery->nickname, text, NICKNAME_MAX_LEN);
		discovery->name_len = strlen(discovery->nickname);

		info->discovery = discovery;
	} else
		info->discovery = NULL;

	irlap_do_event(self, RECV_DISCOVERY_XID_CMD, skb, info);
}

/*
 * Function irlap_send_rr_frame (self, command)
 *
 *    Build and transmit RR (Receive Ready) frame. Notice that it is currently
 *    only possible to send RR frames with the poll bit set.
 */
void irlap_send_rr_frame(struct irlap_cb *self, int command) 
{
	struct sk_buff *skb;
	__u8 *frame;

	skb = dev_alloc_skb(16);
	if (!skb)
		return;
	
	frame = skb_put(skb, 2);
	
	frame[0] = self->caddr;
	frame[0] |= (command) ? CMD_FRAME : 0;

	frame[1] = RR | PF_BIT | (self->vr << 5);

	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_send_rd_frame (self)
 *
 *    Request disconnect. Used by a secondary station to request the 
 *    disconnection of the link.
 */
void irlap_send_rd_frame(struct irlap_cb *self)
{
	struct sk_buff *skb;
	__u8 *frame;

	skb = dev_alloc_skb(16);
	if (!skb)
		return;
	
	frame = skb_put(skb, 2);
	
	frame[0] = self->caddr;
	frame[1] = RD_RSP | PF_BIT;

	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_recv_rr_frame (skb, info)
 *
 *    Received RR (Receive Ready) frame from peer station, no harm in
 *    making it inline since its called only from one single place
 *    (irlap_driver_rcv).
 */
static inline void irlap_recv_rr_frame(struct irlap_cb *self, 
				       struct sk_buff *skb, 
				       struct irlap_info *info, int command)
{
	info->nr = skb->data[1] >> 5;

	/* Check if this is a command or a response frame */
	if (command)
		irlap_do_event(self, RECV_RR_CMD, skb, info);
	else
		irlap_do_event(self, RECV_RR_RSP, skb, info);
}

void irlap_send_frmr_frame( struct irlap_cb *self, int command)
{
	struct sk_buff *skb = NULL;
	__u8 *frame;
	
	ASSERT( self != NULL, return;);
	ASSERT( self->magic == LAP_MAGIC, return;);

	skb = dev_alloc_skb( 32);
	if (!skb)
		return;

	frame = skb_put( skb, 2);
	
	frame[0] = self->caddr;
	frame[0] |= (command) ? CMD_FRAME : 0;

	frame[1]  = (self->vs << 1);
	frame[1] |= PF_BIT;
	frame[1] |= (self->vr << 5);

	frame[2] = 0;

   	IRDA_DEBUG(4, __FUNCTION__ "(), vr=%d, %ld\n",self->vr, jiffies); 

	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_recv_rnr_frame (self, skb, info)
 *
 *    Received RNR (Receive Not Ready) frame from peer station
 *
 */
static void irlap_recv_rnr_frame(struct irlap_cb *self, struct sk_buff *skb, 
				 struct irlap_info *info, int command) 
{
	info->nr = skb->data[1] >> 5;

	IRDA_DEBUG(4, __FUNCTION__ "(), nr=%d, %ld\n", info->nr, jiffies);

	if (command)
		irlap_do_event(self, RECV_RNR_CMD, skb, info);
	else
		irlap_do_event(self, RECV_RNR_RSP, skb, info);
}

static void irlap_recv_rej_frame(struct irlap_cb *self, struct sk_buff *skb, 
				 struct irlap_info *info, int command)
{
	IRDA_DEBUG(0, __FUNCTION__ "()\n");

	info->nr = skb->data[1] >> 5;
	
	/* Check if this is a command or a response frame */
	if (command)
		irlap_do_event(self, RECV_REJ_CMD, skb, info);
	else
		irlap_do_event(self, RECV_REJ_RSP, skb, info);
}

static void irlap_recv_srej_frame(struct irlap_cb *self, struct sk_buff *skb, 
				  struct irlap_info *info, int command)
{
	IRDA_DEBUG(0, __FUNCTION__ "()\n");

	info->nr = skb->data[1] >> 5;
	
	/* Check if this is a command or a response frame */
	if (command)
		irlap_do_event(self, RECV_SREJ_CMD, skb, info);
	else
		irlap_do_event(self, RECV_SREJ_RSP, skb, info);
}

static void irlap_recv_disc_frame(struct irlap_cb *self, struct sk_buff *skb, 
				  struct irlap_info *info, int command)
{
	IRDA_DEBUG(0, __FUNCTION__ "()\n");

	/* Check if this is a command or a response frame */
	if (command)
		irlap_do_event(self, RECV_DISC_CMD, skb, info);
	else
		irlap_do_event(self, RECV_RD_RSP, skb, info);
}

/*
 * Function irlap_recv_ua_frame (skb, frame)
 *
 *    Received UA (Unnumbered Acknowledgement) frame
 *
 */
static inline void irlap_recv_ua_frame(struct irlap_cb *self, 
				       struct sk_buff *skb, 
				       struct irlap_info *info) 
{
	irlap_do_event(self, RECV_UA_RSP, skb, info);
}

/*
 * Function irlap_send_data_primary(self, skb)
 *
 *    Send I-frames as the primary station but without the poll bit set
 *
 */
void irlap_send_data_primary(struct irlap_cb *self, struct sk_buff *skb)
{
	struct sk_buff *tx_skb;

	if (skb->data[1] == I_FRAME) {

		/*  
		 *  Insert frame sequence number (Vs) in control field before
		 *  inserting into transmit window queue.
		 */
		skb->data[1] = I_FRAME | (self->vs << 1);
		
		/* Copy buffer */
		tx_skb = skb_clone(skb, GFP_ATOMIC);
		if (tx_skb == NULL) {
			return;
		}
		
		/*
		 *  make sure the skb->sk accounting of memory usage is sane
		 */
		if (skb->sk != NULL)
			skb_set_owner_w(tx_skb, skb->sk);
		
		/* 
		 *  Insert frame in store, in case of retransmissions 
		 */
		skb_queue_tail(&self->wx_list, skb_get(skb));
		
		self->vs = (self->vs + 1) % 8;
		self->ack_required = FALSE;		
		self->window -= 1;

		irlap_send_i_frame( self, tx_skb, CMD_FRAME);
	} else {
		IRDA_DEBUG(4, __FUNCTION__ "(), sending unreliable frame\n");
		irlap_send_ui_frame(self, skb_get(skb), self->caddr, CMD_FRAME);
		self->window -= 1;
	}
}
/*
 * Function irlap_send_data_primary_poll (self, skb)
 *
 *    Send I(nformation) frame as primary with poll bit set
 */
void irlap_send_data_primary_poll(struct irlap_cb *self, struct sk_buff *skb) 
{
	struct sk_buff *tx_skb;

	/* Is this reliable or unreliable data? */
	if (skb->data[1] == I_FRAME) {
		
		/*  
		 *  Insert frame sequence number (Vs) in control field before
		 *  inserting into transmit window queue.
		 */
		skb->data[1] = I_FRAME | (self->vs << 1);
		
		/* Copy buffer */
		tx_skb = skb_clone(skb, GFP_ATOMIC);
		if (tx_skb == NULL) {
			return;
		}
		
		/*
		 *  make sure the skb->sk accounting of memory usage is sane
		 */
		if (skb->sk != NULL)
			skb_set_owner_w(tx_skb, skb->sk);
		
		/* 
		 *  Insert frame in store, in case of retransmissions 
		 */
		skb_queue_tail(&self->wx_list, skb_get(skb));
		
		/*  
		 *  Set poll bit if necessary. We do this to the copied
		 *  skb, since retransmitted need to set or clear the poll
		 *  bit depending on when they are sent.  
		 */
		/* Stop P timer */
		del_timer(&self->poll_timer);
		
		tx_skb->data[1] |= PF_BIT;
		
		self->vs = (self->vs + 1) % 8;
		self->ack_required = FALSE;
		self->window = self->window_size;

		irlap_start_final_timer(self, self->final_timeout);

		irlap_send_i_frame(self, tx_skb, CMD_FRAME);
	} else {
		IRDA_DEBUG(4, __FUNCTION__ "(), sending unreliable frame\n");

		del_timer(&self->poll_timer);

		if (self->ack_required) {
			irlap_send_ui_frame(self, skb_get(skb), self->caddr, CMD_FRAME);
			irlap_send_rr_frame(self, CMD_FRAME);
			self->ack_required = FALSE;
		} else {
			skb->data[1] |= PF_BIT;
			irlap_send_ui_frame(self, skb_get(skb), self->caddr, CMD_FRAME);
		}
		self->window = self->window_size;
		irlap_start_final_timer(self, self->final_timeout);
	}
}

/*
 * Function irlap_send_data_secondary_final (self, skb)
 *
 *    Send I(nformation) frame as secondary with final bit set
 *
 */
void irlap_send_data_secondary_final(struct irlap_cb *self, 
				     struct sk_buff *skb) 
{
	struct sk_buff *tx_skb = NULL;

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);
	ASSERT(skb != NULL, return;);

	/* Is this reliable or unreliable data? */
	if (skb->data[1] == I_FRAME) {

		/*  
		 *  Insert frame sequence number (Vs) in control field before
		 *  inserting into transmit window queue.
		 */
		skb->data[1] = I_FRAME | (self->vs << 1);
		
		tx_skb = skb_clone(skb, GFP_ATOMIC);
		if (tx_skb == NULL) {
			return;
		}		

		if (skb->sk != NULL)
			skb_set_owner_w(tx_skb, skb->sk);
		
		/* Insert frame in store */
		skb_queue_tail(&self->wx_list, skb_get(skb));
		
		tx_skb->data[1] |= PF_BIT;
		
		self->vs = (self->vs + 1) % 8; 
		self->window = self->window_size;
		self->ack_required = FALSE;
		
		irlap_start_wd_timer(self, self->wd_timeout);

		irlap_send_i_frame(self, tx_skb, RSP_FRAME); 
	} else {
		if (self->ack_required) {
			irlap_send_ui_frame(self, skb_get(skb), self->caddr, RSP_FRAME);
			irlap_send_rr_frame(self, RSP_FRAME);
			self->ack_required = FALSE;
		} else {
			skb->data[1] |= PF_BIT;
			irlap_send_ui_frame(self, skb_get(skb), self->caddr, RSP_FRAME);
		}
		self->window = self->window_size;

		irlap_start_wd_timer(self, self->wd_timeout);
	}
}

/*
 * Function irlap_send_data_secondary (self, skb)
 *
 *    Send I(nformation) frame as secondary without final bit set
 *
 */
void irlap_send_data_secondary(struct irlap_cb *self, struct sk_buff *skb) 
{
	struct sk_buff *tx_skb = NULL;

	/* Is this reliable or unreliable data? */
	if (skb->data[1] == I_FRAME) {
		
		/*  
		 *  Insert frame sequence number (Vs) in control field before
		 *  inserting into transmit window queue.
		 */
		skb->data[1] = I_FRAME | (self->vs << 1);
		
		tx_skb = skb_clone(skb, GFP_ATOMIC);
		if (tx_skb == NULL) {
			return;
		}		
		
		if (skb->sk != NULL)
			skb_set_owner_w(tx_skb, skb->sk);
		
		/* Insert frame in store */
		skb_queue_tail(&self->wx_list, skb_get(skb));
		
		self->vs = (self->vs + 1) % 8;
		self->ack_required = FALSE;		
		self->window -= 1;

		irlap_send_i_frame(self, tx_skb, RSP_FRAME); 
	} else {
		irlap_send_ui_frame(self, skb_get(skb), self->caddr, RSP_FRAME);
		self->window -= 1;
	}
}

/*
 * Function irlap_resend_rejected_frames (nr)
 *
 *    Resend frames which has not been acknowledged. Should be safe to 
 *    traverse the list without locking it since this function will only be 
 *    called from interrupt context (BH)
 */
void irlap_resend_rejected_frames(struct irlap_cb *self, int command)
{
	struct sk_buff *tx_skb;
	struct sk_buff *skb;
	int count;

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);

	/* Initialize variables */
	skb = tx_skb = NULL;

	count = skb_queue_len(&self->wx_list);

	/*  Resend unacknowledged frame(s) */
	skb = skb_peek(&self->wx_list);
	while (skb != NULL) {
		irlap_wait_min_turn_around(self, &self->qos_tx);

		/* We copy the skb to be retransmitted since we will have to 
		 * modify it. Cloning will confuse packet sniffers 
		 */
		/* tx_skb = skb_clone( skb, GFP_ATOMIC); */
		tx_skb = skb_copy(skb, GFP_ATOMIC);
		if (!tx_skb) {
			IRDA_DEBUG(0, __FUNCTION__ "(), unable to copy\n");
			return;
		}
		/* Unlink tx_skb from list */
		tx_skb->next = tx_skb->prev = NULL;
		tx_skb->list = NULL;

		/*
		 *  make sure the skb->sk accounting of memory usage is sane
		 */
		if (skb->sk != NULL)
			skb_set_owner_w(tx_skb, skb->sk);

		/* Clear old Nr field + poll bit */
		tx_skb->data[1] &= 0x0f;

		/* 
		 *  Set poll bit on the last frame retransmitted
		 */
	 	if (count-- == 1)
	 		tx_skb->data[1] |= PF_BIT; /* Set p/f bit */
		else
			tx_skb->data[1] &= ~PF_BIT; /* Clear p/f bit */
	      	
		irlap_send_i_frame(self, tx_skb, command);

		/* 
		 *  If our skb is the last buffer in the list, then
		 *  we are finished, if not, move to the next sk-buffer
		 */
		if (skb == skb_peek_tail(&self->wx_list))
			skb = NULL;
		else
			skb = skb->next;
	}
#if 0 /* Not yet */
	/* 
	 *  We can now fill the window with additinal data frames
	 */
	while (skb_queue_len( &self->txq) > 0) {
		
		IRDA_DEBUG(0, __FUNCTION__ "(), sending additional frames!\n");
		if ((skb_queue_len( &self->txq) > 0) && 
		    (self->window > 0)) {
			skb = skb_dequeue( &self->txq); 
			ASSERT(skb != NULL, return;);

			/*
			 *  If send window > 1 then send frame with pf 
			 *  bit cleared
			 */ 
			if ((self->window > 1) && 
			    skb_queue_len(&self->txq) > 0) 
			{
				irlap_send_data_primary(self, skb);
			} else {
				irlap_send_data_primary_poll(self, skb);
			}
			kfree_skb(skb);
		}
	}
#endif
}

void irlap_resend_rejected_frame(struct irlap_cb *self, int command)
{
	struct sk_buff *tx_skb;
	struct sk_buff *skb;

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);

	/* Initialize variables */
	skb = tx_skb = NULL;

	/*  Resend unacknowledged frame(s) */
	skb = skb_peek(&self->wx_list);
	if (skb != NULL) {
		irlap_wait_min_turn_around(self, &self->qos_tx);

		/* We copy the skb to be retransmitted since we will have to 
		 * modify it. Cloning will confuse packet sniffers 
		 */
		/* tx_skb = skb_clone( skb, GFP_ATOMIC); */
		tx_skb = skb_copy(skb, GFP_ATOMIC);
		if (!tx_skb) {
			IRDA_DEBUG(0, __FUNCTION__ "(), unable to copy\n");
			return;	
		}
		/* Unlink tx_skb from list */
		tx_skb->next = tx_skb->prev = NULL;
		tx_skb->list = NULL;

		/*
		 *  make sure the skb->sk accounting of memory usage is sane
		 */
		if (skb->sk != NULL)
			skb_set_owner_w(tx_skb, skb->sk);

		/* Clear old Nr field + poll bit */
		tx_skb->data[1] &= 0x0f;

		/*  Set poll/final bit */
		tx_skb->data[1] |= PF_BIT; /* Set p/f bit */
	      	
		irlap_send_i_frame(self, tx_skb, command);
	}
}

/*
 * Function irlap_send_ui_frame (self, skb, command)
 *
 *    Contruct and transmit an Unnumbered Information (UI) frame
 *
 */
void irlap_send_ui_frame(struct irlap_cb *self, struct sk_buff *skb, 
			 __u8 caddr, int command)
{
	IRDA_DEBUG(4, __FUNCTION__ "()\n");
	
	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);
	ASSERT(skb != NULL, return;);
	
	/* Insert connection address */
	skb->data[0] = caddr | ((command) ? CMD_FRAME : 0);

	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_send_i_frame (skb)
 *
 *    Contruct and transmit Information (I) frame
 */
void irlap_send_i_frame(struct irlap_cb *self, struct sk_buff *skb, 
			int command) 
{
	/* Insert connection address */
	skb->data[0] = self->caddr;
	skb->data[0] |= (command) ? CMD_FRAME : 0;
	
	/* Insert next to receive (Vr) */
	skb->data[1] |= (self->vr << 5);  /* insert nr */

	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_recv_i_frame (skb, frame)
 *
 *    Receive and parse an I (Information) frame, no harm in making it inline
 *    since it's called only from one single place (irlap_driver_rcv).
 */
static inline void irlap_recv_i_frame(struct irlap_cb *self, 
				      struct sk_buff *skb, 
				      struct irlap_info *info, int command) 
{
	info->nr = skb->data[1] >> 5;          /* Next to receive */
	info->pf = skb->data[1] & PF_BIT;      /* Final bit */
	info->ns = (skb->data[1] >> 1) & 0x07; /* Next to send */

	/* Check if this is a command or a response frame */
	if (command)
		irlap_do_event(self, RECV_I_CMD, skb, info);
	else
		irlap_do_event(self, RECV_I_RSP, skb, info);
}

/*
 * Function irlap_recv_ui_frame (self, skb, info)
 *
 *    Receive and parse an Unnumbered Information (UI) frame
 *
 */
static void irlap_recv_ui_frame(struct irlap_cb *self, struct sk_buff *skb, 
				struct irlap_info *info)
{
	IRDA_DEBUG( 4, __FUNCTION__ "()\n");

	info->pf = skb->data[1] & PF_BIT;      /* Final bit */

	irlap_do_event(self, RECV_UI_FRAME, skb, info);
}

/*
 * Function irlap_recv_frmr_frame (skb, frame)
 *
 *    Received Frame Reject response.
 *
 */
static void irlap_recv_frmr_frame(struct irlap_cb *self, struct sk_buff *skb, 
				  struct irlap_info *info) 
{
	__u8 *frame;
	int w, x, y, z;

	IRDA_DEBUG(0, __FUNCTION__ "()\n");
	
	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);
	ASSERT(skb != NULL, return;);
	ASSERT(info != NULL, return;);
	
	frame = skb->data;

	info->nr = frame[2] >> 5;          /* Next to receive */
	info->pf = frame[2] & PF_BIT;      /* Final bit */
	info->ns = (frame[2] >> 1) & 0x07; /* Next to send */

	w = frame[3] & 0x01;
	x = frame[3] & 0x02;
	y = frame[3] & 0x04;
	z = frame[3] & 0x08;
	
	if (w) {
		IRDA_DEBUG(0, "Rejected control field is undefined or not "
		      "implemented.\n");
	} 
	if (x) {
		IRDA_DEBUG(0, "Rejected control field was invalid because it "
		      "contained a non permitted I field.\n");
	}
	if (y) {
		IRDA_DEBUG(0, "Received I field exceeded the maximum negotiated "
		      "for the existing connection or exceeded the maximum "
		      "this station supports if no connection exists.\n");
	}
	if (z) {
		IRDA_DEBUG(0, "Rejected control field control field contained an "
		      "invalid Nr count.\n");
	}
	irlap_do_event(self, RECV_FRMR_RSP, skb, info);
}

/*
 * Function irlap_send_test_frame (self, daddr)
 *
 *    Send a test frame response
 *
 */
void irlap_send_test_frame(struct irlap_cb *self, __u8 caddr, __u32 daddr, 
			   struct sk_buff *cmd)
{
	struct sk_buff *skb;
	struct test_frame *frame;
	__u8 *info;

	skb = dev_alloc_skb(cmd->len+sizeof(struct test_frame));
	if (!skb)
		return;

	/* Broadcast frames must include saddr and daddr fields */
	if (caddr == CBROADCAST) {
		frame = (struct test_frame *) 
			skb_put(skb, sizeof(struct test_frame));

		/* Insert the swapped addresses */
		frame->saddr = cpu_to_le32(self->saddr);
		frame->daddr = cpu_to_le32(daddr);
	} else
		frame = (struct test_frame *) skb_put(skb, LAP_ADDR_HEADER + LAP_CTRL_HEADER);

	frame->caddr = caddr;
	frame->control = TEST_RSP | PF_BIT;

	/* Copy info */
	info = skb_put(skb, cmd->len);
	memcpy(info, cmd->data, cmd->len);

	/* Return to sender */
	irlap_wait_min_turn_around(self, &self->qos_tx);
	irlap_queue_xmit(self, skb);
}

/*
 * Function irlap_recv_test_frame (self, skb)
 *
 *    Receive a test frame
 *
 */
static void irlap_recv_test_frame(struct irlap_cb *self, struct sk_buff *skb, 
				  struct irlap_info *info, int command)
{
	struct test_frame *frame;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");
	
	frame = (struct test_frame *) skb->data;
		
	/* Broadcast frames must carry saddr and daddr fields */
	if (info->caddr == CBROADCAST) {
		if (skb->len < sizeof(struct test_frame)) {
			IRDA_DEBUG(0, __FUNCTION__ 
				   "() test frame to short!\n");
			return;
		}
		
		/* Read and swap addresses */
		info->daddr = le32_to_cpu(frame->saddr);
		info->saddr = le32_to_cpu(frame->daddr);

		/* Make sure frame is addressed to us */
		if ((info->saddr != self->saddr) && 
		    (info->saddr != BROADCAST)) {
			return;
		}
	}

	if (command)
		irlap_do_event(self, RECV_TEST_CMD, skb, info);
	else
		irlap_do_event(self, RECV_TEST_RSP, skb, info);
}

/*
 * Function irlap_driver_rcv (skb, netdev, ptype)
 *
 *    Called when a frame is received. Dispatches the right receive function 
 *    for processing of the frame.
 *
 */
int irlap_driver_rcv(struct sk_buff *skb, struct net_device *dev, 
		     struct packet_type *ptype)
{
	struct irlap_info info;
	struct irlap_cb *self;
	int command;
	__u8 control;
	
	/* FIXME: should we get our own field? */
	self = (struct irlap_cb *) dev->atalk_ptr;

	/* If the net device is down, then IrLAP is gone! */
	if (!self || self->magic != LAP_MAGIC) {
		dev_kfree_skb(skb);
		return -1;
	}

	/* Check if frame is large enough for parsing */
	if (skb->len < 2) {
		ERROR(__FUNCTION__ "(), frame to short!\n");
		dev_kfree_skb(skb);
		return -1;
	}
	
	command    = skb->data[0] & CMD_FRAME;
	info.caddr = skb->data[0] & CBROADCAST;
	
	info.pf      = skb->data[1] &  PF_BIT;
	info.control = skb->data[1] & ~PF_BIT; /* Mask away poll/final bit */

	control = info.control;

	/*  First we check if this frame has a valid connection address */
	if ((info.caddr != self->caddr) && (info.caddr != CBROADCAST)) {
		IRDA_DEBUG(0, __FUNCTION__ "(), wrong connection address!\n");
		goto out;
	}
	/*  
	 *  Optimize for the common case and check if the frame is an
	 *  I(nformation) frame. Only I-frames have bit 0 set to 0
	 */
	if (~control & 0x01) {
		irlap_recv_i_frame(self, skb, &info, command);
		goto out;
	}
	/*
	 *  We now check is the frame is an S(upervisory) frame. Only 
	 *  S-frames have bit 0 set to 1 and bit 1 set to 0
	 */
	if (~control & 0x02) {
		/* 
		 *  Received S(upervisory) frame, check which frame type it is
		 *  only the first nibble is of interest
		 */
		switch (control & 0x0f) {
		case RR:
			irlap_recv_rr_frame(self, skb, &info, command);
			break;
		case RNR:
			irlap_recv_rnr_frame(self, skb, &info, command);
			break;
		case REJ:
			irlap_recv_rej_frame(self, skb, &info, command);
			break;
		case SREJ:
			irlap_recv_srej_frame(self, skb, &info, command);
			break;
		default:
			WARNING(__FUNCTION__ 
				"() Unknown S-frame %02x received!\n",
				info.control);
			break;
		}
		goto out;
	}
	/* 
	 *  This must be a C(ontrol) frame 
	 */
	switch (control) {
	case XID_RSP:
		irlap_recv_discovery_xid_rsp(self, skb, &info);
		break;
	case XID_CMD:
		irlap_recv_discovery_xid_cmd(self, skb, &info);
		break;
	case SNRM_CMD:
		irlap_recv_snrm_cmd(self, skb, &info);
		break;
	case DM_RSP:
		irlap_do_event(self, RECV_DM_RSP, skb, &info);
		break;
	case DISC_CMD: /* And RD_RSP since they have the same value */
		irlap_recv_disc_frame(self, skb, &info, command);
		break;
	case TEST_CMD:
		irlap_recv_test_frame(self, skb, &info, command);
		break;
	case UA_RSP:
		irlap_recv_ua_frame(self, skb, &info);
		break;
	case FRMR_RSP:
		irlap_recv_frmr_frame(self, skb, &info);
		break;
	case UI_FRAME:
		irlap_recv_ui_frame(self, skb, &info);
		break;
	default:
		WARNING(__FUNCTION__ "(), Unknown frame %02x received!\n", 
			info.control);
		break;
	}
out:
	dev_kfree_skb(skb); 
	return 0;
}
