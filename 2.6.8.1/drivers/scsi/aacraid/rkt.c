/*
 *	Adaptec AAC series RAID controller driver
 *	(c) Copyright 2001 Red Hat Inc.	<alan@redhat.com>
 *
 * based on the old aacraid driver that is..
 * Adaptec aacraid device driver for Linux.
 *
 * Copyright (c) 2000 Adaptec, Inc. (aacraid@adaptec.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Module Name:
 *  rkt.c
 *
 * Abstract: Hardware miniport for Drawbridge specific hardware functions.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <asm/semaphore.h>

#include <scsi/scsi_host.h>

#include "aacraid.h"

static irqreturn_t aac_rkt_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct aac_dev *dev = dev_id;
	unsigned long bellbits;
	u8 intstat, mask;
	intstat = rkt_readb(dev, MUnit.OISR);
	/*
	 *	Read mask and invert because drawbridge is reversed.
	 *	This allows us to only service interrupts that have 
	 *	been enabled.
	 */
	mask = ~(dev->OIMR);
	/* Check to see if this is our interrupt.  If it isn't just return */
	if (intstat & mask) 
	{
		bellbits = rkt_readl(dev, OutboundDoorbellReg);
		if (bellbits & DoorBellPrintfReady) {
			aac_printf(dev, le32_to_cpu(rkt_readl (dev, IndexRegs.Mailbox[5])));
			rkt_writel(dev, MUnit.ODR,DoorBellPrintfReady);
			rkt_writel(dev, InboundDoorbellReg,DoorBellPrintfDone);
		}
		else if (bellbits & DoorBellAdapterNormCmdReady) {
			rkt_writel(dev, MUnit.ODR, DoorBellAdapterNormCmdReady);
			aac_command_normal(&dev->queues->queue[HostNormCmdQueue]);
		}
		else if (bellbits & DoorBellAdapterNormRespReady) {
			aac_response_normal(&dev->queues->queue[HostNormRespQueue]);
			rkt_writel(dev, MUnit.ODR,DoorBellAdapterNormRespReady);
		}
		else if (bellbits & DoorBellAdapterNormCmdNotFull) {
			rkt_writel(dev, MUnit.ODR, DoorBellAdapterNormCmdNotFull);
		}
		else if (bellbits & DoorBellAdapterNormRespNotFull) {
			rkt_writel(dev, MUnit.ODR, DoorBellAdapterNormCmdNotFull);
			rkt_writel(dev, MUnit.ODR, DoorBellAdapterNormRespNotFull);
		}
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

/**
 *	aac_rkt_enable_interrupt	-	Enable event reporting
 *	@dev: Adapter
 *	@event: Event to enable
 *
 *	Enable event reporting from the i960 for a given event.
 */
 
static void aac_rkt_enable_interrupt(struct aac_dev * dev, u32 event)
{
	switch (event) {

	case HostNormCmdQue:
		dev->irq_mask &= ~(OUTBOUNDDOORBELL_1);
		break;

	case HostNormRespQue:
		dev->irq_mask &= ~(OUTBOUNDDOORBELL_2);
		break;

	case AdapNormCmdNotFull:
		dev->irq_mask &= ~(OUTBOUNDDOORBELL_3);
		break;

	case AdapNormRespNotFull:
		dev->irq_mask &= ~(OUTBOUNDDOORBELL_4);
		break;
	}
}

/**
 *	aac_rkt_disable_interrupt	-	Disable event reporting
 *	@dev: Adapter
 *	@event: Event to enable
 *
 *	Disable event reporting from the i960 for a given event.
 */

static void aac_rkt_disable_interrupt(struct aac_dev *dev, u32 event)
{
	switch (event) {

	case HostNormCmdQue:
		dev->irq_mask |= (OUTBOUNDDOORBELL_1);
		break;

	case HostNormRespQue:
		dev->irq_mask |= (OUTBOUNDDOORBELL_2);
		break;

	case AdapNormCmdNotFull:
		dev->irq_mask |= (OUTBOUNDDOORBELL_3);
		break;

	case AdapNormRespNotFull:
		dev->irq_mask |= (OUTBOUNDDOORBELL_4);
		break;
	}
}

/**
 *	rkt_sync_cmd	-	send a command and wait
 *	@dev: Adapter
 *	@command: Command to execute
 *	@p1: first parameter
 *	@ret: adapter status
 *
 *	This routine will send a synchronous comamnd to the adapter and wait 
 *	for its	completion.
 */

static int rkt_sync_cmd(struct aac_dev *dev, u32 command, u32 p1, u32 *status)
{
	unsigned long start;
	int ok;
	/*
	 *	Write the command into Mailbox 0
	 */
	rkt_writel(dev, InboundMailbox0, cpu_to_le32(command));
	/*
	 *	Write the parameters into Mailboxes 1 - 4
	 */
	rkt_writel(dev, InboundMailbox1, cpu_to_le32(p1));
	rkt_writel(dev, InboundMailbox2, 0);
	rkt_writel(dev, InboundMailbox3, 0);
	rkt_writel(dev, InboundMailbox4, 0);
	/*
	 *	Clear the synch command doorbell to start on a clean slate.
	 */
	rkt_writel(dev, OutboundDoorbellReg, OUTBOUNDDOORBELL_0);
	/*
	 *	Disable doorbell interrupts
	 */
	rkt_writeb(dev, MUnit.OIMR, dev->OIMR |= 0x04);
	/*
	 *	Force the completion of the mask register write before issuing
	 *	the interrupt.
	 */
	rkt_readb (dev, MUnit.OIMR);
	/*
	 *	Signal that there is a new synch command
	 */
	rkt_writel(dev, InboundDoorbellReg, INBOUNDDOORBELL_0);

	ok = 0;
	start = jiffies;

	/*
	 *	Wait up to 30 seconds
	 */
	while (time_before(jiffies, start+30*HZ)) 
	{
		udelay(5);	/* Delay 5 microseconds to let Mon960 get info. */
		/*
		 *	Mon960 will set doorbell0 bit when it has completed the command.
		 */
		if (rkt_readl(dev, OutboundDoorbellReg) & OUTBOUNDDOORBELL_0) {
			/*
			 *	Clear the doorbell.
			 */
			rkt_writel(dev, OutboundDoorbellReg, OUTBOUNDDOORBELL_0);
			ok = 1;
			break;
		}
		/*
		 *	Yield the processor in case we are slow 
		 */
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
	}
	if (ok != 1) {
		/*
		 *	Restore interrupt mask even though we timed out
		 */
		rkt_writeb(dev, MUnit.OIMR, dev->OIMR &= 0xfb);
		return -ETIMEDOUT;
	}
	/*
	 *	Pull the synch status from Mailbox 0.
	 */
	*status = le32_to_cpu(rkt_readl(dev, IndexRegs.Mailbox[0]));
	/*
	 *	Clear the synch command doorbell.
	 */
	rkt_writel(dev, OutboundDoorbellReg, OUTBOUNDDOORBELL_0);
	/*
	 *	Restore interrupt mask
	 */
	rkt_writeb(dev, MUnit.OIMR, dev->OIMR &= 0xfb);
	return 0;

}

/**
 *	aac_rkt_interrupt_adapter	-	interrupt adapter
 *	@dev: Adapter
 *
 *	Send an interrupt to the i960 and breakpoint it.
 */

static void aac_rkt_interrupt_adapter(struct aac_dev *dev)
{
	u32 ret;
	rkt_sync_cmd(dev, BREAKPOINT_REQUEST, 0, &ret);
}

/**
 *	aac_rkt_notify_adapter		-	send an event to the adapter
 *	@dev: Adapter
 *	@event: Event to send
 *
 *	Notify the i960 that something it probably cares about has
 *	happened.
 */

static void aac_rkt_notify_adapter(struct aac_dev *dev, u32 event)
{
	switch (event) {

	case AdapNormCmdQue:
		rkt_writel(dev, MUnit.IDR,INBOUNDDOORBELL_1);
		break;
	case HostNormRespNotFull:
		rkt_writel(dev, MUnit.IDR,INBOUNDDOORBELL_4);
		break;
	case AdapNormRespQue:
		rkt_writel(dev, MUnit.IDR,INBOUNDDOORBELL_2);
		break;
	case HostNormCmdNotFull:
		rkt_writel(dev, MUnit.IDR,INBOUNDDOORBELL_3);
		break;
	case HostShutdown:
//		rkt_sync_cmd(dev, HOST_CRASHING, 0, 0, 0, 0, &ret);
		break;
	case FastIo:
		rkt_writel(dev, MUnit.IDR,INBOUNDDOORBELL_6);
		break;
	case AdapPrintfDone:
		rkt_writel(dev, MUnit.IDR,INBOUNDDOORBELL_5);
		break;
	default:
		BUG();
		break;
	}
}

/**
 *	aac_rkt_start_adapter		-	activate adapter
 *	@dev:	Adapter
 *
 *	Start up processing on an i960 based AAC adapter
 */

static void aac_rkt_start_adapter(struct aac_dev *dev)
{
	u32 status;
	struct aac_init *init;

	init = dev->init;
	init->HostElapsedSeconds = cpu_to_le32(get_seconds());
	/*
	 *	Tell the adapter we are back and up and running so it will scan
	 *	its command queues and enable our interrupts
	 */
	dev->irq_mask = (DoorBellPrintfReady | OUTBOUNDDOORBELL_1 | OUTBOUNDDOORBELL_2 | OUTBOUNDDOORBELL_3 | OUTBOUNDDOORBELL_4);
	/*
	 *	First clear out all interrupts.  Then enable the one's that we
	 *	can handle.
	 */
	rkt_writeb(dev, MUnit.OIMR, 0xff);
	rkt_writel(dev, MUnit.ODR, 0xffffffff);
//	rkt_writeb(dev, MUnit.OIMR, ~(u8)OUTBOUND_DOORBELL_INTERRUPT_MASK);
	rkt_writeb(dev, MUnit.OIMR, dev->OIMR = 0xfb);

	// We can only use a 32 bit address here
	rkt_sync_cmd(dev, INIT_STRUCT_BASE_ADDRESS, (u32)(ulong)dev->init_pa, &status);
}

/**
 *	aac_rkt_check_health
 *	@dev: device to check if healthy
 *
 *	Will attempt to determine if the specified adapter is alive and
 *	capable of handling requests, returning 0 if alive.
 */
static int aac_rkt_check_health(struct aac_dev *dev)
{
	long status = rkt_readl(dev, IndexRegs.Mailbox[7]);

	/*
	 *	Check to see if the board failed any self tests.
	 */
	if (status & SELF_TEST_FAILED)
		return -1;
	/*
	 *	Check to see if the board panic'd.
	 */
	if (status & KERNEL_PANIC)
	{
		char * buffer = kmalloc(512, GFP_KERNEL|__GFP_DMA);
		struct POSTSTATUS {
			u32 Post_Command;
			u32 Post_Address;
		} * post = kmalloc(sizeof(struct POSTSTATUS), GFP_KERNEL);
		dma_addr_t paddr = pci_map_single(dev->pdev, post, sizeof(struct POSTSTATUS), 2);
		dma_addr_t baddr = pci_map_single(dev->pdev, buffer, 512, 1);
		u32 status = -1;
		int ret = -2;
		
		memset(buffer, 0, 512);
		post->Post_Command = cpu_to_le32(COMMAND_POST_RESULTS);
		post->Post_Address = cpu_to_le32(baddr);
		rkt_writel(dev, MUnit.IMRx[0], cpu_to_le32(paddr));
		rkt_sync_cmd(dev, COMMAND_POST_RESULTS, baddr, &status);
		pci_unmap_single(dev->pdev, paddr, sizeof(struct POSTSTATUS),2);
		kfree(post);
		if ((buffer[0] == '0') && (buffer[1] == 'x')) {
			ret = (buffer[2] <= '9') ? (buffer[2] - '0') : (buffer[2] - 'A' + 10);
			ret <<= 4;
			ret += (buffer[3] <= '9') ? (buffer[3] - '0') : (buffer[3] - 'A' + 10);
		}
		pci_unmap_single(dev->pdev, baddr, 512, 1);
		kfree(buffer);
		return ret;
	}
	/*
	 *	Wait for the adapter to be up and running.
	 */
	if (!(status & KERNEL_UP_AND_RUNNING))
		return -3;
	/*
	 *	Everything is OK
	 */
	return 0;
}

/**
 *	aac_rkt_init	-	initialize an i960 based AAC card
 *	@dev: device to configure
 *
 *	Allocate and set up resources for the i960 based AAC variants. The 
 *	device_interface in the commregion will be allocated and linked 
 *	to the comm region.
 */

int aac_rkt_init(struct aac_dev *dev)
{
	unsigned long start;
	unsigned long status;
	int instance;
	const char * name;

	instance = dev->id;
	name     = dev->name;

	/*
	 *	Map in the registers from the adapter.
	 */
	if((dev->regs.rkt = (struct rkt_registers *)ioremap((unsigned long)dev->scsi_host_ptr->base, 8192))==NULL)
	{	
		printk(KERN_WARNING "aacraid: unable to map i960.\n" );
		goto error_iounmap;
	}
	/*
	 *	Check to see if the board failed any self tests.
	 */
	if (rkt_readl(dev, MUnit.OMRx[0]) & SELF_TEST_FAILED) {
		printk(KERN_ERR "%s%d: adapter self-test failed.\n", dev->name, instance);
		goto error_iounmap;
	}
	/*
	 *	Check to see if the monitor panic'd while booting.
	 */
	if (rkt_readl(dev, MUnit.OMRx[0]) & MONITOR_PANIC) {
		printk(KERN_ERR "%s%d: adapter monitor panic.\n", dev->name, instance);
		goto error_iounmap;
	}
	/*
	 *	Check to see if the board panic'd while booting.
	 */
	if (rkt_readl(dev, MUnit.OMRx[0]) & KERNEL_PANIC) {
		printk(KERN_ERR "%s%d: adapter kernel panic'd.\n", dev->name, instance);
		goto error_iounmap;
	}
	start = jiffies;
	/*
	 *	Wait for the adapter to be up and running. Wait up to 3 minutes
	 */
	while (!(rkt_readl(dev, MUnit.OMRx[0]) & KERNEL_UP_AND_RUNNING))
	{
		if(time_after(jiffies, start+180*HZ))
		{
			status = rkt_readl(dev, IndexRegs.Mailbox[7]) >> 16;
			printk(KERN_ERR "%s%d: adapter kernel failed to start, init status = %ld.\n", dev->name, instance, status);
			goto error_iounmap;
		}
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
	}
	if (request_irq(dev->scsi_host_ptr->irq, aac_rkt_intr, SA_SHIRQ|SA_INTERRUPT, "aacraid", (void *)dev)<0) 
	{
		printk(KERN_ERR "%s%d: Interrupt unavailable.\n", name, instance);
		goto error_iounmap;
	}
	/*
	 *	Fill in the function dispatch table.
	 */
	dev->a_ops.adapter_interrupt = aac_rkt_interrupt_adapter;
	dev->a_ops.adapter_enable_int = aac_rkt_enable_interrupt;
	dev->a_ops.adapter_disable_int = aac_rkt_disable_interrupt;
	dev->a_ops.adapter_notify = aac_rkt_notify_adapter;
	dev->a_ops.adapter_sync_cmd = rkt_sync_cmd;
	dev->a_ops.adapter_check_health = aac_rkt_check_health;

	if (aac_init_adapter(dev) == NULL)
		goto error_irq;
	/*
	 *	Start any kernel threads needed
	 */
	dev->thread_pid = kernel_thread((int (*)(void *))aac_command_thread, dev, 0);
	if(dev->thread_pid < 0)
	{
		printk(KERN_ERR "aacraid: Unable to create rkt thread.\n");
		goto error_kfree;
	}	
	/*
	 *	Tell the adapter that all is configured, and it can start
	 *	accepting requests
	 */
	aac_rkt_start_adapter(dev);
	return 0;

error_kfree:
	kfree(dev->queues);

error_irq:
	free_irq(dev->scsi_host_ptr->irq, (void *)dev);

error_iounmap:
	iounmap(dev->regs.rkt);

	return -1;
}
