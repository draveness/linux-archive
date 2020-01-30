/*
 * linux/fs/nfs/rpcauth.c
 *
 * Generic RPC authentication API.
 *
 * Copyright (C) 1996, Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/malloc.h>
#include <linux/errno.h>
#include <linux/socket.h>
#include <linux/sunrpc/clnt.h>
#include <linux/spinlock.h>

#ifdef RPC_DEBUG
# define RPCDBG_FACILITY	RPCDBG_AUTH
#endif

#define RPC_MAXFLAVOR	8

static struct rpc_authops *	auth_flavors[RPC_MAXFLAVOR] = {
	&authnull_ops,		/* AUTH_NULL */
	&authunix_ops,		/* AUTH_UNIX */
	NULL,			/* others can be loadable modules */
};

int
rpcauth_register(struct rpc_authops *ops)
{
	unsigned int	flavor;

	if ((flavor = ops->au_flavor) >= RPC_MAXFLAVOR)
		return -EINVAL;
	if (auth_flavors[flavor] != NULL)
		return -EPERM;		/* what else? */
	auth_flavors[flavor] = ops;
	return 0;
}

int
rpcauth_unregister(struct rpc_authops *ops)
{
	unsigned int	flavor;

	if ((flavor = ops->au_flavor) >= RPC_MAXFLAVOR)
		return -EINVAL;
	if (auth_flavors[flavor] != ops)
		return -EPERM;		/* what else? */
	auth_flavors[flavor] = NULL;
	return 0;
}

struct rpc_auth *
rpcauth_create(unsigned int flavor, struct rpc_clnt *clnt)
{
	struct rpc_authops	*ops;

	if (flavor >= RPC_MAXFLAVOR || !(ops = auth_flavors[flavor]))
		return NULL;
	clnt->cl_auth = ops->create(clnt);
	return clnt->cl_auth;
}

void
rpcauth_destroy(struct rpc_auth *auth)
{
	auth->au_ops->destroy(auth);
}

spinlock_t rpc_credcache_lock = SPIN_LOCK_UNLOCKED;

/*
 * Initialize RPC credential cache
 */
void
rpcauth_init_credcache(struct rpc_auth *auth)
{
	memset(auth->au_credcache, 0, sizeof(auth->au_credcache));
	auth->au_nextgc = jiffies + (auth->au_expire >> 1);
}

static inline void
rpcauth_crdestroy(struct rpc_auth *auth, struct rpc_cred *cred)
{
#ifdef RPC_DEBUG
	if (cred->cr_magic != RPCAUTH_CRED_MAGIC)
		BUG();
	cred->cr_magic = 0;
#endif
	if (auth->au_ops->crdestroy)
		auth->au_ops->crdestroy(cred);
	else
		rpc_free(cred);
}

/*
 * Clear the RPC credential cache
 */
void
rpcauth_free_credcache(struct rpc_auth *auth)
{
	struct rpc_cred	**q, *cred;
	void		(*destroy)(struct rpc_cred *);
	int		i;

	if (!(destroy = auth->au_ops->crdestroy))
		destroy = (void (*)(struct rpc_cred *)) rpc_free;

	spin_lock(&rpc_credcache_lock);
	for (i = 0; i < RPC_CREDCACHE_NR; i++) {
		q = &auth->au_credcache[i];
		while ((cred = *q) != NULL) {
			*q = cred->cr_next;
			destroy(cred);
		}
	}
	spin_unlock(&rpc_credcache_lock);
}

/*
 * Remove stale credentials. Avoid sleeping inside the loop.
 */
static void
rpcauth_gc_credcache(struct rpc_auth *auth)
{
	struct rpc_cred	**q, *cred, *free = NULL;
	int		i;

	dprintk("RPC: gc'ing RPC credentials for auth %p\n", auth);
	spin_lock(&rpc_credcache_lock);
	for (i = 0; i < RPC_CREDCACHE_NR; i++) {
		q = &auth->au_credcache[i];
		while ((cred = *q) != NULL) {
			if (!cred->cr_count &&
			    time_before(cred->cr_expire, jiffies)) {
				*q = cred->cr_next;
				cred->cr_next = free;
				free = cred;
				continue;
			}
			q = &cred->cr_next;
		}
	}
	spin_unlock(&rpc_credcache_lock);
	while ((cred = free) != NULL) {
		free = cred->cr_next;
		rpcauth_crdestroy(auth, cred);
	}
	auth->au_nextgc = jiffies + auth->au_expire;
}

/*
 * Insert credential into cache
 */
void
rpcauth_insert_credcache(struct rpc_auth *auth, struct rpc_cred *cred)
{
	int		nr;

	nr = (cred->cr_uid & RPC_CREDCACHE_MASK);
	spin_lock(&rpc_credcache_lock);
	cred->cr_next = auth->au_credcache[nr];
	auth->au_credcache[nr] = cred;
	cred->cr_count++;
	cred->cr_expire = jiffies + auth->au_expire;
	spin_unlock(&rpc_credcache_lock);
}

/*
 * Look up a process' credentials in the authentication cache
 */
static struct rpc_cred *
rpcauth_lookup_credcache(struct rpc_auth *auth, int taskflags)
{
	struct rpc_cred	**q, *cred = NULL;
	int		nr = 0;

	if (!(taskflags & RPC_TASK_ROOTCREDS))
		nr = current->uid & RPC_CREDCACHE_MASK;

	if (time_before(auth->au_nextgc, jiffies))
		rpcauth_gc_credcache(auth);

	spin_lock(&rpc_credcache_lock);
	q = &auth->au_credcache[nr];
	while ((cred = *q) != NULL) {
		if (!(cred->cr_flags & RPCAUTH_CRED_DEAD) &&
		    auth->au_ops->crmatch(cred, taskflags)) {
			*q = cred->cr_next;
			break;
		}
		q = &cred->cr_next;
	}
	spin_unlock(&rpc_credcache_lock);

	if (!cred) {
		cred = auth->au_ops->crcreate(taskflags);
#ifdef RPC_DEBUG
		if (cred)
			cred->cr_magic = RPCAUTH_CRED_MAGIC;
#endif
	}

	if (cred)
		rpcauth_insert_credcache(auth, cred);

	return (struct rpc_cred *) cred;
}

/*
 * Remove cred handle from cache
 */
static void
rpcauth_remove_credcache(struct rpc_auth *auth, struct rpc_cred *cred)
{
	struct rpc_cred	**q, *cr;
	int		nr;

	nr = (cred->cr_uid & RPC_CREDCACHE_MASK);
	spin_lock(&rpc_credcache_lock);
	q = &auth->au_credcache[nr];
	while ((cr = *q) != NULL) {
		if (cred == cr) {
			*q = cred->cr_next;
			cred->cr_next = NULL;
			break;
		}
		q = &cred->cr_next;
	}
	spin_unlock(&rpc_credcache_lock);
}

struct rpc_cred *
rpcauth_lookupcred(struct rpc_auth *auth, int taskflags)
{
	dprintk("RPC:     looking up %s cred\n",
		auth->au_ops->au_name);
	return rpcauth_lookup_credcache(auth, taskflags);
}

struct rpc_cred *
rpcauth_bindcred(struct rpc_task *task)
{
	struct rpc_auth *auth = task->tk_auth;

	dprintk("RPC: %4d looking up %s cred\n",
		task->tk_pid, task->tk_auth->au_ops->au_name);
	task->tk_msg.rpc_cred = rpcauth_lookup_credcache(auth, task->tk_flags);
	if (task->tk_msg.rpc_cred == 0)
		task->tk_status = -ENOMEM;
	return task->tk_msg.rpc_cred;
}

int
rpcauth_matchcred(struct rpc_auth *auth, struct rpc_cred *cred, int taskflags)
{
	dprintk("RPC:     matching %s cred %d\n",
		auth->au_ops->au_name, taskflags);
	return auth->au_ops->crmatch(cred, taskflags);
}

void
rpcauth_holdcred(struct rpc_task *task)
{
	dprintk("RPC: %4d holding %s cred %p\n",
		task->tk_pid, task->tk_auth->au_ops->au_name, task->tk_msg.rpc_cred);
	if (task->tk_msg.rpc_cred) {
		task->tk_msg.rpc_cred->cr_count++;
		task->tk_msg.rpc_cred->cr_expire = jiffies + task->tk_auth->au_expire;
	}
}

void
rpcauth_releasecred(struct rpc_auth *auth, struct rpc_cred *cred)
{
	if (cred != NULL && cred->cr_count > 0) {
		cred->cr_count--;
		if (cred->cr_flags & RPCAUTH_CRED_DEAD) {
			rpcauth_remove_credcache(auth, cred);
			if (!cred->cr_count)
				rpcauth_crdestroy(auth, cred);
		}
	}
}

void
rpcauth_unbindcred(struct rpc_task *task)
{
	struct rpc_auth	*auth = task->tk_auth;
	struct rpc_cred	*cred = task->tk_msg.rpc_cred;

	dprintk("RPC: %4d releasing %s cred %p\n",
		task->tk_pid, auth->au_ops->au_name, cred);

	rpcauth_releasecred(auth, cred);
	task->tk_msg.rpc_cred = NULL;
}

u32 *
rpcauth_marshcred(struct rpc_task *task, u32 *p)
{
	struct rpc_auth	*auth = task->tk_auth;

	dprintk("RPC: %4d marshaling %s cred %p\n",
		task->tk_pid, auth->au_ops->au_name, task->tk_msg.rpc_cred);
	return auth->au_ops->crmarshal(task, p,
				task->tk_flags & RPC_CALL_REALUID);
}

u32 *
rpcauth_checkverf(struct rpc_task *task, u32 *p)
{
	struct rpc_auth	*auth = task->tk_auth;

	dprintk("RPC: %4d validating %s cred %p\n",
		task->tk_pid, auth->au_ops->au_name, task->tk_msg.rpc_cred);
	return auth->au_ops->crvalidate(task, p);
}

int
rpcauth_refreshcred(struct rpc_task *task)
{
	struct rpc_auth	*auth = task->tk_auth;

	dprintk("RPC: %4d refreshing %s cred %p\n",
		task->tk_pid, auth->au_ops->au_name, task->tk_msg.rpc_cred);
	task->tk_status = auth->au_ops->crrefresh(task);
	return task->tk_status;
}

void
rpcauth_invalcred(struct rpc_task *task)
{
	dprintk("RPC: %4d invalidating %s cred %p\n",
		task->tk_pid, task->tk_auth->au_ops->au_name, task->tk_msg.rpc_cred);
	if (task->tk_msg.rpc_cred)
		task->tk_msg.rpc_cred->cr_flags &= ~RPCAUTH_CRED_UPTODATE;
}

int
rpcauth_uptodatecred(struct rpc_task *task)
{
	return !(task->tk_msg.rpc_cred) ||
		(task->tk_msg.rpc_cred->cr_flags & RPCAUTH_CRED_UPTODATE);
}
