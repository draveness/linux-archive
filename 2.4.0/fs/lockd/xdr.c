/*
 * linux/fs/lockd/xdr.c
 *
 * XDR support for lockd and the lock client.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/utsname.h>
#include <linux/nfs.h>

#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/stats.h>
#include <linux/lockd/lockd.h>
#include <linux/lockd/sm_inter.h>

#define NLMDBG_FACILITY		NLMDBG_XDR


static inline loff_t
s32_to_loff_t(__s32 offset)
{
	return (loff_t)offset;
}

static inline __s32
loff_t_to_s32(loff_t offset)
{
	__s32 res;
	if (offset >= NLM_OFFSET_MAX)
		res = NLM_OFFSET_MAX;
	else if (offset <= -NLM_OFFSET_MAX)
		res = -NLM_OFFSET_MAX;
	else
		res = offset;
	return res;
}

/*
 * XDR functions for basic NLM types
 */
static inline u32 *nlm_decode_cookie(u32 *p, struct nlm_cookie *c)
{
	unsigned int	len;

	len = ntohl(*p++);
	
	if(len==0)
	{
		c->len=4;
		memset(c->data, 0, 4);	/* hockeypux brain damage */
	}
	else if(len<=8)
	{
		c->len=len;
		memcpy(c->data, p, len);
		p+=(len+3)>>2;
	}
	else 
	{
		printk(KERN_NOTICE
			"lockd: bad cookie size %d (only cookies under 8 bytes are supported.)\n", len);
		return NULL;
	}
	return p;
}

static inline u32 *
nlm_encode_cookie(u32 *p, struct nlm_cookie *c)
{
	*p++ = htonl(c->len);
	memcpy(p, c->data, c->len);
	p+=(c->len+3)>>2;
	return p;
}

static inline u32 *
nlm_decode_fh(u32 *p, struct nfs_fh *f)
{
	unsigned int	len;

	if ((len = ntohl(*p++)) != NFS2_FHSIZE) {
		printk(KERN_NOTICE
			"lockd: bad fhandle size %x (should be %d)\n",
			len, NFS2_FHSIZE);
		return NULL;
	}
	f->size = NFS2_FHSIZE;
	memcpy(f->data, p, NFS2_FHSIZE);
	return p + XDR_QUADLEN(NFS2_FHSIZE);
}

static inline u32 *
nlm_encode_fh(u32 *p, struct nfs_fh *f)
{
	*p++ = htonl(NFS2_FHSIZE);
	memcpy(p, f->data, NFS2_FHSIZE);
	return p + XDR_QUADLEN(NFS2_FHSIZE);
}

/*
 * Encode and decode owner handle
 */
static inline u32 *
nlm_decode_oh(u32 *p, struct xdr_netobj *oh)
{
	return xdr_decode_netobj(p, oh);
}

static inline u32 *
nlm_encode_oh(u32 *p, struct xdr_netobj *oh)
{
	return xdr_encode_netobj(p, oh);
}

static inline u32 *
nlm_decode_lock(u32 *p, struct nlm_lock *lock)
{
	struct file_lock	*fl = &lock->fl;
	s32			start, len, end;

	if (!(p = xdr_decode_string(p, &lock->caller, &len, NLM_MAXSTRLEN))
	 || !(p = nlm_decode_fh(p, &lock->fh))
	 || !(p = nlm_decode_oh(p, &lock->oh)))
		return NULL;

	locks_init_lock(fl);
	fl->fl_owner = current->files;
	fl->fl_pid   = ntohl(*p++);
	fl->fl_flags = FL_POSIX;
	fl->fl_type  = F_RDLCK;		/* as good as anything else */
	start = ntohl(*p++);
	len = ntohl(*p++);
	end = start + len - 1;

	fl->fl_start = s32_to_loff_t(start);

	if (len == 0 || end < 0)
		fl->fl_end = OFFSET_MAX;
	else
		fl->fl_end = s32_to_loff_t(end);
	return p;
}

/*
 * Encode a lock as part of an NLM call
 */
static u32 *
nlm_encode_lock(u32 *p, struct nlm_lock *lock)
{
	struct file_lock	*fl = &lock->fl;
	__s32			start, len;

	if (!(p = xdr_encode_string(p, lock->caller))
	 || !(p = nlm_encode_fh(p, &lock->fh))
	 || !(p = nlm_encode_oh(p, &lock->oh)))
		return NULL;

	if (fl->fl_start > NLM_OFFSET_MAX
	 || (fl->fl_end > NLM_OFFSET_MAX && fl->fl_end != OFFSET_MAX))
		return NULL;

	start = loff_t_to_s32(fl->fl_start);
	if (fl->fl_end == OFFSET_MAX)
		len = 0;
	else
		len = loff_t_to_s32(fl->fl_end - fl->fl_start + 1);

	*p++ = htonl(fl->fl_pid);
	*p++ = htonl(start);
	*p++ = htonl(len);

	return p;
}

/*
 * Encode result of a TEST/TEST_MSG call
 */
static u32 *
nlm_encode_testres(u32 *p, struct nlm_res *resp)
{
	s32		start, len;

	if (!(p = nlm_encode_cookie(p, &resp->cookie)))
		return 0;
	*p++ = resp->status;

	if (resp->status == nlm_lck_denied) {
		struct file_lock	*fl = &resp->lock.fl;

		*p++ = (fl->fl_type == F_RDLCK)? xdr_zero : xdr_one;
		*p++ = htonl(fl->fl_pid);

		/* Encode owner handle. */
		if (!(p = xdr_encode_netobj(p, &resp->lock.oh)))
			return 0;

		start = loff_t_to_s32(fl->fl_start);
		if (fl->fl_end == OFFSET_MAX)
			len = 0;
		else
			len = loff_t_to_s32(fl->fl_end - fl->fl_start + 1);

		*p++ = htonl(start);
		*p++ = htonl(len);
	}

	return p;
}

/*
 * Check buffer bounds after decoding arguments
 */
static inline int
xdr_argsize_check(struct svc_rqst *rqstp, u32 *p)
{
	struct svc_buf	*buf = &rqstp->rq_argbuf;

	return p - buf->base <= buf->buflen;
}

static inline int
xdr_ressize_check(struct svc_rqst *rqstp, u32 *p)
{
	struct svc_buf	*buf = &rqstp->rq_resbuf;

	buf->len = p - buf->base;
	return (buf->len <= buf->buflen);
}

/*
 * First, the server side XDR functions
 */
int
nlmsvc_decode_testargs(struct svc_rqst *rqstp, u32 *p, nlm_args *argp)
{
	u32	exclusive;

	if (!(p = nlm_decode_cookie(p, &argp->cookie)))
		return 0;

	exclusive = ntohl(*p++);
	if (!(p = nlm_decode_lock(p, &argp->lock)))
		return 0;
	if (exclusive)
		argp->lock.fl.fl_type = F_WRLCK;

	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_encode_testres(struct svc_rqst *rqstp, u32 *p, struct nlm_res *resp)
{
	if (!(p = nlm_encode_testres(p, resp)))
		return 0;
	return xdr_ressize_check(rqstp, p);
}

int
nlmsvc_decode_lockargs(struct svc_rqst *rqstp, u32 *p, nlm_args *argp)
{
	u32	exclusive;

	if (!(p = nlm_decode_cookie(p, &argp->cookie)))
		return 0;
	argp->block  = ntohl(*p++);
	exclusive    = ntohl(*p++);
	if (!(p = nlm_decode_lock(p, &argp->lock)))
		return 0;
	if (exclusive)
		argp->lock.fl.fl_type = F_WRLCK;
	argp->reclaim = ntohl(*p++);
	argp->state   = ntohl(*p++);
	argp->monitor = 1;		/* monitor client by default */

	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_decode_cancargs(struct svc_rqst *rqstp, u32 *p, nlm_args *argp)
{
	u32	exclusive;

	if (!(p = nlm_decode_cookie(p, &argp->cookie)))
		return 0;
	argp->block = ntohl(*p++);
	exclusive = ntohl(*p++);
	if (!(p = nlm_decode_lock(p, &argp->lock)))
		return 0;
	if (exclusive)
		argp->lock.fl.fl_type = F_WRLCK;
	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_decode_unlockargs(struct svc_rqst *rqstp, u32 *p, nlm_args *argp)
{
	if (!(p = nlm_decode_cookie(p, &argp->cookie))
	 || !(p = nlm_decode_lock(p, &argp->lock)))
		return 0;
	argp->lock.fl.fl_type = F_UNLCK;
	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_decode_shareargs(struct svc_rqst *rqstp, u32 *p, nlm_args *argp)
{
	struct nlm_lock	*lock = &argp->lock;
	int		len;

	memset(lock, 0, sizeof(*lock));
	locks_init_lock(&lock->fl);
	lock->fl.fl_pid = ~(u32) 0;

	if (!(p = nlm_decode_cookie(p, &argp->cookie))
	 || !(p = xdr_decode_string(p, &lock->caller, &len, NLM_MAXSTRLEN))
	 || !(p = nlm_decode_fh(p, &lock->fh))
	 || !(p = nlm_decode_oh(p, &lock->oh)))
		return 0;
	argp->fsm_mode = ntohl(*p++);
	argp->fsm_access = ntohl(*p++);
	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_encode_shareres(struct svc_rqst *rqstp, u32 *p, struct nlm_res *resp)
{
	if (!(p = nlm_encode_cookie(p, &resp->cookie)))
		return 0;
	*p++ = resp->status;
	*p++ = xdr_zero;		/* sequence argument */
	return xdr_ressize_check(rqstp, p);
}

int
nlmsvc_encode_res(struct svc_rqst *rqstp, u32 *p, struct nlm_res *resp)
{
	if (!(p = nlm_encode_cookie(p, &resp->cookie)))
		return 0;
	*p++ = resp->status;
	return xdr_ressize_check(rqstp, p);
}

int
nlmsvc_decode_notify(struct svc_rqst *rqstp, u32 *p, struct nlm_args *argp)
{
	struct nlm_lock	*lock = &argp->lock;
	int		len;

	if (!(p = xdr_decode_string(p, &lock->caller, &len, NLM_MAXSTRLEN)))
		return 0;
	argp->state = ntohl(*p++);
	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_decode_reboot(struct svc_rqst *rqstp, u32 *p, struct nlm_reboot *argp)
{
	if (!(p = xdr_decode_string(p, &argp->mon, &argp->len, SM_MAXSTRLEN)))
		return 0;
	argp->state = ntohl(*p++);
	argp->addr = ntohl(*p++);
	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_decode_res(struct svc_rqst *rqstp, u32 *p, struct nlm_res *resp)
{
	if (!(p = nlm_decode_cookie(p, &resp->cookie)))
		return 0;
	resp->status = ntohl(*p++);
	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_decode_void(struct svc_rqst *rqstp, u32 *p, void *dummy)
{
	return xdr_argsize_check(rqstp, p);
}

int
nlmsvc_encode_void(struct svc_rqst *rqstp, u32 *p, void *dummy)
{
	return xdr_ressize_check(rqstp, p);
}

/*
 * Now, the client side XDR functions
 */
static int
nlmclt_encode_void(struct rpc_rqst *req, u32 *p, void *ptr)
{
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

static int
nlmclt_decode_void(struct rpc_rqst *req, u32 *p, void *ptr)
{
	return 0;
}

static int
nlmclt_encode_testargs(struct rpc_rqst *req, u32 *p, nlm_args *argp)
{
	struct nlm_lock	*lock = &argp->lock;

	if (!(p = nlm_encode_cookie(p, &argp->cookie)))
		return -EIO;
	*p++ = (lock->fl.fl_type == F_WRLCK)? xdr_one : xdr_zero;
	if (!(p = nlm_encode_lock(p, lock)))
		return -EIO;
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

static int
nlmclt_decode_testres(struct rpc_rqst *req, u32 *p, struct nlm_res *resp)
{
	if (!(p = nlm_decode_cookie(p, &resp->cookie)))
		return -EIO;
	resp->status = ntohl(*p++);
	if (resp->status == NLM_LCK_DENIED) {
		struct file_lock	*fl = &resp->lock.fl;
		u32			excl;
		s32			start, len, end;

		memset(&resp->lock, 0, sizeof(resp->lock));
		locks_init_lock(fl);
		excl = ntohl(*p++);
		fl->fl_pid = ntohl(*p++);
		if (!(p = nlm_decode_oh(p, &resp->lock.oh)))
			return -EIO;

		fl->fl_flags = FL_POSIX;
		fl->fl_type  = excl? F_WRLCK : F_RDLCK;
		start = ntohl(*p++);
		len = ntohl(*p++);
		end = start + len - 1;

		fl->fl_start = s32_to_loff_t(start);
		if (len == 0 || end < 0)
			fl->fl_end = OFFSET_MAX;
		else
			fl->fl_end = s32_to_loff_t(end);
	}
	return 0;
}


static int
nlmclt_encode_lockargs(struct rpc_rqst *req, u32 *p, nlm_args *argp)
{
	struct nlm_lock	*lock = &argp->lock;

	if (!(p = nlm_encode_cookie(p, &argp->cookie)))
		return -EIO;
	*p++ = argp->block? xdr_one : xdr_zero;
	*p++ = (lock->fl.fl_type == F_WRLCK)? xdr_one : xdr_zero;
	if (!(p = nlm_encode_lock(p, lock)))
		return -EIO;
	*p++ = argp->reclaim? xdr_one : xdr_zero;
	*p++ = htonl(argp->state);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

static int
nlmclt_encode_cancargs(struct rpc_rqst *req, u32 *p, nlm_args *argp)
{
	struct nlm_lock	*lock = &argp->lock;

	if (!(p = nlm_encode_cookie(p, &argp->cookie)))
		return -EIO;
	*p++ = argp->block? xdr_one : xdr_zero;
	*p++ = (lock->fl.fl_type == F_WRLCK)? xdr_one : xdr_zero;
	if (!(p = nlm_encode_lock(p, lock)))
		return -EIO;
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

static int
nlmclt_encode_unlockargs(struct rpc_rqst *req, u32 *p, nlm_args *argp)
{
	struct nlm_lock	*lock = &argp->lock;

	if (!(p = nlm_encode_cookie(p, &argp->cookie)))
		return -EIO;
	if (!(p = nlm_encode_lock(p, lock)))
		return -EIO;
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

static int
nlmclt_encode_res(struct rpc_rqst *req, u32 *p, struct nlm_res *resp)
{
	if (!(p = nlm_encode_cookie(p, &resp->cookie)))
		return -EIO;
	*p++ = resp->status;
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

static int
nlmclt_encode_testres(struct rpc_rqst *req, u32 *p, struct nlm_res *resp)
{
	if (!(p = nlm_encode_testres(p, resp)))
		return -EIO;
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

static int
nlmclt_decode_res(struct rpc_rqst *req, u32 *p, struct nlm_res *resp)
{
	if (!(p = nlm_decode_cookie(p, &resp->cookie)))
		return -EIO;
	resp->status = ntohl(*p++);
	return 0;
}

/*
 * Buffer requirements for NLM
 */
#define NLM_void_sz		0
#define NLM_cookie_sz		3	/* 1 len , 2 data */
#define NLM_caller_sz		1+QUADLEN(sizeof(system_utsname.nodename))
#define NLM_netobj_sz		1+QUADLEN(XDR_MAX_NETOBJ)
/* #define NLM_owner_sz		1+QUADLEN(NLM_MAXOWNER) */
#define NLM_fhandle_sz		1+QUADLEN(NFS2_FHSIZE)
#define NLM_lock_sz		3+NLM_caller_sz+NLM_netobj_sz+NLM_fhandle_sz
#define NLM_holder_sz		4+NLM_netobj_sz

#define NLM_testargs_sz		NLM_cookie_sz+1+NLM_lock_sz
#define NLM_lockargs_sz		NLM_cookie_sz+4+NLM_lock_sz
#define NLM_cancargs_sz		NLM_cookie_sz+2+NLM_lock_sz
#define NLM_unlockargs_sz	NLM_cookie_sz+NLM_lock_sz

#define NLM_testres_sz		NLM_cookie_sz+1+NLM_holder_sz
#define NLM_res_sz		NLM_cookie_sz+1
#define NLM_norep_sz		0

#ifndef MAX
# define MAX(a, b)		(((a) > (b))? (a) : (b))
#endif

/*
 * For NLM, a void procedure really returns nothing
 */
#define nlmclt_decode_norep	NULL

#define PROC(proc, argtype, restype)	\
    { "nlm_" #proc,						\
      (kxdrproc_t) nlmclt_encode_##argtype,			\
      (kxdrproc_t) nlmclt_decode_##restype,			\
      MAX(NLM_##argtype##_sz, NLM_##restype##_sz) << 2,		\
      0								\
    }

static struct rpc_procinfo	nlm_procedures[] = {
    PROC(null,		void,		void),
    PROC(test,		testargs,	testres),
    PROC(lock,		lockargs,	res),
    PROC(canc,		cancargs,	res),
    PROC(unlock,	unlockargs,	res),
    PROC(granted,	testargs,	res),
    PROC(test_msg,	testargs,	norep),
    PROC(lock_msg,	lockargs,	norep),
    PROC(canc_msg,	cancargs,	norep),
    PROC(unlock_msg,	unlockargs,	norep),
    PROC(granted_msg,	testargs,	norep),
    PROC(test_res,	testres,	norep),
    PROC(lock_res,	res,		norep),
    PROC(canc_res,	res,		norep),
    PROC(unlock_res,	res,		norep),
    PROC(granted_res,	res,		norep),
    PROC(undef,		void,		void),
    PROC(undef,		void,		void),
    PROC(undef,		void,		void),
    PROC(undef,		void,		void),
#ifdef NLMCLNT_SUPPORT_SHARES
    PROC(share,		shareargs,	shareres),
    PROC(unshare,	shareargs,	shareres),
    PROC(nm_lock,	lockargs,	res),
    PROC(free_all,	notify,		void),
#else
    PROC(undef,		void,		void),
    PROC(undef,		void,		void),
    PROC(undef,		void,		void),
    PROC(undef,		void,		void),
#endif
};

static struct rpc_version	nlm_version1 = {
	1, 16, nlm_procedures,
};

static struct rpc_version	nlm_version3 = {
	3, 24, nlm_procedures,
};

#ifdef 	CONFIG_LOCKD_V4
extern struct rpc_version nlm_version4;
#endif

static struct rpc_version *	nlm_versions[] = {
	NULL,
	&nlm_version1,
	NULL,
	&nlm_version3,
#ifdef 	CONFIG_LOCKD_V4
	&nlm_version4,
#endif
};

static struct rpc_stat		nlm_stats;

struct rpc_program		nlm_program = {
	"lockd",
	NLM_PROGRAM,
	sizeof(nlm_versions) / sizeof(nlm_versions[0]),
	nlm_versions,
	&nlm_stats,
};

#ifdef LOCKD_DEBUG
char *
nlm_procname(u32 proc)
{
	if (proc < sizeof(nlm_procedures)/sizeof(nlm_procedures[0]))
		return nlm_procedures[proc].p_procname;
	return "unknown";
}
#endif

