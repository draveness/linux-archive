/*
 * linux/fs/ext2/ioctl.c
 *
 * Copyright (C) 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 */

#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/sched.h>
#include <asm/uaccess.h>


int ext2_ioctl (struct inode * inode, struct file * filp, unsigned int cmd,
		unsigned long arg)
{
	unsigned int flags;

	ext2_debug ("cmd = %u, arg = %lu\n", cmd, arg);

	switch (cmd) {
	case EXT2_IOC_GETFLAGS:
		flags = inode->u.ext2_i.i_flags & EXT2_FL_USER_VISIBLE;
		return put_user(flags, (int *) arg);
	case EXT2_IOC_SETFLAGS: {
		unsigned int oldflags;

		if (IS_RDONLY(inode))
			return -EROFS;

		if ((current->fsuid != inode->i_uid) && !capable(CAP_FOWNER))
			return -EPERM;

		if (get_user(flags, (int *) arg))
			return -EFAULT;

		oldflags = inode->u.ext2_i.i_flags;

		/*
		 * The IMMUTABLE and APPEND_ONLY flags can only be changed by
		 * the relevant capability.
		 *
		 * This test looks nicer. Thanks to Pauline Middelink
		 */
		if ((flags ^ oldflags) & (EXT2_APPEND_FL | EXT2_IMMUTABLE_FL)) {
			if (!capable(CAP_LINUX_IMMUTABLE))
				return -EPERM;
		}

		flags = flags & EXT2_FL_USER_MODIFIABLE;
		flags |= oldflags & ~EXT2_FL_USER_MODIFIABLE;
		inode->u.ext2_i.i_flags = flags;

		if (flags & EXT2_SYNC_FL)
			inode->i_flags |= S_SYNC;
		else
			inode->i_flags &= ~S_SYNC;
		if (flags & EXT2_APPEND_FL)
			inode->i_flags |= S_APPEND;
		else
			inode->i_flags &= ~S_APPEND;
		if (flags & EXT2_IMMUTABLE_FL)
			inode->i_flags |= S_IMMUTABLE;
		else
			inode->i_flags &= ~S_IMMUTABLE;
		if (flags & EXT2_NOATIME_FL)
			inode->i_flags |= S_NOATIME;
		else
			inode->i_flags &= ~S_NOATIME;
		inode->i_ctime = CURRENT_TIME;
		mark_inode_dirty(inode);
		return 0;
	}
	case EXT2_IOC_GETVERSION:
		return put_user(inode->i_generation, (int *) arg);
	case EXT2_IOC_SETVERSION:
		if ((current->fsuid != inode->i_uid) && !capable(CAP_FOWNER))
			return -EPERM;
		if (IS_RDONLY(inode))
			return -EROFS;
		if (get_user(inode->i_generation, (int *) arg))
			return -EFAULT;	
		inode->i_ctime = CURRENT_TIME;
		mark_inode_dirty(inode);
		return 0;
	default:
		return -ENOTTY;
	}
}
