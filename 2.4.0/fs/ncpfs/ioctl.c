/*
 *  ioctl.c
 *
 *  Copyright (C) 1995, 1996 by Volker Lendecke
 *  Modified 1997 Peter Waltenberg, Bill Hawes, David Woodhouse for 2.1 dcache
 *  Modified 1998, 1999 Wolfram Pienkoss for NLS
 *
 */

#include <linux/config.h>

#include <asm/uaccess.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/highuid.h>
#include <linux/vmalloc.h>

#include <linux/ncp_fs.h>

#include "ncplib_kernel.h"

/* maximum limit for ncp_objectname_ioctl */
#define NCP_OBJECT_NAME_MAX_LEN	4096
/* maximum limit for ncp_privatedata_ioctl */
#define NCP_PRIVATE_DATA_MAX_LEN 8192
/* maximum negotiable packet size */
#define NCP_PACKET_SIZE_INTERNAL 65536

int ncp_ioctl(struct inode *inode, struct file *filp,
	      unsigned int cmd, unsigned long arg)
{
	struct ncp_server *server = NCP_SERVER(inode);
	int result;
	struct ncp_ioctl_request request;
	char* bouncebuffer;

	switch (cmd) {
	case NCP_IOC_NCPREQUEST:

		if ((permission(inode, MAY_WRITE) != 0)
		    && (current->uid != server->m.mounted_uid)) {
			return -EACCES;
		}
		if (copy_from_user(&request, (struct ncp_ioctl_request *) arg,
			       sizeof(request)))
			return -EFAULT;

		if ((request.function > 255)
		    || (request.size >
		  NCP_PACKET_SIZE - sizeof(struct ncp_request_header))) {
			return -EINVAL;
		}
		bouncebuffer = vmalloc(NCP_PACKET_SIZE_INTERNAL);
		if (!bouncebuffer)
			return -ENOMEM;
		if (copy_from_user(bouncebuffer, request.data, request.size)) {
			vfree(bouncebuffer);
			return -EFAULT;
		}
		ncp_lock_server(server);

		/* FIXME: We hack around in the server's structures
		   here to be able to use ncp_request */

		server->has_subfunction = 0;
		server->current_size = request.size;
		memcpy(server->packet, bouncebuffer, request.size);

		result = ncp_request2(server, request.function, 
			bouncebuffer, NCP_PACKET_SIZE_INTERNAL);
		if (result < 0)
			result = -EIO;
		else
			result = server->reply_size;
		ncp_unlock_server(server);
		DPRINTK("ncp_ioctl: copy %d bytes\n",
			result);
		if (result >= 0)
			if (copy_to_user(request.data, bouncebuffer, result))
				result = -EFAULT;
		vfree(bouncebuffer);
		return result;

	case NCP_IOC_CONN_LOGGED_IN:

		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (!(server->m.int_flags & NCP_IMOUNT_LOGGEDIN_POSSIBLE))
			return -EINVAL;
		if (server->root_setuped)
			return -EBUSY;
		server->root_setuped = 1;
		return ncp_conn_logged_in(inode->i_sb);

	case NCP_IOC_GET_FS_INFO:
		{
			struct ncp_fs_info info;

			if ((permission(inode, MAY_WRITE) != 0)
			    && (current->uid != server->m.mounted_uid)) {
				return -EACCES;
			}
			if (copy_from_user(&info, (struct ncp_fs_info *) arg, 
				sizeof(info)))
				return -EFAULT;

			if (info.version != NCP_GET_FS_INFO_VERSION) {
				DPRINTK("info.version invalid: %d\n", info.version);
				return -EINVAL;
			}
			/* TODO: info.addr = server->m.serv_addr; */
			info.mounted_uid	= NEW_TO_OLD_UID(server->m.mounted_uid);
			info.connection		= server->connection;
			info.buffer_size	= server->buffer_size;
			info.volume_number	= NCP_FINFO(inode)->volNumber;
			info.directory_id	= NCP_FINFO(inode)->DosDirNum;

			if (copy_to_user((struct ncp_fs_info *) arg, &info, 
				sizeof(info))) return -EFAULT;
			return 0;
		}

	case NCP_IOC_GET_FS_INFO_V2:
		{
			struct ncp_fs_info_v2 info2;

			if ((permission(inode, MAY_WRITE) != 0)
			    && (current->uid != server->m.mounted_uid)) {
				return -EACCES;
			}
			if (copy_from_user(&info2, (struct ncp_fs_info_v2 *) arg, 
				sizeof(info2)))
				return -EFAULT;

			if (info2.version != NCP_GET_FS_INFO_VERSION_V2) {
				DPRINTK("info.version invalid: %d\n", info2.version);
				return -EINVAL;
			}
			info2.mounted_uid   = server->m.mounted_uid;
			info2.connection    = server->connection;
			info2.buffer_size   = server->buffer_size;
			info2.volume_number = NCP_FINFO(inode)->volNumber;
			info2.directory_id  = NCP_FINFO(inode)->DosDirNum;
			info2.dummy1 = info2.dummy2 = info2.dummy3 = 0;

			if (copy_to_user((struct ncp_fs_info_v2 *) arg, &info2, 
				sizeof(info2))) return -EFAULT;
			return 0;
		}

	case NCP_IOC_GETMOUNTUID2:
		{
			unsigned long tmp = server->m.mounted_uid;

			if (   (permission(inode, MAY_READ) != 0)
			    && (current->uid != server->m.mounted_uid))
			{
				return -EACCES;
			}
			if (put_user(tmp, (unsigned long*) arg)) 
				return -EFAULT;
			return 0;
		}

	case NCP_IOC_GETROOT:
		{
			struct ncp_setroot_ioctl sr;

			if (   (permission(inode, MAY_READ) != 0)
			    && (current->uid != server->m.mounted_uid))
			{
				return -EACCES;
			}
			if (server->m.mounted_vol[0]) {
				struct dentry* dentry = inode->i_sb->s_root;

				if (dentry) {
					struct inode* inode = dentry->d_inode;
				
					if (inode) {
						sr.volNumber = NCP_FINFO(inode)->volNumber;
						sr.dirEntNum = NCP_FINFO(inode)->dirEntNum;
						sr.namespace = server->name_space[sr.volNumber];
					} else
						DPRINTK("ncpfs: s_root->d_inode==NULL\n");
				} else
					DPRINTK("ncpfs: s_root==NULL\n");
			} else {
				sr.volNumber = -1;
				sr.namespace = 0;
				sr.dirEntNum = 0;
			}
			if (copy_to_user((struct ncp_setroot_ioctl*)arg, 
				    	  &sr, 
					  sizeof(sr))) return -EFAULT;
			return 0;
		}
	case NCP_IOC_SETROOT:
		{
			struct ncp_setroot_ioctl sr;
			struct nw_info_struct i;
			struct dentry* dentry;

			if (!capable(CAP_SYS_ADMIN))
			{
				return -EACCES;
			}
			if (server->root_setuped) return -EBUSY;
			if (copy_from_user(&sr,
					   (struct ncp_setroot_ioctl*)arg, 
					   sizeof(sr))) return -EFAULT;
			if (sr.volNumber < 0) {
				server->m.mounted_vol[0] = 0;
				i.volNumber = NCP_NUMBER_OF_VOLUMES + 1;
				i.dirEntNum = 0;
				i.DosDirNum = 0;
			} else if (sr.volNumber >= NCP_NUMBER_OF_VOLUMES) {
				return -EINVAL;
			} else
				if (ncp_mount_subdir(server, &i, sr.volNumber,
						sr.namespace, sr.dirEntNum))
					return -ENOENT;

			dentry = inode->i_sb->s_root;
			server->root_setuped = 1;
			if (dentry) {
				struct inode* inode = dentry->d_inode;
				
				if (inode) {
					NCP_FINFO(inode)->volNumber = i.volNumber;
					NCP_FINFO(inode)->dirEntNum = i.dirEntNum;
					NCP_FINFO(inode)->DosDirNum = i.DosDirNum;
				} else
					DPRINTK("ncpfs: s_root->d_inode==NULL\n");
			} else
				DPRINTK("ncpfs: s_root==NULL\n");

			return 0;
		}

#ifdef CONFIG_NCPFS_PACKET_SIGNING	
	case NCP_IOC_SIGN_INIT:
		if ((permission(inode, MAY_WRITE) != 0)
		    && (current->uid != server->m.mounted_uid))
		{
			return -EACCES;
		}
		if (arg) {
			if (server->sign_wanted)
			{
				struct ncp_sign_init sign;

				if (copy_from_user(&sign, (struct ncp_sign_init *) arg,
				      sizeof(sign))) return -EFAULT;
				memcpy(server->sign_root,sign.sign_root,8);
				memcpy(server->sign_last,sign.sign_last,16);
				server->sign_active = 1;
			}
			/* ignore when signatures not wanted */
		} else {
			server->sign_active = 0;
		}
		return 0;		
		
        case NCP_IOC_SIGN_WANTED:
		if (   (permission(inode, MAY_READ) != 0)
		    && (current->uid != server->m.mounted_uid))
		{
			return -EACCES;
		}
		
                if (put_user(server->sign_wanted, (int*) arg))
			return -EFAULT;
                return 0;
	case NCP_IOC_SET_SIGN_WANTED:
		{
			int newstate;

			if (   (permission(inode, MAY_WRITE) != 0)
			    && (current->uid != server->m.mounted_uid))
			{
				return -EACCES;
			}
			/* get only low 8 bits... */
			if (get_user(newstate, (unsigned char *) arg))
				return -EFAULT;
			if (server->sign_active) {
				/* cannot turn signatures OFF when active */
				if (!newstate) return -EINVAL;
			} else {
				server->sign_wanted = newstate != 0;
			}
			return 0;
		}

#endif /* CONFIG_NCPFS_PACKET_SIGNING */

#ifdef CONFIG_NCPFS_IOCTL_LOCKING
	case NCP_IOC_LOCKUNLOCK:
		if (   (permission(inode, MAY_WRITE) != 0)
		    && (current->uid != server->m.mounted_uid))
		{
			return -EACCES;
		}
		{
			struct ncp_lock_ioctl	 rqdata;
			int result;

			if (copy_from_user(&rqdata, (struct ncp_lock_ioctl*)arg,
				sizeof(rqdata))) return -EFAULT;
			if (rqdata.origin != 0)
				return -EINVAL;
			/* check for cmd */
			switch (rqdata.cmd) {
				case NCP_LOCK_EX:
				case NCP_LOCK_SH:
						if (rqdata.timeout == 0)
							rqdata.timeout = NCP_LOCK_DEFAULT_TIMEOUT;
						else if (rqdata.timeout > NCP_LOCK_MAX_TIMEOUT)
							rqdata.timeout = NCP_LOCK_MAX_TIMEOUT;
						break;
				case NCP_LOCK_LOG:
						rqdata.timeout = NCP_LOCK_DEFAULT_TIMEOUT;	/* has no effect */
				case NCP_LOCK_CLEAR:
						break;
				default:
						return -EINVAL;
			}
			/* locking needs both read and write access */
			if ((result = ncp_make_open(inode, O_RDWR)) != 0)
			{
				return result;
			}
			result = -EIO;
			if (!ncp_conn_valid(server))
				goto outrel;
			result = -EISDIR;
			if (!S_ISREG(inode->i_mode))
				goto outrel;
			if (rqdata.cmd == NCP_LOCK_CLEAR)
			{
				result = ncp_ClearPhysicalRecord(NCP_SERVER(inode),
							NCP_FINFO(inode)->file_handle, 
							rqdata.offset,
							rqdata.length);
				if (result > 0) result = 0;	/* no such lock */
			}
			else
			{
				int lockcmd;

				switch (rqdata.cmd)
				{
					case NCP_LOCK_EX:  lockcmd=1; break;
					case NCP_LOCK_SH:  lockcmd=3; break;
					default:	   lockcmd=0; break;
				}
				result = ncp_LogPhysicalRecord(NCP_SERVER(inode),
							NCP_FINFO(inode)->file_handle,
							lockcmd,
							rqdata.offset,
							rqdata.length,
							rqdata.timeout);
				if (result > 0) result = -EAGAIN;
			}
outrel:			
			ncp_inode_close(inode);
			return result;
		}
#endif	/* CONFIG_NCPFS_IOCTL_LOCKING */

	case NCP_IOC_GETOBJECTNAME:
		if (current->uid != server->m.mounted_uid) {
			return -EACCES;
		}
		{
			struct ncp_objectname_ioctl user;
			int outl;

			if (copy_from_user(&user, 
					   (struct ncp_objectname_ioctl*)arg,
					   sizeof(user))) return -EFAULT;
			user.auth_type = server->auth.auth_type;
			outl = user.object_name_len;
			user.object_name_len = server->auth.object_name_len;
			if (outl > user.object_name_len)
				outl = user.object_name_len;
			if (outl) {
				if (copy_to_user(user.object_name,
						 server->auth.object_name,
						 outl)) return -EFAULT;
			}
			if (copy_to_user((struct ncp_objectname_ioctl*)arg,
					 &user,
					 sizeof(user))) return -EFAULT;
			return 0;
		}
	case NCP_IOC_SETOBJECTNAME:
		if (current->uid != server->m.mounted_uid) {
			return -EACCES;
		}
		{
			struct ncp_objectname_ioctl user;
			void* newname;
			void* oldname;
			size_t oldnamelen;
			void* oldprivate;
			size_t oldprivatelen;

			if (copy_from_user(&user, 
					   (struct ncp_objectname_ioctl*)arg,
					   sizeof(user))) return -EFAULT;
			if (user.object_name_len > NCP_OBJECT_NAME_MAX_LEN)
				return -ENOMEM;
			if (user.object_name_len) {
				newname = ncp_kmalloc(user.object_name_len, GFP_USER);
				if (!newname) return -ENOMEM;
				if (copy_from_user(newname, user.object_name, sizeof(user))) {
					ncp_kfree_s(newname, user.object_name_len);
					return -EFAULT;
				}
			} else {
				newname = NULL;
			}
			/* enter critical section */
			/* maybe that kfree can sleep so do that this way */
			/* it is at least more SMP friendly (in future...) */
			oldname = server->auth.object_name;
			oldnamelen = server->auth.object_name_len;
			oldprivate = server->priv.data;
			oldprivatelen = server->priv.len;
			server->auth.auth_type = user.auth_type;
			server->auth.object_name_len = user.object_name_len;
			server->auth.object_name = user.object_name;
			server->priv.len = 0;
			server->priv.data = NULL;
			/* leave critical section */
			if (oldprivate) ncp_kfree_s(oldprivate, oldprivatelen);
			if (oldname) ncp_kfree_s(oldname, oldnamelen);
			return 0;
		}
	case NCP_IOC_GETPRIVATEDATA:
		if (current->uid != server->m.mounted_uid) {
			return -EACCES;
		}
		{
			struct ncp_privatedata_ioctl user;
			int outl;

			if (copy_from_user(&user, 
					   (struct ncp_privatedata_ioctl*)arg,
					   sizeof(user))) return -EFAULT;
			outl = user.len;
			user.len = server->priv.len;
			if (outl > user.len) outl = user.len;
			if (outl) {
				if (copy_to_user(user.data,
						 server->priv.data,
						 outl)) return -EFAULT;
			}
			if (copy_to_user((struct ncp_privatedata_ioctl*)arg,
					 &user,
					 sizeof(user))) return -EFAULT;
			return 0;
		}
	case NCP_IOC_SETPRIVATEDATA:
		if (current->uid != server->m.mounted_uid) {
			return -EACCES;
		}
		{
			struct ncp_privatedata_ioctl user;
			void* new;
			void* old;
			size_t oldlen;

			if (copy_from_user(&user, 
					   (struct ncp_privatedata_ioctl*)arg,
					   sizeof(user))) return -EFAULT;
			if (user.len > NCP_PRIVATE_DATA_MAX_LEN)
				return -ENOMEM;
			if (user.len) {
				new = ncp_kmalloc(user.len, GFP_USER);
				if (!new) return -ENOMEM;
				if (copy_from_user(new, user.data, user.len)) {
					ncp_kfree_s(new, user.len);
					return -EFAULT;
				}
			} else {
				new = NULL;
			}
			/* enter critical section */
			old = server->priv.data;
			oldlen = server->priv.len;
			server->priv.len = user.len;
			server->priv.data = new;
			/* leave critical section */
			if (old) ncp_kfree_s(old, oldlen);
			return 0;
		}

#ifdef CONFIG_NCPFS_NLS
/* Here we are select the iocharset and the codepage for NLS.
 * Thanks Petr Vandrovec for idea and many hints.
 */
	case NCP_IOC_SETCHARSETS:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (server->root_setuped)
			return -EBUSY;

		{
			struct ncp_nls_ioctl user;
			struct nls_table *codepage;
			struct nls_table *iocharset;
			struct nls_table *oldset_io;
			struct nls_table *oldset_cp;
			
			if (copy_from_user(&user, (struct ncp_nls_ioctl*)arg,
					sizeof(user)))
				return -EFAULT;

			codepage = NULL;
			user.codepage[NCP_IOCSNAME_LEN] = 0;
			if (!user.codepage[0] ||
					!strcmp(user.codepage, "default"))
				codepage = load_nls_default();
			else {
				codepage = load_nls(user.codepage);
				if (!codepage) {
					return -EBADRQC;
				}
			}

			iocharset = NULL;
			user.iocharset[NCP_IOCSNAME_LEN] = 0;
			if (!user.iocharset[0] ||
					!strcmp(user.iocharset, "default")) {
				iocharset = load_nls_default();
				NCP_CLR_FLAG(server, NCP_FLAG_UTF8);
			} else {
				if (!strcmp(user.iocharset, "utf8")) {
					iocharset = load_nls_default();
					NCP_SET_FLAG(server, NCP_FLAG_UTF8);
				} else {
					iocharset = load_nls(user.iocharset);
					if (!iocharset) {
						unload_nls(codepage);
						return -EBADRQC;
					}
					NCP_CLR_FLAG(server, NCP_FLAG_UTF8);
				}
			}

			oldset_cp = server->nls_vol;
			server->nls_vol = codepage;
			oldset_io = server->nls_io;
			server->nls_io = iocharset;

			if (oldset_cp)
				unload_nls(oldset_cp);
			if (oldset_io)
				unload_nls(oldset_io);

			return 0;
		}
		
	case NCP_IOC_GETCHARSETS: /* not tested */
		{
			struct ncp_nls_ioctl user;
			int len;

			memset(&user, 0, sizeof(user));
			if (server->nls_vol && server->nls_vol->charset) {
				len = strlen(server->nls_vol->charset);
				if (len > NCP_IOCSNAME_LEN)
					len = NCP_IOCSNAME_LEN;
				strncpy(user.codepage,
						server->nls_vol->charset, len);
				user.codepage[len] = 0;
			}

			if (NCP_IS_FLAG(server, NCP_FLAG_UTF8))
				strcpy(user.iocharset, "utf8");
			else
				if (server->nls_io && server->nls_io->charset) {
					len = strlen(server->nls_io->charset);
					if (len > NCP_IOCSNAME_LEN)
						len = NCP_IOCSNAME_LEN;
					strncpy(user.iocharset,
						server->nls_io->charset, len);
					user.iocharset[len] = 0;
				}

			if (copy_to_user((struct ncp_nls_ioctl*)arg, &user,
					sizeof(user)))
				return -EFAULT;

			return 0;
		}
#endif /* CONFIG_NCPFS_NLS */
	case NCP_IOC_SETDENTRYTTL:
		if ((permission(inode, MAY_WRITE) != 0) &&
				 (current->uid != server->m.mounted_uid))
			return -EACCES;
		{
			u_int32_t user;

			if (copy_from_user(&user, (u_int32_t*)arg, sizeof(user)))
				return -EFAULT;
			/* 20 secs at most... */
			if (user > 20000)
				return -EINVAL;
			user = (user * HZ) / 1000;
			server->dentry_ttl = user;
			return 0;
		}
		
	case NCP_IOC_GETDENTRYTTL:
		{
			u_int32_t user = (server->dentry_ttl * 1000) / HZ;
			if (copy_to_user((u_int32_t*)arg, &user, sizeof(user)))
				return -EFAULT;
			return 0;
		}

	}
/* #ifdef CONFIG_UID16 */
	/* NCP_IOC_GETMOUNTUID may be same as NCP_IOC_GETMOUNTUID2,
           so we have this out of switch */
	if (cmd == NCP_IOC_GETMOUNTUID) {
		if ((permission(inode, MAY_READ) != 0)
		    && (current->uid != server->m.mounted_uid)) {
			return -EACCES;
		}
		if (put_user(NEW_TO_OLD_UID(server->m.mounted_uid), (__kernel_uid_t *) arg))
			return -EFAULT;
		return 0;
	}
/* #endif */
	return -EINVAL;
}
