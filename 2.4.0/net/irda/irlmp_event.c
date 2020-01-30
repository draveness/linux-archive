/*********************************************************************
 *                
 * Filename:      irlmp_event.c
 * Version:       0.8
 * Description:   An IrDA LMP event driver for Linux
 * Status:        Experimental.
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Mon Aug  4 20:40:53 1997
 * Modified at:   Tue Dec 14 23:04:16 1999
 * Modified by:   Dag Brattli <dagb@cs.uit.no>
 * 
 *     Copyright (c) 1998-1999 Dag Brattli <dagb@cs.uit.no>, 
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
#include <linux/kernel.h>

#include <net/irda/irda.h>
#include <net/irda/timer.h>
#include <net/irda/irlap.h>
#include <net/irda/irlmp.h>
#include <net/irda/irlmp_frame.h>
#include <net/irda/irlmp_event.h>

const char *irlmp_state[] = {
	"LAP_STANDBY",
	"LAP_U_CONNECT",
	"LAP_ACTIVE",
};

const char *irlsap_state[] = {
	"LSAP_DISCONNECTED",
	"LSAP_CONNECT",
	"LSAP_CONNECT_PEND",
	"LSAP_DATA_TRANSFER_READY",
	"LSAP_SETUP",
	"LSAP_SETUP_PEND",
};

static const char *irlmp_event[] = {
	"LM_CONNECT_REQUEST",
 	"LM_CONNECT_CONFIRM",
	"LM_CONNECT_RESPONSE",
 	"LM_CONNECT_INDICATION", 	
	
	"LM_DISCONNECT_INDICATION",
	"LM_DISCONNECT_REQUEST",

 	"LM_DATA_REQUEST",
	"LM_UDATA_REQUEST",
 	"LM_DATA_INDICATION",
	"LM_UDATA_INDICATION",

	"LM_WATCHDOG_TIMEOUT",

	/* IrLAP events */
	"LM_LAP_CONNECT_REQUEST",
 	"LM_LAP_CONNECT_INDICATION", 
 	"LM_LAP_CONNECT_CONFIRM",
 	"LM_LAP_DISCONNECT_INDICATION", 
	"LM_LAP_DISCONNECT_REQUEST",
	"LM_LAP_DISCOVERY_REQUEST",
 	"LM_LAP_DISCOVERY_CONFIRM",
	"LM_LAP_IDLE_TIMEOUT",
};

/* LAP Connection control proto declarations */
static void irlmp_state_standby  (struct lap_cb *, IRLMP_EVENT, 
				  struct sk_buff *);
static void irlmp_state_u_connect(struct lap_cb *, IRLMP_EVENT, 
				  struct sk_buff *);
static void irlmp_state_active   (struct lap_cb *, IRLMP_EVENT, 
				  struct sk_buff *);

/* LSAP Connection control proto declarations */
static int irlmp_state_disconnected(struct lsap_cb *, IRLMP_EVENT, 
				    struct sk_buff *);
static int irlmp_state_connect     (struct lsap_cb *, IRLMP_EVENT, 
				    struct sk_buff *);
static int irlmp_state_connect_pend(struct lsap_cb *, IRLMP_EVENT,
				    struct sk_buff *);
static int irlmp_state_dtr         (struct lsap_cb *, IRLMP_EVENT, 
				    struct sk_buff *);
static int irlmp_state_setup       (struct lsap_cb *, IRLMP_EVENT, 
				    struct sk_buff *);
static int irlmp_state_setup_pend  (struct lsap_cb *, IRLMP_EVENT, 
				    struct sk_buff *);

static void (*lap_state[]) (struct lap_cb *, IRLMP_EVENT, struct sk_buff *) =
{
	irlmp_state_standby,
	irlmp_state_u_connect,
	irlmp_state_active,
};

static int (*lsap_state[])( struct lsap_cb *, IRLMP_EVENT, struct sk_buff *) =
{
	irlmp_state_disconnected,
	irlmp_state_connect,
	irlmp_state_connect_pend,
	irlmp_state_dtr,
	irlmp_state_setup,
	irlmp_state_setup_pend
};

/* Do connection control events */
int irlmp_do_lsap_event(struct lsap_cb *self, IRLMP_EVENT event, 
			struct sk_buff *skb)
{
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LMP_LSAP_MAGIC, return -1;);

	IRDA_DEBUG(4, __FUNCTION__ "(), EVENT = %s, STATE = %s\n",
		   irlmp_event[event], irlsap_state[ self->lsap_state]);

	return (*lsap_state[self->lsap_state]) (self, event, skb);
}

/*
 * Function do_lap_event (event, skb, info)
 *
 *    Do IrLAP control events
 *
 */
void irlmp_do_lap_event(struct lap_cb *self, IRLMP_EVENT event, 
			struct sk_buff *skb) 
{
	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LAP_MAGIC, return;);
	
	IRDA_DEBUG(4, __FUNCTION__ "(), EVENT = %s, STATE = %s\n",
		   irlmp_event[event], 
		   irlmp_state[self->lap_state]);

	(*lap_state[self->lap_state]) (self, event, skb);
}

void irlmp_discovery_timer_expired(void *data)
{
	IRDA_DEBUG(4, __FUNCTION__ "()\n");
	
	if (sysctl_discovery)
		irlmp_do_discovery(sysctl_discovery_slots);

	/* Restart timer */
	irlmp_start_discovery_timer(irlmp, sysctl_discovery_timeout * HZ);
}

void irlmp_watchdog_timer_expired(void *data)
{
	struct lsap_cb *self = (struct lsap_cb *) data;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LSAP_MAGIC, return;);

	irlmp_do_lsap_event(self, LM_WATCHDOG_TIMEOUT, NULL);
}

void irlmp_idle_timer_expired(void *data)
{
	struct lap_cb *self = (struct lap_cb *) data;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);
	ASSERT(self->magic == LMP_LAP_MAGIC, return;);

	irlmp_do_lap_event(self, LM_LAP_IDLE_TIMEOUT, NULL);
}

/*********************************************************************
 *
 *    LAP connection control states
 *
 ********************************************************************/

/*
 * Function irlmp_state_standby (event, skb, info)
 *
 *    STANDBY, The IrLAP connection does not exist.
 *
 */
static void irlmp_state_standby(struct lap_cb *self, IRLMP_EVENT event, 
				struct sk_buff *skb)
{	
	IRDA_DEBUG(4, __FUNCTION__ "()\n"); 
	ASSERT(self->irlap != NULL, return;);
	
	switch (event) {
	case LM_LAP_DISCOVERY_REQUEST:
		/* irlmp_next_station_state( LMP_DISCOVER); */
		
		irlap_discovery_request(self->irlap, &irlmp->discovery_cmd);
		break;
	case LM_LAP_DISCOVERY_CONFIRM:
 		/* irlmp_next_station_state( LMP_READY); */
		irlmp_discovery_confirm(irlmp->cachelog);
 		break;
	case LM_LAP_CONNECT_INDICATION:
		/*  It's important to switch state first, to avoid IrLMP to 
		 *  think that the link is free since IrLMP may then start
		 *  discovery before the connection is properly set up. DB.
		 */
		irlmp_next_lap_state(self, LAP_ACTIVE);

		/* Just accept connection TODO, this should be fixed */
		irlap_connect_response(self->irlap, skb);
		break;
	case LM_LAP_CONNECT_REQUEST:
		IRDA_DEBUG(4, __FUNCTION__ "() LS_CONNECT_REQUEST\n");

		irlmp_next_lap_state(self, LAP_U_CONNECT);
		self->refcount++;

		/* FIXME: need to set users requested QoS */
		irlap_connect_request(self->irlap, self->daddr, NULL, 0);
		break;
	case LM_LAP_DISCONNECT_INDICATION:
		IRDA_DEBUG(4, __FUNCTION__ 
			   "(), Error LM_LAP_DISCONNECT_INDICATION\n");
		
		irlmp_next_lap_state(self, LAP_STANDBY);
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "(), Unknown event %s\n",
			   irlmp_event[event]);
		if (skb)
 			dev_kfree_skb(skb);
		break;
	}
}

/*
 * Function irlmp_state_u_connect (event, skb, info)
 *
 *    U_CONNECT, The layer above has tried to open an LSAP connection but
 *    since the IrLAP connection does not exist, we must first start an
 *    IrLAP connection. We are now waiting response from IrLAP.
 * */
static void irlmp_state_u_connect(struct lap_cb *self, IRLMP_EVENT event, 
				  struct sk_buff *skb)
{
	struct lsap_cb *lsap;
	struct lsap_cb *lsap_current;
	
	IRDA_DEBUG(2, __FUNCTION__ "(), event=%s\n", irlmp_event[event]);

	switch (event) {
	case LM_LAP_CONNECT_INDICATION:
		/*  It's important to switch state first, to avoid IrLMP to 
		 *  think that the link is free since IrLMP may then start
		 *  discovery before the connection is properly set up. DB.
		 */
		irlmp_next_lap_state(self, LAP_ACTIVE);

		/* Just accept connection TODO, this should be fixed */
		irlap_connect_response(self->irlap, skb);

		lsap = (struct lsap_cb *) hashbin_get_first(self->lsaps);
		while (lsap != NULL) {
			irlmp_do_lsap_event(lsap, LM_LAP_CONNECT_CONFIRM, NULL);
			lsap = (struct lsap_cb*) hashbin_get_next(self->lsaps);
		}
		break;
	case LM_LAP_CONNECT_REQUEST:
		/* Already trying to connect */
		self->refcount++;
		break;
	case LM_LAP_CONNECT_CONFIRM:
		/* For all lsap_ce E Associated do LS_Connect_confirm */
		irlmp_next_lap_state(self, LAP_ACTIVE);

		lsap = (struct lsap_cb *) hashbin_get_first(self->lsaps);
		while (lsap != NULL) {
			irlmp_do_lsap_event(lsap, LM_LAP_CONNECT_CONFIRM, NULL);
			lsap = (struct lsap_cb*) hashbin_get_next(self->lsaps);
		}
		break;
	case LM_LAP_DISCONNECT_INDICATION:
		irlmp_next_lap_state(self, LAP_STANDBY);
		self->refcount = 0;

		/* Send disconnect event to all LSAPs using this link */
		lsap = (struct lsap_cb *) hashbin_get_first( self->lsaps);
		while (lsap != NULL ) {
			ASSERT(lsap->magic == LMP_LSAP_MAGIC, return;);
			
			lsap_current = lsap;

			/* Be sure to stay one item ahead */
			lsap = (struct lsap_cb *) hashbin_get_next(self->lsaps);
			irlmp_do_lsap_event(lsap_current, 
					    LM_LAP_DISCONNECT_INDICATION,
					    NULL);
		}
		break;
	case LM_LAP_DISCONNECT_REQUEST:
		IRDA_DEBUG(4, __FUNCTION__ "(), LM_LAP_DISCONNECT_REQUEST\n");

		self->refcount--;
		if (self->refcount == 0)
			irlmp_next_lap_state(self, LAP_STANDBY);
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "(), Unknown event %s\n",
			   irlmp_event[event]);
		if (skb)
 			dev_kfree_skb(skb);
		break;
	}	
}

/*
 * Function irlmp_state_active (event, skb, info)
 *
 *    ACTIVE, IrLAP connection is active
 *
 */
static void irlmp_state_active(struct lap_cb *self, IRLMP_EVENT event, 
			       struct sk_buff *skb)
{
	struct lsap_cb *lsap;
	struct lsap_cb *lsap_current;

	IRDA_DEBUG(4, __FUNCTION__ "()\n"); 

 	switch (event) {
	case LM_LAP_CONNECT_REQUEST:
		IRDA_DEBUG(4, __FUNCTION__ "(), LS_CONNECT_REQUEST\n");
		self->refcount++;

		/*
		 *  LAP connection allready active, just bounce back! Since we 
		 *  don't know which LSAP that tried to do this, we have to 
		 *  notify all LSAPs using this LAP, but that should be safe to
		 *  do anyway.
		 */
		lsap = (struct lsap_cb *) hashbin_get_first(self->lsaps);
		while (lsap != NULL) {
			irlmp_do_lsap_event(lsap, LM_LAP_CONNECT_CONFIRM, NULL);
 			lsap = (struct lsap_cb*) hashbin_get_next(self->lsaps);
		}
		
		/* Needed by connect indication */
		lsap = (struct lsap_cb *) hashbin_get_first(irlmp->unconnected_lsaps);
		while (lsap != NULL) {
			lsap_current = lsap;
			
			/* Be sure to stay one item ahead */
 			lsap = (struct lsap_cb*) hashbin_get_next(irlmp->unconnected_lsaps);
			irlmp_do_lsap_event(lsap_current, 
					    LM_LAP_CONNECT_CONFIRM, NULL);
		}
		/* Keep state */
		break;
	case LM_LAP_DISCONNECT_REQUEST:
		self->refcount--;

		/*
		 *  Need to find out if we should close IrLAP or not. If there
		 *  is only one LSAP connection left on this link, that LSAP 
		 *  must be the one that tries to close IrLAP. It will be 
		 *  removed later and moved to the list of unconnected LSAPs
		 */
		if (HASHBIN_GET_SIZE(self->lsaps) > 0)
			irlmp_start_idle_timer(self, LM_IDLE_TIMEOUT);
		else {
			/* No more connections, so close IrLAP */
			irlmp_next_lap_state(self, LAP_STANDBY);
			irlap_disconnect_request(self->irlap);
		}
		break;
	case LM_LAP_IDLE_TIMEOUT:
		if (HASHBIN_GET_SIZE(self->lsaps) == 0) {
			irlmp_next_lap_state(self, LAP_STANDBY);
			irlap_disconnect_request(self->irlap);
		}
		break;
	case LM_LAP_DISCONNECT_INDICATION:
		irlmp_next_lap_state(self, LAP_STANDBY);		
		self->refcount = 0;
		
		/* In some case, at this point our side has already closed
		 * all lsaps, and we are waiting for the idle_timer to
		 * expire. If another device reconnect immediately, the
		 * idle timer will expire in the midle of the connection
		 * initialisation, screwing up things a lot...
		 * Therefore, we must stop the timer... */
		irlmp_stop_idle_timer(self);

		/* 
		 *  Inform all connected LSAP's using this link
		 */
		lsap = (struct lsap_cb *) hashbin_get_first(self->lsaps);
		while (lsap != NULL ) {
			ASSERT(lsap->magic == LMP_LSAP_MAGIC, return;);
			
			lsap_current = lsap;

			/* Be sure to stay one item ahead */
			lsap = (struct lsap_cb *) hashbin_get_next(self->lsaps);
			irlmp_do_lsap_event(lsap_current, 
					    LM_LAP_DISCONNECT_INDICATION,
					    NULL);
		}
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "(), Unknown event %s\n",
			   irlmp_event[event]);
		if (skb)
 			dev_kfree_skb(skb);
		break;
	}	
}

/*********************************************************************
 *
 *    LSAP connection control states
 *
 ********************************************************************/

/*
 * Function irlmp_state_disconnected (event, skb, info)
 *
 *    DISCONNECTED
 *
 */
static int irlmp_state_disconnected(struct lsap_cb *self, IRLMP_EVENT event,
				    struct sk_buff *skb) 
{
	int ret = 0;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LMP_LSAP_MAGIC, return -1;);

	switch (event) {
#ifdef CONFIG_IRDA_ULTRA
	case LM_UDATA_INDICATION:
		irlmp_connless_data_indication(self, skb); 
		break;
#endif /* CONFIG_IRDA_ULTRA */
	case LM_CONNECT_REQUEST:
		IRDA_DEBUG(4, __FUNCTION__ "(), LM_CONNECT_REQUEST\n");

		if (self->conn_skb) {
			WARNING(__FUNCTION__ 
				"(), busy with another request!\n");
			return -EBUSY;
		}
		self->conn_skb = skb;

		irlmp_next_lsap_state(self, LSAP_SETUP_PEND);

		irlmp_do_lap_event(self->lap, LM_LAP_CONNECT_REQUEST, NULL);

		/* Start watchdog timer (5 secs for now) */
		irlmp_start_watchdog_timer(self, 5*HZ);
		break;
	case LM_CONNECT_INDICATION:
		irlmp_next_lsap_state(self, LSAP_CONNECT_PEND);

		if (self->conn_skb) {
			WARNING(__FUNCTION__ 
				"(), busy with another request!\n");
			return -EBUSY;
		}
		self->conn_skb = skb;

		irlmp_do_lap_event(self->lap, LM_LAP_CONNECT_REQUEST, NULL);
		break;
	default:
		IRDA_DEBUG(2, __FUNCTION__ "(), Unknown event %s\n", 
			   irlmp_event[event]);
		if (skb)
  			dev_kfree_skb(skb);
		break;
	}
	return ret;
}

/*
 * Function irlmp_state_connect (self, event, skb)
 *
 *    CONNECT
 *
 */
static int irlmp_state_connect(struct lsap_cb *self, IRLMP_EVENT event, 
				struct sk_buff *skb) 
{
	struct lsap_cb *lsap;
	int ret = 0;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");
	
	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LMP_LSAP_MAGIC, return -1;);

	switch (event) {
	case LM_CONNECT_RESPONSE:
		/* 
		 *  Bind this LSAP to the IrLAP link where the connect was
		 *  received 
		 */
		lsap = hashbin_remove(irlmp->unconnected_lsaps, (int) self, 
				      NULL);

		ASSERT(lsap == self, return -1;);		
		ASSERT(self->lap != NULL, return -1;);
		ASSERT(self->lap->lsaps != NULL, return -1;);
		
		hashbin_insert(self->lap->lsaps, (irda_queue_t *) self, (int) self, 
			       NULL);

		irlmp_send_lcf_pdu(self->lap, self->dlsap_sel, 
				   self->slsap_sel, CONNECT_CNF, skb);

		del_timer(&self->watchdog_timer);

		irlmp_next_lsap_state(self, LSAP_DATA_TRANSFER_READY);
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "(), Unknown event %s\n",
			   irlmp_event[event]);
		if (skb)
 			dev_kfree_skb(skb);
		break;
	}
	return ret;
}

/*
 * Function irlmp_state_connect_pend (event, skb, info)
 *
 *    CONNECT_PEND
 *
 */
static int irlmp_state_connect_pend(struct lsap_cb *self, IRLMP_EVENT event,
				    struct sk_buff *skb) 
{
	int ret = 0;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LMP_LSAP_MAGIC, return -1;);

	switch (event) {
	case LM_CONNECT_REQUEST:
		/* Keep state */
		break;
	case LM_CONNECT_RESPONSE:
		IRDA_DEBUG(0, __FUNCTION__ "(), LM_CONNECT_RESPONSE, "
			   "no indication issued yet\n");
		/* Keep state */
		break;
	case LM_DISCONNECT_REQUEST:
		IRDA_DEBUG(0, __FUNCTION__ "(), LM_DISCONNECT_REQUEST, "
			   "not yet bound to IrLAP connection\n");
		/* Keep state */
		break;
	case LM_LAP_CONNECT_CONFIRM:
		IRDA_DEBUG(4, __FUNCTION__ "(), LS_CONNECT_CONFIRM\n");
		irlmp_next_lsap_state(self, LSAP_CONNECT);

		skb = self->conn_skb;
		self->conn_skb = NULL;

		irlmp_connect_indication(self, skb);
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "Unknown event %s\n", 
			   irlmp_event[event]);
		if (skb)
 			dev_kfree_skb(skb);
		break;	
	}
	return ret;
}

/*
 * Function irlmp_state_dtr (self, event, skb)
 *
 *    DATA_TRANSFER_READY
 *
 */
static int irlmp_state_dtr(struct lsap_cb *self, IRLMP_EVENT event, 
			   struct sk_buff *skb) 
{
	LM_REASON reason;
	int ret = 0;

 	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LMP_LSAP_MAGIC, return -1;);
	ASSERT(self->lap != NULL, return -1;);

	switch (event) {
	case LM_DATA_REQUEST: /* Optimize for the common case */
		irlmp_send_data_pdu(self->lap, self->dlsap_sel, 
				    self->slsap_sel, FALSE, skb);
		break;
	case LM_DATA_INDICATION: /* Optimize for the common case */
		irlmp_data_indication(self, skb); 
		break;
	case LM_UDATA_REQUEST:
		ASSERT(skb != NULL, return -1;);
		irlmp_send_data_pdu(self->lap, self->dlsap_sel, 
				    self->slsap_sel, TRUE, skb);
		break;
	case LM_UDATA_INDICATION:
		irlmp_udata_indication(self, skb); 
		break;
	case LM_CONNECT_REQUEST:
		IRDA_DEBUG(0, __FUNCTION__ "(), LM_CONNECT_REQUEST, "
			   "error, LSAP already connected\n");
		/* Keep state */
		break;
	case LM_CONNECT_RESPONSE:
		IRDA_DEBUG(0, __FUNCTION__ "(), LM_CONNECT_RESPONSE, " 
			   "error, LSAP allready connected\n");
		/* Keep state */
		break;
	case LM_DISCONNECT_REQUEST:
		irlmp_send_lcf_pdu(self->lap, self->dlsap_sel, self->slsap_sel,
				   DISCONNECT, skb);
		irlmp_next_lsap_state(self, LSAP_DISCONNECTED);
		
		/* Try to close the LAP connection if its still there */
		if (self->lap) {
			IRDA_DEBUG(4, __FUNCTION__ "(), trying to close IrLAP\n");
			irlmp_do_lap_event(self->lap, 
					   LM_LAP_DISCONNECT_REQUEST, 
					   NULL);
		}
		break;
	case LM_LAP_DISCONNECT_INDICATION:
		irlmp_next_lsap_state(self, LSAP_DISCONNECTED);

		reason = irlmp_convert_lap_reason(self->lap->reason);

		irlmp_disconnect_indication(self, reason, NULL);
		break;
	case LM_DISCONNECT_INDICATION:
		irlmp_next_lsap_state(self, LSAP_DISCONNECTED);
			
		ASSERT(self->lap != NULL, return -1;);
		ASSERT(self->lap->magic == LMP_LAP_MAGIC, return -1;);
	
		ASSERT(skb != NULL, return -1;);
		ASSERT(skb->len > 3, return -1;);
		reason = skb->data[3];

		 /* Try to close the LAP connection */
		IRDA_DEBUG(4, __FUNCTION__ "(), trying to close IrLAP\n");
		irlmp_do_lap_event(self->lap, LM_LAP_DISCONNECT_REQUEST, NULL);

		irlmp_disconnect_indication(self, reason, skb);
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "(), Unknown event %s\n", 
			   irlmp_event[event]);
		if (skb)
 			dev_kfree_skb(skb);
		break;	
	}
	return ret;
}

/*
 * Function irlmp_state_setup (event, skb, info)
 *
 *    SETUP, Station Control has set up the underlying IrLAP connection.
 *    An LSAP connection request has been transmitted to the peer
 *    LSAP-Connection Control FSM and we are awaiting reply.
 */
static int irlmp_state_setup(struct lsap_cb *self, IRLMP_EVENT event, 
			     struct sk_buff *skb) 
{
	LM_REASON reason;
	int ret = 0;

	ASSERT(self != NULL, return -1;);
	ASSERT(self->magic == LMP_LSAP_MAGIC, return -1;);

	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	switch (event) {
	case LM_CONNECT_CONFIRM:
		irlmp_next_lsap_state(self, LSAP_DATA_TRANSFER_READY);

		del_timer(&self->watchdog_timer);
		
		irlmp_connect_confirm(self, skb);
		break;
	case LM_DISCONNECT_INDICATION:
		irlmp_next_lsap_state(self, LSAP_DISCONNECTED);
			
		ASSERT(self->lap != NULL, return -1;);
		ASSERT(self->lap->magic == LMP_LAP_MAGIC, return -1;);
	
		ASSERT(skb != NULL, return -1;);
		ASSERT(skb->len > 3, return -1;);
		reason = skb->data[3];

		 /* Try to close the LAP connection */
		IRDA_DEBUG(4, __FUNCTION__ "(), trying to close IrLAP\n");
		irlmp_do_lap_event(self->lap, LM_LAP_DISCONNECT_REQUEST, NULL);

		irlmp_disconnect_indication(self, reason, skb);
		break;
	case LM_LAP_DISCONNECT_INDICATION:
		irlmp_next_lsap_state(self, LSAP_DISCONNECTED);

		del_timer(&self->watchdog_timer);

		ASSERT(self->lap != NULL, return -1;);
		ASSERT(self->lap->magic == LMP_LAP_MAGIC, return -1;);
		
		reason = irlmp_convert_lap_reason(self->lap->reason);

		irlmp_disconnect_indication(self, reason, skb);
		break;
	case LM_WATCHDOG_TIMEOUT:
		IRDA_DEBUG(0, __FUNCTION__ "() WATCHDOG_TIMEOUT!\n");
		
		ASSERT(self->lap != NULL, return -1;);
		irlmp_do_lap_event(self->lap, LM_LAP_DISCONNECT_REQUEST, NULL);
		irlmp_next_lsap_state(self, LSAP_DISCONNECTED);
		
		irlmp_disconnect_indication(self, LM_CONNECT_FAILURE, NULL);
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "(), Unknown event %s\n", 
			   irlmp_event[event]);
		if (skb)
 			dev_kfree_skb(skb);
		break;	
	}
	return ret;
}

/*
 * Function irlmp_state_setup_pend (event, skb, info)
 *
 *    SETUP_PEND, An LM_CONNECT_REQUEST has been received from the service
 *    user to set up an LSAP connection. A request has been sent to the
 *    LAP FSM to set up the underlying IrLAP connection, and we
 *    are awaiting confirm.
 */
static int irlmp_state_setup_pend(struct lsap_cb *self, IRLMP_EVENT event, 
				  struct sk_buff *skb) 
{
	LM_REASON reason;
	int ret = 0;

	IRDA_DEBUG(4, __FUNCTION__ "()\n"); 

	ASSERT(self != NULL, return -1;);
	ASSERT(irlmp != NULL, return -1;);

	switch (event) {
	case LM_LAP_CONNECT_CONFIRM:
		ASSERT(self->conn_skb != NULL, return -1;);

		skb = self->conn_skb;
		self->conn_skb = NULL;

		irlmp_send_lcf_pdu(self->lap, self->dlsap_sel, 
				   self->slsap_sel, CONNECT_CMD, skb);

		irlmp_next_lsap_state(self, LSAP_SETUP);
		break;
	case LM_WATCHDOG_TIMEOUT:
		IRDA_DEBUG(0, __FUNCTION__ "() WATCHDOG_TIMEOUT!\n");

		ASSERT(self->lap != NULL, return -1;);
		irlmp_do_lap_event(self->lap, LM_LAP_DISCONNECT_REQUEST, NULL);
		irlmp_next_lsap_state(self, LSAP_DISCONNECTED);

		irlmp_disconnect_indication(self, LM_CONNECT_FAILURE, NULL);
		break;
	case LM_LAP_DISCONNECT_INDICATION: /* LS_Disconnect.indication */
		del_timer( &self->watchdog_timer);

		irlmp_next_lsap_state(self, LSAP_DISCONNECTED);
		
		reason = irlmp_convert_lap_reason(self->lap->reason);
		
		irlmp_disconnect_indication(self, reason, NULL);
		break;
	default:
		IRDA_DEBUG(0, __FUNCTION__ "(), Unknown event %s\n", 
			   irlmp_event[event]);
		if (skb)
 			dev_kfree_skb(skb);
		break;	
	}
	return ret;
}

void irlmp_next_lap_state(struct lap_cb *self, IRLMP_STATE state) 
{
	IRDA_DEBUG(4, __FUNCTION__ "(), LMP LAP = %s\n", irlmp_state[state]);
	self->lap_state = state;
}

void irlmp_next_lsap_state(struct lsap_cb *self, LSAP_STATE state) 
{
	ASSERT(self != NULL, return;);

	IRDA_DEBUG(4, __FUNCTION__ "(), LMP LSAP = %s\n", irlsap_state[state]);
	self->lsap_state = state;
}
