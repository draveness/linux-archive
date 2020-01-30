/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * io.c
 *
 * Buffer cache handling
 *
 * Copyright (C) 2002, 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/highmem.h>

#include <cluster/masklog.h>

#include "ocfs2.h"

#include "alloc.h"
#include "inode.h"
#include "journal.h"
#include "uptodate.h"

#include "buffer_head_io.h"

int ocfs2_write_block(struct ocfs2_super *osb, struct buffer_head *bh,
		      struct inode *inode)
{
	int ret = 0;

	mlog_entry("(bh->b_blocknr = %llu, inode=%p)\n",
		   (unsigned long long)bh->b_blocknr, inode);

	BUG_ON(bh->b_blocknr < OCFS2_SUPER_BLOCK_BLKNO);
	BUG_ON(buffer_jbd(bh));

	/* No need to check for a soft readonly file system here. non
	 * journalled writes are only ever done on system files which
	 * can get modified during recovery even if read-only. */
	if (ocfs2_is_hard_readonly(osb)) {
		ret = -EROFS;
		goto out;
	}

	mutex_lock(&OCFS2_I(inode)->ip_io_mutex);

	lock_buffer(bh);
	set_buffer_uptodate(bh);

	/* remove from dirty list before I/O. */
	clear_buffer_dirty(bh);

	get_bh(bh); /* for end_buffer_write_sync() */                   
	bh->b_end_io = end_buffer_write_sync;
	submit_bh(WRITE, bh);

	wait_on_buffer(bh);

	if (buffer_uptodate(bh)) {
		ocfs2_set_buffer_uptodate(inode, bh);
	} else {
		/* We don't need to remove the clustered uptodate
		 * information for this bh as it's not marked locally
		 * uptodate. */
		ret = -EIO;
		brelse(bh);
	}

	mutex_unlock(&OCFS2_I(inode)->ip_io_mutex);
out:
	mlog_exit(ret);
	return ret;
}

int ocfs2_read_blocks(struct ocfs2_super *osb, u64 block, int nr,
		      struct buffer_head *bhs[], int flags,
		      struct inode *inode)
{
	int status = 0;
	struct super_block *sb;
	int i, ignore_cache = 0;
	struct buffer_head *bh;

	mlog_entry("(block=(%llu), nr=(%d), flags=%d, inode=%p)\n",
		   (unsigned long long)block, nr, flags, inode);

	BUG_ON((flags & OCFS2_BH_READAHEAD) &&
	       (!inode || !(flags & OCFS2_BH_CACHED)));

	if (osb == NULL || osb->sb == NULL || bhs == NULL) {
		status = -EINVAL;
		mlog_errno(status);
		goto bail;
	}

	if (nr < 0) {
		mlog(ML_ERROR, "asked to read %d blocks!\n", nr);
		status = -EINVAL;
		mlog_errno(status);
		goto bail;
	}

	if (nr == 0) {
		mlog(ML_BH_IO, "No buffers will be read!\n");
		status = 0;
		goto bail;
	}

	sb = osb->sb;

	if (flags & OCFS2_BH_CACHED && !inode)
		flags &= ~OCFS2_BH_CACHED;

	if (inode)
		mutex_lock(&OCFS2_I(inode)->ip_io_mutex);
	for (i = 0 ; i < nr ; i++) {
		if (bhs[i] == NULL) {
			bhs[i] = sb_getblk(sb, block++);
			if (bhs[i] == NULL) {
				if (inode)
					mutex_unlock(&OCFS2_I(inode)->ip_io_mutex);
				status = -EIO;
				mlog_errno(status);
				goto bail;
			}
		}
		bh = bhs[i];
		ignore_cache = 0;

		/* There are three read-ahead cases here which we need to
		 * be concerned with. All three assume a buffer has
		 * previously been submitted with OCFS2_BH_READAHEAD
		 * and it hasn't yet completed I/O.
		 *
		 * 1) The current request is sync to disk. This rarely
		 *    happens these days, and never when performance
		 *    matters - the code can just wait on the buffer
		 *    lock and re-submit.
		 *
		 * 2) The current request is cached, but not
		 *    readahead. ocfs2_buffer_uptodate() will return
		 *    false anyway, so we'll wind up waiting on the
		 *    buffer lock to do I/O. We re-check the request
		 *    with after getting the lock to avoid a re-submit.
		 *
		 * 3) The current request is readahead (and so must
		 *    also be a caching one). We short circuit if the
		 *    buffer is locked (under I/O) and if it's in the
		 *    uptodate cache. The re-check from #2 catches the
		 *    case that the previous read-ahead completes just
		 *    before our is-it-in-flight check.
		 */

		if (flags & OCFS2_BH_CACHED &&
		    !ocfs2_buffer_uptodate(inode, bh)) {
			mlog(ML_UPTODATE,
			     "bh (%llu), inode %llu not uptodate\n",
			     (unsigned long long)bh->b_blocknr,
			     (unsigned long long)OCFS2_I(inode)->ip_blkno);
			ignore_cache = 1;
		}

		/* XXX: Can we ever get this and *not* have the cached
		 * flag set? */
		if (buffer_jbd(bh)) {
			if (!(flags & OCFS2_BH_CACHED) || ignore_cache)
				mlog(ML_BH_IO, "trying to sync read a jbd "
					       "managed bh (blocknr = %llu)\n",
				     (unsigned long long)bh->b_blocknr);
			continue;
		}

		if (!(flags & OCFS2_BH_CACHED) || ignore_cache) {
			if (buffer_dirty(bh)) {
				/* This should probably be a BUG, or
				 * at least return an error. */
				mlog(ML_BH_IO, "asking me to sync read a dirty "
					       "buffer! (blocknr = %llu)\n",
				     (unsigned long long)bh->b_blocknr);
				continue;
			}

			/* A read-ahead request was made - if the
			 * buffer is already under read-ahead from a
			 * previously submitted request than we are
			 * done here. */
			if ((flags & OCFS2_BH_READAHEAD)
			    && ocfs2_buffer_read_ahead(inode, bh))
				continue;

			lock_buffer(bh);
			if (buffer_jbd(bh)) {
#ifdef CATCH_BH_JBD_RACES
				mlog(ML_ERROR, "block %llu had the JBD bit set "
					       "while I was in lock_buffer!",
				     (unsigned long long)bh->b_blocknr);
				BUG();
#else
				unlock_buffer(bh);
				continue;
#endif
			}

			/* Re-check ocfs2_buffer_uptodate() as a
			 * previously read-ahead buffer may have
			 * completed I/O while we were waiting for the
			 * buffer lock. */
			if ((flags & OCFS2_BH_CACHED)
			    && !(flags & OCFS2_BH_READAHEAD)
			    && ocfs2_buffer_uptodate(inode, bh)) {
				unlock_buffer(bh);
				continue;
			}

			clear_buffer_uptodate(bh);
			get_bh(bh); /* for end_buffer_read_sync() */
			bh->b_end_io = end_buffer_read_sync;
			submit_bh(READ, bh);
			continue;
		}
	}

	status = 0;

	for (i = (nr - 1); i >= 0; i--) {
		bh = bhs[i];

		if (!(flags & OCFS2_BH_READAHEAD)) {
			/* We know this can't have changed as we hold the
			 * inode sem. Avoid doing any work on the bh if the
			 * journal has it. */
			if (!buffer_jbd(bh))
				wait_on_buffer(bh);

			if (!buffer_uptodate(bh)) {
				/* Status won't be cleared from here on out,
				 * so we can safely record this and loop back
				 * to cleanup the other buffers. Don't need to
				 * remove the clustered uptodate information
				 * for this bh as it's not marked locally
				 * uptodate. */
				status = -EIO;
				brelse(bh);
				bhs[i] = NULL;
				continue;
			}
		}

		/* Always set the buffer in the cache, even if it was
		 * a forced read, or read-ahead which hasn't yet
		 * completed. */
		if (inode)
			ocfs2_set_buffer_uptodate(inode, bh);
	}
	if (inode)
		mutex_unlock(&OCFS2_I(inode)->ip_io_mutex);

	mlog(ML_BH_IO, "block=(%llu), nr=(%d), cached=%s, flags=0x%x\n", 
	     (unsigned long long)block, nr,
	     (!(flags & OCFS2_BH_CACHED) || ignore_cache) ? "no" : "yes", flags);

bail:

	mlog_exit(status);
	return status;
}
