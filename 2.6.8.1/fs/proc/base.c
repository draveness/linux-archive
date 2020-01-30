/*
 *  linux/fs/proc/base.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  proc base directory handling functions
 *
 *  1999, Al Viro. Rewritten. Now it covers the whole per-process part.
 *  Instead of using magical inumbers to determine the kind of object
 *  we allocate and fill in-core inodes upon lookup. They don't even
 *  go into icache. We cache the reference to task_struct upon lookup too.
 *  Eventually it should become a filesystem in its own. We don't use the
 *  rest of procfs anymore.
 */

#include <asm/uaccess.h>

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/file.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/namei.h>
#include <linux/namespace.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/kallsyms.h>
#include <linux/mount.h>
#include <linux/security.h>
#include <linux/ptrace.h>

/*
 * For hysterical raisins we keep the same inumbers as in the old procfs.
 * Feel free to change the macro below - just keep the range distinct from
 * inumbers of the rest of procfs (currently those are in 0x0000--0xffff).
 * As soon as we'll get a separate superblock we will be able to forget
 * about magical ranges too.
 */

#define fake_ino(pid,ino) (((pid)<<16)|(ino))

enum pid_directory_inos {
	PROC_TGID_INO = 2,
	PROC_TGID_TASK,
	PROC_TGID_STATUS,
	PROC_TGID_MEM,
	PROC_TGID_CWD,
	PROC_TGID_ROOT,
	PROC_TGID_EXE,
	PROC_TGID_FD,
	PROC_TGID_ENVIRON,
	PROC_TGID_AUXV,
	PROC_TGID_CMDLINE,
	PROC_TGID_STAT,
	PROC_TGID_STATM,
	PROC_TGID_MAPS,
	PROC_TGID_MOUNTS,
	PROC_TGID_WCHAN,
#ifdef CONFIG_SECURITY
	PROC_TGID_ATTR,
	PROC_TGID_ATTR_CURRENT,
	PROC_TGID_ATTR_PREV,
	PROC_TGID_ATTR_EXEC,
	PROC_TGID_ATTR_FSCREATE,
#endif
	PROC_TGID_FD_DIR,
	PROC_TID_INO,
	PROC_TID_STATUS,
	PROC_TID_MEM,
	PROC_TID_CWD,
	PROC_TID_ROOT,
	PROC_TID_EXE,
	PROC_TID_FD,
	PROC_TID_ENVIRON,
	PROC_TID_AUXV,
	PROC_TID_CMDLINE,
	PROC_TID_STAT,
	PROC_TID_STATM,
	PROC_TID_MAPS,
	PROC_TID_MOUNTS,
	PROC_TID_WCHAN,
#ifdef CONFIG_SECURITY
	PROC_TID_ATTR,
	PROC_TID_ATTR_CURRENT,
	PROC_TID_ATTR_PREV,
	PROC_TID_ATTR_EXEC,
	PROC_TID_ATTR_FSCREATE,
#endif
	PROC_TID_FD_DIR = 0x8000,	/* 0x8000-0xffff */
};

struct pid_entry {
	int type;
	int len;
	char *name;
	mode_t mode;
};

#define E(type,name,mode) {(type),sizeof(name)-1,(name),(mode)}

static struct pid_entry tgid_base_stuff[] = {
	E(PROC_TGID_TASK,      "task",    S_IFDIR|S_IRUGO|S_IXUGO),
	E(PROC_TGID_FD,        "fd",      S_IFDIR|S_IRUSR|S_IXUSR),
	E(PROC_TGID_ENVIRON,   "environ", S_IFREG|S_IRUSR),
	E(PROC_TGID_AUXV,      "auxv",	  S_IFREG|S_IRUSR),
	E(PROC_TGID_STATUS,    "status",  S_IFREG|S_IRUGO),
	E(PROC_TGID_CMDLINE,   "cmdline", S_IFREG|S_IRUGO),
	E(PROC_TGID_STAT,      "stat",    S_IFREG|S_IRUGO),
	E(PROC_TGID_STATM,     "statm",   S_IFREG|S_IRUGO),
	E(PROC_TGID_MAPS,      "maps",    S_IFREG|S_IRUGO),
	E(PROC_TGID_MEM,       "mem",     S_IFREG|S_IRUSR|S_IWUSR),
	E(PROC_TGID_CWD,       "cwd",     S_IFLNK|S_IRWXUGO),
	E(PROC_TGID_ROOT,      "root",    S_IFLNK|S_IRWXUGO),
	E(PROC_TGID_EXE,       "exe",     S_IFLNK|S_IRWXUGO),
	E(PROC_TGID_MOUNTS,    "mounts",  S_IFREG|S_IRUGO),
#ifdef CONFIG_SECURITY
	E(PROC_TGID_ATTR,      "attr",    S_IFDIR|S_IRUGO|S_IXUGO),
#endif
#ifdef CONFIG_KALLSYMS
	E(PROC_TGID_WCHAN,     "wchan",   S_IFREG|S_IRUGO),
#endif
	{0,0,NULL,0}
};
static struct pid_entry tid_base_stuff[] = {
	E(PROC_TID_FD,         "fd",      S_IFDIR|S_IRUSR|S_IXUSR),
	E(PROC_TID_ENVIRON,    "environ", S_IFREG|S_IRUSR),
	E(PROC_TID_AUXV,       "auxv",	  S_IFREG|S_IRUSR),
	E(PROC_TID_STATUS,     "status",  S_IFREG|S_IRUGO),
	E(PROC_TID_CMDLINE,    "cmdline", S_IFREG|S_IRUGO),
	E(PROC_TID_STAT,       "stat",    S_IFREG|S_IRUGO),
	E(PROC_TID_STATM,      "statm",   S_IFREG|S_IRUGO),
	E(PROC_TID_MAPS,       "maps",    S_IFREG|S_IRUGO),
	E(PROC_TID_MEM,        "mem",     S_IFREG|S_IRUSR|S_IWUSR),
	E(PROC_TID_CWD,        "cwd",     S_IFLNK|S_IRWXUGO),
	E(PROC_TID_ROOT,       "root",    S_IFLNK|S_IRWXUGO),
	E(PROC_TID_EXE,        "exe",     S_IFLNK|S_IRWXUGO),
	E(PROC_TID_MOUNTS,     "mounts",  S_IFREG|S_IRUGO),
#ifdef CONFIG_SECURITY
	E(PROC_TID_ATTR,       "attr",    S_IFDIR|S_IRUGO|S_IXUGO),
#endif
#ifdef CONFIG_KALLSYMS
	E(PROC_TID_WCHAN,      "wchan",   S_IFREG|S_IRUGO),
#endif
	{0,0,NULL,0}
};

#ifdef CONFIG_SECURITY
static struct pid_entry tgid_attr_stuff[] = {
	E(PROC_TGID_ATTR_CURRENT,  "current",  S_IFREG|S_IRUGO|S_IWUGO),
	E(PROC_TGID_ATTR_PREV,     "prev",     S_IFREG|S_IRUGO),
	E(PROC_TGID_ATTR_EXEC,     "exec",     S_IFREG|S_IRUGO|S_IWUGO),
	E(PROC_TGID_ATTR_FSCREATE, "fscreate", S_IFREG|S_IRUGO|S_IWUGO),
	{0,0,NULL,0}
};
static struct pid_entry tid_attr_stuff[] = {
	E(PROC_TID_ATTR_CURRENT,   "current",  S_IFREG|S_IRUGO|S_IWUGO),
	E(PROC_TID_ATTR_PREV,      "prev",     S_IFREG|S_IRUGO),
	E(PROC_TID_ATTR_EXEC,      "exec",     S_IFREG|S_IRUGO|S_IWUGO),
	E(PROC_TID_ATTR_FSCREATE,  "fscreate", S_IFREG|S_IRUGO|S_IWUGO),
	{0,0,NULL,0}
};
#endif

#undef E

static inline struct task_struct *proc_task(struct inode *inode)
{
	return PROC_I(inode)->task;
}

static inline int proc_type(struct inode *inode)
{
	return PROC_I(inode)->type;
}

int proc_pid_stat(struct task_struct*,char*);
int proc_pid_status(struct task_struct*,char*);
int proc_pid_statm(struct task_struct*,char*);
int proc_pid_cpu(struct task_struct*,char*);

static int proc_fd_link(struct inode *inode, struct dentry **dentry, struct vfsmount **mnt)
{
	struct task_struct *task = proc_task(inode);
	struct files_struct *files;
	struct file *file;
	int fd = proc_type(inode) - PROC_TID_FD_DIR;

	files = get_files_struct(task);
	if (files) {
		spin_lock(&files->file_lock);
		file = fcheck_files(files, fd);
		if (file) {
			*mnt = mntget(file->f_vfsmnt);
			*dentry = dget(file->f_dentry);
			spin_unlock(&files->file_lock);
			put_files_struct(files);
			return 0;
		}
		spin_unlock(&files->file_lock);
		put_files_struct(files);
	}
	return -ENOENT;
}

static int proc_exe_link(struct inode *inode, struct dentry **dentry, struct vfsmount **mnt)
{
	struct vm_area_struct * vma;
	int result = -ENOENT;
	struct task_struct *task = proc_task(inode);
	struct mm_struct * mm = get_task_mm(task);

	if (!mm)
		goto out;
	down_read(&mm->mmap_sem);
	vma = mm->mmap;
	while (vma) {
		if ((vma->vm_flags & VM_EXECUTABLE) && 
		    vma->vm_file) {
			*mnt = mntget(vma->vm_file->f_vfsmnt);
			*dentry = dget(vma->vm_file->f_dentry);
			result = 0;
			break;
		}
		vma = vma->vm_next;
	}
	up_read(&mm->mmap_sem);
	mmput(mm);
out:
	return result;
}

static int proc_cwd_link(struct inode *inode, struct dentry **dentry, struct vfsmount **mnt)
{
	struct fs_struct *fs;
	int result = -ENOENT;
	task_lock(proc_task(inode));
	fs = proc_task(inode)->fs;
	if(fs)
		atomic_inc(&fs->count);
	task_unlock(proc_task(inode));
	if (fs) {
		read_lock(&fs->lock);
		*mnt = mntget(fs->pwdmnt);
		*dentry = dget(fs->pwd);
		read_unlock(&fs->lock);
		result = 0;
		put_fs_struct(fs);
	}
	return result;
}

static int proc_root_link(struct inode *inode, struct dentry **dentry, struct vfsmount **mnt)
{
	struct fs_struct *fs;
	int result = -ENOENT;
	task_lock(proc_task(inode));
	fs = proc_task(inode)->fs;
	if(fs)
		atomic_inc(&fs->count);
	task_unlock(proc_task(inode));
	if (fs) {
		read_lock(&fs->lock);
		*mnt = mntget(fs->rootmnt);
		*dentry = dget(fs->root);
		read_unlock(&fs->lock);
		result = 0;
		put_fs_struct(fs);
	}
	return result;
}

#define MAY_PTRACE(task) \
	(task == current || \
	(task->parent == current && \
	(task->ptrace & PT_PTRACED) &&  task->state == TASK_STOPPED && \
	 security_ptrace(current,task) == 0))

static int may_ptrace_attach(struct task_struct *task)
{
	int retval = 0;

	task_lock(task);

	if (!task->mm)
		goto out;
	if (((current->uid != task->euid) ||
	     (current->uid != task->suid) ||
	     (current->uid != task->uid) ||
	     (current->gid != task->egid) ||
	     (current->gid != task->sgid) ||
	     (current->gid != task->gid)) && !capable(CAP_SYS_PTRACE))
		goto out;
	rmb();
	if (!task->mm->dumpable && !capable(CAP_SYS_PTRACE))
		goto out;
	if (security_ptrace(current, task))
		goto out;

	retval = 1;
out:
	task_unlock(task);
	return retval;
}

static int proc_pid_environ(struct task_struct *task, char * buffer)
{
	int res = 0;
	struct mm_struct *mm = get_task_mm(task);
	if (mm) {
		unsigned int len = mm->env_end - mm->env_start;
		if (len > PAGE_SIZE)
			len = PAGE_SIZE;
		res = access_process_vm(task, mm->env_start, buffer, len, 0);
		if (!may_ptrace_attach(task))
			res = -ESRCH;
		mmput(mm);
	}
	return res;
}

static int proc_pid_cmdline(struct task_struct *task, char * buffer)
{
	int res = 0;
	unsigned int len;
	struct mm_struct *mm = get_task_mm(task);
	if (!mm)
		goto out;

 	len = mm->arg_end - mm->arg_start;
 
	if (len > PAGE_SIZE)
		len = PAGE_SIZE;
 
	res = access_process_vm(task, mm->arg_start, buffer, len, 0);

	// If the nul at the end of args has been overwritten, then
	// assume application is using setproctitle(3).
	if (res > 0 && buffer[res-1] != '\0') {
		len = strnlen(buffer, res);
		if (len < res) {
		    res = len;
		} else {
			len = mm->env_end - mm->env_start;
			if (len > PAGE_SIZE - res)
				len = PAGE_SIZE - res;
			res += access_process_vm(task, mm->env_start, buffer+res, len, 0);
			res = strnlen(buffer, res);
		}
	}
	mmput(mm);

out:
	return res;
}

static int proc_pid_auxv(struct task_struct *task, char *buffer)
{
	int res = 0;
	struct mm_struct *mm = get_task_mm(task);
	if (mm) {
		unsigned int nwords = 0;
		do
			nwords += 2;
		while (mm->saved_auxv[nwords - 2] != 0); /* AT_NULL */
		res = nwords * sizeof(mm->saved_auxv[0]);
		if (res > PAGE_SIZE)
			res = PAGE_SIZE;
		memcpy(buffer, mm->saved_auxv, res);
		mmput(mm);
	}
	return res;
}


#ifdef CONFIG_KALLSYMS
/*
 * Provides a wchan file via kallsyms in a proper one-value-per-file format.
 * Returns the resolved symbol.  If that fails, simply return the address.
 */
static int proc_pid_wchan(struct task_struct *task, char *buffer)
{
	char *modname;
	const char *sym_name;
	unsigned long wchan, size, offset;
	char namebuf[128];

	wchan = get_wchan(task);

	sym_name = kallsyms_lookup(wchan, &size, &offset, &modname, namebuf);
	if (sym_name)
		return sprintf(buffer, "%s", sym_name);
	return sprintf(buffer, "%lu", wchan);
}
#endif /* CONFIG_KALLSYMS */

/************************************************************************/
/*                       Here the fs part begins                        */
/************************************************************************/

/* permission checks */

static int proc_check_root(struct inode *inode)
{
	struct dentry *de, *base, *root;
	struct vfsmount *our_vfsmnt, *vfsmnt, *mnt;
	int res = 0;

	if (proc_root_link(inode, &root, &vfsmnt)) /* Ewww... */
		return -ENOENT;
	read_lock(&current->fs->lock);
	our_vfsmnt = mntget(current->fs->rootmnt);
	base = dget(current->fs->root);
	read_unlock(&current->fs->lock);

	spin_lock(&vfsmount_lock);
	de = root;
	mnt = vfsmnt;

	while (vfsmnt != our_vfsmnt) {
		if (vfsmnt == vfsmnt->mnt_parent)
			goto out;
		de = vfsmnt->mnt_mountpoint;
		vfsmnt = vfsmnt->mnt_parent;
	}

	if (!is_subdir(de, base))
		goto out;
	spin_unlock(&vfsmount_lock);

exit:
	dput(base);
	mntput(our_vfsmnt);
	dput(root);
	mntput(mnt);
	return res;
out:
	spin_unlock(&vfsmount_lock);
	res = -EACCES;
	goto exit;
}

static int proc_permission(struct inode *inode, int mask, struct nameidata *nd)
{
	if (vfs_permission(inode, mask) != 0)
		return -EACCES;
	return proc_check_root(inode);
}

extern struct seq_operations proc_pid_maps_op;
static int maps_open(struct inode *inode, struct file *file)
{
	struct task_struct *task = proc_task(inode);
	int ret = seq_open(file, &proc_pid_maps_op);
	if (!ret) {
		struct seq_file *m = file->private_data;
		m->private = task;
	}
	return ret;
}

static struct file_operations proc_maps_operations = {
	.open		= maps_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

extern struct seq_operations mounts_op;
static int mounts_open(struct inode *inode, struct file *file)
{
	struct task_struct *task = proc_task(inode);
	int ret = seq_open(file, &mounts_op);

	if (!ret) {
		struct seq_file *m = file->private_data;
		struct namespace *namespace;
		task_lock(task);
		namespace = task->namespace;
		if (namespace)
			get_namespace(namespace);
		task_unlock(task);

		if (namespace)
			m->private = namespace;
		else {
			seq_release(inode, file);
			ret = -EINVAL;
		}
	}
	return ret;
}

static int mounts_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct namespace *namespace = m->private;
	put_namespace(namespace);
	return seq_release(inode, file);
}

static struct file_operations proc_mounts_operations = {
	.open		= mounts_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= mounts_release,
};

#define PROC_BLOCK_SIZE	(3*1024)		/* 4K page size but our output routines use some slack for overruns */

static ssize_t proc_info_read(struct file * file, char __user * buf,
			  size_t count, loff_t *ppos)
{
	struct inode * inode = file->f_dentry->d_inode;
	unsigned long page;
	ssize_t length;
	ssize_t end;
	struct task_struct *task = proc_task(inode);

	if (count > PROC_BLOCK_SIZE)
		count = PROC_BLOCK_SIZE;
	if (!(page = __get_free_page(GFP_KERNEL)))
		return -ENOMEM;

	length = PROC_I(inode)->op.proc_read(task, (char*)page);

	if (length < 0) {
		free_page(page);
		return length;
	}
	/* Static 4kB (or whatever) block capacity */
	if (*ppos >= length) {
		free_page(page);
		return 0;
	}
	if (count + *ppos > length)
		count = length - *ppos;
	end = count + *ppos;
	if (copy_to_user(buf, (char *) page + *ppos, count))
		count = -EFAULT;
	else
		*ppos = end;
	free_page(page);
	return count;
}

static struct file_operations proc_info_file_operations = {
	.read		= proc_info_read,
};

static int mem_open(struct inode* inode, struct file* file)
{
	file->private_data = (void*)((long)current->self_exec_id);
	return 0;
}

static ssize_t mem_read(struct file * file, char __user * buf,
			size_t count, loff_t *ppos)
{
	struct task_struct *task = proc_task(file->f_dentry->d_inode);
	char *page;
	unsigned long src = *ppos;
	int ret = -ESRCH;
	struct mm_struct *mm;

	if (!MAY_PTRACE(task) || !may_ptrace_attach(task))
		goto out;

	ret = -ENOMEM;
	page = (char *)__get_free_page(GFP_USER);
	if (!page)
		goto out;

	ret = 0;
 
	mm = get_task_mm(task);
	if (!mm)
		goto out_free;

	ret = -EIO;
 
	if (file->private_data != (void*)((long)current->self_exec_id))
		goto out_put;

	ret = 0;
 
	while (count > 0) {
		int this_len, retval;

		this_len = (count > PAGE_SIZE) ? PAGE_SIZE : count;
		retval = access_process_vm(task, src, page, this_len, 0);
		if (!retval || !MAY_PTRACE(task) || !may_ptrace_attach(task)) {
			if (!ret)
				ret = -EIO;
			break;
		}

		if (copy_to_user(buf, page, retval)) {
			ret = -EFAULT;
			break;
		}
 
		ret += retval;
		src += retval;
		buf += retval;
		count -= retval;
	}
	*ppos = src;

out_put:
	mmput(mm);
out_free:
	free_page((unsigned long) page);
out:
	return ret;
}

#define mem_write NULL

#ifndef mem_write
/* This is a security hazard */
static ssize_t mem_write(struct file * file, const char * buf,
			 size_t count, loff_t *ppos)
{
	int copied = 0;
	char *page;
	struct task_struct *task = proc_task(file->f_dentry->d_inode);
	unsigned long dst = *ppos;

	if (!MAY_PTRACE(task) || !may_ptrace_attach(task))
		return -ESRCH;

	page = (char *)__get_free_page(GFP_USER);
	if (!page)
		return -ENOMEM;

	while (count > 0) {
		int this_len, retval;

		this_len = (count > PAGE_SIZE) ? PAGE_SIZE : count;
		if (copy_from_user(page, buf, this_len)) {
			copied = -EFAULT;
			break;
		}
		retval = access_process_vm(task, dst, page, this_len, 1);
		if (!retval) {
			if (!copied)
				copied = -EIO;
			break;
		}
		copied += retval;
		buf += retval;
		dst += retval;
		count -= retval;			
	}
	*ppos = dst;
	free_page((unsigned long) page);
	return copied;
}
#endif

static loff_t mem_lseek(struct file * file, loff_t offset, int orig)
{
	switch (orig) {
	case 0:
		file->f_pos = offset;
		break;
	case 1:
		file->f_pos += offset;
		break;
	default:
		return -EINVAL;
	}
	force_successful_syscall_return();
	return file->f_pos;
}

static struct file_operations proc_mem_operations = {
	.llseek		= mem_lseek,
	.read		= mem_read,
	.write		= mem_write,
	.open		= mem_open,
};

static struct inode_operations proc_mem_inode_operations = {
	.permission	= proc_permission,
};

static int proc_pid_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode = dentry->d_inode;
	int error = -EACCES;

	/* We don't need a base pointer in the /proc filesystem */
	path_release(nd);

	if (current->fsuid != inode->i_uid && !capable(CAP_DAC_OVERRIDE))
		goto out;
	error = proc_check_root(inode);
	if (error)
		goto out;

	error = PROC_I(inode)->op.proc_get_link(inode, &nd->dentry, &nd->mnt);
	nd->last_type = LAST_BIND;
out:
	return error;
}

static int do_proc_readlink(struct dentry *dentry, struct vfsmount *mnt,
			    char __user *buffer, int buflen)
{
	struct inode * inode;
	char *tmp = (char*)__get_free_page(GFP_KERNEL), *path;
	int len;

	if (!tmp)
		return -ENOMEM;
		
	inode = dentry->d_inode;
	path = d_path(dentry, mnt, tmp, PAGE_SIZE);
	len = PTR_ERR(path);
	if (IS_ERR(path))
		goto out;
	len = tmp + PAGE_SIZE - 1 - path;

	if (len > buflen)
		len = buflen;
	if (copy_to_user(buffer, path, len))
		len = -EFAULT;
 out:
	free_page((unsigned long)tmp);
	return len;
}

static int proc_pid_readlink(struct dentry * dentry, char __user * buffer, int buflen)
{
	int error = -EACCES;
	struct inode *inode = dentry->d_inode;
	struct dentry *de;
	struct vfsmount *mnt = NULL;

	lock_kernel();

	if (current->fsuid != inode->i_uid && !capable(CAP_DAC_OVERRIDE))
		goto out;
	error = proc_check_root(inode);
	if (error)
		goto out;

	error = PROC_I(inode)->op.proc_get_link(inode, &de, &mnt);
	if (error)
		goto out;

	error = do_proc_readlink(de, mnt, buffer, buflen);
	dput(de);
	mntput(mnt);
out:
	unlock_kernel();
	return error;
}

static struct inode_operations proc_pid_link_inode_operations = {
	.readlink	= proc_pid_readlink,
	.follow_link	= proc_pid_follow_link
};

static int pid_alive(struct task_struct *p)
{
	BUG_ON(p->pids[PIDTYPE_PID].pidptr != &p->pids[PIDTYPE_PID].pid);
	return atomic_read(&p->pids[PIDTYPE_PID].pid.count);
}

#define NUMBUF 10

static int proc_readfd(struct file * filp, void * dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct task_struct *p = proc_task(inode);
	unsigned int fd, tid, ino;
	int retval;
	char buf[NUMBUF];
	struct files_struct * files;

	retval = -ENOENT;
	if (!pid_alive(p))
		goto out;
	retval = 0;
	tid = p->pid;

	fd = filp->f_pos;
	switch (fd) {
		case 0:
			if (filldir(dirent, ".", 1, 0, inode->i_ino, DT_DIR) < 0)
				goto out;
			filp->f_pos++;
		case 1:
			ino = fake_ino(tid, PROC_TID_INO);
			if (filldir(dirent, "..", 2, 1, ino, DT_DIR) < 0)
				goto out;
			filp->f_pos++;
		default:
			files = get_files_struct(p);
			if (!files)
				goto out;
			spin_lock(&files->file_lock);
			for (fd = filp->f_pos-2;
			     fd < files->max_fds;
			     fd++, filp->f_pos++) {
				unsigned int i,j;

				if (!fcheck_files(files, fd))
					continue;
				spin_unlock(&files->file_lock);

				j = NUMBUF;
				i = fd;
				do {
					j--;
					buf[j] = '0' + (i % 10);
					i /= 10;
				} while (i);

				ino = fake_ino(tid, PROC_TID_FD_DIR + fd);
				if (filldir(dirent, buf+j, NUMBUF-j, fd+2, ino, DT_LNK) < 0) {
					spin_lock(&files->file_lock);
					break;
				}
				spin_lock(&files->file_lock);
			}
			spin_unlock(&files->file_lock);
			put_files_struct(files);
	}
out:
	return retval;
}

static int proc_pident_readdir(struct file *filp,
		void *dirent, filldir_t filldir,
		struct pid_entry *ents, unsigned int nents)
{
	int i;
	int pid;
	struct dentry *dentry = filp->f_dentry;
	struct inode *inode = dentry->d_inode;
	struct pid_entry *p;
	ino_t ino;
	int ret;

	ret = -ENOENT;
	if (!pid_alive(proc_task(inode)))
		goto out;

	ret = 0;
	pid = proc_task(inode)->pid;
	i = filp->f_pos;
	switch (i) {
	case 0:
		ino = inode->i_ino;
		if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0)
			goto out;
		i++;
		filp->f_pos++;
		/* fall through */
	case 1:
		ino = parent_ino(dentry);
		if (filldir(dirent, "..", 2, i, ino, DT_DIR) < 0)
			goto out;
		i++;
		filp->f_pos++;
		/* fall through */
	default:
		i -= 2;
		if (i >= nents) {
			ret = 1;
			goto out;
		}
		p = ents + i;
		while (p->name) {
			if (filldir(dirent, p->name, p->len, filp->f_pos,
				    fake_ino(pid, p->type), p->mode >> 12) < 0)
				goto out;
			filp->f_pos++;
			p++;
		}
	}

	ret = 1;
out:
	return ret;
}

static int proc_tgid_base_readdir(struct file * filp,
			     void * dirent, filldir_t filldir)
{
	return proc_pident_readdir(filp,dirent,filldir,
				   tgid_base_stuff,ARRAY_SIZE(tgid_base_stuff));
}

static int proc_tid_base_readdir(struct file * filp,
			     void * dirent, filldir_t filldir)
{
	return proc_pident_readdir(filp,dirent,filldir,
				   tid_base_stuff,ARRAY_SIZE(tid_base_stuff));
}

/* building an inode */

static int task_dumpable(struct task_struct *task)
{
	int dumpable = 0;
	struct mm_struct *mm;

	task_lock(task);
	mm = task->mm;
	if (mm)
		dumpable = mm->dumpable;
	task_unlock(task);
	return dumpable;
}


static struct inode *proc_pid_make_inode(struct super_block * sb, struct task_struct *task, int ino)
{
	struct inode * inode;
	struct proc_inode *ei;

	/* We need a new inode */
	
	inode = new_inode(sb);
	if (!inode)
		goto out;

	/* Common stuff */
	ei = PROC_I(inode);
	ei->task = NULL;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_ino = fake_ino(task->pid, ino);

	if (!pid_alive(task))
		goto out_unlock;

	/*
	 * grab the reference to task.
	 */
	get_task_struct(task);
	ei->task = task;
	ei->type = ino;
	inode->i_uid = 0;
	inode->i_gid = 0;
	if (ino == PROC_TGID_INO || ino == PROC_TID_INO || task_dumpable(task)) {
		inode->i_uid = task->euid;
		inode->i_gid = task->egid;
	}
	security_task_to_inode(task, inode);

out:
	return inode;

out_unlock:
	ei->pde = NULL;
	iput(inode);
	return NULL;
}

/* dentry stuff */

/*
 *	Exceptional case: normally we are not allowed to unhash a busy
 * directory. In this case, however, we can do it - no aliasing problems
 * due to the way we treat inodes.
 *
 * Rewrite the inode's ownerships here because the owning task may have
 * performed a setuid(), etc.
 */
static int pid_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode = dentry->d_inode;
	struct task_struct *task = proc_task(inode);
	if (pid_alive(task)) {
		if (proc_type(inode) == PROC_TGID_INO || proc_type(inode) == PROC_TID_INO || task_dumpable(task)) {
			inode->i_uid = task->euid;
			inode->i_gid = task->egid;
		} else {
			inode->i_uid = 0;
			inode->i_gid = 0;
		}
		security_task_to_inode(task, inode);
		return 1;
	}
	d_drop(dentry);
	return 0;
}

static int tid_fd_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode = dentry->d_inode;
	struct task_struct *task = proc_task(inode);
	int fd = proc_type(inode) - PROC_TID_FD_DIR;
	struct files_struct *files;

	files = get_files_struct(task);
	if (files) {
		spin_lock(&files->file_lock);
		if (fcheck_files(files, fd)) {
			spin_unlock(&files->file_lock);
			put_files_struct(files);
			if (task_dumpable(task)) {
				inode->i_uid = task->euid;
				inode->i_gid = task->egid;
			} else {
				inode->i_uid = 0;
				inode->i_gid = 0;
			}
			security_task_to_inode(task, inode);
			return 1;
		}
		spin_unlock(&files->file_lock);
		put_files_struct(files);
	}
	d_drop(dentry);
	return 0;
}

static void pid_base_iput(struct dentry *dentry, struct inode *inode)
{
	struct task_struct *task = proc_task(inode);
	spin_lock(&task->proc_lock);
	if (task->proc_dentry == dentry)
		task->proc_dentry = NULL;
	spin_unlock(&task->proc_lock);
	iput(inode);
}

static int pid_delete_dentry(struct dentry * dentry)
{
	/* Is the task we represent dead?
	 * If so, then don't put the dentry on the lru list,
	 * kill it immediately.
	 */
	return !pid_alive(proc_task(dentry->d_inode));
}

static struct dentry_operations tid_fd_dentry_operations =
{
	.d_revalidate	= tid_fd_revalidate,
	.d_delete	= pid_delete_dentry,
};

static struct dentry_operations pid_dentry_operations =
{
	.d_revalidate	= pid_revalidate,
	.d_delete	= pid_delete_dentry,
};

static struct dentry_operations pid_base_dentry_operations =
{
	.d_revalidate	= pid_revalidate,
	.d_iput		= pid_base_iput,
	.d_delete	= pid_delete_dentry,
};

/* Lookups */

static unsigned name_to_int(struct dentry *dentry)
{
	const char *name = dentry->d_name.name;
	int len = dentry->d_name.len;
	unsigned n = 0;

	if (len > 1 && *name == '0')
		goto out;
	while (len-- > 0) {
		unsigned c = *name++ - '0';
		if (c > 9)
			goto out;
		if (n >= (~0U-9)/10)
			goto out;
		n *= 10;
		n += c;
	}
	return n;
out:
	return ~0U;
}

/* SMP-safe */
static struct dentry *proc_lookupfd(struct inode * dir, struct dentry * dentry, struct nameidata *nd)
{
	struct task_struct *task = proc_task(dir);
	unsigned fd = name_to_int(dentry);
	struct file * file;
	struct files_struct * files;
	struct inode *inode;
	struct proc_inode *ei;

	if (fd == ~0U)
		goto out;
	if (!pid_alive(task))
		goto out;

	inode = proc_pid_make_inode(dir->i_sb, task, PROC_TID_FD_DIR+fd);
	if (!inode)
		goto out;
	ei = PROC_I(inode);
	files = get_files_struct(task);
	if (!files)
		goto out_unlock;
	inode->i_mode = S_IFLNK;
	spin_lock(&files->file_lock);
	file = fcheck_files(files, fd);
	if (!file)
		goto out_unlock2;
	if (file->f_mode & 1)
		inode->i_mode |= S_IRUSR | S_IXUSR;
	if (file->f_mode & 2)
		inode->i_mode |= S_IWUSR | S_IXUSR;
	spin_unlock(&files->file_lock);
	put_files_struct(files);
	inode->i_op = &proc_pid_link_inode_operations;
	inode->i_size = 64;
	ei->op.proc_get_link = proc_fd_link;
	dentry->d_op = &tid_fd_dentry_operations;
	d_add(dentry, inode);
	return NULL;

out_unlock2:
	spin_unlock(&files->file_lock);
	put_files_struct(files);
out_unlock:
	iput(inode);
out:
	return ERR_PTR(-ENOENT);
}

static int proc_task_readdir(struct file * filp, void * dirent, filldir_t filldir);
static struct dentry *proc_task_lookup(struct inode *dir, struct dentry * dentry, struct nameidata *nd);

static struct file_operations proc_fd_operations = {
	.read		= generic_read_dir,
	.readdir	= proc_readfd,
};

static struct file_operations proc_task_operations = {
	.read		= generic_read_dir,
	.readdir	= proc_task_readdir,
};

/*
 * proc directories can do almost nothing..
 */
static struct inode_operations proc_fd_inode_operations = {
	.lookup		= proc_lookupfd,
	.permission	= proc_permission,
};

static struct inode_operations proc_task_inode_operations = {
	.lookup		= proc_task_lookup,
	.permission	= proc_permission,
};

#ifdef CONFIG_SECURITY
static ssize_t proc_pid_attr_read(struct file * file, char __user * buf,
				  size_t count, loff_t *ppos)
{
	struct inode * inode = file->f_dentry->d_inode;
	unsigned long page;
	ssize_t length;
	ssize_t end;
	struct task_struct *task = proc_task(inode);

	if (count > PAGE_SIZE)
		count = PAGE_SIZE;
	if (!(page = __get_free_page(GFP_KERNEL)))
		return -ENOMEM;

	length = security_getprocattr(task, 
				      (char*)file->f_dentry->d_name.name, 
				      (void*)page, count);
	if (length < 0) {
		free_page(page);
		return length;
	}
	/* Static 4kB (or whatever) block capacity */
	if (*ppos >= length) {
		free_page(page);
		return 0;
	}
	if (count + *ppos > length)
		count = length - *ppos;
	end = count + *ppos;
	if (copy_to_user(buf, (char *) page + *ppos, count))
		count = -EFAULT;
	else
		*ppos = end;
	free_page(page);
	return count;
}

static ssize_t proc_pid_attr_write(struct file * file, const char __user * buf,
				   size_t count, loff_t *ppos)
{ 
	struct inode * inode = file->f_dentry->d_inode;
	char *page; 
	ssize_t length; 
	struct task_struct *task = proc_task(inode); 

	if (count > PAGE_SIZE) 
		count = PAGE_SIZE; 
	if (*ppos != 0) {
		/* No partial writes. */
		return -EINVAL;
	}
	page = (char*)__get_free_page(GFP_USER); 
	if (!page) 
		return -ENOMEM;
	length = -EFAULT; 
	if (copy_from_user(page, buf, count)) 
		goto out;

	length = security_setprocattr(task, 
				      (char*)file->f_dentry->d_name.name, 
				      (void*)page, count);
out:
	free_page((unsigned long) page);
	return length;
} 

static struct file_operations proc_pid_attr_operations = {
	.read		= proc_pid_attr_read,
	.write		= proc_pid_attr_write,
};

static struct file_operations proc_tid_attr_operations;
static struct inode_operations proc_tid_attr_inode_operations;
static struct file_operations proc_tgid_attr_operations;
static struct inode_operations proc_tgid_attr_inode_operations;
#endif

/* SMP-safe */
static struct dentry *proc_pident_lookup(struct inode *dir, 
					 struct dentry *dentry,
					 struct pid_entry *ents)
{
	struct inode *inode;
	int error;
	struct task_struct *task = proc_task(dir);
	struct pid_entry *p;
	struct proc_inode *ei;

	error = -ENOENT;
	inode = NULL;

	if (!pid_alive(task))
		goto out;

	for (p = ents; p->name; p++) {
		if (p->len != dentry->d_name.len)
			continue;
		if (!memcmp(dentry->d_name.name, p->name, p->len))
			break;
	}
	if (!p->name)
		goto out;

	error = -EINVAL;
	inode = proc_pid_make_inode(dir->i_sb, task, p->type);
	if (!inode)
		goto out;

	ei = PROC_I(inode);
	inode->i_mode = p->mode;
	/*
	 * Yes, it does not scale. And it should not. Don't add
	 * new entries into /proc/<tgid>/ without very good reasons.
	 */
	switch(p->type) {
		case PROC_TGID_TASK:
			inode->i_nlink = 3;
			inode->i_op = &proc_task_inode_operations;
			inode->i_fop = &proc_task_operations;
			break;
		case PROC_TID_FD:
		case PROC_TGID_FD:
			inode->i_nlink = 2;
			inode->i_op = &proc_fd_inode_operations;
			inode->i_fop = &proc_fd_operations;
			break;
		case PROC_TID_EXE:
		case PROC_TGID_EXE:
			inode->i_op = &proc_pid_link_inode_operations;
			ei->op.proc_get_link = proc_exe_link;
			break;
		case PROC_TID_CWD:
		case PROC_TGID_CWD:
			inode->i_op = &proc_pid_link_inode_operations;
			ei->op.proc_get_link = proc_cwd_link;
			break;
		case PROC_TID_ROOT:
		case PROC_TGID_ROOT:
			inode->i_op = &proc_pid_link_inode_operations;
			ei->op.proc_get_link = proc_root_link;
			break;
		case PROC_TID_ENVIRON:
		case PROC_TGID_ENVIRON:
			inode->i_fop = &proc_info_file_operations;
			ei->op.proc_read = proc_pid_environ;
			break;
		case PROC_TID_AUXV:
		case PROC_TGID_AUXV:
			inode->i_fop = &proc_info_file_operations;
			ei->op.proc_read = proc_pid_auxv;
			break;
		case PROC_TID_STATUS:
		case PROC_TGID_STATUS:
			inode->i_fop = &proc_info_file_operations;
			ei->op.proc_read = proc_pid_status;
			break;
		case PROC_TID_STAT:
		case PROC_TGID_STAT:
			inode->i_fop = &proc_info_file_operations;
			ei->op.proc_read = proc_pid_stat;
			break;
		case PROC_TID_CMDLINE:
		case PROC_TGID_CMDLINE:
			inode->i_fop = &proc_info_file_operations;
			ei->op.proc_read = proc_pid_cmdline;
			break;
		case PROC_TID_STATM:
		case PROC_TGID_STATM:
			inode->i_fop = &proc_info_file_operations;
			ei->op.proc_read = proc_pid_statm;
			break;
		case PROC_TID_MAPS:
		case PROC_TGID_MAPS:
			inode->i_fop = &proc_maps_operations;
			break;
		case PROC_TID_MEM:
		case PROC_TGID_MEM:
			inode->i_op = &proc_mem_inode_operations;
			inode->i_fop = &proc_mem_operations;
			break;
		case PROC_TID_MOUNTS:
		case PROC_TGID_MOUNTS:
			inode->i_fop = &proc_mounts_operations;
			break;
#ifdef CONFIG_SECURITY
		case PROC_TID_ATTR:
			inode->i_nlink = 2;
			inode->i_op = &proc_tid_attr_inode_operations;
			inode->i_fop = &proc_tid_attr_operations;
			break;
		case PROC_TGID_ATTR:
			inode->i_nlink = 2;
			inode->i_op = &proc_tgid_attr_inode_operations;
			inode->i_fop = &proc_tgid_attr_operations;
			break;
		case PROC_TID_ATTR_CURRENT:
		case PROC_TGID_ATTR_CURRENT:
		case PROC_TID_ATTR_PREV:
		case PROC_TGID_ATTR_PREV:
		case PROC_TID_ATTR_EXEC:
		case PROC_TGID_ATTR_EXEC:
		case PROC_TID_ATTR_FSCREATE:
		case PROC_TGID_ATTR_FSCREATE:
			inode->i_fop = &proc_pid_attr_operations;
			break;
#endif
#ifdef CONFIG_KALLSYMS
		case PROC_TID_WCHAN:
		case PROC_TGID_WCHAN:
			inode->i_fop = &proc_info_file_operations;
			ei->op.proc_read = proc_pid_wchan;
			break;
#endif
		default:
			printk("procfs: impossible type (%d)",p->type);
			iput(inode);
			return ERR_PTR(-EINVAL);
	}
	dentry->d_op = &pid_dentry_operations;
	d_add(dentry, inode);
	return NULL;

out:
	return ERR_PTR(error);
}

static struct dentry *proc_tgid_base_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd){
	return proc_pident_lookup(dir, dentry, tgid_base_stuff);
}

static struct dentry *proc_tid_base_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd){
	return proc_pident_lookup(dir, dentry, tid_base_stuff);
}

static struct file_operations proc_tgid_base_operations = {
	.read		= generic_read_dir,
	.readdir	= proc_tgid_base_readdir,
};

static struct file_operations proc_tid_base_operations = {
	.read		= generic_read_dir,
	.readdir	= proc_tid_base_readdir,
};

static struct inode_operations proc_tgid_base_inode_operations = {
	.lookup		= proc_tgid_base_lookup,
};

static struct inode_operations proc_tid_base_inode_operations = {
	.lookup		= proc_tid_base_lookup,
};

#ifdef CONFIG_SECURITY
static int proc_tgid_attr_readdir(struct file * filp,
			     void * dirent, filldir_t filldir)
{
	return proc_pident_readdir(filp,dirent,filldir,
				   tgid_attr_stuff,ARRAY_SIZE(tgid_attr_stuff));
}

static int proc_tid_attr_readdir(struct file * filp,
			     void * dirent, filldir_t filldir)
{
	return proc_pident_readdir(filp,dirent,filldir,
				   tid_attr_stuff,ARRAY_SIZE(tid_attr_stuff));
}

static struct file_operations proc_tgid_attr_operations = {
	.read		= generic_read_dir,
	.readdir	= proc_tgid_attr_readdir,
};

static struct file_operations proc_tid_attr_operations = {
	.read		= generic_read_dir,
	.readdir	= proc_tid_attr_readdir,
};

static struct dentry *proc_tgid_attr_lookup(struct inode *dir,
				struct dentry *dentry, struct nameidata *nd)
{
	return proc_pident_lookup(dir, dentry, tgid_attr_stuff);
}

static struct dentry *proc_tid_attr_lookup(struct inode *dir,
				struct dentry *dentry, struct nameidata *nd)
{
	return proc_pident_lookup(dir, dentry, tid_attr_stuff);
}

static struct inode_operations proc_tgid_attr_inode_operations = {
	.lookup		= proc_tgid_attr_lookup,
};

static struct inode_operations proc_tid_attr_inode_operations = {
	.lookup		= proc_tid_attr_lookup,
};
#endif

/*
 * /proc/self:
 */
static int proc_self_readlink(struct dentry *dentry, char __user *buffer,
			      int buflen)
{
	char tmp[30];
	sprintf(tmp, "%d", current->tgid);
	return vfs_readlink(dentry,buffer,buflen,tmp);
}

static int proc_self_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	char tmp[30];
	sprintf(tmp, "%d", current->tgid);
	return vfs_follow_link(nd,tmp);
}	

static struct inode_operations proc_self_inode_operations = {
	.readlink	= proc_self_readlink,
	.follow_link	= proc_self_follow_link,
};

/**
 * proc_pid_unhash -  Unhash /proc/<pid> entry from the dcache.
 * @p: task that should be flushed.
 *
 * Drops the /proc/<pid> dcache entry from the hash chains.
 *
 * Dropping /proc/<pid> entries and detach_pid must be synchroneous,
 * otherwise e.g. /proc/<pid>/exe might point to the wrong executable,
 * if the pid value is immediately reused. This is enforced by
 * - caller must acquire spin_lock(p->proc_lock)
 * - must be called before detach_pid()
 * - proc_pid_lookup acquires proc_lock, and checks that
 *   the target is not dead by looking at the attach count
 *   of PIDTYPE_PID.
 */

struct dentry *proc_pid_unhash(struct task_struct *p)
{
	struct dentry *proc_dentry;

	proc_dentry = p->proc_dentry;
	if (proc_dentry != NULL) {

		spin_lock(&dcache_lock);
		if (!d_unhashed(proc_dentry)) {
			dget_locked(proc_dentry);
			__d_drop(proc_dentry);
		} else
			proc_dentry = NULL;
		spin_unlock(&dcache_lock);
	}
	return proc_dentry;
}

/**
 * proc_pid_flush - recover memory used by stale /proc/<pid>/x entries
 * @proc_entry: directoy to prune.
 *
 * Shrink the /proc directory that was used by the just killed thread.
 */
	
void proc_pid_flush(struct dentry *proc_dentry)
{
	if(proc_dentry != NULL) {
		shrink_dcache_parent(proc_dentry);
		dput(proc_dentry);
	}
}

/* SMP-safe */
struct dentry *proc_pid_lookup(struct inode *dir, struct dentry * dentry, struct nameidata *nd)
{
	struct task_struct *task;
	struct inode *inode;
	struct proc_inode *ei;
	unsigned tgid;
	int died;

	if (dentry->d_name.len == 4 && !memcmp(dentry->d_name.name,"self",4)) {
		inode = new_inode(dir->i_sb);
		if (!inode)
			return ERR_PTR(-ENOMEM);
		ei = PROC_I(inode);
		inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
		inode->i_ino = fake_ino(0, PROC_TGID_INO);
		ei->pde = NULL;
		inode->i_mode = S_IFLNK|S_IRWXUGO;
		inode->i_uid = inode->i_gid = 0;
		inode->i_size = 64;
		inode->i_op = &proc_self_inode_operations;
		d_add(dentry, inode);
		return NULL;
	}
	tgid = name_to_int(dentry);
	if (tgid == ~0U)
		goto out;

	read_lock(&tasklist_lock);
	task = find_task_by_pid(tgid);
	if (task)
		get_task_struct(task);
	read_unlock(&tasklist_lock);
	if (!task)
		goto out;

	inode = proc_pid_make_inode(dir->i_sb, task, PROC_TGID_INO);


	if (!inode) {
		put_task_struct(task);
		goto out;
	}
	inode->i_mode = S_IFDIR|S_IRUGO|S_IXUGO;
	inode->i_op = &proc_tgid_base_inode_operations;
	inode->i_fop = &proc_tgid_base_operations;
	inode->i_nlink = 3;
	inode->i_flags|=S_IMMUTABLE;

	dentry->d_op = &pid_base_dentry_operations;

	died = 0;
	d_add(dentry, inode);
	spin_lock(&task->proc_lock);
	task->proc_dentry = dentry;
	if (!pid_alive(task)) {
		dentry = proc_pid_unhash(task);
		died = 1;
	}
	spin_unlock(&task->proc_lock);

	put_task_struct(task);
	if (died) {
		proc_pid_flush(dentry);
		goto out;
	}
	return NULL;
out:
	return ERR_PTR(-ENOENT);
}

/* SMP-safe */
static struct dentry *proc_task_lookup(struct inode *dir, struct dentry * dentry, struct nameidata *nd)
{
	struct task_struct *task;
	struct task_struct *leader = proc_task(dir);
	struct inode *inode;
	unsigned tid;

	tid = name_to_int(dentry);
	if (tid == ~0U)
		goto out;

	read_lock(&tasklist_lock);
	task = find_task_by_pid(tid);
	if (task)
		get_task_struct(task);
	read_unlock(&tasklist_lock);
	if (!task)
		goto out;
	if (leader->tgid != task->tgid)
		goto out_drop_task;

	inode = proc_pid_make_inode(dir->i_sb, task, PROC_TID_INO);


	if (!inode)
		goto out_drop_task;
	inode->i_mode = S_IFDIR|S_IRUGO|S_IXUGO;
	inode->i_op = &proc_tid_base_inode_operations;
	inode->i_fop = &proc_tid_base_operations;
	inode->i_nlink = 3;
	inode->i_flags|=S_IMMUTABLE;

	dentry->d_op = &pid_base_dentry_operations;

	d_add(dentry, inode);

	put_task_struct(task);
	return NULL;
out_drop_task:
	put_task_struct(task);
out:
	return ERR_PTR(-ENOENT);
}

#define PROC_NUMBUF 10
#define PROC_MAXPIDS 20

/*
 * Get a few tgid's to return for filldir - we need to hold the
 * tasklist lock while doing this, and we must release it before
 * we actually do the filldir itself, so we use a temp buffer..
 */
static int get_tgid_list(int index, unsigned long version, unsigned int *tgids)
{
	struct task_struct *p;
	int nr_tgids = 0;

	index--;
	read_lock(&tasklist_lock);
	p = NULL;
	if (version) {
		p = find_task_by_pid(version);
		if (!thread_group_leader(p))
			p = NULL;
	}

	if (p)
		index = 0;
	else
		p = next_task(&init_task);

	for ( ; p != &init_task; p = next_task(p)) {
		int tgid = p->pid;
		if (!pid_alive(p))
			continue;
		if (--index >= 0)
			continue;
		tgids[nr_tgids] = tgid;
		nr_tgids++;
		if (nr_tgids >= PROC_MAXPIDS)
			break;
	}
	read_unlock(&tasklist_lock);
	return nr_tgids;
}

/*
 * Get a few tid's to return for filldir - we need to hold the
 * tasklist lock while doing this, and we must release it before
 * we actually do the filldir itself, so we use a temp buffer..
 */
static int get_tid_list(int index, unsigned int *tids, struct inode *dir)
{
	struct task_struct *leader_task = proc_task(dir);
	struct task_struct *task = leader_task;
	int nr_tids = 0;

	index -= 2;
	read_lock(&tasklist_lock);
	/*
	 * The starting point task (leader_task) might be an already
	 * unlinked task, which cannot be used to access the task-list
	 * via next_thread().
	 */
	if (pid_alive(task)) do {
		int tid = task->pid;

		if (--index >= 0)
			continue;
		tids[nr_tids] = tid;
		nr_tids++;
		if (nr_tids >= PROC_MAXPIDS)
			break;
	} while ((task = next_thread(task)) != leader_task);
	read_unlock(&tasklist_lock);
	return nr_tids;
}

/* for the /proc/ directory itself, after non-process stuff has been done */
int proc_pid_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	unsigned int tgid_array[PROC_MAXPIDS];
	char buf[PROC_NUMBUF];
	unsigned int nr = filp->f_pos - FIRST_PROCESS_ENTRY;
	unsigned int nr_tgids, i;

	if (!nr) {
		ino_t ino = fake_ino(0,PROC_TGID_INO);
		if (filldir(dirent, "self", 4, filp->f_pos, ino, DT_LNK) < 0)
			return 0;
		filp->f_pos++;
		nr++;
	}

	/*
	 * f_version caches the last tgid which was returned from readdir
	 */
	nr_tgids = get_tgid_list(nr, filp->f_version, tgid_array);

	for (i = 0; i < nr_tgids; i++) {
		int tgid = tgid_array[i];
		ino_t ino = fake_ino(tgid,PROC_TGID_INO);
		unsigned long j = PROC_NUMBUF;

		do
			buf[--j] = '0' + (tgid % 10);
		while ((tgid /= 10) != 0);

		if (filldir(dirent, buf+j, PROC_NUMBUF-j, filp->f_pos, ino, DT_DIR) < 0) {
			filp->f_version = tgid;
			break;
		}
		filp->f_pos++;
	}
	return 0;
}

/* for the /proc/TGID/task/ directories */
static int proc_task_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	unsigned int tid_array[PROC_MAXPIDS];
	char buf[PROC_NUMBUF];
	unsigned int nr_tids, i;
	struct dentry *dentry = filp->f_dentry;
	struct inode *inode = dentry->d_inode;
	int retval = -ENOENT;
	ino_t ino;
	unsigned long pos = filp->f_pos;  /* avoiding "long long" filp->f_pos */

	if (!pid_alive(proc_task(inode)))
		goto out;
	retval = 0;

	switch (pos) {
	case 0:
		ino = inode->i_ino;
		if (filldir(dirent, ".", 1, pos, ino, DT_DIR) < 0)
			goto out;
		pos++;
		/* fall through */
	case 1:
		ino = parent_ino(dentry);
		if (filldir(dirent, "..", 2, pos, ino, DT_DIR) < 0)
			goto out;
		pos++;
		/* fall through */
	}

	nr_tids = get_tid_list(pos, tid_array, inode);

	for (i = 0; i < nr_tids; i++) {
		unsigned long j = PROC_NUMBUF;
		int tid = tid_array[i];

		ino = fake_ino(tid,PROC_TID_INO);

		do
			buf[--j] = '0' + (tid % 10);
		while ((tid /= 10) != 0);

		if (filldir(dirent, buf+j, PROC_NUMBUF-j, pos, ino, DT_DIR) < 0)
			break;
		pos++;
	}
out:
	filp->f_pos = pos;
	return retval;
}
