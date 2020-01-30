/*
 *   fs/cifs/misc.c
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

#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/mempool.h>
#include "cifspdu.h"
#include "cifsglob.h"
#include "cifsproto.h"
#include "cifs_debug.h"
#include "smberr.h"
#include "nterr.h"

extern mempool_t *cifs_req_poolp;
extern struct task_struct * oplockThread;

__u16 GlobalMid;		/* multiplex id - rotating counter */

/* The xid serves as a useful identifier for each incoming vfs request, 
   in a similar way to the mid which is useful to track each sent smb, 
   and CurrentXid can also provide a running counter (although it 
   will eventually wrap past zero) of the total vfs operations handled 
   since the cifs fs was mounted */

unsigned int
_GetXid(void)
{
	unsigned int xid;

	spin_lock(&GlobalMid_Lock);
	GlobalTotalActiveXid++;
	if (GlobalTotalActiveXid > GlobalMaxActiveXid)
		GlobalMaxActiveXid = GlobalTotalActiveXid;	/* keep high water mark for number of simultaneous vfs ops in our filesystem */
	xid = GlobalCurrentXid++;
	spin_unlock(&GlobalMid_Lock);
	return xid;
}

void
_FreeXid(unsigned int xid)
{
	spin_lock(&GlobalMid_Lock);
	/* if(GlobalTotalActiveXid == 0)
		BUG(); */
	GlobalTotalActiveXid--;
	spin_unlock(&GlobalMid_Lock);
}

struct cifsSesInfo *
sesInfoAlloc(void)
{
	struct cifsSesInfo *ret_buf;

	ret_buf =
	    (struct cifsSesInfo *) kmalloc(sizeof (struct cifsSesInfo),
					   GFP_KERNEL);
	if (ret_buf) {
		memset(ret_buf, 0, sizeof (struct cifsSesInfo));
		write_lock(&GlobalSMBSeslock);
		atomic_inc(&sesInfoAllocCount);
		ret_buf->status = CifsNew;
		list_add(&ret_buf->cifsSessionList, &GlobalSMBSessionList);
		init_MUTEX(&ret_buf->sesSem);
		write_unlock(&GlobalSMBSeslock);
	}
	return ret_buf;
}

void
sesInfoFree(struct cifsSesInfo *buf_to_free)
{
	if (buf_to_free == NULL) {
		cFYI(1, ("Null buffer passed to sesInfoFree"));
		return;
	}

	write_lock(&GlobalSMBSeslock);
	atomic_dec(&sesInfoAllocCount);
	list_del(&buf_to_free->cifsSessionList);
	write_unlock(&GlobalSMBSeslock);
	if (buf_to_free->serverOS)
		kfree(buf_to_free->serverOS);
	if (buf_to_free->serverDomain)
		kfree(buf_to_free->serverDomain);
	if (buf_to_free->serverNOS)
		kfree(buf_to_free->serverNOS);
	if (buf_to_free->password)
		kfree(buf_to_free->password);
	kfree(buf_to_free);
}

struct cifsTconInfo *
tconInfoAlloc(void)
{
	struct cifsTconInfo *ret_buf;
	ret_buf =
	    (struct cifsTconInfo *) kmalloc(sizeof (struct cifsTconInfo),
					    GFP_KERNEL);
	if (ret_buf) {
		memset(ret_buf, 0, sizeof (struct cifsTconInfo));
		write_lock(&GlobalSMBSeslock);
		atomic_inc(&tconInfoAllocCount);
		list_add(&ret_buf->cifsConnectionList,
			 &GlobalTreeConnectionList);
		ret_buf->tidStatus = CifsNew;
		INIT_LIST_HEAD(&ret_buf->openFileList);
		init_MUTEX(&ret_buf->tconSem);
#ifdef CONFIG_CIFS_STATS
		ret_buf->stat_lock = SPIN_LOCK_UNLOCKED;
#endif
		write_unlock(&GlobalSMBSeslock);
	}
	return ret_buf;
}

void
tconInfoFree(struct cifsTconInfo *buf_to_free)
{
	if (buf_to_free == NULL) {
		cFYI(1, ("Null buffer passed to tconInfoFree"));
		return;
	}
	write_lock(&GlobalSMBSeslock);
	atomic_dec(&tconInfoAllocCount);
	list_del(&buf_to_free->cifsConnectionList);
	write_unlock(&GlobalSMBSeslock);
	if (buf_to_free->nativeFileSystem)
		kfree(buf_to_free->nativeFileSystem);
	kfree(buf_to_free);
}

struct smb_hdr *
cifs_buf_get(void)
{
	struct smb_hdr *ret_buf = NULL;

/* We could use negotiated size instead of max_msgsize - 
   but it may be more efficient to always alloc same size 
   albeit slightly larger than necessary and maxbuffersize 
   defaults to this and can not be bigger */
	ret_buf =
	    (struct smb_hdr *) mempool_alloc(cifs_req_poolp, SLAB_KERNEL | SLAB_NOFS);

	/* clear the first few header bytes */
	if (ret_buf) {
		memset(ret_buf, 0, sizeof (struct smb_hdr));
		atomic_inc(&bufAllocCount);
	}

	return ret_buf;
}

void
cifs_buf_release(void *buf_to_free)
{

	if (buf_to_free == NULL) {
		cFYI(1, ("Null buffer passed to cifs_buf_release"));
		return;
	}
	mempool_free(buf_to_free,cifs_req_poolp);

	atomic_dec(&bufAllocCount);
	return;
}

void
header_assemble(struct smb_hdr *buffer, char smb_command /* command */ ,
		const struct cifsTconInfo *treeCon, int word_count
		/* length of fixed section (word count) in two byte units  */
    )
{
	int i;
	__u32 tmp;
	struct list_head* temp_item;
	struct cifsSesInfo * ses;
	char *temp = (char *) buffer;

	for (i = 0; i < MAX_CIFS_HDR_SIZE; i++) {
		temp[i] = 0;	/* BB is this needed ?? */
	}

	buffer->smb_buf_length =
	    (2 * word_count) + sizeof (struct smb_hdr) -
	    4 /*  RFC 1001 length field does not count */  +
	    2 /* for bcc field itself */ ;
	/* Note that this is the only network field that has to be converted to big endian and it is done just before we send it */

	buffer->Protocol[0] = 0xFF;
	buffer->Protocol[1] = 'S';
	buffer->Protocol[2] = 'M';
	buffer->Protocol[3] = 'B';
	buffer->Command = smb_command;
	buffer->Flags = 0x00;	/* case sensitive */
	buffer->Flags2 = SMBFLG2_KNOWS_LONG_NAMES;
	tmp = cpu_to_le32(current->tgid);
	buffer->Pid = tmp & 0xFFFF;
	tmp >>= 16;
	buffer->PidHigh = tmp & 0xFFFF;
	spin_lock(&GlobalMid_Lock);
	GlobalMid++;
	buffer->Mid = GlobalMid;
	spin_unlock(&GlobalMid_Lock);
	if (treeCon) {
		buffer->Tid = treeCon->tid;
		if (treeCon->ses) {
			if (treeCon->ses->capabilities & CAP_UNICODE)
				buffer->Flags2 |= SMBFLG2_UNICODE;
			if (treeCon->ses->capabilities & CAP_STATUS32) {
				buffer->Flags2 |= SMBFLG2_ERR_STATUS;
			}

			buffer->Uid = treeCon->ses->Suid;	/* always in LE format */
			if(multiuser_mount != 0) {
		/* For the multiuser case, there are few obvious technically  */
		/* possible mechanisms to match the local linux user (uid)    */
		/* to a valid remote smb user (smb_uid):		      */
		/* 	1) Query Winbind (or other local pam/nss daemon       */
		/* 	  for userid/password/logon_domain or credential      */
		/*      2) Query Winbind for uid to sid to username mapping   */
		/* 	   and see if we have a matching password for existing*/
		/*         session for that user perhas getting password by   */
		/*         adding a new pam_cifs module that stores passwords */
		/*         so that the cifs vfs can get at that for all logged*/
		/*	   on users					      */
		/*	3) (Which is the mechanism we have chosen)	      */
		/*	   Search through sessions to the same server for a   */
		/*	   a match on the uid that was passed in on mount     */
		/*         with the current processes uid (or euid?) and use  */
		/* 	   that smb uid.   If no existing smb session for     */
		/* 	   that uid found, use the default smb session ie     */
		/*         the smb session for the volume mounted which is    */
		/* 	   the same as would be used if the multiuser mount   */
		/* 	   flag were disabled.  */

		/*  BB Add support for establishing new tCon and SMB Session  */
		/*      with userid/password pairs found on the smb session   */ 
		/*	for other target tcp/ip addresses 		BB    */
				if(current->uid != treeCon->ses->linux_uid) {
					cFYI(1,("Multiuser mode and UID did not match tcon uid "));
					read_lock(&GlobalSMBSeslock);
					list_for_each(temp_item, &GlobalSMBSessionList) {
						ses = list_entry(temp_item, struct cifsSesInfo, cifsSessionList);
						if(ses->linux_uid == current->uid) {
							if(ses->server == treeCon->ses->server) {
								cFYI(1,("found matching uid substitute right smb_uid"));  
								buffer->Uid = ses->Suid;
								break;
							} else {
								/* BB eventually call cifs_setup_session here */
								cFYI(1,("local UID found but smb sess with this server does not exist"));  
							}
						}
					}
					read_unlock(&GlobalSMBSeslock);
				}
			}
		}
		if (treeCon->Flags & SMB_SHARE_IS_IN_DFS)
			buffer->Flags2 |= SMBFLG2_DFS;
		if(treeCon->ses->server)
			if(treeCon->ses->server->secMode & 
			  (SECMODE_SIGN_REQUIRED | SECMODE_SIGN_ENABLED))
				buffer->Flags2 |= SMBFLG2_SECURITY_SIGNATURE;
	}

/*  endian conversion of flags is now done just before sending */
	buffer->WordCount = (char) word_count;
	return;
}

int
checkSMBhdr(struct smb_hdr *smb, __u16 mid)
{
	/* Make sure that this really is an SMB, that it is a response, 
	   and that the message ids match */
	if ((*(unsigned int *) smb->Protocol == cpu_to_le32(0x424d53ff)) && 
		(mid == smb->Mid)) {    
		if(smb->Flags & SMBFLG_RESPONSE)
			return 0;                    
		else {        
		/* only one valid case where server sends us request */
			if(smb->Command == SMB_COM_LOCKING_ANDX)
				return 0;
			else
				cERROR(1, ("Rcvd Request not response "));         
		}
	} else { /* bad signature or mid */
		if (*(unsigned int *) smb->Protocol != cpu_to_le32(0x424d53ff))
			cERROR(1,
			       ("Bad protocol string signature header %x ",
				*(unsigned int *) smb->Protocol));
		if (mid != smb->Mid)
			cERROR(1, ("Mids do not match"));
	}
	cERROR(1, ("bad smb detected. The Mid=%d", smb->Mid));
	return 1;
}

int
checkSMB(struct smb_hdr *smb, __u16 mid, int length)
{
	cFYI(0,
	     ("Entering checkSMB with Length: %x, smb_buf_length: %x ",
	      length, ntohl(smb->smb_buf_length)));
	if (((unsigned int)length < 2 + sizeof (struct smb_hdr))
	    || (ntohl(smb->smb_buf_length) >
		CIFS_MAX_MSGSIZE + MAX_CIFS_HDR_SIZE - 4)) {
		if ((unsigned int)length < 2 + sizeof (struct smb_hdr)) {
			cERROR(1, ("Length less than 2 + sizeof smb_hdr "));
			if (((unsigned int)length >= sizeof (struct smb_hdr) - 1)
			    && (smb->Status.CifsError != 0))
				return 0;	/* some error cases do not return wct and bcc */

		}
		if (ntohl(smb->smb_buf_length) >
		    CIFS_MAX_MSGSIZE + MAX_CIFS_HDR_SIZE - 4)
			cERROR(1,
			       ("smb_buf_length greater than CIFS_MAX_MSGSIZE ... "));
		cERROR(1,
		       ("bad smb detected. Illegal length. The mid=%d",
			smb->Mid));
		return 1;
	}

	if (checkSMBhdr(smb, mid))
		return 1;

	if ((4 + ntohl(smb->smb_buf_length) != smbCalcSize(smb))
	    || (4 + ntohl(smb->smb_buf_length) != (unsigned int)length)) {
		return 0;
	} else {
		cERROR(1, ("smbCalcSize %x ", smbCalcSize(smb)));
		cERROR(1,
		       ("bad smb size detected. The Mid=%d", smb->Mid));
		return 1;
	}
}
int
is_valid_oplock_break(struct smb_hdr *buf)
{    
	struct smb_com_lock_req * pSMB = (struct smb_com_lock_req *)buf;
	struct list_head *tmp;
	struct list_head *tmp1;
	struct cifsTconInfo *tcon;
	struct cifsFileInfo *netfile;

	/* could add check for smb response flag 0x80 */
	cFYI(1,("Checking for oplock break"));    
	if(pSMB->hdr.Command != SMB_COM_LOCKING_ANDX)
		return FALSE;
	if(pSMB->hdr.Flags & SMBFLG_RESPONSE) {
		/* no sense logging error on invalid handle on oplock
		   break - harmless race between close request and oplock
		   break response is expected from time to time writing out
		   large dirty files cached on the client */
		if ((NT_STATUS_INVALID_HANDLE) == 
		   le32_to_cpu(pSMB->hdr.Status.CifsError)) { 
			cFYI(1,("invalid handle on oplock break"));
			return TRUE;
		} else if (ERRbadfid == 
		   le16_to_cpu(pSMB->hdr.Status.DosError.Error)) {
			return TRUE;	  
		} else {
			return FALSE; /* on valid oplock brk we get "request" */
		}
	}
	if(pSMB->hdr.WordCount != 8)
		return FALSE;

	cFYI(1,(" oplock type 0x%d level 0x%d",pSMB->LockType,pSMB->OplockLevel));
	if(!(pSMB->LockType & LOCKING_ANDX_OPLOCK_RELEASE))
		return FALSE;    

	/* look up tcon based on tid & uid */
	read_lock(&GlobalSMBSeslock);
	list_for_each(tmp, &GlobalTreeConnectionList) {
		tcon = list_entry(tmp, struct cifsTconInfo, cifsConnectionList);
		if (tcon->tid == buf->Tid) {
#ifdef CONFIG_CIFS_STATS
			atomic_inc(&tcon->num_oplock_brks);
#endif
			list_for_each(tmp1,&tcon->openFileList){
				netfile = list_entry(tmp1,struct cifsFileInfo,tlist);
				if(pSMB->Fid == netfile->netfid) {
					struct cifsInodeInfo *pCifsInode;
					read_unlock(&GlobalSMBSeslock);
					cFYI(1,("Matching file id, processing oplock break"));
					pCifsInode = 
						CIFS_I(netfile->pInode);
					pCifsInode->clientCanCacheAll = FALSE;
					if(pSMB->OplockLevel == 0)
						pCifsInode->clientCanCacheRead = FALSE;
					pCifsInode->oplockPending = TRUE;
					AllocOplockQEntry(netfile->pInode, netfile->netfid, tcon);
					cFYI(1,("about to wake up oplock thd"));
					wake_up_process(oplockThread);               
					return TRUE;
				}
			}
			read_unlock(&GlobalSMBSeslock);
			cFYI(1,("No matching file for oplock break on connection"));
			return TRUE;
		}
	}
	read_unlock(&GlobalSMBSeslock);
	cFYI(1,("Can not process oplock break for non-existent connection"));
	return TRUE;
}

void
dump_smb(struct smb_hdr *smb_buf, int smb_buf_length)
{
	int i, j;
	char debug_line[17];
	unsigned char *buffer;

	if (traceSMB == 0)
		return;

	buffer = (unsigned char *) smb_buf;
	for (i = 0, j = 0; i < smb_buf_length; i++, j++) {
		if (i % 8 == 0) {	/* we have reached the beginning of line  */
			printk(KERN_DEBUG "| ");
			j = 0;
		}
		printk("%0#4x ", buffer[i]);
		debug_line[2 * j] = ' ';
		if (isprint(buffer[i]))
			debug_line[1 + (2 * j)] = buffer[i];
		else
			debug_line[1 + (2 * j)] = '_';

		if (i % 8 == 7) {	/* we have reached end of line, time to print ascii */
			debug_line[16] = 0;
			printk(" | %s\n", debug_line);
		}
	}
	for (; j < 8; j++) {
		printk("     ");
		debug_line[2 * j] = ' ';
		debug_line[1 + (2 * j)] = ' ';
	}
	printk( " | %s\n", debug_line);
	return;
}
