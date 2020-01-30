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
#include <linux/namei.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <asm/semaphore.h>
#include "sysfs.h"


static void remove_files(struct sysfs_dirent *dir_sd,
			 const struct attribute_group *grp)
{
	struct attribute *const* attr;

	for (attr = grp->attrs; *attr; attr++)
		sysfs_hash_and_remove(dir_sd, (*attr)->name);
}

static int create_files(struct sysfs_dirent *dir_sd,
			const struct attribute_group *grp)
{
	struct attribute *const* attr;
	int error = 0;

	for (attr = grp->attrs; *attr && !error; attr++)
		error = sysfs_add_file(dir_sd, *attr, SYSFS_KOBJ_ATTR);
	if (error)
		remove_files(dir_sd, grp);
	return error;
}


int sysfs_create_group(struct kobject * kobj, 
		       const struct attribute_group * grp)
{
	struct sysfs_dirent *sd;
	int error;

	BUG_ON(!kobj || !kobj->sd);

	if (grp->name) {
		error = sysfs_create_subdir(kobj, grp->name, &sd);
		if (error)
			return error;
	} else
		sd = kobj->sd;
	sysfs_get(sd);
	error = create_files(sd, grp);
	if (error) {
		if (grp->name)
			sysfs_remove_subdir(sd);
	}
	sysfs_put(sd);
	return error;
}

void sysfs_remove_group(struct kobject * kobj, 
			const struct attribute_group * grp)
{
	struct sysfs_dirent *dir_sd = kobj->sd;
	struct sysfs_dirent *sd;

	if (grp->name) {
		sd = sysfs_get_dirent(dir_sd, grp->name);
		BUG_ON(!sd);
	} else
		sd = sysfs_get(dir_sd);

	remove_files(sd, grp);
	if (grp->name)
		sysfs_remove_subdir(sd);

	sysfs_put(sd);
}


EXPORT_SYMBOL_GPL(sysfs_create_group);
EXPORT_SYMBOL_GPL(sysfs_remove_group);
