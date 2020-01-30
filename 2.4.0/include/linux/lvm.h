/*
 * include/linux/lvm.h
 * kernel/lvm.h
 * tools/lib/lvm.h
 *
 * Copyright (C) 1997 - 2000  Heinz Mauelshagen, Sistina Software
 *
 * February-November 1997
 * May-July 1998
 * January-March,July,September,October,Dezember 1999
 * January,February,July,November 2000
 *
 * lvm is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * lvm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA. 
 *
 */

/*
 * Changelog
 *
 *    10/10/1997 - beginning of new structure creation
 *    12/05/1998 - incorporated structures from lvm_v1.h and deleted lvm_v1.h
 *    07/06/1998 - avoided LVM_KMALLOC_MAX define by using vmalloc/vfree
 *                 instead of kmalloc/kfree
 *    01/07/1998 - fixed wrong LVM_MAX_SIZE
 *    07/07/1998 - extended pe_t structure by ios member (for statistic)
 *    02/08/1998 - changes for official char/block major numbers
 *    07/08/1998 - avoided init_module() and cleanup_module() to be static
 *    29/08/1998 - seprated core and disk structure type definitions
 *    01/09/1998 - merged kernel integration version (mike)
 *    20/01/1999 - added LVM_PE_DISK_OFFSET macro for use in
 *                 vg_read_with_pv_and_lv(), pv_move_pe(), pv_show_pe_text()...
 *    18/02/1999 - added definition of time_disk_t structure for;
 *                 keeps time stamps on disk for nonatomic writes (future)
 *    15/03/1999 - corrected LV() and VG() macro definition to use argument
 *                 instead of minor
 *    03/07/1999 - define for genhd.c name handling
 *    23/07/1999 - implemented snapshot part
 *    08/12/1999 - changed LVM_LV_SIZE_MAX macro to reflect current 1TB limit
 *    01/01/2000 - extended lv_v2 core structure by wait_queue member
 *    12/02/2000 - integrated Andrea Arcagnelli's snapshot work
 *    18/02/2000 - seperated user and kernel space parts by 
 *                 #ifdef them with __KERNEL__
 *    08/03/2000 - implemented cluster/shared bits for vg_access
 *    26/06/2000 - implemented snapshot persistency and resizing support
 *    02/11/2000 - added hash table size member to lv structure
 *    12/11/2000 - removed unneeded timestamp definitions
 *
 */


#ifndef _LVM_H_INCLUDE
#define _LVM_H_INCLUDE

#define	_LVM_KERNEL_H_VERSION	"LVM 0.9 (13/11/2000)"

#include <linux/config.h>
#include <linux/version.h>
#include <endian.h>

/*
 * preprocessor definitions
 */
/* if you like emergency reset code in the driver */
#define	LVM_TOTAL_RESET

#ifdef __KERNEL__
#define LVM_GET_INODE
#undef	LVM_HD_NAME /* display nice names in /proc/partitions */

/* lots of debugging output (see driver source)
   #define DEBUG_LVM_GET_INFO
   #define DEBUG
   #define DEBUG_MAP
   #define DEBUG_MAP_SIZE
   #define DEBUG_IOCTL
   #define DEBUG_READ
   #define DEBUG_GENDISK
   #define DEBUG_VG_CREATE
   #define DEBUG_LVM_BLK_OPEN
   #define DEBUG_KFREE
 */
#endif				/* #ifdef __KERNEL__ */

#ifndef __KERNEL__
#define __KERNEL__
#include <linux/kdev_t.h>
#include <linux/list.h>
#undef __KERNEL__
#else
#include <linux/kdev_t.h>
#include <linux/list.h>
#endif				/* #ifndef __KERNEL__ */

#include <asm/types.h>
#include <linux/major.h>

#ifdef __KERNEL__
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#endif				/* #ifdef __KERNEL__ */

#include <asm/page.h>

#if !defined ( LVM_BLK_MAJOR) || !defined ( LVM_CHAR_MAJOR)
#error Bad include/linux/major.h - LVM MAJOR undefined
#endif

#ifdef	BLOCK_SIZE
#undef	BLOCK_SIZE
#endif

#ifdef CONFIG_ARCH_S390 
#define BLOCK_SIZE	4096
#else
#define BLOCK_SIZE	1024
#endif

#ifndef	SECTOR_SIZE
#define SECTOR_SIZE	512
#endif

#define LVM_STRUCT_VERSION	1	/* structure version */

#define	LVM_DIR_PREFIX	"/dev/"

#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#endif
#ifndef max
#define max(a,b) (((a)>(b))?(a):(b))
#endif

/* set the default structure version */
#if ( LVM_STRUCT_VERSION == 1)
#define pv_t pv_v2_t
#define lv_t lv_v4_t
#define vg_t vg_v3_t
#define pv_disk_t pv_disk_v2_t
#define lv_disk_t lv_disk_v3_t
#define vg_disk_t vg_disk_v2_t
#define lv_block_exception_t lv_block_exception_v1_t
#define lv_COW_table_disk_t lv_COW_table_disk_v1_t
#endif



/*
 * i/o protocol version
 *
 * defined here for the driver and defined seperate in the
 * user land tools/lib/liblvm.h
 *
 */
#define	LVM_DRIVER_IOP_VERSION	        10

#define LVM_NAME        "lvm"
#define LVM_GLOBAL	"global"
#define LVM_DIR		"lvm"
#define LVM_VG_SUBDIR	"VGs"
#define LVM_LV_SUBDIR	"LVs"
#define LVM_PV_SUBDIR	"PVs"

/*
 * VG/LV indexing macros
 */
/* character minor maps directly to volume group */
#define	VG_CHR(a) ( a)

/* block minor indexes into a volume group/logical volume indirection table */
#define	VG_BLK(a)	( vg_lv_map[a].vg_number)
#define LV_BLK(a)	( vg_lv_map[a].lv_number)

/*
 * absolute limits for VGs, PVs per VG and LVs per VG
 */
#define ABS_MAX_VG	99
#define ABS_MAX_PV	256
#define ABS_MAX_LV	256	/* caused by 8 bit minor */

#define MAX_VG  ABS_MAX_VG
#define MAX_LV	ABS_MAX_LV
#define	MAX_PV	ABS_MAX_PV

#if ( MAX_VG > ABS_MAX_VG)
#undef MAX_VG
#define MAX_VG ABS_MAX_VG
#endif

#if ( MAX_LV > ABS_MAX_LV)
#undef MAX_LV
#define MAX_LV ABS_MAX_LV
#endif


/*
 * VGDA: default disk spaces and offsets
 *
 *   there's space after the structures for later extensions.
 *
 *   offset            what                                size
 *   ---------------   ----------------------------------  ------------
 *   0                 physical volume structure           ~500 byte
 *
 *   1K                volume group structure              ~200 byte
 *
 *   5K                time stamp structure                ~
 *
 *   6K                namelist of physical volumes        128 byte each
 *
 *   6k + n * 128byte  n logical volume structures         ~300 byte each
 *
 *   + m * 328byte     m physical extent alloc. structs    4 byte each
 *
 *   End of disk -     first physical extent               typical 4 megabyte
 *   PE total *
 *   PE size
 *
 *
 */

/* DONT TOUCH THESE !!! */
/* base of PV structure in disk partition */
#define	LVM_PV_DISK_BASE  	0L

/* size reserved for PV structure on disk */
#define	LVM_PV_DISK_SIZE  	1024L

/* base of VG structure in disk partition */
#define	LVM_VG_DISK_BASE  	LVM_PV_DISK_SIZE

/* size reserved for VG structure */
#define	LVM_VG_DISK_SIZE  	( 9 * 512L)

/* size reserved for timekeeping */
#define	LVM_TIMESTAMP_DISK_BASE	( LVM_VG_DISK_BASE +  LVM_VG_DISK_SIZE)
#define	LVM_TIMESTAMP_DISK_SIZE	512L	/* reserved for timekeeping */

/* name list of physical volumes on disk */
#define	LVM_PV_UUIDLIST_DISK_BASE ( LVM_TIMESTAMP_DISK_BASE + \
                                    LVM_TIMESTAMP_DISK_SIZE)

/* now for the dynamically calculated parts of the VGDA */
#define	LVM_LV_DISK_OFFSET(a, b) ( (a)->lv_on_disk.base + \
                                   sizeof ( lv_disk_t) * b)
#define	LVM_DISK_SIZE(pv) 	 ( (pv)->pe_on_disk.base + \
                                   (pv)->pe_on_disk.size)
#define	LVM_PE_DISK_OFFSET(pe, pv)	( pe * pv->pe_size + \
					  ( LVM_DISK_SIZE ( pv) / SECTOR_SIZE))
#define	LVM_PE_ON_DISK_BASE(pv) \
   { int rest; \
     pv->pe_on_disk.base = pv->lv_on_disk.base + pv->lv_on_disk.size; \
     if ( ( rest = pv->pe_on_disk.base % SECTOR_SIZE) != 0) \
        pv->pe_on_disk.base += ( SECTOR_SIZE - rest); \
   }
/* END default disk spaces and offsets for PVs */


/*
 * LVM_PE_T_MAX corresponds to:
 *
 * 8KB PE size can map a ~512 MB logical volume at the cost of 1MB memory,
 *
 * 128MB PE size can map a 8TB logical volume at the same cost of memory.
 *
 * Default PE size of 4 MB gives a maximum logical volume size of 256 GB.
 *
 * Maximum PE size of 16GB gives a maximum logical volume size of 1024 TB.
 *
 * AFAIK, the actual kernels limit this to 1 TB.
 *
 * Should be a sufficient spectrum ;*)
 */

/* This is the usable size of pe_disk_t.le_num !!!        v     v */
#define	LVM_PE_T_MAX		( ( 1 << ( sizeof ( uint16_t) * 8)) - 2)

#define	LVM_LV_SIZE_MAX(a)	( ( long long) LVM_PE_T_MAX * (a)->pe_size > ( long long) 1024*1024/SECTOR_SIZE*1024*1024 ? ( long long) 1024*1024/SECTOR_SIZE*1024*1024 : ( long long) LVM_PE_T_MAX * (a)->pe_size)
#define	LVM_MIN_PE_SIZE		( 8192L / SECTOR_SIZE) /* 8 KB in sectors */
#define	LVM_MAX_PE_SIZE		( 16L * 1024L * 1024L / SECTOR_SIZE * 1024)	/* 16GB in sectors */
#define	LVM_DEFAULT_PE_SIZE	( 4096L * 1024 / SECTOR_SIZE)	/* 4 MB in sectors */
#define	LVM_DEFAULT_STRIPE_SIZE	16L	/* 16 KB  */
#define	LVM_MIN_STRIPE_SIZE	( PAGE_SIZE>>9)		/* PAGESIZE in sectors */
#define	LVM_MAX_STRIPE_SIZE	( 512L * 1024 / SECTOR_SIZE)	/* 512 KB in sectors */
#define	LVM_MAX_STRIPES		128	/* max # of stripes */
#define	LVM_MAX_SIZE            ( 1024LU * 1024 / SECTOR_SIZE * 1024 * 1024)	/* 1TB[sectors] */
#define	LVM_MAX_MIRRORS    	2	/* future use */
#define	LVM_MIN_READ_AHEAD	2	/* minimum read ahead sectors */
#define	LVM_MAX_READ_AHEAD	120	/* maximum read ahead sectors */
#define	LVM_MAX_LV_IO_TIMEOUT	60	/* seconds I/O timeout (future use) */
#define	LVM_PARTITION           0xfe	/* LVM partition id */
#define	LVM_NEW_PARTITION       0x8e	/* new LVM partition id (10/09/1999) */
#define	LVM_PE_SIZE_PV_SIZE_REL	5	/* max relation PV size and PE size */

#define	LVM_SNAPSHOT_MAX_CHUNK	1024	/* 1024 KB */
#define	LVM_SNAPSHOT_DEF_CHUNK	64	/* 64  KB */
#define	LVM_SNAPSHOT_MIN_CHUNK	1	/* 1   KB */

#define	UNDEF	-1
#define FALSE	0
#define TRUE	1


#define LVM_GET_COW_TABLE_CHUNKS_PER_PE(vg, lv) ( \
	vg->pe_size / lv->lv_chunk_size)

#define LVM_GET_COW_TABLE_ENTRIES_PER_PE(vg, lv) ( \
{ \
	int COW_table_entries_per_PE; \
	int COW_table_chunks_per_PE; \
\
	COW_table_entries_per_PE = LVM_GET_COW_TABLE_CHUNKS_PER_PE(vg, lv); \
	COW_table_chunks_per_PE = ( COW_table_entries_per_PE * sizeof(lv_COW_table_disk_t) / SECTOR_SIZE + lv->lv_chunk_size - 1) / lv->lv_chunk_size; \
	COW_table_entries_per_PE - COW_table_chunks_per_PE;})


/* to disk and to core data conversion macros */
#if __BYTE_ORDER == __BIG_ENDIAN

#define LVM_TO_CORE16(x) ( \
        ((uint16_t)((((uint16_t)(x) & 0x00FFU) << 8) | \
                    (((uint16_t)(x) & 0xFF00U) >> 8))))

#define LVM_TO_DISK16(x) LVM_TO_CORE16(x)

#define LVM_TO_CORE32(x) ( \
        ((uint32_t)((((uint32_t)(x) & 0x000000FFU) << 24) | \
                    (((uint32_t)(x) & 0x0000FF00U) << 8))) \
                    (((uint32_t)(x) & 0x00FF0000U) >> 8))) \
                    (((uint32_t)(x) & 0xFF000000U) >> 24))))

#define LVM_TO_DISK32(x) LVM_TO_CORE32(x)

#define LVM_TO_CORE64(x) \
        ((uint64_t)((((uint64_t)(x) & 0x00000000000000FFULL) << 56) | \
                    (((uint64_t)(x) & 0x000000000000FF00ULL) << 40) | \
                    (((uint64_t)(x) & 0x0000000000FF0000ULL) << 24) | \
                    (((uint64_t)(x) & 0x00000000FF000000ULL) <<  8) | \
                    (((uint64_t)(x) & 0x000000FF00000000ULL) >>  8) | \
                    (((uint64_t)(x) & 0x0000FF0000000000ULL) >> 24) | \
                    (((uint64_t)(x) & 0x00FF000000000000ULL) >> 40) | \
                    (((uint64_t)(x) & 0xFF00000000000000ULL) >> 56))) 

#define LVM_TO_DISK64(x) LVM_TO_CORE64(x)

#elif __BYTE_ORDER == __LITTLE_ENDIAN

#define LVM_TO_CORE16(x) x
#define LVM_TO_DISK16(x) x
#define LVM_TO_CORE32(x) x
#define LVM_TO_DISK32(x) x
#define LVM_TO_CORE64(x) x
#define LVM_TO_DISK64(x) x

#else

#error "__BYTE_ORDER must be defined as __LITTLE_ENDIAN or __BIG_ENDIAN"

#endif /* #if __BYTE_ORDER == __BIG_ENDIAN */


/*
 * ioctls
 */
/* volume group */
#define	VG_CREATE               _IOW ( 0xfe, 0x00, 1)
#define	VG_REMOVE               _IOW ( 0xfe, 0x01, 1)

#define	VG_EXTEND               _IOW ( 0xfe, 0x03, 1)
#define	VG_REDUCE               _IOW ( 0xfe, 0x04, 1)

#define	VG_STATUS               _IOWR ( 0xfe, 0x05, 1)
#define	VG_STATUS_GET_COUNT     _IOWR ( 0xfe, 0x06, 1)
#define	VG_STATUS_GET_NAMELIST  _IOWR ( 0xfe, 0x07, 1)

#define	VG_SET_EXTENDABLE       _IOW ( 0xfe, 0x08, 1)
#define	VG_RENAME		_IOW ( 0xfe, 0x09, 1)


/* logical volume */
#define	LV_CREATE               _IOW ( 0xfe, 0x20, 1)
#define	LV_REMOVE               _IOW ( 0xfe, 0x21, 1)

#define	LV_ACTIVATE             _IO ( 0xfe, 0x22)
#define	LV_DEACTIVATE           _IO ( 0xfe, 0x23)

#define	LV_EXTEND               _IOW ( 0xfe, 0x24, 1)
#define	LV_REDUCE               _IOW ( 0xfe, 0x25, 1)

#define	LV_STATUS_BYNAME        _IOWR ( 0xfe, 0x26, 1)
#define	LV_STATUS_BYINDEX       _IOWR ( 0xfe, 0x27, 1)

#define LV_SET_ACCESS           _IOW ( 0xfe, 0x28, 1)
#define LV_SET_ALLOCATION       _IOW ( 0xfe, 0x29, 1)
#define LV_SET_STATUS           _IOW ( 0xfe, 0x2a, 1)

#define LE_REMAP                _IOW ( 0xfe, 0x2b, 1)

#define LV_SNAPSHOT_USE_RATE    _IOWR ( 0xfe, 0x2c, 1)

#define	LV_STATUS_BYDEV		_IOWR ( 0xfe, 0x2e, 1)

#define	LV_RENAME		_IOW ( 0xfe, 0x2f, 1)

#define	LV_BMAP			_IOWR ( 0xfe, 0x30, 1)


/* physical volume */
#define	PV_STATUS               _IOWR ( 0xfe, 0x40, 1)
#define	PV_CHANGE               _IOWR ( 0xfe, 0x41, 1)
#define	PV_FLUSH                _IOW ( 0xfe, 0x42, 1)

/* physical extent */
#define	PE_LOCK_UNLOCK          _IOW ( 0xfe, 0x50, 1)

/* i/o protocol version */
#define	LVM_GET_IOP_VERSION     _IOR ( 0xfe, 0x98, 1)

#ifdef LVM_TOTAL_RESET
/* special reset function for testing purposes */
#define	LVM_RESET               _IO ( 0xfe, 0x99)
#endif

/* lock the logical volume manager */
#define	LVM_LOCK_LVM            _IO ( 0xfe, 0x100)
/* END ioctls */


/*
 * Status flags
 */
/* volume group */
#define	VG_ACTIVE            0x01	/* vg_status */
#define	VG_EXPORTED          0x02	/*     "     */
#define	VG_EXTENDABLE        0x04	/*     "     */

#define	VG_READ              0x01	/* vg_access */
#define	VG_WRITE             0x02	/*     "     */
#define	VG_CLUSTERED         0x04	/*     "     */
#define	VG_SHARED            0x08	/*     "     */

/* logical volume */
#define	LV_ACTIVE            0x01	/* lv_status */
#define	LV_SPINDOWN          0x02	/*     "     */

#define	LV_READ              0x01	/* lv_access */
#define	LV_WRITE             0x02	/*     "     */
#define	LV_SNAPSHOT          0x04	/*     "     */
#define	LV_SNAPSHOT_ORG      0x08	/*     "     */

#define	LV_BADBLOCK_ON       0x01	/* lv_badblock */

#define	LV_STRICT            0x01	/* lv_allocation */
#define	LV_CONTIGUOUS        0x02	/*       "       */

/* physical volume */
#define	PV_ACTIVE            0x01	/* pv_status */
#define	PV_ALLOCATABLE       0x02	/* pv_allocatable */


/*
 * Structure definitions core/disk follow
 *
 * conditional conversion takes place on big endian architectures
 * in functions * pv_copy_*(), vg_copy_*() and lv_copy_*()
 *
 */

#define	NAME_LEN		128	/* don't change!!! */
#define	UUID_LEN		32	/* don't change!!! */

/* copy on write tables in disk format */
typedef struct {
	uint64_t pv_org_number;
	uint64_t pv_org_rsector;
	uint64_t pv_snap_number;
	uint64_t pv_snap_rsector;
} lv_COW_table_disk_v1_t;

/* remap physical sector/rdev pairs including hash */
typedef struct {
	struct list_head hash;
	ulong rsector_org;
	kdev_t rdev_org;
	ulong rsector_new;
	kdev_t rdev_new;
} lv_block_exception_v1_t;

/* disk stored pe information */
typedef struct {
	uint16_t lv_num;
	uint16_t le_num;
} pe_disk_t;

/* disk stored PV, VG, LV and PE size and offset information */
typedef struct {
	uint32_t base;
	uint32_t size;
} lvm_disk_data_t;


/*
 * Structure Physical Volume (PV) Version 1
 */

/* core */
typedef struct {
	char id[2];		/* Identifier */
	unsigned short version;	/* HM lvm version */
	lvm_disk_data_t pv_on_disk;
	lvm_disk_data_t vg_on_disk;
	lvm_disk_data_t pv_namelist_on_disk;
	lvm_disk_data_t lv_on_disk;
	lvm_disk_data_t pe_on_disk;
	char pv_name[NAME_LEN];
	char vg_name[NAME_LEN];
	char system_id[NAME_LEN];	/* for vgexport/vgimport */
	kdev_t pv_dev;
	uint pv_number;
	uint pv_status;
	uint pv_allocatable;
	uint pv_size;		/* HM */
	uint lv_cur;
	uint pe_size;
	uint pe_total;
	uint pe_allocated;
	uint pe_stale;		/* for future use */
	pe_disk_t *pe;		/* HM */
	struct inode *inode;	/* HM */
} pv_v1_t;

/* core */
typedef struct {
	char id[2];		/* Identifier */
	unsigned short version;	/* HM lvm version */
	lvm_disk_data_t pv_on_disk;
	lvm_disk_data_t vg_on_disk;
	lvm_disk_data_t pv_uuidlist_on_disk;
	lvm_disk_data_t lv_on_disk;
	lvm_disk_data_t pe_on_disk;
	char pv_name[NAME_LEN];
	char vg_name[NAME_LEN];
	char system_id[NAME_LEN];	/* for vgexport/vgimport */
	kdev_t pv_dev;
	uint pv_number;
	uint pv_status;
	uint pv_allocatable;
	uint pv_size;		/* HM */
	uint lv_cur;
	uint pe_size;
	uint pe_total;
	uint pe_allocated;
	uint pe_stale;		/* for future use */
	pe_disk_t *pe;		/* HM */
	struct inode *inode;	/* HM */
	char pv_uuid[UUID_LEN+1];
} pv_v2_t;


/* disk */
typedef struct {
	uint8_t id[2];		/* Identifier */
	uint16_t version;		/* HM lvm version */
	lvm_disk_data_t pv_on_disk;
	lvm_disk_data_t vg_on_disk;
	lvm_disk_data_t pv_namelist_on_disk;
	lvm_disk_data_t lv_on_disk;
	lvm_disk_data_t pe_on_disk;
	uint8_t pv_name[NAME_LEN];
	uint8_t vg_name[NAME_LEN];
	uint8_t system_id[NAME_LEN];	/* for vgexport/vgimport */
	uint32_t pv_major;
	uint32_t pv_number;
	uint32_t pv_status;
	uint32_t pv_allocatable;
	uint32_t pv_size;		/* HM */
	uint32_t lv_cur;
	uint32_t pe_size;
	uint32_t pe_total;
	uint32_t pe_allocated;
} pv_disk_v1_t;

/* disk */
typedef struct {
	uint8_t id[2];		/* Identifier */
	uint16_t version;		/* HM lvm version */
	lvm_disk_data_t pv_on_disk;
	lvm_disk_data_t vg_on_disk;
	lvm_disk_data_t pv_uuidlist_on_disk;
	lvm_disk_data_t lv_on_disk;
	lvm_disk_data_t pe_on_disk;
	uint8_t pv_uuid[NAME_LEN];
	uint8_t vg_name[NAME_LEN];
	uint8_t system_id[NAME_LEN];	/* for vgexport/vgimport */
	uint32_t pv_major;
	uint32_t pv_number;
	uint32_t pv_status;
	uint32_t pv_allocatable;
	uint32_t pv_size;		/* HM */
	uint32_t lv_cur;
	uint32_t pe_size;
	uint32_t pe_total;
	uint32_t pe_allocated;
} pv_disk_v2_t;


/*
 * Structures for Logical Volume (LV)
 */

/* core PE information */
typedef struct {
	kdev_t dev;
	ulong pe;		/* to be changed if > 2TB */
	ulong reads;
	ulong writes;
} pe_t;

typedef struct {
	char lv_name[NAME_LEN];
	kdev_t old_dev;
	kdev_t new_dev;
	ulong old_pe;
	ulong new_pe;
} le_remap_req_t;

typedef struct lv_bmap {
	ulong lv_block;
	dev_t lv_dev;
} lv_bmap_t;

/*
 * Structure Logical Volume (LV) Version 3
 */

/* core */
typedef struct lv_v4 {
	char lv_name[NAME_LEN];
	char vg_name[NAME_LEN];
	uint lv_access;
	uint lv_status;
	uint lv_open;		/* HM */
	kdev_t lv_dev;		/* HM */
	uint lv_number;		/* HM */
	uint lv_mirror_copies;	/* for future use */
	uint lv_recovery;	/*       "        */
	uint lv_schedule;	/*       "        */
	uint lv_size;
	pe_t *lv_current_pe;	/* HM */
	uint lv_current_le;	/* for future use */
	uint lv_allocated_le;
	uint lv_stripes;
	uint lv_stripesize;
	uint lv_badblock;	/* for future use */
	uint lv_allocation;
	uint lv_io_timeout;	/* for future use */
	uint lv_read_ahead;

	/* delta to version 1 starts here */
	struct lv_v4 *lv_snapshot_org;
	struct lv_v4 *lv_snapshot_prev;
	struct lv_v4 *lv_snapshot_next;
	lv_block_exception_t *lv_block_exception;
	uint lv_remap_ptr;
	uint lv_remap_end;
	uint lv_chunk_size;
	uint lv_snapshot_minor;
#ifdef __KERNEL__
	struct kiobuf *lv_iobuf;
	struct semaphore lv_snapshot_sem;
	struct list_head *lv_snapshot_hash_table;
	ulong lv_snapshot_hash_table_size;
	ulong lv_snapshot_hash_mask;
	struct page *lv_COW_table_page;
	wait_queue_head_t lv_snapshot_wait;
	int	lv_snapshot_use_rate;
	void	*vg;
#else
	char dummy[200];
#endif
} lv_v4_t;

/* disk */
typedef struct {
	uint8_t lv_name[NAME_LEN];
	uint8_t vg_name[NAME_LEN];
	uint32_t lv_access;
	uint32_t lv_status;
	uint32_t lv_open;		/* HM */
	uint32_t lv_dev;		/* HM */
	uint32_t lv_number;	/* HM */
	uint32_t lv_mirror_copies;	/* for future use */
	uint32_t lv_recovery;	/*       "        */
	uint32_t lv_schedule;	/*       "        */
	uint32_t lv_size;
	uint32_t lv_snapshot_minor;/* minor number of original */
	uint16_t lv_chunk_size;	/* chunk size of snapshot */
	uint16_t dummy;
	uint32_t lv_allocated_le;
	uint32_t lv_stripes;
	uint32_t lv_stripesize;
	uint32_t lv_badblock;	/* for future use */
	uint32_t lv_allocation;
	uint32_t lv_io_timeout;	/* for future use */
	uint32_t lv_read_ahead;	/* HM */
} lv_disk_v3_t;

/*
 * Structure Volume Group (VG) Version 1
 */

/* core */
typedef struct {
	char vg_name[NAME_LEN];	/* volume group name */
	uint vg_number;		/* volume group number */
	uint vg_access;		/* read/write */
	uint vg_status;		/* active or not */
	uint lv_max;		/* maximum logical volumes */
	uint lv_cur;		/* current logical volumes */
	uint lv_open;		/* open    logical volumes */
	uint pv_max;		/* maximum physical volumes */
	uint pv_cur;		/* current physical volumes FU */
	uint pv_act;		/* active physical volumes */
	uint dummy;		/* was obsolete max_pe_per_pv */
	uint vgda;		/* volume group descriptor arrays FU */
	uint pe_size;		/* physical extent size in sectors */
	uint pe_total;		/* total of physical extents */
	uint pe_allocated;	/* allocated physical extents */
	uint pvg_total;		/* physical volume groups FU */
	struct proc_dir_entry *proc;
	pv_t *pv[ABS_MAX_PV + 1];	/* physical volume struct pointers */
	lv_t *lv[ABS_MAX_LV + 1];	/* logical  volume struct pointers */
} vg_v1_t;

typedef struct {
	char vg_name[NAME_LEN];	/* volume group name */
	uint vg_number;		/* volume group number */
	uint vg_access;		/* read/write */
	uint vg_status;		/* active or not */
	uint lv_max;		/* maximum logical volumes */
	uint lv_cur;		/* current logical volumes */
	uint lv_open;		/* open    logical volumes */
	uint pv_max;		/* maximum physical volumes */
	uint pv_cur;		/* current physical volumes FU */
	uint pv_act;		/* active physical volumes */
	uint dummy;		/* was obsolete max_pe_per_pv */
	uint vgda;		/* volume group descriptor arrays FU */
	uint pe_size;		/* physical extent size in sectors */
	uint pe_total;		/* total of physical extents */
	uint pe_allocated;	/* allocated physical extents */
	uint pvg_total;		/* physical volume groups FU */
	struct proc_dir_entry *proc;
	pv_t *pv[ABS_MAX_PV + 1];	/* physical volume struct pointers */
	lv_t *lv[ABS_MAX_LV + 1];	/* logical  volume struct pointers */
	char vg_uuid[UUID_LEN+1];	/* volume group UUID */
#ifdef __KERNEL__
	struct proc_dir_entry *vg_dir_pde;
	struct proc_dir_entry *lv_subdir_pde;
	struct proc_dir_entry *pv_subdir_pde;
#else
	char dummy1[200];
#endif
} vg_v3_t;


/* disk */
typedef struct {
	uint8_t vg_name[NAME_LEN];	/* volume group name */
	uint32_t vg_number;	/* volume group number */
	uint32_t vg_access;	/* read/write */
	uint32_t vg_status;	/* active or not */
	uint32_t lv_max;		/* maximum logical volumes */
	uint32_t lv_cur;		/* current logical volumes */
	uint32_t lv_open;		/* open    logical volumes */
	uint32_t pv_max;		/* maximum physical volumes */
	uint32_t pv_cur;		/* current physical volumes FU */
	uint32_t pv_act;		/* active physical volumes */
	uint32_t dummy;
	uint32_t vgda;		/* volume group descriptor arrays FU */
	uint32_t pe_size;		/* physical extent size in sectors */
	uint32_t pe_total;		/* total of physical extents */
	uint32_t pe_allocated;	/* allocated physical extents */
	uint32_t pvg_total;	/* physical volume groups FU */
} vg_disk_v1_t;

typedef struct {
	uint8_t vg_uuid[UUID_LEN];	/* volume group UUID */
	uint8_t vg_name_dummy[NAME_LEN-UUID_LEN];	/* rest of v1 VG name */
	uint32_t vg_number;	/* volume group number */
	uint32_t vg_access;	/* read/write */
	uint32_t vg_status;	/* active or not */
	uint32_t lv_max;		/* maximum logical volumes */
	uint32_t lv_cur;		/* current logical volumes */
	uint32_t lv_open;		/* open    logical volumes */
	uint32_t pv_max;		/* maximum physical volumes */
	uint32_t pv_cur;		/* current physical volumes FU */
	uint32_t pv_act;		/* active physical volumes */
	uint32_t dummy;
	uint32_t vgda;		/* volume group descriptor arrays FU */
	uint32_t pe_size;		/* physical extent size in sectors */
	uint32_t pe_total;		/* total of physical extents */
	uint32_t pe_allocated;	/* allocated physical extents */
	uint32_t pvg_total;	/* physical volume groups FU */
} vg_disk_v2_t;


/*
 * Request structures for ioctls
 */

/* Request structure PV_STATUS_BY_NAME... */
typedef struct {
	char pv_name[NAME_LEN];
	pv_t *pv;
} pv_status_req_t, pv_change_req_t;

/* Request structure PV_FLUSH */
typedef struct {
	char pv_name[NAME_LEN];
	kdev_t pv_dev;
} pv_flush_req_t;


/* Request structure PE_MOVE */
typedef struct {
	enum {
		LOCK_PE, UNLOCK_PE
	} lock;
	struct {
		kdev_t lv_dev;
		kdev_t pv_dev;
		ulong pv_offset;
	} data;
} pe_lock_req_t;


/* Request structure LV_STATUS_BYNAME */
typedef struct {
	char lv_name[NAME_LEN];
	lv_t *lv;
} lv_status_byname_req_t, lv_req_t;

/* Request structure LV_STATUS_BYINDEX */
typedef struct {
	ulong lv_index;
	lv_t *lv;
	/* Transfer size because user space and kernel space differ */
	ushort size;
} lv_status_byindex_req_t;

/* Request structure LV_STATUS_BYDEV... */
typedef struct {
	dev_t dev;
	pv_t *lv;
} lv_status_bydev_req_t;


/* Request structure LV_SNAPSHOT_USE_RATE */
typedef struct {
	int	block;
	int	rate;
} lv_snapshot_use_rate_req_t;

#endif				/* #ifndef _LVM_H_INCLUDE */
