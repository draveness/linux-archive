/* SCTP kernel reference Implementation
 * Copyright (c) 1999-2000 Cisco, Inc.
 * Copyright (c) 1999-2001 Motorola, Inc.
 * Copyright (c) 2001-2003 International Business Machines, Corp.
 * Copyright (c) 2001 Intel Corp.
 * Copyright (c) 2001 Nokia, Inc.
 * Copyright (c) 2001 La Monte H.P. Yarroll
 *
 * This file is part of the SCTP kernel reference Implementation
 *
 * These functions handle all input from the IP layer into SCTP.
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
 *    Karl Knutson <karl@athena.chicago.il.us>
 *    Xingang Guo <xingang.guo@intel.com>
 *    Jon Grimm <jgrimm@us.ibm.com>
 *    Hui Huang <hui.huang@nokia.com>
 *    Daisy Chang <daisyc@us.ibm.com>
 *    Sridhar Samudrala <sri@us.ibm.com>
 *    Ardelle Fan <ardelle.fan@intel.com>
 *
 * Any bugs reported given to us we will try to fix... any fixes shared will
 * be incorporated into the next SCTP release.
 */

#include <linux/types.h>
#include <linux/list.h> /* For struct list_head */
#include <linux/socket.h>
#include <linux/ip.h>
#include <linux/time.h> /* For struct timeval */
#include <net/ip.h>
#include <net/icmp.h>
#include <net/snmp.h>
#include <net/sock.h>
#include <net/xfrm.h>
#include <net/sctp/sctp.h>
#include <net/sctp/sm.h>

/* Forward declarations for internal helpers. */
static int sctp_rcv_ootb(struct sk_buff *);
struct sctp_association *__sctp_rcv_lookup(struct sk_buff *skb,
				      const union sctp_addr *laddr,
				      const union sctp_addr *paddr,
				      struct sctp_transport **transportp);
struct sctp_endpoint *__sctp_rcv_lookup_endpoint(const union sctp_addr *laddr);


/* Calculate the SCTP checksum of an SCTP packet.  */
static inline int sctp_rcv_checksum(struct sk_buff *skb)
{
	struct sctphdr *sh;
	__u32 cmp, val;
	struct sk_buff *list = skb_shinfo(skb)->frag_list;

	sh = (struct sctphdr *) skb->h.raw;
	cmp = ntohl(sh->checksum);

	val = sctp_start_cksum((__u8 *)sh, skb_headlen(skb));

	for (; list; list = list->next)
		val = sctp_update_cksum((__u8 *)list->data, skb_headlen(list),
					val);

	val = sctp_end_cksum(val);

	if (val != cmp) {
		/* CRC failure, dump it. */
		SCTP_INC_STATS_BH(SCTP_MIB_CHECKSUMERRORS);
		return -1;
	}
	return 0;
}

/*
 * This is the routine which IP calls when receiving an SCTP packet.
 */
int sctp_rcv(struct sk_buff *skb)
{
	struct sock *sk;
	struct sctp_association *asoc;
	struct sctp_endpoint *ep = NULL;
	struct sctp_ep_common *rcvr;
	struct sctp_transport *transport = NULL;
	struct sctp_chunk *chunk;
	struct sctphdr *sh;
	union sctp_addr src;
	union sctp_addr dest;
	int family;
	struct sctp_af *af;
	int ret = 0;

	if (skb->pkt_type!=PACKET_HOST)
		goto discard_it;

	SCTP_INC_STATS_BH(SCTP_MIB_INSCTPPACKS);

	sh = (struct sctphdr *) skb->h.raw;

	/* Pull up the IP and SCTP headers. */
	__skb_pull(skb, skb->h.raw - skb->data);
	if (skb->len < sizeof(struct sctphdr))
		goto discard_it;
	if (sctp_rcv_checksum(skb) < 0)
		goto discard_it;

	skb_pull(skb, sizeof(struct sctphdr));

	family = ipver2af(skb->nh.iph->version);
	af = sctp_get_af_specific(family);
	if (unlikely(!af))
		goto discard_it;

	/* Initialize local addresses for lookups. */
	af->from_skb(&src, skb, 1);
	af->from_skb(&dest, skb, 0);

	/* If the packet is to or from a non-unicast address,
	 * silently discard the packet.
	 *
	 * This is not clearly defined in the RFC except in section
	 * 8.4 - OOTB handling.  However, based on the book "Stream Control
	 * Transmission Protocol" 2.1, "It is important to note that the
	 * IP address of an SCTP transport address must be a routable
	 * unicast address.  In other words, IP multicast addresses and
	 * IP broadcast addresses cannot be used in an SCTP transport
	 * address."
	 */
	if (!af->addr_valid(&src, NULL) || !af->addr_valid(&dest, NULL))
		goto discard_it;

	asoc = __sctp_rcv_lookup(skb, &src, &dest, &transport);

	/*
	 * RFC 2960, 8.4 - Handle "Out of the blue" Packets.
	 * An SCTP packet is called an "out of the blue" (OOTB)
	 * packet if it is correctly formed, i.e., passed the
	 * receiver's checksum check, but the receiver is not
	 * able to identify the association to which this
	 * packet belongs.
	 */
	if (!asoc) {
		ep = __sctp_rcv_lookup_endpoint(&dest);
		if (sctp_rcv_ootb(skb)) {
			SCTP_INC_STATS_BH(SCTP_MIB_OUTOFBLUES);
			goto discard_release;
		}
	}

	/* Retrieve the common input handling substructure. */
	rcvr = asoc ? &asoc->base : &ep->base;
	sk = rcvr->sk;

	/* SCTP seems to always need a timestamp right now (FIXME) */
	if (skb->stamp.tv_sec == 0) {
		do_gettimeofday(&skb->stamp);
		sock_enable_timestamp(sk); 
	}

	if (!xfrm_policy_check(sk, XFRM_POLICY_IN, skb, family))
		goto discard_release;

	ret = sk_filter(sk, skb, 1);
	if (ret)
                goto discard_release;

	/* Create an SCTP packet structure. */
	chunk = sctp_chunkify(skb, asoc, sk);
	if (!chunk) {
		ret = -ENOMEM;
		goto discard_release;
	}

	/* Remember what endpoint is to handle this packet. */
	chunk->rcvr = rcvr;

	/* Remember the SCTP header. */
	chunk->sctp_hdr = sh;

	/* Set the source and destination addresses of the incoming chunk.  */
	sctp_init_addrs(chunk, &src, &dest);

	/* Remember where we came from.  */
	chunk->transport = transport;

	/* Acquire access to the sock lock. Note: We are safe from other
	 * bottom halves on this lock, but a user may be in the lock too,
	 * so check if it is busy.
	 */
	sctp_bh_lock_sock(sk);

	if (sock_owned_by_user(sk))
		sk_add_backlog(sk, (struct sk_buff *) chunk);
	else
		sctp_backlog_rcv(sk, (struct sk_buff *) chunk);

	/* Release the sock and any reference counts we took in the
	 * lookup calls.
	 */
	sctp_bh_unlock_sock(sk);
	if (asoc)
		sctp_association_put(asoc);
	else
		sctp_endpoint_put(ep);
	sock_put(sk);
	return ret;

discard_it:
	kfree_skb(skb);
	return ret;

discard_release:
	/* Release any structures we may be holding. */
	if (asoc) {
		sock_put(asoc->base.sk);
		sctp_association_put(asoc);
	} else {
		sock_put(ep->base.sk);
		sctp_endpoint_put(ep);
	}

	goto discard_it;
}

/* Handle second half of inbound skb processing.  If the sock was busy,
 * we may have need to delay processing until later when the sock is
 * released (on the backlog).   If not busy, we call this routine
 * directly from the bottom half.
 */
int sctp_backlog_rcv(struct sock *sk, struct sk_buff *skb)
{
	struct sctp_chunk *chunk;
	struct sctp_inq *inqueue;

	/* One day chunk will live inside the skb, but for
	 * now this works.
	 */
	chunk = (struct sctp_chunk *) skb;
	inqueue = &chunk->rcvr->inqueue;

	sctp_inq_push(inqueue, chunk);
        return 0;
}

/* Handle icmp frag needed error. */
void sctp_icmp_frag_needed(struct sock *sk, struct sctp_association *asoc,
			   struct sctp_transport *t, __u32 pmtu)
{
	if (unlikely(pmtu < SCTP_DEFAULT_MINSEGMENT)) {
		printk(KERN_WARNING "%s: Reported pmtu %d too low, "
		       "using default minimum of %d\n", __FUNCTION__, pmtu,
		       SCTP_DEFAULT_MINSEGMENT);
		pmtu = SCTP_DEFAULT_MINSEGMENT;
	}

	if (!sock_owned_by_user(sk) && t && (t->pmtu != pmtu)) {
		t->pmtu = pmtu;
		sctp_assoc_sync_pmtu(asoc);
		sctp_retransmit(&asoc->outqueue, t, SCTP_RTXR_PMTUD);
	}
}

/* Common lookup code for icmp/icmpv6 error handler. */
struct sock *sctp_err_lookup(int family, struct sk_buff *skb,
			     struct sctphdr *sctphdr,
			     struct sctp_endpoint **epp,
			     struct sctp_association **app,
			     struct sctp_transport **tpp)
{
	union sctp_addr saddr;
	union sctp_addr daddr;
	struct sctp_af *af;
	struct sock *sk = NULL;
	struct sctp_endpoint *ep = NULL;
	struct sctp_association *asoc = NULL;
	struct sctp_transport *transport = NULL;

	*app = NULL; *epp = NULL; *tpp = NULL;

	af = sctp_get_af_specific(family);
	if (unlikely(!af)) {
		return NULL;
	}

	/* Initialize local addresses for lookups. */
	af->from_skb(&saddr, skb, 1);
	af->from_skb(&daddr, skb, 0);

	/* Look for an association that matches the incoming ICMP error
	 * packet.
	 */
	asoc = __sctp_lookup_association(&saddr, &daddr, &transport);
	if (!asoc) {
		/* If there is no matching association, see if it matches any
		 * endpoint. This may happen for an ICMP error generated in
		 * response to an INIT_ACK.
		 */
		ep = __sctp_rcv_lookup_endpoint(&daddr);
		if (!ep) {
			return NULL;
		}
	}

	if (asoc) {
		if (ntohl(sctphdr->vtag) != asoc->c.peer_vtag) {
			ICMP_INC_STATS_BH(ICMP_MIB_INERRORS);
			goto out;
		}
		sk = asoc->base.sk;
	} else
		sk = ep->base.sk;

	sctp_bh_lock_sock(sk);

	/* If too many ICMPs get dropped on busy
	 * servers this needs to be solved differently.
	 */
	if (sock_owned_by_user(sk))
		NET_INC_STATS_BH(LINUX_MIB_LOCKDROPPEDICMPS);

	*epp = ep;
	*app = asoc;
	*tpp = transport;
	return sk;

out:
	sock_put(sk);
	if (asoc)
		sctp_association_put(asoc);
	if (ep)
		sctp_endpoint_put(ep);
	return NULL;
}

/* Common cleanup code for icmp/icmpv6 error handler. */
void sctp_err_finish(struct sock *sk, struct sctp_endpoint *ep,
		     struct sctp_association *asoc)
{
	sctp_bh_unlock_sock(sk);
	sock_put(sk);
	if (asoc)
		sctp_association_put(asoc);
	if (ep)
		sctp_endpoint_put(ep);
}

/*
 * This routine is called by the ICMP module when it gets some
 * sort of error condition.  If err < 0 then the socket should
 * be closed and the error returned to the user.  If err > 0
 * it's just the icmp type << 8 | icmp code.  After adjustment
 * header points to the first 8 bytes of the sctp header.  We need
 * to find the appropriate port.
 *
 * The locking strategy used here is very "optimistic". When
 * someone else accesses the socket the ICMP is just dropped
 * and for some paths there is no check at all.
 * A more general error queue to queue errors for later handling
 * is probably better.
 *
 */
void sctp_v4_err(struct sk_buff *skb, __u32 info)
{
	struct iphdr *iph = (struct iphdr *)skb->data;
	struct sctphdr *sh = (struct sctphdr *)(skb->data + (iph->ihl <<2));
	int type = skb->h.icmph->type;
	int code = skb->h.icmph->code;
	struct sock *sk;
	struct sctp_endpoint *ep;
	struct sctp_association *asoc;
	struct sctp_transport *transport;
	struct inet_opt *inet;
	char *saveip, *savesctp;
	int err;

	if (skb->len < ((iph->ihl << 2) + 8)) {
		ICMP_INC_STATS_BH(ICMP_MIB_INERRORS);
		return;
	}

	/* Fix up skb to look at the embedded net header. */
	saveip = skb->nh.raw;
	savesctp  = skb->h.raw;
	skb->nh.iph = iph;
	skb->h.raw = (char *)sh;
	sk = sctp_err_lookup(AF_INET, skb, sh, &ep, &asoc, &transport);
	/* Put back, the original pointers. */
	skb->nh.raw = saveip;
	skb->h.raw = savesctp;
	if (!sk) {
		ICMP_INC_STATS_BH(ICMP_MIB_INERRORS);
		return;
	}
	/* Warning:  The sock lock is held.  Remember to call
	 * sctp_err_finish!
	 */

	switch (type) {
	case ICMP_PARAMETERPROB:
		err = EPROTO;
		break;
	case ICMP_DEST_UNREACH:
		if (code > NR_ICMP_UNREACH)
			goto out_unlock;

		/* PMTU discovery (RFC1191) */
		if (ICMP_FRAG_NEEDED == code) {
			sctp_icmp_frag_needed(sk, asoc, transport, info);
			goto out_unlock;
		}

		err = icmp_err_convert[code].errno;
		break;
	case ICMP_TIME_EXCEEDED:
		/* Ignore any time exceeded errors due to fragment reassembly
		 * timeouts.
		 */
		if (ICMP_EXC_FRAGTIME == code)
			goto out_unlock;

		err = EHOSTUNREACH;
		break;
	default:
		goto out_unlock;
	}

	inet = inet_sk(sk);
	if (!sock_owned_by_user(sk) && inet->recverr) {
		sk->sk_err = err;
		sk->sk_error_report(sk);
	} else {  /* Only an error on timeout */
		sk->sk_err_soft = err;
	}

out_unlock:
	sctp_err_finish(sk, ep, asoc);
}

/*
 * RFC 2960, 8.4 - Handle "Out of the blue" Packets.
 *
 * This function scans all the chunks in the OOTB packet to determine if
 * the packet should be discarded right away.  If a response might be needed
 * for this packet, or, if further processing is possible, the packet will
 * be queued to a proper inqueue for the next phase of handling.
 *
 * Output:
 * Return 0 - If further processing is needed.
 * Return 1 - If the packet can be discarded right away.
 */
int sctp_rcv_ootb(struct sk_buff *skb)
{
	sctp_chunkhdr_t *ch;
	__u8 *ch_end;
	sctp_errhdr_t *err;

	ch = (sctp_chunkhdr_t *) skb->data;

	/* Scan through all the chunks in the packet.  */
	do {
		ch_end = ((__u8 *) ch) + WORD_ROUND(ntohs(ch->length));

		/* RFC 8.4, 2) If the OOTB packet contains an ABORT chunk, the
		 * receiver MUST silently discard the OOTB packet and take no
		 * further action.
		 */
		if (SCTP_CID_ABORT == ch->type)
			goto discard;

		/* RFC 8.4, 6) If the packet contains a SHUTDOWN COMPLETE
		 * chunk, the receiver should silently discard the packet
		 * and take no further action.
		 */
		if (SCTP_CID_SHUTDOWN_COMPLETE == ch->type)
			goto discard;

		/* RFC 8.4, 7) If the packet contains a "Stale cookie" ERROR
		 * or a COOKIE ACK the SCTP Packet should be silently
		 * discarded.
		 */
		if (SCTP_CID_COOKIE_ACK == ch->type)
			goto discard;

		if (SCTP_CID_ERROR == ch->type) {
			sctp_walk_errors(err, ch) {
				if (SCTP_ERROR_STALE_COOKIE == err->cause)
					goto discard;
			}
		}

		ch = (sctp_chunkhdr_t *) ch_end;
	} while (ch_end < skb->tail);

	return 0;

discard:
	return 1;
}

/* Insert endpoint into the hash table.  */
void __sctp_hash_endpoint(struct sctp_endpoint *ep)
{
	struct sctp_ep_common **epp;
	struct sctp_ep_common *epb;
	struct sctp_hashbucket *head;

	epb = &ep->base;

	epb->hashent = sctp_ep_hashfn(epb->bind_addr.port);
	head = &sctp_ep_hashtable[epb->hashent];

	sctp_write_lock(&head->lock);
	epp = &head->chain;
	epb->next = *epp;
	if (epb->next)
		(*epp)->pprev = &epb->next;
	*epp = epb;
	epb->pprev = epp;
	sctp_write_unlock(&head->lock);
}

/* Add an endpoint to the hash. Local BH-safe. */
void sctp_hash_endpoint(struct sctp_endpoint *ep)
{
	sctp_local_bh_disable();
	__sctp_hash_endpoint(ep);
	sctp_local_bh_enable();
}

/* Remove endpoint from the hash table.  */
void __sctp_unhash_endpoint(struct sctp_endpoint *ep)
{
	struct sctp_hashbucket *head;
	struct sctp_ep_common *epb;

	epb = &ep->base;

	epb->hashent = sctp_ep_hashfn(epb->bind_addr.port);

	head = &sctp_ep_hashtable[epb->hashent];

	sctp_write_lock(&head->lock);

	if (epb->pprev) {
		if (epb->next)
			epb->next->pprev = epb->pprev;
		*epb->pprev = epb->next;
		epb->pprev = NULL;
	}

	sctp_write_unlock(&head->lock);
}

/* Remove endpoint from the hash.  Local BH-safe. */
void sctp_unhash_endpoint(struct sctp_endpoint *ep)
{
	sctp_local_bh_disable();
	__sctp_unhash_endpoint(ep);
	sctp_local_bh_enable();
}

/* Look up an endpoint. */
struct sctp_endpoint *__sctp_rcv_lookup_endpoint(const union sctp_addr *laddr)
{
	struct sctp_hashbucket *head;
	struct sctp_ep_common *epb;
	struct sctp_endpoint *ep;
	int hash;

	hash = sctp_ep_hashfn(laddr->v4.sin_port);
	head = &sctp_ep_hashtable[hash];
	read_lock(&head->lock);
	for (epb = head->chain; epb; epb = epb->next) {
		ep = sctp_ep(epb);
		if (sctp_endpoint_is_match(ep, laddr))
			goto hit;
	}

	ep = sctp_sk((sctp_get_ctl_sock()))->ep;
	epb = &ep->base;

hit:
	sctp_endpoint_hold(ep);
	sock_hold(epb->sk);
	read_unlock(&head->lock);
	return ep;
}

/* Add an association to the hash. Local BH-safe. */
void sctp_hash_established(struct sctp_association *asoc)
{
	sctp_local_bh_disable();
	__sctp_hash_established(asoc);
	sctp_local_bh_enable();
}

/* Insert association into the hash table.  */
void __sctp_hash_established(struct sctp_association *asoc)
{
	struct sctp_ep_common **epp;
	struct sctp_ep_common *epb;
	struct sctp_hashbucket *head;

	epb = &asoc->base;

	/* Calculate which chain this entry will belong to. */
	epb->hashent = sctp_assoc_hashfn(epb->bind_addr.port, asoc->peer.port);

	head = &sctp_assoc_hashtable[epb->hashent];

	sctp_write_lock(&head->lock);
	epp = &head->chain;
	epb->next = *epp;
	if (epb->next)
		(*epp)->pprev = &epb->next;
	*epp = epb;
	epb->pprev = epp;
	sctp_write_unlock(&head->lock);
}

/* Remove association from the hash table.  Local BH-safe. */
void sctp_unhash_established(struct sctp_association *asoc)
{
	sctp_local_bh_disable();
	__sctp_unhash_established(asoc);
	sctp_local_bh_enable();
}

/* Remove association from the hash table.  */
void __sctp_unhash_established(struct sctp_association *asoc)
{
	struct sctp_hashbucket *head;
	struct sctp_ep_common *epb;

	epb = &asoc->base;

	epb->hashent = sctp_assoc_hashfn(epb->bind_addr.port,
					 asoc->peer.port);

	head = &sctp_assoc_hashtable[epb->hashent];

	sctp_write_lock(&head->lock);

	if (epb->pprev) {
		if (epb->next)
			epb->next->pprev = epb->pprev;
		*epb->pprev = epb->next;
		epb->pprev = NULL;
	}

	sctp_write_unlock(&head->lock);
}

/* Look up an association. */
struct sctp_association *__sctp_lookup_association(
					const union sctp_addr *local,
					const union sctp_addr *peer,
					struct sctp_transport **pt)
{
	struct sctp_hashbucket *head;
	struct sctp_ep_common *epb;
	struct sctp_association *asoc;
	struct sctp_transport *transport;
	int hash;

	/* Optimize here for direct hit, only listening connections can
	 * have wildcards anyways.
	 */
	hash = sctp_assoc_hashfn(local->v4.sin_port, peer->v4.sin_port);
	head = &sctp_assoc_hashtable[hash];
	read_lock(&head->lock);
	for (epb = head->chain; epb; epb = epb->next) {
		asoc = sctp_assoc(epb);
		transport = sctp_assoc_is_match(asoc, local, peer);
		if (transport)
			goto hit;
	}

	read_unlock(&head->lock);

	return NULL;

hit:
	*pt = transport;
	sctp_association_hold(asoc);
	sock_hold(epb->sk);
	read_unlock(&head->lock);
	return asoc;
}

/* Look up an association. BH-safe. */
struct sctp_association *sctp_lookup_association(const union sctp_addr *laddr,
					    const union sctp_addr *paddr,
					    struct sctp_transport **transportp)
{
	struct sctp_association *asoc;

	sctp_local_bh_disable();
	asoc = __sctp_lookup_association(laddr, paddr, transportp);
	sctp_local_bh_enable();

	return asoc;
}

/* Is there an association matching the given local and peer addresses? */
int sctp_has_association(const union sctp_addr *laddr,
			 const union sctp_addr *paddr)
{
	struct sctp_association *asoc;
	struct sctp_transport *transport;

	if ((asoc = sctp_lookup_association(laddr, paddr, &transport))) {
		sock_put(asoc->base.sk);
		sctp_association_put(asoc);
		return 1;
	}

	return 0;
}

/*
 * SCTP Implementors Guide, 2.18 Handling of address
 * parameters within the INIT or INIT-ACK.
 *
 * D) When searching for a matching TCB upon reception of an INIT
 *    or INIT-ACK chunk the receiver SHOULD use not only the
 *    source address of the packet (containing the INIT or
 *    INIT-ACK) but the receiver SHOULD also use all valid
 *    address parameters contained within the chunk.
 *
 * 2.18.3 Solution description
 *
 * This new text clearly specifies to an implementor the need
 * to look within the INIT or INIT-ACK. Any implementation that
 * does not do this, may not be able to establish associations
 * in certain circumstances.
 *
 */
static struct sctp_association *__sctp_rcv_init_lookup(struct sk_buff *skb,
	const union sctp_addr *laddr, struct sctp_transport **transportp)
{
	struct sctp_association *asoc;
	union sctp_addr addr;
	union sctp_addr *paddr = &addr;
	struct sctphdr *sh = (struct sctphdr *) skb->h.raw;
	sctp_chunkhdr_t *ch;
	union sctp_params params;
	sctp_init_chunk_t *init;
	struct sctp_transport *transport;
	struct sctp_af *af;

	ch = (sctp_chunkhdr_t *) skb->data;

	/* If this is INIT/INIT-ACK look inside the chunk too. */
	switch (ch->type) {
	case SCTP_CID_INIT:
	case SCTP_CID_INIT_ACK:
		break;
	default:
		return NULL;
	}

	/*
	 * This code will NOT touch anything inside the chunk--it is
	 * strictly READ-ONLY.
	 *
	 * RFC 2960 3  SCTP packet Format
	 *
	 * Multiple chunks can be bundled into one SCTP packet up to
	 * the MTU size, except for the INIT, INIT ACK, and SHUTDOWN
	 * COMPLETE chunks.  These chunks MUST NOT be bundled with any
	 * other chunk in a packet.  See Section 6.10 for more details
	 * on chunk bundling.
	 */

	/* Find the start of the TLVs and the end of the chunk.  This is
	 * the region we search for address parameters.
	 */
	init = (sctp_init_chunk_t *)skb->data;

	/* Walk the parameters looking for embedded addresses. */
	sctp_walk_params(params, init, init_hdr.params) {

		/* Note: Ignoring hostname addresses. */
		af = sctp_get_af_specific(param_type2af(params.p->type));
		if (!af)
			continue;

		af->from_addr_param(paddr, params.addr, ntohs(sh->source), 0);

		asoc = __sctp_lookup_association(laddr, paddr, &transport);
		if (asoc)
			return asoc;
	}

	return NULL;
}

/* Lookup an association for an inbound skb. */
struct sctp_association *__sctp_rcv_lookup(struct sk_buff *skb,
				      const union sctp_addr *paddr,
				      const union sctp_addr *laddr,
				      struct sctp_transport **transportp)
{
	struct sctp_association *asoc;

	asoc = __sctp_lookup_association(laddr, paddr, transportp);

	/* Further lookup for INIT/INIT-ACK packets.
	 * SCTP Implementors Guide, 2.18 Handling of address
	 * parameters within the INIT or INIT-ACK.
	 */
	if (!asoc)
		asoc = __sctp_rcv_init_lookup(skb, laddr, transportp);

	return asoc;
}
