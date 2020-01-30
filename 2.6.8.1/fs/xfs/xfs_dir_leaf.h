/*
 * Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
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
#ifndef __XFS_DIR_LEAF_H__
#define	__XFS_DIR_LEAF_H__

/*
 * Directory layout, internal structure, access macros, etc.
 *
 * Large directories are structured around Btrees where all the data
 * elements are in the leaf nodes.  Filenames are hashed into an int,
 * then that int is used as the index into the Btree.  Since the hashval
 * of a filename may not be unique, we may have duplicate keys.  The
 * internal links in the Btree are logical block offsets into the file.
 */

struct uio;
struct xfs_bmap_free;
struct xfs_dabuf;
struct xfs_da_args;
struct xfs_da_state;
struct xfs_da_state_blk;
struct xfs_dir_put_args;
struct xfs_inode;
struct xfs_mount;
struct xfs_trans;

/*========================================================================
 * Directory Structure when equal to XFS_LBSIZE(mp) bytes.
 *========================================================================*/

/*
 * This is the structure of the leaf nodes in the Btree.
 *
 * Struct leaf_entry's are packed from the top.  Names grow from the bottom
 * but are not packed.  The freemap contains run-length-encoded entries
 * for the free bytes after the leaf_entry's, but only the N largest such,
 * smaller runs are dropped.  When the freemap doesn't show enough space
 * for an allocation, we compact the namelist area and try again.  If we
 * still don't have enough space, then we have to split the block.
 *
 * Since we have duplicate hash keys, for each key that matches, compare
 * the actual string.  The root and intermediate node search always takes
 * the first-in-the-block key match found, so we should only have to work
 * "forw"ard.  If none matches, continue with the "forw"ard leaf nodes
 * until the hash key changes or the filename is found.
 *
 * The parent directory and the self-pointer are explicitly represented
 * (ie: there are entries for "." and "..").
 *
 * Note that the count being a __uint16_t limits us to something like a
 * blocksize of 1.3MB in the face of worst case (short) filenames.
 */
#define XFS_DIR_LEAF_MAPSIZE	3	/* how many freespace slots */

typedef struct xfs_dir_leafblock {
	struct xfs_dir_leaf_hdr {	/* constant-structure header block */
		xfs_da_blkinfo_t info;	/* block type, links, etc. */
		__uint16_t count;	/* count of active leaf_entry's */
		__uint16_t namebytes;	/* num bytes of name strings stored */
		__uint16_t firstused;	/* first used byte in name area */
		__uint8_t  holes;	/* != 0 if blk needs compaction */
		__uint8_t  pad1;
		struct xfs_dir_leaf_map {/* RLE map of free bytes */
			__uint16_t base; /* base of free region */
			__uint16_t size; /* run length of free region */
		} freemap[XFS_DIR_LEAF_MAPSIZE]; /* N largest free regions */
	} hdr;
	struct xfs_dir_leaf_entry {	/* sorted on key, not name */
		xfs_dahash_t hashval;	/* hash value of name */
		__uint16_t nameidx;	/* index into buffer of name */
		__uint8_t namelen;	/* length of name string */
		__uint8_t pad2;
	} entries[1];			/* var sized array */
	struct xfs_dir_leaf_name {
		xfs_dir_ino_t inumber;	/* inode number for this key */
		__uint8_t name[1];	/* name string itself */
	} namelist[1];			/* grows from bottom of buf */
} xfs_dir_leafblock_t;
typedef struct xfs_dir_leaf_hdr xfs_dir_leaf_hdr_t;
typedef struct xfs_dir_leaf_map xfs_dir_leaf_map_t;
typedef struct xfs_dir_leaf_entry xfs_dir_leaf_entry_t;
typedef struct xfs_dir_leaf_name xfs_dir_leaf_name_t;

/*
 * Length of name for which a 512-byte block filesystem
 * can get a double split.
 */
#define	XFS_DIR_LEAF_CAN_DOUBLE_SPLIT_LEN	\
	(512 - (uint)sizeof(xfs_dir_leaf_hdr_t) - \
	 (uint)sizeof(xfs_dir_leaf_entry_t) * 2 - \
	 (uint)sizeof(xfs_dir_leaf_name_t) * 2 - (MAXNAMELEN - 2) + 1 + 1)

typedef int (*xfs_dir_put_t)(struct xfs_dir_put_args *pa);

typedef union {
	xfs_off_t		o;		/* offset (cookie) */
	/*
	 * Watch the order here (endian-ness dependent).
	 */
	struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
		xfs_dahash_t	h;	/* hash value */
		__uint32_t	be;	/* block and entry */
#else	/* __BYTE_ORDER == __BIG_ENDIAN */
		__uint32_t	be;	/* block and entry */
		xfs_dahash_t	h;	/* hash value */
#endif	/* __BYTE_ORDER == __BIG_ENDIAN */
	} s;
} xfs_dircook_t;

#define	XFS_PUT_COOKIE(c,mp,bno,entry,hash)	\
	((c).s.be = XFS_DA_MAKE_BNOENTRY(mp, bno, entry), (c).s.h = (hash))

#define	XFS_GET_DIR_INO_ARCH(mp,di,arch) \
    DIRINO_GET_ARCH(&(di),arch)
#define	XFS_GET_DIR_INO(mp,di) \
    XFS_GET_DIR_INO_ARCH(mp,di,ARCH_NOCONVERT)

typedef struct xfs_dir_put_args
{
	xfs_dircook_t	cook;		/* cookie of (next) entry */
	xfs_intino_t	ino;		/* inode number */
	struct xfs_dirent	*dbp;		/* buffer pointer */
	char		*name;		/* directory entry name */
	int		namelen;	/* length of name */
	int		done;		/* output: set if value was stored */
	xfs_dir_put_t	put;		/* put function ptr (i/o) */
	struct uio	*uio;		/* uio control structure */
} xfs_dir_put_args_t;

#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_DIR_LEAF_ENTSIZE_BYNAME)
int xfs_dir_leaf_entsize_byname(int len);
#define XFS_DIR_LEAF_ENTSIZE_BYNAME(len)	xfs_dir_leaf_entsize_byname(len)
#else
#define XFS_DIR_LEAF_ENTSIZE_BYNAME(len)	/* space a name will use */ \
	((uint)sizeof(xfs_dir_leaf_name_t)-1 + len)
#endif
#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_DIR_LEAF_ENTSIZE_BYENTRY)
int xfs_dir_leaf_entsize_byentry(xfs_dir_leaf_entry_t *entry);
#define XFS_DIR_LEAF_ENTSIZE_BYENTRY(entry)	\
	xfs_dir_leaf_entsize_byentry(entry)
#else
#define XFS_DIR_LEAF_ENTSIZE_BYENTRY(entry)	/* space an entry will use */ \
	((uint)sizeof(xfs_dir_leaf_name_t)-1 + (entry)->namelen)
#endif
#if XFS_WANT_FUNCS || (XFS_WANT_SPACE && XFSSO_XFS_DIR_LEAF_NAMESTRUCT)
xfs_dir_leaf_name_t *
xfs_dir_leaf_namestruct(xfs_dir_leafblock_t *leafp, int offset);
#define XFS_DIR_LEAF_NAMESTRUCT(leafp,offset)	\
	xfs_dir_leaf_namestruct(leafp,offset)
#else
#define XFS_DIR_LEAF_NAMESTRUCT(leafp,offset)	/* point to name struct */ \
	((xfs_dir_leaf_name_t *)&((char *)(leafp))[offset])
#endif

/*========================================================================
 * Function prototypes for the kernel.
 *========================================================================*/

/*
 * Internal routines when dirsize < XFS_LITINO(mp).
 */
int xfs_dir_shortform_create(struct xfs_da_args *args, xfs_ino_t parent);
int xfs_dir_shortform_addname(struct xfs_da_args *args);
int xfs_dir_shortform_lookup(struct xfs_da_args *args);
int xfs_dir_shortform_to_leaf(struct xfs_da_args *args);
int xfs_dir_shortform_removename(struct xfs_da_args *args);
int xfs_dir_shortform_getdents(struct xfs_inode *dp, struct uio *uio, int *eofp,
				      struct xfs_dirent *dbp, xfs_dir_put_t put);
int xfs_dir_shortform_replace(struct xfs_da_args *args);

/*
 * Internal routines when dirsize == XFS_LBSIZE(mp).
 */
int xfs_dir_leaf_to_node(struct xfs_da_args *args);
int xfs_dir_leaf_to_shortform(struct xfs_da_args *args);

/*
 * Routines used for growing the Btree.
 */
int	xfs_dir_leaf_create(struct xfs_da_args *args, xfs_dablk_t which_block,
				   struct xfs_dabuf **bpp);
int	xfs_dir_leaf_split(struct xfs_da_state *state,
				  struct xfs_da_state_blk *oldblk,
				  struct xfs_da_state_blk *newblk);
int	xfs_dir_leaf_add(struct xfs_dabuf *leaf_buffer,
				struct xfs_da_args *args, int insertion_index);
int	xfs_dir_leaf_addname(struct xfs_da_args *args);
int	xfs_dir_leaf_lookup_int(struct xfs_dabuf *leaf_buffer,
				       struct xfs_da_args *args,
				       int *index_found_at);
int	xfs_dir_leaf_remove(struct xfs_trans *trans,
				   struct xfs_dabuf *leaf_buffer,
				   int index_to_remove);
int	xfs_dir_leaf_getdents_int(struct xfs_dabuf *bp, struct xfs_inode *dp,
					 xfs_dablk_t bno, struct uio *uio,
					 int *eobp, struct xfs_dirent *dbp,
					 xfs_dir_put_t put, xfs_daddr_t nextda);

/*
 * Routines used for shrinking the Btree.
 */
int	xfs_dir_leaf_toosmall(struct xfs_da_state *state, int *retval);
void	xfs_dir_leaf_unbalance(struct xfs_da_state *state,
					     struct xfs_da_state_blk *drop_blk,
					     struct xfs_da_state_blk *save_blk);

/*
 * Utility routines.
 */
uint	xfs_dir_leaf_lasthash(struct xfs_dabuf *bp, int *count);
int	xfs_dir_leaf_order(struct xfs_dabuf *leaf1_bp,
				  struct xfs_dabuf *leaf2_bp);
int	xfs_dir_put_dirent64_direct(xfs_dir_put_args_t *pa);
int	xfs_dir_put_dirent64_uio(xfs_dir_put_args_t *pa);
int	xfs_dir_ino_validate(struct xfs_mount *mp, xfs_ino_t ino);


/*
 * Global data.
 */
extern xfs_dahash_t	xfs_dir_hash_dot, xfs_dir_hash_dotdot;

#endif /* __XFS_DIR_LEAF_H__ */
