/* Copyright(c) 2000, Compaq Computer Corporation 
 * Fibre Channel Host Bus Adapter 
 * 64-bit, 66MHz PCI 
 * Originally developed and tested on:
 * (front): [chip] Tachyon TS HPFC-5166A/1.2  L2C1090 ...
 *          SP# P225CXCBFIEL6T, Rev XC
 *          SP# 161290-001, Rev XD
 * (back): Board No. 010008-001 A/W Rev X5, FAB REV X5
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * Written by Don Zimmerman
 * IOCTL and procfs added by Jouke Numan
 * SMP testing by Chel Van Gennip
 *
 * portions copied from:
 * QLogic CPQFCTS SCSI-FCP
 * Written by Erik H. Moe, ehm@cris.com
 * Copyright 1995, Erik H. Moe
 * Renamed and updated to 1.3.x by Michael Griffith <grif@cs.ucr.edu> 
 * Chris Loveland <cwl@iol.unh.edu> to support the isp2100 and isp2200
*/


#define LinuxVersionCode(v, p, s) (((v)<<16)+((p)<<8)+(s))

#include <linux/blk.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/ioport.h>  // request_region() prototype
#include <linux/vmalloc.h> // ioremap()
#ifdef __alpha__
#define __KERNEL_SYSCALLS__
#endif
#include <asm/unistd.h>
#include <asm/io.h>
#include <asm/uaccess.h>   // ioctl related
#include <asm/irq.h>
#if LINUX_VERSION_CODE < LinuxVersionCode(2,3,18)
#include <asm/spinlock.h>
#else
#include <linux/spinlock.h>
#endif
#include "sd.h"
#include <scsi/scsi_ioctl.h>
#include "hosts.h"
#include "cpqfcTSchip.h"
#include "cpqfcTSstructs.h"

#include "cpqfcTS.h"

#include <linux/module.h>
/* Embedded module documentation macros - see module.h */
MODULE_AUTHOR("Compaq Computer Corporation");
MODULE_DESCRIPTION("Driver for Compaq 64-bit/66Mhz PCI Fibre Channel HBA");

// This struct was originally defined in 
// /usr/src/linux/include/linux/proc_fs.h
// since it's only partially implemented, we only use first
// few fields...
// NOTE: proc_fs changes in 2.4 kernel

#if LINUX_VERSION_CODE < LinuxVersionCode(2,3,27)
static struct proc_dir_entry proc_scsi_cpqfcTS =
{
  PROC_SCSI_CPQFCTS,           // ushort low_ino (enumerated list)
  7,                           // ushort namelen
  DEV_NAME,                    // const char* name
  S_IFDIR | S_IRUGO | S_IXUGO, // mode_t mode
  2                            // nlink_t nlink
	                       // etc. ...
};


#endif



/* local function to load our per-HBA (local) data for chip
   registers, FC link state, all FC exchanges, etc.

   We allocate space and compute address offsets for the
   most frequently accessed addresses; others (like World Wide
   Name) are not necessary.
   
*/
static void Cpqfc_initHBAdata( CPQFCHBA *cpqfcHBAdata, struct pci_dev *PciDev )
{
             
  cpqfcHBAdata->PciDev = PciDev; // copy PCI info ptr

  // since x86 port space is 64k, we only need the lower 16 bits
  cpqfcHBAdata->fcChip.Registers.IOBaseL = 
    PciDev->base_address[1] & PCI_BASE_ADDRESS_IO_MASK;
  
  cpqfcHBAdata->fcChip.Registers.IOBaseU = 
    PciDev->base_address[2] & PCI_BASE_ADDRESS_IO_MASK;
  
  // 32-bit memory addresses
  cpqfcHBAdata->fcChip.Registers.MemBase = 
    PciDev->base_address[3] & PCI_BASE_ADDRESS_MEM_MASK;

  cpqfcHBAdata->fcChip.Registers.ReMapMemBase = 
    ioremap( PciDev->base_address[3] & PCI_BASE_ADDRESS_MEM_MASK,
             0x200);
  
  cpqfcHBAdata->fcChip.Registers.RAMBase = 
    PciDev->base_address[4];
  
  cpqfcHBAdata->fcChip.Registers.SROMBase =  // NULL for HP TS adapter
    PciDev->base_address[5];
  
  // now the Tachlite chip registers
  // the REGISTER struct holds both the physical address & last
  // written value (some TL registers are WRITE ONLY)

  cpqfcHBAdata->fcChip.Registers.SFQconsumerIndex.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_SFQ_CONSUMER_INDEX;

  cpqfcHBAdata->fcChip.Registers.ERQproducerIndex.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_ERQ_PRODUCER_INDEX;
      
  // TL Frame Manager
  cpqfcHBAdata->fcChip.Registers.FMconfig.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_FM_CONFIG;
  cpqfcHBAdata->fcChip.Registers.FMcontrol.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_FM_CONTROL;
  cpqfcHBAdata->fcChip.Registers.FMstatus.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_FM_STATUS;
  cpqfcHBAdata->fcChip.Registers.FMLinkStatus1.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_FM_LINK_STAT1;
  cpqfcHBAdata->fcChip.Registers.FMLinkStatus2.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_FM_LINK_STAT2;
  cpqfcHBAdata->fcChip.Registers.FMBB_CreditZero.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_FM_BB_CREDIT0;
      
      // TL Control Regs
  cpqfcHBAdata->fcChip.Registers.TYconfig.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_TACH_CONFIG;
  cpqfcHBAdata->fcChip.Registers.TYcontrol.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_TACH_CONTROL;
  cpqfcHBAdata->fcChip.Registers.TYstatus.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_TACH_STATUS;
  cpqfcHBAdata->fcChip.Registers.rcv_al_pa.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_FM_RCV_AL_PA;
  cpqfcHBAdata->fcChip.Registers.ed_tov.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + TL_MEM_FM_ED_TOV;


  cpqfcHBAdata->fcChip.Registers.INTEN.address = 
	        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + IINTEN;
  cpqfcHBAdata->fcChip.Registers.INTPEND.address = 
	        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + IINTPEND;
  cpqfcHBAdata->fcChip.Registers.INTSTAT.address = 
        cpqfcHBAdata->fcChip.Registers.ReMapMemBase + IINTSTAT;

  DEBUG_PCI(printk("  cpqfcHBAdata->fcChip.Registers. :\n"));
  DEBUG_PCI(printk("    IOBaseL = %x\n", 
    cpqfcHBAdata->fcChip.Registers.IOBaseL));
  DEBUG_PCI(printk("    IOBaseU = %x\n", 
    cpqfcHBAdata->fcChip.Registers.IOBaseU));
  
  printk(" ioremap'd Membase: %p\n", cpqfcHBAdata->fcChip.Registers.ReMapMemBase);
  
  DEBUG_PCI(printk("    SFQconsumerIndex.address = %p\n", 
    cpqfcHBAdata->fcChip.Registers.SFQconsumerIndex.address));
  DEBUG_PCI(printk("    ERQproducerIndex.address = %p\n", 
    cpqfcHBAdata->fcChip.Registers.ERQproducerIndex.address));
  DEBUG_PCI(printk("    TYconfig.address = %p\n", 
    cpqfcHBAdata->fcChip.Registers.TYconfig.address));
  DEBUG_PCI(printk("    FMconfig.address = %p\n", 
    cpqfcHBAdata->fcChip.Registers.FMconfig.address));
  DEBUG_PCI(printk("    FMcontrol.address = %p\n", 
    cpqfcHBAdata->fcChip.Registers.FMcontrol.address));

  // set default options for FC controller (chip)
  cpqfcHBAdata->fcChip.Options.initiator = 1;  // default: SCSI initiator
  cpqfcHBAdata->fcChip.Options.target = 0;     // default: SCSI target
  cpqfcHBAdata->fcChip.Options.extLoopback = 0;// default: no loopback @GBIC
  cpqfcHBAdata->fcChip.Options.intLoopback = 0;// default: no loopback inside chip

  // set highest and lowest FC-PH version the adapter/driver supports
  // (NOT strict compliance)
  cpqfcHBAdata->fcChip.highest_FCPH_ver = FC_PH3;
  cpqfcHBAdata->fcChip.lowest_FCPH_ver = FC_PH43;

  // set function points for this controller / adapter
  cpqfcHBAdata->fcChip.ResetTachyon = CpqTsResetTachLite;
  cpqfcHBAdata->fcChip.FreezeTachyon = CpqTsFreezeTachlite;
  cpqfcHBAdata->fcChip.UnFreezeTachyon = CpqTsUnFreezeTachlite;
  cpqfcHBAdata->fcChip.CreateTachyonQues = CpqTsCreateTachLiteQues;
  cpqfcHBAdata->fcChip.DestroyTachyonQues = CpqTsDestroyTachLiteQues;
  cpqfcHBAdata->fcChip.InitializeTachyon = CpqTsInitializeTachLite;  
  cpqfcHBAdata->fcChip.LaserControl = CpqTsLaserControl;  
  cpqfcHBAdata->fcChip.ProcessIMQEntry = CpqTsProcessIMQEntry;
  cpqfcHBAdata->fcChip.InitializeFrameManager = CpqTsInitializeFrameManager;;  
  cpqfcHBAdata->fcChip.ReadWriteWWN = CpqTsReadWriteWWN;
  cpqfcHBAdata->fcChip.ReadWriteNVRAM = CpqTsReadWriteNVRAM;

 

}


/* (borrowed from linux/drivers/scsi/hosts.c) */
static void launch_FCworker_thread(struct Scsi_Host *HostAdapter)
{
  DECLARE_MUTEX_LOCKED(sem);

  CPQFCHBA *cpqfcHBAdata = (CPQFCHBA *)HostAdapter->hostdata;

  ENTER("launch_FC_worker_thread");
             
  cpqfcHBAdata->notify_wt = &sem;

  kernel_thread((int (*)(void *))cpqfcTSWorkerThread, 
                          (void *) HostAdapter, 0);
  /*
   * Now wait for the kernel error thread to initialize itself

   */
  down (&sem);
  cpqfcHBAdata->notify_wt = NULL;

  LEAVE("launch_FC_worker_thread");
 
}


/* "Entry" point to discover if any supported PCI 
   bus adapter can be found
*/
// We're supporting:
// Compaq 64-bit, 66MHz HBA with Tachyon TS
// Agilent XL2 
#define HBA_TYPES 2


int cpqfcTS_detect(Scsi_Host_Template *ScsiHostTemplate)
{
  int NumberOfAdapters=0; // how many of our PCI adapters are found?
  struct pci_dev *PciDev = NULL;
  struct Scsi_Host *HostAdapter = NULL;
  CPQFCHBA *cpqfcHBAdata = NULL; 
  struct timer_list *cpqfcTStimer = NULL;
  SupportedPCIcards PCIids[HBA_TYPES];
  int i;
  
  ENTER("cpqfcTS_detect");
  
#if LINUX_VERSION_CODE < LinuxVersionCode(2,3,27)
  ScsiHostTemplate->proc_dir = &proc_scsi_cpqfcTS;
#else
  ScsiHostTemplate->proc_name = "cpqfcTS";
#endif
  
  if( pci_present() == 0) // no PCI busses?
  {
    printk( "  no PCI bus?@#!\n");
    return NumberOfAdapters;
  }

  // what HBA adapters are we supporting?
  PCIids[0].vendor_id = PCI_VENDOR_ID_COMPAQ;
  PCIids[0].device_id = CPQ_DEVICE_ID;
  PCIids[1].vendor_id = PCI_VENDOR_ID_HP; // i.e. 103Ch (Agilent == HP for now)
  PCIids[1].device_id = AGILENT_XL2_ID;   // i.e. 1029h

  for( i=0; i < HBA_TYPES; i++)
  {
    // look for all HBAs of each type

    while( (PciDev =
      pci_find_device( PCIids[i].vendor_id, PCIids[i].device_id, PciDev) ))
    {
      // NOTE: (kernel 2.2.12-32) limits allocation to 128k bytes...
      printk(" scsi_register allocating %d bytes for FC HBA\n",
		      (ULONG)sizeof(CPQFCHBA));

      HostAdapter = scsi_register( ScsiHostTemplate, sizeof( CPQFCHBA ) );
      
      if(HostAdapter == NULL)
      	continue;
      DEBUG_PCI( printk("  HBA found!\n"));
      DEBUG_PCI( printk("  HostAdapter->PciDev->irq = %u\n", PciDev->irq) );
      DEBUG_PCI(printk("  PciDev->baseaddress[]= %lx\n", PciDev->base_address[0]));
      DEBUG_PCI(printk("  PciDev->baseaddress[]= %lx\n", PciDev->base_address[1]));
      DEBUG_PCI(printk("  PciDev->baseaddress[]= %lx\n", PciDev->base_address[2]));
      DEBUG_PCI(printk("  PciDev->baseaddress[]= %lx\n", PciDev->base_address[3]));

      
      HostAdapter->irq = PciDev->irq;  // copy for Scsi layers
      
      // HP Tachlite uses two (255-byte) ranges of Port I/O (lower & upper),
      // for a total I/O port address space of 512 bytes.
      // mask out the I/O port address (lower) & record
      HostAdapter->io_port = (unsigned int)
	     PciDev->base_address[1] & PCI_BASE_ADDRESS_IO_MASK;
      HostAdapter->n_io_port = 0xff;
      
      // i.e., expect 128 targets (arbitrary number), while the
      //  RA-4000 supports 32 LUNs
      HostAdapter->max_id =  0;   // incremented as devices log in    
      HostAdapter->max_lun = CPQFCTS_MAX_LUN;         // LUNs per FC device
      HostAdapter->max_channel = CPQFCTS_MAX_CHANNEL; // multiple busses?
      HostAdapter->hostt->use_new_eh_code = 1; // new error handling
      
      // get the pointer to our HBA specific data... (one for
      // each HBA on the PCI bus(ses)).
      cpqfcHBAdata = (CPQFCHBA *)HostAdapter->hostdata;
      
      // make certain our data struct is clear
      memset( cpqfcHBAdata, 0, sizeof( CPQFCHBA ) );


      // initialize our HBA info
      cpqfcHBAdata->HBAnum = NumberOfAdapters;

      cpqfcHBAdata->HostAdapter = HostAdapter; // back ptr
      Cpqfc_initHBAdata( cpqfcHBAdata, PciDev ); // fill MOST fields
     
      cpqfcHBAdata->HBAnum = NumberOfAdapters;


      // request necessary resources and check for conflicts
      if( request_irq( HostAdapter->irq,
		       cpqfcTS_intr_handler,
	               SA_INTERRUPT | SA_SHIRQ,
	               DEV_NAME,
		       HostAdapter) )
      {
	printk(" IRQ %u already used\n", HostAdapter->irq);
        scsi_unregister( HostAdapter);
	continue;
      }

      // Since we have two 256-byte I/O port ranges (upper
      // and lower), check them both
      if( check_region( cpqfcHBAdata->fcChip.Registers.IOBaseU, 0xff) )
      {
	printk("  cpqfcTS address in use: %x\n", 
			cpqfcHBAdata->fcChip.Registers.IOBaseU);
	free_irq( HostAdapter->irq, HostAdapter);
        scsi_unregister( HostAdapter);
	continue;
      }	
      
      if( check_region( cpqfcHBAdata->fcChip.Registers.IOBaseL, 0xff) )
      {
  	printk("  cpqfcTS address in use: %x\n", 
	      			cpqfcHBAdata->fcChip.Registers.IOBaseL);
	free_irq( HostAdapter->irq, HostAdapter);
        scsi_unregister( HostAdapter);
	continue;
      }	
      
      // OK, we should be able to grab everything we need now.
      request_region( cpqfcHBAdata->fcChip.Registers.IOBaseL, 0xff, DEV_NAME);
      request_region( cpqfcHBAdata->fcChip.Registers.IOBaseU, 0xff, DEV_NAME);
      DEBUG_PCI(printk("  Requesting 255 I/O addresses @ %x\n",
        cpqfcHBAdata->fcChip.Registers.IOBaseL ));
      DEBUG_PCI(printk("  Requesting 255 I/O addresses @ %x\n",
        cpqfcHBAdata->fcChip.Registers.IOBaseU ));

      
      // start our kernel worker thread

      launch_FCworker_thread(HostAdapter);


      // start our TimerTask...

      cpqfcTStimer = &cpqfcHBAdata->cpqfcTStimer;

      init_timer( cpqfcTStimer); // Linux clears next/prev values
      cpqfcTStimer->expires = jiffies + HZ; // one second
      cpqfcTStimer->data = (unsigned long)cpqfcHBAdata; // this adapter
      cpqfcTStimer->function = cpqfcTSheartbeat; // handles timeouts, housekeeping

      add_timer( cpqfcTStimer);  // give it to Linux


      // now initialize our hardware...

      cpqfcHBAdata->fcChip.InitializeTachyon( cpqfcHBAdata, 1,1);

      cpqfcHBAdata->fcStatsTime = jiffies;  // (for FC Statistics delta)
      
      // give our HBA time to initialize and login current devices...
      {
	// The Brocade switch (e.g. 2400, 2010, etc.) as of March 2000,
	// has the following algorithm for FL_Port startup:
	// Time(sec) Action
	// 0:        Device Plugin and LIP(F7,F7) transmission
	// 1.0       LIP incoming
        // 1.027     LISA incoming, no CLS! (link not up)
	// 1.028     NOS incoming (switch test for N_Port)
        // 1.577     ED_TOV expired, transmit LIPs again	
	// 3.0       LIP(F8,F7) incoming (switch passes Tach Prim.Sig)
	// 3.028     LILP received, link up, FLOGI starts
	// slowest(worst) case, measured on 1Gb Finisar GT analyzer
	
	int wait_time;
        for( wait_time = jiffies + 4*HZ; wait_time > jiffies; )
	  schedule();  // (our worker task needs to run)

      }
      
      NumberOfAdapters++; 
    } // end of while()
  }

  LEAVE("cpqfcTS_detect");
 
  return NumberOfAdapters;
}


static void my_ioctl_done (Scsi_Cmnd * SCpnt)
{
    struct request * req;
    
    req = &SCpnt->request;
    req->rq_status = RQ_SCSI_DONE; /* Busy, but indicate request done */
  
    if (req->sem != NULL) {
	up(req->sem);
    }
}   



int cpqfcTS_ioctl( Scsi_Device *ScsiDev, int Cmnd, void *arg)
{
  int result = 0;
  struct Scsi_Host *HostAdapter = ScsiDev->host;
  CPQFCHBA *cpqfcHBAdata = (CPQFCHBA *)HostAdapter->hostdata;
  PTACHYON fcChip = &cpqfcHBAdata->fcChip;
  PFC_LOGGEDIN_PORT pLoggedInPort;
  Scsi_Cmnd DumCmnd;
  int i, j;
  VENDOR_IOCTL_REQ ioc;
  cpqfc_passthru_t *vendor_cmd;
  Scsi_Device *SDpnt;
  Scsi_Cmnd *ScsiPassThruCmnd;
  unsigned long flags;

  ENTER("cpqfcTS_ioctl");
  
  // can we find an FC device mapping to this SCSI target?
  DumCmnd.channel = ScsiDev->channel;		// For searching
  DumCmnd.target  = ScsiDev->id;
  pLoggedInPort = fcFindLoggedInPort( fcChip,
    &DumCmnd, // search Scsi Nexus
    0,        // DON'T search linked list for FC port id
    NULL,     // DON'T search linked list for FC WWN
    NULL);    // DON'T care about end of list
 
  if( pLoggedInPort == NULL )      // not found!
  {
    result = -ENXIO;
  }
 
  else  // we know what FC device to operate on...
  {
    switch (Cmnd) 
    {
      // Passthrough provides a mechanism to bypass the RAID
      // or other controller and talk directly to the devices
      // (e.g. physical disk drive)
      // Passthrough commands, unfortunately, tend to be vendor
      // specific; this is tailored to COMPAQ's RAID (RA4x00)
      case CPQFCTS_SCSI_PASSTHRU:
      {
	void *buf = NULL; // for kernel space buffer for user data
	
	if( !arg)
	  return -EINVAL;

	// must be super user to send stuff directly to the
	// controller and/or physical drives...
	if( !suser() )
	  return -EPERM;

	// copy the caller's struct to our space.
        copy_from_user_ret( &ioc, arg, 
			  sizeof( VENDOR_IOCTL_REQ), -EFAULT);

	vendor_cmd = ioc.argp;  // i.e., CPQ specific command struct

	// If necessary, grab a kernel/DMA buffer
	if( vendor_cmd->len)
	{
  	  buf = kmalloc( vendor_cmd->len, GFP_KERNEL);
	  if( !buf)
	    return -ENOMEM;
	}

        // Now build a SCSI_CMND to pass down...
	// This function allocates and sets Scsi_Cmnd ptrs such as
	//  ->channel, ->target, ->host
        ScsiPassThruCmnd = scsi_allocate_device(NULL, ScsiDev, 1);

        // Need data from user?
	// make sure caller's buffer is in kernel space.
	if( (vendor_cmd->rw_flag == VENDOR_WRITE_OPCODE) &&
	    vendor_cmd->len)
          copy_from_user_ret( buf, vendor_cmd->bufp, vendor_cmd->len, -EFAULT);
	    
	// copy the CDB (if/when MAX_COMMAND_SIZE is 16, remove copy below)
        memcpy( &ScsiPassThruCmnd->cmnd[0], 
		&vendor_cmd->cdb[0], 
		MAX_COMMAND_SIZE);  
        // we want to copy all 16 bytes into the FCP-SCSI CDB,
	// although the actual passthru only uses up to the
	// first 12.
	
	ScsiPassThruCmnd->cmd_len = 16; // sizeof FCP-SCSI CDB

	// Unfortunately, the SCSI command cmnd[] field has only
	// 12 bytes.  Ideally the MAX_COMMAND_SIZE should be increased
	// to 16 for newer Fibre Channel and SCSI-3 larger CDBs.
	// However, to avoid a mandatory kernel rebuild, we use the SCp
	// spare field to store the extra 4 bytes ( ugly :-(

	if( MAX_COMMAND_SIZE < 16)
	{
          memcpy( &ScsiPassThruCmnd->SCp.buffers_residual,
		  &vendor_cmd->cdb[12], 4);
	}	  
                  
	
        ScsiPassThruCmnd->SCp.sent_command = 1; // PASSTHRU!
	                                        // suppress LUN masking
	                                        // and VSA logic

	// Use spare fields to copy FCP-SCSI LUN address info...
        ScsiPassThruCmnd->SCp.phase = vendor_cmd->bus;
	ScsiPassThruCmnd->SCp.have_data_in = vendor_cmd->pdrive;



        // We copy the scheme used by scsi.c to submit commands
	// to our own HBA.  We do this in order to stall the
	// thread calling the IOCTL until it completes, and use
	// the same "_quecommand" function for synchronizing
	// FC Link events with our "worker thread".
	
        spin_lock_irqsave(&io_request_lock, flags);
        {
          DECLARE_MUTEX_LOCKED(sem);
          ScsiPassThruCmnd->request.sem = &sem;
          // eventually gets us to our own _quecommand routine
          scsi_do_cmd( ScsiPassThruCmnd, &vendor_cmd->cdb[0], 
	       buf, 
	       vendor_cmd->len, 
	       my_ioctl_done, 
	       10*HZ, 1);// timeout,retries
          spin_unlock_irqrestore(&io_request_lock, flags);
          // Other I/Os can now resume; we wait for our ioctl
	  // command to complete
	  down(&sem);
          spin_lock_irqsave(&io_request_lock, flags);
          ScsiPassThruCmnd->request.sem = NULL;
        }
	
        result = ScsiPassThruCmnd->result;

        // copy any sense data back to caller
        if( result != 0 )
	{
	  memcpy( vendor_cmd->sense_data, // see struct def - size=40
		  ScsiPassThruCmnd->sense_buffer, 
		  sizeof(ScsiPassThruCmnd->sense_buffer)); 
	}
        SDpnt = ScsiPassThruCmnd->device;
        scsi_release_command(ScsiPassThruCmnd); // "de-allocate"
        ScsiPassThruCmnd = NULL;

        if (!SDpnt->was_reset && SDpnt->scsi_request_fn)
          (*SDpnt->scsi_request_fn)();

        wake_up(&SDpnt->device_wait);
        spin_unlock_irqrestore(&io_request_lock, flags);

	// need to pass data back to user (space)?
	if( (vendor_cmd->rw_flag == VENDOR_READ_OPCODE) &&
	     vendor_cmd->len )
          copy_to_user_ret( vendor_cmd->bufp, buf, vendor_cmd->len, -EFAULT);

        if( buf) 
	  kfree( buf);
	
        return result;
      }
      
      case CPQFCTS_GETPCIINFO:
      {
	cpqfc_pci_info_struct pciinfo;
	
	if( !arg)
	  return -EINVAL;

         	
	
        pciinfo.bus = cpqfcHBAdata->PciDev->bus->number;
        pciinfo.dev_fn = cpqfcHBAdata->PciDev->devfn;  
	pciinfo.board_id = cpqfcHBAdata->PciDev->device |
			  (cpqfcHBAdata->PciDev->vendor <<16); 
	      
        copy_to_user_ret( arg, &pciinfo, 
			  sizeof(cpqfc_pci_info_struct), -EFAULT);
        return 0;
      }

      case CPQFCTS_GETDRIVVER:
      {
	DriverVer_type DriverVer = 
		CPQFCTS_DRIVER_VER( VER_MAJOR,VER_MINOR,VER_SUBMINOR);
	
	if( !arg)
	  return -EINVAL;

        copy_to_user_ret( arg, &DriverVer, 
			  sizeof(DriverVer), -EFAULT);
        return 0;
      }



      case SCSI_IOCTL_FC_TARGET_ADDRESS:
      result = 
        verify_area(VERIFY_WRITE, arg, sizeof(Scsi_FCTargAddress));
      if (result) 
	break;
 
      put_user(pLoggedInPort->port_id,
		&((Scsi_FCTargAddress *) arg)->host_port_id);
 
      for( i=3,j=0; i>=0; i--)   	// copy the LOGIN port's WWN
        put_user(pLoggedInPort->u.ucWWN[i], 
		&((Scsi_FCTargAddress *) arg)->host_wwn[j++]);
      for( i=7; i>3; i--)		// copy the LOGIN port's WWN
        put_user(pLoggedInPort->u.ucWWN[i], 
		&((Scsi_FCTargAddress *) arg)->host_wwn[j++]);
        break;
    default:
      result = -EINVAL;
      break;
    }
  }

  LEAVE("cpqfcTS_ioctl");
  return result;
}


/* "Release" the Host Bus Adapter...
   disable interrupts, stop the HBA, release the interrupt,
   and free all resources */

int cpqfcTS_release(struct Scsi_Host *HostAdapter)
{
  CPQFCHBA *cpqfcHBAdata = (CPQFCHBA *)HostAdapter->hostdata; 


  ENTER("cpqfcTS_release");
	
  DEBUG_PCI( printk(" cpqfcTS: delete timer...\n"));
  del_timer( &cpqfcHBAdata->cpqfcTStimer);  
    
  // disable the hardware...
  DEBUG_PCI( printk(" disable hardware, destroy queues, free mem\n"));
  cpqfcHBAdata->fcChip.ResetTachyon( cpqfcHBAdata, CLEAR_FCPORTS);

  // kill kernel thread
  if( cpqfcHBAdata->worker_thread ) // (only if exists)
  {
    DECLARE_MUTEX_LOCKED(sem);  // synchronize thread kill

    cpqfcHBAdata->notify_wt = &sem;
    DEBUG_PCI( printk(" killing kernel thread\n"));
    send_sig( SIGKILL, cpqfcHBAdata->worker_thread, 1);
    down( &sem);
    cpqfcHBAdata->notify_wt = NULL;
    
  }

  // free Linux resources
  DEBUG_PCI( printk(" cpqfcTS: freeing resources...\n"));
  free_irq( HostAdapter->irq, HostAdapter);
  scsi_unregister( HostAdapter);
  release_region( cpqfcHBAdata->fcChip.Registers.IOBaseL, 0xff);
  release_region( cpqfcHBAdata->fcChip.Registers.IOBaseU, 0xff);
 /* we get "vfree: bad address" executing this - need to investigate... 
  if( (void*)((unsigned long)cpqfcHBAdata->fcChip.Registers.MemBase) !=
      cpqfcHBAdata->fcChip.Registers.ReMapMemBase)
    vfree( cpqfcHBAdata->fcChip.Registers.ReMapMemBase);
*/

  LEAVE("cpqfcTS_release");
  return 0;
}


const char * cpqfcTS_info(struct Scsi_Host *HostAdapter)
{
  static char buf[300];
  CPQFCHBA *cpqfcHBA;
  int BusSpeed, BusWidth;
  
  // get the pointer to our Scsi layer HBA buffer  
  cpqfcHBA = (CPQFCHBA *)HostAdapter->hostdata;

  BusWidth = (cpqfcHBA->fcChip.Registers.PCIMCTR &0x4) > 0 ?
               64 : 32;

  if( cpqfcHBA->fcChip.Registers.TYconfig.value & 0x80000000)
    BusSpeed = 66;
  else
    BusSpeed = 33;

  sprintf(buf, 
"%s: WWN %08X%08X\n on PCI bus %d device 0x%02x irq %d IObaseL 0x%x, MEMBASE 0x%x\nPCI bus width %d bits, bus speed %d MHz\nFCP-SCSI Driver v%d.%d.%d",
      cpqfcHBA->fcChip.Name, 
      cpqfcHBA->fcChip.Registers.wwn_hi,
      cpqfcHBA->fcChip.Registers.wwn_lo,
      cpqfcHBA->PciDev->bus->number,
      cpqfcHBA->PciDev->device,  
      HostAdapter->irq,
      cpqfcHBA->fcChip.Registers.IOBaseL,
      cpqfcHBA->fcChip.Registers.MemBase,
      BusWidth,
      BusSpeed,
      VER_MAJOR, VER_MINOR, VER_SUBMINOR
);

  
  cpqfcTSDecodeGBICtype( &cpqfcHBA->fcChip, &buf[ strlen(buf)]);
  cpqfcTSGetLPSM( &cpqfcHBA->fcChip, &buf[ strlen(buf)]);
  return buf;
}

//
// /proc/scsi support. The following routines allow us to do 'normal'
// sprintf like calls to return the currently requested piece (buflenght
// chars, starting at bufoffset) of the file. Although procfs allows for
// a 1 Kb bytes overflow after te supplied buffer, I consider it bad 
// programming to use it to make programming a little simpler. This piece
// of coding is borrowed from ncr53c8xx.c with some modifications 
//
struct info_str
{
        char *buffer;			// Pointer to output buffer
        int buflength;			// It's length
        int bufoffset;			// File offset corresponding with buf[0]
	int buffillen;			// Current filled length 
        int filpos;			// Current file offset
};

static void copy_mem_info(struct info_str *info, char *data, int datalen)
{

  if (info->filpos < info->bufoffset) {	// Current offset before buffer offset
    if (info->filpos + datalen <= info->bufoffset) {
      info->filpos += datalen; 		// Discard if completely before buffer
      return;
    } else {				// Partial copy, set to begin
      data += (info->bufoffset - info->filpos);
      datalen  -= (info->bufoffset - info->filpos);
      info->filpos = info->bufoffset;
    }
  }

  info->filpos += datalen;		// Update current offset

  if (info->buffillen == info->buflength) // Buffer full, discard
    return;

  if (info->buflength - info->buffillen < datalen)  // Overflows buffer ?
    datalen = info->buflength - info->buffillen;

  memcpy(info->buffer + info->buffillen, data, datalen);
  info->buffillen += datalen;
}

static int copy_info(struct info_str *info, char *fmt, ...)
{
        va_list args;
        char buf[400];
        int len;

        va_start(args, fmt);
        len = vsprintf(buf, fmt, args);
        va_end(args);

        copy_mem_info(info, buf, len);
        return len;
}


// Routine to get data for /proc RAM filesystem
//
int cpqfcTS_proc_info (char *buffer, char **start, off_t offset, int length, 
		       int hostno, int inout)
{
  struct Scsi_Host *host;
  Scsi_Cmnd DumCmnd;
  int Chan, Targ, i;
  struct info_str info;
  CPQFCHBA *cpqfcHBA;
  PTACHYON fcChip;
  PFC_LOGGEDIN_PORT pLoggedInPort;
  char buf[81];

  // Search the Scsi host list for our controller
  for (host=scsi_hostlist; host; host=host->next)
    if (host->host_no == hostno)
      break;

  if (!host) return -ESRCH;

  if (inout) return -EINVAL;

  // get the pointer to our Scsi layer HBA buffer  
  cpqfcHBA = (CPQFCHBA *)host->hostdata;
  fcChip = &cpqfcHBA->fcChip;
  
  *start 	  = buffer;

  info.buffer     = buffer;
  info.buflength  = length;
  info.bufoffset  = offset;
  info.filpos     = 0;
  info.buffillen  = 0;
  copy_info(&info, "Driver version = %d.%d.%d", VER_MAJOR, VER_MINOR, VER_SUBMINOR); 
  cpqfcTSDecodeGBICtype( &cpqfcHBA->fcChip, &buf[0]);
  cpqfcTSGetLPSM( &cpqfcHBA->fcChip, &buf[ strlen(buf)]);
  copy_info(&info, "%s\n", buf); 
		  

#define DISPLAY_WWN_INFO
#ifdef DISPLAY_WWN_INFO
  copy_info(&info, "WWN database: (\"port_id: 000000\" means disconnected)\n");
  for ( Chan=0; Chan <= host->max_channel; Chan++) {
    DumCmnd.channel = Chan;
    for (Targ=0; Targ <= host->max_id; Targ++) {
      DumCmnd.target = Targ;
      if ((pLoggedInPort = fcFindLoggedInPort( fcChip,
	    			&DumCmnd, // search Scsi Nexus
    				0,        // DON'T search list for FC port id
    				NULL,     // DON'T search list for FC WWN
    				NULL))){   // DON'T care about end of list
	copy_info(&info, "Host: scsi%d Channel: %02d TargetId: %02d -> WWN: ",
			   hostno, Chan, Targ);
        for( i=3; i>=0; i--)        // copy the LOGIN port's WWN
          copy_info(&info, "%02X", pLoggedInPort->u.ucWWN[i]);
        for( i=7; i>3; i--)             // copy the LOGIN port's WWN
          copy_info(&info, "%02X", pLoggedInPort->u.ucWWN[i]);
	copy_info(&info, " port_id: %06X\n", pLoggedInPort->port_id); 
      }
    }
  }
#endif
  
  
// Unfortunately, the proc_info buffer isn't big enough
// for everything we would like...
// For FC stats, compile this and turn off WWN stuff above  
//#define DISPLAY_FC_STATS
#ifdef DISPLAY_FC_STATS
// get the Fibre Channel statistics
  {
    int DeltaSecs = (jiffies - cpqfcHBA->fcStatsTime) / HZ;
    int days,hours,minutes,secs;
    
    days = DeltaSecs / (3600*24); // days
    hours = (DeltaSecs% (3600*24)) / 3600; // hours
    minutes = (DeltaSecs%3600 /60); // minutes
    secs =  DeltaSecs%60;  // secs
copy_info( &info, "Fibre Channel Stats (time dd:hh:mm:ss %02u:%02u:%02u:%02u\n",
      days, hours, minutes, secs);
  }
    
  cpqfcHBA->fcStatsTime = jiffies;  // (for next delta)

  copy_info( &info, "  LinkUp           %9u     LinkDown      %u\n",
        fcChip->fcStats.linkUp, fcChip->fcStats.linkDown);
        
  copy_info( &info, "  Loss of Signal   %9u     Loss of Sync  %u\n",
    fcChip->fcStats.LossofSignal, fcChip->fcStats.LossofSync);
		  
  copy_info( &info, "  Discarded Frames %9u     Bad CRC Frame %u\n",
    fcChip->fcStats.Dis_Frm, fcChip->fcStats.Bad_CRC);

  copy_info( &info, "  TACH LinkFailTX  %9u     TACH LinkFailRX     %u\n",
    fcChip->fcStats.linkFailTX, fcChip->fcStats.linkFailRX);
  
  copy_info( &info, "  TACH RxEOFa      %9u     TACH Elastic Store  %u\n",
    fcChip->fcStats.Rx_EOFa, fcChip->fcStats.e_stores);

  copy_info( &info, "  BufferCreditWait %9uus   TACH FM Inits %u\n",
    fcChip->fcStats.BB0_Timer*10, fcChip->fcStats.FMinits );
	
  copy_info( &info, "  FC-2 Timeouts    %9u     FC-2 Logouts  %u\n",
    fcChip->fcStats.timeouts, fcChip->fcStats.logouts); 
        
  copy_info( &info, "  FC-2 Aborts      %9u     FC-4 Aborts   %u\n",
    fcChip->fcStats.FC2aborted, fcChip->fcStats.FC4aborted);
   
  // clear the counters
  cpqfcTSClearLinkStatusCounters( fcChip);
#endif
	
  return info.buffillen;
}


#if DEBUG_CMND

UCHAR *ScsiToAscii( UCHAR ScsiCommand)
{

/*++

Routine Description:

   Converts a SCSI command to a text string for debugging purposes.


Arguments:

   ScsiCommand -- hex value SCSI Command


Return Value:

   An ASCII, null-terminated string if found, else returns NULL.

Original code from M. McGowen, Compaq
--*/


   switch (ScsiCommand)
   {
      case 0x00:
         return( "Test Unit Ready" );

      case 0x01:
         return( "Rezero Unit or Rewind" );

      case 0x02:
         return( "Request Block Address" );

      case 0x03:
         return( "Requese Sense" );

      case 0x04:
         return( "Format Unit" );

      case 0x05:
         return( "Read Block Limits" );

      case 0x07:
         return( "Reassign Blocks" );

      case 0x08:
         return( "Read (6)" );

      case 0x0a:
         return( "Write (6)" );

      case 0x0b:
         return( "Seek (6)" );

      case 0x12:
         return( "Inquiry" );

      case 0x15:
         return( "Mode Select (6)" );

      case 0x16:
         return( "Reserve" );

      case 0x17:
         return( "Release" );

      case 0x1a:
         return( "ModeSen(6)" );

      case 0x1b:
         return( "Start/Stop Unit" );

      case 0x1c:
         return( "Receive Diagnostic Results" );

      case 0x1d:
         return( "Send Diagnostic" );

      case 0x25:
         return( "Read Capacity" );

      case 0x28:
         return( "Read (10)" );

      case 0x2a:
         return( "Write (10)" );

      case 0x2b:
         return( "Seek (10)" );

      case 0x2e:
         return( "Write and Verify" );

      case 0x2f:
         return( "Verify" );

      case 0x34:
         return( "Pre-Fetch" );

      case 0x35:
         return( "Synchronize Cache" );

      case 0x37:
         return( "Read Defect Data (10)" );

      case 0x3b:
         return( "Write Buffer" );

      case 0x3c:
         return( "Read Buffer" );

      case 0x3e:
         return( "Read Long" );

      case 0x3f:
         return( "Write Long" );

      case 0x41:
         return( "Write Same" );

      case 0x4c:
         return( "Log Select" );

      case 0x4d:
         return( "Log Sense" );

      case 0x56:
         return( "Reserve (10)" );

      case 0x57:
         return( "Release (10)" );

      case 0xa0:
         return( "ReportLuns" );

      case 0xb7:
         return( "Read Defect Data (12)" );

      case 0xca:
         return( "Peripheral Device Addressing SCSI Passthrough" );

      case 0xcb:
         return( "Compaq Array Firmware Passthrough" );

      default:
         return( NULL );
   }

} // end ScsiToAscii()

void cpqfcTS_print_scsi_cmd(Scsi_Cmnd * cmd)
{

printk("cpqfcTS: (%s) chnl 0x%02x, trgt = 0x%02x, lun = 0x%02x, cmd_len = 0x%02x\n", 
    ScsiToAscii( cmd->cmnd[0]), cmd->channel, cmd->target, cmd->lun, cmd->cmd_len);

if( cmd->cmnd[0] == 0)   // Test Unit Ready?
{
  int i;

  printk("Cmnd->request_bufflen = 0x%X, ->use_sg = %d, ->bufflen = %d\n",
    cmd->request_bufflen, cmd->use_sg, cmd->bufflen);
  printk("Cmnd->request_buffer = %p, ->sglist_len = %d, ->buffer = %p\n",
    cmd->request_buffer, cmd->sglist_len, cmd->buffer);
  for (i = 0; i < cmd->cmd_len; i++)
    printk("0x%02x ", cmd->cmnd[i]);
  printk("\n");
}

}

#endif				/* DEBUG_CMND */




static void QueCmndOnBoardLock( CPQFCHBA *cpqfcHBAdata, Scsi_Cmnd *Cmnd)
{
  int i;

  for( i=0; i< CPQFCTS_REQ_QUEUE_LEN; i++)
  {    // find spare slot
    if( cpqfcHBAdata->BoardLockCmnd[i] == NULL )
    {
      cpqfcHBAdata->BoardLockCmnd[i] = Cmnd;
//      printk(" BoardLockCmnd[%d] %p Queued, chnl/target/lun %d/%d/%d\n",
//        i,Cmnd, Cmnd->channel, Cmnd->target, Cmnd->lun);
      break;
    }
  }
  if( i >= CPQFCTS_REQ_QUEUE_LEN)
  {
    printk(" cpqfcTS WARNING: Lost Cmnd %p on BoardLock Q full!", Cmnd);
  }

}


static void QueLinkDownCmnd( CPQFCHBA *cpqfcHBAdata, Scsi_Cmnd *Cmnd)
{
  int indx;

  // Remember the command ptr so we can return; we'll complete when
  // the device comes back, causing immediate retry
  for( indx=0; indx < CPQFCTS_REQ_QUEUE_LEN; indx++)//, SCptr++)
  {
    if( cpqfcHBAdata->LinkDnCmnd[indx] == NULL ) // available?
    {
#ifdef DUMMYCMND_DBG
      printk(" @add Cmnd %p to LnkDnCmnd[%d]@ ", Cmnd,indx);
#endif
      cpqfcHBAdata->LinkDnCmnd[indx] = Cmnd;
      break;
    }
  }

  if( indx >= CPQFCTS_REQ_QUEUE_LEN ) // no space for Cmnd??
  {
    // this will result in an _abort call later (with possible trouble)
    printk("no buffer for LinkDnCmnd!! %p\n", Cmnd);
  }
}





// The file "hosts.h" says not to call scsi_done from
// inside _queuecommand, so we'll do it from the heartbeat timer

static void QueBadTargetCmnd( CPQFCHBA *cpqfcHBAdata, Scsi_Cmnd *Cmnd)
{
  int i;
    //    printk(" can't find target %d\n", Cmnd->target);

  for( i=0; i< CPQFCTS_MAX_TARGET_ID; i++)
  {    // find spare slot
    if( cpqfcHBAdata->BadTargetCmnd[i] == NULL )
    {
      cpqfcHBAdata->BadTargetCmnd[i] = Cmnd;
//      printk(" BadTargetCmnd[%d] %p Queued, chnl/target/lun %d/%d/%d\n",
//          i,Cmnd, Cmnd->channel, Cmnd->target, Cmnd->lun);
      break;
    }
  }
}


// This is the "main" entry point for Linux Scsi commands --
// it all starts here.

int cpqfcTS_queuecommand(Scsi_Cmnd *Cmnd, void (* done)(Scsi_Cmnd *))
{
  struct Scsi_Host *HostAdapter = Cmnd->host;
  CPQFCHBA *cpqfcHBAdata = (CPQFCHBA *)HostAdapter->hostdata;
  PTACHYON fcChip = &cpqfcHBAdata->fcChip;
  TachFCHDR_GCMND fchs;  // only use for FC destination id field  
  PFC_LOGGEDIN_PORT pLoggedInPort;
  ULONG ulStatus, SESTtype;
  LONG ExchangeID;




  ENTER("cpqfcTS_queuecommand");
      
  PCI_TRACEO( (ULONG)Cmnd, 0x98)
      
  
  Cmnd->scsi_done = done;
#ifdef DEBUG_CMND  
  cpqfcTS_print_scsi_cmd( Cmnd);
#endif

  // prevent board contention with kernel thread...  
  
   if( cpqfcHBAdata->BoardLock )
  {
//    printk(" @BrdLck Hld@ ");
    QueCmndOnBoardLock( cpqfcHBAdata, Cmnd);
  }
  
  else
  {

    // in the current system (2.2.12), this routine is called
    // after spin_lock_irqsave(), so INTs are disabled. However,
    // we might have something pending in the LinkQ, which
    // might cause the WorkerTask to run.  In case that
    // happens, make sure we lock it out.
    
    
    
    PCI_TRACE( 0x98) 
    CPQ_SPINLOCK_HBA( cpqfcHBAdata)
    PCI_TRACE( 0x98) 
	    
  // can we find an FC device mapping to this SCSI target?
    pLoggedInPort = fcFindLoggedInPort( fcChip,
      Cmnd,     // search Scsi Nexus
      0,        // DON'T search linked list for FC port id
      NULL,     // DON'T search linked list for FC WWN
      NULL);    // DON'T care about end of list
 
    if( pLoggedInPort == NULL )      // not found!
    {
//    printk(" @Q bad targ cmnd %p@ ", Cmnd);
      QueBadTargetCmnd( cpqfcHBAdata, Cmnd);
    }

    else  // we know what FC device to send to...
    {

      // does this device support FCP target functions?
      // (determined by PRLI field)

      if( !(pLoggedInPort->fcp_info & TARGET_FUNCTION) )
      {
        printk(" Doesn't support TARGET functions port_id %Xh\n",
          pLoggedInPort->port_id );
        QueBadTargetCmnd( cpqfcHBAdata, Cmnd);
      }

    // In this case (previous login OK), the device is temporarily
    // unavailable waiting for re-login, in which case we expect it
    // to be back in between 25 - 500ms.  
    // If the FC port doesn't log back in within several seconds
    // (i.e. implicit "logout"), or we get an explicit logout,
    // we set "device_blocked" in Scsi_Device struct; in this
    // case 30 seconds will elapse before Linux/Scsi sends another
    // command to the device.
      else if( pLoggedInPort->prli != TRUE )
      {
//      printk("Device (Chnl/Target %d/%d) invalid PRLI, port_id %06lXh\n",
//        Cmnd->channel, Cmnd->target, pLoggedInPort->port_id);
        QueLinkDownCmnd( cpqfcHBAdata, Cmnd);
//    Need to use "blocked" flag??   	
//	Cmnd->device->device_blocked = TRUE; // just let it timeout
      }
      else  // device supports TARGET functions, and is logged in...
      {
      // (context of fchs is to "reply" to...)
        fchs.s_id = pLoggedInPort->port_id; // destination FC address

      // what is the data direction?  For data TO the device,
      // we need IWE (Intiator Write Entry).  Otherwise, IRE.

        if( Cmnd->cmnd[0] == WRITE_10 ||
  	  Cmnd->cmnd[0] == WRITE_6 ||
	  Cmnd->cmnd[0] == WRITE_BUFFER ||      
	  Cmnd->cmnd[0] == VENDOR_WRITE_OPCODE ||  // CPQ specific 
	  Cmnd->cmnd[0] == MODE_SELECT )
        {
          SESTtype = SCSI_IWE; // data from HBA to Device
        }
        else
          SESTtype = SCSI_IRE; // data from Device to HBA
    	  
        ulStatus = cpqfcTSBuildExchange(
          cpqfcHBAdata,
          SESTtype,     // e.g. Initiator Read Entry (IRE)
          &fchs,        // we are originator; only use d_id
          Cmnd,         // Linux SCSI command (with scatter/gather list)
          &ExchangeID );// fcController->fcExchanges index, -1 if failed

        if( !ulStatus ) // Exchange setup?
   
        {
          if( cpqfcHBAdata->BoardLock )
          {
    TriggerHBA( fcChip->Registers.ReMapMemBase, 0);
	    printk(" @bl! %d, xID %Xh@ ", current->pid, ExchangeID);
          }

	  ulStatus = cpqfcTSStartExchange( cpqfcHBAdata, ExchangeID );
	  if( !ulStatus )
          {
            PCI_TRACEO( ExchangeID, 0xB8) 
          // submitted to Tach's Outbound Que (ERQ PI incremented)
          // waited for completion for ELS type (Login frames issued
          // synchronously)
	  }
          else
            // check reason for Exchange not being started - we might
            // want to Queue and start later, or fail with error
          {
            printk("quecommand: cpqfcTSStartExchange failed: %Xh\n", ulStatus );
          }
        }            // end good BuildExchange status
        
        else  // SEST table probably full  -- why? hardware hang?
        {
	  printk("quecommand: cpqfcTSBuildExchange faild: %Xh\n", ulStatus);
        }
      }  // end can't do FCP-SCSI target functions
    } // end can't find target (FC device)

    CPQ_SPINUNLOCK_HBA( cpqfcHBAdata)
  }
	
  PCI_TRACEO( (ULONG)Cmnd, 0x9C) 
  LEAVE("cpqfcTS_queuecommand");
  return 0;
}    


// Entry point for upper Scsi layer intiated abort.  Typically
// this is called if the command (for hard disk) fails to complete
// in 30 seconds.  This driver intends to complete all disk commands
// within Exchange ".timeOut" seconds (now 7) with target status, or
// in case of ".timeOut" expiration, a DID_SOFT_ERROR which causes
// immediate retry.
// If any disk commands get the _abort call, except for the case that
// the physical device was removed or unavailable due to hardware
// errors, it should be considered a driver error and reported to
// the author.

int cpqfcTS_abort(Scsi_Cmnd *Cmnd)
{
  struct Scsi_Host *HostAdapter = Cmnd->host;
  // get the pointer to our Scsi layer HBA buffer  
  CPQFCHBA *cpqfcHBAdata = (CPQFCHBA *)HostAdapter->hostdata;
  PTACHYON fcChip = &cpqfcHBAdata->fcChip;
  FC_EXCHANGES *Exchanges = fcChip->Exchanges;
  int i;
  ENTER("cpqfcTS_abort");

  Cmnd->result = DID_ABORT <<16;  // assume we'll find it

  printk(" @Linux _abort Scsi_Cmnd %p ", Cmnd);
  // See if we can find a Cmnd pointer that matches...
  // The most likely case is we accepted the command
  // from Linux Scsi (e.g. ceated a SEST entry) and it
  // got lost somehow.  If we can't find any reference
  // to the passed pointer, we can only presume it
  // got completed as far as our driver is concerned.
  // If we found it, we will try to abort it through
  // common mechanism.  If FC ABTS is successful (ACC)
  // or is rejected (RJT) by target, we will call
  // Scsi "done" quickly.  Otherwise, the ABTS will timeout
  // and we'll call "done" later.

  // Search the SEST exchanges for a matching Cmnd ptr.
  for( i=0; i< TACH_SEST_LEN; i++)
  {
    if( Exchanges->fcExchange[i].Cmnd == Cmnd )
    {
      
      // found it!
      printk(" x_ID %Xh, type %Xh\n", i, Exchanges->fcExchange[i].type);

      Exchanges->fcExchange[i].status = INITIATOR_ABORT; // seconds default
      Exchanges->fcExchange[i].timeOut = 10; // seconds default (changed later)

      // Since we need to immediately return the aborted Cmnd to Scsi 
      // upper layers, we can't make future reference to any of it's 
      // fields (e.g the Nexus).

      cpqfcTSPutLinkQue( cpqfcHBAdata, BLS_ABTS, &i);

      break;
    }
  }

  if( i >= TACH_SEST_LEN ) // didn't find Cmnd ptr in chip's SEST?
  {
    // now search our non-SEST buffers (i.e. Cmnd waiting to
    // start on the HBA or waiting to complete with error for retry).
    
    // first check BadTargetCmnd
    for( i=0; i< CPQFCTS_MAX_TARGET_ID; i++)
    { 
      if( cpqfcHBAdata->BadTargetCmnd[i] == Cmnd )
      {
        cpqfcHBAdata->BadTargetCmnd[i] = NULL;
	printk("in BadTargetCmnd Q\n");
	goto Done; // exit
      }
    }

    // if not found above...

    for( i=0; i < CPQFCTS_REQ_QUEUE_LEN; i++)
    {
      if( cpqfcHBAdata->LinkDnCmnd[i] == Cmnd ) 
      {
	cpqfcHBAdata->LinkDnCmnd[i] = NULL;
	printk("in LinkDnCmnd Q\n");
	goto Done;
      }
    }


    for( i=0; i< CPQFCTS_REQ_QUEUE_LEN; i++)
    {    // find spare slot
      if( cpqfcHBAdata->BoardLockCmnd[i] == Cmnd )
      {
        cpqfcHBAdata->BoardLockCmnd[i] = NULL;
	printk("in BoardLockCmnd Q\n");
	goto Done;
      }
    }
    
    Cmnd->result = DID_ERROR <<16;  // Hmmm...
    printk("Not found! ");
//    panic("_abort");
  }
  
Done:
  
//    panic("_abort");
  LEAVE("cpqfcTS_abort");
  return 0;  // (see scsi.h)
}    




// To be done...	
int cpqfcTS_reset(Scsi_Cmnd *Cmnd, unsigned int reset_flags)
{
  int return_status = SUCCESS;

  ENTER("cpqfcTS_reset");


            

  LEAVE("cpqfcTS_reset");
  return return_status;
}    



/* This function determines the bios parameters for a given
   harddisk. These tend to be numbers that are made up by the
   host adapter.  Parameters:
   size, device number, list (heads, sectors,cylinders).
   (from hosts.h)
*/

int cpqfcTS_biosparam(Disk *disk, kdev_t n, int ip[])
{
  int size = disk->capacity;
  
  ENTER("cpqfcTS_biosparam");
  ip[0] = 64;
  ip[1] = 32;
  ip[2] = size >> 11;
  
  if( ip[2] > 1024 )
  {
    ip[0] = 255;
    ip[1] = 63;
    ip[2] = size / (ip[0] * ip[1]);
  }

  LEAVE("cpqfcTS_biosparam");
  return 0;
}    



void cpqfcTS_intr_handler( int irq, 
		void *dev_id, 
		struct pt_regs *regs)
{

  unsigned long flags, InfLoopBrk=0;
  struct Scsi_Host *HostAdapter = dev_id;
  CPQFCHBA *cpqfcHBA = (CPQFCHBA *)HostAdapter->hostdata;
  int MoreMessages = 1; // assume we have something to do
  UCHAR IntPending;
  
  ENTER("intr_handler");

  spin_lock_irqsave( &io_request_lock, flags);
  // is this our INT?
  IntPending = readb( cpqfcHBA->fcChip.Registers.INTPEND.address);

  // broken boards can generate messages forever, so
  // prevent the infinite loop
#define INFINITE_IMQ_BREAK 10000
  if( IntPending )
  {
    
    // mask our HBA interrupts until we handle it...
    writeb( 0, cpqfcHBA->fcChip.Registers.INTEN.address);

    if( IntPending & 0x4) // "INT" - Tach wrote to IMQ
    {
      while( (++InfLoopBrk < INFINITE_IMQ_BREAK) && (MoreMessages ==1) ) 
      {
        MoreMessages = CpqTsProcessIMQEntry( HostAdapter); // ret 0 when done
      }
      if( InfLoopBrk >= INFINITE_IMQ_BREAK )
      {
        printk("WARNING: Compaq FC adapter generating excessive INTs -REPLACE\n");
        printk("or investigate alternate causes (e.g. physical FC layer)\n");
      }

      else  // working normally - re-enable INTs and continue
        writeb( 0x1F, cpqfcHBA->fcChip.Registers.INTEN.address);
    
    }  // (...ProcessIMQEntry() clears INT by writing IMQ consumer)
    else  // indications of errors or problems...
          // these usually indicate critical system hardware problems.
    {
      if( IntPending & 0x10 )
  	printk(" cpqfcTS adapter external memory parity error detected\n");
      if( IntPending & 0x8 )
  	printk(" cpqfcTS adapter PCI master address crossed 45-bit boundary\n");
      if( IntPending & 0x2 )
	printk(" cpqfcTS adapter DMA error detected\n");
      if( IntPending & 0x1 )
  	printk(" cpqfcTS adapter PCI error detected\n");
    }      
  }
  spin_unlock_irqrestore( &io_request_lock, flags);
  LEAVE("intr_handler");
}




int cpqfcTSDecodeGBICtype( PTACHYON fcChip, char cErrorString[])
{
        // Verify GBIC type (if any) and correct Tachyon Port State Machine
        // (GBIC) module definition is:
        // GPIO1, GPIO0, GPIO4 for MD2, MD1, MD0.  The input states appear
        // to be inverted -- i.e., a setting of 111 is read when there is NO
        // GBIC present.  The Module Def (MD) spec says 000 is "no GBIC"
        // Hard code the bit states to detect Copper, 
        // Long wave (single mode), Short wave (multi-mode), and absent GBIC

  ULONG ulBuff;

  sprintf( cErrorString, "\nGBIC detected: ");

  ulBuff = fcChip->Registers.TYstatus.value & 0x13; 
  switch( ulBuff )
  {
  case 0x13:  // GPIO4, GPIO1, GPIO0 = 111; no GBIC!
    sprintf( &cErrorString[ strlen( cErrorString)],
            "NONE! ");
    return FALSE;          
          
       
  case 0x11:   // Copper GBIC detected
    sprintf( &cErrorString[ strlen( cErrorString)],
            "Copper. ");
    break;

  case 0x10:   // Long-wave (single mode) GBIC detected
    sprintf( &cErrorString[ strlen( cErrorString)],
        "Long-wave. ");
    break;
  case 0x1:    // Short-wave (multi mode) GBIC detected
    sprintf( &cErrorString[ strlen( cErrorString)],
        "Short-wave. ");
    break;
  default:     // unknown GBIC - presumably it will work (?)
    sprintf( &cErrorString[ strlen( cErrorString)],
            "Unknown. ");
          
    break;
  }  // end switch GBIC detection

  return TRUE;
}






int cpqfcTSGetLPSM( PTACHYON fcChip, char cErrorString[])
{
  // Tachyon's Frame Manager LPSM in LinkDown state?
  // (For non-loop port, check PSM instead.)
  // return string with state and FALSE is Link Down

  int LinkUp;

  if( fcChip->Registers.FMstatus.value & 0x80 ) 
    LinkUp = FALSE;
  else
    LinkUp = TRUE;

  sprintf( &cErrorString[ strlen( cErrorString)],
    " LPSM %Xh ", 
     (fcChip->Registers.FMstatus.value >>4) & 0xf );


  switch( fcChip->Registers.FMstatus.value & 0xF0)
  {
                    // bits set in LPSM
    case 0x10:
      sprintf( &cErrorString[ strlen( cErrorString)], "ARB");
      break;
    case 0x20:
      sprintf( &cErrorString[ strlen( cErrorString)], "ARBwon");
      break;
    case 0x30:
      sprintf( &cErrorString[ strlen( cErrorString)], "OPEN");
      break;
    case 0x40:
      sprintf( &cErrorString[ strlen( cErrorString)], "OPENed");
      break;
    case 0x50:
      sprintf( &cErrorString[ strlen( cErrorString)], "XmitCLS");
      break;
    case 0x60:
      sprintf( &cErrorString[ strlen( cErrorString)], "RxCLS");
      break;
    case 0x70:
      sprintf( &cErrorString[ strlen( cErrorString)], "Xfer");
      break;
    case 0x80:
      sprintf( &cErrorString[ strlen( cErrorString)], "Init");
      break;
    case 0x90:
      sprintf( &cErrorString[ strlen( cErrorString)], "O-IInitFin");
      break;
    case 0xa0:
      sprintf( &cErrorString[ strlen( cErrorString)], "O-IProtocol");
      break;
    case 0xb0:
      sprintf( &cErrorString[ strlen( cErrorString)], "O-ILipRcvd");
      break;
    case 0xc0:
      sprintf( &cErrorString[ strlen( cErrorString)], "HostControl");
      break;
    case 0xd0:
      sprintf( &cErrorString[ strlen( cErrorString)], "LoopFail");
      break;
    case 0xe0:
      sprintf( &cErrorString[ strlen( cErrorString)], "Offline");
      break;
    case 0xf0:
      sprintf( &cErrorString[ strlen( cErrorString)], "OldPort");
      break;
    case 0:
    default:
      sprintf( &cErrorString[ strlen( cErrorString)], "Monitor");
      break;

  }

  return LinkUp;
}




#include "linux/malloc.h"

// Dynamic memory allocation alignment routines
// HP's Tachyon Fibre Channel Controller chips require
// certain memory queues and register pointers to be aligned
// on various boundaries, usually the size of the Queue in question.
// Alignment might be on 2, 4, 8, ... or even 512 byte boundaries.
// Since most O/Ss don't allow this (usually only Cache aligned -
// 32-byte boundary), these routines provide generic alignment (after
// O/S allocation) at any boundary, and store the original allocated
// pointer for deletion (O/S free function).  Typically, we expect
// these functions to only be called at HBA initialization and
// removal time (load and unload times)
// ALGORITHM notes:
// Memory allocation varies by compiler and platform.  In the worst case,
// we are only assured BYTE allignment, but in the best case, we can
// request allocation on any desired boundary.  Our strategy: pad the
// allocation request size (i.e. waste memory) so that we are assured
// of passing desired boundary near beginning of contiguous space, then
// mask out lower address bits.
// We define the following algorithm:
//   allocBoundary - compiler/platform specific address alignment
//                   in number of bytes (default is single byte; i.e. 1)
//   n_alloc       - number of bytes application wants @ aligned address
//   ab            - alignment boundary, in bytes (e.g. 4, 32, ...)
//   t_alloc       - total allocation needed to ensure desired boundary
//   mask          - to clear least significant address bits for boundary
//   Compute:
//   t_alloc = n_alloc + (ab - allocBoundary)
//   allocate t_alloc bytes @ alloc_address
//   mask =  NOT (ab - 1)
//       (e.g. if ab=32  _0001 1111  -> _1110 0000
//   aligned_address = alloc_address & mask
//   set n_alloc bytes to 0
//   return aligned_address (NULL if failed)
//
// If u32_AlignedAddress is non-zero, then search for BaseAddress (stored
// from previous allocation).  If found, invoke call to FREE the memory.
// Return NULL if BaseAddress not found

// we need about 8 allocations per HBA.  Figuring at most 10 HBAs per server
// size the dynamic_mem array at 80.

void* fcMemManager( ALIGNED_MEM *dynamic_mem, ULONG n_alloc, ULONG ab,
                   ULONG u32_AlignedAddress)
{
  USHORT allocBoundary=1;   // compiler specific - worst case 1
                                  // best case - replace malloc() call
                                  // with function that allocates exactly
                                  // at desired boundary

  unsigned long ulAddress;
  ULONG t_alloc, i;
  void *alloc_address = 0;  // def. error code / address not found
  LONG mask;                // must be 32-bits wide!

  ENTER("fcMemManager");
  if( u32_AlignedAddress )          // are we freeing existing memory?
  {
//    printk(" freeing AlignedAddress %Xh\n", u32_AlignedAddress);
    for( i=0; i<DYNAMIC_ALLOCATIONS; i++) // look for the base address
    {
//    printk("dynamic_mem[%u].AlignedAddress %lX\n", i, dynamic_mem[i].AlignedAddress);
      if( dynamic_mem[i].AlignedAddress == u32_AlignedAddress )
      {
        alloc_address = dynamic_mem[i].BaseAllocated; // 'success' status
        kfree( dynamic_mem[i].BaseAllocated);  // return pages to kernel
        dynamic_mem[i].BaseAllocated = 0;   // clear for next use
        dynamic_mem[i].AlignedAddress = 0;
        break;                        // quit for loop; done
      }
    }
  }
  else if( n_alloc )                   // want new memory?
  {
    t_alloc = n_alloc + (ab - allocBoundary); // pad bytes for alignment
//    printk("kmalloc() for Tach alignment: %ld bytes\n", t_alloc);

    alloc_address =                  // total bytes (NumberOfBytes)
      kmalloc( t_alloc, GFP_KERNEL); // allow thread block to free pages 


                                  // now mask off least sig. bits of address
    if( alloc_address )           // (only if non-NULL)
    {
                                  // find place to store ptr, so we
                                  // can free it later...
      for( i=0; i<DYNAMIC_ALLOCATIONS; i++) // look for free slot
      {
        if( dynamic_mem[i].BaseAllocated == 0) // take 1st available
        {
          dynamic_mem[i].BaseAllocated = alloc_address;// address from O/S
          break;
        }
      }
      mask = (LONG)(ab - 1);            // mask all low-order bits
      mask = ~mask;                            // invert bits

      ulAddress = (unsigned long)alloc_address;
      
      ulAddress += (ab - allocBoundary);    // add the alignment bytes-
                                            // then truncate address...
      alloc_address = (void*)(ulAddress & mask);
      
      dynamic_mem[i].AlignedAddress = 
	(ULONG)(ulAddress & mask); // 32bit Tach address
      memset( alloc_address, 0, n_alloc );  // clear new memory
    }
    else  // O/S dynamic mem alloc failed!
      alloc_address = 0;  // (for debugging breakpt)

  }

  LEAVE("fcMemManager");
  return alloc_address;  // good (or NULL) address
}




#ifdef MODULE

Scsi_Host_Template driver_template = CPQFCTS;

#include "scsi_module.c"


#endif


