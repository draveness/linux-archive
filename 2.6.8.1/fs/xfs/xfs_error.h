/*
 * Copyright (c) 2000-2002 Silicon Graphics, Inc.  All Rights Reserved.
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
#ifndef	__XFS_ERROR_H__
#define	__XFS_ERROR_H__

#define prdev(fmt,targ,args...) \
	printk("XFS: device %s- " fmt "\n", XFS_BUFTARG_NAME(targ), ## args)

#define XFS_ERECOVER	1	/* Failure to recover log */
#define XFS_ELOGSTAT	2	/* Failure to stat log in user space */
#define XFS_ENOLOGSPACE	3	/* Reservation too large */
#define XFS_ENOTSUP	4	/* Operation not supported */
#define	XFS_ENOLSN	5	/* Can't find the lsn you asked for */
#define XFS_ENOTFOUND	6
#define XFS_ENOTXFS	7	/* Not XFS filesystem */

#ifdef DEBUG
#define	XFS_ERROR_NTRAP	10
extern int	xfs_etrap[XFS_ERROR_NTRAP];
extern int	xfs_error_trap(int);
#define	XFS_ERROR(e)	xfs_error_trap(e)
#else
#define	XFS_ERROR(e)	(e)
#endif

struct xfs_mount;

extern void
xfs_error_report(
	char		*tag,
	int		level,
	struct xfs_mount *mp,
	char		*fname,
	int		linenum,
	inst_t		*ra);

extern void
xfs_corruption_error(
	char		*tag,
	int		level,
	struct xfs_mount *mp,
	void		*p,
	char		*fname,
	int		linenum,
	inst_t		*ra);

extern void
xfs_hex_dump(void *p, int length);

#define	XFS_ERROR_REPORT(e, lvl, mp)	\
	xfs_error_report(e, lvl, mp, __FILE__, __LINE__, __return_address)
#define	XFS_CORRUPTION_ERROR(e, lvl, mp, mem)	\
	xfs_corruption_error(e, lvl, mp, mem, \
			     __FILE__, __LINE__, __return_address)

#define XFS_ERRLEVEL_OFF	0
#define XFS_ERRLEVEL_LOW	1
#define XFS_ERRLEVEL_HIGH	5

/*
 * error injection tags - the labels can be anything you want
 * but each tag should have its own unique number
 */

#define XFS_ERRTAG_NOERROR				0
#define XFS_ERRTAG_IFLUSH_1				1
#define XFS_ERRTAG_IFLUSH_2				2
#define XFS_ERRTAG_IFLUSH_3				3
#define XFS_ERRTAG_IFLUSH_4				4
#define XFS_ERRTAG_IFLUSH_5				5
#define XFS_ERRTAG_IFLUSH_6				6
#define	XFS_ERRTAG_DA_READ_BUF				7
#define	XFS_ERRTAG_BTREE_CHECK_LBLOCK			8
#define	XFS_ERRTAG_BTREE_CHECK_SBLOCK			9
#define	XFS_ERRTAG_ALLOC_READ_AGF			10
#define	XFS_ERRTAG_IALLOC_READ_AGI			11
#define	XFS_ERRTAG_ITOBP_INOTOBP			12
#define	XFS_ERRTAG_IUNLINK				13
#define	XFS_ERRTAG_IUNLINK_REMOVE			14
#define	XFS_ERRTAG_DIR_INO_VALIDATE			15
#define XFS_ERRTAG_BULKSTAT_READ_CHUNK			16
#define XFS_ERRTAG_IODONE_IOERR				17
#define XFS_ERRTAG_STRATREAD_IOERR			18
#define XFS_ERRTAG_STRATCMPL_IOERR			19
#define XFS_ERRTAG_DIOWRITE_IOERR			20
#define XFS_ERRTAG_BMAPIFORMAT				21
#define XFS_ERRTAG_MAX					22

/*
 * Random factors for above tags, 1 means always, 2 means 1/2 time, etc.
 */
#define XFS_RANDOM_DEFAULT				100
#define XFS_RANDOM_IFLUSH_1				XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IFLUSH_2				XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IFLUSH_3				XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IFLUSH_4				XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IFLUSH_5				XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IFLUSH_6				XFS_RANDOM_DEFAULT
#define XFS_RANDOM_DA_READ_BUF				XFS_RANDOM_DEFAULT
#define XFS_RANDOM_BTREE_CHECK_LBLOCK			(XFS_RANDOM_DEFAULT/4)
#define XFS_RANDOM_BTREE_CHECK_SBLOCK			XFS_RANDOM_DEFAULT
#define XFS_RANDOM_ALLOC_READ_AGF			XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IALLOC_READ_AGI			XFS_RANDOM_DEFAULT
#define XFS_RANDOM_ITOBP_INOTOBP			XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IUNLINK				XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IUNLINK_REMOVE			XFS_RANDOM_DEFAULT
#define XFS_RANDOM_DIR_INO_VALIDATE			XFS_RANDOM_DEFAULT
#define XFS_RANDOM_BULKSTAT_READ_CHUNK			XFS_RANDOM_DEFAULT
#define XFS_RANDOM_IODONE_IOERR				(XFS_RANDOM_DEFAULT/10)
#define XFS_RANDOM_STRATREAD_IOERR			(XFS_RANDOM_DEFAULT/10)
#define XFS_RANDOM_STRATCMPL_IOERR			(XFS_RANDOM_DEFAULT/10)
#define XFS_RANDOM_DIOWRITE_IOERR			(XFS_RANDOM_DEFAULT/10)
#define	XFS_RANDOM_BMAPIFORMAT				XFS_RANDOM_DEFAULT

#if (defined(DEBUG) || defined(INDUCE_IO_ERROR))
extern int	xfs_error_test(int, int *, char *, int, char *, unsigned long);
void xfs_error_test_init(void);

#define	XFS_NUM_INJECT_ERROR				10

#ifdef __ANSI_CPP__
#define XFS_TEST_ERROR(expr, mp, tag, rf)		\
	((expr) || \
	 xfs_error_test((tag), (mp)->m_fixedfsid, #expr, __LINE__, __FILE__, \
			 (rf)))
#else
#define XFS_TEST_ERROR(expr, mp, tag, rf)		\
	((expr) || \
	 xfs_error_test((tag), (mp)->m_fixedfsid, "expr", __LINE__, __FILE__, \
			(rf)))
#endif /* __ANSI_CPP__ */

int		xfs_errortag_add(int error_tag, xfs_mount_t *mp);
int		xfs_errortag_clear(int error_tag, xfs_mount_t *mp);

int		xfs_errortag_clearall(xfs_mount_t *mp);
int		xfs_errortag_clearall_umount(int64_t fsid, char *fsname,
						int loud);
#else
#define XFS_TEST_ERROR(expr, mp, tag, rf)	(expr)
#define xfs_errortag_add(tag, mp)		(ENOSYS)
#define xfs_errortag_clearall(mp)		(ENOSYS)
#endif /* (DEBUG || INDUCE_IO_ERROR) */

/*
 * XFS panic tags -- allow a call to xfs_cmn_err() be turned into
 *			a panic by setting xfs_panic_mask in a
 *			sysctl.  update xfs_max[XFS_PARAM] if
 *			more are added.
 */
#define		XFS_NO_PTAG			0
#define		XFS_PTAG_IFLUSH			0x00000001
#define		XFS_PTAG_LOGRES			0x00000002
#define		XFS_PTAG_AILDELETE		0x00000004
#define		XFS_PTAG_ERROR_REPORT		0x00000008
#define		XFS_PTAG_SHUTDOWN_CORRUPT	0x00000010
#define		XFS_PTAG_SHUTDOWN_IOERROR	0x00000020
#define		XFS_PTAG_SHUTDOWN_LOGERROR	0x00000040

struct xfs_mount;
/* PRINTFLIKE4 */
void		xfs_cmn_err(int panic_tag, int level, struct xfs_mount *mp,
			    char *fmt, ...);
/* PRINTFLIKE3 */
void		xfs_fs_cmn_err(int level, struct xfs_mount *mp, char *fmt, ...);

#endif	/* __XFS_ERROR_H__ */
