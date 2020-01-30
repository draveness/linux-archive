/* internal.h: internal procfs definitions
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/proc_fs.h>

#ifdef CONFIG_PROC_SYSCTL
extern int proc_sys_init(void);
#else
static inline void proc_sys_init(void) { }
#endif

struct vmalloc_info {
	unsigned long	used;
	unsigned long	largest_chunk;
};

#ifdef CONFIG_MMU
#define VMALLOC_TOTAL (VMALLOC_END - VMALLOC_START)
extern void get_vmalloc_info(struct vmalloc_info *vmi);
#else

#define VMALLOC_TOTAL 0UL
#define get_vmalloc_info(vmi)			\
do {						\
	(vmi)->used = 0;			\
	(vmi)->largest_chunk = 0;		\
} while(0)

extern int nommu_vma_show(struct seq_file *, struct vm_area_struct *);
#endif

extern int maps_protect;

extern void create_seq_entry(char *name, mode_t mode, const struct file_operations *f);
extern int proc_exe_link(struct inode *, struct dentry **, struct vfsmount **);
extern int proc_tid_stat(struct task_struct *,  char *);
extern int proc_tgid_stat(struct task_struct *, char *);
extern int proc_pid_status(struct task_struct *, char *);
extern int proc_pid_statm(struct task_struct *, char *);

extern const struct file_operations proc_maps_operations;
extern const struct file_operations proc_numa_maps_operations;
extern const struct file_operations proc_smaps_operations;

extern const struct file_operations proc_maps_operations;
extern const struct file_operations proc_numa_maps_operations;
extern const struct file_operations proc_smaps_operations;


void free_proc_entry(struct proc_dir_entry *de);

int proc_init_inodecache(void);

static inline struct pid *proc_pid(struct inode *inode)
{
	return PROC_I(inode)->pid;
}

static inline struct task_struct *get_proc_task(struct inode *inode)
{
	return get_pid_task(proc_pid(inode), PIDTYPE_PID);
}

static inline int proc_fd(struct inode *inode)
{
	return PROC_I(inode)->fd;
}
