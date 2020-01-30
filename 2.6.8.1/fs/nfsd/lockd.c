/*
 * linux/fs/nfsd/lockd.c
 *
 * This file contains all the stubs needed when communicating with lockd.
 * This level of indirection is necessary so we can run nfsd+lockd without
 * requiring the nfs client to be compiled in/loaded, and vice versa.
 *
 * Copyright (C) 1996, Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#include <linux/lockd/bind.h>

#define NFSDDBG_FACILITY		NFSDDBG_LOCKD

/*
 * Note: we hold the dentry use count while the file is open.
 */
static u32
nlm_fopen(struct svc_rqst *rqstp, struct nfs_fh *f, struct file *filp)
{
	u32		nfserr;
	struct svc_fh	fh;

	/* must initialize before using! but maxsize doesn't matter */
	fh_init(&fh,0);
	fh.fh_handle.fh_size = f->size;
	memcpy((char*)&fh.fh_handle.fh_base, f->data, f->size);
	fh.fh_export = NULL;

	exp_readlock();
	nfserr = nfsd_open(rqstp, &fh, S_IFREG, MAY_LOCK, filp);
	if (!nfserr) {
		dget(filp->f_dentry);
		mntget(filp->f_vfsmnt);
	}
	fh_put(&fh);
	rqstp->rq_client = NULL;
	exp_readunlock();
 	/* nlm and nfsd don't share error codes.
	 * we invent: 0 = no error
	 *            1 = stale file handle
	 *	      2 = other error
	 */
	switch (nfserr) {
	case nfs_ok:
		return 0;
	case nfserr_stale:
		return 1;
	default:
		return 2;
	}
}

static void
nlm_fclose(struct file *filp)
{
	nfsd_close(filp);
	dput(filp->f_dentry);
	mntput(filp->f_vfsmnt);
}

struct nlmsvc_binding		nfsd_nlm_ops = {
	.fopen		= nlm_fopen,		/* open file for locking */
	.fclose		= nlm_fclose,		/* close file */
};

void
nfsd_lockd_init(void)
{
	dprintk("nfsd: initializing lockd\n");
	nlmsvc_ops = &nfsd_nlm_ops;
}

void
nfsd_lockd_shutdown(void)
{
	nlmsvc_ops = NULL;
}
