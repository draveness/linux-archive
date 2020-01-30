/*
 * attrib.h - Defines for attribute handling in NTFS Linux kernel driver.
 *	      Part of the Linux-NTFS project.
 *
 * Copyright (c) 2001-2004 Anton Altaparmakov
 * Copyright (c) 2002 Richard Russon
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

#ifndef _LINUX_NTFS_ATTRIB_H
#define _LINUX_NTFS_ATTRIB_H

#include <linux/fs.h>

#include "endian.h"
#include "types.h"
#include "layout.h"

static inline void init_run_list(run_list *rl)
{
	rl->rl = NULL;
	init_rwsem(&rl->lock);
}

typedef enum {
	LCN_HOLE		= -1,	/* Keep this as highest value or die! */
	LCN_RL_NOT_MAPPED	= -2,
	LCN_ENOENT		= -3,
	LCN_EINVAL		= -4,
} LCN_SPECIAL_VALUES;

/**
 * attr_search_context - used in attribute search functions
 * @mrec:	buffer containing mft record to search
 * @attr:	attribute record in @mrec where to begin/continue search
 * @is_first:	if true lookup_attr() begins search with @attr, else after @attr
 *
 * Structure must be initialized to zero before the first call to one of the
 * attribute search functions. Initialize @mrec to point to the mft record to
 * search, and @attr to point to the first attribute within @mrec (not necessary
 * if calling the _first() functions), and set @is_first to TRUE (not necessary
 * if calling the _first() functions).
 *
 * If @is_first is TRUE, the search begins with @attr. If @is_first is FALSE,
 * the search begins after @attr. This is so that, after the first call to one
 * of the search attribute functions, we can call the function again, without
 * any modification of the search context, to automagically get the next
 * matching attribute.
 */
typedef struct {
	MFT_RECORD *mrec;
	ATTR_RECORD *attr;
	BOOL is_first;
	ntfs_inode *ntfs_ino;
	ATTR_LIST_ENTRY *al_entry;
	ntfs_inode *base_ntfs_ino;
	MFT_RECORD *base_mrec;
	ATTR_RECORD *base_attr;
} attr_search_context;

extern run_list_element *decompress_mapping_pairs(const ntfs_volume *vol,
		const ATTR_RECORD *attr, run_list_element *old_rl);

extern int map_run_list(ntfs_inode *ni, VCN vcn);

extern LCN vcn_to_lcn(const run_list_element *rl, const VCN vcn);

extern BOOL find_attr(const ATTR_TYPES type, const ntfschar *name,
		const u32 name_len, const IGNORE_CASE_BOOL ic, const u8 *val,
		const u32 val_len, attr_search_context *ctx);

BOOL lookup_attr(const ATTR_TYPES type, const ntfschar *name,
		const u32 name_len, const IGNORE_CASE_BOOL ic,
		const VCN lowest_vcn, const u8 *val, const u32 val_len,
		attr_search_context *ctx);

extern int load_attribute_list(ntfs_volume *vol, run_list *rl, u8 *al_start,
		const s64 size, const s64 initialized_size);

static inline s64 attribute_value_length(const ATTR_RECORD *a)
{
	if (!a->non_resident)
		return (s64)le32_to_cpu(a->data.resident.value_length);
	return sle64_to_cpu(a->data.non_resident.data_size);
}

extern void reinit_attr_search_ctx(attr_search_context *ctx);
extern attr_search_context *get_attr_search_ctx(ntfs_inode *ni,
		MFT_RECORD *mrec);
extern void put_attr_search_ctx(attr_search_context *ctx);

#endif /* _LINUX_NTFS_ATTRIB_H */
