/*
 *  linux/include/linux/ufs_fs_i.h
 *
 * Copyright (C) 1996
 * Adrian Rodriguez (adrian@franklins-tower.rutgers.edu)
 * Laboratory for Computer Science Research Computing Facility
 * Rutgers, The State University of New Jersey
 *
 * NeXTstep support added on February 5th 1998 by
 * Niels Kristian Bech Jensen <nkbj@image.dk>.
 */

#ifndef _LINUX_UFS_FS_I_H
#define _LINUX_UFS_FS_I_H

struct ufs_inode_info {
	union {
		__u32	i_data[15];
		__u8	i_symlink[4*15];
	} i_u1;
	__u64	i_size;
	__u32	i_flags;
	__u32	i_gen;
	__u32	i_shadow;
	__u32	i_unused1;
	__u32	i_unused2;
	__u32	i_oeftflag;
	__u16	i_osync;
	__u32	i_lastfrag;
};

#endif /* _LINUX_UFS_FS_I_H */
