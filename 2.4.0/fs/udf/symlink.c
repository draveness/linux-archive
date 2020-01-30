/*
 * symlink.c
 *
 * PURPOSE
 *	Symlink handling routines for the OSTA-UDF(tm) filesystem.
 *
 * CONTACTS
 *	E-mail regarding any portion of the Linux UDF file system should be
 *	directed to the development team mailing list (run by majordomo):
 *		linux_udf@hootie.lvld.hp.com
 *
 * COPYRIGHT
 *	This file is distributed under the terms of the GNU General Public
 *	License (GPL). Copies of the GPL can be obtained from:
 *		ftp://prep.ai.mit.edu/pub/gnu/GPL
 *	Each contributing author retains all rights to their own work.
 *
 *  (C) 1998-2000 Ben Fennema
 *  (C) 1999 Stelias Computing Inc 
 *
 * HISTORY
 *
 *  04/16/99 blf  Created.
 *
 */

#include "udfdecl.h"
#include <asm/uaccess.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/udf_fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/stat.h>
#include <linux/malloc.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#include "udf_i.h"

static void udf_pc_to_char(char *from, int fromlen, char *to)
{
	struct PathComponent *pc;
	int elen = 0;
	char *p = to;

	while (elen < fromlen)
	{
		pc = (struct PathComponent *)(from + elen);
		switch (pc->componentType)
		{
			case 1:
				if (pc->lengthComponentIdent == 0)
				{
					p = to;
					*p++ = '/';
				}
				break;
			case 3:
				memcpy(p, "../", 3);
				p += 3;
				break;
			case 4:
				memcpy(p, "./", 2);
				p += 2;
				/* that would be . - just ignore */
				break;
			case 5:
				memcpy(p, pc->componentIdent, pc->lengthComponentIdent);
				p += pc->lengthComponentIdent;
				*p++ = '/';
		}
		elen += sizeof(struct PathComponent) + pc->lengthComponentIdent;
	}
	if (p > to+1)
		p[-1] = '\0';
	else
		p[0] = '\0';
}

static int udf_symlink_filler(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *bh = NULL;
	char *symlink;
	int err = -EIO;
	char *p = kmap(page);
	
	lock_kernel();
	if (UDF_I_ALLOCTYPE(inode) == ICB_FLAG_AD_IN_ICB)
	{
		bh = udf_tread(inode->i_sb, inode->i_ino, inode->i_sb->s_blocksize);

		if (!bh)
			goto out;

		symlink = bh->b_data + udf_file_entry_alloc_offset(inode);
	}
	else
	{
		bh = bread(inode->i_dev, udf_block_map(inode, 0),
				inode->i_sb->s_blocksize);

		if (!bh)
			goto out;

		symlink = bh->b_data;
	}

	udf_pc_to_char(symlink, inode->i_size, p);
	udf_release_data(bh);

	unlock_kernel();
	SetPageUptodate(page);
	kunmap(page);
	UnlockPage(page);
	return 0;
out:
	unlock_kernel();
	SetPageError(page);
	kunmap(page);
	UnlockPage(page);
	return err;
}

/*
 * symlinks can't do much...
 */
struct address_space_operations udf_symlink_aops = {
	readpage:			udf_symlink_filler,
};
