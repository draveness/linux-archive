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

#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_macros.h"
#include "xfs_inum.h"
#include "xfs_log.h"
#include "xfs_clnt.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_ag.h"
#include "xfs_dir.h"
#include "xfs_dir2.h"
#include "xfs_imap.h"
#include "xfs_alloc.h"
#include "xfs_dmapi.h"
#include "xfs_mount.h"
#include "xfs_quota.h"

int
vfs_mount(
	struct bhv_desc		*bdp,
	struct xfs_mount_args	*args,
	struct cred		*cr)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_mount)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_mount)(next, args, cr));
}

int
vfs_parseargs(
	struct bhv_desc		*bdp,
	char			*s,
	struct xfs_mount_args	*args,
	int			f)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_parseargs)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_parseargs)(next, s, args, f));
}

int
vfs_showargs(
	struct bhv_desc		*bdp,
	struct seq_file		*m)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_showargs)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_showargs)(next, m));
}

int
vfs_unmount(
	struct bhv_desc		*bdp,
	int			fl,
	struct cred		*cr)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_unmount)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_unmount)(next, fl, cr));
}

int
vfs_mntupdate(
	struct bhv_desc		*bdp,
	int			*fl,
	struct xfs_mount_args	*args)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_mntupdate)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_mntupdate)(next, fl, args));
}

int
vfs_root(
	struct bhv_desc		*bdp,
	struct vnode		**vpp)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_root)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_root)(next, vpp));
}

int
vfs_statvfs(
	struct bhv_desc		*bdp,
	xfs_statfs_t		*sp,
	struct vnode		*vp)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_statvfs)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_statvfs)(next, sp, vp));
}

int
vfs_sync(
	struct bhv_desc		*bdp,
	int			fl,
	struct cred		*cr)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_sync)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_sync)(next, fl, cr));
}

int
vfs_vget(
	struct bhv_desc		*bdp,
	struct vnode		**vpp,
	struct fid		*fidp)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_vget)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_vget)(next, vpp, fidp));
}

int
vfs_dmapiops(
	struct bhv_desc		*bdp,
	caddr_t			addr)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_dmapiops)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_dmapiops)(next, addr));
}

int
vfs_quotactl(
	struct bhv_desc		*bdp,
	int			cmd,
	int			id,
	caddr_t			addr)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_quotactl)
		next = BHV_NEXT(next);
	return ((*bhvtovfsops(next)->vfs_quotactl)(next, cmd, id, addr));
}

void
vfs_init_vnode(
	struct bhv_desc		*bdp,
	struct vnode		*vp,
	struct bhv_desc		*bp,
	int			unlock)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_init_vnode)
		next = BHV_NEXT(next);
	((*bhvtovfsops(next)->vfs_init_vnode)(next, vp, bp, unlock));
}

void
vfs_force_shutdown(
	struct bhv_desc		*bdp,
	int			fl,
	char			*file,
	int			line)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_force_shutdown)
		next = BHV_NEXT(next);
	((*bhvtovfsops(next)->vfs_force_shutdown)(next, fl, file, line));
}

void
vfs_freeze(
	struct bhv_desc		*bdp)
{
	struct bhv_desc		*next = bdp;

	ASSERT(next);
	while (! (bhvtovfsops(next))->vfs_freeze)
		next = BHV_NEXT(next);
	((*bhvtovfsops(next)->vfs_freeze)(next));
}

vfs_t *
vfs_allocate( void )
{
	struct vfs		*vfsp;

	vfsp = kmem_zalloc(sizeof(vfs_t), KM_SLEEP);
	bhv_head_init(VFS_BHVHEAD(vfsp), "vfs");
	init_waitqueue_head(&vfsp->vfs_wait_sync_task);
	init_waitqueue_head(&vfsp->vfs_wait_single_sync_task);
	return vfsp;
}

void
vfs_deallocate(
	struct vfs		*vfsp)
{
	bhv_head_destroy(VFS_BHVHEAD(vfsp));
	kmem_free(vfsp, sizeof(vfs_t));
}

void
vfs_insertops(
	struct vfs		*vfsp,
	struct bhv_vfsops	*vfsops)
{
	struct bhv_desc		*bdp;

	bdp = kmem_alloc(sizeof(struct bhv_desc), KM_SLEEP);
	bhv_desc_init(bdp, NULL, vfsp, vfsops);
	bhv_insert(&vfsp->vfs_bh, bdp);
}

void
vfs_insertbhv(
	struct vfs		*vfsp,
	struct bhv_desc		*bdp,
	struct vfsops		*vfsops,
	void			*mount)
{
	bhv_desc_init(bdp, mount, vfsp, vfsops);
	bhv_insert_initial(&vfsp->vfs_bh, bdp);
}

void
bhv_remove_vfsops(
	struct vfs		*vfsp,
	int			pos)
{
	struct bhv_desc		*bhv;

	bhv = bhv_lookup_range(&vfsp->vfs_bh, pos, pos);
	if (!bhv)
		return;
	bhv_remove(&vfsp->vfs_bh, bhv);
	kmem_free(bhv, sizeof(*bhv));
}

void
bhv_remove_all_vfsops(
	struct vfs		*vfsp,
	int			freebase)
{
	struct xfs_mount	*mp;

	bhv_remove_vfsops(vfsp, VFS_POSITION_QM);
	bhv_remove_vfsops(vfsp, VFS_POSITION_DM);
	if (!freebase)
		return;
	mp = XFS_BHVTOM(bhv_lookup(VFS_BHVHEAD(vfsp), &xfs_vfsops));
	VFS_REMOVEBHV(vfsp, &mp->m_bhv);
	xfs_mount_free(mp, 0);
}

void
bhv_insert_all_vfsops(
	struct vfs		*vfsp)
{
	struct xfs_mount	*mp;

	mp = xfs_mount_init();
	vfs_insertbhv(vfsp, &mp->m_bhv, &xfs_vfsops, mp);
	vfs_insertdmapi(vfsp);
	vfs_insertquota(vfsp);
}
