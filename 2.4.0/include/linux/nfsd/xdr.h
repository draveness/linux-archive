/*
 * linux/inxlude/linux/nfsd/xdr.h
 *
 * XDR types for nfsd. This is mainly a typing exercise.
 */

#ifndef LINUX_NFSD_H
#define LINUX_NFSD_H

#include <linux/fs.h>
#include <linux/nfs.h>

struct nfsd_fhandle {
	struct svc_fh		fh;
};

struct nfsd_sattrargs {
	struct svc_fh		fh;
	struct iattr		attrs;
};

struct nfsd_diropargs {
	struct svc_fh		fh;
	char *			name;
	int			len;
};

struct nfsd_readargs {
	struct svc_fh		fh;
	__u32			offset;
	__u32			count;
	__u32			totalsize;
};

struct nfsd_writeargs {
	svc_fh			fh;
	__u32			beginoffset;
	__u32			offset;
	__u32			totalcount;
	__u8 *			data;
	int			len;
};

struct nfsd_createargs {
	struct svc_fh		fh;
	char *			name;
	int			len;
	struct iattr		attrs;
};

struct nfsd_renameargs {
	struct svc_fh		ffh;
	char *			fname;
	int			flen;
	struct svc_fh		tfh;
	char *			tname;
	int			tlen;
};

struct nfsd_linkargs {
	struct svc_fh		ffh;
	struct svc_fh		tfh;
	char *			tname;
	int			tlen;
};

struct nfsd_symlinkargs {
	struct svc_fh		ffh;
	char *			fname;
	int			flen;
	char *			tname;
	int			tlen;
	struct iattr		attrs;
};

struct nfsd_readdirargs {
	struct svc_fh		fh;
	__u32			cookie;
	__u32			count;
};

struct nfsd_attrstat {
	struct svc_fh		fh;
};

struct nfsd_diropres  {
	struct svc_fh		fh;
};

struct nfsd_readlinkres {
	int			len;
};

struct nfsd_readres {
	struct svc_fh		fh;
	unsigned long		count;
};

struct nfsd_readdirres {
	int			count;
};

struct nfsd_statfsres {
	struct statfs		stats;
};

/*
 * Storage requirements for XDR arguments and results.
 */
union nfsd_xdrstore {
	struct nfsd_sattrargs	sattr;
	struct nfsd_diropargs	dirop;
	struct nfsd_readargs	read;
	struct nfsd_writeargs	write;
	struct nfsd_createargs	create;
	struct nfsd_renameargs	rename;
	struct nfsd_linkargs	link;
	struct nfsd_symlinkargs	symlink;
	struct nfsd_readdirargs	readdir;
};

#define NFSSVC_XDRSIZE		sizeof(union nfsd_xdrstore)


int nfssvc_decode_void(struct svc_rqst *, u32 *, void *);
int nfssvc_decode_fhandle(struct svc_rqst *, u32 *, struct svc_fh *);
int nfssvc_decode_sattrargs(struct svc_rqst *, u32 *,
				struct nfsd_sattrargs *);
int nfssvc_decode_diropargs(struct svc_rqst *, u32 *,
				struct nfsd_diropargs *);
int nfssvc_decode_readargs(struct svc_rqst *, u32 *,
				struct nfsd_readargs *);
int nfssvc_decode_writeargs(struct svc_rqst *, u32 *,
				struct nfsd_writeargs *);
int nfssvc_decode_createargs(struct svc_rqst *, u32 *,
				struct nfsd_createargs *);
int nfssvc_decode_renameargs(struct svc_rqst *, u32 *,
				struct nfsd_renameargs *);
int nfssvc_decode_linkargs(struct svc_rqst *, u32 *,
				struct nfsd_linkargs *);
int nfssvc_decode_symlinkargs(struct svc_rqst *, u32 *,
				struct nfsd_symlinkargs *);
int nfssvc_decode_readdirargs(struct svc_rqst *, u32 *,
				struct nfsd_readdirargs *);
int nfssvc_encode_void(struct svc_rqst *, u32 *, void *);
int nfssvc_encode_attrstat(struct svc_rqst *, u32 *, struct nfsd_attrstat *);
int nfssvc_encode_diropres(struct svc_rqst *, u32 *, struct nfsd_diropres *);
int nfssvc_encode_readlinkres(struct svc_rqst *, u32 *, struct nfsd_readlinkres *);
int nfssvc_encode_readres(struct svc_rqst *, u32 *, struct nfsd_readres *);
int nfssvc_encode_statfsres(struct svc_rqst *, u32 *, struct nfsd_statfsres *);
int nfssvc_encode_readdirres(struct svc_rqst *, u32 *, struct nfsd_readdirres *);

int nfssvc_encode_entry(struct readdir_cd *, const char *name,
				int namlen, off_t offset, ino_t ino, unsigned int);

int nfssvc_release_fhandle(struct svc_rqst *, u32 *, struct nfsd_fhandle *);

#endif /* LINUX_NFSD_H */
