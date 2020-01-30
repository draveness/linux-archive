/*
 * linux/fs/nfs/nfs3xdr.c
 *
 * XDR functions to encode/decode NFSv3 RPC arguments and results.
 *
 * Copyright (C) 1996, 1997 Olaf Kirch
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
#include <linux/kdev_t.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs.h>
#include <linux/nfs3.h>
#include <linux/nfs_fs.h>

/* Uncomment this to support servers requiring longword lengths */
#define NFS_PAD_WRITES		1

#define NFSDBG_FACILITY		NFSDBG_XDR

/* Mapping from NFS error code to "errno" error code. */
#define errno_NFSERR_IO		EIO

extern int			nfs_stat_to_errno(int);

/*
 * Declare the space requirements for NFS arguments and replies as
 * number of 32bit-words
 */
#define NFS3_fhandle_sz		1+16
#define NFS3_fh_sz		NFS3_fhandle_sz	/* shorthand */
#define NFS3_sattr_sz		15
#define NFS3_filename_sz	1+(NFS3_MAXNAMLEN>>2)
#define NFS3_path_sz		1+(NFS3_MAXPATHLEN>>2)
#define NFS3_fattr_sz		21
#define NFS3_wcc_attr_sz		6
#define NFS3_pre_op_attr_sz	1+NFS3_wcc_attr_sz
#define NFS3_post_op_attr_sz	1+NFS3_fattr_sz
#define NFS3_wcc_data_sz		NFS3_pre_op_attr_sz+NFS3_post_op_attr_sz
#define NFS3_fsstat_sz		
#define NFS3_fsinfo_sz		
#define NFS3_pathconf_sz		
#define NFS3_entry_sz		NFS3_filename_sz+3

#define NFS3_enc_void_sz	0
#define NFS3_sattrargs_sz	NFS3_fh_sz+NFS3_sattr_sz+3
#define NFS3_diropargs_sz	NFS3_fh_sz+NFS3_filename_sz
#define NFS3_accessargs_sz	NFS3_fh_sz+1
#define NFS3_readlinkargs_sz	NFS3_fh_sz
#define NFS3_readargs_sz	NFS3_fh_sz+3
#define NFS3_writeargs_sz	NFS3_fh_sz+5
#define NFS3_createargs_sz	NFS3_diropargs_sz+NFS3_sattr_sz
#define NFS3_mkdirargs_sz	NFS3_diropargs_sz+NFS3_sattr_sz
#define NFS3_symlinkargs_sz	NFS3_diropargs_sz+NFS3_path_sz+NFS3_sattr_sz
#define NFS3_mknodargs_sz	NFS3_diropargs_sz+2+NFS3_sattr_sz
#define NFS3_renameargs_sz	NFS3_diropargs_sz+NFS3_diropargs_sz
#define NFS3_linkargs_sz		NFS3_fh_sz+NFS3_diropargs_sz
#define NFS3_readdirargs_sz	NFS3_fh_sz+2
#define NFS3_commitargs_sz	NFS3_fh_sz+3

#define NFS3_dec_void_sz	0
#define NFS3_attrstat_sz	1+NFS3_fattr_sz
#define NFS3_wccstat_sz		1+NFS3_wcc_data_sz
#define NFS3_lookupres_sz	1+NFS3_fh_sz+(2 * NFS3_post_op_attr_sz)
#define NFS3_accessres_sz	1+NFS3_post_op_attr_sz+1
#define NFS3_readlinkres_sz	1+NFS3_post_op_attr_sz
#define NFS3_readres_sz		1+NFS3_post_op_attr_sz+3
#define NFS3_writeres_sz	1+NFS3_wcc_data_sz+4
#define NFS3_createres_sz	1+NFS3_fh_sz+NFS3_post_op_attr_sz+NFS3_wcc_data_sz
#define NFS3_renameres_sz	1+(2 * NFS3_wcc_data_sz)
#define NFS3_linkres_sz		1+NFS3_post_op_attr_sz+NFS3_wcc_data_sz
#define NFS3_readdirres_sz	1+NFS3_post_op_attr_sz+2
#define NFS3_fsstatres_sz	1+NFS3_post_op_attr_sz+13
#define NFS3_fsinfores_sz	1+NFS3_post_op_attr_sz+12
#define NFS3_pathconfres_sz	1+NFS3_post_op_attr_sz+6
#define NFS3_commitres_sz	1+NFS3_wcc_data_sz+2

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
	*p++ = htonl(fh->size);
	memcpy(p, fh->data, fh->size);
	return p + XDR_QUADLEN(fh->size);
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
 * Since the VFS doesn't care for fractional times, we ignore the
 * nanosecond field.
 */
static inline u32 *
xdr_encode_time(u32 *p, time_t time)
{
	*p++ = htonl(time);
	*p++ = 0;
	return p;
}

static inline u32 *
xdr_decode_time3(u32 *p, u64 *timep)
{
	u64 tmp = (u64)ntohl(*p++) << 32;
	*timep = tmp + (u64)ntohl(*p++);
	return p;
}

static inline u32 *
xdr_encode_time3(u32 *p, u64 time)
{
	*p++ = htonl(time >> 32);
	*p++ = htonl(time & 0xFFFFFFFF);
	return p;
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

static inline u32 *
xdr_decode_fattr(u32 *p, struct nfs_fattr *fattr)
{
	unsigned int	type;
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
	fattr->rdev = ntohl(*p++) << MINORBITS;
	fattr->rdev |= ntohl(*p++) & MINORMASK;
	p = xdr_decode_hyper(p, &fattr->fsid);
	p = xdr_decode_hyper(p, &fattr->fileid);
	p = xdr_decode_time3(p, &fattr->atime);
	p = xdr_decode_time3(p, &fattr->mtime);
	p = xdr_decode_time3(p, &fattr->ctime);

	/* Update the mode bits */
	fattr->valid |= (NFS_ATTR_FATTR | NFS_ATTR_FATTR_V3);
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
		p = xdr_encode_time(p, attr->ia_atime);
	} else if (attr->ia_valid & ATTR_ATIME) {
		*p++ = xdr_one;
	} else {
		*p++ = xdr_zero;
	}
	if (attr->ia_valid & ATTR_MTIME_SET) {
		*p++ = xdr_two;
		p = xdr_encode_time(p, attr->ia_mtime);
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
 * Encode void argument
 */
static int
nfs3_xdr_enc_void(struct rpc_rqst *req, u32 *p, void *dummy)
{
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	return 0;
}

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
		p = xdr_encode_time3(p, args->guardtime);
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
	int		buflen, replen;
	unsigned int	nr;

	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_hyper(p, args->offset);
	*p++ = htonl(args->count);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* Get the number of buffers in the receive iovec */
	nr = args->nriov;

	if (nr+2 > MAX_IOVEC) {
		printk(KERN_ERR "NFS: Bad number of iov's in xdr_readargs\n");
		return -EINVAL;
	}

	/* set up reply iovec */
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS3_readres_sz) << 2;
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
 * Write arguments. Splice the buffer to be written into the iovec.
 */
static int
nfs3_xdr_writeargs(struct rpc_rqst *req, u32 *p, struct nfs_writeargs *args)
{
	unsigned int	nr;
	u32 count = args->count;

	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_hyper(p, args->offset);
	*p++ = htonl(count);
	*p++ = htonl(args->stable);
	*p++ = htonl(count);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* Get the number of buffers in the send iovec */
	nr = args->nriov;

	if (nr+2 > MAX_IOVEC) {
		printk(KERN_ERR "NFS: Bad number of iov's in xdr_writeargs\n");
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
	req->rq_snr  += nr;

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
		*p++ = htonl(args->rdev >> MINORBITS);
		*p++ = htonl(args->rdev & MINORMASK);
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
	int		buflen, replen;

	p = xdr_encode_fhandle(p, args->fh);
	p = xdr_encode_hyper(p, args->cookie);
	*p++ = args->verf[0];
	*p++ = args->verf[1];
	if (args->plus) {
		/* readdirplus: need dircount + buffer size.
		 * We just make sure we make dircount big enough */
		*p++ = htonl(args->bufsiz >> 3);
	}
	*p++ = htonl(args->bufsiz);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);

	/* set up reply iovec */
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS3_readdirres_sz) << 2;
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
 * We just check for syntactical correctness.
 */
static int
nfs3_xdr_readdirres(struct rpc_rqst *req, u32 *p, struct nfs3_readdirres *res)
{
	struct iovec	*iov = req->rq_rvec;
	int		hdrlen;
	int		status, nr;
	unsigned int	len;
	u32		*entry, *end;

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
	if (iov->iov_len > hdrlen) {
		dprintk("NFS: READDIR header is short. iovec will be shifted.\n");
		xdr_shift_iovec(iov, req->rq_rnr, iov->iov_len - hdrlen);
	}

	p   = (u32 *) iov[1].iov_base;
	end = (u32 *) ((u8 *) p + iov[1].iov_len);
	for (nr = 0; *p++; nr++) {
		entry = p - 1;
		p += 2;				/* inode # */
		len = ntohl(*p++);		/* string length */
		p += XDR_QUADLEN(len) + 2;	/* name + cookie */
		if (len > NFS3_MAXNAMLEN) {
			printk(KERN_WARNING "NFS: giant filename in readdir (len %x)!\n",
						len);
			return -errno_NFSERR_IO;
		}

		if (res->plus) {
			/* post_op_attr */
			if (*p++)
				p += 21;
			/* post_op_fh3 */
			if (*p++) {
				len = ntohl(*p++);
				if (len > NFS3_FHSIZE) {
					printk(KERN_WARNING "NFS: giant filehandle in "
						"readdir (len %x)!\n", len);
					return -errno_NFSERR_IO;
				}
				p += XDR_QUADLEN(len);
			}
		}

		if (p + 2 > end) {
			printk(KERN_NOTICE
				"NFS: short packet in readdir reply!\n");
			/* truncate listing */
			entry[0] = entry[1] = 0;
			break;
		}
	}

	return nr;
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
		p = xdr_decode_post_op_attr(p, &entry->fattr);
		/* In fact, a post_op_fh3: */
		if (*p++) {
			p = xdr_decode_fhandle(p, &entry->fh);
			/* Ugh -- server reply was truncated */
			if (p == NULL) {
				dprintk("NFS: FH truncated\n");
				*entry = old;
				return ERR_PTR(-EAGAIN);
			}
		} else {
			/* If we don't get a file handle, the attrs
			 * aren't worth a lot. */
			entry->fattr.valid = 0;
		}
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
 * Decode void reply
 */
static int
nfs3_xdr_dec_void(struct rpc_rqst *req, u32 *p, void *dummy)
{
	return 0;
}

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
	struct rpc_task *task = req->rq_task;
	struct rpc_auth *auth = task->tk_auth;
	int		buflen, replen;

	p = xdr_encode_fhandle(p, args->fh);
	req->rq_slen = xdr_adjust_iovec(req->rq_svec, p);
	replen = (RPC_REPHDRSIZE + auth->au_rslack + NFS3_readlinkres_sz) << 2;
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
nfs3_xdr_readlinkres(struct rpc_rqst *req, u32 *p, struct nfs3_readlinkres *res)
{
	struct iovec	*iov = req->rq_rvec;
	int		hdrlen;
	u32	*strlen;
	char	*string;
	int	status;
	unsigned int len;

	status = ntohl(*p++);
	p = xdr_decode_post_op_attr(p, res->fattr);

	if (status != 0)
		return -nfs_stat_to_errno(status);

	hdrlen = (u8 *) p - (u8 *) iov->iov_base;
	if (iov->iov_len > hdrlen) {
		dprintk("NFS: READLINK header is short. iovec will be shifted.\n");
		xdr_shift_iovec(iov, req->rq_rnr, iov->iov_len - hdrlen);
	}

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
 * Decode READ reply
 */
static int
nfs3_xdr_readres(struct rpc_rqst *req, u32 *p, struct nfs_readres *res)
{
	struct iovec *iov = req->rq_rvec;
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
	if (iov->iov_len > hdrlen) {
		dprintk("NFS: READ header is short. iovec will be shifted.\n");
		xdr_shift_iovec(iov, req->rq_rnr, iov->iov_len - hdrlen);
	}

	recvd = req->rq_rlen - hdrlen;
	if (count > recvd) {
		printk(KERN_WARNING "NFS: server cheating in read reply: "
			"count %d > recvd %d\n", count, recvd);
		count = recvd;
	}

	if (count < res->count) {
		xdr_zero_iovec(iov+1, req->rq_rnr-2, res->count - count);
		res->count = count;
	}

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
nfs3_xdr_fsstatres(struct rpc_rqst *req, u32 *p, struct nfs_fsinfo *res)
{
	struct nfs_fattr dummy;
	int		status;

	status = ntohl(*p++);

	p = xdr_decode_post_op_attr(p, &dummy);
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
	struct nfs_fattr dummy;
	int		status;

	status = ntohl(*p++);

	p = xdr_decode_post_op_attr(p, &dummy);
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
	return 0;
}

/*
 * Decode PATHCONF reply
 */
static int
nfs3_xdr_pathconfres(struct rpc_rqst *req, u32 *p, struct nfs_fsinfo *res)
{
	struct nfs_fattr dummy;
	int		status;

	status = ntohl(*p++);

	p = xdr_decode_post_op_attr(p, &dummy);
	if (status != 0)
		return -nfs_stat_to_errno(status);
	res->linkmax = ntohl(*p++);
	res->namelen = ntohl(*p++);

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

#define PROC(proc, argtype, restype)				\
    { "nfs3_" #proc,						\
      (kxdrproc_t) nfs3_xdr_##argtype,				\
      (kxdrproc_t) nfs3_xdr_##restype,				\
      MAX(NFS3_##argtype##_sz,NFS3_##restype##_sz) << 2,	\
      0							\
    }

static struct rpc_procinfo	nfs3_procedures[22] = {
  PROC(null,		enc_void,	dec_void),
  PROC(getattr,		fhandle,	attrstat),
  PROC(setattr, 	sattrargs,	wccstat),
  PROC(lookup,		diropargs,	lookupres),
  PROC(access,		accessargs,	accessres),
  PROC(readlink,	readlinkargs,	readlinkres),
  PROC(read,		readargs,	readres),
  PROC(write,		writeargs,	writeres),
  PROC(create,		createargs,	createres),
  PROC(mkdir,		mkdirargs,	createres),
  PROC(symlink,		symlinkargs,	createres),
  PROC(mknod,		mknodargs,	createres),
  PROC(remove,		diropargs,	wccstat),
  PROC(rmdir,		diropargs,	wccstat),
  PROC(rename,		renameargs,	renameres),
  PROC(link,		linkargs,	linkres),
  PROC(readdir,		readdirargs,	readdirres),
  PROC(readdirplus,	readdirargs,	readdirres),
  PROC(fsstat,		fhandle,	fsstatres),
  PROC(fsinfo,  	fhandle,	fsinfores),
  PROC(pathconf,	fhandle,	pathconfres),
  PROC(commit,		commitargs,	commitres),
};

struct rpc_version		nfs_version3 = {
	3,
	sizeof(nfs3_procedures)/sizeof(nfs3_procedures[0]),
	nfs3_procedures
};

