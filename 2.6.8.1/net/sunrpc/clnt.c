/*
 *  linux/net/sunrpc/rpcclnt.c
 *
 *  This file contains the high-level RPC interface.
 *  It is modeled as a finite state machine to support both synchronous
 *  and asynchronous requests.
 *
 *  -	RPC header generation and argument serialization.
 *  -	Credential refresh.
 *  -	TCP connect handling.
 *  -	Retry of operation when it is suspected the operation failed because
 *	of uid squashing on the server, or when the credentials were stale
 *	and need to be refreshed, or when a packet was damaged in transit.
 *	This may be have to be moved to the VFS layer.
 *
 *  NB: BSD uses a more intelligent approach to guessing when a request
 *  or reply has been lost by keeping the RTO estimate for each procedure.
 *  We currently make do with a constant timeout value.
 *
 *  Copyright (C) 1992,1993 Rick Sladkey <jrs@world.std.com>
 *  Copyright (C) 1995,1996 Olaf Kirch <okir@monad.swb.de>
 */

#include <asm/system.h>

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/in.h>
#include <linux/utsname.h>

#include <linux/sunrpc/clnt.h>
#include <linux/workqueue.h>
#include <linux/sunrpc/rpc_pipe_fs.h>

#include <linux/nfs.h>


#define RPC_SLACK_SPACE		(1024)	/* total overkill */

#ifdef RPC_DEBUG
# define RPCDBG_FACILITY	RPCDBG_CALL
#endif

static DECLARE_WAIT_QUEUE_HEAD(destroy_wait);


static void	call_start(struct rpc_task *task);
static void	call_reserve(struct rpc_task *task);
static void	call_reserveresult(struct rpc_task *task);
static void	call_allocate(struct rpc_task *task);
static void	call_encode(struct rpc_task *task);
static void	call_decode(struct rpc_task *task);
static void	call_bind(struct rpc_task *task);
static void	call_transmit(struct rpc_task *task);
static void	call_status(struct rpc_task *task);
static void	call_refresh(struct rpc_task *task);
static void	call_refreshresult(struct rpc_task *task);
static void	call_timeout(struct rpc_task *task);
static void	call_connect(struct rpc_task *task);
static void	call_connect_status(struct rpc_task *task);
static u32 *	call_header(struct rpc_task *task);
static u32 *	call_verify(struct rpc_task *task);


static int
rpc_setup_pipedir(struct rpc_clnt *clnt, char *dir_name)
{
	static uint32_t clntid;
	int error;

	if (dir_name == NULL)
		return 0;
	for (;;) {
		snprintf(clnt->cl_pathname, sizeof(clnt->cl_pathname),
				"%s/clnt%x", dir_name,
				(unsigned int)clntid++);
		clnt->cl_pathname[sizeof(clnt->cl_pathname) - 1] = '\0';
		clnt->cl_dentry = rpc_mkdir(clnt->cl_pathname, clnt);
		if (!IS_ERR(clnt->cl_dentry))
			return 0;
		error = PTR_ERR(clnt->cl_dentry);
		if (error != -EEXIST) {
			printk(KERN_INFO "RPC: Couldn't create pipefs entry %s, error %d\n",
					clnt->cl_pathname, error);
			return error;
		}
	}
}

/*
 * Create an RPC client
 * FIXME: This should also take a flags argument (as in task->tk_flags).
 * It's called (among others) from pmap_create_client, which may in
 * turn be called by an async task. In this case, rpciod should not be
 * made to sleep too long.
 */
struct rpc_clnt *
rpc_create_client(struct rpc_xprt *xprt, char *servname,
		  struct rpc_program *program, u32 vers,
		  rpc_authflavor_t flavor)
{
	struct rpc_version	*version;
	struct rpc_clnt		*clnt = NULL;
	int err;
	int len;

	dprintk("RPC: creating %s client for %s (xprt %p)\n",
		program->name, servname, xprt);

	err = -EINVAL;
	if (!xprt)
		goto out_err;
	if (vers >= program->nrvers || !(version = program->version[vers]))
		goto out_err;

	err = -ENOMEM;
	clnt = (struct rpc_clnt *) kmalloc(sizeof(*clnt), GFP_KERNEL);
	if (!clnt)
		goto out_err;
	memset(clnt, 0, sizeof(*clnt));
	atomic_set(&clnt->cl_users, 0);
	atomic_set(&clnt->cl_count, 1);
	clnt->cl_parent = clnt;

	clnt->cl_server = clnt->cl_inline_name;
	len = strlen(servname) + 1;
	if (len > sizeof(clnt->cl_inline_name)) {
		char *buf = kmalloc(len, GFP_KERNEL);
		if (buf != 0)
			clnt->cl_server = buf;
		else
			len = sizeof(clnt->cl_inline_name);
	}
	strlcpy(clnt->cl_server, servname, len);

	clnt->cl_xprt     = xprt;
	clnt->cl_procinfo = version->procs;
	clnt->cl_maxproc  = version->nrprocs;
	clnt->cl_protname = program->name;
	clnt->cl_pmap	  = &clnt->cl_pmap_default;
	clnt->cl_port     = xprt->addr.sin_port;
	clnt->cl_prog     = program->number;
	clnt->cl_vers     = version->number;
	clnt->cl_prot     = xprt->prot;
	clnt->cl_stats    = program->stats;
	rpc_init_wait_queue(&clnt->cl_pmap_default.pm_bindwait, "bindwait");

	if (!clnt->cl_port)
		clnt->cl_autobind = 1;

	clnt->cl_rtt = &clnt->cl_rtt_default;
	rpc_init_rtt(&clnt->cl_rtt_default, xprt->timeout.to_initval);

	err = rpc_setup_pipedir(clnt, program->pipe_dir_name);
	if (err < 0)
		goto out_no_path;

	err = -ENOMEM;
	if (!rpcauth_create(flavor, clnt)) {
		printk(KERN_INFO "RPC: Couldn't create auth handle (flavor %u)\n",
				flavor);
		goto out_no_auth;
	}

	/* save the nodename */
	clnt->cl_nodelen = strlen(system_utsname.nodename);
	if (clnt->cl_nodelen > UNX_MAXNODENAME)
		clnt->cl_nodelen = UNX_MAXNODENAME;
	memcpy(clnt->cl_nodename, system_utsname.nodename, clnt->cl_nodelen);
	return clnt;

out_no_auth:
	rpc_rmdir(clnt->cl_pathname);
out_no_path:
	if (clnt->cl_server != clnt->cl_inline_name)
		kfree(clnt->cl_server);
	kfree(clnt);
out_err:
	return ERR_PTR(err);
}

/*
 * This function clones the RPC client structure. It allows us to share the
 * same transport while varying parameters such as the authentication
 * flavour.
 */
struct rpc_clnt *
rpc_clone_client(struct rpc_clnt *clnt)
{
	struct rpc_clnt *new;

	new = (struct rpc_clnt *)kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		goto out_no_clnt;
	memcpy(new, clnt, sizeof(*new));
	atomic_set(&new->cl_count, 1);
	atomic_set(&new->cl_users, 0);
	atomic_inc(&new->cl_parent->cl_count);
	if (new->cl_auth)
		atomic_inc(&new->cl_auth->au_count);
	return new;
out_no_clnt:
	printk(KERN_INFO "RPC: out of memory in %s\n", __FUNCTION__);
	return ERR_PTR(-ENOMEM);
}

/*
 * Properly shut down an RPC client, terminating all outstanding
 * requests. Note that we must be certain that cl_oneshot and
 * cl_dead are cleared, or else the client would be destroyed
 * when the last task releases it.
 */
int
rpc_shutdown_client(struct rpc_clnt *clnt)
{
	dprintk("RPC: shutting down %s client for %s, tasks=%d\n",
			clnt->cl_protname, clnt->cl_server,
			atomic_read(&clnt->cl_users));

	while (atomic_read(&clnt->cl_users) > 0) {
		/* Don't let rpc_release_client destroy us */
		clnt->cl_oneshot = 0;
		clnt->cl_dead = 0;
		rpc_killall_tasks(clnt);
		sleep_on_timeout(&destroy_wait, 1*HZ);
	}

	if (atomic_read(&clnt->cl_users) < 0) {
		printk(KERN_ERR "RPC: rpc_shutdown_client clnt %p tasks=%d\n",
				clnt, atomic_read(&clnt->cl_users));
#ifdef RPC_DEBUG
		rpc_show_tasks();
#endif
		BUG();
	}

	return rpc_destroy_client(clnt);
}

/*
 * Delete an RPC client
 */
int
rpc_destroy_client(struct rpc_clnt *clnt)
{
	if (!atomic_dec_and_test(&clnt->cl_count))
		return 1;
	BUG_ON(atomic_read(&clnt->cl_users) != 0);

	dprintk("RPC: destroying %s client for %s\n",
			clnt->cl_protname, clnt->cl_server);
	if (clnt->cl_auth) {
		rpcauth_destroy(clnt->cl_auth);
		clnt->cl_auth = NULL;
	}
	if (clnt->cl_parent != clnt) {
		rpc_destroy_client(clnt->cl_parent);
		goto out_free;
	}
	if (clnt->cl_pathname[0])
		rpc_rmdir(clnt->cl_pathname);
	if (clnt->cl_xprt) {
		xprt_destroy(clnt->cl_xprt);
		clnt->cl_xprt = NULL;
	}
	if (clnt->cl_server != clnt->cl_inline_name)
		kfree(clnt->cl_server);
out_free:
	kfree(clnt);
	return 0;
}

/*
 * Release an RPC client
 */
void
rpc_release_client(struct rpc_clnt *clnt)
{
	dprintk("RPC:      rpc_release_client(%p, %d)\n",
				clnt, atomic_read(&clnt->cl_users));

	if (!atomic_dec_and_test(&clnt->cl_users))
		return;
	wake_up(&destroy_wait);
	if (clnt->cl_oneshot || clnt->cl_dead)
		rpc_destroy_client(clnt);
}

/*
 * Default callback for async RPC calls
 */
static void
rpc_default_callback(struct rpc_task *task)
{
}

/*
 *	Export the signal mask handling for aysnchronous code that
 *	sleeps on RPC calls
 */
 
void rpc_clnt_sigmask(struct rpc_clnt *clnt, sigset_t *oldset)
{
	unsigned long	sigallow = sigmask(SIGKILL);
	unsigned long	irqflags;
	
	/* Turn off various signals */
	if (clnt->cl_intr) {
		struct k_sigaction *action = current->sighand->action;
		if (action[SIGINT-1].sa.sa_handler == SIG_DFL)
			sigallow |= sigmask(SIGINT);
		if (action[SIGQUIT-1].sa.sa_handler == SIG_DFL)
			sigallow |= sigmask(SIGQUIT);
	}
	spin_lock_irqsave(&current->sighand->siglock, irqflags);
	*oldset = current->blocked;
	siginitsetinv(&current->blocked, sigallow & ~oldset->sig[0]);
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, irqflags);
}

void rpc_clnt_sigunmask(struct rpc_clnt *clnt, sigset_t *oldset)
{
	unsigned long	irqflags;
	
	spin_lock_irqsave(&current->sighand->siglock, irqflags);
	current->blocked = *oldset;
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, irqflags);
}

/*
 * New rpc_call implementation
 */
int rpc_call_sync(struct rpc_clnt *clnt, struct rpc_message *msg, int flags)
{
	struct rpc_task	my_task, *task = &my_task;
	sigset_t	oldset;
	int		status;

	/* If this client is slain all further I/O fails */
	if (clnt->cl_dead) 
		return -EIO;

	if (flags & RPC_TASK_ASYNC) {
		printk("rpc_call_sync: Illegal flag combination for synchronous task\n");
		flags &= ~RPC_TASK_ASYNC;
	}

	rpc_clnt_sigmask(clnt, &oldset);		

	/* Create/initialize a new RPC task */
	rpc_init_task(task, clnt, NULL, flags);
	rpc_call_setup(task, msg, 0);

	/* Set up the call info struct and execute the task */
	if (task->tk_status == 0)
		status = rpc_execute(task);
	else {
		status = task->tk_status;
		rpc_release_task(task);
	}

	rpc_clnt_sigunmask(clnt, &oldset);		

	return status;
}

/*
 * New rpc_call implementation
 */
int
rpc_call_async(struct rpc_clnt *clnt, struct rpc_message *msg, int flags,
	       rpc_action callback, void *data)
{
	struct rpc_task	*task;
	sigset_t	oldset;
	int		status;

	/* If this client is slain all further I/O fails */
	if (clnt->cl_dead) 
		return -EIO;

	flags |= RPC_TASK_ASYNC;

	rpc_clnt_sigmask(clnt, &oldset);		

	/* Create/initialize a new RPC task */
	if (!callback)
		callback = rpc_default_callback;
	status = -ENOMEM;
	if (!(task = rpc_new_task(clnt, callback, flags)))
		goto out;
	task->tk_calldata = data;

	rpc_call_setup(task, msg, 0);

	/* Set up the call info struct and execute the task */
	if (task->tk_status == 0)
		status = rpc_execute(task);
	else {
		status = task->tk_status;
		rpc_release_task(task);
	}

out:
	rpc_clnt_sigunmask(clnt, &oldset);		

	return status;
}


void
rpc_call_setup(struct rpc_task *task, struct rpc_message *msg, int flags)
{
	task->tk_msg   = *msg;
	task->tk_flags |= flags;
	/* Bind the user cred */
	if (task->tk_msg.rpc_cred != NULL) {
		rpcauth_holdcred(task);
	} else
		rpcauth_bindcred(task);

	if (task->tk_status == 0)
		task->tk_action = call_start;
	else
		task->tk_action = NULL;
}

void
rpc_setbufsize(struct rpc_clnt *clnt, unsigned int sndsize, unsigned int rcvsize)
{
	struct rpc_xprt *xprt = clnt->cl_xprt;

	xprt->sndsize = 0;
	if (sndsize)
		xprt->sndsize = sndsize + RPC_SLACK_SPACE;
	xprt->rcvsize = 0;
	if (rcvsize)
		xprt->rcvsize = rcvsize + RPC_SLACK_SPACE;
	if (xprt_connected(xprt))
		xprt_sock_setbufsize(xprt);
}

/*
 * Restart an (async) RPC call. Usually called from within the
 * exit handler.
 */
void
rpc_restart_call(struct rpc_task *task)
{
	if (RPC_ASSASSINATED(task))
		return;

	task->tk_action = call_start;
}

/*
 * 0.  Initial state
 *
 *     Other FSM states can be visited zero or more times, but
 *     this state is visited exactly once for each RPC.
 */
static void
call_start(struct rpc_task *task)
{
	struct rpc_clnt	*clnt = task->tk_client;

	dprintk("RPC: %4d call_start %s%d proc %d (%s)\n", task->tk_pid,
		clnt->cl_protname, clnt->cl_vers, task->tk_msg.rpc_proc->p_proc,
		(RPC_IS_ASYNC(task) ? "async" : "sync"));

	/* Increment call count */
	task->tk_msg.rpc_proc->p_count++;
	clnt->cl_stats->rpccnt++;
	task->tk_action = call_reserve;
}

/*
 * 1.	Reserve an RPC call slot
 */
static void
call_reserve(struct rpc_task *task)
{
	dprintk("RPC: %4d call_reserve\n", task->tk_pid);

	if (!rpcauth_uptodatecred(task)) {
		task->tk_action = call_refresh;
		return;
	}

	task->tk_status  = 0;
	task->tk_action  = call_reserveresult;
	xprt_reserve(task);
}

/*
 * 1b.	Grok the result of xprt_reserve()
 */
static void
call_reserveresult(struct rpc_task *task)
{
	int status = task->tk_status;

	dprintk("RPC: %4d call_reserveresult (status %d)\n",
				task->tk_pid, task->tk_status);

	/*
	 * After a call to xprt_reserve(), we must have either
	 * a request slot or else an error status.
	 */
	task->tk_status = 0;
	if (status >= 0) {
		if (task->tk_rqstp) {
			task->tk_action = call_allocate;
			return;
		}

		printk(KERN_ERR "%s: status=%d, but no request slot, exiting\n",
				__FUNCTION__, status);
		rpc_exit(task, -EIO);
		return;
	}

	/*
	 * Even though there was an error, we may have acquired
	 * a request slot somehow.  Make sure not to leak it.
	 */
	if (task->tk_rqstp) {
		printk(KERN_ERR "%s: status=%d, request allocated anyway\n",
				__FUNCTION__, status);
		xprt_release(task);
	}

	switch (status) {
	case -EAGAIN:	/* woken up; retry */
		task->tk_action = call_reserve;
		return;
	case -EIO:	/* probably a shutdown */
		break;
	default:
		printk(KERN_ERR "%s: unrecognized error %d, exiting\n",
				__FUNCTION__, status);
		break;
	}
	rpc_exit(task, status);
}

/*
 * 2.	Allocate the buffer. For details, see sched.c:rpc_malloc.
 *	(Note: buffer memory is freed in rpc_task_release).
 */
static void
call_allocate(struct rpc_task *task)
{
	unsigned int	bufsiz;

	dprintk("RPC: %4d call_allocate (status %d)\n", 
				task->tk_pid, task->tk_status);
	task->tk_action = call_bind;
	if (task->tk_buffer)
		return;

	/* FIXME: compute buffer requirements more exactly using
	 * auth->au_wslack */
	bufsiz = task->tk_msg.rpc_proc->p_bufsiz + RPC_SLACK_SPACE;

	if (rpc_malloc(task, bufsiz << 1) != NULL)
		return;
	printk(KERN_INFO "RPC: buffer allocation failed for task %p\n", task); 

	if (RPC_IS_ASYNC(task) || !(task->tk_client->cl_intr && signalled())) {
		xprt_release(task);
		task->tk_action = call_reserve;
		rpc_delay(task, HZ>>4);
		return;
	}

	rpc_exit(task, -ERESTARTSYS);
}

/*
 * 3.	Encode arguments of an RPC call
 */
static void
call_encode(struct rpc_task *task)
{
	struct rpc_clnt	*clnt = task->tk_client;
	struct rpc_rqst	*req = task->tk_rqstp;
	struct xdr_buf *sndbuf = &req->rq_snd_buf;
	struct xdr_buf *rcvbuf = &req->rq_rcv_buf;
	unsigned int	bufsiz;
	kxdrproc_t	encode;
	int		status;
	u32		*p;

	dprintk("RPC: %4d call_encode (status %d)\n", 
				task->tk_pid, task->tk_status);

	/* Default buffer setup */
	bufsiz = task->tk_bufsize >> 1;
	sndbuf->head[0].iov_base = (void *)task->tk_buffer;
	sndbuf->head[0].iov_len  = bufsiz;
	sndbuf->tail[0].iov_len  = 0;
	sndbuf->page_len	 = 0;
	sndbuf->len		 = 0;
	sndbuf->buflen		 = bufsiz;
	rcvbuf->head[0].iov_base = (void *)((char *)task->tk_buffer + bufsiz);
	rcvbuf->head[0].iov_len  = bufsiz;
	rcvbuf->tail[0].iov_len  = 0;
	rcvbuf->page_len	 = 0;
	rcvbuf->len		 = 0;
	rcvbuf->buflen		 = bufsiz;

	/* Encode header and provided arguments */
	encode = task->tk_msg.rpc_proc->p_encode;
	if (!(p = call_header(task))) {
		printk(KERN_INFO "RPC: call_header failed, exit EIO\n");
		rpc_exit(task, -EIO);
		return;
	}
	if (encode && (status = rpcauth_wrap_req(task, encode, req, p,
						 task->tk_msg.rpc_argp)) < 0) {
		printk(KERN_WARNING "%s: can't encode arguments: %d\n",
				clnt->cl_protname, -status);
		rpc_exit(task, status);
	}
}

/*
 * 4.	Get the server port number if not yet set
 */
static void
call_bind(struct rpc_task *task)
{
	struct rpc_clnt	*clnt = task->tk_client;
	struct rpc_xprt *xprt = clnt->cl_xprt;

	dprintk("RPC: %4d call_bind xprt %p %s connected\n", task->tk_pid,
			xprt, (xprt_connected(xprt) ? "is" : "is not"));

	task->tk_action = (xprt_connected(xprt)) ? call_transmit : call_connect;

	if (!clnt->cl_port) {
		task->tk_action = call_connect;
		task->tk_timeout = RPC_CONNECT_TIMEOUT;
		rpc_getport(task, clnt);
	}
}

/*
 * 4a.	Connect to the RPC server (TCP case)
 */
static void
call_connect(struct rpc_task *task)
{
	struct rpc_clnt *clnt = task->tk_client;

	dprintk("RPC: %4d call_connect status %d\n",
				task->tk_pid, task->tk_status);

	if (xprt_connected(clnt->cl_xprt)) {
		task->tk_action = call_transmit;
		return;
	}
	task->tk_action = call_connect_status;
	if (task->tk_status < 0)
		return;
	xprt_connect(task);
}

/*
 * 4b. Sort out connect result
 */
static void
call_connect_status(struct rpc_task *task)
{
	struct rpc_clnt *clnt = task->tk_client;
	int status = task->tk_status;

	task->tk_status = 0;
	if (status >= 0) {
		clnt->cl_stats->netreconn++;
		task->tk_action = call_transmit;
		return;
	}

	/* Something failed: we may have to rebind */
	if (clnt->cl_autobind)
		clnt->cl_port = 0;
	switch (status) {
	case -ENOTCONN:
	case -ETIMEDOUT:
	case -EAGAIN:
		task->tk_action = (clnt->cl_port == 0) ? call_bind : call_connect;
		break;
	default:
		rpc_exit(task, -EIO);
	}
}

/*
 * 5.	Transmit the RPC request, and wait for reply
 */
static void
call_transmit(struct rpc_task *task)
{
	dprintk("RPC: %4d call_transmit (status %d)\n", 
				task->tk_pid, task->tk_status);

	task->tk_action = call_status;
	if (task->tk_status < 0)
		return;
	task->tk_status = xprt_prepare_transmit(task);
	if (task->tk_status != 0)
		return;
	/* Encode here so that rpcsec_gss can use correct sequence number. */
	if (!task->tk_rqstp->rq_bytes_sent)
		call_encode(task);
	if (task->tk_status < 0)
		return;
	xprt_transmit(task);
	if (task->tk_status < 0)
		return;
	if (!task->tk_msg.rpc_proc->p_decode) {
		task->tk_action = NULL;
		rpc_wake_up_task(task);
	}
}

/*
 * 6.	Sort out the RPC call status
 */
static void
call_status(struct rpc_task *task)
{
	struct rpc_clnt	*clnt = task->tk_client;
	struct rpc_rqst	*req = task->tk_rqstp;
	int		status;

	if (req->rq_received > 0 && !req->rq_bytes_sent)
		task->tk_status = req->rq_received;

	dprintk("RPC: %4d call_status (status %d)\n", 
				task->tk_pid, task->tk_status);

	status = task->tk_status;
	if (status >= 0) {
		task->tk_action = call_decode;
		return;
	}

	task->tk_status = 0;
	switch(status) {
	case -ETIMEDOUT:
		task->tk_action = call_timeout;
		break;
	case -ECONNREFUSED:
	case -ENOTCONN:
		req->rq_bytes_sent = 0;
		if (clnt->cl_autobind)
			clnt->cl_port = 0;
		task->tk_action = call_bind;
		break;
	case -EAGAIN:
		task->tk_action = call_transmit;
		break;
	case -EIO:
		/* shutdown or soft timeout */
		rpc_exit(task, status);
		break;
	default:
		if (clnt->cl_chatty)
			printk("%s: RPC call returned error %d\n",
			       clnt->cl_protname, -status);
		rpc_exit(task, status);
		break;
	}
}

/*
 * 6a.	Handle RPC timeout
 * 	We do not release the request slot, so we keep using the
 *	same XID for all retransmits.
 */
static void
call_timeout(struct rpc_task *task)
{
	struct rpc_clnt	*clnt = task->tk_client;

	if (xprt_adjust_timeout(task->tk_rqstp) == 0) {
		dprintk("RPC: %4d call_timeout (minor)\n", task->tk_pid);
		goto retry;
	}

	dprintk("RPC: %4d call_timeout (major)\n", task->tk_pid);
	if (RPC_IS_SOFT(task)) {
		if (clnt->cl_chatty)
			printk(KERN_NOTICE "%s: server %s not responding, timed out\n",
				clnt->cl_protname, clnt->cl_server);
		rpc_exit(task, -EIO);
		return;
	}

	if (clnt->cl_chatty && !(task->tk_flags & RPC_CALL_MAJORSEEN)) {
		task->tk_flags |= RPC_CALL_MAJORSEEN;
		printk(KERN_NOTICE "%s: server %s not responding, still trying\n",
			clnt->cl_protname, clnt->cl_server);
	}
	if (clnt->cl_autobind)
		clnt->cl_port = 0;

retry:
	clnt->cl_stats->rpcretrans++;
	task->tk_action = call_bind;
	task->tk_status = 0;
}

/*
 * 7.	Decode the RPC reply
 */
static void
call_decode(struct rpc_task *task)
{
	struct rpc_clnt	*clnt = task->tk_client;
	struct rpc_rqst	*req = task->tk_rqstp;
	kxdrproc_t	decode = task->tk_msg.rpc_proc->p_decode;
	u32		*p;

	dprintk("RPC: %4d call_decode (status %d)\n", 
				task->tk_pid, task->tk_status);

	if (clnt->cl_chatty && (task->tk_flags & RPC_CALL_MAJORSEEN)) {
		printk(KERN_NOTICE "%s: server %s OK\n",
			clnt->cl_protname, clnt->cl_server);
		task->tk_flags &= ~RPC_CALL_MAJORSEEN;
	}

	if (task->tk_status < 12) {
		if (!RPC_IS_SOFT(task)) {
			task->tk_action = call_bind;
			clnt->cl_stats->rpcretrans++;
			goto out_retry;
		}
		printk(KERN_WARNING "%s: too small RPC reply size (%d bytes)\n",
			clnt->cl_protname, task->tk_status);
		rpc_exit(task, -EIO);
		return;
	}

	req->rq_rcv_buf.len = req->rq_private_buf.len;

	/* Check that the softirq receive buffer is valid */
	WARN_ON(memcmp(&req->rq_rcv_buf, &req->rq_private_buf,
				sizeof(req->rq_rcv_buf)) != 0);

	/* Verify the RPC header */
	if (!(p = call_verify(task))) {
		if (task->tk_action == NULL)
			return;
		goto out_retry;
	}

	/*
	 * The following is an NFS-specific hack to cater for setuid
	 * processes whose uid is mapped to nobody on the server.
	 */
	if (task->tk_client->cl_droppriv && 
            (ntohl(*p) == NFSERR_ACCES || ntohl(*p) == NFSERR_PERM)) {
		if (RPC_IS_SETUID(task) && task->tk_suid_retry) {
			dprintk("RPC: %4d retry squashed uid\n", task->tk_pid);
			task->tk_flags ^= RPC_CALL_REALUID;
			task->tk_action = call_bind;
			task->tk_suid_retry--;
			goto out_retry;
		}
	}

	task->tk_action = NULL;

	if (decode)
		task->tk_status = rpcauth_unwrap_resp(task, decode, req, p,
						      task->tk_msg.rpc_resp);
	dprintk("RPC: %4d call_decode result %d\n", task->tk_pid,
					task->tk_status);
	return;
out_retry:
	req->rq_received = req->rq_private_buf.len = 0;
	task->tk_status = 0;
}

/*
 * 8.	Refresh the credentials if rejected by the server
 */
static void
call_refresh(struct rpc_task *task)
{
	dprintk("RPC: %4d call_refresh\n", task->tk_pid);

	xprt_release(task);	/* Must do to obtain new XID */
	task->tk_action = call_refreshresult;
	task->tk_status = 0;
	task->tk_client->cl_stats->rpcauthrefresh++;
	rpcauth_refreshcred(task);
}

/*
 * 8a.	Process the results of a credential refresh
 */
static void
call_refreshresult(struct rpc_task *task)
{
	int status = task->tk_status;
	dprintk("RPC: %4d call_refreshresult (status %d)\n", 
				task->tk_pid, task->tk_status);

	task->tk_status = 0;
	task->tk_action = call_reserve;
	if (status >= 0 && rpcauth_uptodatecred(task))
		return;
	if (rpcauth_deadcred(task)) {
		rpc_exit(task, -EACCES);
		return;
	}
	task->tk_action = call_refresh;
	if (status != -ETIMEDOUT)
		rpc_delay(task, 3*HZ);
	return;
}

/*
 * Call header serialization
 */
static u32 *
call_header(struct rpc_task *task)
{
	struct rpc_clnt *clnt = task->tk_client;
	struct rpc_xprt *xprt = clnt->cl_xprt;
	struct rpc_rqst	*req = task->tk_rqstp;
	u32		*p = req->rq_svec[0].iov_base;

	/* FIXME: check buffer size? */
	if (xprt->stream)
		*p++ = 0;		/* fill in later */
	*p++ = req->rq_xid;		/* XID */
	*p++ = htonl(RPC_CALL);		/* CALL */
	*p++ = htonl(RPC_VERSION);	/* RPC version */
	*p++ = htonl(clnt->cl_prog);	/* program number */
	*p++ = htonl(clnt->cl_vers);	/* program version */
	*p++ = htonl(task->tk_msg.rpc_proc->p_proc);	/* procedure */
	return rpcauth_marshcred(task, p);
}

/*
 * Reply header verification
 */
static u32 *
call_verify(struct rpc_task *task)
{
	u32	*p = task->tk_rqstp->rq_rcv_buf.head[0].iov_base, n;

	p += 1;	/* skip XID */

	if ((n = ntohl(*p++)) != RPC_REPLY) {
		printk(KERN_WARNING "call_verify: not an RPC reply: %x\n", n);
		goto garbage;
	}
	if ((n = ntohl(*p++)) != RPC_MSG_ACCEPTED) {
		int	error = -EACCES;

		if ((n = ntohl(*p++)) != RPC_AUTH_ERROR) {
			printk(KERN_WARNING "call_verify: RPC call rejected: %x\n", n);
		} else
		switch ((n = ntohl(*p++))) {
		case RPC_AUTH_REJECTEDCRED:
		case RPC_AUTH_REJECTEDVERF:
		case RPCSEC_GSS_CREDPROBLEM:
		case RPCSEC_GSS_CTXPROBLEM:
			if (!task->tk_cred_retry)
				break;
			task->tk_cred_retry--;
			dprintk("RPC: %4d call_verify: retry stale creds\n",
							task->tk_pid);
			rpcauth_invalcred(task);
			task->tk_action = call_refresh;
			return NULL;
		case RPC_AUTH_BADCRED:
		case RPC_AUTH_BADVERF:
			/* possibly garbled cred/verf? */
			if (!task->tk_garb_retry)
				break;
			task->tk_garb_retry--;
			dprintk("RPC: %4d call_verify: retry garbled creds\n",
							task->tk_pid);
			task->tk_action = call_bind;
			return NULL;
		case RPC_AUTH_TOOWEAK:
			printk(KERN_NOTICE "call_verify: server requires stronger "
			       "authentication.\n");
			break;
		default:
			printk(KERN_WARNING "call_verify: unknown auth error: %x\n", n);
			error = -EIO;
		}
		dprintk("RPC: %4d call_verify: call rejected %d\n",
						task->tk_pid, n);
		rpc_exit(task, error);
		return NULL;
	}
	if (!(p = rpcauth_checkverf(task, p))) {
		printk(KERN_WARNING "call_verify: auth check failed\n");
		goto garbage;		/* bad verifier, retry */
	}
	switch ((n = ntohl(*p++))) {
	case RPC_SUCCESS:
		return p;
	case RPC_PROG_UNAVAIL:
		printk(KERN_WARNING "RPC: call_verify: program %u is unsupported by server %s\n",
				(unsigned int)task->tk_client->cl_prog,
				task->tk_client->cl_server);
		goto out_eio;
	case RPC_PROG_MISMATCH:
		printk(KERN_WARNING "RPC: call_verify: program %u, version %u unsupported by server %s\n",
				(unsigned int)task->tk_client->cl_prog,
				(unsigned int)task->tk_client->cl_vers,
				task->tk_client->cl_server);
		goto out_eio;
	case RPC_PROC_UNAVAIL:
		printk(KERN_WARNING "RPC: call_verify: proc %p unsupported by program %u, version %u on server %s\n",
				task->tk_msg.rpc_proc,
				task->tk_client->cl_prog,
				task->tk_client->cl_vers,
				task->tk_client->cl_server);
		goto out_eio;
	case RPC_GARBAGE_ARGS:
		break;			/* retry */
	default:
		printk(KERN_WARNING "call_verify: server accept status: %x\n", n);
		/* Also retry */
	}

garbage:
	dprintk("RPC: %4d call_verify: server saw garbage\n", task->tk_pid);
	task->tk_client->cl_stats->rpcgarbage++;
	if (task->tk_garb_retry) {
		task->tk_garb_retry--;
		dprintk(KERN_WARNING "RPC: garbage, retrying %4d\n", task->tk_pid);
		task->tk_action = call_bind;
		return NULL;
	}
	printk(KERN_WARNING "RPC: garbage, exit EIO\n");
out_eio:
	rpc_exit(task, -EIO);
	return NULL;
}
