/*
 * Copyright (c) 2003-2006, Cluster File Systems, Inc, info@clusterfs.com
 * Written by Alex Tomas <alex@clusterfs.com>
 *
 * Architecture independence:
 *   Copyright (c) 2005, Bull S.A.
 *   Written by Pierre Peiffer <pierre.peiffer@bull.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 */

/*
 * Extents support for EXT4
 *
 * TODO:
 *   - ext4*_error() should be used in some situations
 *   - analyze all BUG()/BUG_ON(), use -EIO where appropriate
 *   - smart tree reduction
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/ext4_jbd2.h>
#include <linux/jbd.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/falloc.h>
#include <linux/ext4_fs_extents.h>
#include <asm/uaccess.h>


/*
 * ext_pblock:
 * combine low and high parts of physical block number into ext4_fsblk_t
 */
static ext4_fsblk_t ext_pblock(struct ext4_extent *ex)
{
	ext4_fsblk_t block;

	block = le32_to_cpu(ex->ee_start);
	block |= ((ext4_fsblk_t) le16_to_cpu(ex->ee_start_hi) << 31) << 1;
	return block;
}

/*
 * idx_pblock:
 * combine low and high parts of a leaf physical block number into ext4_fsblk_t
 */
static ext4_fsblk_t idx_pblock(struct ext4_extent_idx *ix)
{
	ext4_fsblk_t block;

	block = le32_to_cpu(ix->ei_leaf);
	block |= ((ext4_fsblk_t) le16_to_cpu(ix->ei_leaf_hi) << 31) << 1;
	return block;
}

/*
 * ext4_ext_store_pblock:
 * stores a large physical block number into an extent struct,
 * breaking it into parts
 */
static void ext4_ext_store_pblock(struct ext4_extent *ex, ext4_fsblk_t pb)
{
	ex->ee_start = cpu_to_le32((unsigned long) (pb & 0xffffffff));
	ex->ee_start_hi = cpu_to_le16((unsigned long) ((pb >> 31) >> 1) & 0xffff);
}

/*
 * ext4_idx_store_pblock:
 * stores a large physical block number into an index struct,
 * breaking it into parts
 */
static void ext4_idx_store_pblock(struct ext4_extent_idx *ix, ext4_fsblk_t pb)
{
	ix->ei_leaf = cpu_to_le32((unsigned long) (pb & 0xffffffff));
	ix->ei_leaf_hi = cpu_to_le16((unsigned long) ((pb >> 31) >> 1) & 0xffff);
}

static handle_t *ext4_ext_journal_restart(handle_t *handle, int needed)
{
	int err;

	if (handle->h_buffer_credits > needed)
		return handle;
	if (!ext4_journal_extend(handle, needed))
		return handle;
	err = ext4_journal_restart(handle, needed);

	return handle;
}

/*
 * could return:
 *  - EROFS
 *  - ENOMEM
 */
static int ext4_ext_get_access(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	if (path->p_bh) {
		/* path points to block */
		return ext4_journal_get_write_access(handle, path->p_bh);
	}
	/* path points to leaf/index in inode body */
	/* we use in-core data, no need to protect them */
	return 0;
}

/*
 * could return:
 *  - EROFS
 *  - ENOMEM
 *  - EIO
 */
static int ext4_ext_dirty(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	int err;
	if (path->p_bh) {
		/* path points to block */
		err = ext4_journal_dirty_metadata(handle, path->p_bh);
	} else {
		/* path points to leaf/index in inode body */
		err = ext4_mark_inode_dirty(handle, inode);
	}
	return err;
}

static ext4_fsblk_t ext4_ext_find_goal(struct inode *inode,
			      struct ext4_ext_path *path,
			      ext4_fsblk_t block)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	ext4_fsblk_t bg_start;
	ext4_grpblk_t colour;
	int depth;

	if (path) {
		struct ext4_extent *ex;
		depth = path->p_depth;

		/* try to predict block placement */
		ex = path[depth].p_ext;
		if (ex)
			return ext_pblock(ex)+(block-le32_to_cpu(ex->ee_block));

		/* it looks like index is empty;
		 * try to find starting block from index itself */
		if (path[depth].p_bh)
			return path[depth].p_bh->b_blocknr;
	}

	/* OK. use inode's group */
	bg_start = (ei->i_block_group * EXT4_BLOCKS_PER_GROUP(inode->i_sb)) +
		le32_to_cpu(EXT4_SB(inode->i_sb)->s_es->s_first_data_block);
	colour = (current->pid % 16) *
			(EXT4_BLOCKS_PER_GROUP(inode->i_sb) / 16);
	return bg_start + colour + block;
}

static ext4_fsblk_t
ext4_ext_new_block(handle_t *handle, struct inode *inode,
			struct ext4_ext_path *path,
			struct ext4_extent *ex, int *err)
{
	ext4_fsblk_t goal, newblock;

	goal = ext4_ext_find_goal(inode, path, le32_to_cpu(ex->ee_block));
	newblock = ext4_new_block(handle, inode, goal, err);
	return newblock;
}

static int ext4_ext_space_block(struct inode *inode)
{
	int size;

	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
			/ sizeof(struct ext4_extent);
#ifdef AGGRESSIVE_TEST
	if (size > 6)
		size = 6;
#endif
	return size;
}

static int ext4_ext_space_block_idx(struct inode *inode)
{
	int size;

	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
			/ sizeof(struct ext4_extent_idx);
#ifdef AGGRESSIVE_TEST
	if (size > 5)
		size = 5;
#endif
	return size;
}

static int ext4_ext_space_root(struct inode *inode)
{
	int size;

	size = sizeof(EXT4_I(inode)->i_data);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent);
#ifdef AGGRESSIVE_TEST
	if (size > 3)
		size = 3;
#endif
	return size;
}

static int ext4_ext_space_root_idx(struct inode *inode)
{
	int size;

	size = sizeof(EXT4_I(inode)->i_data);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent_idx);
#ifdef AGGRESSIVE_TEST
	if (size > 4)
		size = 4;
#endif
	return size;
}

static int
ext4_ext_max_entries(struct inode *inode, int depth)
{
	int max;

	if (depth == ext_depth(inode)) {
		if (depth == 0)
			max = ext4_ext_space_root(inode);
		else
			max = ext4_ext_space_root_idx(inode);
	} else {
		if (depth == 0)
			max = ext4_ext_space_block(inode);
		else
			max = ext4_ext_space_block_idx(inode);
	}

	return max;
}

static int __ext4_ext_check_header(const char *function, struct inode *inode,
					struct ext4_extent_header *eh,
					int depth)
{
	const char *error_msg;
	int max = 0;

	if (unlikely(eh->eh_magic != EXT4_EXT_MAGIC)) {
		error_msg = "invalid magic";
		goto corrupted;
	}
	if (unlikely(le16_to_cpu(eh->eh_depth) != depth)) {
		error_msg = "unexpected eh_depth";
		goto corrupted;
	}
	if (unlikely(eh->eh_max == 0)) {
		error_msg = "invalid eh_max";
		goto corrupted;
	}
	max = ext4_ext_max_entries(inode, depth);
	if (unlikely(le16_to_cpu(eh->eh_max) > max)) {
		error_msg = "too large eh_max";
		goto corrupted;
	}
	if (unlikely(le16_to_cpu(eh->eh_entries) > le16_to_cpu(eh->eh_max))) {
		error_msg = "invalid eh_entries";
		goto corrupted;
	}
	return 0;

corrupted:
	ext4_error(inode->i_sb, function,
			"bad header in inode #%lu: %s - magic %x, "
			"entries %u, max %u(%u), depth %u(%u)",
			inode->i_ino, error_msg, le16_to_cpu(eh->eh_magic),
			le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max),
			max, le16_to_cpu(eh->eh_depth), depth);

	return -EIO;
}

#define ext4_ext_check_header(inode, eh, depth)	\
	__ext4_ext_check_header(__FUNCTION__, inode, eh, depth)

#ifdef EXT_DEBUG
static void ext4_ext_show_path(struct inode *inode, struct ext4_ext_path *path)
{
	int k, l = path->p_depth;

	ext_debug("path:");
	for (k = 0; k <= l; k++, path++) {
		if (path->p_idx) {
		  ext_debug("  %d->%llu", le32_to_cpu(path->p_idx->ei_block),
			    idx_pblock(path->p_idx));
		} else if (path->p_ext) {
			ext_debug("  %d:%d:%llu ",
				  le32_to_cpu(path->p_ext->ee_block),
				  ext4_ext_get_actual_len(path->p_ext),
				  ext_pblock(path->p_ext));
		} else
			ext_debug("  []");
	}
	ext_debug("\n");
}

static void ext4_ext_show_leaf(struct inode *inode, struct ext4_ext_path *path)
{
	int depth = ext_depth(inode);
	struct ext4_extent_header *eh;
	struct ext4_extent *ex;
	int i;

	if (!path)
		return;

	eh = path[depth].p_hdr;
	ex = EXT_FIRST_EXTENT(eh);

	for (i = 0; i < le16_to_cpu(eh->eh_entries); i++, ex++) {
		ext_debug("%d:%d:%llu ", le32_to_cpu(ex->ee_block),
			  ext4_ext_get_actual_len(ex), ext_pblock(ex));
	}
	ext_debug("\n");
}
#else
#define ext4_ext_show_path(inode,path)
#define ext4_ext_show_leaf(inode,path)
#endif

static void ext4_ext_drop_refs(struct ext4_ext_path *path)
{
	int depth = path->p_depth;
	int i;

	for (i = 0; i <= depth; i++, path++)
		if (path->p_bh) {
			brelse(path->p_bh);
			path->p_bh = NULL;
		}
}

/*
 * ext4_ext_binsearch_idx:
 * binary search for the closest index of the given block
 * the header must be checked before calling this
 */
static void
ext4_ext_binsearch_idx(struct inode *inode, struct ext4_ext_path *path, int block)
{
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent_idx *r, *l, *m;


	ext_debug("binsearch for %d(idx):  ", block);

	l = EXT_FIRST_INDEX(eh) + 1;
	r = EXT_LAST_INDEX(eh);
	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < le32_to_cpu(m->ei_block))
			r = m - 1;
		else
			l = m + 1;
		ext_debug("%p(%u):%p(%u):%p(%u) ", l, le32_to_cpu(l->ei_block),
				m, le32_to_cpu(m->ei_block),
				r, le32_to_cpu(r->ei_block));
	}

	path->p_idx = l - 1;
	ext_debug("  -> %d->%lld ", le32_to_cpu(path->p_idx->ei_block),
		  idx_pblock(path->p_idx));

#ifdef CHECK_BINSEARCH
	{
		struct ext4_extent_idx *chix, *ix;
		int k;

		chix = ix = EXT_FIRST_INDEX(eh);
		for (k = 0; k < le16_to_cpu(eh->eh_entries); k++, ix++) {
		  if (k != 0 &&
		      le32_to_cpu(ix->ei_block) <= le32_to_cpu(ix[-1].ei_block)) {
				printk("k=%d, ix=0x%p, first=0x%p\n", k,
					ix, EXT_FIRST_INDEX(eh));
				printk("%u <= %u\n",
				       le32_to_cpu(ix->ei_block),
				       le32_to_cpu(ix[-1].ei_block));
			}
			BUG_ON(k && le32_to_cpu(ix->ei_block)
					   <= le32_to_cpu(ix[-1].ei_block));
			if (block < le32_to_cpu(ix->ei_block))
				break;
			chix = ix;
		}
		BUG_ON(chix != path->p_idx);
	}
#endif

}

/*
 * ext4_ext_binsearch:
 * binary search for closest extent of the given block
 * the header must be checked before calling this
 */
static void
ext4_ext_binsearch(struct inode *inode, struct ext4_ext_path *path, int block)
{
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent *r, *l, *m;

	if (eh->eh_entries == 0) {
		/*
		 * this leaf is empty:
		 * we get such a leaf in split/add case
		 */
		return;
	}

	ext_debug("binsearch for %d:  ", block);

	l = EXT_FIRST_EXTENT(eh) + 1;
	r = EXT_LAST_EXTENT(eh);

	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < le32_to_cpu(m->ee_block))
			r = m - 1;
		else
			l = m + 1;
		ext_debug("%p(%u):%p(%u):%p(%u) ", l, le32_to_cpu(l->ee_block),
				m, le32_to_cpu(m->ee_block),
				r, le32_to_cpu(r->ee_block));
	}

	path->p_ext = l - 1;
	ext_debug("  -> %d:%llu:%d ",
			le32_to_cpu(path->p_ext->ee_block),
			ext_pblock(path->p_ext),
			ext4_ext_get_actual_len(path->p_ext));

#ifdef CHECK_BINSEARCH
	{
		struct ext4_extent *chex, *ex;
		int k;

		chex = ex = EXT_FIRST_EXTENT(eh);
		for (k = 0; k < le16_to_cpu(eh->eh_entries); k++, ex++) {
			BUG_ON(k && le32_to_cpu(ex->ee_block)
					  <= le32_to_cpu(ex[-1].ee_block));
			if (block < le32_to_cpu(ex->ee_block))
				break;
			chex = ex;
		}
		BUG_ON(chex != path->p_ext);
	}
#endif

}

int ext4_ext_tree_init(handle_t *handle, struct inode *inode)
{
	struct ext4_extent_header *eh;

	eh = ext_inode_hdr(inode);
	eh->eh_depth = 0;
	eh->eh_entries = 0;
	eh->eh_magic = EXT4_EXT_MAGIC;
	eh->eh_max = cpu_to_le16(ext4_ext_space_root(inode));
	ext4_mark_inode_dirty(handle, inode);
	ext4_ext_invalidate_cache(inode);
	return 0;
}

struct ext4_ext_path *
ext4_ext_find_extent(struct inode *inode, int block, struct ext4_ext_path *path)
{
	struct ext4_extent_header *eh;
	struct buffer_head *bh;
	short int depth, i, ppos = 0, alloc = 0;

	eh = ext_inode_hdr(inode);
	depth = ext_depth(inode);
	if (ext4_ext_check_header(inode, eh, depth))
		return ERR_PTR(-EIO);


	/* account possible depth increase */
	if (!path) {
		path = kzalloc(sizeof(struct ext4_ext_path) * (depth + 2),
				GFP_NOFS);
		if (!path)
			return ERR_PTR(-ENOMEM);
		alloc = 1;
	}
	path[0].p_hdr = eh;

	i = depth;
	/* walk through the tree */
	while (i) {
		ext_debug("depth %d: num %d, max %d\n",
			  ppos, le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));

		ext4_ext_binsearch_idx(inode, path + ppos, block);
		path[ppos].p_block = idx_pblock(path[ppos].p_idx);
		path[ppos].p_depth = i;
		path[ppos].p_ext = NULL;

		bh = sb_bread(inode->i_sb, path[ppos].p_block);
		if (!bh)
			goto err;

		eh = ext_block_hdr(bh);
		ppos++;
		BUG_ON(ppos > depth);
		path[ppos].p_bh = bh;
		path[ppos].p_hdr = eh;
		i--;

		if (ext4_ext_check_header(inode, eh, i))
			goto err;
	}

	path[ppos].p_depth = i;
	path[ppos].p_hdr = eh;
	path[ppos].p_ext = NULL;
	path[ppos].p_idx = NULL;

	/* find extent */
	ext4_ext_binsearch(inode, path + ppos, block);

	ext4_ext_show_path(inode, path);

	return path;

err:
	ext4_ext_drop_refs(path);
	if (alloc)
		kfree(path);
	return ERR_PTR(-EIO);
}

/*
 * ext4_ext_insert_index:
 * insert new index [@logical;@ptr] into the block at @curp;
 * check where to insert: before @curp or after @curp
 */
static int ext4_ext_insert_index(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *curp,
				int logical, ext4_fsblk_t ptr)
{
	struct ext4_extent_idx *ix;
	int len, err;

	err = ext4_ext_get_access(handle, inode, curp);
	if (err)
		return err;

	BUG_ON(logical == le32_to_cpu(curp->p_idx->ei_block));
	len = EXT_MAX_INDEX(curp->p_hdr) - curp->p_idx;
	if (logical > le32_to_cpu(curp->p_idx->ei_block)) {
		/* insert after */
		if (curp->p_idx != EXT_LAST_INDEX(curp->p_hdr)) {
			len = (len - 1) * sizeof(struct ext4_extent_idx);
			len = len < 0 ? 0 : len;
			ext_debug("insert new index %d after: %llu. "
					"move %d from 0x%p to 0x%p\n",
					logical, ptr, len,
					(curp->p_idx + 1), (curp->p_idx + 2));
			memmove(curp->p_idx + 2, curp->p_idx + 1, len);
		}
		ix = curp->p_idx + 1;
	} else {
		/* insert before */
		len = len * sizeof(struct ext4_extent_idx);
		len = len < 0 ? 0 : len;
		ext_debug("insert new index %d before: %llu. "
				"move %d from 0x%p to 0x%p\n",
				logical, ptr, len,
				curp->p_idx, (curp->p_idx + 1));
		memmove(curp->p_idx + 1, curp->p_idx, len);
		ix = curp->p_idx;
	}

	ix->ei_block = cpu_to_le32(logical);
	ext4_idx_store_pblock(ix, ptr);
	curp->p_hdr->eh_entries = cpu_to_le16(le16_to_cpu(curp->p_hdr->eh_entries)+1);

	BUG_ON(le16_to_cpu(curp->p_hdr->eh_entries)
			     > le16_to_cpu(curp->p_hdr->eh_max));
	BUG_ON(ix > EXT_LAST_INDEX(curp->p_hdr));

	err = ext4_ext_dirty(handle, inode, curp);
	ext4_std_error(inode->i_sb, err);

	return err;
}

/*
 * ext4_ext_split:
 * inserts new subtree into the path, using free index entry
 * at depth @at:
 * - allocates all needed blocks (new leaf and all intermediate index blocks)
 * - makes decision where to split
 * - moves remaining extents and index entries (right to the split point)
 *   into the newly allocated blocks
 * - initializes subtree
 */
static int ext4_ext_split(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path,
				struct ext4_extent *newext, int at)
{
	struct buffer_head *bh = NULL;
	int depth = ext_depth(inode);
	struct ext4_extent_header *neh;
	struct ext4_extent_idx *fidx;
	struct ext4_extent *ex;
	int i = at, k, m, a;
	ext4_fsblk_t newblock, oldblock;
	__le32 border;
	ext4_fsblk_t *ablocks = NULL; /* array of allocated blocks */
	int err = 0;

	/* make decision: where to split? */
	/* FIXME: now decision is simplest: at current extent */

	/* if current leaf will be split, then we should use
	 * border from split point */
	BUG_ON(path[depth].p_ext > EXT_MAX_EXTENT(path[depth].p_hdr));
	if (path[depth].p_ext != EXT_MAX_EXTENT(path[depth].p_hdr)) {
		border = path[depth].p_ext[1].ee_block;
		ext_debug("leaf will be split."
				" next leaf starts at %d\n",
				  le32_to_cpu(border));
	} else {
		border = newext->ee_block;
		ext_debug("leaf will be added."
				" next leaf starts at %d\n",
				le32_to_cpu(border));
	}

	/*
	 * If error occurs, then we break processing
	 * and mark filesystem read-only. index won't
	 * be inserted and tree will be in consistent
	 * state. Next mount will repair buffers too.
	 */

	/*
	 * Get array to track all allocated blocks.
	 * We need this to handle errors and free blocks
	 * upon them.
	 */
	ablocks = kzalloc(sizeof(ext4_fsblk_t) * depth, GFP_NOFS);
	if (!ablocks)
		return -ENOMEM;

	/* allocate all needed blocks */
	ext_debug("allocate %d blocks for indexes/leaf\n", depth - at);
	for (a = 0; a < depth - at; a++) {
		newblock = ext4_ext_new_block(handle, inode, path, newext, &err);
		if (newblock == 0)
			goto cleanup;
		ablocks[a] = newblock;
	}

	/* initialize new leaf */
	newblock = ablocks[--a];
	BUG_ON(newblock == 0);
	bh = sb_getblk(inode->i_sb, newblock);
	if (!bh) {
		err = -EIO;
		goto cleanup;
	}
	lock_buffer(bh);

	err = ext4_journal_get_create_access(handle, bh);
	if (err)
		goto cleanup;

	neh = ext_block_hdr(bh);
	neh->eh_entries = 0;
	neh->eh_max = cpu_to_le16(ext4_ext_space_block(inode));
	neh->eh_magic = EXT4_EXT_MAGIC;
	neh->eh_depth = 0;
	ex = EXT_FIRST_EXTENT(neh);

	/* move remainder of path[depth] to the new leaf */
	BUG_ON(path[depth].p_hdr->eh_entries != path[depth].p_hdr->eh_max);
	/* start copy from next extent */
	/* TODO: we could do it by single memmove */
	m = 0;
	path[depth].p_ext++;
	while (path[depth].p_ext <=
			EXT_MAX_EXTENT(path[depth].p_hdr)) {
		ext_debug("move %d:%llu:%d in new leaf %llu\n",
				le32_to_cpu(path[depth].p_ext->ee_block),
				ext_pblock(path[depth].p_ext),
				ext4_ext_get_actual_len(path[depth].p_ext),
				newblock);
		/*memmove(ex++, path[depth].p_ext++,
				sizeof(struct ext4_extent));
		neh->eh_entries++;*/
		path[depth].p_ext++;
		m++;
	}
	if (m) {
		memmove(ex, path[depth].p_ext-m, sizeof(struct ext4_extent)*m);
		neh->eh_entries = cpu_to_le16(le16_to_cpu(neh->eh_entries)+m);
	}

	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	err = ext4_journal_dirty_metadata(handle, bh);
	if (err)
		goto cleanup;
	brelse(bh);
	bh = NULL;

	/* correct old leaf */
	if (m) {
		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto cleanup;
		path[depth].p_hdr->eh_entries =
		     cpu_to_le16(le16_to_cpu(path[depth].p_hdr->eh_entries)-m);
		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto cleanup;

	}

	/* create intermediate indexes */
	k = depth - at - 1;
	BUG_ON(k < 0);
	if (k)
		ext_debug("create %d intermediate indices\n", k);
	/* insert new index into current index block */
	/* current depth stored in i var */
	i = depth - 1;
	while (k--) {
		oldblock = newblock;
		newblock = ablocks[--a];
		bh = sb_getblk(inode->i_sb, (ext4_fsblk_t)newblock);
		if (!bh) {
			err = -EIO;
			goto cleanup;
		}
		lock_buffer(bh);

		err = ext4_journal_get_create_access(handle, bh);
		if (err)
			goto cleanup;

		neh = ext_block_hdr(bh);
		neh->eh_entries = cpu_to_le16(1);
		neh->eh_magic = EXT4_EXT_MAGIC;
		neh->eh_max = cpu_to_le16(ext4_ext_space_block_idx(inode));
		neh->eh_depth = cpu_to_le16(depth - i);
		fidx = EXT_FIRST_INDEX(neh);
		fidx->ei_block = border;
		ext4_idx_store_pblock(fidx, oldblock);

		ext_debug("int.index at %d (block %llu): %lu -> %llu\n", i,
				newblock, (unsigned long) le32_to_cpu(border),
				oldblock);
		/* copy indexes */
		m = 0;
		path[i].p_idx++;

		ext_debug("cur 0x%p, last 0x%p\n", path[i].p_idx,
				EXT_MAX_INDEX(path[i].p_hdr));
		BUG_ON(EXT_MAX_INDEX(path[i].p_hdr) !=
				EXT_LAST_INDEX(path[i].p_hdr));
		while (path[i].p_idx <= EXT_MAX_INDEX(path[i].p_hdr)) {
			ext_debug("%d: move %d:%llu in new index %llu\n", i,
					le32_to_cpu(path[i].p_idx->ei_block),
					idx_pblock(path[i].p_idx),
					newblock);
			/*memmove(++fidx, path[i].p_idx++,
					sizeof(struct ext4_extent_idx));
			neh->eh_entries++;
			BUG_ON(neh->eh_entries > neh->eh_max);*/
			path[i].p_idx++;
			m++;
		}
		if (m) {
			memmove(++fidx, path[i].p_idx - m,
				sizeof(struct ext4_extent_idx) * m);
			neh->eh_entries =
				cpu_to_le16(le16_to_cpu(neh->eh_entries) + m);
		}
		set_buffer_uptodate(bh);
		unlock_buffer(bh);

		err = ext4_journal_dirty_metadata(handle, bh);
		if (err)
			goto cleanup;
		brelse(bh);
		bh = NULL;

		/* correct old index */
		if (m) {
			err = ext4_ext_get_access(handle, inode, path + i);
			if (err)
				goto cleanup;
			path[i].p_hdr->eh_entries = cpu_to_le16(le16_to_cpu(path[i].p_hdr->eh_entries)-m);
			err = ext4_ext_dirty(handle, inode, path + i);
			if (err)
				goto cleanup;
		}

		i--;
	}

	/* insert new index */
	err = ext4_ext_insert_index(handle, inode, path + at,
				    le32_to_cpu(border), newblock);

cleanup:
	if (bh) {
		if (buffer_locked(bh))
			unlock_buffer(bh);
		brelse(bh);
	}

	if (err) {
		/* free all allocated blocks in error case */
		for (i = 0; i < depth; i++) {
			if (!ablocks[i])
				continue;
			ext4_free_blocks(handle, inode, ablocks[i], 1);
		}
	}
	kfree(ablocks);

	return err;
}

/*
 * ext4_ext_grow_indepth:
 * implements tree growing procedure:
 * - allocates new block
 * - moves top-level data (index block or leaf) into the new block
 * - initializes new top-level, creating index that points to the
 *   just created block
 */
static int ext4_ext_grow_indepth(handle_t *handle, struct inode *inode,
					struct ext4_ext_path *path,
					struct ext4_extent *newext)
{
	struct ext4_ext_path *curp = path;
	struct ext4_extent_header *neh;
	struct ext4_extent_idx *fidx;
	struct buffer_head *bh;
	ext4_fsblk_t newblock;
	int err = 0;

	newblock = ext4_ext_new_block(handle, inode, path, newext, &err);
	if (newblock == 0)
		return err;

	bh = sb_getblk(inode->i_sb, newblock);
	if (!bh) {
		err = -EIO;
		ext4_std_error(inode->i_sb, err);
		return err;
	}
	lock_buffer(bh);

	err = ext4_journal_get_create_access(handle, bh);
	if (err) {
		unlock_buffer(bh);
		goto out;
	}

	/* move top-level index/leaf into new block */
	memmove(bh->b_data, curp->p_hdr, sizeof(EXT4_I(inode)->i_data));

	/* set size of new block */
	neh = ext_block_hdr(bh);
	/* old root could have indexes or leaves
	 * so calculate e_max right way */
	if (ext_depth(inode))
	  neh->eh_max = cpu_to_le16(ext4_ext_space_block_idx(inode));
	else
	  neh->eh_max = cpu_to_le16(ext4_ext_space_block(inode));
	neh->eh_magic = EXT4_EXT_MAGIC;
	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	err = ext4_journal_dirty_metadata(handle, bh);
	if (err)
		goto out;

	/* create index in new top-level index: num,max,pointer */
	err = ext4_ext_get_access(handle, inode, curp);
	if (err)
		goto out;

	curp->p_hdr->eh_magic = EXT4_EXT_MAGIC;
	curp->p_hdr->eh_max = cpu_to_le16(ext4_ext_space_root_idx(inode));
	curp->p_hdr->eh_entries = cpu_to_le16(1);
	curp->p_idx = EXT_FIRST_INDEX(curp->p_hdr);

	if (path[0].p_hdr->eh_depth)
		curp->p_idx->ei_block =
			EXT_FIRST_INDEX(path[0].p_hdr)->ei_block;
	else
		curp->p_idx->ei_block =
			EXT_FIRST_EXTENT(path[0].p_hdr)->ee_block;
	ext4_idx_store_pblock(curp->p_idx, newblock);

	neh = ext_inode_hdr(inode);
	fidx = EXT_FIRST_INDEX(neh);
	ext_debug("new root: num %d(%d), lblock %d, ptr %llu\n",
		  le16_to_cpu(neh->eh_entries), le16_to_cpu(neh->eh_max),
		  le32_to_cpu(fidx->ei_block), idx_pblock(fidx));

	neh->eh_depth = cpu_to_le16(path->p_depth + 1);
	err = ext4_ext_dirty(handle, inode, curp);
out:
	brelse(bh);

	return err;
}

/*
 * ext4_ext_create_new_leaf:
 * finds empty index and adds new leaf.
 * if no free index is found, then it requests in-depth growing.
 */
static int ext4_ext_create_new_leaf(handle_t *handle, struct inode *inode,
					struct ext4_ext_path *path,
					struct ext4_extent *newext)
{
	struct ext4_ext_path *curp;
	int depth, i, err = 0;

repeat:
	i = depth = ext_depth(inode);

	/* walk up to the tree and look for free index entry */
	curp = path + depth;
	while (i > 0 && !EXT_HAS_FREE_INDEX(curp)) {
		i--;
		curp--;
	}

	/* we use already allocated block for index block,
	 * so subsequent data blocks should be contiguous */
	if (EXT_HAS_FREE_INDEX(curp)) {
		/* if we found index with free entry, then use that
		 * entry: create all needed subtree and add new leaf */
		err = ext4_ext_split(handle, inode, path, newext, i);

		/* refill path */
		ext4_ext_drop_refs(path);
		path = ext4_ext_find_extent(inode,
					    le32_to_cpu(newext->ee_block),
					    path);
		if (IS_ERR(path))
			err = PTR_ERR(path);
	} else {
		/* tree is full, time to grow in depth */
		err = ext4_ext_grow_indepth(handle, inode, path, newext);
		if (err)
			goto out;

		/* refill path */
		ext4_ext_drop_refs(path);
		path = ext4_ext_find_extent(inode,
					    le32_to_cpu(newext->ee_block),
					    path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			goto out;
		}

		/*
		 * only first (depth 0 -> 1) produces free space;
		 * in all other cases we have to split the grown tree
		 */
		depth = ext_depth(inode);
		if (path[depth].p_hdr->eh_entries == path[depth].p_hdr->eh_max) {
			/* now we need to split */
			goto repeat;
		}
	}

out:
	return err;
}

/*
 * ext4_ext_next_allocated_block:
 * returns allocated block in subsequent extent or EXT_MAX_BLOCK.
 * NOTE: it considers block number from index entry as
 * allocated block. Thus, index entries have to be consistent
 * with leaves.
 */
static unsigned long
ext4_ext_next_allocated_block(struct ext4_ext_path *path)
{
	int depth;

	BUG_ON(path == NULL);
	depth = path->p_depth;

	if (depth == 0 && path->p_ext == NULL)
		return EXT_MAX_BLOCK;

	while (depth >= 0) {
		if (depth == path->p_depth) {
			/* leaf */
			if (path[depth].p_ext !=
					EXT_LAST_EXTENT(path[depth].p_hdr))
			  return le32_to_cpu(path[depth].p_ext[1].ee_block);
		} else {
			/* index */
			if (path[depth].p_idx !=
					EXT_LAST_INDEX(path[depth].p_hdr))
			  return le32_to_cpu(path[depth].p_idx[1].ei_block);
		}
		depth--;
	}

	return EXT_MAX_BLOCK;
}

/*
 * ext4_ext_next_leaf_block:
 * returns first allocated block from next leaf or EXT_MAX_BLOCK
 */
static unsigned ext4_ext_next_leaf_block(struct inode *inode,
					struct ext4_ext_path *path)
{
	int depth;

	BUG_ON(path == NULL);
	depth = path->p_depth;

	/* zero-tree has no leaf blocks at all */
	if (depth == 0)
		return EXT_MAX_BLOCK;

	/* go to index block */
	depth--;

	while (depth >= 0) {
		if (path[depth].p_idx !=
				EXT_LAST_INDEX(path[depth].p_hdr))
		  return le32_to_cpu(path[depth].p_idx[1].ei_block);
		depth--;
	}

	return EXT_MAX_BLOCK;
}

/*
 * ext4_ext_correct_indexes:
 * if leaf gets modified and modified extent is first in the leaf,
 * then we have to correct all indexes above.
 * TODO: do we need to correct tree in all cases?
 */
int ext4_ext_correct_indexes(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	struct ext4_extent_header *eh;
	int depth = ext_depth(inode);
	struct ext4_extent *ex;
	__le32 border;
	int k, err = 0;

	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;
	BUG_ON(ex == NULL);
	BUG_ON(eh == NULL);

	if (depth == 0) {
		/* there is no tree at all */
		return 0;
	}

	if (ex != EXT_FIRST_EXTENT(eh)) {
		/* we correct tree if first leaf got modified only */
		return 0;
	}

	/*
	 * TODO: we need correction if border is smaller than current one
	 */
	k = depth - 1;
	border = path[depth].p_ext->ee_block;
	err = ext4_ext_get_access(handle, inode, path + k);
	if (err)
		return err;
	path[k].p_idx->ei_block = border;
	err = ext4_ext_dirty(handle, inode, path + k);
	if (err)
		return err;

	while (k--) {
		/* change all left-side indexes */
		if (path[k+1].p_idx != EXT_FIRST_INDEX(path[k+1].p_hdr))
			break;
		err = ext4_ext_get_access(handle, inode, path + k);
		if (err)
			break;
		path[k].p_idx->ei_block = border;
		err = ext4_ext_dirty(handle, inode, path + k);
		if (err)
			break;
	}

	return err;
}

static int
ext4_can_extents_be_merged(struct inode *inode, struct ext4_extent *ex1,
				struct ext4_extent *ex2)
{
	unsigned short ext1_ee_len, ext2_ee_len, max_len;

	/*
	 * Make sure that either both extents are uninitialized, or
	 * both are _not_.
	 */
	if (ext4_ext_is_uninitialized(ex1) ^ ext4_ext_is_uninitialized(ex2))
		return 0;

	if (ext4_ext_is_uninitialized(ex1))
		max_len = EXT_UNINIT_MAX_LEN;
	else
		max_len = EXT_INIT_MAX_LEN;

	ext1_ee_len = ext4_ext_get_actual_len(ex1);
	ext2_ee_len = ext4_ext_get_actual_len(ex2);

	if (le32_to_cpu(ex1->ee_block) + ext1_ee_len !=
			le32_to_cpu(ex2->ee_block))
		return 0;

	/*
	 * To allow future support for preallocated extents to be added
	 * as an RO_COMPAT feature, refuse to merge to extents if
	 * this can result in the top bit of ee_len being set.
	 */
	if (ext1_ee_len + ext2_ee_len > max_len)
		return 0;
#ifdef AGGRESSIVE_TEST
	if (le16_to_cpu(ex1->ee_len) >= 4)
		return 0;
#endif

	if (ext_pblock(ex1) + ext1_ee_len == ext_pblock(ex2))
		return 1;
	return 0;
}

/*
 * This function tries to merge the "ex" extent to the next extent in the tree.
 * It always tries to merge towards right. If you want to merge towards
 * left, pass "ex - 1" as argument instead of "ex".
 * Returns 0 if the extents (ex and ex+1) were _not_ merged and returns
 * 1 if they got merged.
 */
int ext4_ext_try_to_merge(struct inode *inode,
			  struct ext4_ext_path *path,
			  struct ext4_extent *ex)
{
	struct ext4_extent_header *eh;
	unsigned int depth, len;
	int merge_done = 0;
	int uninitialized = 0;

	depth = ext_depth(inode);
	BUG_ON(path[depth].p_hdr == NULL);
	eh = path[depth].p_hdr;

	while (ex < EXT_LAST_EXTENT(eh)) {
		if (!ext4_can_extents_be_merged(inode, ex, ex + 1))
			break;
		/* merge with next extent! */
		if (ext4_ext_is_uninitialized(ex))
			uninitialized = 1;
		ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
				+ ext4_ext_get_actual_len(ex + 1));
		if (uninitialized)
			ext4_ext_mark_uninitialized(ex);

		if (ex + 1 < EXT_LAST_EXTENT(eh)) {
			len = (EXT_LAST_EXTENT(eh) - ex - 1)
				* sizeof(struct ext4_extent);
			memmove(ex + 1, ex + 2, len);
		}
		eh->eh_entries = cpu_to_le16(le16_to_cpu(eh->eh_entries) - 1);
		merge_done = 1;
		WARN_ON(eh->eh_entries == 0);
		if (!eh->eh_entries)
			ext4_error(inode->i_sb, "ext4_ext_try_to_merge",
			   "inode#%lu, eh->eh_entries = 0!", inode->i_ino);
	}

	return merge_done;
}

/*
 * check if a portion of the "newext" extent overlaps with an
 * existing extent.
 *
 * If there is an overlap discovered, it updates the length of the newext
 * such that there will be no overlap, and then returns 1.
 * If there is no overlap found, it returns 0.
 */
unsigned int ext4_ext_check_overlap(struct inode *inode,
				    struct ext4_extent *newext,
				    struct ext4_ext_path *path)
{
	unsigned long b1, b2;
	unsigned int depth, len1;
	unsigned int ret = 0;

	b1 = le32_to_cpu(newext->ee_block);
	len1 = ext4_ext_get_actual_len(newext);
	depth = ext_depth(inode);
	if (!path[depth].p_ext)
		goto out;
	b2 = le32_to_cpu(path[depth].p_ext->ee_block);

	/*
	 * get the next allocated block if the extent in the path
	 * is before the requested block(s) 
	 */
	if (b2 < b1) {
		b2 = ext4_ext_next_allocated_block(path);
		if (b2 == EXT_MAX_BLOCK)
			goto out;
	}

	/* check for wrap through zero */
	if (b1 + len1 < b1) {
		len1 = EXT_MAX_BLOCK - b1;
		newext->ee_len = cpu_to_le16(len1);
		ret = 1;
	}

	/* check for overlap */
	if (b1 + len1 > b2) {
		newext->ee_len = cpu_to_le16(b2 - b1);
		ret = 1;
	}
out:
	return ret;
}

/*
 * ext4_ext_insert_extent:
 * tries to merge requsted extent into the existing extent or
 * inserts requested extent as new one into the tree,
 * creating new leaf in the no-space case.
 */
int ext4_ext_insert_extent(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path,
				struct ext4_extent *newext)
{
	struct ext4_extent_header * eh;
	struct ext4_extent *ex, *fex;
	struct ext4_extent *nearex; /* nearest extent */
	struct ext4_ext_path *npath = NULL;
	int depth, len, err, next;
	unsigned uninitialized = 0;

	BUG_ON(ext4_ext_get_actual_len(newext) == 0);
	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	BUG_ON(path[depth].p_hdr == NULL);

	/* try to insert block into found extent and return */
	if (ex && ext4_can_extents_be_merged(inode, ex, newext)) {
		ext_debug("append %d block to %d:%d (from %llu)\n",
				ext4_ext_get_actual_len(newext),
				le32_to_cpu(ex->ee_block),
				ext4_ext_get_actual_len(ex), ext_pblock(ex));
		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			return err;

		/*
		 * ext4_can_extents_be_merged should have checked that either
		 * both extents are uninitialized, or both aren't. Thus we
		 * need to check only one of them here.
		 */
		if (ext4_ext_is_uninitialized(ex))
			uninitialized = 1;
		ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
					+ ext4_ext_get_actual_len(newext));
		if (uninitialized)
			ext4_ext_mark_uninitialized(ex);
		eh = path[depth].p_hdr;
		nearex = ex;
		goto merge;
	}

repeat:
	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
	if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max))
		goto has_space;

	/* probably next leaf has space for us? */
	fex = EXT_LAST_EXTENT(eh);
	next = ext4_ext_next_leaf_block(inode, path);
	if (le32_to_cpu(newext->ee_block) > le32_to_cpu(fex->ee_block)
	    && next != EXT_MAX_BLOCK) {
		ext_debug("next leaf block - %d\n", next);
		BUG_ON(npath != NULL);
		npath = ext4_ext_find_extent(inode, next, NULL);
		if (IS_ERR(npath))
			return PTR_ERR(npath);
		BUG_ON(npath->p_depth != path->p_depth);
		eh = npath[depth].p_hdr;
		if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max)) {
			ext_debug("next leaf isnt full(%d)\n",
				  le16_to_cpu(eh->eh_entries));
			path = npath;
			goto repeat;
		}
		ext_debug("next leaf has no free space(%d,%d)\n",
			  le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));
	}

	/*
	 * There is no free space in the found leaf.
	 * We're gonna add a new leaf in the tree.
	 */
	err = ext4_ext_create_new_leaf(handle, inode, path, newext);
	if (err)
		goto cleanup;
	depth = ext_depth(inode);
	eh = path[depth].p_hdr;

has_space:
	nearex = path[depth].p_ext;

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto cleanup;

	if (!nearex) {
		/* there is no extent in this leaf, create first one */
		ext_debug("first extent in the leaf: %d:%llu:%d\n",
				le32_to_cpu(newext->ee_block),
				ext_pblock(newext),
				ext4_ext_get_actual_len(newext));
		path[depth].p_ext = EXT_FIRST_EXTENT(eh);
	} else if (le32_to_cpu(newext->ee_block)
			   > le32_to_cpu(nearex->ee_block)) {
/*		BUG_ON(newext->ee_block == nearex->ee_block); */
		if (nearex != EXT_LAST_EXTENT(eh)) {
			len = EXT_MAX_EXTENT(eh) - nearex;
			len = (len - 1) * sizeof(struct ext4_extent);
			len = len < 0 ? 0 : len;
			ext_debug("insert %d:%llu:%d after: nearest 0x%p, "
					"move %d from 0x%p to 0x%p\n",
					le32_to_cpu(newext->ee_block),
					ext_pblock(newext),
					ext4_ext_get_actual_len(newext),
					nearex, len, nearex + 1, nearex + 2);
			memmove(nearex + 2, nearex + 1, len);
		}
		path[depth].p_ext = nearex + 1;
	} else {
		BUG_ON(newext->ee_block == nearex->ee_block);
		len = (EXT_MAX_EXTENT(eh) - nearex) * sizeof(struct ext4_extent);
		len = len < 0 ? 0 : len;
		ext_debug("insert %d:%llu:%d before: nearest 0x%p, "
				"move %d from 0x%p to 0x%p\n",
				le32_to_cpu(newext->ee_block),
				ext_pblock(newext),
				ext4_ext_get_actual_len(newext),
				nearex, len, nearex + 1, nearex + 2);
		memmove(nearex + 1, nearex, len);
		path[depth].p_ext = nearex;
	}

	eh->eh_entries = cpu_to_le16(le16_to_cpu(eh->eh_entries)+1);
	nearex = path[depth].p_ext;
	nearex->ee_block = newext->ee_block;
	nearex->ee_start = newext->ee_start;
	nearex->ee_start_hi = newext->ee_start_hi;
	nearex->ee_len = newext->ee_len;

merge:
	/* try to merge extents to the right */
	ext4_ext_try_to_merge(inode, path, nearex);

	/* try to merge extents to the left */

	/* time to correct all indexes above */
	err = ext4_ext_correct_indexes(handle, inode, path);
	if (err)
		goto cleanup;

	err = ext4_ext_dirty(handle, inode, path + depth);

cleanup:
	if (npath) {
		ext4_ext_drop_refs(npath);
		kfree(npath);
	}
	ext4_ext_tree_changed(inode);
	ext4_ext_invalidate_cache(inode);
	return err;
}

int ext4_ext_walk_space(struct inode *inode, unsigned long block,
			unsigned long num, ext_prepare_callback func,
			void *cbdata)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_ext_cache cbex;
	struct ext4_extent *ex;
	unsigned long next, start = 0, end = 0;
	unsigned long last = block + num;
	int depth, exists, err = 0;

	BUG_ON(func == NULL);
	BUG_ON(inode == NULL);

	while (block < last && block != EXT_MAX_BLOCK) {
		num = last - block;
		/* find extent for this block */
		path = ext4_ext_find_extent(inode, block, path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			path = NULL;
			break;
		}

		depth = ext_depth(inode);
		BUG_ON(path[depth].p_hdr == NULL);
		ex = path[depth].p_ext;
		next = ext4_ext_next_allocated_block(path);

		exists = 0;
		if (!ex) {
			/* there is no extent yet, so try to allocate
			 * all requested space */
			start = block;
			end = block + num;
		} else if (le32_to_cpu(ex->ee_block) > block) {
			/* need to allocate space before found extent */
			start = block;
			end = le32_to_cpu(ex->ee_block);
			if (block + num < end)
				end = block + num;
		} else if (block >= le32_to_cpu(ex->ee_block)
					+ ext4_ext_get_actual_len(ex)) {
			/* need to allocate space after found extent */
			start = block;
			end = block + num;
			if (end >= next)
				end = next;
		} else if (block >= le32_to_cpu(ex->ee_block)) {
			/*
			 * some part of requested space is covered
			 * by found extent
			 */
			start = block;
			end = le32_to_cpu(ex->ee_block)
				+ ext4_ext_get_actual_len(ex);
			if (block + num < end)
				end = block + num;
			exists = 1;
		} else {
			BUG();
		}
		BUG_ON(end <= start);

		if (!exists) {
			cbex.ec_block = start;
			cbex.ec_len = end - start;
			cbex.ec_start = 0;
			cbex.ec_type = EXT4_EXT_CACHE_GAP;
		} else {
			cbex.ec_block = le32_to_cpu(ex->ee_block);
			cbex.ec_len = ext4_ext_get_actual_len(ex);
			cbex.ec_start = ext_pblock(ex);
			cbex.ec_type = EXT4_EXT_CACHE_EXTENT;
		}

		BUG_ON(cbex.ec_len == 0);
		err = func(inode, path, &cbex, cbdata);
		ext4_ext_drop_refs(path);

		if (err < 0)
			break;
		if (err == EXT_REPEAT)
			continue;
		else if (err == EXT_BREAK) {
			err = 0;
			break;
		}

		if (ext_depth(inode) != depth) {
			/* depth was changed. we have to realloc path */
			kfree(path);
			path = NULL;
		}

		block = cbex.ec_block + cbex.ec_len;
	}

	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}

	return err;
}

static void
ext4_ext_put_in_cache(struct inode *inode, __u32 block,
			__u32 len, ext4_fsblk_t start, int type)
{
	struct ext4_ext_cache *cex;
	BUG_ON(len == 0);
	cex = &EXT4_I(inode)->i_cached_extent;
	cex->ec_type = type;
	cex->ec_block = block;
	cex->ec_len = len;
	cex->ec_start = start;
}

/*
 * ext4_ext_put_gap_in_cache:
 * calculate boundaries of the gap that the requested block fits into
 * and cache this gap
 */
static void
ext4_ext_put_gap_in_cache(struct inode *inode, struct ext4_ext_path *path,
				unsigned long block)
{
	int depth = ext_depth(inode);
	unsigned long lblock, len;
	struct ext4_extent *ex;

	ex = path[depth].p_ext;
	if (ex == NULL) {
		/* there is no extent yet, so gap is [0;-] */
		lblock = 0;
		len = EXT_MAX_BLOCK;
		ext_debug("cache gap(whole file):");
	} else if (block < le32_to_cpu(ex->ee_block)) {
		lblock = block;
		len = le32_to_cpu(ex->ee_block) - block;
		ext_debug("cache gap(before): %lu [%lu:%lu]",
				(unsigned long) block,
				(unsigned long) le32_to_cpu(ex->ee_block),
				(unsigned long) ext4_ext_get_actual_len(ex));
	} else if (block >= le32_to_cpu(ex->ee_block)
			+ ext4_ext_get_actual_len(ex)) {
		lblock = le32_to_cpu(ex->ee_block)
			+ ext4_ext_get_actual_len(ex);
		len = ext4_ext_next_allocated_block(path);
		ext_debug("cache gap(after): [%lu:%lu] %lu",
				(unsigned long) le32_to_cpu(ex->ee_block),
				(unsigned long) ext4_ext_get_actual_len(ex),
				(unsigned long) block);
		BUG_ON(len == lblock);
		len = len - lblock;
	} else {
		lblock = len = 0;
		BUG();
	}

	ext_debug(" -> %lu:%lu\n", (unsigned long) lblock, len);
	ext4_ext_put_in_cache(inode, lblock, len, 0, EXT4_EXT_CACHE_GAP);
}

static int
ext4_ext_in_cache(struct inode *inode, unsigned long block,
			struct ext4_extent *ex)
{
	struct ext4_ext_cache *cex;

	cex = &EXT4_I(inode)->i_cached_extent;

	/* has cache valid data? */
	if (cex->ec_type == EXT4_EXT_CACHE_NO)
		return EXT4_EXT_CACHE_NO;

	BUG_ON(cex->ec_type != EXT4_EXT_CACHE_GAP &&
			cex->ec_type != EXT4_EXT_CACHE_EXTENT);
	if (block >= cex->ec_block && block < cex->ec_block + cex->ec_len) {
		ex->ee_block = cpu_to_le32(cex->ec_block);
		ext4_ext_store_pblock(ex, cex->ec_start);
		ex->ee_len = cpu_to_le16(cex->ec_len);
		ext_debug("%lu cached by %lu:%lu:%llu\n",
				(unsigned long) block,
				(unsigned long) cex->ec_block,
				(unsigned long) cex->ec_len,
				cex->ec_start);
		return cex->ec_type;
	}

	/* not in cache */
	return EXT4_EXT_CACHE_NO;
}

/*
 * ext4_ext_rm_idx:
 * removes index from the index block.
 * It's used in truncate case only, thus all requests are for
 * last index in the block only.
 */
int ext4_ext_rm_idx(handle_t *handle, struct inode *inode,
			struct ext4_ext_path *path)
{
	struct buffer_head *bh;
	int err;
	ext4_fsblk_t leaf;

	/* free index block */
	path--;
	leaf = idx_pblock(path->p_idx);
	BUG_ON(path->p_hdr->eh_entries == 0);
	err = ext4_ext_get_access(handle, inode, path);
	if (err)
		return err;
	path->p_hdr->eh_entries = cpu_to_le16(le16_to_cpu(path->p_hdr->eh_entries)-1);
	err = ext4_ext_dirty(handle, inode, path);
	if (err)
		return err;
	ext_debug("index is empty, remove it, free block %llu\n", leaf);
	bh = sb_find_get_block(inode->i_sb, leaf);
	ext4_forget(handle, 1, inode, bh, leaf);
	ext4_free_blocks(handle, inode, leaf, 1);
	return err;
}

/*
 * ext4_ext_calc_credits_for_insert:
 * This routine returns max. credits that the extent tree can consume.
 * It should be OK for low-performance paths like ->writepage()
 * To allow many writing processes to fit into a single transaction,
 * the caller should calculate credits under truncate_mutex and
 * pass the actual path.
 */
int ext4_ext_calc_credits_for_insert(struct inode *inode,
						struct ext4_ext_path *path)
{
	int depth, needed;

	if (path) {
		/* probably there is space in leaf? */
		depth = ext_depth(inode);
		if (le16_to_cpu(path[depth].p_hdr->eh_entries)
				< le16_to_cpu(path[depth].p_hdr->eh_max))
			return 1;
	}

	/*
	 * given 32-bit logical block (4294967296 blocks), max. tree
	 * can be 4 levels in depth -- 4 * 340^4 == 53453440000.
	 * Let's also add one more level for imbalance.
	 */
	depth = 5;

	/* allocation of new data block(s) */
	needed = 2;

	/*
	 * tree can be full, so it would need to grow in depth:
	 * we need one credit to modify old root, credits for
	 * new root will be added in split accounting
	 */
	needed += 1;

	/*
	 * Index split can happen, we would need:
	 *    allocate intermediate indexes (bitmap + group)
	 *  + change two blocks at each level, but root (already included)
	 */
	needed += (depth * 2) + (depth * 2);

	/* any allocation modifies superblock */
	needed += 1;

	return needed;
}

static int ext4_remove_blocks(handle_t *handle, struct inode *inode,
				struct ext4_extent *ex,
				unsigned long from, unsigned long to)
{
	struct buffer_head *bh;
	unsigned short ee_len =  ext4_ext_get_actual_len(ex);
	int i;

#ifdef EXTENTS_STATS
	{
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
		spin_lock(&sbi->s_ext_stats_lock);
		sbi->s_ext_blocks += ee_len;
		sbi->s_ext_extents++;
		if (ee_len < sbi->s_ext_min)
			sbi->s_ext_min = ee_len;
		if (ee_len > sbi->s_ext_max)
			sbi->s_ext_max = ee_len;
		if (ext_depth(inode) > sbi->s_depth_max)
			sbi->s_depth_max = ext_depth(inode);
		spin_unlock(&sbi->s_ext_stats_lock);
	}
#endif
	if (from >= le32_to_cpu(ex->ee_block)
	    && to == le32_to_cpu(ex->ee_block) + ee_len - 1) {
		/* tail removal */
		unsigned long num;
		ext4_fsblk_t start;
		num = le32_to_cpu(ex->ee_block) + ee_len - from;
		start = ext_pblock(ex) + ee_len - num;
		ext_debug("free last %lu blocks starting %llu\n", num, start);
		for (i = 0; i < num; i++) {
			bh = sb_find_get_block(inode->i_sb, start + i);
			ext4_forget(handle, 0, inode, bh, start + i);
		}
		ext4_free_blocks(handle, inode, start, num);
	} else if (from == le32_to_cpu(ex->ee_block)
		   && to <= le32_to_cpu(ex->ee_block) + ee_len - 1) {
		printk("strange request: removal %lu-%lu from %u:%u\n",
			from, to, le32_to_cpu(ex->ee_block), ee_len);
	} else {
		printk("strange request: removal(2) %lu-%lu from %u:%u\n",
			from, to, le32_to_cpu(ex->ee_block), ee_len);
	}
	return 0;
}

static int
ext4_ext_rm_leaf(handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path, unsigned long start)
{
	int err = 0, correct_index = 0;
	int depth = ext_depth(inode), credits;
	struct ext4_extent_header *eh;
	unsigned a, b, block, num;
	unsigned long ex_ee_block;
	unsigned short ex_ee_len;
	unsigned uninitialized = 0;
	struct ext4_extent *ex;

	/* the header must be checked already in ext4_ext_remove_space() */
	ext_debug("truncate since %lu in leaf\n", start);
	if (!path[depth].p_hdr)
		path[depth].p_hdr = ext_block_hdr(path[depth].p_bh);
	eh = path[depth].p_hdr;
	BUG_ON(eh == NULL);

	/* find where to start removing */
	ex = EXT_LAST_EXTENT(eh);

	ex_ee_block = le32_to_cpu(ex->ee_block);
	if (ext4_ext_is_uninitialized(ex))
		uninitialized = 1;
	ex_ee_len = ext4_ext_get_actual_len(ex);

	while (ex >= EXT_FIRST_EXTENT(eh) &&
			ex_ee_block + ex_ee_len > start) {
		ext_debug("remove ext %lu:%u\n", ex_ee_block, ex_ee_len);
		path[depth].p_ext = ex;

		a = ex_ee_block > start ? ex_ee_block : start;
		b = ex_ee_block + ex_ee_len - 1 < EXT_MAX_BLOCK ?
			ex_ee_block + ex_ee_len - 1 : EXT_MAX_BLOCK;

		ext_debug("  border %u:%u\n", a, b);

		if (a != ex_ee_block && b != ex_ee_block + ex_ee_len - 1) {
			block = 0;
			num = 0;
			BUG();
		} else if (a != ex_ee_block) {
			/* remove tail of the extent */
			block = ex_ee_block;
			num = a - block;
		} else if (b != ex_ee_block + ex_ee_len - 1) {
			/* remove head of the extent */
			block = a;
			num = b - a;
			/* there is no "make a hole" API yet */
			BUG();
		} else {
			/* remove whole extent: excellent! */
			block = ex_ee_block;
			num = 0;
			BUG_ON(a != ex_ee_block);
			BUG_ON(b != ex_ee_block + ex_ee_len - 1);
		}

		/* at present, extent can't cross block group: */
		/* leaf + bitmap + group desc + sb + inode */
		credits = 5;
		if (ex == EXT_FIRST_EXTENT(eh)) {
			correct_index = 1;
			credits += (ext_depth(inode)) + 1;
		}
#ifdef CONFIG_QUOTA
		credits += 2 * EXT4_QUOTA_TRANS_BLOCKS(inode->i_sb);
#endif

		handle = ext4_ext_journal_restart(handle, credits);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto out;
		}

		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto out;

		err = ext4_remove_blocks(handle, inode, ex, a, b);
		if (err)
			goto out;

		if (num == 0) {
			/* this extent is removed; mark slot entirely unused */
			ext4_ext_store_pblock(ex, 0);
			eh->eh_entries = cpu_to_le16(le16_to_cpu(eh->eh_entries)-1);
		}

		ex->ee_block = cpu_to_le32(block);
		ex->ee_len = cpu_to_le16(num);
		/*
		 * Do not mark uninitialized if all the blocks in the
		 * extent have been removed.
		 */
		if (uninitialized && num)
			ext4_ext_mark_uninitialized(ex);

		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto out;

		ext_debug("new extent: %u:%u:%llu\n", block, num,
				ext_pblock(ex));
		ex--;
		ex_ee_block = le32_to_cpu(ex->ee_block);
		ex_ee_len = ext4_ext_get_actual_len(ex);
	}

	if (correct_index && eh->eh_entries)
		err = ext4_ext_correct_indexes(handle, inode, path);

	/* if this leaf is free, then we should
	 * remove it from index block above */
	if (err == 0 && eh->eh_entries == 0 && path[depth].p_bh != NULL)
		err = ext4_ext_rm_idx(handle, inode, path + depth);

out:
	return err;
}

/*
 * ext4_ext_more_to_rm:
 * returns 1 if current index has to be freed (even partial)
 */
static int
ext4_ext_more_to_rm(struct ext4_ext_path *path)
{
	BUG_ON(path->p_idx == NULL);

	if (path->p_idx < EXT_FIRST_INDEX(path->p_hdr))
		return 0;

	/*
	 * if truncate on deeper level happened, it wasn't partial,
	 * so we have to consider current index for truncation
	 */
	if (le16_to_cpu(path->p_hdr->eh_entries) == path->p_block)
		return 0;
	return 1;
}

int ext4_ext_remove_space(struct inode *inode, unsigned long start)
{
	struct super_block *sb = inode->i_sb;
	int depth = ext_depth(inode);
	struct ext4_ext_path *path;
	handle_t *handle;
	int i = 0, err = 0;

	ext_debug("truncate since %lu\n", start);

	/* probably first extent we're gonna free will be last in block */
	handle = ext4_journal_start(inode, depth + 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ext4_ext_invalidate_cache(inode);

	/*
	 * We start scanning from right side, freeing all the blocks
	 * after i_size and walking into the tree depth-wise.
	 */
	path = kzalloc(sizeof(struct ext4_ext_path) * (depth + 1), GFP_KERNEL);
	if (path == NULL) {
		ext4_journal_stop(handle);
		return -ENOMEM;
	}
	path[0].p_hdr = ext_inode_hdr(inode);
	if (ext4_ext_check_header(inode, path[0].p_hdr, depth)) {
		err = -EIO;
		goto out;
	}
	path[0].p_depth = depth;

	while (i >= 0 && err == 0) {
		if (i == depth) {
			/* this is leaf block */
			err = ext4_ext_rm_leaf(handle, inode, path, start);
			/* root level has p_bh == NULL, brelse() eats this */
			brelse(path[i].p_bh);
			path[i].p_bh = NULL;
			i--;
			continue;
		}

		/* this is index block */
		if (!path[i].p_hdr) {
			ext_debug("initialize header\n");
			path[i].p_hdr = ext_block_hdr(path[i].p_bh);
		}

		if (!path[i].p_idx) {
			/* this level hasn't been touched yet */
			path[i].p_idx = EXT_LAST_INDEX(path[i].p_hdr);
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries)+1;
			ext_debug("init index ptr: hdr 0x%p, num %d\n",
				  path[i].p_hdr,
				  le16_to_cpu(path[i].p_hdr->eh_entries));
		} else {
			/* we were already here, see at next index */
			path[i].p_idx--;
		}

		ext_debug("level %d - index, first 0x%p, cur 0x%p\n",
				i, EXT_FIRST_INDEX(path[i].p_hdr),
				path[i].p_idx);
		if (ext4_ext_more_to_rm(path + i)) {
			struct buffer_head *bh;
			/* go to the next level */
			ext_debug("move to level %d (block %llu)\n",
				  i + 1, idx_pblock(path[i].p_idx));
			memset(path + i + 1, 0, sizeof(*path));
			bh = sb_bread(sb, idx_pblock(path[i].p_idx));
			if (!bh) {
				/* should we reset i_size? */
				err = -EIO;
				break;
			}
			if (WARN_ON(i + 1 > depth)) {
				err = -EIO;
				break;
			}
			if (ext4_ext_check_header(inode, ext_block_hdr(bh),
							depth - i - 1)) {
				err = -EIO;
				break;
			}
			path[i + 1].p_bh = bh;

			/* save actual number of indexes since this
			 * number is changed at the next iteration */
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries);
			i++;
		} else {
			/* we finished processing this index, go up */
			if (path[i].p_hdr->eh_entries == 0 && i > 0) {
				/* index is empty, remove it;
				 * handle must be already prepared by the
				 * truncatei_leaf() */
				err = ext4_ext_rm_idx(handle, inode, path + i);
			}
			/* root level has p_bh == NULL, brelse() eats this */
			brelse(path[i].p_bh);
			path[i].p_bh = NULL;
			i--;
			ext_debug("return to level %d\n", i);
		}
	}

	/* TODO: flexible tree reduction should be here */
	if (path->p_hdr->eh_entries == 0) {
		/*
		 * truncate to zero freed all the tree,
		 * so we need to correct eh_depth
		 */
		err = ext4_ext_get_access(handle, inode, path);
		if (err == 0) {
			ext_inode_hdr(inode)->eh_depth = 0;
			ext_inode_hdr(inode)->eh_max =
				cpu_to_le16(ext4_ext_space_root(inode));
			err = ext4_ext_dirty(handle, inode, path);
		}
	}
out:
	ext4_ext_tree_changed(inode);
	ext4_ext_drop_refs(path);
	kfree(path);
	ext4_journal_stop(handle);

	return err;
}

/*
 * called at mount time
 */
void ext4_ext_init(struct super_block *sb)
{
	/*
	 * possible initialization would be here
	 */

	if (test_opt(sb, EXTENTS)) {
		printk("EXT4-fs: file extents enabled");
#ifdef AGGRESSIVE_TEST
		printk(", aggressive tests");
#endif
#ifdef CHECK_BINSEARCH
		printk(", check binsearch");
#endif
#ifdef EXTENTS_STATS
		printk(", stats");
#endif
		printk("\n");
#ifdef EXTENTS_STATS
		spin_lock_init(&EXT4_SB(sb)->s_ext_stats_lock);
		EXT4_SB(sb)->s_ext_min = 1 << 30;
		EXT4_SB(sb)->s_ext_max = 0;
#endif
	}
}

/*
 * called at umount time
 */
void ext4_ext_release(struct super_block *sb)
{
	if (!test_opt(sb, EXTENTS))
		return;

#ifdef EXTENTS_STATS
	if (EXT4_SB(sb)->s_ext_blocks && EXT4_SB(sb)->s_ext_extents) {
		struct ext4_sb_info *sbi = EXT4_SB(sb);
		printk(KERN_ERR "EXT4-fs: %lu blocks in %lu extents (%lu ave)\n",
			sbi->s_ext_blocks, sbi->s_ext_extents,
			sbi->s_ext_blocks / sbi->s_ext_extents);
		printk(KERN_ERR "EXT4-fs: extents: %lu min, %lu max, max depth %lu\n",
			sbi->s_ext_min, sbi->s_ext_max, sbi->s_depth_max);
	}
#endif
}

/*
 * This function is called by ext4_ext_get_blocks() if someone tries to write
 * to an uninitialized extent. It may result in splitting the uninitialized
 * extent into multiple extents (upto three - one initialized and two
 * uninitialized).
 * There are three possibilities:
 *   a> There is no split required: Entire extent should be initialized
 *   b> Splits in two extents: Write is happening at either end of the extent
 *   c> Splits in three extents: Somone is writing in middle of the extent
 */
int ext4_ext_convert_to_initialized(handle_t *handle, struct inode *inode,
					struct ext4_ext_path *path,
					ext4_fsblk_t iblock,
					unsigned long max_blocks)
{
	struct ext4_extent *ex, newex;
	struct ext4_extent *ex1 = NULL;
	struct ext4_extent *ex2 = NULL;
	struct ext4_extent *ex3 = NULL;
	struct ext4_extent_header *eh;
	unsigned int allocated, ee_block, ee_len, depth;
	ext4_fsblk_t newblock;
	int err = 0;
	int ret = 0;

	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);
	allocated = ee_len - (iblock - ee_block);
	newblock = iblock - ee_block + ext_pblock(ex);
	ex2 = ex;

	/* ex1: ee_block to iblock - 1 : uninitialized */
	if (iblock > ee_block) {
		ex1 = ex;
		ex1->ee_len = cpu_to_le16(iblock - ee_block);
		ext4_ext_mark_uninitialized(ex1);
		ex2 = &newex;
	}
	/*
	 * for sanity, update the length of the ex2 extent before
	 * we insert ex3, if ex1 is NULL. This is to avoid temporary
	 * overlap of blocks.
	 */
	if (!ex1 && allocated > max_blocks)
		ex2->ee_len = cpu_to_le16(max_blocks);
	/* ex3: to ee_block + ee_len : uninitialised */
	if (allocated > max_blocks) {
		unsigned int newdepth;
		ex3 = &newex;
		ex3->ee_block = cpu_to_le32(iblock + max_blocks);
		ext4_ext_store_pblock(ex3, newblock + max_blocks);
		ex3->ee_len = cpu_to_le16(allocated - max_blocks);
		ext4_ext_mark_uninitialized(ex3);
		err = ext4_ext_insert_extent(handle, inode, path, ex3);
		if (err)
			goto out;
		/*
		 * The depth, and hence eh & ex might change
		 * as part of the insert above.
		 */
		newdepth = ext_depth(inode);
		if (newdepth != depth) {
			depth = newdepth;
			path = ext4_ext_find_extent(inode, iblock, NULL);
			if (IS_ERR(path)) {
				err = PTR_ERR(path);
				path = NULL;
				goto out;
			}
			eh = path[depth].p_hdr;
			ex = path[depth].p_ext;
			if (ex2 != &newex)
				ex2 = ex;
		}
		allocated = max_blocks;
	}
	/*
	 * If there was a change of depth as part of the
	 * insertion of ex3 above, we need to update the length
	 * of the ex1 extent again here
	 */
	if (ex1 && ex1 != ex) {
		ex1 = ex;
		ex1->ee_len = cpu_to_le16(iblock - ee_block);
		ext4_ext_mark_uninitialized(ex1);
		ex2 = &newex;
	}
	/* ex2: iblock to iblock + maxblocks-1 : initialised */
	ex2->ee_block = cpu_to_le32(iblock);
	ex2->ee_start = cpu_to_le32(newblock);
	ext4_ext_store_pblock(ex2, newblock);
	ex2->ee_len = cpu_to_le16(allocated);
	if (ex2 != ex)
		goto insert;
	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto out;
	/*
	 * New (initialized) extent starts from the first block
	 * in the current extent. i.e., ex2 == ex
	 * We have to see if it can be merged with the extent
	 * on the left.
	 */
	if (ex2 > EXT_FIRST_EXTENT(eh)) {
		/*
		 * To merge left, pass "ex2 - 1" to try_to_merge(),
		 * since it merges towards right _only_.
		 */
		ret = ext4_ext_try_to_merge(inode, path, ex2 - 1);
		if (ret) {
			err = ext4_ext_correct_indexes(handle, inode, path);
			if (err)
				goto out;
			depth = ext_depth(inode);
			ex2--;
		}
	}
	/*
	 * Try to Merge towards right. This might be required
	 * only when the whole extent is being written to.
	 * i.e. ex2 == ex and ex3 == NULL.
	 */
	if (!ex3) {
		ret = ext4_ext_try_to_merge(inode, path, ex2);
		if (ret) {
			err = ext4_ext_correct_indexes(handle, inode, path);
			if (err)
				goto out;
		}
	}
	/* Mark modified extent as dirty */
	err = ext4_ext_dirty(handle, inode, path + depth);
	goto out;
insert:
	err = ext4_ext_insert_extent(handle, inode, path, &newex);
out:
	return err ? err : allocated;
}

int ext4_ext_get_blocks(handle_t *handle, struct inode *inode,
			ext4_fsblk_t iblock,
			unsigned long max_blocks, struct buffer_head *bh_result,
			int create, int extend_disksize)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent_header *eh;
	struct ext4_extent newex, *ex;
	ext4_fsblk_t goal, newblock;
	int err = 0, depth, ret;
	unsigned long allocated = 0;

	__clear_bit(BH_New, &bh_result->b_state);
	ext_debug("blocks %d/%lu requested for inode %u\n", (int) iblock,
			max_blocks, (unsigned) inode->i_ino);
	mutex_lock(&EXT4_I(inode)->truncate_mutex);

	/* check in cache */
	goal = ext4_ext_in_cache(inode, iblock, &newex);
	if (goal) {
		if (goal == EXT4_EXT_CACHE_GAP) {
			if (!create) {
				/*
				 * block isn't allocated yet and
				 * user doesn't want to allocate it
				 */
				goto out2;
			}
			/* we should allocate requested block */
		} else if (goal == EXT4_EXT_CACHE_EXTENT) {
			/* block is already allocated */
			newblock = iblock
				   - le32_to_cpu(newex.ee_block)
				   + ext_pblock(&newex);
			/* number of remaining blocks in the extent */
			allocated = le16_to_cpu(newex.ee_len) -
					(iblock - le32_to_cpu(newex.ee_block));
			goto out;
		} else {
			BUG();
		}
	}

	/* find extent for this block */
	path = ext4_ext_find_extent(inode, iblock, NULL);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		path = NULL;
		goto out2;
	}

	depth = ext_depth(inode);

	/*
	 * consistent leaf must not be empty;
	 * this situation is possible, though, _during_ tree modification;
	 * this is why assert can't be put in ext4_ext_find_extent()
	 */
	BUG_ON(path[depth].p_ext == NULL && depth != 0);
	eh = path[depth].p_hdr;

	ex = path[depth].p_ext;
	if (ex) {
		unsigned long ee_block = le32_to_cpu(ex->ee_block);
		ext4_fsblk_t ee_start = ext_pblock(ex);
		unsigned short ee_len;

		/*
		 * Uninitialized extents are treated as holes, except that
		 * we split out initialized portions during a write.
		 */
		ee_len = ext4_ext_get_actual_len(ex);
		/* if found extent covers block, simply return it */
		if (iblock >= ee_block && iblock < ee_block + ee_len) {
			newblock = iblock - ee_block + ee_start;
			/* number of remaining blocks in the extent */
			allocated = ee_len - (iblock - ee_block);
			ext_debug("%d fit into %lu:%d -> %llu\n", (int) iblock,
					ee_block, ee_len, newblock);

			/* Do not put uninitialized extent in the cache */
			if (!ext4_ext_is_uninitialized(ex)) {
				ext4_ext_put_in_cache(inode, ee_block,
							ee_len, ee_start,
							EXT4_EXT_CACHE_EXTENT);
				goto out;
			}
			if (create == EXT4_CREATE_UNINITIALIZED_EXT)
				goto out;
			if (!create)
				goto out2;

			ret = ext4_ext_convert_to_initialized(handle, inode,
								path, iblock,
								max_blocks);
			if (ret <= 0)
				goto out2;
			else
				allocated = ret;
			goto outnew;
		}
	}

	/*
	 * requested block isn't allocated yet;
	 * we couldn't try to create block if create flag is zero
	 */
	if (!create) {
		/*
		 * put just found gap into cache to speed up
		 * subsequent requests
		 */
		ext4_ext_put_gap_in_cache(inode, path, iblock);
		goto out2;
	}
	/*
	 * Okay, we need to do block allocation.  Lazily initialize the block
	 * allocation info here if necessary.
	 */
	if (S_ISREG(inode->i_mode) && (!EXT4_I(inode)->i_block_alloc_info))
		ext4_init_block_alloc_info(inode);

	/* allocate new block */
	goal = ext4_ext_find_goal(inode, path, iblock);

	/*
	 * See if request is beyond maximum number of blocks we can have in
	 * a single extent. For an initialized extent this limit is
	 * EXT_INIT_MAX_LEN and for an uninitialized extent this limit is
	 * EXT_UNINIT_MAX_LEN.
	 */
	if (max_blocks > EXT_INIT_MAX_LEN &&
	    create != EXT4_CREATE_UNINITIALIZED_EXT)
		max_blocks = EXT_INIT_MAX_LEN;
	else if (max_blocks > EXT_UNINIT_MAX_LEN &&
		 create == EXT4_CREATE_UNINITIALIZED_EXT)
		max_blocks = EXT_UNINIT_MAX_LEN;

	/* Check if we can really insert (iblock)::(iblock+max_blocks) extent */
	newex.ee_block = cpu_to_le32(iblock);
	newex.ee_len = cpu_to_le16(max_blocks);
	err = ext4_ext_check_overlap(inode, &newex, path);
	if (err)
		allocated = le16_to_cpu(newex.ee_len);
	else
		allocated = max_blocks;
	newblock = ext4_new_blocks(handle, inode, goal, &allocated, &err);
	if (!newblock)
		goto out2;
	ext_debug("allocate new block: goal %llu, found %llu/%lu\n",
			goal, newblock, allocated);

	/* try to insert new extent into found leaf and return */
	ext4_ext_store_pblock(&newex, newblock);
	newex.ee_len = cpu_to_le16(allocated);
	if (create == EXT4_CREATE_UNINITIALIZED_EXT)  /* Mark uninitialized */
		ext4_ext_mark_uninitialized(&newex);
	err = ext4_ext_insert_extent(handle, inode, path, &newex);
	if (err) {
		/* free data blocks we just allocated */
		ext4_free_blocks(handle, inode, ext_pblock(&newex),
					le16_to_cpu(newex.ee_len));
		goto out2;
	}

	if (extend_disksize && inode->i_size > EXT4_I(inode)->i_disksize)
		EXT4_I(inode)->i_disksize = inode->i_size;

	/* previous routine could use block we allocated */
	newblock = ext_pblock(&newex);
outnew:
	__set_bit(BH_New, &bh_result->b_state);

	/* Cache only when it is _not_ an uninitialized extent */
	if (create != EXT4_CREATE_UNINITIALIZED_EXT)
		ext4_ext_put_in_cache(inode, iblock, allocated, newblock,
						EXT4_EXT_CACHE_EXTENT);
out:
	if (allocated > max_blocks)
		allocated = max_blocks;
	ext4_ext_show_leaf(inode, path);
	__set_bit(BH_Mapped, &bh_result->b_state);
	bh_result->b_bdev = inode->i_sb->s_bdev;
	bh_result->b_blocknr = newblock;
out2:
	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}
	mutex_unlock(&EXT4_I(inode)->truncate_mutex);

	return err ? err : allocated;
}

void ext4_ext_truncate(struct inode * inode, struct page *page)
{
	struct address_space *mapping = inode->i_mapping;
	struct super_block *sb = inode->i_sb;
	unsigned long last_block;
	handle_t *handle;
	int err = 0;

	/*
	 * probably first extent we're gonna free will be last in block
	 */
	err = ext4_writepage_trans_blocks(inode) + 3;
	handle = ext4_journal_start(inode, err);
	if (IS_ERR(handle)) {
		if (page) {
			clear_highpage(page);
			flush_dcache_page(page);
			unlock_page(page);
			page_cache_release(page);
		}
		return;
	}

	if (page)
		ext4_block_truncate_page(handle, page, mapping, inode->i_size);

	mutex_lock(&EXT4_I(inode)->truncate_mutex);
	ext4_ext_invalidate_cache(inode);

	/*
	 * TODO: optimization is possible here.
	 * Probably we need not scan at all,
	 * because page truncation is enough.
	 */
	if (ext4_orphan_add(handle, inode))
		goto out_stop;

	/* we have to know where to truncate from in crash case */
	EXT4_I(inode)->i_disksize = inode->i_size;
	ext4_mark_inode_dirty(handle, inode);

	last_block = (inode->i_size + sb->s_blocksize - 1)
			>> EXT4_BLOCK_SIZE_BITS(sb);
	err = ext4_ext_remove_space(inode, last_block);

	/* In a multi-transaction truncate, we only make the final
	 * transaction synchronous.
	 */
	if (IS_SYNC(inode))
		handle->h_sync = 1;

out_stop:
	/*
	 * If this was a simple ftruncate() and the file will remain alive,
	 * then we need to clear up the orphan record which we created above.
	 * However, if this was a real unlink then we were called by
	 * ext4_delete_inode(), and we allow that function to clean up the
	 * orphan info for us.
	 */
	if (inode->i_nlink)
		ext4_orphan_del(handle, inode);

	mutex_unlock(&EXT4_I(inode)->truncate_mutex);
	ext4_journal_stop(handle);
}

/*
 * ext4_ext_writepage_trans_blocks:
 * calculate max number of blocks we could modify
 * in order to allocate new block for an inode
 */
int ext4_ext_writepage_trans_blocks(struct inode *inode, int num)
{
	int needed;

	needed = ext4_ext_calc_credits_for_insert(inode, NULL);

	/* caller wants to allocate num blocks, but note it includes sb */
	needed = needed * num - (num - 1);

#ifdef CONFIG_QUOTA
	needed += 2 * EXT4_QUOTA_TRANS_BLOCKS(inode->i_sb);
#endif

	return needed;
}

/*
 * preallocate space for a file. This implements ext4's fallocate inode
 * operation, which gets called from sys_fallocate system call.
 * For block-mapped files, posix_fallocate should fall back to the method
 * of writing zeroes to the required new blocks (the same behavior which is
 * expected for file systems which do not support fallocate() system call).
 */
long ext4_fallocate(struct inode *inode, int mode, loff_t offset, loff_t len)
{
	handle_t *handle;
	ext4_fsblk_t block, max_blocks;
	ext4_fsblk_t nblocks = 0;
	int ret = 0;
	int ret2 = 0;
	int retries = 0;
	struct buffer_head map_bh;
	unsigned int credits, blkbits = inode->i_blkbits;

	/*
	 * currently supporting (pre)allocate mode for extent-based
	 * files _only_
	 */
	if (!(EXT4_I(inode)->i_flags & EXT4_EXTENTS_FL))
		return -EOPNOTSUPP;

	/* preallocation to directories is currently not supported */
	if (S_ISDIR(inode->i_mode))
		return -ENODEV;

	block = offset >> blkbits;
	max_blocks = (EXT4_BLOCK_ALIGN(len + offset, blkbits) >> blkbits)
			- block;

	/*
	 * credits to insert 1 extent into extent tree + buffers to be able to
	 * modify 1 super block, 1 block bitmap and 1 group descriptor.
	 */
	credits = EXT4_DATA_TRANS_BLOCKS(inode->i_sb) + 3;
retry:
	while (ret >= 0 && ret < max_blocks) {
		block = block + ret;
		max_blocks = max_blocks - ret;
		handle = ext4_journal_start(inode, credits);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			break;
		}

		ret = ext4_ext_get_blocks(handle, inode, block,
					  max_blocks, &map_bh,
					  EXT4_CREATE_UNINITIALIZED_EXT, 0);
		WARN_ON(!ret);
		if (!ret) {
			ext4_error(inode->i_sb, "ext4_fallocate",
				   "ext4_ext_get_blocks returned 0! inode#%lu"
				   ", block=%llu, max_blocks=%llu",
				   inode->i_ino, block, max_blocks);
			ret = -EIO;
			ext4_mark_inode_dirty(handle, inode);
			ret2 = ext4_journal_stop(handle);
			break;
		}
		if (ret > 0) {
			/* check wrap through sign-bit/zero here */
			if ((block + ret) < 0 || (block + ret) < block) {
				ret = -EIO;
				ext4_mark_inode_dirty(handle, inode);
				ret2 = ext4_journal_stop(handle);
				break;
			}
			if (buffer_new(&map_bh) && ((block + ret) >
			    (EXT4_BLOCK_ALIGN(i_size_read(inode), blkbits)
			    >> blkbits)))
					nblocks = nblocks + ret;
		}

		/* Update ctime if new blocks get allocated */
		if (nblocks) {
			struct timespec now;

			now = current_fs_time(inode->i_sb);
			if (!timespec_equal(&inode->i_ctime, &now))
				inode->i_ctime = now;
		}

		ext4_mark_inode_dirty(handle, inode);
		ret2 = ext4_journal_stop(handle);
		if (ret2)
			break;
	}

	if (ret == -ENOSPC && ext4_should_retry_alloc(inode->i_sb, &retries))
		goto retry;

	/*
	 * Time to update the file size.
	 * Update only when preallocation was requested beyond the file size.
	 */
	if (!(mode & FALLOC_FL_KEEP_SIZE) &&
	    (offset + len) > i_size_read(inode)) {
		if (ret > 0) {
			/*
			 * if no error, we assume preallocation succeeded
			 * completely
			 */
			mutex_lock(&inode->i_mutex);
			i_size_write(inode, offset + len);
			EXT4_I(inode)->i_disksize = i_size_read(inode);
			mutex_unlock(&inode->i_mutex);
		} else if (ret < 0 && nblocks) {
			/* Handle partial allocation scenario */
			loff_t newsize;

			mutex_lock(&inode->i_mutex);
			newsize  = (nblocks << blkbits) + i_size_read(inode);
			i_size_write(inode, EXT4_BLOCK_ALIGN(newsize, blkbits));
			EXT4_I(inode)->i_disksize = i_size_read(inode);
			mutex_unlock(&inode->i_mutex);
		}
	}

	return ret > 0 ? ret2 : ret;
}
