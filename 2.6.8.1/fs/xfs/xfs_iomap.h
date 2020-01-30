/*
 * Copyright (c) 2003 Silicon Graphics, Inc.  All Rights Reserved.
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



#ifndef __XFS_IOMAP_H__
#define __XFS_IOMAP_H__

#define IOMAP_DADDR_NULL ((xfs_daddr_t) (-1LL))


typedef enum {				/* iomap_flags values */
	IOMAP_EOF =		0x01,	/* mapping contains EOF   */
	IOMAP_HOLE =		0x02,	/* mapping covers a hole  */
	IOMAP_DELAY =		0x04,	/* mapping covers delalloc region  */
	IOMAP_UNWRITTEN =	0x20,	/* mapping covers allocated */
					/* but uninitialized file data  */
	IOMAP_NEW =		0x40	/* just allocate */
} iomap_flags_t;

typedef enum {
	/* base extent manipulation calls */
	BMAPI_READ = (1 << 0),		/* read extents */
	BMAPI_WRITE = (1 << 1),		/* create extents */
	BMAPI_ALLOCATE = (1 << 2),	/* delayed allocate to real extents */
	BMAPI_UNWRITTEN  = (1 << 3),	/* unwritten extents to real extents */
	/* modifiers */
	BMAPI_IGNSTATE = (1 << 4),	/* ignore unwritten state on read */
	BMAPI_DIRECT = (1 << 5),		/* direct instead of buffered write */
	BMAPI_MMAP = (1 << 6),		/* allocate for mmap write */
	BMAPI_SYNC = (1 << 7),		/* sync write */
	BMAPI_TRYLOCK = (1 << 8),	/* non-blocking request */
	BMAPI_DEVICE = (1 << 9),	/* we only want to know the device */
} bmapi_flags_t;


/*
 * xfs_iomap_t:  File system I/O map
 *
 * The iomap_bn field is expressed in 512-byte blocks, and is where the 
 * mapping starts on disk.
 *
 * The iomap_offset, iomap_bsize and iomap_delta fields are in bytes.
 * iomap_offset is the offset of the mapping in the file itself.
 * iomap_bsize is the size of the mapping,  iomap_delta is the 
 * desired data's offset into the mapping, given the offset supplied 
 * to the file I/O map routine.
 *
 * When a request is made to read beyond the logical end of the object,
 * iomap_size may be set to 0, but iomap_offset and iomap_length should be set
 * to the actual amount of underlying storage that has been allocated, if any.
 */

typedef struct xfs_iomap {
	xfs_daddr_t		iomap_bn;	/* first 512b blk of mapping */
	xfs_buftarg_t		*iomap_target;
	loff_t			iomap_offset;	/* offset of mapping, bytes */
	loff_t			iomap_bsize;	/* size of mapping, bytes */
	size_t			iomap_delta;	/* offset into mapping, bytes */
	iomap_flags_t		iomap_flags;
} xfs_iomap_t;

struct xfs_iocore;
struct xfs_inode;
struct xfs_bmbt_irec;

extern int xfs_iomap(struct xfs_iocore *, xfs_off_t, ssize_t, int,
		     struct xfs_iomap *, int *);
extern int xfs_iomap_write_direct(struct xfs_inode *, loff_t, size_t,
				  int, struct xfs_bmbt_irec *, int *, int);
extern int xfs_iomap_write_delay(struct xfs_inode *, loff_t, size_t, int,
				 struct xfs_bmbt_irec *, int *);
extern int xfs_iomap_write_allocate(struct xfs_inode *,
				struct xfs_bmbt_irec *, int *);
extern int xfs_iomap_write_unwritten(struct xfs_inode *, loff_t, size_t);

#endif /* __XFS_IOMAP_H__*/
