/*
 * NET4:	Implementation of BSD Unix domain sockets.
 *
 * Authors:	Alan Cox, <alan.cox@linux.org>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Version:	$Id: af_unix.c,v 1.133 2002/02/08 03:57:19 davem Exp $
 *
 * Fixes:
 *		Linus Torvalds	:	Assorted bug cures.
 *		Niibe Yutaka	:	async I/O support.
 *		Carsten Paeth	:	PF_UNIX check, address fixes.
 *		Alan Cox	:	Limit size of allocated blocks.
 *		Alan Cox	:	Fixed the stupid socketpair bug.
 *		Alan Cox	:	BSD compatibility fine tuning.
 *		Alan Cox	:	Fixed a bug in connect when interrupted.
 *		Alan Cox	:	Sorted out a proper draft version of
 *					file descriptor passing hacked up from
 *					Mike Shaver's work.
 *		Marty Leisner	:	Fixes to fd passing
 *		Nick Nevin	:	recvmsg bugfix.
 *		Alan Cox	:	Started proper garbage collector
 *		Heiko EiBfeldt	:	Missing verify_area check
 *		Alan Cox	:	Started POSIXisms
 *		Andreas Schwab	:	Replace inode by dentry for proper
 *					reference counting
 *		Kirk Petersen	:	Made this a module
 *	    Christoph Rohland	:	Elegant non-blocking accept/connect algorithm.
 *					Lots of bug fixes.
 *	     Alexey Kuznetosv	:	Repaired (I hope) bugs introduces
 *					by above two patches.
 *	     Andrea Arcangeli	:	If possible we block in connect(2)
 *					if the max backlog of the listen socket
 *					is been reached. This won't break
 *					old apps and it will avoid huge amount
 *					of socks hashed (this for unix_gc()
 *					performances reasons).
 *					Security fix that limits the max
 *					number of socks to 2*max_files and
 *					the number of skb queueable in the
 *					dgram receiver.
 *		Artur Skawina   :	Hash function optimizations
 *	     Alexey Kuznetsov   :	Full scale SMP. Lot of bugs are introduced 8)
 *	      Malcolm Beattie   :	Set peercred for socketpair
 *	     Michal Ostrowski   :       Module initialization cleanup.
 *	     Arnaldo C. Melo	:	Remove MOD_{INC,DEC}_USE_COUNT,
 *	     				the core infrastructure is doing that
 *	     				for all net proto families now (2.5.69+)
 *
 *
 * Known differences from reference BSD that was tested:
 *
 *	[TO FIX]
 *	ECONNREFUSED is not returned from one end of a connected() socket to the
 *		other the moment one end closes.
 *	fstat() doesn't return st_dev=0, and give the blksize as high water mark
 *		and a fake inode identifier (nor the BSD first socket fstat twice bug).
 *	[NOT TO FIX]
 *	accept() returns a path name even if the connecting socket has closed
 *		in the meantime (BSD loses the path and gives up).
 *	accept() returns 0 length path for an unbound connector. BSD returns 16
 *		and a null first byte in the path (but not for gethost/peername - BSD bug ??)
 *	socketpair(...SOCK_RAW..) doesn't panic the kernel.
 *	BSD af_unix apparently has connect forgetting to block properly.
 *		(need to check this with the POSIX spec in detail)
 *
 * Differences from 2.0.0-11-... (ANK)
 *	Bug fixes and improvements.
 *		- client shutdown killed server socket.
 *		- removed all useless cli/sti pairs.
 *
 *	Semantic changes/extensions.
 *		- generic control message passing.
 *		- SCM_CREDENTIALS control message.
 *		- "Abstract" (not FS based) socket bindings.
 *		  Abstract names are sequences of bytes (not zero terminated)
 *		  started by 0, so that this name space does not intersect
 *		  with BSD names.
 */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/fcntl.h>
#include <linux/termios.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <net/af_unix.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/scm.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/smp_lock.h>
#include <linux/rtnetlink.h>
#include <linux/mount.h>
#include <net/checksum.h>
#include <linux/security.h>

int sysctl_unix_max_dgram_qlen = 10;

kmem_cache_t *unix_sk_cachep;

struct hlist_head unix_socket_table[UNIX_HASH_SIZE + 1];
rwlock_t unix_table_lock = RW_LOCK_UNLOCKED;
static atomic_t unix_nr_socks = ATOMIC_INIT(0);

#define unix_sockets_unbound	(&unix_socket_table[UNIX_HASH_SIZE])

#define UNIX_ABSTRACT(sk)	(unix_sk(sk)->addr->hash != UNIX_HASH_SIZE)

/*
 *  SMP locking strategy:
 *    hash table is protected with rwlock unix_table_lock
 *    each socket state is protected by separate rwlock.
 */

static inline unsigned unix_hash_fold(unsigned hash)
{
	hash ^= hash>>16;
	hash ^= hash>>8;
	return hash&(UNIX_HASH_SIZE-1);
}

#define unix_peer(sk) ((sk)->sk_pair)

static inline int unix_our_peer(struct sock *sk, struct sock *osk)
{
	return unix_peer(osk) == sk;
}

static inline int unix_may_send(struct sock *sk, struct sock *osk)
{
	return (unix_peer(osk) == NULL || unix_our_peer(sk, osk));
}

static struct sock *unix_peer_get(struct sock *s)
{
	struct sock *peer;

	unix_state_rlock(s);
	peer = unix_peer(s);
	if (peer)
		sock_hold(peer);
	unix_state_runlock(s);
	return peer;
}

static inline void unix_release_addr(struct unix_address *addr)
{
	if (atomic_dec_and_test(&addr->refcnt))
		kfree(addr);
}

/*
 *	Check unix socket name:
 *		- should be not zero length.
 *	        - if started by not zero, should be NULL terminated (FS object)
 *		- if started by zero, it is abstract name.
 */
 
static int unix_mkname(struct sockaddr_un * sunaddr, int len, unsigned *hashp)
{
	if (len <= sizeof(short) || len > sizeof(*sunaddr))
		return -EINVAL;
	if (!sunaddr || sunaddr->sun_family != AF_UNIX)
		return -EINVAL;
	if (sunaddr->sun_path[0])
	{
		/*
		 *	This may look like an off by one error but it is
		 *	a bit more subtle. 108 is the longest valid AF_UNIX
		 *	path for a binding. sun_path[108] doesn't as such
		 *	exist. However in kernel space we are guaranteed that
		 *	it is a valid memory location in our kernel
		 *	address buffer.
		 */
		if (len > sizeof(*sunaddr))
			len = sizeof(*sunaddr);
		((char *)sunaddr)[len]=0;
		len = strlen(sunaddr->sun_path)+1+sizeof(short);
		return len;
	}

	*hashp = unix_hash_fold(csum_partial((char*)sunaddr, len, 0));
	return len;
}

static void __unix_remove_socket(struct sock *sk)
{
	sk_del_node_init(sk);
}

static void __unix_insert_socket(struct hlist_head *list, struct sock *sk)
{
	BUG_TRAP(sk_unhashed(sk));
	sk_add_node(sk, list);
}

static inline void unix_remove_socket(struct sock *sk)
{
	write_lock(&unix_table_lock);
	__unix_remove_socket(sk);
	write_unlock(&unix_table_lock);
}

static inline void unix_insert_socket(struct hlist_head *list, struct sock *sk)
{
	write_lock(&unix_table_lock);
	__unix_insert_socket(list, sk);
	write_unlock(&unix_table_lock);
}

static struct sock *__unix_find_socket_byname(struct sockaddr_un *sunname,
					      int len, int type, unsigned hash)
{
	struct sock *s;
	struct hlist_node *node;

	sk_for_each(s, node, &unix_socket_table[hash ^ type]) {
		struct unix_sock *u = unix_sk(s);

		if (u->addr->len == len &&
		    !memcmp(u->addr->name, sunname, len))
			goto found;
	}
	s = NULL;
found:
	return s;
}

static inline struct sock *unix_find_socket_byname(struct sockaddr_un *sunname,
						   int len, int type,
						   unsigned hash)
{
	struct sock *s;

	read_lock(&unix_table_lock);
	s = __unix_find_socket_byname(sunname, len, type, hash);
	if (s)
		sock_hold(s);
	read_unlock(&unix_table_lock);
	return s;
}

static struct sock *unix_find_socket_byinode(struct inode *i)
{
	struct sock *s;
	struct hlist_node *node;

	read_lock(&unix_table_lock);
	sk_for_each(s, node,
		    &unix_socket_table[i->i_ino & (UNIX_HASH_SIZE - 1)]) {
		struct dentry *dentry = unix_sk(s)->dentry;

		if(dentry && dentry->d_inode == i)
		{
			sock_hold(s);
			goto found;
		}
	}
	s = NULL;
found:
	read_unlock(&unix_table_lock);
	return s;
}

static inline int unix_writable(struct sock *sk)
{
	return (atomic_read(&sk->sk_wmem_alloc) << 2) <= sk->sk_sndbuf;
}

static void unix_write_space(struct sock *sk)
{
	read_lock(&sk->sk_callback_lock);
	if (unix_writable(sk)) {
		if (sk->sk_sleep && waitqueue_active(sk->sk_sleep))
			wake_up_interruptible(sk->sk_sleep);
		sk_wake_async(sk, 2, POLL_OUT);
	}
	read_unlock(&sk->sk_callback_lock);
}

/* When dgram socket disconnects (or changes its peer), we clear its receive
 * queue of packets arrived from previous peer. First, it allows to do
 * flow control based only on wmem_alloc; second, sk connected to peer
 * may receive messages only from that peer. */
static void unix_dgram_disconnected(struct sock *sk, struct sock *other)
{
	if (skb_queue_len(&sk->sk_receive_queue)) {
		skb_queue_purge(&sk->sk_receive_queue);
		wake_up_interruptible_all(&unix_sk(sk)->peer_wait);

		/* If one link of bidirectional dgram pipe is disconnected,
		 * we signal error. Messages are lost. Do not make this,
		 * when peer was not connected to us.
		 */
		if (!sock_flag(other, SOCK_DEAD) && unix_peer(other) == sk) {
			other->sk_err = ECONNRESET;
			other->sk_error_report(other);
		}
	}
}

static void unix_sock_destructor(struct sock *sk)
{
	struct unix_sock *u = unix_sk(sk);

	skb_queue_purge(&sk->sk_receive_queue);

	BUG_TRAP(!atomic_read(&sk->sk_wmem_alloc));
	BUG_TRAP(sk_unhashed(sk));
	BUG_TRAP(!sk->sk_socket);
	if (!sock_flag(sk, SOCK_DEAD)) {
		printk("Attempt to release alive unix socket: %p\n", sk);
		return;
	}

	if (u->addr)
		unix_release_addr(u->addr);

	atomic_dec(&unix_nr_socks);
#ifdef UNIX_REFCNT_DEBUG
	printk(KERN_DEBUG "UNIX %p is destroyed, %d are still alive.\n", sk, atomic_read(&unix_nr_socks));
#endif
}

static int unix_release_sock (struct sock *sk, int embrion)
{
	struct unix_sock *u = unix_sk(sk);
	struct dentry *dentry;
	struct vfsmount *mnt;
	struct sock *skpair;
	struct sk_buff *skb;
	int state;

	unix_remove_socket(sk);

	/* Clear state */
	unix_state_wlock(sk);
	sock_orphan(sk);
	sk->sk_shutdown = SHUTDOWN_MASK;
	dentry	     = u->dentry;
	u->dentry    = NULL;
	mnt	     = u->mnt;
	u->mnt	     = NULL;
	state = sk->sk_state;
	sk->sk_state = TCP_CLOSE;
	unix_state_wunlock(sk);

	wake_up_interruptible_all(&u->peer_wait);

	skpair=unix_peer(sk);

	if (skpair!=NULL) {
		if (sk->sk_type == SOCK_STREAM || sk->sk_type == SOCK_SEQPACKET) {
			unix_state_wlock(skpair);
			/* No more writes */
			skpair->sk_shutdown = SHUTDOWN_MASK;
			if (!skb_queue_empty(&sk->sk_receive_queue) || embrion)
				skpair->sk_err = ECONNRESET;
			unix_state_wunlock(skpair);
			skpair->sk_state_change(skpair);
			read_lock(&skpair->sk_callback_lock);
			sk_wake_async(skpair,1,POLL_HUP);
			read_unlock(&skpair->sk_callback_lock);
		}
		sock_put(skpair); /* It may now die */
		unix_peer(sk) = NULL;
	}

	/* Try to flush out this socket. Throw out buffers at least */

	while ((skb = skb_dequeue(&sk->sk_receive_queue)) != NULL) {
		if (state==TCP_LISTEN)
			unix_release_sock(skb->sk, 1);
		/* passed fds are erased in the kfree_skb hook	      */
		kfree_skb(skb);
	}

	if (dentry) {
		dput(dentry);
		mntput(mnt);
	}

	sock_put(sk);

	/* ---- Socket is dead now and most probably destroyed ---- */

	/*
	 * Fixme: BSD difference: In BSD all sockets connected to use get
	 *	  ECONNRESET and we die on the spot. In Linux we behave
	 *	  like files and pipes do and wait for the last
	 *	  dereference.
	 *
	 * Can't we simply set sock->err?
	 *
	 *	  What the above comment does talk about? --ANK(980817)
	 */

	if (atomic_read(&unix_tot_inflight))
		unix_gc();		/* Garbage collect fds */	

	return 0;
}

static int unix_listen(struct socket *sock, int backlog)
{
	int err;
	struct sock *sk = sock->sk;
	struct unix_sock *u = unix_sk(sk);

	err = -EOPNOTSUPP;
	if (sock->type!=SOCK_STREAM && sock->type!=SOCK_SEQPACKET)
		goto out;			/* Only stream/seqpacket sockets accept */
	err = -EINVAL;
	if (!u->addr)
		goto out;			/* No listens on an unbound socket */
	unix_state_wlock(sk);
	if (sk->sk_state != TCP_CLOSE && sk->sk_state != TCP_LISTEN)
		goto out_unlock;
	if (backlog > sk->sk_max_ack_backlog)
		wake_up_interruptible_all(&u->peer_wait);
	sk->sk_max_ack_backlog	= backlog;
	sk->sk_state		= TCP_LISTEN;
	/* set credentials so connect can copy them */
	sk->sk_peercred.pid	= current->tgid;
	sk->sk_peercred.uid	= current->euid;
	sk->sk_peercred.gid	= current->egid;
	err = 0;

out_unlock:
	unix_state_wunlock(sk);
out:
	return err;
}

static int unix_release(struct socket *);
static int unix_bind(struct socket *, struct sockaddr *, int);
static int unix_stream_connect(struct socket *, struct sockaddr *,
			       int addr_len, int flags);
static int unix_socketpair(struct socket *, struct socket *);
static int unix_accept(struct socket *, struct socket *, int);
static int unix_getname(struct socket *, struct sockaddr *, int *, int);
static unsigned int unix_poll(struct file *, struct socket *, poll_table *);
static int unix_ioctl(struct socket *, unsigned int, unsigned long);
static int unix_shutdown(struct socket *, int);
static int unix_stream_sendmsg(struct kiocb *, struct socket *,
			       struct msghdr *, size_t);
static int unix_stream_recvmsg(struct kiocb *, struct socket *,
			       struct msghdr *, size_t, int);
static int unix_dgram_sendmsg(struct kiocb *, struct socket *,
			      struct msghdr *, size_t);
static int unix_dgram_recvmsg(struct kiocb *, struct socket *,
			      struct msghdr *, size_t, int);
static int unix_dgram_connect(struct socket *, struct sockaddr *,
			      int, int);

static struct proto_ops unix_stream_ops = {
	.family =	PF_UNIX,
	.owner =	THIS_MODULE,
	.release =	unix_release,
	.bind =		unix_bind,
	.connect =	unix_stream_connect,
	.socketpair =	unix_socketpair,
	.accept =	unix_accept,
	.getname =	unix_getname,
	.poll =		unix_poll,
	.ioctl =	unix_ioctl,
	.listen =	unix_listen,
	.shutdown =	unix_shutdown,
	.setsockopt =	sock_no_setsockopt,
	.getsockopt =	sock_no_getsockopt,
	.sendmsg =	unix_stream_sendmsg,
	.recvmsg =	unix_stream_recvmsg,
	.mmap =		sock_no_mmap,
	.sendpage =	sock_no_sendpage,
};

static struct proto_ops unix_dgram_ops = {
	.family =	PF_UNIX,
	.owner =	THIS_MODULE,
	.release =	unix_release,
	.bind =		unix_bind,
	.connect =	unix_dgram_connect,
	.socketpair =	unix_socketpair,
	.accept =	sock_no_accept,
	.getname =	unix_getname,
	.poll =		datagram_poll,
	.ioctl =	unix_ioctl,
	.listen =	sock_no_listen,
	.shutdown =	unix_shutdown,
	.setsockopt =	sock_no_setsockopt,
	.getsockopt =	sock_no_getsockopt,
	.sendmsg =	unix_dgram_sendmsg,
	.recvmsg =	unix_dgram_recvmsg,
	.mmap =		sock_no_mmap,
	.sendpage =	sock_no_sendpage,
};

static struct proto_ops unix_seqpacket_ops = {
	.family =	PF_UNIX,
	.owner =	THIS_MODULE,
	.release =	unix_release,
	.bind =		unix_bind,
	.connect =	unix_stream_connect,
	.socketpair =	unix_socketpair,
	.accept =	unix_accept,
	.getname =	unix_getname,
	.poll =		datagram_poll,
	.ioctl =	unix_ioctl,
	.listen =	unix_listen,
	.shutdown =	unix_shutdown,
	.setsockopt =	sock_no_setsockopt,
	.getsockopt =	sock_no_getsockopt,
	.sendmsg =	unix_dgram_sendmsg,
	.recvmsg =	unix_dgram_recvmsg,
	.mmap =		sock_no_mmap,
	.sendpage =	sock_no_sendpage,
};

static struct sock * unix_create1(struct socket *sock)
{
	struct sock *sk = NULL;
	struct unix_sock *u;

	if (atomic_read(&unix_nr_socks) >= 2*files_stat.max_files)
		goto out;

	sk = sk_alloc(PF_UNIX, GFP_KERNEL, sizeof(struct unix_sock),
		      unix_sk_cachep);
	if (!sk)
		goto out;

	atomic_inc(&unix_nr_socks);

	sock_init_data(sock,sk);
	sk_set_owner(sk, THIS_MODULE);

	sk->sk_write_space	= unix_write_space;
	sk->sk_max_ack_backlog	= sysctl_unix_max_dgram_qlen;
	sk->sk_destruct		= unix_sock_destructor;
	u	  = unix_sk(sk);
	u->dentry = NULL;
	u->mnt	  = NULL;
	rwlock_init(&u->lock);
	atomic_set(&u->inflight, sock ? 0 : -1);
	init_MUTEX(&u->readsem); /* single task reading lock */
	init_waitqueue_head(&u->peer_wait);
	unix_insert_socket(unix_sockets_unbound, sk);
out:
	return sk;
}

static int unix_create(struct socket *sock, int protocol)
{
	if (protocol && protocol != PF_UNIX)
		return -EPROTONOSUPPORT;

	sock->state = SS_UNCONNECTED;

	switch (sock->type) {
	case SOCK_STREAM:
		sock->ops = &unix_stream_ops;
		break;
		/*
		 *	Believe it or not BSD has AF_UNIX, SOCK_RAW though
		 *	nothing uses it.
		 */
	case SOCK_RAW:
		sock->type=SOCK_DGRAM;
	case SOCK_DGRAM:
		sock->ops = &unix_dgram_ops;
		break;
	case SOCK_SEQPACKET:
		sock->ops = &unix_seqpacket_ops;
		break;
	default:
		return -ESOCKTNOSUPPORT;
	}

	return unix_create1(sock) ? 0 : -ENOMEM;
}

static int unix_release(struct socket *sock)
{
	struct sock *sk = sock->sk;

	if (!sk)
		return 0;

	sock->sk = NULL;

	return unix_release_sock (sk, 0);
}

static int unix_autobind(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct unix_sock *u = unix_sk(sk);
	static u32 ordernum = 1;
	struct unix_address * addr;
	int err;

	down(&u->readsem);

	err = 0;
	if (u->addr)
		goto out;

	err = -ENOMEM;
	addr = kmalloc(sizeof(*addr) + sizeof(short) + 16, GFP_KERNEL);
	if (!addr)
		goto out;

	memset(addr, 0, sizeof(*addr) + sizeof(short) + 16);
	addr->name->sun_family = AF_UNIX;
	atomic_set(&addr->refcnt, 1);

retry:
	addr->len = sprintf(addr->name->sun_path+1, "%05x", ordernum) + 1 + sizeof(short);
	addr->hash = unix_hash_fold(csum_partial((void*)addr->name, addr->len, 0));

	write_lock(&unix_table_lock);
	ordernum = (ordernum+1)&0xFFFFF;

	if (__unix_find_socket_byname(addr->name, addr->len, sock->type,
				      addr->hash)) {
		write_unlock(&unix_table_lock);
		/* Sanity yield. It is unusual case, but yet... */
		if (!(ordernum&0xFF))
			yield();
		goto retry;
	}
	addr->hash ^= sk->sk_type;

	__unix_remove_socket(sk);
	u->addr = addr;
	__unix_insert_socket(&unix_socket_table[addr->hash], sk);
	write_unlock(&unix_table_lock);
	err = 0;

out:	up(&u->readsem);
	return err;
}

static struct sock *unix_find_other(struct sockaddr_un *sunname, int len,
				    int type, unsigned hash, int *error)
{
	struct sock *u;
	struct nameidata nd;
	int err = 0;
	
	if (sunname->sun_path[0]) {
		err = path_lookup(sunname->sun_path, LOOKUP_FOLLOW, &nd);
		if (err)
			goto fail;
		err = permission(nd.dentry->d_inode,MAY_WRITE, &nd);
		if (err)
			goto put_fail;

		err = -ECONNREFUSED;
		if (!S_ISSOCK(nd.dentry->d_inode->i_mode))
			goto put_fail;
		u=unix_find_socket_byinode(nd.dentry->d_inode);
		if (!u)
			goto put_fail;

		if (u->sk_type == type)
			touch_atime(nd.mnt, nd.dentry);

		path_release(&nd);

		err=-EPROTOTYPE;
		if (u->sk_type != type) {
			sock_put(u);
			goto fail;
		}
	} else {
		err = -ECONNREFUSED;
		u=unix_find_socket_byname(sunname, len, type, hash);
		if (u) {
			struct dentry *dentry;
			dentry = unix_sk(u)->dentry;
			if (dentry)
				touch_atime(unix_sk(u)->mnt, dentry);
		} else
			goto fail;
	}
	return u;

put_fail:
	path_release(&nd);
fail:
	*error=err;
	return NULL;
}


static int unix_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
	struct sock *sk = sock->sk;
	struct unix_sock *u = unix_sk(sk);
	struct sockaddr_un *sunaddr=(struct sockaddr_un *)uaddr;
	struct dentry * dentry = NULL;
	struct nameidata nd;
	int err;
	unsigned hash;
	struct unix_address *addr;
	struct hlist_head *list;

	err = -EINVAL;
	if (sunaddr->sun_family != AF_UNIX)
		goto out;

	if (addr_len==sizeof(short)) {
		err = unix_autobind(sock);
		goto out;
	}

	err = unix_mkname(sunaddr, addr_len, &hash);
	if (err < 0)
		goto out;
	addr_len = err;

	down(&u->readsem);

	err = -EINVAL;
	if (u->addr)
		goto out_up;

	err = -ENOMEM;
	addr = kmalloc(sizeof(*addr)+addr_len, GFP_KERNEL);
	if (!addr)
		goto out_up;

	memcpy(addr->name, sunaddr, addr_len);
	addr->len = addr_len;
	addr->hash = hash ^ sk->sk_type;
	atomic_set(&addr->refcnt, 1);

	if (sunaddr->sun_path[0]) {
		unsigned int mode;
		err = 0;
		/*
		 * Get the parent directory, calculate the hash for last
		 * component.
		 */
		err = path_lookup(sunaddr->sun_path, LOOKUP_PARENT, &nd);
		if (err)
			goto out_mknod_parent;
		/*
		 * Yucky last component or no last component at all?
		 * (foo/., foo/.., /////)
		 */
		err = -EEXIST;
		if (nd.last_type != LAST_NORM)
			goto out_mknod;
		/*
		 * Lock the directory.
		 */
		down(&nd.dentry->d_inode->i_sem);
		/*
		 * Do the final lookup.
		 */
		dentry = lookup_hash(&nd.last, nd.dentry);
		err = PTR_ERR(dentry);
		if (IS_ERR(dentry))
			goto out_mknod_unlock;
		err = -ENOENT;
		/*
		 * Special case - lookup gave negative, but... we had foo/bar/
		 * From the vfs_mknod() POV we just have a negative dentry -
		 * all is fine. Let's be bastards - you had / on the end, you've
		 * been asking for (non-existent) directory. -ENOENT for you.
		 */
		if (nd.last.name[nd.last.len] && !dentry->d_inode)
			goto out_mknod_dput;
		/*
		 * All right, let's create it.
		 */
		mode = S_IFSOCK |
		       (SOCK_INODE(sock)->i_mode & ~current->fs->umask);
		err = vfs_mknod(nd.dentry->d_inode, dentry, mode, 0);
		if (err)
			goto out_mknod_dput;
		up(&nd.dentry->d_inode->i_sem);
		dput(nd.dentry);
		nd.dentry = dentry;

		addr->hash = UNIX_HASH_SIZE;
	}

	write_lock(&unix_table_lock);

	if (!sunaddr->sun_path[0]) {
		err = -EADDRINUSE;
		if (__unix_find_socket_byname(sunaddr, addr_len,
					      sk->sk_type, hash)) {
			unix_release_addr(addr);
			goto out_unlock;
		}

		list = &unix_socket_table[addr->hash];
	} else {
		list = &unix_socket_table[dentry->d_inode->i_ino & (UNIX_HASH_SIZE-1)];
		u->dentry = nd.dentry;
		u->mnt    = nd.mnt;
	}

	err = 0;
	__unix_remove_socket(sk);
	u->addr = addr;
	__unix_insert_socket(list, sk);

out_unlock:
	write_unlock(&unix_table_lock);
out_up:
	up(&u->readsem);
out:
	return err;

out_mknod_dput:
	dput(dentry);
out_mknod_unlock:
	up(&nd.dentry->d_inode->i_sem);
out_mknod:
	path_release(&nd);
out_mknod_parent:
	if (err==-EEXIST)
		err=-EADDRINUSE;
	unix_release_addr(addr);
	goto out_up;
}

static int unix_dgram_connect(struct socket *sock, struct sockaddr *addr,
			      int alen, int flags)
{
	struct sock *sk = sock->sk;
	struct sockaddr_un *sunaddr=(struct sockaddr_un*)addr;
	struct sock *other;
	unsigned hash;
	int err;

	if (addr->sa_family != AF_UNSPEC) {
		err = unix_mkname(sunaddr, alen, &hash);
		if (err < 0)
			goto out;
		alen = err;

		if (sock->passcred && !unix_sk(sk)->addr &&
		    (err = unix_autobind(sock)) != 0)
			goto out;

		other=unix_find_other(sunaddr, alen, sock->type, hash, &err);
		if (!other)
			goto out;

		unix_state_wlock(sk);

		err = -EPERM;
		if (!unix_may_send(sk, other))
			goto out_unlock;

		err = security_unix_may_send(sk->sk_socket, other->sk_socket);
		if (err)
			goto out_unlock;

	} else {
		/*
		 *	1003.1g breaking connected state with AF_UNSPEC
		 */
		other = NULL;
		unix_state_wlock(sk);
	}

	/*
	 * If it was connected, reconnect.
	 */
	if (unix_peer(sk)) {
		struct sock *old_peer = unix_peer(sk);
		unix_peer(sk)=other;
		unix_state_wunlock(sk);

		if (other != old_peer)
			unix_dgram_disconnected(sk, old_peer);
		sock_put(old_peer);
	} else {
		unix_peer(sk)=other;
		unix_state_wunlock(sk);
	}
 	return 0;

out_unlock:
	unix_state_wunlock(sk);
	sock_put(other);
out:
	return err;
}

static long unix_wait_for_peer(struct sock *other, long timeo)
{
	struct unix_sock *u = unix_sk(other);
	int sched;
	DEFINE_WAIT(wait);

	prepare_to_wait_exclusive(&u->peer_wait, &wait, TASK_INTERRUPTIBLE);

	sched = !sock_flag(other, SOCK_DEAD) &&
		!(other->sk_shutdown & RCV_SHUTDOWN) &&
		(skb_queue_len(&other->sk_receive_queue) >
		 other->sk_max_ack_backlog);

	unix_state_runlock(other);

	if (sched)
		timeo = schedule_timeout(timeo);

	finish_wait(&u->peer_wait, &wait);
	return timeo;
}

static int unix_stream_connect(struct socket *sock, struct sockaddr *uaddr,
			       int addr_len, int flags)
{
	struct sockaddr_un *sunaddr=(struct sockaddr_un *)uaddr;
	struct sock *sk = sock->sk;
	struct unix_sock *u = unix_sk(sk), *newu, *otheru;
	struct sock *newsk = NULL;
	struct sock *other = NULL;
	struct sk_buff *skb = NULL;
	unsigned hash;
	int st;
	int err;
	long timeo;

	err = unix_mkname(sunaddr, addr_len, &hash);
	if (err < 0)
		goto out;
	addr_len = err;

	if (sock->passcred && !u->addr && (err = unix_autobind(sock)) != 0)
		goto out;

	timeo = sock_sndtimeo(sk, flags & O_NONBLOCK);

	/* First of all allocate resources.
	   If we will make it after state is locked,
	   we will have to recheck all again in any case.
	 */

	err = -ENOMEM;

	/* create new sock for complete connection */
	newsk = unix_create1(NULL);
	if (newsk == NULL)
		goto out;

	/* Allocate skb for sending to listening sock */
	skb = sock_wmalloc(newsk, 1, 0, GFP_KERNEL);
	if (skb == NULL)
		goto out;

restart:
	/*  Find listening sock. */
	other = unix_find_other(sunaddr, addr_len, sk->sk_type, hash, &err);
	if (!other)
		goto out;

	/* Latch state of peer */
	unix_state_rlock(other);

	/* Apparently VFS overslept socket death. Retry. */
	if (sock_flag(other, SOCK_DEAD)) {
		unix_state_runlock(other);
		sock_put(other);
		goto restart;
	}

	err = -ECONNREFUSED;
	if (other->sk_state != TCP_LISTEN)
		goto out_unlock;

	if (skb_queue_len(&other->sk_receive_queue) >
	    other->sk_max_ack_backlog) {
		err = -EAGAIN;
		if (!timeo)
			goto out_unlock;

		timeo = unix_wait_for_peer(other, timeo);

		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			goto out;
		sock_put(other);
		goto restart;
        }

	/* Latch our state.

	   It is tricky place. We need to grab write lock and cannot
	   drop lock on peer. It is dangerous because deadlock is
	   possible. Connect to self case and simultaneous
	   attempt to connect are eliminated by checking socket
	   state. other is TCP_LISTEN, if sk is TCP_LISTEN we
	   check this before attempt to grab lock.

	   Well, and we have to recheck the state after socket locked.
	 */
	st = sk->sk_state;

	switch (st) {
	case TCP_CLOSE:
		/* This is ok... continue with connect */
		break;
	case TCP_ESTABLISHED:
		/* Socket is already connected */
		err = -EISCONN;
		goto out_unlock;
	default:
		err = -EINVAL;
		goto out_unlock;
	}

	unix_state_wlock(sk);

	if (sk->sk_state != st) {
		unix_state_wunlock(sk);
		unix_state_runlock(other);
		sock_put(other);
		goto restart;
	}

	err = security_unix_stream_connect(sock, other->sk_socket, newsk);
	if (err) {
		unix_state_wunlock(sk);
		goto out_unlock;
	}

	/* The way is open! Fastly set all the necessary fields... */

	sock_hold(sk);
	unix_peer(newsk)	= sk;
	newsk->sk_state		= TCP_ESTABLISHED;
	newsk->sk_type		= sk->sk_type;
	newsk->sk_peercred.pid	= current->tgid;
	newsk->sk_peercred.uid	= current->euid;
	newsk->sk_peercred.gid	= current->egid;
	newu = unix_sk(newsk);
	newsk->sk_sleep		= &newu->peer_wait;
	otheru = unix_sk(other);

	/* copy address information from listening to new sock*/
	if (otheru->addr) {
		atomic_inc(&otheru->addr->refcnt);
		newu->addr = otheru->addr;
	}
	if (otheru->dentry) {
		newu->dentry	= dget(otheru->dentry);
		newu->mnt	= mntget(otheru->mnt);
	}

	/* Set credentials */
	sk->sk_peercred = other->sk_peercred;

	sock_hold(newsk);
	unix_peer(sk)	= newsk;
	sock->state	= SS_CONNECTED;
	sk->sk_state	= TCP_ESTABLISHED;

	unix_state_wunlock(sk);

	/* take ten and and send info to listening sock */
	spin_lock(&other->sk_receive_queue.lock);
	__skb_queue_tail(&other->sk_receive_queue, skb);
	/* Undo artificially decreased inflight after embrion
	 * is installed to listening socket. */
	atomic_inc(&newu->inflight);
	spin_unlock(&other->sk_receive_queue.lock);
	unix_state_runlock(other);
	other->sk_data_ready(other, 0);
	sock_put(other);
	return 0;

out_unlock:
	if (other)
		unix_state_runlock(other);

out:
	if (skb)
		kfree_skb(skb);
	if (newsk)
		unix_release_sock(newsk, 0);
	if (other)
		sock_put(other);
	return err;
}

static int unix_socketpair(struct socket *socka, struct socket *sockb)
{
	struct sock *ska=socka->sk, *skb = sockb->sk;

	/* Join our sockets back to back */
	sock_hold(ska);
	sock_hold(skb);
	unix_peer(ska)=skb;
	unix_peer(skb)=ska;
	ska->sk_peercred.pid = skb->sk_peercred.pid = current->tgid;
	ska->sk_peercred.uid = skb->sk_peercred.uid = current->euid;
	ska->sk_peercred.gid = skb->sk_peercred.gid = current->egid;

	if (ska->sk_type != SOCK_DGRAM) {
		ska->sk_state = TCP_ESTABLISHED;
		skb->sk_state = TCP_ESTABLISHED;
		socka->state  = SS_CONNECTED;
		sockb->state  = SS_CONNECTED;
	}
	return 0;
}

static int unix_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct sock *sk = sock->sk;
	struct sock *tsk;
	struct sk_buff *skb;
	int err;

	err = -EOPNOTSUPP;
	if (sock->type!=SOCK_STREAM && sock->type!=SOCK_SEQPACKET)
		goto out;

	err = -EINVAL;
	if (sk->sk_state != TCP_LISTEN)
		goto out;

	/* If socket state is TCP_LISTEN it cannot change (for now...),
	 * so that no locks are necessary.
	 */

	skb = skb_recv_datagram(sk, 0, flags&O_NONBLOCK, &err);
	if (!skb) {
		/* This means receive shutdown. */
		if (err == 0)
			err = -EINVAL;
		goto out;
	}

	tsk = skb->sk;
	skb_free_datagram(sk, skb);
	wake_up_interruptible(&unix_sk(sk)->peer_wait);

	/* attach accepted sock to socket */
	unix_state_wlock(tsk);
	newsock->state = SS_CONNECTED;
	sock_graft(tsk, newsock);
	unix_state_wunlock(tsk);
	return 0;

out:
	return err;
}


static int unix_getname(struct socket *sock, struct sockaddr *uaddr, int *uaddr_len, int peer)
{
	struct sock *sk = sock->sk;
	struct unix_sock *u;
	struct sockaddr_un *sunaddr=(struct sockaddr_un *)uaddr;
	int err = 0;

	if (peer) {
		sk = unix_peer_get(sk);

		err = -ENOTCONN;
		if (!sk)
			goto out;
		err = 0;
	} else {
		sock_hold(sk);
	}

	u = unix_sk(sk);
	unix_state_rlock(sk);
	if (!u->addr) {
		sunaddr->sun_family = AF_UNIX;
		sunaddr->sun_path[0] = 0;
		*uaddr_len = sizeof(short);
	} else {
		struct unix_address *addr = u->addr;

		*uaddr_len = addr->len;
		memcpy(sunaddr, addr->name, *uaddr_len);
	}
	unix_state_runlock(sk);
	sock_put(sk);
out:
	return err;
}

static void unix_detach_fds(struct scm_cookie *scm, struct sk_buff *skb)
{
	int i;

	scm->fp = UNIXCB(skb).fp;
	skb->destructor = sock_wfree;
	UNIXCB(skb).fp = NULL;

	for (i=scm->fp->count-1; i>=0; i--)
		unix_notinflight(scm->fp->fp[i]);
}

static void unix_destruct_fds(struct sk_buff *skb)
{
	struct scm_cookie scm;
	memset(&scm, 0, sizeof(scm));
	unix_detach_fds(&scm, skb);

	/* Alas, it calls VFS */
	/* So fscking what? fput() had been SMP-safe since the last Summer */
	scm_destroy(&scm);
	sock_wfree(skb);
}

static void unix_attach_fds(struct scm_cookie *scm, struct sk_buff *skb)
{
	int i;
	for (i=scm->fp->count-1; i>=0; i--)
		unix_inflight(scm->fp->fp[i]);
	UNIXCB(skb).fp = scm->fp;
	skb->destructor = unix_destruct_fds;
	scm->fp = NULL;
}

/*
 *	Send AF_UNIX data.
 */

static int unix_dgram_sendmsg(struct kiocb *kiocb, struct socket *sock,
			      struct msghdr *msg, size_t len)
{
	struct sock_iocb *siocb = kiocb_to_siocb(kiocb);
	struct sock *sk = sock->sk;
	struct unix_sock *u = unix_sk(sk);
	struct sockaddr_un *sunaddr=msg->msg_name;
	struct sock *other = NULL;
	int namelen = 0; /* fake GCC */
	int err;
	unsigned hash;
	struct sk_buff *skb;
	long timeo;
	struct scm_cookie tmp_scm;

	if (NULL == siocb->scm)
		siocb->scm = &tmp_scm;
	err = scm_send(sock, msg, siocb->scm);
	if (err < 0)
		return err;

	err = -EOPNOTSUPP;
	if (msg->msg_flags&MSG_OOB)
		goto out;

	if (msg->msg_namelen) {
		err = unix_mkname(sunaddr, msg->msg_namelen, &hash);
		if (err < 0)
			goto out;
		namelen = err;
	} else {
		sunaddr = NULL;
		err = -ENOTCONN;
		other = unix_peer_get(sk);
		if (!other)
			goto out;
	}

	if (sock->passcred && !u->addr && (err = unix_autobind(sock)) != 0)
		goto out;

	err = -EMSGSIZE;
	if (len > sk->sk_sndbuf - 32)
		goto out;

	skb = sock_alloc_send_skb(sk, len, msg->msg_flags&MSG_DONTWAIT, &err);
	if (skb==NULL)
		goto out;

	memcpy(UNIXCREDS(skb), &siocb->scm->creds, sizeof(struct ucred));
	if (siocb->scm->fp)
		unix_attach_fds(siocb->scm, skb);

	skb->h.raw = skb->data;
	err = memcpy_fromiovec(skb_put(skb,len), msg->msg_iov, len);
	if (err)
		goto out_free;

	timeo = sock_sndtimeo(sk, msg->msg_flags & MSG_DONTWAIT);

restart:
	if (!other) {
		err = -ECONNRESET;
		if (sunaddr == NULL)
			goto out_free;

		other = unix_find_other(sunaddr, namelen, sk->sk_type,
					hash, &err);
		if (other==NULL)
			goto out_free;
	}

	unix_state_rlock(other);
	err = -EPERM;
	if (!unix_may_send(sk, other))
		goto out_unlock;

	if (sock_flag(other, SOCK_DEAD)) {
		/*
		 *	Check with 1003.1g - what should
		 *	datagram error
		 */
		unix_state_runlock(other);
		sock_put(other);

		err = 0;
		unix_state_wlock(sk);
		if (unix_peer(sk) == other) {
			unix_peer(sk)=NULL;
			unix_state_wunlock(sk);

			unix_dgram_disconnected(sk, other);
			sock_put(other);
			err = -ECONNREFUSED;
		} else {
			unix_state_wunlock(sk);
		}

		other = NULL;
		if (err)
			goto out_free;
		goto restart;
	}

	err = -EPIPE;
	if (other->sk_shutdown & RCV_SHUTDOWN)
		goto out_unlock;

	err = security_unix_may_send(sk->sk_socket, other->sk_socket);
	if (err)
		goto out_unlock;

	if (unix_peer(other) != sk &&
	    (skb_queue_len(&other->sk_receive_queue) >
	     other->sk_max_ack_backlog)) {
		if (!timeo) {
			err = -EAGAIN;
			goto out_unlock;
		}

		timeo = unix_wait_for_peer(other, timeo);

		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			goto out_free;

		goto restart;
	}

	skb_queue_tail(&other->sk_receive_queue, skb);
	unix_state_runlock(other);
	other->sk_data_ready(other, len);
	sock_put(other);
	scm_destroy(siocb->scm);
	return len;

out_unlock:
	unix_state_runlock(other);
out_free:
	kfree_skb(skb);
out:
	if (other)
		sock_put(other);
	scm_destroy(siocb->scm);
	return err;
}

		
static int unix_stream_sendmsg(struct kiocb *kiocb, struct socket *sock,
			       struct msghdr *msg, size_t len)
{
	struct sock_iocb *siocb = kiocb_to_siocb(kiocb);
	struct sock *sk = sock->sk;
	struct sock *other = NULL;
	struct sockaddr_un *sunaddr=msg->msg_name;
	int err,size;
	struct sk_buff *skb;
	int sent=0;
	struct scm_cookie tmp_scm;

	if (NULL == siocb->scm)
		siocb->scm = &tmp_scm;
	err = scm_send(sock, msg, siocb->scm);
	if (err < 0)
		return err;

	err = -EOPNOTSUPP;
	if (msg->msg_flags&MSG_OOB)
		goto out_err;

	if (msg->msg_namelen) {
		err = sk->sk_state == TCP_ESTABLISHED ? -EISCONN : -EOPNOTSUPP;
		goto out_err;
	} else {
		sunaddr = NULL;
		err = -ENOTCONN;
		other = unix_peer_get(sk);
		if (!other)
			goto out_err;
	}

	if (sk->sk_shutdown & SEND_SHUTDOWN)
		goto pipe_err;

	while(sent < len)
	{
		/*
		 *	Optimisation for the fact that under 0.01% of X messages typically
		 *	need breaking up.
		 */

		size=len-sent;

		/* Keep two messages in the pipe so it schedules better */
		if (size > sk->sk_sndbuf / 2 - 64)
			size = sk->sk_sndbuf / 2 - 64;

		if (size > SKB_MAX_ALLOC)
			size = SKB_MAX_ALLOC;
			
		/*
		 *	Grab a buffer
		 */
		 
		skb=sock_alloc_send_skb(sk,size,msg->msg_flags&MSG_DONTWAIT, &err);

		if (skb==NULL)
			goto out_err;

		/*
		 *	If you pass two values to the sock_alloc_send_skb
		 *	it tries to grab the large buffer with GFP_NOFS
		 *	(which can fail easily), and if it fails grab the
		 *	fallback size buffer which is under a page and will
		 *	succeed. [Alan]
		 */
		size = min_t(int, size, skb_tailroom(skb));

		memcpy(UNIXCREDS(skb), &siocb->scm->creds, sizeof(struct ucred));
		if (siocb->scm->fp)
			unix_attach_fds(siocb->scm, skb);

		if ((err = memcpy_fromiovec(skb_put(skb,size), msg->msg_iov, size)) != 0) {
			kfree_skb(skb);
			goto out_err;
		}

		unix_state_rlock(other);

		if (sock_flag(other, SOCK_DEAD) ||
		    (other->sk_shutdown & RCV_SHUTDOWN))
			goto pipe_err_free;

		skb_queue_tail(&other->sk_receive_queue, skb);
		unix_state_runlock(other);
		other->sk_data_ready(other, size);
		sent+=size;
	}
	sock_put(other);

	scm_destroy(siocb->scm);
	siocb->scm = NULL;

	return sent;

pipe_err_free:
	unix_state_runlock(other);
	kfree_skb(skb);
pipe_err:
	if (sent==0 && !(msg->msg_flags&MSG_NOSIGNAL))
		send_sig(SIGPIPE,current,0);
	err = -EPIPE;
out_err:
        if (other)
		sock_put(other);
	scm_destroy(siocb->scm);
	siocb->scm = NULL;
	return sent ? : err;
}

static void unix_copy_addr(struct msghdr *msg, struct sock *sk)
{
	struct unix_sock *u = unix_sk(sk);

	msg->msg_namelen = 0;
	if (u->addr) {
		msg->msg_namelen = u->addr->len;
		memcpy(msg->msg_name, u->addr->name, u->addr->len);
	}
}

static int unix_dgram_recvmsg(struct kiocb *iocb, struct socket *sock,
			      struct msghdr *msg, size_t size,
			      int flags)
{
	struct sock_iocb *siocb = kiocb_to_siocb(iocb);
	struct scm_cookie tmp_scm;
	struct sock *sk = sock->sk;
	struct unix_sock *u = unix_sk(sk);
	int noblock = flags & MSG_DONTWAIT;
	struct sk_buff *skb;
	int err;

	err = -EOPNOTSUPP;
	if (flags&MSG_OOB)
		goto out;

	msg->msg_namelen = 0;

	skb = skb_recv_datagram(sk, flags, noblock, &err);
	if (!skb)
		goto out;

	wake_up_interruptible(&u->peer_wait);

	if (msg->msg_name)
		unix_copy_addr(msg, skb->sk);

	if (size > skb->len)
		size = skb->len;
	else if (size < skb->len)
		msg->msg_flags |= MSG_TRUNC;

	err = skb_copy_datagram_iovec(skb, 0, msg->msg_iov, size);
	if (err)
		goto out_free;

	if (!siocb->scm) {
		siocb->scm = &tmp_scm;
		memset(&tmp_scm, 0, sizeof(tmp_scm));
	}
	siocb->scm->creds = *UNIXCREDS(skb);

	if (!(flags & MSG_PEEK))
	{
		if (UNIXCB(skb).fp)
			unix_detach_fds(siocb->scm, skb);
	}
	else 
	{
		/* It is questionable: on PEEK we could:
		   - do not return fds - good, but too simple 8)
		   - return fds, and do not return them on read (old strategy,
		     apparently wrong)
		   - clone fds (I chose it for now, it is the most universal
		     solution)
		
	           POSIX 1003.1g does not actually define this clearly
	           at all. POSIX 1003.1g doesn't define a lot of things
	           clearly however!		     
		   
		*/
		if (UNIXCB(skb).fp)
			siocb->scm->fp = scm_fp_dup(UNIXCB(skb).fp);
	}
	err = size;

	scm_recv(sock, msg, siocb->scm, flags);

out_free:
	skb_free_datagram(sk,skb);
out:
	return err;
}

/*
 *	Sleep until data has arrive. But check for races..
 */
 
static long unix_stream_data_wait(struct sock * sk, long timeo)
{
	DEFINE_WAIT(wait);

	unix_state_rlock(sk);

	for (;;) {
		prepare_to_wait(sk->sk_sleep, &wait, TASK_INTERRUPTIBLE);

		if (skb_queue_len(&sk->sk_receive_queue) ||
		    sk->sk_err ||
		    (sk->sk_shutdown & RCV_SHUTDOWN) ||
		    signal_pending(current) ||
		    !timeo)
			break;

		set_bit(SOCK_ASYNC_WAITDATA, &sk->sk_socket->flags);
		unix_state_runlock(sk);
		timeo = schedule_timeout(timeo);
		unix_state_rlock(sk);
		clear_bit(SOCK_ASYNC_WAITDATA, &sk->sk_socket->flags);
	}

	finish_wait(sk->sk_sleep, &wait);
	unix_state_runlock(sk);
	return timeo;
}



static int unix_stream_recvmsg(struct kiocb *iocb, struct socket *sock,
			       struct msghdr *msg, size_t size,
			       int flags)
{
	struct sock_iocb *siocb = kiocb_to_siocb(iocb);
	struct scm_cookie tmp_scm;
	struct sock *sk = sock->sk;
	struct unix_sock *u = unix_sk(sk);
	struct sockaddr_un *sunaddr=msg->msg_name;
	int copied = 0;
	int check_creds = 0;
	int target;
	int err = 0;
	long timeo;

	err = -EINVAL;
	if (sk->sk_state != TCP_ESTABLISHED)
		goto out;

	err = -EOPNOTSUPP;
	if (flags&MSG_OOB)
		goto out;

	target = sock_rcvlowat(sk, flags&MSG_WAITALL, size);
	timeo = sock_rcvtimeo(sk, flags&MSG_DONTWAIT);

	msg->msg_namelen = 0;

	/* Lock the socket to prevent queue disordering
	 * while sleeps in memcpy_tomsg
	 */

	if (!siocb->scm) {
		siocb->scm = &tmp_scm;
		memset(&tmp_scm, 0, sizeof(tmp_scm));
	}

	down(&u->readsem);

	do
	{
		int chunk;
		struct sk_buff *skb;

		skb = skb_dequeue(&sk->sk_receive_queue);
		if (skb==NULL)
		{
			if (copied >= target)
				break;

			/*
			 *	POSIX 1003.1g mandates this order.
			 */
			 
			if ((err = sock_error(sk)) != 0)
				break;
			if (sk->sk_shutdown & RCV_SHUTDOWN)
				break;
			err = -EAGAIN;
			if (!timeo)
				break;
			up(&u->readsem);

			timeo = unix_stream_data_wait(sk, timeo);

			if (signal_pending(current)) {
				err = sock_intr_errno(timeo);
				goto out;
			}
			down(&u->readsem);
			continue;
		}

		if (check_creds) {
			/* Never glue messages from different writers */
			if (memcmp(UNIXCREDS(skb), &siocb->scm->creds, sizeof(siocb->scm->creds)) != 0) {
				skb_queue_head(&sk->sk_receive_queue, skb);
				break;
			}
		} else {
			/* Copy credentials */
			siocb->scm->creds = *UNIXCREDS(skb);
			check_creds = 1;
		}

		/* Copy address just once */
		if (sunaddr)
		{
			unix_copy_addr(msg, skb->sk);
			sunaddr = NULL;
		}

		chunk = min_t(unsigned int, skb->len, size);
		if (memcpy_toiovec(msg->msg_iov, skb->data, chunk)) {
			skb_queue_head(&sk->sk_receive_queue, skb);
			if (copied == 0)
				copied = -EFAULT;
			break;
		}
		copied += chunk;
		size -= chunk;

		/* Mark read part of skb as used */
		if (!(flags & MSG_PEEK))
		{
			skb_pull(skb, chunk);

			if (UNIXCB(skb).fp)
				unix_detach_fds(siocb->scm, skb);

			/* put the skb back if we didn't use it up.. */
			if (skb->len)
			{
				skb_queue_head(&sk->sk_receive_queue, skb);
				break;
			}

			kfree_skb(skb);

			if (siocb->scm->fp)
				break;
		}
		else
		{
			/* It is questionable, see note in unix_dgram_recvmsg.
			 */
			if (UNIXCB(skb).fp)
				siocb->scm->fp = scm_fp_dup(UNIXCB(skb).fp);

			/* put message back and return */
			skb_queue_head(&sk->sk_receive_queue, skb);
			break;
		}
	} while (size);

	up(&u->readsem);
	scm_recv(sock, msg, siocb->scm, flags);
out:
	return copied ? : err;
}

static int unix_shutdown(struct socket *sock, int mode)
{
	struct sock *sk = sock->sk;
	struct sock *other;

	mode = (mode+1)&(RCV_SHUTDOWN|SEND_SHUTDOWN);

	if (mode) {
		unix_state_wlock(sk);
		sk->sk_shutdown |= mode;
		other=unix_peer(sk);
		if (other)
			sock_hold(other);
		unix_state_wunlock(sk);
		sk->sk_state_change(sk);

		if (other &&
			(sk->sk_type == SOCK_STREAM || sk->sk_type == SOCK_SEQPACKET)) {

			int peer_mode = 0;

			if (mode&RCV_SHUTDOWN)
				peer_mode |= SEND_SHUTDOWN;
			if (mode&SEND_SHUTDOWN)
				peer_mode |= RCV_SHUTDOWN;
			unix_state_wlock(other);
			other->sk_shutdown |= peer_mode;
			unix_state_wunlock(other);
			other->sk_state_change(other);
			read_lock(&other->sk_callback_lock);
			if (peer_mode == SHUTDOWN_MASK)
				sk_wake_async(other,1,POLL_HUP);
			else if (peer_mode & RCV_SHUTDOWN)
				sk_wake_async(other,1,POLL_IN);
			read_unlock(&other->sk_callback_lock);
		}
		if (other)
			sock_put(other);
	}
	return 0;
}

static int unix_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = sock->sk;
	long amount=0;
	int err;

	switch(cmd)
	{
		case SIOCOUTQ:
			amount = atomic_read(&sk->sk_wmem_alloc);
			err = put_user(amount, (int __user *)arg);
			break;
		case SIOCINQ:
		{
			struct sk_buff *skb;
			if (sk->sk_state == TCP_LISTEN) {
				err = -EINVAL;
				break;
			}

			spin_lock(&sk->sk_receive_queue.lock);
			skb = skb_peek(&sk->sk_receive_queue);
			if (skb)
				amount=skb->len;
			spin_unlock(&sk->sk_receive_queue.lock);
			err = put_user(amount, (int __user *)arg);
			break;
		}

		default:
			err = dev_ioctl(cmd, (void __user *)arg);
			break;
	}
	return err;
}

static unsigned int unix_poll(struct file * file, struct socket *sock, poll_table *wait)
{
	struct sock *sk = sock->sk;
	unsigned int mask;

	poll_wait(file, sk->sk_sleep, wait);
	mask = 0;

	/* exceptional events? */
	if (sk->sk_err)
		mask |= POLLERR;
	if (sk->sk_shutdown == SHUTDOWN_MASK)
		mask |= POLLHUP;

	/* readable? */
	if (!skb_queue_empty(&sk->sk_receive_queue) ||
	    (sk->sk_shutdown & RCV_SHUTDOWN))
		mask |= POLLIN | POLLRDNORM;

	/* Connection-based need to check for termination and startup */
	if ((sk->sk_type == SOCK_STREAM || sk->sk_type == SOCK_SEQPACKET) && sk->sk_state == TCP_CLOSE)
		mask |= POLLHUP;

	/*
	 * we set writable also when the other side has shut down the
	 * connection. This prevents stuck sockets.
	 */
	if (unix_writable(sk))
		mask |= POLLOUT | POLLWRNORM | POLLWRBAND;

	return mask;
}


#ifdef CONFIG_PROC_FS
static struct sock *unix_seq_idx(int *iter, loff_t pos)
{
	loff_t off = 0;
	struct sock *s;

	for (s = first_unix_socket(iter); s; s = next_unix_socket(iter, s)) {
		if (off == pos) 
			return s;
		++off;
	}
	return NULL;
}


static void *unix_seq_start(struct seq_file *seq, loff_t *pos)
{
	read_lock(&unix_table_lock);
	return *pos ? unix_seq_idx(seq->private, *pos - 1) : ((void *) 1);
}

static void *unix_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	++*pos;

	if (v == (void *)1) 
		return first_unix_socket(seq->private);
	return next_unix_socket(seq->private, v);
}

static void unix_seq_stop(struct seq_file *seq, void *v)
{
	read_unlock(&unix_table_lock);
}

static int unix_seq_show(struct seq_file *seq, void *v)
{
	
	if (v == (void *)1)
		seq_puts(seq, "Num       RefCount Protocol Flags    Type St "
			 "Inode Path\n");
	else {
		struct sock *s = v;
		struct unix_sock *u = unix_sk(s);
		unix_state_rlock(s);

		seq_printf(seq, "%p: %08X %08X %08X %04X %02X %5lu",
			s,
			atomic_read(&s->sk_refcnt),
			0,
			s->sk_state == TCP_LISTEN ? __SO_ACCEPTCON : 0,
			s->sk_type,
			s->sk_socket ?
			(s->sk_state == TCP_ESTABLISHED ? SS_CONNECTED : SS_UNCONNECTED) :
			(s->sk_state == TCP_ESTABLISHED ? SS_CONNECTING : SS_DISCONNECTING),
			sock_i_ino(s));

		if (u->addr) {
			int i, len;
			seq_putc(seq, ' ');

			i = 0;
			len = u->addr->len - sizeof(short);
			if (!UNIX_ABSTRACT(s))
				len--;
			else {
				seq_putc(seq, '@');
				i++;
			}
			for ( ; i < len; i++)
				seq_putc(seq, u->addr->name->sun_path[i]);
		}
		unix_state_runlock(s);
		seq_putc(seq, '\n');
	}

	return 0;
}

static struct seq_operations unix_seq_ops = {
	.start  = unix_seq_start,
	.next   = unix_seq_next,
	.stop   = unix_seq_stop,
	.show   = unix_seq_show,
};


static int unix_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	int rc = -ENOMEM;
	int *iter = kmalloc(sizeof(int), GFP_KERNEL);

	if (!iter)
		goto out;

	rc = seq_open(file, &unix_seq_ops);
	if (rc)
		goto out_kfree;

	seq	     = file->private_data;
	seq->private = iter;
	*iter = 0;
out:
	return rc;
out_kfree:
	kfree(iter);
	goto out;
}

static struct file_operations unix_seq_fops = {
	.owner		= THIS_MODULE,
	.open		= unix_seq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

#endif

static struct net_proto_family unix_family_ops = {
	.family = PF_UNIX,
	.create = unix_create,
	.owner	= THIS_MODULE,
};

#ifdef CONFIG_SYSCTL
extern void unix_sysctl_register(void);
extern void unix_sysctl_unregister(void);
#else
static inline void unix_sysctl_register(void) {}
static inline void unix_sysctl_unregister(void) {}
#endif

static int __init af_unix_init(void)
{
	struct sk_buff *dummy_skb;

	if (sizeof(struct unix_skb_parms) > sizeof(dummy_skb->cb)) {
		printk(KERN_CRIT "%s: panic\n", __FUNCTION__);
		return -1;
	}
        /* allocate our sock slab cache */
        unix_sk_cachep = kmem_cache_create("unix_sock",
					   sizeof(struct unix_sock), 0,
					   SLAB_HWCACHE_ALIGN, NULL, NULL);
        if (!unix_sk_cachep)
                printk(KERN_CRIT
                        "af_unix_init: Cannot create unix_sock SLAB cache!\n");

	sock_register(&unix_family_ops);
#ifdef CONFIG_PROC_FS
	proc_net_fops_create("unix", 0, &unix_seq_fops);
#endif
	unix_sysctl_register();
	return 0;
}

static void __exit af_unix_exit(void)
{
	sock_unregister(PF_UNIX);
	unix_sysctl_unregister();
	proc_net_remove("unix");
	kmem_cache_destroy(unix_sk_cachep);
}

module_init(af_unix_init);
module_exit(af_unix_exit);

MODULE_LICENSE("GPL");
MODULE_ALIAS_NETPROTO(PF_UNIX);
