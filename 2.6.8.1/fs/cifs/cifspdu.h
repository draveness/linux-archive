/*
 *   fs/cifs/cifspdu.h
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

#ifndef _CIFSPDU_H
#define _CIFSPDU_H

#include <net/sock.h>

#define CIFS_PROT   0
#define BAD_PROT    CIFS_PROT+1

/* SMB command codes */
#define SMB_COM_CREATE_DIRECTORY      0x00
#define SMB_COM_DELETE_DIRECTORY      0x01
#define SMB_COM_CLOSE                 0x04
#define SMB_COM_DELETE                0x06
#define SMB_COM_RENAME                0x07
#define SMB_COM_LOCKING_ANDX          0x24
#define SMB_COM_COPY                  0x29
#define SMB_COM_READ_ANDX             0x2E
#define SMB_COM_WRITE_ANDX            0x2F
#define SMB_COM_TRANSACTION2          0x32
#define SMB_COM_TRANSACTION2_SECONDARY 0x33
#define SMB_COM_FIND_CLOSE2           0x34
#define SMB_COM_TREE_DISCONNECT       0x71
#define SMB_COM_NEGOTIATE             0x72
#define SMB_COM_SESSION_SETUP_ANDX    0x73
#define SMB_COM_LOGOFF_ANDX           0x74
#define SMB_COM_TREE_CONNECT_ANDX     0x75
#define SMB_COM_NT_TRANSACT           0xA0
#define SMB_COM_NT_TRANSACT_SECONDARY 0xA1
#define SMB_COM_NT_CREATE_ANDX        0xA2
#define SMB_COM_NT_RENAME             0xA5

/* Transact2 subcommand codes */
#define TRANS2_OPEN                   0x00
#define TRANS2_FIND_FIRST             0x01
#define TRANS2_FIND_NEXT              0x02
#define TRANS2_QUERY_FS_INFORMATION   0x03
#define TRANS2_QUERY_PATH_INFORMATION 0x05
#define TRANS2_SET_PATH_INFORMATION   0x06
#define TRANS2_QUERY_FILE_INFORMATION 0x07
#define TRANS2_SET_FILE_INFORMATION   0x08
#define TRANS2_GET_DFS_REFERRAL       0x10
#define TRANS2_REPORT_DFS_INCOSISTENCY 0x11

/* NT Transact subcommand codes */
#define NT_TRANSACT_CREATE            0x01
#define NT_TRANSACT_IOCTL             0x02
#define NT_TRANSACT_SET_SECURITY_DESC 0x03
#define NT_TRANSACT_NOTIFY_CHANGE     0x04
#define NT_TRANSACT_RENAME            0x05
#define NT_TRANSACT_QUERY_SECURITY_DESC 0x06
#define NT_TRANSACT_GET_USER_QUOTA    0x07
#define NT_TRANSACT_SET_USER_QUOTA    0x08

#define MAX_CIFS_HDR_SIZE 256	/* chained NTCreateXReadX will probably be biggest */

/* internal cifs vfs structures */
/*****************************************************************
 * All constants go here
 *****************************************************************
 */

/*
 * Starting value for maximum SMB size negotiation
 */
#define CIFS_MAX_MSGSIZE (4*4096)

/*
 * Size of encrypted user password in bytes
 */
#define CIFS_ENCPWD_SIZE (16)

/*
 * Size of the crypto key returned on the negotiate SMB in bytes
 */
#define CIFS_CRYPTO_KEY_SIZE (8)

/*
 * Size of the session key (crypto key encrypted with the password
 */
#define CIFS_SESSION_KEY_SIZE (24)

/*
 * Maximum user name length
 */
#define CIFS_UNLEN (20)

/*
 * Flags on SMB open
 */
#define SMBOPEN_WRITE_THROUGH 0x4000
#define SMBOPEN_DENY_ALL      0x0010
#define SMBOPEN_DENY_WRITE    0x0020
#define SMBOPEN_DENY_READ     0x0030
#define SMBOPEN_DENY_NONE     0x0040
#define SMBOPEN_READ          0x0000
#define SMBOPEN_WRITE         0x0001
#define SMBOPEN_READWRITE     0x0002
#define SMBOPEN_EXECUTE       0x0003

#define SMBOPEN_OCREATE       0x0010
#define SMBOPEN_OTRUNC        0x0002
#define SMBOPEN_OAPPEND       0x0001

/*
 * SMB flag definitions 
 */
#define SMBFLG_EXTD_LOCK 0x01	/* server supports lock-read write-unlock primitives */
#define SMBFLG_RCV_POSTED 0x02	/* obsolete */
#define SMBFLG_RSVD 0x04
#define SMBFLG_CASELESS 0x08	/* all pathnames treated as caseless (off implies case sensitive file handling requested) */
#define SMBFLG_CANONICAL_PATH_FORMAT 0x10	/* obsolete */
#define SMBFLG_OLD_OPLOCK 0x20	/* obsolete */
#define SMBFLG_OLD_OPLOCK_NOTIFY 0x40	/* obsolete */
#define SMBFLG_RESPONSE 0x80	/* this PDU is a response from server */

/*
 * SMB flag2 definitions 
 */
#define SMBFLG2_KNOWS_LONG_NAMES 0x0001	/* can send long (non-8.3) path names in response */
#define SMBFLG2_KNOWS_EAS 0x0002
#define SMBFLG2_SECURITY_SIGNATURE 0x0004
#define SMBFLG2_IS_LONG_NAME 0x0040
#define SMBFLG2_EXT_SEC 0x0800
#define SMBFLG2_DFS 0x1000
#define SMBFLG2_PAGING_IO 0x2000
#define SMBFLG2_ERR_STATUS 0x4000
#define SMBFLG2_UNICODE 0x8000

/*
 * These are the file access permission bits defined in CIFS for the
 * NTCreateAndX as well as the level 0x107
 * TRANS2_QUERY_PATH_INFORMATION API.  The level 0x107, SMB_QUERY_FILE_ALL_INFO
 * responds with the AccessFlags.
 * The AccessFlags specifies the access permissions a caller has to the
 * file and can have any suitable combination of the following values:
 */

#define FILE_READ_DATA        0x00000001	/* Data can be read from the file   */
#define FILE_WRITE_DATA       0x00000002	/* Data can be written to the file  */
#define FILE_APPEND_DATA      0x00000004	/* Data can be appended to the file */
#define FILE_READ_EA          0x00000008	/* Extended attributes associated   */
					 /* with the file can be read        */
#define FILE_WRITE_EA         0x00000010	/* Extended attributes associated   */
					 /* with the file can be written     */
#define FILE_EXECUTE          0x00000020	/*Data can be read into memory from */
					 /* the file using system paging I/O */
#define FILE_DELETE_CHILD     0x00000040
#define FILE_READ_ATTRIBUTES  0x00000080	/* Attributes associated with the   */
					 /* file can be read                 */
#define FILE_WRITE_ATTRIBUTES 0x00000100	/* Attributes associated with the   */
					 /* file can be written              */
#define DELETE                0x00010000	/* The file can be deleted          */
#define READ_CONTROL          0x00020000	/* The access control list and      */
					 /* ownership associated with the    */
					 /* file can be read                 */
#define WRITE_DAC             0x00040000	/* The access control list and      */
					 /* ownership associated with the    */
					 /* file can be written.             */
#define WRITE_OWNER           0x00080000	/* Ownership information associated */
					 /* with the file can be written     */
#define SYNCHRONIZE           0x00100000	/* The file handle can waited on to */
					 /* synchronize with the completion  */
					 /* of an input/output request       */
#define GENERIC_ALL           0x10000000
#define GENERIC_EXECUTE       0x20000000
#define GENERIC_WRITE         0x40000000
#define GENERIC_READ          0x80000000
					 /* In summary - Relevant file       */
					 /* access flags from CIFS are       */
					 /* file_read_data, file_write_data  */
					 /* file_execute, file_read_attributes */
					 /* write_dac, and delete.           */

/*
 * Invalid readdir handle
 */
#define CIFS_NO_HANDLE        0xFFFF

/* IPC$ in ASCII */
#define CIFS_IPC_RESOURCE "\x49\x50\x43\x24"

/* IPC$ in Unicode */
#define CIFS_IPC_UNICODE_RESOURCE "\x00\x49\x00\x50\x00\x43\x00\x24\x00\x00"

/* Unicode Null terminate 2 bytes of 0 */
#define UNICODE_NULL "\x00\x00"
#define ASCII_NULL 0x00

/*
 * Server type values (returned on EnumServer API
 */
#define CIFS_SV_TYPE_DC     0x00000008
#define CIFS_SV_TYPE_BACKDC 0x00000010

/*
 * Alias type flags (From EnumAlias API call
 */
#define CIFS_ALIAS_TYPE_FILE 0x0001
#define CIFS_SHARE_TYPE_FILE 0x0000

/*
 * File Attribute flags
 */
#define ATTR_READONLY  0x0001
#define ATTR_HIDDEN    0x0002
#define ATTR_SYSTEM    0x0004
#define ATTR_VOLUME    0x0008
#define ATTR_DIRECTORY 0x0010
#define ATTR_ARCHIVE   0x0020
#define ATTR_DEVICE    0x0040
#define ATTR_NORMAL    0x0080
#define ATTR_TEMPORARY 0x0100
#define ATTR_SPARSE    0x0200
#define ATTR_REPARSE   0x0400
#define ATTR_COMPRESSED 0x0800
#define ATTR_OFFLINE    0x1000	/* ie file not immediately available - offline storage */
#define ATTR_NOT_CONTENT_INDEXED 0x2000
#define ATTR_ENCRYPTED  0x4000
#define ATTR_POSIX_SEMANTICS 0x01000000
#define ATTR_BACKUP_SEMANTICS 0x02000000
#define ATTR_DELETE_ON_CLOSE 0x04000000
#define ATTR_SEQUENTIAL_SCAN 0x08000000
#define ATTR_RANDOM_ACCESS   0x10000000
#define ATTR_NO_BUFFERING    0x20000000
#define ATTR_WRITE_THROUGH   0x80000000

/* ShareAccess flags */
#define FILE_NO_SHARE     0x00000000
#define FILE_SHARE_READ   0x00000001
#define FILE_SHARE_WRITE  0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define FILE_SHARE_ALL    0x00000007

/* CreateDisposition flags */
#define FILE_SUPERSEDE    0x00000000
#define FILE_OPEN         0x00000001
#define FILE_CREATE       0x00000002
#define FILE_OPEN_IF      0x00000003
#define FILE_OVERWRITE    0x00000004
#define FILE_OVERWRITE_IF 0x00000005

/* CreateOptions */
#define CREATE_NOT_FILE		0x00000001	/* if set must not be file */
#define CREATE_WRITE_THROUGH	0x00000002
#define CREATE_NOT_DIR		0x00000040	/* if set must not be directory */
#define CREATE_RANDOM_ACCESS	0x00000800
#define CREATE_DELETE_ON_CLOSE	0x00001000
#define OPEN_REPARSE_POINT	0x00200000

/* ImpersonationLevel flags */
#define SECURITY_ANONYMOUS      0
#define SECURITY_IDENTIFICATION 1
#define SECURITY_IMPERSONATION  2
#define SECURITY_DELEGATION     3

/* SecurityFlags */
#define SECURITY_CONTEXT_TRACKING 0x01
#define SECURITY_EFFECTIVE_ONLY   0x02

/*
 * Default PID value, used in all SMBs where the PID is not important
 */
#define CIFS_DFT_PID  0x1234

/*
 * We use the same routine for Copy and Move SMBs.  This flag is used to
 * distinguish
 */
#define CIFS_COPY_OP 1
#define CIFS_RENAME_OP 2

#define GETU16(var)  (*((__u16 *)var))	/* BB check for endian issues */
#define GETU32(var)  (*((__u32 *)var))	/* BB check for endian issues */

#pragma pack(1)

struct smb_hdr {
	__u32 smb_buf_length;	/* big endian on wire *//* BB length is only two or three bytes - with one or two byte type preceding it but that is always zero - we could mask the type byte off just in case BB */
	__u8 Protocol[4];
	__u8 Command;
	union {
		struct {
			__u8 ErrorClass;
			__u8 Reserved;
			__u16 Error;	/* note: treated as little endian (le) on wire */
		} DosError;
		__u32 CifsError;	/* note: le */
	} Status;
	__u8 Flags;
	__u16 Flags2;		/* note: le */
	__u16 PidHigh;		/* note: le */
	union {
		struct {
			__u32 SequenceNumber;  /* le */
			__u32 Reserved; /* zero */
		} Sequence;
		__u8 SecuritySignature[8];	/* le */
	} Signature;
	__u8 pad[2];
	__u16 Tid;
	__u16 Pid;		/* note: le */
	__u16 Uid;
	__u16 Mid;
	__u8 WordCount;
};
/* given a pointer to an smb_hdr retrieve the value of byte count */
#define BCC(smb_var) ( *(__u16 *)((char *)smb_var + sizeof(struct smb_hdr) + (2* smb_var->WordCount) ) )

/* given a pointer to an smb_hdr retrieve the pointer to the byte area */
#define pByteArea(smb_var) ((char *)smb_var + sizeof(struct smb_hdr) + (2* smb_var->WordCount) + 2 )

/*
 * Computer Name Length
 */
#define CNLEN 15

/*
 * Share Name Length					  @S8A
 * Note:  This length is limited by the SMB used to get   @S8A
 *        the Share info.   NetShareEnum only returns 13  @S8A
 *        chars, including the null termination.          @S8A 
 */
#define SNLEN 12		/*@S8A */

/*
 * Comment Length
 */
#define MAXCOMMENTLEN 40

/*
 * The OS/2 maximum path name
 */
#define MAX_PATHCONF 256

/*
 *  SMB frame definitions  (following must be packed structs)
 *  See the SNIA CIFS Specification for details.
 *
 *  The Naming convention is the lower case version of the
 *  smb command code name for the struct and this is typedef to the
 *  uppercase version of the same name with the prefix SMB_ removed 
 *  for brevity.  Although typedefs are not commonly used for 
 *  structure definitions in the Linux kernel, their use in the
 *  CIFS standards document, which this code is based on, may
 *  make this one of the cases where typedefs for structures make
 *  sense to improve readability for readers of the standards doc.
 *  Typedefs can always be removed later if they are too distracting
 *  and they are only used for the CIFSs PDUs themselves, not
 *  internal cifs vfs structures
 *  
 */

typedef struct negotiate_req {
	struct smb_hdr hdr;	/* wct = 0 */
	__u16 ByteCount;
	unsigned char DialectsArray[1];
} NEGOTIATE_REQ;

typedef struct negotiate_rsp {
	struct smb_hdr hdr;	/* wct = 17 */
	__u16 DialectIndex;
	__u8 SecurityMode;
	__u16 MaxMpxCount;
	__u16 MaxNumberVcs;
	__u32 MaxBufferSize;
	__u32 MaxRawSize;
	__u32 SessionKey;
	__u32 Capabilities;	/* see below */
	__u32 SystemTimeLow;
	__u32 SystemTimeHigh;
	__u16 ServerTimeZone;
	__u8 EncryptionKeyLength;
	__u16 ByteCount;
	union {
		unsigned char EncryptionKey[1];	/* if cap extended security is off */
		/* followed by Domain name - if extended security is off */
		/* followed by 16 bytes of server GUID */
		/* followed by security blob if cap_extended_security negotiated */
		struct {
			unsigned char GUID[16];
			unsigned char SecurityBlob[1];
		} extended_response;
	} u;
} NEGOTIATE_RSP;

/* SecurityMode bits */
#define SECMODE_USER          0x01	/* off indicates share level security */
#define SECMODE_PW_ENCRYPT    0x02
#define SECMODE_SIGN_ENABLED  0x04	/* SMB security signatures enabled */
#define SECMODE_SIGN_REQUIRED 0x08	/* SMB security signatures required */

/* Negotiate response Capabilities */
#define CAP_RAW_MODE           0x00000001
#define CAP_MPX_MODE           0x00000002
#define CAP_UNICODE            0x00000004
#define CAP_LARGE_FILES        0x00000008
#define CAP_NT_SMBS            0x00000010	/* implies CAP_NT_FIND */
#define CAP_RPC_REMOTE_APIS    0x00000020
#define CAP_STATUS32           0x00000040
#define CAP_LEVEL_II_OPLOCKS   0x00000080
#define CAP_LOCK_AND_READ      0x00000100
#define CAP_NT_FIND            0x00000200
#define CAP_DFS                0x00001000
#define CAP_INFOLEVEL_PASSTHRU 0x00002000
#define CAP_LARGE_READ_X       0x00004000
#define CAP_LARGE_WRITE_X      0x00008000
#define CAP_UNIX               0x00800000
#define CAP_RESERVED           0x02000000
#define CAP_BULK_TRANSFER      0x20000000
#define CAP_COMPRESSED_DATA    0x40000000
#define CAP_EXTENDED_SECURITY  0x80000000

typedef union smb_com_session_setup_andx {
	struct {		/* request format */
		struct smb_hdr hdr;	/* wct = 12 */
		__u8 AndXCommand;
		__u8 AndXReserved;
		__u16 AndXOffset;
		__u16 MaxBufferSize;
		__u16 MaxMpxCount;
		__u16 VcNumber;
		__u32 SessionKey;
		__u16 SecurityBlobLength;
		__u32 Reserved;
		__u32 Capabilities;	/* see below */
		__u16 ByteCount;
		unsigned char SecurityBlob[1];	/* followed by */
		/* STRING NativeOS */
		/* STRING NativeLanMan */
	} req;			/* NTLM request format (with extended security */

	struct {		/* request format */
		struct smb_hdr hdr;	/* wct = 13 */
		__u8 AndXCommand;
		__u8 AndXReserved;
		__u16 AndXOffset;
		__u16 MaxBufferSize;
		__u16 MaxMpxCount;
		__u16 VcNumber;
		__u32 SessionKey;
		__u16 CaseInsensitivePasswordLength;	/* ASCII password length */
		__u16 CaseSensitivePasswordLength;	/* Unicode password length */
		__u32 Reserved;	/* see below */
		__u32 Capabilities;
		__u16 ByteCount;
		unsigned char CaseInsensitivePassword[1];	/* followed by: */
		/* unsigned char * CaseSensitivePassword; */
		/* STRING AccountName */
		/* STRING PrimaryDomain */
		/* STRING NativeOS */
		/* STRING NativeLanMan */
	} req_no_secext;	/* NTLM request format (without extended security */

	struct {		/* default (NTLM) response format */
		struct smb_hdr hdr;	/* wct = 4 */
		__u8 AndXCommand;
		__u8 AndXReserved;
		__u16 AndXOffset;
		__u16 Action;	/* see below */
		__u16 SecurityBlobLength;
		__u16 ByteCount;
		unsigned char SecurityBlob[1];	/* followed by */
/*      unsigned char  * NativeOS;      */
/*	unsigned char  * NativeLanMan;  */
/*      unsigned char  * PrimaryDomain; */
	} resp;			/* NTLM response format (with or without extended security */

	struct {		/* request format */
		struct smb_hdr hdr;	/* wct = 10 */
		__u8 AndXCommand;
		__u8 AndXReserved;
		__u16 AndXOffset;
		__u16 MaxBufferSize;
		__u16 MaxMpxCount;
		__u16 VcNumber;
		__u32 SessionKey;
		__u16 PassswordLength;
		__u32 Reserved;
		__u16 ByteCount;
		unsigned char AccountPassword[1];	/* followed by */
		/* STRING AccountName */
		/* STRING PrimaryDomain */
		/* STRING NativeOS */
		/* STRING NativeLanMan */
	} old_req;		/* pre-NTLM (LANMAN2.1) request format */

	struct {		/* default (NTLM) response format */
		struct smb_hdr hdr;	/* wct = 3 */
		__u8 AndXCommand;
		__u8 AndXReserved;
		__u16 AndXOffset;
		__u16 Action;	/* see below */
		__u16 ByteCount;
		unsigned char NativeOS[1];	/* followed by */
/*	unsigned char * NativeLanMan; */
/*      unsigned char * PrimaryDomain; */
	} old_resp;		/* pre-NTLM (LANMAN2.1) response format */
} SESSION_SETUP_ANDX;

#define CIFS_NETWORK_OPSYS "CIFS VFS Client for Linux"

/* Capabilities bits (for NTLM SessSetup request) */
#define CAP_UNICODE            0x00000004
#define CAP_LARGE_FILES        0x00000008
#define CAP_NT_SMBS            0x00000010
#define CAP_STATUS32           0x00000040
#define CAP_LEVEL_II_OPLOCKS   0x00000080
#define CAP_NT_FIND            0x00000200	/* reserved should be zero (presumably because NT_SMBs implies the same thing) */
#define CAP_BULK_TRANSFER      0x20000000
#define CAP_EXTENDED_SECURITY  0x80000000

/* Action bits */
#define GUEST_LOGIN 1

typedef struct smb_com_tconx_req {
	struct smb_hdr hdr;	/* wct = 4 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 Flags;		/* see below */
	__u16 PasswordLength;
	__u16 ByteCount;
	unsigned char Password[1];	/* followed by */
/* STRING Path    *//* \\server\share name */
	/* STRING Service */
} TCONX_REQ;

typedef struct smb_com_tconx_rsp {
	struct smb_hdr hdr;	/* wct = 3 *//* note that Win2000 has sent wct=7 in some cases on responses. Four unspecified words followed OptionalSupport */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 OptionalSupport;	/* see below */
	__u16 ByteCount;
	unsigned char Service[1];	/* always ASCII, not Unicode */
	/* STRING NativeFileSystem */
} TCONX_RSP;

/* tree connect Flags */
#define DISCONNECT_TID          0x0001
#define TCON_EXTENDED_SECINFO   0x0008
/* OptionalSupport bits */
#define SMB_SUPPORT_SEARCH_BITS 0x0001	/* must have bits (exclusive searches suppt. */
#define SMB_SHARE_IS_IN_DFS     0x0002

typedef struct smb_com_logoff_andx_req {

	struct smb_hdr hdr;	/* wct = 2 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 ByteCount;
} LOGOFF_ANDX_REQ;

typedef struct smb_com_logoff_andx_rsp {
	struct smb_hdr hdr;	/* wct = 2 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 ByteCount;
} LOGOFF_ANDX_RSP;

typedef union smb_com_tree_disconnect {	/* as an altetnative can use flag on tree_connect PDU to effect disconnect *//* probably the simplest SMB PDU */
	struct {
		struct smb_hdr hdr;	/* wct = 0 */
		__u16 ByteCount;	/* bcc = 0 */
	} req;
	struct {
		struct smb_hdr hdr;	/* wct = 0 */
		__u16 ByteCount;	/* bcc = 0 */
	} resp;
} TREE_DISCONNECT;

typedef struct smb_com_close_req {
	struct smb_hdr hdr;	/* wct = 3 */
	__u16 FileID;
	__u32 LastWriteTime;	/* should be zero */
	__u16 ByteCount;	/* 0 */
} CLOSE_REQ;

typedef struct smb_com_close_rsp {
	struct smb_hdr hdr;	/* wct = 0 */
	__u16 ByteCount;	/* bct = 0 */
} CLOSE_RSP;

typedef struct smb_com_findclose_req {
	struct smb_hdr hdr; /* wct = 1 */
	__u16 FileID;
	__u16 ByteCount;    /* 0 */
} FINDCLOSE_REQ;

/* OpenFlags */
#define REQ_OPLOCK         0x00000002
#define REQ_BATCHOPLOCK    0x00000004
#define REQ_OPENDIRONLY    0x00000008

typedef struct smb_com_open_req {	/* also handles create */
	struct smb_hdr hdr;	/* wct = 24 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u8 Reserved;		/* Must Be Zero */
	__u16 NameLength;
	__u32 OpenFlags;
	__u32 RootDirectoryFid;
	__u32 DesiredAccess;
	__u64 AllocationSize;
	__u32 FileAttributes;
	__u32 ShareAccess;
	__u32 CreateDisposition;
	__u32 CreateOptions;
	__u32 ImpersonationLevel;
	__u8 SecurityFlags;
	__u16 ByteCount;
	char fileName[1];
} OPEN_REQ;

/* open response: oplock levels */
#define OPLOCK_NONE  	 0
#define OPLOCK_EXCLUSIVE 1
#define OPLOCK_BATCH	 2
#define OPLOCK_READ	 3  /* level 2 oplock */

/* open response for CreateAction shifted left */
#define CIFS_CREATE_ACTION 0x20000 /* file created */

typedef struct smb_com_open_rsp {
	struct smb_hdr hdr;	/* wct = 34 BB */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u8 OplockLevel;
	__u16 Fid;
	__u32 CreateAction;
	__u64 CreationTime;
	__u64 LastAccessTime;
	__u64 LastWriteTime;
	__u64 ChangeTime;
	__u32 FileAttributes;
	__u64 AllocationSize;
	__u64 EndOfFile;
	__u16 FileType;
	__u16 DeviceState;
	__u8 DirectoryFlag;
	__u16 ByteCount;	/* bct = 0 */
} OPEN_RSP;

typedef struct smb_com_write_req {
	struct smb_hdr hdr;	/* wct = 14 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 Fid;
	__u32 OffsetLow;
	__u32 Reserved;
	__u16 WriteMode;
	__u16 Remaining;
	__u16 DataLengthHigh;
	__u16 DataLengthLow;
	__u16 DataOffset;
	__u32 OffsetHigh;
	__u16 ByteCount;
	__u8 Pad;		/* BB check for whether padded to DWORD boundary and optimum performance here */
	char Data[1];
} WRITE_REQ;

typedef struct smb_com_write_rsp {
	struct smb_hdr hdr;	/* wct = 6 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 Count;
	__u16 Remaining;
	__u32 Reserved;
	__u16 ByteCount;
} WRITE_RSP;

typedef struct smb_com_read_req {
	struct smb_hdr hdr;	/* wct = 12 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 Fid;
	__u32 OffsetLow;
	__u16 MaxCount;
	__u16 MinCount;		/* obsolete */
	__u32 MaxCountHigh;
	__u16 Remaining;
	__u32 OffsetHigh;
	__u16 ByteCount;
} READ_REQ;

typedef struct smb_com_read_rsp {
	struct smb_hdr hdr;	/* wct = 12 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 Remaining;
	__u16 DataCompactionMode;
	__u16 Reserved;
	__u16 DataLength;
	__u16 DataOffset;
	__u16 DataLengthHigh;
	__u64 Reserved2;
	__u16 ByteCount;
	__u8 Pad;		/* BB check for whether padded to DWORD boundary and optimum performance here */
	char Data[1];
} READ_RSP;

typedef struct locking_andx_range {
	__u16 Pid;
	__u16 Pad;
	__u32 OffsetHigh;
	__u32 OffsetLow;
	__u32 LengthHigh;
	__u32 LengthLow;
} LOCKING_ANDX_RANGE;

#define LOCKING_ANDX_SHARED_LOCK     0x01
#define LOCKING_ANDX_OPLOCK_RELEASE  0x02
#define LOCKING_ANDX_CHANGE_LOCKTYPE 0x04
#define LOCKING_ANDX_CANCEL_LOCK     0x08
#define LOCKING_ANDX_LARGE_FILES     0x10	/* always on for us */

typedef struct smb_com_lock_req {
	struct smb_hdr hdr;	/* wct = 8 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 Fid;
	__u8 LockType;
	__u8 OplockLevel;
	__u32 Timeout;
	__u16 NumberOfUnlocks;
	__u16 NumberOfLocks;
	__u16 ByteCount;
	LOCKING_ANDX_RANGE Locks[1];
} LOCK_REQ;

typedef struct smb_com_lock_rsp {
	struct smb_hdr hdr;	/* wct = 2 */
	__u8 AndXCommand;
	__u8 AndXReserved;
	__u16 AndXOffset;
	__u16 ByteCount;
} LOCK_RSP;

typedef struct smb_com_rename_req {
	struct smb_hdr hdr;	/* wct = 1 */
	__u16 SearchAttributes;	/* target file attributes */
	__u16 ByteCount;
	__u8 BufferFormat;	/* 4 = ASCII or Unicode */
	unsigned char OldFileName[1];
	/* followed by __u8 BufferFormat2 */
	/* followed by NewFileName */
} RENAME_REQ;

	/* copy request flags */
#define COPY_MUST_BE_FILE      0x0001
#define COPY_MUST_BE_DIR       0x0002
#define COPY_TARGET_MODE_ASCII 0x0004 /* if not set, binary */
#define COPY_SOURCE_MODE_ASCII 0x0008 /* if not set, binary */
#define COPY_VERIFY_WRITES     0x0010
#define COPY_TREE              0x0020 

typedef struct smb_com_copy_req {
	struct smb_hdr hdr;	/* wct = 3 */
	__u16 Tid2;
	__u16 OpenFunction;
	__u16 Flags;
	__u16 ByteCount;
	__u8 BufferFormat;	/* 4 = ASCII or Unicode */ 
	unsigned char OldFileName[1];
	/* followed by __u8 BufferFormat2 */
	/* followed by NewFileName string */
} COPY_REQ;

typedef struct smb_com_copy_rsp {
	struct smb_hdr hdr;     /* wct = 1 */
	__u16 CopyCount;    /* number of files copied */
	__u16 ByteCount;    /* may be zero */
	__u8 BufferFormat;  /* 0x04 - only present if errored file follows */
	unsigned char ErrorFileName[1]; /* only present if error in copy */
} COPY_RSP;

#define CREATE_HARD_LINK		0x103
#define MOVEFILE_COPY_ALLOWED		0x0002
#define MOVEFILE_REPLACE_EXISTING	0x0001

typedef struct smb_com_nt_rename_req {	/* A5 - also used for create hardlink */
	struct smb_hdr hdr;	/* wct = 4 */
	__u16 SearchAttributes;	/* target file attributes */
	__u16 Flags;		/* spec says Information Level */
	__u32 ClusterCount;
	__u16 ByteCount;
	__u8 BufferFormat;	/* 4 = ASCII or Unicode */
	unsigned char OldFileName[1];
	/* followed by __u8 BufferFormat2 */
	/* followed by NewFileName */
} NT_RENAME_REQ;

typedef struct smb_com_rename_rsp {
	struct smb_hdr hdr;	/* wct = 0 */
	__u16 ByteCount;	/* bct = 0 */
} RENAME_RSP;

typedef struct smb_com_delete_file_req {
	struct smb_hdr hdr;	/* wct = 1 */
	__u16 SearchAttributes;
	__u16 ByteCount;
	__u8 BufferFormat;	/* 4 = ASCII */
	unsigned char fileName[1];
} DELETE_FILE_REQ;

typedef struct smb_com_delete_file_rsp {
	struct smb_hdr hdr;	/* wct = 0 */
	__u16 ByteCount;	/* bct = 0 */
} DELETE_FILE_RSP;

typedef struct smb_com_delete_directory_req {
	struct smb_hdr hdr;	/* wct = 0 */
	__u16 ByteCount;
	__u8 BufferFormat;	/* 4 = ASCII */
	unsigned char DirName[1];
} DELETE_DIRECTORY_REQ;

typedef struct smb_com_delete_directory_rsp {
	struct smb_hdr hdr;	/* wct = 0 */
	__u16 ByteCount;	/* bct = 0 */
} DELETE_DIRECTORY_RSP;

typedef struct smb_com_create_directory_req {
	struct smb_hdr hdr;	/* wct = 0 */
	__u16 ByteCount;
	__u8 BufferFormat;	/* 4 = ASCII */
	unsigned char DirName[1];
} CREATE_DIRECTORY_REQ;

typedef struct smb_com_create_directory_rsp {
	struct smb_hdr hdr;	/* wct = 0 */
	__u16 ByteCount;	/* bct = 0 */
} CREATE_DIRECTORY_RSP;

/***************************************************/
/* NT Transact structure defintions follow         */
/* Currently only ioctl and notify are implemented */
/***************************************************/
typedef struct smb_com_transaction_ioctl_req {
	struct smb_hdr hdr;	/* wct = 23 */
	__u8 MaxSetupCount;
	__u16 Reserved;
	__u32 TotalParameterCount;
	__u32 TotalDataCount;
	__u32 MaxParameterCount;
	__u32 MaxDataCount;
	__u32 ParameterCount;
	__u32 ParameterOffset;
	__u32 DataCount;
	__u32 DataOffset;
	__u8 SetupCount; /* four setup words follow subcommand */
	/* SNIA spec incorrectly included spurious pad here */
	__u16 SubCommand;/* 2 = IOCTL/FSCTL */
	__u32 FunctionCode;
	__u16 Fid;
	__u8 IsFsctl;    /* 1 = File System Control, 0 = device control (IOCTL)*/
	__u8 IsRootFlag; /* 1 = apply command to root of share (must be DFS share)*/
	__u16 ByteCount;
	__u8 Pad[3];
	__u8 Data[1];
} TRANSACT_IOCTL_REQ;

typedef struct smb_com_transaction_ioctl_rsp {
	struct smb_hdr hdr;	/* wct = 19 */
	__u8 Reserved[3];
	__u32 TotalParameterCount;
	__u32 TotalDataCount;
	__u32 ParameterCount;
	__u32 ParameterOffset;
	__u32 ParameterDisplacement;
	__u32 DataCount;
	__u32 DataOffset;
	__u32 DataDisplacement;
	__u8 SetupCount;	/* 1 */
	__u16 ReturnedDataLen;
	__u16 ByteCount;
	__u8 Pad[3];
} TRANSACT_IOCTL_RSP;

typedef struct smb_com_transaction_change_notify_req {
	struct smb_hdr hdr;     /* wct = 23 */
	__u8 MaxSetupCount;
	__u16 Reserved;
	__u32 TotalParameterCount;
	__u32 TotalDataCount;
	__u32 MaxParameterCount;
	__u32 MaxDataCount;
	__u32 ParameterCount;
	__u32 ParameterOffset;
	__u32 DataCount;
	__u32 DataOffset;
	__u8 SetupCount; /* four setup words follow subcommand */
	/* SNIA spec incorrectly included spurious pad here */
	__u16 SubCommand;/* 4 = Change Notify */
	__u32 CompletionFilter;  /* operation to monitor */
	__u16 Fid;
	__u8 WatchTree;  /* 1 = Monitor subdirectories */
	__u8 Reserved2;
	__u16 ByteCount;
/* __u8 Pad[3];*/
/*	__u8 Data[1];*/
} TRANSACT_CHANGE_NOTIFY_REQ;

typedef struct smb_com_transaction_change_notify_rsp {
	struct smb_hdr hdr;	/* wct = 18 */
	__u8 Reserved[3];
	__u32 TotalParameterCount;
	__u32 TotalDataCount;
	__u32 ParameterCount;
	__u32 ParameterOffset;
	__u32 ParameterDisplacement;
	__u32 DataCount;
	__u32 DataOffset;
	__u32 DataDisplacement;
	__u8 SetupCount;   /* 0 */
	__u16 ByteCount;
	/* __u8 Pad[3]; */
} TRANSACT_CHANGE_NOTIFY_RSP;
/* Completion Filter flags for Notify */
#define FILE_NOTIFY_CHANGE_FILE_NAME    0x00000001
#define FILE_NOTIFY_CHANGE_DIR_NAME     0x00000002
#define FILE_NOTIFY_CHANGE_NAME         0x00000003
#define FILE_NOTIFY_CHANGE_ATTRIBUTES   0x00000004
#define FILE_NOTIFY_CHANGE_SIZE         0x00000008
#define FILE_NOTIFY_CHANGE_LAST_WRITE   0x00000010
#define FILE_NOTIFY_CHANGE_LAST_ACCESS  0x00000020
#define FILE_NOTIFY_CHANGE_CREATION     0x00000040
#define FILE_NOTIFY_CHANGE_EA           0x00000080
#define FILE_NOTIFY_CHANGE_SECURITY     0x00000100
#define FILE_NOTIFY_CHANGE_STREAM_NAME  0x00000200
#define FILE_NOTIFY_CHANGE_STREAM_SIZE  0x00000400
#define FILE_NOTIFY_CHANGE_STREAM_WRITE 0x00000800

#define FILE_ACTION_ADDED		0x00000001
#define FILE_ACTION_REMOVED		0x00000002
#define FILE_ACTION_MODIFIED		0x00000003
#define FILE_ACTION_RENAMED_OLD_NAME	0x00000004
#define FILE_ACTION_RENAMED_NEW_NAME	0x00000005
#define FILE_ACTION_ADDED_STREAM	0x00000006
#define FILE_ACTION_REMOVED_STREAM	0x00000007
#define FILE_ACTION_MODIFIED_STREAM	0x00000008

/* response contains array of the following structures */
struct file_notify_information {
	__u32 NextEntryOffset;
	__u32 Action;
	__u32 FileNameLength;
	__u8  FileName[1];
}; 

struct reparse_data {
	__u32	ReparseTag;
	__u16	ReparseDataLength;
	__u16	Reserved;
	__u16	AltNameOffset;
	__u16	AltNameLen;
	__u16	TargetNameOffset;
	__u16	TargetNameLen;
	char	LinkNamesBuf[1];
};

struct cifs_quota_data {
	__u32	rsrvd1;  /* 0 */
	__u32	sid_size;
	__u64	rsrvd2;  /* 0 */
	__u64	space_used;
	__u64	soft_limit;
	__u64	hard_limit;
	char	sid[1];  /* variable size? */
};

/* quota sub commands */
#define QUOTA_LIST_CONTINUE	    0
#define QUOTA_LIST_START	0x100
#define QUOTA_FOR_SID		0x101

typedef union smb_com_transaction2 {
	struct {
		struct smb_hdr hdr;	/* wct = 14+ */
		__u16 TotalParameterCount;
		__u16 TotalDataCount;
		__u16 MaxParameterCount;
		__u16 MaxDataCount;
		__u8 MaxSetupCount;
		__u8 Reserved;
		__u16 Flags;
		__u32 Timeout;
		__u16 Reserved2;
		__u16 ParameterCount;
		__u16 ParameterOffset;
		__u16 DataCount;
		__u16 DataOffset;
		__u8 SetupCount;
		__u8 Reserved3;
		__u16 SubCommand;	/* 1st setup word - can be followed by SetupCount words */
		__u16 ByteCount;	/* careful - setupcount is not always one */
	} req;
	struct {
		struct smb_hdr hdr;	/* wct = 0 */
		__u16 TotalParameterCount;
		__u16 TotalDataCount;
		__u16 Reserved;
		__u16 ParameterCount;
		__u16 ParamterOffset;
		__u16 ParameterDisplacement;
		__u16 DataCount;
		__u16 DataOffset;
		__u16 DataDisplacement;
		__u8 SetupCount;
		__u8 Reserved1;	/* should be zero setup words following */
		__u16 ByteCount;
		__u16 Reserved2;	/* parameter word reserved - present for infolevels > 100 */
		/* data area follows */
	} resp;
} TRANSACTION2;

/* PathInfo/FileInfo infolevels */
#define SMB_INFO_STANDARD                   1
#define SMB_SET_FILE_EA                     2
#define SMB_QUERY_FILE_EA_SIZE              2
#define SMB_INFO_QUERY_EAS_FROM_LIST        3
#define SMB_INFO_QUERY_ALL_EAS              4
#define SMB_INFO_IS_NAME_VALID              6
#define SMB_QUERY_FILE_BASIC_INFO       0x101
#define SMB_QUERY_FILE_STANDARD_INFO    0x102
#define SMB_QUERY_FILE_EA_INFO          0x103
#define SMB_QUERY_FILE_NAME_INFO        0x104
#define SMB_QUERY_FILE_ALLOCATION_INFO  0x105
#define SMB_QUERY_FILE_END_OF_FILEINFO  0x106
#define SMB_QUERY_FILE_ALL_INFO         0x107
#define SMB_QUERY_ALT_NAME_INFO         0x108
#define SMB_QUERY_FILE_STREAM_INFO      0x109
#define SMB_QUERY_FILE_COMPRESSION_INFO 0x10B
#define SMB_QUERY_FILE_UNIX_BASIC       0x200
#define SMB_QUERY_FILE_UNIX_LINK        0x201

#define SMB_SET_FILE_BASIC_INFO	        0x101
#define SMB_SET_FILE_DISPOSITION_INFO   0x102
#define SMB_SET_FILE_ALLOCATION_INFO    0x103
#define SMB_SET_FILE_END_OF_FILE_INFO   0x104
#define SMB_SET_FILE_UNIX_BASIC         0x200
#define SMB_SET_FILE_UNIX_LINK          0x201
#define SMB_SET_FILE_UNIX_HLINK         0x203
#define SMB_SET_FILE_BASIC_INFO2        0x3ec
#define SMB_SET_FILE_RENAME_INFORMATION 0x3f2
#define SMB_FILE_ALL_INFO2              0x3fa
#define SMB_SET_FILE_ALLOCATION_INFO2   0x3fb
#define SMB_SET_FILE_END_OF_FILE_INFO2  0x3fc
#define SMB_FILE_MOVE_CLUSTER_INFO      0x407
#define SMB_FILE_QUOTA_INFO             0x408
#define SMB_FILE_REPARSEPOINT_INFO      0x409
#define SMB_FILE_MAXIMUM_INFO           0x40d

/* Find File infolevels */
#define SMB_FIND_FILE_DIRECTORY_INFO      0x101
#define SMB_FIND_FILE_FULL_DIRECTORY_INFO 0x102
#define SMB_FIND_FILE_NAMES_INFO          0x103
#define SMB_FIND_FILE_BOTH_DIRECTORY_INFO 0x104
#define SMB_FIND_FILE_UNIX                0x202

typedef struct smb_com_transaction2_qpi_req {
	struct smb_hdr hdr;	/* wct = 14+ */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 MaxParameterCount;
	__u16 MaxDataCount;
	__u8 MaxSetupCount;
	__u8 Reserved;
	__u16 Flags;
	__u32 Timeout;
	__u16 Reserved2;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 DataCount;
	__u16 DataOffset;
	__u8 SetupCount;
	__u8 Reserved3;
	__u16 SubCommand;	/* one setup word */
	__u16 ByteCount;
	__u8 Pad;
	__u16 InformationLevel;
	__u32 Reserved4;
	char FileName[1];
} TRANSACTION2_QPI_REQ;

typedef struct smb_com_transaction2_qpi_rsp {
	struct smb_hdr hdr;	/* wct = 10 + SetupCount */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 Reserved;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 ParameterDisplacement;
	__u16 DataCount;
	__u16 DataOffset;
	__u16 DataDisplacement;
	__u8 SetupCount;
	__u8 Reserved1;		/* should be zero setup words following */
	__u16 ByteCount;
	__u16 Reserved2;	/* parameter word reserved - present for infolevels > 100 */
} TRANSACTION2_QPI_RSP;

typedef struct smb_com_transaction2_spi_req {
	struct smb_hdr hdr;	/* wct = 15 */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 MaxParameterCount;
	__u16 MaxDataCount;
	__u8 MaxSetupCount;
	__u8 Reserved;
	__u16 Flags;
	__u32 Timeout;
	__u16 Reserved2;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 DataCount;
	__u16 DataOffset;
	__u8 SetupCount;
	__u8 Reserved3;
	__u16 SubCommand;	/* one setup word */
	__u16 ByteCount;
	__u8 Pad;
	__u16 Pad1;
	__u16 InformationLevel;
	__u32 Reserved4;
	char FileName[1];
} TRANSACTION2_SPI_REQ;

typedef struct smb_com_transaction2_spi_rsp {
	struct smb_hdr hdr;	/* wct = 10 + SetupCount */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 Reserved;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 ParameterDisplacement;
	__u16 DataCount;
	__u16 DataOffset;
	__u16 DataDisplacement;
	__u8 SetupCount;
	__u8 Reserved1;		/* should be zero setup words following */
	__u16 ByteCount;
	__u16 Reserved2;	/* parameter word reserved - present for infolevels > 100 */
} TRANSACTION2_SPI_RSP;

struct set_file_rename {
	__u32 overwrite;   /* 1 = overwrite dest */
	__u32 root_fid;   /* zero */
	__u32 target_name_len;
	char  target_name[0];  /* Must be unicode */
};

struct smb_com_transaction2_sfi_req {
	struct smb_hdr hdr;	/* wct = 15 */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 MaxParameterCount;
	__u16 MaxDataCount;
	__u8 MaxSetupCount;
	__u8 Reserved;
	__u16 Flags;
	__u32 Timeout;
	__u16 Reserved2;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 DataCount;
	__u16 DataOffset;
	__u8 SetupCount;
	__u8 Reserved3;
	__u16 SubCommand;	/* one setup word */
	__u16 ByteCount;
	__u8 Pad;
	__u16 Pad1;
	__u16 Fid;
	__u16 InformationLevel;
	__u16 Reserved4;	
};

struct smb_com_transaction2_sfi_rsp {
	struct smb_hdr hdr;	/* wct = 10 + SetupCount */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 Reserved;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 ParameterDisplacement;
	__u16 DataCount;
	__u16 DataOffset;
	__u16 DataDisplacement;
	__u8 SetupCount;
	__u8 Reserved1;		/* should be zero setup words following */
	__u16 ByteCount;
	__u16 Reserved2;	/* parameter word reserved - present for infolevels > 100 */
};


/*
 * Flags on T2 FINDFIRST and FINDNEXT 
 */
#define CIFS_SEARCH_CLOSE_ALWAYS  0x0001
#define CIFS_SEARCH_CLOSE_AT_END  0x0002
#define CIFS_SEARCH_RETURN_RESUME 0x0004
#define CIFS_SEARCH_CONTINUE_FROM_LAST 0x0008
#define CIFS_SEARCH_BACKUP_SEARCH 0x0010

/*
 * Size of the resume key on FINDFIRST and FINDNEXT calls
 */
#define CIFS_SMB_RESUME_KEY_SIZE 4

typedef struct smb_com_transaction2_ffirst_req {
	struct smb_hdr hdr;	/* wct = 15 */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 MaxParameterCount;
	__u16 MaxDataCount;
	__u8 MaxSetupCount;
	__u8 Reserved;
	__u16 Flags;
	__u32 Timeout;
	__u16 Reserved2;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 DataCount;
	__u16 DataOffset;
	__u8 SetupCount;	/* one */
	__u8 Reserved3;
	__u16 SubCommand;	/* TRANS2_FIND_FIRST */
	__u16 ByteCount;
	__u8 Pad;
	__u16 SearchAttributes;
	__u16 SearchCount;
	__u16 SearchFlags;
	__u16 InformationLevel;
	__u32 SearchStorageType;
	char FileName[1];
} TRANSACTION2_FFIRST_REQ;

typedef struct smb_com_transaction2_ffirst_rsp {
	struct smb_hdr hdr;	/* wct = 10 */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 Reserved;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 ParameterDisplacement;
	__u16 DataCount;
	__u16 DataOffset;
	__u16 DataDisplacement;
	__u8 SetupCount;
	__u8 Reserved1;		/* should be zero setup words following */
	__u16 ByteCount;
} TRANSACTION2_FFIRST_RSP;

typedef struct smb_com_transaction2_ffirst_rsp_parms {
	__u16 SearchHandle;
	__u16 SearchCount;
	__u16 EndofSearch;
	__u16 EAErrorOffset;
	__u16 LastNameOffset;
} T2_FFIRST_RSP_PARMS;

typedef struct smb_com_transaction2_fnext_req {
	struct smb_hdr hdr;	/* wct = 15 */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 MaxParameterCount;
	__u16 MaxDataCount;
	__u8 MaxSetupCount;
	__u8 Reserved;
	__u16 Flags;
	__u32 Timeout;
	__u16 Reserved2;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 DataCount;
	__u16 DataOffset;
	__u8 SetupCount;	/* one */
	__u8 Reserved3;
	__u16 SubCommand;	/* TRANS2_FIND_NEXT */
	__u16 ByteCount;
	__u8 Pad;
	__u16 SearchHandle;
	__u16 SearchCount;
	__u16 InformationLevel;
	__u32 ResumeKey;
	__u16 SearchFlags;
	char ResumeFileName[1];
} TRANSACTION2_FNEXT_REQ;

typedef struct smb_com_transaction2_fnext_rsp {
	struct smb_hdr hdr;	/* wct = 10 */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 Reserved;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 ParameterDisplacement;
	__u16 DataCount;
	__u16 DataOffset;
	__u16 DataDisplacement;
	__u8 SetupCount;
	__u8 Reserved1;		/* should be zero setup words following */
	__u16 ByteCount;
} TRANSACTION2_FNEXT_RSP;

typedef struct smb_com_transaction2_fnext_rsp_parms {
	__u16 SearchCount;
	__u16 EndofSearch;
	__u16 EAErrorOffset;
	__u16 LastNameOffset;
} T2_FNEXT_RSP_PARMS;

/* QFSInfo Levels */
#define SMB_INFO_ALLOCATION         1
#define SMB_INFO_VOLUME             2
#define SMB_QUERY_FS_VOLUME_INFO    0x102
#define SMB_QUERY_FS_SIZE_INFO      0x103
#define SMB_QUERY_FS_DEVICE_INFO    0x104
#define SMB_QUERY_FS_ATTRIBUTE_INFO 0x105
#define SMB_QUERY_CIFS_UNIX_INFO    0x200
#define SMB_QUERY_LABEL_INFO        0x3ea
#define SMB_QUERY_FS_QUOTA_INFO     0x3ee

typedef struct smb_com_transaction2_qfsi_req {
	struct smb_hdr hdr;	/* wct = 14+ */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 MaxParameterCount;
	__u16 MaxDataCount;
	__u8 MaxSetupCount;
	__u8 Reserved;
	__u16 Flags;
	__u32 Timeout;
	__u16 Reserved2;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 DataCount;
	__u16 DataOffset;
	__u8 SetupCount;
	__u8 Reserved3;
	__u16 SubCommand;	/* one setup word */
	__u16 ByteCount;
	__u8 Pad;
	__u16 InformationLevel;
} TRANSACTION2_QFSI_REQ;

typedef struct smb_com_transaction_qfsi_rsp {
	struct smb_hdr hdr;	/* wct = 10 + SetupCount */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 Reserved;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 ParameterDisplacement;
	__u16 DataCount;
	__u16 DataOffset;
	__u16 DataDisplacement;
	__u8 SetupCount;
	__u8 Reserved1;		/* should be zero setup words following */
	__u16 ByteCount;
	__u8 Pad;		/* may be three bytes *//* followed by data area */
} TRANSACTION2_QFSI_RSP;

typedef struct smb_com_transaction2_get_dfs_refer_req {
	struct smb_hdr hdr;	/* wct = 15 */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 MaxParameterCount;
	__u16 MaxDataCount;
	__u8 MaxSetupCount;
	__u8 Reserved;
	__u16 Flags;
	__u32 Timeout;
	__u16 Reserved2;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 DataCount;
	__u16 DataOffset;
	__u8 SetupCount;
	__u8 Reserved3;
	__u16 SubCommand;	/* one setup word */
	__u16 ByteCount;
	__u8 Pad[3];		/* Win2K has sent 0x0F01 (max resp length perhaps?) followed by one byte pad - doesn't seem to matter though */
	__u16 MaxReferralLevel;
	char RequestFileName[1];
} TRANSACTION2_GET_DFS_REFER_REQ;

typedef struct dfs_referral_level_3 {
	__u16 VersionNumber;
	__u16 ReferralSize;
	__u16 ServerType;	/* 0x0001 = CIFS server */
	__u16 ReferralFlags;	/* or proximity - not clear which since always set to zero - SNIA spec says 0x01 means strip off PathConsumed chars before submitting RequestFileName to remote node */
	__u16 TimeToLive;
	__u16 Proximity;
	__u16 DfsPathOffset;
	__u16 DfsAlternatePathOffset;
	__u16 NetworkAddressOffset;
} REFERRAL3;

typedef struct smb_com_transaction_get_dfs_refer_rsp {
	struct smb_hdr hdr;	/* wct = 10 */
	__u16 TotalParameterCount;
	__u16 TotalDataCount;
	__u16 Reserved;
	__u16 ParameterCount;
	__u16 ParameterOffset;
	__u16 ParameterDisplacement;
	__u16 DataCount;
	__u16 DataOffset;
	__u16 DataDisplacement;
	__u8 SetupCount;
	__u8 Reserved1;		/* zero setup words following */
	__u16 ByteCount;
	__u8 Pad;
	__u16 PathConsumed;
	__u16 NumberOfReferrals;
	__u16 DFSFlags;
	__u16 Pad2;
	REFERRAL3 referrals[1];	/* array of level 3 dfs_referral structures */
	/* followed by the strings pointed to by the referral structures */
} TRANSACTION2_GET_DFS_REFER_RSP;

/* DFS Flags */
#define DFSREF_REFERRAL_SERVER  0x0001
#define DFSREF_STORAGE_SERVER   0x0002

/* IOCTL information */
/* List of ioctl function codes that look to be of interest to remote clients like this. */
/* Need to do some experimentation to make sure they all work remotely.                  */
/* Some of the following such as the encryption/compression ones would be                */
/* invoked from tools via a specialized hook into the VFS rather than via the            */
/* standard vfs entry points */
#define FSCTL_REQUEST_OPLOCK_LEVEL_1 0x00090000
#define FSCTL_REQUEST_OPLOCK_LEVEL_2 0x00090004
#define FSCTL_REQUEST_BATCH_OPLOCK   0x00090008
#define FSCTL_LOCK_VOLUME            0x00090018
#define FSCTL_UNLOCK_VOLUME          0x0009001C
#define FSCTL_GET_COMPRESSION        0x0009003C
#define FSCTL_SET_COMPRESSION        0x0009C040
#define FSCTL_REQUEST_FILTER_OPLOCK  0x0009008C
#define FSCTL_FILESYS_GET_STATISTICS 0x00090090
#define FSCTL_SET_REPARSE_POINT      0x000900A4
#define FSCTL_GET_REPARSE_POINT      0x000900A8
#define FSCTL_DELETE_REPARSE_POINT   0x000900AC
#define FSCTL_SET_SPARSE             0x000900C4
#define FSCTL_SET_ZERO_DATA          0x000900C8
#define FSCTL_SET_ENCRYPTION         0x000900D7
#define FSCTL_ENCRYPTION_FSCTL_IO    0x000900DB
#define FSCTL_WRITE_RAW_ENCRYPTED    0x000900DF
#define FSCTL_READ_RAW_ENCRYPTED     0x000900E3
#define FSCTL_SIS_COPYFILE           0x00090100
#define FSCTL_SIS_LINK_FILES         0x0009C104

#define IO_REPARSE_TAG_MOUNT_POINT   0xA0000003
#define IO_REPARSE_TAG_HSM           0xC0000004
#define IO_REPARSE_TAG_SIS           0x80000007

/*
 ************************************************************************
 * All structs for everything above the SMB PDUs themselves
 * (such as the T2 level specific data) go here                  
 ************************************************************************
 */

/*
 * Information on a server
 */

struct serverInfo {
	char name[16];
	unsigned char versionMajor;
	unsigned char versionMinor;
	unsigned long type;
	unsigned int commentOffset;
};

/*
 * The following structure is the format of the data returned on a NetShareEnum
 * with level "90" (x5A)
 */

struct shareInfo {
	char shareName[13];
	char pad;
	unsigned short type;
	unsigned int commentOffset;
};

struct aliasInfo {
	char aliasName[9];
	char pad;
	unsigned int commentOffset;
	unsigned char type[2];
};

struct aliasInfo92 {
	int aliasNameOffset;
	int serverNameOffset;
	int shareNameOffset;
};

typedef struct {
	__u64 TotalAllocationUnits;
	__u64 FreeAllocationUnits;
	__u32 SectorsPerAllocationUnit;
	__u32 BytesPerSector;
} FILE_SYSTEM_INFO;		/* size info, level 0x103 */

typedef struct {
	__u16 MajorVersionNumber;
	__u16 MinorVersionNumber;
	__u64 Capability;
} FILE_SYSTEM_UNIX_INFO;	/* Unix extensions info, level 0x200 */
/* Linux/Unix extensions capability flags */
#define CIFS_UNIX_FCNTL_CAP             0x00000001 /* support for fcntl locks */
#define CIFS_UNIX_POSIX_ACL_CAP         0x00000002

/* DeviceType Flags */
#define FILE_DEVICE_CD_ROM              0x00000002
#define FILE_DEVICE_CD_ROM_FILE_SYSTEM  0x00000003
#define FILE_DEVICE_DFS                 0x00000006
#define FILE_DEVICE_DISK                0x00000007
#define FILE_DEVICE_DISK_FILE_SYSTEM    0x00000008
#define FILE_DEVICE_FILE_SYSTEM         0x00000009
#define FILE_DEVICE_NAMED_PIPE          0x00000011
#define FILE_DEVICE_NETWORK             0x00000012
#define FILE_DEVICE_NETWORK_FILE_SYSTEM 0x00000014
#define FILE_DEVICE_NULL                0x00000015
#define FILE_DEVICE_PARALLEL_PORT       0x00000016
#define FILE_DEVICE_PRINTER             0x00000018
#define FILE_DEVICE_SERIAL_PORT         0x0000001b
#define FILE_DEVICE_STREAMS             0x0000001e
#define FILE_DEVICE_TAPE                0x0000001f
#define FILE_DEVICE_TAPE_FILE_SYSTEM    0x00000020
#define FILE_DEVICE_VIRTUAL_DISK        0x00000024
#define FILE_DEVICE_NETWORK_REDIRECTOR  0x00000028

typedef struct {
	__u32 DeviceType;
	__u32 DeviceCharacteristics;
} FILE_SYSTEM_DEVICE_INFO;	/* device info, level 0x104 */

typedef struct {
	__u32 Attributes;
	__u32 MaxPathNameComponentLength;
	__u32 FileSystemNameLen;
	char FileSystemName[52];	/* do not really need to save this - so potentially get only subset of name */
} FILE_SYSTEM_ATTRIBUTE_INFO;

typedef struct {		/* data block encoding of response to level 263 QPathInfo */
	__u64 CreationTime;
	__u64 LastAccessTime;
	__u64 LastWriteTime;
	__u64 ChangeTime;
	__u32 Attributes;
	__u32 Pad1;
	__u64 AllocationSize;
	__u64 EndOfFile;	/* size ie offset to first free byte in file */
	__u32 NumberOfLinks;	/* hard links */
	__u8 DeletePending;
	__u8 Directory;
	__u16 Pad2;
	__u64 IndexNumber;
	__u32 EASize;
	__u32 AccessFlags;
	__u64 IndexNumber1;
	__u64 CurrentByteOffset;
	__u32 Mode;
	__u32 AlignmentRequirement;
	__u32 FileNameLength;
	char FileName[1];
} FILE_ALL_INFO;		/* level 263 QPathInfo */

typedef struct {
	__u64 EndOfFile;
	__u64 NumOfBytes;
	__u64 LastStatusChange;	/*SNIA spec says DCE time for the three time fields */
	__u64 LastAccessTime;
	__u64 LastModificationTime;
	__u64 Uid;
	__u64 Gid;
	__u32 Type;
	__u64 DevMajor;
	__u64 DevMinor;
	__u64 UniqueId;
	__u64 Permissions;
	__u64 Nlinks;
} FILE_UNIX_BASIC_INFO;		/* level 512 QPathInfo */

typedef struct {
	char LinkDest[1];
} FILE_UNIX_LINK_INFO;		/* level 513 QPathInfo */

typedef struct {
	__u16 CreationDate;
	__u16 CreationTime;
	__u16 LastAccessDate;
	__u16 LastAccessTime;
	__u16 LastWriteDate;
	__u16 LastWriteTime;
	__u32 DataSize; /* File Size (EOF) */
	__u32 AllocationSize;
	__u16 Attributes; /* verify not u32 */
	__u32 EASize;
} FILE_INFO_STANDARD;  /* level 1 SetPath/FileInfo */

/* defines for enumerating possible values of the Unix type field below */
#define UNIX_FILE      0
#define UNIX_DIR       1
#define UNIX_SYMLINK   2
#define UNIX_CHARDEV   3
#define UNIX_BLOCKDEV  4
#define UNIX_FIFO      5
#define UNIX_SOCKET    6

typedef struct {
	__u32 NextEntryOffset;
	__u32 ResumeKey;
	__u64 EndOfFile;
	__u64 NumOfBytes;
	__u64 LastStatusChange;	/*SNIA spec says DCE time for the three time fields */
	__u64 LastAccessTime;
	__u64 LastModificationTime;
	__u64 Uid;
	__u64 Gid;
	__u32 Type;
	__u64 DevMajor;
	__u64 DevMinor;
	__u64 UniqueId;
	__u64 Permissions;
	__u64 Nlinks;
	char FileName[1];
} FILE_UNIX_INFO;

typedef struct {
	__u64 CreationTime;
	__u64 LastAccessTime;
	__u64 LastWriteTime;
	__u64 ChangeTime;
	__u32 Attributes;
	__u32 Pad;
} FILE_BASIC_INFO;		/* size info, level 0x101 */

struct file_allocation_info {
	__u64 AllocationSize;
};		/* size info, level 0x103 */

struct file_end_of_file_info {
	__u64 FileSize;		/* offset to end of file */
};	/* size info, level 0x104 */

typedef struct {
	__u32 NextEntryOffset;
	__u32 FileIndex;
	__u64 CreationTime;
	__u64 LastAccessTime;
	__u64 LastWriteTime;
	__u64 ChangeTime;
	__u64 EndOfFile;
	__u64 AllocationSize;
	__u32 ExtFileAttributes;
	__u32 FileNameLength;
	char FileName[1];
} FILE_DIRECTORY_INFO;   /* level 257 FF response data area */

struct gea {
	unsigned char name_len;
	char name[1];
};

struct gealist {
	unsigned long list_len;
	struct gea list[1];
};

struct fea {
	unsigned char EA_flags;
	__u8 name_len;
	__u16 value_len;
	char name[1];
	/* optionally followed by value */
};
/* flags for _FEA.fEA */
#define FEA_NEEDEA         0x80	/* need EA bit */

struct fealist {
	__u32 list_len;
	struct fea list[1];
};

/* used to hold an arbitrary blob of data */
struct data_blob {
	__u8 *data;
	size_t length;
	void (*free) (struct data_blob * data_blob);
};

#ifdef CONFIG_CIFS_POSIX
/* 
	For better POSIX semantics from Linux client, (even better
	than the existing CIFS Unix Extensions) we need updated PDUs for:
	
	1) PosixCreateX - to set and return the mode, inode#, device info and
	perhaps add a CreateDevice - to create Pipes and other special .inodes
	Also note POSIX open flags
	2) Close - to return the last write time to do cache across close more safely
	3) PosixQFSInfo - to return statfs info
	4) FindFirst return unique inode number - what about resume key, two forms short (matches readdir) and full (enough info to cache inodes)
	5) Mkdir - set mode
	
	And under consideration: 
	6) FindClose2 (return nanosecond timestamp ??)
	7) Use nanosecond timestamps throughout all time fields if 
	   corresponding attribute flag is set
	8) sendfile - handle based copy
	9) Direct i/o
	10) "POSIX ACL" support
	11) Misc fcntls?
	
	what about fixing 64 bit alignment
	
	There are also various legacy SMB/CIFS requests used as is
	
	From existing Lanman and NTLM dialects:
	--------------------------------------
	NEGOTIATE
	SESSION_SETUP_ANDX (BB which?)
	TREE_CONNECT_ANDX (BB which wct?)
	TREE_DISCONNECT (BB add volume timestamp on response)
	LOGOFF_ANDX
	DELETE (note delete open file behavior)
	DELETE_DIRECTORY
	READ_AND_X
	WRITE_AND_X
	LOCKING_AND_X (note posix lock semantics)
	RENAME (note rename across dirs and open file rename posix behaviors)
	NT_RENAME (for hardlinks) Is this good enough for all features?
	FIND_CLOSE2
	TRANSACTION2 (18 cases)
		SMB_SET_FILE_END_OF_FILE_INFO2 SMB_SET_PATH_END_OF_FILE_INFO2
		(BB verify that never need to set allocation size)
		SMB_SET_FILE_BASIC_INFO2 (setting times - BB can it be done via Unix ext?)
	
	COPY (note support for copy across directories) - FUTURE, OPTIONAL
	setting/getting OS/2 EAs - FUTURE (BB can this handle
	        setting Linux xattrs perfectly)         - OPTIONAL
    dnotify                                         - FUTURE, OPTIONAL
    quota                                           - FUTURE, OPTIONAL
			
	Note that various requests implemented for NT interop such as 
		NT_TRANSACT (IOCTL) QueryReparseInfo
	are unneeded to servers compliant with the CIFS POSIX extensions
	
	From CIFS Unix Extensions:
	-------------------------
	T2 SET_PATH_INFO (SMB_SET_FILE_UNIX_LINK) for symlinks
	T2 SET_PATH_INFO (SMB_SET_FILE_BASIC_INFO2)
	T2 QUERY_PATH_INFO (SMB_QUERY_FILE_UNIX_LINK)
	T2 QUERY_PATH_INFO (SMB_QUERY_FILE_UNIX_BASIC) - BB check for missing inode fields
					Actually need QUERY_FILE_UNIX_INFO since has inode num
					BB what about a) blksize/blkbits/blocks
								  b) i_version
								  c) i_rdev
								  d) notify mask?
								  e) generation
								  f) size_seqcount
	T2 FIND_FIRST/FIND_NEXT FIND_FILE_UNIX
	TRANS2_GET_DFS_REFERRAL				  - OPTIONAL but recommended
	T2_QFS_INFO QueryDevice/AttributeInfo - OPTIONAL
	
	
 */
#endif 

#pragma pack()			/* resume default structure packing */

#endif				/* _CIFSPDU_H */
