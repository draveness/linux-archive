/*
 *  inode.c
 *
 *  Copyright (C) 1995, 1996 by Volker Lendecke
 *  Modified for big endian by J.F. Chadima and David S. Miller
 *  Modified 1997 Peter Waltenberg, Bill Hawes, David Woodhouse for 2.1 dcache
 *  Modified 1998 Wolfram Pienkoss for NLS
 *  Modified 2000 Ben Harris, University of Cambridge for NFS NS meta-info
 *
 */

#include <linux/config.h>
#include <linux/module.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/byteorder.h>

#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/smp_lock.h>
#include <linux/vfs.h>

#include <linux/ncp_fs.h>

#include <net/sock.h>

#include "ncplib_kernel.h"
#include "getopt.h"

static void ncp_delete_inode(struct inode *);
static void ncp_put_super(struct super_block *);
static int  ncp_statfs(struct super_block *, struct kstatfs *);

static kmem_cache_t * ncp_inode_cachep;

static struct inode *ncp_alloc_inode(struct super_block *sb)
{
	struct ncp_inode_info *ei;
	ei = (struct ncp_inode_info *)kmem_cache_alloc(ncp_inode_cachep, SLAB_KERNEL);
	if (!ei)
		return NULL;
	return &ei->vfs_inode;
}

static void ncp_destroy_inode(struct inode *inode)
{
	kmem_cache_free(ncp_inode_cachep, NCP_FINFO(inode));
}

static void init_once(void * foo, kmem_cache_t * cachep, unsigned long flags)
{
	struct ncp_inode_info *ei = (struct ncp_inode_info *) foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR) {
		init_MUTEX(&ei->open_sem);
		inode_init_once(&ei->vfs_inode);
	}
}
 
static int init_inodecache(void)
{
	ncp_inode_cachep = kmem_cache_create("ncp_inode_cache",
					     sizeof(struct ncp_inode_info),
					     0, SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT,
					     init_once, NULL);
	if (ncp_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	if (kmem_cache_destroy(ncp_inode_cachep))
		printk(KERN_INFO "ncp_inode_cache: not all structures were freed\n");
}

static int ncp_remount(struct super_block *sb, int *flags, char* data)
{
	*flags |= MS_NODIRATIME;
	return 0;
}

static struct super_operations ncp_sops =
{
	.alloc_inode	= ncp_alloc_inode,
	.destroy_inode	= ncp_destroy_inode,
	.drop_inode	= generic_delete_inode,
	.delete_inode	= ncp_delete_inode,
	.put_super	= ncp_put_super,
	.statfs		= ncp_statfs,
	.remount_fs	= ncp_remount,
};

extern struct dentry_operations ncp_root_dentry_operations;
#if defined(CONFIG_NCPFS_EXTRAS) || defined(CONFIG_NCPFS_NFS_NS)
extern struct address_space_operations ncp_symlink_aops;
extern int ncp_symlink(struct inode*, struct dentry*, const char*);
#endif

/*
 * Fill in the ncpfs-specific information in the inode.
 */
static void ncp_update_dirent(struct inode *inode, struct ncp_entry_info *nwinfo)
{
	NCP_FINFO(inode)->DosDirNum = nwinfo->i.DosDirNum;
	NCP_FINFO(inode)->dirEntNum = nwinfo->i.dirEntNum;
	NCP_FINFO(inode)->volNumber = nwinfo->volume;
}

void ncp_update_inode(struct inode *inode, struct ncp_entry_info *nwinfo)
{
	ncp_update_dirent(inode, nwinfo);
	NCP_FINFO(inode)->nwattr = nwinfo->i.attributes;
	NCP_FINFO(inode)->access = nwinfo->access;
	memcpy(NCP_FINFO(inode)->file_handle, nwinfo->file_handle,
			sizeof(nwinfo->file_handle));
	DPRINTK("ncp_update_inode: updated %s, volnum=%d, dirent=%u\n",
		nwinfo->i.entryName, NCP_FINFO(inode)->volNumber,
		NCP_FINFO(inode)->dirEntNum);
}

static void ncp_update_dates(struct inode *inode, struct nw_info_struct *nwi)
{
	/* NFS namespace mode overrides others if it's set. */
	DPRINTK(KERN_DEBUG "ncp_update_dates_and_mode: (%s) nfs.mode=0%o\n",
		nwi->entryName, nwi->nfs.mode);
	if (nwi->nfs.mode) {
		/* XXX Security? */
		inode->i_mode = nwi->nfs.mode;
	}

	inode->i_blocks = (inode->i_size + NCP_BLOCK_SIZE - 1) >> NCP_BLOCK_SHIFT;

	inode->i_mtime.tv_sec = ncp_date_dos2unix(le16_to_cpu(nwi->modifyTime),
					   le16_to_cpu(nwi->modifyDate));
	inode->i_ctime.tv_sec = ncp_date_dos2unix(le16_to_cpu(nwi->creationTime),
					   le16_to_cpu(nwi->creationDate));
	inode->i_atime.tv_sec = ncp_date_dos2unix(0,
					   le16_to_cpu(nwi->lastAccessDate));
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_nsec = 0;
	inode->i_ctime.tv_nsec = 0;
}

static void ncp_update_attrs(struct inode *inode, struct ncp_entry_info *nwinfo)
{
	struct nw_info_struct *nwi = &nwinfo->i;
	struct ncp_server *server = NCP_SERVER(inode);

	if (nwi->attributes & aDIR) {
		inode->i_mode = server->m.dir_mode;
		/* for directories dataStreamSize seems to be some
		   Object ID ??? */
		inode->i_size = NCP_BLOCK_SIZE;
	} else {
		inode->i_mode = server->m.file_mode;
		inode->i_size = le32_to_cpu(nwi->dataStreamSize);
#ifdef CONFIG_NCPFS_EXTRAS
		if ((server->m.flags & (NCP_MOUNT_EXTRAS|NCP_MOUNT_SYMLINKS)) 
		 && (nwi->attributes & aSHARED)) {
			switch (nwi->attributes & (aHIDDEN|aSYSTEM)) {
				case aHIDDEN:
					if (server->m.flags & NCP_MOUNT_SYMLINKS) {
						if (/* (inode->i_size >= NCP_MIN_SYMLINK_SIZE)
						 && */ (inode->i_size <= NCP_MAX_SYMLINK_SIZE)) {
							inode->i_mode = (inode->i_mode & ~S_IFMT) | S_IFLNK;
							NCP_FINFO(inode)->flags |= NCPI_KLUDGE_SYMLINK;
							break;
						}
					}
					/* FALLTHROUGH */
				case 0:
					if (server->m.flags & NCP_MOUNT_EXTRAS)
						inode->i_mode |= S_IRUGO;
					break;
				case aSYSTEM:
					if (server->m.flags & NCP_MOUNT_EXTRAS)
						inode->i_mode |= (inode->i_mode >> 2) & S_IXUGO;
					break;
				/* case aSYSTEM|aHIDDEN: */
				default:
					/* reserved combination */
					break;
			}
		}
#endif
	}
	if (nwi->attributes & aRONLY) inode->i_mode &= ~S_IWUGO;
}

void ncp_update_inode2(struct inode* inode, struct ncp_entry_info *nwinfo)
{
	NCP_FINFO(inode)->flags = 0;
	if (!atomic_read(&NCP_FINFO(inode)->opened)) {
		NCP_FINFO(inode)->nwattr = nwinfo->i.attributes;
		ncp_update_attrs(inode, nwinfo);
	}

	ncp_update_dates(inode, &nwinfo->i);
	ncp_update_dirent(inode, nwinfo);
}

/*
 * Fill in the inode based on the ncp_entry_info structure.
 */
static void ncp_set_attr(struct inode *inode, struct ncp_entry_info *nwinfo)
{
	struct ncp_server *server = NCP_SERVER(inode);

	NCP_FINFO(inode)->flags = 0;
	
	ncp_update_attrs(inode, nwinfo);

	DDPRINTK("ncp_read_inode: inode->i_mode = %u\n", inode->i_mode);

	inode->i_nlink = 1;
	inode->i_uid = server->m.uid;
	inode->i_gid = server->m.gid;
	inode->i_blksize = NCP_BLOCK_SIZE;

	ncp_update_dates(inode, &nwinfo->i);
	ncp_update_inode(inode, nwinfo);
}

#if defined(CONFIG_NCPFS_EXTRAS) || defined(CONFIG_NCPFS_NFS_NS)
static struct inode_operations ncp_symlink_inode_operations = {
	.readlink	= page_readlink,
	.follow_link	= page_follow_link,
	.setattr	= ncp_notify_change,
};
#endif

/*
 * Get a new inode.
 */
struct inode * 
ncp_iget(struct super_block *sb, struct ncp_entry_info *info)
{
	struct inode *inode;

	if (info == NULL) {
		printk(KERN_ERR "ncp_iget: info is NULL\n");
		return NULL;
	}

	inode = new_inode(sb);
	if (inode) {
		atomic_set(&NCP_FINFO(inode)->opened, info->opened);

		inode->i_ino = info->ino;
		ncp_set_attr(inode, info);
		if (S_ISREG(inode->i_mode)) {
			inode->i_op = &ncp_file_inode_operations;
			inode->i_fop = &ncp_file_operations;
		} else if (S_ISDIR(inode->i_mode)) {
			inode->i_op = &ncp_dir_inode_operations;
			inode->i_fop = &ncp_dir_operations;
#ifdef CONFIG_NCPFS_NFS_NS
		} else if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode) || S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
			init_special_inode(inode, inode->i_mode,
				new_decode_dev(info->i.nfs.rdev));
#endif
#if defined(CONFIG_NCPFS_EXTRAS) || defined(CONFIG_NCPFS_NFS_NS)
		} else if (S_ISLNK(inode->i_mode)) {
			inode->i_op = &ncp_symlink_inode_operations;
			inode->i_data.a_ops = &ncp_symlink_aops;
#endif
		} else {
			make_bad_inode(inode);
		}
		insert_inode_hash(inode);
	} else
		printk(KERN_ERR "ncp_iget: iget failed!\n");
	return inode;
}

static void
ncp_delete_inode(struct inode *inode)
{
	if (S_ISDIR(inode->i_mode)) {
		DDPRINTK("ncp_delete_inode: put directory %ld\n", inode->i_ino);
	}

	if (ncp_make_closed(inode) != 0) {
		/* We can't do anything but complain. */
		printk(KERN_ERR "ncp_delete_inode: could not close\n");
	}
	clear_inode(inode);
}

static void ncp_stop_tasks(struct ncp_server *server) {
	struct sock* sk = server->ncp_sock->sk;
		
	sk->sk_error_report = server->error_report;
	sk->sk_data_ready   = server->data_ready;
	sk->sk_write_space  = server->write_space;
	del_timer_sync(&server->timeout_tm);
	flush_scheduled_work();
}

static const struct ncp_option ncp_opts[] = {
	{ "uid",	OPT_INT,	'u' },
	{ "gid",	OPT_INT,	'g' },
	{ "owner",	OPT_INT,	'o' },
	{ "mode",	OPT_INT,	'm' },
	{ "dirmode",	OPT_INT,	'd' },
	{ "timeout",	OPT_INT,	't' },
	{ "retry",	OPT_INT,	'r' },
	{ "flags",	OPT_INT,	'f' },
	{ "wdogpid",	OPT_INT,	'w' },
	{ "ncpfd",	OPT_INT,	'n' },
	{ "infofd",	OPT_INT,	'i' },	/* v5 */
	{ "version",	OPT_INT,	'v' },
	{ NULL,		0,		0 } };

static int ncp_parse_options(struct ncp_mount_data_kernel *data, char *options) {
	int optval;
	char *optarg;
	unsigned long optint;
	int version = 0;

	data->flags = 0;
	data->int_flags = 0;
	data->mounted_uid = 0;
	data->wdog_pid = -1;
	data->ncp_fd = ~0;
	data->time_out = 10;
	data->retry_count = 20;
	data->uid = 0;
	data->gid = 0;
	data->file_mode = 0600;
	data->dir_mode = 0700;
	data->info_fd = -1;
	data->mounted_vol[0] = 0;
	
	while ((optval = ncp_getopt("ncpfs", &options, ncp_opts, NULL, &optarg, &optint)) != 0) {
		if (optval < 0)
			return optval;
		switch (optval) {
			case 'u':
				data->uid = optint;
				break;
			case 'g':
				data->gid = optint;
				break;
			case 'o':
				data->mounted_uid = optint;
				break;
			case 'm':
				data->file_mode = optint;
				break;
			case 'd':
				data->dir_mode = optint;
				break;
			case 't':
				data->time_out = optint;
				break;
			case 'r':
				data->retry_count = optint;
				break;
			case 'f':
				data->flags = optint;
				break;
			case 'w':
				data->wdog_pid = optint;
				break;
			case 'n':
				data->ncp_fd = optint;
				break;
			case 'i':
				data->info_fd = optint;
				break;
			case 'v':
				if (optint < NCP_MOUNT_VERSION_V4) {
					return -ECHRNG;
				}
				if (optint > NCP_MOUNT_VERSION_V5) {
					return -ECHRNG;
				}
				version = optint;
				break;
			
		}
	}
	return 0;
}

static int ncp_fill_super(struct super_block *sb, void *raw_data, int silent)
{
	struct ncp_mount_data_kernel data;
	struct ncp_server *server;
	struct file *ncp_filp;
	struct inode *root_inode;
	struct inode *sock_inode;
	struct socket *sock;
	int error;
	int default_bufsize;
#ifdef CONFIG_NCPFS_PACKET_SIGNING
	int options;
#endif
	struct ncp_entry_info finfo;

	server = kmalloc(sizeof(struct ncp_server), GFP_KERNEL);
	if (!server)
		return -ENOMEM;
	sb->s_fs_info = server;
	memset(server, 0, sizeof(struct ncp_server));

	error = -EFAULT;
	if (raw_data == NULL)
		goto out;
	switch (*(int*)raw_data) {
		case NCP_MOUNT_VERSION:
			{
				struct ncp_mount_data* md = (struct ncp_mount_data*)raw_data;

				data.flags = md->flags;
				data.int_flags = NCP_IMOUNT_LOGGEDIN_POSSIBLE;
				data.mounted_uid = md->mounted_uid;
				data.wdog_pid = md->wdog_pid;
				data.ncp_fd = md->ncp_fd;
				data.time_out = md->time_out;
				data.retry_count = md->retry_count;
				data.uid = md->uid;
				data.gid = md->gid;
				data.file_mode = md->file_mode;
				data.dir_mode = md->dir_mode;
				data.info_fd = -1;
				memcpy(data.mounted_vol, md->mounted_vol,
					NCP_VOLNAME_LEN+1);
			}
			break;
		case NCP_MOUNT_VERSION_V4:
			{
				struct ncp_mount_data_v4* md = (struct ncp_mount_data_v4*)raw_data;

				data.flags = md->flags;
				data.int_flags = 0;
				data.mounted_uid = md->mounted_uid;
				data.wdog_pid = md->wdog_pid;
				data.ncp_fd = md->ncp_fd;
				data.time_out = md->time_out;
				data.retry_count = md->retry_count;
				data.uid = md->uid;
				data.gid = md->gid;
				data.file_mode = md->file_mode;
				data.dir_mode = md->dir_mode;
				data.info_fd = -1;
				data.mounted_vol[0] = 0;
			}
			break;
		default:
			error = -ECHRNG;
			if (*(__u32*)raw_data == cpu_to_be32(0x76657273)) {
				error = ncp_parse_options(&data, raw_data);
			}
			if (error)
				goto out;
			break;
	}
	error = -EBADF;
	ncp_filp = fget(data.ncp_fd);
	if (!ncp_filp)
		goto out;
	error = -ENOTSOCK;
	sock_inode = ncp_filp->f_dentry->d_inode;
	if (!S_ISSOCK(sock_inode->i_mode))
		goto out_fput;
	sock = SOCKET_I(sock_inode);
	if (!sock)
		goto out_fput;
		
	if (sock->type == SOCK_STREAM)
		default_bufsize = 0xF000;
	else
		default_bufsize = 1024;

	sb->s_flags |= MS_NODIRATIME;	/* probably even noatime */
	sb->s_maxbytes = 0xFFFFFFFFU;
	sb->s_blocksize = 1024;	/* Eh...  Is this correct? */
	sb->s_blocksize_bits = 10;
	sb->s_magic = NCP_SUPER_MAGIC;
	sb->s_op = &ncp_sops;

	server = NCP_SBP(sb);
	memset(server, 0, sizeof(*server));

	server->ncp_filp = ncp_filp;
	server->ncp_sock = sock;
	
	if (data.info_fd != -1) {
		struct socket *info_sock;

		error = -EBADF;
		server->info_filp = fget(data.info_fd);
		if (!server->info_filp)
			goto out_fput;
		error = -ENOTSOCK;
		sock_inode = server->info_filp->f_dentry->d_inode;
		if (!S_ISSOCK(sock_inode->i_mode))
			goto out_fput2;
		info_sock = SOCKET_I(sock_inode);
		if (!info_sock)
			goto out_fput2;
		error = -EBADFD;
		if (info_sock->type != SOCK_STREAM)
			goto out_fput2;
		server->info_sock = info_sock;
	}

/*	server->lock = 0;	*/
	init_MUTEX(&server->sem);
	server->packet = NULL;
/*	server->buffer_size = 0;	*/
/*	server->conn_status = 0;	*/
/*	server->root_dentry = NULL;	*/
/*	server->root_setuped = 0;	*/
#ifdef CONFIG_NCPFS_PACKET_SIGNING
/*	server->sign_wanted = 0;	*/
/*	server->sign_active = 0;	*/
#endif
	server->auth.auth_type = NCP_AUTH_NONE;
/*	server->auth.object_name_len = 0;	*/
/*	server->auth.object_name = NULL;	*/
/*	server->auth.object_type = 0;		*/
/*	server->priv.len = 0;			*/
/*	server->priv.data = NULL;		*/

	server->m = data;
	/* Althought anything producing this is buggy, it happens
	   now because of PATH_MAX changes.. */
	if (server->m.time_out < 1) {
		server->m.time_out = 10;
		printk(KERN_INFO "You need to recompile your ncpfs utils..\n");
	}
	server->m.time_out = server->m.time_out * HZ / 100;
	server->m.file_mode = (server->m.file_mode & S_IRWXUGO) | S_IFREG;
	server->m.dir_mode = (server->m.dir_mode & S_IRWXUGO) | S_IFDIR;

#ifdef CONFIG_NCPFS_NLS
	/* load the default NLS charsets */
	server->nls_vol = load_nls_default();
	server->nls_io = load_nls_default();
#endif /* CONFIG_NCPFS_NLS */

	server->dentry_ttl = 0;	/* no caching */

	INIT_LIST_HEAD(&server->tx.requests);
	init_MUTEX(&server->rcv.creq_sem);
	server->tx.creq		= NULL;
	server->rcv.creq	= NULL;
	server->data_ready	= sock->sk->sk_data_ready;
	server->write_space	= sock->sk->sk_write_space;
	server->error_report	= sock->sk->sk_error_report;
	sock->sk->sk_user_data	= server;

	init_timer(&server->timeout_tm);
#undef NCP_PACKET_SIZE
#define NCP_PACKET_SIZE 131072
	error = -ENOMEM;
	server->packet_size = NCP_PACKET_SIZE;
	server->packet = vmalloc(NCP_PACKET_SIZE);
	if (server->packet == NULL)
		goto out_nls;

	sock->sk->sk_data_ready	  = ncp_tcp_data_ready;
	sock->sk->sk_error_report = ncp_tcp_error_report;
	if (sock->type == SOCK_STREAM) {
		server->rcv.ptr = (unsigned char*)&server->rcv.buf;
		server->rcv.len = 10;
		server->rcv.state = 0;
		INIT_WORK(&server->rcv.tq, ncp_tcp_rcv_proc, server);
		INIT_WORK(&server->tx.tq, ncp_tcp_tx_proc, server);
		sock->sk->sk_write_space = ncp_tcp_write_space;
	} else {
		INIT_WORK(&server->rcv.tq, ncpdgram_rcv_proc, server);
		INIT_WORK(&server->timeout_tq, ncpdgram_timeout_proc, server);
		server->timeout_tm.data = (unsigned long)server;
		server->timeout_tm.function = ncpdgram_timeout_call;
	}

	ncp_lock_server(server);
	error = ncp_connect(server);
	ncp_unlock_server(server);
	if (error < 0)
		goto out_packet;
	DPRINTK("ncp_fill_super: NCP_SBP(sb) = %x\n", (int) NCP_SBP(sb));

	error = -EMSGSIZE;	/* -EREMOTESIDEINCOMPATIBLE */
#ifdef CONFIG_NCPFS_PACKET_SIGNING
	if (ncp_negotiate_size_and_options(server, default_bufsize,
		NCP_DEFAULT_OPTIONS, &(server->buffer_size), &options) == 0)
	{
		if (options != NCP_DEFAULT_OPTIONS)
		{
			if (ncp_negotiate_size_and_options(server, 
				default_bufsize,
				options & 2, 
				&(server->buffer_size), &options) != 0)
				
			{
				goto out_disconnect;
			}
		}
		if (options & 2)
			server->sign_wanted = 1;
	}
	else 
#endif	/* CONFIG_NCPFS_PACKET_SIGNING */
	if (ncp_negotiate_buffersize(server, default_bufsize,
  				     &(server->buffer_size)) != 0)
		goto out_disconnect;
	DPRINTK("ncpfs: bufsize = %d\n", server->buffer_size);

	memset(&finfo, 0, sizeof(finfo));
	finfo.i.attributes	= aDIR;
	finfo.i.dataStreamSize	= NCP_BLOCK_SIZE;
	finfo.i.dirEntNum	= 0;
	finfo.i.DosDirNum	= 0;
#ifdef CONFIG_NCPFS_SMALLDOS
	finfo.i.NSCreator	= NW_NS_DOS;
#endif
	finfo.volume		= NCP_NUMBER_OF_VOLUMES;
	/* set dates of mountpoint to Jan 1, 1986; 00:00 */
	finfo.i.creationTime	= finfo.i.modifyTime
				= cpu_to_le16(0x0000);
	finfo.i.creationDate	= finfo.i.modifyDate
				= finfo.i.lastAccessDate
				= cpu_to_le16(0x0C21);
	finfo.i.nameLen		= 0;
	finfo.i.entryName[0]	= '\0';

	finfo.opened		= 0;
	finfo.ino		= 2;	/* tradition */

	server->name_space[finfo.volume] = NW_NS_DOS;

	error = -ENOMEM;
        root_inode = ncp_iget(sb, &finfo);
        if (!root_inode)
		goto out_disconnect;
	DPRINTK("ncp_fill_super: root vol=%d\n", NCP_FINFO(root_inode)->volNumber);
	sb->s_root = d_alloc_root(root_inode);
        if (!sb->s_root)
		goto out_no_root;
	sb->s_root->d_op = &ncp_root_dentry_operations;
	return 0;

out_no_root:
	iput(root_inode);
out_disconnect:
	ncp_lock_server(server);
	ncp_disconnect(server);
	ncp_unlock_server(server);
out_packet:
	ncp_stop_tasks(server);
	vfree(server->packet);
out_nls:
#ifdef CONFIG_NCPFS_NLS
	unload_nls(server->nls_io);
	unload_nls(server->nls_vol);
#endif
out_fput2:
	if (server->info_filp)
		fput(server->info_filp);
out_fput:
	/* 23/12/1998 Marcin Dalecki <dalecki@cs.net.pl>:
	 * 
	 * The previously used put_filp(ncp_filp); was bogous, since
	 * it doesn't proper unlocking.
	 */
	fput(ncp_filp);
out:
	sb->s_fs_info = NULL;
	kfree(server);
	return error;
}

static void ncp_put_super(struct super_block *sb)
{
	struct ncp_server *server = NCP_SBP(sb);

	ncp_lock_server(server);
	ncp_disconnect(server);
	ncp_unlock_server(server);

	ncp_stop_tasks(server);

#ifdef CONFIG_NCPFS_NLS
	/* unload the NLS charsets */
	if (server->nls_vol)
	{
		unload_nls(server->nls_vol);
		server->nls_vol = NULL;
	}
	if (server->nls_io)
	{
		unload_nls(server->nls_io);
		server->nls_io = NULL;
	}
#endif /* CONFIG_NCPFS_NLS */

	if (server->info_filp)
		fput(server->info_filp);
	fput(server->ncp_filp);
	kill_proc(server->m.wdog_pid, SIGTERM, 1);

	if (server->priv.data) 
		ncp_kfree_s(server->priv.data, server->priv.len);
	if (server->auth.object_name)
		ncp_kfree_s(server->auth.object_name, server->auth.object_name_len);
	vfree(server->packet);
	sb->s_fs_info = NULL;
	kfree(server);
}

static int ncp_statfs(struct super_block *sb, struct kstatfs *buf)
{
	struct dentry* d;
	struct inode* i;
	struct ncp_inode_info* ni;
	struct ncp_server* s;
	struct ncp_volume_info vi;
	int err;
	__u8 dh;
	
	d = sb->s_root;
	if (!d) {
		goto dflt;
	}
	i = d->d_inode;
	if (!i) {
		goto dflt;
	}
	ni = NCP_FINFO(i);
	if (!ni) {
		goto dflt;
	}
	s = NCP_SBP(sb);
	if (!s) {
		goto dflt;
	}
	if (!s->m.mounted_vol[0]) {
		goto dflt;
	}

	err = ncp_dirhandle_alloc(s, ni->volNumber, ni->DosDirNum, &dh);
	if (err) {
		goto dflt;
	}
	err = ncp_get_directory_info(s, dh, &vi);
	ncp_dirhandle_free(s, dh);
	if (err) {
		goto dflt;
	}
	buf->f_type = NCP_SUPER_MAGIC;
	buf->f_bsize = vi.sectors_per_block * 512;
	buf->f_blocks = vi.total_blocks;
	buf->f_bfree = vi.free_blocks;
	buf->f_bavail = vi.free_blocks;
	buf->f_files = vi.total_dir_entries;
	buf->f_ffree = vi.available_dir_entries;
	buf->f_namelen = 12;
	return 0;

	/* We cannot say how much disk space is left on a mounted
	   NetWare Server, because free space is distributed over
	   volumes, and the current user might have disk quotas. So
	   free space is not that simple to determine. Our decision
	   here is to err conservatively. */

dflt:;
	buf->f_type = NCP_SUPER_MAGIC;
	buf->f_bsize = NCP_BLOCK_SIZE;
	buf->f_blocks = 0;
	buf->f_bfree = 0;
	buf->f_bavail = 0;
	buf->f_namelen = 12;
	return 0;
}

int ncp_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	int result = 0;
	int info_mask;
	struct nw_modify_dos_info info;
	struct ncp_server *server;

	result = -EIO;

	lock_kernel();	

	server = NCP_SERVER(inode);
	if ((!server) || !ncp_conn_valid(server))
		goto out;

	/* ageing the dentry to force validation */
	ncp_age_dentry(server, dentry);

	result = inode_change_ok(inode, attr);
	if (result < 0)
		goto out;

	result = -EPERM;
	if (((attr->ia_valid & ATTR_UID) &&
	     (attr->ia_uid != server->m.uid)))
		goto out;

	if (((attr->ia_valid & ATTR_GID) &&
	     (attr->ia_gid != server->m.gid)))
		goto out;

	if (((attr->ia_valid & ATTR_MODE) &&
	     (attr->ia_mode &
	      ~(S_IFREG | S_IFDIR | S_IRWXUGO))))
		goto out;

	info_mask = 0;
	memset(&info, 0, sizeof(info));

#if 1 
        if ((attr->ia_valid & ATTR_MODE) != 0)
        {
		umode_t newmode = attr->ia_mode;

		info_mask |= DM_ATTRIBUTES;

                if (S_ISDIR(inode->i_mode)) {
                	newmode &= server->m.dir_mode;
		} else {
#ifdef CONFIG_NCPFS_EXTRAS			
			if (server->m.flags & NCP_MOUNT_EXTRAS) {
				/* any non-default execute bit set */
				if (newmode & ~server->m.file_mode & S_IXUGO)
					info.attributes |= aSHARED | aSYSTEM;
				/* read for group/world and not in default file_mode */
				else if (newmode & ~server->m.file_mode & S_IRUGO)
					info.attributes |= aSHARED;
			} else
#endif
				newmode &= server->m.file_mode;			
                }
                if (newmode & S_IWUGO)
                	info.attributes &= ~(aRONLY|aRENAMEINHIBIT|aDELETEINHIBIT);
                else
			info.attributes |=  (aRONLY|aRENAMEINHIBIT|aDELETEINHIBIT);

#ifdef CONFIG_NCPFS_NFS_NS
		if (ncp_is_nfs_extras(server, NCP_FINFO(inode)->volNumber)) {
			result = ncp_modify_nfs_info(server,
						     NCP_FINFO(inode)->volNumber,
						     NCP_FINFO(inode)->dirEntNum,
						     attr->ia_mode, 0);
			if (result != 0)
				goto out;
			info.attributes &= ~(aSHARED | aSYSTEM);
			{
				/* mark partial success */
				struct iattr tmpattr;
				
				tmpattr.ia_valid = ATTR_MODE;
				tmpattr.ia_mode = attr->ia_mode;

				result = inode_setattr(inode, &tmpattr);
				if (result)
					goto out;
			}
		}
#endif
        }
#endif

	/* Do SIZE before attributes, otherwise mtime together with size does not work...
	 */
	if ((attr->ia_valid & ATTR_SIZE) != 0) {
		int written;

		DPRINTK("ncpfs: trying to change size to %ld\n",
			attr->ia_size);

		if ((result = ncp_make_open(inode, O_WRONLY)) < 0) {
			result = -EACCES;
			goto out;
		}
		ncp_write_kernel(NCP_SERVER(inode), NCP_FINFO(inode)->file_handle,
			  attr->ia_size, 0, "", &written);

		/* According to ndir, the changes only take effect after
		   closing the file */
		ncp_inode_close(inode);
		result = ncp_make_closed(inode);
		if (result)
			goto out;
		{
			struct iattr tmpattr;
			
			tmpattr.ia_valid = ATTR_SIZE;
			tmpattr.ia_size = attr->ia_size;
			
			result = inode_setattr(inode, &tmpattr);
			if (result)
				goto out;
		}
	}
	if ((attr->ia_valid & ATTR_CTIME) != 0) {
		info_mask |= (DM_CREATE_TIME | DM_CREATE_DATE);
		ncp_date_unix2dos(attr->ia_ctime.tv_sec,
			     &(info.creationTime), &(info.creationDate));
		info.creationTime = le16_to_cpu(info.creationTime);
		info.creationDate = le16_to_cpu(info.creationDate);
	}
	if ((attr->ia_valid & ATTR_MTIME) != 0) {
		info_mask |= (DM_MODIFY_TIME | DM_MODIFY_DATE);
		ncp_date_unix2dos(attr->ia_mtime.tv_sec,
				  &(info.modifyTime), &(info.modifyDate));
		info.modifyTime = le16_to_cpu(info.modifyTime);
		info.modifyDate = le16_to_cpu(info.modifyDate);
	}
	if ((attr->ia_valid & ATTR_ATIME) != 0) {
		__u16 dummy;
		info_mask |= (DM_LAST_ACCESS_DATE);
		ncp_date_unix2dos(attr->ia_atime.tv_sec,
				  &(dummy), &(info.lastAccessDate));
		info.lastAccessDate = le16_to_cpu(info.lastAccessDate);
	}
	if (info_mask != 0) {
		result = ncp_modify_file_or_subdir_dos_info(NCP_SERVER(inode),
				      inode, info_mask, &info);
		if (result != 0) {
			result = -EACCES;

			if (info_mask == (DM_CREATE_TIME | DM_CREATE_DATE)) {
				/* NetWare seems not to allow this. I
				   do not know why. So, just tell the
				   user everything went fine. This is
				   a terrible hack, but I do not know
				   how to do this correctly. */
				result = 0;
			} else
				goto out;
		}
#ifdef CONFIG_NCPFS_STRONG		
		if ((!result) && (info_mask & DM_ATTRIBUTES))
			NCP_FINFO(inode)->nwattr = info.attributes;
#endif
	}
	if (!result)
		result = inode_setattr(inode, attr);
out:
	unlock_kernel();
	return result;
}

#ifdef DEBUG_NCP_MALLOC
int ncp_malloced;
int ncp_current_malloced;
#endif

static struct super_block *ncp_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return get_sb_nodev(fs_type, flags, data, ncp_fill_super);
}

static struct file_system_type ncp_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "ncpfs",
	.get_sb		= ncp_get_sb,
	.kill_sb	= kill_anon_super,
};

static int __init init_ncp_fs(void)
{
	int err;
	DPRINTK("ncpfs: init_module called\n");

#ifdef DEBUG_NCP_MALLOC
	ncp_malloced = 0;
	ncp_current_malloced = 0;
#endif
	err = init_inodecache();
	if (err)
		goto out1;
	err = register_filesystem(&ncp_fs_type);
	if (err)
		goto out;
	return 0;
out:
	destroy_inodecache();
out1:
	return err;
}

static void __exit exit_ncp_fs(void)
{
	DPRINTK("ncpfs: cleanup_module called\n");
	unregister_filesystem(&ncp_fs_type);
	destroy_inodecache();
#ifdef DEBUG_NCP_MALLOC
	PRINTK("ncp_malloced: %d\n", ncp_malloced);
	PRINTK("ncp_current_malloced: %d\n", ncp_current_malloced);
#endif
}

module_init(init_ncp_fs)
module_exit(exit_ncp_fs)
MODULE_LICENSE("GPL");
