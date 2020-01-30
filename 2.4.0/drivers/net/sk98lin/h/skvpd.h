/******************************************************************************
 *
 * Name:	skvpd.h
 * Project:	GEnesis, PCI Gigabit Ethernet Adapter
 * Version:	$Revision: 1.10 $
 * Date:	$Date: 2000/08/10 11:29:07 $
 * Purpose:	Defines and Macros for VPD handling
 *
 ******************************************************************************/

/******************************************************************************
 *
 *	(C)Copyright 1998-2000 SysKonnect,
 *	a business unit of Schneider & Koch & Co. Datensysteme GmbH.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	The information in this file is provided "AS IS" without warranty.
 *
 ******************************************************************************/

/******************************************************************************
 *
 * History:
 *
 *	$Log: skvpd.h,v $
 *	Revision 1.10  2000/08/10 11:29:07  rassmann
 *	Editorial changes.
 *	Preserving 32-bit alignment in structs for the adapter context.
 *	Removed unused function VpdWriteDword() (#if 0).
 *	Made VpdReadKeyword() available for SKDIAG only.
 *	
 *	Revision 1.9  1999/11/22 14:02:27  cgoos
 *	Changed license header to GPL.
 *	
 *	Revision 1.8  1999/03/11 14:26:40  malthoff
 *	Replace __STDC__ with SK_KR_PROTO.
 *	
 *	Revision 1.7  1998/10/28 07:27:17  gklug
 *	rmv: SWAP macros
 *	add: VPD_IN/OUT8 macros
 *	chg: interface definition
 *	
 *	Revision 1.6  1998/10/22 10:03:44  gklug
 *	fix: use SK_OUT16 instead of SK_OUTW
 *	
 *	Revision 1.5  1998/10/14 07:05:31  cgoos
 *	Changed constants in SK_SWAP_32 to UL.
 *	
 *	Revision 1.4  1998/08/19 08:14:09  gklug
 *	fix: remove struct keyword as much as possible from the c-code (see CCC)
 *	
 *	Revision 1.3  1998/08/18 08:18:56  malthoff
 *	Modify VPD in and out macros for SK_DIAG
 *	
 *	Revision 1.2  1998/07/03 14:49:08  malthoff
 *	Add VPD_INxx() and VPD_OUTxx() macros for the Diagnostics tool.
 *	
 *	Revision 1.1  1998/06/19 14:08:03  malthoff
 *	Created.
 *	
 *
 ******************************************************************************/

/*
 * skvpd.h	contains Diagnostic specific defines for VPD handling
 */

#ifndef __INC_SKVPD_H_
#define __INC_SKVPD_H_

/*
 * Define Resource Type Identifiers and VPD keywords
 */
#define	RES_ID		0x82	/* Resource Type ID String (Product Name) */
#define RES_VPD_R	0x90	/* start of VPD read only area */
#define RES_VPD_W	0x91	/* start of VPD read/write area */
#define RES_END		0x78	/* Resource Type End Tag */

#ifndef VPD_NAME
#define VPD_NAME	"Name"	/* Product Name, VPD name of RES_ID */
#endif	/* VPD_NAME */
#define VPD_PN		"PN"	/* Adapter Part Number */
#define	VPD_EC		"EC"	/* Adapter Engineering Level */
#define VPD_MN		"MN"	/* Manufacture ID */
#define VPD_SN		"SN"	/* Serial Number */
#define VPD_CP		"CP"	/* Extended Capability */
#define VPD_RV		"RV"	/* Checksum and Reserved */
#define	VPD_YA		"YA"	/* Asset Tag Identifier */
#define VPD_VL		"VL"	/* First Error Log Message (SK specific) */
#define VPD_VF		"VF"	/* Second Error Log Message (SK specific) */
#define VPD_RW		"RW"	/* Remaining Read / Write Area */

/* 'type' values for vpd_setup_para() */
#define VPD_RO_KEY	1	/* RO keys are "PN", "EC", "MN", "SN", "RV" */
#define VPD_RW_KEY	2	/* RW keys are "Yx", "Vx", and "RW" */

/* 'op' values for vpd_setup_para() */
#define	ADD_KEY		1	/* add the key at the pos "RV" or "RW" */
#define OWR_KEY		2	/* overwrite key if already exists */

/*
 * Define READ and WRITE Constants.
 */
#define	VPD_SIZE	512
#define VPD_READ	0x0000
#define VPD_WRITE	0x8000

#define VPD_STOP(pAC,IoC)	VPD_OUT16(pAC,IoC,PCI_VPD_ADR_REG,VPD_WRITE)

#define VPD_GET_RES_LEN(p)	((unsigned int) \
					(* (SK_U8 *)&(p)[1]) |\
					((* (SK_U8 *)&(p)[2]) << 8))
#define VPD_GET_VPD_LEN(p)	((unsigned int)(* (SK_U8 *)&(p)[2]))
#define VPD_GET_VAL(p)		((char *)&(p)[3])

#define VPD_MAX_LEN	50

/* VPD status */
	/* bit 7..1 reserved */
#define VPD_VALID	(1<<0)	/* VPD data buffer, vpd_free_ro, */
							/* and vpd_free_rw valid	 */

/*
 * VPD structs
 */
typedef	struct s_vpd_status {
	unsigned short	Align01;			/* Alignment */
	unsigned short	vpd_status;			/* VPD status, description see above */
	int				vpd_free_ro;		/* unused bytes in read only area */
	int				vpd_free_rw;		/* bytes available in read/write area */
} SK_VPD_STATUS;

typedef	struct s_vpd {
	SK_VPD_STATUS	v;					/* VPD status structure */
	char			vpd_buf[VPD_SIZE];	/* VPD buffer */
} SK_VPD;

typedef	struct s_vpd_para {
	unsigned int	p_len;	/* parameter length */
	char			*p_val;	/* points to the value */
} SK_VPD_PARA;

/*
 * structure of Large Resource Type Identifiers
 */

/* was removed because of alignment problems */

/*
 * sturcture of VPD keywords
 */
typedef	struct s_vpd_key {
	char			p_key[2];	/* 2 bytes ID string */
	unsigned char	p_len;		/* 1 byte length */
	char			p_val;		/* start of the value string */
} SK_VPD_KEY;


/*
 * System specific VPD macros
 */
#ifndef SKDIAG
#ifndef VPD_DO_IO
#define VPD_OUT8(pAC,IoC,Addr,Val)	(void)SkPciWriteCfgByte(pAC,Addr,Val)
#define VPD_OUT16(pAC,IoC,Addr,Val)	(void)SkPciWriteCfgWord(pAC,Addr,Val)
#define VPD_OUT32(pAC,IoC,Addr,Val)	(void)SkPciWriteCfgDWord(pAC,Addr,Val)
#define VPD_IN8(pAC,IoC,Addr,pVal)	(void)SkPciReadCfgByte(pAC,Addr,pVal)
#define VPD_IN16(pAC,IoC,Addr,pVal)	(void)SkPciReadCfgWord(pAC,Addr,pVal)
#define VPD_IN32(pAC,IoC,Addr,pVal)	(void)SkPciReadCfgDWord(pAC,Addr,pVal)
#else	/* VPD_DO_IO */
#define VPD_OUT8(pAC,IoC,Addr,Val)	SK_OUT8(IoC,PCI_C(Addr),Val)
#define VPD_OUT16(pAC,IoC,Addr,Val)	SK_OUT16(IoC,PCI_C(Addr),Val)
#define VPD_OUT32(pAC,IoC,Addr,Val)	SK_OUT32(IoC,PCI_C(Addr),Val)
#define VPD_IN8(pAC,IoC,Addr,pVal)	SK_IN8(IoC,PCI_C(Addr),pVal)
#define VPD_IN16(pAC,IoC,Addr,pVal)	SK_IN16(IoC,PCI_C(Addr),pVal)
#define VPD_IN32(pAC,IoC,Addr,pVal)	SK_IN32(IoC,PCI_C(Addr),pVal)
#endif	/* VPD_DO_IO */
#else	/* SKDIAG */
#define VPD_OUT8(pAC,Ioc,Addr,Val) {			\
		if ((pAC)->DgT.DgUseCfgCycle)			\
			SkPciWriteCfgByte(pAC,Addr,Val);	\
		else									\
			SK_OUT8(pAC,PCI_C(Addr),Val);		\
		}
#define VPD_OUT16(pAC,Ioc,Addr,Val) {			\
		if ((pAC)->DgT.DgUseCfgCycle)			\
			SkPciWriteCfgWord(pAC,Addr,Val);	\
		else						\
			SK_OUT16(pAC,PCI_C(Addr),Val);		\
		}
#define VPD_OUT32(pAC,Ioc,Addr,Val) {			\
		if ((pAC)->DgT.DgUseCfgCycle)			\
			SkPciWriteCfgDWord(pAC,Addr,Val);	\
		else						\
			SK_OUT32(pAC,PCI_C(Addr),Val); 		\
		}
#define VPD_IN8(pAC,Ioc,Addr,pVal) {			\
		if ((pAC)->DgT.DgUseCfgCycle) 			\
			SkPciReadCfgByte(pAC,Addr,pVal);	\
		else						\
			SK_IN8(pAC,PCI_C(Addr),pVal); 		\
		}
#define VPD_IN16(pAC,Ioc,Addr,pVal) {			\
		if ((pAC)->DgT.DgUseCfgCycle) 			\
			SkPciReadCfgWord(pAC,Addr,pVal);	\
		else						\
			SK_IN16(pAC,PCI_C(Addr),pVal); 		\
		}
#define VPD_IN32(pAC,Ioc,Addr,pVal) {			\
		if ((pAC)->DgT.DgUseCfgCycle)			\
			SkPciReadCfgDWord(pAC,Addr,pVal);	\
		else						\
			SK_IN32(pAC,PCI_C(Addr),pVal);		\
		}
#endif	/* nSKDIAG */

/* function prototypes ********************************************************/

#ifndef	SK_KR_PROTO
#ifdef SKDIAG
extern SK_U32	VpdReadDWord(
	SK_AC		*pAC,
	SK_IOC		IoC,
	int			addr);
#endif	/* SKDIAG */

extern int	VpdSetupPara(
	SK_AC		*pAC,
	char		*key,
	char		*buf,
	int			len,
	int			type,
	int			op);

extern SK_VPD_STATUS	*VpdStat(
	SK_AC		*pAC,
	SK_IOC		IoC);

extern int	VpdKeys(
	SK_AC		*pAC,
	SK_IOC		IoC,
	char		*buf,
	int			*len,
	int			*elements);

extern int	VpdRead(
	SK_AC		*pAC,
	SK_IOC		IoC,
	char		*key,
	char		*buf,
	int			*len);

extern	SK_BOOL	VpdMayWrite(
	char		*key);

extern int	VpdWrite(
	SK_AC		*pAC,
	SK_IOC		IoC,
	char		*key,
	char		*buf);

extern int	VpdDelete(
	SK_AC		*pAC,
	SK_IOC		IoC,
	char		*key);

extern int	VpdUpdate(
	SK_AC		*pAC,
	SK_IOC		IoC);

extern void	VpdErrLog(
	SK_AC		*pAC,
	SK_IOC		IoC,
	char		*msg);

#ifdef	SKDIAG
extern int	VpdReadBlock(
	SK_AC		*pAC,
	SK_IOC		IoC,
	char		*buf,
	int			addr,
	int			len);

extern int	VpdWriteBlock(
	SK_AC		*pAC,
	SK_IOC		IoC,
	char		*buf,
	int			addr,
	int			len);
#endif	/* SKDIAG */
#else	/* SK_KR_PROTO */
extern SK_U32	VpdReadDWord();
extern int	VpdSetupPara();
extern SK_VPD_STATUS	*VpdStat();
extern int	VpdKeys();
extern int	VpdRead();
extern SK_BOOL	VpdMayWrite();
extern int	VpdWrite();
extern int	VpdDelete();
extern int	VpdUpdate();
extern void	VpdErrLog();
#endif	/* SK_KR_PROTO */

#endif	/* __INC_SKVPD_H_ */
