/*
 * Copyright (c) 2000-2003 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.  Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */
#ifndef __XFS_IOPS_H__
#define __XFS_IOPS_H__

extern struct inode_operations linvfs_file_inode_operations;
extern struct inode_operations linvfs_dir_inode_operations;
extern struct inode_operations linvfs_symlink_inode_operations;

extern struct file_operations linvfs_file_operations;
extern struct file_operations linvfs_invis_file_operations;
extern struct file_operations linvfs_dir_operations;

extern struct address_space_operations linvfs_aops;

extern int linvfs_get_block(struct inode *, sector_t, struct buffer_head *, int);
extern void linvfs_unwritten_done(struct buffer_head *, int);

extern int xfs_ioctl(struct bhv_desc *, struct inode *, struct file *,
                        int, unsigned int, unsigned long);

#endif /* __XFS_IOPS_H__ */
