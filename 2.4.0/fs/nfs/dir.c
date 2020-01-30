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

#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/malloc.h>
#include <linux/mm.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_mount.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>

#define NFS_PARANOIA 1
/* #define NFS_DEBUG_VERBOSE 1 */

static int nfs_readdir(struct file *, void *, filldir_t);
static struct dentry *nfs_lookup(struct inode *, struct dentry *);
static int nfs_create(struct inode *, struct dentry *, int);
static int nfs_mkdir(struct inode *, struct dentry *, int);
static int nfs_rmdir(struct inode *, struct dentry *);
static int nfs_unlink(struct inode *, struct dentry *);
static int nfs_symlink(struct inode *, struct dentry *, const char *);
static int nfs_link(struct dentry *, struct inode *, struct dentry *);
static int nfs_mknod(struct inode *, struct dentry *, int, int);
static int nfs_rename(struct inode *, struct dentry *,
		      struct inode *, struct dentry *);

struct file_operations nfs_dir_operations = {
	read:		generic_read_dir,
	readdir:	nfs_readdir,
	open:		nfs_open,
	release:	nfs_release,
};

struct inode_operations nfs_dir_inode_operations = {
	create:		nfs_create,
	lookup:		nfs_lookup,
	link:		nfs_link,
	unlink:		nfs_unlink,
	symlink:	nfs_symlink,
	mkdir:		nfs_mkdir,
	rmdir:		nfs_rmdir,
	mknod:		nfs_mknod,
	rename:		nfs_rename,
	permission:	nfs_permission,
	revalidate:	nfs_revalidate,
	setattr:	nfs_notify_change,
};

typedef u32 * (*decode_dirent_t)(u32 *, struct nfs_entry *, int);
typedef struct {
	struct file	*file;
	struct page	*page;
	unsigned long	page_index;
	unsigned	page_offset;
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
	void		*buffer = kmap(page);
	int		plus = NFS_USE_READDIRPLUS(inode);
	int		error;

	dfprintk(VFS, "NFS: nfs_readdir_filler() reading cookie %Lu into page %lu.\n", (long long)desc->entry->cookie, page->index);

 again:
	error = NFS_PROTO(inode)->readdir(inode, cred, desc->entry->cookie, buffer,
					  NFS_SERVER(inode)->dtsize, plus);
	/* We requested READDIRPLUS, but the server doesn't grok it */
	if (desc->plus && error == -ENOTSUPP) {
		NFS_FLAGS(inode) &= ~NFS_INO_ADVISE_RDPLUS;
		plus = 0;
		goto again;
	}
	if (error < 0)
		goto error;
	SetPageUptodate(page);
	kunmap(page);
	/* Ensure consistent page alignment of the data.
	 * Note: assumes we have exclusive access to this mapping either
	 *	 throught inode->i_sem or some other mechanism.
	 */
	if (page->index == 0)
		invalidate_inode_pages(inode);
	UnlockPage(page);
	return 0;
 error:
	SetPageError(page);
	kunmap(page);
	UnlockPage(page);
	invalidate_inode_pages(inode);
	desc->error = error;
	return -EIO;
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
	char		*start = kmap(page),
			*p = start;
	int		loop_count = 0,
			status = 0;

	for(;;) {
		p = (char *)desc->decode((u32*)p, entry, desc->plus);
		if (IS_ERR(p)) {
			status = PTR_ERR(p);
			break;
		}
		desc->page_offset = p - start;
		dfprintk(VFS, "NFS: found cookie %Lu\n", (long long)entry->cookie);
		if (entry->prev_cookie == desc->target)
			break;
		if (loop_count++ > 200) {
			loop_count = 0;
			schedule();
		}
	}
	kunmap(page);
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
	unsigned long	index = desc->page_index;
	int		status;

	dfprintk(VFS, "NFS: find_dirent_page() searching directory page %ld\n", desc->page_index);

	if (desc->page) {
		page_cache_release(desc->page);
		desc->page = NULL;
	}

	page = read_cache_page(&inode->i_data, index,
			       (filler_t *)nfs_readdir_filler, desc);
	if (IS_ERR(page)) {
		status = PTR_ERR(page);
		goto out;
	}
	if (!Page_Uptodate(page))
		goto read_error;

	/* NOTE: Someone else may have changed the READDIRPLUS flag */
	desc->plus = NFS_USE_READDIRPLUS(inode);
	status = find_dirent(desc, page);
	if (status >= 0)
		desc->page = page;
	else
		page_cache_release(page);
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
	int		res = 0;
	int		loop_count = 0;

	dfprintk(VFS, "NFS: readdir_search_pagecache() searching for cookie %Lu\n", (long long)desc->target);
	for (;;) {
		res = find_dirent_page(desc);
		if (res != -EAGAIN)
			break;
		/* Align to beginning of next page */
		desc->page_offset = 0;
		desc->page_index ++;
		if (loop_count++ > 200) {
			loop_count = 0;
			schedule();
		}
	}
	dfprintk(VFS, "NFS: readdir_search_pagecache() returned %d\n", res);
	return res;
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
	char		*start = kmap(desc->page),
			*p = start + desc->page_offset;
	unsigned long	fileid;
	int		loop_count = 0,
			res = 0;

	dfprintk(VFS, "NFS: nfs_do_filldir() filling starting @ cookie %Lu\n", (long long)desc->target);

	for(;;) {
		/* Note: entry->prev_cookie contains the cookie for
		 *	 retrieving the current dirent on the server */
		fileid = nfs_fileid_to_ino_t(entry->ino);
		res = filldir(dirent, entry->name, entry->len, 
			      entry->prev_cookie, fileid, DT_UNKNOWN);
		if (res < 0)
			break;
		file->f_pos = desc->target = entry->cookie;
		p = (char *)desc->decode((u32 *)p, entry, desc->plus);
		if (IS_ERR(p)) {
			if (PTR_ERR(p) == -EAGAIN) {
				desc->page_offset = 0;
				desc->page_index ++;
			}
			break;
		}
		desc->page_offset = p - start;
		if (loop_count++ > 200) {
			loop_count = 0;
			schedule();
		}
	}
	kunmap(desc->page);
	page_cache_release(desc->page);
	desc->page = NULL;

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
	u32		*p;
	int		status = -EIO;

	dfprintk(VFS, "NFS: uncached_readdir() searching for cookie %Lu\n", (long long)desc->target);
	if (desc->page) {
		page_cache_release(desc->page);
		desc->page = NULL;
	}

	page = page_cache_alloc();
	if (!page) {
		status = -ENOMEM;
		goto out;
	}
	p = kmap(page);
	status = NFS_PROTO(inode)->readdir(inode, cred, desc->target, p,
					   NFS_SERVER(inode)->dtsize, 0);
	if (status >= 0) {
		p = desc->decode(p, desc->entry, 0);
		if (IS_ERR(p))
			status = PTR_ERR(p);
		else
			desc->entry->prev_cookie = desc->target;
	}
	kunmap(page);
	if (status < 0)
		goto out_release;

	desc->page_index = 0;
	desc->page_offset = 0;
	desc->page = page;
	status = nfs_do_filldir(desc, dirent, filldir);

	/* Reset read descriptor so it searches the page cache from
	 * the start upon the next call to readdir_search_pagecache() */
	desc->page_index = 0;
	desc->page_offset = 0;
	memset(desc->entry, 0, sizeof(*desc->entry));
 out:
	dfprintk(VFS, "NFS: uncached_readdir() returns %d\n", status);
	return status;
 out_release:
	page_cache_release(page);
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
	long		res;

	res = nfs_revalidate(dentry);
	if (res < 0)
		return res;

	/*
	 * filp->f_pos points to the file offset in the page cache.
	 * but if the cache has meanwhile been zapped, we need to
	 * read from the last dirent to revalidate f_pos
	 * itself.
	 */
	memset(desc, 0, sizeof(*desc));
	memset(&my_entry, 0, sizeof(my_entry));

	desc->file = filp;
	desc->target = filp->f_pos;
	desc->entry = &my_entry;
	desc->decode = NFS_PROTO(inode)->decode_dirent;

	while(!desc->entry->eof) {
		res = readdir_search_pagecache(desc);
		if (res == -EBADCOOKIE) {
			/* This means either end of directory */
			if (desc->entry->cookie == desc->target) {
				res = 0;
				break;
			}
			/* Or that the server has 'lost' a cookie */
			res = uncached_readdir(desc, dirent, filldir);
			if (res >= 0)
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
	if (desc->page)
		page_cache_release(desc->page);
	if (desc->error < 0)
		return desc->error;
	if (res < 0)
		return res;
	return 0;
}

/*
 * Whenever an NFS operation succeeds, we know that the dentry
 * is valid, so we update the revalidation timestamp.
 */
static inline void nfs_renew_times(struct dentry * dentry)
{
	dentry->d_time = jiffies;
}

static inline int nfs_dentry_force_reval(struct dentry *dentry, int flags)
{
	struct inode *inode = dentry->d_inode;
	unsigned long timeout = NFS_ATTRTIMEO(inode);

	/*
	 * If it's the last lookup in a series, we use a stricter
	 * cache consistency check by looking at the parent mtime.
	 *
	 * If it's been modified in the last hour, be really strict.
	 * (This still means that we can avoid doing unnecessary
	 * work on directories like /usr/share/bin etc which basically
	 * never change).
	 */
	if (!(flags & LOOKUP_CONTINUE)) {
		long diff = CURRENT_TIME - dentry->d_parent->d_inode->i_mtime;

		if (diff < 15*60)
			timeout = 0;
	}
	
	return time_after(jiffies,dentry->d_time + timeout);
}

/*
 * We judge how long we want to trust negative
 * dentries by looking at the parent inode mtime.
 *
 * If mtime is close to present time, we revalidate
 * more often.
 */
#define NFS_REVALIDATE_NEGATIVE (1 * HZ)
static inline int nfs_neg_need_reval(struct dentry *dentry)
{
	struct inode *dir = dentry->d_parent->d_inode;
	unsigned long timeout = NFS_ATTRTIMEO(dir);
	long diff = CURRENT_TIME - dir->i_mtime;

	if (diff < 5*60 && timeout > NFS_REVALIDATE_NEGATIVE)
		timeout = NFS_REVALIDATE_NEGATIVE;

	return time_after(jiffies, dentry->d_time + timeout);
}

/*
 * This is called every time the dcache has a lookup hit,
 * and we should check whether we can really trust that
 * lookup.
 *
 * NOTE! The hit can be a negative hit too, don't assume
 * we have an inode!
 *
 * If the dentry is older than the revalidation interval, 
 * we do a new lookup and verify that the dentry is still
 * correct.
 */
static int nfs_lookup_revalidate(struct dentry * dentry, int flags)
{
	struct inode *dir;
	struct inode *inode;
	int error;
	struct nfs_fh fhandle;
	struct nfs_fattr fattr;

	lock_kernel();
	dir = dentry->d_parent->d_inode;
	inode = dentry->d_inode;
	/*
	 * If we don't have an inode, let's look at the parent
	 * directory mtime to get a hint about how often we
	 * should validate things..
	 */
	if (!inode) {
		if (nfs_neg_need_reval(dentry))
			goto out_bad;
		goto out_valid;
	}

	if (is_bad_inode(inode)) {
		dfprintk(VFS, "nfs_lookup_validate: %s/%s has dud inode\n",
			dentry->d_parent->d_name.name, dentry->d_name.name);
		goto out_bad;
	}

	if (!nfs_dentry_force_reval(dentry, flags))
		goto out_valid;

	if (IS_ROOT(dentry)) {
		__nfs_revalidate_inode(NFS_SERVER(inode), inode);
		goto out_valid_renew;
	}

	/*
	 * Do a new lookup and check the dentry attributes.
	 */
	error = NFS_PROTO(dir)->lookup(dir, &dentry->d_name, &fhandle, &fattr);
	if (error)
		goto out_bad;

	/* Inode number matches? */
	if (!(fattr.valid & NFS_ATTR_FATTR) ||
	    NFS_FSID(inode) != fattr.fsid ||
	    NFS_FILEID(inode) != fattr.fileid)
		goto out_bad;

	/* Ok, remember that we successfully checked it.. */
	nfs_refresh_inode(inode, &fattr);

	if (nfs_inode_is_stale(inode, &fhandle, &fattr))
		goto out_bad;

 out_valid_renew:
	nfs_renew_times(dentry);
out_valid:
	unlock_kernel();
	return 1;
out_bad:
	shrink_dcache_parent(dentry);
	/* If we have submounts, don't unhash ! */
	if (have_submounts(dentry))
		goto out_valid;
	d_drop(dentry);
	/* Purge readdir caches. */
	nfs_zap_caches(dir);
	if (inode && S_ISDIR(inode->i_mode))
		nfs_zap_caches(inode);
	unlock_kernel();
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
		nfs_complete_unlink(dentry);
		unlock_kernel();
	}
	iput(inode);
}

struct dentry_operations nfs_dentry_operations = {
	d_revalidate:	nfs_lookup_revalidate,
	d_delete:	nfs_dentry_delete,
	d_iput:		nfs_dentry_iput,
};

static struct dentry *nfs_lookup(struct inode *dir, struct dentry * dentry)
{
	struct inode *inode;
	int error;
	struct nfs_fh fhandle;
	struct nfs_fattr fattr;

	dfprintk(VFS, "NFS: lookup(%s/%s)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name);

	error = -ENAMETOOLONG;
	if (dentry->d_name.len > NFS_SERVER(dir)->namelen)
		goto out;

	error = -ENOMEM;
	dentry->d_op = &nfs_dentry_operations;

	error = NFS_PROTO(dir)->lookup(dir, &dentry->d_name, &fhandle, &fattr);
	inode = NULL;
	if (error == -ENOENT)
		goto no_entry;
	if (!error) {
		error = -EACCES;
		inode = nfs_fhget(dentry, &fhandle, &fattr);
		if (inode) {
	    no_entry:
			d_add(dentry, inode);
			nfs_renew_times(dentry);
			error = 0;
		}
	}
out:
	return ERR_PTR(error);
}

/*
 * Code common to create, mkdir, and mknod.
 */
static int nfs_instantiate(struct dentry *dentry, struct nfs_fh *fhandle,
				struct nfs_fattr *fattr)
{
	struct inode *inode;
	int error = -EACCES;

	inode = nfs_fhget(dentry, fhandle, fattr);
	if (inode) {
		d_instantiate(dentry, inode);
		nfs_renew_times(dentry);
		error = 0;
	}
	return error;
}

/*
 * Following a failed create operation, we drop the dentry rather
 * than retain a negative dentry. This avoids a problem in the event
 * that the operation succeeded on the server, but an error in the
 * reply path made it appear to have failed.
 */
static int nfs_create(struct inode *dir, struct dentry *dentry, int mode)
{
	struct iattr attr;
	struct nfs_fattr fattr;
	struct nfs_fh fhandle;
	int error;

	dfprintk(VFS, "NFS: create(%x/%ld, %s\n",
		dir->i_dev, dir->i_ino, dentry->d_name.name);

	attr.ia_mode = mode;
	attr.ia_valid = ATTR_MODE;

	/*
	 * The 0 argument passed into the create function should one day
	 * contain the O_EXCL flag if requested. This allows NFSv3 to
	 * select the appropriate create strategy. Currently open_namei
	 * does not pass the create flags.
	 */
	nfs_zap_caches(dir);
	error = NFS_PROTO(dir)->create(dir, &dentry->d_name,
					 &attr, 0, &fhandle, &fattr);
	if (!error && fhandle.size != 0)
		error = nfs_instantiate(dentry, &fhandle, &fattr);
	if (error || fhandle.size == 0)
		d_drop(dentry);
	return error;
}

/*
 * See comments for nfs_proc_create regarding failed operations.
 */
static int nfs_mknod(struct inode *dir, struct dentry *dentry, int mode, int rdev)
{
	struct iattr attr;
	struct nfs_fattr fattr;
	struct nfs_fh fhandle;
	int error;

	dfprintk(VFS, "NFS: mknod(%x/%ld, %s\n",
		dir->i_dev, dir->i_ino, dentry->d_name.name);

	attr.ia_mode = mode;
	attr.ia_valid = ATTR_MODE;

	nfs_zap_caches(dir);
	error = NFS_PROTO(dir)->mknod(dir, &dentry->d_name, &attr, rdev,
					&fhandle, &fattr);
	if (!error && fhandle.size != 0)
		error = nfs_instantiate(dentry, &fhandle, &fattr);
	if (error || fhandle.size == 0)
		d_drop(dentry);
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

	dfprintk(VFS, "NFS: mkdir(%x/%ld, %s\n",
		dir->i_dev, dir->i_ino, dentry->d_name.name);

	attr.ia_valid = ATTR_MODE;
	attr.ia_mode = mode | S_IFDIR;

#if 0
	/*
	 * Always drop the dentry, we can't always depend on
	 * the fattr returned by the server (AIX seems to be
	 * broken). We're better off doing another lookup than
	 * depending on potentially bogus information.
	 */
	d_drop(dentry);
#endif
	nfs_zap_caches(dir);
	error = NFS_PROTO(dir)->mkdir(dir, &dentry->d_name, &attr, &fhandle,
					&fattr);
	if (!error && fhandle.size != 0)
		error = nfs_instantiate(dentry, &fhandle, &fattr);
	if (error || fhandle.size == 0)
		d_drop(dentry);
	return error;
}

static int nfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int error;

	dfprintk(VFS, "NFS: rmdir(%x/%ld, %s\n",
		dir->i_dev, dir->i_ino, dentry->d_name.name);

	nfs_zap_caches(dir);
	error = NFS_PROTO(dir)->rmdir(dir, &dentry->d_name);

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

	if (atomic_read(&dentry->d_count) == 1)
		goto out;  /* No need to silly rename. */


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
		
		sdentry = lookup_one(silly, dentry->d_parent);
		/*
		 * N.B. Better to return EBUSY here ... it could be
		 * dangerous to delete the file while it's in use.
		 */
		if (IS_ERR(sdentry))
			goto out;
	} while(sdentry->d_inode != NULL); /* need negative lookup */

	nfs_zap_caches(dir);
	qsilly.name = silly;
	qsilly.len  = strlen(silly);
	error = NFS_PROTO(dir)->rename(dir, &dentry->d_name, dir, &qsilly);
	if (!error) {
		nfs_renew_times(dentry);
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
	int error = -EBUSY, rehash = 0;
		
	dfprintk(VFS, "NFS: safe_remove(%s/%s)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name);

	/*
	 * Unhash the dentry while we remove the file ...
	 */
	if (!d_unhashed(dentry)) {
		d_drop(dentry);
		rehash = 1;
	}
	if (atomic_read(&dentry->d_count) > 1) {
#ifdef NFS_PARANOIA
		printk("nfs_safe_remove: %s/%s busy, d_count=%d\n",
			dentry->d_parent->d_name.name, dentry->d_name.name,
			atomic_read(&dentry->d_count));
#endif
		goto out;
	}

	/* If the dentry was sillyrenamed, we simply call d_delete() */
	if (dentry->d_flags & DCACHE_NFSFS_RENAMED) {
		error = 0;
		goto out_delete;
	}

	nfs_zap_caches(dir);
	if (inode)
		NFS_CACHEINV(inode);
	error = NFS_PROTO(dir)->remove(dir, &dentry->d_name);
	if (error < 0)
		goto out;

 out_delete:
	/*
	 * Free the inode
	 */
	d_delete(dentry);
out:
	if (rehash)
		d_rehash(dentry);
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

	dfprintk(VFS, "NFS: unlink(%x/%ld, %s)\n",
		dir->i_dev, dir->i_ino, dentry->d_name.name);

	error = nfs_sillyrename(dir, dentry);
	if (error && error != -EBUSY) {
		error = nfs_safe_remove(dentry);
		if (!error) {
			nfs_renew_times(dentry);
		}
	}
	return error;
}

static int
nfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct iattr attr;
	struct nfs_fattr sym_attr;
	struct nfs_fh sym_fh;
	struct qstr qsymname;
	unsigned int maxlen;
	int error;

	dfprintk(VFS, "NFS: symlink(%x/%ld, %s, %s)\n",
		dir->i_dev, dir->i_ino, dentry->d_name.name, symname);

	error = -ENAMETOOLONG;
	maxlen = (NFS_PROTO(dir)->version==2) ? NFS2_MAXPATHLEN : NFS3_MAXPATHLEN;
	if (strlen(symname) > maxlen)
		goto out;

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

	nfs_zap_caches(dir);
	error = NFS_PROTO(dir)->symlink(dir, &dentry->d_name, &qsymname,
					  &attr, &sym_fh, &sym_attr);
	if (!error && sym_fh.size != 0 && (sym_attr.valid & NFS_ATTR_FATTR)) {
		error = nfs_instantiate(dentry, &sym_fh, &sym_attr);
	} else {
		if (error == -EEXIST)
			printk("nfs_proc_symlink: %s/%s already exists??\n",
			       dentry->d_parent->d_name.name, dentry->d_name.name);
		d_drop(dentry);
	}

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
	d_drop(dentry);
	nfs_zap_caches(dir);
	NFS_CACHEINV(inode);
	error = NFS_PROTO(dir)->link(inode, dir, &dentry->d_name);
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

	nfs_zap_caches(new_dir);
	nfs_zap_caches(old_dir);
	error = NFS_PROTO(old_dir)->rename(old_dir, &old_dentry->d_name,
					   new_dir, &new_dentry->d_name);
out:
	if (rehash)
		d_rehash(rehash);
	if (!error && !S_ISDIR(old_inode->i_mode))
		d_move(old_dentry, new_dentry);

	/* new dentry created? */
	if (dentry)
		dput(dentry);
	return error;
}

int
nfs_permission(struct inode *inode, int mask)
{
	int			error = vfs_permission(inode, mask);

	if (!NFS_PROTO(inode)->access)
		goto out;
	/*
	 * Trust UNIX mode bits except:
	 *
	 * 1) When override capabilities may have been invoked
	 * 2) When root squashing may be involved
	 * 3) When ACLs may overturn a negative answer */
	if (!capable(CAP_DAC_OVERRIDE) && !capable(CAP_DAC_READ_SEARCH)
	    && (current->fsuid != 0) && (current->fsgid != 0)
	    && error != -EACCES)
		goto out;

	error = NFS_PROTO(inode)->access(inode, mask, 0);

	if (error == -EACCES && NFS_CLIENT(inode)->cl_droppriv &&
	    current->uid != 0 && current->gid != 0 &&
	    (current->fsuid != current->uid || current->fsgid != current->gid))
		error = NFS_PROTO(inode)->access(inode, mask, 1);

 out:
	return error;
}

/*
 * Local variables:
 *  version-control: t
 *  kept-new-versions: 5
 * End:
 */
