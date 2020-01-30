/*
 * include/linux/nfsd/export.h
 * 
 * Public declarations for NFS exports. The definitions for the
 * syscall interface are in nfsctl.h
 *
 * Copyright (C) 1995-1997 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef NFSD_EXPORT_H
#define NFSD_EXPORT_H

#include <asm/types.h>
#ifdef __KERNEL__
# include <linux/types.h>
# include <linux/in.h>
#endif

/*
 * Important limits for the exports stuff.
 */
#define NFSCLNT_IDMAX		1024
#define NFSCLNT_ADDRMAX		16
#define NFSCLNT_KEYMAX		32

/*
 * Export flags.
 */
#define NFSEXP_READONLY		0x0001
#define NFSEXP_INSECURE_PORT	0x0002
#define NFSEXP_ROOTSQUASH	0x0004
#define NFSEXP_ALLSQUASH	0x0008
#define NFSEXP_ASYNC		0x0010
#define NFSEXP_GATHERED_WRITES	0x0020
/* 40 80 100 currently unused */
#define NFSEXP_NOHIDE		0x0200
#define NFSEXP_NOSUBTREECHECK	0x0400
#define	NFSEXP_NOAUTHNLM	0x0800		/* Don't authenticate NLM requests - just trust */
#define NFSEXP_MSNFS		0x1000	/* do silly things that MS clients expect */
#define NFSEXP_FSID		0x2000
#define	NFSEXP_CROSSMOUNT	0x4000
#define	NFSEXP_NOACL		0x8000	/* reserved for possible ACL related use */
#define NFSEXP_ALLFLAGS		0xFE3F


#ifdef __KERNEL__

struct svc_export {
	struct cache_head	h;
	struct auth_domain *	ex_client;
	int			ex_flags;
	struct vfsmount *	ex_mnt;
	struct dentry *		ex_dentry;
	uid_t			ex_anon_uid;
	gid_t			ex_anon_gid;
	int			ex_fsid;
};

/* an "export key" (expkey) maps a filehandlefragement to an
 * svc_export for a given client.  There can be two per export, one
 * for type 0 (dev/ino), one for type 1 (fsid)
 */
struct svc_expkey {
	struct cache_head	h;

	struct auth_domain *	ek_client;
	int			ek_fsidtype;
	u32			ek_fsid[3];

	struct svc_export *	ek_export;
};

#define EX_SECURE(exp)		(!((exp)->ex_flags & NFSEXP_INSECURE_PORT))
#define EX_ISSYNC(exp)		(!((exp)->ex_flags & NFSEXP_ASYNC))
#define EX_RDONLY(exp)		((exp)->ex_flags & NFSEXP_READONLY)
#define EX_NOHIDE(exp)		((exp)->ex_flags & NFSEXP_NOHIDE)
#define EX_WGATHER(exp)		((exp)->ex_flags & NFSEXP_GATHERED_WRITES)


/*
 * Function declarations
 */
void			nfsd_export_init(void);
void			nfsd_export_shutdown(void);
void			nfsd_export_flush(void);
void			exp_readlock(void);
void			exp_readunlock(void);
struct svc_expkey *	exp_find_key(struct auth_domain *clp, 
				     int fsid_type, u32 *fsidv,
				     struct cache_req *reqp);
struct svc_export *	exp_get_by_name(struct auth_domain *clp,
					struct vfsmount *mnt,
					struct dentry *dentry,
					struct cache_req *reqp);
struct svc_export *	exp_parent(struct auth_domain *clp,
				   struct vfsmount *mnt,
				   struct dentry *dentry,
				   struct cache_req *reqp);
int			exp_rootfh(struct auth_domain *, 
					char *path, struct knfsd_fh *, int maxsize);
int			exp_pseudoroot(struct auth_domain *, struct svc_fh *fhp, struct cache_req *creq);
int			nfserrno(int errno);

extern void expkey_put(struct cache_head *item, struct cache_detail *cd);
extern void svc_export_put(struct cache_head *item, struct cache_detail *cd);
extern struct cache_detail svc_export_cache, svc_expkey_cache;

static inline void exp_put(struct svc_export *exp)
{
	svc_export_put(&exp->h, &svc_export_cache);
}

static inline void exp_get(struct svc_export *exp)
{
	cache_get(&exp->h);
}
static inline struct svc_export *
exp_find(struct auth_domain *clp, int fsid_type, u32 *fsidv,
	 struct cache_req *reqp)
{
	struct svc_expkey *ek = exp_find_key(clp, fsid_type, fsidv, reqp);
	if (ek && !IS_ERR(ek)) {
		struct svc_export *exp = ek->ek_export;
		int err;
		exp_get(exp);
		expkey_put(&ek->h, &svc_expkey_cache);
		if ((err = cache_check(&svc_export_cache, &exp->h, reqp)))
			exp = ERR_PTR(err);
		return exp;
	} else
		return ERR_PTR(PTR_ERR(ek));
}

#endif /* __KERNEL__ */

#endif /* NFSD_EXPORT_H */

