/*
 * nfsproc2.c	Process version 2 NFS requests.
 * linux/fs/nfsd/nfs2proc.c
 * 
 * Process version 2 NFS requests.
 *
 * Copyright (C) 1995-1997 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/linkage.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/locks.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/version.h>
#include <linux/unistd.h>
#include <linux/malloc.h>

#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfsd/cache.h>
#include <linux/nfsd/xdr.h>

typedef struct svc_rqst	svc_rqst;
typedef struct svc_buf	svc_buf;

#define NFSDDBG_FACILITY		NFSDDBG_PROC


static void
svcbuf_reserve(struct svc_buf *buf, u32 **ptr, int *len, int nr)
{
	*ptr = buf->buf + nr;
	*len = buf->buflen - buf->len - nr;
}

static int
nfsd_proc_null(struct svc_rqst *rqstp, void *argp, void *resp)
{
	return nfs_ok;
}

/*
 * Get a file's attributes
 * N.B. After this call resp->fh needs an fh_put
 */
static int
nfsd_proc_getattr(struct svc_rqst *rqstp, struct nfsd_fhandle  *argp,
					  struct nfsd_attrstat *resp)
{
	dprintk("nfsd: GETATTR  %s\n", SVCFH_fmt(&argp->fh));

	fh_copy(&resp->fh, &argp->fh);
	return fh_verify(rqstp, &resp->fh, 0, MAY_NOP);
}

/*
 * Set a file's attributes
 * N.B. After this call resp->fh needs an fh_put
 */
static int
nfsd_proc_setattr(struct svc_rqst *rqstp, struct nfsd_sattrargs *argp,
					  struct nfsd_attrstat  *resp)
{
	dprintk("nfsd: SETATTR  %s, valid=%x, size=%ld\n",
		SVCFH_fmt(&argp->fh),
		argp->attrs.ia_valid, (long) argp->attrs.ia_size);

	fh_copy(&resp->fh, &argp->fh);
	return nfsd_setattr(rqstp, &resp->fh, &argp->attrs);
}

/*
 * Look up a path name component
 * Note: the dentry in the resp->fh may be negative if the file
 * doesn't exist yet.
 * N.B. After this call resp->fh needs an fh_put
 */
static int
nfsd_proc_lookup(struct svc_rqst *rqstp, struct nfsd_diropargs *argp,
					 struct nfsd_diropres  *resp)
{
	int	nfserr;

	dprintk("nfsd: LOOKUP   %s %s\n",
		SVCFH_fmt(&argp->fh), argp->name);

	fh_init(&resp->fh, NFS_FHSIZE);
	nfserr = nfsd_lookup(rqstp, &argp->fh, argp->name, argp->len,
				 &resp->fh);

	fh_put(&argp->fh);
	return nfserr;
}

/*
 * Read a symlink.
 */
static int
nfsd_proc_readlink(struct svc_rqst *rqstp, struct nfsd_fhandle     *argp,
					   struct nfsd_readlinkres *resp)
{
	u32		*path;
	int		dummy, nfserr;

	dprintk("nfsd: READLINK %s\n", SVCFH_fmt(&argp->fh));

	/* Reserve room for status and path length */
	svcbuf_reserve(&rqstp->rq_resbuf, &path, &dummy, 2);

	/* Read the symlink. */
	resp->len = NFS_MAXPATHLEN;
	nfserr = nfsd_readlink(rqstp, &argp->fh, (char *) path, &resp->len);

	fh_put(&argp->fh);
	return nfserr;
}

/*
 * Read a portion of a file.
 * N.B. After this call resp->fh needs an fh_put
 */
static int
nfsd_proc_read(struct svc_rqst *rqstp, struct nfsd_readargs *argp,
				       struct nfsd_readres  *resp)
{
	u32 *	buffer;
	int	nfserr, avail;

	dprintk("nfsd: READ    %s %d bytes at %d\n",
		SVCFH_fmt(&argp->fh),
		argp->count, argp->offset);

	/* Obtain buffer pointer for payload. 19 is 1 word for
	 * status, 17 words for fattr, and 1 word for the byte count.
	 */
	svcbuf_reserve(&rqstp->rq_resbuf, &buffer, &avail, 19);

	if ((avail << 2) < argp->count) {
		printk(KERN_NOTICE
			"oversized read request from %08x:%d (%d bytes)\n",
				ntohl(rqstp->rq_addr.sin_addr.s_addr),
				ntohs(rqstp->rq_addr.sin_port),
				argp->count);
		argp->count = avail << 2;
	}

	resp->count = argp->count;
	nfserr = nfsd_read(rqstp, fh_copy(&resp->fh, &argp->fh),
				  argp->offset,
				  (char *) buffer,
				  &resp->count);

	return nfserr;
}

/*
 * Write data to a file
 * N.B. After this call resp->fh needs an fh_put
 */
static int
nfsd_proc_write(struct svc_rqst *rqstp, struct nfsd_writeargs *argp,
					struct nfsd_attrstat  *resp)
{
	int	nfserr;
	int	stable = 1;

	dprintk("nfsd: WRITE    %s %d bytes at %d\n",
		SVCFH_fmt(&argp->fh),
		argp->len, argp->offset);

	nfserr = nfsd_write(rqstp, fh_copy(&resp->fh, &argp->fh),
				   argp->offset,
				   argp->data,
				   argp->len,
				   &stable);
	return nfserr;
}

/*
 * CREATE processing is complicated. The keyword here is `overloaded.'
 * The parent directory is kept locked between the check for existence
 * and the actual create() call in compliance with VFS protocols.
 * N.B. After this call _both_ argp->fh and resp->fh need an fh_put
 */
static int
nfsd_proc_create(struct svc_rqst *rqstp, struct nfsd_createargs *argp,
					 struct nfsd_diropres   *resp)
{
	svc_fh		*dirfhp = &argp->fh;
	svc_fh		*newfhp = &resp->fh;
	struct iattr	*attr = &argp->attrs;
	struct inode	*inode;
	struct dentry	*dchild;
	int		nfserr, type, mode, rdonly = 0;
	dev_t		rdev = NODEV;

	dprintk("nfsd: CREATE   %s %s\n",
		SVCFH_fmt(dirfhp), argp->name);

	/* First verify the parent file handle */
	nfserr = fh_verify(rqstp, dirfhp, S_IFDIR, MAY_EXEC);
	if (nfserr)
		goto done; /* must fh_put dirfhp even on error */

	/* Check for MAY_WRITE separately. */
	nfserr = nfsd_permission(dirfhp->fh_export, dirfhp->fh_dentry,
				 MAY_WRITE);
	if (nfserr == nfserr_rofs) {
		rdonly = 1;	/* Non-fatal error for echo > /dev/null */
	} else if (nfserr)
		goto done;

	nfserr = nfserr_acces;
	if (!argp->len)
		goto done;
	nfserr = nfserr_exist;
	if (isdotent(argp->name, argp->len))
		goto done;
	fh_lock(dirfhp);
	dchild = lookup_one(argp->name, dirfhp->fh_dentry);
	if (IS_ERR(dchild)) {
		nfserr = nfserrno(PTR_ERR(dchild));
		goto out_unlock;
	}
	fh_init(newfhp, NFS_FHSIZE);
	nfserr = fh_compose(newfhp, dirfhp->fh_export, dchild);
	if (!nfserr && !dchild->d_inode)
		nfserr = nfserr_noent;
	if (nfserr) {
		if (nfserr != nfserr_noent)
			goto out_unlock;
		/*
		 * If the new file handle wasn't verified, we can't tell
		 * whether the file exists or not. Time to bail ...
		 */
		nfserr = nfserr_acces;
		if (!newfhp->fh_dentry) {
			printk(KERN_WARNING 
				"nfsd_proc_create: file handle not verified\n");
			goto out_unlock;
		}
	}

	inode = newfhp->fh_dentry->d_inode;

	/* Unfudge the mode bits */
	if (attr->ia_valid & ATTR_MODE) { 
		type = attr->ia_mode & S_IFMT;
		mode = attr->ia_mode & ~S_IFMT;
		if (!type)	/* HP weirdness */
			type = S_IFREG;
	} else if (inode) {
		type = inode->i_mode & S_IFMT;
		mode = inode->i_mode & ~S_IFMT;
	} else {
		type = S_IFREG;
		mode = 0;	/* ??? */
	}

	/* This is for "echo > /dev/null" a la SunOS. Argh. */
	nfserr = nfserr_rofs;
	if (rdonly && (!inode || type == S_IFREG))
		goto out_unlock;

	attr->ia_valid |= ATTR_MODE;
	attr->ia_mode = mode;

	/* Special treatment for non-regular files according to the
	 * gospel of sun micro
	 */
	if (type != S_IFREG) {
		int	is_borc = 0;
		u32	size = attr->ia_size;

		rdev = (dev_t) size;
		if (type != S_IFBLK && type != S_IFCHR) {
			rdev = 0;
		} else if (type == S_IFCHR && !(attr->ia_valid & ATTR_SIZE)) {
			/* If you think you've seen the worst, grok this. */
			type = S_IFIFO;
		} else if (size != rdev) {
			/* dev got truncated because of 16bit Linux dev_t */
			nfserr = nfserr_inval;
			goto out_unlock;
		} else {
			/* Okay, char or block special */
			is_borc = 1;
		}

		/* we've used the SIZE information, so discard it */
		attr->ia_valid &= ~ATTR_SIZE;

		/* Make sure the type and device matches */
		nfserr = nfserr_exist;
		if (inode && (type != (inode->i_mode & S_IFMT) || 
		    (is_borc && inode->i_rdev != rdev)))
			goto out_unlock;
	}
	
	nfserr = 0;
	if (!inode) {
		/* File doesn't exist. Create it and set attrs */
		nfserr = nfsd_create(rqstp, dirfhp, argp->name, argp->len,
					attr, type, rdev, newfhp);
	} else if (type == S_IFREG) {
		dprintk("nfsd:   existing %s, valid=%x, size=%ld\n",
			argp->name, attr->ia_valid, (long) attr->ia_size);
		/* File already exists. We ignore all attributes except
		 * size, so that creat() behaves exactly like
		 * open(..., O_CREAT|O_TRUNC|O_WRONLY).
		 */
		attr->ia_valid &= ATTR_SIZE;
		if (attr->ia_valid)
			nfserr = nfsd_setattr(rqstp, newfhp, attr);
	}

out_unlock:
	/* We don't really need to unlock, as fh_put does it. */
	fh_unlock(dirfhp);

done:
	fh_put(dirfhp);
	return nfserr;
}

static int
nfsd_proc_remove(struct svc_rqst *rqstp, struct nfsd_diropargs *argp,
					 void		       *resp)
{
	int	nfserr;

	dprintk("nfsd: REMOVE   %s %s\n", SVCFH_fmt(&argp->fh), argp->name);

	/* Unlink. -SIFDIR means file must not be a directory */
	nfserr = nfsd_unlink(rqstp, &argp->fh, -S_IFDIR, argp->name, argp->len);
	fh_put(&argp->fh);
	return nfserr;
}

static int
nfsd_proc_rename(struct svc_rqst *rqstp, struct nfsd_renameargs *argp,
				  	 void		        *resp)
{
	int	nfserr;

	dprintk("nfsd: RENAME   %s %s -> \n",
		SVCFH_fmt(&argp->ffh), argp->fname);
	dprintk("nfsd:        ->  %s %s\n",
		SVCFH_fmt(&argp->tfh), argp->tname);

	nfserr = nfsd_rename(rqstp, &argp->ffh, argp->fname, argp->flen,
				    &argp->tfh, argp->tname, argp->tlen);
	fh_put(&argp->ffh);
	fh_put(&argp->tfh);
	return nfserr;
}

static int
nfsd_proc_link(struct svc_rqst *rqstp, struct nfsd_linkargs *argp,
				void			    *resp)
{
	int	nfserr;

	dprintk("nfsd: LINK     %s ->\n",
		SVCFH_fmt(&argp->ffh));
	dprintk("nfsd:    %s %s\n",
		SVCFH_fmt(&argp->tfh),
		argp->tname);

	nfserr = nfsd_link(rqstp, &argp->tfh, argp->tname, argp->tlen,
				  &argp->ffh);
	fh_put(&argp->ffh);
	fh_put(&argp->tfh);
	return nfserr;
}

static int
nfsd_proc_symlink(struct svc_rqst *rqstp, struct nfsd_symlinkargs *argp,
				          void			  *resp)
{
	struct svc_fh	newfh;
	int		nfserr;

	dprintk("nfsd: SYMLINK  %s %s -> %s\n",
		SVCFH_fmt(&argp->ffh), argp->fname, argp->tname);

	fh_init(&newfh, NFS_FHSIZE);
	/*
	 * Create the link, look up new file and set attrs.
	 */
	nfserr = nfsd_symlink(rqstp, &argp->ffh, argp->fname, argp->flen,
						 argp->tname, argp->tlen,
				 		 &newfh, &argp->attrs);


	fh_put(&argp->ffh);
	fh_put(&newfh);
	return nfserr;
}

/*
 * Make directory. This operation is not idempotent.
 * N.B. After this call resp->fh needs an fh_put
 */
static int
nfsd_proc_mkdir(struct svc_rqst *rqstp, struct nfsd_createargs *argp,
					struct nfsd_diropres   *resp)
{
	int	nfserr;

	dprintk("nfsd: MKDIR    %s %s\n", SVCFH_fmt(&argp->fh), argp->name);

	if (resp->fh.fh_dentry) {
		printk(KERN_WARNING
			"nfsd_proc_mkdir: response already verified??\n");
	}

	argp->attrs.ia_valid &= ~ATTR_SIZE;
	fh_init(&resp->fh, NFS_FHSIZE);
	nfserr = nfsd_create(rqstp, &argp->fh, argp->name, argp->len,
				    &argp->attrs, S_IFDIR, 0, &resp->fh);
	fh_put(&argp->fh);
	return nfserr;
}

/*
 * Remove a directory
 */
static int
nfsd_proc_rmdir(struct svc_rqst *rqstp, struct nfsd_diropargs *argp,
				 	void		      *resp)
{
	int	nfserr;

	dprintk("nfsd: RMDIR    %s %s\n", SVCFH_fmt(&argp->fh), argp->name);

	nfserr = nfsd_unlink(rqstp, &argp->fh, S_IFDIR, argp->name, argp->len);
	fh_put(&argp->fh);
	return nfserr;
}

/*
 * Read a portion of a directory.
 */
static int
nfsd_proc_readdir(struct svc_rqst *rqstp, struct nfsd_readdirargs *argp,
					  struct nfsd_readdirres  *resp)
{
	u32 *		buffer;
	int		nfserr, count;

	dprintk("nfsd: READDIR  %s %d bytes at %d\n",
		SVCFH_fmt(&argp->fh),		
		argp->count, argp->cookie);

	/* Reserve buffer space for status */
	svcbuf_reserve(&rqstp->rq_resbuf, &buffer, &count, 1);

	/* Shrink to the client read size */
	if (count > (argp->count >> 2))
		count = argp->count >> 2;

	/* Make sure we've room for the NULL ptr & eof flag */
	count -= 2;
	if (count < 0)
		count = 0;

	/* Read directory and encode entries on the fly */
	nfserr = nfsd_readdir(rqstp, &argp->fh, (loff_t) argp->cookie, 
			      nfssvc_encode_entry,
			      buffer, &count, NULL);
	resp->count = count;

	fh_put(&argp->fh);
	return nfserr;
}

/*
 * Get file system info
 */
static int
nfsd_proc_statfs(struct svc_rqst * rqstp, struct nfsd_fhandle   *argp,
					  struct nfsd_statfsres *resp)
{
	int	nfserr;

	dprintk("nfsd: STATFS   %s\n", SVCFH_fmt(&argp->fh));

	nfserr = nfsd_statfs(rqstp, &argp->fh, &resp->stats);
	fh_put(&argp->fh);
	return nfserr;
}

/*
 * NFSv2 Server procedures.
 * Only the results of non-idempotent operations are cached.
 */
#define nfsd_proc_none		NULL
#define nfssvc_release_none	NULL
struct nfsd_void { int dummy; };

#define PROC(name, argt, rest, relt, cache)	\
 { (svc_procfunc) nfsd_proc_##name,		\
   (kxdrproc_t) nfssvc_decode_##argt,		\
   (kxdrproc_t) nfssvc_encode_##rest,		\
   (kxdrproc_t) nfssvc_release_##relt,		\
   sizeof(struct nfsd_##argt),			\
   sizeof(struct nfsd_##rest),			\
   0,						\
   cache					\
 }
struct svc_procedure		nfsd_procedures2[18] = {
  PROC(null,	 void,		void,		none,		RC_NOCACHE),
  PROC(getattr,	 fhandle,	attrstat,	fhandle,	RC_NOCACHE),
  PROC(setattr,  sattrargs,	attrstat,	fhandle,	RC_REPLBUFF),
  PROC(none,	 void,		void,		none,		RC_NOCACHE),
  PROC(lookup,	 diropargs,	diropres,	fhandle,	RC_NOCACHE),
  PROC(readlink, fhandle,	readlinkres,	none,		RC_NOCACHE),
  PROC(read,	 readargs,	readres,	fhandle,	RC_NOCACHE),
  PROC(none,	 void,		void,		none,		RC_NOCACHE),
  PROC(write,	 writeargs,	attrstat,	fhandle,	RC_REPLBUFF),
  PROC(create,	 createargs,	diropres,	fhandle,	RC_REPLBUFF),
  PROC(remove,	 diropargs,	void,		none,		RC_REPLSTAT),
  PROC(rename,	 renameargs,	void,		none,		RC_REPLSTAT),
  PROC(link,	 linkargs,	void,		none,		RC_REPLSTAT),
  PROC(symlink,	 symlinkargs,	void,		none,		RC_REPLSTAT),
  PROC(mkdir,	 createargs,	diropres,	fhandle,	RC_REPLBUFF),
  PROC(rmdir,	 diropargs,	void,		none,		RC_REPLSTAT),
  PROC(readdir,	 readdirargs,	readdirres,	none,		RC_REPLBUFF),
  PROC(statfs,	 fhandle,	statfsres,	none,		RC_NOCACHE),
};


/*
 * Map errnos to NFS errnos.
 */
int
nfserrno (int errno)
{
	static struct {
		int	nfserr;
		int	syserr;
	} nfs_errtbl[] = {
		{ nfs_ok, 0 },
		{ nfserr_perm, -EPERM },
		{ nfserr_noent, -ENOENT },
		{ nfserr_io, -EIO },
		{ nfserr_nxio, -ENXIO },
		{ nfserr_acces, -EACCES },
		{ nfserr_exist, -EEXIST },
		{ nfserr_xdev, -EXDEV },
		{ nfserr_mlink, -EMLINK },
		{ nfserr_nodev, -ENODEV },
		{ nfserr_notdir, -ENOTDIR },
		{ nfserr_isdir, -EISDIR },
		{ nfserr_inval, -EINVAL },
		{ nfserr_fbig, -EFBIG },
		{ nfserr_nospc, -ENOSPC },
		{ nfserr_rofs, -EROFS },
		{ nfserr_mlink, -EMLINK },
		{ nfserr_nametoolong, -ENAMETOOLONG },
		{ nfserr_notempty, -ENOTEMPTY },
#ifdef EDQUOT
		{ nfserr_dquot, -EDQUOT },
#endif
		{ nfserr_stale, -ESTALE },
		{ -1, -EIO }
	};
	int	i;

	for (i = 0; nfs_errtbl[i].nfserr != -1; i++) {
		if (nfs_errtbl[i].syserr == errno)
			return nfs_errtbl[i].nfserr;
	}
	printk (KERN_INFO "nfsd: non-standard errno: %d\n", errno);
	return nfserr_io;
}

