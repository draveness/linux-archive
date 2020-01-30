/*
 * linux/fs/nfs/nfs3xdr.c
 *
 * XDR functions to encode/decode NFSv3 RPC arguments and results.
 *
 * Copyright (C) 1996, 1997 Olaf Kirch
 */

#include <linux/param.h>
#include <linux/time.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/utsname.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/pagemap.h>
#include <linux/proc_fs.h>
#include <linux/kdev_t.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs.h>
#include <linux/nfs3.h>
#include <linux/nfs_fs.h>

#define NFSDBG_FACILITY		NFSDBG_XDR

/* Mapping from NFS error code to "errno" error code. */
#define errno_NFSERR_IO		EIO

extern int			nfs_stat_to_errno(int);

/*
 * Declare the space requirements for NFS arguments and replies as
 * number of 32bit-words
 */
#define NFS3_fhandle_sz		(1+16)
#define NFS3_fh_sz		(NFS3_fhandle_sz)	/* shorthand */
#define NFS3_sattr_sz		(15)
#define NFS3_filename_sz	(1+(NFS3_MAXNAMLEN>>2))
#define NFS3_path_sz		(1+(NFS3_MAXPATHLEN>>2))
#define NFS3_fattr_sz		(21)
#define NFS3_wcc_attr_sz		(6)
#define NFS3_pre_op_attr_sz	(1+NFS3_wcc_attr_sz)
#define NFS3_post_op_attr_sz	(1+NFS3_fattr_sz)
#define NFS3_wcc_data_sz		(NFS3_pre_op_attr_sz+NFS3_post_op_attr_sz)
#define NFS3_fsstat_sz		
#define NFS3_fsinfo_sz		
#define NFS3_pathconf_sz		
#define NFS3_entry_sz		(NFS3_filename_sz+3)

#define NFS3_sattrargs_sz	(NFS3_fh_sz+NFS3_sattr_sz+3)
#define NFS3_diropargs_sz	(NFS3_fh_sz+NFS3_filename_sz)
#define NFS3_accessargs_sz	(NFS3_fh_sz+1)
#define NFS3_readlinkargs_sz	(NFS3_fh_sz)
#define NFS3_readargs_sz	(NFS3_fh_sz+3)
#define NFS3_writeargs_sz	(NFS3_fh_sz+5)
#define NFS3_createargs_sz	(NFS3_diropargs_sz+NFS3_sattr_sz)
#define NFS3_mkdirargs_sz	(NFS3_diropargs_sz+NFS3_sattr_sz)
#define NFS3_symlinkargs_sz	(NFS3_diropargs_sz+NFS3_path_sz+NFS3_sattr_sz)
#define NFS3_mknodargs_sz	(NFS3_diropargs_sz+2+NFS3_sattr_sz)
#define NFS3_renameargs_sz	(NFS3_diropargs_sz+NFS3_diropargs_sz)
#define NFS3_linkargs_sz		(NFS3_fh_sz+NFS3_diropargs_sz)
#define NFS3_readdirargs_sz	(NFS3_fh_sz+2)
#define NFS3_commitargs_sz	(NFS3_fh_sz+3)

#define NFS3_attrstat_sz	(1+NFS3_fattr_sz)
#define NFS3_wccstat_sz		(1+NFS3_wcc_data_sz)
#define NFS3_lookupres_sz	(1+NFS3_fh_sz+(2 * NFS3_post_op_attr_sz))
#define NFS3_accessres_sz	(1+NFS3_post_op_attr_sz+1)
#define NFS3_readlinkres_sz	(1+NFS3_post_op_attr_sz)
#define NFS3_readres_sz		(1+NFS3_post_op_attr_sz+3)
#define NFS3_writeres_sz	(1+NFS3_wcc_data_sz+4)
#define NFS3_createres_sz	(1+NFS3_fh_sz+NFS3_post_op_attr_sz+NFS3_wcc_data_sz)
#define NFS3_renameres_sz	(1+(2 * NFS3_wcc_data_sz))
#define NFS3_linkres_sz		(1+NFS3_post_op_attr_sz+NFS3_wcc_data_sz)
#define NFS3_readdirres_sz	(1+NFS3_post_op_attr_sz+2)
#define NFS3_fsstatres_sz	(1+NFS3_post_op_attr_sz+13)
#define NFS3_fsinfores_sz	(1+NFS3_post_op_attr_sz+12)
#define NFS3_pathconfres_sz	(1+NFS3_post_op_attr_sz+6)
#define NFS3_commitres_sz	(1+NFS3_wcc_data_sz+2)

/*
 * Map file type to S_IFMT bits
 */
static struct {
	unsigned int	mode;
	unsigned int	nfs2type;
} nfs_type2fmt[] = {
      { 0,		NFNON	},
      { S_IFREG,	NFREG	},
      { S_IFDIR,	NFDIR	},
      { S_IFBLK,	NFBLK	},
      { S_IFCHR,	NFCHR	},
      { S_IFLNK,	NFLNK	},
      { S_IFSOCK,	NFSOCK	},
      { S_IFIFO,	NFFIFO	},
      { 0,		NFBAD	}
};

/*
 * Common NFS XDR functions as inlines
 */
static inline u32 *
xdr_encode_fhandle(u32 *p, struct nfs_fh *fh)
{
	return xdr_encode_array(p, fh->data, fh->size);
}

static inline u32 *
xdr_decode_fhandle(u32 *p, struct nfs_fh *fh)
{
	/*
	 * Zero all nonused bytes
	 */
	memset((u8 *)fh, 0, sizeof(*fh));
	if ((fh->size = ntohl(*p++)) <= NFS3_FHSIZE) {
		memcpy(fh->data, p, fh->size);
		return p + XDR_QUADLEN(fh->size);
	}
	return NULL;
}

/*
 * Encode/decode time.
 */
static inline u32 *
xdr_encode_time3(u32 *p, struct timespec *timep)
{
	*p++ = htonl(timep->tv_sec);
	*p++ = htonl(timep->tv_nsec);
	return p;
}

static inline u32 *
xdr_decode_time3(u32 *p, struct timespec *timep)
{
	timep->tv_sec = ntohl(*p++);
	timep->tv_nsec = ntohl(*p++);
	return p;
}

static u32 *
xdr_decode_fattr(u32 *p, struct nfs_fattr *fattr)
{
	unsigned int	type, major, minor;
	int		fmode;

	type = ntohl(*p++);
	if (type >= NF3BAD)
		type = NF3BAD;
	fmode = nfs_type2fmt[type].mode;
	fattr->type = nfs_type2fmt[type].nfs2type;
	fattr->mode = (ntohl(*p++) & ~S_IFMT) | fmode;
	fattr->nlink = ntohl(*p++);
	fattr->uid = ntohl(*p++);
	fattr->gid = ntohl(*p++);
	p = xdr_decode_hyper(p, &fattr->size);
	p = xdr_decode_hyper(p, &fattr->du.nfs3.used);

	/* Turn remote device info into Linux-specific dev_t */
	major = ntohl(*p++);
	minor = ntohl(*p++);
	fattr->rdev = MKDEV(major, minor);
	if (MAJOR(fattr->rdev) != major || MINOR(fattr->rdev) != minor)
		fattr->rdev = 0;

	p = xdr_decode_hyper(p, &fattr->fsid_u.nfs3);
	p = xdr_decode_hyper(p, &fattr->fileid);
	p = xdr_decode_time3(p, &fattr->atime);
	p = xdr_decode_time3(p, &fattr->mtime);
	p = xdr_decode_time3(p, &fattr->ctime);

	/* Update the mode bits */
	fattr->valid |= (NFS_ATTR_FATTR | NFS_ATTR_FATTR_V3);
	fattr->timestamp = jiffies;
	return p;
}

static inline u32 *
xdr_encode_sattr(u32 *p, struct iattr *attr)
{
	if (attr->ia_valid & ATTR_MODE) {
		*p++ = xdr_one;
		*p++ = htonl(attr->ia_mode);
	} else {
		*p++ = xdr_zero;
	}
	if (attr->ia_valid & ATTR_UID) {
		*p++ = xdr_one;
		*p++ = htonl(attr->ia_uid);
	} else {
		*p++ = xdr_zero;
	}
	if (attr->ia_valid & ATTR_GID) {
		*p++ = xdr_one;
		*p++ = htonl(attr->ia_gid);
	} else {
		*p++ = xdr_zero;
	}
	if (attr->ia_valid & ATTR_SIZE) {
		*p++ = xdr_one;
		p = xdr_encode_hyper(p, (__u64) attr->ia_size);
	} else {
		*p++ = xdr_zero;
	}
	if (attr->ia_valid & ATTR_ATIME_SET) {
		*p++ = xdr_two;
		p = xdr_encode_time3(p, &attr->ia_atime);
	} else if (attr->ia_valid & ATTR_ATIME) {
		*p++ = xdr_one;
	} else {
		*p++ = xdr_zero;
	}
	if (attr->ia_valid & ATTR_MTIME_SET) {
		*p++ = xdr_two;
		p = xdr_encode_time3(p, &attr->ia_mtime);
	} else if (attr->ia_valid & ATTR_MTIME) {
		*p++ = xdr_one;
	} else {
		*p++ = xdr_zero;
	}
	return p;
}

static inline u32 *
xdr_decode_wcc_attr(u32 *p, struct nfs_fattr *fattr)
{
	p = xdr_decode_hyper(p, &fattr->pre_size);
	p = xdr_decode_time3(p, &fattr->pre_mtime);
	p = xdr_decode_time3(p, &fattr->pre_ctime);
	fattr->valid |= NFS_ATTR_WCC;
	return p;
}

static inline u32 *
xdr_decode_post_op_attr(u32 *p, struct nfs_fattr *fattr)
{
	if (*p++)
		p = xdr_decode_fattr(p, fattr);
	return p;
}

static inline u32 *
xdr_decode_pre_op_attr(u32 *p, struct nfs_fattr *fattr)
{
	if (*p++)
		return xdr_decode_wcc_attr(p, fattr);
	return p;
}


static inline u32 *
xdr_decode_wcc_data(u32 *p, struct nfs_fattr *fattr)
{
	p = xdr_decode_pre_op_attr(p, fattr);
	return xdr_decode_post_op_attr(p, fattr);
}

/*
 * NFS encode functions
 */

/*
 * Encode file handle argument
 */
static int
nfs3_xdr_fhandle(struct rpc_rqst *req, u32 *p, struct nfs_fh *fh)
{
	p = xdr_encode_fhandle(p, fh);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode SETATTR arguments
 */
static int
nfs3_xdr_sattrargs(struct rpc_rqst *req, u32 *p, struct nfs3_sattrargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_sattr(p, args->sattr);
	*p++ = htonl(args->guard);
	if (args->guard)
		p = xdr_encode_time3(p, &args->guardtime);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode directory ops argument
 */
static int
nfs3_xdr_diropargs(struct rpc_rqst *req, u32 *p, struct nfs3_diropargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_array(p, args->name, args->len);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode access() argument
 */
static int
nfs3_xdr_accessargs(struct rpc_rqst *req, u32 *p, struct nfs3_accessargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	*p++ = htonl(args->access);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Arguments to a READ call. Since we read data directly into the page
 * cache, we also set up the reply iovec here so that iov[1] points
 * exactly to the page we want to fetch.
 */
static int
nfs3_xdr_readargs(struct rpc_rqst *req, u32 *p, struct nfs_readargs *args)
{
	struct rpc_auth	*auth = req->rq_task->tk_auth;
	unsigned int replen;
	u32 count = args->count;

	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_hyper(p, args->offset);
	*p++ = htonl(count);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* Inline the page array */
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS3_readres_sz) << 2;
	xdr_inline_pages(&req->rq_rcv_buf, replen,
			 args->pages, args->pgbase, count);
	return 0;
}

/*
 * Write arguments. Splice the buffer to be written into the iovec.
 */
static int
nfs3_xdr_writeargs(struct rpc_rqst *req, u32 *p, struct nfs_writeargs *args)
{
	struct xdr_buf *sndbuf = &req->rq_snd_buf;
	u32 count = args->count;

	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_hyper(p, args->offset);
	*p++ = htonl(count);
	*p++ = htonl(args->stable);
	*p++ = htonl(count);
	sndbuf->len = xdr_adjust_iovec(sndbuf->head, p);

	/* Copy the page array */
	xdr_encode_pages(sndbuf, args->pages, args->pgbase, count);
	return 0;
}

/*
 * Encode CREATE arguments
 */
static int
nfs3_xdr_createargs(struct rpc_rqst *req, u32 *p, struct nfs3_createargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_array(p, args->name, args->len);

	*p++ = htonl(args->createmode);
	if (args->createmode == NFS3_CREATE_EXCLUSIVE) {
		*p++ = args->verifier[0];
		*p++ = args->verifier[1];
	} else
		p = xdr_encode_sattr(p, args->sattr);

	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode MKDIR arguments
 */
static int
nfs3_xdr_mkdirargs(struct rpc_rqst *req, u32 *p, struct nfs3_mkdirargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_array(p, args->name, args->len);
	p = xdr_encode_sattr(p, args->sattr);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode SYMLINK arguments
 */
static int
nfs3_xdr_symlinkargs(struct rpc_rqst *req, u32 *p, struct nfs3_symlinkargs *args)
{
	p = xdr_encode_fhandle(p, args->fromfh);
	p = xdr_encode_array(p, args->fromname, args->fromlen);
	p = xdr_encode_sattr(p, args->sattr);
	p = xdr_encode_array(p, args->topath, args->tolen);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode MKNOD arguments
 */
static int
nfs3_xdr_mknodargs(struct rpc_rqst *req, u32 *p, struct nfs3_mknodargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_array(p, args->name, args->len);
	*p++ = htonl(args->type);
	p = xdr_encode_sattr(p, args->sattr);
	if (args->type == NF3CHR || args->type == NF3BLK) {
		*p++ = htonl(MAJOR(args->rdev));
		*p++ = htonl(MINOR(args->rdev));
	}

	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode RENAME arguments
 */
static int
nfs3_xdr_renameargs(struct rpc_rqst *req, u32 *p, struct nfs3_renameargs *args)
{
	p = xdr_encode_fhandle(p, args->fromfh);
	p = xdr_encode_array(p, args->fromname, args->fromlen);
	p = xdr_encode_fhandle(p, args->tofh);
	p = xdr_encode_array(p, args->toname, args->tolen);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode LINK arguments
 */
static int
nfs3_xdr_linkargs(struct rpc_rqst *req, u32 *p, struct nfs3_linkargs *args)
{
	p = xdr_encode_fhandle(p, args->fromfh);
	p = xdr_encode_fhandle(p, args->tofh);
	p = xdr_encode_array(p, args->toname, args->tolen);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode arguments to readdir call
 */
static int
nfs3_xdr_readdirargs(struct rpc_rqst *req, u32 *p, struct nfs3_readdirargs *args)
{
	struct rpc_auth	*auth = req->rq_task->tk_auth;
	unsigned int replen;
	u32 count = args->count;

	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_hyper(p, args->cookie);
	*p++ = args->verf[0];
	*p++ = args->verf[1];
	if (args->plus) {
		/* readdirplus: need dircount + buffer size.
		 * We just make sure we make dircount big enough */
		*p++ = htonl(count >> 3);
	}
	*p++ = htonl(count);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* Inline the page array */
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS3_readdirres_sz) << 2;
	xdr_inline_pages(&req->rq_rcv_buf, replen, args->pages, 0, count);
	return 0;
}

/*
 * Decode the result of a readdir call.
 * We just check for syntactical correctness.
 */
static int
nfs3_xdr_readdirres(struct rpc_rqst *req, u32 *p, struct nfs3_readdirres *res)
{
	struct xdr_buf *rcvbuf = &req->rq_rcv_buf;
	struct kvec *iov = rcvbuf->head;
	struct page **page;
	int hdrlen, recvd;
	int status, nr;
	unsigned int len, pglen;
	u32 *entry, *end, *kaddr;

	status = ntohl(*p++);
	/* Decode post_op_attrs */
	p = xdr_decode_post_op_attr(p, res->dir_attr);
	if (status)
		return -nfs_stat_to_errno(status);
	/* Decode verifier cookie */
	if (res->verf) {
		res->verf[0] = *p++;
		res->verf[1] = *p++;
	} else {
		p += 2;
	}

	hdrlen = (u8 *) p - (u8 *) iov->iov_base;
	if (iov->iov_len < hdrlen) {
		printk(KERN_WARNING "NFS: READDIR reply header overflowed:"
				"length %d > %Zu\n", hdrlen, iov->iov_len);
		return -errno_NFSERR_IO;
	} else if (iov->iov_len != hdrlen) {
		dprintk("NFS: READDIR header is short. iovec will be shifted.\n");
		xdr_shift_buf(rcvbuf, iov->iov_len - hdrlen);
	}

	pglen = rcvbuf->page_len;
	recvd = rcvbuf->len - hdrlen;
	if (pglen > recvd)
		pglen = recvd;
	page = rcvbuf->pages;
	kaddr = p = (u32 *)kmap_atomic(*page, KM_USER0);
	end = (u32 *)((char *)p + pglen);
	entry = p;
	for (nr = 0; *p++; nr++) {
		if (p + 3 > end)
			goto short_pkt;
		p += 2;				/* inode # */
		len = ntohl(*p++);		/* string length */
		p += XDR_QUADLEN(len) + 2;	/* name + cookie */
		if (len > NFS3_MAXNAMLEN) {
			printk(KERN_WARNING "NFS: giant filename in readdir (len %x)!\n",
						len);
			goto err_unmap;
		}

		if (res->plus) {
			/* post_op_attr */
			if (p + 2 > end)
				goto short_pkt;
			if (*p++) {
				p += 21;
				if (p + 1 > end)
					goto short_pkt;
			}
			/* post_op_fh3 */
			if (*p++) {
				if (p + 1 > end)
					goto short_pkt;
				len = ntohl(*p++);
				if (len > NFS3_FHSIZE) {
					printk(KERN_WARNING "NFS: giant filehandle in "
						"readdir (len %x)!\n", len);
					goto err_unmap;
				}
				p += XDR_QUADLEN(len);
			}
		}

		if (p + 2 > end)
			goto short_pkt;
		entry = p;
	}
	if (!nr && (entry[0] != 0 || entry[1] == 0))
		goto short_pkt;
 out:
	kunmap_atomic(kaddr, KM_USER0);
	return nr;
 short_pkt:
	entry[0] = entry[1] = 0;
	/* truncate listing ? */
	if (!nr) {
		printk(KERN_NOTICE "NFS: readdir reply truncated!\n");
		entry[1] = 1;
	}
	goto out;
err_unmap:
	nr = -errno_NFSERR_IO;
	goto out;
}

u32 *
nfs3_decode_dirent(u32 *p, struct nfs_entry *entry, int plus)
{
	struct nfs_entry old = *entry;

	if (!*p++) {
		if (!*p)
			return ERR_PTR(-EAGAIN);
		entry->eof = 1;
		return ERR_PTR(-EBADCOOKIE);
	}

	p = xdr_decode_hyper(p, &entry->ino);
	entry->len  = ntohl(*p++);
	entry->name = (const char *) p;
	p += XDR_QUADLEN(entry->len);
	entry->prev_cookie = entry->cookie;
	p = xdr_decode_hyper(p, &entry->cookie);

	if (plus) {
		entry->fattr->valid = 0;
		p = xdr_decode_post_op_attr(p, entry->fattr);
		/* In fact, a post_op_fh3: */
		if (*p++) {
			p = xdr_decode_fhandle(p, entry->fh);
			/* Ugh -- server reply was truncated */
			if (p == NULL) {
				dprintk("NFS: FH truncated\n");
				*entry = old;
				return ERR_PTR(-EAGAIN);
			}
		} else
			memset((u8*)(entry->fh), 0, sizeof(*entry->fh));
	}

	entry->eof = !p[0] && p[1];
	return p;
}

/*
 * Encode COMMIT arguments
 */
static int
nfs3_xdr_commitargs(struct rpc_rqst *req, u32 *p, struct nfs_writeargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_hyper(p, args->offset);
	*p++ = htonl(args->count);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * NFS XDR decode functions
 */

/*
 * Decode attrstat reply.
 */
static int
nfs3_xdr_attrstat(struct rpc_rqst *req, u32 *p, struct nfs_fattr *fattr)
{
	int	status;

	if ((status = ntohl(*p++)))
		return -nfs_stat_to_errno(status);
	xdr_decode_fattr(p, fattr);
	return 0;
}

/*
 * Decode status+wcc_data reply
 * SATTR, REMOVE, RMDIR
 */
static int
nfs3_xdr_wccstat(struct rpc_rqst *req, u32 *p, struct nfs_fattr *fattr)
{
	int	status;

	if ((status = ntohl(*p++)))
		status = -nfs_stat_to_errno(status);
	xdr_decode_wcc_data(p, fattr);
	return status;
}

/*
 * Decode LOOKUP reply
 */
static int
nfs3_xdr_lookupres(struct rpc_rqst *req, u32 *p, struct nfs3_diropres *res)
{
	int	status;

	if ((status = ntohl(*p++))) {
		status = -nfs_stat_to_errno(status);
	} else {
		if (!(p = xdr_decode_fhandle(p, res->fh)))
			return -errno_NFSERR_IO;
		p = xdr_decode_post_op_attr(p, res->fattr);
	}
	xdr_decode_post_op_attr(p, res->dir_attr);
	return status;
}

/*
 * Decode ACCESS reply
 */
static int
nfs3_xdr_accessres(struct rpc_rqst *req, u32 *p, struct nfs3_accessres *res)
{
	int	status = ntohl(*p++);

	p = xdr_decode_post_op_attr(p, res->fattr);
	if (status)
		return -nfs_stat_to_errno(status);
	res->access = ntohl(*p++);
	return 0;
}

static int
nfs3_xdr_readlinkargs(struct rpc_rqst *req, u32 *p, struct nfs3_readlinkargs *args)
{
	struct rpc_auth *auth = req->rq_task->tk_auth;
	unsigned int count = args->count - 5;
	unsigned int replen;

	p = xdr_encode_fhandle(p, args->fh);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* Inline the page array */
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS3_readlinkres_sz) << 2;
	xdr_inline_pages(&req->rq_rcv_buf, replen, args->pages, 0, count);
	return 0;
}

/*
 * Decode READLINK reply
 */
static int
nfs3_xdr_readlinkres(struct rpc_rqst *req, u32 *p, struct nfs_fattr *fattr)
{
	struct xdr_buf *rcvbuf = &req->rq_rcv_buf;
	struct kvec *iov = rcvbuf->head;
	unsigned int hdrlen;
	u32	*strlen, len;
	char	*string;
	int	status;

	status = ntohl(*p++);
	p = xdr_decode_post_op_attr(p, fattr);

	if (status != 0)
		return -nfs_stat_to_errno(status);

	hdrlen = (u8 *) p - (u8 *) iov->iov_base;
	if (iov->iov_len > hdrlen) {
		dprintk("NFS: READLINK header is short. iovec will be shifted.\n");
		xdr_shift_buf(rcvbuf, iov->iov_len - hdrlen);
	}

	strlen = (u32*)kmap_atomic(rcvbuf->pages[0], KM_USER0);
	/* Convert length of symlink */
	len = ntohl(*strlen);
	if (len > rcvbuf->page_len) {
		dprintk(KERN_WARNING "nfs: server returned giant symlink!\n");
		kunmap_atomic(strlen, KM_USER0);
		return -ENAMETOOLONG;
	}
	*strlen = len;
	/* NULL terminate the string we got */
	string = (char *)(strlen + 1);
	string[len] = '\0';
	kunmap_atomic(strlen, KM_USER0);
	return 0;
}

/*
 * Decode READ reply
 */
static int
nfs3_xdr_readres(struct rpc_rqst *req, u32 *p, struct nfs_readres *res)
{
	struct kvec *iov = req->rq_rcv_buf.head;
	int	status, count, ocount, recvd, hdrlen;

	status = ntohl(*p++);
	p = xdr_decode_post_op_attr(p, res->fattr);

	if (status != 0)
		return -nfs_stat_to_errno(status);

	/* Decode reply could and EOF flag. NFSv3 is somewhat redundant
	 * in that it puts the count both in the res struct and in the
	 * opaque data count. */
	count    = ntohl(*p++);
	res->eof = ntohl(*p++);
	ocount   = ntohl(*p++);

	if (ocount != count) {
		printk(KERN_WARNING "NFS: READ count doesn't match RPC opaque count.\n");
		return -errno_NFSERR_IO;
	}

	hdrlen = (u8 *) p - (u8 *) iov->iov_base;
	if (iov->iov_len < hdrlen) {
		printk(KERN_WARNING "NFS: READ reply header overflowed:"
				"length %d > %Zu\n", hdrlen, iov->iov_len);
       		return -errno_NFSERR_IO;
	} else if (iov->iov_len != hdrlen) {
		dprintk("NFS: READ header is short. iovec will be shifted.\n");
		xdr_shift_buf(&req->rq_rcv_buf, iov->iov_len - hdrlen);
	}

	recvd = req->rq_rcv_buf.len - hdrlen;
	if (count > recvd) {
		printk(KERN_WARNING "NFS: server cheating in read reply: "
			"count %d > recvd %d\n", count, recvd);
		count = recvd;
		res->eof = 0;
	}

	if (count < res->count)
		res->count = count;

	return count;
}

/*
 * Decode WRITE response
 */
static int
nfs3_xdr_writeres(struct rpc_rqst *req, u32 *p, struct nfs_writeres *res)
{
	int	status;

	status = ntohl(*p++);
	p = xdr_decode_wcc_data(p, res->fattr);

	if (status != 0)
		return -nfs_stat_to_errno(status);

	res->count = ntohl(*p++);
	res->verf->committed = (enum nfs3_stable_how)ntohl(*p++);
	res->verf->verifier[0] = *p++;
	res->verf->verifier[1] = *p++;

	return res->count;
}

/*
 * Decode a CREATE response
 */
static int
nfs3_xdr_createres(struct rpc_rqst *req, u32 *p, struct nfs3_diropres *res)
{
	int	status;

	status = ntohl(*p++);
	if (status == 0) {
		if (*p++) {
			if (!(p = xdr_decode_fhandle(p, res->fh)))
				return -errno_NFSERR_IO;
			p = xdr_decode_post_op_attr(p, res->fattr);
		} else {
			memset(res->fh, 0, sizeof(*res->fh));
			/* Do decode post_op_attr but set it to NULL */
			p = xdr_decode_post_op_attr(p, res->fattr);
			res->fattr->valid = 0;
		}
	} else {
		status = -nfs_stat_to_errno(status);
	}
	p = xdr_decode_wcc_data(p, res->dir_attr);
	return status;
}

/*
 * Decode RENAME reply
 */
static int
nfs3_xdr_renameres(struct rpc_rqst *req, u32 *p, struct nfs3_renameres *res)
{
	int	status;

	if ((status = ntohl(*p++)) != 0)
		status = -nfs_stat_to_errno(status);
	p = xdr_decode_wcc_data(p, res->fromattr);
	p = xdr_decode_wcc_data(p, res->toattr);
	return status;
}

/*
 * Decode LINK reply
 */
static int
nfs3_xdr_linkres(struct rpc_rqst *req, u32 *p, struct nfs3_linkres *res)
{
	int	status;

	if ((status = ntohl(*p++)) != 0)
		status = -nfs_stat_to_errno(status);
	p = xdr_decode_post_op_attr(p, res->fattr);
	p = xdr_decode_wcc_data(p, res->dir_attr);
	return status;
}

/*
 * Decode FSSTAT reply
 */
static int
nfs3_xdr_fsstatres(struct rpc_rqst *req, u32 *p, struct nfs_fsstat *res)
{
	int		status;

	status = ntohl(*p++);

	p = xdr_decode_post_op_attr(p, res->fattr);
	if (status != 0)
		return -nfs_stat_to_errno(status);

	p = xdr_decode_hyper(p, &res->tbytes);
	p = xdr_decode_hyper(p, &res->fbytes);
	p = xdr_decode_hyper(p, &res->abytes);
	p = xdr_decode_hyper(p, &res->tfiles);
	p = xdr_decode_hyper(p, &res->ffiles);
	p = xdr_decode_hyper(p, &res->afiles);

	/* ignore invarsec */
	return 0;
}

/*
 * Decode FSINFO reply
 */
static int
nfs3_xdr_fsinfores(struct rpc_rqst *req, u32 *p, struct nfs_fsinfo *res)
{
	int		status;

	status = ntohl(*p++);

	p = xdr_decode_post_op_attr(p, res->fattr);
	if (status != 0)
		return -nfs_stat_to_errno(status);

	res->rtmax  = ntohl(*p++);
	res->rtpref = ntohl(*p++);
	res->rtmult = ntohl(*p++);
	res->wtmax  = ntohl(*p++);
	res->wtpref = ntohl(*p++);
	res->wtmult = ntohl(*p++);
	res->dtpref = ntohl(*p++);
	p = xdr_decode_hyper(p, &res->maxfilesize);

	/* ignore time_delta and properties */
	res->lease_time = 0;
	return 0;
}

/*
 * Decode PATHCONF reply
 */
static int
nfs3_xdr_pathconfres(struct rpc_rqst *req, u32 *p, struct nfs_pathconf *res)
{
	int		status;

	status = ntohl(*p++);

	p = xdr_decode_post_op_attr(p, res->fattr);
	if (status != 0)
		return -nfs_stat_to_errno(status);
	res->max_link = ntohl(*p++);
	res->max_namelen = ntohl(*p++);

	/* ignore remaining fields */
	return 0;
}

/*
 * Decode COMMIT reply
 */
static int
nfs3_xdr_commitres(struct rpc_rqst *req, u32 *p, struct nfs_writeres *res)
{
	int		status;

	status = ntohl(*p++);
	p = xdr_decode_wcc_data(p, res->fattr);
	if (status != 0)
		return -nfs_stat_to_errno(status);

	res->verf->verifier[0] = *p++;
	res->verf->verifier[1] = *p++;
	return 0;
}

#ifndef MAX
# define MAX(a, b)	(((a) > (b))? (a) : (b))
#endif

#define PROC(proc, argtype, restype, timer)				\
[NFS3PROC_##proc] = {							\
	.p_proc      = NFS3PROC_##proc,					\
	.p_encode    = (kxdrproc_t) nfs3_xdr_##argtype,			\
	.p_decode    = (kxdrproc_t) nfs3_xdr_##restype,			\
	.p_bufsiz    = MAX(NFS3_##argtype##_sz,NFS3_##restype##_sz) << 2,	\
	.p_timer     = timer						\
	}

struct rpc_procinfo	nfs3_procedures[] = {
  PROC(GETATTR,		fhandle,	attrstat, 1),
  PROC(SETATTR, 	sattrargs,	wccstat, 0),
  PROC(LOOKUP,		diropargs,	lookupres, 2),
  PROC(ACCESS,		accessargs,	accessres, 1),
  PROC(READLINK,	readlinkargs,	readlinkres, 3),
  PROC(READ,		readargs,	readres, 3),
  PROC(WRITE,		writeargs,	writeres, 4),
  PROC(CREATE,		createargs,	createres, 0),
  PROC(MKDIR,		mkdirargs,	createres, 0),
  PROC(SYMLINK,		symlinkargs,	createres, 0),
  PROC(MKNOD,		mknodargs,	createres, 0),
  PROC(REMOVE,		diropargs,	wccstat, 0),
  PROC(RMDIR,		diropargs,	wccstat, 0),
  PROC(RENAME,		renameargs,	renameres, 0),
  PROC(LINK,		linkargs,	linkres, 0),
  PROC(READDIR,		readdirargs,	readdirres, 3),
  PROC(READDIRPLUS,	readdirargs,	readdirres, 3),
  PROC(FSSTAT,		fhandle,	fsstatres, 0),
  PROC(FSINFO,  	fhandle,	fsinfores, 0),
  PROC(PATHCONF,	fhandle,	pathconfres, 0),
  PROC(COMMIT,		commitargs,	commitres, 5),
};

struct rpc_version		nfs_version3 = {
	.number			= 3,
	.nrprocs		= sizeof(nfs3_procedures)/sizeof(nfs3_procedures[0]),
	.procs			= nfs3_procedures
};

