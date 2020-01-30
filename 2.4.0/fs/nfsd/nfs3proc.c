/*
 * linux/fs/nfsd/nfs3proc.c
 *
 * Process version 3 NFS requests.
 *
 * Copyright (C) 1996, 1997, 1998 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/linkage.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/locks.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/version.h>
#include <linux/unistd.h>
#include <linux/malloc.h>
#include <linux/major.h>

#include <linux/sunrpc/svc.h>
#include <linux/nfsd/nfsd.h>
#include <linux/nfsd/cache.h>
#include <linux/nfsd/xdr3.h>
#include <linux/nfs3.h>

#define NFSDDBG_FACILITY		NFSDDBG_PROC

#define RETURN_STATUS(st)	{ resp->status = (st); return (st); }

static int	nfs3_ftypes[] = {
	0,			/* NF3NON */
	S_IFREG,		/* NF3REG */
	S_IFDIR,		/* NF3DIR */
	S_IFBLK,		/* NF3BLK */
	S_IFCHR,		/* NF3CHR */
	S_IFLNK,		/* NF3LNK */
	S_IFSOCK,		/* NF3SOCK */
	S_IFIFO,		/* NF3FIFO */
};

/*
 * Reserve room in the send buffer
 */
static void
svcbuf_reserve(struct svc_buf *buf, u32 **ptr, int *len, int nr)
{
	*ptr = buf->buf + nr;
	*len = buf->buflen - buf->len - nr;
}

/*
 * NULL call.
 */
static int
nfsd3_proc_null(struct svc_rqst *rqstp, void *argp, void *resp)
{
	return nfs_ok;
}

/*
 * Get a file's attributes
 */
static int
nfsd3_proc_getattr(struct svc_rqst *rqstp, struct nfsd_fhandle  *argp,
					   struct nfsd3_attrstat *resp)
{
	int	nfserr;

	dprintk("nfsd: GETATTR(3)  %s\n",
				SVCFH_fmt(&argp->fh));

	fh_copy(&resp->fh, &argp->fh);
	nfserr = fh_verify(rqstp, &resp->fh, 0, MAY_NOP);
	RETURN_STATUS(nfserr);
}

/*
 * Set a file's attributes
 */
static int
nfsd3_proc_setattr(struct svc_rqst *rqstp, struct nfsd3_sattrargs *argp,
					   struct nfsd3_attrstat  *resp)
{
	int	nfserr;

	dprintk("nfsd: SETATTR(3)  %s\n",
				SVCFH_fmt(&argp->fh));

	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_setattr(rqstp, &resp->fh, &argp->attrs);
	RETURN_STATUS(nfserr);
}

/*
 * Look up a path name component
 */
static int
nfsd3_proc_lookup(struct svc_rqst *rqstp, struct nfsd3_diropargs *argp,
					  struct nfsd3_diropres  *resp)
{
	int	nfserr;

	dprintk("nfsd: LOOKUP(3)   %s %s\n",
				SVCFH_fmt(&argp->fh),
				argp->name);

	fh_copy(&resp->dirfh, &argp->fh);
	fh_init(&resp->fh, NFS3_FHSIZE);

	nfserr = nfsd_lookup(rqstp, &resp->dirfh,
				    argp->name,
				    argp->len,
				    &resp->fh);
	RETURN_STATUS(nfserr);
}

/*
 * Check file access
 */
static int
nfsd3_proc_access(struct svc_rqst *rqstp, struct nfsd3_accessargs *argp,
					  struct nfsd3_accessres *resp)
{
	int	nfserr;

	dprintk("nfsd: ACCESS(3)   %s 0x%x\n",
				SVCFH_fmt(&argp->fh),
				argp->access);

	fh_copy(&resp->fh, &argp->fh);
	resp->access = argp->access;
	nfserr = nfsd_access(rqstp, &resp->fh, &resp->access);
	RETURN_STATUS(nfserr);
}

/*
 * Read a symlink.
 */
static int
nfsd3_proc_readlink(struct svc_rqst *rqstp, struct nfsd_fhandle     *argp,
					   struct nfsd3_readlinkres *resp)
{
	u32		*path;
	int		dummy, nfserr;

	dprintk("nfsd: READLINK(3) %s\n", SVCFH_fmt(&argp->fh));

	/* Reserve room for status, post_op_attr, and path length */
	svcbuf_reserve(&rqstp->rq_resbuf, &path, &dummy,
				1 + NFS3_POST_OP_ATTR_WORDS + 1);

	/* Read the symlink. */
	fh_copy(&resp->fh, &argp->fh);
	resp->len = NFS3_MAXPATHLEN;
	nfserr = nfsd_readlink(rqstp, &resp->fh, (char *) path, &resp->len);
	RETURN_STATUS(nfserr);
}

/*
 * Read a portion of a file.
 */
static int
nfsd3_proc_read(struct svc_rqst *rqstp, struct nfsd3_readargs *argp,
				        struct nfsd3_readres  *resp)
{
	u32 *	buffer;
	int	nfserr, avail;

	dprintk("nfsd: READ(3) %s %lu bytes at %lu\n",
				SVCFH_fmt(&argp->fh),
				(unsigned long) argp->count,
				(unsigned long) argp->offset);

	/* Obtain buffer pointer for payload.
	 * 1 (status) + 22 (post_op_attr) + 1 (count) + 1 (eof)
	 * + 1 (xdr opaque byte count) = 26
	 */
	svcbuf_reserve(&rqstp->rq_resbuf, &buffer, &avail,
			1 + NFS3_POST_OP_ATTR_WORDS + 3);

	resp->count = argp->count;
	if ((avail << 2) < resp->count)
		resp->count = avail << 2;

	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_read(rqstp, &resp->fh,
				  argp->offset,
				  (char *) buffer,
				  &resp->count);
	if (nfserr == 0) {
		struct inode	*inode = resp->fh.fh_dentry->d_inode;

		resp->eof = (argp->offset + resp->count) >= inode->i_size;
	}

	RETURN_STATUS(nfserr);
}

/*
 * Write data to a file
 */
static int
nfsd3_proc_write(struct svc_rqst *rqstp, struct nfsd3_writeargs *argp,
					 struct nfsd3_writeres  *resp)
{
	int	nfserr;

	dprintk("nfsd: WRITE(3)    %s %d bytes at %ld%s\n",
				SVCFH_fmt(&argp->fh),
				argp->len,
				(unsigned long) argp->offset,
				argp->stable? " stable" : "");

	fh_copy(&resp->fh, &argp->fh);
	resp->committed = argp->stable;
	nfserr = nfsd_write(rqstp, &resp->fh,
				   argp->offset,
				   argp->data,
				   argp->len,
				   &resp->committed);
	resp->count = argp->count;
	RETURN_STATUS(nfserr);
}

/*
 * With NFSv3, CREATE processing is a lot easier than with NFSv2.
 * At least in theory; we'll see how it fares in practice when the
 * first reports about SunOS compatibility problems start to pour in...
 */
static int
nfsd3_proc_create(struct svc_rqst *rqstp, struct nfsd3_createargs *argp,
					  struct nfsd3_diropres   *resp)
{
	svc_fh		*dirfhp, *newfhp = NULL;
	struct iattr	*attr;
	u32		nfserr;

	dprintk("nfsd: CREATE(3)   %s %s\n",
				SVCFH_fmt(&argp->fh),
				argp->name);

	dirfhp = fh_copy(&resp->dirfh, &argp->fh);
	newfhp = fh_init(&resp->fh, NFS3_FHSIZE);
	attr   = &argp->attrs;

	/* Get the directory inode */
	nfserr = fh_verify(rqstp, dirfhp, S_IFDIR, MAY_CREATE);
	if (nfserr)
		RETURN_STATUS(nfserr);

	/* Unfudge the mode bits */
	attr->ia_mode &= ~S_IFMT;
	if (!(attr->ia_valid & ATTR_MODE)) { 
		attr->ia_valid |= ATTR_MODE;
		attr->ia_mode = S_IFREG;
	} else {
		attr->ia_mode = (attr->ia_mode & ~S_IFMT) | S_IFREG;
	}

	/* Now create the file and set attributes */
	nfserr = nfsd_create_v3(rqstp, dirfhp, argp->name, argp->len,
				attr, newfhp,
				argp->createmode, argp->verf);

	RETURN_STATUS(nfserr);
}

/*
 * Make directory. This operation is not idempotent.
 */
static int
nfsd3_proc_mkdir(struct svc_rqst *rqstp, struct nfsd3_createargs *argp,
					 struct nfsd3_diropres   *resp)
{
	int	nfserr;

	dprintk("nfsd: MKDIR(3)    %s %s\n",
				SVCFH_fmt(&argp->fh),
				argp->name);

	argp->attrs.ia_valid &= ~ATTR_SIZE;
	fh_copy(&resp->dirfh, &argp->fh);
	fh_init(&resp->fh, NFS3_FHSIZE);
	nfserr = nfsd_create(rqstp, &resp->dirfh, argp->name, argp->len,
				    &argp->attrs, S_IFDIR, 0, &resp->fh);

	RETURN_STATUS(nfserr);
}

static int
nfsd3_proc_symlink(struct svc_rqst *rqstp, struct nfsd3_symlinkargs *argp,
					   struct nfsd3_diropres    *resp)
{
	int	nfserr;

	dprintk("nfsd: SYMLINK(3)  %s %s -> %s\n",
				SVCFH_fmt(&argp->ffh),
				argp->fname, argp->tname);

	fh_copy(&resp->dirfh, &argp->ffh);
	fh_init(&resp->fh, NFS3_FHSIZE);
	nfserr = nfsd_symlink(rqstp, &resp->dirfh, argp->fname, argp->flen,
						   argp->tname, argp->tlen,
						   &resp->fh, &argp->attrs);
	RETURN_STATUS(nfserr);
}

/*
 * Make socket/fifo/device.
 */
static int
nfsd3_proc_mknod(struct svc_rqst *rqstp, struct nfsd3_mknodargs *argp,
					 struct nfsd3_diropres  *resp)
{
	int	nfserr, type;
	dev_t	rdev = 0;

	dprintk("nfsd: MKNOD(3)    %s %s\n",
				SVCFH_fmt(&argp->fh),
				argp->name);

	fh_copy(&resp->dirfh, &argp->fh);
	fh_init(&resp->fh, NFS3_FHSIZE);

	if (argp->ftype == 0 || argp->ftype >= NF3BAD)
		RETURN_STATUS(nfserr_inval);
	if (argp->ftype == NF3CHR || argp->ftype == NF3BLK) {
		if ((argp->ftype == NF3CHR && argp->major >= MAX_CHRDEV)
		    || (argp->ftype == NF3BLK && argp->major >= MAX_BLKDEV)
		    || argp->minor > 0xFF)
			RETURN_STATUS(nfserr_inval);
		rdev = ((argp->major) << 8) | (argp->minor);
	} else
		if (argp->ftype != NF3SOCK && argp->ftype != NF3FIFO)
			RETURN_STATUS(nfserr_inval);

	type = nfs3_ftypes[argp->ftype];
	nfserr = nfsd_create(rqstp, &resp->dirfh, argp->name, argp->len,
				    &argp->attrs, type, rdev, &resp->fh);

	RETURN_STATUS(nfserr);
}

/*
 * Remove file/fifo/socket etc.
 */
static int
nfsd3_proc_remove(struct svc_rqst *rqstp, struct nfsd3_diropargs *argp,
					  struct nfsd3_attrstat  *resp)
{
	int	nfserr;

	dprintk("nfsd: REMOVE(3)   %s %s\n",
				SVCFH_fmt(&argp->fh),
				argp->name);

	/* Unlink. -S_IFDIR means file must not be a directory */
	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_unlink(rqstp, &resp->fh, -S_IFDIR, argp->name, argp->len);
	RETURN_STATUS(nfserr);
}

/*
 * Remove a directory
 */
static int
nfsd3_proc_rmdir(struct svc_rqst *rqstp, struct nfsd3_diropargs *argp,
					 struct nfsd3_attrstat  *resp)
{
	int	nfserr;

	dprintk("nfsd: RMDIR(3)    %s %s\n",
				SVCFH_fmt(&argp->fh),
				argp->name);

	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_unlink(rqstp, &resp->fh, S_IFDIR, argp->name, argp->len);
	RETURN_STATUS(nfserr);
}

static int
nfsd3_proc_rename(struct svc_rqst *rqstp, struct nfsd3_renameargs *argp,
					  struct nfsd3_renameres  *resp)
{
	int	nfserr;

	dprintk("nfsd: RENAME(3)   %s %s ->\n",
				SVCFH_fmt(&argp->ffh),
				argp->fname);
	dprintk("nfsd: -> %s %s\n",
				SVCFH_fmt(&argp->tfh),
				argp->tname);

	fh_copy(&resp->ffh, &argp->ffh);
	fh_copy(&resp->tfh, &argp->tfh);
	nfserr = nfsd_rename(rqstp, &resp->ffh, argp->fname, argp->flen,
				    &resp->tfh, argp->tname, argp->tlen);
	RETURN_STATUS(nfserr);
}

static int
nfsd3_proc_link(struct svc_rqst *rqstp, struct nfsd3_linkargs *argp,
					struct nfsd3_linkres  *resp)
{
	int	nfserr;

	dprintk("nfsd: LINK(3)     %s ->\n",
				SVCFH_fmt(&argp->ffh));
	dprintk("nfsd:   -> %s %s\n",
				SVCFH_fmt(&argp->tfh),
				argp->tname);

	fh_copy(&resp->fh,  &argp->ffh);
	fh_copy(&resp->tfh, &argp->tfh);
	nfserr = nfsd_link(rqstp, &resp->tfh, argp->tname, argp->tlen,
				  &resp->fh);
	RETURN_STATUS(nfserr);
}

/*
 * Read a portion of a directory.
 */
static int
nfsd3_proc_readdir(struct svc_rqst *rqstp, struct nfsd3_readdirargs *argp,
					   struct nfsd3_readdirres  *resp)
{
	u32 *		buffer;
	int		nfserr, count;
	unsigned int	want;

	dprintk("nfsd: READDIR(3)  %s %d bytes at %d\n",
				SVCFH_fmt(&argp->fh),
				argp->count, (u32) argp->cookie);

	/* Reserve buffer space for status, attributes and verifier */
	svcbuf_reserve(&rqstp->rq_resbuf, &buffer, &count,
				1 + NFS3_POST_OP_ATTR_WORDS + 2);

	/* Make sure we've room for the NULL ptr & eof flag, and shrink to
	 * client read size */
	if ((count -= 2) > (want = (argp->count >> 2) - 2))
		count = want;

	/* Read directory and encode entries on the fly */
	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_readdir(rqstp, &resp->fh, (loff_t) argp->cookie, 
					nfs3svc_encode_entry,
					buffer, &count, argp->verf);
	memcpy(resp->verf, argp->verf, 8);
	resp->count = count;

	RETURN_STATUS(nfserr);
}

/*
 * Read a portion of a directory, including file handles and attrs.
 * For now, we choose to ignore the dircount parameter.
 */
static int
nfsd3_proc_readdirplus(struct svc_rqst *rqstp, struct nfsd3_readdirargs *argp,
					       struct nfsd3_readdirres  *resp)
{
	u32 *	buffer;
	int	nfserr, count, want;

	dprintk("nfsd: READDIR+(3) %s %d bytes at %d\n",
				SVCFH_fmt(&argp->fh),
				argp->count, (u32) argp->cookie);

	/* Reserve buffer space for status, attributes and verifier */
	svcbuf_reserve(&rqstp->rq_resbuf, &buffer, &count,
				1 + NFS3_POST_OP_ATTR_WORDS + 2);

	/* Make sure we've room for the NULL ptr & eof flag, and shrink to
	 * client read size */
	if ((count -= 2) > (want = argp->count >> 2))
		count = want;

	/* Read directory and encode entries on the fly */
	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_readdir(rqstp, &resp->fh, (loff_t) argp->cookie, 
					nfs3svc_encode_entry_plus,
					buffer, &count, argp->verf);
	memcpy(resp->verf, argp->verf, 8);
	resp->count = count;

	RETURN_STATUS(nfserr);
}

/*
 * Get file system stats
 */
static int
nfsd3_proc_fsstat(struct svc_rqst * rqstp, struct nfsd_fhandle    *argp,
					   struct nfsd3_fsstatres *resp)
{
	int	nfserr;

	dprintk("nfsd: FSSTAT(3)   %s\n",
				SVCFH_fmt(&argp->fh));

	nfserr = nfsd_statfs(rqstp, &argp->fh, &resp->stats);
	fh_put(&argp->fh);
	RETURN_STATUS(nfserr);
}

/*
 * Get file system info
 */
static int
nfsd3_proc_fsinfo(struct svc_rqst * rqstp, struct nfsd_fhandle    *argp,
					   struct nfsd3_fsinfores *resp)
{
	int	nfserr;

	dprintk("nfsd: FSINFO(3)   %s\n",
				SVCFH_fmt(&argp->fh));

	resp->f_rtmax  = NFSSVC_MAXBLKSIZE;
	resp->f_rtpref = NFSSVC_MAXBLKSIZE;
	resp->f_rtmult = PAGE_SIZE;
	resp->f_wtmax  = NFSSVC_MAXBLKSIZE;
	resp->f_wtpref = NFSSVC_MAXBLKSIZE;
	resp->f_wtmult = PAGE_SIZE;
	resp->f_dtpref = PAGE_SIZE;
	resp->f_maxfilesize = ~(u32) 0;
	resp->f_properties = NFS3_FSF_DEFAULT;

	nfserr = fh_verify(rqstp, &argp->fh, 0, MAY_NOP);

	/* Check special features of the file system. May request
	 * different read/write sizes for file systems known to have
	 * problems with large blocks */
	if (nfserr == 0) {
		struct super_block *sb = argp->fh.fh_dentry->d_inode->i_sb;

		/* Note that we don't care for remote fs's here */
		if (sb->s_magic == 0x4d44 /* MSDOS_SUPER_MAGIC */) {
			resp->f_properties = NFS3_FSF_BILLYBOY;
		}
	}

	fh_put(&argp->fh);
	RETURN_STATUS(nfserr);
}

/*
 * Get pathconf info for the specified file
 */
static int
nfsd3_proc_pathconf(struct svc_rqst * rqstp, struct nfsd_fhandle      *argp,
					     struct nfsd3_pathconfres *resp)
{
	int	nfserr;

	dprintk("nfsd: PATHCONF(3) %s\n",
				SVCFH_fmt(&argp->fh));

	/* Set default pathconf */
	resp->p_link_max = 255;		/* at least */
	resp->p_name_max = 255;		/* at least */
	resp->p_no_trunc = 0;
	resp->p_chown_restricted = 1;
	resp->p_case_insensitive = 0;
	resp->p_case_preserving = 1;

	nfserr = fh_verify(rqstp, &argp->fh, 0, MAY_NOP);

	if (nfserr == 0) {
		struct super_block *sb = argp->fh.fh_dentry->d_inode->i_sb;

		/* Note that we don't care for remote fs's here */
		switch (sb->s_magic) {
		case EXT2_SUPER_MAGIC:
			resp->p_link_max = EXT2_LINK_MAX;
			resp->p_name_max = EXT2_NAME_LEN;
			break;
		case 0x4d44:	/* MSDOS_SUPER_MAGIC */
			resp->p_case_insensitive = 1;
			resp->p_case_preserving  = 0;
			break;
		}
	}

	fh_put(&argp->fh);
	RETURN_STATUS(nfserr);
}


/*
 * Commit a file (range) to stable storage.
 */
static int
nfsd3_proc_commit(struct svc_rqst * rqstp, struct nfsd3_commitargs *argp,
					   struct nfsd3_commitres  *resp)
{
	int	nfserr;

	dprintk("nfsd: COMMIT(3)   %s %d@%ld\n",
				SVCFH_fmt(&argp->fh),
				argp->count,
				(unsigned long) argp->offset);

	if (argp->offset > NFS_OFFSET_MAX)
		RETURN_STATUS(nfserr_inval);

	fh_copy(&resp->fh, &argp->fh);
	nfserr = nfsd_commit(rqstp, &resp->fh, argp->offset, argp->count);

	RETURN_STATUS(nfserr);
}


/*
 * NFSv3 Server procedures.
 * Only the results of non-idempotent operations are cached.
 */
#define nfs3svc_decode_voidargs		NULL
#define nfs3svc_release_void		NULL
#define nfs3svc_decode_fhandleargs	nfs3svc_decode_fhandle
#define nfs3svc_encode_attrstatres	nfs3svc_encode_attrstat
#define nfs3svc_encode_wccstatres	nfs3svc_encode_wccstat
#define nfsd3_mkdirargs			nfsd3_createargs
#define nfsd3_readdirplusargs		nfsd3_readdirargs
#define nfsd3_fhandleargs		nfsd_fhandle
#define nfsd3_fhandleres		nfsd3_attrstat
#define nfsd3_attrstatres		nfsd3_attrstat
#define nfsd3_wccstatres		nfsd3_attrstat
#define nfsd3_createres			nfsd3_diropres
#define nfsd3_voidres			nfsd3_voidargs
struct nfsd3_voidargs { int dummy; };

#define PROC(name, argt, rest, relt, cache)	\
 { (svc_procfunc) nfsd3_proc_##name,		\
   (kxdrproc_t) nfs3svc_decode_##argt##args,	\
   (kxdrproc_t) nfs3svc_encode_##rest##res,	\
   (kxdrproc_t) nfs3svc_release_##relt,		\
   sizeof(struct nfsd3_##argt##args),		\
   sizeof(struct nfsd3_##rest##res),		\
   0,						\
   cache					\
 }
struct svc_procedure		nfsd_procedures3[22] = {
  PROC(null,	 void,		void,		void,	 RC_NOCACHE),
  PROC(getattr,	 fhandle,	attrstat,	fhandle, RC_NOCACHE),
  PROC(setattr,  sattr,		wccstat,	fhandle,  RC_REPLBUFF),
  PROC(lookup,	 dirop,		dirop,		fhandle2, RC_NOCACHE),
  PROC(access,	 access,	access,		fhandle,  RC_NOCACHE),
  PROC(readlink, fhandle,	readlink,	fhandle,  RC_NOCACHE),
  PROC(read,	 read,		read,		fhandle, RC_NOCACHE),
  PROC(write,	 write,		write,		fhandle,  RC_REPLBUFF),
  PROC(create,	 create,	create,		fhandle2, RC_REPLBUFF),
  PROC(mkdir,	 mkdir,		create,		fhandle2, RC_REPLBUFF),
  PROC(symlink,	 symlink,	create,		fhandle2, RC_REPLBUFF),
  PROC(mknod,	 mknod,		create,		fhandle2, RC_REPLBUFF),
  PROC(remove,	 dirop,		wccstat,	fhandle,  RC_REPLBUFF),
  PROC(rmdir,	 dirop,		wccstat,	fhandle,  RC_REPLBUFF),
  PROC(rename,	 rename,	rename,		fhandle2, RC_REPLBUFF),
  PROC(link,	 link,		link,		fhandle2, RC_REPLBUFF),
  PROC(readdir,	 readdir,	readdir,	fhandle,  RC_NOCACHE),
  PROC(readdirplus,readdirplus,	readdir,	fhandle,  RC_NOCACHE),
  PROC(fsstat,	 fhandle,	fsstat,		void,     RC_NOCACHE),
  PROC(fsinfo,   fhandle,	fsinfo,		void,     RC_NOCACHE),
  PROC(pathconf, fhandle,	pathconf,	void,     RC_NOCACHE),
  PROC(commit,	 commit,	commit,		fhandle,  RC_NOCACHE)
};
