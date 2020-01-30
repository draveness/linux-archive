/*
 *  linux/fs/ufs/file.c
 *
 * Copyright (C) 1998
 * Daniel Pirkl <daniel.pirkl@email.cz>
 * Charles University, Faculty of Mathematics and Physics
 *
 *  from
 *
 *  linux/fs/ext2/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 fs regular file handling primitives
 */

#include <asm/uaccess.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ufs_fs.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/locks.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

/*
 * Make sure the offset never goes beyond the 32-bit mark..
 */
static long long ufs_file_lseek(
	struct file *file,
	long long offset,
	int origin )
{
	long long retval;
	struct inode *inode = file->f_dentry->d_inode;

	switch (origin) {
		case 2:
			offset += inode->i_size;
			break;
		case 1:
			offset += file->f_pos;
	}
	retval = -EINVAL;
	/* make sure the offset fits in 32 bits */
	if (((unsigned long long) offset >> 32) == 0) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_reada = 0;
			file->f_version = ++event;
		}
		retval = offset;
	}
	return retval;
}

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the ufs filesystem.
 */
struct file_operations ufs_file_operations = {
	llseek:		ufs_file_lseek,
	read:		generic_file_read,
	write:		generic_file_write,
	mmap:		generic_file_mmap,
};

struct inode_operations ufs_file_inode_operations = {
	truncate:	ufs_truncate,
};
