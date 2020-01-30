/*
 * Symlink inode operations for Coda filesystem
 * Original version: (C) 1996 P. Braam and M. Callahan
 * Rewritten for Linux 2.1. (C) 1997 Carnegie Mellon University
 * 
 * Carnegie Mellon encourages users to contribute improvements to
 * the Coda project. Contact Peter Braam (coda@cs.cmu.edu).
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/locks.h>
#include <linux/smp_lock.h>

#include <linux/coda.h>
#include <linux/coda_linux.h>
#include <linux/coda_psdev.h>
#include <linux/coda_fs_i.h>
#include <linux/coda_proc.h>

static int coda_symlink_filler(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	int error;
	struct coda_inode_info *cnp;
	unsigned int len = PAGE_SIZE;
	char *p = kmap(page);

	lock_kernel();
        cnp = ITOC(inode);
	coda_vfs_stat.follow_link++;

	error = venus_readlink(inode->i_sb, &(cnp->c_fid), p, &len);
	unlock_kernel();
	if (error)
		goto fail;
	SetPageUptodate(page);
	kunmap(page);
	UnlockPage(page);
	return 0;

fail:
	SetPageError(page);
	kunmap(page);
	UnlockPage(page);
	return error;
}

struct address_space_operations coda_symlink_aops = {
	readpage:	coda_symlink_filler
};
