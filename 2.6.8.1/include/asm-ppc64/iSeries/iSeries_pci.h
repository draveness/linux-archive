#ifndef _ISERIES_64_PCI_H
#define _ISERIES_64_PCI_H

/************************************************************************/
/* File iSeries_pci.h created by Allan Trautman on Tue Feb 20, 2001.    */
/************************************************************************/
/* Define some useful macros for the iSeries pci routines.              */
/* Copyright (C) 2001  Allan H Trautman, IBM Corporation                */
/*                                                                      */
/* This program is free software; you can redistribute it and/or modify */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation; either version 2 of the License, or    */
/* (at your option) any later version.                                  */
/*                                                                      */
/* This program is distributed in the hope that it will be useful,      */ 
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU General Public License for more details.                         */
/*                                                                      */
/* You should have received a copy of the GNU General Public License    */ 
/* along with this program; if not, write to the:                       */
/* Free Software Foundation, Inc.,                                      */ 
/* 59 Temple Place, Suite 330,                                          */ 
/* Boston, MA  02111-1307  USA                                          */
/************************************************************************/
/* Change Activity:                                                     */
/*   Created Feb 20, 2001                                               */
/*   Added device reset, March 22, 2001                                 */
/*   Ported to ppc64, May 25, 2001                                      */
/* End Change Activity                                                  */
/************************************************************************/

#include <asm/iSeries/HvCallPci.h>
#include <asm/abs_addr.h>

struct pci_dev;				/* For Forward Reference        */
struct iSeries_Device_Node;

/************************************************************************/
/* Gets iSeries Bus, SubBus, DevFn using iSeries_Device_Node structure */
/************************************************************************/

#define ISERIES_BUS(DevPtr)	DevPtr->DsaAddr.Dsa.busNumber
#define ISERIES_SUBBUS(DevPtr)	DevPtr->DsaAddr.Dsa.subBusNumber
#define ISERIES_DEVICE(DevPtr)	DevPtr->DsaAddr.Dsa.deviceId
#define ISERIES_DSA(DevPtr)	DevPtr->DsaAddr.DsaAddr
#define ISERIES_DEVFUN(DevPtr)	DevPtr->DevFn
#define ISERIES_DEVNODE(PciDev) ((struct iSeries_Device_Node*)PciDev->sysdata)

#define EADsMaxAgents 7

/************************************************************************/
/* Decodes Linux DevFn to iSeries DevFn, bridge device, or function.    */
/* For Linux, see PCI_SLOT and PCI_FUNC in include/linux/pci.h          */
/************************************************************************/

#define ISERIES_PCI_AGENTID(idsel,func)	((idsel & 0x0F) << 4) | (func  & 0x07)
#define ISERIES_ENCODE_DEVICE(agentid)	((0x10) | ((agentid&0x20)>>2) | (agentid&07))

#define ISERIES_GET_DEVICE_FROM_SUBBUS(subbus)   ((subbus >> 5) & 0x7)
#define ISERIES_GET_FUNCTION_FROM_SUBBUS(subbus) ((subbus >> 2) & 0x7)

/*
 * N.B. the ISERIES_DECODE_* macros are not used anywhere, and I think
 * the 0x71 (at least) must be wrong - 0x78 maybe?  -- paulus.
 */
#define ISERIES_DECODE_DEVFN(linuxdevfn)  (((linuxdevfn & 0x71) << 1) | (linuxdevfn & 0x07))
#define ISERIES_DECODE_DEVICE(linuxdevfn) (((linuxdevfn & 0x38) >> 3) |(((linuxdevfn & 0x40) >> 2) + 0x10))
#define ISERIES_DECODE_FUNCTION(linuxdevfn) (linuxdevfn & 0x07)

/************************************************************************/
/* Converts Virtual Address to Real Address for Hypervisor calls        */
/************************************************************************/

#define ISERIES_HV_ADDR(virtaddr)  (0x8000000000000000 | virt_to_abs(virtaddr))

/************************************************************************/
/* iSeries Device Information                                           */
/************************************************************************/

struct iSeries_Device_Node {
	struct list_head Device_List;
	struct pci_dev* PciDev;         /* Pointer to pci_dev structure*/
        union HvDsaMap	DsaAddr;	/* Direct Select Address       */
                                        /* busNumber,subBusNumber,     */ 
	                                /* deviceId, barNumber         */
	HvAgentId       AgentId;	/* Hypervisor DevFn            */
	int             DevFn;          /* Linux devfn                 */
	int             BarOffset;
	int             Irq;            /* Assigned IRQ                */
	int             ReturnCode;	/* Return Code Holder          */
	int             IoRetry;        /* Current Retry Count         */
	int             Flags;          /* Possible flags(disable/bist)*/
	u16             Vendor;         /* Vendor ID                   */
	u8              LogicalSlot;    /* Hv Slot Index for Tces      */
	struct iommu_table* iommu_table;/* Device TCE Table            */ 
	u8              PhbId;          /* Phb Card is on.             */
	u16             Board;          /* Board Number                */
	u8              FrameId;	/* iSeries spcn Frame Id       */
	char            CardLocation[4];/* Char format of planar vpd   */
	char            Location[20];   /* Frame  1, Card C10          */
};

/************************************************************************/
/* Location Data extracted from the VPD list and device info.           */
/************************************************************************/

struct LocationDataStruct { 	/* Location data structure for device  */
	u16  Bus;               /* iSeries Bus Number              0x00*/
	u16  Board;             /* iSeries Board                   0x02*/
	u8   FrameId;           /* iSeries spcn Frame Id           0x04*/
	u8   PhbId;             /* iSeries Phb Location            0x05*/
	u8   AgentId;           /* iSeries AgentId                 0x06*/
	u8   Card;
	char CardLocation[4];      
};

typedef struct LocationDataStruct  LocationData;
#define LOCATION_DATA_SIZE      48

/************************************************************************/
/* Functions                                                            */
/************************************************************************/

extern LocationData* iSeries_GetLocationData(struct pci_dev* PciDev);
extern int           iSeries_Device_Information(struct pci_dev*,char*, int);
extern void          iSeries_Get_Location_Code(struct iSeries_Device_Node*);
extern int           iSeries_Device_ToggleReset(struct pci_dev* PciDev, int AssertTime, int DelayTime);

#endif /* _ISERIES_64_PCI_H */
