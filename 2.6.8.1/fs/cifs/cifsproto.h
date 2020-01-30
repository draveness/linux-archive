/*
 *   fs/cifs/cifsproto.h
 *
 *   Copyright (c) International Business Machines  Corp., 2002
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
#ifndef _CIFSPROTO_H
#define _CIFSPROTO_H
#include <linux/nls.h>

struct statfs;

/*
 *****************************************************************
 * All Prototypes
 *****************************************************************
 */

extern struct smb_hdr *cifs_buf_get(void);
extern void cifs_buf_release(void *);
extern int smb_send(struct socket *, struct smb_hdr *,
			unsigned int /* length */ , struct sockaddr *);
extern unsigned int _GetXid(void);
extern void _FreeXid(unsigned int);
#define GetXid() (int)_GetXid(); cFYI(1,("CIFS VFS: in %s as Xid: %d with uid: %d",__FUNCTION__, xid,current->fsuid));
#define FreeXid(curr_xid) {_FreeXid(curr_xid); cFYI(1,("CIFS VFS: leaving %s (xid = %d) rc = %d",__FUNCTION__,curr_xid,rc));}
extern char *build_path_from_dentry(struct dentry *);
extern char *build_wildcard_path_from_dentry(struct dentry *direntry);
extern void renew_parental_timestamps(struct dentry *direntry);
extern int SendReceive(const unsigned int /* xid */ , struct cifsSesInfo *,
			struct smb_hdr * /* input */ ,
			struct smb_hdr * /* out */ ,
			int * /* bytes returned */ , const int long_op);
extern int checkSMBhdr(struct smb_hdr *smb, __u16 mid);
extern int checkSMB(struct smb_hdr *smb, __u16 mid, int length);
extern int is_valid_oplock_break(struct smb_hdr *smb);
extern unsigned int smbCalcSize(struct smb_hdr *ptr);
extern int decode_negTokenInit(unsigned char *security_blob, int length,
			enum securityEnum *secType);
extern int map_smb_to_linux_error(struct smb_hdr *smb);
extern void header_assemble(struct smb_hdr *, char /* command */ ,
			const struct cifsTconInfo *, int
			/* length of fixed section (word count) in two byte units  */
			);
struct oplock_q_entry * AllocOplockQEntry(struct inode *, u16, struct cifsTconInfo *);
void DeleteOplockQEntry(struct oplock_q_entry *);
extern struct timespec cifs_NTtimeToUnix(u64 /* utc nanoseconds since 1601 */ );
extern u64 cifs_UnixTimeToNT(struct timespec);
extern int cifs_get_inode_info(struct inode **pinode,
			const unsigned char *search_path, 
			FILE_ALL_INFO * pfile_info,
			struct super_block *sb, int xid);
extern int cifs_get_inode_info_unix(struct inode **pinode,
			const unsigned char *search_path,
			struct super_block *sb,int xid);

extern int cifs_setup_session(unsigned int xid, struct cifsSesInfo *pSesInfo, 
			struct nls_table * nls_info);
extern int CIFSSMBNegotiate(unsigned int xid, struct cifsSesInfo *ses);

extern int CIFSTCon(unsigned int xid, struct cifsSesInfo *ses,
			const char *tree, struct cifsTconInfo *tcon,
			const struct nls_table *);

extern int CIFSFindFirst(const int xid, struct cifsTconInfo *tcon,
			const char *searchName,
			FILE_DIRECTORY_INFO * findData,
			T2_FFIRST_RSP_PARMS * findParms,
			const struct nls_table *nls_codepage,
			int *pUnicodeFlag,
			int *pUnixFlag /* if Unix extensions used */ );
extern int CIFSFindNext(const int xid, struct cifsTconInfo *tcon,
			FILE_DIRECTORY_INFO * findData,
			T2_FNEXT_RSP_PARMS * findParms,
			const __u16 searchHandle, char * resume_name,
			int name_length, __u32 resume_key,
			int *UnicodeFlag, int *pUnixFlag);

extern int CIFSFindClose(const int, struct cifsTconInfo *tcon,
			const __u16 search_handle);

extern int CIFSSMBQPathInfo(const int xid, struct cifsTconInfo *tcon,
			const unsigned char *searchName,
			FILE_ALL_INFO * findData,
			const struct nls_table *nls_codepage);

extern int CIFSSMBUnixQPathInfo(const int xid,
			struct cifsTconInfo *tcon,
			const unsigned char *searchName,
			FILE_UNIX_BASIC_INFO * pFindData,
			const struct nls_table *nls_codepage);

extern int CIFSGetDFSRefer(const int xid, struct cifsSesInfo *ses,
			const unsigned char *searchName,
			unsigned char **targetUNCs,
			unsigned int *number_of_UNC_in_array,
			const struct nls_table *nls_codepage);

extern int connect_to_dfs_path(int xid, struct cifsSesInfo *pSesInfo,
			const char *old_path,
			const struct nls_table *nls_codepage);
extern int get_dfs_path(int xid, struct cifsSesInfo *pSesInfo,
			const char *old_path, const struct nls_table *nls_codepage, 
			unsigned int *pnum_referrals, unsigned char ** preferrals);
extern int CIFSSMBQFSInfo(const int xid, struct cifsTconInfo *tcon,
			struct kstatfs *FSData,
			const struct nls_table *nls_codepage);
extern int CIFSSMBQFSAttributeInfo(const int xid,
			struct cifsTconInfo *tcon,
			const struct nls_table *nls_codepage);
extern int CIFSSMBQFSDeviceInfo(const int xid, struct cifsTconInfo *tcon,
			const struct nls_table *nls_codepage);
extern int CIFSSMBQFSUnixInfo(const int xid, struct cifsTconInfo *tcon,
			const struct nls_table *nls_codepage);

extern int CIFSSMBSetTimes(const int xid, struct cifsTconInfo *tcon,
			const char *fileName, const FILE_BASIC_INFO * data,
			const struct nls_table *nls_codepage);
extern int CIFSSMBSetEOF(const int xid, struct cifsTconInfo *tcon,
			const char *fileName, __u64 size,int setAllocationSizeFlag,
			const struct nls_table *nls_codepage);
extern int CIFSSMBSetFileSize(const int xid, struct cifsTconInfo *tcon,
			 __u64 size, __u16 fileHandle,__u32 opener_pid, int AllocSizeFlag);
extern int CIFSSMBUnixSetPerms(const int xid, struct cifsTconInfo *pTcon,
			char *full_path, __u64 mode, __u64 uid,
			__u64 gid, dev_t dev, const struct nls_table *nls_codepage);

extern int CIFSSMBMkDir(const int xid, struct cifsTconInfo *tcon,
			const char *newName,
			const struct nls_table *nls_codepage);
extern int CIFSSMBRmDir(const int xid, struct cifsTconInfo *tcon,
			const char *name, const struct nls_table *nls_codepage);

extern int CIFSSMBDelFile(const int xid, struct cifsTconInfo *tcon,
			const char *name,
			const struct nls_table *nls_codepage);
extern int CIFSSMBRename(const int xid, struct cifsTconInfo *tcon,
			const char *fromName, const char *toName,
			const struct nls_table *nls_codepage);
extern int CIFSSMBRenameOpenFile(const int xid,struct cifsTconInfo *pTcon,
			int netfid, char * target_name, const struct nls_table *nls_codepage);
extern int CIFSCreateHardLink(const int xid,
			struct cifsTconInfo *tcon,
			const char *fromName, const char *toName,
			const struct nls_table *nls_codepage);
extern int CIFSUnixCreateHardLink(const int xid,
			struct cifsTconInfo *tcon,
			const char *fromName, const char *toName,
			const struct nls_table *nls_codepage);
extern int CIFSUnixCreateSymLink(const int xid,
			struct cifsTconInfo *tcon,
			const char *fromName, const char *toName,
			const struct nls_table *nls_codepage);
extern int CIFSSMBUnixQuerySymLink(const int xid,
			struct cifsTconInfo *tcon,
			const unsigned char *searchName,
			char *syminfo, const int buflen,
			const struct nls_table *nls_codepage);
extern int CIFSSMBQueryReparseLinkInfo(const int xid, 
			struct cifsTconInfo *tcon,
			const unsigned char *searchName,
			char *symlinkinfo, const int buflen, __u16 fid,
			const struct nls_table *nls_codepage);

extern int CIFSSMBOpen(const int xid, struct cifsTconInfo *tcon,
			const char *fileName, const int disposition,
			const int access_flags, const int omode,
			__u16 * netfid, int *pOplock, FILE_ALL_INFO *,
			const struct nls_table *nls_codepage);
extern int CIFSSMBClose(const int xid, struct cifsTconInfo *tcon,
			const int smb_file_id);

extern int CIFSSMBRead(const int xid, struct cifsTconInfo *tcon,
			const int netfid, unsigned int count,
			const __u64 lseek, unsigned int *nbytes, char **buf);
extern int CIFSSMBWrite(const int xid, struct cifsTconInfo *tcon,
			const int netfid, const unsigned int count,
			const __u64 lseek, unsigned int *nbytes,
			const char *buf, const int long_op);
extern int CIFSSMBLock(const int xid, struct cifsTconInfo *tcon,
			const __u16 netfid, const __u64 len,
			const __u64 offset, const __u32 numUnlock,
			const __u32 numLock, const __u8 lockType,
			const int waitFlag);

extern int CIFSSMBTDis(const int xid, struct cifsTconInfo *tcon);
extern int CIFSSMBLogoff(const int xid, struct cifsSesInfo *ses);

extern struct cifsSesInfo *sesInfoAlloc(void);
extern void sesInfoFree(struct cifsSesInfo *);
extern struct cifsTconInfo *tconInfoAlloc(void);
extern void tconInfoFree(struct cifsTconInfo *);

extern int cifs_reconnect(struct TCP_Server_Info *server);

extern int cifs_sign_smb(struct smb_hdr *, struct cifsSesInfo *,__u32 *);
extern int cifs_verify_signature(const struct smb_hdr *, const char * mac_key,
	__u32 expected_sequence_number);
extern int cifs_calculate_mac_key(char * key,const char * rn,const char * pass);
extern void CalcNTLMv2_partial_mac_key(struct cifsSesInfo *, struct nls_table *);
extern void CalcNTLMv2_response(const struct cifsSesInfo *,char * );
extern int CIFSSMBCopy(int xid,
			struct cifsTconInfo *source_tcon,
			const char *fromName,
			const __u16 target_tid,
			const char *toName, const int flags,
			const struct nls_table *nls_codepage);
extern int CIFSSMBNotify(const int xid, struct cifsTconInfo *tcon, 
			const int notify_subdirs,const __u16 netfid,__u32 filter,
			const struct nls_table *nls_codepage);
extern ssize_t CIFSSMBQAllEAs(const int xid, struct cifsTconInfo *tcon,
			const unsigned char *searchName, char * EAData,
			size_t bufsize, const struct nls_table *nls_codepage);
extern ssize_t CIFSSMBQueryEA(const int xid,struct cifsTconInfo * tcon,
		const unsigned char * searchName,const unsigned char * ea_name,
		unsigned char * ea_value, size_t buf_size, 
		const struct nls_table *nls_codepage);
extern int CIFSSMBSetEA(const int xid, struct cifsTconInfo *tcon, 
		const char *fileName, const char * ea_name, 
		const void * ea_value, const __u16 ea_value_len, 
		const struct nls_table *nls_codepage);
#endif			/* _CIFSPROTO_H */
