/*
 * sysctl.c: General linux system control interface
 *
 * Begun 24 March 1995, Stephen Tweedie
 * Added /proc support, Dec 1995
 * Added bdflush entry and intvec min/max checking, 2/23/96, Tom Dyas.
 * Added hooks for /proc/sys/net (minor, minor patch), 96/4/1, Mike Shaver.
 * Added kernel/java-{interpreter,appletviewer}, 96/5/10, Mike Shaver.
 * Dynamic registration fixes, Stephen Tweedie.
 * Added kswapd-interval, ctrl-alt-del, printk stuff, 1/8/97, Chris Horn.
 * Made sysctl support optional via CONFIG_SYSCTL, 1/10/97, Chris
 *  Horn.
 * Added proc_doulongvec_ms_jiffies_minmax, 09/08/99, Carlos H. Bauer.
 * Added proc_doulongvec_minmax, 09/08/99, Carlos H. Bauer.
 * Changed linked lists to use list.h instead of lists.h, 02/24/00, Bill
 *  Wendling.
 * The list_for_each() macro wasn't appropriate for the sysctl loop.
 *  Removed it and replaced it with older style, 03/23/00, Bill Wendling
 */

#include <linux/config.h>
#include <linux/malloc.h>
#include <linux/sysctl.h>
#include <linux/swapctl.h>
#include <linux/proc_fs.h>
#include <linux/ctype.h>
#include <linux/utsname.h>
#include <linux/capability.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/sysrq.h>
#include <linux/highuid.h>

#include <asm/uaccess.h>

#ifdef CONFIG_ROOT_NFS
#include <linux/nfs_fs.h>
#endif

#if defined(CONFIG_SYSCTL)

/* External variables not in a header file. */
extern int panic_timeout;
extern int C_A_D;
extern int bdf_prm[], bdflush_min[], bdflush_max[];
extern int sysctl_overcommit_memory;
extern int max_threads;
extern int nr_queued_signals, max_queued_signals;
extern int sysrq_enabled;

/* this is needed for the proc_dointvec_minmax for [fs_]overflow UID and GID */
static int maxolduid = 65535;
static int minolduid;

#ifdef CONFIG_KMOD
extern char modprobe_path[];
#endif
#ifdef CONFIG_HOTPLUG
extern char hotplug_path[];
#endif
#ifdef CONFIG_CHR_DEV_SG
extern int sg_big_buff;
#endif
#ifdef CONFIG_SYSVIPC
extern size_t shm_ctlmax;
extern size_t shm_ctlall;
extern int shm_ctlmni;
extern int msg_ctlmax;
extern int msg_ctlmnb;
extern int msg_ctlmni;
extern int sem_ctls[];
#endif

#ifdef __sparc__
extern char reboot_command [];
extern int stop_a_enabled;
#endif
#ifdef __powerpc__
extern unsigned long htab_reclaim_on, zero_paged_on, powersave_nap;
int proc_dol2crvec(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp);
#endif

#ifdef CONFIG_BSD_PROCESS_ACCT
extern int acct_parm[];
#endif

extern int pgt_cache_water[];

static int parse_table(int *, int, void *, size_t *, void *, size_t,
		       ctl_table *, void **);
static int proc_doutsstring(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp);

static ctl_table root_table[];
static struct ctl_table_header root_table_header =
	{ root_table, LIST_HEAD_INIT(root_table_header.ctl_entry) };

static ctl_table kern_table[];
static ctl_table vm_table[];
#ifdef CONFIG_NET
extern ctl_table net_table[];
#endif
static ctl_table proc_table[];
static ctl_table fs_table[];
static ctl_table debug_table[];
static ctl_table dev_table[];
extern ctl_table random_table[];

/* /proc declarations: */

#ifdef CONFIG_PROC_FS

static ssize_t proc_readsys(struct file *, char *, size_t, loff_t *);
static ssize_t proc_writesys(struct file *, const char *, size_t, loff_t *);
static int proc_sys_permission(struct inode *, int);

struct file_operations proc_sys_file_operations = {
	read:		proc_readsys,
	write:		proc_writesys,
};

static struct inode_operations proc_sys_inode_operations = {
	permission:	proc_sys_permission,
};

extern struct proc_dir_entry *proc_sys_root;

static void register_proc_table(ctl_table *, struct proc_dir_entry *);
static void unregister_proc_table(ctl_table *, struct proc_dir_entry *);
#endif

extern int inodes_stat[];
extern int dentry_stat[];

/* The default sysctl tables: */

static ctl_table root_table[] = {
	{CTL_KERN, "kernel", NULL, 0, 0555, kern_table},
	{CTL_VM, "vm", NULL, 0, 0555, vm_table},
#ifdef CONFIG_NET
	{CTL_NET, "net", NULL, 0, 0555, net_table},
#endif
	{CTL_PROC, "proc", NULL, 0, 0555, proc_table},
	{CTL_FS, "fs", NULL, 0, 0555, fs_table},
	{CTL_DEBUG, "debug", NULL, 0, 0555, debug_table},
        {CTL_DEV, "dev", NULL, 0, 0555, dev_table},
	{0}
};

static ctl_table kern_table[] = {
	{KERN_OSTYPE, "ostype", system_utsname.sysname, 64,
	 0444, NULL, &proc_doutsstring, &sysctl_string},
	{KERN_OSRELEASE, "osrelease", system_utsname.release, 64,
	 0444, NULL, &proc_doutsstring, &sysctl_string},
	{KERN_VERSION, "version", system_utsname.version, 64,
	 0444, NULL, &proc_doutsstring, &sysctl_string},
	{KERN_NODENAME, "hostname", system_utsname.nodename, 64,
	 0644, NULL, &proc_doutsstring, &sysctl_string},
	{KERN_DOMAINNAME, "domainname", system_utsname.domainname, 64,
	 0644, NULL, &proc_doutsstring, &sysctl_string},
	{KERN_PANIC, "panic", &panic_timeout, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{KERN_CAP_BSET, "cap-bound", &cap_bset, sizeof(kernel_cap_t),
	 0600, NULL, &proc_dointvec_bset},
#ifdef CONFIG_BLK_DEV_INITRD
	{KERN_REALROOTDEV, "real-root-dev", &real_root_dev, sizeof(int),
	 0644, NULL, &proc_dointvec},
#endif
#ifdef __sparc__
	{KERN_SPARC_REBOOT, "reboot-cmd", reboot_command,
	 256, 0644, NULL, &proc_dostring, &sysctl_string },
	{KERN_SPARC_STOP_A, "stop-a", &stop_a_enabled, sizeof (int),
	 0644, NULL, &proc_dointvec},
#endif
#ifdef __powerpc__
	{KERN_PPC_HTABRECLAIM, "htab-reclaim", &htab_reclaim_on, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{KERN_PPC_ZEROPAGED, "zero-paged", &zero_paged_on, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{KERN_PPC_POWERSAVE_NAP, "powersave-nap", &powersave_nap, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{KERN_PPC_L2CR, "l2cr", NULL, 0,
	 0644, NULL, &proc_dol2crvec},
#endif
	{KERN_CTLALTDEL, "ctrl-alt-del", &C_A_D, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{KERN_PRINTK, "printk", &console_loglevel, 4*sizeof(int),
	 0644, NULL, &proc_dointvec},
#ifdef CONFIG_KMOD
	{KERN_MODPROBE, "modprobe", &modprobe_path, 256,
	 0644, NULL, &proc_dostring, &sysctl_string },
#endif
#ifdef CONFIG_HOTPLUG
	{KERN_HOTPLUG, "hotplug", &hotplug_path, 256,
	 0644, NULL, &proc_dostring, &sysctl_string },
#endif
#ifdef CONFIG_CHR_DEV_SG
	{KERN_SG_BIG_BUFF, "sg-big-buff", &sg_big_buff, sizeof (int),
	 0444, NULL, &proc_dointvec},
#endif
#ifdef CONFIG_BSD_PROCESS_ACCT
	{KERN_ACCT, "acct", &acct_parm, 3*sizeof(int),
	0644, NULL, &proc_dointvec},
#endif
	{KERN_RTSIGNR, "rtsig-nr", &nr_queued_signals, sizeof(int),
	 0444, NULL, &proc_dointvec},
	{KERN_RTSIGMAX, "rtsig-max", &max_queued_signals, sizeof(int),
	 0644, NULL, &proc_dointvec},
#ifdef CONFIG_SYSVIPC
	{KERN_SHMMAX, "shmmax", &shm_ctlmax, sizeof (size_t),
	 0644, NULL, &proc_doulongvec_minmax},
	{KERN_SHMALL, "shmall", &shm_ctlall, sizeof (size_t),
	 0644, NULL, &proc_doulongvec_minmax},
	{KERN_SHMMNI, "shmmni", &shm_ctlmni, sizeof (int),
	 0644, NULL, &proc_dointvec},
	{KERN_MSGMAX, "msgmax", &msg_ctlmax, sizeof (int),
	 0644, NULL, &proc_dointvec},
	{KERN_MSGMNI, "msgmni", &msg_ctlmni, sizeof (int),
	 0644, NULL, &proc_dointvec},
	{KERN_MSGMNB, "msgmnb", &msg_ctlmnb, sizeof (int),
	 0644, NULL, &proc_dointvec},
	{KERN_SEM, "sem", &sem_ctls, 4*sizeof (int),
	 0644, NULL, &proc_dointvec},
#endif
#ifdef CONFIG_MAGIC_SYSRQ
	{KERN_SYSRQ, "sysrq", &sysrq_enabled, sizeof (int),
	 0644, NULL, &proc_dointvec},
#endif	 
	{KERN_MAX_THREADS, "threads-max", &max_threads, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{KERN_RANDOM, "random", NULL, 0, 0555, random_table},
	{KERN_OVERFLOWUID, "overflowuid", &overflowuid, sizeof(int), 0644, NULL,
	 &proc_dointvec_minmax, &sysctl_intvec, NULL,
	 &minolduid, &maxolduid},
	{KERN_OVERFLOWGID, "overflowgid", &overflowgid, sizeof(int), 0644, NULL,
	 &proc_dointvec_minmax, &sysctl_intvec, NULL,
	 &minolduid, &maxolduid},
	{0}
};

static ctl_table vm_table[] = {
	{VM_FREEPG, "freepages", 
	 &freepages, sizeof(freepages_t), 0444, NULL, &proc_dointvec},
	{VM_BDFLUSH, "bdflush", &bdf_prm, 9*sizeof(int), 0644, NULL,
	 &proc_dointvec_minmax, &sysctl_intvec, NULL,
	 &bdflush_min, &bdflush_max},
	{VM_OVERCOMMIT_MEMORY, "overcommit_memory", &sysctl_overcommit_memory,
	 sizeof(sysctl_overcommit_memory), 0644, NULL, &proc_dointvec},
	{VM_BUFFERMEM, "buffermem",
	 &buffer_mem, sizeof(buffer_mem_t), 0644, NULL, &proc_dointvec},
	{VM_PAGECACHE, "pagecache",
	 &page_cache, sizeof(buffer_mem_t), 0644, NULL, &proc_dointvec},
	{VM_PAGERDAEMON, "kswapd",
	 &pager_daemon, sizeof(pager_daemon_t), 0644, NULL, &proc_dointvec},
	{VM_PGT_CACHE, "pagetable_cache", 
	 &pgt_cache_water, 2*sizeof(int), 0644, NULL, &proc_dointvec},
	{VM_PAGE_CLUSTER, "page-cluster", 
	 &page_cluster, sizeof(int), 0644, NULL, &proc_dointvec},
	{0}
};

static ctl_table proc_table[] = {
	{0}
};

static ctl_table fs_table[] = {
	{FS_NRINODE, "inode-nr", &inodes_stat, 2*sizeof(int),
	 0444, NULL, &proc_dointvec},
	{FS_STATINODE, "inode-state", &inodes_stat, 7*sizeof(int),
	 0444, NULL, &proc_dointvec},
	{FS_NRFILE, "file-nr", &files_stat, 3*sizeof(int),
	 0444, NULL, &proc_dointvec},
	{FS_MAXFILE, "file-max", &files_stat.max_files, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{FS_NRSUPER, "super-nr", &nr_super_blocks, sizeof(int),
	 0444, NULL, &proc_dointvec},
	{FS_MAXSUPER, "super-max", &max_super_blocks, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{FS_NRDQUOT, "dquot-nr", &nr_dquots, 2*sizeof(int),
	 0444, NULL, &proc_dointvec},
	{FS_MAXDQUOT, "dquot-max", &max_dquots, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{FS_DENTRY, "dentry-state", &dentry_stat, 6*sizeof(int),
	 0444, NULL, &proc_dointvec},
	{FS_OVERFLOWUID, "overflowuid", &fs_overflowuid, sizeof(int), 0644, NULL,
	 &proc_dointvec_minmax, &sysctl_intvec, NULL,
	 &minolduid, &maxolduid},
	{FS_OVERFLOWGID, "overflowgid", &fs_overflowgid, sizeof(int), 0644, NULL,
	 &proc_dointvec_minmax, &sysctl_intvec, NULL,
	 &minolduid, &maxolduid},
	{FS_LEASES, "leases-enable", &leases_enable, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{FS_DIR_NOTIFY, "dir-notify-enable", &dir_notify_enable,
	 sizeof(int), 0644, NULL, &proc_dointvec},
	{FS_LEASE_TIME, "lease-break-time", &lease_break_time, sizeof(int),
	 0644, NULL, &proc_dointvec},
	{0}
};

static ctl_table debug_table[] = {
	{0}
};

static ctl_table dev_table[] = {
	{0}
};  

extern void init_irq_proc (void);

void __init sysctl_init(void)
{
#ifdef CONFIG_PROC_FS
	register_proc_table(root_table, proc_sys_root);
	init_irq_proc();
#endif
}

int do_sysctl(int *name, int nlen, void *oldval, size_t *oldlenp,
	       void *newval, size_t newlen)
{
	struct list_head *tmp;

	if (nlen == 0 || nlen >= CTL_MAXNAME)
		return -ENOTDIR;
	if (oldval) {
		int old_len;
		if (!oldlenp || get_user(old_len, oldlenp))
			return -EFAULT;
	}
	tmp = &root_table_header.ctl_entry;
	do {
		struct ctl_table_header *head =
			list_entry(tmp, struct ctl_table_header, ctl_entry);
		void *context = NULL;
		int error = parse_table(name, nlen, oldval, oldlenp, 
					newval, newlen, head->ctl_table,
					&context);
		if (context)
			kfree(context);
		if (error != -ENOTDIR)
			return error;
		tmp = tmp->next;
	} while (tmp != &root_table_header.ctl_entry);
	return -ENOTDIR;
}

extern asmlinkage long sys_sysctl(struct __sysctl_args *args)
{
	struct __sysctl_args tmp;
	int error;

	if (copy_from_user(&tmp, args, sizeof(tmp)))
		return -EFAULT;
		
	lock_kernel();
	error = do_sysctl(tmp.name, tmp.nlen, tmp.oldval, tmp.oldlenp,
			  tmp.newval, tmp.newlen);
	unlock_kernel();
	return error;
}

/*
 * ctl_perm does NOT grant the superuser all rights automatically, because
 * some sysctl variables are readonly even to root.
 */

static int test_perm(int mode, int op)
{
	if (!current->euid)
		mode >>= 6;
	else if (in_egroup_p(0))
		mode >>= 3;
	if ((mode & op & 0007) == op)
		return 0;
	return -EACCES;
}

static inline int ctl_perm(ctl_table *table, int op)
{
	return test_perm(table->mode, op);
}

static int parse_table(int *name, int nlen,
		       void *oldval, size_t *oldlenp,
		       void *newval, size_t newlen,
		       ctl_table *table, void **context)
{
	int n;
repeat:
	if (!nlen)
		return -ENOTDIR;
	if (get_user(n, name))
		return -EFAULT;
	for ( ; table->ctl_name; table++) {
		if (n == table->ctl_name || table->ctl_name == CTL_ANY) {
			int error;
			if (table->child) {
				if (ctl_perm(table, 001))
					return -EPERM;
				if (table->strategy) {
					error = table->strategy(
						table, name, nlen,
						oldval, oldlenp,
						newval, newlen, context);
					if (error)
						return error;
				}
				name++;
				nlen--;
				table = table->child;
				goto repeat;
			}
			error = do_sysctl_strategy(table, name, nlen,
						   oldval, oldlenp,
						   newval, newlen, context);
			return error;
		}
	}
	return -ENOTDIR;
}

/* Perform the actual read/write of a sysctl table entry. */
int do_sysctl_strategy (ctl_table *table, 
			int *name, int nlen,
			void *oldval, size_t *oldlenp,
			void *newval, size_t newlen, void **context)
{
	int op = 0, rc, len;

	if (oldval)
		op |= 004;
	if (newval) 
		op |= 002;
	if (ctl_perm(table, op))
		return -EPERM;

	if (table->strategy) {
		rc = table->strategy(table, name, nlen, oldval, oldlenp,
				     newval, newlen, context);
		if (rc < 0)
			return rc;
		if (rc > 0)
			return 0;
	}

	/* If there is no strategy routine, or if the strategy returns
	 * zero, proceed with automatic r/w */
	if (table->data && table->maxlen) {
		if (oldval && oldlenp) {
			get_user(len, oldlenp);
			if (len) {
				if (len > table->maxlen)
					len = table->maxlen;
				if(copy_to_user(oldval, table->data, len))
					return -EFAULT;
				if(put_user(len, oldlenp))
					return -EFAULT;
			}
		}
		if (newval && newlen) {
			len = newlen;
			if (len > table->maxlen)
				len = table->maxlen;
			if(copy_from_user(table->data, newval, len))
				return -EFAULT;
		}
	}
	return 0;
}

struct ctl_table_header *register_sysctl_table(ctl_table * table, 
					       int insert_at_head)
{
	struct ctl_table_header *tmp;
	tmp = kmalloc(sizeof(struct ctl_table_header), GFP_KERNEL);
	if (!tmp)
		return 0;
	tmp->ctl_table = table;
	INIT_LIST_HEAD(&tmp->ctl_entry);
	if (insert_at_head)
		list_add(&tmp->ctl_entry, &root_table_header.ctl_entry);
	else
		list_add_tail(&tmp->ctl_entry, &root_table_header.ctl_entry);
#ifdef CONFIG_PROC_FS
	register_proc_table(table, proc_sys_root);
#endif
	return tmp;
}

/*
 * Unlink and free a ctl_table.
 */
void unregister_sysctl_table(struct ctl_table_header * header)
{
	list_del(&header->ctl_entry);
#ifdef CONFIG_PROC_FS
	unregister_proc_table(header->ctl_table, proc_sys_root);
#endif
	kfree(header);
}

/*
 * /proc/sys support
 */

#ifdef CONFIG_PROC_FS

/* Scan the sysctl entries in table and add them all into /proc */
static void register_proc_table(ctl_table * table, struct proc_dir_entry *root)
{
	struct proc_dir_entry *de;
	int len;
	mode_t mode;
	
	for (; table->ctl_name; table++) {
		/* Can't do anything without a proc name. */
		if (!table->procname)
			continue;
		/* Maybe we can't do anything with it... */
		if (!table->proc_handler && !table->child) {
			printk(KERN_WARNING "SYSCTL: Can't register %s\n",
				table->procname);
			continue;
		}

		len = strlen(table->procname);
		mode = table->mode;

		de = NULL;
		if (table->proc_handler)
			mode |= S_IFREG;
		else {
			mode |= S_IFDIR;
			for (de = root->subdir; de; de = de->next) {
				if (proc_match(len, table->procname, de))
					break;
			}
			/* If the subdir exists already, de is non-NULL */
		}

		if (!de) {
			de = create_proc_entry(table->procname, mode, root);
			if (!de)
				continue;
			de->data = (void *) table;
			if (table->proc_handler) {
				de->proc_fops = &proc_sys_file_operations;
				de->proc_iops = &proc_sys_inode_operations;
			}
		}
		table->de = de;
		if (de->mode & S_IFDIR)
			register_proc_table(table->child, de);
	}
}

/*
 * Unregister a /proc sysctl table and any subdirectories.
 */
static void unregister_proc_table(ctl_table * table, struct proc_dir_entry *root)
{
	struct proc_dir_entry *de;
	for (; table->ctl_name; table++) {
		if (!(de = table->de))
			continue;
		if (de->mode & S_IFDIR) {
			if (!table->child) {
				printk (KERN_ALERT "Help - malformed sysctl tree on free\n");
				continue;
			}
			unregister_proc_table(table->child, de);

			/* Don't unregister directories which still have entries.. */
			if (de->subdir)
				continue;
		}

		/* Don't unregister proc entries that are still being used.. */
		if (atomic_read(&de->count))
			continue;

		table->de = NULL;
		remove_proc_entry(table->procname, root);
	}
}

static ssize_t do_rw_proc(int write, struct file * file, char * buf,
			  size_t count, loff_t *ppos)
{
	int op;
	struct proc_dir_entry *de;
	struct ctl_table *table;
	size_t res;
	ssize_t error;
	
	de = (struct proc_dir_entry*) file->f_dentry->d_inode->u.generic_ip;
	if (!de || !de->data)
		return -ENOTDIR;
	table = (struct ctl_table *) de->data;
	if (!table || !table->proc_handler)
		return -ENOTDIR;
	op = (write ? 002 : 004);
	if (ctl_perm(table, op))
		return -EPERM;
	
	res = count;

	/*
	 * FIXME: we need to pass on ppos to the handler.
	 */

	error = (*table->proc_handler) (table, write, file, buf, &res);
	if (error)
		return error;
	return res;
}

static ssize_t proc_readsys(struct file * file, char * buf,
			    size_t count, loff_t *ppos)
{
	return do_rw_proc(0, file, buf, count, ppos);
}

static ssize_t proc_writesys(struct file * file, const char * buf,
			     size_t count, loff_t *ppos)
{
	return do_rw_proc(1, file, (char *) buf, count, ppos);
}

static int proc_sys_permission(struct inode *inode, int op)
{
	return test_perm(inode->i_mode, op);
}

int proc_dostring(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp)
{
	int len;
	char *p, c;
	
	if (!table->data || !table->maxlen || !*lenp ||
	    (filp->f_pos && !write)) {
		*lenp = 0;
		return 0;
	}
	
	if (write) {
		len = 0;
		p = buffer;
		while (len < *lenp) {
			if(get_user(c, p++))
				return -EFAULT;
			if (c == 0 || c == '\n')
				break;
			len++;
		}
		if (len >= table->maxlen)
			len = table->maxlen-1;
		if(copy_from_user(table->data, buffer, len))
			return -EFAULT;
		((char *) table->data)[len] = 0;
		filp->f_pos += *lenp;
	} else {
		len = strlen(table->data);
		if (len > table->maxlen)
			len = table->maxlen;
		if (len > *lenp)
			len = *lenp;
		if (len)
			if(copy_to_user(buffer, table->data, len))
				return -EFAULT;
		if (len < *lenp) {
			if(put_user('\n', ((char *) buffer) + len))
				return -EFAULT;
			len++;
		}
		*lenp = len;
		filp->f_pos += len;
	}
	return 0;
}

/*
 *	Special case of dostring for the UTS structure. This has locks
 *	to observe. Should this be in kernel/sys.c ????
 */
 
static int proc_doutsstring(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp)
{
	int r;

	if (!write) {
		down_read(&uts_sem);
		r=proc_dostring(table,0,filp,buffer,lenp);
		up_read(&uts_sem);
	} else {
		down_write(&uts_sem);
		r=proc_dostring(table,1,filp,buffer,lenp);
		up_write(&uts_sem);
	}
	return r;
}

#define OP_SET	0
#define OP_AND	1
#define OP_OR	2
#define OP_MAX	3
#define OP_MIN	4

static int do_proc_dointvec(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp, int conv, int op)
{
	int *i, vleft, first=1, len, left, neg, val;
	#define TMPBUFLEN 20
	char buf[TMPBUFLEN], *p;
	
	if (!table->data || !table->maxlen || !*lenp ||
	    (filp->f_pos && !write)) {
		*lenp = 0;
		return 0;
	}
	
	i = (int *) table->data;
	vleft = table->maxlen / sizeof(int);
	left = *lenp;
	
	for (; left && vleft--; i++, first=0) {
		if (write) {
			while (left) {
				char c;
				if(get_user(c,(char *) buffer))
					return -EFAULT;
				if (!isspace(c))
					break;
				left--;
				((char *) buffer)++;
			}
			if (!left)
				break;
			neg = 0;
			len = left;
			if (len > TMPBUFLEN-1)
				len = TMPBUFLEN-1;
			if(copy_from_user(buf, buffer, len))
				return -EFAULT;
			buf[len] = 0;
			p = buf;
			if (*p == '-' && left > 1) {
				neg = 1;
				left--, p++;
			}
			if (*p < '0' || *p > '9')
				break;
			val = simple_strtoul(p, &p, 0) * conv;
			len = p-buf;
			if ((len < left) && *p && !isspace(*p))
				break;
			if (neg)
				val = -val;
			buffer += len;
			left -= len;
			switch(op) {
			case OP_SET:	*i = val; break;
			case OP_AND:	*i &= val; break;
			case OP_OR:	*i |= val; break;
			case OP_MAX:	if(*i < val)
						*i = val;
					break;
			case OP_MIN:	if(*i > val)
						*i = val;
					break;
			}
		} else {
			p = buf;
			if (!first)
				*p++ = '\t';
			sprintf(p, "%d", (*i) / conv);
			len = strlen(buf);
			if (len > left)
				len = left;
			if(copy_to_user(buffer, buf, len))
				return -EFAULT;
			left -= len;
			buffer += len;
		}
	}

	if (!write && !first && left) {
		if(put_user('\n', (char *) buffer))
			return -EFAULT;
		left--, buffer++;
	}
	if (write) {
		p = (char *) buffer;
		while (left) {
			char c;
			if(get_user(c, p++))
				return -EFAULT;
			if (!isspace(c))
				break;
			left--;
		}
	}
	if (write && first)
		return -EINVAL;
	*lenp -= left;
	filp->f_pos += *lenp;
	return 0;
}

int proc_dointvec(ctl_table *table, int write, struct file *filp,
		     void *buffer, size_t *lenp)
{
    return do_proc_dointvec(table,write,filp,buffer,lenp,1,OP_SET);
}

/*
 *	init may raise the set.
 */
 
int proc_dointvec_bset(ctl_table *table, int write, struct file *filp,
			void *buffer, size_t *lenp)
{
	if (!capable(CAP_SYS_MODULE)) {
		return -EPERM;
	}
	return do_proc_dointvec(table,write,filp,buffer,lenp,1,
				(current->pid == 1) ? OP_SET : OP_AND);
}

int proc_dointvec_minmax(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp)
{
	int *i, *min, *max, vleft, first=1, len, left, neg, val;
	#define TMPBUFLEN 20
	char buf[TMPBUFLEN], *p;
	
	if (!table->data || !table->maxlen || !*lenp ||
	    (filp->f_pos && !write)) {
		*lenp = 0;
		return 0;
	}
	
	i = (int *) table->data;
	min = (int *) table->extra1;
	max = (int *) table->extra2;
	vleft = table->maxlen / sizeof(int);
	left = *lenp;
	
	for (; left && vleft--; i++, first=0) {
		if (write) {
			while (left) {
				char c;
				if(get_user(c, (char *) buffer))
					return -EFAULT;
				if (!isspace(c))
					break;
				left--;
				((char *) buffer)++;
			}
			if (!left)
				break;
			neg = 0;
			len = left;
			if (len > TMPBUFLEN-1)
				len = TMPBUFLEN-1;
			if(copy_from_user(buf, buffer, len))
				return -EFAULT;
			buf[len] = 0;
			p = buf;
			if (*p == '-' && left > 1) {
				neg = 1;
				left--, p++;
			}
			if (*p < '0' || *p > '9')
				break;
			val = simple_strtoul(p, &p, 0);
			len = p-buf;
			if ((len < left) && *p && !isspace(*p))
				break;
			if (neg)
				val = -val;
			buffer += len;
			left -= len;

			if (min && val < *min++)
				continue;
			if (max && val > *max++)
				continue;
			*i = val;
		} else {
			p = buf;
			if (!first)
				*p++ = '\t';
			sprintf(p, "%d", *i);
			len = strlen(buf);
			if (len > left)
				len = left;
			if(copy_to_user(buffer, buf, len))
				return -EFAULT;
			left -= len;
			buffer += len;
		}
	}

	if (!write && !first && left) {
		if(put_user('\n', (char *) buffer))
			return -EFAULT;
		left--, buffer++;
	}
	if (write) {
		p = (char *) buffer;
		while (left) {
			char c;
			if(get_user(c, p++))
				return -EFAULT;
			if (!isspace(c))
				break;
			left--;
		}
	}
	if (write && first)
		return -EINVAL;
	*lenp -= left;
	filp->f_pos += *lenp;
	return 0;
}

/*
 * an unsigned long function version
 */

static int do_proc_doulongvec_minmax(ctl_table *table, int write,
				     struct file *filp,
				     void *buffer, size_t *lenp,
				     unsigned long convmul,
				     unsigned long convdiv)
{
#define TMPBUFLEN 20
	unsigned long *i, *min, *max, val;
	int vleft, first=1, len, left, neg;
	char buf[TMPBUFLEN], *p;
	
	if (!table->data || !table->maxlen || !*lenp ||
	    (filp->f_pos && !write)) {
		*lenp = 0;
		return 0;
	}
	
	i = (unsigned long *) table->data;
	min = (unsigned long *) table->extra1;
	max = (unsigned long *) table->extra2;
	vleft = table->maxlen / sizeof(unsigned long);
	left = *lenp;
	
	for (; left && vleft--; i++, first=0) {
		if (write) {
			while (left) {
				char c;
				if(get_user(c, (char *) buffer))
					return -EFAULT;
				if (!isspace(c))
					break;
				left--;
				((char *) buffer)++;
			}
			if (!left)
				break;
			neg = 0;
			len = left;
			if (len > TMPBUFLEN-1)
				len = TMPBUFLEN-1;
			if(copy_from_user(buf, buffer, len))
				return -EFAULT;
			buf[len] = 0;
			p = buf;
			if (*p == '-' && left > 1) {
				neg = 1;
				left--, p++;
			}
			if (*p < '0' || *p > '9')
				break;
			val = simple_strtoul(p, &p, 0) * convmul / convdiv ;
			len = p-buf;
			if ((len < left) && *p && !isspace(*p))
				break;
			if (neg)
				val = -val;
			buffer += len;
			left -= len;

			if(neg)
				continue;
			if (min && val < *min++)
				continue;
			if (max && val > *max++)
				continue;
			*i = val;
		} else {
			p = buf;
			if (!first)
				*p++ = '\t';
			sprintf(p, "%lu", convdiv * (*i) / convmul);
			len = strlen(buf);
			if (len > left)
				len = left;
			if(copy_to_user(buffer, buf, len))
				return -EFAULT;
			left -= len;
			buffer += len;
		}
	}

	if (!write && !first && left) {
		if(put_user('\n', (char *) buffer))
			return -EFAULT;
		left--, buffer++;
	}
	if (write) {
		p = (char *) buffer;
		while (left) {
			char c;
			if(get_user(c, p++))
				return -EFAULT;
			if (!isspace(c))
				break;
			left--;
		}
	}
	if (write && first)
		return -EINVAL;
	*lenp -= left;
	filp->f_pos += *lenp;
	return 0;
#undef TMPBUFLEN
}

int proc_doulongvec_minmax(ctl_table *table, int write, struct file *filp,
			   void *buffer, size_t *lenp)
{
    return do_proc_doulongvec_minmax(table, write, filp, buffer, lenp, 1l, 1l);
}

int proc_doulongvec_ms_jiffies_minmax(ctl_table *table, int write,
				      struct file *filp,
				      void *buffer, size_t *lenp)
{
    return do_proc_doulongvec_minmax(table, write, filp, buffer,
				     lenp, HZ, 1000l);
}


/* Like proc_dointvec, but converts seconds to jiffies */
int proc_dointvec_jiffies(ctl_table *table, int write, struct file *filp,
			  void *buffer, size_t *lenp)
{
    return do_proc_dointvec(table,write,filp,buffer,lenp,HZ,OP_SET);
}

#else /* CONFIG_PROC_FS */

int proc_dostring(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

static int proc_doutsstring(ctl_table *table, int write, struct file *filp,
			    void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_dointvec(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_dointvec_bset(ctl_table *table, int write, struct file *filp,
			void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_dointvec_minmax(ctl_table *table, int write, struct file *filp,
		    void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_dointvec_jiffies(ctl_table *table, int write, struct file *filp,
		    void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_doulongvec_minmax(ctl_table *table, int write, struct file *filp,
		    void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_doulongvec_ms_jiffies_minmax(ctl_table *table, int write,
				      struct file *filp,
				      void *buffer, size_t *lenp)
{
    return -ENOSYS;
}


#endif /* CONFIG_PROC_FS */


/*
 * General sysctl support routines 
 */

/* The generic string strategy routine: */
int sysctl_string(ctl_table *table, int *name, int nlen,
		  void *oldval, size_t *oldlenp,
		  void *newval, size_t newlen, void **context)
{
	int l, len;
	
	if (!table->data || !table->maxlen) 
		return -ENOTDIR;
	
	if (oldval && oldlenp) {
		if(get_user(len, oldlenp))
			return -EFAULT;
		if (len) {
			l = strlen(table->data);
			if (len > l) len = l;
			if (len >= table->maxlen)
				len = table->maxlen;
			if(copy_to_user(oldval, table->data, len))
				return -EFAULT;
			if(put_user(0, ((char *) oldval) + len))
				return -EFAULT;
			if(put_user(len, oldlenp))
				return -EFAULT;
		}
	}
	if (newval && newlen) {
		len = newlen;
		if (len > table->maxlen)
			len = table->maxlen;
		if(copy_from_user(table->data, newval, len))
			return -EFAULT;
		if (len == table->maxlen)
			len--;
		((char *) table->data)[len] = 0;
	}
	return 0;
}

/*
 * This function makes sure that all of the integers in the vector
 * are between the minimum and maximum values given in the arrays
 * table->extra1 and table->extra2, respectively.
 */
int sysctl_intvec(ctl_table *table, int *name, int nlen,
		void *oldval, size_t *oldlenp,
		void *newval, size_t newlen, void **context)
{
	int i, length, *vec, *min, *max;

	if (newval && newlen) {
		if (newlen % sizeof(int) != 0)
			return -EINVAL;

		if (!table->extra1 && !table->extra2)
			return 0;

		if (newlen > table->maxlen)
			newlen = table->maxlen;
		length = newlen / sizeof(int);

		vec = (int *) newval;
		min = (int *) table->extra1;
		max = (int *) table->extra2;

		for (i = 0; i < length; i++) {
			int value;
			get_user(value, vec + i);
			if (min && value < min[i])
				return -EINVAL;
			if (max && value > max[i])
				return -EINVAL;
		}
	}
	return 0;
}

/* Strategy function to convert jiffies to seconds */ 
int sysctl_jiffies(ctl_table *table, int *name, int nlen,
		void *oldval, size_t *oldlenp,
		void *newval, size_t newlen, void **context)
{
	if (oldval) {
		size_t olen;
		if (oldlenp) { 
			if (get_user(olen, oldlenp))
				return -EFAULT;
			if (olen!=sizeof(int))
				return -EINVAL; 
		}
		if (put_user(*(int *)(table->data) / HZ, (int *)oldval) || 
		    (oldlenp && put_user(sizeof(int),oldlenp)))
			return -EFAULT;
	}
	if (newval && newlen) { 
		int new;
		if (newlen != sizeof(int))
			return -EINVAL; 
		if (get_user(new, (int *)newval))
			return -EFAULT;
		*(int *)(table->data) = new*HZ; 
	}
	return 1;
}


#else /* CONFIG_SYSCTL */


extern asmlinkage long sys_sysctl(struct __sysctl_args *args)
{
	return -ENOSYS;
}

int sysctl_string(ctl_table *table, int *name, int nlen,
		  void *oldval, size_t *oldlenp,
		  void *newval, size_t newlen, void **context)
{
	return -ENOSYS;
}

int sysctl_intvec(ctl_table *table, int *name, int nlen,
		void *oldval, size_t *oldlenp,
		void *newval, size_t newlen, void **context)
{
	return -ENOSYS;
}

int sysctl_jiffies(ctl_table *table, int *name, int nlen,
		void *oldval, size_t *oldlenp,
		void *newval, size_t newlen, void **context)
{
	return -ENOSYS;
}

int proc_dostring(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_dointvec(ctl_table *table, int write, struct file *filp,
		  void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_dointvec_bset(ctl_table *table, int write, struct file *filp,
			void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_dointvec_minmax(ctl_table *table, int write, struct file *filp,
		    void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_dointvec_jiffies(ctl_table *table, int write, struct file *filp,
			  void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_doulongvec_minmax(ctl_table *table, int write, struct file *filp,
		    void *buffer, size_t *lenp)
{
	return -ENOSYS;
}

int proc_doulongvec_ms_jiffies_minmax(ctl_table *table, int write,
				      struct file *filp,
				      void *buffer, size_t *lenp)
{
    return -ENOSYS;
}

struct ctl_table_header * register_sysctl_table(ctl_table * table, 
						int insert_at_head)
{
	return 0;
}

void unregister_sysctl_table(struct ctl_table_header * table)
{
}

#endif /* CONFIG_SYSCTL */
