/*
 *  linux/fs/affs/symlink.c
 *
 *  1995  Hans-Joachim Widmaier - Modified for affs.
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  affs symlink handling code
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/affs_fs.h>
#include <linux/amigaffs.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>

static int affs_symlink_readpage(struct file *file, struct page *page)
{
	struct buffer_head *bh;
	struct inode *inode = page->mapping->host;
	char *link = kmap(page);
	struct slink_front *lf;
	int err;
	int			 i, j;
	char			 c;
	char			 lc;
	char			*pf;

	pr_debug("AFFS: follow_link(ino=%lu)\n",inode->i_ino);

	err = -EIO;
	bh = affs_bread(inode->i_sb, inode->i_ino);
	if (!bh)
		goto fail;
	i  = 0;
	j  = 0;
	lf = (struct slink_front *)bh->b_data;
	lc = 0;
	pf = AFFS_SB(inode->i_sb)->s_prefix ? AFFS_SB(inode->i_sb)->s_prefix : "/";

	if (strchr(lf->symname,':')) {	/* Handle assign or volume name */
		while (i < 1023 && (c = pf[i]))
			link[i++] = c;
		while (i < 1023 && lf->symname[j] != ':')
			link[i++] = lf->symname[j++];
		if (i < 1023)
			link[i++] = '/';
		j++;
		lc = '/';
	}
	while (i < 1023 && (c = lf->symname[j])) {
		if (c == '/' && lc == '/' && i < 1020) {	/* parent dir */
			link[i++] = '.';
			link[i++] = '.';
		}
		link[i++] = c;
		lc = c;
		j++;
	}
	link[i] = '\0';
	affs_brelse(bh);
	SetPageUptodate(page);
	kunmap(page);
	unlock_page(page);
	return 0;
fail:
	SetPageError(page);
	kunmap(page);
	unlock_page(page);
	return err;
}

struct address_space_operations affs_symlink_aops = {
	.readpage	= affs_symlink_readpage,
};

struct inode_operations affs_symlink_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= page_follow_link_light,
	.put_link	= page_put_link,
	.setattr	= affs_notify_change,
};
