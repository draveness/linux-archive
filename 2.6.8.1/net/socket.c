/*
 * NET		An implementation of the SOCKET network access protocol.
 *
 * Version:	@(#)socket.c	1.1.93	18/02/95
 *
 * Authors:	Orest Zborowski, <obz@Kodak.COM>
 *		Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Anonymous	:	NOTSOCK/BADF cleanup. Error fix in
 *					shutdown()
 *		Alan Cox	:	verify_area() fixes
 *		Alan Cox	:	Removed DDI
 *		Jonathan Kamens	:	SOCK_DGRAM reconnect bug
 *		Alan Cox	:	Moved a load of checks to the very
 *					top level.
 *		Alan Cox	:	Move address structures to/from user
 *					mode above the protocol layers.
 *		Rob Janssen	:	Allow 0 length sends.
 *		Alan Cox	:	Asynchronous I/O support (cribbed from the
 *					tty drivers).
 *		Niibe Yutaka	:	Asynchronous I/O for writes (4.4BSD style)
 *		Jeff Uphoff	:	Made max number of sockets command-line
 *					configurable.
 *		Matti Aarnio	:	Made the number of sockets dynamic,
 *					to be allocated when needed, and mr.
 *					Uphoff's max is used as max to be
 *					allowed to allocate.
 *		Linus		:	Argh. removed all the socket allocation
 *					altogether: it's in the inode now.
 *		Alan Cox	:	Made sock_alloc()/sock_release() public
 *					for NetROM and future kernel nfsd type
 *					stuff.
 *		Alan Cox	:	sendmsg/recvmsg basics.
 *		Tom Dyas	:	Export net symbols.
 *		Marcin Dalecki	:	Fixed problems with CONFIG_NET="n".
 *		Alan Cox	:	Added thread locking to sys_* calls
 *					for sockets. May have errors at the
 *					moment.
 *		Kevin Buhr	:	Fixed the dumb errors in the above.
 *		Andi Kleen	:	Some small cleanups, optimizations,
 *					and fixed a copy_from_user() bug.
 *		Tigran Aivazian	:	sys_send(args) calls sys_sendto(args, NULL, 0)
 *		Tigran Aivazian	:	Made listen(2) backlog sanity checks 
 *					protocol-independent
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *
 *	This module is effectively the top level interface to the BSD socket
 *	paradigm. 
 *
 *	Based upon Swansea University Computer Society NET3.039
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/socket.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/wanrouter.h>
#include <linux/if_bridge.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/cache.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/divert.h>
#include <linux/mount.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/compat.h>
#include <linux/kmod.h>

#ifdef CONFIG_NET_RADIO
#include <linux/wireless.h>		/* Note : will define WIRELESS_EXT */
#endif	/* CONFIG_NET_RADIO */

#include <asm/uaccess.h>
#include <asm/unistd.h>

#include <net/compat.h>

#include <net/sock.h>
#include <linux/netfilter.h>

static int sock_no_open(struct inode *irrelevant, struct file *dontcare);
static ssize_t sock_aio_read(struct kiocb *iocb, char __user *buf,
			 size_t size, loff_t pos);
static ssize_t sock_aio_write(struct kiocb *iocb, const char __user *buf,
			  size_t size, loff_t pos);
static int sock_mmap(struct file *file, struct vm_area_struct * vma);

static int sock_close(struct inode *inode, struct file *file);
static unsigned int sock_poll(struct file *file,
			      struct poll_table_struct *wait);
static int sock_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg);
static int sock_fasync(int fd, struct file *filp, int on);
static ssize_t sock_readv(struct file *file, const struct iovec *vector,
			  unsigned long count, loff_t *ppos);
static ssize_t sock_writev(struct file *file, const struct iovec *vector,
			  unsigned long count, loff_t *ppos);
static ssize_t sock_sendpage(struct file *file, struct page *page,
			     int offset, size_t size, loff_t *ppos, int more);


/*
 *	Socket files have a set of 'special' operations as well as the generic file ones. These don't appear
 *	in the operation structures but are done directly via the socketcall() multiplexor.
 */

static struct file_operations socket_file_ops = {
	.owner =	THIS_MODULE,
	.llseek =	no_llseek,
	.aio_read =	sock_aio_read,
	.aio_write =	sock_aio_write,
	.poll =		sock_poll,
	.ioctl =	sock_ioctl,
	.mmap =		sock_mmap,
	.open =		sock_no_open,	/* special open code to disallow open via /proc */
	.release =	sock_close,
	.fasync =	sock_fasync,
	.readv =	sock_readv,
	.writev =	sock_writev,
	.sendpage =	sock_sendpage
};

/*
 *	The protocol list. Each protocol is registered in here.
 */

static struct net_proto_family *net_families[NPROTO];

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT)
static atomic_t net_family_lockct = ATOMIC_INIT(0);
static spinlock_t net_family_lock = SPIN_LOCK_UNLOCKED;

/* The strategy is: modifications net_family vector are short, do not
   sleep and veeery rare, but read access should be free of any exclusive
   locks.
 */

static void net_family_write_lock(void)
{
	spin_lock(&net_family_lock);
	while (atomic_read(&net_family_lockct) != 0) {
		spin_unlock(&net_family_lock);

		yield();

		spin_lock(&net_family_lock);
	}
}

static __inline__ void net_family_write_unlock(void)
{
	spin_unlock(&net_family_lock);
}

static __inline__ void net_family_read_lock(void)
{
	atomic_inc(&net_family_lockct);
	spin_unlock_wait(&net_family_lock);
}

static __inline__ void net_family_read_unlock(void)
{
	atomic_dec(&net_family_lockct);
}

#else
#define net_family_write_lock() do { } while(0)
#define net_family_write_unlock() do { } while(0)
#define net_family_read_lock() do { } while(0)
#define net_family_read_unlock() do { } while(0)
#endif


/*
 *	Statistics counters of the socket lists
 */

static DEFINE_PER_CPU(int, sockets_in_use) = 0;

/*
 *	Support routines. Move socket addresses back and forth across the kernel/user
 *	divide and look after the messy bits.
 */

#define MAX_SOCK_ADDR	128		/* 108 for Unix domain - 
					   16 for IP, 16 for IPX,
					   24 for IPv6,
					   about 80 for AX.25 
					   must be at least one bigger than
					   the AF_UNIX size (see net/unix/af_unix.c
					   :unix_mkname()).  
					 */
					 
/**
 *	move_addr_to_kernel	-	copy a socket address into kernel space
 *	@uaddr: Address in user space
 *	@kaddr: Address in kernel space
 *	@ulen: Length in user space
 *
 *	The address is copied into kernel space. If the provided address is
 *	too long an error code of -EINVAL is returned. If the copy gives
 *	invalid addresses -EFAULT is returned. On a success 0 is returned.
 */

int move_addr_to_kernel(void __user *uaddr, int ulen, void *kaddr)
{
	if(ulen<0||ulen>MAX_SOCK_ADDR)
		return -EINVAL;
	if(ulen==0)
		return 0;
	if(copy_from_user(kaddr,uaddr,ulen))
		return -EFAULT;
	return 0;
}

/**
 *	move_addr_to_user	-	copy an address to user space
 *	@kaddr: kernel space address
 *	@klen: length of address in kernel
 *	@uaddr: user space address
 *	@ulen: pointer to user length field
 *
 *	The value pointed to by ulen on entry is the buffer length available.
 *	This is overwritten with the buffer space used. -EINVAL is returned
 *	if an overlong buffer is specified or a negative buffer size. -EFAULT
 *	is returned if either the buffer or the length field are not
 *	accessible.
 *	After copying the data up to the limit the user specifies, the true
 *	length of the data is written over the length limit the user
 *	specified. Zero is returned for a success.
 */
 
int move_addr_to_user(void *kaddr, int klen, void __user *uaddr, int __user *ulen)
{
	int err;
	int len;

	if((err=get_user(len, ulen)))
		return err;
	if(len>klen)
		len=klen;
	if(len<0 || len> MAX_SOCK_ADDR)
		return -EINVAL;
	if(len)
	{
		if(copy_to_user(uaddr,kaddr,len))
			return -EFAULT;
	}
	/*
	 *	"fromlen shall refer to the value before truncation.."
	 *			1003.1g
	 */
	return __put_user(klen, ulen);
}

#define SOCKFS_MAGIC 0x534F434B

static kmem_cache_t * sock_inode_cachep;

static struct inode *sock_alloc_inode(struct super_block *sb)
{
	struct socket_alloc *ei;
	ei = (struct socket_alloc *)kmem_cache_alloc(sock_inode_cachep, SLAB_KERNEL);
	if (!ei)
		return NULL;
	init_waitqueue_head(&ei->socket.wait);
	
	ei->socket.fasync_list = NULL;
	ei->socket.state = SS_UNCONNECTED;
	ei->socket.flags = 0;
	ei->socket.ops = NULL;
	ei->socket.sk = NULL;
	ei->socket.file = NULL;
	ei->socket.passcred = 0;

	return &ei->vfs_inode;
}

static void sock_destroy_inode(struct inode *inode)
{
	kmem_cache_free(sock_inode_cachep,
			container_of(inode, struct socket_alloc, vfs_inode));
}

static void init_once(void * foo, kmem_cache_t * cachep, unsigned long flags)
{
	struct socket_alloc *ei = (struct socket_alloc *) foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR)
		inode_init_once(&ei->vfs_inode);
}
 
static int init_inodecache(void)
{
	sock_inode_cachep = kmem_cache_create("sock_inode_cache",
				sizeof(struct socket_alloc),
				0, SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT,
				init_once, NULL);
	if (sock_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static struct super_operations sockfs_ops = {
	.alloc_inode =	sock_alloc_inode,
	.destroy_inode =sock_destroy_inode,
	.statfs =	simple_statfs,
};

static struct super_block *sockfs_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return get_sb_pseudo(fs_type, "socket:", &sockfs_ops, SOCKFS_MAGIC);
}

static struct vfsmount *sock_mnt;

static struct file_system_type sock_fs_type = {
	.name =		"sockfs",
	.get_sb =	sockfs_get_sb,
	.kill_sb =	kill_anon_super,
};
static int sockfs_delete_dentry(struct dentry *dentry)
{
	return 1;
}
static struct dentry_operations sockfs_dentry_operations = {
	.d_delete =	sockfs_delete_dentry,
};

/*
 *	Obtains the first available file descriptor and sets it up for use.
 *
 *	This function creates file structure and maps it to fd space
 *	of current process. On success it returns file descriptor
 *	and file struct implicitly stored in sock->file.
 *	Note that another thread may close file descriptor before we return
 *	from this function. We use the fact that now we do not refer
 *	to socket after mapping. If one day we will need it, this
 *	function will increment ref. count on file by 1.
 *
 *	In any case returned fd MAY BE not valid!
 *	This race condition is unavoidable
 *	with shared fd spaces, we cannot solve it inside kernel,
 *	but we take care of internal coherence yet.
 */

int sock_map_fd(struct socket *sock)
{
	int fd;
	struct qstr this;
	char name[32];

	/*
	 *	Find a file descriptor suitable for return to the user. 
	 */

	fd = get_unused_fd();
	if (fd >= 0) {
		struct file *file = get_empty_filp();

		if (!file) {
			put_unused_fd(fd);
			fd = -ENFILE;
			goto out;
		}

		sprintf(name, "[%lu]", SOCK_INODE(sock)->i_ino);
		this.name = name;
		this.len = strlen(name);
		this.hash = SOCK_INODE(sock)->i_ino;

		file->f_dentry = d_alloc(sock_mnt->mnt_sb->s_root, &this);
		if (!file->f_dentry) {
			put_filp(file);
			put_unused_fd(fd);
			fd = -ENOMEM;
			goto out;
		}
		file->f_dentry->d_op = &sockfs_dentry_operations;
		d_add(file->f_dentry, SOCK_INODE(sock));
		file->f_vfsmnt = mntget(sock_mnt);
		file->f_mapping = file->f_dentry->d_inode->i_mapping;

		sock->file = file;
		file->f_op = SOCK_INODE(sock)->i_fop = &socket_file_ops;
		file->f_mode = FMODE_READ | FMODE_WRITE;
		file->f_flags = O_RDWR;
		file->f_pos = 0;
		fd_install(fd, file);
	}

out:
	return fd;
}

/**
 *	sockfd_lookup	- 	Go from a file number to its socket slot
 *	@fd: file handle
 *	@err: pointer to an error code return
 *
 *	The file handle passed in is locked and the socket it is bound
 *	too is returned. If an error occurs the err pointer is overwritten
 *	with a negative errno code and NULL is returned. The function checks
 *	for both invalid handles and passing a handle which is not a socket.
 *
 *	On a success the socket object pointer is returned.
 */

struct socket *sockfd_lookup(int fd, int *err)
{
	struct file *file;
	struct inode *inode;
	struct socket *sock;

	if (!(file = fget(fd)))
	{
		*err = -EBADF;
		return NULL;
	}

	inode = file->f_dentry->d_inode;
	if (!inode->i_sock || !(sock = SOCKET_I(inode)))
	{
		*err = -ENOTSOCK;
		fput(file);
		return NULL;
	}

	if (sock->file != file) {
		printk(KERN_ERR "socki_lookup: socket file changed!\n");
		sock->file = file;
	}
	return sock;
}

/**
 *	sock_alloc	-	allocate a socket
 *	
 *	Allocate a new inode and socket object. The two are bound together
 *	and initialised. The socket is then returned. If we are out of inodes
 *	NULL is returned.
 */

struct socket *sock_alloc(void)
{
	struct inode * inode;
	struct socket * sock;

	inode = new_inode(sock_mnt->mnt_sb);
	if (!inode)
		return NULL;

	sock = SOCKET_I(inode);

	inode->i_mode = S_IFSOCK|S_IRWXUGO;
	inode->i_sock = 1;
	inode->i_uid = current->fsuid;
	inode->i_gid = current->fsgid;

	get_cpu_var(sockets_in_use)++;
	put_cpu_var(sockets_in_use);
	return sock;
}

/*
 *	In theory you can't get an open on this inode, but /proc provides
 *	a back door. Remember to keep it shut otherwise you'll let the
 *	creepy crawlies in.
 */
  
static int sock_no_open(struct inode *irrelevant, struct file *dontcare)
{
	return -ENXIO;
}

struct file_operations bad_sock_fops = {
	.owner = THIS_MODULE,
	.open = sock_no_open,
};

/**
 *	sock_release	-	close a socket
 *	@sock: socket to close
 *
 *	The socket is released from the protocol stack if it has a release
 *	callback, and the inode is then released if the socket is bound to
 *	an inode not a file. 
 */
 
void sock_release(struct socket *sock)
{
	if (sock->ops) {
		struct module *owner = sock->ops->owner;

		sock->ops->release(sock);
		sock->ops = NULL;
		module_put(owner);
	}

	if (sock->fasync_list)
		printk(KERN_ERR "sock_release: fasync list not empty!\n");

	get_cpu_var(sockets_in_use)--;
	put_cpu_var(sockets_in_use);
	if (!sock->file) {
		iput(SOCK_INODE(sock));
		return;
	}
	sock->file=NULL;
}

static inline int __sock_sendmsg(struct kiocb *iocb, struct socket *sock, 
				 struct msghdr *msg, size_t size)
{
	struct sock_iocb *si = kiocb_to_siocb(iocb);
	int err;

	si->sock = sock;
	si->scm = NULL;
	si->msg = msg;
	si->size = size;

	err = security_socket_sendmsg(sock, msg, size);
	if (err)
		return err;

	return sock->ops->sendmsg(iocb, sock, msg, size);
}

int sock_sendmsg(struct socket *sock, struct msghdr *msg, size_t size)
{
	struct kiocb iocb;
	struct sock_iocb siocb;
	int ret;

	init_sync_kiocb(&iocb, NULL);
	iocb.private = &siocb;
	ret = __sock_sendmsg(&iocb, sock, msg, size);
	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&iocb);
	return ret;
}

int kernel_sendmsg(struct socket *sock, struct msghdr *msg,
		   struct kvec *vec, size_t num, size_t size)
{
	mm_segment_t oldfs = get_fs();
	int result;

	set_fs(KERNEL_DS);
	/*
	 * the following is safe, since for compiler definitions of kvec and
	 * iovec are identical, yielding the same in-core layout and alignment
	 */
	msg->msg_iov = (struct iovec *)vec,
	msg->msg_iovlen = num;
	result = sock_sendmsg(sock, msg, size);
	set_fs(oldfs);
	return result;
}

static inline int __sock_recvmsg(struct kiocb *iocb, struct socket *sock, 
				 struct msghdr *msg, size_t size, int flags)
{
	int err;
	struct sock_iocb *si = kiocb_to_siocb(iocb);

	si->sock = sock;
	si->scm = NULL;
	si->msg = msg;
	si->size = size;
	si->flags = flags;

	err = security_socket_recvmsg(sock, msg, size, flags);
	if (err)
		return err;

	return sock->ops->recvmsg(iocb, sock, msg, size, flags);
}

int sock_recvmsg(struct socket *sock, struct msghdr *msg, 
		 size_t size, int flags)
{
	struct kiocb iocb;
	struct sock_iocb siocb;
	int ret;

        init_sync_kiocb(&iocb, NULL);
	iocb.private = &siocb;
	ret = __sock_recvmsg(&iocb, sock, msg, size, flags);
	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&iocb);
	return ret;
}

int kernel_recvmsg(struct socket *sock, struct msghdr *msg, 
		   struct kvec *vec, size_t num,
		   size_t size, int flags)
{
	mm_segment_t oldfs = get_fs();
	int result;

	set_fs(KERNEL_DS);
	/*
	 * the following is safe, since for compiler definitions of kvec and
	 * iovec are identical, yielding the same in-core layout and alignment
	 */
	msg->msg_iov = (struct iovec *)vec,
	msg->msg_iovlen = num;
	result = sock_recvmsg(sock, msg, size, flags);
	set_fs(oldfs);
	return result;
}

static void sock_aio_dtor(struct kiocb *iocb)
{
	kfree(iocb->private);
}

/*
 *	Read data from a socket. ubuf is a user mode pointer. We make sure the user
 *	area ubuf...ubuf+size-1 is writable before asking the protocol.
 */

static ssize_t sock_aio_read(struct kiocb *iocb, char __user *ubuf,
			 size_t size, loff_t pos)
{
	struct sock_iocb *x, siocb;
	struct socket *sock;
	int flags;

	if (pos != 0)
		return -ESPIPE;
	if (size==0)		/* Match SYS5 behaviour */
		return 0;

	if (is_sync_kiocb(iocb))
		x = &siocb;
	else {
		x = kmalloc(sizeof(struct sock_iocb), GFP_KERNEL);
		if (!x)
			return -ENOMEM;
		iocb->ki_dtor = sock_aio_dtor;
	}
	iocb->private = x;
	x->kiocb = iocb;
	sock = SOCKET_I(iocb->ki_filp->f_dentry->d_inode); 

	x->async_msg.msg_name = NULL;
	x->async_msg.msg_namelen = 0;
	x->async_msg.msg_iov = &x->async_iov;
	x->async_msg.msg_iovlen = 1;
	x->async_msg.msg_control = NULL;
	x->async_msg.msg_controllen = 0;
	x->async_iov.iov_base = ubuf;
	x->async_iov.iov_len = size;
	flags = !(iocb->ki_filp->f_flags & O_NONBLOCK) ? 0 : MSG_DONTWAIT;

	return __sock_recvmsg(iocb, sock, &x->async_msg, size, flags);
}


/*
 *	Write data to a socket. We verify that the user area ubuf..ubuf+size-1
 *	is readable by the user process.
 */

static ssize_t sock_aio_write(struct kiocb *iocb, const char __user *ubuf,
			  size_t size, loff_t pos)
{
	struct sock_iocb *x, siocb;
	struct socket *sock;
	
	if (pos != 0)
		return -ESPIPE;
	if(size==0)		/* Match SYS5 behaviour */
		return 0;

	if (is_sync_kiocb(iocb))
		x = &siocb;
	else {
		x = kmalloc(sizeof(struct sock_iocb), GFP_KERNEL);
		if (!x)
			return -ENOMEM;
		iocb->ki_dtor = sock_aio_dtor;
	}
	iocb->private = x;
	x->kiocb = iocb;
	sock = SOCKET_I(iocb->ki_filp->f_dentry->d_inode); 

	x->async_msg.msg_name = NULL;
	x->async_msg.msg_namelen = 0;
	x->async_msg.msg_iov = &x->async_iov;
	x->async_msg.msg_iovlen = 1;
	x->async_msg.msg_control = NULL;
	x->async_msg.msg_controllen = 0;
	x->async_msg.msg_flags = !(iocb->ki_filp->f_flags & O_NONBLOCK) ? 0 : MSG_DONTWAIT;
	if (sock->type == SOCK_SEQPACKET)
		x->async_msg.msg_flags |= MSG_EOR;
	x->async_iov.iov_base = (void __user *)ubuf;
	x->async_iov.iov_len = size;
	
	return __sock_sendmsg(iocb, sock, &x->async_msg, size);
}

ssize_t sock_sendpage(struct file *file, struct page *page,
		      int offset, size_t size, loff_t *ppos, int more)
{
	struct socket *sock;
	int flags;

	sock = SOCKET_I(file->f_dentry->d_inode);

	flags = !(file->f_flags & O_NONBLOCK) ? 0 : MSG_DONTWAIT;
	if (more)
		flags |= MSG_MORE;

	return sock->ops->sendpage(sock, page, offset, size, flags);
}

int sock_readv_writev(int type, struct inode * inode, struct file * file,
		      const struct iovec * iov, long count, size_t size)
{
	struct msghdr msg;
	struct socket *sock;

	sock = SOCKET_I(inode);

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_iov = (struct iovec *) iov;
	msg.msg_iovlen = count;
	msg.msg_flags = (file->f_flags & O_NONBLOCK) ? MSG_DONTWAIT : 0;

	/* read() does a VERIFY_WRITE */
	if (type == VERIFY_WRITE)
		return sock_recvmsg(sock, &msg, size, msg.msg_flags);

	if (sock->type == SOCK_SEQPACKET)
		msg.msg_flags |= MSG_EOR;

	return sock_sendmsg(sock, &msg, size);
}

static ssize_t sock_readv(struct file *file, const struct iovec *vector,
			  unsigned long count, loff_t *ppos)
{
	size_t tot_len = 0;
	int i;
        for (i = 0 ; i < count ; i++)
                tot_len += vector[i].iov_len;
	return sock_readv_writev(VERIFY_WRITE, file->f_dentry->d_inode,
				 file, vector, count, tot_len);
}
	
static ssize_t sock_writev(struct file *file, const struct iovec *vector,
			   unsigned long count, loff_t *ppos)
{
	size_t tot_len = 0;
	int i;
        for (i = 0 ; i < count ; i++)
                tot_len += vector[i].iov_len;
	return sock_readv_writev(VERIFY_READ, file->f_dentry->d_inode,
				 file, vector, count, tot_len);
}


/*
 * Atomic setting of ioctl hooks to avoid race
 * with module unload.
 */

static DECLARE_MUTEX(br_ioctl_mutex);
static int (*br_ioctl_hook)(unsigned int cmd, void __user *arg) = NULL;

void brioctl_set(int (*hook)(unsigned int, void __user *))
{
	down(&br_ioctl_mutex);
	br_ioctl_hook = hook;
	up(&br_ioctl_mutex);
}
EXPORT_SYMBOL(brioctl_set);

static DECLARE_MUTEX(vlan_ioctl_mutex);
static int (*vlan_ioctl_hook)(void __user *arg);

void vlan_ioctl_set(int (*hook)(void __user *))
{
	down(&vlan_ioctl_mutex);
	vlan_ioctl_hook = hook;
	up(&vlan_ioctl_mutex);
}
EXPORT_SYMBOL(vlan_ioctl_set);

static DECLARE_MUTEX(dlci_ioctl_mutex);
static int (*dlci_ioctl_hook)(unsigned int, void __user *);

void dlci_ioctl_set(int (*hook)(unsigned int, void __user *))
{
	down(&dlci_ioctl_mutex);
	dlci_ioctl_hook = hook;
	up(&dlci_ioctl_mutex);
}
EXPORT_SYMBOL(dlci_ioctl_set);

/*
 *	With an ioctl, arg may well be a user mode pointer, but we don't know
 *	what to do with it - that's up to the protocol still.
 */

static int sock_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
		      unsigned long arg)
{
	struct socket *sock;
	void __user *argp = (void __user *)arg;
	int pid, err;

	unlock_kernel();
	sock = SOCKET_I(inode);
	if (cmd >= SIOCDEVPRIVATE && cmd <= (SIOCDEVPRIVATE + 15)) {
		err = dev_ioctl(cmd, argp);
	} else
#ifdef WIRELESS_EXT
	if (cmd >= SIOCIWFIRST && cmd <= SIOCIWLAST) {
		err = dev_ioctl(cmd, argp);
	} else
#endif	/* WIRELESS_EXT */
	switch (cmd) {
		case FIOSETOWN:
		case SIOCSPGRP:
			err = -EFAULT;
			if (get_user(pid, (int __user *)argp))
				break;
			err = f_setown(sock->file, pid, 1);
			break;
		case FIOGETOWN:
		case SIOCGPGRP:
			err = put_user(sock->file->f_owner.pid, (int __user *)argp);
			break;
		case SIOCGIFBR:
		case SIOCSIFBR:
		case SIOCBRADDBR:
		case SIOCBRDELBR:
			err = -ENOPKG;
			if (!br_ioctl_hook)
				request_module("bridge");

			down(&br_ioctl_mutex);
			if (br_ioctl_hook) 
				err = br_ioctl_hook(cmd, argp);
			up(&br_ioctl_mutex);
			break;
		case SIOCGIFVLAN:
		case SIOCSIFVLAN:
			err = -ENOPKG;
			if (!vlan_ioctl_hook)
				request_module("8021q");

			down(&vlan_ioctl_mutex);
			if (vlan_ioctl_hook)
				err = vlan_ioctl_hook(argp);
			up(&vlan_ioctl_mutex);
			break;
		case SIOCGIFDIVERT:
		case SIOCSIFDIVERT:
		/* Convert this to call through a hook */
			err = divert_ioctl(cmd, argp);
			break;
		case SIOCADDDLCI:
		case SIOCDELDLCI:
			err = -ENOPKG;
			if (!dlci_ioctl_hook)
				request_module("dlci");

			if (dlci_ioctl_hook) {
				down(&dlci_ioctl_mutex);
				err = dlci_ioctl_hook(cmd, argp);
				up(&dlci_ioctl_mutex);
			}
			break;
		default:
			err = sock->ops->ioctl(sock, cmd, arg);
			break;
	}
	lock_kernel();

	return err;
}

int sock_create_lite(int family, int type, int protocol, struct socket **res)
{
	int err;
	struct socket *sock = NULL;
	
	err = security_socket_create(family, type, protocol, 1);
	if (err)
		goto out;

	sock = sock_alloc();
	if (!sock) {
		err = -ENOMEM;
		goto out;
	}

	security_socket_post_create(sock, family, type, protocol, 1);
	sock->type = type;
out:
	*res = sock;
	return err;
}

/* No kernel lock held - perfect */
static unsigned int sock_poll(struct file *file, poll_table * wait)
{
	struct socket *sock;

	/*
	 *	We can't return errors to poll, so it's either yes or no. 
	 */
	sock = SOCKET_I(file->f_dentry->d_inode);
	return sock->ops->poll(file, sock, wait);
}

static int sock_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct socket *sock = SOCKET_I(file->f_dentry->d_inode);

	return sock->ops->mmap(file, sock, vma);
}

int sock_close(struct inode *inode, struct file *filp)
{
	/*
	 *	It was possible the inode is NULL we were 
	 *	closing an unfinished socket. 
	 */

	if (!inode)
	{
		printk(KERN_DEBUG "sock_close: NULL inode\n");
		return 0;
	}
	sock_fasync(-1, filp, 0);
	sock_release(SOCKET_I(inode));
	return 0;
}

/*
 *	Update the socket async list
 *
 *	Fasync_list locking strategy.
 *
 *	1. fasync_list is modified only under process context socket lock
 *	   i.e. under semaphore.
 *	2. fasync_list is used under read_lock(&sk->sk_callback_lock)
 *	   or under socket lock.
 *	3. fasync_list can be used from softirq context, so that
 *	   modification under socket lock have to be enhanced with
 *	   write_lock_bh(&sk->sk_callback_lock).
 *							--ANK (990710)
 */

static int sock_fasync(int fd, struct file *filp, int on)
{
	struct fasync_struct *fa, *fna=NULL, **prev;
	struct socket *sock;
	struct sock *sk;

	if (on)
	{
		fna=(struct fasync_struct *)kmalloc(sizeof(struct fasync_struct), GFP_KERNEL);
		if(fna==NULL)
			return -ENOMEM;
	}

	sock = SOCKET_I(filp->f_dentry->d_inode);

	if ((sk=sock->sk) == NULL) {
		if (fna)
			kfree(fna);
		return -EINVAL;
	}

	lock_sock(sk);

	prev=&(sock->fasync_list);

	for (fa=*prev; fa!=NULL; prev=&fa->fa_next,fa=*prev)
		if (fa->fa_file==filp)
			break;

	if(on)
	{
		if(fa!=NULL)
		{
			write_lock_bh(&sk->sk_callback_lock);
			fa->fa_fd=fd;
			write_unlock_bh(&sk->sk_callback_lock);

			kfree(fna);
			goto out;
		}
		fna->fa_file=filp;
		fna->fa_fd=fd;
		fna->magic=FASYNC_MAGIC;
		fna->fa_next=sock->fasync_list;
		write_lock_bh(&sk->sk_callback_lock);
		sock->fasync_list=fna;
		write_unlock_bh(&sk->sk_callback_lock);
	}
	else
	{
		if (fa!=NULL)
		{
			write_lock_bh(&sk->sk_callback_lock);
			*prev=fa->fa_next;
			write_unlock_bh(&sk->sk_callback_lock);
			kfree(fa);
		}
	}

out:
	release_sock(sock->sk);
	return 0;
}

/* This function may be called only under socket lock or callback_lock */

int sock_wake_async(struct socket *sock, int how, int band)
{
	if (!sock || !sock->fasync_list)
		return -1;
	switch (how)
	{
	case 1:
		
		if (test_bit(SOCK_ASYNC_WAITDATA, &sock->flags))
			break;
		goto call_kill;
	case 2:
		if (!test_and_clear_bit(SOCK_ASYNC_NOSPACE, &sock->flags))
			break;
		/* fall through */
	case 0:
	call_kill:
		__kill_fasync(sock->fasync_list, SIGIO, band);
		break;
	case 3:
		__kill_fasync(sock->fasync_list, SIGURG, band);
	}
	return 0;
}

static int __sock_create(int family, int type, int protocol, struct socket **res, int kern)
{
	int i;
	int err;
	struct socket *sock;

	/*
	 *	Check protocol is in range
	 */
	if (family < 0 || family >= NPROTO)
		return -EAFNOSUPPORT;
	if (type < 0 || type >= SOCK_MAX)
		return -EINVAL;

	/* Compatibility.

	   This uglymoron is moved from INET layer to here to avoid
	   deadlock in module load.
	 */
	if (family == PF_INET && type == SOCK_PACKET) {
		static int warned; 
		if (!warned) {
			warned = 1;
			printk(KERN_INFO "%s uses obsolete (PF_INET,SOCK_PACKET)\n", current->comm);
		}
		family = PF_PACKET;
	}

	err = security_socket_create(family, type, protocol, kern);
	if (err)
		return err;
		
#if defined(CONFIG_KMOD)
	/* Attempt to load a protocol module if the find failed. 
	 * 
	 * 12/09/1996 Marcin: But! this makes REALLY only sense, if the user 
	 * requested real, full-featured networking support upon configuration.
	 * Otherwise module support will break!
	 */
	if (net_families[family]==NULL)
	{
		request_module("net-pf-%d",family);
	}
#endif

	net_family_read_lock();
	if (net_families[family] == NULL) {
		i = -EAFNOSUPPORT;
		goto out;
	}

/*
 *	Allocate the socket and allow the family to set things up. if
 *	the protocol is 0, the family is instructed to select an appropriate
 *	default.
 */

	if (!(sock = sock_alloc())) 
	{
		printk(KERN_WARNING "socket: no more sockets\n");
		i = -ENFILE;		/* Not exactly a match, but its the
					   closest posix thing */
		goto out;
	}

	sock->type  = type;

	/*
	 * We will call the ->create function, that possibly is in a loadable
	 * module, so we have to bump that loadable module refcnt first.
	 */
	i = -EAFNOSUPPORT;
	if (!try_module_get(net_families[family]->owner))
		goto out_release;

	if ((i = net_families[family]->create(sock, protocol)) < 0)
		goto out_module_put;
	/*
	 * Now to bump the refcnt of the [loadable] module that owns this
	 * socket at sock_release time we decrement its refcnt.
	 */
	if (!try_module_get(sock->ops->owner)) {
		sock->ops = NULL;
		goto out_module_put;
	}
	/*
	 * Now that we're done with the ->create function, the [loadable]
	 * module can have its refcnt decremented
	 */
	module_put(net_families[family]->owner);
	*res = sock;
	security_socket_post_create(sock, family, type, protocol, kern);

out:
	net_family_read_unlock();
	return i;
out_module_put:
	module_put(net_families[family]->owner);
out_release:
	sock_release(sock);
	goto out;
}

int sock_create(int family, int type, int protocol, struct socket **res)
{
	return __sock_create(family, type, protocol, res, 0);
}

int sock_create_kern(int family, int type, int protocol, struct socket **res)
{
	return __sock_create(family, type, protocol, res, 1);
}

asmlinkage long sys_socket(int family, int type, int protocol)
{
	int retval;
	struct socket *sock;

	retval = sock_create(family, type, protocol, &sock);
	if (retval < 0)
		goto out;

	retval = sock_map_fd(sock);
	if (retval < 0)
		goto out_release;

out:
	/* It may be already another descriptor 8) Not kernel problem. */
	return retval;

out_release:
	sock_release(sock);
	return retval;
}

/*
 *	Create a pair of connected sockets.
 */

asmlinkage long sys_socketpair(int family, int type, int protocol, int __user *usockvec)
{
	struct socket *sock1, *sock2;
	int fd1, fd2, err;

	/*
	 * Obtain the first socket and check if the underlying protocol
	 * supports the socketpair call.
	 */

	err = sock_create(family, type, protocol, &sock1);
	if (err < 0)
		goto out;

	err = sock_create(family, type, protocol, &sock2);
	if (err < 0)
		goto out_release_1;

	err = sock1->ops->socketpair(sock1, sock2);
	if (err < 0) 
		goto out_release_both;

	fd1 = fd2 = -1;

	err = sock_map_fd(sock1);
	if (err < 0)
		goto out_release_both;
	fd1 = err;

	err = sock_map_fd(sock2);
	if (err < 0)
		goto out_close_1;
	fd2 = err;

	/* fd1 and fd2 may be already another descriptors.
	 * Not kernel problem.
	 */

	err = put_user(fd1, &usockvec[0]); 
	if (!err)
		err = put_user(fd2, &usockvec[1]);
	if (!err)
		return 0;

	sys_close(fd2);
	sys_close(fd1);
	return err;

out_close_1:
        sock_release(sock2);
	sys_close(fd1);
	return err;

out_release_both:
        sock_release(sock2);
out_release_1:
        sock_release(sock1);
out:
	return err;
}


/*
 *	Bind a name to a socket. Nothing much to do here since it's
 *	the protocol's responsibility to handle the local address.
 *
 *	We move the socket address to kernel space before we call
 *	the protocol layer (having also checked the address is ok).
 */

asmlinkage long sys_bind(int fd, struct sockaddr __user *umyaddr, int addrlen)
{
	struct socket *sock;
	char address[MAX_SOCK_ADDR];
	int err;

	if((sock = sockfd_lookup(fd,&err))!=NULL)
	{
		if((err=move_addr_to_kernel(umyaddr,addrlen,address))>=0) {
			err = security_socket_bind(sock, (struct sockaddr *)address, addrlen);
			if (err) {
				sockfd_put(sock);
				return err;
			}
			err = sock->ops->bind(sock, (struct sockaddr *)address, addrlen);
		}
		sockfd_put(sock);
	}			
	return err;
}


/*
 *	Perform a listen. Basically, we allow the protocol to do anything
 *	necessary for a listen, and if that works, we mark the socket as
 *	ready for listening.
 */

int sysctl_somaxconn = SOMAXCONN;

asmlinkage long sys_listen(int fd, int backlog)
{
	struct socket *sock;
	int err;
	
	if ((sock = sockfd_lookup(fd, &err)) != NULL) {
		if ((unsigned) backlog > sysctl_somaxconn)
			backlog = sysctl_somaxconn;

		err = security_socket_listen(sock, backlog);
		if (err) {
			sockfd_put(sock);
			return err;
		}

		err=sock->ops->listen(sock, backlog);
		sockfd_put(sock);
	}
	return err;
}


/*
 *	For accept, we attempt to create a new socket, set up the link
 *	with the client, wake up the client, then return the new
 *	connected fd. We collect the address of the connector in kernel
 *	space and move it to user at the very end. This is unclean because
 *	we open the socket then return an error.
 *
 *	1003.1g adds the ability to recvmsg() to query connection pending
 *	status to recvmsg. We need to add that support in a way thats
 *	clean when we restucture accept also.
 */

asmlinkage long sys_accept(int fd, struct sockaddr __user *upeer_sockaddr, int __user *upeer_addrlen)
{
	struct socket *sock, *newsock;
	int err, len;
	char address[MAX_SOCK_ADDR];

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		goto out;

	err = -EMFILE;
	if (!(newsock = sock_alloc())) 
		goto out_put;

	newsock->type = sock->type;
	newsock->ops = sock->ops;

	err = security_socket_accept(sock, newsock);
	if (err)
		goto out_release;

	/*
	 * We don't need try_module_get here, as the listening socket (sock)
	 * has the protocol module (sock->ops->owner) held.
	 */
	__module_get(newsock->ops->owner);

	err = sock->ops->accept(sock, newsock, sock->file->f_flags);
	if (err < 0)
		goto out_release;

	if (upeer_sockaddr) {
		if(newsock->ops->getname(newsock, (struct sockaddr *)address, &len, 2)<0) {
			err = -ECONNABORTED;
			goto out_release;
		}
		err = move_addr_to_user(address, len, upeer_sockaddr, upeer_addrlen);
		if (err < 0)
			goto out_release;
	}

	/* File flags are not inherited via accept() unlike another OSes. */

	if ((err = sock_map_fd(newsock)) < 0)
		goto out_release;

	security_socket_post_accept(sock, newsock);

out_put:
	sockfd_put(sock);
out:
	return err;
out_release:
	sock_release(newsock);
	goto out_put;
}


/*
 *	Attempt to connect to a socket with the server address.  The address
 *	is in user space so we verify it is OK and move it to kernel space.
 *
 *	For 1003.1g we need to add clean support for a bind to AF_UNSPEC to
 *	break bindings
 *
 *	NOTE: 1003.1g draft 6.3 is broken with respect to AX.25/NetROM and
 *	other SEQPACKET protocols that take time to connect() as it doesn't
 *	include the -EINPROGRESS status for such sockets.
 */

asmlinkage long sys_connect(int fd, struct sockaddr __user *uservaddr, int addrlen)
{
	struct socket *sock;
	char address[MAX_SOCK_ADDR];
	int err;

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		goto out;
	err = move_addr_to_kernel(uservaddr, addrlen, address);
	if (err < 0)
		goto out_put;

	err = security_socket_connect(sock, (struct sockaddr *)address, addrlen);
	if (err)
		goto out_put;

	err = sock->ops->connect(sock, (struct sockaddr *) address, addrlen,
				 sock->file->f_flags);
out_put:
	sockfd_put(sock);
out:
	return err;
}

/*
 *	Get the local address ('name') of a socket object. Move the obtained
 *	name to user space.
 */

asmlinkage long sys_getsockname(int fd, struct sockaddr __user *usockaddr, int __user *usockaddr_len)
{
	struct socket *sock;
	char address[MAX_SOCK_ADDR];
	int len, err;
	
	sock = sockfd_lookup(fd, &err);
	if (!sock)
		goto out;

	err = security_socket_getsockname(sock);
	if (err)
		goto out_put;

	err = sock->ops->getname(sock, (struct sockaddr *)address, &len, 0);
	if (err)
		goto out_put;
	err = move_addr_to_user(address, len, usockaddr, usockaddr_len);

out_put:
	sockfd_put(sock);
out:
	return err;
}

/*
 *	Get the remote address ('name') of a socket object. Move the obtained
 *	name to user space.
 */

asmlinkage long sys_getpeername(int fd, struct sockaddr __user *usockaddr, int __user *usockaddr_len)
{
	struct socket *sock;
	char address[MAX_SOCK_ADDR];
	int len, err;

	if ((sock = sockfd_lookup(fd, &err))!=NULL)
	{
		err = security_socket_getpeername(sock);
		if (err) {
			sockfd_put(sock);
			return err;
		}

		err = sock->ops->getname(sock, (struct sockaddr *)address, &len, 1);
		if (!err)
			err=move_addr_to_user(address,len, usockaddr, usockaddr_len);
		sockfd_put(sock);
	}
	return err;
}

/*
 *	Send a datagram to a given address. We move the address into kernel
 *	space and check the user space data area is readable before invoking
 *	the protocol.
 */

asmlinkage long sys_sendto(int fd, void __user * buff, size_t len, unsigned flags,
			   struct sockaddr __user *addr, int addr_len)
{
	struct socket *sock;
	char address[MAX_SOCK_ADDR];
	int err;
	struct msghdr msg;
	struct iovec iov;
	
	sock = sockfd_lookup(fd, &err);
	if (!sock)
		goto out;
	iov.iov_base=buff;
	iov.iov_len=len;
	msg.msg_name=NULL;
	msg.msg_iov=&iov;
	msg.msg_iovlen=1;
	msg.msg_control=NULL;
	msg.msg_controllen=0;
	msg.msg_namelen=0;
	if(addr)
	{
		err = move_addr_to_kernel(addr, addr_len, address);
		if (err < 0)
			goto out_put;
		msg.msg_name=address;
		msg.msg_namelen=addr_len;
	}
	if (sock->file->f_flags & O_NONBLOCK)
		flags |= MSG_DONTWAIT;
	msg.msg_flags = flags;
	err = sock_sendmsg(sock, &msg, len);

out_put:		
	sockfd_put(sock);
out:
	return err;
}

/*
 *	Send a datagram down a socket. 
 */

asmlinkage long sys_send(int fd, void __user * buff, size_t len, unsigned flags)
{
	return sys_sendto(fd, buff, len, flags, NULL, 0);
}

/*
 *	Receive a frame from the socket and optionally record the address of the 
 *	sender. We verify the buffers are writable and if needed move the
 *	sender address from kernel to user space.
 */

asmlinkage long sys_recvfrom(int fd, void __user * ubuf, size_t size, unsigned flags,
			     struct sockaddr __user *addr, int __user *addr_len)
{
	struct socket *sock;
	struct iovec iov;
	struct msghdr msg;
	char address[MAX_SOCK_ADDR];
	int err,err2;

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		goto out;

	msg.msg_control=NULL;
	msg.msg_controllen=0;
	msg.msg_iovlen=1;
	msg.msg_iov=&iov;
	iov.iov_len=size;
	iov.iov_base=ubuf;
	msg.msg_name=address;
	msg.msg_namelen=MAX_SOCK_ADDR;
	if (sock->file->f_flags & O_NONBLOCK)
		flags |= MSG_DONTWAIT;
	err=sock_recvmsg(sock, &msg, size, flags);

	if(err >= 0 && addr != NULL)
	{
		err2=move_addr_to_user(address, msg.msg_namelen, addr, addr_len);
		if(err2<0)
			err=err2;
	}
	sockfd_put(sock);			
out:
	return err;
}

/*
 *	Receive a datagram from a socket. 
 */

asmlinkage long sys_recv(int fd, void __user * ubuf, size_t size, unsigned flags)
{
	return sys_recvfrom(fd, ubuf, size, flags, NULL, NULL);
}

/*
 *	Set a socket option. Because we don't know the option lengths we have
 *	to pass the user mode parameter for the protocols to sort out.
 */

asmlinkage long sys_setsockopt(int fd, int level, int optname, char __user *optval, int optlen)
{
	int err;
	struct socket *sock;

	if (optlen < 0)
		return -EINVAL;
			
	if ((sock = sockfd_lookup(fd, &err))!=NULL)
	{
		err = security_socket_setsockopt(sock,level,optname);
		if (err) {
			sockfd_put(sock);
			return err;
		}

		if (level == SOL_SOCKET)
			err=sock_setsockopt(sock,level,optname,optval,optlen);
		else
			err=sock->ops->setsockopt(sock, level, optname, optval, optlen);
		sockfd_put(sock);
	}
	return err;
}

/*
 *	Get a socket option. Because we don't know the option lengths we have
 *	to pass a user mode parameter for the protocols to sort out.
 */

asmlinkage long sys_getsockopt(int fd, int level, int optname, char __user *optval, int __user *optlen)
{
	int err;
	struct socket *sock;

	if ((sock = sockfd_lookup(fd, &err))!=NULL)
	{
		err = security_socket_getsockopt(sock, level, 
							   optname);
		if (err) {
			sockfd_put(sock);
			return err;
		}

		if (level == SOL_SOCKET)
			err=sock_getsockopt(sock,level,optname,optval,optlen);
		else
			err=sock->ops->getsockopt(sock, level, optname, optval, optlen);
		sockfd_put(sock);
	}
	return err;
}


/*
 *	Shutdown a socket.
 */

asmlinkage long sys_shutdown(int fd, int how)
{
	int err;
	struct socket *sock;

	if ((sock = sockfd_lookup(fd, &err))!=NULL)
	{
		err = security_socket_shutdown(sock, how);
		if (err) {
			sockfd_put(sock);
			return err;
		}
				
		err=sock->ops->shutdown(sock, how);
		sockfd_put(sock);
	}
	return err;
}

/* A couple of helpful macros for getting the address of the 32/64 bit 
 * fields which are the same type (int / unsigned) on our platforms.
 */
#define COMPAT_MSG(msg, member)	((MSG_CMSG_COMPAT & flags) ? &msg##_compat->member : &msg->member)
#define COMPAT_NAMELEN(msg)	COMPAT_MSG(msg, msg_namelen)
#define COMPAT_FLAGS(msg)	COMPAT_MSG(msg, msg_flags)


/*
 *	BSD sendmsg interface
 */

asmlinkage long sys_sendmsg(int fd, struct msghdr __user *msg, unsigned flags)
{
	struct compat_msghdr __user *msg_compat = (struct compat_msghdr __user *)msg;
	struct socket *sock;
	char address[MAX_SOCK_ADDR];
	struct iovec iovstack[UIO_FASTIOV], *iov = iovstack;
	unsigned char ctl[sizeof(struct cmsghdr) + 20];	/* 20 is size of ipv6_pktinfo */
	unsigned char *ctl_buf = ctl;
	struct msghdr msg_sys;
	int err, ctl_len, iov_size, total_len;
	
	err = -EFAULT;
	if (MSG_CMSG_COMPAT & flags) {
		if (get_compat_msghdr(&msg_sys, msg_compat))
			return -EFAULT;
	} else if (copy_from_user(&msg_sys, msg, sizeof(struct msghdr)))
		return -EFAULT;

	sock = sockfd_lookup(fd, &err);
	if (!sock) 
		goto out;

	/* do not move before msg_sys is valid */
	err = -EMSGSIZE;
	if (msg_sys.msg_iovlen > UIO_MAXIOV)
		goto out_put;

	/* Check whether to allocate the iovec area*/
	err = -ENOMEM;
	iov_size = msg_sys.msg_iovlen * sizeof(struct iovec);
	if (msg_sys.msg_iovlen > UIO_FASTIOV) {
		iov = sock_kmalloc(sock->sk, iov_size, GFP_KERNEL);
		if (!iov)
			goto out_put;
	}

	/* This will also move the address data into kernel space */
	if (MSG_CMSG_COMPAT & flags) {
		err = verify_compat_iovec(&msg_sys, iov, address, VERIFY_READ);
	} else
		err = verify_iovec(&msg_sys, iov, address, VERIFY_READ);
	if (err < 0) 
		goto out_freeiov;
	total_len = err;

	err = -ENOBUFS;

	if (msg_sys.msg_controllen > INT_MAX)
		goto out_freeiov;
	ctl_len = msg_sys.msg_controllen; 
	if ((MSG_CMSG_COMPAT & flags) && ctl_len) {
		err = cmsghdr_from_user_compat_to_kern(&msg_sys, ctl, sizeof(ctl));
		if (err)
			goto out_freeiov;
		ctl_buf = msg_sys.msg_control;
	} else if (ctl_len) {
		if (ctl_len > sizeof(ctl))
		{
			ctl_buf = sock_kmalloc(sock->sk, ctl_len, GFP_KERNEL);
			if (ctl_buf == NULL) 
				goto out_freeiov;
		}
		err = -EFAULT;
		/*
		 * Careful! Before this, msg_sys.msg_control contains a user pointer.
		 * Afterwards, it will be a kernel pointer. Thus the compiler-assisted
		 * checking falls down on this.
		 */
		if (copy_from_user(ctl_buf, (void __user *) msg_sys.msg_control, ctl_len))
			goto out_freectl;
		msg_sys.msg_control = ctl_buf;
	}
	msg_sys.msg_flags = flags;

	if (sock->file->f_flags & O_NONBLOCK)
		msg_sys.msg_flags |= MSG_DONTWAIT;
	err = sock_sendmsg(sock, &msg_sys, total_len);

out_freectl:
	if (ctl_buf != ctl)    
		sock_kfree_s(sock->sk, ctl_buf, ctl_len);
out_freeiov:
	if (iov != iovstack)
		sock_kfree_s(sock->sk, iov, iov_size);
out_put:
	sockfd_put(sock);
out:       
	return err;
}

/*
 *	BSD recvmsg interface
 */

asmlinkage long sys_recvmsg(int fd, struct msghdr __user *msg, unsigned int flags)
{
	struct compat_msghdr __user *msg_compat = (struct compat_msghdr __user *)msg;
	struct socket *sock;
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov=iovstack;
	struct msghdr msg_sys;
	unsigned long cmsg_ptr;
	int err, iov_size, total_len, len;

	/* kernel mode address */
	char addr[MAX_SOCK_ADDR];

	/* user mode address pointers */
	struct sockaddr __user *uaddr;
	int __user *uaddr_len;
	
	if (MSG_CMSG_COMPAT & flags) {
		if (get_compat_msghdr(&msg_sys, msg_compat))
			return -EFAULT;
	} else
		if (copy_from_user(&msg_sys,msg,sizeof(struct msghdr)))
			return -EFAULT;

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		goto out;

	err = -EMSGSIZE;
	if (msg_sys.msg_iovlen > UIO_MAXIOV)
		goto out_put;
	
	/* Check whether to allocate the iovec area*/
	err = -ENOMEM;
	iov_size = msg_sys.msg_iovlen * sizeof(struct iovec);
	if (msg_sys.msg_iovlen > UIO_FASTIOV) {
		iov = sock_kmalloc(sock->sk, iov_size, GFP_KERNEL);
		if (!iov)
			goto out_put;
	}

	/*
	 *	Save the user-mode address (verify_iovec will change the
	 *	kernel msghdr to use the kernel address space)
	 */
	 
	uaddr = (void __user *) msg_sys.msg_name;
	uaddr_len = COMPAT_NAMELEN(msg);
	if (MSG_CMSG_COMPAT & flags) {
		err = verify_compat_iovec(&msg_sys, iov, addr, VERIFY_WRITE);
	} else
		err = verify_iovec(&msg_sys, iov, addr, VERIFY_WRITE);
	if (err < 0)
		goto out_freeiov;
	total_len=err;

	cmsg_ptr = (unsigned long)msg_sys.msg_control;
	msg_sys.msg_flags = 0;
	if (MSG_CMSG_COMPAT & flags)
		msg_sys.msg_flags = MSG_CMSG_COMPAT;
	
	if (sock->file->f_flags & O_NONBLOCK)
		flags |= MSG_DONTWAIT;
	err = sock_recvmsg(sock, &msg_sys, total_len, flags);
	if (err < 0)
		goto out_freeiov;
	len = err;

	if (uaddr != NULL) {
		err = move_addr_to_user(addr, msg_sys.msg_namelen, uaddr, uaddr_len);
		if (err < 0)
			goto out_freeiov;
	}
	err = __put_user(msg_sys.msg_flags, COMPAT_FLAGS(msg));
	if (err)
		goto out_freeiov;
	if (MSG_CMSG_COMPAT & flags)
		err = __put_user((unsigned long)msg_sys.msg_control-cmsg_ptr, 
				 &msg_compat->msg_controllen);
	else
		err = __put_user((unsigned long)msg_sys.msg_control-cmsg_ptr, 
				 &msg->msg_controllen);
	if (err)
		goto out_freeiov;
	err = len;

out_freeiov:
	if (iov != iovstack)
		sock_kfree_s(sock->sk, iov, iov_size);
out_put:
	sockfd_put(sock);
out:
	return err;
}

#ifdef __ARCH_WANT_SYS_SOCKETCALL

/* Argument list sizes for sys_socketcall */
#define AL(x) ((x) * sizeof(unsigned long))
static unsigned char nargs[18]={AL(0),AL(3),AL(3),AL(3),AL(2),AL(3),
				AL(3),AL(3),AL(4),AL(4),AL(4),AL(6),
				AL(6),AL(2),AL(5),AL(5),AL(3),AL(3)};
#undef AL

/*
 *	System call vectors. 
 *
 *	Argument checking cleaned up. Saved 20% in size.
 *  This function doesn't need to set the kernel lock because
 *  it is set by the callees. 
 */

asmlinkage long sys_socketcall(int call, unsigned long __user *args)
{
	unsigned long a[6];
	unsigned long a0,a1;
	int err;

	if(call<1||call>SYS_RECVMSG)
		return -EINVAL;

	/* copy_from_user should be SMP safe. */
	if (copy_from_user(a, args, nargs[call]))
		return -EFAULT;
		
	a0=a[0];
	a1=a[1];
	
	switch(call) 
	{
		case SYS_SOCKET:
			err = sys_socket(a0,a1,a[2]);
			break;
		case SYS_BIND:
			err = sys_bind(a0,(struct sockaddr __user *)a1, a[2]);
			break;
		case SYS_CONNECT:
			err = sys_connect(a0, (struct sockaddr __user *)a1, a[2]);
			break;
		case SYS_LISTEN:
			err = sys_listen(a0,a1);
			break;
		case SYS_ACCEPT:
			err = sys_accept(a0,(struct sockaddr __user *)a1, (int __user *)a[2]);
			break;
		case SYS_GETSOCKNAME:
			err = sys_getsockname(a0,(struct sockaddr __user *)a1, (int __user *)a[2]);
			break;
		case SYS_GETPEERNAME:
			err = sys_getpeername(a0, (struct sockaddr __user *)a1, (int __user *)a[2]);
			break;
		case SYS_SOCKETPAIR:
			err = sys_socketpair(a0,a1, a[2], (int __user *)a[3]);
			break;
		case SYS_SEND:
			err = sys_send(a0, (void __user *)a1, a[2], a[3]);
			break;
		case SYS_SENDTO:
			err = sys_sendto(a0,(void __user *)a1, a[2], a[3],
					 (struct sockaddr __user *)a[4], a[5]);
			break;
		case SYS_RECV:
			err = sys_recv(a0, (void __user *)a1, a[2], a[3]);
			break;
		case SYS_RECVFROM:
			err = sys_recvfrom(a0, (void __user *)a1, a[2], a[3],
					   (struct sockaddr __user *)a[4], (int __user *)a[5]);
			break;
		case SYS_SHUTDOWN:
			err = sys_shutdown(a0,a1);
			break;
		case SYS_SETSOCKOPT:
			err = sys_setsockopt(a0, a1, a[2], (char __user *)a[3], a[4]);
			break;
		case SYS_GETSOCKOPT:
			err = sys_getsockopt(a0, a1, a[2], (char __user *)a[3], (int __user *)a[4]);
			break;
		case SYS_SENDMSG:
			err = sys_sendmsg(a0, (struct msghdr __user *) a1, a[2]);
			break;
		case SYS_RECVMSG:
			err = sys_recvmsg(a0, (struct msghdr __user *) a1, a[2]);
			break;
		default:
			err = -EINVAL;
			break;
	}
	return err;
}

#endif /* __ARCH_WANT_SYS_SOCKETCALL */

/*
 *	This function is called by a protocol handler that wants to
 *	advertise its address family, and have it linked into the
 *	SOCKET module.
 */

int sock_register(struct net_proto_family *ops)
{
	int err;

	if (ops->family >= NPROTO) {
		printk(KERN_CRIT "protocol %d >= NPROTO(%d)\n", ops->family, NPROTO);
		return -ENOBUFS;
	}
	net_family_write_lock();
	err = -EEXIST;
	if (net_families[ops->family] == NULL) {
		net_families[ops->family]=ops;
		err = 0;
	}
	net_family_write_unlock();
	printk(KERN_INFO "NET: Registered protocol family %d\n",
	       ops->family);
	return err;
}

/*
 *	This function is called by a protocol handler that wants to
 *	remove its address family, and have it unlinked from the
 *	SOCKET module.
 */

int sock_unregister(int family)
{
	if (family < 0 || family >= NPROTO)
		return -1;

	net_family_write_lock();
	net_families[family]=NULL;
	net_family_write_unlock();
	printk(KERN_INFO "NET: Unregistered protocol family %d\n",
	       family);
	return 0;
}


extern void sk_init(void);

void __init sock_init(void)
{
	int i;

	/*
	 *	Initialize all address (protocol) families. 
	 */
	 
	for (i = 0; i < NPROTO; i++) 
		net_families[i] = NULL;

	/*
	 *	Initialize sock SLAB cache.
	 */
	 
	sk_init();

#ifdef SLAB_SKB
	/*
	 *	Initialize skbuff SLAB cache 
	 */
	skb_init();
#endif

	/*
	 *	Initialize the protocols module. 
	 */

	init_inodecache();
	register_filesystem(&sock_fs_type);
	sock_mnt = kern_mount(&sock_fs_type);
	/* The real protocol initialization is performed when
	 *  do_initcalls is run.  
	 */

#ifdef CONFIG_NETFILTER
	netfilter_init();
#endif
}

#ifdef CONFIG_PROC_FS
void socket_seq_show(struct seq_file *seq)
{
	int cpu;
	int counter = 0;

	for (cpu = 0; cpu < NR_CPUS; cpu++)
		counter += per_cpu(sockets_in_use, cpu);

	/* It can be negative, by the way. 8) */
	if (counter < 0)
		counter = 0;

	seq_printf(seq, "sockets: used %d\n", counter);
}
#endif /* CONFIG_PROC_FS */

/* ABI emulation layers need these two */
EXPORT_SYMBOL(move_addr_to_kernel);
EXPORT_SYMBOL(move_addr_to_user);
EXPORT_SYMBOL(sock_alloc);
EXPORT_SYMBOL(sock_alloc_inode);
EXPORT_SYMBOL(sock_create);
EXPORT_SYMBOL(sock_create_kern);
EXPORT_SYMBOL(sock_create_lite);
EXPORT_SYMBOL(sock_map_fd);
EXPORT_SYMBOL(sock_recvmsg);
EXPORT_SYMBOL(sock_register);
EXPORT_SYMBOL(sock_release);
EXPORT_SYMBOL(sock_sendmsg);
EXPORT_SYMBOL(sock_unregister);
EXPORT_SYMBOL(sock_wake_async);
EXPORT_SYMBOL(sockfd_lookup);
EXPORT_SYMBOL(kernel_sendmsg);
EXPORT_SYMBOL(kernel_recvmsg);
