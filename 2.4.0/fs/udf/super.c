/*
 * super.c
 *
 * PURPOSE
 *  Super block routines for the OSTA-UDF(tm) filesystem.
 *
 * DESCRIPTION
 *  OSTA-UDF(tm) = Optical Storage Technology Association
 *  Universal Disk Format.
 *
 *  This code is based on version 2.00 of the UDF specification,
 *  and revision 3 of the ECMA 167 standard [equivalent to ISO 13346].
 *    http://www.osta.org/
 *    http://www.ecma.ch/
 *    http://www.iso.org/
 *
 * CONTACTS
 *  E-mail regarding any portion of the Linux UDF file system should be
 *  directed to the development team mailing list (run by majordomo):
 *	  linux_udf@hootie.lvld.hp.com
 *
 * COPYRIGHT
 *  This file is distributed under the terms of the GNU General Public
 *  License (GPL). Copies of the GPL can be obtained from:
 *    ftp://prep.ai.mit.edu/pub/gnu/GPL
 *  Each contributing author retains all rights to their own work.
 *
 *  (C) 1998 Dave Boynton
 *  (C) 1998-2000 Ben Fennema
 *  (C) 2000 Stelias Computing Inc
 *
 * HISTORY
 *
 *  09/24/98 dgb  changed to allow compiling outside of kernel, and
 *                added some debugging.
 *  10/01/98 dgb  updated to allow (some) possibility of compiling w/2.0.34
 *  10/16/98      attempting some multi-session support
 *  10/17/98      added freespace count for "df"
 *  11/11/98 gr   added novrs option
 *  11/26/98 dgb  added fileset,anchor mount options
 *  12/06/98 blf  really hosed things royally. vat/sparing support. sequenced vol descs
 *                rewrote option handling based on isofs
 *  12/20/98      find the free space bitmap (if it exists)
 */

#include "udfdecl.h"    

#include <linux/config.h>
#include <linux/version.h>
#include <linux/blkdev.h>
#include <linux/malloc.h>
#include <linux/kernel.h>
#include <linux/locks.h>
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/cdrom.h>
#include <linux/nls.h>
#include <asm/byteorder.h>

#include <linux/udf_fs.h>
#include "udf_sb.h"
#include "udf_i.h"

#include <linux/init.h>
#include <asm/uaccess.h>

#define VDS_POS_PRIMARY_VOL_DESC	0
#define VDS_POS_UNALLOC_SPACE_DESC	1
#define VDS_POS_LOGICAL_VOL_DESC	2
#define VDS_POS_PARTITION_DESC		3
#define VDS_POS_IMP_USE_VOL_DESC	4
#define VDS_POS_VOL_DESC_PTR		5
#define VDS_POS_TERMINATING_DESC	6
#define VDS_POS_LENGTH				7

static char error_buf[1024];

/* These are the "meat" - everything else is stuffing */
static struct super_block *udf_read_super(struct super_block *, void *, int);
static void udf_put_super(struct super_block *);
static void udf_write_super(struct super_block *);
static int udf_remount_fs(struct super_block *, int *, char *);
static int udf_check_valid(struct super_block *, int, int);
static int udf_vrs(struct super_block *sb, int silent);
static int udf_load_partition(struct super_block *, lb_addr *);
static int udf_load_logicalvol(struct super_block *, struct buffer_head *, lb_addr *);
static void udf_load_logicalvolint(struct super_block *, extent_ad);
static int udf_find_anchor(struct super_block *, int, int);
static int udf_find_fileset(struct super_block *, lb_addr *, lb_addr *);
static void udf_load_pvoldesc(struct super_block *, struct buffer_head *);
static void udf_load_fileset(struct super_block *, struct buffer_head *, lb_addr *);
static void udf_load_partdesc(struct super_block *, struct buffer_head *);
static void udf_open_lvid(struct super_block *);
static void udf_close_lvid(struct super_block *);
static unsigned int udf_count_free(struct super_block *);
static int udf_statfs(struct super_block *, struct statfs *);

/* UDF filesystem type */
static DECLARE_FSTYPE_DEV(udf_fstype, "udf", udf_read_super);

/* Superblock operations */
static struct super_operations udf_sb_ops = {
	read_inode:		udf_read_inode,
	write_inode:		udf_write_inode,
	put_inode:		udf_put_inode,
	delete_inode:		udf_delete_inode,
	put_super:		udf_put_super,
	write_super:		udf_write_super,
	statfs:			udf_statfs,
	remount_fs:		udf_remount_fs,
};

struct udf_options
{
	unsigned char novrs;
	unsigned int blocksize;
	unsigned int session;
	unsigned int lastblock;
	unsigned int anchor;
	unsigned int volume;
	unsigned short partition;
	unsigned int fileset;
	unsigned int rootdir;
	unsigned int flags;
	mode_t umask;
	gid_t gid;
	uid_t uid;
};

static int __init init_udf_fs(void)
{
	printk(KERN_NOTICE "udf: registering filesystem\n");
	return register_filesystem(&udf_fstype);
}

static void __exit exit_udf_fs(void)
{
	printk(KERN_NOTICE "udf: unregistering filesystem\n");
	unregister_filesystem(&udf_fstype);
}

module_init(init_udf_fs)
module_exit(exit_udf_fs)

/*
 * udf_parse_options
 *
 * PURPOSE
 *	Parse mount options.
 *
 * DESCRIPTION
 *	The following mount options are supported:
 *
 *	gid=		Set the default group.
 *	umask=		Set the default umask.
 *	uid=		Set the default user.
 *	bs=			Set the block size.
 *	unhide		Show otherwise hidden files.
 *	undelete	Show deleted files in lists.
 *	adinicb		Embed data in the inode (default)
 *	noadinicb	Don't embed data in the inode
 *	shortad		Use short ad's
 *	longad		Use long ad's (default)
 *	strict		Set strict conformance (unused)
 *
 *	The remaining are for debugging and disaster recovery:
 *
 *	novrs		Skip volume sequence recognition 
 *
 *	The following expect a offset from 0.
 *
 *	session=	Set the CDROM session (default= last session)
 *	anchor=		Override standard anchor location. (default= 256)
 *	volume=		Override the VolumeDesc location. (unused)
 *	partition=	Override the PartitionDesc location. (unused)
 *	lastblock=	Set the last block of the filesystem/
 *
 *	The following expect a offset from the partition root.
 *
 *	fileset=	Override the fileset block location. (unused)
 *	rootdir=	Override the root directory location. (unused)
 *		WARNING: overriding the rootdir to a non-directory may
 *		yield highly unpredictable results.
 *
 * PRE-CONDITIONS
 *	options		Pointer to mount options string.
 *	uopts		Pointer to mount options variable.
 *
 * POST-CONDITIONS
 *	<return>	0	Mount options parsed okay.
 *	<return>	-1	Error parsing mount options.
 *
 * HISTORY
 *	July 1, 1997 - Andrew E. Mileski
 *	Written, tested, and released.
 */

static int
udf_parse_options(char *options, struct udf_options *uopt)
{
	char *opt, *val;

	uopt->novrs = 0;
	uopt->blocksize = 512;
	uopt->partition = 0xFFFF;
	uopt->session = 0xFFFFFFFF;
	uopt->lastblock = 0xFFFFFFFF;
	uopt->anchor = 0xFFFFFFFF;
	uopt->volume = 0xFFFFFFFF;
	uopt->rootdir = 0xFFFFFFFF;
	uopt->fileset = 0xFFFFFFFF;

	if (!options)
		return 1;

	for (opt = strtok(options, ","); opt; opt = strtok(NULL, ","))
	{
		/* Make "opt=val" into two strings */
		val = strchr(opt, '=');
		if (val)
			*(val++) = 0;
		if (!strcmp(opt, "novrs") && !val)
			uopt->novrs = 1;
		else if (!strcmp(opt, "bs") && val)
			uopt->blocksize = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "unhide") && !val)
			uopt->flags |= (1 << UDF_FLAG_UNHIDE);
		else if (!strcmp(opt, "undelete") && !val)
			uopt->flags |= (1 << UDF_FLAG_UNDELETE);
		else if (!strcmp(opt, "noadinicb") && !val)
			uopt->flags &= ~(1 << UDF_FLAG_USE_AD_IN_ICB);
		else if (!strcmp(opt, "adinicb") && !val)
			uopt->flags |= (1 << UDF_FLAG_USE_AD_IN_ICB);
		else if (!strcmp(opt, "shortad") && !val)
			uopt->flags |= (1 << UDF_FLAG_USE_SHORT_AD);
		else if (!strcmp(opt, "longad") && !val)
			uopt->flags &= ~(1 << UDF_FLAG_USE_SHORT_AD);
		else if (!strcmp(opt, "gid") && val)
			uopt->gid = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "umask") && val)
			uopt->umask = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "strict") && !val)
			uopt->flags |= (1 << UDF_FLAG_STRICT);
		else if (!strcmp(opt, "uid") && val)
			uopt->uid = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "session") && val)
			uopt->session = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "lastblock") && val)
			uopt->lastblock = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "anchor") && val)
			uopt->anchor = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "volume") && val)
			uopt->volume = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "partition") && val)
			uopt->partition = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "fileset") && val)
			uopt->fileset = simple_strtoul(val, NULL, 0);
		else if (!strcmp(opt, "rootdir") && val)
			uopt->rootdir = simple_strtoul(val, NULL, 0);
		else if (val)
		{
			printk(KERN_ERR "udf: bad mount option \"%s=%s\"\n",
				opt, val);
			return 0;
		}
		else
		{
			printk(KERN_ERR "udf: bad mount option \"%s\"\n",
				opt);
			return 0;
		}
	}
	return 1;
}

void
udf_write_super(struct super_block *sb)
{
	if (!(sb->s_flags & MS_RDONLY))
		udf_open_lvid(sb);
	sb->s_dirt = 0;
}
		
static int
udf_remount_fs(struct super_block *sb, int *flags, char *options)
{
	struct udf_options uopt;

	uopt.flags = UDF_SB(sb)->s_flags ;
	uopt.uid   = UDF_SB(sb)->s_uid ;
	uopt.gid   = UDF_SB(sb)->s_gid ;
	uopt.umask = UDF_SB(sb)->s_umask ;

	if ( !udf_parse_options(options, &uopt) )
		return -EINVAL;

	UDF_SB(sb)->s_flags = uopt.flags;
	UDF_SB(sb)->s_uid   = uopt.uid;
	UDF_SB(sb)->s_gid   = uopt.gid;
	UDF_SB(sb)->s_umask = uopt.umask;

#if CONFIG_UDF_RW != 1
	*flags |= MS_RDONLY;
#endif

	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;
	if (*flags & MS_RDONLY)
		udf_close_lvid(sb);
	else
		udf_open_lvid(sb);

	return 0;
}

/*
 * udf_set_blocksize
 *
 * PURPOSE
 *	Set the block size to be used in all transfers.
 *
 * DESCRIPTION
 *	To allow room for a DMA transfer, it is best to guess big when unsure.
 *	This routine picks 2048 bytes as the blocksize when guessing. This
 *	should be adequate until devices with larger block sizes become common.
 *
 *	Note that the Linux kernel can currently only deal with blocksizes of
 *	512, 1024, 2048, 4096, and 8192 bytes.
 *
 * PRE-CONDITIONS
 *	sb			Pointer to _locked_ superblock.
 *
 * POST-CONDITIONS
 *	sb->s_blocksize		Blocksize.
 *	sb->s_blocksize_bits	log2 of blocksize.
 *	<return>	0	Blocksize is valid.
 *	<return>	1	Blocksize is invalid.
 *
 * HISTORY
 *	July 1, 1997 - Andrew E. Mileski
 *	Written, tested, and released.
 */
static  int
udf_set_blocksize(struct super_block *sb, int bsize)
{
	/* Use specified block size if specified */
	if (!(sb->s_blocksize = get_hardblocksize(sb->s_dev)))
		sb->s_blocksize = 2048;
	if (bsize > sb->s_blocksize)
		sb->s_blocksize = bsize;

	/* Block size must be an even multiple of 512 */
	switch (sb->s_blocksize) {
		case 512: sb->s_blocksize_bits = 9;	break;
		case 1024: sb->s_blocksize_bits = 10; break;
		case 2048: sb->s_blocksize_bits = 11; break;
		case 4096: sb->s_blocksize_bits = 12; break;
		case 8192: sb->s_blocksize_bits = 13; break;
		default:
		{
			udf_debug("Bad block size (%ld)\n", sb->s_blocksize);
			printk(KERN_ERR "udf: bad block size (%ld)\n", sb->s_blocksize);
			return 0;
		}
	}

	/* Set the block size */
	set_blocksize(sb->s_dev, sb->s_blocksize);
	return sb->s_blocksize;
}

static int
udf_vrs(struct super_block *sb, int silent)
{
	struct VolStructDesc *vsd = NULL;
	int sector = 32768;
	struct buffer_head *bh = NULL;
	int iso9660=0;
	int nsr02=0;
	int nsr03=0;

	/* Block size must be a multiple of 512 */
	if (sb->s_blocksize & 511)
		return sector;

	sector += (UDF_SB_SESSION(sb) << sb->s_blocksize_bits);

	udf_debug("Starting at sector %u (%ld byte sectors)\n",
		(sector >> sb->s_blocksize_bits), sb->s_blocksize);
	/* Process the sequence (if applicable) */
	for (;!nsr02 && !nsr03; sector += 2048)
	{
		/* Read a block */
		bh = udf_tread(sb, sector >> sb->s_blocksize_bits, sb->s_blocksize);
		if (!bh)
			break;

		/* Look for ISO  descriptors */
		vsd = (struct VolStructDesc *)(bh->b_data +
			(sector & (sb->s_blocksize - 1)));

		if (vsd->stdIdent[0] == 0)
		{
			udf_release_data(bh);
			break;
		}
		else if (!strncmp(vsd->stdIdent, STD_ID_CD001, STD_ID_LEN))
		{
			iso9660 = sector;
			switch (vsd->structType)
			{
				case 0: 
					udf_debug("ISO9660 Boot Record found\n");
					break;
				case 1: 
					udf_debug("ISO9660 Primary Volume Descriptor found\n");
					break;
				case 2: 
					udf_debug("ISO9660 Supplementary Volume Descriptor found\n");
					break;
				case 3: 
					udf_debug("ISO9660 Volume Partition Descriptor found\n");
					break;
				case 255: 
					udf_debug("ISO9660 Volume Descriptor Set Terminator found\n");
					break;
				default: 
					udf_debug("ISO9660 VRS (%u) found\n", vsd->structType);
					break;
			}
		}
		else if (!strncmp(vsd->stdIdent, STD_ID_BEA01, STD_ID_LEN))
		{
		}
		else if (!strncmp(vsd->stdIdent, STD_ID_TEA01, STD_ID_LEN))
		{
			udf_release_data(bh);
			break;
		}
		else if (!strncmp(vsd->stdIdent, STD_ID_NSR02, STD_ID_LEN))
		{
			nsr02 = sector;
		}
		else if (!strncmp(vsd->stdIdent, STD_ID_NSR03, STD_ID_LEN))
		{
			nsr03 = sector;
		}
		udf_release_data(bh);
	}

	if (nsr03)
		return nsr03;
	else if (nsr02)
		return nsr02;
	else if (sector - (UDF_SB_SESSION(sb) << sb->s_blocksize_bits) == 32768)
		return -1;
	else
		return 0;
}

/*
 * udf_find_anchor
 *
 * PURPOSE
 *	Find an anchor volume descriptor.
 *
 * PRE-CONDITIONS
 *	sb			Pointer to _locked_ superblock.
 *	lastblock		Last block on media.
 *
 * POST-CONDITIONS
 *	<return>		1 if not found, 0 if ok
 *
 * HISTORY
 *	July 1, 1997 - Andrew E. Mileski
 *	Written, tested, and released.
 */
static int
udf_find_anchor(struct super_block *sb, int useranchor, int lastblock)
{
	int varlastblock = udf_variable_to_fixed(lastblock);
	int last[] =  { lastblock, lastblock - 2,
					lastblock - 150, lastblock - 152,
					varlastblock, varlastblock - 2,
					varlastblock - 150, varlastblock - 152 };
	struct buffer_head *bh = NULL;
	Uint16 ident;
	Uint32 location;
	int i;

	UDF_SB_ANCHOR(sb)[0] = 0;
	UDF_SB_ANCHOR(sb)[1] = 0;
	UDF_SB_ANCHOR(sb)[2] = 0;
	UDF_SB_ANCHOR(sb)[3] = 256 + UDF_SB_SESSION(sb);

	lastblock = 0;

	/* Search for an anchor volume descriptor pointer */

	/*  according to spec, anchor is in either:
	 *     block 256
	 *     lastblock-256
	 *     lastblock
	 *  however, if the disc isn't closed, it could be 512 */

	for (i=0; (!lastblock && i<sizeof(last)/sizeof(int)); i++)
	{
		if (!(bh = bread(sb->s_dev, last[i], sb->s_blocksize)))
		{
			ident = location = 0;
		}
		else
		{
			ident = le16_to_cpu(((tag *)bh->b_data)->tagIdent);
			location = le32_to_cpu(((tag *)bh->b_data)->tagLocation);
			udf_release_data(bh);
		}

		if (ident == TID_ANCHOR_VOL_DESC_PTR)
		{
			if (location == last[i] - UDF_SB_SESSION(sb))
			{
				lastblock = UDF_SB_ANCHOR(sb)[0] = last[i];
				UDF_SB_ANCHOR(sb)[1] = last[i] - 256;
			}
			else if (location == udf_variable_to_fixed(last[i]) - UDF_SB_SESSION(sb))
			{
				UDF_SET_FLAG(sb, UDF_FLAG_VARCONV);
				lastblock = UDF_SB_ANCHOR(sb)[0] = udf_variable_to_fixed(last[i]);
				UDF_SB_ANCHOR(sb)[1] = lastblock - 256;
			}
			else
				udf_debug("Anchor found at block %d, location mismatch %d.\n",
					last[i], location);
		}
		else if (ident == TID_FILE_ENTRY || ident == TID_EXTENDED_FILE_ENTRY)
		{
			lastblock = last[i];
			UDF_SB_ANCHOR(sb)[2] = 512 + UDF_SB_SESSION(sb);
		}
		else
		{
			if (!(bh = bread(sb->s_dev, last[i] - 256, sb->s_blocksize)))
			{
				ident = location = 0;
			}
			else
			{
				ident = le16_to_cpu(((tag *)bh->b_data)->tagIdent);
				location = le32_to_cpu(((tag *)bh->b_data)->tagLocation);
				udf_release_data(bh);
			}

			if (ident == TID_ANCHOR_VOL_DESC_PTR &&
				location == last[i] - 256 - UDF_SB_SESSION(sb))
			{
				lastblock = last[i];
				UDF_SB_ANCHOR(sb)[1] = last[i] - 256;
			}
			else
			{
				if (!(bh = bread(sb->s_dev, last[i] - 312 - UDF_SB_SESSION(sb),
					sb->s_blocksize)))
				{
					ident = location = 0;
				}
				else
				{
					ident = le16_to_cpu(((tag *)bh->b_data)->tagIdent);
					location = le32_to_cpu(((tag *)bh->b_data)->tagLocation);
					udf_release_data(bh);
				}

				if (ident == TID_ANCHOR_VOL_DESC_PTR &&
					location == udf_variable_to_fixed(last[i]) - 256)
				{
					UDF_SET_FLAG(sb, UDF_FLAG_VARCONV);
					lastblock = udf_variable_to_fixed(last[i]);
					UDF_SB_ANCHOR(sb)[1] = lastblock - 256;
				}
			}
		}
	}

	if (!lastblock)
	{
		/* We havn't found the lastblock. check 312 */
		if ((bh = bread(sb->s_dev, 312 + UDF_SB_SESSION(sb), sb->s_blocksize)))
		{
			ident = le16_to_cpu(((tag *)bh->b_data)->tagIdent);
			location = le32_to_cpu(((tag *)bh->b_data)->tagLocation);
			udf_release_data(bh);

			if (ident == TID_ANCHOR_VOL_DESC_PTR && location == 256)
				UDF_SET_FLAG(sb, UDF_FLAG_VARCONV);
		}
	}

	for (i=0; i<sizeof(UDF_SB_ANCHOR(sb))/sizeof(int); i++)
	{
		if (UDF_SB_ANCHOR(sb)[i])
		{
			if (!(bh = udf_read_tagged(sb,
				UDF_SB_ANCHOR(sb)[i], UDF_SB_ANCHOR(sb)[i], &ident)))
			{
				UDF_SB_ANCHOR(sb)[i] = 0;
			}
			else
			{
				udf_release_data(bh);
				if ((ident != TID_ANCHOR_VOL_DESC_PTR) && (i ||
					(ident != TID_FILE_ENTRY && ident != TID_EXTENDED_FILE_ENTRY)))
				{
					UDF_SB_ANCHOR(sb)[i] = 0;
				}
			}
		}
		else if (useranchor != 0xFFFFFFFF)
		{
			UDF_SB_ANCHOR(sb)[i] = useranchor;
			useranchor = 0xFFFFFFFF;
			i --;
		}
	}

	return lastblock;
}

static int 
udf_find_fileset(struct super_block *sb, lb_addr *fileset, lb_addr *root)
{
	struct buffer_head *bh = NULL;
	long lastblock;
	Uint16 ident;

	if (fileset->logicalBlockNum != 0xFFFFFFFF ||
		fileset->partitionReferenceNum != 0xFFFF)
	{
		bh = udf_read_ptagged(sb, *fileset, 0, &ident);

		if (!bh)
			return 1;
		else if (ident != TID_FILE_SET_DESC)
		{
			udf_release_data(bh);
			return 1;
		}
			
	}

	if (!bh) /* Search backwards through the partitions */
	{
		lb_addr newfileset;

		return 1;
		
		for (newfileset.partitionReferenceNum=UDF_SB_NUMPARTS(sb)-1;
			(newfileset.partitionReferenceNum != 0xFFFF &&
				fileset->logicalBlockNum == 0xFFFFFFFF &&
				fileset->partitionReferenceNum == 0xFFFF);
			newfileset.partitionReferenceNum--)
		{
			lastblock = UDF_SB_PARTLEN(sb, newfileset.partitionReferenceNum);
			newfileset.logicalBlockNum = 0;

			do
			{
				bh = udf_read_ptagged(sb, newfileset, 0, &ident);
				if (!bh)
				{
					newfileset.logicalBlockNum ++;
					continue;
				}

				switch (ident)
				{
					case TID_SPACE_BITMAP_DESC:
					{
						struct SpaceBitmapDesc *sp;
						sp = (struct SpaceBitmapDesc *)bh->b_data;
						newfileset.logicalBlockNum += 1 +
							((le32_to_cpu(sp->numOfBytes) + sizeof(struct SpaceBitmapDesc) - 1)
								>> sb->s_blocksize_bits);
						udf_release_data(bh);
						break;
					}
					case TID_FILE_SET_DESC:
					{
						*fileset = newfileset;
						break;
					}
					default:
					{
						newfileset.logicalBlockNum ++;
						udf_release_data(bh);
						bh = NULL;
						break;
					}
				}
			}
			while (newfileset.logicalBlockNum < lastblock &&
				fileset->logicalBlockNum == 0xFFFFFFFF &&
				fileset->partitionReferenceNum == 0xFFFF);
		}
	}

	if ((fileset->logicalBlockNum != 0xFFFFFFFF ||
		fileset->partitionReferenceNum != 0xFFFF) && bh)
	{
		udf_debug("Fileset at block=%d, partition=%d\n",
			fileset->logicalBlockNum, fileset->partitionReferenceNum);

		UDF_SB_PARTITION(sb) = fileset->partitionReferenceNum;
		udf_load_fileset(sb, bh, root);
		udf_release_data(bh);
		return 0;
	}
	return 1;
}

static void 
udf_load_pvoldesc(struct super_block *sb, struct buffer_head *bh)
{
	struct PrimaryVolDesc *pvoldesc;
	time_t recording;
	long recording_usec;
	struct ustr instr;
	struct ustr outstr;

	pvoldesc = (struct PrimaryVolDesc *)bh->b_data;

	if ( udf_stamp_to_time(&recording, &recording_usec,
		lets_to_cpu(pvoldesc->recordingDateAndTime)) )
	{
		timestamp ts;
		ts = lets_to_cpu(pvoldesc->recordingDateAndTime);
		udf_debug("recording time %ld/%ld, %04u/%02u/%02u %02u:%02u (%x)\n",
			recording, recording_usec,
			ts.year, ts.month, ts.day, ts.hour, ts.minute, ts.typeAndTimezone);
		UDF_SB_RECORDTIME(sb) = recording;
	}

	if ( !udf_build_ustr(&instr, pvoldesc->volIdent, 32) )
	{
		if (udf_CS0toUTF8(&outstr, &instr))
		{
			strncpy( UDF_SB_VOLIDENT(sb), outstr.u_name,
				outstr.u_len > 31 ? 31 : outstr.u_len);
			udf_debug("volIdent[] = '%s'\n", UDF_SB_VOLIDENT(sb));
		}
	}

	if ( !udf_build_ustr(&instr, pvoldesc->volSetIdent, 128) )
	{
		if (udf_CS0toUTF8(&outstr, &instr))
			udf_debug("volSetIdent[] = '%s'\n", outstr.u_name);
	}
}

static void 
udf_load_fileset(struct super_block *sb, struct buffer_head *bh, lb_addr *root)
{
	struct FileSetDesc *fset;

	fset = (struct FileSetDesc *)bh->b_data;

	*root = lelb_to_cpu(fset->rootDirectoryICB.extLocation);

	UDF_SB_SERIALNUM(sb) = le16_to_cpu(fset->descTag.tagSerialNum);

	udf_debug("Rootdir at block=%d, partition=%d\n", 
		root->logicalBlockNum, root->partitionReferenceNum);
}

static void 
udf_load_partdesc(struct super_block *sb, struct buffer_head *bh)
{
	struct PartitionDesc *p;
	int i;

	p=(struct PartitionDesc *)bh->b_data;

	for (i=0; i<UDF_SB_NUMPARTS(sb); i++)
	{
		udf_debug("Searching map: (%d == %d)\n", 
			UDF_SB_PARTMAPS(sb)[i].s_partition_num, le16_to_cpu(p->partitionNumber));
		if (UDF_SB_PARTMAPS(sb)[i].s_partition_num == le16_to_cpu(p->partitionNumber))
		{
			UDF_SB_PARTLEN(sb,i) = le32_to_cpu(p->partitionLength); /* blocks */
			UDF_SB_PARTROOT(sb,i) = le32_to_cpu(p->partitionStartingLocation) + UDF_SB_SESSION(sb);

			if (UDF_SB_PARTTYPE(sb,i) == UDF_SPARABLE_MAP15)
				udf_fill_spartable(sb, &UDF_SB_TYPESPAR(sb,i), UDF_SB_PARTLEN(sb,i));

			if (!strcmp(p->partitionContents.ident, PARTITION_CONTENTS_NSR02) ||
				!strcmp(p->partitionContents.ident, PARTITION_CONTENTS_NSR03))
			{
				struct PartitionHeaderDesc *phd;

				phd = (struct PartitionHeaderDesc *)(p->partitionContentsUse);
				if (phd->unallocatedSpaceTable.extLength)
					udf_debug("unallocatedSpaceTable (part %d)\n", i);
				if (phd->unallocatedSpaceBitmap.extLength)
				{
					UDF_SB_PARTMAPS(sb)[i].s_uspace.bitmap =
						le32_to_cpu(phd->unallocatedSpaceBitmap.extPosition);
					UDF_SB_PARTFLAGS(sb,i) |= UDF_PART_FLAG_UNALLOC_BITMAP;
					udf_debug("unallocatedSpaceBitmap (part %d) @ %d\n",
						i, UDF_SB_PARTMAPS(sb)[i].s_uspace.bitmap);
				}
				if (phd->partitionIntegrityTable.extLength)
					udf_debug("partitionIntegrityTable (part %d)\n", i);
				if (phd->freedSpaceTable.extLength)
					udf_debug("freedSpaceTable (part %d)\n", i);
				if (phd->freedSpaceBitmap.extLength)
				{
					UDF_SB_PARTMAPS(sb)[i].s_fspace.bitmap =
						le32_to_cpu(phd->freedSpaceBitmap.extPosition);
					UDF_SB_PARTFLAGS(sb,i) |= UDF_PART_FLAG_FREED_BITMAP;
					udf_debug("freedSpaceBitmap (part %d) @ %d\n",
						i, UDF_SB_PARTMAPS(sb)[i].s_fspace.bitmap);
				}
			}
			break;
		}
	}
	if (i == UDF_SB_NUMPARTS(sb))
	{
		udf_debug("Partition (%d) not found in partition map\n", le16_to_cpu(p->partitionNumber));
	}
	else
	{
		udf_debug("Partition (%d:%d type %x) starts at physical %d, block length %d\n",
			le16_to_cpu(p->partitionNumber), i, UDF_SB_PARTTYPE(sb,i),
			UDF_SB_PARTROOT(sb,i), UDF_SB_PARTLEN(sb,i));
	}
}

static int 
udf_load_logicalvol(struct super_block *sb, struct buffer_head * bh, lb_addr *fileset)
{
	struct LogicalVolDesc *lvd;
	int i, j, offset;
	Uint8 type;

	lvd = (struct LogicalVolDesc *)bh->b_data;

	UDF_SB_NUMPARTS(sb) = le32_to_cpu(lvd->numPartitionMaps);
	UDF_SB_ALLOC_PARTMAPS(sb, UDF_SB_NUMPARTS(sb));

	for (i=0,offset=0;
		 i<UDF_SB_NUMPARTS(sb) && offset<le32_to_cpu(lvd->mapTableLength);
		 i++,offset+=((struct GenericPartitionMap *)&(lvd->partitionMaps[offset]))->partitionMapLength)
	{
		type = ((struct GenericPartitionMap *)&(lvd->partitionMaps[offset]))->partitionMapType;
		udf_debug("Partition (%d) type %d\n", i, type);
		if (type == 1)
		{
			struct GenericPartitionMap1 *gpm1 = (struct GenericPartitionMap1 *)&(lvd->partitionMaps[offset]);
			UDF_SB_PARTTYPE(sb,i) = UDF_TYPE1_MAP15;
			UDF_SB_PARTVSN(sb,i) = le16_to_cpu(gpm1->volSeqNum);
			UDF_SB_PARTNUM(sb,i) = le16_to_cpu(gpm1->partitionNum);
			UDF_SB_PARTFUNC(sb,i) = NULL;
		}
		else if (type == 2)
		{
			struct UdfPartitionMap2 *upm2 = (struct UdfPartitionMap2 *)&(lvd->partitionMaps[offset]);
			if (!strncmp(upm2->partIdent.ident, UDF_ID_VIRTUAL, strlen(UDF_ID_VIRTUAL)))
			{
				if (le16_to_cpu(((Uint16 *)upm2->partIdent.identSuffix)[0]) == 0x0150)
				{
					UDF_SB_PARTTYPE(sb,i) = UDF_VIRTUAL_MAP15;
					UDF_SB_PARTFUNC(sb,i) = udf_get_pblock_virt15;
				}
				else if (le16_to_cpu(((Uint16 *)upm2->partIdent.identSuffix)[0]) == 0x0200)
				{
					UDF_SB_PARTTYPE(sb,i) = UDF_VIRTUAL_MAP20;
					UDF_SB_PARTFUNC(sb,i) = udf_get_pblock_virt20;
				}
			}
			else if (!strncmp(upm2->partIdent.ident, UDF_ID_SPARABLE, strlen(UDF_ID_SPARABLE)))
			{
				int plen;

				struct SparablePartitionMap *spm = (struct SparablePartitionMap *)&(lvd->partitionMaps[offset]);
				UDF_SB_PARTTYPE(sb,i) = UDF_SPARABLE_MAP15;
				plen = le16_to_cpu(spm->packetLength);
				UDF_SB_TYPESPAR(sb,i).s_spar_pshift = 0;
				while (plen >>= 1)
					UDF_SB_TYPESPAR(sb,i).s_spar_pshift ++;
				for (j=0; j<spm->numSparingTables; j++)
					UDF_SB_TYPESPAR(sb,i).s_spar_loc[j] = le32_to_cpu(spm->locSparingTable[j]);
				UDF_SB_PARTFUNC(sb,i) = udf_get_pblock_spar15;
			}
			else
			{
				udf_debug("Unknown ident: %s\n", upm2->partIdent.ident);
				continue;
			}
			UDF_SB_PARTVSN(sb,i) = le16_to_cpu(upm2->volSeqNum);
			UDF_SB_PARTNUM(sb,i) = le16_to_cpu(upm2->partitionNum);
		}
	}

	if (fileset)
	{
		long_ad *la = (long_ad *)&(lvd->logicalVolContentsUse[0]);

		*fileset = lelb_to_cpu(la->extLocation);
		udf_debug("FileSet found in LogicalVolDesc at block=%d, partition=%d\n",
			fileset->logicalBlockNum,
			fileset->partitionReferenceNum);
	}
	if (lvd->integritySeqExt.extLength)
		udf_load_logicalvolint(sb, leea_to_cpu(lvd->integritySeqExt));
	return 0;
}

/*
 * udf_load_logicalvolint
 *
 */
static void
udf_load_logicalvolint(struct super_block *sb, extent_ad loc)
{
	struct buffer_head *bh = NULL;
	Uint16 ident;

	while ((bh = udf_read_tagged(sb, loc.extLocation, loc.extLocation, &ident)) &&
		ident == TID_LOGICAL_VOL_INTEGRITY_DESC && loc.extLength > 0)
	{
		UDF_SB_LVIDBH(sb) = bh;
		
		if (UDF_SB_LVID(sb)->nextIntegrityExt.extLength)
			udf_load_logicalvolint(sb, leea_to_cpu(UDF_SB_LVID(sb)->nextIntegrityExt));
		
		if (UDF_SB_LVIDBH(sb) != bh)
			udf_release_data(bh);
		loc.extLength -= sb->s_blocksize;
		loc.extLocation ++;
	}
	if (UDF_SB_LVIDBH(sb) != bh)
		udf_release_data(bh);
}

/*
 * udf_process_sequence
 *
 * PURPOSE
 *	Process a main/reserve volume descriptor sequence.
 *
 * PRE-CONDITIONS
 *	sb			Pointer to _locked_ superblock.
 *	block			First block of first extent of the sequence.
 *	lastblock		Lastblock of first extent of the sequence.
 *
 * HISTORY
 *	July 1, 1997 - Andrew E. Mileski
 *	Written, tested, and released.
 */
static  int
udf_process_sequence(struct super_block *sb, long block, long lastblock, lb_addr *fileset)
{
	struct buffer_head *bh = NULL;
	struct udf_vds_record vds[VDS_POS_LENGTH];
	struct GenericDesc *gd;
	int done=0;
	int i,j;
	Uint32 vdsn;
	Uint16 ident;

	memset(vds, 0, sizeof(struct udf_vds_record) * VDS_POS_LENGTH);

	/* Read the main descriptor sequence */
	for (;(!done && block <= lastblock); block++)
	{

		bh = udf_read_tagged(sb, block, block, &ident);
		if (!bh) 
			break;

		/* Process each descriptor (ISO 13346 3/8.3-8.4) */
		gd = (struct GenericDesc *)bh->b_data;
		vdsn = le32_to_cpu(gd->volDescSeqNum);
		switch (ident)
		{
			case TID_PRIMARY_VOL_DESC: /* ISO 13346 3/10.1 */
				if (vdsn >= vds[VDS_POS_PRIMARY_VOL_DESC].volDescSeqNum)
				{
					vds[VDS_POS_PRIMARY_VOL_DESC].volDescSeqNum = vdsn;
					vds[VDS_POS_PRIMARY_VOL_DESC].block = block;
				}
				break;
			case TID_VOL_DESC_PTR: /* ISO 13346 3/10.3 */
				if (vdsn >= vds[VDS_POS_VOL_DESC_PTR].volDescSeqNum)
				{
					vds[VDS_POS_VOL_DESC_PTR].volDescSeqNum = vdsn;
					vds[VDS_POS_VOL_DESC_PTR].block = block;
				}
				break;
			case TID_IMP_USE_VOL_DESC: /* ISO 13346 3/10.4 */
				if (vdsn >= vds[VDS_POS_IMP_USE_VOL_DESC].volDescSeqNum)
				{
					vds[VDS_POS_IMP_USE_VOL_DESC].volDescSeqNum = vdsn;
					vds[VDS_POS_IMP_USE_VOL_DESC].block = block;
				}
				break;
			case TID_PARTITION_DESC: /* ISO 13346 3/10.5 */
				if (!vds[VDS_POS_PARTITION_DESC].block)
					vds[VDS_POS_PARTITION_DESC].block = block;
				break;
			case TID_LOGICAL_VOL_DESC: /* ISO 13346 3/10.6 */
				if (vdsn >= vds[VDS_POS_LOGICAL_VOL_DESC].volDescSeqNum)
				{
					vds[VDS_POS_LOGICAL_VOL_DESC].volDescSeqNum = vdsn;
					vds[VDS_POS_LOGICAL_VOL_DESC].block = block;
				}
				break;
			case TID_UNALLOC_SPACE_DESC: /* ISO 13346 3/10.8 */
				if (vdsn >= vds[VDS_POS_UNALLOC_SPACE_DESC].volDescSeqNum)
				{
					vds[VDS_POS_UNALLOC_SPACE_DESC].volDescSeqNum = vdsn;
					vds[VDS_POS_UNALLOC_SPACE_DESC].block = block;
				}
				break;
			case TID_TERMINATING_DESC: /* ISO 13346 3/10.9 */
				vds[VDS_POS_TERMINATING_DESC].block = block;
				done = 1;
				break;
		}
		udf_release_data(bh);
	}
	for (i=0; i<VDS_POS_LENGTH; i++)
	{
		if (vds[i].block)
		{
			bh = udf_read_tagged(sb, vds[i].block, vds[i].block, &ident);

			if (i == VDS_POS_PRIMARY_VOL_DESC)
				udf_load_pvoldesc(sb, bh);
			else if (i == VDS_POS_LOGICAL_VOL_DESC)
				udf_load_logicalvol(sb, bh, fileset);
			else if (i == VDS_POS_PARTITION_DESC)
			{
				struct buffer_head *bh2 = NULL;
				udf_load_partdesc(sb, bh);
				for (j=vds[i].block+1; j<vds[VDS_POS_TERMINATING_DESC].block; j++)
				{
					bh2 = udf_read_tagged(sb, j, j, &ident);
					gd = (struct GenericDesc *)bh2->b_data;
					if (ident == TID_PARTITION_DESC)
						udf_load_partdesc(sb, bh2);
					udf_release_data(bh2);
				}
			}
			udf_release_data(bh);
		}
	}

	return 0;
}

/*
 * udf_check_valid()
 */
static int
udf_check_valid(struct super_block *sb, int novrs, int silent)
{
	long block;

	if (novrs)
	{
		udf_debug("Validity check skipped because of novrs option\n");
		return 0;
	}
	/* Check that it is NSR02 compliant */
	/* Process any "CD-ROM Volume Descriptor Set" (ECMA 167 2/8.3.1) */
	else if ((block = udf_vrs(sb, silent)) == -1)
	{
		udf_debug("Failed to read byte 32768. Assuming open disc. Skipping validity check\n");
		return 0;
	}
	else 
		return !block;
}

static int
udf_load_partition(struct super_block *sb, lb_addr *fileset)
{
	struct AnchorVolDescPtr *anchor;
	Uint16 ident;
	struct buffer_head *bh;
	long main_s, main_e, reserve_s, reserve_e;
	int i, j;

	if (!sb)
		return 1;

	for (i=0; i<sizeof(UDF_SB_ANCHOR(sb))/sizeof(int); i++)
	{
		if (UDF_SB_ANCHOR(sb)[i] && (bh = udf_read_tagged(sb,
			UDF_SB_ANCHOR(sb)[i], UDF_SB_ANCHOR(sb)[i] - UDF_SB_SESSION(sb), &ident)))
		{
			anchor = (struct AnchorVolDescPtr *)bh->b_data;

			/* Locate the main sequence */
			main_s = le32_to_cpu( anchor->mainVolDescSeqExt.extLocation );
			main_e = le32_to_cpu( anchor->mainVolDescSeqExt.extLength );
			main_e = main_e >> sb->s_blocksize_bits;
			main_e += main_s;
	
			/* Locate the reserve sequence */
			reserve_s = le32_to_cpu(anchor->reserveVolDescSeqExt.extLocation);
			reserve_e = le32_to_cpu(anchor->reserveVolDescSeqExt.extLength);
			reserve_e = reserve_e >> sb->s_blocksize_bits;
			reserve_e += reserve_s;

			udf_release_data(bh);

			/* Process the main & reserve sequences */
			/* responsible for finding the PartitionDesc(s) */
			if (!(udf_process_sequence(sb, main_s, main_e, fileset) &&
				udf_process_sequence(sb, reserve_s, reserve_e, fileset)))
			{
				break;
			}
		}
	}

	if (i == sizeof(UDF_SB_ANCHOR(sb))/sizeof(int))
	{
		udf_debug("No Anchor block found\n");
		return 1;
	}
	else
		udf_debug("Using anchor in block %d\n", UDF_SB_ANCHOR(sb)[i]);

	for (i=0; i<UDF_SB_NUMPARTS(sb); i++)
	{
		switch UDF_SB_PARTTYPE(sb, i)
		{
			case UDF_VIRTUAL_MAP15:
			case UDF_VIRTUAL_MAP20:
			{
				lb_addr ino;

				if (!UDF_SB_LASTBLOCK(sb))
				{
					udf_debug("Unable to determine Lastblock (For Virtual Partition)\n");
					return 1;
				}

				for (j=0; j<UDF_SB_NUMPARTS(sb); j++)
				{
					if (j != i &&
						UDF_SB_PARTVSN(sb,i) == UDF_SB_PARTVSN(sb,j) &&
						UDF_SB_PARTNUM(sb,i) == UDF_SB_PARTNUM(sb,j))
					{
							ino.partitionReferenceNum = j;
							ino.logicalBlockNum = UDF_SB_LASTBLOCK(sb) -
								UDF_SB_PARTROOT(sb,j);
							break;
					}
				}

				if (j == UDF_SB_NUMPARTS(sb))
					return 1;

				if (!(UDF_SB_VAT(sb) = udf_iget(sb, ino)))
					return 1;

				if (UDF_SB_PARTTYPE(sb,i) == UDF_VIRTUAL_MAP15)
				{
					UDF_SB_TYPEVIRT(sb,i).s_start_offset = udf_ext0_offset(UDF_SB_VAT(sb));
					UDF_SB_TYPEVIRT(sb,i).s_num_entries = (UDF_SB_VAT(sb)->i_size - 36) >> 2;
				}
				else if (UDF_SB_PARTTYPE(sb,i) == UDF_VIRTUAL_MAP20)
				{
					struct buffer_head *bh = NULL;
					Uint32 pos;

					pos = udf_block_map(UDF_SB_VAT(sb), 0);
					bh = bread(sb->s_dev, pos, sb->s_blocksize);
					UDF_SB_TYPEVIRT(sb,i).s_start_offset =
						le16_to_cpu(((struct VirtualAllocationTable20 *)bh->b_data + udf_ext0_offset(UDF_SB_VAT(sb)))->lengthHeader) +
							udf_ext0_offset(UDF_SB_VAT(sb));
					UDF_SB_TYPEVIRT(sb,i).s_num_entries = (UDF_SB_VAT(sb)->i_size -
						UDF_SB_TYPEVIRT(sb,i).s_start_offset) >> 2;
					udf_release_data(bh);
				}
				UDF_SB_PARTROOT(sb,i) = udf_get_pblock(sb, 0, i, 0);
				UDF_SB_PARTLEN(sb,i) = UDF_SB_PARTLEN(sb,ino.partitionReferenceNum);
			}
		}
	}
	return 0;
}

static void udf_open_lvid(struct super_block *sb)
{
	if (UDF_SB_LVIDBH(sb))
	{
		int i;
		timestamp cpu_time;

		UDF_SB_LVIDIU(sb)->impIdent.identSuffix[0] = UDF_OS_CLASS_UNIX;
		UDF_SB_LVIDIU(sb)->impIdent.identSuffix[1] = UDF_OS_ID_LINUX;
		if (udf_time_to_stamp(&cpu_time, CURRENT_TIME, CURRENT_UTIME))
			UDF_SB_LVID(sb)->recordingDateAndTime = cpu_to_lets(cpu_time);
		UDF_SB_LVID(sb)->integrityType = INTEGRITY_TYPE_OPEN;

		UDF_SB_LVID(sb)->descTag.descCRC =
			cpu_to_le16(udf_crc((char *)UDF_SB_LVID(sb) + sizeof(tag),
			le16_to_cpu(UDF_SB_LVID(sb)->descTag.descCRCLength), 0));

		UDF_SB_LVID(sb)->descTag.tagChecksum = 0;
		for (i=0; i<16; i++)
			if (i != 4)
				UDF_SB_LVID(sb)->descTag.tagChecksum +=
					((Uint8 *)&(UDF_SB_LVID(sb)->descTag))[i];

		mark_buffer_dirty(UDF_SB_LVIDBH(sb));
		sb->s_dirt = 0;
	}
}

static void udf_close_lvid(struct super_block *sb)
{
	if (UDF_SB_LVIDBH(sb) &&
		UDF_SB_LVID(sb)->integrityType == INTEGRITY_TYPE_OPEN)
	{
		int i;
		timestamp cpu_time;

		UDF_SB_LVIDIU(sb)->impIdent.identSuffix[0] = UDF_OS_CLASS_UNIX;
		UDF_SB_LVIDIU(sb)->impIdent.identSuffix[1] = UDF_OS_ID_LINUX;
		if (udf_time_to_stamp(&cpu_time, CURRENT_TIME, CURRENT_UTIME))
			UDF_SB_LVID(sb)->recordingDateAndTime = cpu_to_lets(cpu_time);
		if (UDF_MAX_WRITE_VERSION > le16_to_cpu(UDF_SB_LVIDIU(sb)->maxUDFWriteRev))
			UDF_SB_LVIDIU(sb)->maxUDFWriteRev = cpu_to_le16(UDF_MAX_WRITE_VERSION);
		if (UDF_SB_UDFREV(sb) > le16_to_cpu(UDF_SB_LVIDIU(sb)->minUDFReadRev))
			UDF_SB_LVIDIU(sb)->minUDFReadRev = cpu_to_le16(UDF_SB_UDFREV(sb));
		if (UDF_SB_UDFREV(sb) > le16_to_cpu(UDF_SB_LVIDIU(sb)->minUDFWriteRev))
			UDF_SB_LVIDIU(sb)->minUDFWriteRev = cpu_to_le16(UDF_SB_UDFREV(sb));
		UDF_SB_LVID(sb)->integrityType = INTEGRITY_TYPE_CLOSE;

		UDF_SB_LVID(sb)->descTag.descCRC =
			cpu_to_le16(udf_crc((char *)UDF_SB_LVID(sb) + sizeof(tag),
			le16_to_cpu(UDF_SB_LVID(sb)->descTag.descCRCLength), 0));

		UDF_SB_LVID(sb)->descTag.tagChecksum = 0;
		for (i=0; i<16; i++)
			if (i != 4)
				UDF_SB_LVID(sb)->descTag.tagChecksum +=
					((Uint8 *)&(UDF_SB_LVID(sb)->descTag))[i];

		mark_buffer_dirty(UDF_SB_LVIDBH(sb));
	}
}

/*
 * udf_read_super
 *
 * PURPOSE
 *	Complete the specified super block.
 *
 * PRE-CONDITIONS
 *	sb			Pointer to superblock to complete - never NULL.
 *	sb->s_dev		Device to read suberblock from.
 *	options			Pointer to mount options.
 *	silent			Silent flag.
 *
 * HISTORY
 *	July 1, 1997 - Andrew E. Mileski
 *	Written, tested, and released.
 */
static struct super_block *
udf_read_super(struct super_block *sb, void *options, int silent)
{
	struct inode *inode=NULL;
	struct udf_options uopt;
	lb_addr rootdir, fileset;
	int i;

	uopt.flags = (1 << UDF_FLAG_USE_AD_IN_ICB);
	uopt.uid = -1;
	uopt.gid = -1;
	uopt.umask = 0;

	memset(UDF_SB(sb), 0x00, sizeof(struct udf_sb_info));

#if CONFIG_UDF_RW != 1
	sb->s_flags |= MS_RDONLY;
#endif

	if (!udf_parse_options((char *)options, &uopt))
		goto error_out;

	fileset.logicalBlockNum = 0xFFFFFFFF;
	fileset.partitionReferenceNum = 0xFFFF;

	UDF_SB(sb)->s_flags = uopt.flags;
	UDF_SB(sb)->s_uid = uopt.uid;
	UDF_SB(sb)->s_gid = uopt.gid;
	UDF_SB(sb)->s_umask = uopt.umask;

	/* Set the block size for all transfers */
	if (!udf_set_blocksize(sb, uopt.blocksize))
		goto error_out;

	if ( uopt.session == 0xFFFFFFFF )
		UDF_SB_SESSION(sb) = udf_get_last_session(sb);
	else
		UDF_SB_SESSION(sb) = uopt.session;

	udf_debug("Multi-session=%d\n", UDF_SB_SESSION(sb));

	if ( uopt.lastblock == 0xFFFFFFFF )
		UDF_SB_LASTBLOCK(sb) = udf_get_last_block(sb);
	else
		UDF_SB_LASTBLOCK(sb) = uopt.lastblock;

	UDF_SB_LASTBLOCK(sb) = udf_find_anchor(sb, uopt.anchor, UDF_SB_LASTBLOCK(sb));

	udf_debug("Lastblock=%d\n", UDF_SB_LASTBLOCK(sb));

	if (udf_check_valid(sb, uopt.novrs, silent)) /* read volume recognition sequences */
	{
		printk("UDF-fs: No VRS found\n");
 		goto error_out;
	}

	/* Fill in the rest of the superblock */
	sb->s_op = &udf_sb_ops;
	sb->dq_op = NULL;
	sb->s_dirt = 0;
	sb->s_magic = UDF_SUPER_MAGIC;

	for (i=0; i<UDF_MAX_BLOCK_LOADED; i++)
	{
		UDF_SB_BLOCK_BITMAP_NUMBER(sb,i) = 0;
		UDF_SB_BLOCK_BITMAP(sb,i) = NULL;
	}
	UDF_SB_LOADED_BLOCK_BITMAPS(sb) = 0;

	if (udf_load_partition(sb, &fileset))
	{
		printk("UDF-fs: No partition found (1)\n");
		goto error_out;
	}

	if ( UDF_SB_LVIDBH(sb) )
	{
		Uint16 minUDFReadRev = le16_to_cpu(UDF_SB_LVIDIU(sb)->minUDFReadRev);
		Uint16 minUDFWriteRev = le16_to_cpu(UDF_SB_LVIDIU(sb)->minUDFWriteRev);
		/* Uint16 maxUDFWriteRev = le16_to_cpu(UDF_SB_LVIDIU(sb)->maxUDFWriteRev); */

		if (minUDFReadRev > UDF_MAX_READ_VERSION)
		{
			printk("UDF-fs: minUDFReadRev=%x (max is %x)\n",
				UDF_SB_LVIDIU(sb)->minUDFReadRev, UDF_MAX_READ_VERSION);
			goto error_out;
		}
		else if (minUDFWriteRev > UDF_MAX_WRITE_VERSION)
		{
			sb->s_flags |= MS_RDONLY;
		}

		if (minUDFReadRev >= UDF_VERS_USE_EXTENDED_FE)
			UDF_SET_FLAG(sb, UDF_FLAG_USE_EXTENDED_FE);
		if (minUDFReadRev >= UDF_VERS_USE_STREAMS)
			UDF_SET_FLAG(sb, UDF_FLAG_USE_STREAMS);
	}

	if ( !UDF_SB_NUMPARTS(sb) )
	{
		printk("UDF-fs: No partition found (2)\n");
		goto error_out;
	}

	if ( udf_find_fileset(sb, &fileset, &rootdir) )
	{
		printk("UDF-fs: No fileset found\n");
		goto error_out;
	}

	if (!silent)
	{
		timestamp ts;
		udf_time_to_stamp(&ts, UDF_SB_RECORDTIME(sb), 0);
		udf_info("UDF %s (%s) Mounting volume '%s', timestamp %04u/%02u/%02u %02u:%02u (%x)\n",
			UDFFS_VERSION, UDFFS_DATE,
			UDF_SB_VOLIDENT(sb), ts.year, ts.month, ts.day, ts.hour, ts.minute,
			ts.typeAndTimezone);
	}
	if (!(sb->s_flags & MS_RDONLY))
		udf_open_lvid(sb);

	/* Assign the root inode */
	/* assign inodes by physical block number */
	/* perhaps it's not extensible enough, but for now ... */
	inode = udf_iget(sb, rootdir); 
	if (!inode)
	{
		printk("UDF-fs: Error in udf_iget, block=%d, partition=%d\n",
			rootdir.logicalBlockNum, rootdir.partitionReferenceNum);
		goto error_out;
	}

	/* Allocate a dentry for the root inode */
	sb->s_root = d_alloc_root(inode);
	if (!sb->s_root)
	{
		printk("UDF-fs: Couldn't allocate root dentry\n");
		iput(inode);
		goto error_out;
	}

	return sb;

error_out:
	if (UDF_SB_VAT(sb))
		iput(UDF_SB_VAT(sb));
	if (!(sb->s_flags & MS_RDONLY))
		udf_close_lvid(sb);
	udf_release_data(UDF_SB_LVIDBH(sb));
	UDF_SB_FREE(sb);
	return NULL;
}

void udf_error(struct super_block *sb, const char *function,
	const char *fmt, ...)
{
	va_list args;

	if (!(sb->s_flags & MS_RDONLY))
	{
		/* mark sb error */
		sb->s_dirt = 1;
	}
	va_start(args, fmt);
	vsprintf(error_buf, fmt, args);
	va_end(args);
	printk (KERN_CRIT "UDF-fs error (device %s): %s: %s\n",
		bdevname(sb->s_dev), function, error_buf);
}

void udf_warning(struct super_block *sb, const char *function,
	const char *fmt, ...)
{
	va_list args;

	va_start (args, fmt);
	vsprintf(error_buf, fmt, args);
	va_end(args);
	printk(KERN_WARNING "UDF-fs warning (device %s): %s: %s\n",
		bdevname(sb->s_dev), function, error_buf);
}

/*
 * udf_put_super
 *
 * PURPOSE
 *	Prepare for destruction of the superblock.
 *
 * DESCRIPTION
 *	Called before the filesystem is unmounted.
 *
 * HISTORY
 *	July 1, 1997 - Andrew E. Mileski
 *	Written, tested, and released.
 */
static void
udf_put_super(struct super_block *sb)
{
	int i;

	if (UDF_SB_VAT(sb))
		iput(UDF_SB_VAT(sb));
	if (!(sb->s_flags & MS_RDONLY))
		udf_close_lvid(sb);
	udf_release_data(UDF_SB_LVIDBH(sb));
	for (i=0; i<UDF_MAX_BLOCK_LOADED; i++)
		udf_release_data(UDF_SB_BLOCK_BITMAP(sb, i));
	UDF_SB_FREE(sb);
}

/*
 * udf_stat_fs
 *
 * PURPOSE
 *	Return info about the filesystem.
 *
 * DESCRIPTION
 *	Called by sys_statfs()
 *
 * HISTORY
 *	July 1, 1997 - Andrew E. Mileski
 *	Written, tested, and released.
 */
static int
udf_statfs(struct super_block *sb, struct statfs *buf)
{
	buf->f_type = UDF_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = UDF_SB_PARTLEN(sb, UDF_SB_PARTITION(sb));
	buf->f_bfree = udf_count_free(sb);
	buf->f_bavail = buf->f_bfree;
	buf->f_files = (UDF_SB_LVIDBH(sb) ?
		(le32_to_cpu(UDF_SB_LVIDIU(sb)->numFiles) +
		le32_to_cpu(UDF_SB_LVIDIU(sb)->numDirs)) : 0) + buf->f_bfree;
	buf->f_ffree = buf->f_bfree;
	/* __kernel_fsid_t f_fsid */
	buf->f_namelen = UDF_NAME_LEN;

	return 0;
}

static unsigned char udf_bitmap_lookup[16] = {
	0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
};
	
static unsigned int
udf_count_free(struct super_block *sb)
{
	struct buffer_head *bh = NULL;
	unsigned int accum = 0;
	lb_addr loc;
	Uint32 bitmap;

	if (UDF_SB_PARTFLAGS(sb,UDF_SB_PARTITION(sb)) & UDF_PART_FLAG_UNALLOC_BITMAP)
		bitmap = UDF_SB_PARTMAPS(sb)[UDF_SB_PARTITION(sb)].s_uspace.bitmap;
	else if (UDF_SB_PARTFLAGS(sb,UDF_SB_PARTITION(sb)) & UDF_PART_FLAG_FREED_BITMAP)
		bitmap = UDF_SB_PARTMAPS(sb)[UDF_SB_PARTITION(sb)].s_fspace.bitmap;
	else
		bitmap = 0xFFFFFFFF;

	if (bitmap != 0xFFFFFFFF)
	{
		struct SpaceBitmapDesc *bm;
		int block = 0, newblock, index;
		Uint16 ident;
		Uint32 bytes;
		Uint8 value;
		Uint8 * ptr;

		loc.logicalBlockNum = bitmap;
		loc.partitionReferenceNum = UDF_SB_PARTITION(sb);
		bh = udf_read_ptagged(sb, loc, 0, &ident);

		if (!bh)
		{
			printk(KERN_ERR "udf: udf_count_free failed\n");
			return 0;
		}
		else if (ident != TID_SPACE_BITMAP_DESC)
		{
			udf_release_data(bh);
			printk(KERN_ERR "udf: udf_count_free failed\n");
			return 0;
		}

		bm = (struct SpaceBitmapDesc *)bh->b_data;
		bytes = bm->numOfBytes;
		index = sizeof(struct SpaceBitmapDesc); /* offset in first block only */
		ptr = (Uint8 *)bh->b_data;

		while ( bytes > 0 )
		{
			while ((bytes > 0) && (index < sb->s_blocksize))
			{
				value = ptr[index];
				accum += udf_bitmap_lookup[ value & 0x0f ];
				accum += udf_bitmap_lookup[ value >> 4 ];
				index++;
				bytes--;
			}
			if ( bytes )
			{
				udf_release_data(bh);
				newblock = udf_get_lb_pblock(sb, loc, ++block);
				bh = udf_tread(sb, newblock, sb->s_blocksize);
				if (!bh)
				{
					udf_debug("read failed\n");
					return accum;
				}
				index = 0;
				ptr = (Uint8 *)bh->b_data;
			}
		}
		udf_release_data(bh);
	}
	else
	{
		if (UDF_SB_LVIDBH(sb))
		{
			if (le32_to_cpu(UDF_SB_LVID(sb)->numOfPartitions) > UDF_SB_PARTITION(sb))
				accum = le32_to_cpu(UDF_SB_LVID(sb)->freeSpaceTable[UDF_SB_PARTITION(sb)]);

			if (accum == 0xFFFFFFFF)
				accum = 0;
		}
	}

	return accum;
}
