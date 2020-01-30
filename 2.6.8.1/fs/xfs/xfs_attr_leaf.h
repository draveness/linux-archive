/*
 * Copyright (c) 2000, 2002-2003 Silicon Graphics, Inc.  All Rights Reserved.
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
#ifndef __XFS_ATTR_LEAF_H__
#define	__XFS_ATTR_LEAF_H__

/*
 * Attribute storage layout, internal structure, access macros, etc.
 *
 * Attribute lists are structured around Btrees where all the data
 * elements are in the leaf nodes.  Attribute names are hashed into an int,
 * then that int is used as the index into the Btree.  Since the hashval
 * of an attribute name may not be unique, we may have duplicate keys.  The
 * internal links in the Btree are logical block offsets into the file.
 */

struct attrlist;
struct attrlist_cursor_kern;
struct attrnames;
struct xfs_dabuf;
struct xfs_da_args;
struct xfs_da_state;
struct xfs_da_state_blk;
struct xfs_inode;
struct xfs_trans;

/*========================================================================
 * Attribute structure when equal to XFS_LBSIZE(mp) bytes.
 *========================================================================*/

/*
 * This is the structure of the leaf nodes in the Btree.
 *
 * Struct leaf_entry's are packed from the top.  Name/values grow from the
 * bottom but are not packed.  The freemap contains run-length-encoded entries
 * for the free bytes after the leaf_entry's, but only the N largest such,
 * smaller runs are dropped.  When the freemap doesn't show enough space
 * for an allocation, we compact the name/value area and try again.  If we
 * still don't have enough space, then we have to split the block.  The
 * name/value structs (both local and remote versions) must be 32bit aligned.
 *
 * Since we have duplicate hash keys, for each key that matches, compare
 * the actual name string.  The root and intermediate node search always
 * takes the first-in-the-block key match found, so we should only have
 * to work "forw"ard.  If none matches, continue with the "forw"ard leaf
 * nodes until the hash key changes or the attribute name is found.
 *
 * We store the fact that an attribute is a ROOT/USER/SECURE attribute in
 * the leaf_entry.  The namespaces are independent only because we also look
 * at the namespace bit when we are looking for a matching attribute name.
 *
 * We also store a "incomplete" bit in the leaf_entry.  It shows that an
 * attribute is in the middle of being created and should not be shown to
 * the user if we crash during the time that the bit is set.  We clear the
 * bit when we have finished setting up the attribute.  We do this because
 * we cannot create some large attributes inside a single transaction, and we
 * need some indication that we weren't finished if we crash in the middle.
 */
#define XFS_ATTR_LEAF_MAPSIZE	3	/* how many freespace slots */

typedef struct xfs_attr_leafblock {
	struct xfs_attr_leaf_hdr {	/* constant-structure header block */
		xfs_da_blkinfo_t info;	/* block type, links, etc. */
		__uint16_t count;	/* count of active leaf_entry's */
		__uint16_t usedbytes;	/* num bytes of names/values stored */
		__uint16_t firstused;	/* first used byte in name area */
		__uint8_t  holes;	/* != 0 if blk needs compaction */
		__uint8_t  pad1;
		struct xfs_attr_leaf_map {	  /* RLE map of free bytes */
			__uint16_t base;	  /* base of free region */
			__uint16_t size;	  /* length of free region */
		} freemap[XFS_ATTR_LEAF_MAPSIZE]; /* N largest free regions */
	} hdr;
	struct xfs_attr_leaf_entry {	/* sorted on key, not name */
		xfs_dahash_t hashval;	/* hash value of name */
		__uint16_t nameidx;	/* index into buffer of name/value */
		__uint8_t flags;	/* LOCAL/ROOT/SECURE/INCOMPLETE flag */
		__uint8_t pad2;		/* unused pad byte */
	} entries[1];			/* variable sized array */
	struct xfs_attr_leaf_name_local {
		__uint16_t valuelen;	/* number of bytes in value */
		__uint8_t namelen;	/* length of name bytes */
		__uint8_t nameval[1];	/* name/value bytes */
	} namelist;			/* grows from bottom of buf */
	struct xfs_attr_leaf_name_remote {
		xfs_dablk_t valueblk;	/* block number of value bytes */
		__uint32_t valuelen;	/* number of bytes in value */
		__uint8_t namelen;	/* length of name bytes */
		__uint8_t name[1];	/* name bytes */
	} valuelist;			/* grows from bottom of buf */
} xfs_attr_leafblock_t;
typedef struct xfs_attr_leaf_hdr xfs_attr_leaf_hdr_t;
typedef struct xfs_attr_leaf_map xfs_attr_leaf_map_t;
typedef struct xfs_attr_leaf_entry xfs_attr_leaf_entry_t;
typedef struct xfs_attr_leaf_name_local xfs_attr_leaf_name_local_t;
typedef struct xfs_attr_leaf_name_remote xfs_attr_leaf_name_remote_t;

/*
 * Flags used in the leaf_entry[i].flags field.
 * NOTE: the INCOMPLETE bit must not collide with the flags bits specified
 * on the system call, they are "or"ed together for various operations.
 */
#define	XFS_ATTR_LOCAL_BIT	0	/* attr is stored locally */
#define	XFS_ATTR_ROOT_BIT	1	/* limit access to trusted attrs */
#define	XFS_ATTR_SECURE_BIT	2	/* limit access to secure attrs */
#define	XFS_ATTR_INCOMPLETE_BIT	7	/* attr in middle of create/delete */
#define XFS_ATTR_LOCAL		(1 << XFS_ATTR_LOCAL_BIT)
#define XFS_ATTR_ROOT		(1 << XFS_ATTR_ROOT_BIT)
#define XFS_ATTR_SECURE		(1 << XFS_ATTR_SECURE_BIT)
#define XFS_ATTR_INCOMPLETE	(1 << XFS_ATTR_INCOMPLETE_BIT)

/*
 * Alignment for namelist and valuelist entries (since they are mixed
 * there can be only one alignment value)
 */
#define	XFS_ATTR_LEAF_NAME_ALIGN	((uint)sizeof(xfs_dablk_t))

/*
 * Cast typed pointers for "local" and "remote" name/value structs.
 */
#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_ATTR_LEAF_NAME_REMOTE)
xfs_attr_leaf_name_remote_t *
xfs_attr_leaf_name_remote(xfs_attr_leafblock_t *leafp, int idx);
#define XFS_ATTR_LEAF_NAME_REMOTE(leafp,idx)	\
	xfs_attr_leaf_name_remote(leafp,idx)
#else
#define XFS_ATTR_LEAF_NAME_REMOTE(leafp,idx)	/* remote name struct ptr */ \
	((xfs_attr_leaf_name_remote_t *)		\
	 &((char *)(leafp))[ INT_GET((leafp)->entries[idx].nameidx, ARCH_CONVERT) ])
#endif
#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_ATTR_LEAF_NAME_LOCAL)
xfs_attr_leaf_name_local_t *
xfs_attr_leaf_name_local(xfs_attr_leafblock_t *leafp, int idx);
#define XFS_ATTR_LEAF_NAME_LOCAL(leafp,idx)	\
	xfs_attr_leaf_name_local(leafp,idx)
#else
#define XFS_ATTR_LEAF_NAME_LOCAL(leafp,idx)	/* local name struct ptr */ \
	((xfs_attr_leaf_name_local_t *)		\
	 &((char *)(leafp))[ INT_GET((leafp)->entries[idx].nameidx, ARCH_CONVERT) ])
#endif
#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_ATTR_LEAF_NAME)
char *xfs_attr_leaf_name(xfs_attr_leafblock_t *leafp, int idx);
#define XFS_ATTR_LEAF_NAME(leafp,idx)		xfs_attr_leaf_name(leafp,idx)
#else
#define XFS_ATTR_LEAF_NAME(leafp,idx)		/* generic name struct ptr */ \
	(&((char *)(leafp))[ INT_GET((leafp)->entries[idx].nameidx, ARCH_CONVERT) ])
#endif

/*
 * Calculate total bytes used (including trailing pad for alignment) for
 * a "local" name/value structure, a "remote" name/value structure, and
 * a pointer which might be either.
 */
#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_ATTR_LEAF_ENTSIZE_REMOTE)
int xfs_attr_leaf_entsize_remote(int nlen);
#define XFS_ATTR_LEAF_ENTSIZE_REMOTE(nlen)	\
	xfs_attr_leaf_entsize_remote(nlen)
#else
#define XFS_ATTR_LEAF_ENTSIZE_REMOTE(nlen)	/* space for remote struct */ \
	(((uint)sizeof(xfs_attr_leaf_name_remote_t) - 1 + (nlen) + \
	  XFS_ATTR_LEAF_NAME_ALIGN - 1) & ~(XFS_ATTR_LEAF_NAME_ALIGN - 1))
#endif
#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_ATTR_LEAF_ENTSIZE_LOCAL)
int xfs_attr_leaf_entsize_local(int nlen, int vlen);
#define XFS_ATTR_LEAF_ENTSIZE_LOCAL(nlen,vlen)	\
	xfs_attr_leaf_entsize_local(nlen,vlen)
#else
#define XFS_ATTR_LEAF_ENTSIZE_LOCAL(nlen,vlen)	/* space for local struct */ \
	(((uint)sizeof(xfs_attr_leaf_name_local_t) - 1 + (nlen) + (vlen) + \
	  XFS_ATTR_LEAF_NAME_ALIGN - 1) & ~(XFS_ATTR_LEAF_NAME_ALIGN - 1))
#endif
#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_ATTR_LEAF_ENTSIZE_LOCAL_MAX)
int xfs_attr_leaf_entsize_local_max(int bsize);
#define XFS_ATTR_LEAF_ENTSIZE_LOCAL_MAX(bsize)	\
	xfs_attr_leaf_entsize_local_max(bsize)
#else
#define XFS_ATTR_LEAF_ENTSIZE_LOCAL_MAX(bsize)	/* max local struct size */ \
	(((bsize) >> 1) + ((bsize) >> 2))
#endif


/*========================================================================
 * Structure used to pass context around among the routines.
 *========================================================================*/

typedef struct xfs_attr_list_context {
	struct xfs_inode		*dp;	/* inode */
	struct attrlist_cursor_kern	*cursor;/* position in list */
	struct attrlist			*alist;	/* output buffer */
	int				count;	/* num used entries */
	int				dupcnt;	/* count dup hashvals seen */
	int				bufsize;/* total buffer size */
	int				firstu;	/* first used byte in buffer */
	int				flags;	/* from VOP call */
	int				resynch;/* T/F: resynch with cursor */
} xfs_attr_list_context_t;

/*
 * Used to keep a list of "remote value" extents when unlinking an inode.
 */
typedef struct xfs_attr_inactive_list {
	xfs_dablk_t	valueblk;	/* block number of value bytes */
	int		valuelen;	/* number of bytes in value */
} xfs_attr_inactive_list_t;


/*========================================================================
 * Function prototypes for the kernel.
 *========================================================================*/

/*
 * Internal routines when dirsize < XFS_LITINO(mp).
 */
int	xfs_attr_shortform_create(struct xfs_da_args *args);
int	xfs_attr_shortform_add(struct xfs_da_args *add);
int	xfs_attr_shortform_lookup(struct xfs_da_args *args);
int	xfs_attr_shortform_getvalue(struct xfs_da_args *args);
int	xfs_attr_shortform_to_leaf(struct xfs_da_args *args);
int	xfs_attr_shortform_remove(struct xfs_da_args *remove);
int	xfs_attr_shortform_list(struct xfs_attr_list_context *context);
int	xfs_attr_shortform_replace(struct xfs_da_args *args);
int	xfs_attr_shortform_allfit(struct xfs_dabuf *bp, struct xfs_inode *dp);

/*
 * Internal routines when dirsize == XFS_LBSIZE(mp).
 */
int	xfs_attr_leaf_to_node(struct xfs_da_args *args);
int	xfs_attr_leaf_to_shortform(struct xfs_dabuf *bp,
					  struct xfs_da_args *args);
int	xfs_attr_leaf_clearflag(struct xfs_da_args *args);
int	xfs_attr_leaf_setflag(struct xfs_da_args *args);
int	xfs_attr_leaf_flipflags(xfs_da_args_t *args);

/*
 * Routines used for growing the Btree.
 */
int	xfs_attr_leaf_create(struct xfs_da_args *args, xfs_dablk_t which_block,
				    struct xfs_dabuf **bpp);
int	xfs_attr_leaf_split(struct xfs_da_state *state,
				   struct xfs_da_state_blk *oldblk,
				   struct xfs_da_state_blk *newblk);
int	xfs_attr_leaf_lookup_int(struct xfs_dabuf *leaf,
					struct xfs_da_args *args);
int	xfs_attr_leaf_getvalue(struct xfs_dabuf *bp, struct xfs_da_args *args);
int	xfs_attr_leaf_add(struct xfs_dabuf *leaf_buffer,
				 struct xfs_da_args *args);
int	xfs_attr_leaf_remove(struct xfs_dabuf *leaf_buffer,
				    struct xfs_da_args *args);
int	xfs_attr_leaf_list_int(struct xfs_dabuf *bp,
				      struct xfs_attr_list_context *context);

/*
 * Routines used for shrinking the Btree.
 */
int	xfs_attr_leaf_toosmall(struct xfs_da_state *state, int *retval);
void	xfs_attr_leaf_unbalance(struct xfs_da_state *state,
				       struct xfs_da_state_blk *drop_blk,
				       struct xfs_da_state_blk *save_blk);
int	xfs_attr_root_inactive(struct xfs_trans **trans, struct xfs_inode *dp);
int	xfs_attr_node_inactive(struct xfs_trans **trans, struct xfs_inode *dp,
				      struct xfs_dabuf *bp, int level);
int	xfs_attr_leaf_inactive(struct xfs_trans **trans, struct xfs_inode *dp,
				      struct xfs_dabuf *bp);
int	xfs_attr_leaf_freextent(struct xfs_trans **trans, struct xfs_inode *dp,
				       xfs_dablk_t blkno, int blkcnt);

/*
 * Utility routines.
 */
xfs_dahash_t	xfs_attr_leaf_lasthash(struct xfs_dabuf *bp, int *count);
int	xfs_attr_leaf_order(struct xfs_dabuf *leaf1_bp,
				   struct xfs_dabuf *leaf2_bp);
int	xfs_attr_leaf_newentsize(struct xfs_da_args *args, int blocksize,
					int *local);
int	xfs_attr_leaf_entsize(struct xfs_attr_leafblock *leaf, int index);
int	xfs_attr_put_listent(struct xfs_attr_list_context *context,
			     struct attrnames *, char *name, int namelen,
			     int valuelen);
int	xfs_attr_rolltrans(struct xfs_trans **transp, struct xfs_inode *dp);

#endif	/* __XFS_ATTR_LEAF_H__ */
