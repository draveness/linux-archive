/*
 * linux/fs/nfs/nfs2xdr.c
 *
 * XDR functions to encode/decode NFS RPC arguments and results.
 *
 * Copyright (C) 1992, 1993, 1994  Rick Sladkey
 * Copyright (C) 1996 Olaf Kirch
 * 04 Aug 1998  Ion Badulescu <ionut@cs.columbia.edu>
 * 		FIFO's need special handling in NFSv2
 */

#include <linux/param.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/utsname.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/pagemap.h>
#include <linux/proc_fs.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs.h>
#include <linux/nfs2.h>
#include <linux/nfs_fs.h>

/* Uncomment this to support servers requiring longword lengths */
#define NFS_PAD_WRITES 1

#define NFSDBG_FACILITY		NFSDBG_XDR
/* #define NFS_PARANOIA 1 */

extern int			nfs_stat_to_errno(int stat);

/* Mapping from NFS error code to "errno" error code. */
#define errno_NFSERR_IO		EIO

/*
 * Declare the space requirements for NFS arguments and replies as
 * number of 32bit-words
 */
#define NFS_fhandle_sz		8
#define NFS_sattr_sz		8
#define NFS_filename_sz		1+(NFS2_MAXNAMLEN>>2)
#define NFS_path_sz		1+(NFS2_MAXPATHLEN>>2)
#define NFS_fattr_sz		17
#define NFS_info_sz		5
#define NFS_entry_sz		NFS_filename_sz+3

#define NFS_enc_void_sz		0
#define NFS_diropargs_sz	NFS_fhandle_sz+NFS_filename_sz
#define NFS_sattrargs_sz	NFS_fhandle_sz+NFS_sattr_sz
#define NFS_readlinkargs_sz	NFS_fhandle_sz
#define NFS_readargs_sz		NFS_fhandle_sz+3
#define NFS_writeargs_sz	NFS_fhandle_sz+4
#define NFS_createargs_sz	NFS_diropargs_sz+NFS_sattr_sz
#define NFS_renameargs_sz	NFS_diropargs_sz+NFS_diropargs_sz
#define NFS_linkargs_sz		NFS_fhandle_sz+NFS_diropargs_sz
#define NFS_symlinkargs_sz	NFS_diropargs_sz+NFS_path_sz+NFS_sattr_sz
#define NFS_readdirargs_sz	NFS_fhandle_sz+2

#define NFS_dec_void_sz		0
#define NFS_attrstat_sz		1+NFS_fattr_sz
#define NFS_diropres_sz		1+NFS_fhandle_sz+NFS_fattr_sz
#define NFS_readlinkres_sz	1
#define NFS_readres_sz		1+NFS_fattr_sz+1
#define NFS_writeres_sz         NFS_attrstat_sz
#define NFS_stat_sz		1
#define NFS_readdirres_sz	1
#define NFS_statfsres_sz	1+NFS_info_sz

/*
 * Common NFS XDR functions as inlines
 */
static inline u32 *
xdr_encode_fhandle(u32 *p, struct nfs_fh *fhandle)
{
	memcpy(p, fhandle->data, NFS2_FHSIZE);
	return p + XDR_QUADLEN(NFS2_FHSIZE);
}

static inline u32 *
xdr_decode_fhandle(u32 *p, struct nfs_fh *fhandle)
{
	/* Zero handle first to allow comparisons */
	memset(fhandle, 0, sizeof(*fhandle));
	/* NFSv2 handles have a fixed length */
	fhandle->size = NFS2_FHSIZE;
	memcpy(fhandle->data, p, NFS2_FHSIZE);
	return p + XDR_QUADLEN(NFS2_FHSIZE);
}

static inline u32 *
xdr_decode_string2(u32 *p, char **string, unsigned int *len,
			unsigned int maxlen)
{
	*len = ntohl(*p++);
	if (*len > maxlen)
		return NULL;
	*string = (char *) p;
	return p + XDR_QUADLEN(*len);
}

static inline u32*
xdr_decode_time(u32 *p, u64 *timep)
{
	u64 tmp = (u64)ntohl(*p++) << 32;
	*timep = tmp + (u64)ntohl(*p++);
	return p;
}

static inline u32 *
xdr_decode_fattr(u32 *p, struct nfs_fattr *fattr)
{
	fattr->type = (enum nfs_ftype) ntohl(*p++);
	fattr->mode = ntohl(*p++);
	fattr->nlink = ntohl(*p++);
	fattr->uid = ntohl(*p++);
	fattr->gid = ntohl(*p++);
	fattr->size = ntohl(*p++);
	fattr->du.nfs2.blocksize = ntohl(*p++);
	fattr->rdev = ntohl(*p++);
	fattr->du.nfs2.blocks = ntohl(*p++);
	fattr->fsid = ntohl(*p++);
	fattr->fileid = ntohl(*p++);
	p = xdr_decode_time(p, &fattr->atime);
	p = xdr_decode_time(p, &fattr->mtime);
	p = xdr_decode_time(p, &fattr->ctime);
	fattr->valid |= NFS_ATTR_FATTR;
	if (fattr->type == NFCHR && fattr->rdev == NFS2_FIFO_DEV) {
		fattr->type = NFFIFO;
		fattr->mode = (fattr->mode & ~S_IFMT) | S_IFIFO;
		fattr->rdev = 0;
	}
	return p;
}

#define SATTR(p, attr, flag, field) \
        *p++ = (attr->ia_valid & flag) ? htonl(attr->field) : ~(u32) 0
static inline u32 *
xdr_encode_sattr(u32 *p, struct iattr *attr)
{
	SATTR(p, attr, ATTR_MODE, ia_mode);
	SATTR(p, attr, ATTR_UID, ia_uid);
	SATTR(p, attr, ATTR_GID, ia_gid);
	SATTR(p, attr, ATTR_SIZE, ia_size);

	if (attr->ia_valid & (ATTR_ATIME|ATTR_ATIME_SET)) {
		*p++ = htonl(attr->ia_atime);
		*p++ = 0;
	} else {
		*p++ = ~(u32) 0;
		*p++ = ~(u32) 0;
	}

	if (attr->ia_valid & (ATTR_MTIME|ATTR_MTIME_SET)) {
		*p++ = htonl(attr->ia_mtime);
		*p++ = 0;
	} else {
		*p++ = ~(u32) 0;	
		*p++ = ~(u32) 0;
	}
  	return p;
}
#undef SATTR

/*
 * NFS encode functions
 */
/*
 * Encode void argument
 */
static int
nfs_xdr_enc_void(struct rpc_rqst *req, u32 *p, void *dummy)
{
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode file handle argument
 * GETATTR, READLINK, STATFS
 */
static int
nfs_xdr_fhandle(struct rpc_rqst *req, u32 *p, struct nfs_fh *fh)
{
	p = xdr_encode_fhandle(p, fh);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode SETATTR arguments
 */
static int
nfs_xdr_sattrargs(struct rpc_rqst *req, u32 *p, struct nfs_sattrargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_sattr(p, args->sattr);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode directory ops argument
 * LOOKUP, REMOVE, RMDIR
 */
static int
nfs_xdr_diropargs(struct rpc_rqst *req, u32 *p, struct nfs_diropargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_array(p, args->name, args->len);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Arguments to a READ call. Since we read data directly into the page
 * cache, we also set up the reply iovec here so that iov[1] points
 * exactly to the page we want to fetch.
 */
static int
nfs_xdr_readargs(struct rpc_rqst *req, u32 *p, struct nfs_readargs *args)
{
	struct rpc_auth	*auth = req->rq_task->tk_auth;
	int		buflen, replen;
	unsigned int	nr;

	p = xdr_encode_fhandle(p, args->fh);
	*p++ = htonl(args->offset);
	*p++ = htonl(args->count);
	*p++ = htonl(args->count);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* Get the number of buffers in the receive iovec */
        nr = args->nriov;

        if (nr+2 > MAX_IOVEC) {
                printk(KERN_ERR "NFS: Bad number of iov's in xdr_readargs\n");
                return -EINVAL;
        }

	/* set up reply iovec */
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS_readres_sz) << 2;
	buflen = req->rq_rvec[0].iov_len;
	req->rq_rvec[0].iov_len  = replen;
        /* Copy the iovec */
        memcpy(req->rq_rvec + 1, args->iov, nr * sizeof(struct iovec));

	req->rq_rvec[nr+1].iov_base = (u8 *) req->rq_rvec[0].iov_base + replen;
	req->rq_rvec[nr+1].iov_len  = buflen - replen;
	req->rq_rlen = args->count + buflen;
	req->rq_rnr += nr+1;

	return 0;
}

/*
 * Decode READ reply
 */
static int
nfs_xdr_readres(struct rpc_rqst *req, u32 *p, struct nfs_readres *res)
{
	struct iovec *iov = req->rq_rvec;
	int	status, count, recvd, hdrlen;

	if ((status = ntohl(*p++)))
		return -nfs_stat_to_errno(status);
	p = xdr_decode_fattr(p, res->fattr);

	count = ntohl(*p++);
	hdrlen = (u8 *) p - (u8 *) iov->iov_base;
	recvd = req->rq_rlen - hdrlen;
	if (p != iov[req->rq_rnr-1].iov_base) {
		/* Unexpected reply header size. Punt.
		 * XXX: Move iovec contents to align data on page
		 * boundary and adjust RPC header size guess */
		printk(KERN_WARNING "NFS: Odd RPC header size in read reply: %d\n", hdrlen);
		return -errno_NFSERR_IO;
	}
	if (count > recvd) {
		printk(KERN_WARNING "NFS: server cheating in read reply: "
			"count %d > recvd %d\n", count, recvd);
		count = recvd;
	}

	dprintk("RPC:      readres OK count %d\n", count);
	if (count < res->count) {
		xdr_zero_iovec(iov+1, req->rq_rnr-2, res->count - count);
		res->count = count;
		res->eof = 1;  /* Silly NFSv3ism which can't be helped */
	} else
		res->eof = 0;

	return count;
}


/*
 * Write arguments. Splice the buffer to be written into the iovec.
 */
static int
nfs_xdr_writeargs(struct rpc_rqst *req, u32 *p, struct nfs_writeargs *args)
{
	unsigned int nr;
	u32 count = args->count;

	p = xdr_encode_fhandle(p, args->fh);
	*p++ = htonl(args->offset);
	*p++ = htonl(args->offset);
	*p++ = htonl(count);
	*p++ = htonl(count);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* Get the number of buffers in the send iovec */
	nr = args->nriov;

	if (nr+2 > MAX_IOVEC) {
                printk(KERN_ERR "NFS: Bad number of iov's in xdr_writeargs "
                        "(nr %d max %d)\n", nr, MAX_IOVEC);
                return -EINVAL;
        }

	/* Copy the iovec */
        memcpy(req->rq_svec + 1, args->iov, nr * sizeof(struct iovec));

#ifdef NFS_PAD_WRITES
	/*
	 * Some old servers require that the message length
	 * be a multiple of 4, so we pad it here if needed.
	 */
	if (count & 3) {
		struct iovec	*iov = req->rq_svec + nr + 1;
		int		pad = 4 - (count & 3);

		iov->iov_base = (void *) "\0\0\0";
		iov->iov_len  = pad;
		count += pad;
		nr++;
	}
#endif
	req->rq_slen += count;
	req->rq_snr += nr;

	return 0;
}

/*
 * Encode create arguments
 * CREATE, MKDIR
 */
static int
nfs_xdr_createargs(struct rpc_rqst *req, u32 *p, struct nfs_createargs *args)
{
	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_array(p, args->name, args->len);
	p = xdr_encode_sattr(p, args->sattr);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode RENAME arguments
 */
static int
nfs_xdr_renameargs(struct rpc_rqst *req, u32 *p, struct nfs_renameargs *args)
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
nfs_xdr_linkargs(struct rpc_rqst *req, u32 *p, struct nfs_linkargs *args)
{
	p = xdr_encode_fhandle(p, args->fromfh);
	p = xdr_encode_fhandle(p, args->tofh);
	p = xdr_encode_array(p, args->toname, args->tolen);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode SYMLINK arguments
 */
static int
nfs_xdr_symlinkargs(struct rpc_rqst *req, u32 *p, struct nfs_symlinkargs *args)
{
	p = xdr_encode_fhandle(p, args->fromfh);
	p = xdr_encode_array(p, args->fromname, args->fromlen);
	p = xdr_encode_array(p, args->topath, args->tolen);
	p = xdr_encode_sattr(p, args->sattr);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

/*
 * Encode arguments to readdir call
 */
static int
nfs_xdr_readdirargs(struct rpc_rqst *req, u32 *p, struct nfs_readdirargs *args)
{
	struct rpc_task	*task = req->rq_task;
	struct rpc_auth	*auth = task->tk_auth;
	u32		bufsiz = args->bufsiz;
	int		buflen, replen;

	/*
	 * Some servers (e.g. HP OS 9.5) seem to expect the buffer size
	 * to be in longwords ... check whether to convert the size.
	 */
	if (task->tk_client->cl_flags & NFS_CLNTF_BUFSIZE)
		bufsiz = bufsiz >> 2;

	p = xdr_encode_fhandle(p, args->fh);
	*p++ = htonl(args->cookie);
	*p++ = htonl(bufsiz); /* see above */
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* set up reply iovec */
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS_readdirres_sz) << 2;
	buflen = req->rq_rvec[0].iov_len;
	req->rq_rvec[0].iov_len  = replen;
	req->rq_rvec[1].iov_base = args->buffer;
	req->rq_rvec[1].iov_len  = args->bufsiz;
	req->rq_rvec[2].iov_base = (u8 *) req->rq_rvec[0].iov_base + replen;
	req->rq_rvec[2].iov_len  = buflen - replen;
	req->rq_rlen = buflen + args->bufsiz;
	req->rq_rnr += 2;

	return 0;
}

/*
 * Decode the result of a readdir call.
 * We're not really decoding anymore, we just leave the buffer untouched
 * and only check that it is syntactically correct.
 * The real decoding happens in nfs_decode_entry below, called directly
 * from nfs_readdir for each entry.
 */
static int
nfs_xdr_readdirres(struct rpc_rqst *req, u32 *p, struct nfs_readdirres *res)
{
	struct iovec		*iov = req->rq_rvec;
	int			 status, nr;
	u32			*end, *entry, len;

	if ((status = ntohl(*p++)))
		return -nfs_stat_to_errno(status);
	if ((void *) p != ((u8 *) iov->iov_base+iov->iov_len)) {
		/* Unexpected reply header size. Punt. */
		printk(KERN_WARNING "NFS: Odd RPC header size in readdirres reply\n");
		return -errno_NFSERR_IO;
	}

	/* Get start and end address of XDR data */
	p   = (u32 *) iov[1].iov_base;
	end = (u32 *) ((u8 *) p + iov[1].iov_len);

	/* Get start and end of dirent buffer */
	if (res->buffer != p) {
		printk(KERN_ERR "NFS: Bad result buffer in readdir\n");
		return -errno_NFSERR_IO;
	}

	for (nr = 0; *p++; nr++) {
		entry = p - 1;
		p++; /* fileid */
		len = ntohl(*p++);
		p += XDR_QUADLEN(len) + 1;	/* name plus cookie */
		if (len > NFS2_MAXNAMLEN) {
			printk(KERN_WARNING "NFS: giant filename in readdir (len 0x%x)!\n",
						len);
			return -errno_NFSERR_IO;
		}
		if (p + 2 > end) {
			printk(KERN_NOTICE
				"NFS: short packet in readdir reply!\n");
			entry[0] = entry[1] = 0;
			break;
		}
	}
	return nr;
}

u32 *
nfs_decode_dirent(u32 *p, struct nfs_entry *entry, int plus)
{
	if (!*p++) {
		if (!*p)
			return ERR_PTR(-EAGAIN);
		entry->eof = 1;
		return ERR_PTR(-EBADCOOKIE);
	}

	entry->ino	  = ntohl(*p++);
	entry->len	  = ntohl(*p++);
	entry->name	  = (const char *) p;
	p		 += XDR_QUADLEN(entry->len);
	entry->prev_cookie	  = entry->cookie;
	entry->cookie	  = ntohl(*p++);
	entry->eof	  = !p[0] && p[1];

	return p;
}

/*
 * NFS XDR decode functions
 */
/*
 * Decode void reply
 */
static int
nfs_xdr_dec_void(struct rpc_rqst *req, u32 *p, void *dummy)
{
	return 0;
}

/*
 * Decode simple status reply
 */
static int
nfs_xdr_stat(struct rpc_rqst *req, u32 *p, void *dummy)
{
	int	status;

	if ((status = ntohl(*p++)) != 0)
		status = -nfs_stat_to_errno(status);
	return status;
}

/*
 * Decode attrstat reply
 * GETATTR, SETATTR, WRITE
 */
static int
nfs_xdr_attrstat(struct rpc_rqst *req, u32 *p, struct nfs_fattr *fattr)
{
	int	status;

	if ((status = ntohl(*p++)))
		return -nfs_stat_to_errno(status);
	xdr_decode_fattr(p, fattr);
	return 0;
}

/*
 * Decode diropres reply
 * LOOKUP, CREATE, MKDIR
 */
static int
nfs_xdr_diropres(struct rpc_rqst *req, u32 *p, struct nfs_diropok *res)
{
	int	status;

	if ((status = ntohl(*p++)))
		return -nfs_stat_to_errno(status);
	p = xdr_decode_fhandle(p, res->fh);
	xdr_decode_fattr(p, res->fattr);
	return 0;
}

/*
 * Encode READLINK args
 */
static int
nfs_xdr_readlinkargs(struct rpc_rqst *req, u32 *p, struct nfs_readlinkargs *args)
{
	struct rpc_task *task = req->rq_task;
	struct rpc_auth *auth = task->tk_auth;
	int		buflen, replen;

	p = xdr_encode_fhandle(p, args->fh);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS_readlinkres_sz) << 2;
	buflen = req->rq_rvec[0].iov_len;
	req->rq_rvec[0].iov_len  = replen;
	req->rq_rvec[1].iov_base = args->buffer;
	req->rq_rvec[1].iov_len  = args->bufsiz;
	req->rq_rvec[2].iov_base = (u8 *) req->rq_rvec[0].iov_base + replen;
	req->rq_rvec[2].iov_len  = buflen - replen;
	req->rq_rlen = buflen + args->bufsiz;
	req->rq_rnr += 2;
	return 0;
}

/*
 * Decode READLINK reply
 */
static int
nfs_xdr_readlinkres(struct rpc_rqst *req, u32 *p, struct nfs_readlinkres *res)
{
	u32	*strlen;
	char	*string;
	int	status;
	unsigned int len;

	if ((status = ntohl(*p++)))
		return -nfs_stat_to_errno(status);
	strlen = (u32*)res->buffer;
	/* Convert length of symlink */
	len = ntohl(*strlen);
	if (len > res->bufsiz - 5)
		len = res->bufsiz - 5;
	*strlen = len;
	/* NULL terminate the string we got */
	string = (char *)(strlen + 1);
	string[len] = 0;
	return 0;
}

/*
 * Decode WRITE reply
 */
static int
nfs_xdr_writeres(struct rpc_rqst *req, u32 *p, struct nfs_writeres *res)
{
	res->verf->committed = NFS_FILE_SYNC;
	return nfs_xdr_attrstat(req, p, res->fattr);
}

/*
 * Decode STATFS reply
 */
static int
nfs_xdr_statfsres(struct rpc_rqst *req, u32 *p, struct nfs_fsinfo *res)
{
	int	status;
	u32	xfer_size;

	if ((status = ntohl(*p++)))
		return -nfs_stat_to_errno(status);

	/* For NFSv2, we more or less have to guess the preferred
	 * read/write/readdir sizes from the single 'transfer size'
	 * value.
	 */
	xfer_size = ntohl(*p++);	/* tsize */
	res->rtmax  = 8 * 1024;
	res->rtpref = xfer_size;
	res->rtmult = xfer_size;
	res->wtmax  = 8 * 1024;
	res->wtpref = xfer_size;
	res->wtmult = xfer_size;
	res->dtpref = PAGE_CACHE_SIZE;
	res->maxfilesize = 0x7FFFFFFF;	/* just a guess */
	res->bsize  = ntohl(*p++);

	res->tbytes = ntohl(*p++) * res->bsize;
	res->fbytes = ntohl(*p++) * res->bsize;
	res->abytes = ntohl(*p++) * res->bsize;
	res->tfiles = 0;
	res->ffiles = 0;
	res->afiles = 0;
	res->namelen = 0;

	return 0;
}

/*
 * We need to translate between nfs status return values and
 * the local errno values which may not be the same.
 */
static struct {
	int stat;
	int errno;
} nfs_errtbl[] = {
	{ NFS_OK,		0		},
	{ NFSERR_PERM,		EPERM		},
	{ NFSERR_NOENT,		ENOENT		},
	{ NFSERR_IO,		errno_NFSERR_IO	},
	{ NFSERR_NXIO,		ENXIO		},
/*	{ NFSERR_EAGAIN,	EAGAIN		}, */
	{ NFSERR_ACCES,		EACCES		},
	{ NFSERR_EXIST,		EEXIST		},
	{ NFSERR_XDEV,		EXDEV		},
	{ NFSERR_NODEV,		ENODEV		},
	{ NFSERR_NOTDIR,	ENOTDIR		},
	{ NFSERR_ISDIR,		EISDIR		},
	{ NFSERR_INVAL,		EINVAL		},
	{ NFSERR_FBIG,		EFBIG		},
	{ NFSERR_NOSPC,		ENOSPC		},
	{ NFSERR_ROFS,		EROFS		},
	{ NFSERR_MLINK,		EMLINK		},
	{ NFSERR_NAMETOOLONG,	ENAMETOOLONG	},
	{ NFSERR_NOTEMPTY,	ENOTEMPTY	},
	{ NFSERR_DQUOT,		EDQUOT		},
	{ NFSERR_STALE,		ESTALE		},
	{ NFSERR_REMOTE,	EREMOTE		},
#ifdef EWFLUSH
	{ NFSERR_WFLUSH,	EWFLUSH		},
#endif
	{ NFSERR_BADHANDLE,	EBADHANDLE	},
	{ NFSERR_NOT_SYNC,	ENOTSYNC	},
	{ NFSERR_BAD_COOKIE,	EBADCOOKIE	},
	{ NFSERR_NOTSUPP,	ENOTSUPP	},
	{ NFSERR_TOOSMALL,	ETOOSMALL	},
	{ NFSERR_SERVERFAULT,	ESERVERFAULT	},
	{ NFSERR_BADTYPE,	EBADTYPE	},
	{ NFSERR_JUKEBOX,	EJUKEBOX	},
	{ -1,			EIO		}
};

/*
 * Convert an NFS error code to a local one.
 * This one is used jointly by NFSv2 and NFSv3.
 */
int
nfs_stat_to_errno(int stat)
{
	int i;

	for (i = 0; nfs_errtbl[i].stat != -1; i++) {
		if (nfs_errtbl[i].stat == stat)
			return nfs_errtbl[i].errno;
	}
	printk(KERN_ERR "nfs_stat_to_errno: bad nfs status return value: %d\n", stat);
	return nfs_errtbl[i].errno;
}

#ifndef MAX
# define MAX(a, b)	(((a) > (b))? (a) : (b))
#endif

#define PROC(proc, argtype, restype)	\
    { "nfs_" #proc,					\
      (kxdrproc_t) nfs_xdr_##argtype,			\
      (kxdrproc_t) nfs_xdr_##restype,			\
      MAX(NFS_##argtype##_sz,NFS_##restype##_sz) << 2,	\
      0							\
    }

static struct rpc_procinfo	nfs_procedures[18] = {
    PROC(null,		enc_void,	dec_void),
    PROC(getattr,	fhandle,	attrstat),
    PROC(setattr,	sattrargs,	attrstat),
    PROC(root,		enc_void,	dec_void),
    PROC(lookup,	diropargs,	diropres),
    PROC(readlink,	readlinkargs,	readlinkres),
    PROC(read,		readargs,	readres),
    PROC(writecache,	enc_void,	dec_void),
    PROC(write,		writeargs,	writeres),
    PROC(create,	createargs,	diropres),
    PROC(remove,	diropargs,	stat),
    PROC(rename,	renameargs,	stat),
    PROC(link,		linkargs,	stat),
    PROC(symlink,	symlinkargs,	stat),
    PROC(mkdir,		createargs,	diropres),
    PROC(rmdir,		diropargs,	stat),
    PROC(readdir,	readdirargs,	readdirres),
    PROC(statfs,	fhandle,	statfsres),
};

struct rpc_version		nfs_version2 = {
	2,
	sizeof(nfs_procedures)/sizeof(nfs_procedures[0]),
	nfs_procedures
};
