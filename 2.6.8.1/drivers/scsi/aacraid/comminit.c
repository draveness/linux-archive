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
 *  comminit.c
 *
 * Abstract: This supports the initialization of the host adapter commuication interface.
 *    This is a platform dependent module for the pci cyclone board.
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
#include <linux/completion.h>
#include <linux/mm.h>
#include <asm/semaphore.h>

#include "aacraid.h"

struct aac_common aac_config;

static int aac_alloc_comm(struct aac_dev *dev, void **commaddr, unsigned long commsize, unsigned long commalign)
{
	unsigned char *base;
	unsigned long size, align;
	unsigned long fibsize = 4096;
	unsigned long printfbufsiz = 256;
	struct aac_init *init;
	dma_addr_t phys;

	size = fibsize + sizeof(struct aac_init) + commsize + commalign + printfbufsiz;

 
	base = pci_alloc_consistent(dev->pdev, size, &phys);

	if(base == NULL)
	{
		printk(KERN_ERR "aacraid: unable to create mapping.\n");
		return 0;
	}
	dev->comm_addr = (void *)base;
	dev->comm_phys = phys;
	dev->comm_size = size;
	
	dev->init = (struct aac_init *)(base + fibsize);
	dev->init_pa = phys + fibsize;

	init = dev->init;

	init->InitStructRevision = cpu_to_le32(ADAPTER_INIT_STRUCT_REVISION);
	init->MiniPortRevision = cpu_to_le32(Sa_MINIPORT_REVISION);
	init->fsrev = cpu_to_le32(dev->fsrev);

	/*
	 *	Adapter Fibs are the first thing allocated so that they
	 *	start page aligned
	 */
	dev->aif_base_va = (struct hw_fib *)base;
	
	init->AdapterFibsVirtualAddress = cpu_to_le32(0);
	init->AdapterFibsPhysicalAddress = cpu_to_le32((u32)phys);
	init->AdapterFibsSize = cpu_to_le32(fibsize);
	init->AdapterFibAlign = cpu_to_le32(sizeof(struct hw_fib));
	/* 
	 * number of 4k pages of host physical memory. The aacraid fw needs
	 * this number to be less than 4gb worth of pages. num_physpages is in
	 * system page units. New firmware doesn't have any issues with the
	 * mapping system, but older Firmware did, and had *troubles* dealing
	 * with the math overloading past 32 bits, thus we must limit this
	 * field.
	 *
	 * This assumes the memory is mapped zero->n, which isnt
	 * always true on real computers. It also has some slight problems
	 * with the GART on x86-64. I've btw never tried DMA from PCI space
	 * on this platform but don't be suprised if its problematic.
	 */
#ifndef CONFIG_GART_IOMMU
	if ((num_physpages << (PAGE_SHIFT - 12)) <= AAC_MAX_HOSTPHYSMEMPAGES) {
		init->HostPhysMemPages = 
			cpu_to_le32(num_physpages << (PAGE_SHIFT-12));
	} else 
#endif	
	{
		init->HostPhysMemPages = cpu_to_le32(AAC_MAX_HOSTPHYSMEMPAGES);
	}


	/*
	 * Increment the base address by the amount already used
	 */
	base = base + fibsize + sizeof(struct aac_init);
	phys = (dma_addr_t)((ulong)phys + fibsize + sizeof(struct aac_init));
	/*
	 *	Align the beginning of Headers to commalign
	 */
	align = (commalign - ((unsigned long)(base) & (commalign - 1)));
	base = base + align;
	phys = phys + align;
	/*
	 *	Fill in addresses of the Comm Area Headers and Queues
	 */
	*commaddr = base;
	init->CommHeaderAddress = cpu_to_le32((u32)phys);
	/*
	 *	Increment the base address by the size of the CommArea
	 */
	base = base + commsize;
	phys = phys + commsize;
	/*
	 *	 Place the Printf buffer area after the Fast I/O comm area.
	 */
	dev->printfbuf = (void *)base;
	init->printfbuf = cpu_to_le32(phys);
	init->printfbufsiz = cpu_to_le32(printfbufsiz);
	memset(base, 0, printfbufsiz);
	return 1;
}
    
static void aac_queue_init(struct aac_dev * dev, struct aac_queue * q, u32 *mem, int qsize)
{
	q->numpending = 0;
	q->dev = dev;
	INIT_LIST_HEAD(&q->pendingq);
	init_waitqueue_head(&q->cmdready);
	INIT_LIST_HEAD(&q->cmdq);
	init_waitqueue_head(&q->qfull);
	spin_lock_init(&q->lockdata);
	q->lock = &q->lockdata;
	q->headers.producer = mem;
	q->headers.consumer = mem+1;
	*(q->headers.producer) = cpu_to_le32(qsize);
	*(q->headers.consumer) = cpu_to_le32(qsize);
	q->entries = qsize;
}

/**
 *	aac_send_shutdown		-	shutdown an adapter
 *	@dev: Adapter to shutdown
 *
 *	This routine will send a VM_CloseAll (shutdown) request to the adapter.
 */

int aac_send_shutdown(struct aac_dev * dev)
{
	struct fib * fibctx;
	struct aac_close *cmd;
	int status;

	fibctx = fib_alloc(dev);
	fib_init(fibctx);

	cmd = (struct aac_close *) fib_data(fibctx);

	cmd->command = cpu_to_le32(VM_CloseAll);
	cmd->cid = cpu_to_le32(0xffffffff);

	status = fib_send(ContainerCommand,
			  fibctx,
			  sizeof(struct aac_close),
			  FsaNormal,
			  1, 1,
			  NULL, NULL);

	if (status == 0)
		fib_complete(fibctx);
	fib_free(fibctx);
	return status;
}

/**
 *	aac_comm_init	-	Initialise FSA data structures
 *	@dev:	Adapter to initialise
 *
 *	Initializes the data structures that are required for the FSA commuication
 *	interface to operate. 
 *	Returns
 *		1 - if we were able to init the commuication interface.
 *		0 - If there were errors initing. This is a fatal error.
 */
 
int aac_comm_init(struct aac_dev * dev)
{
	unsigned long hdrsize = (sizeof(u32) * NUMBER_OF_COMM_QUEUES) * 2;
	unsigned long queuesize = sizeof(struct aac_entry) * TOTAL_QUEUE_ENTRIES;
	u32 *headers;
	struct aac_entry * queues;
	unsigned long size;
	struct aac_queue_block * comm = dev->queues;
	/*
	 *	Now allocate and initialize the zone structures used as our 
	 *	pool of FIB context records.  The size of the zone is based
	 *	on the system memory size.  We also initialize the mutex used
	 *	to protect the zone.
	 */
	spin_lock_init(&dev->fib_lock);

	/*
	 *	Allocate the physically contigous space for the commuication
	 *	queue headers. 
	 */

	size = hdrsize + queuesize;

	if (!aac_alloc_comm(dev, (void * *)&headers, size, QUEUE_ALIGNMENT))
		return -ENOMEM;

	queues = (struct aac_entry *)(((ulong)headers) + hdrsize);

	/* Adapter to Host normal priority Command queue */ 
	comm->queue[HostNormCmdQueue].base = queues;
	aac_queue_init(dev, &comm->queue[HostNormCmdQueue], headers, HOST_NORM_CMD_ENTRIES);
	queues += HOST_NORM_CMD_ENTRIES;
	headers += 2;

	/* Adapter to Host high priority command queue */
	comm->queue[HostHighCmdQueue].base = queues;
	aac_queue_init(dev, &comm->queue[HostHighCmdQueue], headers, HOST_HIGH_CMD_ENTRIES);
    
	queues += HOST_HIGH_CMD_ENTRIES;
	headers +=2;

	/* Host to adapter normal priority command queue */
	comm->queue[AdapNormCmdQueue].base = queues;
	aac_queue_init(dev, &comm->queue[AdapNormCmdQueue], headers, ADAP_NORM_CMD_ENTRIES);
    
	queues += ADAP_NORM_CMD_ENTRIES;
	headers += 2;

	/* host to adapter high priority command queue */
	comm->queue[AdapHighCmdQueue].base = queues;
	aac_queue_init(dev, &comm->queue[AdapHighCmdQueue], headers, ADAP_HIGH_CMD_ENTRIES);
    
	queues += ADAP_HIGH_CMD_ENTRIES;
	headers += 2;

	/* adapter to host normal priority response queue */
	comm->queue[HostNormRespQueue].base = queues;
	aac_queue_init(dev, &comm->queue[HostNormRespQueue], headers, HOST_NORM_RESP_ENTRIES);
	queues += HOST_NORM_RESP_ENTRIES;
	headers += 2;

	/* adapter to host high priority response queue */
	comm->queue[HostHighRespQueue].base = queues;
	aac_queue_init(dev, &comm->queue[HostHighRespQueue], headers, HOST_HIGH_RESP_ENTRIES);
   
	queues += HOST_HIGH_RESP_ENTRIES;
	headers += 2;

	/* host to adapter normal priority response queue */
	comm->queue[AdapNormRespQueue].base = queues;
	aac_queue_init(dev, &comm->queue[AdapNormRespQueue], headers, ADAP_NORM_RESP_ENTRIES);

	queues += ADAP_NORM_RESP_ENTRIES;
	headers += 2;
	
	/* host to adapter high priority response queue */ 
	comm->queue[AdapHighRespQueue].base = queues;
	aac_queue_init(dev, &comm->queue[AdapHighRespQueue], headers, ADAP_HIGH_RESP_ENTRIES);

	comm->queue[AdapNormCmdQueue].lock = comm->queue[HostNormRespQueue].lock;
	comm->queue[AdapHighCmdQueue].lock = comm->queue[HostHighRespQueue].lock;
	comm->queue[AdapNormRespQueue].lock = comm->queue[HostNormCmdQueue].lock;
	comm->queue[AdapHighRespQueue].lock = comm->queue[HostHighCmdQueue].lock;

	return 0;
}

struct aac_dev *aac_init_adapter(struct aac_dev *dev)
{
	/*
	 *	Ok now init the communication subsystem
	 */

	dev->queues = (struct aac_queue_block *) kmalloc(sizeof(struct aac_queue_block), GFP_KERNEL);
	if (dev->queues == NULL) {
		printk(KERN_ERR "Error could not allocate comm region.\n");
		return NULL;
	}
	memset(dev->queues, 0, sizeof(struct aac_queue_block));

	if (aac_comm_init(dev)<0){
		kfree(dev->queues);
		return NULL;
	}
	/*
	 *	Initialize the list of fibs
	 */
	if(fib_setup(dev)<0){
		kfree(dev->queues);
		return NULL;
	}
		
	INIT_LIST_HEAD(&dev->fib_list);
	init_completion(&dev->aif_completion);

	return dev;
}

    
