/*
 * Copyright (c) 2000-2003 Silicon Graphics, Inc.  All Rights Reserved.
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
#ifndef	__XFS_LOG_H__
#define __XFS_LOG_H__

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define LSN_FIELD_CYCLE(arch) (((arch)==ARCH_NOCONVERT)?1:0)
#define LSN_FIELD_BLOCK(arch) (((arch)==ARCH_NOCONVERT)?0:1)
#else
#define LSN_FIELD_CYCLE(arch) (0)
#define LSN_FIELD_BLOCK(arch) (1)
#endif

/* get lsn fields */

#define CYCLE_LSN(lsn,arch) (INT_GET(((uint *)&(lsn))[LSN_FIELD_CYCLE(arch)], arch))
#define BLOCK_LSN(lsn,arch) (INT_GET(((uint *)&(lsn))[LSN_FIELD_BLOCK(arch)], arch))
/* this is used in a spot where we might otherwise double-endian-flip */
#define CYCLE_LSN_NOCONV(lsn,arch) (((uint *)&(lsn))[LSN_FIELD_CYCLE(arch)])

#ifdef __KERNEL__
/*
 * By comparing each compnent, we don't have to worry about extra
 * endian issues in treating two 32 bit numbers as one 64 bit number
 */
static
#if defined(__GNUC__) && (__GNUC__ == 2) && ( (__GNUC_MINOR__ == 95) || (__GNUC_MINOR__ == 96))
__attribute__((unused))	/* gcc 2.95, 2.96 miscompile this when inlined */
#else
__inline__
#endif
xfs_lsn_t	_lsn_cmp(xfs_lsn_t lsn1, xfs_lsn_t lsn2, xfs_arch_t arch)
{
	if (CYCLE_LSN(lsn1, arch) != CYCLE_LSN(lsn2, arch))
		return (CYCLE_LSN(lsn1, arch)<CYCLE_LSN(lsn2, arch))? -999 : 999;

	if (BLOCK_LSN(lsn1, arch) != BLOCK_LSN(lsn2, arch))
		return (BLOCK_LSN(lsn1, arch)<BLOCK_LSN(lsn2, arch))? -999 : 999;

	return 0;
}

#define	XFS_LSN_CMP_ARCH(x,y,arch)	_lsn_cmp(x, y, arch)
#define	XFS_LSN_CMP(x,y) XFS_LSN_CMP_ARCH(x,y,ARCH_NOCONVERT)
#define	XFS_LSN_DIFF_ARCH(x,y,arch)	_lsn_cmp(x, y, arch)
#define	XFS_LSN_DIFF(x,y) XFS_LSN_DIFF_ARCH(x,y,ARCH_NOCONVERT)

/*
 * Macros, structures, prototypes for interface to the log manager.
 */

/*
 * Flags to xfs_log_mount
 */
#define XFS_LOG_RECOVER		0x1

/*
 * Flags to xfs_log_done()
 */
#define XFS_LOG_REL_PERM_RESERV	0x1


/*
 * Flags to xfs_log_reserve()
 *
 *	XFS_LOG_SLEEP:	 If space is not available, sleep (default)
 *	XFS_LOG_NOSLEEP: If space is not available, return error
 *	XFS_LOG_PERM_RESERV: Permanent reservation.  When writes are
 *		performed against this type of reservation, the reservation
 *		is not decreased.  Long running transactions should use this.
 */
#define XFS_LOG_SLEEP		0x0
#define XFS_LOG_NOSLEEP		0x1
#define XFS_LOG_PERM_RESERV	0x2
#define XFS_LOG_RESV_ALL	(XFS_LOG_NOSLEEP|XFS_LOG_PERM_RESERV)


/*
 * Flags to xfs_log_force()
 *
 *	XFS_LOG_SYNC:	Synchronous force in-core log to disk
 *	XFS_LOG_FORCE:	Start in-core log write now.
 *	XFS_LOG_URGE:	Start write within some window of time.
 *
 * Note: Either XFS_LOG_FORCE or XFS_LOG_URGE must be set.
 */
#define XFS_LOG_SYNC		0x1
#define XFS_LOG_FORCE		0x2
#define XFS_LOG_URGE		0x4

#endif	/* __KERNEL__ */


/* Log Clients */
#define XFS_TRANSACTION		0x69
#define XFS_VOLUME		0x2
#define XFS_LOG			0xaa

typedef struct xfs_log_iovec {
	xfs_caddr_t		i_addr;		/* beginning address of region */
	int		i_len;		/* length in bytes of region */
} xfs_log_iovec_t;

typedef void* xfs_log_ticket_t;

/*
 * Structure used to pass callback function and the function's argument
 * to the log manager.
 */
typedef struct xfs_log_callback {
	struct xfs_log_callback	*cb_next;
	void			(*cb_func)(void *, int);
	void			*cb_arg;
} xfs_log_callback_t;


#ifdef __KERNEL__
/* Log manager interfaces */
struct xfs_mount;
xfs_lsn_t xfs_log_done(struct xfs_mount *mp,
		       xfs_log_ticket_t ticket,
		       void		**iclog,
		       uint		flags);
int	  xfs_log_force(struct xfs_mount *mp,
			xfs_lsn_t	 lsn,
			uint		 flags);
int	  xfs_log_mount(struct xfs_mount	*mp,
			struct xfs_buftarg	*log_target,
			xfs_daddr_t		start_block,
			int		 	num_bblocks);
int	  xfs_log_mount_finish(struct xfs_mount *mp, int);
void	  xfs_log_move_tail(struct xfs_mount	*mp,
			    xfs_lsn_t		tail_lsn);
int	  xfs_log_notify(struct xfs_mount	*mp,
			 void			*iclog,
			 xfs_log_callback_t	*callback_entry);
int	  xfs_log_release_iclog(struct xfs_mount *mp,
			 void			 *iclog_hndl);
int	  xfs_log_reserve(struct xfs_mount *mp,
			  int		   length,
			  int		   count,
			  xfs_log_ticket_t *ticket,
			  __uint8_t	   clientid,
			  uint		   flags);
int	  xfs_log_write(struct xfs_mount *mp,
			xfs_log_iovec_t  region[],
			int		 nentries,
			xfs_log_ticket_t ticket,
			xfs_lsn_t	 *start_lsn);
int	  xfs_log_unmount(struct xfs_mount *mp);
int	  xfs_log_unmount_write(struct xfs_mount *mp);
void      xfs_log_unmount_dealloc(struct xfs_mount *mp);
int	  xfs_log_force_umount(struct xfs_mount *mp, int logerror);
int	  xfs_log_need_covered(struct xfs_mount *mp);

void	  xlog_iodone(struct xfs_buf *);

#endif


extern int xlog_debug;		/* set to 1 to enable real log */


#endif	/* __XFS_LOG_H__ */
