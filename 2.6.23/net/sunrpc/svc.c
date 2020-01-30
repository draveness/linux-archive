/*
 * linux/net/sunrpc/svc.c
 *
 * High-level RPC service routines
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 *
 * Multiple threads pools and NUMAisation
 * Copyright (c) 2006 Silicon Graphics, Inc.
 * by Greg Banks <gnb@melbourne.sgi.com>
 */

#include <linux/linkage.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/module.h>

#include <linux/sunrpc/types.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/stats.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/clnt.h>

#define RPCDBG_FACILITY	RPCDBG_SVCDSP

#define svc_serv_is_pooled(serv)    ((serv)->sv_function)

/*
 * Mode for mapping cpus to pools.
 */
enum {
	SVC_POOL_AUTO = -1,	/* choose one of the others */
	SVC_POOL_GLOBAL,	/* no mapping, just a single global pool
				 * (legacy & UP mode) */
	SVC_POOL_PERCPU,	/* one pool per cpu */
	SVC_POOL_PERNODE	/* one pool per numa node */
};
#define SVC_POOL_DEFAULT	SVC_POOL_GLOBAL

/*
 * Structure for mapping cpus to pools and vice versa.
 * Setup once during sunrpc initialisation.
 */
static struct svc_pool_map {
	int count;			/* How many svc_servs use us */
	int mode;			/* Note: int not enum to avoid
					 * warnings about "enumeration value
					 * not handled in switch" */
	unsigned int npools;
	unsigned int *pool_to;		/* maps pool id to cpu or node */
	unsigned int *to_pool;		/* maps cpu or node to pool id */
} svc_pool_map = {
	.count = 0,
	.mode = SVC_POOL_DEFAULT
};
static DEFINE_MUTEX(svc_pool_map_mutex);/* protects svc_pool_map.count only */

static int
param_set_pool_mode(const char *val, struct kernel_param *kp)
{
	int *ip = (int *)kp->arg;
	struct svc_pool_map *m = &svc_pool_map;
	int err;

	mutex_lock(&svc_pool_map_mutex);

	err = -EBUSY;
	if (m->count)
		goto out;

	err = 0;
	if (!strncmp(val, "auto", 4))
		*ip = SVC_POOL_AUTO;
	else if (!strncmp(val, "global", 6))
		*ip = SVC_POOL_GLOBAL;
	else if (!strncmp(val, "percpu", 6))
		*ip = SVC_POOL_PERCPU;
	else if (!strncmp(val, "pernode", 7))
		*ip = SVC_POOL_PERNODE;
	else
		err = -EINVAL;

out:
	mutex_unlock(&svc_pool_map_mutex);
	return err;
}

static int
param_get_pool_mode(char *buf, struct kernel_param *kp)
{
	int *ip = (int *)kp->arg;

	switch (*ip)
	{
	case SVC_POOL_AUTO:
		return strlcpy(buf, "auto", 20);
	case SVC_POOL_GLOBAL:
		return strlcpy(buf, "global", 20);
	case SVC_POOL_PERCPU:
		return strlcpy(buf, "percpu", 20);
	case SVC_POOL_PERNODE:
		return strlcpy(buf, "pernode", 20);
	default:
		return sprintf(buf, "%d", *ip);
	}
}

module_param_call(pool_mode, param_set_pool_mode, param_get_pool_mode,
		 &svc_pool_map.mode, 0644);

/*
 * Detect best pool mapping mode heuristically,
 * according to the machine's topology.
 */
static int
svc_pool_map_choose_mode(void)
{
	unsigned int node;

	if (num_online_nodes() > 1) {
		/*
		 * Actually have multiple NUMA nodes,
		 * so split pools on NUMA node boundaries
		 */
		return SVC_POOL_PERNODE;
	}

	node = any_online_node(node_online_map);
	if (nr_cpus_node(node) > 2) {
		/*
		 * Non-trivial SMP, or CONFIG_NUMA on
		 * non-NUMA hardware, e.g. with a generic
		 * x86_64 kernel on Xeons.  In this case we
		 * want to divide the pools on cpu boundaries.
		 */
		return SVC_POOL_PERCPU;
	}

	/* default: one global pool */
	return SVC_POOL_GLOBAL;
}

/*
 * Allocate the to_pool[] and pool_to[] arrays.
 * Returns 0 on success or an errno.
 */
static int
svc_pool_map_alloc_arrays(struct svc_pool_map *m, unsigned int maxpools)
{
	m->to_pool = kcalloc(maxpools, sizeof(unsigned int), GFP_KERNEL);
	if (!m->to_pool)
		goto fail;
	m->pool_to = kcalloc(maxpools, sizeof(unsigned int), GFP_KERNEL);
	if (!m->pool_to)
		goto fail_free;

	return 0;

fail_free:
	kfree(m->to_pool);
fail:
	return -ENOMEM;
}

/*
 * Initialise the pool map for SVC_POOL_PERCPU mode.
 * Returns number of pools or <0 on error.
 */
static int
svc_pool_map_init_percpu(struct svc_pool_map *m)
{
	unsigned int maxpools = nr_cpu_ids;
	unsigned int pidx = 0;
	unsigned int cpu;
	int err;

	err = svc_pool_map_alloc_arrays(m, maxpools);
	if (err)
		return err;

	for_each_online_cpu(cpu) {
		BUG_ON(pidx > maxpools);
		m->to_pool[cpu] = pidx;
		m->pool_to[pidx] = cpu;
		pidx++;
	}
	/* cpus brought online later all get mapped to pool0, sorry */

	return pidx;
};


/*
 * Initialise the pool map for SVC_POOL_PERNODE mode.
 * Returns number of pools or <0 on error.
 */
static int
svc_pool_map_init_pernode(struct svc_pool_map *m)
{
	unsigned int maxpools = nr_node_ids;
	unsigned int pidx = 0;
	unsigned int node;
	int err;

	err = svc_pool_map_alloc_arrays(m, maxpools);
	if (err)
		return err;

	for_each_node_with_cpus(node) {
		/* some architectures (e.g. SN2) have cpuless nodes */
		BUG_ON(pidx > maxpools);
		m->to_pool[node] = pidx;
		m->pool_to[pidx] = node;
		pidx++;
	}
	/* nodes brought online later all get mapped to pool0, sorry */

	return pidx;
}


/*
 * Add a reference to the global map of cpus to pools (and
 * vice versa).  Initialise the map if we're the first user.
 * Returns the number of pools.
 */
static unsigned int
svc_pool_map_get(void)
{
	struct svc_pool_map *m = &svc_pool_map;
	int npools = -1;

	mutex_lock(&svc_pool_map_mutex);

	if (m->count++) {
		mutex_unlock(&svc_pool_map_mutex);
		return m->npools;
	}

	if (m->mode == SVC_POOL_AUTO)
		m->mode = svc_pool_map_choose_mode();

	switch (m->mode) {
	case SVC_POOL_PERCPU:
		npools = svc_pool_map_init_percpu(m);
		break;
	case SVC_POOL_PERNODE:
		npools = svc_pool_map_init_pernode(m);
		break;
	}

	if (npools < 0) {
		/* default, or memory allocation failure */
		npools = 1;
		m->mode = SVC_POOL_GLOBAL;
	}
	m->npools = npools;

	mutex_unlock(&svc_pool_map_mutex);
	return m->npools;
}


/*
 * Drop a reference to the global map of cpus to pools.
 * When the last reference is dropped, the map data is
 * freed; this allows the sysadmin to change the pool
 * mode using the pool_mode module option without
 * rebooting or re-loading sunrpc.ko.
 */
static void
svc_pool_map_put(void)
{
	struct svc_pool_map *m = &svc_pool_map;

	mutex_lock(&svc_pool_map_mutex);

	if (!--m->count) {
		m->mode = SVC_POOL_DEFAULT;
		kfree(m->to_pool);
		kfree(m->pool_to);
		m->npools = 0;
	}

	mutex_unlock(&svc_pool_map_mutex);
}


/*
 * Set the current thread's cpus_allowed mask so that it
 * will only run on cpus in the given pool.
 *
 * Returns 1 and fills in oldmask iff a cpumask was applied.
 */
static inline int
svc_pool_map_set_cpumask(unsigned int pidx, cpumask_t *oldmask)
{
	struct svc_pool_map *m = &svc_pool_map;
	unsigned int node; /* or cpu */

	/*
	 * The caller checks for sv_nrpools > 1, which
	 * implies that we've been initialized.
	 */
	BUG_ON(m->count == 0);

	switch (m->mode)
	{
	default:
		return 0;
	case SVC_POOL_PERCPU:
		node = m->pool_to[pidx];
		*oldmask = current->cpus_allowed;
		set_cpus_allowed(current, cpumask_of_cpu(node));
		return 1;
	case SVC_POOL_PERNODE:
		node = m->pool_to[pidx];
		*oldmask = current->cpus_allowed;
		set_cpus_allowed(current, node_to_cpumask(node));
		return 1;
	}
}

/*
 * Use the mapping mode to choose a pool for a given CPU.
 * Used when enqueueing an incoming RPC.  Always returns
 * a non-NULL pool pointer.
 */
struct svc_pool *
svc_pool_for_cpu(struct svc_serv *serv, int cpu)
{
	struct svc_pool_map *m = &svc_pool_map;
	unsigned int pidx = 0;

	/*
	 * An uninitialised map happens in a pure client when
	 * lockd is brought up, so silently treat it the
	 * same as SVC_POOL_GLOBAL.
	 */
	if (svc_serv_is_pooled(serv)) {
		switch (m->mode) {
		case SVC_POOL_PERCPU:
			pidx = m->to_pool[cpu];
			break;
		case SVC_POOL_PERNODE:
			pidx = m->to_pool[cpu_to_node(cpu)];
			break;
		}
	}
	return &serv->sv_pools[pidx % serv->sv_nrpools];
}


/*
 * Create an RPC service
 */
static struct svc_serv *
__svc_create(struct svc_program *prog, unsigned int bufsize, int npools,
	   void (*shutdown)(struct svc_serv *serv))
{
	struct svc_serv	*serv;
	int vers;
	unsigned int xdrsize;
	unsigned int i;

	if (!(serv = kzalloc(sizeof(*serv), GFP_KERNEL)))
		return NULL;
	serv->sv_name      = prog->pg_name;
	serv->sv_program   = prog;
	serv->sv_nrthreads = 1;
	serv->sv_stats     = prog->pg_stats;
	if (bufsize > RPCSVC_MAXPAYLOAD)
		bufsize = RPCSVC_MAXPAYLOAD;
	serv->sv_max_payload = bufsize? bufsize : 4096;
	serv->sv_max_mesg  = roundup(serv->sv_max_payload + PAGE_SIZE, PAGE_SIZE);
	serv->sv_shutdown  = shutdown;
	xdrsize = 0;
	while (prog) {
		prog->pg_lovers = prog->pg_nvers-1;
		for (vers=0; vers<prog->pg_nvers ; vers++)
			if (prog->pg_vers[vers]) {
				prog->pg_hivers = vers;
				if (prog->pg_lovers > vers)
					prog->pg_lovers = vers;
				if (prog->pg_vers[vers]->vs_xdrsize > xdrsize)
					xdrsize = prog->pg_vers[vers]->vs_xdrsize;
			}
		prog = prog->pg_next;
	}
	serv->sv_xdrsize   = xdrsize;
	INIT_LIST_HEAD(&serv->sv_tempsocks);
	INIT_LIST_HEAD(&serv->sv_permsocks);
	init_timer(&serv->sv_temptimer);
	spin_lock_init(&serv->sv_lock);

	serv->sv_nrpools = npools;
	serv->sv_pools =
		kcalloc(serv->sv_nrpools, sizeof(struct svc_pool),
			GFP_KERNEL);
	if (!serv->sv_pools) {
		kfree(serv);
		return NULL;
	}

	for (i = 0; i < serv->sv_nrpools; i++) {
		struct svc_pool *pool = &serv->sv_pools[i];

		dprintk("svc: initialising pool %u for %s\n",
				i, serv->sv_name);

		pool->sp_id = i;
		INIT_LIST_HEAD(&pool->sp_threads);
		INIT_LIST_HEAD(&pool->sp_sockets);
		INIT_LIST_HEAD(&pool->sp_all_threads);
		spin_lock_init(&pool->sp_lock);
	}


	/* Remove any stale portmap registrations */
	svc_register(serv, 0, 0);

	return serv;
}

struct svc_serv *
svc_create(struct svc_program *prog, unsigned int bufsize,
		void (*shutdown)(struct svc_serv *serv))
{
	return __svc_create(prog, bufsize, /*npools*/1, shutdown);
}

struct svc_serv *
svc_create_pooled(struct svc_program *prog, unsigned int bufsize,
		void (*shutdown)(struct svc_serv *serv),
		  svc_thread_fn func, int sig, struct module *mod)
{
	struct svc_serv *serv;
	unsigned int npools = svc_pool_map_get();

	serv = __svc_create(prog, bufsize, npools, shutdown);

	if (serv != NULL) {
		serv->sv_function = func;
		serv->sv_kill_signal = sig;
		serv->sv_module = mod;
	}

	return serv;
}

/*
 * Destroy an RPC service.  Should be called with the BKL held
 */
void
svc_destroy(struct svc_serv *serv)
{
	struct svc_sock	*svsk;
	struct svc_sock *tmp;

	dprintk("svc: svc_destroy(%s, %d)\n",
				serv->sv_program->pg_name,
				serv->sv_nrthreads);

	if (serv->sv_nrthreads) {
		if (--(serv->sv_nrthreads) != 0) {
			svc_sock_update_bufs(serv);
			return;
		}
	} else
		printk("svc_destroy: no threads for serv=%p!\n", serv);

	del_timer_sync(&serv->sv_temptimer);

	list_for_each_entry_safe(svsk, tmp, &serv->sv_tempsocks, sk_list)
		svc_force_close_socket(svsk);

	if (serv->sv_shutdown)
		serv->sv_shutdown(serv);

	list_for_each_entry_safe(svsk, tmp, &serv->sv_permsocks, sk_list)
		svc_force_close_socket(svsk);

	BUG_ON(!list_empty(&serv->sv_permsocks));
	BUG_ON(!list_empty(&serv->sv_tempsocks));

	cache_clean_deferred(serv);

	if (svc_serv_is_pooled(serv))
		svc_pool_map_put();

	/* Unregister service with the portmapper */
	svc_register(serv, 0, 0);
	kfree(serv->sv_pools);
	kfree(serv);
}

/*
 * Allocate an RPC server's buffer space.
 * We allocate pages and place them in rq_argpages.
 */
static int
svc_init_buffer(struct svc_rqst *rqstp, unsigned int size)
{
	int pages;
	int arghi;

	pages = size / PAGE_SIZE + 1; /* extra page as we hold both request and reply.
				       * We assume one is at most one page
				       */
	arghi = 0;
	BUG_ON(pages > RPCSVC_MAXPAGES);
	while (pages) {
		struct page *p = alloc_page(GFP_KERNEL);
		if (!p)
			break;
		rqstp->rq_pages[arghi++] = p;
		pages--;
	}
	return ! pages;
}

/*
 * Release an RPC server buffer
 */
static void
svc_release_buffer(struct svc_rqst *rqstp)
{
	int i;
	for (i=0; i<ARRAY_SIZE(rqstp->rq_pages); i++)
		if (rqstp->rq_pages[i])
			put_page(rqstp->rq_pages[i]);
}

/*
 * Create a thread in the given pool.  Caller must hold BKL.
 * On a NUMA or SMP machine, with a multi-pool serv, the thread
 * will be restricted to run on the cpus belonging to the pool.
 */
static int
__svc_create_thread(svc_thread_fn func, struct svc_serv *serv,
		    struct svc_pool *pool)
{
	struct svc_rqst	*rqstp;
	int		error = -ENOMEM;
	int		have_oldmask = 0;
	cpumask_t	oldmask;

	rqstp = kzalloc(sizeof(*rqstp), GFP_KERNEL);
	if (!rqstp)
		goto out;

	init_waitqueue_head(&rqstp->rq_wait);

	if (!(rqstp->rq_argp = kmalloc(serv->sv_xdrsize, GFP_KERNEL))
	 || !(rqstp->rq_resp = kmalloc(serv->sv_xdrsize, GFP_KERNEL))
	 || !svc_init_buffer(rqstp, serv->sv_max_mesg))
		goto out_thread;

	serv->sv_nrthreads++;
	spin_lock_bh(&pool->sp_lock);
	pool->sp_nrthreads++;
	list_add(&rqstp->rq_all, &pool->sp_all_threads);
	spin_unlock_bh(&pool->sp_lock);
	rqstp->rq_server = serv;
	rqstp->rq_pool = pool;

	if (serv->sv_nrpools > 1)
		have_oldmask = svc_pool_map_set_cpumask(pool->sp_id, &oldmask);

	error = kernel_thread((int (*)(void *)) func, rqstp, 0);

	if (have_oldmask)
		set_cpus_allowed(current, oldmask);

	if (error < 0)
		goto out_thread;
	svc_sock_update_bufs(serv);
	error = 0;
out:
	return error;

out_thread:
	svc_exit_thread(rqstp);
	goto out;
}

/*
 * Create a thread in the default pool.  Caller must hold BKL.
 */
int
svc_create_thread(svc_thread_fn func, struct svc_serv *serv)
{
	return __svc_create_thread(func, serv, &serv->sv_pools[0]);
}

/*
 * Choose a pool in which to create a new thread, for svc_set_num_threads
 */
static inline struct svc_pool *
choose_pool(struct svc_serv *serv, struct svc_pool *pool, unsigned int *state)
{
	if (pool != NULL)
		return pool;

	return &serv->sv_pools[(*state)++ % serv->sv_nrpools];
}

/*
 * Choose a thread to kill, for svc_set_num_threads
 */
static inline struct task_struct *
choose_victim(struct svc_serv *serv, struct svc_pool *pool, unsigned int *state)
{
	unsigned int i;
	struct task_struct *task = NULL;

	if (pool != NULL) {
		spin_lock_bh(&pool->sp_lock);
	} else {
		/* choose a pool in round-robin fashion */
		for (i = 0; i < serv->sv_nrpools; i++) {
			pool = &serv->sv_pools[--(*state) % serv->sv_nrpools];
			spin_lock_bh(&pool->sp_lock);
			if (!list_empty(&pool->sp_all_threads))
				goto found_pool;
			spin_unlock_bh(&pool->sp_lock);
		}
		return NULL;
	}

found_pool:
	if (!list_empty(&pool->sp_all_threads)) {
		struct svc_rqst *rqstp;

		/*
		 * Remove from the pool->sp_all_threads list
		 * so we don't try to kill it again.
		 */
		rqstp = list_entry(pool->sp_all_threads.next, struct svc_rqst, rq_all);
		list_del_init(&rqstp->rq_all);
		task = rqstp->rq_task;
	}
	spin_unlock_bh(&pool->sp_lock);

	return task;
}

/*
 * Create or destroy enough new threads to make the number
 * of threads the given number.  If `pool' is non-NULL, applies
 * only to threads in that pool, otherwise round-robins between
 * all pools.  Must be called with a svc_get() reference and
 * the BKL held.
 *
 * Destroying threads relies on the service threads filling in
 * rqstp->rq_task, which only the nfs ones do.  Assumes the serv
 * has been created using svc_create_pooled().
 *
 * Based on code that used to be in nfsd_svc() but tweaked
 * to be pool-aware.
 */
int
svc_set_num_threads(struct svc_serv *serv, struct svc_pool *pool, int nrservs)
{
	struct task_struct *victim;
	int error = 0;
	unsigned int state = serv->sv_nrthreads-1;

	if (pool == NULL) {
		/* The -1 assumes caller has done a svc_get() */
		nrservs -= (serv->sv_nrthreads-1);
	} else {
		spin_lock_bh(&pool->sp_lock);
		nrservs -= pool->sp_nrthreads;
		spin_unlock_bh(&pool->sp_lock);
	}

	/* create new threads */
	while (nrservs > 0) {
		nrservs--;
		__module_get(serv->sv_module);
		error = __svc_create_thread(serv->sv_function, serv,
					    choose_pool(serv, pool, &state));
		if (error < 0) {
			module_put(serv->sv_module);
			break;
		}
	}
	/* destroy old threads */
	while (nrservs < 0 &&
	       (victim = choose_victim(serv, pool, &state)) != NULL) {
		send_sig(serv->sv_kill_signal, victim, 1);
		nrservs++;
	}

	return error;
}

/*
 * Called from a server thread as it's exiting.  Caller must hold BKL.
 */
void
svc_exit_thread(struct svc_rqst *rqstp)
{
	struct svc_serv	*serv = rqstp->rq_server;
	struct svc_pool	*pool = rqstp->rq_pool;

	svc_release_buffer(rqstp);
	kfree(rqstp->rq_resp);
	kfree(rqstp->rq_argp);
	kfree(rqstp->rq_auth_data);

	spin_lock_bh(&pool->sp_lock);
	pool->sp_nrthreads--;
	list_del(&rqstp->rq_all);
	spin_unlock_bh(&pool->sp_lock);

	kfree(rqstp);

	/* Release the server */
	if (serv)
		svc_destroy(serv);
}

/*
 * Register an RPC service with the local portmapper.
 * To unregister a service, call this routine with
 * proto and port == 0.
 */
int
svc_register(struct svc_serv *serv, int proto, unsigned short port)
{
	struct svc_program	*progp;
	unsigned long		flags;
	int			i, error = 0, dummy;

	if (!port)
		clear_thread_flag(TIF_SIGPENDING);

	for (progp = serv->sv_program; progp; progp = progp->pg_next) {
		for (i = 0; i < progp->pg_nvers; i++) {
			if (progp->pg_vers[i] == NULL)
				continue;

			dprintk("svc: svc_register(%s, %s, %d, %d)%s\n",
					progp->pg_name,
					proto == IPPROTO_UDP?  "udp" : "tcp",
					port,
					i,
					progp->pg_vers[i]->vs_hidden?
						" (but not telling portmap)" : "");

			if (progp->pg_vers[i]->vs_hidden)
				continue;

			error = rpcb_register(progp->pg_prog, i, proto, port, &dummy);
			if (error < 0)
				break;
			if (port && !dummy) {
				error = -EACCES;
				break;
			}
		}
	}

	if (!port) {
		spin_lock_irqsave(&current->sighand->siglock, flags);
		recalc_sigpending();
		spin_unlock_irqrestore(&current->sighand->siglock, flags);
	}

	return error;
}

/*
 * Process the RPC request.
 */
int
svc_process(struct svc_rqst *rqstp)
{
	struct svc_program	*progp;
	struct svc_version	*versp = NULL;	/* compiler food */
	struct svc_procedure	*procp = NULL;
	struct kvec *		argv = &rqstp->rq_arg.head[0];
	struct kvec *		resv = &rqstp->rq_res.head[0];
	struct svc_serv		*serv = rqstp->rq_server;
	kxdrproc_t		xdr;
	__be32			*statp;
	u32			dir, prog, vers, proc;
	__be32			auth_stat, rpc_stat;
	int			auth_res;
	__be32			*reply_statp;

	rpc_stat = rpc_success;

	if (argv->iov_len < 6*4)
		goto err_short_len;

	/* setup response xdr_buf.
	 * Initially it has just one page
	 */
	rqstp->rq_resused = 1;
	resv->iov_base = page_address(rqstp->rq_respages[0]);
	resv->iov_len = 0;
	rqstp->rq_res.pages = rqstp->rq_respages + 1;
	rqstp->rq_res.len = 0;
	rqstp->rq_res.page_base = 0;
	rqstp->rq_res.page_len = 0;
	rqstp->rq_res.buflen = PAGE_SIZE;
	rqstp->rq_res.tail[0].iov_base = NULL;
	rqstp->rq_res.tail[0].iov_len = 0;
	/* Will be turned off only in gss privacy case: */
	rqstp->rq_splice_ok = 1;
	/* tcp needs a space for the record length... */
	if (rqstp->rq_prot == IPPROTO_TCP)
		svc_putnl(resv, 0);

	rqstp->rq_xid = svc_getu32(argv);
	svc_putu32(resv, rqstp->rq_xid);

	dir  = svc_getnl(argv);
	vers = svc_getnl(argv);

	/* First words of reply: */
	svc_putnl(resv, 1);		/* REPLY */

	if (dir != 0)		/* direction != CALL */
		goto err_bad_dir;
	if (vers != 2)		/* RPC version number */
		goto err_bad_rpc;

	/* Save position in case we later decide to reject: */
	reply_statp = resv->iov_base + resv->iov_len;

	svc_putnl(resv, 0);		/* ACCEPT */

	rqstp->rq_prog = prog = svc_getnl(argv);	/* program number */
	rqstp->rq_vers = vers = svc_getnl(argv);	/* version number */
	rqstp->rq_proc = proc = svc_getnl(argv);	/* procedure number */

	progp = serv->sv_program;

	for (progp = serv->sv_program; progp; progp = progp->pg_next)
		if (prog == progp->pg_prog)
			break;

	/*
	 * Decode auth data, and add verifier to reply buffer.
	 * We do this before anything else in order to get a decent
	 * auth verifier.
	 */
	auth_res = svc_authenticate(rqstp, &auth_stat);
	/* Also give the program a chance to reject this call: */
	if (auth_res == SVC_OK && progp) {
		auth_stat = rpc_autherr_badcred;
		auth_res = progp->pg_authenticate(rqstp);
	}
	switch (auth_res) {
	case SVC_OK:
		break;
	case SVC_GARBAGE:
		rpc_stat = rpc_garbage_args;
		goto err_bad;
	case SVC_SYSERR:
		rpc_stat = rpc_system_err;
		goto err_bad;
	case SVC_DENIED:
		goto err_bad_auth;
	case SVC_DROP:
		goto dropit;
	case SVC_COMPLETE:
		goto sendit;
	}

	if (progp == NULL)
		goto err_bad_prog;

	if (vers >= progp->pg_nvers ||
	  !(versp = progp->pg_vers[vers]))
		goto err_bad_vers;

	procp = versp->vs_proc + proc;
	if (proc >= versp->vs_nproc || !procp->pc_func)
		goto err_bad_proc;
	rqstp->rq_server   = serv;
	rqstp->rq_procinfo = procp;

	/* Syntactic check complete */
	serv->sv_stats->rpccnt++;

	/* Build the reply header. */
	statp = resv->iov_base +resv->iov_len;
	svc_putnl(resv, RPC_SUCCESS);

	/* Bump per-procedure stats counter */
	procp->pc_count++;

	/* Initialize storage for argp and resp */
	memset(rqstp->rq_argp, 0, procp->pc_argsize);
	memset(rqstp->rq_resp, 0, procp->pc_ressize);

	/* un-reserve some of the out-queue now that we have a
	 * better idea of reply size
	 */
	if (procp->pc_xdrressize)
		svc_reserve_auth(rqstp, procp->pc_xdrressize<<2);

	/* Call the function that processes the request. */
	if (!versp->vs_dispatch) {
		/* Decode arguments */
		xdr = procp->pc_decode;
		if (xdr && !xdr(rqstp, argv->iov_base, rqstp->rq_argp))
			goto err_garbage;

		*statp = procp->pc_func(rqstp, rqstp->rq_argp, rqstp->rq_resp);

		/* Encode reply */
		if (*statp == rpc_drop_reply) {
			if (procp->pc_release)
				procp->pc_release(rqstp, NULL, rqstp->rq_resp);
			goto dropit;
		}
		if (*statp == rpc_success && (xdr = procp->pc_encode)
		 && !xdr(rqstp, resv->iov_base+resv->iov_len, rqstp->rq_resp)) {
			dprintk("svc: failed to encode reply\n");
			/* serv->sv_stats->rpcsystemerr++; */
			*statp = rpc_system_err;
		}
	} else {
		dprintk("svc: calling dispatcher\n");
		if (!versp->vs_dispatch(rqstp, statp)) {
			/* Release reply info */
			if (procp->pc_release)
				procp->pc_release(rqstp, NULL, rqstp->rq_resp);
			goto dropit;
		}
	}

	/* Check RPC status result */
	if (*statp != rpc_success)
		resv->iov_len = ((void*)statp)  - resv->iov_base + 4;

	/* Release reply info */
	if (procp->pc_release)
		procp->pc_release(rqstp, NULL, rqstp->rq_resp);

	if (procp->pc_encode == NULL)
		goto dropit;

 sendit:
	if (svc_authorise(rqstp))
		goto dropit;
	return svc_send(rqstp);

 dropit:
	svc_authorise(rqstp);	/* doesn't hurt to call this twice */
	dprintk("svc: svc_process dropit\n");
	svc_drop(rqstp);
	return 0;

err_short_len:
	if (net_ratelimit())
		printk("svc: short len %Zd, dropping request\n", argv->iov_len);

	goto dropit;			/* drop request */

err_bad_dir:
	if (net_ratelimit())
		printk("svc: bad direction %d, dropping request\n", dir);

	serv->sv_stats->rpcbadfmt++;
	goto dropit;			/* drop request */

err_bad_rpc:
	serv->sv_stats->rpcbadfmt++;
	svc_putnl(resv, 1);	/* REJECT */
	svc_putnl(resv, 0);	/* RPC_MISMATCH */
	svc_putnl(resv, 2);	/* Only RPCv2 supported */
	svc_putnl(resv, 2);
	goto sendit;

err_bad_auth:
	dprintk("svc: authentication failed (%d)\n", ntohl(auth_stat));
	serv->sv_stats->rpcbadauth++;
	/* Restore write pointer to location of accept status: */
	xdr_ressize_check(rqstp, reply_statp);
	svc_putnl(resv, 1);	/* REJECT */
	svc_putnl(resv, 1);	/* AUTH_ERROR */
	svc_putnl(resv, ntohl(auth_stat));	/* status */
	goto sendit;

err_bad_prog:
	dprintk("svc: unknown program %d\n", prog);
	serv->sv_stats->rpcbadfmt++;
	svc_putnl(resv, RPC_PROG_UNAVAIL);
	goto sendit;

err_bad_vers:
	if (net_ratelimit())
		printk("svc: unknown version (%d for prog %d, %s)\n",
		       vers, prog, progp->pg_name);

	serv->sv_stats->rpcbadfmt++;
	svc_putnl(resv, RPC_PROG_MISMATCH);
	svc_putnl(resv, progp->pg_lovers);
	svc_putnl(resv, progp->pg_hivers);
	goto sendit;

err_bad_proc:
	if (net_ratelimit())
		printk("svc: unknown procedure (%d)\n", proc);

	serv->sv_stats->rpcbadfmt++;
	svc_putnl(resv, RPC_PROC_UNAVAIL);
	goto sendit;

err_garbage:
	if (net_ratelimit())
		printk("svc: failed to decode args\n");

	rpc_stat = rpc_garbage_args;
err_bad:
	serv->sv_stats->rpcbadfmt++;
	svc_putnl(resv, ntohl(rpc_stat));
	goto sendit;
}

/*
 * Return (transport-specific) limit on the rpc payload.
 */
u32 svc_max_payload(const struct svc_rqst *rqstp)
{
	int max = RPCSVC_MAXPAYLOAD_TCP;

	if (rqstp->rq_sock->sk_sock->type == SOCK_DGRAM)
		max = RPCSVC_MAXPAYLOAD_UDP;
	if (rqstp->rq_server->sv_max_payload < max)
		max = rqstp->rq_server->sv_max_payload;
	return max;
}
EXPORT_SYMBOL_GPL(svc_max_payload);
