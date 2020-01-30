/* audit.h -- Auditing support -*- linux-c -*-
 *
 * Copyright 2003-2004 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Written by Rickard E. (Rik) Faith <faith@redhat.com>
 *
 */

#ifndef _LINUX_AUDIT_H_
#define _LINUX_AUDIT_H_

/* Request and reply types */
#define AUDIT_GET      1000	/* Get status */
#define AUDIT_SET      1001	/* Set status (enable/disable/auditd) */
#define AUDIT_LIST     1002	/* List filtering rules */
#define AUDIT_ADD      1003	/* Add filtering rule */
#define AUDIT_DEL      1004	/* Delete filtering rule */
#define AUDIT_USER     1005	/* Send a message from user-space */
#define AUDIT_LOGIN    1006     /* Define the login id and informaiton */
#define AUDIT_KERNEL   2000	/* Asynchronous audit record. NOT A REQUEST. */

/* Rule flags */
#define AUDIT_PER_TASK 0x01	/* Apply rule at task creation (not syscall) */
#define AUDIT_AT_ENTRY 0x02	/* Apply rule at syscall entry */
#define AUDIT_AT_EXIT  0x04	/* Apply rule at syscall exit */
#define AUDIT_PREPEND  0x10	/* Prepend to front of list */

/* Rule actions */
#define AUDIT_NEVER    0	/* Do not build context if rule matches */
#define AUDIT_POSSIBLE 1	/* Build context if rule matches  */
#define AUDIT_ALWAYS   2	/* Generate audit record if rule matches */

/* Rule structure sizes -- if these change, different AUDIT_ADD and
 * AUDIT_LIST commands must be implemented. */
#define AUDIT_MAX_FIELDS   64
#define AUDIT_BITMASK_SIZE 64
#define AUDIT_WORD(nr) ((__u32)((nr)/32))
#define AUDIT_BIT(nr)  (1 << ((nr) - AUDIT_WORD(nr)*32))

/* Rule fields */
				/* These are useful when checking the
				 * task structure at task creation time
				 * (AUDIT_PER_TASK).  */
#define AUDIT_PID	0
#define AUDIT_UID	1
#define AUDIT_EUID	2
#define AUDIT_SUID	3
#define AUDIT_FSUID	4
#define AUDIT_GID	5
#define AUDIT_EGID	6
#define AUDIT_SGID	7
#define AUDIT_FSGID	8
#define AUDIT_LOGINUID	9
#define AUDIT_PERS	10

				/* These are ONLY useful when checking
				 * at syscall exit time (AUDIT_AT_EXIT). */
#define AUDIT_DEVMAJOR	100
#define AUDIT_DEVMINOR	101
#define AUDIT_INODE	102
#define AUDIT_EXIT	103
#define AUDIT_SUCCESS   104	/* exit >= 0; value ignored */

#define AUDIT_ARG0      200
#define AUDIT_ARG1      (AUDIT_ARG0+1)
#define AUDIT_ARG2      (AUDIT_ARG0+2)
#define AUDIT_ARG3      (AUDIT_ARG0+3)

#define AUDIT_NEGATE    0x80000000


/* Status symbols */
				/* Mask values */
#define AUDIT_STATUS_ENABLED		0x0001
#define AUDIT_STATUS_FAILURE		0x0002
#define AUDIT_STATUS_PID		0x0004
#define AUDIT_STATUS_RATE_LIMIT		0x0008
#define AUDIT_STATUS_BACKLOG_LIMIT	0x0010
				/* Failure-to-log actions */
#define AUDIT_FAIL_SILENT	0
#define AUDIT_FAIL_PRINTK	1
#define AUDIT_FAIL_PANIC	2

#ifndef __KERNEL__
struct audit_message {
	struct nlmsghdr nlh;
	char		data[1200];
};
#endif

struct audit_status {
	__u32		mask;		/* Bit mask for valid entries */
	__u32		enabled;	/* 1 = enabled, 0 = disbaled */
	__u32		failure;	/* Failure-to-log action */
	__u32		pid;		/* pid of auditd process */
	__u32		rate_limit;	/* messages rate limit (per second) */
	__u32		backlog_limit;	/* waiting messages limit */
	__u32		lost;		/* messages lost */
	__u32		backlog;	/* messages waiting in queue */
};

struct audit_login {
	__u32		loginuid;
	int		msglen;
	char		msg[1024];
};

struct audit_rule {		/* for AUDIT_LIST, AUDIT_ADD, and AUDIT_DEL */
	__u32		flags;	/* AUDIT_PER_{TASK,CALL}, AUDIT_PREPEND */
	__u32		action;	/* AUDIT_NEVER, AUDIT_POSSIBLE, AUDIT_ALWAYS */
	__u32		field_count;
	__u32		mask[AUDIT_BITMASK_SIZE];
	__u32		fields[AUDIT_MAX_FIELDS];
	__u32		values[AUDIT_MAX_FIELDS];
};

#ifdef __KERNEL__

#ifdef CONFIG_AUDIT
struct audit_buffer;
struct audit_context;
#endif

#ifdef CONFIG_AUDITSYSCALL
/* These are defined in auditsc.c */
				/* Public API */
extern int  audit_alloc(struct task_struct *task);
extern void audit_free(struct task_struct *task);
extern void audit_syscall_entry(struct task_struct *task,
				int major, unsigned long a0, unsigned long a1,
				unsigned long a2, unsigned long a3);
extern void audit_syscall_exit(struct task_struct *task, int return_code);
extern void audit_getname(const char *name);
extern void audit_putname(const char *name);
extern void audit_inode(const char *name, unsigned long ino, dev_t rdev);

				/* Private API (for audit.c only) */
extern int  audit_receive_filter(int type, int pid, int uid, int seq,
				 void *data);
extern void audit_get_stamp(struct audit_context *ctx,
			    struct timespec *t, int *serial);
extern int  audit_set_loginuid(struct audit_context *ctx, uid_t loginuid);
#else
#define audit_alloc(t) ({ 0; })
#define audit_free(t) do { ; } while (0)
#define audit_syscall_entry(t,a,b,c,d,e) do { ; } while (0)
#define audit_syscall_exit(t,r) do { ; } while (0)
#define audit_getname(n) do { ; } while (0)
#define audit_putname(n) do { ; } while (0)
#define audit_inode(n,i,d) do { ; } while (0)
#endif

#ifdef CONFIG_AUDIT
/* These are defined in audit.c */
				/* Public API */
extern void		    audit_log(struct audit_context *ctx,
				      const char *fmt, ...)
			    __attribute__((format(printf,2,3)));

extern struct audit_buffer *audit_log_start(struct audit_context *ctx);
extern void		    audit_log_format(struct audit_buffer *ab,
					     const char *fmt, ...)
			    __attribute__((format(printf,2,3)));
extern void		    audit_log_end(struct audit_buffer *ab);
extern void		    audit_log_end_fast(struct audit_buffer *ab);
extern void		    audit_log_end_irq(struct audit_buffer *ab);
extern void		    audit_log_d_path(struct audit_buffer *ab,
					     const char *prefix,
					     struct dentry *dentry,
					     struct vfsmount *vfsmnt);
extern int		    audit_set_rate_limit(int limit);
extern int		    audit_set_backlog_limit(int limit);
extern int		    audit_set_enabled(int state);
extern int		    audit_set_failure(int state);

				/* Private API (for auditsc.c only) */
extern void		    audit_send_reply(int pid, int seq, int type,
					     int done, int multi,
					     void *payload, int size);
extern void		    audit_log_lost(const char *message);
#else
#define audit_log(t,f,...) do { ; } while (0)
#define audit_log_start(t) ({ NULL; })
#define audit_log_vformat(b,f,a) do { ; } while (0)
#define audit_log_format(b,f,...) do { ; } while (0)
#define audit_log_end(b) do { ; } while (0)
#define audit_log_end_fast(b) do { ; } while (0)
#define audit_log_end_irq(b) do { ; } while (0)
#define audit_log_d_path(b,p,d,v) do { ; } while (0)
#define audit_set_rate_limit(l) do { ; } while (0)
#define audit_set_backlog_limit(l) do { ; } while (0)
#define audit_set_enabled(s) do { ; } while (0)
#define audit_set_failure(s) do { ; } while (0)
#endif
#endif
#endif
