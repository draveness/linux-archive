/*
 * Stub functions for the default security function pointers in case no
 * security model is loaded.
 *
 * Copyright (C) 2001 WireX Communications, Inc <chris@wirex.com>
 * Copyright (C) 2001-2002  Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2001 Networks Associates Technology, Inc <ssmalley@nai.com>
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 */

#undef DEBUG

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/security.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <net/sock.h>
#include <linux/xattr.h>
#include <linux/hugetlb.h>
#include <linux/ptrace.h>
#include <linux/file.h>

static int dummy_ptrace (struct task_struct *parent, struct task_struct *child)
{
	return 0;
}

static int dummy_capget (struct task_struct *target, kernel_cap_t * effective,
			 kernel_cap_t * inheritable, kernel_cap_t * permitted)
{
	*effective = *inheritable = *permitted = 0;
	if (!issecure(SECURE_NOROOT)) {
		if (target->euid == 0) {
			*permitted |= (~0 & ~CAP_FS_MASK);
			*effective |= (~0 & ~CAP_TO_MASK(CAP_SETPCAP) & ~CAP_FS_MASK);
		}
		if (target->fsuid == 0) {
			*permitted |= CAP_FS_MASK;
			*effective |= CAP_FS_MASK;
		}
	}
	return 0;
}

static int dummy_capset_check (struct task_struct *target,
			       kernel_cap_t * effective,
			       kernel_cap_t * inheritable,
			       kernel_cap_t * permitted)
{
	return -EPERM;
}

static void dummy_capset_set (struct task_struct *target,
			      kernel_cap_t * effective,
			      kernel_cap_t * inheritable,
			      kernel_cap_t * permitted)
{
	return;
}

static int dummy_acct (struct file *file)
{
	return 0;
}

static int dummy_capable (struct task_struct *tsk, int cap)
{
	if (cap_is_fs_cap (cap) ? tsk->fsuid == 0 : tsk->euid == 0)
		/* capability granted */
		return 0;

	/* capability denied */
	return -EPERM;
}

static int dummy_sysctl (ctl_table * table, int op)
{
	return 0;
}

static int dummy_quotactl (int cmds, int type, int id, struct super_block *sb)
{
	return 0;
}

static int dummy_quota_on (struct file *f)
{
	return 0;
}

static int dummy_syslog (int type)
{
	if ((type != 3 && type != 10) && current->euid)
		return -EPERM;
	return 0;
}

/*
 * Check that a process has enough memory to allocate a new virtual
 * mapping. 0 means there is enough memory for the allocation to
 * succeed and -ENOMEM implies there is not.
 *
 * We currently support three overcommit policies, which are set via the
 * vm.overcommit_memory sysctl.  See Documentation/vm/overcommit-accounting
 */
static int dummy_vm_enough_memory(long pages)
{
	unsigned long free, allowed;

	vm_acct_memory(pages);

	/*
	 * Sometimes we want to use more memory than we have
	 */
	if (sysctl_overcommit_memory == 1)
		return 0;

	if (sysctl_overcommit_memory == 0) {
		free = get_page_cache_size();
		free += nr_free_pages();
		free += nr_swap_pages;

		/*
		 * Any slabs which are created with the
		 * SLAB_RECLAIM_ACCOUNT flag claim to have contents
		 * which are reclaimable, under pressure.  The dentry
		 * cache and most inode caches should fall into this
		 */
		free += atomic_read(&slab_reclaim_pages);

		/*
		 * Leave the last 3% for root
		 */
		if (current->euid)
			free -= free / 32;

		if (free > pages)
			return 0;
		vm_unacct_memory(pages);
		return -ENOMEM;
	}

	allowed = (totalram_pages - hugetlb_total_pages())
		* sysctl_overcommit_ratio / 100;
	allowed += total_swap_pages;

	if (atomic_read(&vm_committed_space) < allowed)
		return 0;

	vm_unacct_memory(pages);

	return -ENOMEM;
}

static int dummy_bprm_alloc_security (struct linux_binprm *bprm)
{
	return 0;
}

static void dummy_bprm_free_security (struct linux_binprm *bprm)
{
	return;
}

static void dummy_bprm_apply_creds (struct linux_binprm *bprm, int unsafe)
{
	if (bprm->e_uid != current->uid || bprm->e_gid != current->gid) {
		current->mm->dumpable = 0;

		if ((unsafe & ~LSM_UNSAFE_PTRACE_CAP) && !capable(CAP_SETUID)) {
			bprm->e_uid = current->uid;
			bprm->e_gid = current->gid;
		}
	}

	current->suid = current->euid = current->fsuid = bprm->e_uid;
	current->sgid = current->egid = current->fsgid = bprm->e_gid;
}

static int dummy_bprm_set_security (struct linux_binprm *bprm)
{
	return 0;
}

static int dummy_bprm_check_security (struct linux_binprm *bprm)
{
	return 0;
}

static int dummy_bprm_secureexec (struct linux_binprm *bprm)
{
	/* The new userland will simply use the value provided
	   in the AT_SECURE field to decide whether secure mode
	   is required.  Hence, this logic is required to preserve
	   the legacy decision algorithm used by the old userland. */
	return (current->euid != current->uid ||
		current->egid != current->gid);
}

static int dummy_sb_alloc_security (struct super_block *sb)
{
	return 0;
}

static void dummy_sb_free_security (struct super_block *sb)
{
	return;
}

static int dummy_sb_copy_data (struct file_system_type *type,
			       void *orig, void *copy)
{
	return 0;
}

static int dummy_sb_kern_mount (struct super_block *sb, void *data)
{
	return 0;
}

static int dummy_sb_statfs (struct super_block *sb)
{
	return 0;
}

static int dummy_sb_mount (char *dev_name, struct nameidata *nd, char *type,
			   unsigned long flags, void *data)
{
	return 0;
}

static int dummy_sb_check_sb (struct vfsmount *mnt, struct nameidata *nd)
{
	return 0;
}

static int dummy_sb_umount (struct vfsmount *mnt, int flags)
{
	return 0;
}

static void dummy_sb_umount_close (struct vfsmount *mnt)
{
	return;
}

static void dummy_sb_umount_busy (struct vfsmount *mnt)
{
	return;
}

static void dummy_sb_post_remount (struct vfsmount *mnt, unsigned long flags,
				   void *data)
{
	return;
}


static void dummy_sb_post_mountroot (void)
{
	return;
}

static void dummy_sb_post_addmount (struct vfsmount *mnt, struct nameidata *nd)
{
	return;
}

static int dummy_sb_pivotroot (struct nameidata *old_nd, struct nameidata *new_nd)
{
	return 0;
}

static void dummy_sb_post_pivotroot (struct nameidata *old_nd, struct nameidata *new_nd)
{
	return;
}

static int dummy_inode_alloc_security (struct inode *inode)
{
	return 0;
}

static void dummy_inode_free_security (struct inode *inode)
{
	return;
}

static int dummy_inode_create (struct inode *inode, struct dentry *dentry,
			       int mask)
{
	return 0;
}

static void dummy_inode_post_create (struct inode *inode, struct dentry *dentry,
				     int mask)
{
	return;
}

static int dummy_inode_link (struct dentry *old_dentry, struct inode *inode,
			     struct dentry *new_dentry)
{
	return 0;
}

static void dummy_inode_post_link (struct dentry *old_dentry,
				   struct inode *inode,
				   struct dentry *new_dentry)
{
	return;
}

static int dummy_inode_unlink (struct inode *inode, struct dentry *dentry)
{
	return 0;
}

static int dummy_inode_symlink (struct inode *inode, struct dentry *dentry,
				const char *name)
{
	return 0;
}

static void dummy_inode_post_symlink (struct inode *inode,
				      struct dentry *dentry, const char *name)
{
	return;
}

static int dummy_inode_mkdir (struct inode *inode, struct dentry *dentry,
			      int mask)
{
	return 0;
}

static void dummy_inode_post_mkdir (struct inode *inode, struct dentry *dentry,
				    int mask)
{
	return;
}

static int dummy_inode_rmdir (struct inode *inode, struct dentry *dentry)
{
	return 0;
}

static int dummy_inode_mknod (struct inode *inode, struct dentry *dentry,
			      int mode, dev_t dev)
{
	return 0;
}

static void dummy_inode_post_mknod (struct inode *inode, struct dentry *dentry,
				    int mode, dev_t dev)
{
	return;
}

static int dummy_inode_rename (struct inode *old_inode,
			       struct dentry *old_dentry,
			       struct inode *new_inode,
			       struct dentry *new_dentry)
{
	return 0;
}

static void dummy_inode_post_rename (struct inode *old_inode,
				     struct dentry *old_dentry,
				     struct inode *new_inode,
				     struct dentry *new_dentry)
{
	return;
}

static int dummy_inode_readlink (struct dentry *dentry)
{
	return 0;
}

static int dummy_inode_follow_link (struct dentry *dentry,
				    struct nameidata *nameidata)
{
	return 0;
}

static int dummy_inode_permission (struct inode *inode, int mask, struct nameidata *nd)
{
	return 0;
}

static int dummy_inode_setattr (struct dentry *dentry, struct iattr *iattr)
{
	return 0;
}

static int dummy_inode_getattr (struct vfsmount *mnt, struct dentry *dentry)
{
	return 0;
}

static void dummy_inode_delete (struct inode *ino)
{
	return;
}

static int dummy_inode_setxattr (struct dentry *dentry, char *name, void *value,
				size_t size, int flags)
{
	if (!strncmp(name, XATTR_SECURITY_PREFIX,
		     sizeof(XATTR_SECURITY_PREFIX) - 1) &&
	    !capable(CAP_SYS_ADMIN))
		return -EPERM;
	return 0;
}

static void dummy_inode_post_setxattr (struct dentry *dentry, char *name, void *value,
				       size_t size, int flags)
{
}

static int dummy_inode_getxattr (struct dentry *dentry, char *name)
{
	return 0;
}

static int dummy_inode_listxattr (struct dentry *dentry)
{
	return 0;
}

static int dummy_inode_removexattr (struct dentry *dentry, char *name)
{
	if (!strncmp(name, XATTR_SECURITY_PREFIX,
		     sizeof(XATTR_SECURITY_PREFIX) - 1) &&
	    !capable(CAP_SYS_ADMIN))
		return -EPERM;
	return 0;
}

static int dummy_inode_getsecurity(struct dentry *dentry, const char *name, void *buffer, size_t size)
{
	return -EOPNOTSUPP;
}

static int dummy_inode_setsecurity(struct dentry *dentry, const char *name, const void *value, size_t size, int flags) 
{
	return -EOPNOTSUPP;
}

static int dummy_inode_listsecurity(struct dentry *dentry, char *buffer)
{
	return 0;
}

static int dummy_file_permission (struct file *file, int mask)
{
	return 0;
}

static int dummy_file_alloc_security (struct file *file)
{
	return 0;
}

static void dummy_file_free_security (struct file *file)
{
	return;
}

static int dummy_file_ioctl (struct file *file, unsigned int command,
			     unsigned long arg)
{
	return 0;
}

static int dummy_file_mmap (struct file *file, unsigned long prot,
			    unsigned long flags)
{
	return 0;
}

static int dummy_file_mprotect (struct vm_area_struct *vma, unsigned long prot)
{
	return 0;
}

static int dummy_file_lock (struct file *file, unsigned int cmd)
{
	return 0;
}

static int dummy_file_fcntl (struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	return 0;
}

static int dummy_file_set_fowner (struct file *file)
{
	return 0;
}

static int dummy_file_send_sigiotask (struct task_struct *tsk,
				      struct fown_struct *fown, int fd,
				      int reason)
{
	return 0;
}

static int dummy_file_receive (struct file *file)
{
	return 0;
}

static int dummy_task_create (unsigned long clone_flags)
{
	return 0;
}

static int dummy_task_alloc_security (struct task_struct *p)
{
	return 0;
}

static void dummy_task_free_security (struct task_struct *p)
{
	return;
}

static int dummy_task_setuid (uid_t id0, uid_t id1, uid_t id2, int flags)
{
	return 0;
}

static int dummy_task_post_setuid (uid_t id0, uid_t id1, uid_t id2, int flags)
{
	return 0;
}

static int dummy_task_setgid (gid_t id0, gid_t id1, gid_t id2, int flags)
{
	return 0;
}

static int dummy_task_setpgid (struct task_struct *p, pid_t pgid)
{
	return 0;
}

static int dummy_task_getpgid (struct task_struct *p)
{
	return 0;
}

static int dummy_task_getsid (struct task_struct *p)
{
	return 0;
}

static int dummy_task_setgroups (struct group_info *group_info)
{
	return 0;
}

static int dummy_task_setnice (struct task_struct *p, int nice)
{
	return 0;
}

static int dummy_task_setrlimit (unsigned int resource, struct rlimit *new_rlim)
{
	return 0;
}

static int dummy_task_setscheduler (struct task_struct *p, int policy,
				    struct sched_param *lp)
{
	return 0;
}

static int dummy_task_getscheduler (struct task_struct *p)
{
	return 0;
}

static int dummy_task_wait (struct task_struct *p)
{
	return 0;
}

static int dummy_task_kill (struct task_struct *p, struct siginfo *info,
			    int sig)
{
	return 0;
}

static int dummy_task_prctl (int option, unsigned long arg2, unsigned long arg3,
			     unsigned long arg4, unsigned long arg5)
{
	return 0;
}

static void dummy_task_reparent_to_init (struct task_struct *p)
{
	p->euid = p->fsuid = 0;
	return;
}

static void dummy_task_to_inode(struct task_struct *p, struct inode *inode)
{ }

static int dummy_ipc_permission (struct kern_ipc_perm *ipcp, short flag)
{
	return 0;
}

static int dummy_msg_msg_alloc_security (struct msg_msg *msg)
{
	return 0;
}

static void dummy_msg_msg_free_security (struct msg_msg *msg)
{
	return;
}

static int dummy_msg_queue_alloc_security (struct msg_queue *msq)
{
	return 0;
}

static void dummy_msg_queue_free_security (struct msg_queue *msq)
{
	return;
}

static int dummy_msg_queue_associate (struct msg_queue *msq, 
				      int msqflg)
{
	return 0;
}

static int dummy_msg_queue_msgctl (struct msg_queue *msq, int cmd)
{
	return 0;
}

static int dummy_msg_queue_msgsnd (struct msg_queue *msq, struct msg_msg *msg,
				   int msgflg)
{
	return 0;
}

static int dummy_msg_queue_msgrcv (struct msg_queue *msq, struct msg_msg *msg,
				   struct task_struct *target, long type,
				   int mode)
{
	return 0;
}

static int dummy_shm_alloc_security (struct shmid_kernel *shp)
{
	return 0;
}

static void dummy_shm_free_security (struct shmid_kernel *shp)
{
	return;
}

static int dummy_shm_associate (struct shmid_kernel *shp, int shmflg)
{
	return 0;
}

static int dummy_shm_shmctl (struct shmid_kernel *shp, int cmd)
{
	return 0;
}

static int dummy_shm_shmat (struct shmid_kernel *shp, char __user *shmaddr,
			    int shmflg)
{
	return 0;
}

static int dummy_sem_alloc_security (struct sem_array *sma)
{
	return 0;
}

static void dummy_sem_free_security (struct sem_array *sma)
{
	return;
}

static int dummy_sem_associate (struct sem_array *sma, int semflg)
{
	return 0;
}

static int dummy_sem_semctl (struct sem_array *sma, int cmd)
{
	return 0;
}

static int dummy_sem_semop (struct sem_array *sma, 
			    struct sembuf *sops, unsigned nsops, int alter)
{
	return 0;
}

static int dummy_netlink_send (struct sock *sk, struct sk_buff *skb)
{
	if (current->euid == 0)
		cap_raise (NETLINK_CB (skb).eff_cap, CAP_NET_ADMIN);
	else
		NETLINK_CB (skb).eff_cap = 0;
	return 0;
}

static int dummy_netlink_recv (struct sk_buff *skb)
{
	if (!cap_raised (NETLINK_CB (skb).eff_cap, CAP_NET_ADMIN))
		return -EPERM;
	return 0;
}

#ifdef CONFIG_SECURITY_NETWORK
static int dummy_unix_stream_connect (struct socket *sock,
				      struct socket *other,
				      struct sock *newsk)
{
	return 0;
}

static int dummy_unix_may_send (struct socket *sock,
				struct socket *other)
{
	return 0;
}

static int dummy_socket_create (int family, int type,
				int protocol, int kern)
{
	return 0;
}

static void dummy_socket_post_create (struct socket *sock, int family, int type,
				      int protocol, int kern)
{
	return;
}

static int dummy_socket_bind (struct socket *sock, struct sockaddr *address,
			      int addrlen)
{
	return 0;
}

static int dummy_socket_connect (struct socket *sock, struct sockaddr *address,
				 int addrlen)
{
	return 0;
}

static int dummy_socket_listen (struct socket *sock, int backlog)
{
	return 0;
}

static int dummy_socket_accept (struct socket *sock, struct socket *newsock)
{
	return 0;
}

static void dummy_socket_post_accept (struct socket *sock, 
				      struct socket *newsock)
{
	return;
}

static int dummy_socket_sendmsg (struct socket *sock, struct msghdr *msg,
				 int size)
{
	return 0;
}

static int dummy_socket_recvmsg (struct socket *sock, struct msghdr *msg,
				 int size, int flags)
{
	return 0;
}

static int dummy_socket_getsockname (struct socket *sock)
{
	return 0;
}

static int dummy_socket_getpeername (struct socket *sock)
{
	return 0;
}

static int dummy_socket_setsockopt (struct socket *sock, int level, int optname)
{
	return 0;
}

static int dummy_socket_getsockopt (struct socket *sock, int level, int optname)
{
	return 0;
}

static int dummy_socket_shutdown (struct socket *sock, int how)
{
	return 0;
}

static int dummy_socket_sock_rcv_skb (struct sock *sk, struct sk_buff *skb)
{
	return 0;
}

static int dummy_socket_getpeersec(struct socket *sock, char __user *optval,
				   int __user *optlen, unsigned len)
{
	return -ENOPROTOOPT;
}

static inline int dummy_sk_alloc_security (struct sock *sk, int family, int priority)
{
	return 0;
}

static inline void dummy_sk_free_security (struct sock *sk)
{
}
#endif	/* CONFIG_SECURITY_NETWORK */

static int dummy_register_security (const char *name, struct security_operations *ops)
{
	return -EINVAL;
}

static int dummy_unregister_security (const char *name, struct security_operations *ops)
{
	return -EINVAL;
}

static void dummy_d_instantiate (struct dentry *dentry, struct inode *inode)
{
	return;
}

static int dummy_getprocattr(struct task_struct *p, char *name, void *value, size_t size)
{
	return -EINVAL;
}

static int dummy_setprocattr(struct task_struct *p, char *name, void *value, size_t size)
{
	return -EINVAL;
}


struct security_operations dummy_security_ops;

#define set_to_dummy_if_null(ops, function)				\
	do {								\
		if (!ops->function) {					\
			ops->function = dummy_##function;		\
			pr_debug("Had to override the " #function	\
				 " security operation with the dummy one.\n");\
			}						\
	} while (0)

void security_fixup_ops (struct security_operations *ops)
{
	set_to_dummy_if_null(ops, ptrace);
	set_to_dummy_if_null(ops, capget);
	set_to_dummy_if_null(ops, capset_check);
	set_to_dummy_if_null(ops, capset_set);
	set_to_dummy_if_null(ops, acct);
	set_to_dummy_if_null(ops, capable);
	set_to_dummy_if_null(ops, quotactl);
	set_to_dummy_if_null(ops, quota_on);
	set_to_dummy_if_null(ops, sysctl);
	set_to_dummy_if_null(ops, syslog);
	set_to_dummy_if_null(ops, vm_enough_memory);
	set_to_dummy_if_null(ops, bprm_alloc_security);
	set_to_dummy_if_null(ops, bprm_free_security);
	set_to_dummy_if_null(ops, bprm_apply_creds);
	set_to_dummy_if_null(ops, bprm_set_security);
	set_to_dummy_if_null(ops, bprm_check_security);
	set_to_dummy_if_null(ops, bprm_secureexec);
	set_to_dummy_if_null(ops, sb_alloc_security);
	set_to_dummy_if_null(ops, sb_free_security);
	set_to_dummy_if_null(ops, sb_copy_data);
	set_to_dummy_if_null(ops, sb_kern_mount);
	set_to_dummy_if_null(ops, sb_statfs);
	set_to_dummy_if_null(ops, sb_mount);
	set_to_dummy_if_null(ops, sb_check_sb);
	set_to_dummy_if_null(ops, sb_umount);
	set_to_dummy_if_null(ops, sb_umount_close);
	set_to_dummy_if_null(ops, sb_umount_busy);
	set_to_dummy_if_null(ops, sb_post_remount);
	set_to_dummy_if_null(ops, sb_post_mountroot);
	set_to_dummy_if_null(ops, sb_post_addmount);
	set_to_dummy_if_null(ops, sb_pivotroot);
	set_to_dummy_if_null(ops, sb_post_pivotroot);
	set_to_dummy_if_null(ops, inode_alloc_security);
	set_to_dummy_if_null(ops, inode_free_security);
	set_to_dummy_if_null(ops, inode_create);
	set_to_dummy_if_null(ops, inode_post_create);
	set_to_dummy_if_null(ops, inode_link);
	set_to_dummy_if_null(ops, inode_post_link);
	set_to_dummy_if_null(ops, inode_unlink);
	set_to_dummy_if_null(ops, inode_symlink);
	set_to_dummy_if_null(ops, inode_post_symlink);
	set_to_dummy_if_null(ops, inode_mkdir);
	set_to_dummy_if_null(ops, inode_post_mkdir);
	set_to_dummy_if_null(ops, inode_rmdir);
	set_to_dummy_if_null(ops, inode_mknod);
	set_to_dummy_if_null(ops, inode_post_mknod);
	set_to_dummy_if_null(ops, inode_rename);
	set_to_dummy_if_null(ops, inode_post_rename);
	set_to_dummy_if_null(ops, inode_readlink);
	set_to_dummy_if_null(ops, inode_follow_link);
	set_to_dummy_if_null(ops, inode_permission);
	set_to_dummy_if_null(ops, inode_setattr);
	set_to_dummy_if_null(ops, inode_getattr);
	set_to_dummy_if_null(ops, inode_delete);
	set_to_dummy_if_null(ops, inode_setxattr);
	set_to_dummy_if_null(ops, inode_post_setxattr);
	set_to_dummy_if_null(ops, inode_getxattr);
	set_to_dummy_if_null(ops, inode_listxattr);
	set_to_dummy_if_null(ops, inode_removexattr);
	set_to_dummy_if_null(ops, inode_getsecurity);
	set_to_dummy_if_null(ops, inode_setsecurity);
	set_to_dummy_if_null(ops, inode_listsecurity);
	set_to_dummy_if_null(ops, file_permission);
	set_to_dummy_if_null(ops, file_alloc_security);
	set_to_dummy_if_null(ops, file_free_security);
	set_to_dummy_if_null(ops, file_ioctl);
	set_to_dummy_if_null(ops, file_mmap);
	set_to_dummy_if_null(ops, file_mprotect);
	set_to_dummy_if_null(ops, file_lock);
	set_to_dummy_if_null(ops, file_fcntl);
	set_to_dummy_if_null(ops, file_set_fowner);
	set_to_dummy_if_null(ops, file_send_sigiotask);
	set_to_dummy_if_null(ops, file_receive);
	set_to_dummy_if_null(ops, task_create);
	set_to_dummy_if_null(ops, task_alloc_security);
	set_to_dummy_if_null(ops, task_free_security);
	set_to_dummy_if_null(ops, task_setuid);
	set_to_dummy_if_null(ops, task_post_setuid);
	set_to_dummy_if_null(ops, task_setgid);
	set_to_dummy_if_null(ops, task_setpgid);
	set_to_dummy_if_null(ops, task_getpgid);
	set_to_dummy_if_null(ops, task_getsid);
	set_to_dummy_if_null(ops, task_setgroups);
	set_to_dummy_if_null(ops, task_setnice);
	set_to_dummy_if_null(ops, task_setrlimit);
	set_to_dummy_if_null(ops, task_setscheduler);
	set_to_dummy_if_null(ops, task_getscheduler);
	set_to_dummy_if_null(ops, task_wait);
	set_to_dummy_if_null(ops, task_kill);
	set_to_dummy_if_null(ops, task_prctl);
	set_to_dummy_if_null(ops, task_reparent_to_init);
 	set_to_dummy_if_null(ops, task_to_inode);
	set_to_dummy_if_null(ops, ipc_permission);
	set_to_dummy_if_null(ops, msg_msg_alloc_security);
	set_to_dummy_if_null(ops, msg_msg_free_security);
	set_to_dummy_if_null(ops, msg_queue_alloc_security);
	set_to_dummy_if_null(ops, msg_queue_free_security);
	set_to_dummy_if_null(ops, msg_queue_associate);
	set_to_dummy_if_null(ops, msg_queue_msgctl);
	set_to_dummy_if_null(ops, msg_queue_msgsnd);
	set_to_dummy_if_null(ops, msg_queue_msgrcv);
	set_to_dummy_if_null(ops, shm_alloc_security);
	set_to_dummy_if_null(ops, shm_free_security);
	set_to_dummy_if_null(ops, shm_associate);
	set_to_dummy_if_null(ops, shm_shmctl);
	set_to_dummy_if_null(ops, shm_shmat);
	set_to_dummy_if_null(ops, sem_alloc_security);
	set_to_dummy_if_null(ops, sem_free_security);
	set_to_dummy_if_null(ops, sem_associate);
	set_to_dummy_if_null(ops, sem_semctl);
	set_to_dummy_if_null(ops, sem_semop);
	set_to_dummy_if_null(ops, netlink_send);
	set_to_dummy_if_null(ops, netlink_recv);
	set_to_dummy_if_null(ops, register_security);
	set_to_dummy_if_null(ops, unregister_security);
	set_to_dummy_if_null(ops, d_instantiate);
 	set_to_dummy_if_null(ops, getprocattr);
 	set_to_dummy_if_null(ops, setprocattr);
#ifdef CONFIG_SECURITY_NETWORK
	set_to_dummy_if_null(ops, unix_stream_connect);
	set_to_dummy_if_null(ops, unix_may_send);
	set_to_dummy_if_null(ops, socket_create);
	set_to_dummy_if_null(ops, socket_post_create);
	set_to_dummy_if_null(ops, socket_bind);
	set_to_dummy_if_null(ops, socket_connect);
	set_to_dummy_if_null(ops, socket_listen);
	set_to_dummy_if_null(ops, socket_accept);
	set_to_dummy_if_null(ops, socket_post_accept);
	set_to_dummy_if_null(ops, socket_sendmsg);
	set_to_dummy_if_null(ops, socket_recvmsg);
	set_to_dummy_if_null(ops, socket_getsockname);
	set_to_dummy_if_null(ops, socket_getpeername);
	set_to_dummy_if_null(ops, socket_setsockopt);
	set_to_dummy_if_null(ops, socket_getsockopt);
	set_to_dummy_if_null(ops, socket_shutdown);
	set_to_dummy_if_null(ops, socket_sock_rcv_skb);
	set_to_dummy_if_null(ops, socket_getpeersec);
	set_to_dummy_if_null(ops, sk_alloc_security);
	set_to_dummy_if_null(ops, sk_free_security);
#endif	/* CONFIG_SECURITY_NETWORK */
}

