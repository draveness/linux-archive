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
#ifndef __XFS_QUOTA_H__
#define __XFS_QUOTA_H__

/*
 * The ondisk form of a dquot structure.
 */
#define XFS_DQUOT_MAGIC		0x4451		/* 'DQ' */
#define XFS_DQUOT_VERSION	(u_int8_t)0x01	/* latest version number */

/*
 * uid_t and gid_t are hard-coded to 32 bits in the inode.
 * Hence, an 'id' in a dquot is 32 bits..
 */
typedef __int32_t	xfs_dqid_t;

/*
 * Eventhough users may not have quota limits occupying all 64-bits,
 * they may need 64-bit accounting. Hence, 64-bit quota-counters,
 * and quota-limits. This is a waste in the common case, but hey ...
 */
typedef __uint64_t	xfs_qcnt_t;
typedef __uint16_t	xfs_qwarncnt_t;

/*
 * This is the main portion of the on-disk representation of quota
 * information for a user. This is the q_core of the xfs_dquot_t that
 * is kept in kernel memory. We pad this with some more expansion room
 * to construct the on disk structure.
 */
typedef struct	xfs_disk_dquot {
/*16*/	u_int16_t	d_magic;	/* dquot magic = XFS_DQUOT_MAGIC */
/*8 */	u_int8_t	d_version;	/* dquot version */
/*8 */	u_int8_t	d_flags;	/* XFS_DQ_USER/PROJ/GROUP */
/*32*/	xfs_dqid_t	d_id;		/* user,project,group id */
/*64*/	xfs_qcnt_t	d_blk_hardlimit;/* absolute limit on disk blks */
/*64*/	xfs_qcnt_t	d_blk_softlimit;/* preferred limit on disk blks */
/*64*/	xfs_qcnt_t	d_ino_hardlimit;/* maximum # allocated inodes */
/*64*/	xfs_qcnt_t	d_ino_softlimit;/* preferred inode limit */
/*64*/	xfs_qcnt_t	d_bcount;	/* disk blocks owned by the user */
/*64*/	xfs_qcnt_t	d_icount;	/* inodes owned by the user */
/*32*/	__int32_t	d_itimer;	/* zero if within inode limits if not,
					   this is when we refuse service */
/*32*/	__int32_t	d_btimer;	/* similar to above; for disk blocks */
/*16*/	xfs_qwarncnt_t	d_iwarns;	/* warnings issued wrt num inodes */
/*16*/	xfs_qwarncnt_t	d_bwarns;	/* warnings issued wrt disk blocks */
/*32*/	__int32_t	d_pad0;		/* 64 bit align */
/*64*/	xfs_qcnt_t	d_rtb_hardlimit;/* absolute limit on realtime blks */
/*64*/	xfs_qcnt_t	d_rtb_softlimit;/* preferred limit on RT disk blks */
/*64*/	xfs_qcnt_t	d_rtbcount;	/* realtime blocks owned */
/*32*/	__int32_t	d_rtbtimer;	/* similar to above; for RT disk blocks */
/*16*/	xfs_qwarncnt_t	d_rtbwarns;	/* warnings issued wrt RT disk blocks */
/*16*/	__uint16_t	d_pad;
} xfs_disk_dquot_t;

/*
 * This is what goes on disk. This is separated from the xfs_disk_dquot because
 * carrying the unnecessary padding would be a waste of memory.
 */
typedef struct xfs_dqblk {
	xfs_disk_dquot_t  dd_diskdq;	/* portion that lives incore as well */
	char		  dd_fill[32];	/* filling for posterity */
} xfs_dqblk_t;

/*
 * flags for q_flags field in the dquot.
 */
#define XFS_DQ_USER		0x0001		/* a user quota */
/* #define XFS_DQ_PROJ		0x0002		-- project quota (IRIX) */
#define XFS_DQ_GROUP		0x0004		/* a group quota */
#define XFS_DQ_FLOCKED		0x0008		/* flush lock taken */
#define XFS_DQ_DIRTY		0x0010		/* dquot is dirty */
#define XFS_DQ_WANT		0x0020		/* for lookup/reclaim race */
#define XFS_DQ_INACTIVE		0x0040		/* dq off mplist & hashlist */
#define XFS_DQ_MARKER		0x0080		/* sentinel */

/*
 * In the worst case, when both user and group quotas are on,
 * we can have a max of three dquots changing in a single transaction.
 */
#define XFS_DQUOT_LOGRES(mp)	(sizeof(xfs_disk_dquot_t) * 3)


/*
 * These are the structures used to lay out dquots and quotaoff
 * records on the log. Quite similar to those of inodes.
 */

/*
 * log format struct for dquots.
 * The first two fields must be the type and size fitting into
 * 32 bits : log_recovery code assumes that.
 */
typedef struct xfs_dq_logformat {
	__uint16_t		qlf_type;      /* dquot log item type */
	__uint16_t		qlf_size;      /* size of this item */
	xfs_dqid_t		qlf_id;	       /* usr/grp id number : 32 bits */
	__int64_t		qlf_blkno;     /* blkno of dquot buffer */
	__int32_t		qlf_len;       /* len of dquot buffer */
	__uint32_t		qlf_boffset;   /* off of dquot in buffer */
} xfs_dq_logformat_t;

/*
 * log format struct for QUOTAOFF records.
 * The first two fields must be the type and size fitting into
 * 32 bits : log_recovery code assumes that.
 * We write two LI_QUOTAOFF logitems per quotaoff, the last one keeps a pointer
 * to the first and ensures that the first logitem is taken out of the AIL
 * only when the last one is securely committed.
 */
typedef struct xfs_qoff_logformat {
	unsigned short		qf_type;	/* quotaoff log item type */
	unsigned short		qf_size;	/* size of this item */
	unsigned int		qf_flags;	/* USR and/or GRP */
	char			qf_pad[12];	/* padding for future */
} xfs_qoff_logformat_t;


/*
 * Disk quotas status in m_qflags, and also sb_qflags. 16 bits.
 */
#define XFS_UQUOTA_ACCT	0x0001  /* user quota accounting ON */
#define XFS_UQUOTA_ENFD	0x0002  /* user quota limits enforced */
#define XFS_UQUOTA_CHKD	0x0004  /* quotacheck run on usr quotas */
#define XFS_PQUOTA_ACCT	0x0008  /* (IRIX) project quota accounting ON */
#define XFS_GQUOTA_ENFD	0x0010  /* group quota limits enforced */
#define XFS_GQUOTA_CHKD	0x0020  /* quotacheck run on grp quotas */
#define XFS_GQUOTA_ACCT	0x0040  /* group quota accounting ON */

/*
 * Incore only flags for quotaoff - these bits get cleared when quota(s)
 * are in the process of getting turned off. These flags are in m_qflags but
 * never in sb_qflags.
 */
#define XFS_UQUOTA_ACTIVE	0x0080  /* uquotas are being turned off */
#define XFS_GQUOTA_ACTIVE	0x0100  /* gquotas are being turned off */

/*
 * Checking XFS_IS_*QUOTA_ON() while holding any inode lock guarantees
 * quota will be not be switched off as long as that inode lock is held.
 */
#define XFS_IS_QUOTA_ON(mp)	((mp)->m_qflags & (XFS_UQUOTA_ACTIVE | \
						   XFS_GQUOTA_ACTIVE))
#define XFS_IS_UQUOTA_ON(mp)	((mp)->m_qflags & XFS_UQUOTA_ACTIVE)
#define XFS_IS_GQUOTA_ON(mp)	((mp)->m_qflags & XFS_GQUOTA_ACTIVE)

/*
 * Flags to tell various functions what to do. Not all of these are meaningful
 * to a single function. None of these XFS_QMOPT_* flags are meant to have
 * persistent values (ie. their values can and will change between versions)
 */
#define XFS_QMOPT_DQLOCK	0x0000001 /* dqlock */
#define XFS_QMOPT_DQALLOC	0x0000002 /* alloc dquot ondisk if needed */
#define XFS_QMOPT_UQUOTA	0x0000004 /* user dquot requested */
#define XFS_QMOPT_GQUOTA	0x0000008 /* group dquot requested */
#define XFS_QMOPT_FORCE_RES	0x0000010 /* ignore quota limits */
#define XFS_QMOPT_DQSUSER	0x0000020 /* don't cache super users dquot */
#define XFS_QMOPT_SBVERSION	0x0000040 /* change superblock version num */
#define XFS_QMOPT_QUOTAOFF	0x0000080 /* quotas are being turned off */
#define XFS_QMOPT_UMOUNTING	0x0000100 /* filesys is being unmounted */
#define XFS_QMOPT_DOLOG		0x0000200 /* log buf changes (in quotacheck) */
#define XFS_QMOPT_DOWARN        0x0000400 /* increase warning cnt if necessary */
#define XFS_QMOPT_ILOCKED	0x0000800 /* inode is already locked (excl) */
#define XFS_QMOPT_DQREPAIR	0x0001000 /* repair dquot, if damaged. */

/*
 * flags to xfs_trans_mod_dquot to indicate which field needs to be
 * modified.
 */
#define XFS_QMOPT_RES_REGBLKS	0x0010000
#define XFS_QMOPT_RES_RTBLKS	0x0020000
#define XFS_QMOPT_BCOUNT	0x0040000
#define XFS_QMOPT_ICOUNT	0x0080000
#define XFS_QMOPT_RTBCOUNT	0x0100000
#define XFS_QMOPT_DELBCOUNT	0x0200000
#define XFS_QMOPT_DELRTBCOUNT	0x0400000
#define XFS_QMOPT_RES_INOS	0x0800000

/*
 * flags for dqflush and dqflush_all.
 */
#define XFS_QMOPT_SYNC		0x1000000
#define XFS_QMOPT_ASYNC		0x2000000
#define XFS_QMOPT_DELWRI	0x4000000

/*
 * flags for dqalloc.
 */
#define XFS_QMOPT_INHERIT	0x8000000

/*
 * flags to xfs_trans_mod_dquot.
 */
#define XFS_TRANS_DQ_RES_BLKS	XFS_QMOPT_RES_REGBLKS
#define XFS_TRANS_DQ_RES_RTBLKS	XFS_QMOPT_RES_RTBLKS
#define XFS_TRANS_DQ_RES_INOS	XFS_QMOPT_RES_INOS
#define XFS_TRANS_DQ_BCOUNT	XFS_QMOPT_BCOUNT
#define XFS_TRANS_DQ_DELBCOUNT	XFS_QMOPT_DELBCOUNT
#define XFS_TRANS_DQ_ICOUNT	XFS_QMOPT_ICOUNT
#define XFS_TRANS_DQ_RTBCOUNT	XFS_QMOPT_RTBCOUNT
#define XFS_TRANS_DQ_DELRTBCOUNT XFS_QMOPT_DELRTBCOUNT


#define XFS_QMOPT_QUOTALL	(XFS_QMOPT_UQUOTA|XFS_QMOPT_GQUOTA)
#define XFS_QMOPT_RESBLK_MASK	(XFS_QMOPT_RES_REGBLKS | XFS_QMOPT_RES_RTBLKS)

#ifdef __KERNEL__
/*
 * This check is done typically without holding the inode lock;
 * that may seem racey, but it is harmless in the context that it is used.
 * The inode cannot go inactive as long a reference is kept, and
 * therefore if dquot(s) were attached, they'll stay consistent.
 * If, for example, the ownership of the inode changes while
 * we didn't have the inode locked, the appropriate dquot(s) will be
 * attached atomically.
 */
#define XFS_NOT_DQATTACHED(mp, ip) ((XFS_IS_UQUOTA_ON(mp) &&\
				     (ip)->i_udquot == NULL) || \
				    (XFS_IS_GQUOTA_ON(mp) && \
				     (ip)->i_gdquot == NULL))

#define XFS_QM_NEED_QUOTACHECK(mp) ((XFS_IS_UQUOTA_ON(mp) && \
				     (mp->m_sb.sb_qflags & \
				      XFS_UQUOTA_CHKD) == 0) || \
				    (XFS_IS_GQUOTA_ON(mp) && \
				     (mp->m_sb.sb_qflags & \
				      XFS_GQUOTA_CHKD) == 0))

#define XFS_MOUNT_QUOTA_ALL	(XFS_UQUOTA_ACCT|XFS_UQUOTA_ENFD|\
				 XFS_UQUOTA_CHKD|XFS_GQUOTA_ACCT|\
				 XFS_GQUOTA_ENFD|XFS_GQUOTA_CHKD)
#define XFS_MOUNT_QUOTA_MASK	(XFS_MOUNT_QUOTA_ALL | XFS_UQUOTA_ACTIVE | \
				 XFS_GQUOTA_ACTIVE)


/*
 * The structure kept inside the xfs_trans_t keep track of dquot changes
 * within a transaction and apply them later.
 */
typedef struct xfs_dqtrx {
	struct xfs_dquot *qt_dquot;	  /* the dquot this refers to */
	ulong		qt_blk_res;	  /* blks reserved on a dquot */
	ulong		qt_blk_res_used;  /* blks used from the reservation */
	ulong		qt_ino_res;	  /* inode reserved on a dquot */
	ulong		qt_ino_res_used;  /* inodes used from the reservation */
	long		qt_bcount_delta;  /* dquot blk count changes */
	long		qt_delbcnt_delta; /* delayed dquot blk count changes */
	long		qt_icount_delta;  /* dquot inode count changes */
	ulong		qt_rtblk_res;	  /* # blks reserved on a dquot */
	ulong		qt_rtblk_res_used;/* # blks used from reservation */
	long		qt_rtbcount_delta;/* dquot realtime blk changes */
	long		qt_delrtb_delta;  /* delayed RT blk count changes */
} xfs_dqtrx_t;

/*
 * Dquot transaction functions, used if quota is enabled.
 */
typedef void	(*qo_dup_dqinfo_t)(struct xfs_trans *, struct xfs_trans *);
typedef void	(*qo_mod_dquot_byino_t)(struct xfs_trans *,
				struct xfs_inode *, uint, long);
typedef void	(*qo_free_dqinfo_t)(struct xfs_trans *);
typedef void	(*qo_apply_dquot_deltas_t)(struct xfs_trans *);
typedef void	(*qo_unreserve_and_mod_dquots_t)(struct xfs_trans *);
typedef int	(*qo_reserve_quota_nblks_t)(
				struct xfs_trans *, struct xfs_mount *,
				struct xfs_inode *, long, long, uint);
typedef int	(*qo_reserve_quota_bydquots_t)(
				struct xfs_trans *, struct xfs_mount *,
				struct xfs_dquot *, struct xfs_dquot *,
				long, long, uint);
typedef struct xfs_dqtrxops {
	qo_dup_dqinfo_t			qo_dup_dqinfo;
	qo_free_dqinfo_t		qo_free_dqinfo;
	qo_mod_dquot_byino_t		qo_mod_dquot_byino;
	qo_apply_dquot_deltas_t		qo_apply_dquot_deltas;
	qo_reserve_quota_nblks_t	qo_reserve_quota_nblks;
	qo_reserve_quota_bydquots_t	qo_reserve_quota_bydquots;
	qo_unreserve_and_mod_dquots_t	qo_unreserve_and_mod_dquots;
} xfs_dqtrxops_t;

#define XFS_DQTRXOP(mp, tp, op, args...) \
			((mp)->m_qm_ops.xfs_dqtrxops ? \
			((mp)->m_qm_ops.xfs_dqtrxops->op)(tp, ## args) : 0)

#define XFS_TRANS_DUP_DQINFO(mp, otp, ntp) \
	XFS_DQTRXOP(mp, otp, qo_dup_dqinfo, ntp)
#define XFS_TRANS_FREE_DQINFO(mp, tp) \
	XFS_DQTRXOP(mp, tp, qo_free_dqinfo)
#define XFS_TRANS_MOD_DQUOT_BYINO(mp, tp, ip, field, delta) \
	XFS_DQTRXOP(mp, tp, qo_mod_dquot_byino, ip, field, delta)
#define XFS_TRANS_APPLY_DQUOT_DELTAS(mp, tp) \
	XFS_DQTRXOP(mp, tp, qo_apply_dquot_deltas)
#define XFS_TRANS_RESERVE_QUOTA_NBLKS(mp, tp, ip, nblks, ninos, fl) \
	XFS_DQTRXOP(mp, tp, qo_reserve_quota_nblks, mp, ip, nblks, ninos, fl)
#define XFS_TRANS_RESERVE_QUOTA_BYDQUOTS(mp, tp, ud, gd, nb, ni, fl) \
	XFS_DQTRXOP(mp, tp, qo_reserve_quota_bydquots, mp, ud, gd, nb, ni, fl)
#define XFS_TRANS_UNRESERVE_AND_MOD_DQUOTS(mp, tp) \
	XFS_DQTRXOP(mp, tp, qo_unreserve_and_mod_dquots)

#define XFS_TRANS_RESERVE_BLKQUOTA(mp, tp, ip, nblks) \
	XFS_TRANS_RESERVE_QUOTA_NBLKS(mp, tp, ip, nblks, 0, \
				XFS_QMOPT_RES_REGBLKS)
#define XFS_TRANS_RESERVE_BLKQUOTA_FORCE(mp, tp, ip, nblks) \
	XFS_TRANS_RESERVE_QUOTA_NBLKS(mp, tp, ip, nblks, 0, \
				XFS_QMOPT_RES_REGBLKS | XFS_QMOPT_FORCE_RES)
#define XFS_TRANS_UNRESERVE_BLKQUOTA(mp, tp, ip, nblks) \
	XFS_TRANS_RESERVE_QUOTA_NBLKS(mp, tp, ip, -(nblks), 0, \
				XFS_QMOPT_RES_REGBLKS)
#define XFS_TRANS_RESERVE_QUOTA(mp, tp, ud, gd, nb, ni, f) \
	XFS_TRANS_RESERVE_QUOTA_BYDQUOTS(mp, tp, ud, gd, nb, ni, \
				f | XFS_QMOPT_RES_REGBLKS)
#define XFS_TRANS_UNRESERVE_QUOTA(mp, tp, ud, gd, nb, ni, f) \
	XFS_TRANS_RESERVE_QUOTA_BYDQUOTS(mp, tp, ud, gd, -(nb), -(ni), \
				f | XFS_QMOPT_RES_REGBLKS)

extern int xfs_qm_dqcheck(xfs_disk_dquot_t *, xfs_dqid_t, uint, uint, char *);

extern struct bhv_vfsops xfs_qmops;

#endif	/* __KERNEL__ */

#endif	/* __XFS_QUOTA_H__ */
