/*
 *  linux/fs/nfs/symlink.c
 *
 *  Copyright (C) 1992  Rick Sladkey
 *
 *  Optimization changes Copyright (C) 1994 Florian La Roche
 *
 *  Jun 7 1999, cache symlink lookups in the page cache.  -DaveM
 *
 *  nfs symlink handling code
 */

#define NFS_NEED_XDR_TYPES
#include <linux/time.h>
#include <linux/errno.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs.h>
#include <linux/nfs2.h>
#include <linux/nfs_fs.h>
#include <linux/pagemap.h>
#include <linux/stat.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/smp_lock.h>

/* Symlink caching in the page cache is even more simplistic
 * and straight-forward than readdir caching.
 */
static int nfs_symlink_filler(struct inode *inode, struct page *page)
{
	int error;

	/* We place the length at the beginning of the page,
	 * in host byte order, followed by the string.  The
	 * XDR response verification will NULL terminate it.
	 */
	lock_kernel();
	error = NFS_PROTO(inode)->readlink(inode, page);
	unlock_kernel();
	if (error < 0)
		goto error;
	SetPageUptodate(page);
	unlock_page(page);
	return 0;

error:
	SetPageError(page);
	unlock_page(page);
	return -EIO;
}

static char *nfs_getlink(struct inode *inode, struct page **ppage)
{
	struct page *page;
	u32 *p;

	page = ERR_PTR(nfs_revalidate_inode(NFS_SERVER(inode), inode));
	if (page)
		goto read_failed;
	page = read_cache_page(&inode->i_data, 0,
				(filler_t *)nfs_symlink_filler, inode);
	if (IS_ERR(page))
		goto read_failed;
	if (!PageUptodate(page))
		goto getlink_read_error;
	*ppage = page;
	p = kmap(page);
	return (char*)(p+1);

getlink_read_error:
	page_cache_release(page);
	page = ERR_PTR(-EIO);
read_failed:
	return (char*)page;
}

static int nfs_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct inode *inode = dentry->d_inode;
	struct page *page = NULL;
	int res = vfs_readlink(dentry,buffer,buflen,nfs_getlink(inode,&page));
	if (page) {
		kunmap(page);
		page_cache_release(page);
	}
	return res;
}

static int nfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode = dentry->d_inode;
	struct page *page = NULL;
	int res = vfs_follow_link(nd, nfs_getlink(inode,&page));
	if (page) {
		kunmap(page);
		page_cache_release(page);
	}
	return res;
}

/*
 * symlinks can't do much...
 */
struct inode_operations nfs_symlink_inode_operations = {
	.readlink	= nfs_readlink,
	.follow_link	= nfs_follow_link,
	.getattr	= nfs_getattr,
	.setattr	= nfs_setattr,
};
