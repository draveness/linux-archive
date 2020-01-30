/*
 * fs/sysfs/group.c - Operations for adding/removing multiple files at once.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 *
 * This file is released undert the GPL v2. 
 *
 */

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include "sysfs.h"


static void remove_files(struct dentry * dir, 
			 const struct attribute_group * grp)
{
	struct attribute *const* attr;

	for (attr = grp->attrs; *attr; attr++)
		sysfs_hash_and_remove(dir,(*attr)->name);
}

static int create_files(struct dentry * dir,
			const struct attribute_group * grp)
{
	struct attribute *const* attr;
	int error = 0;

	for (attr = grp->attrs; *attr && !error; attr++) {
		error = sysfs_add_file(dir,*attr);
	}
	if (error)
		remove_files(dir,grp);
	return error;
}


int sysfs_create_group(struct kobject * kobj, 
		       const struct attribute_group * grp)
{
	struct dentry * dir;
	int error;

	if (grp->name) {
		error = sysfs_create_subdir(kobj,grp->name,&dir);
		if (error)
			return error;
	} else
		dir = kobj->dentry;
	dir = dget(dir);
	if ((error = create_files(dir,grp))) {
		if (grp->name)
			sysfs_remove_subdir(dir);
	}
	dput(dir);
	return error;
}

void sysfs_remove_group(struct kobject * kobj, 
			const struct attribute_group * grp)
{
	struct dentry * dir;

	if (grp->name)
		dir = sysfs_get_dentry(kobj->dentry,grp->name);
	else
		dir = dget(kobj->dentry);

	remove_files(dir,grp);
	if (grp->name)
		sysfs_remove_subdir(dir);
	/* release the ref. taken in this routine */
	dput(dir);
}


EXPORT_SYMBOL(sysfs_create_group);
EXPORT_SYMBOL(sysfs_remove_group);
