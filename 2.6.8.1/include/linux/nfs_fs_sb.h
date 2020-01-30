#ifndef _NFS_FS_SB
#define _NFS_FS_SB

#include <linux/list.h>
#include <linux/backing-dev.h>

/*
 * NFS client parameters stored in the superblock.
 */
struct nfs_server {
	struct rpc_clnt *	client;		/* RPC client handle */
	struct rpc_clnt *	client_sys;	/* 2nd handle for FSINFO */
	struct nfs_rpc_ops *	rpc_ops;	/* NFS protocol vector */
	struct backing_dev_info	backing_dev_info;
	int			flags;		/* various flags */
	unsigned int		caps;		/* server capabilities */
	unsigned int		rsize;		/* read size */
	unsigned int		rpages;		/* read size (in pages) */
	unsigned int		wsize;		/* write size */
	unsigned int		wpages;		/* write size (in pages) */
	unsigned int		dtsize;		/* readdir size */
	unsigned int		bsize;		/* server block size */
	unsigned int		acregmin;	/* attr cache timeouts */
	unsigned int		acregmax;
	unsigned int		acdirmin;
	unsigned int		acdirmax;
	unsigned int		namelen;
	char *			hostname;	/* remote hostname */
	struct nfs_fh		fh;
	struct sockaddr_in	addr;
#ifdef CONFIG_NFS_V4
	/* Our own IP address, as a null-terminated string.
	 * This is used to generate the clientid, and the callback address.
	 */
	char			ip_addr[16];
	char *			mnt_path;
	struct nfs4_client *	nfs4_state;	/* all NFSv4 state starts here */
	struct list_head	nfs4_siblings;	/* List of other nfs_server structs
						 * that share the same clientid
						 */
	u32			attr_bitmask[2];/* V4 bitmask representing the set
						   of attributes supported on this
						   filesystem */
	u32			acl_bitmask;	/* V4 bitmask representing the ACEs
						   that are supported on this
						   filesystem */
#endif
};

/* Server capabilities */
#define NFS_CAP_READDIRPLUS	(1U << 0)
#define NFS_CAP_HARDLINKS	(1U << 1)
#define NFS_CAP_SYMLINKS	(1U << 2)
#define NFS_CAP_ACLS		(1U << 3)

#endif
