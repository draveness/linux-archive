/*
 * sysctl.c - Code for sysctl handling in NTFS Linux kernel driver. Part of
 *	      the Linux-NTFS project. Adapted from the old NTFS driver,
 *	      Copyright (C) 1997 Martin von L�wis, R�gis Duchesne
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

#ifdef DEBUG

#include <linux/module.h>

#ifdef CONFIG_SYSCTL

#include <linux/proc_fs.h>
#include <linux/sysctl.h>

#include "sysctl.h"
#include "debug.h"

#define FS_NTFS	1

/* Definition of the ntfs sysctl. */
static ctl_table ntfs_sysctls[] = {
	{ FS_NTFS, "ntfs-debug",		/* Binary and text IDs. */
	  &debug_msgs,sizeof(debug_msgs),	/* Data pointer and size. */
	  0644,	NULL, &proc_dointvec },		/* Mode, child, proc handler. */
	{ 0 }
};

/* Define the parent directory /proc/sys/fs. */
static ctl_table sysctls_root[] = {
	{ CTL_FS, "fs", NULL, 0, 0555, ntfs_sysctls },
	{ 0 }
};

/* Storage for the sysctls header. */
static struct ctl_table_header *sysctls_root_table = NULL;

/**
 * ntfs_sysctl - add or remove the debug sysctl
 * @add:	add (1) or remove (0) the sysctl
 *
 * Add or remove the debug sysctl. Return 0 on success or -errno on error.
 */
int ntfs_sysctl(int add)
{
	if (add) {
		BUG_ON(sysctls_root_table);
		sysctls_root_table = register_sysctl_table(sysctls_root, 0);
		if (!sysctls_root_table)
			return -ENOMEM;
#ifdef CONFIG_PROC_FS
		/*
		 * If the proc file system is in use and we are a module, need
		 * to set the owner of our proc entry to our module. In the
		 * non-modular case, THIS_MODULE is NULL, so this is ok.
		 */
		ntfs_sysctls[0].de->owner = THIS_MODULE;
#endif
	} else {
		BUG_ON(!sysctls_root_table);
		unregister_sysctl_table(sysctls_root_table);
		sysctls_root_table = NULL;
	}
	return 0;
}

#endif /* CONFIG_SYSCTL */
#endif /* DEBUG */
