/*
 *   fs/cifs/inode.c
 *
 *   Copyright (C) International Business Machines  Corp., 2002,2003
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/stat.h>
#include <linux/pagemap.h>
#include <asm/div64.h>
#include "cifsfs.h"
#include "cifspdu.h"
#include "cifsglob.h"
#include "cifsproto.h"
#include "cifs_debug.h"
#include "cifs_fs_sb.h"

extern int is_size_safe_to_change(struct cifsInodeInfo *);

int
cifs_get_inode_info_unix(struct inode **pinode,
			 const unsigned char *search_path,
			 struct super_block *sb,int xid)
{
	int rc = 0;
	FILE_UNIX_BASIC_INFO findData;
	struct cifsTconInfo *pTcon;
	struct inode *inode;
	struct cifs_sb_info *cifs_sb = CIFS_SB(sb);
	char *tmp_path;

	pTcon = cifs_sb->tcon;
	cFYI(1, (" Getting info on %s ", search_path));
	/* we could have done a find first instead but this returns more info */
	rc = CIFSSMBUnixQPathInfo(xid, pTcon, search_path, &findData,
				  cifs_sb->local_nls);
	/* dump_mem("\nUnixQPathInfo return data", &findData, sizeof(findData)); */
	if (rc) {
		if (rc == -EREMOTE) {
			tmp_path =
			    kmalloc(strnlen
				    (pTcon->treeName,
				     MAX_TREE_SIZE + 1) +
				    strnlen(search_path, MAX_PATHCONF) + 1,
				    GFP_KERNEL);
			if (tmp_path == NULL) {
				return -ENOMEM;
			}
        /* have to skip first of the double backslash of UNC name */
			strncpy(tmp_path, pTcon->treeName, MAX_TREE_SIZE);	
			strncat(tmp_path, search_path, MAX_PATHCONF);
			rc = connect_to_dfs_path(xid, pTcon->ses,
						 /* treename + */ tmp_path,
						 cifs_sb->local_nls);
			kfree(tmp_path);

			/* BB fix up inode etc. */
		} else if (rc) {
			return rc;
		}

	} else {
		struct cifsInodeInfo *cifsInfo;

		/* get new inode */
		if (*pinode == NULL) {
			*pinode = new_inode(sb);
			if(*pinode == NULL) 
				return -ENOMEM;
			insert_inode_hash(*pinode);
		}
			
		inode = *pinode;
		cifsInfo = CIFS_I(inode);

		cFYI(1, (" Old time %ld ", cifsInfo->time));
		cifsInfo->time = jiffies;
		cFYI(1, (" New time %ld ", cifsInfo->time));
		atomic_set(&cifsInfo->inUse,1);	/* ok to set on every refresh of inode */

		inode->i_atime =
		    cifs_NTtimeToUnix(le64_to_cpu(findData.LastAccessTime));
		inode->i_mtime =
		    cifs_NTtimeToUnix(le64_to_cpu
				(findData.LastModificationTime));
		inode->i_ctime =
		    cifs_NTtimeToUnix(le64_to_cpu(findData.LastStatusChange));
		inode->i_mode = le64_to_cpu(findData.Permissions);
		findData.Type = le32_to_cpu(findData.Type);
		if (findData.Type == UNIX_FILE) {
			inode->i_mode |= S_IFREG;
		} else if (findData.Type == UNIX_SYMLINK) {
			inode->i_mode |= S_IFLNK;
		} else if (findData.Type == UNIX_DIR) {
			inode->i_mode |= S_IFDIR;
		} else if (findData.Type == UNIX_CHARDEV) {
			inode->i_mode |= S_IFCHR;
			inode->i_rdev = MKDEV(le64_to_cpu(findData.DevMajor),
				le64_to_cpu(findData.DevMinor) & MINORMASK);
		} else if (findData.Type == UNIX_BLOCKDEV) {
			inode->i_mode |= S_IFBLK;
			inode->i_rdev = MKDEV(le64_to_cpu(findData.DevMajor),
				le64_to_cpu(findData.DevMinor) & MINORMASK);
		} else if (findData.Type == UNIX_FIFO) {
			inode->i_mode |= S_IFIFO;
		} else if (findData.Type == UNIX_SOCKET) {
			inode->i_mode |= S_IFSOCK;
		}
		inode->i_uid = le64_to_cpu(findData.Uid);
		inode->i_gid = le64_to_cpu(findData.Gid);
		inode->i_nlink = le64_to_cpu(findData.Nlinks);
		findData.NumOfBytes = le64_to_cpu(findData.NumOfBytes);
		findData.EndOfFile = le64_to_cpu(findData.EndOfFile);

		if(is_size_safe_to_change(cifsInfo)) {
		/* can not safely change the file size here if the 
		   client is writing to it due to potential races */

			i_size_write(inode,findData.EndOfFile);
/* blksize needs to be multiple of two. So safer to default to blksize
	and blkbits set in superblock so 2**blkbits and blksize will match */
/*		inode->i_blksize =
		    (pTcon->ses->server->maxBuf - MAX_CIFS_HDR_SIZE) & 0xFFFFFE00;*/

		/* This seems incredibly stupid but it turns out that
		i_blocks is not related to (i_size / i_blksize), instead a
		size of 512 is required to be used for calculating num blocks */
		 

/*		inode->i_blocks = 
	                (inode->i_blksize - 1 + findData.NumOfBytes) >> inode->i_blkbits;*/

		/* 512 bytes (2**9) is the fake blocksize that must be used */
		/* for this calculation */
			inode->i_blocks = (512 - 1 + findData.NumOfBytes) >> 9;
		}

		if (findData.NumOfBytes < findData.EndOfFile)
			cFYI(1, ("Server inconsistency Error: it says allocation size less than end of file "));
		cFYI(1,
		     ("Size %ld and blocks %ld ",
		      (unsigned long) inode->i_size, inode->i_blocks));
		if (S_ISREG(inode->i_mode)) {
			cFYI(1, (" File inode "));
			inode->i_op = &cifs_file_inode_ops;
			inode->i_fop = &cifs_file_ops;
			inode->i_data.a_ops = &cifs_addr_ops;
		} else if (S_ISDIR(inode->i_mode)) {
			cFYI(1, (" Directory inode"));
			inode->i_op = &cifs_dir_inode_ops;
			inode->i_fop = &cifs_dir_ops;
		} else if (S_ISLNK(inode->i_mode)) {
			cFYI(1, (" Symbolic Link inode "));
			inode->i_op = &cifs_symlink_inode_ops;
/* tmp_inode->i_fop = *//* do not need to set to anything */
		} else {
			cFYI(1, (" Init special inode "));
			init_special_inode(inode, inode->i_mode,
					   inode->i_rdev);
		}
	}
	return rc;
}

int
cifs_get_inode_info(struct inode **pinode, const unsigned char *search_path, 
		FILE_ALL_INFO * pfindData, struct super_block *sb, int xid)
{
	int rc = 0;
	struct cifsTconInfo *pTcon;
	struct inode *inode;
	struct cifs_sb_info *cifs_sb = CIFS_SB(sb);
	char *tmp_path;
	char *buf = NULL;

	pTcon = cifs_sb->tcon;
	cFYI(1,("Getting info on %s ", search_path));

	if((pfindData == NULL) && (*pinode != NULL)) {
		if(CIFS_I(*pinode)->clientCanCacheRead) {
			cFYI(1,("No need to revalidate inode sizes on cached file "));
			return rc;
		}
	}

	/* if file info not passed in then get it from server */
	if(pfindData == NULL) {
		buf = kmalloc(sizeof(FILE_ALL_INFO),GFP_KERNEL);
		pfindData = (FILE_ALL_INFO *)buf;
	/* could do find first instead but this returns more info */
		rc = CIFSSMBQPathInfo(xid, pTcon, search_path, pfindData,
			      cifs_sb->local_nls);
	}
	/* dump_mem("\nQPathInfo return data",&findData, sizeof(findData)); */
	if (rc) {
		if (rc == -EREMOTE) {
			tmp_path =
			    kmalloc(strnlen
				    (pTcon->treeName,
				     MAX_TREE_SIZE + 1) +
				    strnlen(search_path, MAX_PATHCONF) + 1,
				    GFP_KERNEL);
			if (tmp_path == NULL) {
				if(buf)
					kfree(buf);
				return -ENOMEM;
			}

			strncpy(tmp_path, pTcon->treeName, MAX_TREE_SIZE);
			strncat(tmp_path, search_path, MAX_PATHCONF);
			rc = connect_to_dfs_path(xid, pTcon->ses,
						 /* treename + */ tmp_path,
						 cifs_sb->local_nls);
			kfree(tmp_path);
			/* BB fix up inode etc. */
		} else if (rc) {
			if(buf)
				kfree(buf);
			return rc;
		}
	} else {
		struct cifsInodeInfo *cifsInfo;

		/* get new inode */
		if (*pinode == NULL) {
			*pinode = new_inode(sb);
			if(*pinode == NULL)
				return -ENOMEM;
			insert_inode_hash(*pinode);
		}
		inode = *pinode;
		cifsInfo = CIFS_I(inode);
		pfindData->Attributes = le32_to_cpu(pfindData->Attributes);
		cifsInfo->cifsAttrs = pfindData->Attributes;
		cFYI(1, (" Old time %ld ", cifsInfo->time));
		cifsInfo->time = jiffies;
		cFYI(1, (" New time %ld ", cifsInfo->time));

/* blksize needs to be multiple of two. So safer to default to blksize
        and blkbits set in superblock so 2**blkbits and blksize will match */
/*		inode->i_blksize =
		    (pTcon->ses->server->maxBuf - MAX_CIFS_HDR_SIZE) & 0xFFFFFE00;*/

		/* Linux can not store file creation time unfortunately so we ignore it */
		inode->i_atime =
		    cifs_NTtimeToUnix(le64_to_cpu(pfindData->LastAccessTime));
		inode->i_mtime =
		    cifs_NTtimeToUnix(le64_to_cpu(pfindData->LastWriteTime));
		inode->i_ctime =
		    cifs_NTtimeToUnix(le64_to_cpu(pfindData->ChangeTime));
		cFYI(0,
		     (" Attributes came in as 0x%x ", pfindData->Attributes));

		/* set default mode. will override for dirs below */
		if(atomic_read(&cifsInfo->inUse) == 0)
			/* new inode, can safely set these fields */
			inode->i_mode = cifs_sb->mnt_file_mode;

		if (pfindData->Attributes & ATTR_REPARSE) {
	/* Can IFLNK be set as it basically is on windows with IFREG or IFDIR? */
			inode->i_mode |= S_IFLNK;
		} else if (pfindData->Attributes & ATTR_DIRECTORY) {
	/* override default perms since we do not do byte range locking on dirs */
			inode->i_mode = cifs_sb->mnt_dir_mode;
			inode->i_mode |= S_IFDIR;
		} else {
			inode->i_mode |= S_IFREG;
			/* treat the dos attribute of read-only as read-only mode e.g. 555 */
			if(cifsInfo->cifsAttrs & ATTR_READONLY)
				inode->i_mode &= ~(S_IWUGO);
   /* BB add code here - validate if device or weird share or device type? */
		}
		if(is_size_safe_to_change(cifsInfo)) {
		/* can not safely change the file size here if the 
		client is writing to it due to potential races */

			i_size_write(inode,le64_to_cpu(pfindData->EndOfFile));

		/* 512 bytes (2**9) is the fake blocksize that must be used */
		/* for this calculation */
			inode->i_blocks = (512 - 1 + pfindData->AllocationSize)
				 >> 9;
		}
		pfindData->AllocationSize = le64_to_cpu(pfindData->AllocationSize);

		inode->i_nlink = le32_to_cpu(pfindData->NumberOfLinks);

		/* BB fill in uid and gid here? with help from winbind? 
			or retrieve from NTFS stream extended attribute */
		if(atomic_read(&cifsInfo->inUse) == 0) {
			inode->i_uid = cifs_sb->mnt_uid;
			inode->i_gid = cifs_sb->mnt_gid;
			/* set so we do not keep refreshing these fields with
			bad data after user has changed them in memory */
			atomic_set(&cifsInfo->inUse,1);
		}
		
		if (S_ISREG(inode->i_mode)) {
			cFYI(1, (" File inode "));
			inode->i_op = &cifs_file_inode_ops;
			inode->i_fop = &cifs_file_ops;
			inode->i_data.a_ops = &cifs_addr_ops;
		} else if (S_ISDIR(inode->i_mode)) {
			cFYI(1, (" Directory inode "));
			inode->i_op = &cifs_dir_inode_ops;
			inode->i_fop = &cifs_dir_ops;
		} else if (S_ISLNK(inode->i_mode)) {
			cFYI(1, (" Symbolic Link inode "));
			inode->i_op = &cifs_symlink_inode_ops;
		} else {
			init_special_inode(inode, inode->i_mode,
					   inode->i_rdev);
		}
	}
	if(buf)
	    kfree(buf);
	return rc;
}

void
cifs_read_inode(struct inode *inode)
{				/* gets root inode */
	int xid;
	struct cifs_sb_info *cifs_sb;

	cifs_sb = CIFS_SB(inode->i_sb);
	xid = GetXid();
	if (cifs_sb->tcon->ses->capabilities & CAP_UNIX)
		cifs_get_inode_info_unix(&inode, "", inode->i_sb,xid);
	else
		cifs_get_inode_info(&inode, "", NULL, inode->i_sb,xid);
	/* can not call macro FreeXid here since in a void func */
	_FreeXid(xid);
}

int
cifs_unlink(struct inode *inode, struct dentry *direntry)
{
	int rc = 0;
	int xid;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	char *full_path = NULL;
	struct cifsInodeInfo *cifsInode;
	FILE_BASIC_INFO * pinfo_buf;

	cFYI(1, (" cifs_unlink, inode = 0x%p with ", inode));

	xid = GetXid();

	cifs_sb = CIFS_SB(inode->i_sb);
	pTcon = cifs_sb->tcon;

/* Unlink can be called from rename so we can not grab
	the sem here since we deadlock otherwise */
/*	down(&direntry->d_sb->s_vfs_rename_sem);*/
	full_path = build_path_from_dentry(direntry);
/*	up(&direntry->d_sb->s_vfs_rename_sem);*/
	if(full_path == NULL) {
		FreeXid(xid);
		return -ENOMEM;
	}
	rc = CIFSSMBDelFile(xid, pTcon, full_path, cifs_sb->local_nls);

	if (!rc) {
		direntry->d_inode->i_nlink--;
	} else if (rc == -ENOENT) {
		d_drop(direntry);
	} else if (rc == -ETXTBSY) {
		int oplock = FALSE;
		__u16 netfid;

		rc = CIFSSMBOpen(xid, pTcon, full_path, FILE_OPEN, DELETE, 
				CREATE_NOT_DIR | CREATE_DELETE_ON_CLOSE,
				&netfid, &oplock, NULL, cifs_sb->local_nls);
		if(rc==0) {
			CIFSSMBRenameOpenFile(xid,pTcon,netfid,
				NULL, cifs_sb->local_nls);
			CIFSSMBClose(xid, pTcon, netfid);
			direntry->d_inode->i_nlink--;
		}
	} else if (rc == -EACCES) {
		/* try only if r/o attribute set in local lookup data? */
		pinfo_buf = (FILE_BASIC_INFO *)kmalloc(sizeof(FILE_BASIC_INFO),GFP_KERNEL);
		if(pinfo_buf) {
			memset(pinfo_buf,0,sizeof(FILE_BASIC_INFO));        
		/* ATTRS set to normal clears r/o bit */
			pinfo_buf->Attributes = cpu_to_le32(ATTR_NORMAL);
			rc = CIFSSMBSetTimes(xid, pTcon, full_path, pinfo_buf,
				cifs_sb->local_nls);
			kfree(pinfo_buf);
		}
		if(rc==0) {
			rc = CIFSSMBDelFile(xid, pTcon, full_path, cifs_sb->local_nls);
			if (!rc) {
				direntry->d_inode->i_nlink--;
			} else if (rc == -ETXTBSY) {
				int oplock = FALSE;
				__u16 netfid;

				rc = CIFSSMBOpen(xid, pTcon, full_path, FILE_OPEN, DELETE,
						CREATE_NOT_DIR | CREATE_DELETE_ON_CLOSE,
						&netfid, &oplock, NULL, cifs_sb->local_nls);
				if(rc==0) {
					CIFSSMBRenameOpenFile(xid,pTcon,netfid,NULL,cifs_sb->local_nls);
					CIFSSMBClose(xid, pTcon, netfid);
		                        direntry->d_inode->i_nlink--;
				}
			/* BB if rc = -ETXTBUSY goto the rename logic BB */
			}
		}
	}
	cifsInode = CIFS_I(direntry->d_inode);
	cifsInode->time = 0;	/* will force revalidate to get info when needed */
	direntry->d_inode->i_ctime = inode->i_ctime = inode->i_mtime =
	    CURRENT_TIME;
	cifsInode = CIFS_I(inode);
	cifsInode->time = 0;	/* force revalidate of dir as well */

	if (full_path)
		kfree(full_path);
	FreeXid(xid);
	return rc;
}

int
cifs_mkdir(struct inode *inode, struct dentry *direntry, int mode)
{
	int rc = 0;
	int xid;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	char *full_path = NULL;
	struct inode *newinode = NULL;

	cFYI(1, ("In cifs_mkdir, mode = 0x%x inode = 0x%p ", mode, inode));

	xid = GetXid();

	cifs_sb = CIFS_SB(inode->i_sb);
	pTcon = cifs_sb->tcon;

	down(&inode->i_sb->s_vfs_rename_sem);
	full_path = build_path_from_dentry(direntry);
	up(&inode->i_sb->s_vfs_rename_sem);
	if(full_path == NULL) {
		FreeXid(xid);
		return -ENOMEM;
	}
	/* BB add setting the equivalent of mode via CreateX w/ACLs */
	rc = CIFSSMBMkDir(xid, pTcon, full_path, cifs_sb->local_nls);
	if (rc) {
		cFYI(1, ("cifs_mkdir returned 0x%x ", rc));
		d_drop(direntry);
	} else {
		inode->i_nlink++;
		if (pTcon->ses->capabilities & CAP_UNIX)
			rc = cifs_get_inode_info_unix(&newinode, full_path,
						      inode->i_sb,xid);
		else
			rc = cifs_get_inode_info(&newinode, full_path,NULL,
						 inode->i_sb,xid);

		direntry->d_op = &cifs_dentry_ops;
		d_instantiate(direntry, newinode);
		if(direntry->d_inode)
			direntry->d_inode->i_nlink = 2;
		if (cifs_sb->tcon->ses->capabilities & CAP_UNIX)
			if(cifs_sb->mnt_cifs_flags & CIFS_MOUNT_SET_UID) {
				CIFSSMBUnixSetPerms(xid, pTcon, full_path, mode,
						(__u64)current->euid,  
						(__u64)current->egid,
						0 /* dev_t */,
						cifs_sb->local_nls);
			} else {
				CIFSSMBUnixSetPerms(xid, pTcon, full_path, mode,
						(__u64)-1,  
						(__u64)-1,
						0 /* dev_t */,
						cifs_sb->local_nls);
			}
		else { /* BB to be implemented via Windows secrty descriptors*/
		/* eg CIFSSMBWinSetPerms(xid,pTcon,full_path,mode,-1,-1,local_nls);*/
		}
	}
	if (full_path)
		kfree(full_path);
	FreeXid(xid);

	return rc;
}

int
cifs_rmdir(struct inode *inode, struct dentry *direntry)
{
	int rc = 0;
	int xid;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	char *full_path = NULL;
	struct cifsInodeInfo *cifsInode;

	cFYI(1, (" cifs_rmdir, inode = 0x%p with ", inode));

	xid = GetXid();

	cifs_sb = CIFS_SB(inode->i_sb);
	pTcon = cifs_sb->tcon;

	down(&inode->i_sb->s_vfs_rename_sem);
	full_path = build_path_from_dentry(direntry);
	up(&inode->i_sb->s_vfs_rename_sem);
	if(full_path == NULL) {
		FreeXid(xid);
		return -ENOMEM;
	}

	rc = CIFSSMBRmDir(xid, pTcon, full_path, cifs_sb->local_nls);

	if (!rc) {
		inode->i_nlink--;
		i_size_write(direntry->d_inode,0);
		direntry->d_inode->i_nlink = 0;
	}

	cifsInode = CIFS_I(direntry->d_inode);
	cifsInode->time = 0;	/* force revalidate to go get info when needed */
	direntry->d_inode->i_ctime = inode->i_ctime = inode->i_mtime =
	    CURRENT_TIME;

	if (full_path)
		kfree(full_path);
	FreeXid(xid);
	return rc;
}

int
cifs_rename(struct inode *source_inode, struct dentry *source_direntry,
	    struct inode *target_inode, struct dentry *target_direntry)
{
	char *fromName;
	char *toName;
	struct cifs_sb_info *cifs_sb_source;
	struct cifs_sb_info *cifs_sb_target;
	struct cifsTconInfo *pTcon;
	int xid;
	int rc = 0;

	xid = GetXid();

	cifs_sb_target = CIFS_SB(target_inode->i_sb);
	cifs_sb_source = CIFS_SB(source_inode->i_sb);
	pTcon = cifs_sb_source->tcon;

	if (pTcon != cifs_sb_target->tcon) {
		FreeXid(xid);    
		return -EXDEV;	/* BB actually could be allowed if same server, but
                     different share. Might eventually add support for this */
	}

	/* we already  have the rename sem so we do not need
	to grab it again here to protect the path integrity */
	fromName = build_path_from_dentry(source_direntry);
	toName = build_path_from_dentry(target_direntry);
	if((fromName == NULL) || (toName == NULL)) {
		rc = -ENOMEM;
		goto cifs_rename_exit;
	}

	rc = CIFSSMBRename(xid, pTcon, fromName, toName,
			   cifs_sb_source->local_nls);
	if(rc == -EEXIST) {
		/* check if they are the same file 
		because rename of hardlinked files is a noop */
		FILE_UNIX_BASIC_INFO * info_buf_source;
		FILE_UNIX_BASIC_INFO * info_buf_target;

		info_buf_source = 
			kmalloc(2 * sizeof(FILE_UNIX_BASIC_INFO),GFP_KERNEL);
		if(info_buf_source != NULL) {
			info_buf_target = info_buf_source+1;
			rc = CIFSSMBUnixQPathInfo(xid, pTcon, fromName, 
				info_buf_source, cifs_sb_source->local_nls);
			if(rc == 0) {
				rc = CIFSSMBUnixQPathInfo(xid,pTcon,toName,
						info_buf_target,
						cifs_sb_target->local_nls);
			}
			if((rc == 0) && 
				(info_buf_source->UniqueId == 
				 info_buf_target->UniqueId)) {
			/* do not rename since the files are hardlinked 
			   which is a noop */
			} else {
			/* we either can not tell the files are hardlinked
			(as with Windows servers) or files are not hardlinked 
			so delete the target manually before renaming to
			follow POSIX rather than Windows semantics */
				cifs_unlink(target_inode, target_direntry);
				rc = CIFSSMBRename(xid, pTcon, fromName, toName,
					cifs_sb_source->local_nls);
			}
			kfree(info_buf_source);
		} /* if we can not get memory just leave rc as EEXIST */
	}

	if((rc == -EIO)||(rc == -EEXIST)) {
		int oplock = FALSE;
		__u16 netfid;

		rc = CIFSSMBOpen(xid, pTcon, fromName, FILE_OPEN, GENERIC_READ,
					CREATE_NOT_DIR,
					&netfid, &oplock, NULL, cifs_sb_source->local_nls);
		if(rc==0) {
			CIFSSMBRenameOpenFile(xid,pTcon,netfid,
					toName, cifs_sb_source->local_nls);
			CIFSSMBClose(xid, pTcon, netfid);
		}
	}

cifs_rename_exit:
	if (fromName)
		kfree(fromName);
	if (toName)
		kfree(toName);

	FreeXid(xid);
	return rc;
}

int
cifs_revalidate(struct dentry *direntry)
{
	int xid;
	int rc = 0;
	char *full_path;
	struct cifs_sb_info *cifs_sb;
	struct cifsInodeInfo *cifsInode;
	loff_t local_size;
	struct timespec local_mtime;
	int invalidate_inode = FALSE;

	if(direntry->d_inode == NULL)
		return -ENOENT;

	cifsInode = CIFS_I(direntry->d_inode);

	if(cifsInode == NULL)
		return -ENOENT;

	/* no sense revalidating inode info on file that no one can write */
	if(CIFS_I(direntry->d_inode)->clientCanCacheRead)
		return rc;

	xid = GetXid();

	cifs_sb = CIFS_SB(direntry->d_sb);

	/* can not safely grab the rename sem here if
	rename calls revalidate since that would deadlock */
	full_path = build_path_from_dentry(direntry);
	if(full_path == NULL) {
		FreeXid(xid);
		return -ENOMEM;
	}
	cFYI(1,
	     ("Revalidate: %s inode 0x%p count %d dentry: 0x%p d_time %ld jiffies %ld",
	      full_path, direntry->d_inode,
	      direntry->d_inode->i_count.counter, direntry,
	      direntry->d_time, jiffies));

	if (cifsInode->time == 0){
		/* was set to zero previously to force revalidate */
	} else if (time_before(jiffies, cifsInode->time + HZ) && lookupCacheEnabled) {
	    if((S_ISREG(direntry->d_inode->i_mode) == 0) || 
			(direntry->d_inode->i_nlink == 1)) {  
			if (full_path)
				kfree(full_path);
			FreeXid(xid);
			return rc;
		} else {
			cFYI(1,("Have to revalidate file due to hardlinks"));
		}            
	}
	
	/* save mtime and size */
	local_mtime = direntry->d_inode->i_mtime;
	local_size  = direntry->d_inode->i_size;

	if (cifs_sb->tcon->ses->capabilities & CAP_UNIX) {
		rc = cifs_get_inode_info_unix(&direntry->d_inode, full_path,
					 direntry->d_sb,xid);
		if(rc) {
			cFYI(1,("error on getting revalidate info %d",rc));
/*			if(rc != -ENOENT)
				rc = 0; */ /* BB should we cache info on certain errors? */
		}
	} else {
		rc = cifs_get_inode_info(&direntry->d_inode, full_path, NULL,
				    direntry->d_sb,xid);
		if(rc) {
			cFYI(1,("error on getting revalidate info %d",rc));
/*			if(rc != -ENOENT)
				rc = 0; */  /* BB should we cache info on certain errors? */
		}
	}
	/* should we remap certain errors, access denied?, to zero */

	/* if not oplocked, we invalidate inode pages if mtime 
	   or file size had changed on server */

	if(timespec_equal(&local_mtime,&direntry->d_inode->i_mtime) && 
		(local_size == direntry->d_inode->i_size)) {
		cFYI(1,("cifs_revalidate - inode unchanged"));
	} else {
		/* file may have changed on server */
		if(cifsInode->clientCanCacheRead) {
			/* no need to invalidate inode pages since we were
			   the only ones who could have modified the file and
			   the server copy is staler than ours */
		} else {
			invalidate_inode = TRUE;
		}
	}

	/* can not grab this sem since kernel filesys locking
		documentation indicates i_sem may be taken by the kernel 
		on lookup and rename which could deadlock if we grab
		the i_sem here as well */
/*	down(&direntry->d_inode->i_sem);*/
	/* need to write out dirty pages here  */
	if(direntry->d_inode->i_mapping) {
		/* do we need to lock inode until after invalidate completes below? */
		filemap_fdatawrite(direntry->d_inode->i_mapping);
	}
	if(invalidate_inode) {
		filemap_fdatawait(direntry->d_inode->i_mapping);
		/* may eventually have to do this for open files too */
		if(list_empty(&(cifsInode->openFileList))) {
			/* Has changed on server - flush read ahead pages */
			cFYI(1,("Invalidating read ahead data on closed file"));
			invalidate_remote_inode(direntry->d_inode);
		}
	}
/*	up(&direntry->d_inode->i_sem);*/
	
	if (full_path)
		kfree(full_path);
	FreeXid(xid);

	return rc;
}

int cifs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
	int err = cifs_revalidate(dentry);
	if (!err)
		generic_fillattr(dentry->d_inode, stat);
	return err;
}

static int cifs_truncate_page(struct address_space *mapping, loff_t from)
{
	pgoff_t index = from >> PAGE_CACHE_SHIFT;
	unsigned offset = from & (PAGE_CACHE_SIZE-1);
	struct page *page;
	char *kaddr;
	int rc = 0;

	page = grab_cache_page(mapping, index);
	if (!page)
		return -ENOMEM;

	kaddr = kmap_atomic(page, KM_USER0);
	memset(kaddr + offset, 0, PAGE_CACHE_SIZE - offset);
	flush_dcache_page(page);
	kunmap_atomic(kaddr, KM_USER0);
	unlock_page(page);
	page_cache_release(page);
	return rc;
}

int
cifs_setattr(struct dentry *direntry, struct iattr *attrs)
{
	int xid;
	struct cifs_sb_info *cifs_sb;
	struct cifsTconInfo *pTcon;
	char *full_path = NULL;
	int rc = -EACCES;
	int found = FALSE;
	struct cifsFileInfo *open_file = NULL;
	FILE_BASIC_INFO time_buf;
	int set_time = FALSE;
	__u64 mode = 0xFFFFFFFFFFFFFFFFULL;
	__u64 uid = 0xFFFFFFFFFFFFFFFFULL;
	__u64 gid = 0xFFFFFFFFFFFFFFFFULL;
	struct cifsInodeInfo *cifsInode;
	struct list_head * tmp;

	xid = GetXid();

	cFYI(1,
	     (" In cifs_setattr, name = %s attrs->iavalid 0x%x ",
	      direntry->d_name.name, attrs->ia_valid));
	cifs_sb = CIFS_SB(direntry->d_inode->i_sb);
	pTcon = cifs_sb->tcon;

	down(&direntry->d_sb->s_vfs_rename_sem);
	full_path = build_path_from_dentry(direntry);
	up(&direntry->d_sb->s_vfs_rename_sem);
	if(full_path == NULL) {
		FreeXid(xid);
		return -ENOMEM;
	}
	cifsInode = CIFS_I(direntry->d_inode);

	/* BB check if we need to refresh inode from server now ? BB */

	/* need to flush data before changing file size on server */
	filemap_fdatawrite(direntry->d_inode->i_mapping); 
	filemap_fdatawait(direntry->d_inode->i_mapping);

	if (attrs->ia_valid & ATTR_SIZE) {
		read_lock(&GlobalSMBSeslock); 
		/* To avoid spurious oplock breaks from server, in the case
			of inodes that we already have open, avoid doing path
			based setting of file size if we can do it by handle.
			This keeps our caching token (oplock) and avoids
			timeouts when the local oplock break takes longer to flush
			writebehind data than the SMB timeout for the SetPathInfo 
			request would allow */
		list_for_each(tmp, &cifsInode->openFileList) {            
			open_file = list_entry(tmp,struct cifsFileInfo, flist);
			/* We check if file is open for writing first */
			if((open_file->pfile) &&
				((open_file->pfile->f_flags & O_RDWR) || 
				 (open_file->pfile->f_flags & O_WRONLY))) {
				if(open_file->invalidHandle == FALSE) {
					/* we found a valid, writeable network file 
					handle to use to try to set the file size */
					__u16 nfid = open_file->netfid;
					__u32 npid = open_file->pid;
					read_unlock(&GlobalSMBSeslock);
					found = TRUE;
					rc = CIFSSMBSetFileSize(xid, pTcon, attrs->ia_size,
					   nfid,npid,FALSE);
					cFYI(1,("SetFileSize by handle (setattrs) rc = %d",rc));
				/* Do not need reopen and retry on EAGAIN since we will
					retry by pathname below */

					break;  /* now that we found one valid file handle no
						sense continuing to loop trying others */
				}
			}
		}
		if(found == FALSE) {
			read_unlock(&GlobalSMBSeslock);
		}


		if(rc != 0) {
			/* Set file size by pathname rather than by handle either
			because no valid, writeable file handle for it was found or
			because there was an error setting it by handle */
			rc = CIFSSMBSetEOF(xid, pTcon, full_path, attrs->ia_size,FALSE,
				   cifs_sb->local_nls);
			cFYI(1,(" SetEOF by path (setattrs) rc = %d",rc));
		}
        
	/*  Server is ok setting allocation size implicitly - no need to call: */
	/*CIFSSMBSetEOF(xid, pTcon, full_path, attrs->ia_size, TRUE, cifs_sb->local_nls);*/

		if (rc == 0) {
			rc = vmtruncate(direntry->d_inode, attrs->ia_size);
			cifs_truncate_page(direntry->d_inode->i_mapping, direntry->d_inode->i_size);
		}
	}
	if (attrs->ia_valid & ATTR_UID) {
		cFYI(1, (" CIFS - UID changed to %d", attrs->ia_uid));
		uid = attrs->ia_uid;
		/*        entry->uid = cpu_to_le16(attr->ia_uid); */
	}
	if (attrs->ia_valid & ATTR_GID) {
		cFYI(1, (" CIFS - GID changed to %d", attrs->ia_gid));
		gid = attrs->ia_gid;
		/*      entry->gid = cpu_to_le16(attr->ia_gid); */
	}

	time_buf.Attributes = 0;
	if (attrs->ia_valid & ATTR_MODE) {
		cFYI(1, (" CIFS - Mode changed to 0x%x", attrs->ia_mode));
		mode = attrs->ia_mode;
		/* entry->mode = cpu_to_le16(attr->ia_mode); */
	}

	if ((cifs_sb->tcon->ses->capabilities & CAP_UNIX)
	    && (attrs->ia_valid & (ATTR_MODE | ATTR_GID | ATTR_UID)))
		rc = CIFSSMBUnixSetPerms(xid, pTcon, full_path, mode, uid, gid,
				0 /* dev_t */, cifs_sb->local_nls);
	else if (attrs->ia_valid & ATTR_MODE) {
		if((mode & S_IWUGO) == 0) /* not writeable */ {
			if((cifsInode->cifsAttrs & ATTR_READONLY) == 0)
				time_buf.Attributes = 
					cpu_to_le32(cifsInode->cifsAttrs | ATTR_READONLY);
		} else if((mode & S_IWUGO) == S_IWUGO) {
			if(cifsInode->cifsAttrs & ATTR_READONLY)
				time_buf.Attributes = 
					cpu_to_le32(cifsInode->cifsAttrs & (~ATTR_READONLY));
		}
		/* BB to be implemented - via Windows security descriptors or streams */
		/* CIFSSMBWinSetPerms(xid,pTcon,full_path,mode,uid,gid,cifs_sb->local_nls);*/
	}

	if (attrs->ia_valid & ATTR_ATIME) {
		set_time = TRUE;
		time_buf.LastAccessTime =
		    cpu_to_le64(cifs_UnixTimeToNT(attrs->ia_atime));
	} else
		time_buf.LastAccessTime = 0;

	if (attrs->ia_valid & ATTR_MTIME) {
		set_time = TRUE;
		time_buf.LastWriteTime =
		    cpu_to_le64(cifs_UnixTimeToNT(attrs->ia_mtime));
	} else
		time_buf.LastWriteTime = 0;

	if (attrs->ia_valid & ATTR_CTIME) {
		set_time = TRUE;
		cFYI(1, (" CIFS - CTIME changed ")); /* BB probably do not need */
		time_buf.ChangeTime =
		    cpu_to_le64(cifs_UnixTimeToNT(attrs->ia_ctime));
	} else
		time_buf.ChangeTime = 0;

	if (set_time | time_buf.Attributes) {
		/* BB what if setting one attribute fails  
			(such as size) but time setting works */
		time_buf.CreationTime = 0;	/* do not change */
		/* In the future we should experiment - try setting timestamps
			 via Handle (SetFileInfo) instead of by path */
		rc = CIFSSMBSetTimes(xid, pTcon, full_path, &time_buf,
				cifs_sb->local_nls);
	}

	/* do not  need local check to inode_check_ok since the server does that */
	if (!rc)
		rc = inode_setattr(direntry->d_inode, attrs);
	if (full_path)
		kfree(full_path);
	FreeXid(xid);
	return rc;
}

void
cifs_delete_inode(struct inode *inode)
{
	cFYI(1, ("In cifs_delete_inode, inode = 0x%p ", inode));
	/* may have to add back in if and when safe distributed caching of
		directories added e.g. via FindNotify */
}
