/*
 * logfile.c - NTFS kernel journal handling. Part of the Linux-NTFS project.
 *
 * Copyright (c) 2002-2004 Anton Altaparmakov
 *
 * This program/include file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program/include file is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (in the main directory of the Linux-NTFS
 * distribution in the file COPYING); if not, write to the Free Software
 * Foundation,Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef NTFS_RW

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/buffer_head.h>
#include <linux/bitops.h>

#include "logfile.h"
#include "volume.h"
#include "ntfs.h"
#include "debug.h"

/**
 * ntfs_check_restart_page_header - check the page header for consistency
 * @vi:		$LogFile inode to which the restart page header belongs
 * @rp:		restart page header to check
 * @pos:	position in @vi at which the restart page header resides
 *
 * Check the restart page header @rp for consistency and return TRUE if it is
 * consistent and FALSE otherwise.
 *
 * This function only needs NTFS_BLOCK_SIZE bytes in @rp, i.e. it does not
 * require the full restart page.
 */
static BOOL ntfs_check_restart_page_header(struct inode *vi,
		RESTART_PAGE_HEADER *rp, s64 pos)
{
	u32 logfile_system_page_size, logfile_log_page_size;
	u16 usa_count, usa_ofs, usa_end, ra_ofs;

	ntfs_debug("Entering.");
	/*
	 * If the system or log page sizes are smaller than the ntfs block size
	 * or either is not a power of 2 we cannot handle this log file.
	 */
	logfile_system_page_size = le32_to_cpu(rp->system_page_size);
	logfile_log_page_size = le32_to_cpu(rp->log_page_size);
	if (logfile_system_page_size < NTFS_BLOCK_SIZE ||
			logfile_log_page_size < NTFS_BLOCK_SIZE ||
			logfile_system_page_size &
			(logfile_system_page_size - 1) ||
			logfile_log_page_size & (logfile_log_page_size - 1)) {
		ntfs_error(vi->i_sb, "$LogFile uses unsupported page size.");
		return FALSE;
	}
	/*
	 * We must be either at !pos (1st restart page) or at pos = system page
	 * size (2nd restart page).
	 */
	if (pos && pos != logfile_system_page_size) {
		ntfs_error(vi->i_sb, "Found restart area in incorrect "
				"position in $LogFile.");
		return FALSE;
	}
	/* We only know how to handle version 1.1. */
	if (sle16_to_cpu(rp->major_ver) != 1 ||
			sle16_to_cpu(rp->minor_ver) != 1) {
		ntfs_error(vi->i_sb, "$LogFile version %i.%i is not "
				"supported.  (This driver supports version "
				"1.1 only.)", (int)sle16_to_cpu(rp->major_ver),
				(int)sle16_to_cpu(rp->minor_ver));
		return FALSE;
	}
	/* Verify the size of the update sequence array. */
	usa_count = 1 + (logfile_system_page_size >> NTFS_BLOCK_SIZE_BITS);
	if (usa_count != le16_to_cpu(rp->usa_count)) {
		ntfs_error(vi->i_sb, "$LogFile restart page specifies "
				"inconsistent update sequence array count.");
		return FALSE;
	}
	/* Verify the position of the update sequence array. */
	usa_ofs = le16_to_cpu(rp->usa_ofs);
	usa_end = usa_ofs + usa_count * sizeof(u16);
	if (usa_ofs < sizeof(RESTART_PAGE_HEADER) ||
			usa_end > NTFS_BLOCK_SIZE - sizeof(u16)) {
		ntfs_error(vi->i_sb, "$LogFile restart page specifies "
				"inconsistent update sequence array offset.");
		return FALSE;
	}
	/*
	 * Verify the position of the restart area.  It must be:
	 *	- aligned to 8-byte boundary,
	 *	- after the update sequence array, and
	 *	- within the system page size.
	 */
	ra_ofs = le16_to_cpu(rp->restart_area_offset);
	if (ra_ofs & 7 || ra_ofs < usa_end ||
			ra_ofs > logfile_system_page_size) {
		ntfs_error(vi->i_sb, "$LogFile restart page specifies "
				"inconsistent restart area offset.");
		return FALSE;
	}
	/*
	 * Only restart pages modified by chkdsk are allowed to have chkdsk_lsn
	 * set.
	 */
	if (!ntfs_is_chkd_record(rp->magic) && sle64_to_cpu(rp->chkdsk_lsn)) {
		ntfs_error(vi->i_sb, "$LogFile restart page is not modified "
				"chkdsk but a chkdsk LSN is specified.");
		return FALSE;
	}
	ntfs_debug("Done.");
	return TRUE;
}

/**
 * ntfs_check_restart_area - check the restart area for consistency
 * @vi:		$LogFile inode to which the restart page belongs
 * @rp:		restart page whose restart area to check
 *
 * Check the restart area of the restart page @rp for consistency and return
 * TRUE if it is consistent and FALSE otherwise.
 *
 * This function assumes that the restart page header has already been
 * consistency checked.
 *
 * This function only needs NTFS_BLOCK_SIZE bytes in @rp, i.e. it does not
 * require the full restart page.
 */
static BOOL ntfs_check_restart_area(struct inode *vi, RESTART_PAGE_HEADER *rp)
{
	u64 file_size;
	RESTART_AREA *ra;
	u16 ra_ofs, ra_len, ca_ofs;
	u8 fs_bits;

	ntfs_debug("Entering.");
	ra_ofs = le16_to_cpu(rp->restart_area_offset);
	ra = (RESTART_AREA*)((u8*)rp + ra_ofs);
	/*
	 * Everything before ra->file_size must be before the first word
	 * protected by an update sequence number.  This ensures that it is
	 * safe to access ra->client_array_offset.
	 */
	if (ra_ofs + offsetof(RESTART_AREA, file_size) >
			NTFS_BLOCK_SIZE - sizeof(u16)) {
		ntfs_error(vi->i_sb, "$LogFile restart area specifies "
				"inconsistent file offset.");
		return FALSE;
	}
	/*
	 * Now that we can access ra->client_array_offset, make sure everything
	 * up to the log client array is before the first word protected by an
	 * update sequence number.  This ensures we can access all of the
	 * restart area elements safely.  Also, the client array offset must be
	 * aligned to an 8-byte boundary.
	 */
	ca_ofs = le16_to_cpu(ra->client_array_offset);
	if (((ca_ofs + 7) & ~7) != ca_ofs ||
			ra_ofs + ca_ofs > NTFS_BLOCK_SIZE - sizeof(u16)) {
		ntfs_error(vi->i_sb, "$LogFile restart area specifies "
				"inconsistent client array offset.");
		return FALSE;
	}
	/*
	 * The restart area must end within the system page size both when
	 * calculated manually and as specified by ra->restart_area_length.
	 * Also, the calculated length must not exceed the specified length.
	 */
	ra_len = ca_ofs + le16_to_cpu(ra->log_clients) *
			sizeof(LOG_CLIENT_RECORD);
	if (ra_ofs + ra_len > le32_to_cpu(rp->system_page_size) ||
			ra_ofs + le16_to_cpu(ra->restart_area_length) >
			le32_to_cpu(rp->system_page_size) ||
			ra_len > le16_to_cpu(ra->restart_area_length)) {
		ntfs_error(vi->i_sb, "$LogFile restart area is out of bounds "
				"of the system page size specified by the "
				"restart page header and/or the specified "
				"restart area length is inconsistent.");
		return FALSE;
	}
	/*
	 * The ra->client_free_list and ra->client_in_use_list must be either
	 * LOGFILE_NO_CLIENT or less than ra->log_clients or they are
	 * overflowing the client array.
	 */
	if ((ra->client_free_list != LOGFILE_NO_CLIENT &&
			le16_to_cpu(ra->client_free_list) >=
			le16_to_cpu(ra->log_clients)) ||
			(ra->client_in_use_list != LOGFILE_NO_CLIENT &&
			le16_to_cpu(ra->client_in_use_list) >=
			le16_to_cpu(ra->log_clients))) {
		ntfs_error(vi->i_sb, "$LogFile restart area specifies "
				"overflowing client free and/or in use lists.");
		return FALSE;
	}
	/*
	 * Check ra->seq_number_bits against ra->file_size for consistency.
	 * We cannot just use ffs() because the file size is not a power of 2.
	 */
	file_size = (u64)sle64_to_cpu(ra->file_size);
	fs_bits = 0;
	while (file_size) {
		file_size >>= 1;
		fs_bits++;
	}
	if (le32_to_cpu(ra->seq_number_bits) != 67 - fs_bits) {
		ntfs_error(vi->i_sb, "$LogFile restart area specifies "
				"inconsistent sequence number bits.");
		return FALSE;
	}
	/* The log record header length must be a multiple of 8. */
	if (((le16_to_cpu(ra->log_record_header_length) + 7) & ~7) !=
			le16_to_cpu(ra->log_record_header_length)) {
		ntfs_error(vi->i_sb, "$LogFile restart area specifies "
				"inconsistent log record header length.");
		return FALSE;
	}
	/* Dito for the log page data offset. */
	if (((le16_to_cpu(ra->log_page_data_offset) + 7) & ~7) !=
			le16_to_cpu(ra->log_page_data_offset)) {
		ntfs_error(vi->i_sb, "$LogFile restart area specifies "
				"inconsistent log page data offset.");
		return FALSE;
	}
	ntfs_debug("Done.");
	return TRUE;
}

/**
 * ntfs_check_log_client_array - check the log client array for consistency
 * @vi:		$LogFile inode to which the restart page belongs
 * @rp:		restart page whose log client array to check
 *
 * Check the log client array of the restart page @rp for consistency and
 * return TRUE if it is consistent and FALSE otherwise.
 *
 * This function assumes that the restart page header and the restart area have
 * already been consistency checked.
 *
 * Unlike ntfs_check_restart_page_header() and ntfs_check_restart_area(), this
 * function needs @rp->system_page_size bytes in @rp, i.e. it requires the full
 * restart page and the page must be multi sector transfer deprotected.
 */
static BOOL ntfs_check_log_client_array(struct inode *vi,
		RESTART_PAGE_HEADER *rp)
{
	RESTART_AREA *ra;
	LOG_CLIENT_RECORD *ca, *cr;
	u16 nr_clients, idx;
	BOOL in_free_list, idx_is_first;

	ntfs_debug("Entering.");
	ra = (RESTART_AREA*)((u8*)rp + le16_to_cpu(rp->restart_area_offset));
	ca = (LOG_CLIENT_RECORD*)((u8*)ra +
			le16_to_cpu(ra->client_array_offset));
	/*
	 * Check the ra->client_free_list first and then check the
	 * ra->client_in_use_list.  Check each of the log client records in
	 * each of the lists and check that the array does not overflow the
	 * ra->log_clients value.  Also keep track of the number of records
	 * visited as there cannot be more than ra->log_clients records and
	 * that way we detect eventual loops in within a list.
	 */
	nr_clients = le16_to_cpu(ra->log_clients);
	idx = le16_to_cpu(ra->client_free_list);
	in_free_list = TRUE;
check_list:
	for (idx_is_first = TRUE; idx != LOGFILE_NO_CLIENT_CPU; nr_clients--,
			idx = le16_to_cpu(cr->next_client)) {
		if (!nr_clients || idx >= le16_to_cpu(ra->log_clients))
			goto err_out;
		/* Set @cr to the current log client record. */
		cr = ca + idx;
		/* The first log client record must not have a prev_client. */
		if (idx_is_first) {
			if (cr->prev_client != LOGFILE_NO_CLIENT)
				goto err_out;
			idx_is_first = FALSE;
		}
	}
	/* Switch to and check the in use list if we just did the free list. */
	if (in_free_list) {
		in_free_list = FALSE;
		idx = le16_to_cpu(ra->client_in_use_list);
		goto check_list;
	}
	ntfs_debug("Done.");
	return TRUE;
err_out:
	ntfs_error(vi->i_sb, "$LogFile log client array is corrupt.");
	return FALSE;
}

/**
 * ntfs_check_and_load_restart_page - check the restart page for consistency
 * @vi:		$LogFile inode to which the restart page belongs
 * @rp:		restart page to check
 * @pos:	position in @vi at which the restart page resides
 * @wrp:	copy of the multi sector transfer deprotected restart page
 *
 * Check the restart page @rp for consistency and return TRUE if it is
 * consistent and FALSE otherwise.
 *
 * This function only needs NTFS_BLOCK_SIZE bytes in @rp, i.e. it does not
 * require the full restart page.
 *
 * If @wrp is not NULL, on success, *@wrp will point to a buffer containing a
 * copy of the complete multi sector transfer deprotected page.  On failure,
 * *@wrp is undefined.
 */
static BOOL ntfs_check_and_load_restart_page(struct inode *vi,
		RESTART_PAGE_HEADER *rp, s64 pos, RESTART_PAGE_HEADER **wrp)
{
	RESTART_AREA *ra;
	RESTART_PAGE_HEADER *trp;
	int size;
	BOOL ret;

	ntfs_debug("Entering.");
	/* Check the restart page header for consistency. */
	if (!ntfs_check_restart_page_header(vi, rp, pos)) {
		/* Error output already done inside the function. */
		return FALSE;
	}
	/* Check the restart area for consistency. */
	if (!ntfs_check_restart_area(vi, rp)) {
		/* Error output already done inside the function. */
		return FALSE;
	}
	ra = (RESTART_AREA*)((u8*)rp + le16_to_cpu(rp->restart_area_offset));
	/*
	 * Allocate a buffer to store the whole restart page so we can multi
	 * sector transfer deprotect it.
	 */
	trp = ntfs_malloc_nofs(le32_to_cpu(rp->system_page_size));
	if (!trp) {
		ntfs_error(vi->i_sb, "Failed to allocate memory for $LogFile "
				"restart page buffer.");
		return FALSE;
	}
	/*
	 * Read the whole of the restart page into the buffer.  If it fits
	 * completely inside @rp, just copy it from there.  Otherwise map all
	 * the required pages and copy the data from them.
	 */
	size = PAGE_CACHE_SIZE - (pos & ~PAGE_CACHE_MASK);
	if (size >= le32_to_cpu(rp->system_page_size)) {
		memcpy(trp, rp, le32_to_cpu(rp->system_page_size));
	} else {
		pgoff_t idx;
		struct page *page;
		int have_read, to_read;

		/* First copy what we already have in @rp. */
		memcpy(trp, rp, size);
		/* Copy the remaining data one page at a time. */
		have_read = size;
		to_read = le32_to_cpu(rp->system_page_size) - size;
		idx = (pos + size) >> PAGE_CACHE_SHIFT;
		BUG_ON((pos + size) & ~PAGE_CACHE_MASK);
		do {
			page = ntfs_map_page(vi->i_mapping, idx);
			if (IS_ERR(page)) {
				ntfs_error(vi->i_sb, "Error mapping $LogFile "
						"page (index %lu).", idx);
				goto err_out;
			}
			size = min_t(int, to_read, PAGE_CACHE_SIZE);
			memcpy((u8*)trp + have_read, page_address(page), size);
			ntfs_unmap_page(page);
			have_read += size;
			to_read -= size;
			idx++;
		} while (to_read > 0);
	}
	/* Perform the multi sector transfer deprotection on the buffer. */
	if (post_read_mst_fixup((NTFS_RECORD*)trp,
			le32_to_cpu(rp->system_page_size))) {
		ntfs_error(vi->i_sb, "Multi sector transfer error detected in "
				"$LogFile restart page.");
		goto err_out;
	}
	/* Check the log client records for consistency. */
	ret = ntfs_check_log_client_array(vi, trp);
	if (ret && wrp)
		*wrp = trp;
	else
		ntfs_free(trp);
	ntfs_debug("Done.");
	return ret;
err_out:
	ntfs_free(trp);
	return FALSE;
}

/**
 * ntfs_ckeck_logfile - check in the journal if the volume is consistent
 * @log_vi:	struct inode of loaded journal $LogFile to check
 *
 * Check the $LogFile journal for consistency and return TRUE if it is
 * consistent and FALSE if not.
 *
 * At present we only check the two restart pages and ignore the log record
 * pages.
 *
 * Note that the MstProtected flag is not set on the $LogFile inode and hence
 * when reading pages they are not deprotected.  This is because we do not know
 * if the $LogFile was created on a system with a different page size to ours
 * yet and mst deprotection would fail if our page size is smaller.
 */
BOOL ntfs_check_logfile(struct inode *log_vi)
{
	s64 size, pos, rstr1_pos, rstr2_pos;
	ntfs_volume *vol = NTFS_SB(log_vi->i_sb);
	struct address_space *mapping = log_vi->i_mapping;
	struct page *page = NULL;
	u8 *kaddr = NULL;
	RESTART_PAGE_HEADER *rstr1_ph = NULL;
	RESTART_PAGE_HEADER *rstr2_ph = NULL;
	int log_page_size, log_page_mask, ofs;
	BOOL logfile_is_empty = TRUE;
	BOOL rstr1_found = FALSE;
	BOOL rstr2_found = FALSE;
	u8 log_page_bits;

	ntfs_debug("Entering.");
	/* An empty $LogFile must have been clean before it got emptied. */
	if (NVolLogFileEmpty(vol))
		goto is_empty;
	size = log_vi->i_size;
	/* Make sure the file doesn't exceed the maximum allowed size. */
	if (size > MaxLogFileSize)
		size = MaxLogFileSize;
	/*
	 * Truncate size to a multiple of the page cache size or the default
	 * log page size if the page cache size is between the default log page
	 * log page size if the page cache size is between the default log page
	 * size and twice that.
	 */
	if (PAGE_CACHE_SIZE >= DefaultLogPageSize && PAGE_CACHE_SIZE <=
			DefaultLogPageSize * 2)
		log_page_size = DefaultLogPageSize;
	else
		log_page_size = PAGE_CACHE_SIZE;
	log_page_mask = log_page_size - 1;
	/*
	 * Use generic_ffs() instead of ffs() to enable the compiler to
	 * optimize log_page_size and log_page_bits into constants.
	 */
	log_page_bits = generic_ffs(log_page_size) - 1;
	size &= ~(log_page_size - 1);
	/*
	 * Ensure the log file is big enough to store at least the two restart
	 * pages and the minimum number of log record pages.
	 */
	if (size < log_page_size * 2 || (size - log_page_size * 2) >>
			log_page_bits < MinLogRecordPages) {
		ntfs_error(vol->sb, "$LogFile is too small.");
		return FALSE;
	}
	/*
	 * Read through the file looking for a restart page.  Since the restart
	 * page header is at the beginning of a page we only need to search at
	 * what could be the beginning of a page (for each page size) rather
	 * than scanning the whole file byte by byte.  If all potential places
	 * contain empty and uninitialzed records, the log file can be assumed
	 * to be empty.
	 */
	for (pos = 0; pos < size; pos <<= 1) {
		pgoff_t idx = pos >> PAGE_CACHE_SHIFT;
		if (!page || page->index != idx) {
			if (page)
				ntfs_unmap_page(page);
			page = ntfs_map_page(mapping, idx);
			if (IS_ERR(page)) {
				ntfs_error(vol->sb, "Error mapping $LogFile "
						"page (index %lu).", idx);
				return FALSE;
			}
		}
		kaddr = (u8*)page_address(page) + (pos & ~PAGE_CACHE_MASK);
		/*
		 * A non-empty block means the logfile is not empty while an
		 * empty block after a non-empty block has been encountered
		 * means we are done.
		 */
		if (!ntfs_is_empty_recordp(kaddr))
			logfile_is_empty = FALSE;
		else if (!logfile_is_empty)
			break;
		/*
		 * A log record page means there cannot be a restart page after
		 * this so no need to continue searching.
		 */
		if (ntfs_is_rcrd_recordp(kaddr))
			break;
		/*
		 * A modified by chkdsk restart page means we cannot handle
		 * this log file.
		 */
		if (ntfs_is_chkd_recordp(kaddr)) {
			ntfs_error(vol->sb, "$LogFile has been modified by "
					"chkdsk.  Mount this volume in "
					"Windows.");
			goto err_out;
		}
		/* If not a restart page, continue. */
		if (!ntfs_is_rstr_recordp(kaddr)) {
			/* Skip to the minimum page size for the next one. */
			if (!pos)
				pos = NTFS_BLOCK_SIZE >> 1;
			continue;
		}
		/* We now know we have a restart page. */
		if (!pos) {
			rstr1_found = TRUE;
			rstr1_pos = pos;
		} else {
			if (rstr2_found) {
				ntfs_error(vol->sb, "Found more than two "
						"restart pages in $LogFile.");
				goto err_out;
			}
			rstr2_found = TRUE;
			rstr2_pos = pos;
		}
		/*
		 * Check the restart page for consistency and get a copy of the
		 * complete multi sector transfer deprotected restart page.
		 */
		if (!ntfs_check_and_load_restart_page(log_vi,
				(RESTART_PAGE_HEADER*)kaddr, pos,
				!pos ? &rstr1_ph : &rstr2_ph)) {
			/* Error output already done inside the function. */
			goto err_out;
		}
		/*
		 * We have a valid restart page.  The next one must be after
		 * a whole system page size as specified by the valid restart
		 * page.
		 */
		if (!pos)
			pos = le32_to_cpu(rstr1_ph->system_page_size) >> 1;
	}
	if (page) {
		ntfs_unmap_page(page);
		page = NULL;
	}
	if (logfile_is_empty) {
		NVolSetLogFileEmpty(vol);
is_empty:
		ntfs_debug("Done.  ($LogFile is empty.)");
		return TRUE;
	}
	if (!rstr1_found || !rstr2_found) {
		ntfs_error(vol->sb, "Did not find two restart pages in "
				"$LogFile.");
		goto err_out;
	}
	/*
	 * The two restart areas must be identical except for the update
	 * sequence number.
	 */
	ofs = le16_to_cpu(rstr1_ph->usa_ofs);
	if (memcmp(rstr1_ph, rstr2_ph, ofs) || (ofs += sizeof(u16),
			memcmp((u8*)rstr1_ph + ofs, (u8*)rstr2_ph + ofs,
			le32_to_cpu(rstr1_ph->system_page_size) - ofs))) {
		ntfs_error(vol->sb, "The two restart pages in $LogFile do not "
				"match.");
		goto err_out;
	}
	ntfs_free(rstr1_ph);
	ntfs_free(rstr2_ph);
	/* All consistency checks passed. */
	ntfs_debug("Done.");
	return TRUE;
err_out:
	if (page)
		ntfs_unmap_page(page);
	if (rstr1_ph)
		ntfs_free(rstr1_ph);
	if (rstr2_ph)
		ntfs_free(rstr2_ph);
	return FALSE;
}

/**
 * ntfs_is_logfile_clean - check in the journal if the volume is clean
 * @log_vi:	struct inode of loaded journal $LogFile to check
 *
 * Analyze the $LogFile journal and return TRUE if it indicates the volume was
 * shutdown cleanly and FALSE if not.
 *
 * At present we only look at the two restart pages and ignore the log record
 * pages.  This is a little bit crude in that there will be a very small number
 * of cases where we think that a volume is dirty when in fact it is clean.
 * This should only affect volumes that have not been shutdown cleanly but did
 * not have any pending, non-check-pointed i/o, i.e. they were completely idle
 * at least for the five seconds preceeding the unclean shutdown.
 *
 * This function assumes that the $LogFile journal has already been consistency
 * checked by a call to ntfs_check_logfile() and in particular if the $LogFile
 * is empty this function requires that NVolLogFileEmpty() is true otherwise an
 * empty volume will be reported as dirty.
 */
BOOL ntfs_is_logfile_clean(struct inode *log_vi)
{
	ntfs_volume *vol = NTFS_SB(log_vi->i_sb);
	struct page *page;
	RESTART_PAGE_HEADER *rp;
	RESTART_AREA *ra;

	ntfs_debug("Entering.");
	/* An empty $LogFile must have been clean before it got emptied. */
	if (NVolLogFileEmpty(vol)) {
		ntfs_debug("Done.  ($LogFile is empty.)");
		return TRUE;
	}
	/*
	 * Read the first restart page.  It will be possibly incomplete and
	 * will not be multi sector transfer deprotected but we only need the
	 * first NTFS_BLOCK_SIZE bytes so it does not matter.
	 */
	page = ntfs_map_page(log_vi->i_mapping, 0);
	if (IS_ERR(page)) {
		ntfs_error(vol->sb, "Error mapping $LogFile page (index 0).");
		return FALSE;
	}
	rp = (RESTART_PAGE_HEADER*)page_address(page);
	if (!ntfs_is_rstr_record(rp->magic)) {
		ntfs_error(vol->sb, "No restart page found at offset zero in "
				"$LogFile.  This is probably a bug in that "
				"the $LogFile should have been consistency "
				"checked before calling this function.");
		goto err_out;
	}
	ra = (RESTART_AREA*)((u8*)rp + le16_to_cpu(rp->restart_area_offset));
	/*
	 * If the $LogFile has active clients, i.e. it is open, and we do not
	 * have the RESTART_VOLUME_IS_CLEAN bit set in the restart area flags,
	 * we assume there was an unclean shutdown.
	 */
	if (ra->client_in_use_list != LOGFILE_NO_CLIENT &&
			!(ra->flags & RESTART_VOLUME_IS_CLEAN)) {
		ntfs_debug("Done.  $LogFile indicates a dirty shutdown.");
		goto err_out;
	}
	ntfs_unmap_page(page);
	/* $LogFile indicates a clean shutdown. */
	ntfs_debug("Done.  $LogFile indicates a clean shutdown.");
	return TRUE;
err_out:
	ntfs_unmap_page(page);
	return FALSE;
}

/**
 * ntfs_empty_logfile - empty the contents of the $LogFile journal
 * @log_vi:	struct inode of loaded journal $LogFile to empty
 *
 * Empty the contents of the $LogFile journal @log_vi and return TRUE on
 * success and FALSE on error.
 *
 * This function assumes that the $LogFile journal has already been consistency
 * checked by a call to ntfs_check_logfile() and that ntfs_is_logfile_clean()
 * has been used to ensure that the $LogFile is clean.
 */
BOOL ntfs_empty_logfile(struct inode *log_vi)
{
	ntfs_volume *vol = NTFS_SB(log_vi->i_sb);
	struct address_space *mapping;
	pgoff_t idx, end;

	ntfs_debug("Entering.");
	if (NVolLogFileEmpty(vol))
		goto done;
	mapping = log_vi->i_mapping;
	end = (log_vi->i_size + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	for (idx = 0; idx < end; ++idx) {
		struct page *page;
		u8 *kaddr;

		/* Find or create the current page.  (The page is locked.) */
		page = grab_cache_page(mapping, idx);
		if (unlikely(!page)) {
			ntfs_error(vol->sb, "Insufficient memory to grab "
					"$LogFile page (index %lu).", idx);
			return FALSE;
		}
		/*
		 * Set all bytes in the page to 0xff.  It doesn't matter if we
		 * go beyond i_size, because ntfs_writepage() will take care of
		 * that for us.
		 */
		kaddr = (u8*)kmap_atomic(page, KM_USER0);
		memset(kaddr, 0xff, PAGE_CACHE_SIZE);
		flush_dcache_page(page);
		kunmap_atomic(kaddr, KM_USER0);
		/*
		 * If the page has buffers, mark them uptodate since buffer
		 * state and not page state is definitive in 2.6 kernels.
		 */
		if (page_has_buffers(page)) {
			struct buffer_head *bh, *head;

			bh = head = page_buffers(page);
			do {
				set_buffer_uptodate(bh);
			} while ((bh = bh->b_this_page) != head);
		}
		/* Now that buffers are uptodate, set the page uptodate, too. */
		SetPageUptodate(page);
		/*
		 * Set the page and all its buffers dirty and mark the inode
		 * dirty, too. The VM will write the page later on.
		 */
		set_page_dirty(page);
		/* Finally unlock and release the page. */
		unlock_page(page);
		page_cache_release(page);
	}
	/* We set the flag so we do not clear the log file again on remount. */
	NVolSetLogFileEmpty(vol);
done:
	ntfs_debug("Done.");
	return TRUE;
}

#endif /* NTFS_RW */
