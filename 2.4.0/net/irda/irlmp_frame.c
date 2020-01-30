/*********************************************************************
 *                
 * Filename:      irlmp_frame.c
 * Version:       0.9
 * Description:   IrLMP frame implementation
 * Status:        Experimental.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Tue Aug 19 02:09:59 1997
 * Modified at:   Mon Dec 13 13:41:12 1999
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * 
 *     Copyright (c) 1998-1999 Dag Brattli <dagb@cs.uit.no>
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
#include <linux/skbuff.h>
#include <linux/kernel.h>

#include <net/irda/irda.h>
#include <net/irda/irlap.h>
#include <net/irda/timer.h>
#include <net/irda/irlmp.h>
#include <net/irda/irlmp_frame.h>
#include <net/irda/discovery.h>

#define	DISCO_SMALL_DELAY	250	/* Delay for some discoveries in ms */
struct timer_list disco_delay;		/* The timer associated */

static struct lsap_cb *irlmp_find_lsap(struct lap_cb *self, __u8 dlsap, 
				       __u8 slsap, int status, hashbin_t *);

inline void irlmp_send_data_pdu(struct lap_cb *self, __u8 dlsap, __u8 slsap,
				int expedited, struct sk_buff *skb)
{
	skb->data[0] = dlsap;
	skb->data[1] = slsap;

	if (expedited) {
		IRDA_DEBUG(4, __FUNCTION__ "(), sending expedited data\n");
		irlap_data_request(self->irlap, skb, TRUE);
	} else
		irlap_data_request(self->irlap, skb, FALSE);
}

/*
 * Function irlmp_send_lcf_pdu (dlsap, slsap, opcode,skb)
 *
 *    Send Link Control Frame to IrLAP
 */
void irlmp_send_lcf_pdu(struct lap_cb *self, __u8 dlsap, __u8 slsap,
			__u8 opcode, struct sk_buff *skb) 
{
	__u8 *frame;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LAP_MAGIC, return;);
	ASSERT(skb != NULL, return;);
	
	frame = skb->data;
	
	frame[0] = dlsap | CONTROL_BIT;
	frame[1] = slsap;

	frame[2] = opcode;

	if (opcode == DISCONNECT)
		frame[3] = 0x01; /* Service user request */
	else
		frame[3] = 0x00; /* rsvd */

	irlap_data_request(self->irlap, skb, FALSE);
}

/*
 * Function irlmp_input (skb)
 *
 *    Used by IrLAP to pass received data frames to IrLMP layer
 *
 */
void irlmp_link_data_indication(struct lap_cb *self, struct sk_buff *skb, 
				int unreliable)
{
	struct lsap_cb *lsap;
	__u8   slsap_sel;   /* Source (this) LSAP address */
	__u8   dlsap_sel;   /* Destination LSAP address */
	__u8   *fp;
	
	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LAP_MAGIC, return;);
	ASSERT(skb->len > 2, return;);

	fp = skb->data;

	/*
	 *  The next statements may be confusing, but we do this so that 
	 *  destination LSAP of received frame is source LSAP in our view
	 */
	slsap_sel = fp[0] & LSAP_MASK; 
	dlsap_sel = fp[1];	

	/*
	 *  Check if this is an incoming connection, since we must deal with
	 *  it in a different way than other established connections.
	 */
	if ((fp[0] & CONTROL_BIT) && (fp[2] == CONNECT_CMD)) {
		IRDA_DEBUG(3, __FUNCTION__ "(), incoming connection, "
			   "source LSAP=%d, dest LSAP=%d\n",
			   slsap_sel, dlsap_sel);
		
		/* Try to find LSAP among the unconnected LSAPs */
		lsap = irlmp_find_lsap(self, dlsap_sel, slsap_sel, CONNECT_CMD,
				       irlmp->unconnected_lsaps);
		
		/* Maybe LSAP was already connected, so try one more time */
		if (!lsap) {
			IRDA_DEBUG(1, __FUNCTION__ "(), incoming connection for LSAP already connected\n");
			lsap = irlmp_find_lsap(self, dlsap_sel, slsap_sel, 0,
					       self->lsaps);
		}
	} else
		lsap = irlmp_find_lsap(self, dlsap_sel, slsap_sel, 0, 
				       self->lsaps);
	
	if (lsap == NULL) {
		IRDA_DEBUG(2, "IrLMP, Sorry, no LSAP for received frame!\n");
		IRDA_DEBUG(2, __FUNCTION__ 
		      "(), slsap_sel = %02x, dlsap_sel = %02x\n", slsap_sel, 
		      dlsap_sel);
		if (fp[0] & CONTROL_BIT) {
			IRDA_DEBUG(2, __FUNCTION__ 
			      "(), received control frame %02x\n", fp[2]);
		} else {
			IRDA_DEBUG(2, __FUNCTION__ "(), received data frame\n");
		}
		dev_kfree_skb(skb);
		return;
	}

	/* 
	 *  Check if we received a control frame? 
	 */
	if (fp[0] & CONTROL_BIT) {
		switch (fp[2]) {
		case CONNECT_CMD:
			lsap->lap = self;
			irlmp_do_lsap_event(lsap, LM_CONNECT_INDICATION, skb);
			break;
		case CONNECT_CNF:
			irlmp_do_lsap_event(lsap, LM_CONNECT_CONFIRM, skb);
			break;
		case DISCONNECT:
			IRDA_DEBUG(4, __FUNCTION__ 
				   "(), Disconnect indication!\n");
			irlmp_do_lsap_event(lsap, LM_DISCONNECT_INDICATION, 
					    skb);
			break;
		case ACCESSMODE_CMD:
			IRDA_DEBUG(0, "Access mode cmd not implemented!\n");
			dev_kfree_skb(skb);
			break;
		case ACCESSMODE_CNF:
			IRDA_DEBUG(0, "Access mode cnf not implemented!\n");
			dev_kfree_skb(skb);
			break;
		default:
			IRDA_DEBUG(0, __FUNCTION__ 
				   "(), Unknown control frame %02x\n", fp[2]);
			dev_kfree_skb(skb);
			break;
		}
	} else if (unreliable) {
		/* Optimize and bypass the state machine if possible */
		if (lsap->lsap_state == LSAP_DATA_TRANSFER_READY)
			irlmp_udata_indication(lsap, skb);
		else
			irlmp_do_lsap_event(lsap, LM_UDATA_INDICATION, skb);
	} else {	
		/* Optimize and bypass the state machine if possible */
		if (lsap->lsap_state == LSAP_DATA_TRANSFER_READY)
			irlmp_data_indication(lsap, skb);
		else
			irlmp_do_lsap_event(lsap, LM_DATA_INDICATION, skb);
	}
}

/*
 * Function irlmp_link_unitdata_indication (self, skb)
 *
 *    
 *
 */
#ifdef CONFIG_IRDA_ULTRA
void irlmp_link_unitdata_indication(struct lap_cb *self, struct sk_buff *skb)
{
	struct lsap_cb *lsap;
	__u8   slsap_sel;   /* Source (this) LSAP address */
	__u8   dlsap_sel;   /* Destination LSAP address */
	__u8   pid;         /* Protocol identifier */
	__u8   *fp;
	
	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LAP_MAGIC, return;);
	ASSERT(skb->len > 2, return;);

	fp = skb->data;

	/*
	 *  The next statements may be confusing, but we do this so that 
	 *  destination LSAP of received frame is source LSAP in our view
	 */
	slsap_sel = fp[0] & LSAP_MASK; 
	dlsap_sel = fp[1];
	pid       = fp[2];
	
	if (pid & 0x80) {
		IRDA_DEBUG(0, __FUNCTION__ "(), extension in PID not supp!\n");
		dev_kfree_skb(skb);

		return;
	}

	/* Check if frame is addressed to the connectionless LSAP */
	if ((slsap_sel != LSAP_CONNLESS) || (dlsap_sel != LSAP_CONNLESS)) {
		IRDA_DEBUG(0, __FUNCTION__ "(), dropping frame!\n");
		dev_kfree_skb(skb);
		
		return;
	}
	
	lsap = (struct lsap_cb *) hashbin_get_first(irlmp->unconnected_lsaps);
	while (lsap != NULL) {
		/*
		 *  Check if source LSAP and dest LSAP selectors and PID match.
		 */
		if ((lsap->slsap_sel == slsap_sel) && 
		    (lsap->dlsap_sel == dlsap_sel) && 
		    (lsap->pid == pid)) 
		{			
			break;
		}
		lsap = (struct lsap_cb *) hashbin_get_next(irlmp->unconnected_lsaps);
	}
	if (lsap)
		irlmp_connless_data_indication(lsap, skb);
	else {
		IRDA_DEBUG(0, __FUNCTION__ "(), found no matching LSAP!\n");
		dev_kfree_skb(skb);
	}
}
#endif /* CONFIG_IRDA_ULTRA */

/*
 * Function irlmp_link_disconnect_indication (reason, userdata)
 *
 *    IrLAP has disconnected 
 *
 */
void irlmp_link_disconnect_indication(struct lap_cb *lap, 
				      struct irlap_cb *irlap, 
				      LAP_REASON reason, 
				      struct sk_buff *userdata)
{
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(lap != NULL, return;);
	ASSERT(lap->magic == LMP_LAP_MAGIC, return;);

	lap->reason = reason;
	lap->daddr = DEV_ADDR_ANY;

        /* FIXME: must do something with the userdata if any */
	if (userdata)
		dev_kfree_skb(userdata);
	
	/*
	 *  Inform station state machine
	 */
	irlmp_do_lap_event(lap, LM_LAP_DISCONNECT_INDICATION, NULL);
}

/*
 * Function irlmp_link_connect_indication (qos)
 *
 *    Incoming LAP connection!
 *
 */
void irlmp_link_connect_indication(struct lap_cb *self, __u32 saddr, 
				   __u32 daddr, struct qos_info *qos,
				   struct sk_buff *skb) 
{
	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	/* Copy QoS settings for this session */
	self->qos = qos;

	/* Update destination device address */
	self->daddr = daddr;
	ASSERT(self->saddr == saddr, return;);

	irlmp_do_lap_event(self, LM_LAP_CONNECT_INDICATION, skb);
}

/*
 * Function irlmp_link_connect_confirm (qos)
 *
 *    LAP connection confirmed!
 *
 */
void irlmp_link_connect_confirm(struct lap_cb *self, struct qos_info *qos, 
				struct sk_buff *userdata)
{
	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LAP_MAGIC, return;);
	ASSERT(qos != NULL, return;);

	/* Don't need use the userdata for now */
	if (userdata)
		dev_kfree_skb(userdata);

	/* Copy QoS settings for this session */
	self->qos = qos;

	irlmp_do_lap_event(self, LM_LAP_CONNECT_CONFIRM, NULL);
}

/*
 * Function irlmp_discovery_timeout (priv)
 *
 *    Create a discovery event to the state machine (called after a delay)
 *
 * Note : irlmp_do_lap_event will handle the very rare case where the LAP
 * is destroyed while we were sleeping.
 */
static void irlmp_discovery_timeout(u_long	priv)
{
	struct lap_cb *self;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = (struct lap_cb *) priv;
	ASSERT(self != NULL, return;);

	/* Just handle it the same way as a discovery confirm */
	irlmp_do_lap_event(self, LM_LAP_DISCOVERY_CONFIRM, NULL);
}

/*
 * Function irlmp_link_discovery_indication (self, log)
 *
 *    Device is discovering us
 *
 * It's not an answer to our own discoveries, just another device trying
 * to perform discovery, but we don't want to miss the opportunity
 * to exploit this information, because :
 *	o We may not actively perform discovery (just passive discovery)
 *	o This type of discovery is much more reliable. In some cases, it
 *	  seem that less than 50% of our discoveries get an answer, while
 *	  we always get ~100% of these.
 *	o Make faster discovery, statistically divide time of discovery
 *	  events by 2 (important for the latency aspect and user feel)
 * However, when both devices discover each other, they might attempt to
 * connect to each other, and it would create collisions on the medium.
 * The trick here is to defer the event by a little delay to avoid both
 * devices to jump in exactly at the same time...
 *
 * The delay is currently set to 0.25s, which leave enough time to perform
 * a connection and don't interfer with next discovery (the lowest discovery
 * period/timeout that may be set is 1s). The message triggering this
 * event was the last of the discovery, so the medium is now free...
 * Maybe more testing is needed to get the value right...
 */
void irlmp_link_discovery_indication(struct lap_cb *self, 
				     discovery_t *discovery)
{
	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LAP_MAGIC, return;);

	irlmp_add_discovery(irlmp->cachelog, discovery);
	
	/* If delay was activated, kill it! */
	if(timer_pending(&disco_delay))
		del_timer(&disco_delay);
	/* Set delay timer to expire in 0.25s. */
	disco_delay.expires = jiffies + (DISCO_SMALL_DELAY * HZ/1000);
	disco_delay.function = irlmp_discovery_timeout;
	disco_delay.data = (unsigned long) self;
	add_timer(&disco_delay);
}

/*
 * Function irlmp_link_discovery_confirm (self, log)
 *
 *    Called by IrLAP with a list of discoveries after the discovery
 *    request has been carried out. A NULL log is received if IrLAP
 *    was unable to carry out the discovery request
 *
 */
void irlmp_link_discovery_confirm(struct lap_cb *self, hashbin_t *log)
{
	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LAP_MAGIC, return;);
	
	irlmp_add_discovery_log(irlmp->cachelog, log);

	/* If discovery delay was activated, kill it! */
	if(timer_pending(&disco_delay))
		del_timer(&disco_delay);

	/* Propagate event to the state machine */
	irlmp_do_lap_event(self, LM_LAP_DISCOVERY_CONFIRM, NULL);
}

#ifdef CONFIG_IRDA_CACHE_LAST_LSAP
inline void irlmp_update_cache(struct lsap_cb *self)
{
	/* Update cache entry */
	irlmp->cache.dlsap_sel = self->dlsap_sel;
	irlmp->cache.slsap_sel = self->slsap_sel;
	irlmp->cache.lsap = self;
	irlmp->cache.valid = TRUE;
}
#endif

/*
 * Function irlmp_find_handle (self, dlsap_sel, slsap_sel, status, queue)
 *
 *    Find handle assosiated with destination and source LSAP
 *
 */
static struct lsap_cb *irlmp_find_lsap(struct lap_cb *self, __u8 dlsap_sel,
				       __u8 slsap_sel, int status,
				       hashbin_t *queue) 
{
	struct lsap_cb *lsap;
	
	/* 
	 *  Optimize for the common case. We assume that the last frame
	 *  received is in the same connection as the last one, so check in
	 *  cache first to avoid the linear search
	 */
#ifdef CONFIG_IRDA_CACHE_LAST_LSAP
	if ((irlmp->cache.valid) && 
	    (irlmp->cache.slsap_sel == slsap_sel) && 
	    (irlmp->cache.dlsap_sel == dlsap_sel)) 
	{
		return (irlmp->cache.lsap);
	}
#endif
	lsap = (struct lsap_cb *) hashbin_get_first(queue);
	while (lsap != NULL) {
		/* 
		 *  If this is an incomming connection, then the destination 
		 *  LSAP selector may have been specified as LM_ANY so that 
		 *  any client can connect. In that case we only need to check
		 *  if the source LSAP (in our view!) match!
		 */
		if ((status == CONNECT_CMD) && 
		    (lsap->slsap_sel == slsap_sel) &&      
		    (lsap->dlsap_sel == LSAP_ANY)) 
		{
			lsap->dlsap_sel = dlsap_sel;
			
#ifdef CONFIG_IRDA_CACHE_LAST_LSAP
			irlmp_update_cache(lsap);
#endif
			return lsap;
		}
		/*
		 *  Check if source LSAP and dest LSAP selectors match.
		 */
		if ((lsap->slsap_sel == slsap_sel) && 
		    (lsap->dlsap_sel == dlsap_sel)) 
		{
#ifdef CONFIG_IRDA_CACHE_LAST_LSAP
			irlmp_update_cache(lsap);
#endif
			return lsap;
		}
		lsap = (struct lsap_cb *) hashbin_get_next(queue);
	}

	/* Sorry not found! */
	return NULL;
}
