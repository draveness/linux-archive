/*
 * linux/fs/lockd/host.c
 *
 * Management for NLM peer hosts. The nlm_host struct is shared
 * between client and server implementation. The only reason to
 * do so is to reduce code bloat.
 *
 * Copyright (C) 1996, Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/malloc.h>
#include <linux/in.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svc.h>
#include <linux/lockd/lockd.h>
#include <linux/lockd/sm_inter.h>


#define NLMDBG_FACILITY		NLMDBG_HOSTCACHE
#define NLM_HOST_MAX		64
#define NLM_HOST_NRHASH		32
#define NLM_ADDRHASH(addr)	(ntohl(addr) & (NLM_HOST_NRHASH-1))
#define NLM_PTRHASH(ptr)	((((u32)(unsigned long) ptr) / 32) & (NLM_HOST_NRHASH-1))
#define NLM_HOST_REBIND		(60 * HZ)
#define NLM_HOST_EXPIRE		((nrhosts > NLM_HOST_MAX)? 300 * HZ : 120 * HZ)
#define NLM_HOST_COLLECT	((nrhosts > NLM_HOST_MAX)? 120 * HZ :  60 * HZ)
#define NLM_HOST_ADDR(sv)	(&(sv)->s_nlmclnt->cl_xprt->addr)

static struct nlm_host *	nlm_hosts[NLM_HOST_NRHASH];
static unsigned long		next_gc;
static int			nrhosts;
static DECLARE_MUTEX(nlm_host_sema);


static void			nlm_gc_hosts(void);

/*
 * Find an NLM server handle in the cache. If there is none, create it.
 */
struct nlm_host *
nlmclnt_lookup_host(struct sockaddr_in *sin, int proto, int version)
{
	return nlm_lookup_host(NULL, sin, proto, version);
}

/*
 * Find an NLM client handle in the cache. If there is none, create it.
 */
struct nlm_host *
nlmsvc_lookup_host(struct svc_rqst *rqstp)
{
	return nlm_lookup_host(rqstp->rq_client, &rqstp->rq_addr, 0, 0);
}

/*
 * Match the given host against client/address
 */
static inline int
nlm_match_host(struct nlm_host *host, struct svc_client *clnt,
					struct sockaddr_in *sin)
{
	if (clnt)
		return host->h_exportent == clnt;
	return nlm_cmp_addr(&host->h_addr, sin);
}

/*
 * Common host lookup routine for server & client
 */
struct nlm_host *
nlm_lookup_host(struct svc_client *clnt, struct sockaddr_in *sin,
					int proto, int version)
{
	struct nlm_host	*host, **hp;
	u32		addr;
	int		hash;

	if (!clnt && !sin) {
		printk(KERN_NOTICE "lockd: no clnt or addr in lookup_host!\n");
		return NULL;
	}

	dprintk("lockd: nlm_lookup_host(%08x, p=%d, v=%d)\n",
			(unsigned)(sin? ntohl(sin->sin_addr.s_addr) : 0), proto, version);

	if (clnt)
		hash = NLM_PTRHASH(clnt);
	else
		hash = NLM_ADDRHASH(sin->sin_addr.s_addr);

	/* Lock hash table */
	down(&nlm_host_sema);

	if (time_after_eq(jiffies, next_gc))
		nlm_gc_hosts();

	for (hp = &nlm_hosts[hash]; (host = *hp); hp = &host->h_next) {
		if (host->h_version != version || host->h_proto != proto)
			continue;

		if (nlm_match_host(host, clnt, sin)) {
			if (hp != nlm_hosts + hash) {
				*hp = host->h_next;
				host->h_next = nlm_hosts[hash];
				nlm_hosts[hash] = host;
			}
			nlm_get_host(host);
			up(&nlm_host_sema);
			return host;
		}
	}

	/* special hack for nlmsvc_invalidate_client */
	if (sin == NULL)
		goto nohost;

	/* Ooops, no host found, create it */
	dprintk("lockd: creating host entry\n");

	if (!(host = (struct nlm_host *) kmalloc(sizeof(*host), GFP_KERNEL)))
		goto nohost;
	memset(host, 0, sizeof(*host));

	addr = sin->sin_addr.s_addr;
	sprintf(host->h_name, "%d.%d.%d.%d",
			(unsigned char) (ntohl(addr) >> 24),
			(unsigned char) (ntohl(addr) >> 16),
			(unsigned char) (ntohl(addr) >>  8),
			(unsigned char) (ntohl(addr) >>  0));

	host->h_addr       = *sin;
	host->h_addr.sin_port = 0;	/* ouch! */
	host->h_version    = version;
	host->h_proto      = proto;
	host->h_authflavor = RPC_AUTH_UNIX;
	host->h_rpcclnt    = NULL;
	init_MUTEX(&host->h_sema);
	host->h_nextrebind = jiffies + NLM_HOST_REBIND;
	host->h_expires    = jiffies + NLM_HOST_EXPIRE;
	host->h_count      = 1;
	init_waitqueue_head(&host->h_gracewait);
	host->h_state      = 0;			/* pseudo NSM state */
	host->h_nsmstate   = 0;			/* real NSM state */
	host->h_exportent  = clnt;

	host->h_next       = nlm_hosts[hash];
	nlm_hosts[hash]    = host;

	if (++nrhosts > NLM_HOST_MAX)
		next_gc = 0;

nohost:
	up(&nlm_host_sema);
	return host;
}

/*
 * Create the NLM RPC client for an NLM peer
 */
struct rpc_clnt *
nlm_bind_host(struct nlm_host *host)
{
	struct rpc_clnt	*clnt;
	struct rpc_xprt	*xprt;

	dprintk("lockd: nlm_bind_host(%08x)\n",
			(unsigned)ntohl(host->h_addr.sin_addr.s_addr));

	/* Lock host handle */
	down(&host->h_sema);

	/* If we've already created an RPC client, check whether
	 * RPC rebind is required
	 * Note: why keep rebinding if we're on a tcp connection?
	 */
	if ((clnt = host->h_rpcclnt) != NULL) {
		xprt = clnt->cl_xprt;
		if (!xprt->stream && time_after_eq(jiffies, host->h_nextrebind)) {
			clnt->cl_port = 0;
			host->h_nextrebind = jiffies + NLM_HOST_REBIND;
			dprintk("lockd: next rebind in %ld jiffies\n",
					host->h_nextrebind - jiffies);
		}
	} else {
		uid_t saved_fsuid = current->fsuid;
		kernel_cap_t saved_cap = current->cap_effective;

		/* Create RPC socket as root user so we get a priv port */
		current->fsuid = 0;
		cap_raise (current->cap_effective, CAP_NET_BIND_SERVICE);
		xprt = xprt_create_proto(host->h_proto, &host->h_addr, NULL);
		current->fsuid = saved_fsuid;
		current->cap_effective = saved_cap;
		if (xprt == NULL)
			goto forgetit;

		xprt_set_timeout(&xprt->timeout, 5, nlmsvc_timeout);

		clnt = rpc_create_client(xprt, host->h_name, &nlm_program,
					host->h_version, host->h_authflavor);
		if (clnt == NULL) {
			xprt_destroy(xprt);
			goto forgetit;
		}
		clnt->cl_autobind = 1;	/* turn on pmap queries */
		xprt->nocong = 1;	/* No congestion control for NLM */

		host->h_rpcclnt = clnt;
	}

	up(&host->h_sema);
	return clnt;

forgetit:
	printk("lockd: couldn't create RPC handle for %s\n", host->h_name);
	up(&host->h_sema);
	return NULL;
}

/*
 * Force a portmap lookup of the remote lockd port
 */
void
nlm_rebind_host(struct nlm_host *host)
{
	dprintk("lockd: rebind host %s\n", host->h_name);
	if (host->h_rpcclnt && time_after_eq(jiffies, host->h_nextrebind)) {
		host->h_rpcclnt->cl_port = 0;
		host->h_nextrebind = jiffies + NLM_HOST_REBIND;
	}
}

/*
 * Increment NLM host count
 */
struct nlm_host * nlm_get_host(struct nlm_host *host)
{
	if (host) {
		dprintk("lockd: get host %s\n", host->h_name);
		host->h_count ++;
		host->h_expires = jiffies + NLM_HOST_EXPIRE;
	}
	return host;
}

/*
 * Release NLM host after use
 */
void nlm_release_host(struct nlm_host *host)
{
	if (host && host->h_count) {
		dprintk("lockd: release host %s\n", host->h_name);
		host->h_count --;
	}
}

/*
 * Shut down the hosts module.
 * Note that this routine is called only at server shutdown time.
 */
void
nlm_shutdown_hosts(void)
{
	struct nlm_host	*host;
	int		i;

	dprintk("lockd: shutting down host module\n");
	down(&nlm_host_sema);

	/* First, make all hosts eligible for gc */
	dprintk("lockd: nuking all hosts...\n");
	for (i = 0; i < NLM_HOST_NRHASH; i++) {
		for (host = nlm_hosts[i]; host; host = host->h_next)
			host->h_expires = 0;
	}

	/* Then, perform a garbage collection pass */
	nlm_gc_hosts();
	up(&nlm_host_sema);

	/* complain if any hosts are left */
	if (nrhosts) {
		printk(KERN_WARNING "lockd: couldn't shutdown host module!\n");
		dprintk("lockd: %d hosts left:\n", nrhosts);
		for (i = 0; i < NLM_HOST_NRHASH; i++) {
			for (host = nlm_hosts[i]; host; host = host->h_next) {
				dprintk("       %s (cnt %d use %d exp %ld)\n",
					host->h_name, host->h_count,
					host->h_inuse, host->h_expires);
			}
		}
	}
}

/*
 * Garbage collect any unused NLM hosts.
 * This GC combines reference counting for async operations with
 * mark & sweep for resources held by remote clients.
 */
static void
nlm_gc_hosts(void)
{
	struct nlm_host	**q, *host;
	struct rpc_clnt	*clnt;
	int		i;

	dprintk("lockd: host garbage collection\n");
	for (i = 0; i < NLM_HOST_NRHASH; i++) {
		for (host = nlm_hosts[i]; host; host = host->h_next)
			host->h_inuse = 0;
	}

	/* Mark all hosts that hold locks, blocks or shares */
	nlmsvc_mark_resources();

	for (i = 0; i < NLM_HOST_NRHASH; i++) {
		q = &nlm_hosts[i];
		while ((host = *q) != NULL) {
			if (host->h_count || host->h_inuse
			 || time_before(jiffies, host->h_expires)) {
				q = &host->h_next;
				continue;
			}
			dprintk("lockd: delete host %s\n", host->h_name);
			*q = host->h_next;
			if (host->h_monitored)
				nsm_unmonitor(host);
			if ((clnt = host->h_rpcclnt) != NULL) {
				if (atomic_read(&clnt->cl_users)) {
					printk(KERN_WARNING
						"lockd: active RPC handle\n");
					clnt->cl_dead = 1;
				} else {
					rpc_destroy_client(host->h_rpcclnt);
				}
			}
			kfree(host);
			nrhosts--;
		}
	}

	next_gc = jiffies + NLM_HOST_COLLECT;
}

