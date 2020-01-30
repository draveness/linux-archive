/* SCTP kernel reference Implementation
 * (C) Copyright IBM Corp. 2001, 2004
 * Copyright (c) 1999-2000 Cisco, Inc.
 * Copyright (c) 1999-2001 Motorola, Inc.
 * Copyright (c) 2001 Intel Corp.
 * Copyright (c) 2001 La Monte H.P. Yarroll
 *
 * This file is part of the SCTP kernel reference Implementation
 *
 * This module provides the abstraction for an SCTP association.
 *
 * The SCTP reference implementation is free software;
 * you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * The SCTP reference implementation is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 *                 ************************
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Please send any bug reports or fixes you make to the
 * email address(es):
 *    lksctp developers <lksctp-developers@lists.sourceforge.net>
 *
 * Or submit a bug report through the following website:
 *    http://www.sf.net/projects/lksctp
 *
 * Written or modified by:
 *    La Monte H.P. Yarroll <piggy@acm.org>
 *    Karl Knutson          <karl@athena.chicago.il.us>
 *    Jon Grimm             <jgrimm@us.ibm.com>
 *    Xingang Guo           <xingang.guo@intel.com>
 *    Hui Huang             <hui.huang@nokia.com>
 *    Sridhar Samudrala	    <sri@us.ibm.com>
 *    Daisy Chang	    <daisyc@us.ibm.com>
 *    Ryan Layer	    <rmlayer@us.ibm.com>
 *    Kevin Gao             <kevin.gao@intel.com>
 *
 * Any bugs reported given to us we will try to fix... any fixes shared will
 * be incorporated into the next SCTP release.
 */

#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/init.h>

#include <linux/slab.h>
#include <linux/in.h>
#include <net/ipv6.h>
#include <net/sctp/sctp.h>
#include <net/sctp/sm.h>

/* Forward declarations for internal functions. */
static void sctp_assoc_bh_rcv(struct work_struct *work);


/* 1st Level Abstractions. */

/* Initialize a new association from provided memory. */
static struct sctp_association *sctp_association_init(struct sctp_association *asoc,
					  const struct sctp_endpoint *ep,
					  const struct sock *sk,
					  sctp_scope_t scope,
					  gfp_t gfp)
{
	struct sctp_sock *sp;
	int i;

	/* Retrieve the SCTP per socket area.  */
	sp = sctp_sk((struct sock *)sk);

	/* Init all variables to a known value.  */
	memset(asoc, 0, sizeof(struct sctp_association));

	/* Discarding const is appropriate here.  */
	asoc->ep = (struct sctp_endpoint *)ep;
	sctp_endpoint_hold(asoc->ep);

	/* Hold the sock.  */
	asoc->base.sk = (struct sock *)sk;
	sock_hold(asoc->base.sk);

	/* Initialize the common base substructure.  */
	asoc->base.type = SCTP_EP_TYPE_ASSOCIATION;

	/* Initialize the object handling fields.  */
	atomic_set(&asoc->base.refcnt, 1);
	asoc->base.dead = 0;
	asoc->base.malloced = 0;

	/* Initialize the bind addr area.  */
	sctp_bind_addr_init(&asoc->base.bind_addr, ep->base.bind_addr.port);

	asoc->state = SCTP_STATE_CLOSED;

	/* Set these values from the socket values, a conversion between
	 * millsecons to seconds/microseconds must also be done.
	 */
	asoc->cookie_life.tv_sec = sp->assocparams.sasoc_cookie_life / 1000;
	asoc->cookie_life.tv_usec = (sp->assocparams.sasoc_cookie_life % 1000)
					* 1000;
	asoc->frag_point = 0;

	/* Set the association max_retrans and RTO values from the
	 * socket values.
	 */
	asoc->max_retrans = sp->assocparams.sasoc_asocmaxrxt;
	asoc->rto_initial = msecs_to_jiffies(sp->rtoinfo.srto_initial);
	asoc->rto_max = msecs_to_jiffies(sp->rtoinfo.srto_max);
	asoc->rto_min = msecs_to_jiffies(sp->rtoinfo.srto_min);

	asoc->overall_error_count = 0;

	/* Initialize the association's heartbeat interval based on the
	 * sock configured value.
	 */
	asoc->hbinterval = msecs_to_jiffies(sp->hbinterval);

	/* Initialize path max retrans value. */
	asoc->pathmaxrxt = sp->pathmaxrxt;

	/* Initialize default path MTU. */
	asoc->pathmtu = sp->pathmtu;

	/* Set association default SACK delay */
	asoc->sackdelay = msecs_to_jiffies(sp->sackdelay);

	/* Set the association default flags controlling
	 * Heartbeat, SACK delay, and Path MTU Discovery.
	 */
	asoc->param_flags = sp->param_flags;

	/* Initialize the maximum mumber of new data packets that can be sent
	 * in a burst.
	 */
	asoc->max_burst = sp->max_burst;

	/* initialize association timers */
	asoc->timeouts[SCTP_EVENT_TIMEOUT_NONE] = 0;
	asoc->timeouts[SCTP_EVENT_TIMEOUT_T1_COOKIE] = asoc->rto_initial;
	asoc->timeouts[SCTP_EVENT_TIMEOUT_T1_INIT] = asoc->rto_initial;
	asoc->timeouts[SCTP_EVENT_TIMEOUT_T2_SHUTDOWN] = asoc->rto_initial;
	asoc->timeouts[SCTP_EVENT_TIMEOUT_T3_RTX] = 0;
	asoc->timeouts[SCTP_EVENT_TIMEOUT_T4_RTO] = 0;

	/* sctpimpguide Section 2.12.2
	 * If the 'T5-shutdown-guard' timer is used, it SHOULD be set to the
	 * recommended value of 5 times 'RTO.Max'.
	 */
	asoc->timeouts[SCTP_EVENT_TIMEOUT_T5_SHUTDOWN_GUARD]
		= 5 * asoc->rto_max;

	asoc->timeouts[SCTP_EVENT_TIMEOUT_HEARTBEAT] = 0;
	asoc->timeouts[SCTP_EVENT_TIMEOUT_SACK] = asoc->sackdelay;
	asoc->timeouts[SCTP_EVENT_TIMEOUT_AUTOCLOSE] =
		sp->autoclose * HZ;

	/* Initilizes the timers */
	for (i = SCTP_EVENT_TIMEOUT_NONE; i < SCTP_NUM_TIMEOUT_TYPES; ++i) {
		init_timer(&asoc->timers[i]);
		asoc->timers[i].function = sctp_timer_events[i];
		asoc->timers[i].data = (unsigned long) asoc;
	}

	/* Pull default initialization values from the sock options.
	 * Note: This assumes that the values have already been
	 * validated in the sock.
	 */
	asoc->c.sinit_max_instreams = sp->initmsg.sinit_max_instreams;
	asoc->c.sinit_num_ostreams  = sp->initmsg.sinit_num_ostreams;
	asoc->max_init_attempts	= sp->initmsg.sinit_max_attempts;

	asoc->max_init_timeo =
		 msecs_to_jiffies(sp->initmsg.sinit_max_init_timeo);

	/* Allocate storage for the ssnmap after the inbound and outbound
	 * streams have been negotiated during Init.
	 */
	asoc->ssnmap = NULL;

	/* Set the local window size for receive.
	 * This is also the rcvbuf space per association.
	 * RFC 6 - A SCTP receiver MUST be able to receive a minimum of
	 * 1500 bytes in one SCTP packet.
	 */
	if ((sk->sk_rcvbuf/2) < SCTP_DEFAULT_MINWINDOW)
		asoc->rwnd = SCTP_DEFAULT_MINWINDOW;
	else
		asoc->rwnd = sk->sk_rcvbuf/2;

	asoc->a_rwnd = asoc->rwnd;

	asoc->rwnd_over = 0;

	/* Use my own max window until I learn something better.  */
	asoc->peer.rwnd = SCTP_DEFAULT_MAXWINDOW;

	/* Set the sndbuf size for transmit.  */
	asoc->sndbuf_used = 0;

	/* Initialize the receive memory counter */
	atomic_set(&asoc->rmem_alloc, 0);

	init_waitqueue_head(&asoc->wait);

	asoc->c.my_vtag = sctp_generate_tag(ep);
	asoc->peer.i.init_tag = 0;     /* INIT needs a vtag of 0. */
	asoc->c.peer_vtag = 0;
	asoc->c.my_ttag   = 0;
	asoc->c.peer_ttag = 0;
	asoc->c.my_port = ep->base.bind_addr.port;

	asoc->c.initial_tsn = sctp_generate_tsn(ep);

	asoc->next_tsn = asoc->c.initial_tsn;

	asoc->ctsn_ack_point = asoc->next_tsn - 1;
	asoc->adv_peer_ack_point = asoc->ctsn_ack_point;
	asoc->highest_sacked = asoc->ctsn_ack_point;
	asoc->last_cwr_tsn = asoc->ctsn_ack_point;
	asoc->unack_data = 0;

	/* ADDIP Section 4.1 Asconf Chunk Procedures
	 *
	 * When an endpoint has an ASCONF signaled change to be sent to the
	 * remote endpoint it should do the following:
	 * ...
	 * A2) a serial number should be assigned to the chunk. The serial
	 * number SHOULD be a monotonically increasing number. The serial
	 * numbers SHOULD be initialized at the start of the
	 * association to the same value as the initial TSN.
	 */
	asoc->addip_serial = asoc->c.initial_tsn;

	INIT_LIST_HEAD(&asoc->addip_chunk_list);

	/* Make an empty list of remote transport addresses.  */
	INIT_LIST_HEAD(&asoc->peer.transport_addr_list);
	asoc->peer.transport_count = 0;

	/* RFC 2960 5.1 Normal Establishment of an Association
	 *
	 * After the reception of the first data chunk in an
	 * association the endpoint must immediately respond with a
	 * sack to acknowledge the data chunk.  Subsequent
	 * acknowledgements should be done as described in Section
	 * 6.2.
	 *
	 * [We implement this by telling a new association that it
	 * already received one packet.]
	 */
	asoc->peer.sack_needed = 1;

	/* Assume that the peer recongizes ASCONF until reported otherwise
	 * via an ERROR chunk.
	 */
	asoc->peer.asconf_capable = 1;

	/* Create an input queue.  */
	sctp_inq_init(&asoc->base.inqueue);
	sctp_inq_set_th_handler(&asoc->base.inqueue, sctp_assoc_bh_rcv);

	/* Create an output queue.  */
	sctp_outq_init(asoc, &asoc->outqueue);

	if (!sctp_ulpq_init(&asoc->ulpq, asoc))
		goto fail_init;

	/* Set up the tsn tracking. */
	sctp_tsnmap_init(&asoc->peer.tsn_map, SCTP_TSN_MAP_SIZE, 0);

	asoc->need_ecne = 0;

	asoc->assoc_id = 0;

	/* Assume that peer would support both address types unless we are
	 * told otherwise.
	 */
	asoc->peer.ipv4_address = 1;
	asoc->peer.ipv6_address = 1;
	INIT_LIST_HEAD(&asoc->asocs);

	asoc->autoclose = sp->autoclose;

	asoc->default_stream = sp->default_stream;
	asoc->default_ppid = sp->default_ppid;
	asoc->default_flags = sp->default_flags;
	asoc->default_context = sp->default_context;
	asoc->default_timetolive = sp->default_timetolive;
	asoc->default_rcv_context = sp->default_rcv_context;

	return asoc;

fail_init:
	sctp_endpoint_put(asoc->ep);
	sock_put(asoc->base.sk);
	return NULL;
}

/* Allocate and initialize a new association */
struct sctp_association *sctp_association_new(const struct sctp_endpoint *ep,
					 const struct sock *sk,
					 sctp_scope_t scope,
					 gfp_t gfp)
{
	struct sctp_association *asoc;

	asoc = t_new(struct sctp_association, gfp);
	if (!asoc)
		goto fail;

	if (!sctp_association_init(asoc, ep, sk, scope, gfp))
		goto fail_init;

	asoc->base.malloced = 1;
	SCTP_DBG_OBJCNT_INC(assoc);
	SCTP_DEBUG_PRINTK("Created asoc %p\n", asoc);

	return asoc;

fail_init:
	kfree(asoc);
fail:
	return NULL;
}

/* Free this association if possible.  There may still be users, so
 * the actual deallocation may be delayed.
 */
void sctp_association_free(struct sctp_association *asoc)
{
	struct sock *sk = asoc->base.sk;
	struct sctp_transport *transport;
	struct list_head *pos, *temp;
	int i;

	/* Only real associations count against the endpoint, so
	 * don't bother for if this is a temporary association.
	 */
	if (!asoc->temp) {
		list_del(&asoc->asocs);

		/* Decrement the backlog value for a TCP-style listening
		 * socket.
		 */
		if (sctp_style(sk, TCP) && sctp_sstate(sk, LISTENING))
			sk->sk_ack_backlog--;
	}

	/* Mark as dead, so other users can know this structure is
	 * going away.
	 */
	asoc->base.dead = 1;

	/* Dispose of any data lying around in the outqueue. */
	sctp_outq_free(&asoc->outqueue);

	/* Dispose of any pending messages for the upper layer. */
	sctp_ulpq_free(&asoc->ulpq);

	/* Dispose of any pending chunks on the inqueue. */
	sctp_inq_free(&asoc->base.inqueue);

	/* Free ssnmap storage. */
	sctp_ssnmap_free(asoc->ssnmap);

	/* Clean up the bound address list. */
	sctp_bind_addr_free(&asoc->base.bind_addr);

	/* Do we need to go through all of our timers and
	 * delete them?   To be safe we will try to delete all, but we
	 * should be able to go through and make a guess based
	 * on our state.
	 */
	for (i = SCTP_EVENT_TIMEOUT_NONE; i < SCTP_NUM_TIMEOUT_TYPES; ++i) {
		if (timer_pending(&asoc->timers[i]) &&
		    del_timer(&asoc->timers[i]))
			sctp_association_put(asoc);
	}

	/* Free peer's cached cookie. */
	kfree(asoc->peer.cookie);

	/* Release the transport structures. */
	list_for_each_safe(pos, temp, &asoc->peer.transport_addr_list) {
		transport = list_entry(pos, struct sctp_transport, transports);
		list_del(pos);
		sctp_transport_free(transport);
	}

	asoc->peer.transport_count = 0;

	/* Free any cached ASCONF_ACK chunk. */
	if (asoc->addip_last_asconf_ack)
		sctp_chunk_free(asoc->addip_last_asconf_ack);

	/* Free any cached ASCONF chunk. */
	if (asoc->addip_last_asconf)
		sctp_chunk_free(asoc->addip_last_asconf);

	sctp_association_put(asoc);
}

/* Cleanup and free up an association. */
static void sctp_association_destroy(struct sctp_association *asoc)
{
	SCTP_ASSERT(asoc->base.dead, "Assoc is not dead", return);

	sctp_endpoint_put(asoc->ep);
	sock_put(asoc->base.sk);

	if (asoc->assoc_id != 0) {
		spin_lock_bh(&sctp_assocs_id_lock);
		idr_remove(&sctp_assocs_id, asoc->assoc_id);
		spin_unlock_bh(&sctp_assocs_id_lock);
	}

	BUG_TRAP(!atomic_read(&asoc->rmem_alloc));

	if (asoc->base.malloced) {
		kfree(asoc);
		SCTP_DBG_OBJCNT_DEC(assoc);
	}
}

/* Change the primary destination address for the peer. */
void sctp_assoc_set_primary(struct sctp_association *asoc,
			    struct sctp_transport *transport)
{
	asoc->peer.primary_path = transport;

	/* Set a default msg_name for events. */
	memcpy(&asoc->peer.primary_addr, &transport->ipaddr,
	       sizeof(union sctp_addr));

	/* If the primary path is changing, assume that the
	 * user wants to use this new path.
	 */
	if ((transport->state == SCTP_ACTIVE) ||
	    (transport->state == SCTP_UNKNOWN))
		asoc->peer.active_path = transport;

	/*
	 * SFR-CACC algorithm:
	 * Upon the receipt of a request to change the primary
	 * destination address, on the data structure for the new
	 * primary destination, the sender MUST do the following:
	 *
	 * 1) If CHANGEOVER_ACTIVE is set, then there was a switch
	 * to this destination address earlier. The sender MUST set
	 * CYCLING_CHANGEOVER to indicate that this switch is a
	 * double switch to the same destination address.
	 */
	if (transport->cacc.changeover_active)
		transport->cacc.cycling_changeover = 1;

	/* 2) The sender MUST set CHANGEOVER_ACTIVE to indicate that
	 * a changeover has occurred.
	 */
	transport->cacc.changeover_active = 1;

	/* 3) The sender MUST store the next TSN to be sent in
	 * next_tsn_at_change.
	 */
	transport->cacc.next_tsn_at_change = asoc->next_tsn;
}

/* Remove a transport from an association.  */
void sctp_assoc_rm_peer(struct sctp_association *asoc,
			struct sctp_transport *peer)
{
	struct list_head	*pos;
	struct sctp_transport	*transport;

	SCTP_DEBUG_PRINTK_IPADDR("sctp_assoc_rm_peer:association %p addr: ",
				 " port: %d\n",
				 asoc,
				 (&peer->ipaddr),
				 ntohs(peer->ipaddr.v4.sin_port));

	/* If we are to remove the current retran_path, update it
	 * to the next peer before removing this peer from the list.
	 */
	if (asoc->peer.retran_path == peer)
		sctp_assoc_update_retran_path(asoc);

	/* Remove this peer from the list. */
	list_del(&peer->transports);

	/* Get the first transport of asoc. */
	pos = asoc->peer.transport_addr_list.next;
	transport = list_entry(pos, struct sctp_transport, transports);

	/* Update any entries that match the peer to be deleted. */
	if (asoc->peer.primary_path == peer)
		sctp_assoc_set_primary(asoc, transport);
	if (asoc->peer.active_path == peer)
		asoc->peer.active_path = transport;
	if (asoc->peer.last_data_from == peer)
		asoc->peer.last_data_from = transport;

	/* If we remove the transport an INIT was last sent to, set it to
	 * NULL. Combined with the update of the retran path above, this
	 * will cause the next INIT to be sent to the next available
	 * transport, maintaining the cycle.
	 */
	if (asoc->init_last_sent_to == peer)
		asoc->init_last_sent_to = NULL;

	asoc->peer.transport_count--;

	sctp_transport_free(peer);
}

/* Add a transport address to an association.  */
struct sctp_transport *sctp_assoc_add_peer(struct sctp_association *asoc,
					   const union sctp_addr *addr,
					   const gfp_t gfp,
					   const int peer_state)
{
	struct sctp_transport *peer;
	struct sctp_sock *sp;
	unsigned short port;

	sp = sctp_sk(asoc->base.sk);

	/* AF_INET and AF_INET6 share common port field. */
	port = ntohs(addr->v4.sin_port);

	SCTP_DEBUG_PRINTK_IPADDR("sctp_assoc_add_peer:association %p addr: ",
				 " port: %d state:%d\n",
				 asoc,
				 addr,
				 port,
				 peer_state);

	/* Set the port if it has not been set yet.  */
	if (0 == asoc->peer.port)
		asoc->peer.port = port;

	/* Check to see if this is a duplicate. */
	peer = sctp_assoc_lookup_paddr(asoc, addr);
	if (peer) {
		if (peer->state == SCTP_UNKNOWN) {
			if (peer_state == SCTP_ACTIVE)
				peer->state = SCTP_ACTIVE;
			if (peer_state == SCTP_UNCONFIRMED)
				peer->state = SCTP_UNCONFIRMED;
		}
		return peer;
	}

	peer = sctp_transport_new(addr, gfp);
	if (!peer)
		return NULL;

	sctp_transport_set_owner(peer, asoc);

	/* Initialize the peer's heartbeat interval based on the
	 * association configured value.
	 */
	peer->hbinterval = asoc->hbinterval;

	/* Set the path max_retrans.  */
	peer->pathmaxrxt = asoc->pathmaxrxt;

	/* Initialize the peer's SACK delay timeout based on the
	 * association configured value.
	 */
	peer->sackdelay = asoc->sackdelay;

	/* Enable/disable heartbeat, SACK delay, and path MTU discovery
	 * based on association setting.
	 */
	peer->param_flags = asoc->param_flags;

	/* Initialize the pmtu of the transport. */
	if (peer->param_flags & SPP_PMTUD_ENABLE)
		sctp_transport_pmtu(peer);
	else if (asoc->pathmtu)
		peer->pathmtu = asoc->pathmtu;
	else
		peer->pathmtu = SCTP_DEFAULT_MAXSEGMENT;

	/* If this is the first transport addr on this association,
	 * initialize the association PMTU to the peer's PMTU.
	 * If not and the current association PMTU is higher than the new
	 * peer's PMTU, reset the association PMTU to the new peer's PMTU.
	 */
	if (asoc->pathmtu)
		asoc->pathmtu = min_t(int, peer->pathmtu, asoc->pathmtu);
	else
		asoc->pathmtu = peer->pathmtu;

	SCTP_DEBUG_PRINTK("sctp_assoc_add_peer:association %p PMTU set to "
			  "%d\n", asoc, asoc->pathmtu);

	asoc->frag_point = sctp_frag_point(sp, asoc->pathmtu);

	/* The asoc->peer.port might not be meaningful yet, but
	 * initialize the packet structure anyway.
	 */
	sctp_packet_init(&peer->packet, peer, asoc->base.bind_addr.port,
			 asoc->peer.port);

	/* 7.2.1 Slow-Start
	 *
	 * o The initial cwnd before DATA transmission or after a sufficiently
	 *   long idle period MUST be set to
	 *      min(4*MTU, max(2*MTU, 4380 bytes))
	 *
	 * o The initial value of ssthresh MAY be arbitrarily high
	 *   (for example, implementations MAY use the size of the
	 *   receiver advertised window).
	 */
	peer->cwnd = min(4*asoc->pathmtu, max_t(__u32, 2*asoc->pathmtu, 4380));

	/* At this point, we may not have the receiver's advertised window,
	 * so initialize ssthresh to the default value and it will be set
	 * later when we process the INIT.
	 */
	peer->ssthresh = SCTP_DEFAULT_MAXWINDOW;

	peer->partial_bytes_acked = 0;
	peer->flight_size = 0;

	/* Set the transport's RTO.initial value */
	peer->rto = asoc->rto_initial;

	/* Set the peer's active state. */
	peer->state = peer_state;

	/* Attach the remote transport to our asoc.  */
	list_add_tail(&peer->transports, &asoc->peer.transport_addr_list);
	asoc->peer.transport_count++;

	/* If we do not yet have a primary path, set one.  */
	if (!asoc->peer.primary_path) {
		sctp_assoc_set_primary(asoc, peer);
		asoc->peer.retran_path = peer;
	}

	if (asoc->peer.active_path == asoc->peer.retran_path) {
		asoc->peer.retran_path = peer;
	}

	return peer;
}

/* Delete a transport address from an association.  */
void sctp_assoc_del_peer(struct sctp_association *asoc,
			 const union sctp_addr *addr)
{
	struct list_head	*pos;
	struct list_head	*temp;
	struct sctp_transport	*transport;

	list_for_each_safe(pos, temp, &asoc->peer.transport_addr_list) {
		transport = list_entry(pos, struct sctp_transport, transports);
		if (sctp_cmp_addr_exact(addr, &transport->ipaddr)) {
			/* Do book keeping for removing the peer and free it. */
			sctp_assoc_rm_peer(asoc, transport);
			break;
		}
	}
}

/* Lookup a transport by address. */
struct sctp_transport *sctp_assoc_lookup_paddr(
					const struct sctp_association *asoc,
					const union sctp_addr *address)
{
	struct sctp_transport *t;
	struct list_head *pos;

	/* Cycle through all transports searching for a peer address. */

	list_for_each(pos, &asoc->peer.transport_addr_list) {
		t = list_entry(pos, struct sctp_transport, transports);
		if (sctp_cmp_addr_exact(address, &t->ipaddr))
			return t;
	}

	return NULL;
}

/* Engage in transport control operations.
 * Mark the transport up or down and send a notification to the user.
 * Select and update the new active and retran paths.
 */
void sctp_assoc_control_transport(struct sctp_association *asoc,
				  struct sctp_transport *transport,
				  sctp_transport_cmd_t command,
				  sctp_sn_error_t error)
{
	struct sctp_transport *t = NULL;
	struct sctp_transport *first;
	struct sctp_transport *second;
	struct sctp_ulpevent *event;
	struct sockaddr_storage addr;
	struct list_head *pos;
	int spc_state = 0;

	/* Record the transition on the transport.  */
	switch (command) {
	case SCTP_TRANSPORT_UP:
		/* If we are moving from UNCONFIRMED state due
		 * to heartbeat success, report the SCTP_ADDR_CONFIRMED
		 * state to the user, otherwise report SCTP_ADDR_AVAILABLE.
		 */
		if (SCTP_UNCONFIRMED == transport->state &&
		    SCTP_HEARTBEAT_SUCCESS == error)
			spc_state = SCTP_ADDR_CONFIRMED;
		else
			spc_state = SCTP_ADDR_AVAILABLE;
		transport->state = SCTP_ACTIVE;
		break;

	case SCTP_TRANSPORT_DOWN:
		/* if the transort was never confirmed, do not transition it
		 * to inactive state.
		 */
		if (transport->state != SCTP_UNCONFIRMED)
			transport->state = SCTP_INACTIVE;

		spc_state = SCTP_ADDR_UNREACHABLE;
		break;

	default:
		return;
	}

	/* Generate and send a SCTP_PEER_ADDR_CHANGE notification to the
	 * user.
	 */
	memset(&addr, 0, sizeof(struct sockaddr_storage));
	memcpy(&addr, &transport->ipaddr, transport->af_specific->sockaddr_len);
	event = sctp_ulpevent_make_peer_addr_change(asoc, &addr,
				0, spc_state, error, GFP_ATOMIC);
	if (event)
		sctp_ulpq_tail_event(&asoc->ulpq, event);

	/* Select new active and retran paths. */

	/* Look for the two most recently used active transports.
	 *
	 * This code produces the wrong ordering whenever jiffies
	 * rolls over, but we still get usable transports, so we don't
	 * worry about it.
	 */
	first = NULL; second = NULL;

	list_for_each(pos, &asoc->peer.transport_addr_list) {
		t = list_entry(pos, struct sctp_transport, transports);

		if ((t->state == SCTP_INACTIVE) ||
		    (t->state == SCTP_UNCONFIRMED))
			continue;
		if (!first || t->last_time_heard > first->last_time_heard) {
			second = first;
			first = t;
		}
		if (!second || t->last_time_heard > second->last_time_heard)
			second = t;
	}

	/* RFC 2960 6.4 Multi-Homed SCTP Endpoints
	 *
	 * By default, an endpoint should always transmit to the
	 * primary path, unless the SCTP user explicitly specifies the
	 * destination transport address (and possibly source
	 * transport address) to use.
	 *
	 * [If the primary is active but not most recent, bump the most
	 * recently used transport.]
	 */
	if (((asoc->peer.primary_path->state == SCTP_ACTIVE) ||
	     (asoc->peer.primary_path->state == SCTP_UNKNOWN)) &&
	    first != asoc->peer.primary_path) {
		second = first;
		first = asoc->peer.primary_path;
	}

	/* If we failed to find a usable transport, just camp on the
	 * primary, even if it is inactive.
	 */
	if (!first) {
		first = asoc->peer.primary_path;
		second = asoc->peer.primary_path;
	}

	/* Set the active and retran transports.  */
	asoc->peer.active_path = first;
	asoc->peer.retran_path = second;
}

/* Hold a reference to an association. */
void sctp_association_hold(struct sctp_association *asoc)
{
	atomic_inc(&asoc->base.refcnt);
}

/* Release a reference to an association and cleanup
 * if there are no more references.
 */
void sctp_association_put(struct sctp_association *asoc)
{
	if (atomic_dec_and_test(&asoc->base.refcnt))
		sctp_association_destroy(asoc);
}

/* Allocate the next TSN, Transmission Sequence Number, for the given
 * association.
 */
__u32 sctp_association_get_next_tsn(struct sctp_association *asoc)
{
	/* From Section 1.6 Serial Number Arithmetic:
	 * Transmission Sequence Numbers wrap around when they reach
	 * 2**32 - 1.  That is, the next TSN a DATA chunk MUST use
	 * after transmitting TSN = 2*32 - 1 is TSN = 0.
	 */
	__u32 retval = asoc->next_tsn;
	asoc->next_tsn++;
	asoc->unack_data++;

	return retval;
}

/* Compare two addresses to see if they match.  Wildcard addresses
 * only match themselves.
 */
int sctp_cmp_addr_exact(const union sctp_addr *ss1,
			const union sctp_addr *ss2)
{
	struct sctp_af *af;

	af = sctp_get_af_specific(ss1->sa.sa_family);
	if (unlikely(!af))
		return 0;

	return af->cmp_addr(ss1, ss2);
}

/* Return an ecne chunk to get prepended to a packet.
 * Note:  We are sly and return a shared, prealloced chunk.  FIXME:
 * No we don't, but we could/should.
 */
struct sctp_chunk *sctp_get_ecne_prepend(struct sctp_association *asoc)
{
	struct sctp_chunk *chunk;

	/* Send ECNE if needed.
	 * Not being able to allocate a chunk here is not deadly.
	 */
	if (asoc->need_ecne)
		chunk = sctp_make_ecne(asoc, asoc->last_ecne_tsn);
	else
		chunk = NULL;

	return chunk;
}

/*
 * Find which transport this TSN was sent on.
 */
struct sctp_transport *sctp_assoc_lookup_tsn(struct sctp_association *asoc,
					     __u32 tsn)
{
	struct sctp_transport *active;
	struct sctp_transport *match;
	struct list_head *entry, *pos;
	struct sctp_transport *transport;
	struct sctp_chunk *chunk;
	__be32 key = htonl(tsn);

	match = NULL;

	/*
	 * FIXME: In general, find a more efficient data structure for
	 * searching.
	 */

	/*
	 * The general strategy is to search each transport's transmitted
	 * list.   Return which transport this TSN lives on.
	 *
	 * Let's be hopeful and check the active_path first.
	 * Another optimization would be to know if there is only one
	 * outbound path and not have to look for the TSN at all.
	 *
	 */

	active = asoc->peer.active_path;

	list_for_each(entry, &active->transmitted) {
		chunk = list_entry(entry, struct sctp_chunk, transmitted_list);

		if (key == chunk->subh.data_hdr->tsn) {
			match = active;
			goto out;
		}
	}

	/* If not found, go search all the other transports. */
	list_for_each(pos, &asoc->peer.transport_addr_list) {
		transport = list_entry(pos, struct sctp_transport, transports);

		if (transport == active)
			break;
		list_for_each(entry, &transport->transmitted) {
			chunk = list_entry(entry, struct sctp_chunk,
					   transmitted_list);
			if (key == chunk->subh.data_hdr->tsn) {
				match = transport;
				goto out;
			}
		}
	}
out:
	return match;
}

/* Is this the association we are looking for? */
struct sctp_transport *sctp_assoc_is_match(struct sctp_association *asoc,
					   const union sctp_addr *laddr,
					   const union sctp_addr *paddr)
{
	struct sctp_transport *transport;

	if ((htons(asoc->base.bind_addr.port) == laddr->v4.sin_port) &&
	    (htons(asoc->peer.port) == paddr->v4.sin_port)) {
		transport = sctp_assoc_lookup_paddr(asoc, paddr);
		if (!transport)
			goto out;

		if (sctp_bind_addr_match(&asoc->base.bind_addr, laddr,
					 sctp_sk(asoc->base.sk)))
			goto out;
	}
	transport = NULL;

out:
	return transport;
}

/* Do delayed input processing.  This is scheduled by sctp_rcv(). */
static void sctp_assoc_bh_rcv(struct work_struct *work)
{
	struct sctp_association *asoc =
		container_of(work, struct sctp_association,
			     base.inqueue.immediate);
	struct sctp_endpoint *ep;
	struct sctp_chunk *chunk;
	struct sock *sk;
	struct sctp_inq *inqueue;
	int state;
	sctp_subtype_t subtype;
	int error = 0;

	/* The association should be held so we should be safe. */
	ep = asoc->ep;
	sk = asoc->base.sk;

	inqueue = &asoc->base.inqueue;
	sctp_association_hold(asoc);
	while (NULL != (chunk = sctp_inq_pop(inqueue))) {
		state = asoc->state;
		subtype = SCTP_ST_CHUNK(chunk->chunk_hdr->type);

		/* Remember where the last DATA chunk came from so we
		 * know where to send the SACK.
		 */
		if (sctp_chunk_is_data(chunk))
			asoc->peer.last_data_from = chunk->transport;
		else
			SCTP_INC_STATS(SCTP_MIB_INCTRLCHUNKS);

		if (chunk->transport)
			chunk->transport->last_time_heard = jiffies;

		/* Run through the state machine. */
		error = sctp_do_sm(SCTP_EVENT_T_CHUNK, subtype,
				   state, ep, asoc, chunk, GFP_ATOMIC);

		/* Check to see if the association is freed in response to
		 * the incoming chunk.  If so, get out of the while loop.
		 */
		if (asoc->base.dead)
			break;

		/* If there is an error on chunk, discard this packet. */
		if (error && chunk)
			chunk->pdiscard = 1;
	}
	sctp_association_put(asoc);
}

/* This routine moves an association from its old sk to a new sk.  */
void sctp_assoc_migrate(struct sctp_association *assoc, struct sock *newsk)
{
	struct sctp_sock *newsp = sctp_sk(newsk);
	struct sock *oldsk = assoc->base.sk;

	/* Delete the association from the old endpoint's list of
	 * associations.
	 */
	list_del_init(&assoc->asocs);

	/* Decrement the backlog value for a TCP-style socket. */
	if (sctp_style(oldsk, TCP))
		oldsk->sk_ack_backlog--;

	/* Release references to the old endpoint and the sock.  */
	sctp_endpoint_put(assoc->ep);
	sock_put(assoc->base.sk);

	/* Get a reference to the new endpoint.  */
	assoc->ep = newsp->ep;
	sctp_endpoint_hold(assoc->ep);

	/* Get a reference to the new sock.  */
	assoc->base.sk = newsk;
	sock_hold(assoc->base.sk);

	/* Add the association to the new endpoint's list of associations.  */
	sctp_endpoint_add_asoc(newsp->ep, assoc);
}

/* Update an association (possibly from unexpected COOKIE-ECHO processing).  */
void sctp_assoc_update(struct sctp_association *asoc,
		       struct sctp_association *new)
{
	struct sctp_transport *trans;
	struct list_head *pos, *temp;

	/* Copy in new parameters of peer. */
	asoc->c = new->c;
	asoc->peer.rwnd = new->peer.rwnd;
	asoc->peer.sack_needed = new->peer.sack_needed;
	asoc->peer.i = new->peer.i;
	sctp_tsnmap_init(&asoc->peer.tsn_map, SCTP_TSN_MAP_SIZE,
			 asoc->peer.i.initial_tsn);

	/* Remove any peer addresses not present in the new association. */
	list_for_each_safe(pos, temp, &asoc->peer.transport_addr_list) {
		trans = list_entry(pos, struct sctp_transport, transports);
		if (!sctp_assoc_lookup_paddr(new, &trans->ipaddr))
			sctp_assoc_del_peer(asoc, &trans->ipaddr);

		if (asoc->state >= SCTP_STATE_ESTABLISHED)
			sctp_transport_reset(trans);
	}

	/* If the case is A (association restart), use
	 * initial_tsn as next_tsn. If the case is B, use
	 * current next_tsn in case data sent to peer
	 * has been discarded and needs retransmission.
	 */
	if (asoc->state >= SCTP_STATE_ESTABLISHED) {
		asoc->next_tsn = new->next_tsn;
		asoc->ctsn_ack_point = new->ctsn_ack_point;
		asoc->adv_peer_ack_point = new->adv_peer_ack_point;

		/* Reinitialize SSN for both local streams
		 * and peer's streams.
		 */
		sctp_ssnmap_clear(asoc->ssnmap);

		/* Flush the ULP reassembly and ordered queue.
		 * Any data there will now be stale and will
		 * cause problems.
		 */
		sctp_ulpq_flush(&asoc->ulpq);

		/* reset the overall association error count so
		 * that the restarted association doesn't get torn
		 * down on the next retransmission timer.
		 */
		asoc->overall_error_count = 0;

	} else {
		/* Add any peer addresses from the new association. */
		list_for_each(pos, &new->peer.transport_addr_list) {
			trans = list_entry(pos, struct sctp_transport,
					   transports);
			if (!sctp_assoc_lookup_paddr(asoc, &trans->ipaddr))
				sctp_assoc_add_peer(asoc, &trans->ipaddr,
						    GFP_ATOMIC, trans->state);
		}

		asoc->ctsn_ack_point = asoc->next_tsn - 1;
		asoc->adv_peer_ack_point = asoc->ctsn_ack_point;
		if (!asoc->ssnmap) {
			/* Move the ssnmap. */
			asoc->ssnmap = new->ssnmap;
			new->ssnmap = NULL;
		}

		if (!asoc->assoc_id) {
			/* get a new association id since we don't have one
			 * yet.
			 */
			sctp_assoc_set_id(asoc, GFP_ATOMIC);
		}
	}
}

/* Update the retran path for sending a retransmitted packet.
 * Round-robin through the active transports, else round-robin
 * through the inactive transports as this is the next best thing
 * we can try.
 */
void sctp_assoc_update_retran_path(struct sctp_association *asoc)
{
	struct sctp_transport *t, *next;
	struct list_head *head = &asoc->peer.transport_addr_list;
	struct list_head *pos;

	/* Find the next transport in a round-robin fashion. */
	t = asoc->peer.retran_path;
	pos = &t->transports;
	next = NULL;

	while (1) {
		/* Skip the head. */
		if (pos->next == head)
			pos = head->next;
		else
			pos = pos->next;

		t = list_entry(pos, struct sctp_transport, transports);

		/* Try to find an active transport. */

		if ((t->state == SCTP_ACTIVE) ||
		    (t->state == SCTP_UNKNOWN)) {
			break;
		} else {
			/* Keep track of the next transport in case
			 * we don't find any active transport.
			 */
			if (!next)
				next = t;
		}

		/* We have exhausted the list, but didn't find any
		 * other active transports.  If so, use the next
		 * transport.
		 */
		if (t == asoc->peer.retran_path) {
			t = next;
			break;
		}
	}

	asoc->peer.retran_path = t;

	SCTP_DEBUG_PRINTK_IPADDR("sctp_assoc_update_retran_path:association"
				 " %p addr: ",
				 " port: %d\n",
				 asoc,
				 (&t->ipaddr),
				 ntohs(t->ipaddr.v4.sin_port));
}

/* Choose the transport for sending a INIT packet.  */
struct sctp_transport *sctp_assoc_choose_init_transport(
	struct sctp_association *asoc)
{
	struct sctp_transport *t;

	/* Use the retran path. If the last INIT was sent over the
	 * retran path, update the retran path and use it.
	 */
	if (!asoc->init_last_sent_to) {
		t = asoc->peer.active_path;
	} else {
		if (asoc->init_last_sent_to == asoc->peer.retran_path)
			sctp_assoc_update_retran_path(asoc);
		t = asoc->peer.retran_path;
	}

	SCTP_DEBUG_PRINTK_IPADDR("sctp_assoc_update_retran_path:association"
				 " %p addr: ",
				 " port: %d\n",
				 asoc,
				 (&t->ipaddr),
				 ntohs(t->ipaddr.v4.sin_port));

	return t;
}

/* Choose the transport for sending a SHUTDOWN packet.  */
struct sctp_transport *sctp_assoc_choose_shutdown_transport(
	struct sctp_association *asoc)
{
	/* If this is the first time SHUTDOWN is sent, use the active path,
	 * else use the retran path. If the last SHUTDOWN was sent over the
	 * retran path, update the retran path and use it.
	 */
	if (!asoc->shutdown_last_sent_to)
		return asoc->peer.active_path;
	else {
		if (asoc->shutdown_last_sent_to == asoc->peer.retran_path)
			sctp_assoc_update_retran_path(asoc);
		return asoc->peer.retran_path;
	}

}

/* Update the association's pmtu and frag_point by going through all the
 * transports. This routine is called when a transport's PMTU has changed.
 */
void sctp_assoc_sync_pmtu(struct sctp_association *asoc)
{
	struct sctp_transport *t;
	struct list_head *pos;
	__u32 pmtu = 0;

	if (!asoc)
		return;

	/* Get the lowest pmtu of all the transports. */
	list_for_each(pos, &asoc->peer.transport_addr_list) {
		t = list_entry(pos, struct sctp_transport, transports);
		if (t->pmtu_pending && t->dst) {
			sctp_transport_update_pmtu(t, dst_mtu(t->dst));
			t->pmtu_pending = 0;
		}
		if (!pmtu || (t->pathmtu < pmtu))
			pmtu = t->pathmtu;
	}

	if (pmtu) {
		struct sctp_sock *sp = sctp_sk(asoc->base.sk);
		asoc->pathmtu = pmtu;
		asoc->frag_point = sctp_frag_point(sp, pmtu);
	}

	SCTP_DEBUG_PRINTK("%s: asoc:%p, pmtu:%d, frag_point:%d\n",
			  __FUNCTION__, asoc, asoc->pathmtu, asoc->frag_point);
}

/* Should we send a SACK to update our peer? */
static inline int sctp_peer_needs_update(struct sctp_association *asoc)
{
	switch (asoc->state) {
	case SCTP_STATE_ESTABLISHED:
	case SCTP_STATE_SHUTDOWN_PENDING:
	case SCTP_STATE_SHUTDOWN_RECEIVED:
	case SCTP_STATE_SHUTDOWN_SENT:
		if ((asoc->rwnd > asoc->a_rwnd) &&
		    ((asoc->rwnd - asoc->a_rwnd) >=
		     min_t(__u32, (asoc->base.sk->sk_rcvbuf >> 1), asoc->pathmtu)))
			return 1;
		break;
	default:
		break;
	}
	return 0;
}

/* Increase asoc's rwnd by len and send any window update SACK if needed. */
void sctp_assoc_rwnd_increase(struct sctp_association *asoc, unsigned len)
{
	struct sctp_chunk *sack;
	struct timer_list *timer;

	if (asoc->rwnd_over) {
		if (asoc->rwnd_over >= len) {
			asoc->rwnd_over -= len;
		} else {
			asoc->rwnd += (len - asoc->rwnd_over);
			asoc->rwnd_over = 0;
		}
	} else {
		asoc->rwnd += len;
	}

	SCTP_DEBUG_PRINTK("%s: asoc %p rwnd increased by %d to (%u, %u) "
			  "- %u\n", __FUNCTION__, asoc, len, asoc->rwnd,
			  asoc->rwnd_over, asoc->a_rwnd);

	/* Send a window update SACK if the rwnd has increased by at least the
	 * minimum of the association's PMTU and half of the receive buffer.
	 * The algorithm used is similar to the one described in
	 * Section 4.2.3.3 of RFC 1122.
	 */
	if (sctp_peer_needs_update(asoc)) {
		asoc->a_rwnd = asoc->rwnd;
		SCTP_DEBUG_PRINTK("%s: Sending window update SACK- asoc: %p "
				  "rwnd: %u a_rwnd: %u\n", __FUNCTION__,
				  asoc, asoc->rwnd, asoc->a_rwnd);
		sack = sctp_make_sack(asoc);
		if (!sack)
			return;

		asoc->peer.sack_needed = 0;

		sctp_outq_tail(&asoc->outqueue, sack);

		/* Stop the SACK timer.  */
		timer = &asoc->timers[SCTP_EVENT_TIMEOUT_SACK];
		if (timer_pending(timer) && del_timer(timer))
			sctp_association_put(asoc);
	}
}

/* Decrease asoc's rwnd by len. */
void sctp_assoc_rwnd_decrease(struct sctp_association *asoc, unsigned len)
{
	SCTP_ASSERT(asoc->rwnd, "rwnd zero", return);
	SCTP_ASSERT(!asoc->rwnd_over, "rwnd_over not zero", return);
	if (asoc->rwnd >= len) {
		asoc->rwnd -= len;
	} else {
		asoc->rwnd_over = len - asoc->rwnd;
		asoc->rwnd = 0;
	}
	SCTP_DEBUG_PRINTK("%s: asoc %p rwnd decreased by %d to (%u, %u)\n",
			  __FUNCTION__, asoc, len, asoc->rwnd,
			  asoc->rwnd_over);
}

/* Build the bind address list for the association based on info from the
 * local endpoint and the remote peer.
 */
int sctp_assoc_set_bind_addr_from_ep(struct sctp_association *asoc,
				     gfp_t gfp)
{
	sctp_scope_t scope;
	int flags;

	/* Use scoping rules to determine the subset of addresses from
	 * the endpoint.
	 */
	scope = sctp_scope(&asoc->peer.active_path->ipaddr);
	flags = (PF_INET6 == asoc->base.sk->sk_family) ? SCTP_ADDR6_ALLOWED : 0;
	if (asoc->peer.ipv4_address)
		flags |= SCTP_ADDR4_PEERSUPP;
	if (asoc->peer.ipv6_address)
		flags |= SCTP_ADDR6_PEERSUPP;

	return sctp_bind_addr_copy(&asoc->base.bind_addr,
				   &asoc->ep->base.bind_addr,
				   scope, gfp, flags);
}

/* Build the association's bind address list from the cookie.  */
int sctp_assoc_set_bind_addr_from_cookie(struct sctp_association *asoc,
					 struct sctp_cookie *cookie,
					 gfp_t gfp)
{
	int var_size2 = ntohs(cookie->peer_init->chunk_hdr.length);
	int var_size3 = cookie->raw_addr_list_len;
	__u8 *raw = (__u8 *)cookie->peer_init + var_size2;

	return sctp_raw_to_bind_addrs(&asoc->base.bind_addr, raw, var_size3,
				      asoc->ep->base.bind_addr.port, gfp);
}

/* Lookup laddr in the bind address list of an association. */
int sctp_assoc_lookup_laddr(struct sctp_association *asoc,
			    const union sctp_addr *laddr)
{
	int found = 0;

	if ((asoc->base.bind_addr.port == ntohs(laddr->v4.sin_port)) &&
	    sctp_bind_addr_match(&asoc->base.bind_addr, laddr,
				 sctp_sk(asoc->base.sk)))
		found = 1;

	return found;
}

/* Set an association id for a given association */
int sctp_assoc_set_id(struct sctp_association *asoc, gfp_t gfp)
{
	int assoc_id;
	int error = 0;
retry:
	if (unlikely(!idr_pre_get(&sctp_assocs_id, gfp)))
		return -ENOMEM;

	spin_lock_bh(&sctp_assocs_id_lock);
	error = idr_get_new_above(&sctp_assocs_id, (void *)asoc,
				    1, &assoc_id);
	spin_unlock_bh(&sctp_assocs_id_lock);
	if (error == -EAGAIN)
		goto retry;
	else if (error)
		return error;

	asoc->assoc_id = (sctp_assoc_t) assoc_id;
	return error;
}
