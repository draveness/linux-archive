/* Updated: Karl MacMillan <kmacmillan@tresys.com>
 *
 * 	Added conditional policy language extensions
 *
 * Copyright (C) 2003 - 2004 Tresys Technology, LLC
 *	This program is free software; you can redistribute it and/or modify
 *  	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, version 2.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/security.h>
#include <asm/uaccess.h>
#include <asm/semaphore.h>

/* selinuxfs pseudo filesystem for exporting the security policy API.
   Based on the proc code and the fs/nfsd/nfsctl.c code. */

#include "flask.h"
#include "avc.h"
#include "avc_ss.h"
#include "security.h"
#include "objsec.h"
#include "conditional.h"

static DECLARE_MUTEX(sel_sem);

/* global data for booleans */
static struct dentry *bool_dir = NULL;
static int bool_num = 0;
static int *bool_pending_values = NULL;

extern void selnl_notify_setenforce(int val);

/* Check whether a task is allowed to use a security operation. */
int task_has_security(struct task_struct *tsk,
		      u32 perms)
{
	struct task_security_struct *tsec;

	tsec = tsk->security;
	if (!tsec)
		return -EACCES;

	return avc_has_perm(tsec->sid, SECINITSID_SECURITY,
			    SECCLASS_SECURITY, perms, NULL, NULL);
}

enum sel_inos {
	SEL_ROOT_INO = 2,
	SEL_LOAD,	/* load policy */
	SEL_ENFORCE,	/* get or set enforcing status */
	SEL_CONTEXT,	/* validate context */
	SEL_ACCESS,	/* compute access decision */
	SEL_CREATE,	/* compute create labeling decision */
	SEL_RELABEL,	/* compute relabeling decision */
	SEL_USER,	/* compute reachable user contexts */
	SEL_POLICYVERS,	/* return policy version for this kernel */
	SEL_COMMIT_BOOLS, /* commit new boolean values */
	SEL_MLS,	/* return if MLS policy is enabled */
	SEL_DISABLE	/* disable SELinux until next reboot */
};

static ssize_t sel_read_enforce(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	char *page;
	ssize_t length;
	ssize_t end;

	if (count < 0 || count > PAGE_SIZE)
		return -EINVAL;
	if (!(page = (char*)__get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	memset(page, 0, PAGE_SIZE);

	length = scnprintf(page, PAGE_SIZE, "%d", selinux_enforcing);
	if (length < 0) {
		free_page((unsigned long)page);
		return length;
	}

	if (*ppos >= length) {
		free_page((unsigned long)page);
		return 0;
	}
	if (count + *ppos > length)
		count = length - *ppos;
	end = count + *ppos;
	if (copy_to_user(buf, (char *) page + *ppos, count)) {
		count = -EFAULT;
		goto out;
	}
	*ppos = end;
out:
	free_page((unsigned long)page);
	return count;
}

#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
static ssize_t sel_write_enforce(struct file * file, const char __user * buf,
				 size_t count, loff_t *ppos)

{
	char *page;
	ssize_t length;
	int new_value;

	if (count < 0 || count >= PAGE_SIZE)
		return -ENOMEM;
	if (*ppos != 0) {
		/* No partial writes. */
		return -EINVAL;
	}
	page = (char*)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	memset(page, 0, PAGE_SIZE);
	length = -EFAULT;
	if (copy_from_user(page, buf, count))
		goto out;

	length = -EINVAL;
	if (sscanf(page, "%d", &new_value) != 1)
		goto out;

	if (new_value != selinux_enforcing) {
		length = task_has_security(current, SECURITY__SETENFORCE);
		if (length)
			goto out;
		selinux_enforcing = new_value;
		if (selinux_enforcing)
			avc_ss_reset(0);
		selnl_notify_setenforce(selinux_enforcing);
	}
	length = count;
out:
	free_page((unsigned long) page);
	return length;
}
#else
#define sel_write_enforce NULL
#endif

static struct file_operations sel_enforce_ops = {
	.read		= sel_read_enforce,
	.write		= sel_write_enforce,
};

#ifdef CONFIG_SECURITY_SELINUX_DISABLE
static ssize_t sel_write_disable(struct file * file, const char __user * buf,
				 size_t count, loff_t *ppos)

{
	char *page;
	ssize_t length;
	int new_value;
	extern int selinux_disable(void);

	if (count < 0 || count >= PAGE_SIZE)
		return -ENOMEM;
	if (*ppos != 0) {
		/* No partial writes. */
		return -EINVAL;
	}
	page = (char*)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	memset(page, 0, PAGE_SIZE);
	length = -EFAULT;
	if (copy_from_user(page, buf, count))
		goto out;

	length = -EINVAL;
	if (sscanf(page, "%d", &new_value) != 1)
		goto out;

	if (new_value) {
		length = selinux_disable();
		if (length < 0)
			goto out;
	}

	length = count;
out:
	free_page((unsigned long) page);
	return length;
}
#else
#define sel_write_disable NULL
#endif

static struct file_operations sel_disable_ops = {
	.write		= sel_write_disable,
};

static ssize_t sel_read_policyvers(struct file *filp, char __user *buf,
                                   size_t count, loff_t *ppos)
{
	char *page;
	ssize_t length;
	ssize_t end;

	if (count < 0 || count > PAGE_SIZE)
		return -EINVAL;
	if (!(page = (char*)__get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	memset(page, 0, PAGE_SIZE);

	length = scnprintf(page, PAGE_SIZE, "%u", POLICYDB_VERSION_MAX);
	if (length < 0) {
		free_page((unsigned long)page);
		return length;
	}

	if (*ppos >= length) {
		free_page((unsigned long)page);
		return 0;
	}
	if (count + *ppos > length)
		count = length - *ppos;
	end = count + *ppos;
	if (copy_to_user(buf, (char *) page + *ppos, count)) {
		count = -EFAULT;
		goto out;
	}
	*ppos = end;
out:
	free_page((unsigned long)page);
	return count;
}

static struct file_operations sel_policyvers_ops = {
	.read		= sel_read_policyvers,
};

/* declaration for sel_write_load */
static int sel_make_bools(void);

static ssize_t sel_read_mls(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	char *page;
	ssize_t length;
	ssize_t end;

	if (count < 0 || count > PAGE_SIZE)
		return -EINVAL;
	if (!(page = (char*)__get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	memset(page, 0, PAGE_SIZE);

	length = scnprintf(page, PAGE_SIZE, "%d", selinux_mls_enabled);
	if (length < 0) {
		free_page((unsigned long)page);
		return length;
	}

	if (*ppos >= length) {
		free_page((unsigned long)page);
		return 0;
	}
	if (count + *ppos > length)
		count = length - *ppos;
	end = count + *ppos;
	if (copy_to_user(buf, (char *) page + *ppos, count)) {
		count = -EFAULT;
		goto out;
	}
	*ppos = end;
out:
	free_page((unsigned long)page);
	return count;
}

static struct file_operations sel_mls_ops = {
	.read		= sel_read_mls,
};

static ssize_t sel_write_load(struct file * file, const char __user * buf,
			      size_t count, loff_t *ppos)

{
	int ret;
	ssize_t length;
	void *data = NULL;

	down(&sel_sem);

	length = task_has_security(current, SECURITY__LOAD_POLICY);
	if (length)
		goto out;

	if (*ppos != 0) {
		/* No partial writes. */
		length = -EINVAL;
		goto out;
	}

	if ((count < 0) || (count > 64 * 1024 * 1024)
	    || (data = vmalloc(count)) == NULL) {
		length = -ENOMEM;
		goto out;
	}

	length = -EFAULT;
	if (copy_from_user(data, buf, count) != 0)
		goto out;

	length = security_load_policy(data, count);
	if (length)
		goto out;

	ret = sel_make_bools();
	if (ret)
		length = ret;
	else
		length = count;
out:
	up(&sel_sem);
	vfree(data);
	return length;
}

static struct file_operations sel_load_ops = {
	.write		= sel_write_load,
};


static ssize_t sel_write_context(struct file * file, const char __user * buf,
				 size_t count, loff_t *ppos)

{
	char *page;
	u32 sid;
	ssize_t length;

	length = task_has_security(current, SECURITY__CHECK_CONTEXT);
	if (length)
		return length;

	if (count < 0 || count >= PAGE_SIZE)
		return -ENOMEM;
	if (*ppos != 0) {
		/* No partial writes. */
		return -EINVAL;
	}
	page = (char*)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	memset(page, 0, PAGE_SIZE);
	length = -EFAULT;
	if (copy_from_user(page, buf, count))
		goto out;

	length = security_context_to_sid(page, count, &sid);
	if (length < 0)
		goto out;

	length = count;
out:
	free_page((unsigned long) page);
	return length;
}

static struct file_operations sel_context_ops = {
	.write		= sel_write_context,
};


/*
 * Remaining nodes use transaction based IO methods like nfsd/nfsctl.c
 */
static ssize_t sel_write_access(struct file * file, char *buf, size_t size);
static ssize_t sel_write_create(struct file * file, char *buf, size_t size);
static ssize_t sel_write_relabel(struct file * file, char *buf, size_t size);
static ssize_t sel_write_user(struct file * file, char *buf, size_t size);

static ssize_t (*write_op[])(struct file *, char *, size_t) = {
	[SEL_ACCESS] = sel_write_access,
	[SEL_CREATE] = sel_write_create,
	[SEL_RELABEL] = sel_write_relabel,
	[SEL_USER] = sel_write_user,
};

/* an argresp is stored in an allocated page and holds the
 * size of the argument or response, along with its content
 */
struct argresp {
	ssize_t size;
	char data[0];
};

#define PAYLOAD_SIZE (PAGE_SIZE - sizeof(struct argresp))

/*
 * transaction based IO methods.
 * The file expects a single write which triggers the transaction, and then
 * possibly a read which collects the result - which is stored in a
 * file-local buffer.
 */
static ssize_t TA_write(struct file *file, const char __user *buf, size_t size, loff_t *pos)
{
	ino_t ino =  file->f_dentry->d_inode->i_ino;
	struct argresp *ar;
	ssize_t rv = 0;

	if (ino >= sizeof(write_op)/sizeof(write_op[0]) || !write_op[ino])
		return -EINVAL;
	if (file->private_data)
		return -EINVAL; /* only one write allowed per open */
	if (size > PAYLOAD_SIZE - 1) /* allow one byte for null terminator */
		return -EFBIG;

	ar = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!ar)
		return -ENOMEM;
	memset(ar, 0, PAGE_SIZE); /* clear buffer, particularly last byte */
	ar->size = 0;
	down(&file->f_dentry->d_inode->i_sem);
	if (file->private_data)
		rv = -EINVAL;
	else
		file->private_data = ar;
	up(&file->f_dentry->d_inode->i_sem);
	if (rv) {
		kfree(ar);
		return rv;
	}
	if (copy_from_user(ar->data, buf, size))
		return -EFAULT;

	rv =  write_op[ino](file, ar->data, size);
	if (rv>0) {
		ar->size = rv;
		rv = size;
	}
	return rv;
}

static ssize_t TA_read(struct file *file, char __user *buf, size_t size, loff_t *pos)
{
	struct argresp *ar;
	ssize_t rv = 0;

	if (file->private_data == NULL)
		rv = TA_write(file, buf, 0, pos);
	if (rv < 0)
		return rv;

	ar = file->private_data;
	if (!ar)
		return 0;
	if (*pos >= ar->size)
		return 0;
	if (*pos + size > ar->size)
		size = ar->size - *pos;
	if (copy_to_user(buf, ar->data + *pos, size))
		return -EFAULT;
	*pos += size;
	return size;
}

static int TA_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int TA_release(struct inode *inode, struct file *file)
{
	void *p = file->private_data;
	file->private_data = NULL;
	kfree(p);
	return 0;
}

static struct file_operations transaction_ops = {
	.write		= TA_write,
	.read		= TA_read,
	.open		= TA_open,
	.release	= TA_release,
};

/*
 * payload - write methods
 * If the method has a response, the response should be put in buf,
 * and the length returned.  Otherwise return 0 or and -error.
 */

static ssize_t sel_write_access(struct file * file, char *buf, size_t size)
{
	char *scon, *tcon;
	u32 ssid, tsid;
	u16 tclass;
	u32 req;
	struct av_decision avd;
	ssize_t length;

	length = task_has_security(current, SECURITY__COMPUTE_AV);
	if (length)
		return length;

	length = -ENOMEM;
	scon = kmalloc(size+1, GFP_KERNEL);
	if (!scon)
		return length;
	memset(scon, 0, size+1);

	tcon = kmalloc(size+1, GFP_KERNEL);
	if (!tcon)
		goto out;
	memset(tcon, 0, size+1);

	length = -EINVAL;
	if (sscanf(buf, "%s %s %hu %x", scon, tcon, &tclass, &req) != 4)
		goto out2;

	length = security_context_to_sid(scon, strlen(scon)+1, &ssid);
	if (length < 0)
		goto out2;
	length = security_context_to_sid(tcon, strlen(tcon)+1, &tsid);
	if (length < 0)
		goto out2;

	length = security_compute_av(ssid, tsid, tclass, req, &avd);
	if (length < 0)
		goto out2;

	length = scnprintf(buf, PAYLOAD_SIZE, "%x %x %x %x %u",
			  avd.allowed, avd.decided,
			  avd.auditallow, avd.auditdeny,
			  avd.seqno);
out2:
	kfree(tcon);
out:
	kfree(scon);
	return length;
}

static ssize_t sel_write_create(struct file * file, char *buf, size_t size)
{
	char *scon, *tcon;
	u32 ssid, tsid, newsid;
	u16 tclass;
	ssize_t length;
	char *newcon;
	u32 len;

	length = task_has_security(current, SECURITY__COMPUTE_CREATE);
	if (length)
		return length;

	length = -ENOMEM;
	scon = kmalloc(size+1, GFP_KERNEL);
	if (!scon)
		return length;
	memset(scon, 0, size+1);

	tcon = kmalloc(size+1, GFP_KERNEL);
	if (!tcon)
		goto out;
	memset(tcon, 0, size+1);

	length = -EINVAL;
	if (sscanf(buf, "%s %s %hu", scon, tcon, &tclass) != 3)
		goto out2;

	length = security_context_to_sid(scon, strlen(scon)+1, &ssid);
	if (length < 0)
		goto out2;
	length = security_context_to_sid(tcon, strlen(tcon)+1, &tsid);
	if (length < 0)
		goto out2;

	length = security_transition_sid(ssid, tsid, tclass, &newsid);
	if (length < 0)
		goto out2;

	length = security_sid_to_context(newsid, &newcon, &len);
	if (length < 0)
		goto out2;

	if (len > PAYLOAD_SIZE) {
		printk(KERN_ERR "%s:  context size (%u) exceeds payload "
		       "max\n", __FUNCTION__, len);
		length = -ERANGE;
		goto out3;
	}

	memcpy(buf, newcon, len);
	length = len;
out3:
	kfree(newcon);
out2:
	kfree(tcon);
out:
	kfree(scon);
	return length;
}

static ssize_t sel_write_relabel(struct file * file, char *buf, size_t size)
{
	char *scon, *tcon;
	u32 ssid, tsid, newsid;
	u16 tclass;
	ssize_t length;
	char *newcon;
	u32 len;

	length = task_has_security(current, SECURITY__COMPUTE_RELABEL);
	if (length)
		return length;

	length = -ENOMEM;
	scon = kmalloc(size+1, GFP_KERNEL);
	if (!scon)
		return length;
	memset(scon, 0, size+1);

	tcon = kmalloc(size+1, GFP_KERNEL);
	if (!tcon)
		goto out;
	memset(tcon, 0, size+1);

	length = -EINVAL;
	if (sscanf(buf, "%s %s %hu", scon, tcon, &tclass) != 3)
		goto out2;

	length = security_context_to_sid(scon, strlen(scon)+1, &ssid);
	if (length < 0)
		goto out2;
	length = security_context_to_sid(tcon, strlen(tcon)+1, &tsid);
	if (length < 0)
		goto out2;

	length = security_change_sid(ssid, tsid, tclass, &newsid);
	if (length < 0)
		goto out2;

	length = security_sid_to_context(newsid, &newcon, &len);
	if (length < 0)
		goto out2;

	if (len > PAYLOAD_SIZE) {
		length = -ERANGE;
		goto out3;
	}

	memcpy(buf, newcon, len);
	length = len;
out3:
	kfree(newcon);
out2:
	kfree(tcon);
out:
	kfree(scon);
	return length;
}

static ssize_t sel_write_user(struct file * file, char *buf, size_t size)
{
	char *con, *user, *ptr;
	u32 sid, *sids;
	ssize_t length;
	char *newcon;
	int i, rc;
	u32 len, nsids;

	length = task_has_security(current, SECURITY__COMPUTE_USER);
	if (length)
		return length;

	length = -ENOMEM;
	con = kmalloc(size+1, GFP_KERNEL);
	if (!con)
		return length;
	memset(con, 0, size+1);

	user = kmalloc(size+1, GFP_KERNEL);
	if (!user)
		goto out;
	memset(user, 0, size+1);

	length = -EINVAL;
	if (sscanf(buf, "%s %s", con, user) != 2)
		goto out2;

	length = security_context_to_sid(con, strlen(con)+1, &sid);
	if (length < 0)
		goto out2;

	length = security_get_user_sids(sid, user, &sids, &nsids);
	if (length < 0)
		goto out2;

	length = sprintf(buf, "%u", nsids) + 1;
	ptr = buf + length;
	for (i = 0; i < nsids; i++) {
		rc = security_sid_to_context(sids[i], &newcon, &len);
		if (rc) {
			length = rc;
			goto out3;
		}
		if ((length + len) >= PAYLOAD_SIZE) {
			kfree(newcon);
			length = -ERANGE;
			goto out3;
		}
		memcpy(ptr, newcon, len);
		kfree(newcon);
		ptr += len;
		length += len;
	}
out3:
	kfree(sids);
out2:
	kfree(user);
out:
	kfree(con);
	return length;
}

static struct inode *sel_make_inode(struct super_block *sb, int mode)
{
	struct inode *ret = new_inode(sb);

	if (ret) {
		ret->i_mode = mode;
		ret->i_uid = ret->i_gid = 0;
		ret->i_blksize = PAGE_CACHE_SIZE;
		ret->i_blocks = 0;
		ret->i_atime = ret->i_mtime = ret->i_ctime = CURRENT_TIME;
	}
	return ret;
}

#define BOOL_INO_OFFSET 30

static ssize_t sel_read_bool(struct file *filep, char __user *buf,
			     size_t count, loff_t *ppos)
{
	char *page = NULL;
	ssize_t length;
	ssize_t end;
	ssize_t ret;
	int cur_enforcing;
	struct inode *inode;

	down(&sel_sem);

	ret = -EFAULT;

	/* check to see if this file has been deleted */
	if (!filep->f_op)
		goto out;

	if (count < 0 || count > PAGE_SIZE) {
		ret = -EINVAL;
		goto out;
	}
	if (!(page = (char*)__get_free_page(GFP_KERNEL))) {
		ret = -ENOMEM;
		goto out;
	}
	memset(page, 0, PAGE_SIZE);

	inode = filep->f_dentry->d_inode;
	cur_enforcing = security_get_bool_value(inode->i_ino - BOOL_INO_OFFSET);
	if (cur_enforcing < 0) {
		ret = cur_enforcing;
		goto out;
	}

	length = scnprintf(page, PAGE_SIZE, "%d %d", cur_enforcing,
			  bool_pending_values[inode->i_ino - BOOL_INO_OFFSET]);
	if (length < 0) {
		ret = length;
		goto out;
	}

	if (*ppos >= length) {
		ret = 0;
		goto out;
	}
	if (count + *ppos > length)
		count = length - *ppos;
	end = count + *ppos;
	if (copy_to_user(buf, (char *) page + *ppos, count)) {
		ret = -EFAULT;
		goto out;
	}
	*ppos = end;
	ret = count;
out:
	up(&sel_sem);
	if (page)
		free_page((unsigned long)page);
	return ret;
}

static ssize_t sel_write_bool(struct file *filep, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	char *page = NULL;
	ssize_t length = -EFAULT;
	int new_value;
	struct inode *inode;

	down(&sel_sem);

	length = task_has_security(current, SECURITY__SETBOOL);
	if (length)
		goto out;

	/* check to see if this file has been deleted */
	if (!filep->f_op)
		goto out;

	if (count < 0 || count >= PAGE_SIZE) {
		length = -ENOMEM;
		goto out;
	}
	if (*ppos != 0) {
		/* No partial writes. */
		goto out;
	}
	page = (char*)__get_free_page(GFP_KERNEL);
	if (!page) {
		length = -ENOMEM;
		goto out;
	}
	memset(page, 0, PAGE_SIZE);

	if (copy_from_user(page, buf, count))
		goto out;

	length = -EINVAL;
	if (sscanf(page, "%d", &new_value) != 1)
		goto out;

	if (new_value)
		new_value = 1;

	inode = filep->f_dentry->d_inode;
	bool_pending_values[inode->i_ino - BOOL_INO_OFFSET] = new_value;
	length = count;

out:
	up(&sel_sem);
	if (page)
		free_page((unsigned long) page);
	return length;
}

static struct file_operations sel_bool_ops = {
	.read           = sel_read_bool,
	.write          = sel_write_bool,
};

static ssize_t sel_commit_bools_write(struct file *filep,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	char *page = NULL;
	ssize_t length = -EFAULT;
	int new_value;

	down(&sel_sem);

	length = task_has_security(current, SECURITY__SETBOOL);
	if (length)
		goto out;

	/* check to see if this file has been deleted */
	if (!filep->f_op)
		goto out;

	if (count < 0 || count >= PAGE_SIZE) {
		length = -ENOMEM;
		goto out;
	}
	if (*ppos != 0) {
		/* No partial writes. */
		goto out;
	}
	page = (char*)__get_free_page(GFP_KERNEL);
	if (!page) {
		length = -ENOMEM;
		goto out;
	}

	memset(page, 0, PAGE_SIZE);

	if (copy_from_user(page, buf, count))
		goto out;

	length = -EINVAL;
	if (sscanf(page, "%d", &new_value) != 1)
		goto out;

	if (new_value) {
		security_set_bools(bool_num, bool_pending_values);
	}

	length = count;

out:
	up(&sel_sem);
	if (page)
		free_page((unsigned long) page);
	return length;
}

static struct file_operations sel_commit_bools_ops = {
	.write          = sel_commit_bools_write,
};

/* delete booleans - partial revoke() from
 * fs/proc/generic.c proc_kill_inodes */
static void sel_remove_bools(struct dentry *de)
{
	struct list_head *p, *node;
	struct super_block *sb = de->d_sb;

	spin_lock(&dcache_lock);
	node = de->d_subdirs.next;
	while (node != &de->d_subdirs) {
		struct dentry *d = list_entry(node, struct dentry, d_child);
		list_del_init(node);

		if (d->d_inode) {
			d = dget_locked(d);
			spin_unlock(&dcache_lock);
			d_delete(d);
			simple_unlink(de->d_inode, d);
			dput(d);
			spin_lock(&dcache_lock);
		}
		node = de->d_subdirs.next;
	}

	spin_unlock(&dcache_lock);

	file_list_lock();
	list_for_each(p, &sb->s_files) {
		struct file * filp = list_entry(p, struct file, f_list);
		struct dentry * dentry = filp->f_dentry;

		if (dentry->d_parent != de) {
			continue;
		}
		filp->f_op = NULL;
	}
	file_list_unlock();
}

#define BOOL_DIR_NAME "booleans"

static int sel_make_bools(void)
{
	int i, ret = 0;
	ssize_t len;
	struct dentry *dentry = NULL;
	struct dentry *dir = bool_dir;
	struct inode *inode = NULL;
	struct inode_security_struct *isec;
	struct qstr qname;
	char **names = NULL, *page;
	int num;
	int *values = NULL;
	u32 sid;

	/* remove any existing files */
	if (bool_pending_values)
		kfree(bool_pending_values);

	sel_remove_bools(dir);

	if (!(page = (char*)__get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	memset(page, 0, PAGE_SIZE);

	ret = security_get_bools(&num, &names, &values);
	if (ret != 0)
		goto out;

	for (i = 0; i < num; i++) {
		qname.name = names[i];
		qname.len = strlen(qname.name);
		qname.hash = full_name_hash(qname.name, qname.len);
		dentry = d_alloc(dir, &qname);
		if (!dentry) {
			ret = -ENOMEM;
			goto err;
		}
		inode = sel_make_inode(dir->d_sb, S_IFREG | S_IRUGO | S_IWUSR);
		if (!inode) {
			ret = -ENOMEM;
			goto err;
		}

		len = snprintf(page, PAGE_SIZE, "/%s/%s", BOOL_DIR_NAME, names[i]);
		if (len < 0) {
			ret = -EINVAL;
			goto err;
		} else if (len >= PAGE_SIZE) {
			ret = -ENAMETOOLONG;
			goto err;
		}
		isec = (struct inode_security_struct*)inode->i_security;
		if ((ret = security_genfs_sid("selinuxfs", page, SECCLASS_FILE, &sid)))
			goto err;
		isec->sid = sid;
		isec->initialized = 1;
		inode->i_fop = &sel_bool_ops;
		inode->i_ino = i + BOOL_INO_OFFSET;
		d_add(dentry, inode);
	}
	bool_num = num;
	bool_pending_values = values;
out:
	free_page((unsigned long)page);
	if (names) {
		for (i = 0; i < num; i++) {
			if (names[i])
				kfree(names[i]);
		}
		kfree(names);
	}
	return ret;
err:
	d_genocide(dir);
	ret = -ENOMEM;
	goto out;
}

static int sel_fill_super(struct super_block * sb, void * data, int silent)
{
	int ret;
	struct dentry *dentry;
	struct inode *inode;
	struct qstr qname;

	static struct tree_descr selinux_files[] = {
		[SEL_LOAD] = {"load", &sel_load_ops, S_IRUSR|S_IWUSR},
		[SEL_ENFORCE] = {"enforce", &sel_enforce_ops, S_IRUGO|S_IWUSR},
		[SEL_CONTEXT] = {"context", &sel_context_ops, S_IRUGO|S_IWUGO},
		[SEL_ACCESS] = {"access", &transaction_ops, S_IRUGO|S_IWUGO},
		[SEL_CREATE] = {"create", &transaction_ops, S_IRUGO|S_IWUGO},
		[SEL_RELABEL] = {"relabel", &transaction_ops, S_IRUGO|S_IWUGO},
		[SEL_USER] = {"user", &transaction_ops, S_IRUGO|S_IWUGO},
		[SEL_POLICYVERS] = {"policyvers", &sel_policyvers_ops, S_IRUGO},
		[SEL_COMMIT_BOOLS] = {"commit_pending_bools", &sel_commit_bools_ops, S_IWUSR},
		[SEL_MLS] = {"mls", &sel_mls_ops, S_IRUGO},
		[SEL_DISABLE] = {"disable", &sel_disable_ops, S_IWUSR},
		/* last one */ {""}
	};
	ret = simple_fill_super(sb, SELINUX_MAGIC, selinux_files);
	if (ret)
		return ret;

	qname.name = BOOL_DIR_NAME;
	qname.len = strlen(qname.name);
	qname.hash = full_name_hash(qname.name, qname.len);
	dentry = d_alloc(sb->s_root, &qname);
	if (!dentry)
		return -ENOMEM;

	inode = sel_make_inode(sb, S_IFDIR | S_IRUGO | S_IXUGO);
	if (!inode)
		goto out;
	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	d_add(dentry, inode);
	bool_dir = dentry;
	ret = sel_make_bools();
	if (ret)
		goto out;

	return 0;
out:
	dput(dentry);
	printk(KERN_ERR "security:	error creating conditional out_dput\n");
	return -ENOMEM;
}

static struct super_block *sel_get_sb(struct file_system_type *fs_type,
				      int flags, const char *dev_name, void *data)
{
	return get_sb_single(fs_type, flags, data, sel_fill_super);
}

static struct file_system_type sel_fs_type = {
	.name		= "selinuxfs",
	.get_sb		= sel_get_sb,
	.kill_sb	= kill_litter_super,
};

static int __init init_sel_fs(void)
{
	return selinux_enabled ? register_filesystem(&sel_fs_type) : 0;
}

__initcall(init_sel_fs);

#ifdef CONFIG_SECURITY_SELINUX_DISABLE
void exit_sel_fs(void)
{
	unregister_filesystem(&sel_fs_type);
}
#endif
