/*
 *  NSA Security-Enhanced Linux (SELinux) security module
 *
 *  This file contains the SELinux security data structures for kernel objects.
 *
 *  Author(s):  Stephen Smalley, <sds@epoch.ncsc.mil>
 *              Chris Vance, <cvance@nai.com>
 *              Wayne Salamon, <wsalamon@nai.com>
 *              James Morris <jmorris@redhat.com>
 *
 *  Copyright (C) 2001,2002 Networks Associates Technology, Inc.
 *  Copyright (C) 2003 Red Hat, Inc., James Morris <jmorris@redhat.com>
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2,
 *      as published by the Free Software Foundation.
 */
#ifndef _SELINUX_OBJSEC_H_
#define _SELINUX_OBJSEC_H_

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/binfmts.h>
#include <linux/in.h>
#include "flask.h"
#include "avc.h"

struct task_security_struct {
        unsigned long magic;           /* magic number for this module */
	struct task_struct *task;      /* back pointer to task object */
	u32 osid;            /* SID prior to last execve */
	u32 sid;             /* current SID */
	u32 exec_sid;        /* exec SID */
	u32 create_sid;      /* fscreate SID */
        struct avc_entry_ref avcr;     /* reference to process permissions */
	u32 ptrace_sid;      /* SID of ptrace parent */
};

struct inode_security_struct {
	unsigned long magic;           /* magic number for this module */
        struct inode *inode;           /* back pointer to inode object */
	struct list_head list;         /* list of inode_security_struct */
	u32 task_sid;        /* SID of creating task */
	u32 sid;             /* SID of this object */
	u16 sclass;       /* security class of this object */
	struct avc_entry_ref avcr;     /* reference to object permissions */
	unsigned char initialized;     /* initialization flag */
	struct semaphore sem;
	unsigned char inherit;         /* inherit SID from parent entry */
};

struct file_security_struct {
	unsigned long magic;            /* magic number for this module */
	struct file *file;              /* back pointer to file object */
	u32 sid;              /* SID of open file description */
	u32 fown_sid;         /* SID of file owner (for SIGIO) */
	struct avc_entry_ref avcr;	/* reference to fd permissions */
	struct avc_entry_ref inode_avcr;     /* reference to object permissions */
};

struct superblock_security_struct {
	unsigned long magic;            /* magic number for this module */
	struct super_block *sb;         /* back pointer to sb object */
	struct list_head list;          /* list of superblock_security_struct */
	u32 sid;              /* SID of file system */
	u32 def_sid;			/* default SID for labeling */
	unsigned int behavior;          /* labeling behavior */
	unsigned char initialized;      /* initialization flag */
	unsigned char proc;             /* proc fs */
	struct semaphore sem;
	struct list_head isec_head;
	spinlock_t isec_lock;
};

struct msg_security_struct {
        unsigned long magic;		/* magic number for this module */
	struct msg_msg *msg;		/* back pointer */
	u32 sid;              /* SID of message */
        struct avc_entry_ref avcr;	/* reference to permissions */
};

struct ipc_security_struct {
        unsigned long magic;		/* magic number for this module */
	struct kern_ipc_perm *ipc_perm; /* back pointer */
	u16 sclass;	/* security class of this object */
	u32 sid;              /* SID of IPC resource */
        struct avc_entry_ref avcr;	/* reference to permissions */
};

struct bprm_security_struct {
	unsigned long magic;           /* magic number for this module */
	struct linux_binprm *bprm;     /* back pointer to bprm object */
	u32 sid;                       /* SID for transformed process */
	unsigned char set;
};

struct netif_security_struct {
	struct net_device *dev;		/* back pointer */
	u32 if_sid;			/* SID for this interface */
	u32 msg_sid;			/* default SID for messages received on this interface */
	struct avc_entry_ref avcr;	/* reference to permissions */
};

struct sk_security_struct {
	unsigned long magic;		/* magic number for this module */
	struct sock *sk;		/* back pointer to sk object */
	u32 peer_sid;			/* SID of peer */
};

extern int inode_security_set_sid(struct inode *inode, u32 sid);

#endif /* _SELINUX_OBJSEC_H_ */
