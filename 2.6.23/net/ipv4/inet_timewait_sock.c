/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Generic TIME_WAIT sockets functions
 *
 *		From code orinally in TCP
 */


#include <net/inet_hashtables.h>
#include <net/inet_timewait_sock.h>
#include <net/ip.h>

/* Must be called with locally disabled BHs. */
static void __inet_twsk_kill(struct inet_timewait_sock *tw,
			     struct inet_hashinfo *hashinfo)
{
	struct inet_bind_hashbucket *bhead;
	struct inet_bind_bucket *tb;
	/* Unlink from established hashes. */
	struct inet_ehash_bucket *ehead = inet_ehash_bucket(hashinfo, tw->tw_hash);

	write_lock(&ehead->lock);
	if (hlist_unhashed(&tw->tw_node)) {
		write_unlock(&ehead->lock);
		return;
	}
	__hlist_del(&tw->tw_node);
	sk_node_init(&tw->tw_node);
	write_unlock(&ehead->lock);

	/* Disassociate with bind bucket. */
	bhead = &hashinfo->bhash[inet_bhashfn(tw->tw_num, hashinfo->bhash_size)];
	spin_lock(&bhead->lock);
	tb = tw->tw_tb;
	__hlist_del(&tw->tw_bind_node);
	tw->tw_tb = NULL;
	inet_bind_bucket_destroy(hashinfo->bind_bucket_cachep, tb);
	spin_unlock(&bhead->lock);
#ifdef SOCK_REFCNT_DEBUG
	if (atomic_read(&tw->tw_refcnt) != 1) {
		printk(KERN_DEBUG "%s timewait_sock %p refcnt=%d\n",
		       tw->tw_prot->name, tw, atomic_read(&tw->tw_refcnt));
	}
#endif
	inet_twsk_put(tw);
}

/*
 * Enter the time wait state. This is called with locally disabled BH.
 * Essentially we whip up a timewait bucket, copy the relevant info into it
 * from the SK, and mess with hash chains and list linkage.
 */
void __inet_twsk_hashdance(struct inet_timewait_sock *tw, struct sock *sk,
			   struct inet_hashinfo *hashinfo)
{
	const struct inet_sock *inet = inet_sk(sk);
	const struct inet_connection_sock *icsk = inet_csk(sk);
	struct inet_ehash_bucket *ehead = inet_ehash_bucket(hashinfo, sk->sk_hash);
	struct inet_bind_hashbucket *bhead;
	/* Step 1: Put TW into bind hash. Original socket stays there too.
	   Note, that any socket with inet->num != 0 MUST be bound in
	   binding cache, even if it is closed.
	 */
	bhead = &hashinfo->bhash[inet_bhashfn(inet->num, hashinfo->bhash_size)];
	spin_lock(&bhead->lock);
	tw->tw_tb = icsk->icsk_bind_hash;
	BUG_TRAP(icsk->icsk_bind_hash);
	inet_twsk_add_bind_node(tw, &tw->tw_tb->owners);
	spin_unlock(&bhead->lock);

	write_lock(&ehead->lock);

	/* Step 2: Remove SK from established hash. */
	if (__sk_del_node_init(sk))
		sock_prot_dec_use(sk->sk_prot);

	/* Step 3: Hash TW into TIMEWAIT chain. */
	inet_twsk_add_node(tw, &ehead->twchain);
	atomic_inc(&tw->tw_refcnt);

	write_unlock(&ehead->lock);
}

EXPORT_SYMBOL_GPL(__inet_twsk_hashdance);

struct inet_timewait_sock *inet_twsk_alloc(const struct sock *sk, const int state)
{
	struct inet_timewait_sock *tw =
		kmem_cache_alloc(sk->sk_prot_creator->twsk_prot->twsk_slab,
				 GFP_ATOMIC);
	if (tw != NULL) {
		const struct inet_sock *inet = inet_sk(sk);

		/* Give us an identity. */
		tw->tw_daddr	    = inet->daddr;
		tw->tw_rcv_saddr    = inet->rcv_saddr;
		tw->tw_bound_dev_if = sk->sk_bound_dev_if;
		tw->tw_num	    = inet->num;
		tw->tw_state	    = TCP_TIME_WAIT;
		tw->tw_substate	    = state;
		tw->tw_sport	    = inet->sport;
		tw->tw_dport	    = inet->dport;
		tw->tw_family	    = sk->sk_family;
		tw->tw_reuse	    = sk->sk_reuse;
		tw->tw_hash	    = sk->sk_hash;
		tw->tw_ipv6only	    = 0;
		tw->tw_prot	    = sk->sk_prot_creator;
		atomic_set(&tw->tw_refcnt, 1);
		inet_twsk_dead_node_init(tw);
		__module_get(tw->tw_prot->owner);
	}

	return tw;
}

EXPORT_SYMBOL_GPL(inet_twsk_alloc);

/* Returns non-zero if quota exceeded.  */
static int inet_twdr_do_twkill_work(struct inet_timewait_death_row *twdr,
				    const int slot)
{
	struct inet_timewait_sock *tw;
	struct hlist_node *node;
	unsigned int killed;
	int ret;

	/* NOTE: compare this to previous version where lock
	 * was released after detaching chain. It was racy,
	 * because tw buckets are scheduled in not serialized context
	 * in 2.3 (with netfilter), and with softnet it is common, because
	 * soft irqs are not sequenced.
	 */
	killed = 0;
	ret = 0;
rescan:
	inet_twsk_for_each_inmate(tw, node, &twdr->cells[slot]) {
		__inet_twsk_del_dead_node(tw);
		spin_unlock(&twdr->death_lock);
		__inet_twsk_kill(tw, twdr->hashinfo);
		inet_twsk_put(tw);
		killed++;
		spin_lock(&twdr->death_lock);
		if (killed > INET_TWDR_TWKILL_QUOTA) {
			ret = 1;
			break;
		}

		/* While we dropped twdr->death_lock, another cpu may have
		 * killed off the next TW bucket in the list, therefore
		 * do a fresh re-read of the hlist head node with the
		 * lock reacquired.  We still use the hlist traversal
		 * macro in order to get the prefetches.
		 */
		goto rescan;
	}

	twdr->tw_count -= killed;
	NET_ADD_STATS_BH(LINUX_MIB_TIMEWAITED, killed);

	return ret;
}

void inet_twdr_hangman(unsigned long data)
{
	struct inet_timewait_death_row *twdr;
	int unsigned need_timer;

	twdr = (struct inet_timewait_death_row *)data;
	spin_lock(&twdr->death_lock);

	if (twdr->tw_count == 0)
		goto out;

	need_timer = 0;
	if (inet_twdr_do_twkill_work(twdr, twdr->slot)) {
		twdr->thread_slots |= (1 << twdr->slot);
		schedule_work(&twdr->twkill_work);
		need_timer = 1;
	} else {
		/* We purged the entire slot, anything left?  */
		if (twdr->tw_count)
			need_timer = 1;
	}
	twdr->slot = ((twdr->slot + 1) & (INET_TWDR_TWKILL_SLOTS - 1));
	if (need_timer)
		mod_timer(&twdr->tw_timer, jiffies + twdr->period);
out:
	spin_unlock(&twdr->death_lock);
}

EXPORT_SYMBOL_GPL(inet_twdr_hangman);

extern void twkill_slots_invalid(void);

void inet_twdr_twkill_work(struct work_struct *work)
{
	struct inet_timewait_death_row *twdr =
		container_of(work, struct inet_timewait_death_row, twkill_work);
	int i;

	if ((INET_TWDR_TWKILL_SLOTS - 1) > (sizeof(twdr->thread_slots) * 8))
		twkill_slots_invalid();

	while (twdr->thread_slots) {
		spin_lock_bh(&twdr->death_lock);
		for (i = 0; i < INET_TWDR_TWKILL_SLOTS; i++) {
			if (!(twdr->thread_slots & (1 << i)))
				continue;

			while (inet_twdr_do_twkill_work(twdr, i) != 0) {
				if (need_resched()) {
					spin_unlock_bh(&twdr->death_lock);
					schedule();
					spin_lock_bh(&twdr->death_lock);
				}
			}

			twdr->thread_slots &= ~(1 << i);
		}
		spin_unlock_bh(&twdr->death_lock);
	}
}

EXPORT_SYMBOL_GPL(inet_twdr_twkill_work);

/* These are always called from BH context.  See callers in
 * tcp_input.c to verify this.
 */

/* This is for handling early-kills of TIME_WAIT sockets. */
void inet_twsk_deschedule(struct inet_timewait_sock *tw,
			  struct inet_timewait_death_row *twdr)
{
	spin_lock(&twdr->death_lock);
	if (inet_twsk_del_dead_node(tw)) {
		inet_twsk_put(tw);
		if (--twdr->tw_count == 0)
			del_timer(&twdr->tw_timer);
	}
	spin_unlock(&twdr->death_lock);
	__inet_twsk_kill(tw, twdr->hashinfo);
}

EXPORT_SYMBOL(inet_twsk_deschedule);

void inet_twsk_schedule(struct inet_timewait_sock *tw,
		       struct inet_timewait_death_row *twdr,
		       const int timeo, const int timewait_len)
{
	struct hlist_head *list;
	int slot;

	/* timeout := RTO * 3.5
	 *
	 * 3.5 = 1+2+0.5 to wait for two retransmits.
	 *
	 * RATIONALE: if FIN arrived and we entered TIME-WAIT state,
	 * our ACK acking that FIN can be lost. If N subsequent retransmitted
	 * FINs (or previous seqments) are lost (probability of such event
	 * is p^(N+1), where p is probability to lose single packet and
	 * time to detect the loss is about RTO*(2^N - 1) with exponential
	 * backoff). Normal timewait length is calculated so, that we
	 * waited at least for one retransmitted FIN (maximal RTO is 120sec).
	 * [ BTW Linux. following BSD, violates this requirement waiting
	 *   only for 60sec, we should wait at least for 240 secs.
	 *   Well, 240 consumes too much of resources 8)
	 * ]
	 * This interval is not reduced to catch old duplicate and
	 * responces to our wandering segments living for two MSLs.
	 * However, if we use PAWS to detect
	 * old duplicates, we can reduce the interval to bounds required
	 * by RTO, rather than MSL. So, if peer understands PAWS, we
	 * kill tw bucket after 3.5*RTO (it is important that this number
	 * is greater than TS tick!) and detect old duplicates with help
	 * of PAWS.
	 */
	slot = (timeo + (1 << INET_TWDR_RECYCLE_TICK) - 1) >> INET_TWDR_RECYCLE_TICK;

	spin_lock(&twdr->death_lock);

	/* Unlink it, if it was scheduled */
	if (inet_twsk_del_dead_node(tw))
		twdr->tw_count--;
	else
		atomic_inc(&tw->tw_refcnt);

	if (slot >= INET_TWDR_RECYCLE_SLOTS) {
		/* Schedule to slow timer */
		if (timeo >= timewait_len) {
			slot = INET_TWDR_TWKILL_SLOTS - 1;
		} else {
			slot = (timeo + twdr->period - 1) / twdr->period;
			if (slot >= INET_TWDR_TWKILL_SLOTS)
				slot = INET_TWDR_TWKILL_SLOTS - 1;
		}
		tw->tw_ttd = jiffies + timeo;
		slot = (twdr->slot + slot) & (INET_TWDR_TWKILL_SLOTS - 1);
		list = &twdr->cells[slot];
	} else {
		tw->tw_ttd = jiffies + (slot << INET_TWDR_RECYCLE_TICK);

		if (twdr->twcal_hand < 0) {
			twdr->twcal_hand = 0;
			twdr->twcal_jiffie = jiffies;
			twdr->twcal_timer.expires = twdr->twcal_jiffie +
					      (slot << INET_TWDR_RECYCLE_TICK);
			add_timer(&twdr->twcal_timer);
		} else {
			if (time_after(twdr->twcal_timer.expires,
				       jiffies + (slot << INET_TWDR_RECYCLE_TICK)))
				mod_timer(&twdr->twcal_timer,
					  jiffies + (slot << INET_TWDR_RECYCLE_TICK));
			slot = (twdr->twcal_hand + slot) & (INET_TWDR_RECYCLE_SLOTS - 1);
		}
		list = &twdr->twcal_row[slot];
	}

	hlist_add_head(&tw->tw_death_node, list);

	if (twdr->tw_count++ == 0)
		mod_timer(&twdr->tw_timer, jiffies + twdr->period);
	spin_unlock(&twdr->death_lock);
}

EXPORT_SYMBOL_GPL(inet_twsk_schedule);

void inet_twdr_twcal_tick(unsigned long data)
{
	struct inet_timewait_death_row *twdr;
	int n, slot;
	unsigned long j;
	unsigned long now = jiffies;
	int killed = 0;
	int adv = 0;

	twdr = (struct inet_timewait_death_row *)data;

	spin_lock(&twdr->death_lock);
	if (twdr->twcal_hand < 0)
		goto out;

	slot = twdr->twcal_hand;
	j = twdr->twcal_jiffie;

	for (n = 0; n < INET_TWDR_RECYCLE_SLOTS; n++) {
		if (time_before_eq(j, now)) {
			struct hlist_node *node, *safe;
			struct inet_timewait_sock *tw;

			inet_twsk_for_each_inmate_safe(tw, node, safe,
						       &twdr->twcal_row[slot]) {
				__inet_twsk_del_dead_node(tw);
				__inet_twsk_kill(tw, twdr->hashinfo);
				inet_twsk_put(tw);
				killed++;
			}
		} else {
			if (!adv) {
				adv = 1;
				twdr->twcal_jiffie = j;
				twdr->twcal_hand = slot;
			}

			if (!hlist_empty(&twdr->twcal_row[slot])) {
				mod_timer(&twdr->twcal_timer, j);
				goto out;
			}
		}
		j += 1 << INET_TWDR_RECYCLE_TICK;
		slot = (slot + 1) & (INET_TWDR_RECYCLE_SLOTS - 1);
	}
	twdr->twcal_hand = -1;

out:
	if ((twdr->tw_count -= killed) == 0)
		del_timer(&twdr->tw_timer);
	NET_ADD_STATS_BH(LINUX_MIB_TIMEWAITKILLED, killed);
	spin_unlock(&twdr->death_lock);
}

EXPORT_SYMBOL_GPL(inet_twdr_twcal_tick);
