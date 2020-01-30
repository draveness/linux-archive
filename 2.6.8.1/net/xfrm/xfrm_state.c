/*
 * xfrm_state.c
 *
 * Changes:
 *	Mitsuru KANDA @USAGI
 * 	Kazunori MIYAZAWA @USAGI
 * 	Kunihiro Ishiguro <kunihiro@ipinfusion.com>
 * 		IPv6 support
 * 	YOSHIFUJI Hideaki @USAGI
 * 		Split up af-specific functions
 *	Derek Atkins <derek@ihtfp.com>
 *		Add UDP Encapsulation
 * 	
 */

#include <linux/workqueue.h>
#include <net/xfrm.h>
#include <linux/pfkeyv2.h>
#include <linux/ipsec.h>
#include <asm/uaccess.h>

/* Each xfrm_state may be linked to two tables:

   1. Hash table by (spi,daddr,ah/esp) to find SA by SPI. (input,ctl)
   2. Hash table by daddr to find what SAs exist for given
      destination/tunnel endpoint. (output)
 */

static spinlock_t xfrm_state_lock = SPIN_LOCK_UNLOCKED;

/* Hash table to find appropriate SA towards given target (endpoint
 * of tunnel or destination of transport mode) allowed by selector.
 *
 * Main use is finding SA after policy selected tunnel or transport mode.
 * Also, it can be used by ah/esp icmp error handler to find offending SA.
 */
static struct list_head xfrm_state_bydst[XFRM_DST_HSIZE];
static struct list_head xfrm_state_byspi[XFRM_DST_HSIZE];

DECLARE_WAIT_QUEUE_HEAD(km_waitq);

static rwlock_t xfrm_state_afinfo_lock = RW_LOCK_UNLOCKED;
static struct xfrm_state_afinfo *xfrm_state_afinfo[NPROTO];

static struct work_struct xfrm_state_gc_work;
static struct list_head xfrm_state_gc_list = LIST_HEAD_INIT(xfrm_state_gc_list);
static spinlock_t xfrm_state_gc_lock = SPIN_LOCK_UNLOCKED;

static void __xfrm_state_delete(struct xfrm_state *x);

static void xfrm_state_gc_destroy(struct xfrm_state *x)
{
	if (del_timer(&x->timer))
		BUG();
	if (x->aalg)
		kfree(x->aalg);
	if (x->ealg)
		kfree(x->ealg);
	if (x->calg)
		kfree(x->calg);
	if (x->encap)
		kfree(x->encap);
	if (x->type) {
		x->type->destructor(x);
		xfrm_put_type(x->type);
	}
	kfree(x);
}

static void xfrm_state_gc_task(void *data)
{
	struct xfrm_state *x;
	struct list_head *entry, *tmp;
	struct list_head gc_list = LIST_HEAD_INIT(gc_list);

	spin_lock_bh(&xfrm_state_gc_lock);
	list_splice_init(&xfrm_state_gc_list, &gc_list);
	spin_unlock_bh(&xfrm_state_gc_lock);

	list_for_each_safe(entry, tmp, &gc_list) {
		x = list_entry(entry, struct xfrm_state, bydst);
		xfrm_state_gc_destroy(x);
	}
	wake_up(&km_waitq);
}

static inline unsigned long make_jiffies(long secs)
{
	if (secs >= (MAX_SCHEDULE_TIMEOUT-1)/HZ)
		return MAX_SCHEDULE_TIMEOUT-1;
	else
	        return secs*HZ;
}

static void xfrm_timer_handler(unsigned long data)
{
	struct xfrm_state *x = (struct xfrm_state*)data;
	unsigned long now = (unsigned long)xtime.tv_sec;
	long next = LONG_MAX;
	int warn = 0;

	spin_lock(&x->lock);
	if (x->km.state == XFRM_STATE_DEAD)
		goto out;
	if (x->km.state == XFRM_STATE_EXPIRED)
		goto expired;
	if (x->lft.hard_add_expires_seconds) {
		long tmo = x->lft.hard_add_expires_seconds +
			x->curlft.add_time - now;
		if (tmo <= 0)
			goto expired;
		if (tmo < next)
			next = tmo;
	}
	if (x->lft.hard_use_expires_seconds) {
		long tmo = x->lft.hard_use_expires_seconds +
			(x->curlft.use_time ? : now) - now;
		if (tmo <= 0)
			goto expired;
		if (tmo < next)
			next = tmo;
	}
	if (x->km.dying)
		goto resched;
	if (x->lft.soft_add_expires_seconds) {
		long tmo = x->lft.soft_add_expires_seconds +
			x->curlft.add_time - now;
		if (tmo <= 0)
			warn = 1;
		else if (tmo < next)
			next = tmo;
	}
	if (x->lft.soft_use_expires_seconds) {
		long tmo = x->lft.soft_use_expires_seconds +
			(x->curlft.use_time ? : now) - now;
		if (tmo <= 0)
			warn = 1;
		else if (tmo < next)
			next = tmo;
	}

	if (warn)
		km_state_expired(x, 0);
resched:
	if (next != LONG_MAX &&
	    !mod_timer(&x->timer, jiffies + make_jiffies(next)))
		xfrm_state_hold(x);
	goto out;

expired:
	if (x->km.state == XFRM_STATE_ACQ && x->id.spi == 0) {
		x->km.state = XFRM_STATE_EXPIRED;
		wake_up(&km_waitq);
		next = 2;
		goto resched;
	}
	if (x->id.spi != 0)
		km_state_expired(x, 1);
	__xfrm_state_delete(x);

out:
	spin_unlock(&x->lock);
	xfrm_state_put(x);
}

struct xfrm_state *xfrm_state_alloc(void)
{
	struct xfrm_state *x;

	x = kmalloc(sizeof(struct xfrm_state), GFP_ATOMIC);

	if (x) {
		memset(x, 0, sizeof(struct xfrm_state));
		atomic_set(&x->refcnt, 1);
		atomic_set(&x->tunnel_users, 0);
		INIT_LIST_HEAD(&x->bydst);
		INIT_LIST_HEAD(&x->byspi);
		init_timer(&x->timer);
		x->timer.function = xfrm_timer_handler;
		x->timer.data	  = (unsigned long)x;
		x->curlft.add_time = (unsigned long)xtime.tv_sec;
		x->lft.soft_byte_limit = XFRM_INF;
		x->lft.soft_packet_limit = XFRM_INF;
		x->lft.hard_byte_limit = XFRM_INF;
		x->lft.hard_packet_limit = XFRM_INF;
		x->lock = SPIN_LOCK_UNLOCKED;
	}
	return x;
}

void __xfrm_state_destroy(struct xfrm_state *x)
{
	BUG_TRAP(x->km.state == XFRM_STATE_DEAD);

	spin_lock_bh(&xfrm_state_gc_lock);
	list_add(&x->bydst, &xfrm_state_gc_list);
	spin_unlock_bh(&xfrm_state_gc_lock);
	schedule_work(&xfrm_state_gc_work);
}

static void __xfrm_state_delete(struct xfrm_state *x)
{
	if (x->km.state != XFRM_STATE_DEAD) {
		x->km.state = XFRM_STATE_DEAD;
		spin_lock(&xfrm_state_lock);
		list_del(&x->bydst);
		atomic_dec(&x->refcnt);
		if (x->id.spi) {
			list_del(&x->byspi);
			atomic_dec(&x->refcnt);
		}
		spin_unlock(&xfrm_state_lock);
		if (del_timer(&x->timer))
			atomic_dec(&x->refcnt);

		/* The number two in this test is the reference
		 * mentioned in the comment below plus the reference
		 * our caller holds.  A larger value means that
		 * there are DSTs attached to this xfrm_state.
		 */
		if (atomic_read(&x->refcnt) > 2)
			xfrm_flush_bundles();

		/* All xfrm_state objects are created by xfrm_state_alloc.
		 * The xfrm_state_alloc call gives a reference, and that
		 * is what we are dropping here.
		 */
		atomic_dec(&x->refcnt);
	}
}

void xfrm_state_delete(struct xfrm_state *x)
{
	spin_lock_bh(&x->lock);
	__xfrm_state_delete(x);
	spin_unlock_bh(&x->lock);
}

void xfrm_state_flush(u8 proto)
{
	int i;
	struct xfrm_state *x;

	spin_lock_bh(&xfrm_state_lock);
	for (i = 0; i < XFRM_DST_HSIZE; i++) {
restart:
		list_for_each_entry(x, xfrm_state_bydst+i, bydst) {
			if (!xfrm_state_kern(x) &&
			    (proto == IPSEC_PROTO_ANY || x->id.proto == proto)) {
				xfrm_state_hold(x);
				spin_unlock_bh(&xfrm_state_lock);

				xfrm_state_delete(x);
				xfrm_state_put(x);

				spin_lock_bh(&xfrm_state_lock);
				goto restart;
			}
		}
	}
	spin_unlock_bh(&xfrm_state_lock);
	wake_up(&km_waitq);
}

static int
xfrm_init_tempsel(struct xfrm_state *x, struct flowi *fl,
		  struct xfrm_tmpl *tmpl,
		  xfrm_address_t *daddr, xfrm_address_t *saddr,
		  unsigned short family)
{
	struct xfrm_state_afinfo *afinfo = xfrm_state_get_afinfo(family);
	if (!afinfo)
		return -1;
	afinfo->init_tempsel(x, fl, tmpl, daddr, saddr);
	xfrm_state_put_afinfo(afinfo);
	return 0;
}

struct xfrm_state *
xfrm_state_find(xfrm_address_t *daddr, xfrm_address_t *saddr, 
		struct flowi *fl, struct xfrm_tmpl *tmpl,
		struct xfrm_policy *pol, int *err,
		unsigned short family)
{
	unsigned h = xfrm_dst_hash(daddr, family);
	struct xfrm_state *x;
	int acquire_in_progress = 0;
	int error = 0;
	struct xfrm_state *best = NULL;

	spin_lock_bh(&xfrm_state_lock);
	list_for_each_entry(x, xfrm_state_bydst+h, bydst) {
		if (x->props.family == family &&
		    x->props.reqid == tmpl->reqid &&
		    xfrm_state_addr_check(x, daddr, saddr, family) &&
		    tmpl->mode == x->props.mode &&
		    tmpl->id.proto == x->id.proto) {
			/* Resolution logic:
			   1. There is a valid state with matching selector.
			      Done.
			   2. Valid state with inappropriate selector. Skip.

			   Entering area of "sysdeps".

			   3. If state is not valid, selector is temporary,
			      it selects only session which triggered
			      previous resolution. Key manager will do
			      something to install a state with proper
			      selector.
			 */
			if (x->km.state == XFRM_STATE_VALID) {
				if (!xfrm_selector_match(&x->sel, fl, family))
					continue;
				if (!best ||
				    best->km.dying > x->km.dying ||
				    (best->km.dying == x->km.dying &&
				     best->curlft.add_time < x->curlft.add_time))
					best = x;
			} else if (x->km.state == XFRM_STATE_ACQ) {
				acquire_in_progress = 1;
			} else if (x->km.state == XFRM_STATE_ERROR ||
				   x->km.state == XFRM_STATE_EXPIRED) {
				if (xfrm_selector_match(&x->sel, fl, family))
					error = 1;
			}
		}
	}

	x = best;
	if (!x && !error && !acquire_in_progress &&
	    ((x = xfrm_state_alloc()) != NULL)) {
		/* Initialize temporary selector matching only
		 * to current session. */
		xfrm_init_tempsel(x, fl, tmpl, daddr, saddr, family);

		if (km_query(x, tmpl, pol) == 0) {
			x->km.state = XFRM_STATE_ACQ;
			list_add_tail(&x->bydst, xfrm_state_bydst+h);
			xfrm_state_hold(x);
			if (x->id.spi) {
				h = xfrm_spi_hash(&x->id.daddr, x->id.spi, x->id.proto, family);
				list_add(&x->byspi, xfrm_state_byspi+h);
				xfrm_state_hold(x);
			}
			x->lft.hard_add_expires_seconds = XFRM_ACQ_EXPIRES;
			xfrm_state_hold(x);
			x->timer.expires = jiffies + XFRM_ACQ_EXPIRES*HZ;
			add_timer(&x->timer);
		} else {
			x->km.state = XFRM_STATE_DEAD;
			xfrm_state_put(x);
			x = NULL;
			error = 1;
		}
	}
	if (x)
		xfrm_state_hold(x);
	else
		*err = acquire_in_progress ? -EAGAIN :
			(error ? -ESRCH : -ENOMEM);
	spin_unlock_bh(&xfrm_state_lock);
	return x;
}

static void __xfrm_state_insert(struct xfrm_state *x)
{
	unsigned h = xfrm_dst_hash(&x->id.daddr, x->props.family);

	list_add(&x->bydst, xfrm_state_bydst+h);
	xfrm_state_hold(x);

	h = xfrm_spi_hash(&x->id.daddr, x->id.spi, x->id.proto, x->props.family);

	list_add(&x->byspi, xfrm_state_byspi+h);
	xfrm_state_hold(x);

	if (!mod_timer(&x->timer, jiffies + HZ))
		xfrm_state_hold(x);

	wake_up(&km_waitq);
}

void xfrm_state_insert(struct xfrm_state *x)
{
	spin_lock_bh(&xfrm_state_lock);
	__xfrm_state_insert(x);
	spin_unlock_bh(&xfrm_state_lock);
}

int xfrm_state_add(struct xfrm_state *x)
{
	struct xfrm_state_afinfo *afinfo;
	struct xfrm_state *x1;
	int err;

	afinfo = xfrm_state_get_afinfo(x->props.family);
	if (unlikely(afinfo == NULL))
		return -EAFNOSUPPORT;

	spin_lock_bh(&xfrm_state_lock);

	x1 = afinfo->state_lookup(&x->id.daddr, x->id.spi, x->id.proto);
	if (x1) {
		xfrm_state_put(x1);
		x1 = NULL;
		err = -EEXIST;
		goto out;
	}

	x1 = afinfo->find_acq(
		x->props.mode, x->props.reqid, x->id.proto,
		&x->id.daddr, &x->props.saddr, 0);

	__xfrm_state_insert(x);
	err = 0;

out:
	spin_unlock_bh(&xfrm_state_lock);
	xfrm_state_put_afinfo(afinfo);

	if (x1) {
		xfrm_state_delete(x1);
		xfrm_state_put(x1);
	}

	return err;
}

int xfrm_state_update(struct xfrm_state *x)
{
	struct xfrm_state_afinfo *afinfo;
	struct xfrm_state *x1;
	int err;

	afinfo = xfrm_state_get_afinfo(x->props.family);
	if (unlikely(afinfo == NULL))
		return -EAFNOSUPPORT;

	spin_lock_bh(&xfrm_state_lock);
	x1 = afinfo->state_lookup(&x->id.daddr, x->id.spi, x->id.proto);

	err = -ESRCH;
	if (!x1)
		goto out;

	if (xfrm_state_kern(x1)) {
		xfrm_state_put(x1);
		err = -EEXIST;
		goto out;
	}

	if (x1->km.state == XFRM_STATE_ACQ) {
		__xfrm_state_insert(x);
		x = NULL;
	}
	err = 0;

out:
	spin_unlock_bh(&xfrm_state_lock);
	xfrm_state_put_afinfo(afinfo);

	if (err)
		return err;

	if (!x) {
		xfrm_state_delete(x1);
		xfrm_state_put(x1);
		return 0;
	}

	err = -EINVAL;
	spin_lock_bh(&x1->lock);
	if (likely(x1->km.state == XFRM_STATE_VALID)) {
		if (x->encap && x1->encap)
			memcpy(x1->encap, x->encap, sizeof(*x1->encap));
		memcpy(&x1->lft, &x->lft, sizeof(x1->lft));
		x1->km.dying = 0;

		if (!mod_timer(&x1->timer, jiffies + HZ))
			xfrm_state_hold(x1);
		if (x1->curlft.use_time)
			xfrm_state_check_expire(x1);

		err = 0;
	}
	spin_unlock_bh(&x1->lock);

	xfrm_state_put(x1);

	return err;
}

int xfrm_state_check_expire(struct xfrm_state *x)
{
	if (!x->curlft.use_time)
		x->curlft.use_time = (unsigned long)xtime.tv_sec;

	if (x->km.state != XFRM_STATE_VALID)
		return -EINVAL;

	if (x->curlft.bytes >= x->lft.hard_byte_limit ||
	    x->curlft.packets >= x->lft.hard_packet_limit) {
		km_state_expired(x, 1);
		if (!mod_timer(&x->timer, jiffies + XFRM_ACQ_EXPIRES*HZ))
			xfrm_state_hold(x);
		return -EINVAL;
	}

	if (!x->km.dying &&
	    (x->curlft.bytes >= x->lft.soft_byte_limit ||
	     x->curlft.packets >= x->lft.soft_packet_limit))
		km_state_expired(x, 0);
	return 0;
}

int xfrm_state_check_space(struct xfrm_state *x, struct sk_buff *skb)
{
	int nhead = x->props.header_len + LL_RESERVED_SPACE(skb->dst->dev)
		- skb_headroom(skb);

	if (nhead > 0)
		return pskb_expand_head(skb, nhead, 0, GFP_ATOMIC);

	/* Check tail too... */
	return 0;
}

int xfrm_state_check(struct xfrm_state *x, struct sk_buff *skb)
{
	int err = xfrm_state_check_expire(x);
	if (err < 0)
		goto err;
	err = xfrm_state_check_space(x, skb);
err:
	return err;
}

struct xfrm_state *
xfrm_state_lookup(xfrm_address_t *daddr, u32 spi, u8 proto,
		  unsigned short family)
{
	struct xfrm_state *x;
	struct xfrm_state_afinfo *afinfo = xfrm_state_get_afinfo(family);
	if (!afinfo)
		return NULL;

	spin_lock_bh(&xfrm_state_lock);
	x = afinfo->state_lookup(daddr, spi, proto);
	spin_unlock_bh(&xfrm_state_lock);
	xfrm_state_put_afinfo(afinfo);
	return x;
}

struct xfrm_state *
xfrm_find_acq(u8 mode, u32 reqid, u8 proto, 
	      xfrm_address_t *daddr, xfrm_address_t *saddr, 
	      int create, unsigned short family)
{
	struct xfrm_state *x;
	struct xfrm_state_afinfo *afinfo = xfrm_state_get_afinfo(family);
	if (!afinfo)
		return NULL;

	spin_lock_bh(&xfrm_state_lock);
	x = afinfo->find_acq(mode, reqid, proto, daddr, saddr, create);
	spin_unlock_bh(&xfrm_state_lock);
	xfrm_state_put_afinfo(afinfo);
	return x;
}

/* Silly enough, but I'm lazy to build resolution list */

struct xfrm_state * xfrm_find_acq_byseq(u32 seq)
{
	int i;
	struct xfrm_state *x;

	spin_lock_bh(&xfrm_state_lock);
	for (i = 0; i < XFRM_DST_HSIZE; i++) {
		list_for_each_entry(x, xfrm_state_bydst+i, bydst) {
			if (x->km.seq == seq) {
				xfrm_state_hold(x);
				spin_unlock_bh(&xfrm_state_lock);
				return x;
			}
		}
	}
	spin_unlock_bh(&xfrm_state_lock);
	return NULL;
}
 
u32 xfrm_get_acqseq(void)
{
	u32 res;
	static u32 acqseq;
	static spinlock_t acqseq_lock = SPIN_LOCK_UNLOCKED;

	spin_lock_bh(&acqseq_lock);
	res = (++acqseq ? : ++acqseq);
	spin_unlock_bh(&acqseq_lock);
	return res;
}

void
xfrm_alloc_spi(struct xfrm_state *x, u32 minspi, u32 maxspi)
{
	u32 h;
	struct xfrm_state *x0;

	if (x->id.spi)
		return;

	if (minspi == maxspi) {
		x0 = xfrm_state_lookup(&x->id.daddr, minspi, x->id.proto, x->props.family);
		if (x0) {
			xfrm_state_put(x0);
			return;
		}
		x->id.spi = minspi;
	} else {
		u32 spi = 0;
		minspi = ntohl(minspi);
		maxspi = ntohl(maxspi);
		for (h=0; h<maxspi-minspi+1; h++) {
			spi = minspi + net_random()%(maxspi-minspi+1);
			x0 = xfrm_state_lookup(&x->id.daddr, htonl(spi), x->id.proto, x->props.family);
			if (x0 == NULL) {
				x->id.spi = htonl(spi);
				break;
			}
			xfrm_state_put(x0);
		}
	}
	if (x->id.spi) {
		spin_lock_bh(&xfrm_state_lock);
		h = xfrm_spi_hash(&x->id.daddr, x->id.spi, x->id.proto, x->props.family);
		list_add(&x->byspi, xfrm_state_byspi+h);
		xfrm_state_hold(x);
		spin_unlock_bh(&xfrm_state_lock);
		wake_up(&km_waitq);
	}
}

int xfrm_state_walk(u8 proto, int (*func)(struct xfrm_state *, int, void*),
		    void *data)
{
	int i;
	struct xfrm_state *x;
	int count = 0;
	int err = 0;

	spin_lock_bh(&xfrm_state_lock);
	for (i = 0; i < XFRM_DST_HSIZE; i++) {
		list_for_each_entry(x, xfrm_state_bydst+i, bydst) {
			if (proto == IPSEC_PROTO_ANY || x->id.proto == proto)
				count++;
		}
	}
	if (count == 0) {
		err = -ENOENT;
		goto out;
	}

	for (i = 0; i < XFRM_DST_HSIZE; i++) {
		list_for_each_entry(x, xfrm_state_bydst+i, bydst) {
			if (proto != IPSEC_PROTO_ANY && x->id.proto != proto)
				continue;
			err = func(x, --count, data);
			if (err)
				goto out;
		}
	}
out:
	spin_unlock_bh(&xfrm_state_lock);
	return err;
}


int xfrm_replay_check(struct xfrm_state *x, u32 seq)
{
	u32 diff;

	seq = ntohl(seq);

	if (unlikely(seq == 0))
		return -EINVAL;

	if (likely(seq > x->replay.seq))
		return 0;

	diff = x->replay.seq - seq;
	if (diff >= x->props.replay_window) {
		x->stats.replay_window++;
		return -EINVAL;
	}

	if (x->replay.bitmap & (1U << diff)) {
		x->stats.replay++;
		return -EINVAL;
	}
	return 0;
}

void xfrm_replay_advance(struct xfrm_state *x, u32 seq)
{
	u32 diff;

	seq = ntohl(seq);

	if (seq > x->replay.seq) {
		diff = seq - x->replay.seq;
		if (diff < x->props.replay_window)
			x->replay.bitmap = ((x->replay.bitmap) << diff) | 1;
		else
			x->replay.bitmap = 1;
		x->replay.seq = seq;
	} else {
		diff = x->replay.seq - seq;
		x->replay.bitmap |= (1U << diff);
	}
}

int xfrm_check_selectors(struct xfrm_state **x, int n, struct flowi *fl)
{
	int i;

	for (i=0; i<n; i++) {
		int match;
		match = xfrm_selector_match(&x[i]->sel, fl, x[i]->props.family);
		if (!match)
			return -EINVAL;
	}
	return 0;
}

static struct list_head xfrm_km_list = LIST_HEAD_INIT(xfrm_km_list);
static rwlock_t		xfrm_km_lock = RW_LOCK_UNLOCKED;

void km_state_expired(struct xfrm_state *x, int hard)
{
	struct xfrm_mgr *km;

	if (hard)
		x->km.state = XFRM_STATE_EXPIRED;
	else
		x->km.dying = 1;

	read_lock(&xfrm_km_lock);
	list_for_each_entry(km, &xfrm_km_list, list)
		km->notify(x, hard);
	read_unlock(&xfrm_km_lock);

	if (hard)
		wake_up(&km_waitq);
}

int km_query(struct xfrm_state *x, struct xfrm_tmpl *t, struct xfrm_policy *pol)
{
	int err = -EINVAL;
	struct xfrm_mgr *km;

	read_lock(&xfrm_km_lock);
	list_for_each_entry(km, &xfrm_km_list, list) {
		err = km->acquire(x, t, pol, XFRM_POLICY_OUT);
		if (!err)
			break;
	}
	read_unlock(&xfrm_km_lock);
	return err;
}

int km_new_mapping(struct xfrm_state *x, xfrm_address_t *ipaddr, u16 sport)
{
	int err = -EINVAL;
	struct xfrm_mgr *km;

	read_lock(&xfrm_km_lock);
	list_for_each_entry(km, &xfrm_km_list, list) {
		if (km->new_mapping)
			err = km->new_mapping(x, ipaddr, sport);
		if (!err)
			break;
	}
	read_unlock(&xfrm_km_lock);
	return err;
}

void km_policy_expired(struct xfrm_policy *pol, int dir, int hard)
{
	struct xfrm_mgr *km;

	read_lock(&xfrm_km_lock);
	list_for_each_entry(km, &xfrm_km_list, list)
		if (km->notify_policy)
			km->notify_policy(pol, dir, hard);
	read_unlock(&xfrm_km_lock);

	if (hard)
		wake_up(&km_waitq);
}

int xfrm_user_policy(struct sock *sk, int optname, u8 __user *optval, int optlen)
{
	int err;
	u8 *data;
	struct xfrm_mgr *km;
	struct xfrm_policy *pol = NULL;

	if (optlen <= 0 || optlen > PAGE_SIZE)
		return -EMSGSIZE;

	data = kmalloc(optlen, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	err = -EFAULT;
	if (copy_from_user(data, optval, optlen))
		goto out;

	err = -EINVAL;
	read_lock(&xfrm_km_lock);
	list_for_each_entry(km, &xfrm_km_list, list) {
		pol = km->compile_policy(sk->sk_family, optname, data,
					 optlen, &err);
		if (err >= 0)
			break;
	}
	read_unlock(&xfrm_km_lock);

	if (err >= 0) {
		xfrm_sk_policy_insert(sk, err, pol);
		xfrm_pol_put(pol);
		err = 0;
	}

out:
	kfree(data);
	return err;
}

int xfrm_register_km(struct xfrm_mgr *km)
{
	write_lock_bh(&xfrm_km_lock);
	list_add_tail(&km->list, &xfrm_km_list);
	write_unlock_bh(&xfrm_km_lock);
	return 0;
}

int xfrm_unregister_km(struct xfrm_mgr *km)
{
	write_lock_bh(&xfrm_km_lock);
	list_del(&km->list);
	write_unlock_bh(&xfrm_km_lock);
	return 0;
}

int xfrm_state_register_afinfo(struct xfrm_state_afinfo *afinfo)
{
	int err = 0;
	if (unlikely(afinfo == NULL))
		return -EINVAL;
	if (unlikely(afinfo->family >= NPROTO))
		return -EAFNOSUPPORT;
	write_lock(&xfrm_state_afinfo_lock);
	if (unlikely(xfrm_state_afinfo[afinfo->family] != NULL))
		err = -ENOBUFS;
	else {
		afinfo->state_bydst = xfrm_state_bydst;
		afinfo->state_byspi = xfrm_state_byspi;
		xfrm_state_afinfo[afinfo->family] = afinfo;
	}
	write_unlock(&xfrm_state_afinfo_lock);
	return err;
}

int xfrm_state_unregister_afinfo(struct xfrm_state_afinfo *afinfo)
{
	int err = 0;
	if (unlikely(afinfo == NULL))
		return -EINVAL;
	if (unlikely(afinfo->family >= NPROTO))
		return -EAFNOSUPPORT;
	write_lock(&xfrm_state_afinfo_lock);
	if (likely(xfrm_state_afinfo[afinfo->family] != NULL)) {
		if (unlikely(xfrm_state_afinfo[afinfo->family] != afinfo))
			err = -EINVAL;
		else {
			xfrm_state_afinfo[afinfo->family] = NULL;
			afinfo->state_byspi = NULL;
			afinfo->state_bydst = NULL;
		}
	}
	write_unlock(&xfrm_state_afinfo_lock);
	return err;
}

struct xfrm_state_afinfo *xfrm_state_get_afinfo(unsigned short family)
{
	struct xfrm_state_afinfo *afinfo;
	if (unlikely(family >= NPROTO))
		return NULL;
	read_lock(&xfrm_state_afinfo_lock);
	afinfo = xfrm_state_afinfo[family];
	if (likely(afinfo != NULL))
		read_lock(&afinfo->lock);
	read_unlock(&xfrm_state_afinfo_lock);
	return afinfo;
}

void xfrm_state_put_afinfo(struct xfrm_state_afinfo *afinfo)
{
	if (unlikely(afinfo == NULL))
		return;
	read_unlock(&afinfo->lock);
}

/* Temporarily located here until net/xfrm/xfrm_tunnel.c is created */
void xfrm_state_delete_tunnel(struct xfrm_state *x)
{
	if (x->tunnel) {
		struct xfrm_state *t = x->tunnel;

		if (atomic_read(&t->tunnel_users) == 2)
			xfrm_state_delete(t);
		atomic_dec(&t->tunnel_users);
		xfrm_state_put(t);
		x->tunnel = NULL;
	}
}

void __init xfrm_state_init(void)
{
	int i;

	for (i=0; i<XFRM_DST_HSIZE; i++) {
		INIT_LIST_HEAD(&xfrm_state_bydst[i]);
		INIT_LIST_HEAD(&xfrm_state_byspi[i]);
	}
	INIT_WORK(&xfrm_state_gc_work, xfrm_state_gc_task, NULL);
}

