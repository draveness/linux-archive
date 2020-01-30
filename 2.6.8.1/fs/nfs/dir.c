/*
 *  linux/fs/nfs/dir.c
 *
 *  Copyright (C) 1992  Rick Sladkey
 *
 *  nfs directory handling functions
 *
 * 10 Apr 1996	Added silly rename for unlink	--okir
 * 28 Sep 1996	Improved directory cache --okir
 * 23 Aug 1997  Claus Heine claus@momo.math.rwth-aachen.de 
 *              Re-implemented silly rename for unlink, newly implemented
 *              silly rename for nfs_rename() following the suggestions
 *              of Olaf Kirch (okir) found in this file.
 *              Following Linus comments on my original hack, this version
 *              depends only on the dcache stuff and doesn't touch the inode
 *              layer (iput() and friends).
 *  6 Jun 1999	Cache readdir lookups in the page cache. -DaveM
 */

#include <linux/time.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_mount.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#include <linux/namei.h>

#define NFS_PARANOIA 1
/* #define NFS_DEBUG_VERBOSE 1 */

static int nfs_opendir(struct inode *, struct file *);
static int nfs_readdir(struct file *, void *, filldir_t);
static struct dentry *nfs_lookup(struct inode *, struct dentry *, struct nameidata *);
static int nfs_cached_lookup(struct inode *, struct dentry *,
				struct nfs_fh *, struct nfs_fattr *);
static int nfs_create(struct inode *, struct dentry *, int, struct nameidata *);
static int nfs_mkdir(struct inode *, struct dentry *, int);
static int nfs_rmdir(struct inode *, struct dentry *);
static int nfs_unlink(struct inode *, struct dentry *);
static int nfs_symlink(struct inode *, struct dentry *, const char *);
static int nfs_link(struct dentry *, struct inode *, struct dentry *);
static int nfs_mknod(struct inode *, struct dentry *, int, dev_t);
static int nfs_rename(struct inode *, struct dentry *,
		      struct inode *, struct dentry *);

struct file_operations nfs_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= nfs_readdir,
	.open		= nfs_opendir,
	.release	= nfs_release,
};

struct inode_operations nfs_dir_inode_operations = {
	.create		= nfs_create,
	.lookup		= nfs_lookup,
	.link		= nfs_link,
	.unlink		= nfs_unlink,
	.symlink	= nfs_symlink,
	.mkdir		= nfs_mkdir,
	.rmdir		= nfs_rmdir,
	.mknod		= nfs_mknod,
	.rename		= nfs_rename,
	.permission	= nfs_permission,
	.getattr	= nfs_getattr,
	.setattr	= nfs_setattr,
};

#ifdef CONFIG_NFS_V4

static struct dentry *nfs_atomic_lookup(struct inode *, struct dentry *, struct nameidata *);
struct inode_operations nfs4_dir_inode_operations = {
	.create		= nfs_create,
	.lookup		= nfs_atomic_lookup,
	.link		= nfs_link,
	.unlink		= nfs_unlink,
	.symlink	= nfs_symlink,
	.mkdir		= nfs_mkdir,
	.rmdir		= nfs_rmdir,
	.mknod		= nfs_mknod,
	.rename		= nfs_rename,
	.permission	= nfs_permission,
	.getattr	= nfs_getattr,
	.setattr	= nfs_setattr,
};

#endif /* CONFIG_NFS_V4 */

/*
 * Open file
 */
static int
nfs_opendir(struct inode *inode, struct file *filp)
{
	int res = 0;

	lock_kernel();
	/* Call generic open code in order to cache credentials */
	if (!res)
		res = nfs_open(inode, filp);
	unlock_kernel();
	return res;
}

typedef u32 * (*decode_dirent_t)(u32 *, struct nfs_entry *, int);
typedef struct {
	struct file	*file;
	struct page	*page;
	unsigned long	page_index;
	u32		*ptr;
	u64		target;
	struct nfs_entry *entry;
	decode_dirent_t	decode;
	int		plus;
	int		error;
} nfs_readdir_descriptor_t;

/* Now we cache directories properly, by stuffing the dirent
 * data directly in the page cache.
 *
 * Inode invalidation due to refresh etc. takes care of
 * _everything_, no sloppy entry flushing logic, no extraneous
 * copying, network direct to page cache, the way it was meant
 * to be.
 *
 * NOTE: Dirent information verification is done always by the
 *	 page-in of the RPC reply, nowhere else, this simplies
 *	 things substantially.
 */
static
int nfs_readdir_filler(nfs_readdir_descriptor_t *desc, struct page *page)
{
	struct file	*file = desc->file;
	struct inode	*inode = file->f_dentry->d_inode;
	struct rpc_cred	*cred = nfs_file_cred(file);
	unsigned long	timestamp;
	int		error;

	dfprintk(VFS, "NFS: nfs_readdir_filler() reading cookie %Lu into page %lu.\n", (long long)desc->entry->cookie, page->index);

 again:
	timestamp = jiffies;
	error = NFS_PROTO(inode)->readdir(file->f_dentry, cred, desc->entry->cookie, page,
					  NFS_SERVER(inode)->dtsize, desc->plus);
	if (error < 0) {
		/* We requested READDIRPLUS, but the server doesn't grok it */
		if (error == -ENOTSUPP && desc->plus) {
			NFS_SERVER(inode)->caps &= ~NFS_CAP_READDIRPLUS;
			NFS_FLAGS(inode) &= ~NFS_INO_ADVISE_RDPLUS;
			desc->plus = 0;
			goto again;
		}
		goto error;
	}
	SetPageUptodate(page);
	NFS_FLAGS(inode) |= NFS_INO_INVALID_ATIME;
	/* Ensure consistent page alignment of the data.
	 * Note: assumes we have exclusive access to this mapping either
	 *	 throught inode->i_sem or some other mechanism.
	 */
	if (page->index == 0) {
		invalidate_inode_pages(inode->i_mapping);
		NFS_I(inode)->readdir_timestamp = timestamp;
	}
	unlock_page(page);
	return 0;
 error:
	SetPageError(page);
	unlock_page(page);
	nfs_zap_caches(inode);
	desc->error = error;
	return -EIO;
}

static inline
int dir_decode(nfs_readdir_descriptor_t *desc)
{
	u32	*p = desc->ptr;
	p = desc->decode(p, desc->entry, desc->plus);
	if (IS_ERR(p))
		return PTR_ERR(p);
	desc->ptr = p;
	return 0;
}

static inline
void dir_page_release(nfs_readdir_descriptor_t *desc)
{
	kunmap(desc->page);
	page_cache_release(desc->page);
	desc->page = NULL;
	desc->ptr = NULL;
}

/*
 * Given a pointer to a buffer that has already been filled by a call
 * to readdir, find the next entry.
 *
 * If the end of the buffer has been reached, return -EAGAIN, if not,
 * return the offset within the buffer of the next entry to be
 * read.
 */
static inline
int find_dirent(nfs_readdir_descriptor_t *desc, struct page *page)
{
	struct nfs_entry *entry = desc->entry;
	int		loop_count = 0,
			status;

	while((status = dir_decode(desc)) == 0) {
		dfprintk(VFS, "NFS: found cookie %Lu\n", (long long)entry->cookie);
		if (entry->prev_cookie == desc->target)
			break;
		if (loop_count++ > 200) {
			loop_count = 0;
			schedule();
		}
	}
	dfprintk(VFS, "NFS: find_dirent() returns %d\n", status);
	return status;
}

/*
 * Find the given page, and call find_dirent() in order to try to
 * return the next entry.
 */
static inline
int find_dirent_page(nfs_readdir_descriptor_t *desc)
{
	struct inode	*inode = desc->file->f_dentry->d_inode;
	struct page	*page;
	int		status;

	dfprintk(VFS, "NFS: find_dirent_page() searching directory page %ld\n", desc->page_index);

	page = read_cache_page(inode->i_mapping, desc->page_index,
			       (filler_t *)nfs_readdir_filler, desc);
	if (IS_ERR(page)) {
		status = PTR_ERR(page);
		goto out;
	}
	if (!PageUptodate(page))
		goto read_error;

	/* NOTE: Someone else may have changed the READDIRPLUS flag */
	desc->page = page;
	desc->ptr = kmap(page);		/* matching kunmap in nfs_do_filldir */
	status = find_dirent(desc, page);
	if (status < 0)
		dir_page_release(desc);
 out:
	dfprintk(VFS, "NFS: find_dirent_page() returns %d\n", status);
	return status;
 read_error:
	page_cache_release(page);
	return -EIO;
}

/*
 * Recurse through the page cache pages, and return a
 * filled nfs_entry structure of the next directory entry if possible.
 *
 * The target for the search is 'desc->target'.
 */
static inline
int readdir_search_pagecache(nfs_readdir_descriptor_t *desc)
{
	int		loop_count = 0;
	int		res;

	dfprintk(VFS, "NFS: readdir_search_pagecache() searching for cookie %Lu\n", (long long)desc->target);
	for (;;) {
		res = find_dirent_page(desc);
		if (res != -EAGAIN)
			break;
		/* Align to beginning of next page */
		desc->page_index ++;
		if (loop_count++ > 200) {
			loop_count = 0;
			schedule();
		}
	}
	dfprintk(VFS, "NFS: readdir_search_pagecache() returned %d\n", res);
	return res;
}

static unsigned int nfs_type2dtype[] = {
	DT_UNKNOWN,
	DT_REG,
	DT_DIR,
	DT_BLK,
	DT_CHR,
	DT_LNK,
	DT_SOCK,
	DT_UNKNOWN,
	DT_FIFO
};

static inline
unsigned int nfs_type_to_d_type(enum nfs_ftype type)
{
	return nfs_type2dtype[type];
}

/*
 * Once we've found the start of the dirent within a page: fill 'er up...
 */
static 
int nfs_do_filldir(nfs_readdir_descriptor_t *desc, void *dirent,
		   filldir_t filldir)
{
	struct file	*file = desc->file;
	struct nfs_entry *entry = desc->entry;
	unsigned long	fileid;
	int		loop_count = 0,
			res;

	dfprintk(VFS, "NFS: nfs_do_filldir() filling starting @ cookie %Lu\n", (long long)desc->target);

	for(;;) {
		unsigned d_type = DT_UNKNOWN;
		/* Note: entry->prev_cookie contains the cookie for
		 *	 retrieving the current dirent on the server */
		fileid = nfs_fileid_to_ino_t(entry->ino);

		/* Use readdirplus info */
		if (desc->plus && (entry->fattr->valid & NFS_ATTR_FATTR))
			d_type = nfs_type_to_d_type(entry->fattr->type);

		res = filldir(dirent, entry->name, entry->len, 
			      entry->prev_cookie, fileid, d_type);
		if (res < 0)
			break;
		file->f_pos = desc->target = entry->cookie;
		if (dir_decode(desc) != 0) {
			desc->page_index ++;
			break;
		}
		if (loop_count++ > 200) {
			loop_count = 0;
			schedule();
		}
	}
	dir_page_release(desc);

	dfprintk(VFS, "NFS: nfs_do_filldir() filling ended @ cookie %Lu; returning = %d\n", (long long)desc->target, res);
	return res;
}

/*
 * If we cannot find a cookie in our cache, we suspect that this is
 * because it points to a deleted file, so we ask the server to return
 * whatever it thinks is the next entry. We then feed this to filldir.
 * If all goes well, we should then be able to find our way round the
 * cache on the next call to readdir_search_pagecache();
 *
 * NOTE: we cannot add the anonymous page to the pagecache because
 *	 the data it contains might not be page aligned. Besides,
 *	 we should already have a complete representation of the
 *	 directory in the page cache by the time we get here.
 */
static inline
int uncached_readdir(nfs_readdir_descriptor_t *desc, void *dirent,
		     filldir_t filldir)
{
	struct file	*file = desc->file;
	struct inode	*inode = file->f_dentry->d_inode;
	struct rpc_cred	*cred = nfs_file_cred(file);
	struct page	*page = NULL;
	int		status;

	dfprintk(VFS, "NFS: uncached_readdir() searching for cookie %Lu\n", (long long)desc->target);

	page = alloc_page(GFP_HIGHUSER);
	if (!page) {
		status = -ENOMEM;
		goto out;
	}
	desc->error = NFS_PROTO(inode)->readdir(file->f_dentry, cred, desc->target,
						page,
						NFS_SERVER(inode)->dtsize,
						desc->plus);
	NFS_FLAGS(inode) |= NFS_INO_INVALID_ATIME;
	desc->page = page;
	desc->ptr = kmap(page);		/* matching kunmap in nfs_do_filldir */
	if (desc->error >= 0) {
		if ((status = dir_decode(desc)) == 0)
			desc->entry->prev_cookie = desc->target;
	} else
		status = -EIO;
	if (status < 0)
		goto out_release;

	status = nfs_do_filldir(desc, dirent, filldir);

	/* Reset read descriptor so it searches the page cache from
	 * the start upon the next call to readdir_search_pagecache() */
	desc->page_index = 0;
	desc->entry->cookie = desc->entry->prev_cookie = 0;
	desc->entry->eof = 0;
 out:
	dfprintk(VFS, "NFS: uncached_readdir() returns %d\n", status);
	return status;
 out_release:
	dir_page_release(desc);
	goto out;
}

/* The file offset position is now represented as a true offset into the
 * page cache as is the case in most of the other filesystems.
 */
static int nfs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct dentry	*dentry = filp->f_dentry;
	struct inode	*inode = dentry->d_inode;
	nfs_readdir_descriptor_t my_desc,
			*desc = &my_desc;
	struct nfs_entry my_entry;
	struct nfs_fh	 fh;
	struct nfs_fattr fattr;
	long		res;

	lock_kernel();

	res = nfs_revalidate_inode(NFS_SERVER(inode), inode);
	if (res < 0) {
		unlock_kernel();
		return res;
	}

	/*
	 * filp->f_pos points to the file offset in the page cache.
	 * but if the cache has meanwhile been zapped, we need to
	 * read from the last dirent to revalidate f_pos
	 * itself.
	 */
	memset(desc, 0, sizeof(*desc));

	desc->file = filp;
	desc->target = filp->f_pos;
	desc->decode = NFS_PROTO(inode)->decode_dirent;
	desc->plus = NFS_USE_READDIRPLUS(inode);

	my_entry.cookie = my_entry.prev_cookie = 0;
	my_entry.eof = 0;
	my_entry.fh = &fh;
	my_entry.fattr = &fattr;
	desc->entry = &my_entry;

	while(!desc->entry->eof) {
		res = readdir_search_pagecache(desc);
		if (res == -EBADCOOKIE) {
			/* This means either end of directory */
			if (desc->entry->cookie != desc->target) {
				/* Or that the server has 'lost' a cookie */
				res = uncached_readdir(desc, dirent, filldir);
				if (res >= 0)
					continue;
			}
			res = 0;
			break;
		}
		if (res == -ETOOSMALL && desc->plus) {
			NFS_FLAGS(inode) &= ~NFS_INO_ADVISE_RDPLUS;
			nfs_zap_caches(inode);
			desc->plus = 0;
			desc->entry->eof = 0;
			continue;
		}
		if (res < 0)
			break;

		res = nfs_do_filldir(desc, dirent, filldir);
		if (res < 0) {
			res = 0;
			break;
		}
	}
	unlock_kernel();
	if (desc->error < 0)
		return desc->error;
	if (res < 0)
		return res;
	return 0;
}

/*
 * A check for whether or not the parent directory has changed.
 * In the case it has, we assume that the dentries are untrustworthy
 * and may need to be looked up again.
 */
static inline int nfs_check_verifier(struct inode *dir, struct dentry *dentry)
{
	if (IS_ROOT(dentry))
		return 1;
	if ((NFS_FLAGS(dir) & NFS_INO_INVALID_ATTR) != 0
			|| nfs_attribute_timeout(dir))
		return 0;
	return nfs_verify_change_attribute(dir, (unsigned long)dentry->d_fsdata);
}

static inline void nfs_set_verifier(struct dentry * dentry, unsigned long verf)
{
	dentry->d_fsdata = (void *)verf;
}

/*
 * Whenever an NFS operation succeeds, we know that the dentry
 * is valid, so we update the revalidation timestamp.
 */
static inline void nfs_renew_times(struct dentry * dentry)
{
	dentry->d_time = jiffies;
}

static inline
int nfs_lookup_verify_inode(struct inode *inode, int isopen)
{
	struct nfs_server *server = NFS_SERVER(inode);

	if (isopen && !(server->flags & NFS_MOUNT_NOCTO))
		return __nfs_revalidate_inode(server, inode);
	return nfs_revalidate_inode(server, inode);
}

/*
 * We judge how long we want to trust negative
 * dentries by looking at the parent inode mtime.
 *
 * If parent mtime has changed, we revalidate, else we wait for a
 * period corresponding to the parent's attribute cache timeout value.
 */
static inline
int nfs_neg_need_reval(struct inode *dir, struct dentry *dentry,
		       struct nameidata *nd)
{
	int ndflags = 0;

	if (nd)
		ndflags = nd->flags;
	/* Don't revalidate a negative dentry if we're creating a new file */
	if ((ndflags & LOOKUP_CREATE) && !(ndflags & LOOKUP_CONTINUE))
		return 0;
	return !nfs_check_verifier(dir, dentry);
}

/*
 * This is called every time the dcache has a lookup hit,
 * and we should check whether we can really trust that
 * lookup.
 *
 * NOTE! The hit can be a negative hit too, don't assume
 * we have an inode!
 *
 * If the parent directory is seen to have changed, we throw out the
 * cached dentry and do a new lookup.
 */
static int nfs_lookup_revalidate(struct dentry * dentry, struct nameidata *nd)
{
	struct inode *dir;
	struct inode *inode;
	struct dentry *parent;
	int error;
	struct nfs_fh fhandle;
	struct nfs_fattr fattr;
	unsigned long verifier;
	int isopen = 0;

	parent = dget_parent(dentry);
	lock_kernel();
	dir = parent->d_inode;
	inode = dentry->d_inode;

	if (nd && !(nd->flags & LOOKUP_CONTINUE) && (nd->flags & LOOKUP_OPEN))
		isopen = 1;

	if (!inode) {
		if (nfs_neg_need_reval(dir, dentry, nd))
			goto out_bad;
		goto out_valid;
	}

	if (is_bad_inode(inode)) {
		dfprintk(VFS, "nfs_lookup_validate: %s/%s has dud inode\n",
			dentry->d_parent->d_name.name, dentry->d_name.name);
		goto out_bad;
	}

	/* Revalidate parent directory attribute cache */
	nfs_revalidate_inode(NFS_SERVER(dir), dir);

	/* Force a full look up iff the parent directory has changed */
	if (nfs_check_verifier(dir, dentry)) {
		if (nfs_lookup_verify_inode(inode, isopen))
			goto out_zap_parent;
		goto out_valid;
	}

	/*
	 * Note: we're not holding inode->i_sem and so may be racing with
	 * operations that change the directory. We therefore save the
	 * change attribute *before* we do the RPC call.
	 */
	verifier = nfs_save_change_attribute(dir);
	error = nfs_cached_lookup(dir, dentry, &fhandle, &fattr);
	if (!error) {
		if (memcmp(NFS_FH(inode), &fhandle, sizeof(struct nfs_fh))!= 0)
			goto out_bad;
		if (nfs_lookup_verify_inode(inode, isopen))
			goto out_zap_parent;
		goto out_valid_renew;
	}

	if (NFS_STALE(inode))
		goto out_bad;

	error = NFS_PROTO(dir)->lookup(dir, &dentry->d_name, &fhandle, &fattr);
	if (error)
		goto out_bad;
	if (memcmp(NFS_FH(inode), &fhandle, sizeof(struct nfs_fh))!= 0)
		goto out_bad;
	if ((error = nfs_refresh_inode(inode, &fattr)) != 0)
		goto out_bad;

 out_valid_renew:
	nfs_renew_times(dentry);
	nfs_set_verifier(dentry, verifier);
 out_valid:
	unlock_kernel();
	dput(parent);
	return 1;
out_zap_parent:
	nfs_zap_caches(dir);
 out_bad:
	NFS_CACHEINV(dir);
	if (inode && S_ISDIR(inode->i_mode)) {
		/* Purge readdir caches. */
		nfs_zap_caches(inode);
		/* If we have submounts, don't unhash ! */
		if (have_submounts(dentry))
			goto out_valid;
		shrink_dcache_parent(dentry);
	}
	d_drop(dentry);
	unlock_kernel();
	dput(parent);
	return 0;
}

/*
 * This is called from dput() when d_count is going to 0.
 */
static int nfs_dentry_delete(struct dentry *dentry)
{
	dfprintk(VFS, "NFS: dentry_delete(%s/%s, %x)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name,
		dentry->d_flags);

	if (dentry->d_flags & DCACHE_NFSFS_RENAMED) {
		/* Unhash it, so that ->d_iput() would be called */
		return 1;
	}
	if (!(dentry->d_sb->s_flags & MS_ACTIVE)) {
		/* Unhash it, so that ancestors of killed async unlink
		 * files will be cleaned up during umount */
		return 1;
	}
	return 0;

}

/*
 * Called when the dentry loses inode.
 * We use it to clean up silly-renamed files.
 */
static void nfs_dentry_iput(struct dentry *dentry, struct inode *inode)
{
	if (dentry->d_flags & DCACHE_NFSFS_RENAMED) {
		lock_kernel();
		inode->i_nlink--;
		nfs_complete_unlink(dentry);
		unlock_kernel();
	}
	/* When creating a negative dentry, we want to renew d_time */
	nfs_renew_times(dentry);
	iput(inode);
}

struct dentry_operations nfs_dentry_operations = {
	.d_revalidate	= nfs_lookup_revalidate,
	.d_delete	= nfs_dentry_delete,
	.d_iput		= nfs_dentry_iput,
};

static inline
int nfs_is_exclusive_create(struct inode *dir, struct nameidata *nd)
{
	if (NFS_PROTO(dir)->version == 2)
		return 0;
	if (!nd || (nd->flags & LOOKUP_CONTINUE) || !(nd->flags & LOOKUP_CREATE))
		return 0;
	return (nd->intent.open.flags & O_EXCL) != 0;
}

static struct dentry *nfs_lookup(struct inode *dir, struct dentry * dentry, struct nameidata *nd)
{
	struct inode *inode = NULL;
	int error;
	struct nfs_fh fhandle;
	struct nfs_fattr fattr;

	dfprintk(VFS, "NFS: lookup(%s/%s)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name);

	error = -ENAMETOOLONG;
	if (dentry->d_name.len > NFS_SERVER(dir)->namelen)
		goto out;

	error = -ENOMEM;
	dentry->d_op = NFS_PROTO(dir)->dentry_ops;

	lock_kernel();
	/* Revalidate parent directory attribute cache */
	nfs_revalidate_inode(NFS_SERVER(dir), dir);

	/* If we're doing an exclusive create, optimize away the lookup */
	if (nfs_is_exclusive_create(dir, nd))
		goto no_entry;

	error = nfs_cached_lookup(dir, dentry, &fhandle, &fattr);
	if (error != 0) {
		error = NFS_PROTO(dir)->lookup(dir, &dentry->d_name,
				&fhandle, &fattr);
		if (error == -ENOENT)
			goto no_entry;
		if (error != 0)
			goto out_unlock;
	}
	error = -EACCES;
	inode = nfs_fhget(dentry->d_sb, &fhandle, &fattr);
	if (!inode)
		goto out_unlock;
no_entry:
	error = 0;
	d_add(dentry, inode);
	nfs_renew_times(dentry);
	nfs_set_verifier(dentry, nfs_save_change_attribute(dir));
out_unlock:
	unlock_kernel();
out:
	BUG_ON(error > 0);
	return ERR_PTR(error);
}

#ifdef CONFIG_NFS_V4
static int nfs_open_revalidate(struct dentry *, struct nameidata *);

struct dentry_operations nfs4_dentry_operations = {
	.d_revalidate	= nfs_open_revalidate,
	.d_delete	= nfs_dentry_delete,
	.d_iput		= nfs_dentry_iput,
};

static int is_atomic_open(struct inode *dir, struct nameidata *nd)
{
	if (!nd)
		return 0;
	/* Check that we are indeed trying to open this file */
	if ((nd->flags & LOOKUP_CONTINUE) || !(nd->flags & LOOKUP_OPEN))
		return 0;
	/* NFS does not (yet) have a stateful open for directories */
	if (nd->flags & LOOKUP_DIRECTORY)
		return 0;
	/* Are we trying to write to a read only partition? */
	if (IS_RDONLY(dir) && (nd->intent.open.flags & (O_CREAT|O_TRUNC|FMODE_WRITE)))
		return 0;
	return 1;
}

static struct dentry *nfs_atomic_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode = NULL;
	int error = 0;

	/* Check that we are indeed trying to open this file */
	if (!is_atomic_open(dir, nd))
		goto no_open;

	if (dentry->d_name.len > NFS_SERVER(dir)->namelen) {
		error = -ENAMETOOLONG;
		goto out;
	}
	dentry->d_op = NFS_PROTO(dir)->dentry_ops;

	/* Let vfs_create() deal with O_EXCL */
	if (nd->intent.open.flags & O_EXCL)
		goto no_entry;

	/* Open the file on the server */
	lock_kernel();
	/* Revalidate parent directory attribute cache */
	nfs_revalidate_inode(NFS_SERVER(dir), dir);

	if (nd->intent.open.flags & O_CREAT) {
		nfs_begin_data_update(dir);
		inode = nfs4_atomic_open(dir, dentry, nd);
		nfs_end_data_update(dir);
	} else
		inode = nfs4_atomic_open(dir, dentry, nd);
	unlock_kernel();
	if (IS_ERR(inode)) {
		error = PTR_ERR(inode);
		switch (error) {
			/* Make a negative dentry */
			case -ENOENT:
				inode = NULL;
				break;
			/* This turned out not to be a regular file */
			case -ELOOP:
				if (!(nd->intent.open.flags & O_NOFOLLOW))
					goto no_open;
			/* case -EISDIR: */
			/* case -EINVAL: */
			default:
				goto out;
		}
	}
no_entry:
	d_add(dentry, inode);
	nfs_renew_times(dentry);
	nfs_set_verifier(dentry, nfs_save_change_attribute(dir));
out:
	BUG_ON(error > 0);
	return ERR_PTR(error);
no_open:
	return nfs_lookup(dir, dentry, nd);
}

static int nfs_open_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct dentry *parent = NULL;
	struct inode *inode = dentry->d_inode;
	struct inode *dir;
	unsigned long verifier;
	int openflags, ret = 0;

	/* NFS only supports OPEN for regular files */
	if (inode && !S_ISREG(inode->i_mode))
		goto no_open;
	parent = dget_parent(dentry);
	dir = parent->d_inode;
	if (!is_atomic_open(dir, nd))
		goto no_open;
	openflags = nd->intent.open.flags;
	if (openflags & O_CREAT) {
		/* If this is a negative dentry, just drop it */
		if (!inode)
			goto out;
		/* If this is exclusive open, just revalidate */
		if (openflags & O_EXCL)
			goto no_open;
	}
	/* We can't create new files, or truncate existing ones here */
	openflags &= ~(O_CREAT|O_TRUNC);

	/*
	 * Note: we're not holding inode->i_sem and so may be racing with
	 * operations that change the directory. We therefore save the
	 * change attribute *before* we do the RPC call.
	 */
	lock_kernel();
	verifier = nfs_save_change_attribute(dir);
	ret = nfs4_open_revalidate(dir, dentry, openflags);
	if (!ret)
		nfs_set_verifier(dentry, verifier);
	unlock_kernel();
out:
	dput(parent);
	if (!ret)
		d_drop(dentry);
	return ret;
no_open:
	dput(parent);
	return nfs_lookup_revalidate(dentry, nd);
}
#endif /* CONFIG_NFSV4 */

static inline
int find_dirent_name(nfs_readdir_descriptor_t *desc, struct page *page, struct dentry *dentry)
{
	struct nfs_entry *entry = desc->entry;
	int		 status;

	while((status = dir_decode(desc)) == 0) {
		if (entry->len != dentry->d_name.len)
			continue;
		if (memcmp(entry->name, dentry->d_name.name, entry->len))
			continue;
		if (!(entry->fattr->valid & NFS_ATTR_FATTR))
			continue;
		break;
	}
	return status;
}

/*
 * Use the cached Readdirplus results in order to avoid a LOOKUP call
 * whenever we believe that the parent directory has not changed.
 *
 * We assume that any file creation/rename changes the directory mtime.
 * As this results in a page cache invalidation whenever it occurs,
 * we don't require any other tests for cache coherency.
 */
static
int nfs_cached_lookup(struct inode *dir, struct dentry *dentry,
			struct nfs_fh *fh, struct nfs_fattr *fattr)
{
	nfs_readdir_descriptor_t desc;
	struct nfs_server *server;
	struct nfs_entry entry;
	struct page *page;
	unsigned long timestamp;
	int res;

	if (!NFS_USE_READDIRPLUS(dir))
		return -ENOENT;
	server = NFS_SERVER(dir);
	/* Don't use readdirplus unless the cache is stable */
	if ((server->flags & NFS_MOUNT_NOAC) != 0
			|| nfs_caches_unstable(dir)
			|| nfs_attribute_timeout(dir))
		return -ENOENT;
	if ((NFS_FLAGS(dir) & (NFS_INO_INVALID_ATTR|NFS_INO_INVALID_DATA)) != 0)
		return -ENOENT;
	timestamp = NFS_I(dir)->readdir_timestamp;

	entry.fh = fh;
	entry.fattr = fattr;

	desc.decode = NFS_PROTO(dir)->decode_dirent;
	desc.entry = &entry;
	desc.page_index = 0;
	desc.plus = 1;

	for(;(page = find_get_page(dir->i_mapping, desc.page_index)); desc.page_index++) {

		res = -EIO;
		if (PageUptodate(page)) {
			void * kaddr = kmap_atomic(page, KM_USER0);
			desc.ptr = kaddr;
			res = find_dirent_name(&desc, page, dentry);
			kunmap_atomic(kaddr, KM_USER0);
		}
		page_cache_release(page);

		if (res == 0)
			goto out_found;
		if (res != -EAGAIN)
			break;
	}
	return -ENOENT;
 out_found:
	fattr->timestamp = timestamp;
	return 0;
}

/*
 * Code common to create, mkdir, and mknod.
 */
static int nfs_instantiate(struct dentry *dentry, struct nfs_fh *fhandle,
				struct nfs_fattr *fattr)
{
	struct inode *inode;
	int error = -EACCES;

	/* We may have been initialized further down */
	if (dentry->d_inode)
		return 0;
	if (fhandle->size == 0 || !(fattr->valid & NFS_ATTR_FATTR)) {
		struct inode *dir = dentry->d_parent->d_inode;
		error = NFS_PROTO(dir)->lookup(dir, &dentry->d_name, fhandle, fattr);
		if (error)
			goto out_err;
	}
	inode = nfs_fhget(dentry->d_sb, fhandle, fattr);
	if (inode) {
		d_instantiate(dentry, inode);
		nfs_renew_times(dentry);
		nfs_set_verifier(dentry, nfs_save_change_attribute(dentry->d_parent->d_inode));
		return 0;
	}
	error = -ENOMEM;
out_err:
	d_drop(dentry);
	return error;
}

/*
 * Following a failed create operation, we drop the dentry rather
 * than retain a negative dentry. This avoids a problem in the event
 * that the operation succeeded on the server, but an error in the
 * reply path made it appear to have failed.
 */
static int nfs_create(struct inode *dir, struct dentry *dentry, int mode,
		struct nameidata *nd)
{
	struct iattr attr;
	struct inode *inode;
	int error;
	int open_flags = 0;

	dfprintk(VFS, "NFS: create(%s/%ld, %s\n", dir->i_sb->s_id, 
		dir->i_ino, dentry->d_name.name);

	attr.ia_mode = mode;
	attr.ia_valid = ATTR_MODE;

	if (nd && (nd->flags & LOOKUP_CREATE))
		open_flags = nd->intent.open.flags;

	/*
	 * The 0 argument passed into the create function should one day
	 * contain the O_EXCL flag if requested. This allows NFSv3 to
	 * select the appropriate create strategy. Currently open_namei
	 * does not pass the create flags.
	 */
	lock_kernel();
	nfs_begin_data_update(dir);
	inode = NFS_PROTO(dir)->create(dir, &dentry->d_name, &attr, open_flags);
	nfs_end_data_update(dir);
	if (!IS_ERR(inode)) {
		d_instantiate(dentry, inode);
		nfs_renew_times(dentry);
		nfs_set_verifier(dentry, nfs_save_change_attribute(dir));
		error = 0;
	} else {
		error = PTR_ERR(inode);
		d_drop(dentry);
	}
	unlock_kernel();
	return error;
}

/*
 * See comments for nfs_proc_create regarding failed operations.
 */
static int
nfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t rdev)
{
	struct iattr attr;
	struct nfs_fattr fattr;
	struct nfs_fh fhandle;
	int error;

	dfprintk(VFS, "NFS: mknod(%s/%ld, %s\n", dir->i_sb->s_id,
		dir->i_ino, dentry->d_name.name);

	if (!new_valid_dev(rdev))
		return -EINVAL;

	attr.ia_mode = mode;
	attr.ia_valid = ATTR_MODE;

	lock_kernel();
	nfs_begin_data_update(dir);
	error = NFS_PROTO(dir)->mknod(dir, &dentry->d_name, &attr, rdev,
					&fhandle, &fattr);
	nfs_end_data_update(dir);
	if (!error)
		error = nfs_instantiate(dentry, &fhandle, &fattr);
	else
		d_drop(dentry);
	unlock_kernel();
	return error;
}

/*
 * See comments for nfs_proc_create regarding failed operations.
 */
static int nfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct iattr attr;
	struct nfs_fattr fattr;
	struct nfs_fh fhandle;
	int error;

	dfprintk(VFS, "NFS: mkdir(%s/%ld, %s\n", dir->i_sb->s_id,
		dir->i_ino, dentry->d_name.name);

	attr.ia_valid = ATTR_MODE;
	attr.ia_mode = mode | S_IFDIR;

	lock_kernel();
#if 0
	/*
	 * Always drop the dentry, we can't always depend on
	 * the fattr returned by the server (AIX seems to be
	 * broken). We're better off doing another lookup than
	 * depending on potentially bogus information.
	 */
	d_drop(dentry);
#endif
	nfs_begin_data_update(dir);
	error = NFS_PROTO(dir)->mkdir(dir, &dentry->d_name, &attr, &fhandle,
					&fattr);
	nfs_end_data_update(dir);
	if (!error)
		error = nfs_instantiate(dentry, &fhandle, &fattr);
	else
		d_drop(dentry);
	unlock_kernel();
	return error;
}

static int nfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int error;

	dfprintk(VFS, "NFS: rmdir(%s/%ld, %s\n", dir->i_sb->s_id,
		dir->i_ino, dentry->d_name.name);

	lock_kernel();
	nfs_begin_data_update(dir);
	error = NFS_PROTO(dir)->rmdir(dir, &dentry->d_name);
	/* Ensure the VFS deletes this inode */
	if (error == 0 && dentry->d_inode != NULL)
		dentry->d_inode->i_nlink = 0;
	nfs_end_data_update(dir);
	unlock_kernel();

	return error;
}

static int nfs_sillyrename(struct inode *dir, struct dentry *dentry)
{
	static unsigned int sillycounter;
	const int      i_inosize  = sizeof(dir->i_ino)*2;
	const int      countersize = sizeof(sillycounter)*2;
	const int      slen       = strlen(".nfs") + i_inosize + countersize;
	char           silly[slen+1];
	struct qstr    qsilly;
	struct dentry *sdentry;
	int            error = -EIO;

	dfprintk(VFS, "NFS: silly-rename(%s/%s, ct=%d)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name, 
		atomic_read(&dentry->d_count));

#ifdef NFS_PARANOIA
if (!dentry->d_inode)
printk("NFS: silly-renaming %s/%s, negative dentry??\n",
dentry->d_parent->d_name.name, dentry->d_name.name);
#endif
	/*
	 * We don't allow a dentry to be silly-renamed twice.
	 */
	error = -EBUSY;
	if (dentry->d_flags & DCACHE_NFSFS_RENAMED)
		goto out;

	sprintf(silly, ".nfs%*.*lx",
		i_inosize, i_inosize, dentry->d_inode->i_ino);

	sdentry = NULL;
	do {
		char *suffix = silly + slen - countersize;

		dput(sdentry);
		sillycounter++;
		sprintf(suffix, "%*.*x", countersize, countersize, sillycounter);

		dfprintk(VFS, "trying to rename %s to %s\n",
			 dentry->d_name.name, silly);
		
		sdentry = lookup_one_len(silly, dentry->d_parent, slen);
		/*
		 * N.B. Better to return EBUSY here ... it could be
		 * dangerous to delete the file while it's in use.
		 */
		if (IS_ERR(sdentry))
			goto out;
	} while(sdentry->d_inode != NULL); /* need negative lookup */

	qsilly.name = silly;
	qsilly.len  = strlen(silly);
	nfs_begin_data_update(dir);
	if (dentry->d_inode) {
		nfs_begin_data_update(dentry->d_inode);
		error = NFS_PROTO(dir)->rename(dir, &dentry->d_name,
				dir, &qsilly);
		nfs_end_data_update(dentry->d_inode);
	} else
		error = NFS_PROTO(dir)->rename(dir, &dentry->d_name,
				dir, &qsilly);
	nfs_end_data_update(dir);
	if (!error) {
		nfs_renew_times(dentry);
		nfs_set_verifier(dentry, nfs_save_change_attribute(dir));
		d_move(dentry, sdentry);
		error = nfs_async_unlink(dentry);
 		/* If we return 0 we don't unlink */
	}
	dput(sdentry);
out:
	return error;
}

/*
 * Remove a file after making sure there are no pending writes,
 * and after checking that the file has only one user. 
 *
 * We invalidate the attribute cache and free the inode prior to the operation
 * to avoid possible races if the server reuses the inode.
 */
static int nfs_safe_remove(struct dentry *dentry)
{
	struct inode *dir = dentry->d_parent->d_inode;
	struct inode *inode = dentry->d_inode;
	int error = -EBUSY;
		
	dfprintk(VFS, "NFS: safe_remove(%s/%s)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name);

	/* If the dentry was sillyrenamed, we simply call d_delete() */
	if (dentry->d_flags & DCACHE_NFSFS_RENAMED) {
		error = 0;
		goto out;
	}

	nfs_begin_data_update(dir);
	if (inode != NULL) {
		nfs_begin_data_update(inode);
		error = NFS_PROTO(dir)->remove(dir, &dentry->d_name);
		/* The VFS may want to delete this inode */
		if (error == 0)
			inode->i_nlink--;
		nfs_end_data_update(inode);
	} else
		error = NFS_PROTO(dir)->remove(dir, &dentry->d_name);
	nfs_end_data_update(dir);
out:
	return error;
}

/*  We do silly rename. In case sillyrename() returns -EBUSY, the inode
 *  belongs to an active ".nfs..." file and we return -EBUSY.
 *
 *  If sillyrename() returns 0, we do nothing, otherwise we unlink.
 */
static int nfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int error;
	int need_rehash = 0;

	dfprintk(VFS, "NFS: unlink(%s/%ld, %s)\n", dir->i_sb->s_id,
		dir->i_ino, dentry->d_name.name);

	lock_kernel();
	spin_lock(&dcache_lock);
	spin_lock(&dentry->d_lock);
	if (atomic_read(&dentry->d_count) > 1) {
		spin_unlock(&dentry->d_lock);
		spin_unlock(&dcache_lock);
		error = nfs_sillyrename(dir, dentry);
		unlock_kernel();
		return error;
	}
	if (!d_unhashed(dentry)) {
		__d_drop(dentry);
		need_rehash = 1;
	}
	spin_unlock(&dentry->d_lock);
	spin_unlock(&dcache_lock);
	error = nfs_safe_remove(dentry);
	if (!error) {
		nfs_renew_times(dentry);
		nfs_set_verifier(dentry, nfs_save_change_attribute(dir));
	} else if (need_rehash)
		d_rehash(dentry);
	unlock_kernel();
	return error;
}

static int
nfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct iattr attr;
	struct nfs_fattr sym_attr;
	struct nfs_fh sym_fh;
	struct qstr qsymname;
	int error;

	dfprintk(VFS, "NFS: symlink(%s/%ld, %s, %s)\n", dir->i_sb->s_id,
		dir->i_ino, dentry->d_name.name, symname);

	error = -ENAMETOOLONG;
	switch (NFS_PROTO(dir)->version) {
		case 2:
			if (strlen(symname) > NFS2_MAXPATHLEN)
				goto out;
			break;
		case 3:
			if (strlen(symname) > NFS3_MAXPATHLEN)
				goto out;
		default:
			break;
	}

#ifdef NFS_PARANOIA
if (dentry->d_inode)
printk("nfs_proc_symlink: %s/%s not negative!\n",
dentry->d_parent->d_name.name, dentry->d_name.name);
#endif
	/*
	 * Fill in the sattr for the call.
 	 * Note: SunOS 4.1.2 crashes if the mode isn't initialized!
	 */
	attr.ia_valid = ATTR_MODE;
	attr.ia_mode = S_IFLNK | S_IRWXUGO;

	qsymname.name = symname;
	qsymname.len  = strlen(symname);

	lock_kernel();
	nfs_begin_data_update(dir);
	error = NFS_PROTO(dir)->symlink(dir, &dentry->d_name, &qsymname,
					  &attr, &sym_fh, &sym_attr);
	nfs_end_data_update(dir);
	if (!error) {
		error = nfs_instantiate(dentry, &sym_fh, &sym_attr);
	} else {
		if (error == -EEXIST)
			printk("nfs_proc_symlink: %s/%s already exists??\n",
			       dentry->d_parent->d_name.name, dentry->d_name.name);
		d_drop(dentry);
	}
	unlock_kernel();

out:
	return error;
}

static int 
nfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;
	int error;

	dfprintk(VFS, "NFS: link(%s/%s -> %s/%s)\n",
		old_dentry->d_parent->d_name.name, old_dentry->d_name.name,
		dentry->d_parent->d_name.name, dentry->d_name.name);

	/*
	 * Drop the dentry in advance to force a new lookup.
	 * Since nfs_proc_link doesn't return a file handle,
	 * we can't use the existing dentry.
	 */
	lock_kernel();
	d_drop(dentry);

	nfs_begin_data_update(dir);
	nfs_begin_data_update(inode);
	error = NFS_PROTO(dir)->link(inode, dir, &dentry->d_name);
	nfs_end_data_update(inode);
	nfs_end_data_update(dir);
	unlock_kernel();
	return error;
}

/*
 * RENAME
 * FIXME: Some nfsds, like the Linux user space nfsd, may generate a
 * different file handle for the same inode after a rename (e.g. when
 * moving to a different directory). A fail-safe method to do so would
 * be to look up old_dir/old_name, create a link to new_dir/new_name and
 * rename the old file using the sillyrename stuff. This way, the original
 * file in old_dir will go away when the last process iput()s the inode.
 *
 * FIXED.
 * 
 * It actually works quite well. One needs to have the possibility for
 * at least one ".nfs..." file in each directory the file ever gets
 * moved or linked to which happens automagically with the new
 * implementation that only depends on the dcache stuff instead of
 * using the inode layer
 *
 * Unfortunately, things are a little more complicated than indicated
 * above. For a cross-directory move, we want to make sure we can get
 * rid of the old inode after the operation.  This means there must be
 * no pending writes (if it's a file), and the use count must be 1.
 * If these conditions are met, we can drop the dentries before doing
 * the rename.
 */
static int nfs_rename(struct inode *old_dir, struct dentry *old_dentry,
		      struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	struct dentry *dentry = NULL, *rehash = NULL;
	int error = -EBUSY;

	/*
	 * To prevent any new references to the target during the rename,
	 * we unhash the dentry and free the inode in advance.
	 */
	lock_kernel();
	if (!d_unhashed(new_dentry)) {
		d_drop(new_dentry);
		rehash = new_dentry;
	}

	dfprintk(VFS, "NFS: rename(%s/%s -> %s/%s, ct=%d)\n",
		 old_dentry->d_parent->d_name.name, old_dentry->d_name.name,
		 new_dentry->d_parent->d_name.name, new_dentry->d_name.name,
		 atomic_read(&new_dentry->d_count));

	/*
	 * First check whether the target is busy ... we can't
	 * safely do _any_ rename if the target is in use.
	 *
	 * For files, make a copy of the dentry and then do a 
	 * silly-rename. If the silly-rename succeeds, the
	 * copied dentry is hashed and becomes the new target.
	 */
	if (!new_inode)
		goto go_ahead;
	if (S_ISDIR(new_inode->i_mode))
		goto out;
	else if (atomic_read(&new_dentry->d_count) > 1) {
		int err;
		/* copy the target dentry's name */
		dentry = d_alloc(new_dentry->d_parent,
				 &new_dentry->d_name);
		if (!dentry)
			goto out;

		/* silly-rename the existing target ... */
		err = nfs_sillyrename(new_dir, new_dentry);
		if (!err) {
			new_dentry = rehash = dentry;
			new_inode = NULL;
			/* instantiate the replacement target */
			d_instantiate(new_dentry, NULL);
		}

		/* dentry still busy? */
		if (atomic_read(&new_dentry->d_count) > 1) {
#ifdef NFS_PARANOIA
			printk("nfs_rename: target %s/%s busy, d_count=%d\n",
			       new_dentry->d_parent->d_name.name,
			       new_dentry->d_name.name,
			       atomic_read(&new_dentry->d_count));
#endif
			goto out;
		}
	}

go_ahead:
	/*
	 * ... prune child dentries and writebacks if needed.
	 */
	if (atomic_read(&old_dentry->d_count) > 1) {
		nfs_wb_all(old_inode);
		shrink_dcache_parent(old_dentry);
	}

	if (new_inode)
		d_delete(new_dentry);

	nfs_begin_data_update(old_dir);
	nfs_begin_data_update(new_dir);
	nfs_begin_data_update(old_inode);
	error = NFS_PROTO(old_dir)->rename(old_dir, &old_dentry->d_name,
					   new_dir, &new_dentry->d_name);
	nfs_end_data_update(old_inode);
	nfs_end_data_update(new_dir);
	nfs_end_data_update(old_dir);
out:
	if (rehash)
		d_rehash(rehash);
	if (!error) {
		if (!S_ISDIR(old_inode->i_mode))
			d_move(old_dentry, new_dentry);
		nfs_renew_times(new_dentry);
		nfs_set_verifier(new_dentry, nfs_save_change_attribute(new_dir));
	}

	/* new dentry created? */
	if (dentry)
		dput(dentry);
	unlock_kernel();
	return error;
}

int
nfs_permission(struct inode *inode, int mask, struct nameidata *nd)
{
	struct nfs_access_cache *cache = &NFS_I(inode)->cache_access;
	struct rpc_cred *cred;
	int mode = inode->i_mode;
	int res;

	if (mask == 0)
		return 0;
	if (mask & MAY_WRITE) {
		/*
		 *
		 * Nobody gets write access to a read-only fs.
		 *
		 */
		if (IS_RDONLY(inode) &&
		    (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)))
			return -EROFS;

		/*
		 *
		 * Nobody gets write access to an immutable file.
		 *
		 */
		if (IS_IMMUTABLE(inode))
			return -EACCES;
	}
	/* Are we checking permissions on anything other than lookup/execute? */
	if ((mask & MAY_EXEC) == 0) {
		/* We only need to check permissions on file open() and access() */
		if (!nd || !(nd->flags & (LOOKUP_OPEN|LOOKUP_ACCESS)))
			return 0;
		/* NFSv4 has atomic_open... */
		if (NFS_PROTO(inode)->version > 3 && (nd->flags & LOOKUP_OPEN))
			return 0;
	}

	lock_kernel();

	if (!NFS_PROTO(inode)->access)
		goto out_notsup;

	cred = rpcauth_lookupcred(NFS_CLIENT(inode)->cl_auth, 0);
	if (cache->cred == cred
	    && time_before(jiffies, cache->jiffies + NFS_ATTRTIMEO(inode))
	    && !(NFS_FLAGS(inode) & NFS_INO_INVALID_ATTR)) {
		if (!(res = cache->err)) {
			/* Is the mask a subset of an accepted mask? */
			if ((cache->mask & mask) == mask)
				goto out;
		} else {
			/* ...or is it a superset of a rejected mask? */
			if ((cache->mask & mask) == cache->mask)
				goto out;
		}
	}

	res = NFS_PROTO(inode)->access(inode, cred, mask);
	if (!res || res == -EACCES)
		goto add_cache;
out:
	put_rpccred(cred);
	unlock_kernel();
	return res;
out_notsup:
	nfs_revalidate_inode(NFS_SERVER(inode), inode);
	res = vfs_permission(inode, mask);
	unlock_kernel();
	return res;
add_cache:
	cache->jiffies = jiffies;
	if (cache->cred)
		put_rpccred(cache->cred);
	cache->cred = cred;
	cache->mask = mask;
	cache->err = res;
	unlock_kernel();
	return res;
}

/*
 * Local variables:
 *  version-control: t
 *  kept-new-versions: 5
 * End:
 */
