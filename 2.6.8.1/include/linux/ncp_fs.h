/*
 *  ncp_fs.h
 *
 *  Copyright (C) 1995, 1996 by Volker Lendecke
 *
 */

#ifndef _LINUX_NCP_FS_H
#define _LINUX_NCP_FS_H

#include <linux/fs.h>
#include <linux/in.h>
#include <linux/types.h>

#include <linux/ncp_fs_i.h>
#include <linux/ncp_fs_sb.h>
#include <linux/ipx.h>
#include <linux/ncp_no.h>

/*
 * ioctl commands
 */

struct ncp_ioctl_request {
	unsigned int function;
	unsigned int size;
	char __user *data;
};

struct ncp_fs_info {
	int version;
	struct sockaddr_ipx addr;
	__kernel_uid_t mounted_uid;
	int connection;		/* Connection number the server assigned us */
	int buffer_size;	/* The negotiated buffer size, to be
				   used for read/write requests! */

	int volume_number;
	__u32 directory_id;
};

struct ncp_fs_info_v2 {
	int version;
	unsigned long mounted_uid;
	unsigned int connection;
	unsigned int buffer_size;

	unsigned int volume_number;
	__u32 directory_id;

	__u32 dummy1;
	__u32 dummy2;
	__u32 dummy3;
};

struct ncp_sign_init
{
	char sign_root[8];
	char sign_last[16];
};

struct ncp_lock_ioctl
{
#define NCP_LOCK_LOG	0
#define NCP_LOCK_SH	1
#define NCP_LOCK_EX	2
#define NCP_LOCK_CLEAR	256
	int		cmd;
	int		origin;
	unsigned int	offset;
	unsigned int	length;
#define NCP_LOCK_DEFAULT_TIMEOUT	18
#define NCP_LOCK_MAX_TIMEOUT		180
	int		timeout;
};

struct ncp_setroot_ioctl
{
	int		volNumber;
	int		namespace;
	__u32		dirEntNum;
};

struct ncp_objectname_ioctl
{
#define NCP_AUTH_NONE	0x00
#define NCP_AUTH_BIND	0x31
#define NCP_AUTH_NDS	0x32
	int		auth_type;
	size_t		object_name_len;
	void __user *	object_name;	/* an userspace data, in most cases user name */
};

struct ncp_privatedata_ioctl
{
	size_t		len;
	void __user *	data;		/* ~1000 for NDS */
};

/* NLS charsets by ioctl */
#define NCP_IOCSNAME_LEN 20
struct ncp_nls_ioctl
{
	unsigned char codepage[NCP_IOCSNAME_LEN+1];
	unsigned char iocharset[NCP_IOCSNAME_LEN+1];
};

#define	NCP_IOC_NCPREQUEST		_IOR('n', 1, struct ncp_ioctl_request)
#define	NCP_IOC_GETMOUNTUID		_IOW('n', 2, __kernel_old_uid_t)
#define NCP_IOC_GETMOUNTUID2		_IOW('n', 2, unsigned long)

#define NCP_IOC_CONN_LOGGED_IN          _IO('n', 3)

#define NCP_GET_FS_INFO_VERSION    (1)
#define NCP_IOC_GET_FS_INFO             _IOWR('n', 4, struct ncp_fs_info)
#define NCP_GET_FS_INFO_VERSION_V2 (2)
#define NCP_IOC_GET_FS_INFO_V2		_IOWR('n', 4, struct ncp_fs_info_v2)

#define NCP_IOC_SIGN_INIT		_IOR('n', 5, struct ncp_sign_init)
#define NCP_IOC_SIGN_WANTED		_IOR('n', 6, int)
#define NCP_IOC_SET_SIGN_WANTED		_IOW('n', 6, int)

#define NCP_IOC_LOCKUNLOCK		_IOR('n', 7, struct ncp_lock_ioctl)

#define NCP_IOC_GETROOT			_IOW('n', 8, struct ncp_setroot_ioctl)
#define NCP_IOC_SETROOT			_IOR('n', 8, struct ncp_setroot_ioctl)

#define NCP_IOC_GETOBJECTNAME		_IOWR('n', 9, struct ncp_objectname_ioctl)
#define NCP_IOC_SETOBJECTNAME		_IOR('n', 9, struct ncp_objectname_ioctl)
#define NCP_IOC_GETPRIVATEDATA		_IOWR('n', 10, struct ncp_privatedata_ioctl)
#define NCP_IOC_SETPRIVATEDATA		_IOR('n', 10, struct ncp_privatedata_ioctl)

#define NCP_IOC_GETCHARSETS		_IOWR('n', 11, struct ncp_nls_ioctl)
#define NCP_IOC_SETCHARSETS		_IOR('n', 11, struct ncp_nls_ioctl)

#define NCP_IOC_GETDENTRYTTL		_IOW('n', 12, __u32)
#define NCP_IOC_SETDENTRYTTL		_IOR('n', 12, __u32)

/*
 * The packet size to allocate. One page should be enough.
 */
#define NCP_PACKET_SIZE 4070

#define NCP_MAXPATHLEN 255
#define NCP_MAXNAMELEN 14

#ifdef __KERNEL__

#include <linux/config.h>

/* undef because public define in umsdos_fs.h (ncp_fs.h isn't public) */
#undef PRINTK
/* define because it is easy to change PRINTK to {*}PRINTK */
#define PRINTK(format, args...) printk(KERN_DEBUG format , ## args)

#undef NCPFS_PARANOIA
#ifdef NCPFS_PARANOIA
#define PPRINTK(format, args...) PRINTK(format , ## args)
#else
#define PPRINTK(format, args...)
#endif

#ifndef DEBUG_NCP
#define DEBUG_NCP 0
#endif
#if DEBUG_NCP > 0
#define DPRINTK(format, args...) PRINTK(format , ## args)
#else
#define DPRINTK(format, args...)
#endif
#if DEBUG_NCP > 1
#define DDPRINTK(format, args...) PRINTK(format , ## args)
#else
#define DDPRINTK(format, args...)
#endif

#define NCP_MAX_RPC_TIMEOUT (6*HZ)


struct ncp_entry_info {
	struct nw_info_struct	i;
	ino_t			ino;
	int			opened;
	int			access;
	unsigned int		volume;
	__u8			file_handle[6];
};

/* Guess, what 0x564c is :-) */
#define NCP_SUPER_MAGIC  0x564c


static inline struct ncp_server *NCP_SBP(struct super_block *sb)
{
	return sb->s_fs_info;
}

#define NCP_SERVER(inode)	NCP_SBP((inode)->i_sb)
static inline struct ncp_inode_info *NCP_FINFO(struct inode *inode)
{
	return container_of(inode, struct ncp_inode_info, vfs_inode);
}

#ifdef DEBUG_NCP_MALLOC

#include <linux/slab.h>

extern int ncp_malloced;
extern int ncp_current_malloced;

static inline void *
 ncp_kmalloc(unsigned int size, int priority)
{
	ncp_malloced += 1;
	ncp_current_malloced += 1;
	return kmalloc(size, priority);
}

static inline void ncp_kfree_s(void *obj, int size)
{
	ncp_current_malloced -= 1;
	kfree(obj);
}

#else				/* DEBUG_NCP_MALLOC */

#define ncp_kmalloc(s,p) kmalloc(s,p)
#define ncp_kfree_s(o,s) kfree(o)

#endif				/* DEBUG_NCP_MALLOC */

/* linux/fs/ncpfs/inode.c */
int ncp_notify_change(struct dentry *, struct iattr *);
struct inode *ncp_iget(struct super_block *, struct ncp_entry_info *);
void ncp_update_inode(struct inode *, struct ncp_entry_info *);
void ncp_update_inode2(struct inode *, struct ncp_entry_info *);

/* linux/fs/ncpfs/dir.c */
extern struct inode_operations ncp_dir_inode_operations;
extern struct file_operations ncp_dir_operations;
int ncp_conn_logged_in(struct super_block *);
int ncp_date_dos2unix(__u16 time, __u16 date);
void ncp_date_unix2dos(int unix_date, __u16 * time, __u16 * date);

/* linux/fs/ncpfs/ioctl.c */
int ncp_ioctl(struct inode *, struct file *, unsigned int, unsigned long);

/* linux/fs/ncpfs/sock.c */
int ncp_request2(struct ncp_server *server, int function,
	void* reply, int max_reply_size);
static inline int ncp_request(struct ncp_server *server, int function) {
	return ncp_request2(server, function, server->packet, server->packet_size);
}
int ncp_connect(struct ncp_server *server);
int ncp_disconnect(struct ncp_server *server);
void ncp_lock_server(struct ncp_server *server);
void ncp_unlock_server(struct ncp_server *server);

/* linux/fs/ncpfs/file.c */
extern struct inode_operations ncp_file_inode_operations;
extern struct file_operations ncp_file_operations;
int ncp_make_open(struct inode *, int);

/* linux/fs/ncpfs/mmap.c */
int ncp_mmap(struct file *, struct vm_area_struct *);

/* linux/fs/ncpfs/ncplib_kernel.c */
int ncp_make_closed(struct inode *);

#define ncp_namespace(i)	(NCP_SERVER(i)->name_space[NCP_FINFO(i)->volNumber])

static inline int ncp_preserve_entry_case(struct inode *i, __u32 nscreator)
{
#ifdef CONFIG_NCPFS_SMALLDOS
	int ns = ncp_namespace(i);

	if ((ns == NW_NS_DOS)
#ifdef CONFIG_NCPFS_OS2_NS
		|| ((ns == NW_NS_OS2) && (nscreator == NW_NS_DOS))
#endif /* CONFIG_NCPFS_OS2_NS */
				)
		return 0;
#endif /* CONFIG_NCPFS_SMALLDOS */
	return 1;
}

#define ncp_preserve_case(i)	(ncp_namespace(i) != NW_NS_DOS)

static inline int ncp_case_sensitive(struct inode *i)
{
#ifdef CONFIG_NCPFS_NFS_NS
	return ncp_namespace(i) == NW_NS_NFS;
#else
	return 0;
#endif	/* CONFIG_NCPFS_NFS_NS */
} 

#endif				/* __KERNEL__ */

#endif				/* _LINUX_NCP_FS_H */
