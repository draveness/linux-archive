/*****************************************************************************
 *
 * Name:	skgepnmi.h
 * Project:	GEnesis, PCI Gigabit Ethernet Adapter
 * Version:	$Revision: 1.44 $
 * Date:	$Date: 2000/09/07 07:35:27 $
 * Purpose:	Defines for Private Network Management Interface
 *
 ****************************************************************************/

/******************************************************************************
 *
 *	(C)Copyright 1998,1999 SysKonnect,
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

/*****************************************************************************
 *
 * History:
 *
 *	$Log: skgepnmi.h,v $
 *	Revision 1.44  2000/09/07 07:35:27  rwahl
 *	- removed NDIS counter specific data type.
 *	- fixed spelling for OID_SKGE_RLMT_PORT_PREFERRED.
 *	
 *	Revision 1.43  2000/08/04 11:41:08  rwahl
 *	- Fixed compiler warning (port is always >= 0) for macros
 *	  SK_PNMI_CNT_RX_LONGFRAMES & SK_PNMI_CNT_SYNC_OCTETS
 *	
 *	Revision 1.42  2000/08/03 15:14:07  rwahl
 *	- Corrected error in driver macros addressing a physical port.
 *	
 *	Revision 1.41  2000/08/01 16:22:29  rwahl
 *	- Changed MDB version to 3.1.
 *	- Added definitions for StatRxLongFrames counter.
 *	- Added macro to be used by driver to count long frames received.
 *	- Added directive to control width (default = 32bit) of NDIS statistic
 *	  counters (SK_NDIS_64BIT_CTR).
 *	
 *	Revision 1.40  2000/03/31 13:51:34  rwahl
 *	Added SK_UPTR cast to offset calculation for PNMI struct fields;
 *	missing cast caused compiler warnings by Win64 compiler.
 *	
 *	Revision 1.39  1999/12/06 10:09:47  rwahl
 *	Added new error log message.
 *	
 *	Revision 1.38  1999/11/22 13:57:55  cgoos
 *	Changed license header to GPL.
 *	
 *	Revision 1.37  1999/09/14 14:25:32  rwahl
 *	Set MDB version for 1000Base-T (sensors, Master/Slave) changes.
 *	
 *	Revision 1.36  1999/05/20 09:24:56  cgoos
 *	Changes for 1000Base-T (sensors, Master/Slave).
 *	
 *	Revision 1.35  1999/04/13 15:10:51  mhaveman
 *	Replaced RLMT macros SK_RLMT_CHECK_xxx again by those of PNMI to
 *	grant unified interface. But PNMI macros will store the same
 *	value as RLMT macros.
 *	
 *	Revision 1.34  1999/04/13 15:03:49  mhaveman
 *	-Changed copyright
 *	-Removed SK_PNMI_RLMT_MODE_CHK_xxx macros. Those of RLMT should be
 *	 used.
 *	
 *	Revision 1.33  1999/03/23 10:41:02  mhaveman
 *	Changed comments.
 *	
 *	Revision 1.32  1999/01/25 15:01:33  mhaveman
 *	Added support for multiple simultaniously active ports.
 *	
 *	Revision 1.31  1999/01/19 10:06:26  mhaveman
 *	Added new error log message.
 *	
 *	Revision 1.30  1999/01/05 10:34:49  mhaveman
 *	Fixed little error in RlmtChangeEstimate calculation.
 *	
 *	Revision 1.29  1999/01/05 09:59:41  mhaveman
 *	Redesigned port switch average calculation to avoid 64bit
 *	arithmetic.
 *	
 *	Revision 1.28  1998/12/08 10:05:48  mhaveman
 *	Defined macro SK_PNMI_MIN_STRUCT_SIZE.
 *	
 *	Revision 1.27  1998/12/03 14:39:35  mhaveman
 *	Fixed problem that LSTAT was enumerated wrong.
 *	
 *	Revision 1.26  1998/12/03 11:19:51  mhaveman
 *	Changed contents of errlog message SK_PNMI_ERR016MSG
 *	
 *	Revision 1.25  1998/12/01 10:40:04  mhaveman
 *	Changed size of SensorNumber, ChecksumNumber and RlmtPortNumber in
 *	SK_PNMI_STRUCT_DATA to be conform with OID definition.
 *	
 *	Revision 1.24  1998/11/20 08:09:27  mhaveman
 *	Added macros to convert between logical, physical port indexes and
 *	instances.
 *	
 *	Revision 1.23  1998/11/10 13:41:13  mhaveman
 *	Needed to change interface, because NT driver needs a return value
 *	of needed buffer space on TOO_SHORT errors. Therefore all
 *	SkPnmiGet/Preset/Set functions now have a pointer to the length
 *	parameter, where the needed space on error is returned.
 *	
 *	Revision 1.22  1998/11/03 12:05:51  mhaveman
 *	Added pAC parameter to counter macors.
 *	
 *	Revision 1.21  1998/11/02 10:47:36  mhaveman
 *	Added syslog messages for internal errors.
 *	
 *	Revision 1.20  1998/10/30 15:49:36  mhaveman
 *	-Removed unused SK_PNMI_UTILIZATION_BASE and EstOldCnt.
 *	-Redefined SK_PNMI_CHG_EST_BASE to hundreds of seconds.
 *	
 *	Revision 1.19  1998/10/29 15:38:44  mhaveman
 *	Changed string lengths of PNMI_STRUCT_DATA structure because
 *	string OIDs are now encoded with leading length ocetet.
 *	
 *	Revision 1.18  1998/10/29 08:52:27  mhaveman
 *	-Added byte to strings in PNMI_STRUCT_DATA structure.
 *	-Shortened SK_PNMI_RLMT structure to SK_MAX_MACS elements.
 *	
 *	Revision 1.17  1998/10/28 08:49:50  mhaveman
 *	-Changed type of Instance back to SK_U32 because of VPD
 *	-Changed type from SK_U8 to char of PciBusSpeed, PciBusWidth, PMD,
 *	 and Connector.
 *	
 *	Revision 1.16  1998/10/22 10:42:31  mhaveman
 *	-Removed (SK_U32) casts for OIDs
 *	-excluded NDIS OIDs when they are already defined with ifndef _NDIS_
 *	
 *	Revision 1.15  1998/10/20 13:56:28  mhaveman
 *	Headerfile includes now directly other header files to comile correctly.
 *	
 *	Revision 1.14  1998/10/20 07:31:09  mhaveman
 *	Made type changes to unsigned int where possible.
 *	
 *	Revision 1.13  1998/10/19 10:53:13  mhaveman
 *	-Casted OID definitions to SK_U32
 *	-Renamed RlmtMAC... to RlmtPort...
 *	-Changed wrong type of VpdEntriesList from SK_U32 to char *
 *	
 *	Revision 1.12  1998/10/13 07:42:27  mhaveman
 *	-Added OIDs OID_SKGE_TRAP_NUMBER and OID_SKGE_ALL_DATA
 *	-Removed old cvs history entries
 *	-Renamed MacNumber to PortNumber
 *	
 *	Revision 1.11  1998/10/07 10:55:24  mhaveman
 *	-Added OID_MDB_VERSION. Therefore was a renumbering of the VPD OIDs
 *	 necessary.
 *	-Added OID_GEN_ Ids to support the windows driver.
 *	
 *	Revision 1.10  1998/09/30 13:41:10  mhaveman
 *	Renamed some OIDs to reduce usage of 'MAC' which is replaced by 'PORT'.
 *	
 *	Revision 1.9  1998/09/04 17:06:17  mhaveman
 *	-Added SyncCounter as macro.
 *	-Renamed OID_SKGE_.._NO_DESCR_CTS to OID_SKGE_.._NO_BUF_CTS.
 *	-Added macros for driver description and version strings.
 *	
 *	Revision 1.8  1998/09/04 14:36:52  mhaveman
 *	Added OIDs and Structure to access value of macro counters which are
 *	counted by the driver.
 *	
 *	Revision 1.7  1998/09/04 11:59:36  mhaveman
 *	Everything compiles now. Driver Macros for counting still missing.
 *	
 ****************************************************************************/

#ifndef _SKGEPNMI_H_
#define _SKGEPNMI_H_

/*
 * Include dependencies
 */
#include "h/sktypes.h"
#include "h/skerror.h"
#include "h/sktimer.h"
#include "h/ski2c.h"
#include "h/skaddr.h"
#include "h/skrlmt.h"


/*
 * Management Database Version
 */
#define SK_PNMI_MDB_VERSION		0x00030001	/* 3.1 */


/*
 * Event definitions
 */
#define SK_PNMI_EVT_SIRQ_OVERFLOW	1	/* Counter overflow */
#define SK_PNMI_EVT_SEN_WAR_LOW		2	/* Lower war thres exceeded */
#define SK_PNMI_EVT_SEN_WAR_UPP		3	/* Upper war thres exceeded */
#define SK_PNMI_EVT_SEN_ERR_LOW		4	/* Lower err thres exceeded */
#define SK_PNMI_EVT_SEN_ERR_UPP		5	/* Upper err thres exceeded */
#define SK_PNMI_EVT_CHG_EST_TIMER	6	/* Timer event for RLMT Chg */
#define SK_PNMI_EVT_UTILIZATION_TIMER	7	/* Timer event for Utiliza. */
#define SK_PNMI_EVT_CLEAR_COUNTER	8	/* Clear statistic counters */
#define SK_PNMI_EVT_XMAC_RESET		9	/* XMAC will be reset */

#define SK_PNMI_EVT_RLMT_PORT_UP	10	/* Port came logically up */
#define SK_PNMI_EVT_RLMT_PORT_DOWN	11	/* Port went logically down */
#define SK_PNMI_EVT_RLMT_PORT_SWITCH	12	/* Switched active port */
#define SK_PNMI_EVT_RLMT_SEGMENTATION	13	/* Two SP root bridges found */
#define SK_PNMI_EVT_RLMT_ACTIVE_DOWN	14	/* Port went logically down */
#define SK_PNMI_EVT_RLMT_ACTIVE_UP	15	/* Port came logically up */

/*
 * Return values
 */
#define SK_PNMI_ERR_OK			0
#define SK_PNMI_ERR_GENERAL		1
#define SK_PNMI_ERR_TOO_SHORT		2
#define SK_PNMI_ERR_BAD_VALUE		3
#define SK_PNMI_ERR_READ_ONLY		4
#define SK_PNMI_ERR_UNKNOWN_OID		5
#define SK_PNMI_ERR_UNKNOWN_INST	6


/*
 * Return values of driver reset function SK_DRIVER_RESET() and
 * driver event function SK_DRIVER_EVENT()
 */
#define SK_PNMI_ERR_OK			0
#define SK_PNMI_ERR_FAIL		1


/*
 * Return values of driver test function SK_DRIVER_SELFTEST()
 */
#define SK_PNMI_TST_UNKNOWN		(1 << 0)
#define SK_PNMI_TST_TRANCEIVER		(1 << 1)
#define SK_PNMI_TST_ASIC		(1 << 2)
#define SK_PNMI_TST_SENSOR		(1 << 3)
#define SK_PNMI_TST_POWERMGMT		(1 << 4)
#define SK_PNMI_TST_PCI			(1 << 5)
#define SK_PNMI_TST_MAC			(1 << 6)


/*
 * RLMT specific definitions
 */
#define SK_PNMI_RLMT_STATUS_STANDBY	1
#define SK_PNMI_RLMT_STATUS_ACTIVE	2
#define SK_PNMI_RLMT_STATUS_ERROR	3

#define SK_PNMI_RLMT_LSTAT_PHY_DOWN	1
#define SK_PNMI_RLMT_LSTAT_AUTONEG	2
#define SK_PNMI_RLMT_LSTAT_LOG_DOWN	3
#define SK_PNMI_RLMT_LSTAT_LOG_UP	4
#define SK_PNMI_RLMT_LSTAT_INDETERMINATED 5

#define SK_PNMI_RLMT_MODE_CHK_LINK	(SK_RLMT_CHECK_LINK)
#define SK_PNMI_RLMT_MODE_CHK_RX	(SK_RLMT_CHECK_LOC_LINK)
#define SK_PNMI_RLMT_MODE_CHK_SPT	(SK_RLMT_CHECK_SEG)
/* #define SK_PNMI_RLMT_MODE_CHK_EX */

/*
 * OID definition
 */
#ifndef _NDIS_	/* Check, whether NDIS already included OIDs */

#define OID_GEN_XMIT_OK			0x00020101
#define OID_GEN_RCV_OK			0x00020102
#define OID_GEN_XMIT_ERROR		0x00020103
#define OID_GEN_RCV_ERROR		0x00020104
#define OID_GEN_RCV_NO_BUFFER		0x00020105

/* #define OID_GEN_DIRECTED_BYTES_XMIT	0x00020201 */
#define OID_GEN_DIRECTED_FRAMES_XMIT	0x00020202
/* #define OID_GEN_MULTICAST_BYTES_XMIT	0x00020203 */
#define OID_GEN_MULTICAST_FRAMES_XMIT	0x00020204
/* #define OID_GEN_BROADCAST_BYTES_XMIT	0x00020205 */
#define OID_GEN_BROADCAST_FRAMES_XMIT	0x00020206
/* #define OID_GEN_DIRECTED_BYTES_RCV	0x00020207 */
#define OID_GEN_DIRECTED_FRAMES_RCV	0x00020208
/* #define OID_GEN_MULTICAST_BYTES_RCV	0x00020209 */
#define OID_GEN_MULTICAST_FRAMES_RCV	0x0002020A
/* #define OID_GEN_BROADCAST_BYTES_RCV	0x0002020B */
#define OID_GEN_BROADCAST_FRAMES_RCV	0x0002020C
#define OID_GEN_RCV_CRC_ERROR		0x0002020D
#define OID_GEN_TRANSMIT_QUEUE_LENGTH	0x0002020E

#define OID_802_3_PERMANENT_ADDRESS	0x01010101
#define OID_802_3_CURRENT_ADDRESS	0x01010102
/* #define OID_802_3_MULTICAST_LIST	0x01010103 */
/* #define OID_802_3_MAXIMUM_LIST_SIZE	0x01010104 */
/* #define OID_802_3_MAC_OPTIONS	0x01010105 */
			
#define OID_802_3_RCV_ERROR_ALIGNMENT	0x01020101
#define OID_802_3_XMIT_ONE_COLLISION	0x01020102
#define OID_802_3_XMIT_MORE_COLLISIONS	0x01020103
#define OID_802_3_XMIT_DEFERRED		0x01020201
#define OID_802_3_XMIT_MAX_COLLISIONS	0x01020202
#define OID_802_3_RCV_OVERRUN		0x01020203
#define OID_802_3_XMIT_UNDERRUN		0x01020204
#define OID_802_3_XMIT_TIMES_CRS_LOST	0x01020206
#define OID_802_3_XMIT_LATE_COLLISIONS	0x01020207

#endif /* _NDIS_ */

#define OID_SKGE_MDB_VERSION		0xFF010100
#define OID_SKGE_SUPPORTED_LIST		0xFF010101
#define OID_SKGE_VPD_FREE_BYTES		0xFF010102
#define OID_SKGE_VPD_ENTRIES_LIST	0xFF010103
#define OID_SKGE_VPD_ENTRIES_NUMBER	0xFF010104
#define OID_SKGE_VPD_KEY		0xFF010105
#define OID_SKGE_VPD_VALUE		0xFF010106
#define OID_SKGE_VPD_ACCESS		0xFF010107
#define OID_SKGE_VPD_ACTION		0xFF010108
			
#define OID_SKGE_PORT_NUMBER		0xFF010110
#define OID_SKGE_DEVICE_TYPE		0xFF010111
#define OID_SKGE_DRIVER_DESCR		0xFF010112
#define OID_SKGE_DRIVER_VERSION		0xFF010113
#define OID_SKGE_HW_DESCR		0xFF010114
#define OID_SKGE_HW_VERSION		0xFF010115
#define OID_SKGE_CHIPSET		0xFF010116
#define OID_SKGE_ACTION			0xFF010117
#define OID_SKGE_RESULT			0xFF010118
#define OID_SKGE_BUS_TYPE		0xFF010119
#define OID_SKGE_BUS_SPEED		0xFF01011A
#define OID_SKGE_BUS_WIDTH		0xFF01011B

/*#define OID_SKGE_MULTICAST_LIST		0xFF01011C*/

#define OID_SKGE_SENSOR_NUMBER		0xFF020100			
#define OID_SKGE_SENSOR_INDEX		0xFF020101
#define OID_SKGE_SENSOR_DESCR		0xFF020102
#define OID_SKGE_SENSOR_TYPE		0xFF020103
#define OID_SKGE_SENSOR_VALUE		0xFF020104
#define OID_SKGE_SENSOR_WAR_THRES_LOW	0xFF020105
#define OID_SKGE_SENSOR_WAR_THRES_UPP	0xFF020106
#define OID_SKGE_SENSOR_ERR_THRES_LOW	0xFF020107
#define OID_SKGE_SENSOR_ERR_THRES_UPP	0xFF020108
#define OID_SKGE_SENSOR_STATUS		0xFF020109
#define OID_SKGE_SENSOR_WAR_CTS		0xFF02010A
#define OID_SKGE_SENSOR_ERR_CTS		0xFF02010B
#define OID_SKGE_SENSOR_WAR_TIME	0xFF02010C
#define OID_SKGE_SENSOR_ERR_TIME	0xFF02010D

#define OID_SKGE_CHKSM_NUMBER		0xFF020110
#define OID_SKGE_CHKSM_RX_OK_CTS	0xFF020111
#define OID_SKGE_CHKSM_RX_UNABLE_CTS	0xFF020112
#define OID_SKGE_CHKSM_RX_ERR_CTS	0xFF020113
#define OID_SKGE_CHKSM_TX_OK_CTS	0xFF020114
#define OID_SKGE_CHKSM_TX_UNABLE_CTS	0xFF020115
			
#define OID_SKGE_STAT_TX		0xFF020120
#define OID_SKGE_STAT_TX_OCTETS		0xFF020121
#define OID_SKGE_STAT_TX_BROADCAST	0xFF020122
#define OID_SKGE_STAT_TX_MULTICAST	0xFF020123
#define OID_SKGE_STAT_TX_UNICAST	0xFF020124
#define OID_SKGE_STAT_TX_LONGFRAMES	0xFF020125
#define OID_SKGE_STAT_TX_BURST		0xFF020126
#define OID_SKGE_STAT_TX_PFLOWC		0xFF020127
#define OID_SKGE_STAT_TX_FLOWC		0xFF020128
#define OID_SKGE_STAT_TX_SINGLE_COL	0xFF020129
#define OID_SKGE_STAT_TX_MULTI_COL	0xFF02012A
#define OID_SKGE_STAT_TX_EXCESS_COL	0xFF02012B
#define OID_SKGE_STAT_TX_LATE_COL	0xFF02012C
#define OID_SKGE_STAT_TX_DEFFERAL	0xFF02012D
#define OID_SKGE_STAT_TX_EXCESS_DEF	0xFF02012E
#define OID_SKGE_STAT_TX_UNDERRUN	0xFF02012F
#define OID_SKGE_STAT_TX_CARRIER	0xFF020130
/* #define OID_SKGE_STAT_TX_UTIL		0xFF020131 */
#define OID_SKGE_STAT_TX_64		0xFF020132
#define OID_SKGE_STAT_TX_127		0xFF020133
#define OID_SKGE_STAT_TX_255		0xFF020134
#define OID_SKGE_STAT_TX_511		0xFF020135
#define OID_SKGE_STAT_TX_1023		0xFF020136
#define OID_SKGE_STAT_TX_MAX		0xFF020137
#define OID_SKGE_STAT_TX_SYNC		0xFF020138
#define OID_SKGE_STAT_TX_SYNC_OCTETS	0xFF020139
#define OID_SKGE_STAT_RX		0xFF02013A
#define OID_SKGE_STAT_RX_OCTETS		0xFF02013B
#define OID_SKGE_STAT_RX_BROADCAST	0xFF02013C
#define OID_SKGE_STAT_RX_MULTICAST	0xFF02013D
#define OID_SKGE_STAT_RX_UNICAST	0xFF02013E
#define OID_SKGE_STAT_RX_PFLOWC		0xFF02013F
#define OID_SKGE_STAT_RX_FLOWC		0xFF020140
#define OID_SKGE_STAT_RX_PFLOWC_ERR	0xFF020141
#define OID_SKGE_STAT_RX_FLOWC_UNKWN	0xFF020142
#define OID_SKGE_STAT_RX_BURST		0xFF020143
#define OID_SKGE_STAT_RX_MISSED		0xFF020144
#define OID_SKGE_STAT_RX_FRAMING	0xFF020145
#define OID_SKGE_STAT_RX_OVERFLOW	0xFF020146
#define OID_SKGE_STAT_RX_JABBER		0xFF020147
#define OID_SKGE_STAT_RX_CARRIER	0xFF020148
#define OID_SKGE_STAT_RX_IR_LENGTH	0xFF020149
#define OID_SKGE_STAT_RX_SYMBOL		0xFF02014A
#define OID_SKGE_STAT_RX_SHORTS		0xFF02014B
#define OID_SKGE_STAT_RX_RUNT		0xFF02014C
#define OID_SKGE_STAT_RX_CEXT		0xFF02014D
#define OID_SKGE_STAT_RX_TOO_LONG	0xFF02014E
#define OID_SKGE_STAT_RX_FCS		0xFF02014F
/* #define OID_SKGE_STAT_RX_UTIL		0xFF020150 */
#define OID_SKGE_STAT_RX_64		0xFF020151
#define OID_SKGE_STAT_RX_127		0xFF020152
#define OID_SKGE_STAT_RX_255		0xFF020153
#define OID_SKGE_STAT_RX_511		0xFF020154
#define OID_SKGE_STAT_RX_1023		0xFF020155
#define OID_SKGE_STAT_RX_MAX		0xFF020156
#define OID_SKGE_STAT_RX_LONGFRAMES	0xFF020157
			
#define OID_SKGE_PHYS_CUR_ADDR		0xFF010120
#define OID_SKGE_PHYS_FAC_ADDR		0xFF010121
#define OID_SKGE_PMD			0xFF010122
#define OID_SKGE_CONNECTOR		0xFF010123
#define OID_SKGE_LINK_CAP		0xFF010124
#define OID_SKGE_LINK_MODE		0xFF010125
#define OID_SKGE_LINK_MODE_STATUS	0xFF010126
#define OID_SKGE_LINK_STATUS		0xFF010127
#define OID_SKGE_FLOWCTRL_CAP		0xFF010128
#define OID_SKGE_FLOWCTRL_MODE		0xFF010129
#define OID_SKGE_FLOWCTRL_STATUS	0xFF01012A
#define OID_SKGE_PHY_OPERATION_CAP	0xFF01012B
#define OID_SKGE_PHY_OPERATION_MODE	0xFF01012C
#define OID_SKGE_PHY_OPERATION_STATUS	0xFF01012D
			
#define OID_SKGE_TRAP			0xFF010130
#define OID_SKGE_TRAP_NUMBER		0xFF010131

#define OID_SKGE_RLMT_MODE		0xFF010140
#define OID_SKGE_RLMT_PORT_NUMBER	0xFF010141
#define OID_SKGE_RLMT_PORT_ACTIVE	0xFF010142
#define OID_SKGE_RLMT_PORT_PREFERRED	0xFF010143
#define OID_SKGE_RLMT_CHANGE_CTS	0xFF020160
#define OID_SKGE_RLMT_CHANGE_TIME	0xFF020161
#define OID_SKGE_RLMT_CHANGE_ESTIM	0xFF020162
#define OID_SKGE_RLMT_CHANGE_THRES	0xFF020163
			
#define OID_SKGE_RLMT_PORT_INDEX	0xFF020164
#define OID_SKGE_RLMT_STATUS		0xFF020165
#define OID_SKGE_RLMT_TX_HELLO_CTS	0xFF020166
#define OID_SKGE_RLMT_RX_HELLO_CTS	0xFF020167
#define OID_SKGE_RLMT_TX_SP_REQ_CTS	0xFF020168
#define OID_SKGE_RLMT_RX_SP_CTS		0xFF020169

#define OID_SKGE_RLMT_MONITOR_NUMBER	0xFF010150			
#define OID_SKGE_RLMT_MONITOR_INDEX	0xFF010151
#define OID_SKGE_RLMT_MONITOR_ADDR	0xFF010152
#define OID_SKGE_RLMT_MONITOR_ERRS	0xFF010153
#define OID_SKGE_RLMT_MONITOR_TIMESTAMP	0xFF010154
#define OID_SKGE_RLMT_MONITOR_ADMIN	0xFF010155

#define OID_SKGE_TX_SW_QUEUE_LEN	0xFF020170
#define OID_SKGE_TX_SW_QUEUE_MAX	0xFF020171
#define OID_SKGE_TX_RETRY		0xFF020172
#define OID_SKGE_RX_INTR_CTS		0xFF020173
#define OID_SKGE_TX_INTR_CTS		0xFF020174
#define OID_SKGE_RX_NO_BUF_CTS		0xFF020175
#define OID_SKGE_TX_NO_BUF_CTS		0xFF020176
#define OID_SKGE_TX_USED_DESCR_NO	0xFF020177
#define OID_SKGE_RX_DELIVERED_CTS	0xFF020178
#define OID_SKGE_RX_OCTETS_DELIV_CTS	0xFF020179
#define OID_SKGE_RX_HW_ERROR_CTS	0xFF02017A
#define OID_SKGE_TX_HW_ERROR_CTS	0xFF02017B
#define OID_SKGE_IN_ERRORS_CTS		0xFF02017C
#define OID_SKGE_OUT_ERROR_CTS		0xFF02017D
#define OID_SKGE_ERR_RECOVERY_CTS	0xFF02017E
#define OID_SKGE_SYSUPTIME		0xFF02017F

#define OID_SKGE_ALL_DATA		0xFF020190			


#define	OID_SKGE_TRAP_SEN_WAR_LOW	500
#define OID_SKGE_TRAP_SEN_WAR_UPP	501
#define	OID_SKGE_TRAP_SEN_ERR_LOW	502
#define OID_SKGE_TRAP_SEN_ERR_UPP	503
#define OID_SKGE_TRAP_RLMT_CHANGE_THRES	520
#define OID_SKGE_TRAP_RLMT_CHANGE_PORT	521
#define OID_SKGE_TRAP_RLMT_PORT_DOWN	522
#define OID_SKGE_TRAP_RLMT_PORT_UP	523
#define OID_SKGE_TRAP_RLMT_SEGMENTATION	524


/*
 * Define error numbers and messages for syslog
 */
#define SK_PNMI_ERR001		(SK_ERRBASE_PNMI + 1)
#define SK_PNMI_ERR001MSG	"SkPnmiGetStruct: Unknown OID"
#define SK_PNMI_ERR002		(SK_ERRBASE_PNMI + 2)
#define SK_PNMI_ERR002MSG	"SkPnmiGetStruct: Cannot read VPD keys"
#define SK_PNMI_ERR003		(SK_ERRBASE_PNMI + 3)
#define SK_PNMI_ERR003MSG	"OidStruct: Called with wrong OID"
#define SK_PNMI_ERR004		(SK_ERRBASE_PNMI + 4)
#define SK_PNMI_ERR004MSG	"OidStruct: Called with wrong action"
#define SK_PNMI_ERR005		(SK_ERRBASE_PNMI + 5)
#define SK_PNMI_ERR005MSG	"Perform: Cannot reset driver"
#define SK_PNMI_ERR006		(SK_ERRBASE_PNMI + 6)
#define SK_PNMI_ERR006MSG	"Perform: Unknown OID action command"
#define SK_PNMI_ERR007		(SK_ERRBASE_PNMI + 7)
#define SK_PNMI_ERR007MSG	"General: Driver description not initialized"
#define SK_PNMI_ERR008		(SK_ERRBASE_PNMI + 8)
#define SK_PNMI_ERR008MSG	"Addr: Tried to get unknown OID"
#define SK_PNMI_ERR009		(SK_ERRBASE_PNMI + 9)
#define SK_PNMI_ERR009MSG	"Addr: Unknown OID"
#define SK_PNMI_ERR010		(SK_ERRBASE_PNMI + 10)
#define SK_PNMI_ERR010MSG	"CsumStat: Unknown OID"
#define SK_PNMI_ERR011		(SK_ERRBASE_PNMI + 11)
#define SK_PNMI_ERR011MSG	"SensorStat: Sensor descr string too long"
#define SK_PNMI_ERR012		(SK_ERRBASE_PNMI + 12)
#define SK_PNMI_ERR012MSG	"SensorStat: Unknown OID"
#define SK_PNMI_ERR013		(SK_ERRBASE_PNMI + 13)
#define SK_PNMI_ERR013MSG	"SensorStat: Unknown OID should be errored before"
#define SK_PNMI_ERR014		(SK_ERRBASE_PNMI + 14)
#define SK_PNMI_ERR014MSG	"Vpd: Cannot read VPD keys"
#define SK_PNMI_ERR015		(SK_ERRBASE_PNMI + 15)
#define SK_PNMI_ERR015MSG	"Vpd: Internal array for VPD keys to small"
#define SK_PNMI_ERR016		(SK_ERRBASE_PNMI + 16)
#define SK_PNMI_ERR016MSG	"Vpd: Key string too long"
#define SK_PNMI_ERR017		(SK_ERRBASE_PNMI + 17)
#define SK_PNMI_ERR017MSG	"Vpd: Invalid VPD status pointer"
#define SK_PNMI_ERR018		(SK_ERRBASE_PNMI + 18)
#define SK_PNMI_ERR018MSG	"Vpd: VPD data not valid"
#define SK_PNMI_ERR019		(SK_ERRBASE_PNMI + 19)
#define SK_PNMI_ERR019MSG	"Vpd: VPD entries list string too long"
#define SK_PNMI_ERR021		(SK_ERRBASE_PNMI + 21)
#define SK_PNMI_ERR021MSG	"Vpd: VPD data string too long"
#define SK_PNMI_ERR022		(SK_ERRBASE_PNMI + 22)
#define SK_PNMI_ERR022MSG	"Vpd: VPD data string too long should be errored before"
#define SK_PNMI_ERR023		(SK_ERRBASE_PNMI + 23)
#define SK_PNMI_ERR023MSG	"Vpd: Unknown OID in get action"
#define SK_PNMI_ERR024		(SK_ERRBASE_PNMI + 24)
#define SK_PNMI_ERR024MSG	"Vpd: Unknown OID in preset/set action"
#define SK_PNMI_ERR025		(SK_ERRBASE_PNMI + 25)
#define SK_PNMI_ERR025MSG	"Vpd: Cannot write VPD after modify entry"
#define SK_PNMI_ERR026		(SK_ERRBASE_PNMI + 26)
#define SK_PNMI_ERR026MSG	"Vpd: Cannot update VPD"
#define SK_PNMI_ERR027		(SK_ERRBASE_PNMI + 27)
#define SK_PNMI_ERR027MSG	"Vpd: Cannot delete VPD entry"
#define SK_PNMI_ERR028		(SK_ERRBASE_PNMI + 28)
#define SK_PNMI_ERR028MSG	"Vpd: Cannot update VPD after delete entry"
#define SK_PNMI_ERR029		(SK_ERRBASE_PNMI + 29)
#define SK_PNMI_ERR029MSG	"General: Driver description string too long"
#define SK_PNMI_ERR030		(SK_ERRBASE_PNMI + 30)
#define SK_PNMI_ERR030MSG	"General: Driver version not initialized"
#define SK_PNMI_ERR031		(SK_ERRBASE_PNMI + 31)
#define SK_PNMI_ERR031MSG	"General: Driver version string too long"
#define SK_PNMI_ERR032		(SK_ERRBASE_PNMI + 32)
#define SK_PNMI_ERR032MSG	"General: Cannot read VPD Name for HW descr"
#define SK_PNMI_ERR033		(SK_ERRBASE_PNMI + 33)
#define SK_PNMI_ERR033MSG	"General: HW description string too long"
#define SK_PNMI_ERR034		(SK_ERRBASE_PNMI + 34)
#define SK_PNMI_ERR034MSG	"General: Unknown OID"
#define SK_PNMI_ERR035		(SK_ERRBASE_PNMI + 35)
#define SK_PNMI_ERR035MSG	"Rlmt: Unknown OID"
#define SK_PNMI_ERR036		(SK_ERRBASE_PNMI + 36)
#define SK_PNMI_ERR036MSG	"Rlmt: Unknown OID should be errored before"
#define SK_PNMI_ERR037		(SK_ERRBASE_PNMI + 37)
#define SK_PNMI_ERR037MSG	"Rlmt: SK_RLMT_MODE_CHANGE event return not 0"
#define SK_PNMI_ERR038		(SK_ERRBASE_PNMI + 38)
#define SK_PNMI_ERR038MSG	"Rlmt: SK_RLMT_PREFPORT_CHANGE event return not 0"
#define SK_PNMI_ERR039		(SK_ERRBASE_PNMI + 39)
#define SK_PNMI_ERR039MSG	"RlmtStat: Unknown OID"
#define SK_PNMI_ERR040		(SK_ERRBASE_PNMI + 40)
#define SK_PNMI_ERR040MSG	"RlmtStat: Unknown OID should be errored before"
#define SK_PNMI_ERR041		(SK_ERRBASE_PNMI + 41)
#define SK_PNMI_ERR041MSG	"MacPrivateConf: Unknown OID"
#define SK_PNMI_ERR042		(SK_ERRBASE_PNMI + 42)
#define SK_PNMI_ERR042MSG	"MacPrivateConf: Unknown OID should be errored before"
#define SK_PNMI_ERR043		(SK_ERRBASE_PNMI + 43)
#define SK_PNMI_ERR043MSG	"MacPrivateConf: SK_HWEV_SET_LMODE returned not 0"
#define SK_PNMI_ERR044		(SK_ERRBASE_PNMI + 44)
#define SK_PNMI_ERR044MSG	"MacPrivateConf: SK_HWEV_SET_FLOWMODE returned not 0"
#define SK_PNMI_ERR045		(SK_ERRBASE_PNMI + 45)
#define SK_PNMI_ERR045MSG	"MacPrivateConf: Unknown OID in set action"
#define SK_PNMI_ERR046		(SK_ERRBASE_PNMI + 46)
#define SK_PNMI_ERR046MSG	"Monitor: Unknown OID"
#define SK_PNMI_ERR047		(SK_ERRBASE_PNMI + 47)
#define SK_PNMI_ERR047MSG	"SirqUpdate: Event function returns not 0"
#define SK_PNMI_ERR048		(SK_ERRBASE_PNMI + 48)
#define SK_PNMI_ERR048MSG	"RlmtUpdate: Event function returns not 0"
#define SK_PNMI_ERR049		(SK_ERRBASE_PNMI + 49)
#define SK_PNMI_ERR049MSG	""
#define SK_PNMI_ERR050		(SK_ERRBASE_PNMI + 50)
#define SK_PNMI_ERR050MSG	"MacUpdate: Cannot update statistic counter"
#define SK_PNMI_ERR051		(SK_ERRBASE_PNMI + 51)
#define SK_PNMI_ERR051MSG	"SkPnmiEvent: Port switch suspicious"
#define SK_PNMI_ERR052		(SK_ERRBASE_PNMI + 52)
#define SK_PNMI_ERR052MSG	"MacPrivateConf: SK_HWEV_SET_ROLE returned not 0"

/*
 * Management counter macros called by the driver
 */
#define SK_PNMI_SET_DRIVER_DESCR(pAC,v)	((pAC)->Pnmi.pDriverDescription = \
	(char *)(v))

#define SK_PNMI_SET_DRIVER_VER(pAC,v)	((pAC)->Pnmi.pDriverVersion = \
	(char *)(v))

#define SK_PNMI_CNT_TX_QUEUE_LEN(pAC,v) \
	{ \
		(pAC)->Pnmi.TxSwQueueLen = (SK_U64)(v); \
		if ((pAC)->Pnmi.TxSwQueueLen > (pAC)->Pnmi.TxSwQueueMax) { \
			(pAC)->Pnmi.TxSwQueueMax = (pAC)->Pnmi.TxSwQueueLen; \
		} \
	}
#define SK_PNMI_CNT_TX_RETRY(pAC)	(((pAC)->Pnmi.TxRetryCts)++)
#define SK_PNMI_CNT_RX_INTR(pAC)	(((pAC)->Pnmi.RxIntrCts)++)
#define SK_PNMI_CNT_TX_INTR(pAC)	(((pAC)->Pnmi.TxIntrCts)++)
#define SK_PNMI_CNT_NO_RX_BUF(pAC)	(((pAC)->Pnmi.RxNoBufCts)++)
#define SK_PNMI_CNT_NO_TX_BUF(pAC)	(((pAC)->Pnmi.TxNoBufCts)++)
#define SK_PNMI_CNT_USED_TX_DESCR(pAC,v) \
	((pAC)->Pnmi.TxUsedDescrNo=(SK_U64)(v));
#define SK_PNMI_CNT_RX_OCTETS_DELIVERED(pAC,v) \
	{ \
		((pAC)->Pnmi.RxDeliveredCts)++; \
		(pAC)->Pnmi.RxOctetsDeliveredCts += (SK_U64)(v); \
	}
#define SK_PNMI_CNT_ERR_RECOVERY(pAC)	(((pAC)->Pnmi.ErrRecoveryCts)++);

#define SK_PNMI_CNT_SYNC_OCTETS(pAC,p,v) \
	{ \
		if ((p) < SK_MAX_MACS) { \
			((pAC)->Pnmi.Port[p].StatSyncCts)++; \
			(pAC)->Pnmi.Port[p].StatSyncOctetsCts += (SK_U64)(v); \
		} \
	}

#define SK_PNMI_CNT_RX_LONGFRAMES(pAC,p) \
	{ \
		if ((p) < SK_MAX_MACS) { \
			((pAC)->Pnmi.Port[p].StatRxLongFrameCts)++; \
		} \
	}

/*
 * Conversion Macros
 */
#define SK_PNMI_PORT_INST2LOG(i)	((unsigned int)(i) - 1)
#define SK_PNMI_PORT_LOG2INST(l)	((unsigned int)(l) + 1)
#define SK_PNMI_PORT_PHYS2LOG(p)	((unsigned int)(p) + 1)
#define SK_PNMI_PORT_LOG2PHYS(pAC,l)	((unsigned int)(l) - 1)
#define SK_PNMI_PORT_PHYS2INST(p)	((unsigned int)(p) + 2)
#define SK_PNMI_PORT_INST2PHYS(pAC,i)	((unsigned int)(i) - 2)

/*
 * Structure definition for SkPnmiGetStruct and SkPnmiSetStruct
 */
#define SK_PNMI_VPD_ENTRIES		20
#define SK_PNMI_VPD_DATALEN		128
#define SK_PNMI_MULTICAST_LISTLEN	64
#define SK_PNMI_SENSOR_ENTRIES		(SK_MAX_SENSORS)
#define SK_PNMI_CHECKSUM_ENTRIES	3
#define SK_PNMI_MAC_ENTRIES		(SK_MAX_MACS + 1)
#define SK_PNMI_MONITOR_ENTRIES		20
#define SK_PNMI_TRAP_ENTRIES		10
#define SK_PNMI_TRAPLEN			128
#define SK_PNMI_STRINGLEN1		80
#define SK_PNMI_STRINGLEN2		25
#define SK_PNMI_TRAP_QUEUE_LEN		512

typedef struct s_PnmiVpd {
	char			VpdKey[5];
	char			VpdValue[SK_PNMI_VPD_DATALEN];
	SK_U8			VpdAccess;
	SK_U8			VpdAction;
} SK_PNMI_VPD;

typedef struct s_PnmiSensor {
	SK_U8			SensorIndex;
	char			SensorDescr[SK_PNMI_STRINGLEN2];
	SK_U8			SensorType;
	SK_U32			SensorValue;
	SK_U32			SensorWarningThresholdLow;
	SK_U32			SensorWarningThresholdHigh;
	SK_U32			SensorErrorThresholdLow;
	SK_U32			SensorErrorThresholdHigh;
	SK_U8			SensorStatus;
	SK_U64			SensorWarningCts;
	SK_U64			SensorErrorCts;
	SK_U64			SensorWarningTimestamp;
	SK_U64			SensorErrorTimestamp;
} SK_PNMI_SENSOR;

typedef struct s_PnmiChecksum {
	SK_U64			ChecksumRxOkCts;
	SK_U64			ChecksumRxUnableCts;
	SK_U64			ChecksumRxErrCts;
	SK_U64			ChecksumTxOkCts;
	SK_U64			ChecksumTxUnableCts;
} SK_PNMI_CHECKSUM;

typedef struct s_PnmiStat {
	SK_U64			StatTxOkCts;
	SK_U64			StatTxOctetsOkCts;
	SK_U64			StatTxBroadcastOkCts;
	SK_U64			StatTxMulticastOkCts;
	SK_U64			StatTxUnicastOkCts;
	SK_U64			StatTxLongFramesCts;
	SK_U64			StatTxBurstCts;
	SK_U64			StatTxPauseMacCtrlCts;
	SK_U64			StatTxMacCtrlCts;
	SK_U64			StatTxSingleCollisionCts;
	SK_U64			StatTxMultipleCollisionCts;
	SK_U64			StatTxExcessiveCollisionCts;
	SK_U64			StatTxLateCollisionCts;
	SK_U64			StatTxDeferralCts;
	SK_U64			StatTxExcessiveDeferralCts;
	SK_U64			StatTxFifoUnderrunCts;
	SK_U64			StatTxCarrierCts;
	SK_U64			Dummy1; /* StatTxUtilization */
	SK_U64			StatTx64Cts;
	SK_U64			StatTx127Cts;
	SK_U64			StatTx255Cts;
	SK_U64			StatTx511Cts;
	SK_U64			StatTx1023Cts;
	SK_U64			StatTxMaxCts;
	SK_U64			StatTxSyncCts;
	SK_U64			StatTxSyncOctetsCts;
	SK_U64			StatRxOkCts;
	SK_U64			StatRxOctetsOkCts;
	SK_U64			StatRxBroadcastOkCts;
	SK_U64			StatRxMulticastOkCts;
	SK_U64			StatRxUnicastOkCts;
	SK_U64			StatRxLongFramesCts;
	SK_U64			StatRxPauseMacCtrlCts;
	SK_U64			StatRxMacCtrlCts;
	SK_U64			StatRxPauseMacCtrlErrorCts;
	SK_U64			StatRxMacCtrlUnknownCts;
	SK_U64			StatRxBurstCts;
	SK_U64			StatRxMissedCts;
	SK_U64			StatRxFramingCts;
	SK_U64			StatRxFifoOverflowCts;
	SK_U64			StatRxJabberCts;
	SK_U64			StatRxCarrierCts;
	SK_U64			StatRxIRLengthCts;
	SK_U64			StatRxSymbolCts;
	SK_U64			StatRxShortsCts;
	SK_U64			StatRxRuntCts;
	SK_U64			StatRxCextCts;
	SK_U64			StatRxTooLongCts;
	SK_U64			StatRxFcsCts;
	SK_U64			Dummy2; /* StatRxUtilization */
	SK_U64			StatRx64Cts;
	SK_U64			StatRx127Cts;
	SK_U64			StatRx255Cts;
	SK_U64			StatRx511Cts;
	SK_U64			StatRx1023Cts;
	SK_U64			StatRxMaxCts;
} SK_PNMI_STAT;

typedef struct s_PnmiConf {
	char			ConfMacCurrentAddr[6];
	char			ConfMacFactoryAddr[6];
	SK_U8			ConfPMD;
	SK_U8			ConfConnector;
	SK_U8			ConfLinkCapability;
	SK_U8			ConfLinkMode;
	SK_U8			ConfLinkModeStatus;
	SK_U8			ConfLinkStatus;
	SK_U8			ConfFlowCtrlCapability;
	SK_U8			ConfFlowCtrlMode;
	SK_U8			ConfFlowCtrlStatus;
	SK_U8			ConfPhyOperationCapability;
	SK_U8			ConfPhyOperationMode;
	SK_U8			ConfPhyOperationStatus;
} SK_PNMI_CONF;

typedef struct s_PnmiRlmt {
	SK_U32			RlmtIndex;
	SK_U32			RlmtStatus;
	SK_U64			RlmtTxHelloCts;
	SK_U64			RlmtRxHelloCts;
	SK_U64			RlmtTxSpHelloReqCts;
	SK_U64			RlmtRxSpHelloCts;
} SK_PNMI_RLMT;

typedef struct s_PnmiRlmtMonitor {
	SK_U32			RlmtMonitorIndex;
	char			RlmtMonitorAddr[6];
	SK_U64			RlmtMonitorErrorCts;
	SK_U64			RlmtMonitorTimestamp;
	SK_U8			RlmtMonitorAdmin;
} SK_PNMI_RLMT_MONITOR;

typedef struct s_PnmiRequestStatus {
	SK_U32			ErrorStatus;
	SK_U32			ErrorOffset;
} SK_PNMI_REQUEST_STATUS;

typedef struct s_PnmiStrucData {
	SK_U32			MgmtDBVersion;
	SK_PNMI_REQUEST_STATUS	ReturnStatus;
	SK_U32			VpdFreeBytes;
	char			VpdEntriesList[SK_PNMI_VPD_DATALEN];
	SK_U32			VpdEntriesNumber;
	SK_PNMI_VPD		Vpd[SK_PNMI_VPD_ENTRIES];
	SK_U32			PortNumber;
	SK_U32			DeviceType;
	char			DriverDescr[SK_PNMI_STRINGLEN1];
	char			DriverVersion[SK_PNMI_STRINGLEN2];
	char			HwDescr[SK_PNMI_STRINGLEN1];
	char			HwVersion[SK_PNMI_STRINGLEN2];
	SK_U16			Chipset;
	SK_U32			Action;
	SK_U32			TestResult;
	SK_U8			BusType;
	SK_U8			BusSpeed;
	SK_U8			BusWidth;
	SK_U8			SensorNumber;
	SK_PNMI_SENSOR		Sensor[SK_PNMI_SENSOR_ENTRIES];
	SK_U8			ChecksumNumber;
	SK_PNMI_CHECKSUM	Checksum[SK_PNMI_CHECKSUM_ENTRIES];
	SK_PNMI_STAT		Stat[SK_PNMI_MAC_ENTRIES];
	SK_PNMI_CONF		Conf[SK_PNMI_MAC_ENTRIES];
	SK_U8			RlmtMode;
	SK_U32			RlmtPortNumber;
	SK_U8			RlmtPortActive;
	SK_U8			RlmtPortPreferred;
	SK_U64			RlmtChangeCts;
	SK_U64			RlmtChangeTime;
	SK_U64			RlmtChangeEstimate;
	SK_U64			RlmtChangeThreshold;
	SK_PNMI_RLMT		Rlmt[SK_MAX_MACS];
	SK_U32			RlmtMonitorNumber;
	SK_PNMI_RLMT_MONITOR	RlmtMonitor[SK_PNMI_MONITOR_ENTRIES];
	SK_U32			TrapNumber;
	SK_U8			Trap[SK_PNMI_TRAP_QUEUE_LEN];
	SK_U64			TxSwQueueLen;
	SK_U64			TxSwQueueMax;
	SK_U64			TxRetryCts;
	SK_U64			RxIntrCts;
	SK_U64			TxIntrCts;
	SK_U64			RxNoBufCts;
	SK_U64			TxNoBufCts;
	SK_U64			TxUsedDescrNo;
	SK_U64			RxDeliveredCts;
	SK_U64			RxOctetsDeliveredCts;
	SK_U64			RxHwErrorsCts;
	SK_U64			TxHwErrorsCts;
	SK_U64			InErrorsCts;
	SK_U64			OutErrorsCts;
	SK_U64			ErrRecoveryCts;
	SK_U64			SysUpTime;
} SK_PNMI_STRUCT_DATA;

#define SK_PNMI_STRUCT_SIZE	(sizeof(SK_PNMI_STRUCT_DATA))
#define SK_PNMI_MIN_STRUCT_SIZE	((unsigned int)(SK_UPTR)\
				 &(((SK_PNMI_STRUCT_DATA *)0)->VpdFreeBytes))
							/*
							 * ReturnStatus field
							 * must be located
							 * before VpdFreeBytes
							 */

/*
 * Various definitions
 */
#define SK_PNMI_MAX_PROTOS		3

#define SK_PNMI_SCNT_NOT		64
#define SK_PNMI_CNT_NO			67

/*
 * Estimate data structure
 */
typedef struct s_PnmiEstimate {
	unsigned int		EstValueIndex;
	SK_U64			EstValue[7];
	SK_U64			Estimate;
	SK_TIMER		EstTimer;
} SK_PNMI_ESTIMATE;


/*
 * PNMI specific adatper context structure
 */
typedef struct s_PnmiPort {
	SK_U32			CounterHigh[SK_PNMI_SCNT_NOT];
	SK_U64			CounterOffset[SK_PNMI_CNT_NO];
	SK_U64			StatSyncCts;
	SK_U64			StatSyncOctetsCts;
	SK_U64			StatRxLongFrameCts;
	SK_BOOL			ActiveFlag;
} SK_PNMI_PORT;

typedef struct s_PnmiData {
	SK_PNMI_PORT		Port[SK_MAX_MACS];
	SK_U64			VirtualCounterOffset[SK_PNMI_CNT_NO];
	SK_U32			TestResult;
	char			HwVersion[10];

	char			*pDriverDescription;
	char			*pDriverVersion;

	char			TrapBuf[SK_PNMI_TRAP_QUEUE_LEN];
	unsigned int		TrapBufFree;
	unsigned int		TrapQueueBeg;
	unsigned int		TrapQueueEnd;
	unsigned int		TrapBufPad;
	unsigned int		TrapUnique;

	int			MacUpdatedFlag;
	int			RlmtUpdatedFlag;
	int			SirqUpdatedFlag;

	SK_U64			RlmtChangeCts;
	SK_U64			RlmtChangeTime;
	SK_PNMI_ESTIMATE	RlmtChangeEstimate;
	SK_U64			RlmtChangeThreshold;

	SK_U32			DeviceType;
	char			PciBusSpeed;
	char			PciBusWidth;
	char			PMD;
	char			Connector;

	SK_U64			TxSwQueueLen;
	SK_U64			TxSwQueueMax;
	SK_U64			TxRetryCts;
	SK_U64			RxIntrCts;
	SK_U64			TxIntrCts;
	SK_U64			RxNoBufCts;
	SK_U64			TxNoBufCts;
	SK_U64			TxUsedDescrNo;
	SK_U64			RxDeliveredCts;
	SK_U64			RxOctetsDeliveredCts;
	SK_U64			ErrRecoveryCts;
	SK_U64			StartUpTime;
} SK_PNMI;


/*
 * Function prototypes
 */
extern int SkPnmiInit(SK_AC *pAc, SK_IOC IoC, int level);
extern int SkPnmiGetVar(SK_AC *pAc, SK_IOC IoC, SK_U32 Id, void* pBuf,
	unsigned int* pLen, SK_U32 Instance);
extern int SkPnmiPreSetVar(SK_AC *pAc, SK_IOC IoC, SK_U32 Id,
	void* pBuf, unsigned int *pLen, SK_U32 Instance);
extern int SkPnmiSetVar(SK_AC *pAc, SK_IOC IoC, SK_U32 Id, void* pBuf,
	unsigned int *pLen, SK_U32 Instance);
extern int SkPnmiGetStruct(SK_AC *pAc, SK_IOC IoC, void* pBuf,
	unsigned int *pLen);
extern int SkPnmiPreSetStruct(SK_AC *pAc, SK_IOC IoC, void* pBuf,
	unsigned int *pLen);
extern int SkPnmiSetStruct(SK_AC *pAc, SK_IOC IoC, void* pBuf,
	unsigned int *pLen);
extern int SkPnmiEvent(SK_AC *pAc, SK_IOC IoC, SK_U32 Event,
	SK_EVPARA Param);

#endif
