/* -*- linux-c -*- --------------------------------------------------------- *
 *
 * linux/fs/autofs/symlink.c
 *
 *  Copyright 1997-1998 Transmeta Corporation -- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * ------------------------------------------------------------------------- */

#include "autofs_i.h"

static int autofs_readlink(struct dentry *dentry, char *buffer, int buflen)
{
	char *s=((struct autofs_symlink *)dentry->d_inode->u.generic_ip)->data;
	return vfs_readlink(dentry, buffer, buflen, s);
}

static int autofs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	char *s=((struct autofs_symlink *)dentry->d_inode->u.generic_ip)->data;
	return vfs_follow_link(nd, s);
}

struct inode_operations autofs_symlink_inode_operations = {
	readlink:	autofs_readlink,
	follow_link:	autofs_follow_link
};
