/*
 * include/linux/nfsd/nfsfh.h
 *
 * This file describes the layout of the file handles as passed
 * over the wire.
 *
 * Earlier versions of knfsd used to sign file handles using keyed MD5
 * or SHA. I've removed this code, because it doesn't give you more
 * security than blocking external access to port 2049 on your firewall.
 *
 * Copyright (C) 1995, 1996, 1997 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef _LINUX_NFSD_FH_H
#define _LINUX_NFSD_FH_H

#include <asm/types.h>
#ifdef __KERNEL__
# include <linux/config.h>
# include <linux/types.h>
# include <linux/string.h>
# include <linux/fs.h>
#endif
#include <linux/nfsd/const.h>
#include <linux/nfsd/debug.h>

/*
 * This is the old "dentry style" Linux NFSv2 file handle.
 *
 * The xino and xdev fields are currently used to transport the
 * ino/dev of the exported inode.
 */
struct nfs_fhbase_old {
	struct dentry *	fb_dentry;	/* dentry cookie - always 0xfeebbaca */
	__u32		fb_ino;		/* our inode number */
	__u32		fb_dirino;	/* dir inode number, 0 for directories */
	__u32		fb_dev;		/* our device */
	__u32		fb_xdev;
	__u32		fb_xino;
	__u32		fb_generation;
};

/*
 * This is the new flexible, extensible style NFSv2/v3 file handle.
 * by Neil Brown <neilb@cse.unsw.edu.au> - March 2000
 *
 * The file handle is seens as a list of 4byte words.
 * The first word contains a version number (1) and four descriptor bytes
 * that tell how the remaining 3 variable length fields should be handled.
 * These three bytes are auth_type, fsid_type and fileid_type.
 *
 * All 4byte values are in host-byte-order.
 *
 * The auth_type field specifies how the filehandle can be authenticated
 * This might allow a file to be confirmed to be in a writable part of a
 * filetree without checking the path from it upto the root.
 * Current values:
 *     0  - No authentication.  fb_auth is 0 bytes long
 * Possible future values:
 *     1  - 4 bytes taken from MD5 hash of the remainer of the file handle
 *          prefixed by a secret and with the important export flags.
 *
 * The fsid_type identifies how the filesystem (or export point) is
 *    encoded.
 *  Current values:
 *     0  - 4 byte device id (ms-2-bytes major, ls-2-bytes minor), 4byte inode number
 *        NOTE: we cannot use the kdev_t device id value, because kdev_t.h
 *              says we mustn't.  We must break it up and reassemble.
 *  Possible future encodings:
 *     1  - 4 byte user specified identifier
 *
 * The fileid_type identified how the file within the filesystem is encoded.
 * This is (will be) passed to, and set by, the underlying filesystem if it supports
 * filehandle operations.  The filesystem must not use the value '0' or '0xff' and may
 * only use the values 1 and 2 as defined below:
 *  Current values:
 *    0   - The root, or export point, of the filesystem.  fb_fileid is 0 bytes.
 *    1   - 32bit inode number, 32 bit generation number.
 *    2   - 32bit inode number, 32 bit generation number, 32 bit parent directory inode number.
 *
 */
struct nfs_fhbase_new {
	__u8		fb_version;	/* == 1, even => nfs_fhbase_old */
	__u8		fb_auth_type;
	__u8		fb_fsid_type;
	__u8		fb_fileid_type;
	__u32		fb_auth[1];
/*	__u32		fb_fsid[0]; floating */
/*	__u32		fb_fileid[0]; floating */
};

struct knfsd_fh {
	unsigned int	fh_size;	/* significant for NFSv3.
					 * Points to the current size while building
					 * a new file handle
					 */
	union {
		struct nfs_fhbase_old	fh_old;
		__u32			fh_pad[NFS3_FHSIZE/4];
		struct nfs_fhbase_new	fh_new;
	} fh_base;
};

#define ofh_dcookie		fh_base.fh_old.fb_dentry
#define ofh_ino			fh_base.fh_old.fb_ino
#define ofh_dirino		fh_base.fh_old.fb_dirino
#define ofh_dev			fh_base.fh_old.fb_dev
#define ofh_xdev		fh_base.fh_old.fb_xdev
#define ofh_xino		fh_base.fh_old.fb_xino
#define ofh_generation		fh_base.fh_old.fb_generation

#define	fh_version		fh_base.fh_new.fb_version
#define	fh_fsid_type		fh_base.fh_new.fb_fsid_type
#define	fh_auth_type		fh_base.fh_new.fb_auth_type
#define	fh_fileid_type		fh_base.fh_new.fb_fileid_type
#define	fh_auth			fh_base.fh_new.fb_auth

#ifdef __KERNEL__

/*
 * Conversion macros for the filehandle fields.
 */
extern inline __u32 kdev_t_to_u32(kdev_t dev)
{
	return (__u32) dev;
}

extern inline kdev_t u32_to_kdev_t(__u32 udev)
{
	return (kdev_t) udev;
}

extern inline __u32 ino_t_to_u32(ino_t ino)
{
	return (__u32) ino;
}

extern inline ino_t u32_to_ino_t(__u32 uino)
{
	return (ino_t) uino;
}

/*
 * This is the internal representation of an NFS handle used in knfsd.
 * pre_mtime/post_version will be used to support wcc_attr's in NFSv3.
 */
typedef struct svc_fh {
	struct knfsd_fh		fh_handle;	/* FH data */
	struct dentry *		fh_dentry;	/* validated dentry */
	struct svc_export *	fh_export;	/* export pointer */
	int			fh_maxsize;	/* max size for fh_handle */

	unsigned char		fh_locked;	/* inode locked by us */

#ifdef CONFIG_NFSD_V3
	unsigned char		fh_post_saved;	/* post-op attrs saved */
	unsigned char		fh_pre_saved;	/* pre-op attrs saved */

	/* Pre-op attributes saved during fh_lock */
	__u64			fh_pre_size;	/* size before operation */
	time_t			fh_pre_mtime;	/* mtime before oper */
	time_t			fh_pre_ctime;	/* ctime before oper */

	/* Post-op attributes saved in fh_unlock */
	umode_t			fh_post_mode;	/* i_mode */
	nlink_t			fh_post_nlink;	/* i_nlink */
	uid_t			fh_post_uid;	/* i_uid */
	gid_t			fh_post_gid;	/* i_gid */
	__u64			fh_post_size;	/* i_size */
	unsigned long		fh_post_blocks; /* i_blocks */
	unsigned long		fh_post_blksize;/* i_blksize */
	kdev_t			fh_post_rdev;	/* i_rdev */
	time_t			fh_post_atime;	/* i_atime */
	time_t			fh_post_mtime;	/* i_mtime */
	time_t			fh_post_ctime;	/* i_ctime */
#endif /* CONFIG_NFSD_V3 */

} svc_fh;

/*
 * Shorthand for dprintk()'s
 */
inline static char * SVCFH_fmt(struct svc_fh *fhp)
{
	struct knfsd_fh *fh = &fhp->fh_handle;
	
	static char buf[80];
	sprintf(buf, "%d: %08x %08x %08x %08x %08x %08x",
		fh->fh_size,
		fh->fh_base.fh_pad[0],
		fh->fh_base.fh_pad[1],
		fh->fh_base.fh_pad[2],
		fh->fh_base.fh_pad[3],
		fh->fh_base.fh_pad[4],
		fh->fh_base.fh_pad[5]);
	return buf;
}
/*
 * Function prototypes
 */
u32	fh_verify(struct svc_rqst *, struct svc_fh *, int, int);
int	fh_compose(struct svc_fh *, struct svc_export *, struct dentry *);
int	fh_update(struct svc_fh *);
void	fh_put(struct svc_fh *);

static __inline__ struct svc_fh *
fh_copy(struct svc_fh *dst, struct svc_fh *src)
{
	if (src->fh_dentry || src->fh_locked) {
		struct dentry *dentry = src->fh_dentry;
		printk(KERN_ERR "fh_copy: copying %s/%s, already verified!\n",
			dentry->d_parent->d_name.name, dentry->d_name.name);
	}
			
	*dst = *src;
	return dst;
}

static __inline__ struct svc_fh *
fh_init(struct svc_fh *fhp, int maxsize)
{
	memset(fhp, 0, sizeof(*fhp));
	fhp->fh_maxsize = maxsize;
	return fhp;
}

#ifdef CONFIG_NFSD_V3
/*
 * Fill in the pre_op attr for the wcc data
 */
static inline void
fill_pre_wcc(struct svc_fh *fhp)
{
	struct inode    *inode;

	inode = fhp->fh_dentry->d_inode;
	if (!fhp->fh_pre_saved) {
		fhp->fh_pre_mtime = inode->i_mtime;
			fhp->fh_pre_ctime = inode->i_ctime;
			fhp->fh_pre_size  = inode->i_size;
			fhp->fh_pre_saved = 1;
	}
}

/*
 * Fill in the post_op attr for the wcc data
 */
static inline void
fill_post_wcc(struct svc_fh *fhp)
{
	struct inode    *inode = fhp->fh_dentry->d_inode;

	if (fhp->fh_post_saved)
		printk("nfsd: inode locked twice during operation.\n");

	fhp->fh_post_mode       = inode->i_mode;
	fhp->fh_post_nlink      = inode->i_nlink;
	fhp->fh_post_uid	= inode->i_uid;
	fhp->fh_post_gid	= inode->i_gid;
	fhp->fh_post_size       = inode->i_size;
	if (inode->i_blksize) {
		fhp->fh_post_blksize    = inode->i_blksize;
		fhp->fh_post_blocks     = inode->i_blocks;
	} else {
		fhp->fh_post_blksize    = BLOCK_SIZE;
		/* how much do we care for accuracy with MinixFS? */
		fhp->fh_post_blocks     = (inode->i_size+511) >> 9;
	}
	fhp->fh_post_rdev       = inode->i_rdev;
	fhp->fh_post_atime      = inode->i_atime;
	fhp->fh_post_mtime      = inode->i_mtime;
	fhp->fh_post_ctime      = inode->i_ctime;
	fhp->fh_post_saved      = 1;
}
#else
#define	fill_pre_wcc(ignored)
#define fill_post_wcc(notused)
#endif /* CONFIG_NFSD_V3 */


/*
 * Lock a file handle/inode
 * NOTE: both fh_lock and fh_unlock are done "by hand" in
 * vfs.c:nfsd_rename as it needs to grab 2 i_sem's at once
 * so, any changes here should be reflected there.
 */
static inline void
fh_lock(struct svc_fh *fhp)
{
	struct dentry	*dentry = fhp->fh_dentry;
	struct inode	*inode;

	dfprintk(FILEOP, "nfsd: fh_lock(%s) locked = %d\n",
			SVCFH_fmt(fhp), fhp->fh_locked);

	if (!fhp->fh_dentry) {
		printk(KERN_ERR "fh_lock: fh not verified!\n");
		return;
	}
	if (fhp->fh_locked) {
		printk(KERN_WARNING "fh_lock: %s/%s already locked!\n",
			dentry->d_parent->d_name.name, dentry->d_name.name);
		return;
	}

	inode = dentry->d_inode;
	down(&inode->i_sem);
	fill_pre_wcc(fhp);
	fhp->fh_locked = 1;
}

/*
 * Unlock a file handle/inode
 */
static inline void
fh_unlock(struct svc_fh *fhp)
{
	if (!fhp->fh_dentry)
		printk(KERN_ERR "fh_unlock: fh not verified!\n");

	if (fhp->fh_locked) {
		fill_post_wcc(fhp);
		up(&fhp->fh_dentry->d_inode->i_sem);
		fhp->fh_locked = 0;
	}
}
#endif /* __KERNEL__ */


#endif /* _LINUX_NFSD_FH_H */
