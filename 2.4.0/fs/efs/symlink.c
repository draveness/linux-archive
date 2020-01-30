/*
 * symlink.c
 *
 * Copyright (c) 1999 Al Smith
 *
 * Portions derived from work (c) 1995,1996 Christian Vogelgsang.
 */

#include <linux/string.h>
#include <linux/efs_fs.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>

static int efs_symlink_readpage(struct file *file, struct page *page)
{
	char *link = kmap(page);
	struct buffer_head * bh;
	struct inode * inode = page->mapping->host;
	efs_block_t size = inode->i_size;
	int err;
  
	err = -ENAMETOOLONG;
	if (size > 2 * EFS_BLOCKSIZE)
		goto fail;
  
	lock_kernel();
	/* read first 512 bytes of link target */
	err = -EIO;
	bh = bread(inode->i_dev, efs_bmap(inode, 0), EFS_BLOCKSIZE);
	if (!bh)
		goto fail;
	memcpy(link, bh->b_data, (size > EFS_BLOCKSIZE) ? EFS_BLOCKSIZE : size);
	brelse(bh);
	if (size > EFS_BLOCKSIZE) {
		bh = bread(inode->i_dev, efs_bmap(inode, 1), EFS_BLOCKSIZE);
		if (!bh)
			goto fail;
		memcpy(link + EFS_BLOCKSIZE, bh->b_data, size - EFS_BLOCKSIZE);
		brelse(bh);
	}
	link[size] = '\0';
	unlock_kernel();
	SetPageUptodate(page);
	kunmap(page);
	UnlockPage(page);
	return 0;
fail:
	unlock_kernel();
	SetPageError(page);
	kunmap(page);
	UnlockPage(page);
	return err;
}

struct address_space_operations efs_symlink_aops = {
	readpage:	efs_symlink_readpage
};
