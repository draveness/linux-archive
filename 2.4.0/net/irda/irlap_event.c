/*********************************************************************
 *                
 * Filename:      irlap_event.c
 * Version:       0.9
 * Description:   IrLAP state machine implementation
 * Status:        Experimental.
 * Author:        Dag Brattli <dag@brattli.net>
 * Created at:    Sat Aug 16 00:59:29 1997
 * Modified at:   Sat Dec 25 21:07:57 1999
 * Modified by:   Dag Brattli <dag@brattli.net>
 * 
 *     Copyright (c) 1998-2000 Dag Brattli <dag@brattli.net>,
 *     Copyright (c) 1998      Thomas Davis <ratbert@radiks.net>
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

#include <linux/config.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/skbuff.h>

#include <net/irda/irda.h>
#include <net/irda/irlap_event.h>

#include <net/irda/timer.h>
#include <net/irda/irlap.h>
#include <net/irda/irlap_frame.h>
#include <net/irda/qos.h>
#include <net/irda/parameters.h>

#include <net/irda/irda_device.h>

#if CONFIG_IRDA_FAST_RR
int sysctl_fast_poll_increase = 50;
#endif

static int irlap_state_ndm    (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_query  (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_reply  (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_conn   (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_setup  (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_offline(struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_xmit_p (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_pclose (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_nrm_p  (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_reset_wait(struct irlap_cb *self, IRLAP_EVENT event, 
				  struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_reset  (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_nrm_s  (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_xmit_s (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_sclose (struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info);
static int irlap_state_reset_check(struct irlap_cb *, IRLAP_EVENT event, 
				   struct sk_buff *, struct irlap_info *);

static const char *irlap_event[] = {
	"DISCOVERY_REQUEST",
	"CONNECT_REQUEST",
	"CONNECT_RESPONSE",
	"DISCONNECT_REQUEST",
	"DATA_REQUEST",
	"RESET_REQUEST",
	"RESET_RESPONSE",
	"SEND_I_CMD",
	"SEND_UI_FRAME",
	"RECV_DISCOVERY_XID_CMD",
	"RECV_DISCOVERY_XID_RSP",
	"RECV_SNRM_CMD",
	"RECV_TEST_CMD",
	"RECV_TEST_RSP",
	"RECV_UA_RSP",
	"RECV_DM_RSP",
	"RECV_RD_RSP",
	"RECV_I_CMD",
	"RECV_I_RSP",
	"RECV_UI_FRAME",
	"RECV_FRMR_RSP",
	"RECV_RR_CMD",
	"RECV_RR_RSP",
	"RECV_RNR_CMD",
	"RECV_RNR_RSP",
	"RECV_REJ_CMD",
	"RECV_REJ_RSP",
	"RECV_SREJ_CMD",
	"RECV_SREJ_RSP",
	"RECV_DISC_CMD",
	"SLOT_TIMER_EXPIRED",
	"QUERY_TIMER_EXPIRED",
	"FINAL_TIMER_EXPIRED",
	"POLL_TIMER_EXPIRED",
	"DISCOVERY_TIMER_EXPIRED",
	"WD_TIMER_EXPIRED",
	"BACKOFF_TIMER_EXPIRED",
};

const char *irlap_state[] = {
	"LAP_NDM",
	"LAP_QUERY",
	"LAP_REPLY",
	"LAP_CONN",
	"LAP_SETUP",
	"LAP_OFFLINE",
	"LAP_XMIT_P",
	"LAP_PCLOSE",
	"LAP_NRM_P",
	"LAP_RESET_WAIT",
	"LAP_RESET",
	"LAP_NRM_S",
	"LAP_XMIT_S",
	"LAP_SCLOSE",
	"LAP_RESET_CHECK",
};

static int (*state[])(struct irlap_cb *self, IRLAP_EVENT event, 
		      struct sk_buff *skb, struct irlap_info *info) = 
{
	irlap_state_ndm,
	irlap_state_query,
	irlap_state_reply,
	irlap_state_conn,
	irlap_state_setup,
	irlap_state_offline,
	irlap_state_xmit_p,
	irlap_state_pclose,
	irlap_state_nrm_p,
	irlap_state_reset_wait,
	irlap_state_reset,
	irlap_state_nrm_s,
	irlap_state_xmit_s,
	irlap_state_sclose,
	irlap_state_reset_check,
};

/*
 * Function irda_poll_timer_expired (data)
 *
 *    Poll timer has expired. Normally we must now send a RR frame to the
 *    remote device
 */
static void irlap_poll_timer_expired(void *data)
{
	struct irlap_cb *self = (struct irlap_cb *) data;
	
	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);
	
	irlap_do_event(self, POLL_TIMER_EXPIRED, NULL, NULL);
}

void irlap_start_poll_timer(struct irlap_cb *self, int timeout)
{
	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LAP_MAGIC, return;);

#ifdef CONFIG_IRDA_FAST_RR
	/* 
	 * Send out the RR frames faster if our own transmit queue is empty, or
	 * if the peer is busy. The effect is a much faster conversation
	 */
	if ((skb_queue_len(&self->txq) == 0) || (self->remote_busy)) {
		if (self->fast_RR == TRUE) {
			/*
			 *  Assert that the fast poll timer has not reached the
			 *  normal poll timer yet
			 */
			if (self->fast_RR_timeout < timeout) {
				/*
				 *  FIXME: this should be a more configurable
				 *         function
				 */
				self->fast_RR_timeout += 
					(sysctl_fast_poll_increase * HZ/1000);

				/* Use this fast(er) timeout instead */
				timeout = self->fast_RR_timeout;
			}
		} else {
			self->fast_RR = TRUE;

			/* Start with just 0 ms */
			self->fast_RR_timeout = 0;
			timeout = 0;
		}
	} else
		self->fast_RR = FALSE;

	IRDA_DEBUG(3, __FUNCTION__ "(), timeout=%d (%ld)\n", timeout, jiffies);
#endif /* CONFIG_IRDA_FAST_RR */

	if (timeout == 0)
		irlap_do_event(self, POLL_TIMER_EXPIRED, NULL, NULL);
	else
		irda_start_timer(&self->poll_timer, timeout, self, 
				 irlap_poll_timer_expired);
}

/*
 * Function irlap_do_event (event, skb, info)
 *
 *    Rushes through the state machine without any delay. If state == XMIT
 *    then send queued data frames. 
 */
void irlap_do_event(struct irlap_cb *self, IRLAP_EVENT event, 
		    struct sk_buff *skb, struct irlap_info *info) 
{
	int ret;
	
	if (!self || self->magic != LAP_MAGIC)
		return;

  	IRDA_DEBUG(3, __FUNCTION__ "(), event = %s, state = %s\n", 
		   irlap_event[event], irlap_state[self->state]); 
	
	ret = (*state[self->state])(self, event, skb, info);

	/* 
	 *  Check if there are any pending events that needs to be executed
	 */
	switch (self->state) {
	case LAP_XMIT_P: /* FALLTHROUGH */
	case LAP_XMIT_S:
		/* 
		 * Check if there are any queued data frames, and do not
		 * try to disconnect link if we send any data frames, since
		 * that will change the state away form XMIT
		 */
		if (skb_queue_len(&self->txq)) {
			/* Try to send away all queued data frames */
			while ((skb = skb_dequeue(&self->txq)) != NULL) {
				ret = (*state[self->state])(self, SEND_I_CMD,
							    skb, NULL);
				kfree_skb(skb);
				if (ret == -EPROTO)
					break; /* Try again later! */
			}
		} else if (self->disconnect_pending) {
			self->disconnect_pending = FALSE;
			
			ret = (*state[self->state])(self, DISCONNECT_REQUEST,
						    NULL, NULL);
		}
		break;
	case LAP_NDM:
		/* Check if we should try to connect */
		if ((self->connect_pending) && !self->media_busy) {
			self->connect_pending = FALSE;
			
			ret = (*state[self->state])(self, CONNECT_REQUEST, 
						    NULL, NULL);
		}
		break;
/* 	case LAP_CONN: */
/* 	case LAP_RESET_WAIT: */
/* 	case LAP_RESET_CHECK: */
	default:
		break;
	}
}

/*
 * Function irlap_next_state (self, state)
 *
 *    Switches state and provides debug information
 *
 */
void irlap_next_state(struct irlap_cb *self, IRLAP_STATE state) 
{	
	if (!self || self->magic != LAP_MAGIC)
		return;
	
	IRDA_DEBUG(4, "next LAP state = %s\n", irlap_state[state]);

	self->state = state;

#ifdef CONFIG_IRDA_DYNAMIC_WINDOW
	/*
	 *  If we are swithing away from a XMIT state then we are allowed to 
	 *  transmit a maximum number of bytes again when we enter the XMIT 
	 *  state again. Since its possible to "switch" from XMIT to XMIT,
	 *  we cannot do this when swithing into the XMIT state :-)
	 */
	if ((state != LAP_XMIT_P) && (state != LAP_XMIT_S))
		self->bytes_left = self->line_capacity;
#endif /* CONFIG_IRDA_DYNAMIC_WINDOW */
#ifdef CONFIG_IRDA_ULTRA
	/* Send any pending Ultra frames if any */
	/* The higher layers may have sent a few Ultra frames while we
	 * were doing discovery (either query or reply). Those frames
	 * have been queued, but were never sent. It is now time to
	 * send them...
	 * Jean II */
	if ((state == LAP_NDM) && (!skb_queue_empty(&self->txq_ultra)))
		/* Force us to listen 500 ms before sending Ultra */
		irda_device_set_media_busy(self->netdev, TRUE);
#endif /* CONFIG_IRDA_ULTRA */
}

/*
 * Function irlap_state_ndm (event, skb, frame)
 *
 *    NDM (Normal Disconnected Mode) state
 *
 */
static int irlap_state_ndm(struct irlap_cb *self, IRLAP_EVENT event, 
			   struct sk_buff *skb, struct irlap_info *info) 
{
	discovery_t *discovery_rsp;
	int ret = 0;
	int i;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);

	switch (event) {
	case CONNECT_REQUEST:
		ASSERT(self->netdev != NULL, return -1;);

		if (self->media_busy) {
			IRDA_DEBUG(0, __FUNCTION__
				   "(), CONNECT_REQUEST: media busy!\n");
			
			/* Always switch state before calling upper layers */
			irlap_next_state(self, LAP_NDM);
			
			irlap_disconnect_indication(self, LAP_MEDIA_BUSY);
		} else {
			irlap_send_snrm_frame(self, &self->qos_rx);
			
			/* Start Final-bit timer */
			irlap_start_final_timer(self, self->final_timeout);

			self->retry_count = 0;
			irlap_next_state(self, LAP_SETUP);
		}
		break;
	case RECV_SNRM_CMD:
		/* Check if the frame contains and I field */
		if (info) {		       
			self->daddr = info->daddr;
			self->caddr = info->caddr;
			
			irlap_next_state(self, LAP_CONN);

			irlap_connect_indication(self, skb);
		} else {
			IRDA_DEBUG(0, __FUNCTION__ "(), SNRM frame does not "
				   "contain an I field!\n");
		}
		break;
	case DISCOVERY_REQUEST:		
		ASSERT(info != NULL, return -1;);

	 	if (self->media_busy) {
 			IRDA_DEBUG(0, __FUNCTION__ "(), media busy!\n"); 
			/* irlap->log.condition = MEDIA_BUSY; */
						
			/* This will make IrLMP try again */
 			irlap_discovery_confirm(self, NULL);
			return 0;
	 	} 
		
		self->S = info->S;
		self->s = info->s;
		irlap_send_discovery_xid_frame(self, info->S, info->s, TRUE,
					       info->discovery);
		self->frame_sent = FALSE;
		self->s++;

		irlap_start_slot_timer(self, self->slot_timeout);
		irlap_next_state(self, LAP_QUERY);
		break;
	case RECV_DISCOVERY_XID_CMD:
		ASSERT(info != NULL, return -1;);

		/* Assert that this is not the final slot */
		if (info->s <= info->S) {
			self->slot = irlap_generate_rand_time_slot(info->S,
								   info->s);
			if (self->slot == info->s) {
				discovery_rsp = irlmp_get_discovery_response();
				discovery_rsp->daddr = info->daddr;
				
				irlap_send_discovery_xid_frame(self, info->S, 
							       self->slot, 
							       FALSE,
							       discovery_rsp);
				self->frame_sent = TRUE;
			} else
				self->frame_sent = FALSE;
			
			/* 
			 * Remember to multiply the query timeout value with 
			 * the number of slots used
			 */
			irlap_start_query_timer(self, QUERY_TIMEOUT*info->S);
			irlap_next_state(self, LAP_REPLY);
		}
		else {
		/* This is the final slot. How is it possible ?
		 * This would happen is both discoveries are just slightly
		 * offset (if they are in sync, all packets are lost).
		 * Most often, all the discovery requests will be received
		 * in QUERY state (see my comment there), except for the
		 * last frame that will come here.
		 * The big trouble when it happen is that active discovery
		 * doesn't happen, because nobody answer the discoveries
		 * frame of the other guy, so the log shows up empty.
		 * What should we do ?
		 * Not much. It's too late to answer those discovery frames,
		 * so we just pass the info to IrLMP who will put it in the
		 * log (and post an event).
		 * Jean II
		 */
			IRDA_DEBUG(1, __FUNCTION__ "(), Receiving final discovery request, missed the discovery slots :-(\n");

			/* Last discovery request -> in the log */
			irlap_discovery_indication(self, info->discovery); 
		}
		break;
#ifdef CONFIG_IRDA_ULTRA
	case SEND_UI_FRAME:
		/* Only allowed to repeat an operation twice */
		for (i=0; ((i<2) && (self->media_busy == FALSE)); i++) {
			skb = skb_dequeue(&self->txq_ultra);
			if (skb)
				irlap_send_ui_frame(self, skb, CBROADCAST, 
						    CMD_FRAME);
			else
				break;
		}
		if (i == 2) {
			/* Force us to listen 500 ms again */
			irda_device_set_media_busy(self->netdev, TRUE);
		}
		break;
	case RECV_UI_FRAME:
		/* Only accept broadcast frames in NDM mode */
		if (info->caddr != CBROADCAST) {
			IRDA_DEBUG(0, __FUNCTION__ 
				   "(), not a broadcast frame!\n");
		} else
			irlap_unitdata_indication(self, skb);
		break;
#endif /* CONFIG_IRDA_ULTRA */
	case RECV_TEST_CMD:
		/* Remove test frame header */
		skb_pull(skb, sizeof(struct test_frame));

		/* 
		 * Send response. This skb will not be sent out again, and
		 * will only be used to send out the same info as the cmd
		 */
		irlap_send_test_frame(self, CBROADCAST, info->daddr, skb);
		break;
	case RECV_TEST_RSP:
		IRDA_DEBUG(0, __FUNCTION__ "() not implemented!\n");
		break;
	default:
		IRDA_DEBUG(2, __FUNCTION__ "(), Unknown event %s\n", 
			   irlap_event[event]);
		
		ret = -1;
		break;
	}	
	return ret;
}

/*
 * Function irlap_state_query (event, skb, info)
 *
 *    QUERY state
 *
 */
static int irlap_state_query(struct irlap_cb *self, IRLAP_EVENT event, 
			     struct sk_buff *skb, struct irlap_info *info) 
{
	int ret = 0;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);

	switch (event) {
	case RECV_DISCOVERY_XID_RSP:
		ASSERT(info != NULL, return -1;);
		ASSERT(info->discovery != NULL, return -1;);

		IRDA_DEBUG(4, __FUNCTION__ "(), daddr=%08x\n", 
			   info->discovery->daddr);

		if (!self->discovery_log) {
			WARNING(__FUNCTION__ "(), discovery log is gone! "
				"maybe the discovery timeout has been set to "
				"short?\n");
			break;
		}
		hashbin_insert(self->discovery_log, 
			       (irda_queue_t *) info->discovery,
			       info->discovery->daddr, NULL);

		/* Keep state */
		/* irlap_next_state(self, LAP_QUERY);  */

		break;
	case RECV_DISCOVERY_XID_CMD:
		/* Yes, it is possible to receive those frames in this mode.
		 * Note that most often the last discovery request won't
		 * occur here but in NDM state (see my comment there).
		 * What should we do ?
		 * Not much. We are currently performing our own discovery,
		 * therefore we can't answer those frames. We don't want
		 * to change state either. We just pass the info to
		 * IrLMP who will put it in the log (and post an event).
		 * Jean II
		 */

		ASSERT(info != NULL, return -1;);

		IRDA_DEBUG(1, __FUNCTION__ "(), Receiving discovery request (s = %d) while performing discovery :-(\n", info->s);

		/* Last discovery request ? */
		if (info->s == 0xff)
			irlap_discovery_indication(self, info->discovery); 
		break;
	case SLOT_TIMER_EXPIRED:
		/*
		 * Wait a little longer if we detect an incomming frame. This
		 * is not mentioned in the spec, but is a good thing to do, 
		 * since we want to work even with devices that violate the
		 * timing requirements.
		 */
		if (irda_device_is_receiving(self->netdev) && !self->add_wait) {
			IRDA_DEBUG(2, __FUNCTION__ 
				   "(), device is slow to answer, "
				   "waiting some more!\n");
			irlap_start_slot_timer(self, MSECS_TO_JIFFIES(10));
			self->add_wait = TRUE;
			return ret;
		}
		self->add_wait = FALSE;

		if (self->s < self->S) {
			irlap_send_discovery_xid_frame(self, self->S, 
						       self->s, TRUE,
						       self->discovery_cmd);
			self->s++;
			irlap_start_slot_timer(self, self->slot_timeout);
			
			/* Keep state */
			irlap_next_state(self, LAP_QUERY);
		} else {
			/* This is the final slot! */
			irlap_send_discovery_xid_frame(self, self->S, 0xff, 
						       TRUE,
						       self->discovery_cmd);

			/* Always switch state before calling upper layers */
			irlap_next_state(self, LAP_NDM);
	
			/*
			 *  We are now finished with the discovery procedure, 
			 *  so now we must return the results
			 */
			irlap_discovery_confirm(self, self->discovery_log);

			/* IrLMP should now have taken care of the log */
			self->discovery_log = NULL;
		}
		break;
	default:
		IRDA_DEBUG(2, __FUNCTION__ "(), Unknown event %s\n", 
			   irlap_event[event]);

		ret = -1;
		break;
	}
	return ret;
}

/*
 * Function irlap_state_reply (self, event, skb, info)
 *
 *    REPLY, we have received a XID discovery frame from a device and we
 *    are waiting for the right time slot to send a response XID frame
 * 
 */
static int irlap_state_reply(struct irlap_cb *self, IRLAP_EVENT event, 
			     struct sk_buff *skb, struct irlap_info *info) 
{
	discovery_t *discovery_rsp;
	int ret=0;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);

	switch (event) {
	case QUERY_TIMER_EXPIRED:
		IRDA_DEBUG(2, __FUNCTION__ "(), QUERY_TIMER_EXPIRED <%ld>\n",
		      jiffies);
		irlap_next_state(self, LAP_NDM);
		break;
	case RECV_DISCOVERY_XID_CMD:
		ASSERT(info != NULL, return -1;);
		/* Last frame? */
		if (info->s == 0xff) {
			del_timer(&self->query_timer);
			
			/* info->log.condition = REMOTE; */

			/* Always switch state before calling upper layers */
			irlap_next_state(self, LAP_NDM);

			irlap_discovery_indication(self, info->discovery); 
		} else if ((info->s >= self->slot) && (!self->frame_sent)) {
			discovery_rsp = irlmp_get_discovery_response();
			discovery_rsp->daddr = info->daddr;

			irlap_send_discovery_xid_frame(self, info->S,
						       self->slot, FALSE,
						       discovery_rsp);
			
			self->frame_sent = TRUE;
			irlap_next_state(self, LAP_REPLY);
		}
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %d, %s\n", event,
			   irlap_event[event]);

		ret = -1;
		break;
	}
	return ret;
}

/*
 * Function irlap_state_conn (event, skb, info)
 *
 *    CONN, we have received a SNRM command and is waiting for the upper
 *    layer to accept or refuse connection 
 *
 */
static int irlap_state_conn(struct irlap_cb *self, IRLAP_EVENT event, 
			    struct sk_buff *skb, struct irlap_info *info) 
{
	int ret = 0;

	IRDA_DEBUG(4, __FUNCTION__ "(), event=%s\n", irlap_event[ event]);

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);

	switch (event) {
	case CONNECT_RESPONSE:
		skb_pull(skb, sizeof(struct snrm_frame));

		ASSERT(self->netdev != NULL, return -1;);

		irlap_qos_negotiate(self, skb);

		irlap_initiate_connection_state(self);

#if 0
		/* 
		 * We are allowed to send two frames, but this may increase
		 * the connect latency, so lets not do it for now.
		 */
		irlap_send_ua_response_frame(self, &self->qos_rx);
#endif

		/* 
		 * Applying the parameters now will make sure we change speed
		 * after we have sent the next frame
		 */
		irlap_apply_connection_parameters(self);

		/* 
		 * Sending this frame will force a speed change after it has
		 * been sent
		 */
		irlap_send_ua_response_frame(self, &self->qos_rx);

		/*
		 *  The WD-timer could be set to the duration of the P-timer 
		 *  for this case, but it is recommended to use twice the 
		 *  value (note 3 IrLAP p. 60). 
		 */
		irlap_start_wd_timer(self, self->wd_timeout);
		irlap_next_state(self, LAP_NRM_S);

		break;
	case RECV_DISCOVERY_XID_CMD:
		IRDA_DEBUG(3, __FUNCTION__ 
			   "(), event RECV_DISCOVER_XID_CMD!\n");
		irlap_next_state(self, LAP_NDM);

		break;		
	case DISCONNECT_REQUEST:
		irlap_send_dm_frame(self);
		irlap_next_state( self, LAP_CONN);
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %d, %s\n", event,
			   irlap_event[event]);
		
		ret = -1;
		break;
	}
	
	return ret;
}

/*
 * Function irlap_state_setup (event, skb, frame)
 *
 *    SETUP state, The local layer has transmitted a SNRM command frame to
 *    a remote peer layer and is awaiting a reply .
 *
 */
static int irlap_state_setup(struct irlap_cb *self, IRLAP_EVENT event, 
			     struct sk_buff *skb, struct irlap_info *info) 
{
	int ret = 0;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);

	switch (event) {
	case FINAL_TIMER_EXPIRED:
		if (self->retry_count < self->N3) {
/* 
 *  Perform random backoff, Wait a random number of time units, minimum 
 *  duration half the time taken to transmitt a SNRM frame, maximum duration 
 *  1.5 times the time taken to transmit a SNRM frame. So this time should 
 *  between 15 msecs and 45 msecs.
 */
			irlap_start_backoff_timer(self, MSECS_TO_JIFFIES(20 + 
						        (jiffies % 30)));
		} else {
			/* Always switch state before calling upper layers */
			irlap_next_state(self, LAP_NDM);

			irlap_disconnect_indication(self, LAP_FOUND_NONE);
		}
		break;
	case BACKOFF_TIMER_EXPIRED:
		irlap_send_snrm_frame(self, &self->qos_rx);
		irlap_start_final_timer(self, self->final_timeout);
		self->retry_count++;
		break;
	case RECV_SNRM_CMD:
		IRDA_DEBUG(4, __FUNCTION__ "(), SNRM battle!\n");

		ASSERT(skb != NULL, return 0;);
		ASSERT(info != NULL, return 0;);

		/*
		 *  The device with the largest device address wins the battle
		 *  (both have sent a SNRM command!)
		 */
		if (info &&(info->daddr > self->saddr)) {
			del_timer(&self->final_timer);
			irlap_initiate_connection_state(self);

			ASSERT(self->netdev != NULL, return -1;);

			skb_pull(skb, sizeof(struct snrm_frame));

			irlap_qos_negotiate(self, skb);
			
			irlap_send_ua_response_frame(self, &self->qos_rx);
			irlap_apply_connection_parameters(self);

			irlap_next_state(self, LAP_NRM_S);
			irlap_connect_confirm(self, skb);
			
			/* 
			 *  The WD-timer could be set to the duration of the
			 *  P-timer for this case, but it is recommended
			 *  to use twice the value (note 3 IrLAP p. 60).  
			 */
			irlap_start_wd_timer(self, self->wd_timeout);
		} else {
			/* We just ignore the other device! */
			irlap_next_state(self, LAP_SETUP);
		}
		break;
	case RECV_UA_RSP:
		/* Stop F-timer */
		del_timer(&self->final_timer);

		/* Initiate connection state */
		irlap_initiate_connection_state(self);

		/* Negotiate connection parameters */
		ASSERT(skb->len > 10, return -1;);

		skb_pull(skb, sizeof(struct ua_frame));

		ASSERT(self->netdev != NULL, return -1;);

		irlap_qos_negotiate(self, skb);

		irlap_apply_connection_parameters(self);
		self->retry_count = 0;
		
		/* This frame will actually force the speed change */
		irlap_send_rr_frame(self, CMD_FRAME);

		irlap_start_final_timer(self, self->final_timeout/2);
		irlap_next_state(self, LAP_NRM_P);

		irlap_connect_confirm(self, skb);
		break;
	case RECV_DM_RSP:     /* FALLTHROUGH */
	case RECV_DISC_CMD: 
		del_timer(&self->final_timer);
		irlap_next_state(self, LAP_NDM);

		irlap_disconnect_indication(self, LAP_DISC_INDICATION);
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %d, %s\n", event,
			   irlap_event[event]);		

		ret = -1;
		break;
	}	
	return ret;
}

/*
 * Function irlap_state_offline (self, event, skb, info)
 *
 *    OFFLINE state, not used for now!
 *
 */
static int irlap_state_offline(struct irlap_cb *self, IRLAP_EVENT event, 
			       struct sk_buff *skb, struct irlap_info *info) 
{
	IRDA_DEBUG( 0, __FUNCTION__ "(), Unknown event\n");

	return -1;
}

/*
 * Function irlap_state_xmit_p (self, event, skb, info)
 * 
 *    XMIT, Only the primary station has right to transmit, and we
 *    therefore do not expect to receive any transmissions from other
 *    stations.
 * 
 */
static int irlap_state_xmit_p(struct irlap_cb *self, IRLAP_EVENT event, 
			      struct sk_buff *skb, struct irlap_info *info) 
{
	int ret = 0;
	
	switch (event) {
	case SEND_I_CMD:
		/*
		 *  Only send frame if send-window > 0.
		 */ 
		if ((self->window > 0) && (!self->remote_busy)) {
#ifdef CONFIG_IRDA_DYNAMIC_WINDOW
			/*
			 *  Test if we have transmitted more bytes over the 
			 *  link than its possible to do with the current 
			 *  speed and turn-around-time.
			 */
			if (skb->len > self->bytes_left) {
				IRDA_DEBUG(4, __FUNCTION__ 
					   "(), Not allowed to transmit more "
					   "bytes!\n");
				skb_queue_head(&self->txq, skb_get(skb));
				/*
				 *  We should switch state to LAP_NRM_P, but
				 *  that is not possible since we must be sure
				 *  that we poll the other side. Since we have
				 *  used up our time, the poll timer should
				 *  trigger anyway now, so we just wait for it
				 *  DB
				 */
				return -EPROTO;
			}
			self->bytes_left -= skb->len;
#endif /* CONFIG_IRDA_DYNAMIC_WINDOW */
			/*
			 *  Send data with poll bit cleared only if window > 1
			 *  and there is more frames after this one to be sent
			 */
			if ((self->window > 1) && 
			    skb_queue_len( &self->txq) > 0) 
			{   
				irlap_send_data_primary(self, skb);
				irlap_next_state(self, LAP_XMIT_P);
			} else {
				irlap_send_data_primary_poll(self, skb);
				irlap_next_state(self, LAP_NRM_P);
				
				/* 
				 * Make sure state machine does not try to send
				 * any more frames 
				 */
				ret = -EPROTO;
			}
#ifdef CONFIG_IRDA_FAST_RR
			/* Peer may want to reply immediately */
			self->fast_RR = FALSE;
#endif /* CONFIG_IRDA_FAST_RR */
		} else {
			IRDA_DEBUG(4, __FUNCTION__ 
				   "(), Unable to send! remote busy?\n");
			skb_queue_head(&self->txq, skb_get(skb));

			/*
			 *  The next ret is important, because it tells 
			 *  irlap_next_state _not_ to deliver more frames
			 */
			ret = -EPROTO;
		}
		break;
	case POLL_TIMER_EXPIRED:
		IRDA_DEBUG(3, __FUNCTION__ "(), POLL_TIMER_EXPIRED (%ld)\n",
			   jiffies);
		irlap_send_rr_frame(self, CMD_FRAME);
		irlap_start_final_timer(self, self->final_timeout);
		irlap_next_state(self, LAP_NRM_P);
		break;
	case DISCONNECT_REQUEST:
		del_timer(&self->poll_timer);
		irlap_wait_min_turn_around(self, &self->qos_tx);
		irlap_send_disc_frame(self);
		irlap_flush_all_queues(self);
		irlap_start_final_timer(self, self->final_timeout);
		self->retry_count = 0;
		irlap_next_state(self, LAP_PCLOSE);
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "(), Unknown event %s\n", 
			   irlap_event[event]);

		ret = -EINVAL;
		break;
	}
	return ret;
}

/*
 * Function irlap_state_pclose (event, skb, info)
 *
 *    PCLOSE state
 */
static int irlap_state_pclose(struct irlap_cb *self, IRLAP_EVENT event, 
			      struct sk_buff *skb, struct irlap_info *info) 
{
	int ret = 0;

	IRDA_DEBUG(1, __FUNCTION__ "()\n");
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);	

	switch (event) {
	case RECV_UA_RSP: /* FALLTHROUGH */
	case RECV_DM_RSP:
		del_timer(&self->final_timer);
		
		irlap_apply_default_connection_parameters(self);

		/* Always switch state before calling upper layers */
		irlap_next_state(self, LAP_NDM);
		
		irlap_disconnect_indication(self, LAP_DISC_INDICATION);
		break;
	case FINAL_TIMER_EXPIRED:
		if (self->retry_count < self->N3) {
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_disc_frame(self);
			irlap_start_final_timer(self, self->final_timeout);
			self->retry_count++;
			/* Keep state */
		} else {
			irlap_apply_default_connection_parameters(self);

			/*  Always switch state before calling upper layers */
			irlap_next_state(self, LAP_NDM);

			irlap_disconnect_indication(self, LAP_NO_RESPONSE);
		}
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %d\n", event);

		ret = -1;
		break;	
	}
	return ret;
}

/*
 * Function irlap_state_nrm_p (self, event, skb, info)
 *
 *   NRM_P (Normal Response Mode as Primary), The primary station has given
 *   permissions to a secondary station to transmit IrLAP resonse frames
 *   (by sending a frame with the P bit set). The primary station will not
 *   transmit any frames and is expecting to receive frames only from the
 *   secondary to which transmission permissions has been given.
 */
static int irlap_state_nrm_p(struct irlap_cb *self, IRLAP_EVENT event, 
			     struct sk_buff *skb, struct irlap_info *info) 
{
	int ret = 0;
	int ns_status;
	int nr_status;

	switch (event) {
	case RECV_I_RSP: /* Optimize for the common case */
		/* FIXME: must check for remote_busy below */
#ifdef CONFIG_IRDA_FAST_RR
		/* 
		 *  Reset the fast_RR so we can use the fast RR code with
		 *  full speed the next time since peer may have more frames
		 *  to transmitt
		 */
		self->fast_RR = FALSE;
#endif /* CONFIG_IRDA_FAST_RR */
		ASSERT( info != NULL, return -1;);

		ns_status = irlap_validate_ns_received(self, info->ns);
		nr_status = irlap_validate_nr_received(self, info->nr);

		/* 
		 *  Check for expected I(nformation) frame
		 */
		if ((ns_status == NS_EXPECTED) && (nr_status == NR_EXPECTED)) {
			/*  poll bit cleared?  */
			if (!info->pf) {
				self->vr = (self->vr + 1) % 8;
			
				/* Update Nr received */
				irlap_update_nr_received( self, info->nr);
				
				self->ack_required = TRUE;
				
				/* Keep state, do not move this line */
				irlap_next_state(self, LAP_NRM_P);

				irlap_data_indication(self, skb, FALSE);
			} else {
				del_timer(&self->final_timer);

				self->vr = (self->vr + 1) % 8;
			
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
		
				/*  
				 *  Got expected NR, so reset the
				 *  retry_count. This is not done by IrLAP,
				 *  which is strange!  
				 */
				self->retry_count = 0;
				self->ack_required = TRUE;
			
				irlap_wait_min_turn_around(self, &self->qos_tx);
				/* 
				 * Important to switch state before calling
				 * upper layers
				 */
				irlap_next_state(self, LAP_XMIT_P);

				irlap_data_indication(self, skb, FALSE);

				/* This is the last frame */
				irlap_start_poll_timer(self, self->poll_timeout);
			}
			break;
			
		}
		/* Unexpected next to send (Ns) */
		if ((ns_status == NS_UNEXPECTED) && (nr_status == NR_EXPECTED))
		{
			if (!info->pf) {
				irlap_update_nr_received(self, info->nr);
				
				/*
				 *  Wait until the last frame before doing 
				 *  anything
				 */

				/* Keep state */
				irlap_next_state(self, LAP_NRM_P);
			} else {
				IRDA_DEBUG(4, __FUNCTION__
				       "(), missing or duplicate frame!\n");
				
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
				
				irlap_wait_min_turn_around(self, &self->qos_tx);
				irlap_send_rr_frame(self, CMD_FRAME);
				
				self->ack_required = FALSE;
			
				irlap_start_final_timer(self, self->final_timeout);
				irlap_next_state(self, LAP_NRM_P);
			}
			break;
		}
		/* 
		 *  Unexpected next to receive (Nr) 
		 */
		if ((ns_status == NS_EXPECTED) && (nr_status == NR_UNEXPECTED))
		{
			if (info->pf) {
				self->vr = (self->vr + 1) % 8;
			
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
			
				/* Resend rejected frames */
				irlap_resend_rejected_frames(self, CMD_FRAME);
				
				self->ack_required = FALSE;
				irlap_start_final_timer(self, self->final_timeout);
				
				/* Keep state, do not move this line */
				irlap_next_state(self, LAP_NRM_P);

				irlap_data_indication(self, skb, FALSE);
			} else {
				/* 
				 *  Do not resend frames until the last
				 *  frame has arrived from the other
				 *  device. This is not documented in
				 *  IrLAP!!  
				 */
				self->vr = (self->vr + 1) % 8;

				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
				
				self->ack_required = FALSE;

				/* Keep state, do not move this line!*/
				irlap_next_state(self, LAP_NRM_P); 

				irlap_data_indication(self, skb, FALSE);
			}
			break;
		}
		/*
		 *  Unexpected next to send (Ns) and next to receive (Nr)
		 *  Not documented by IrLAP!
		 */
		if ((ns_status == NS_UNEXPECTED) && 
		    (nr_status == NR_UNEXPECTED)) 
		{
			IRDA_DEBUG(4, __FUNCTION__ 
				   "(), unexpected nr and ns!\n");
			if (info->pf) {
				/* Resend rejected frames */
				irlap_resend_rejected_frames(self, CMD_FRAME);
				
				/* Give peer some time to retransmit! */
				irlap_start_final_timer(self, self->final_timeout);

				/* Keep state, do not move this line */
				irlap_next_state(self, LAP_NRM_P);
			} else {
				/* Update Nr received */
				/* irlap_update_nr_received( info->nr); */
				
				self->ack_required = FALSE;
			}
			break;
		}

		/*
		 *  Invalid NR or NS
		 */
		if ((nr_status == NR_INVALID) || (ns_status == NS_INVALID)) {
			if (info->pf) {
				del_timer(&self->final_timer);
				
				irlap_next_state(self, LAP_RESET_WAIT);
				
				irlap_disconnect_indication(self, LAP_RESET_INDICATION);
				self->xmitflag = TRUE;
			} else {
				del_timer(&self->final_timer);
				
				irlap_disconnect_indication(self, LAP_RESET_INDICATION);
				
				self->xmitflag = FALSE;
			}
			break;
		}
		IRDA_DEBUG(1, __FUNCTION__ "(), Not implemented!\n");
		IRDA_DEBUG(1, __FUNCTION__ 
		      "(), event=%s, ns_status=%d, nr_status=%d\n", 
		      irlap_event[ event], ns_status, nr_status);
		break;
	case RECV_UI_FRAME:
		/* Poll bit cleared? */
		if (!info->pf) {
			irlap_data_indication(self, skb, TRUE);
			irlap_next_state(self, LAP_NRM_P);
		} else {
			del_timer(&self->final_timer);
			irlap_data_indication(self, skb, TRUE);
			printk(__FUNCTION__ "(): RECV_UI_FRAME: next state %s\n", irlap_state[self->state]);
			irlap_start_poll_timer(self, self->poll_timeout);
		}
		break;
	case RECV_RR_RSP:
		/*  
		 *  If you get a RR, the remote isn't busy anymore, 
		 *  no matter what the NR 
		 */
		self->remote_busy = FALSE;

		/* 
		 *  Nr as expected? 
		 */
		ret = irlap_validate_nr_received(self, info->nr);
		if (ret == NR_EXPECTED) {	
			/* Stop final timer */
			del_timer(&self->final_timer);
			
			/* Update Nr received */
			irlap_update_nr_received(self, info->nr);
			
			/*
			 *  Got expected NR, so reset the retry_count. This 
			 *  is not done by the IrLAP standard , which is 
			 *  strange! DB.
			 */
			self->retry_count = 0;			
			irlap_wait_min_turn_around(self, &self->qos_tx);

			irlap_next_state(self, LAP_XMIT_P);

			/* Start poll timer */
			irlap_start_poll_timer(self, self->poll_timeout);
		} else if (ret == NR_UNEXPECTED) {
			ASSERT(info != NULL, return -1;);	
			/* 
			 *  Unexpected nr! 
			 */
			
			/* Update Nr received */
			irlap_update_nr_received(self, info->nr);

			IRDA_DEBUG(4, "RECV_RR_FRAME: Retrans:%d, nr=%d, va=%d, "
			      "vs=%d, vr=%d\n",
			      self->retry_count, info->nr, self->va, 
			      self->vs, self->vr);
			
			/* Resend rejected frames */
			irlap_resend_rejected_frames(self, CMD_FRAME);
			
			irlap_next_state(self, LAP_NRM_P);
		} else if (ret == NR_INVALID) {
			IRDA_DEBUG(1, __FUNCTION__ "(), Received RR with "
				   "invalid nr !\n");
			del_timer(&self->final_timer);

			irlap_next_state(self, LAP_RESET_WAIT);

			irlap_disconnect_indication(self, LAP_RESET_INDICATION);
			self->xmitflag = TRUE;
		}
		break;
	case RECV_RNR_RSP:
		ASSERT(info != NULL, return -1;);

		/* Stop final timer */
		del_timer(&self->final_timer);
		self->remote_busy = TRUE;

		/* Update Nr received */
		irlap_update_nr_received(self, info->nr);
		irlap_next_state(self, LAP_XMIT_P);
			
		/* Start poll timer */
		irlap_start_poll_timer(self, self->poll_timeout);
		break;
	case RECV_FRMR_RSP:
		del_timer(&self->final_timer);
		self->xmitflag = TRUE;
		irlap_next_state(self, LAP_RESET_WAIT);
		irlap_reset_indication(self);
		break;
	case FINAL_TIMER_EXPIRED:
		/* 
		 *  We are allowed to wait for additional 300 ms if
		 *  final timer expires when we are in the middle
		 *  of receiving a frame (page 45, IrLAP). Check that
		 *  we only do this once for each frame.
		 */
		if (irda_device_is_receiving(self->netdev) && !self->add_wait) {
			IRDA_DEBUG(1, "FINAL_TIMER_EXPIRED when receiving a "
			      "frame! Waiting a little bit more!\n");
			irlap_start_final_timer(self, MSECS_TO_JIFFIES(300));

			/*
			 *  Don't allow this to happen one more time in a row, 
			 *  or else we can get a pretty tight loop here if 
			 *  if we only receive half a frame. DB.
			 */
			self->add_wait = TRUE;
			break;
		}
		self->add_wait = FALSE;

		if ((self->retry_count < self->N2) && 
		    (self->retry_count != self->N1)) {
			
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_rr_frame(self, CMD_FRAME);
			
			irlap_start_final_timer(self, self->final_timeout);
		 	self->retry_count++;

			IRDA_DEBUG(4, "irlap_state_nrm_p: FINAL_TIMER_EXPIRED:"
				   " retry_count=%d\n", self->retry_count);
			/* Keep state */
		} else if (self->retry_count == self->N1) {
			irlap_status_indication(self, STATUS_NO_ACTIVITY);
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_rr_frame(self, CMD_FRAME);
			
			irlap_start_final_timer(self, self->final_timeout);
			self->retry_count++;

			IRDA_DEBUG(4, "retry count = N1; retry_count=%d\n", 
				   self->retry_count);
			/* Keep state */
		} else if (self->retry_count >= self->N2) {
			irlap_apply_default_connection_parameters(self);

			/* Always switch state before calling upper layers */
			irlap_next_state(self, LAP_NDM);
			irlap_disconnect_indication(self, LAP_NO_RESPONSE);
		}
		break;
	case RECV_REJ_RSP:
		irlap_update_nr_received(self, info->nr);
		if (self->remote_busy) {
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_rr_frame(self, CMD_FRAME);
		} else
			irlap_resend_rejected_frames(self, CMD_FRAME);
		irlap_start_final_timer(self, self->final_timeout);
		break;
	case RECV_SREJ_RSP:
		irlap_update_nr_received(self, info->nr);
		if (self->remote_busy) {
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_rr_frame(self, CMD_FRAME);
		} else
			irlap_resend_rejected_frame(self, CMD_FRAME);
		irlap_start_final_timer(self, self->final_timeout);
		break;
	case RECV_RD_RSP:
		IRDA_DEBUG(0, __FUNCTION__ "(), RECV_RD_RSP\n");

		irlap_next_state(self, LAP_PCLOSE);
		irlap_send_disc_frame(self);
		irlap_flush_all_queues(self);
		irlap_start_final_timer(self, self->final_timeout);
		self->retry_count = 0;
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %s\n", 
			   irlap_event[event]);

		ret = -1;
		break;
	}
	return ret;
}

/*
 * Function irlap_state_reset_wait (event, skb, info)
 *
 *    We have informed the service user of a reset condition, and is
 *    awaiting reset of disconnect request.
 *
 */
static int irlap_state_reset_wait(struct irlap_cb *self, IRLAP_EVENT event, 
				  struct sk_buff *skb, struct irlap_info *info)
{
	int ret = 0;
	
	IRDA_DEBUG(3, __FUNCTION__ "(), event = %s\n", irlap_event[event]);
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);
	
	switch (event) {
	case RESET_REQUEST:
		if (self->xmitflag) {
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_snrm_frame(self, NULL);
			irlap_start_final_timer(self, self->final_timeout);
			irlap_next_state(self, LAP_RESET);
		} else {
			irlap_start_final_timer(self, self->final_timeout);
			irlap_next_state(self, LAP_RESET);
		}
		break;
	case DISCONNECT_REQUEST:
		irlap_wait_min_turn_around( self, &self->qos_tx);
		irlap_send_disc_frame( self);
		irlap_flush_all_queues( self);
		irlap_start_final_timer( self, self->final_timeout);
		self->retry_count = 0;
		irlap_next_state( self, LAP_PCLOSE);
		break;
	default:
		IRDA_DEBUG(2, __FUNCTION__ "(), Unknown event %s\n", 
			   irlap_event[event]);

		ret = -1;
		break;	
	}
	return ret;
}

/*
 * Function irlap_state_reset (self, event, skb, info)
 *
 *    We have sent a SNRM reset command to the peer layer, and is awaiting
 *    reply.
 *
 */
static int irlap_state_reset(struct irlap_cb *self, IRLAP_EVENT event, 
			     struct sk_buff *skb, struct irlap_info *info)
{
	int ret = 0;
	
	IRDA_DEBUG(3, __FUNCTION__ "(), event = %s\n", irlap_event[event]);
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);
	
	switch (event) {
	case RECV_DISC_CMD:
		del_timer(&self->final_timer);

		irlap_apply_default_connection_parameters(self);

		/* Always switch state before calling upper layers */
		irlap_next_state(self, LAP_NDM);

		irlap_disconnect_indication(self, LAP_NO_RESPONSE);

		break;
	case RECV_UA_RSP:
		del_timer(&self->final_timer);
		
		/* Initiate connection state */
		irlap_initiate_connection_state(self);
		
		irlap_reset_confirm();
		
		self->remote_busy = FALSE;

		irlap_next_state(self, LAP_XMIT_P);

		irlap_start_poll_timer(self, self->poll_timeout);

		break;
	case FINAL_TIMER_EXPIRED:
		if (self->retry_count < 3) {
			irlap_wait_min_turn_around(self, &self->qos_tx);

			ASSERT(self->netdev != NULL, return -1;);
			irlap_send_snrm_frame(self, self->qos_dev);

			self->retry_count++; /* Experimental!! */

			irlap_start_final_timer(self, self->final_timeout);
			irlap_next_state(self, LAP_RESET);
		} else if (self->retry_count >= self->N3) {
			irlap_apply_default_connection_parameters(self);
			
			/* Always switch state before calling upper layers */
			irlap_next_state(self, LAP_NDM);
			
			irlap_disconnect_indication(self, LAP_NO_RESPONSE);
		}
		break;
	case RECV_SNRM_CMD:
		/* 
		 * SNRM frame is not allowed to contain an I-field in this 
		 * state
		 */
		if (!info) {
			IRDA_DEBUG(3, __FUNCTION__ "(), RECV_SNRM_CMD\n");
			irlap_initiate_connection_state(self);
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_ua_response_frame(self, &self->qos_rx);
			irlap_reset_confirm();
			irlap_start_wd_timer(self, self->wd_timeout);
			irlap_next_state(self, LAP_NDM);
		} else {
			IRDA_DEBUG(0, __FUNCTION__ 
				   "(), SNRM frame contained an I field!\n");
		}
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %s\n", 
			   irlap_event[event]);	

		ret = -1;
		break;	
	}
	return ret;
}

/*
 * Function irlap_state_xmit_s (event, skb, info)
 * 
 *   XMIT_S, The secondary station has been given the right to transmit,
 *   and we therefor do not expect to receive any transmissions from other
 *   stations.  
 */
static int irlap_state_xmit_s(struct irlap_cb *self, IRLAP_EVENT event, 
			      struct sk_buff *skb, struct irlap_info *info) 
{
	int ret = 0;
	
	IRDA_DEBUG(4, __FUNCTION__ "(), event=%s\n", irlap_event[event]); 

	ASSERT(self != NULL, return -ENODEV;);
	ASSERT(self->magic == LAP_MAGIC, return -EBADR;);
	
	switch (event) {
	case SEND_I_CMD:
		/*
		 *  Send frame only if send window > 1
		 */ 
		if ((self->window > 0) && (!self->remote_busy)) {
#ifdef CONFIG_IRDA_DYNAMIC_WINDOW
			/*
			 *  Test if we have transmitted more bytes over the 
			 *  link than its possible to do with the current 
			 *  speed and turn-around-time.
			 */
			if (skb->len > self->bytes_left) {
				skb_queue_head(&self->txq, skb_get(skb));
				/*
				 *  Switch to NRM_S, this is only possible
				 *  when we are in secondary mode, since we 
				 *  must be sure that we don't miss any RR
				 *  frames
				 */
				irlap_next_state(self, LAP_NRM_S);

				return -EPROTO; /* Try again later */
			}
			self->bytes_left -= skb->len;
#endif /* CONFIG_IRDA_DYNAMIC_WINDOW */
			/*
			 *  Send data with final bit cleared only if window > 1
			 *  and there is more frames to be sent
			 */
			if ((self->window > 1) && 
			    skb_queue_len(&self->txq) > 0) 
			{   
				irlap_send_data_secondary(self, skb);
				irlap_next_state(self, LAP_XMIT_S);
			} else {
				irlap_send_data_secondary_final(self, skb);
				irlap_next_state(self, LAP_NRM_S);

				/* 
				 * Make sure state machine does not try to send
				 * any more frames 
				 */
				ret = -EPROTO;
			}
		} else {
			IRDA_DEBUG(2, __FUNCTION__ "(), Unable to send!\n");
			skb_queue_head(&self->txq, skb_get(skb));
			ret = -EPROTO;
		}
		break;
	case DISCONNECT_REQUEST:
		irlap_send_rd_frame(self);
		irlap_flush_all_queues(self);
		irlap_start_wd_timer(self, self->wd_timeout);
		irlap_next_state(self, LAP_SCLOSE);
		break;
	default:
		IRDA_DEBUG(2, __FUNCTION__ "(), Unknown event %s\n", 
			   irlap_event[event]);

		ret = -EINVAL;
		break;
	}
	return ret;
}

/*
 * Function irlap_state_nrm_s (event, skb, info)
 *
 *    NRM_S (Normal Response Mode as Secondary) state, in this state we are 
 *    expecting to receive frames from the primary station
 *
 */
static int irlap_state_nrm_s(struct irlap_cb *self, IRLAP_EVENT event, 
			     struct sk_buff *skb, struct irlap_info *info) 
{
	int ns_status;
	int nr_status;
	int ret = 0;

	IRDA_DEBUG(4, __FUNCTION__ "(), event=%s\n", irlap_event[ event]);

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LAP_MAGIC, return -1;);

	switch (event) {
	case RECV_I_CMD: /* Optimize for the common case */
		/* FIXME: must check for remote_busy below */
		IRDA_DEBUG(4, __FUNCTION__ "(), event=%s nr=%d, vs=%d, ns=%d, "
			   "vr=%d, pf=%d\n", irlap_event[event], info->nr, 
			   self->vs, info->ns, self->vr, info->pf);

		self->retry_count = 0;

		ns_status = irlap_validate_ns_received(self, info->ns);
		nr_status = irlap_validate_nr_received(self, info->nr);
		/* 
		 *  Check for expected I(nformation) frame
		 */
		if ((ns_status == NS_EXPECTED) && (nr_status == NR_EXPECTED)) {
			/* 
			 *  poll bit cleared?
			 */
			if (!info->pf) {
				self->vr = (self->vr + 1) % 8;
				
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
				
				self->ack_required = TRUE;
				
				/*
				 *  Starting WD-timer here is optional, but
				 *  not recommended. Note 6 IrLAP p. 83
				 */
#if 0
				irda_start_timer(WD_TIMER, self->wd_timeout);
#endif
				/* Keep state, do not move this line */
				irlap_next_state(self, LAP_NRM_S);

				irlap_data_indication(self, skb, FALSE);
				break;
			} else {
				self->vr = (self->vr + 1) % 8;
				
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
				
				/* 
				 *  We should wait before sending RR, and
				 *  also before changing to XMIT_S
				 *  state. (note 1, IrLAP p. 82) 
				 */
				irlap_wait_min_turn_around(self, &self->qos_tx);

				/*  
				 * Give higher layers a chance to
				 * immediately reply with some data before
				 * we decide if we should send a RR frame
				 * or not
				 */
				irlap_data_indication(self, skb, FALSE);

				/* Any pending data requests?  */
				if ((skb_queue_len(&self->txq) > 0) && 
				    (self->window > 0)) 
				{
					self->ack_required = TRUE;
					
					del_timer(&self->wd_timer);
					
					irlap_next_state(self, LAP_XMIT_S);
				} else {
					irlap_send_rr_frame(self, RSP_FRAME);
					irlap_start_wd_timer(self, 
							     self->wd_timeout);

					/* Keep the state */
					irlap_next_state(self, LAP_NRM_S);
				}
				break;
			}
		}
		/*
		 *  Check for Unexpected next to send (Ns)
		 */
		if ((ns_status == NS_UNEXPECTED) && (nr_status == NR_EXPECTED))
		{
			/* Unexpected next to send, with final bit cleared */
			if (!info->pf) {
				irlap_update_nr_received(self, info->nr);
				
				irlap_start_wd_timer(self, self->wd_timeout);
			} else {
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
			
				irlap_wait_min_turn_around(self, &self->qos_tx);
				irlap_send_rr_frame(self, CMD_FRAME);
			
				irlap_start_wd_timer(self, self->wd_timeout);
			}
			break;
		}

		/* 
		 *  Unexpected Next to Receive(NR) ?
		 */
		if ((ns_status == NS_EXPECTED) && (nr_status == NR_UNEXPECTED))
		{
			if (info->pf) {
				IRDA_DEBUG(4, "RECV_I_RSP: frame(s) lost\n");
				
				self->vr = (self->vr + 1) % 8;
				
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
				
				/* Resend rejected frames */
				irlap_resend_rejected_frames(self, RSP_FRAME);

				/* Keep state, do not move this line */
				irlap_next_state(self, LAP_NRM_S);

				irlap_data_indication(self, skb, FALSE);
				irlap_start_wd_timer(self, self->wd_timeout);
				break;
			}
			/*
			 *  This is not documented in IrLAP!! Unexpected NR
			 *  with poll bit cleared
			 */
			if (!info->pf) {
				self->vr = (self->vr + 1) % 8;
				
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
				
				/* Keep state, do not move this line */
				irlap_next_state(self, LAP_NRM_S);

				irlap_data_indication(self, skb, FALSE);
				irlap_start_wd_timer(self, self->wd_timeout);
			}
			break;
		}
		
		if (ret == NR_INVALID) {
			IRDA_DEBUG(0, "NRM_S, NR_INVALID not implemented!\n");
		}
		if (ret == NS_INVALID) {
			IRDA_DEBUG(0, "NRM_S, NS_INVALID not implemented!\n");
		}
		break;
	case RECV_UI_FRAME:
		/* 
		 *  poll bit cleared?
		 */
		if (!info->pf) {
			irlap_data_indication(self, skb, TRUE);
			irlap_next_state(self, LAP_NRM_S); /* Keep state */
		} else {
			/*
			 *  Any pending data requests?
			 */
			if ((skb_queue_len(&self->txq) > 0) && 
			    (self->window > 0) && !self->remote_busy) 
			{
				irlap_data_indication(self, skb, TRUE);
				
				del_timer(&self->wd_timer);

				irlap_next_state(self, LAP_XMIT_S);
			} else {
				irlap_data_indication(self, skb, TRUE);

				irlap_wait_min_turn_around(self, &self->qos_tx);

				irlap_send_rr_frame(self, RSP_FRAME);
				self->ack_required = FALSE;
				
				irlap_start_wd_timer(self, self->wd_timeout);

				/* Keep the state */
				irlap_next_state(self, LAP_NRM_S);
			}
		}
		break;
	case RECV_RR_CMD:
		self->retry_count = 0;

		/* 
		 *  Nr as expected? 
		 */
		nr_status = irlap_validate_nr_received(self, info->nr);
		if (nr_status == NR_EXPECTED) {
			if ((skb_queue_len( &self->txq) > 0) && 
			    (self->window > 0)) {
				self->remote_busy = FALSE;
				
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
				del_timer(&self->wd_timer);
				
				irlap_wait_min_turn_around(self, &self->qos_tx);
				irlap_next_state(self, LAP_XMIT_S);
			} else {			
				self->remote_busy = FALSE;
				/* Update Nr received */
				irlap_update_nr_received(self, info->nr);
				irlap_wait_min_turn_around(self, &self->qos_tx);
				
				irlap_send_rr_frame(self, RSP_FRAME);
				
				irlap_start_wd_timer(self, self->wd_timeout);
				irlap_next_state(self, LAP_NRM_S);
			}
		} else if (nr_status == NR_UNEXPECTED) {
			self->remote_busy = FALSE;
			irlap_update_nr_received(self, info->nr);
			irlap_resend_rejected_frames(self, RSP_FRAME);

			irlap_start_wd_timer(self, self->wd_timeout);

			/* Keep state */
			irlap_next_state(self, LAP_NRM_S); 
		} else {
			IRDA_DEBUG(1, __FUNCTION__ 
				   "(), invalid nr not implemented!\n");
		} 
		break;
	case RECV_SNRM_CMD:
		/* SNRM frame is not allowed to contain an I-field */
		if (!info) {
			del_timer(&self->wd_timer);
			IRDA_DEBUG(1, __FUNCTION__ "(), received SNRM cmd\n");
			irlap_next_state(self, LAP_RESET_CHECK);
			
			irlap_reset_indication(self);
		} else {
			IRDA_DEBUG(0, __FUNCTION__ 
				   "(), SNRM frame contained an I-field!\n");
			
		}
		break;
	case RECV_REJ_CMD:
		irlap_update_nr_received(self, info->nr);
		if (self->remote_busy) {
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_rr_frame(self, CMD_FRAME);
		} else
			irlap_resend_rejected_frames(self, CMD_FRAME);
		irlap_start_wd_timer(self, self->wd_timeout);
		break;
	case RECV_SREJ_CMD:
		irlap_update_nr_received(self, info->nr);
		if (self->remote_busy) {
			irlap_wait_min_turn_around(self, &self->qos_tx);
			irlap_send_rr_frame(self, CMD_FRAME);
		} else
			irlap_resend_rejected_frame(self, CMD_FRAME);
		irlap_start_wd_timer(self, self->wd_timeout);
		break;
	case WD_TIMER_EXPIRED:
		/*
		 *  Wait until retry_count * n matches negotiated threshold/
		 *  disconnect time (note 2 in IrLAP p. 82)
		 *
		 * Note : self->wd_timeout = (self->poll_timeout * 2),
		 *   and self->final_timeout == self->poll_timeout,
		 *   which explain why we use (self->retry_count * 2) here !!!
		 * Jean II
		 */
		IRDA_DEBUG(1, __FUNCTION__ "(), retry_count = %d\n", 
			   self->retry_count);

		if (((self->retry_count * 2) < self->N2)  && 
		    ((self->retry_count * 2) != self->N1)) {
			
			irlap_start_wd_timer(self, self->wd_timeout);
			self->retry_count++;
		} else if ((self->retry_count * 2) == self->N1) {
			irlap_status_indication(self, STATUS_NO_ACTIVITY);
			irlap_start_wd_timer(self, self->wd_timeout);
			self->retry_count++;
		} else if ((self->retry_count * 2) >= self->N2) {
			irlap_apply_default_connection_parameters(self);
			
			/* Always switch state before calling upper layers */
			irlap_next_state(self, LAP_NDM);
			irlap_disconnect_indication(self, LAP_NO_RESPONSE);
		}
		break;
	case RECV_DISC_CMD:
		/* Always switch state before calling upper layers */
		irlap_next_state(self, LAP_NDM);

		irlap_wait_min_turn_around(self, &self->qos_tx);
		irlap_send_ua_response_frame(self, NULL);
		del_timer(&self->wd_timer);
		irlap_flush_all_queues(self);
		irlap_apply_default_connection_parameters(self);

		irlap_disconnect_indication(self, LAP_DISC_INDICATION);
		break;
	case RECV_DISCOVERY_XID_CMD:
		irlap_wait_min_turn_around(self, &self->qos_tx);
		irlap_send_rr_frame(self, RSP_FRAME);
		self->ack_required = TRUE;
		irlap_start_wd_timer(self, self->wd_timeout);
		irlap_next_state(self, LAP_NRM_S);

		break;
	case RECV_TEST_CMD:
		/* Remove test frame header (only LAP header in NRM) */
		skb_pull(skb, LAP_ADDR_HEADER + LAP_CTRL_HEADER);

		irlap_wait_min_turn_around(self, &self->qos_tx);
		irlap_start_wd_timer(self, self->wd_timeout);

		/* Send response (info will be copied) */
		irlap_send_test_frame(self, self->caddr, info->daddr, skb);
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %d, (%s)\n", 
			   event, irlap_event[event]);

		ret = -EINVAL;
		break;
	}
	return ret;
}

/*
 * Function irlap_state_sclose (self, event, skb, info)
 *
 *    
 *
 */
static int irlap_state_sclose(struct irlap_cb *self, IRLAP_EVENT event, 
			      struct sk_buff *skb, struct irlap_info *info) 
{
	int ret = 0;

	IRDA_DEBUG(0, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -ENODEV;);
	ASSERT(self->magic == LAP_MAGIC, return -EBADR;);
	
	switch (event) {
	case RECV_DISC_CMD:
		/* Always switch state before calling upper layers */
		irlap_next_state(self, LAP_NDM);
		
		irlap_wait_min_turn_around(self, &self->qos_tx);
		irlap_send_ua_response_frame(self, NULL);
		del_timer(&self->wd_timer);
		irlap_apply_default_connection_parameters(self);

		irlap_disconnect_indication(self, LAP_DISC_INDICATION);
		break;
	case RECV_DM_RSP:
		/* Always switch state before calling upper layers */
		irlap_next_state(self, LAP_NDM);

		del_timer(&self->wd_timer);
		irlap_apply_default_connection_parameters(self);
		
		irlap_disconnect_indication(self, LAP_DISC_INDICATION);
		break;
	case WD_TIMER_EXPIRED:
		irlap_apply_default_connection_parameters(self);
		
		irlap_disconnect_indication(self, LAP_DISC_INDICATION);
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %d, (%s)\n", 
			   event, irlap_event[event]);

		ret = -EINVAL;
		break;
	}

	return -1;
}

static int irlap_state_reset_check( struct irlap_cb *self, IRLAP_EVENT event, 
				   struct sk_buff *skb, 
				   struct irlap_info *info) 
{
	int ret = 0;

	IRDA_DEBUG(1, __FUNCTION__ "(), event=%s\n", irlap_event[event]); 

	ASSERT(self != NULL, return -ENODEV;);
	ASSERT(self->magic == LAP_MAGIC, return -EBADR;);
	
	switch (event) {
	case RESET_RESPONSE:
		irlap_send_ua_response_frame(self, &self->qos_rx);
		irlap_initiate_connection_state(self);
		irlap_start_wd_timer(self, WD_TIMEOUT);
		irlap_flush_all_queues(self);
		
		irlap_next_state(self, LAP_NRM_S);
		break;
	case DISCONNECT_REQUEST:
		irlap_wait_min_turn_around(self, &self->qos_tx);
		irlap_send_rd_frame(self);
		irlap_start_wd_timer(self, WD_TIMEOUT);
		irlap_next_state(self, LAP_SCLOSE);
		break;
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), Unknown event %d, (%s)\n", 
			   event, irlap_event[event]);

		ret = -EINVAL;
		break;
	}
	return ret;
}
