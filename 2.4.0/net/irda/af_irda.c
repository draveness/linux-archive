/*********************************************************************
 *                
 * Filename:      af_irda.c
 * Version:       0.9
 * Description:   IrDA sockets implementation
 * Status:        Stable
 * Author:        Dag Brattli <dagb@cs.uit.no>
 * Created at:    Sun May 31 10:12:43 1998
 * Modified at:   Sat Dec 25 21:10:23 1999
 * Modified by:   Dag Brattli <dag@brattli.net>
 * Sources:       af_netroom.c, af_ax25.c, af_rose.c, af_x25.c etc.
 * 
 *     Copyright (c) 1999 Dag Brattli <dagb@cs.uit.no>
 *     Copyright (c) 1999 Jean Tourrilhes <jt@hpl.hp.com>
 *     All Rights Reserved.
 *
 *     This program is free software; you can redistribute it and/or 
 *     modify it under the terms of the GNU General Public License as 
 *     published by the Free Software Foundation; either version 2 of 
 *     the License, or (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License 
 *     along with this program; if not, write to the Free Software 
 *     Foundation, Inc., 59 Temple Place, Suite 330, Boston, 
 *     MA 02111-1307 USA
 *
 *     Linux-IrDA now supports four different types of IrDA sockets:
 *
 *     o SOCK_STREAM:    TinyTP connections with SAR disabled. The
 *                       max SDU size is 0 for conn. of this type
 *     o SOCK_SEQPACKET: TinyTP connections with SAR enabled. TTP may 
 *                       fragment the messages, but will preserve
 *                       the message boundaries
 *     o SOCK_DGRAM:     IRDAPROTO_UNITDATA: TinyTP connections with Unitdata 
 *                       (unreliable) transfers
 *                       IRDAPROTO_ULTRA: Connectionless and unreliable data
 *     
 ********************************************************************/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/init.h>
#include <linux/if_arp.h>
#include <linux/net.h>
#include <linux/irda.h>
#include <linux/poll.h>

#include <asm/uaccess.h>

#include <net/sock.h>

#include <net/irda/irda.h>
#include <net/irda/iriap.h>
#include <net/irda/irias_object.h>
#include <net/irda/irlmp.h>
#include <net/irda/irttp.h>
#include <net/irda/discovery.h>

extern int  irda_init(void);
extern void irda_cleanup(void);
extern int  irlap_driver_rcv(struct sk_buff *, struct net_device *, 
			     struct packet_type *);

static int irda_create(struct socket *sock, int protocol);

static struct proto_ops irda_stream_ops;
static struct proto_ops irda_seqpacket_ops;
static struct proto_ops irda_dgram_ops;

#ifdef CONFIG_IRDA_ULTRA
static struct proto_ops irda_ultra_ops;
#define ULTRA_MAX_DATA 382
#endif /* CONFIG_IRDA_ULTRA */

#define IRDA_MAX_HEADER (TTP_MAX_HEADER)

#ifdef CONFIG_IRDA_DEBUG
__u32 irda_debug = IRDA_DEBUG_LEVEL;
#endif

/*
 * Function irda_data_indication (instance, sap, skb)
 *
 *    Received some data from TinyTP. Just queue it on the receive queue
 *
 */
static int irda_data_indication(void *instance, void *sap, struct sk_buff *skb)
{
	struct irda_sock *self;
	struct sock *sk;
	int err;

	self = (struct irda_sock *) instance;
	ASSERT(self != NULL, return -1;);

	sk = self->sk;
	ASSERT(sk != NULL, return -1;);

	err = sock_queue_rcv_skb(sk, skb);
	if (err) {
		IRDA_DEBUG(1, __FUNCTION__ "(), error: no more mem!\n");
		self->rx_flow = FLOW_STOP;

		/* When we return error, TTP will need to requeue the skb */
		return err;
	}

	return 0;
}

/*
 * Function irda_disconnect_indication (instance, sap, reason, skb)
 *
 *    Connection has been closed. Check reason to find out why
 *
 */
static void irda_disconnect_indication(void *instance, void *sap, 
				       LM_REASON reason, struct sk_buff *skb)
{
	struct irda_sock *self;
	struct sock *sk;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = (struct irda_sock *) instance;

	sk = self->sk;
	if (sk == NULL)
		return;

	sk->state     = TCP_CLOSE;
        sk->err       = ECONNRESET;
        sk->shutdown |= SEND_SHUTDOWN;
	if (!sk->dead) {
		sk->state_change(sk);
                sk->dead = 1;
        }

	/* Close our TSAP.
	 * If we leave it open, IrLMP put it back into the list of
	 * unconnected LSAPs. The problem is that any incomming request
	 * can then be matched to this socket (and it will be, because
	 * it is at the head of the list). This would prevent any
	 * listening socket waiting on the same TSAP to get those requests.
	 * Some apps forget to close sockets, or hang to it a bit too long,
	 * so we may stay in this dead state long enough to be noticed...
	 * Note : all socket function do check sk->state, so we are safe...
	 * Jean II
	 */
	irttp_close_tsap(self->tsap);
	self->tsap = NULL;

	/* Note : once we are there, there is not much you want to do
	 * with the socket anymore, apart from closing it.
	 * For example, bind() and connect() won't reset sk->err,
	 * sk->shutdown and sk->dead to valid values...
	 * Jean II
	 */
}

/*
 * Function irda_connect_confirm (instance, sap, qos, max_sdu_size, skb)
 *
 *    Connections has been confirmed by the remote device
 *
 */
static void irda_connect_confirm(void *instance, void *sap, 
				 struct qos_info *qos,
				 __u32 max_sdu_size, __u8 max_header_size, 
				 struct sk_buff *skb)
{
	struct irda_sock *self;
	struct sock *sk;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = (struct irda_sock *) instance;

	sk = self->sk;
	if (sk == NULL)
		return;

	/* How much header space do we need to reserve */
	self->max_header_size = max_header_size;

	/* IrTTP max SDU size in transmit direction */
	self->max_sdu_size_tx = max_sdu_size;

	/* Find out what the largest chunk of data that we can transmit is */
	switch (sk->type) {
	case SOCK_STREAM:
		if (max_sdu_size != 0) {
			ERROR(__FUNCTION__ "(), max_sdu_size must be 0\n");
			return;
		}
		self->max_data_size = irttp_get_max_seg_size(self->tsap);
		break;
	case SOCK_SEQPACKET:
		if (max_sdu_size == 0) {
			ERROR(__FUNCTION__ "(), max_sdu_size cannot be 0\n");
			return;
		}
		self->max_data_size = max_sdu_size;
		break;
	default:
		self->max_data_size = irttp_get_max_seg_size(self->tsap);
	};

	IRDA_DEBUG(2, __FUNCTION__ "(), max_data_size=%d\n", 
		   self->max_data_size);

	memcpy(&self->qos_tx, qos, sizeof(struct qos_info));

	skb_queue_tail(&sk->receive_queue, skb);

	/* We are now connected! */
	sk->state = TCP_ESTABLISHED;
	sk->state_change(sk);
}

/*
 * Function irda_connect_indication(instance, sap, qos, max_sdu_size, userdata)
 *
 *    Incomming connection
 *
 */
static void irda_connect_indication(void *instance, void *sap, 
				    struct qos_info *qos, __u32 max_sdu_size,
				    __u8 max_header_size, struct sk_buff *skb)
{
	struct irda_sock *self;
	struct sock *sk;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

 	self = (struct irda_sock *) instance;

	sk = self->sk;
	if (sk == NULL)
		return;

	/* How much header space do we need to reserve */
	self->max_header_size = max_header_size;

	/* IrTTP max SDU size in transmit direction */
	self->max_sdu_size_tx = max_sdu_size;	

	/* Find out what the largest chunk of data that we can transmit is */
	switch (sk->type) {
	case SOCK_STREAM:
		if (max_sdu_size != 0) {
			ERROR(__FUNCTION__ "(), max_sdu_size must be 0\n");
			return;
		}
		self->max_data_size = irttp_get_max_seg_size(self->tsap);
		break;
	case SOCK_SEQPACKET:
		if (max_sdu_size == 0) {
			ERROR(__FUNCTION__ "(), max_sdu_size cannot be 0\n");
			return;
		}
		self->max_data_size = max_sdu_size;
		break;
	default:
		self->max_data_size = irttp_get_max_seg_size(self->tsap);
	};

	IRDA_DEBUG(2, __FUNCTION__ "(), max_data_size=%d\n", 
		   self->max_data_size);

	memcpy(&self->qos_tx, qos, sizeof(struct qos_info));
	
	skb_queue_tail(&sk->receive_queue, skb);
	sk->state_change(sk);
}

/*
 * Function irda_connect_response (handle)
 *
 *    Accept incomming connection
 *
 */
void irda_connect_response(struct irda_sock *self)
{
	struct sk_buff *skb;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);

	skb = dev_alloc_skb(64);
	if (skb == NULL) {
		IRDA_DEBUG(0, __FUNCTION__ "() Unable to allocate sk_buff!\n");
		return;
	}

	/* Reserve space for MUX_CONTROL and LAP header */
	skb_reserve(skb, IRDA_MAX_HEADER);

	irttp_connect_response(self->tsap, self->max_sdu_size_rx, skb);
}

/*
 * Function irda_flow_indication (instance, sap, flow)
 *
 *    Used by TinyTP to tell us if it can accept more data or not
 *
 */
static void irda_flow_indication(void *instance, void *sap, LOCAL_FLOW flow) 
{
	struct irda_sock *self;
	struct sock *sk;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");
	
	self = (struct irda_sock *) instance;
	ASSERT(self != NULL, return;);

	sk = self->sk;
	ASSERT(sk != NULL, return;);
	
	switch (flow) {
	case FLOW_STOP:
		IRDA_DEBUG(1, __FUNCTION__ "(), IrTTP wants us to slow down\n");
		self->tx_flow = flow;
		break;
	case FLOW_START:
		self->tx_flow = flow;
		IRDA_DEBUG(1, __FUNCTION__ 
			   "(), IrTTP wants us to start again\n");
		wake_up_interruptible(sk->sleep);
		break;
	default:
		IRDA_DEBUG( 0, __FUNCTION__ "(), Unknown flow command!\n");
		/* Unknown flow command, better stop */
		self->tx_flow = flow;
		break;
	}
}

/*
 * Function irda_getvalue_confirm (obj_id, value, priv)
 *
 *    Got answer from remote LM-IAS, just pass object to requester...
 *
 * Note : duplicate from above, but we need our own version that
 * doesn't touch the dtsap_sel and save the full value structure...
 */
static void irda_getvalue_confirm(int result, __u16 obj_id, 
					  struct ias_value *value, void *priv)
{
	struct irda_sock *self;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = (struct irda_sock *) priv;
	if (!self) {
		WARNING(__FUNCTION__ "(), lost myself!\n");
		return;
	}

	/* We probably don't need to make any more queries */
	iriap_close(self->iriap);
	self->iriap = NULL;

	/* Check if request succeeded */
	if (result != IAS_SUCCESS) {
		IRDA_DEBUG(1, __FUNCTION__ "(), IAS query failed! (%d)\n",
			   result);

		self->errno = result;	/* We really need it later */

		/* Wake up any processes waiting for result */
		wake_up_interruptible(&self->query_wait);

		return;
	}

	/* Pass the object to the caller (so the caller must delete it) */
	self->ias_result = value;
	self->errno = 0;

	/* Wake up any processes waiting for result */
	wake_up_interruptible(&self->query_wait);
}

/*
 * Function irda_selective_discovery_indication (discovery)
 *
 *    Got a selective discovery indication from IrLMP.
 *
 * IrLMP is telling us that this node is matching our hint bit
 * filter. Check if it's a newly discovered node (or if node changed its
 * hint bits), and then wake up any process waiting for answer...
 */
static void irda_selective_discovery_indication(discovery_t *discovery,
						void *priv)
{
	struct irda_sock *self;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = (struct irda_sock *) priv;
	if (!self) {
		WARNING(__FUNCTION__ "(), lost myself!\n");
		return;
	}

	/* Check if node is discovered is a new one or an old one.
	 * We check when how long ago this node was discovered, with a
	 * coarse timeout (we may miss some discovery events or be delayed).
	 * Note : by doing this test here, we avoid waking up a process ;-)
	 */
	if((jiffies - discovery->first_timestamp) >
	   (sysctl_discovery_timeout * HZ)) {
		return;		/* Too old, not interesting -> goodbye */
	}

	/* Pass parameter to the caller */
	self->cachediscovery = discovery;

	/* Wake up process if its waiting for device to be discovered */
	wake_up_interruptible(&self->query_wait);
}

/*
 * Function irda_discovery_timeout (priv)
 *
 *    Timeout in the selective discovery process
 *
 * We were waiting for a node to be discovered, but nothing has come up
 * so far. Wake up the user and tell him that we failed...
 */
static void irda_discovery_timeout(u_long	priv)
{
	struct irda_sock *self;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = (struct irda_sock *) priv;
	ASSERT(self != NULL, return;);

	/* Nothing for the caller */
	self->cachelog = NULL;
	self->cachediscovery = NULL;
	self->errno = -ETIME;

	/* Wake up process if its still waiting... */
	wake_up_interruptible(&self->query_wait);
}

/*
 * Function irda_open_tsap (self)
 *
 *    Open local Transport Service Access Point (TSAP)
 *
 */
static int irda_open_tsap(struct irda_sock *self, __u8 tsap_sel, char *name)
{
	notify_t notify;

	if (self->tsap) {
		WARNING(__FUNCTION__ "(), busy!\n");
		return -EBUSY;
	}
	
	/* Initialize callbacks to be used by the IrDA stack */
	irda_notify_init(&notify);
	notify.connect_confirm       = irda_connect_confirm;
	notify.connect_indication    = irda_connect_indication;
	notify.disconnect_indication = irda_disconnect_indication;
	notify.data_indication       = irda_data_indication;
	notify.udata_indication	     = irda_data_indication;
	notify.flow_indication       = irda_flow_indication;
	notify.instance = self;
	strncpy(notify.name, name, NOTIFY_MAX_NAME);

	self->tsap = irttp_open_tsap(tsap_sel, DEFAULT_INITIAL_CREDIT,
				     &notify);	
	if (self->tsap == NULL) {
		IRDA_DEBUG( 0, __FUNCTION__ "(), Unable to allocate TSAP!\n");
		return -ENOMEM;
	}
	/* Remember which TSAP selector we actually got */
	self->stsap_sel = self->tsap->stsap_sel;

	return 0;
}

/*
 * Function irda_open_lsap (self)
 *
 *    Open local Link Service Access Point (LSAP). Used for opening Ultra
 *    sockets
 */
#ifdef CONFIG_IRDA_ULTRA
static int irda_open_lsap(struct irda_sock *self, int pid)
{
	notify_t notify;

	if (self->lsap) {
		WARNING(__FUNCTION__ "(), busy!\n");
		return -EBUSY;
	}
	
	/* Initialize callbacks to be used by the IrDA stack */
	irda_notify_init(&notify);
	notify.udata_indication	= irda_data_indication;
	notify.instance = self;
	strncpy(notify.name, "Ultra", NOTIFY_MAX_NAME);

	self->lsap = irlmp_open_lsap(LSAP_CONNLESS, &notify, pid);	
	if (self->lsap == NULL) {
		IRDA_DEBUG( 0, __FUNCTION__ "(), Unable to allocate LSAP!\n");
		return -ENOMEM;
	}

	return 0;
}
#endif /* CONFIG_IRDA_ULTRA */

/*
 * Function irda_find_lsap_sel (self, name)
 *
 *    Try to lookup LSAP selector in remote LM-IAS
 *
 * Basically, we start a IAP query, and then go to sleep. When the query
 * return, irda_getvalue_confirm will wake us up, and we can examine the
 * result of the query...
 * Note that in some case, the query fail even before we go to sleep,
 * creating some races...
 */
static int irda_find_lsap_sel(struct irda_sock *self, char *name)
{
	IRDA_DEBUG(2, __FUNCTION__ "(), name=%s\n", name);

	ASSERT(self != NULL, return -1;);

	if (self->iriap) {
		WARNING(__FUNCTION__ "(), busy with a previous query\n");
		return -EBUSY;
	}

	self->iriap = iriap_open(LSAP_ANY, IAS_CLIENT, self,
				 irda_getvalue_confirm);

	/* Treat unexpected signals as disconnect */
	self->errno = -EHOSTUNREACH;

	/* Query remote LM-IAS */
	iriap_getvaluebyclass_request(self->iriap, self->saddr, self->daddr,
				      name, "IrDA:TinyTP:LsapSel");
	/* Wait for answer (if not already failed) */
	if(self->iriap != NULL)
		interruptible_sleep_on(&self->query_wait);

	/* Check what happened */
	if (self->errno)
	{
		/* Requested object/attribute doesn't exist */
		if((self->errno == IAS_CLASS_UNKNOWN) ||
		   (self->errno == IAS_ATTRIB_UNKNOWN))
			return (-EADDRNOTAVAIL);
		else
			return (-EHOSTUNREACH);
	}

	/* Get the remote TSAP selector */
	switch (self->ias_result->type) {
	case IAS_INTEGER:
		IRDA_DEBUG(4, __FUNCTION__ "() int=%d\n",
			   self->ias_result->t.integer);
		
		if (self->ias_result->t.integer != -1)
			self->dtsap_sel = self->ias_result->t.integer;
		else 
			self->dtsap_sel = 0;
		break;
	default:
		self->dtsap_sel = 0;
		IRDA_DEBUG(0, __FUNCTION__ "(), bad type!\n");
		break;
	}
	if (self->ias_result)
		irias_delete_value(self->ias_result);

	if (self->dtsap_sel)
		return 0;

	return -EADDRNOTAVAIL;
}

/*
 * Function irda_discover_daddr_and_lsap_sel (self, name)
 *
 *    This try to find a device with the requested service.
 *
 * It basically look into the discovery log. For each address in the list,
 * it queries the LM-IAS of the device to find if this device offer
 * the requested service.
 * If there is more than one node supporting the service, we complain
 * to the user (it should move devices around).
 * The, we set both the destination address and the lsap selector to point
 * on the service on the unique device we have found.
 *
 * Note : this function fails if there is more than one device in range,
 * because IrLMP doesn't disconnect the LAP when the last LSAP is closed.
 * Moreover, we would need to wait the LAP disconnection...
 */
static int irda_discover_daddr_and_lsap_sel(struct irda_sock *self, char *name)
{
	struct irda_device_info *discoveries;	/* Copy of the discovery log */
	int	number;			/* Number of nodes in the log */
	int	i;
	int	err = -ENETUNREACH;
	__u32	daddr = DEV_ADDR_ANY;	/* Address we found the service on */
	__u8	dtsap_sel = 0x0;	/* TSAP associated with it */

	IRDA_DEBUG(2, __FUNCTION__ "(), name=%s\n", name);

	ASSERT(self != NULL, return -1;);

	/* Ask lmp for the current discovery log
	 * Note : we have to use irlmp_get_discoveries(), as opposed
	 * to play with the cachelog directly, because while we are
	 * making our ias query, le log might change... */
	discoveries = irlmp_get_discoveries(&number, self->mask);
	/* Check if the we got some results */
	if (discoveries == NULL)
		return -ENETUNREACH;	/* No nodes discovered */

	/* 
	 * Now, check all discovered devices (if any), and connect
	 * client only about the services that the client is
	 * interested in...
	 */
	for(i = 0; i < number; i++) {
		/* Try the address in the log */
		self->daddr = discoveries[i].daddr;
		self->saddr = 0x0;
		IRDA_DEBUG(1, __FUNCTION__ "(), trying daddr = %08x\n",
			   self->daddr);

		/* Query remote LM-IAS for this service */
		err = irda_find_lsap_sel(self, name);
		switch (err) {
		case 0:
			/* We found the requested service */
			if(daddr != DEV_ADDR_ANY) {
				IRDA_DEBUG(1, __FUNCTION__
					   "(), discovered service ''%s'' in two different devices !!!\n",
					   name);
				self->daddr = DEV_ADDR_ANY;
				kfree(discoveries);
				return(-ENOTUNIQ);
			}
			/* First time we found that one, save it ! */
			daddr = self->daddr;
			dtsap_sel = self->dtsap_sel;
			break;
		case -EADDRNOTAVAIL:
			/* Requested service simply doesn't exist on this node */
			break;
		default:
			/* Something bad did happen :-( */
			IRDA_DEBUG(0, __FUNCTION__
				   "(), unexpected IAS query failure\n");
			self->daddr = DEV_ADDR_ANY;
			kfree(discoveries);
			return(-EHOSTUNREACH);
			break;
		}
	}
	/* Cleanup our copy of the discovery log */
	kfree(discoveries);

	/* Check out what we found */
	if(daddr == DEV_ADDR_ANY) {
		IRDA_DEBUG(1, __FUNCTION__
			   "(), cannot discover service ''%s'' in any device !!!\n",
			   name);
		self->daddr = DEV_ADDR_ANY;
		return(-EADDRNOTAVAIL);
	}

	/* Revert back to discovered device & service */
	self->daddr = daddr;
	self->saddr = 0x0;
	self->dtsap_sel = dtsap_sel;

	IRDA_DEBUG(1, __FUNCTION__ 
		   "(), discovered requested service ''%s'' at address %08x\n",
		   name, self->daddr);

	return 0;
}

/*
 * Function irda_getname (sock, uaddr, uaddr_len, peer)
 *
 *    Return the our own, or peers socket address (sockaddr_irda)
 *
 */
static int irda_getname(struct socket *sock, struct sockaddr *uaddr,
			int *uaddr_len, int peer)
{
	struct sockaddr_irda saddr;
	struct sock *sk = sock->sk;
	struct irda_sock *self = sk->protinfo.irda;

	if (peer) {
		if (sk->state != TCP_ESTABLISHED)
			return -ENOTCONN;
		
		saddr.sir_family = AF_IRDA;
		saddr.sir_lsap_sel = self->dtsap_sel;
		saddr.sir_addr = self->daddr;
	} else {
		saddr.sir_family = AF_IRDA;
		saddr.sir_lsap_sel = self->stsap_sel;
		saddr.sir_addr = self->saddr;
	}
	
	IRDA_DEBUG(1, __FUNCTION__ "(), tsap_sel = %#x\n", saddr.sir_lsap_sel);
	IRDA_DEBUG(1, __FUNCTION__ "(), addr = %08x\n", saddr.sir_addr);

	/* uaddr_len come to us uninitialised */
	*uaddr_len = sizeof (struct sockaddr_irda);
	memcpy(uaddr, &saddr, *uaddr_len);

	return 0;
}

/*
 * Function irda_listen (sock, backlog)
 *
 *    Just move to the listen state
 *
 */
static int irda_listen(struct socket *sock, int backlog)
{
	struct sock *sk = sock->sk;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	if ((sk->type != SOCK_STREAM) && (sk->type != SOCK_SEQPACKET) &&
	    (sk->type != SOCK_DGRAM))
		return -EOPNOTSUPP;

	if (sk->state != TCP_LISTEN) {
		sk->max_ack_backlog = backlog;
		sk->state           = TCP_LISTEN;
		
		return 0;
	}
	
	return -EOPNOTSUPP;
}

/*
 * Function irda_bind (sock, uaddr, addr_len)
 *
 *    Used by servers to register their well known TSAP
 *
 */
static int irda_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
	struct sock *sk = sock->sk;
	struct sockaddr_irda *addr = (struct sockaddr_irda *) uaddr;
	struct irda_sock *self;
	__u16 hints = 0;
	int err;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	if (addr_len != sizeof(struct sockaddr_irda))
		return -EINVAL;

#ifdef CONFIG_IRDA_ULTRA
	/* Special care for Ultra sockets */
	if ((sk->type == SOCK_DGRAM) && (sk->protocol == IRDAPROTO_ULTRA)) {
		self->pid = addr->sir_lsap_sel;
		if (self->pid & 0x80) {
			IRDA_DEBUG(0, __FUNCTION__ 
				   "(), extension in PID not supp!\n");
			return -EOPNOTSUPP;
		}
		err = irda_open_lsap(self, self->pid);
		if (err < 0)
			return err;
		
		self->max_data_size = ULTRA_MAX_DATA - LMP_PID_HEADER;
		self->max_header_size = IRDA_MAX_HEADER + LMP_PID_HEADER;

		/* Pretend we are connected */
		sock->state = SS_CONNECTED;
		sk->state   = TCP_ESTABLISHED;

		return 0;
	}
#endif /* CONFIG_IRDA_ULTRA */

	err = irda_open_tsap(self, addr->sir_lsap_sel, addr->sir_name);
	if (err < 0)
		return err;
	
	/*  Register with LM-IAS */
	self->ias_obj = irias_new_object(addr->sir_name, jiffies);
	irias_add_integer_attrib(self->ias_obj, "IrDA:TinyTP:LsapSel", 
				 self->stsap_sel, IAS_KERNEL_ATTR);
	irias_insert_object(self->ias_obj);
	
#if 1 /* Will be removed in near future */

	/* Fill in some default hint bits values */
	if (strncmp(addr->sir_name, "OBEX", 4) == 0)
		hints = irlmp_service_to_hint(S_OBEX);
	
	if (hints)
		self->skey = irlmp_register_service(hints);
#endif
	return 0;
}

/*
 * Function irda_accept (sock, newsock, flags)
 *
 *    Wait for incomming connection
 *
 */
static int irda_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct irda_sock *self, *new;
	struct sock *sk = sock->sk;
	struct sock *newsk;
	struct sk_buff *skb;
	int err;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	err = irda_create(newsock, sk->protocol);
	if (err)
		return err;

	if (sock->state != SS_UNCONNECTED)
		return -EINVAL;

	if ((sk = sock->sk) == NULL)
		return -EINVAL;

	if ((sk->type != SOCK_STREAM) && (sk->type != SOCK_SEQPACKET) &&
	    (sk->type != SOCK_DGRAM))
		return -EOPNOTSUPP;

	if (sk->state != TCP_LISTEN) 
		return -EINVAL;

	/*
	 *	The read queue this time is holding sockets ready to use
	 *	hooked into the SABM we saved
	 */
	do {
		if ((skb = skb_dequeue(&sk->receive_queue)) == NULL) {
			if (flags & O_NONBLOCK)
				return -EWOULDBLOCK;

			interruptible_sleep_on(sk->sleep);
			if (signal_pending(current)) 
				return -ERESTARTSYS;
		}
	} while (skb == NULL);

 	newsk = newsock->sk;
	newsk->state = TCP_ESTABLISHED;

	new = newsk->protinfo.irda;
	ASSERT(new != NULL, return -1;);

	/* Now attach up the new socket */
	new->tsap = irttp_dup(self->tsap, new);
	if (!new->tsap) {
		IRDA_DEBUG(0, __FUNCTION__ "(), dup failed!\n");
		return -1;
	}
		
	new->stsap_sel = new->tsap->stsap_sel;
	new->dtsap_sel = new->tsap->dtsap_sel;
	new->saddr = irttp_get_saddr(new->tsap);
	new->daddr = irttp_get_daddr(new->tsap);

	new->max_sdu_size_tx = self->max_sdu_size_tx;
	new->max_sdu_size_rx = self->max_sdu_size_rx;
	new->max_data_size   = self->max_data_size;
	new->max_header_size = self->max_header_size;

	memcpy(&new->qos_tx, &self->qos_tx, sizeof(struct qos_info));

	/* Clean up the original one to keep it in listen state */
	self->tsap->dtsap_sel = self->tsap->lsap->dlsap_sel = LSAP_ANY;
	self->tsap->lsap->lsap_state = LSAP_DISCONNECTED;

	skb->sk = NULL;
	skb->destructor = NULL;
	kfree_skb(skb);
	sk->ack_backlog--;

	newsock->state = SS_CONNECTED;

	irda_connect_response(new);

	return 0;
}

/*
 * Function irda_connect (sock, uaddr, addr_len, flags)
 *
 *    Connect to a IrDA device
 *
 * The main difference with a "standard" connect is that with IrDA we need
 * to resolve the service name into a TSAP selector (in TCP, port number
 * doesn't have to be resolved).
 * Because of this service name resoltion, we can offer "auto-connect",
 * where we connect to a service without specifying a destination address.
 *
 * Note : by consulting "errno", the user space caller may learn the cause
 * of the failure. Most of them are visible in the function, others may come
 * from subroutines called and are listed here :
 *	o EBUSY : already processing a connect
 *	o EHOSTUNREACH : bad addr->sir_addr argument
 *	o EADDRNOTAVAIL : bad addr->sir_name argument
 *	o ENOTUNIQ : more than one node has addr->sir_name (auto-connect)
 *	o ENETUNREACH : no node found on the network (auto-connect)
 */
static int irda_connect(struct socket *sock, struct sockaddr *uaddr,
			int addr_len, int flags)
{
	struct sock *sk = sock->sk;
	struct sockaddr_irda *addr = (struct sockaddr_irda *) uaddr;
	struct irda_sock *self;
	int err;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	self = sk->protinfo.irda;
	
	/* Don't allow connect for Ultra sockets */
	if ((sk->type == SOCK_DGRAM) && (sk->protocol == IRDAPROTO_ULTRA))
		return -ESOCKTNOSUPPORT;

	if (sk->state == TCP_ESTABLISHED && sock->state == SS_CONNECTING) {
		sock->state = SS_CONNECTED;
		return 0;   /* Connect completed during a ERESTARTSYS event */
	}
	
	if (sk->state == TCP_CLOSE && sock->state == SS_CONNECTING) {
		sock->state = SS_UNCONNECTED;
		return -ECONNREFUSED;
	}
	
	if (sk->state == TCP_ESTABLISHED)
		return -EISCONN;      /* No reconnect on a seqpacket socket */
	
	sk->state   = TCP_CLOSE;	
	sock->state = SS_UNCONNECTED;

	if (addr_len != sizeof(struct sockaddr_irda))
		return -EINVAL;

	/* Check if user supplied any destination device address */
	if ((!addr->sir_addr) || (addr->sir_addr == DEV_ADDR_ANY)) {
		/* Try to find one suitable */
		err = irda_discover_daddr_and_lsap_sel(self, addr->sir_name);
		if (err) {
			IRDA_DEBUG(0, __FUNCTION__ 
				   "(), auto-connect failed!\n");
			return err;
		}
	} else {
		/* Use the one provided by the user */
		self->daddr = addr->sir_addr;
		IRDA_DEBUG(1, __FUNCTION__ "(), daddr = %08x\n", self->daddr);
		
		/* Query remote LM-IAS */
		err = irda_find_lsap_sel(self, addr->sir_name);
		if (err) {
			IRDA_DEBUG(0, __FUNCTION__ "(), connect failed!\n");
			return err;
		}
	}

	/* Check if we have opened a local TSAP */
	if (!self->tsap)
		irda_open_tsap(self, LSAP_ANY, addr->sir_name);
	
	/* Move to connecting socket, start sending Connect Requests */
	sock->state = SS_CONNECTING;
	sk->state   = TCP_SYN_SENT;

	/* Connect to remote device */
	err = irttp_connect_request(self->tsap, self->dtsap_sel, 
				    self->saddr, self->daddr, NULL, 
				    self->max_sdu_size_rx, NULL);
	if (err) {
		IRDA_DEBUG(0, __FUNCTION__ "(), connect failed!\n");
		return err;
	}

	/* Now the loop */
	if (sk->state != TCP_ESTABLISHED && (flags & O_NONBLOCK))
		return -EINPROGRESS;
		
	cli();	/* To avoid races on the sleep */
	
	/* A Connect Ack with Choke or timeout or failed routing will go to
	 * closed.  */
	while (sk->state == TCP_SYN_SENT) {
		interruptible_sleep_on(sk->sleep);
		if (signal_pending(current)) {
			sti();
			return -ERESTARTSYS;
		}
	}
	
	if (sk->state != TCP_ESTABLISHED) {
		sti();
		sock->state = SS_UNCONNECTED;
		return sock_error(sk);	/* Always set at this point */
	}
	
	sock->state = SS_CONNECTED;
	
	sti();
	
	/* At this point, IrLMP has assigned our source address */
	self->saddr = irttp_get_saddr(self->tsap);

	return 0;
}

/*
 * Function irda_create (sock, protocol)
 *
 *    Create IrDA socket
 *
 */
static int irda_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	struct irda_sock *self;

	IRDA_DEBUG(2, __FUNCTION__ "()\n");
	
	/* Check for valid socket type */
	switch (sock->type) {
	case SOCK_STREAM:     /* For TTP connections with SAR disabled */
	case SOCK_SEQPACKET:  /* For TTP connections with SAR enabled */
	case SOCK_DGRAM:      /* For TTP Unitdata or LMP Ultra transfers */
		break;
	default:
		return -ESOCKTNOSUPPORT;
	}

	/* Allocate socket */
	if ((sk = sk_alloc(PF_IRDA, GFP_ATOMIC, 1)) == NULL)
		return -ENOMEM;
	
	self = kmalloc(sizeof(struct irda_sock), GFP_ATOMIC);
	if (self == NULL)
		return -ENOMEM;
	memset(self, 0, sizeof(struct irda_sock));

	init_waitqueue_head(&self->query_wait);

	self->sk = sk;
	sk->protinfo.irda = self;
	sock_init_data(sock, sk);

	switch (sock->type) {
	case SOCK_STREAM:
		sock->ops = &irda_stream_ops;
		self->max_sdu_size_rx = TTP_SAR_DISABLE;
		break;
	case SOCK_SEQPACKET:
		sock->ops = &irda_seqpacket_ops;
		self->max_sdu_size_rx = TTP_SAR_UNBOUND;
		break;
	case SOCK_DGRAM:
		switch (protocol) {
#ifdef CONFIG_IRDA_ULTRA
		case IRDAPROTO_ULTRA:
			sock->ops = &irda_ultra_ops;
			break;
#endif /* CONFIG_IRDA_ULTRA */
		case IRDAPROTO_UNITDATA:
			sock->ops = &irda_dgram_ops;
			/* We let Unitdata conn. be like seqpack conn. */
			self->max_sdu_size_rx = TTP_SAR_UNBOUND;
			break;
		default:
			ERROR(__FUNCTION__ "(), protocol not supported!\n");
			return -ESOCKTNOSUPPORT;
		}
		break;
	default:
		return -ESOCKTNOSUPPORT;
	}		

	sk->protocol = protocol;

	/* Register as a client with IrLMP */
	self->ckey = irlmp_register_client(0, NULL, NULL, NULL);
	self->mask = 0xffff;
	self->rx_flow = self->tx_flow = FLOW_START;
	self->nslots = DISCOVERY_DEFAULT_SLOTS;
	self->daddr = DEV_ADDR_ANY;	/* Until we get connected */
	self->saddr = 0x0;		/* so IrLMP assign us any link */

	MOD_INC_USE_COUNT;

	return 0;
}

/*
 * Function irda_destroy_socket (self)
 *
 *    Destroy socket
 *
 */
void irda_destroy_socket(struct irda_sock *self)
{
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

	ASSERT(self != NULL, return;);

	/* Unregister with IrLMP */
	irlmp_unregister_client(self->ckey);
	irlmp_unregister_service(self->skey);

	/* Unregister with LM-IAS */
	if (self->ias_obj) {
		irias_delete_object(self->ias_obj);
		self->ias_obj = NULL;
	}

	if (self->iriap) {
		iriap_close(self->iriap);
		self->iriap = NULL;
	}

	if (self->tsap) {
		irttp_disconnect_request(self->tsap, NULL, P_NORMAL);
		irttp_close_tsap(self->tsap);
		self->tsap = NULL;
	}
#ifdef CONFIG_IRDA_ULTRA
	if (self->lsap) {
		irlmp_close_lsap(self->lsap);
		self->lsap = NULL;
	}
#endif /* CONFIG_IRDA_ULTRA */
	kfree(self);
	MOD_DEC_USE_COUNT;
	
	return;
}

/*
 * Function irda_release (sock)
 *
 *    
 *
 */
static int irda_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	
	IRDA_DEBUG(2, __FUNCTION__ "()\n");

        if (sk == NULL) 
		return 0;
	
	sk->state       = TCP_CLOSE;
	sk->shutdown   |= SEND_SHUTDOWN;
	sk->state_change(sk);
	sk->dead        = 1;

	irda_destroy_socket(sk->protinfo.irda);

        sock->sk   = NULL;      
        sk->socket = NULL;      /* Not used, but we should do this. */

        return 0;
}

/*
 * Function irda_sendmsg (sock, msg, len, scm)
 *
 *    Send message down to TinyTP. This function is used for both STREAM and
 *    SEQPACK services. This is possible since it forces the client to 
 *    fragment the message if necessary
 */
static int irda_sendmsg(struct socket *sock, struct msghdr *msg, int len, 
			struct scm_cookie *scm)
{
	struct sock *sk = sock->sk;
	struct irda_sock *self;
	struct sk_buff *skb;
	unsigned char *asmptr;
	int err;

	IRDA_DEBUG(4, __FUNCTION__ "(), len=%d\n", len);

	/* Note : socket.c set MSG_EOR on SEQPACKET sockets */
	if (msg->msg_flags & ~(MSG_DONTWAIT | MSG_EOR))
		return -EINVAL;

	if (sk->shutdown & SEND_SHUTDOWN) {
		send_sig(SIGPIPE, current, 0);
		return -EPIPE;
	}

	if (sk->state != TCP_ESTABLISHED)
		return -ENOTCONN;

	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	/* Check if IrTTP is wants us to slow down */
	while (self->tx_flow == FLOW_STOP) {
		IRDA_DEBUG(2, __FUNCTION__ "(), IrTTP is busy, going to sleep!\n");
		interruptible_sleep_on(sk->sleep);
		
		/* Check if we are still connected */
		if (sk->state != TCP_ESTABLISHED)
			return -ENOTCONN;
	}

	/* Check that we don't send out to big frames */
	if (len > self->max_data_size) {
		IRDA_DEBUG(2, __FUNCTION__ 
			   "(), Chopping frame from %d to %d bytes!\n", len, 
			   self->max_data_size);
		len = self->max_data_size;
	}

	skb = sock_alloc_send_skb(sk, len + self->max_header_size, 0, 
				  msg->msg_flags & MSG_DONTWAIT, &err);
	if (!skb)
		return -ENOBUFS;

	skb_reserve(skb, self->max_header_size);
	
	asmptr = skb->h.raw = skb_put(skb, len);
	memcpy_fromiovec(asmptr, msg->msg_iov, len);

	/* 
	 * Just send the message to TinyTP, and let it deal with possible 
	 * errors. No need to duplicate all that here
	 */
	err = irttp_data_request(self->tsap, skb);
	if (err) {
		IRDA_DEBUG(0, __FUNCTION__ "(), err=%d\n", err);
		return err;
	}
	/* Tell client how much data we actually sent */
	return len;
}

/*
 * Function irda_recvmsg_dgram (sock, msg, size, flags, scm)
 *
 *    Try to receive message and copy it to user. The frame is discarded
 *    after being read, regardless of how much the user actually read
 */
static int irda_recvmsg_dgram(struct socket *sock, struct msghdr *msg, 
			      int size, int flags, struct scm_cookie *scm)
{
	struct irda_sock *self;
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int copied, err;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	skb = skb_recv_datagram(sk, flags & ~MSG_DONTWAIT, 
				flags & MSG_DONTWAIT, &err);
	if (!skb)
		return err;

	skb->h.raw = skb->data;
	copied     = skb->len;
	
	if (copied > size) {
		IRDA_DEBUG(2, __FUNCTION__ 
			   "(), Received truncated frame (%d < %d)!\n",
			   copied, size);
		copied = size;
		msg->msg_flags |= MSG_TRUNC;
	}
	skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);

	skb_free_datagram(sk, skb);

	/*
	 *  Check if we have previously stopped IrTTP and we know
	 *  have more free space in our rx_queue. If so tell IrTTP
	 *  to start delivering frames again before our rx_queue gets
	 *  empty
	 */
	if (self->rx_flow == FLOW_STOP) {
		if ((atomic_read(&sk->rmem_alloc) << 2) <= sk->rcvbuf) {
			IRDA_DEBUG(2, __FUNCTION__ "(), Starting IrTTP\n");
			self->rx_flow = FLOW_START;
			irttp_flow_request(self->tsap, FLOW_START);
		}
	}

	return copied;
}

/*
 * Function irda_data_wait (sk)
 *
 *    Sleep until data has arrive. But check for races..
 *
 */
static void irda_data_wait(struct sock *sk)
{
	if (!skb_peek(&sk->receive_queue)) {
		set_bit(SOCK_ASYNC_WAITDATA, &sk->socket->flags);
		interruptible_sleep_on(sk->sleep);
		clear_bit(SOCK_ASYNC_WAITDATA, &sk->socket->flags);
	}
}

/*
 * Function irda_recvmsg_stream (sock, msg, size, flags, scm)
 *
 *    
 *
 */
static int irda_recvmsg_stream(struct socket *sock, struct msghdr *msg, 
			       int size, int flags, struct scm_cookie *scm)
{
	struct irda_sock *self;
	struct sock *sk = sock->sk;
	int noblock = flags & MSG_DONTWAIT;
	int copied = 0;
	int target = 1;

	IRDA_DEBUG(3, __FUNCTION__ "()\n");

	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	if (sock->flags & __SO_ACCEPTCON) 
		return(-EINVAL);

	if (flags & MSG_OOB)
		return -EOPNOTSUPP;

	if (flags & MSG_WAITALL)
		target = size;
		
	msg->msg_namelen = 0;

	do {
		int chunk;
		struct sk_buff *skb;

		skb=skb_dequeue(&sk->receive_queue);
		if (skb==NULL) {
			if (copied >= target)
				break;
			
			/*
			 *	POSIX 1003.1g mandates this order.
			 */
			
			if (sk->err) {
				return sock_error(sk);
			}

			if (sk->shutdown & RCV_SHUTDOWN)
				break;

			if (noblock)
				return -EAGAIN;
			irda_data_wait(sk);
			if (signal_pending(current))
				return -ERESTARTSYS;
			continue;
		}

		chunk = min(skb->len, size);
		if (memcpy_toiovec(msg->msg_iov, skb->data, chunk)) {
			skb_queue_head(&sk->receive_queue, skb);
			if (copied == 0)
				copied = -EFAULT;
			break;
		}
		copied += chunk;
		size -= chunk;

		/* Mark read part of skb as used */
		if (!(flags & MSG_PEEK)) {
			skb_pull(skb, chunk);

			/* put the skb back if we didn't use it up.. */
			if (skb->len) {
				IRDA_DEBUG(1, __FUNCTION__ "(), back on q!\n");
				skb_queue_head(&sk->receive_queue, skb);
				break;
			}

			kfree_skb(skb);			
		} else {
			IRDA_DEBUG(0, __FUNCTION__ "() questionable!?\n");

			/* put message back and return */
			skb_queue_head(&sk->receive_queue, skb);
			break;
		}
	} while (size);

	/*
	 *  Check if we have previously stopped IrTTP and we know
	 *  have more free space in our rx_queue. If so tell IrTTP
	 *  to start delivering frames again before our rx_queue gets
	 *  empty
	 */
	if (self->rx_flow == FLOW_STOP) {
		if ((atomic_read(&sk->rmem_alloc) << 2) <= sk->rcvbuf) {
			IRDA_DEBUG(2, __FUNCTION__ "(), Starting IrTTP\n");
			self->rx_flow = FLOW_START;
			irttp_flow_request(self->tsap, FLOW_START);
		}
	}

	return copied;
}

/*
 * Function irda_sendmsg_dgram (sock, msg, len, scm)
 *
 *    Send message down to TinyTP for the unreliable sequenced
 *    packet service...
 *
 */
static int irda_sendmsg_dgram(struct socket *sock, struct msghdr *msg,
			      int len, struct scm_cookie *scm)
{
	struct sock *sk = sock->sk;
	struct irda_sock *self;
	struct sk_buff *skb;
	unsigned char *asmptr;
	int err;
	
	IRDA_DEBUG(4, __FUNCTION__ "(), len=%d\n", len);
	
	if (msg->msg_flags & ~MSG_DONTWAIT)
		return -EINVAL;

	if (sk->shutdown & SEND_SHUTDOWN) {
		send_sig(SIGPIPE, current, 0);
		return -EPIPE;
	}

	if (sk->state != TCP_ESTABLISHED)
		return -ENOTCONN;

	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	/*  
	 * Check that we don't send out to big frames. This is an unreliable 
	 * service, so we have no fragmentation and no coalescence 
	 */
	if (len > self->max_data_size) {
		IRDA_DEBUG(0, __FUNCTION__ "(), Warning to much data! "
			   "Chopping frame from %d to %d bytes!\n", len, 
			   self->max_data_size);
		len = self->max_data_size;
	}

	skb = sock_alloc_send_skb(sk, len + self->max_header_size, 0, 
				  msg->msg_flags & MSG_DONTWAIT, &err);
	if (!skb)
		return -ENOBUFS;

	skb_reserve(skb, self->max_header_size);
	
	IRDA_DEBUG(4, __FUNCTION__ "(), appending user data\n");
	asmptr = skb->h.raw = skb_put(skb, len);
	memcpy_fromiovec(asmptr, msg->msg_iov, len);

	/* 
	 * Just send the message to TinyTP, and let it deal with possible 
	 * errors. No need to duplicate all that here
	 */
	err = irttp_udata_request(self->tsap, skb);
	if (err) {
		IRDA_DEBUG(0, __FUNCTION__ "(), err=%d\n", err);
		return err;
	}
	return len;
}

/*
 * Function irda_sendmsg_ultra (sock, msg, len, scm)
 *
 *    Send message down to IrLMP for the unreliable Ultra
 *    packet service...
 */
#ifdef CONFIG_IRDA_ULTRA
static int irda_sendmsg_ultra(struct socket *sock, struct msghdr *msg,
			      int len, struct scm_cookie *scm)
{
	struct sock *sk = sock->sk;
	struct irda_sock *self;
	struct sk_buff *skb;
	unsigned char *asmptr;
	int err;
	
	IRDA_DEBUG(4, __FUNCTION__ "(), len=%d\n", len);
	
	if (msg->msg_flags & ~MSG_DONTWAIT)
		return -EINVAL;

	if (sk->shutdown & SEND_SHUTDOWN) {
		send_sig(SIGPIPE, current, 0);
		return -EPIPE;
	}

	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	/*  
	 * Check that we don't send out to big frames. This is an unreliable 
	 * service, so we have no fragmentation and no coalescence 
	 */
	if (len > self->max_data_size) {
		IRDA_DEBUG(0, __FUNCTION__ "(), Warning to much data! "
			   "Chopping frame from %d to %d bytes!\n", len, 
			   self->max_data_size);
		len = self->max_data_size;
	}

	skb = sock_alloc_send_skb(sk, len + self->max_header_size, 0, 
				  msg->msg_flags & MSG_DONTWAIT, &err);
	if (!skb)
		return -ENOBUFS;

	skb_reserve(skb, self->max_header_size);
	
	IRDA_DEBUG(4, __FUNCTION__ "(), appending user data\n");
	asmptr = skb->h.raw = skb_put(skb, len);
	memcpy_fromiovec(asmptr, msg->msg_iov, len);

	err = irlmp_connless_data_request(self->lsap, skb);
	if (err) {
		IRDA_DEBUG(0, __FUNCTION__ "(), err=%d\n", err);
		return err;
	}
	return len;
}
#endif /* CONFIG_IRDA_ULTRA */

/*
 * Function irda_shutdown (sk, how)
 *
 *    
 *
 */
static int irda_shutdown(struct socket *sock, int how)
{
	struct irda_sock *self;
	struct sock *sk = sock->sk;

	IRDA_DEBUG(0, __FUNCTION__ "()\n");

	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	sk->state       = TCP_CLOSE;
	sk->shutdown   |= SEND_SHUTDOWN;
	sk->state_change(sk);

	if (self->iriap) {
		iriap_close(self->iriap);
		self->iriap = NULL;
	}

	if (self->tsap) {
		irttp_disconnect_request(self->tsap, NULL, P_NORMAL);
		irttp_close_tsap(self->tsap);
		self->tsap = NULL;
	}

	/* A few cleanup so the socket look as good as new... */
	self->rx_flow = self->tx_flow = FLOW_START;	/* needed ??? */
	self->daddr = DEV_ADDR_ANY;	/* Until we get re-connected */
	self->saddr = 0x0;		/* so IrLMP assign us any link */

        return 0;
}

/*
 * Function irda_poll (file, sock, wait)
 *
 *    
 *
 */
static unsigned int irda_poll(struct file * file, struct socket *sock, 
			      poll_table *wait)
{
	struct sock *sk = sock->sk;
	unsigned int mask;

	IRDA_DEBUG(4, __FUNCTION__ "()\n");

	poll_wait(file, sk->sleep, wait);
	mask = 0;

	/* exceptional events? */
	if (sk->err)
		mask |= POLLERR;
	if (sk->shutdown & RCV_SHUTDOWN)
		mask |= POLLHUP;

	/* readable? */
	if (!skb_queue_empty(&sk->receive_queue)) {
		IRDA_DEBUG(4, "Socket is readable\n");
		mask |= POLLIN | POLLRDNORM;
	}
	/* Connection-based need to check for termination and startup */
	if (sk->type == SOCK_STREAM && sk->state==TCP_CLOSE)
		mask |= POLLHUP;

	/*
	 * we set writable also when the other side has shut down the
	 * connection. This prevents stuck sockets.
	 */
	if (sk->sndbuf - (int)atomic_read(&sk->wmem_alloc) >= SOCK_MIN_WRITE_SPACE)
			mask |= POLLOUT | POLLWRNORM | POLLWRBAND;

	return mask;
}

/*
 * Function irda_ioctl (sock, cmd, arg)
 *
 *    
 *
 */
static int irda_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = sock->sk;

	IRDA_DEBUG(4, __FUNCTION__ "(), cmd=%#x\n", cmd);
	
	switch (cmd) {
	case TIOCOUTQ: {
		long amount;
		amount = sk->sndbuf - atomic_read(&sk->wmem_alloc);
		if (amount < 0)
			amount = 0;
		if (put_user(amount, (unsigned int *)arg))
			return -EFAULT;
		return 0;
	}
	
	case TIOCINQ: {
		struct sk_buff *skb;
		long amount = 0L;
		/* These two are safe on a single CPU system as only user tasks fiddle here */
		if ((skb = skb_peek(&sk->receive_queue)) != NULL)
			amount = skb->len;
		if (put_user(amount, (unsigned int *)arg))
			return -EFAULT;
		return 0;
	}
	
	case SIOCGSTAMP:
		if (sk != NULL) {
			if (sk->stamp.tv_sec == 0)
				return -ENOENT;
			if (copy_to_user((void *)arg, &sk->stamp, 
					 sizeof(struct timeval)))
				return -EFAULT;
			return 0;
		}
		return -EINVAL;
		
	case SIOCGIFADDR:
	case SIOCSIFADDR:
	case SIOCGIFDSTADDR:
	case SIOCSIFDSTADDR:
	case SIOCGIFBRDADDR:
	case SIOCSIFBRDADDR:
	case SIOCGIFNETMASK:
	case SIOCSIFNETMASK:
	case SIOCGIFMETRIC:
	case SIOCSIFMETRIC:
		return -EINVAL;		
	default:
		IRDA_DEBUG(1, __FUNCTION__ "(), doing device ioctl!\n");
		return dev_ioctl(cmd, (void *) arg);
	}

	/*NOTREACHED*/
	return 0;
}

/*
 * Function irda_setsockopt (sock, level, optname, optval, optlen)
 *
 *    Set some options for the socket
 *
 */
static int irda_setsockopt(struct socket *sock, int level, int optname, 
			   char *optval, int optlen)
{
 	struct sock *sk = sock->sk;
	struct irda_sock *self;
	struct irda_ias_set	ias_opt;
	struct ias_object      *ias_obj;
	struct ias_attrib *	ias_attr;	/* Attribute in IAS object */
	int opt;
	
	self = sk->protinfo.irda;
	ASSERT(self != NULL, return -1;);

	if (level != SOL_IRLMP)
		return -ENOPROTOOPT;
		
	switch (optname) {
	case IRLMP_IAS_SET:
		/* The user want to add an attribute to an existing IAS object
		 * (in the IAS database) or to create a new object with this
		 * attribute.
		 * We first query IAS to know if the object exist, and then
		 * create the right attribute...
		 */

		if (optlen != sizeof(struct irda_ias_set))
			return -EINVAL;
	
		/* Copy query to the driver. */
		if (copy_from_user(&ias_opt, (char *)optval, optlen))
		  	return -EFAULT;

		/* Find the object we target */
		ias_obj = irias_find_object(ias_opt.irda_class_name);
		if(ias_obj == (struct ias_object *) NULL) {
			/* Create a new object */
			ias_obj = irias_new_object(ias_opt.irda_class_name,
						   jiffies);
		}

		/* Do we have it already ? */
		if(irias_find_attrib(ias_obj, ias_opt.irda_attrib_name))
			return -EINVAL;

		/* Look at the type */
		switch(ias_opt.irda_attrib_type) {
		case IAS_INTEGER:
			/* Add an integer attribute */
			irias_add_integer_attrib(
				ias_obj,
				ias_opt.irda_attrib_name, 
				ias_opt.attribute.irda_attrib_int,
				IAS_USER_ATTR);
			break;
		case IAS_OCT_SEQ:
			/* Check length */
			if(ias_opt.attribute.irda_attrib_octet_seq.len >
			   IAS_MAX_OCTET_STRING)
				return -EINVAL;
			/* Add an octet sequence attribute */
			irias_add_octseq_attrib(
			      ias_obj,
			      ias_opt.irda_attrib_name, 
			      ias_opt.attribute.irda_attrib_octet_seq.octet_seq,
			      ias_opt.attribute.irda_attrib_octet_seq.len,
			      IAS_USER_ATTR);
			break;
		case IAS_STRING:
			/* Should check charset & co */
			/* Check length */
			if(ias_opt.attribute.irda_attrib_string.len >
			   IAS_MAX_STRING)
				return -EINVAL;
			/* NULL terminate the string (avoid troubles) */
			ias_opt.attribute.irda_attrib_string.string[ias_opt.attribute.irda_attrib_string.len] = '\0';
			/* Add a string attribute */
			irias_add_string_attrib(
				ias_obj,
				ias_opt.irda_attrib_name, 
				ias_opt.attribute.irda_attrib_string.string,
				IAS_USER_ATTR);
			break;
		default :
			return -EINVAL;
		}
		irias_insert_object(ias_obj);
		break;
	case IRLMP_IAS_DEL:
		/* The user want to delete an object from our local IAS
		 * database. We just need to query the IAS, check is the
		 * object is not owned by the kernel and delete it.
		 */

		if (optlen != sizeof(struct irda_ias_set))
			return -EINVAL;
	
		/* Copy query to the driver. */
		if (copy_from_user(&ias_opt, (char *)optval, optlen))
		  	return -EFAULT;

		/* Find the object we target */
		ias_obj = irias_find_object(ias_opt.irda_class_name);
		if(ias_obj == (struct ias_object *) NULL)
			return -EINVAL;

		/* Find the attribute (in the object) we target */
		ias_attr = irias_find_attrib(ias_obj,
					     ias_opt.irda_attrib_name); 
		if(ias_attr == (struct ias_attrib *) NULL)
			return -EINVAL;

		/* Check is the user space own the object */
		if(ias_attr->value->owner != IAS_USER_ATTR) {
			IRDA_DEBUG(1, __FUNCTION__ 
				   "(), attempting to delete a kernel attribute\n");
			return -EPERM;
		}

		/* Remove the attribute (and maybe the object) */
		irias_delete_attrib(ias_obj, ias_attr);

		break;
	case IRLMP_MAX_SDU_SIZE:
		if (optlen < sizeof(int))
			return -EINVAL;
	
		if (get_user(opt, (int *)optval))
			return -EFAULT;
		
		/* Only possible for a seqpacket service (TTP with SAR) */
		if (sk->type != SOCK_SEQPACKET) {
			IRDA_DEBUG(2, __FUNCTION__ 
				   "(), setting max_sdu_size = %d\n", opt);
			self->max_sdu_size_rx = opt;
		} else {
			WARNING(__FUNCTION__ 
				"(), not allowed to set MAXSDUSIZE for this "
				"socket type!\n");
			return -ENOPROTOOPT;
		}
		break;
	case IRLMP_HINTS_SET:
		if (optlen < sizeof(int))
			return -EINVAL;
	
		if (get_user(opt, (int *)optval))
			return -EFAULT;

		/* Unregister any old registration */
		if (self->skey)
			irlmp_unregister_service(self->skey);

		self->skey = irlmp_register_service((__u16) opt);
		break;
	case IRLMP_HINT_MASK_SET:
		/* As opposed to the previous case which set the hint bits
		 * that we advertise, this one set the filter we use when
		 * making a discovery (nodes which don't match any hint
		 * bit in the mask are not reported).
		 */
		if (optlen < sizeof(int))
			return -EINVAL;
	
		if (get_user(opt, (int *)optval))
			return -EFAULT;

		/* Set the new hint mask */
		self->mask = (__u16) opt;
		/* Mask out extension bits */
		self->mask &= 0x7f7f;
		/* Check if no bits */
		if(!self->mask)
			self->mask = 0xFFFF;

		break;
	default:
		return -ENOPROTOOPT;
	}
	return 0;
}

/*
 * Function irda_extract_ias_value(ias_opt, ias_value)
 *
 *    Translate internal IAS value structure to the user space representation
 *
 * The external representation of IAS values, as we exchange them with
 * user space program is quite different from the internal representation,
 * as stored in the IAS database (because we need a flat structure for
 * crossing kernel boundary).
 * This function transform the former in the latter. We also check
 * that the value type is valid.
 */
static int irda_extract_ias_value(struct irda_ias_set *ias_opt,
				  struct ias_value *ias_value)
{
	/* Look at the type */
	switch (ias_value->type) {
	case IAS_INTEGER:
		/* Copy the integer */
		ias_opt->attribute.irda_attrib_int = ias_value->t.integer;
		break;
	case IAS_OCT_SEQ:
		/* Set length */
		ias_opt->attribute.irda_attrib_octet_seq.len = ias_value->len;
		/* Copy over */
		memcpy(ias_opt->attribute.irda_attrib_octet_seq.octet_seq,
		       ias_value->t.oct_seq, ias_value->len);
		break;
	case IAS_STRING:
		/* Set length */
		ias_opt->attribute.irda_attrib_string.len = ias_value->len;
		ias_opt->attribute.irda_attrib_string.charset = ias_value->charset;
		/* Copy over */
		memcpy(ias_opt->attribute.irda_attrib_string.string,
		       ias_value->t.string, ias_value->len);
		/* NULL terminate the string (avoid troubles) */
		ias_opt->attribute.irda_attrib_string.string[ias_value->len] = '\0';
		break;
	case IAS_MISSING:
	default :
		return -EINVAL;
	}
	
	/* Copy type over */
	ias_opt->irda_attrib_type = ias_value->type;
	
	return 0;
}

/*
 * Function irda_getsockopt (sock, level, optname, optval, optlen)
 *
 *    
 *
 */
static int irda_getsockopt(struct socket *sock, int level, int optname, 
			   char *optval, int *optlen)
{
	struct sock *sk = sock->sk;
	struct irda_sock *self;
	struct irda_device_list list;
	struct irda_device_info *discoveries;
	struct irda_ias_set	ias_opt;	/* IAS get/query params */
	struct ias_object *	ias_obj;	/* Object in IAS */
	struct ias_attrib *	ias_attr;	/* Attribute in IAS object */
	int daddr = DEV_ADDR_ANY;	/* Dest address for IAS queries */
	int val = 0;
	int len = 0;
	int err;
	int offset, total;

	self = sk->protinfo.irda;

	if (level != SOL_IRLMP)
		return -ENOPROTOOPT;

	if (get_user(len, optlen))
		return -EFAULT;

	switch (optname) {
	case IRLMP_ENUMDEVICES:
		/* Ask lmp for the current discovery log */
		discoveries = irlmp_get_discoveries(&list.len, self->mask);
		/* Check if the we got some results */
		if (discoveries == NULL)
			return -EAGAIN;		/* Didn't find any devices */
		err = 0;

		/* Write total list length back to client */
		if (copy_to_user(optval, &list, 
				 sizeof(struct irda_device_list) -
				 sizeof(struct irda_device_info)))
			err = -EFAULT;

		/* Offset to first device entry */
		offset = sizeof(struct irda_device_list) - 
			sizeof(struct irda_device_info);

		/* Copy the list itself */
		total = offset + (list.len * sizeof(struct irda_device_info));
		if (total > len)
			total = len;
		if (copy_to_user(optval+offset, discoveries, total - offset))
			err = -EFAULT;

		/* Write total number of bytes used back to client */
		if (put_user(total, optlen))
			err = -EFAULT;

		/* Free up our buffer */
		kfree(discoveries);
		if (err)
			return err;
		break;
	case IRLMP_MAX_SDU_SIZE:
		val = self->max_data_size;
		len = sizeof(int);
		if (put_user(len, optlen))
			return -EFAULT;
		
		if (copy_to_user(optval, &val, len))
			return -EFAULT;
		break;
	case IRLMP_IAS_GET:
		/* The user want an object from our local IAS database.
		 * We just need to query the IAS and return the value
		 * that we found */

		/* Check that the user has allocated the right space for us */
		if (len != sizeof(ias_opt))
			return -EINVAL;

		/* Copy query to the driver. */
		if (copy_from_user((char *) &ias_opt, (char *)optval, len))
		  	return -EFAULT;

		/* Find the object we target */
		ias_obj = irias_find_object(ias_opt.irda_class_name);
		if(ias_obj == (struct ias_object *) NULL)
			return -EINVAL;

		/* Find the attribute (in the object) we target */
		ias_attr = irias_find_attrib(ias_obj,
					     ias_opt.irda_attrib_name); 
		if(ias_attr == (struct ias_attrib *) NULL)
			return -EINVAL;

		/* Translate from internal to user structure */
		err = irda_extract_ias_value(&ias_opt, ias_attr->value);
		if(err)
			return err;

		/* Copy reply to the user */
		if (copy_to_user((char *)optval, (char *) &ias_opt,
				 sizeof(ias_opt)))
		  	return -EFAULT;
		/* Note : don't need to put optlen, we checked it */
		break;
	case IRLMP_IAS_QUERY:
		/* The user want an object from a remote IAS database.
		 * We need to use IAP to query the remote database and
		 * then wait for the answer to come back. */

		/* Check that the user has allocated the right space for us */
		if (len != sizeof(ias_opt))
			return -EINVAL;

		/* Copy query to the driver. */
		if (copy_from_user((char *) &ias_opt, (char *)optval, len))
		  	return -EFAULT;

		/* At this point, there are two cases...
		 * 1) the socket is connected - that's the easy case, we
		 *	just query the device we are connected to...
		 * 2) the socket is not connected - the user doesn't want
		 *	to connect and/or may not have a valid service name
		 *	(so can't create a fake connection). In this case,
		 *	we assume that the user pass us a valid destination
		 *	address in the requesting structure...
		 */
		if(self->daddr != DEV_ADDR_ANY) {
			/* We are connected - reuse known daddr */
			daddr = self->daddr;
		} else {
			/* We are not connected, we must specify a valid
			 * destination address */
			daddr = ias_opt.daddr;
			if((!daddr) || (daddr == DEV_ADDR_ANY))
				return -EINVAL;
		}

		/* Check that we can proceed with IAP */
		if (self->iriap) {
			WARNING(__FUNCTION__
				"(), busy with a previous query\n");
			return -EBUSY;
		}

		self->iriap = iriap_open(LSAP_ANY, IAS_CLIENT, self,
					 irda_getvalue_confirm);

		/* Treat unexpected signals as disconnect */
		self->errno = -EHOSTUNREACH;

		/* Query remote LM-IAS */
		iriap_getvaluebyclass_request(self->iriap,
					      self->saddr, daddr,
					      ias_opt.irda_class_name,
					      ias_opt.irda_attrib_name);
		/* Wait for answer (if not already failed) */
		if(self->iriap != NULL)
			interruptible_sleep_on(&self->query_wait);
		/* Check what happened */
		if (self->errno)
		{
			/* Requested object/attribute doesn't exist */
			if((self->errno == IAS_CLASS_UNKNOWN) ||
			   (self->errno == IAS_ATTRIB_UNKNOWN))
				return (-EADDRNOTAVAIL);
			else
				return (-EHOSTUNREACH);
		}

		/* Translate from internal to user structure */
		err = irda_extract_ias_value(&ias_opt, self->ias_result);
		if (self->ias_result)
			irias_delete_value(self->ias_result);
		if (err)
			return err;

		/* Copy reply to the user */
		if (copy_to_user((char *)optval, (char *) &ias_opt,
				 sizeof(ias_opt)))
		  	return -EFAULT;
		/* Note : don't need to put optlen, we checked it */
		break;
	case IRLMP_WAITDEVICE:
		/* This function is just another way of seeing life ;-)
		 * IRLMP_ENUMDEVICES assumes that you have a static network,
		 * and that you just want to pick one of the devices present.
		 * On the other hand, in here we assume that no device is
		 * present and that at some point in the future a device will
		 * come into range. When this device arrive, we just wake
		 * up the caller, so that he has time to connect to it before
		 * the device goes away...
		 * Note : once the node has been discovered for more than a
		 * few second, it won't trigger this function, unless it
		 * goes away and come back changes its hint bits (so we
		 * might call it IRLMP_WAITNEWDEVICE).
		 */

		/* Check that the user is passing us an int */
		if (len != sizeof(int))
			return -EINVAL;
		/* Get timeout in ms (max time we block the caller) */
		if (get_user(val, (int *)optval))
			return -EFAULT;

		/* Tell IrLMP we want to be notified */
		irlmp_update_client(self->ckey, self->mask,
				    irda_selective_discovery_indication,
				    NULL, (void *) self);
		
		/* Do some discovery (and also return cached results) */
		irlmp_discovery_request(self->nslots);
		
		/* Wait until a node is discovered */
		if (!self->cachediscovery) {
			IRDA_DEBUG(1, __FUNCTION__ 
				   "(), nothing discovered yet, going to sleep...\n");

			/* Set watchdog timer to expire in <val> ms. */
			self->watchdog.function = irda_discovery_timeout;
			self->watchdog.data = (unsigned long) self;
			self->watchdog.expires = jiffies + (val * HZ/1000);
			add_timer(&(self->watchdog));

			/* Wait for IR-LMP to call us back */
			interruptible_sleep_on(&self->query_wait);

			/* If watchdog is still activated, kill it! */
			if(timer_pending(&(self->watchdog)))
				del_timer(&(self->watchdog));

			IRDA_DEBUG(1, __FUNCTION__ 
				   "(), ...waking up !\n");
		}
		else
			IRDA_DEBUG(1, __FUNCTION__ 
				   "(), found immediately !\n");

		/* Tell IrLMP that we have been notified */
		irlmp_update_client(self->ckey, self->mask, NULL, NULL, NULL);

		/* Check if the we got some results */
		if (!self->cachediscovery)
			return -EAGAIN;		/* Didn't find any devices */
		/* Cleanup */
		self->cachediscovery = NULL;

		/* Note : We don't return anything to the user.
		 * We could return the device that triggered the wake up,
		 * but it's probably better to force the user to query
		 * the whole discovery log and let him pick one device...
		 */
		break;
	default:
		return -ENOPROTOOPT;
	}
	
	return 0;
}

static struct net_proto_family irda_family_ops =
{
	PF_IRDA,
	irda_create
};

static struct proto_ops SOCKOPS_WRAPPED(irda_stream_ops) = {
	family:		PF_IRDA,
	
	release:	irda_release,
	bind:		irda_bind,
	connect:	irda_connect,
	socketpair:	sock_no_socketpair,
	accept:		irda_accept,
	getname:	irda_getname,
	poll:		irda_poll,
	ioctl:		irda_ioctl,
	listen:		irda_listen,
	shutdown:	irda_shutdown,
	setsockopt:	irda_setsockopt,
	getsockopt:	irda_getsockopt,
	sendmsg:	irda_sendmsg,
	recvmsg:	irda_recvmsg_stream,
	mmap:		sock_no_mmap,
};

static struct proto_ops SOCKOPS_WRAPPED(irda_seqpacket_ops) = {
	family:		PF_IRDA,
	
	release:	irda_release,
	bind:		irda_bind,
	connect:	irda_connect,
	socketpair:	sock_no_socketpair,
	accept:		irda_accept,
	getname:	irda_getname,
	poll:		datagram_poll,
	ioctl:		irda_ioctl,
	listen:		irda_listen,
	shutdown:	irda_shutdown,
	setsockopt:	irda_setsockopt,
	getsockopt:	irda_getsockopt,
	sendmsg:	irda_sendmsg,
	recvmsg:	irda_recvmsg_dgram,
	mmap:		sock_no_mmap,
};

static struct proto_ops SOCKOPS_WRAPPED(irda_dgram_ops) = {
	family:		PF_IRDA,
       
	release:	irda_release,
	bind:		irda_bind,
	connect:	irda_connect,
	socketpair:	sock_no_socketpair,
	accept:		irda_accept,
	getname:	irda_getname,
	poll:		datagram_poll,
	ioctl:		irda_ioctl,
	listen:		irda_listen,
	shutdown:	irda_shutdown,
	setsockopt:	irda_setsockopt,
	getsockopt:	irda_getsockopt,
	sendmsg:	irda_sendmsg_dgram,
	recvmsg:	irda_recvmsg_dgram,
	mmap:		sock_no_mmap,
};

#ifdef CONFIG_IRDA_ULTRA
static struct proto_ops SOCKOPS_WRAPPED(irda_ultra_ops) = {
	family:		PF_IRDA,
       
	release:	irda_release,
	bind:		irda_bind,
	connect:	sock_no_connect,
	socketpair:	sock_no_socketpair,
	accept:		sock_no_accept,
	getname:	irda_getname,
	poll:		datagram_poll,
	ioctl:		irda_ioctl,
	listen:		sock_no_listen,
	shutdown:	irda_shutdown,
	setsockopt:	irda_setsockopt,
	getsockopt:	irda_getsockopt,
	sendmsg:	irda_sendmsg_ultra,
	recvmsg:	irda_recvmsg_dgram,
	mmap:		sock_no_mmap,
};
#endif /* CONFIG_IRDA_ULTRA */

#include <linux/smp_lock.h>
SOCKOPS_WRAP(irda_stream, PF_IRDA);
SOCKOPS_WRAP(irda_seqpacket, PF_IRDA);
SOCKOPS_WRAP(irda_dgram, PF_IRDA);

/*
 * Function irda_device_event (this, event, ptr)
 *
 *    Called when a device is taken up or down
 *
 */
static int irda_device_event(struct notifier_block *this, unsigned long event,
			     void *ptr)
{
	struct net_device *dev = (struct net_device *) ptr;
	
        /* Reject non IrDA devices */
	if (dev->type != ARPHRD_IRDA) 
		return NOTIFY_DONE;
	
        switch (event) {
	case NETDEV_UP:
		IRDA_DEBUG(3, __FUNCTION__ "(), NETDEV_UP\n");
		/* irda_dev_device_up(dev); */
		break;
	case NETDEV_DOWN:
		IRDA_DEBUG(3, __FUNCTION__ "(), NETDEV_DOWN\n");
		/* irda_kill_by_device(dev); */
		/* irda_rt_device_down(dev); */
		/* irda_dev_device_down(dev); */
		break;
	default:
		break;
        }

        return NOTIFY_DONE;
}

static struct packet_type irda_packet_type = 
{
	0,	/* MUTTER ntohs(ETH_P_IRDA),*/
	NULL,
	irlap_driver_rcv,
	NULL,
	NULL,
};

static struct notifier_block irda_dev_notifier = {
	irda_device_event,
	NULL,
	0
};

/*
 * Function irda_proc_modcount (inode, fill)
 *
 *    Use by the proc file system functions to prevent the irda module
 *    being removed while the use is standing in the net/irda directory
 */
void irda_proc_modcount(struct inode *inode, int fill)
{
#ifdef MODULE
#ifdef CONFIG_PROC_FS
	if (fill)
		MOD_INC_USE_COUNT;
	else
		MOD_DEC_USE_COUNT;
#endif /* CONFIG_PROC_FS */
#endif /* MODULE */
}

/*
 * Function irda_proto_init (pro)
 *
 *    Initialize IrDA protocol layer
 *
 */
int __init irda_proto_init(void)
{
	sock_register(&irda_family_ops);

	irda_packet_type.type = htons(ETH_P_IRDA);
        dev_add_pack(&irda_packet_type);

	register_netdevice_notifier(&irda_dev_notifier);

	irda_init();
#ifdef MODULE
 	irda_device_init();  /* Called by init/main.c when non-modular */
#endif
	return 0;
}

/*
 * Function irda_proto_cleanup (void)
 *
 *    Remove IrDA protocol layer
 *
 */
#ifdef MODULE
void irda_proto_cleanup(void)
{
	irda_packet_type.type = htons(ETH_P_IRDA);
        dev_remove_pack(&irda_packet_type);

        unregister_netdevice_notifier(&irda_dev_notifier);
	
	sock_unregister(PF_IRDA);
	irda_cleanup();
	
        return;
}
module_init(irda_proto_init);
module_exit(irda_proto_cleanup);
 
MODULE_AUTHOR("Dag Brattli <dagb@cs.uit.no>");
MODULE_DESCRIPTION("The Linux IrDA Protocol Subsystem"); 
MODULE_PARM(irda_debug, "1l");
#endif /* MODULE */

