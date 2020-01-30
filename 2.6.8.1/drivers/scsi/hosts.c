/*
 *  hosts.c Copyright (C) 1992 Drew Eckhardt
 *          Copyright (C) 1993, 1994, 1995 Eric Youngdale
 *          Copyright (C) 2002-2003 Christoph Hellwig
 *
 *  mid to lowlevel SCSI driver interface
 *      Initial versions: Drew Eckhardt
 *      Subsequent revisions: Eric Youngdale
 *
 *  <drew@colorado.edu>
 *
 *  Jiffies wrap fixes (host->resetting), 3 Dec 1998 Andrea Arcangeli
 *  Added QLOGIC QLA1280 SCSI controller kernel host support. 
 *     August 4, 1999 Fred Lewis, Intel DuPont
 *
 *  Updated to reflect the new initialization scheme for the higher 
 *  level of scsi drivers (sd/sr/st)
 *  September 17, 2000 Torben Mathiasen <tmm@image.dk>
 *
 *  Restructured scsi_host lists and associated functions.
 *  September 04, 2002 Mike Anderson (andmike@us.ibm.com)
 */

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/completion.h>

#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_transport.h>

#include "scsi_priv.h"
#include "scsi_logging.h"


static int scsi_host_next_hn;		/* host_no for next new host */


static void scsi_host_cls_release(struct class_device *class_dev)
{
	put_device(&class_to_shost(class_dev)->shost_gendev);
}

static struct class shost_class = {
	.name		= "scsi_host",
	.release	= scsi_host_cls_release,
};

static int scsi_device_cancel_cb(struct device *dev, void *data)
{
	return scsi_device_cancel(to_scsi_device(dev), *(int *)data);
}

/**
 * scsi_host_cancel - cancel outstanding IO to this host
 * @shost:	pointer to struct Scsi_Host
 * recovery:	recovery requested to run.
 **/
void scsi_host_cancel(struct Scsi_Host *shost, int recovery)
{
	set_bit(SHOST_CANCEL, &shost->shost_state);
	device_for_each_child(&shost->shost_gendev, &recovery,
			      scsi_device_cancel_cb);
	wait_event(shost->host_wait, (!test_bit(SHOST_RECOVERY,
						&shost->shost_state)));
}

/**
 * scsi_remove_host - remove a scsi host
 * @shost:	a pointer to a scsi host to remove
 **/
void scsi_remove_host(struct Scsi_Host *shost)
{
	scsi_host_cancel(shost, 0);
	scsi_proc_host_rm(shost);
	scsi_forget_host(shost);

	set_bit(SHOST_DEL, &shost->shost_state);

	class_device_unregister(&shost->shost_classdev);
	device_del(&shost->shost_gendev);
}

/**
 * scsi_add_host - add a scsi host
 * @shost:	scsi host pointer to add
 * @dev:	a struct device of type scsi class
 *
 * Return value: 
 * 	0 on success / != 0 for error
 **/
int scsi_add_host(struct Scsi_Host *shost, struct device *dev)
{
	struct scsi_host_template *sht = shost->hostt;
	int error;

	printk(KERN_INFO "scsi%d : %s\n", shost->host_no,
			sht->info ? sht->info(shost) : sht->name);

	if (!shost->can_queue) {
		printk(KERN_ERR "%s: can_queue = 0 no longer supported\n",
				sht->name);
		return -EINVAL;
	}

	if (!shost->shost_gendev.parent)
		shost->shost_gendev.parent = dev ? dev : &platform_bus;

	error = device_add(&shost->shost_gendev);
	if (error)
		goto out;

	set_bit(SHOST_ADD, &shost->shost_state);
	get_device(shost->shost_gendev.parent);

	error = class_device_add(&shost->shost_classdev);
	if (error)
		goto out_del_gendev;

	get_device(&shost->shost_gendev);

	error = scsi_sysfs_add_host(shost);
	if (error)
		goto out_del_classdev;

	scsi_proc_host_add(shost);
	return error;

 out_del_classdev:
	class_device_del(&shost->shost_classdev);
 out_del_gendev:
	device_del(&shost->shost_gendev);
 out:
	return error;
}

static void scsi_host_dev_release(struct device *dev)
{
	struct Scsi_Host *shost = dev_to_shost(dev);
	struct device *parent = dev->parent;

	if (shost->ehandler) {
		DECLARE_COMPLETION(sem);
		shost->eh_notify = &sem;
		shost->eh_kill = 1;
		up(shost->eh_wait);
		wait_for_completion(&sem);
		shost->eh_notify = NULL;
	}

	scsi_proc_hostdir_rm(shost->hostt);
	scsi_destroy_command_freelist(shost);

	/*
	 * Some drivers (eg aha1542) do scsi_register()/scsi_unregister()
	 * during probing without performing a scsi_set_device() in between.
	 * In this case dev->parent is NULL.
	 */
	if (parent)
		put_device(parent);
	kfree(shost);
}

/**
 * scsi_host_alloc - register a scsi host adapter instance.
 * @sht:	pointer to scsi host template
 * @privsize:	extra bytes to allocate for driver
 *
 * Note:
 * 	Allocate a new Scsi_Host and perform basic initialization.
 * 	The host is not published to the scsi midlayer until scsi_add_host
 * 	is called.
 *
 * Return value:
 * 	Pointer to a new Scsi_Host
 **/
struct Scsi_Host *scsi_host_alloc(struct scsi_host_template *sht, int privsize)
{
	struct Scsi_Host *shost;
	int gfp_mask = GFP_KERNEL, rval;
	DECLARE_COMPLETION(complete);

	if (sht->unchecked_isa_dma && privsize)
		gfp_mask |= __GFP_DMA;

        /* Check to see if this host has any error handling facilities */
        if (!sht->eh_strategy_handler && !sht->eh_abort_handler &&
	    !sht->eh_device_reset_handler && !sht->eh_bus_reset_handler &&
            !sht->eh_host_reset_handler) {
		printk(KERN_ERR "ERROR: SCSI host `%s' has no error handling\n"
				"ERROR: This is not a safe way to run your "
				        "SCSI host\n"
				"ERROR: The error handling must be added to "
				"this driver\n", sht->proc_name);
		dump_stack();
        }

	shost = kmalloc(sizeof(struct Scsi_Host) + privsize, gfp_mask);
	if (!shost)
		return NULL;
	memset(shost, 0, sizeof(struct Scsi_Host) + privsize);

	spin_lock_init(&shost->default_lock);
	scsi_assign_lock(shost, &shost->default_lock);
	INIT_LIST_HEAD(&shost->__devices);
	INIT_LIST_HEAD(&shost->eh_cmd_q);
	INIT_LIST_HEAD(&shost->starved_list);
	init_waitqueue_head(&shost->host_wait);

	init_MUTEX(&shost->scan_mutex);

	shost->host_no = scsi_host_next_hn++; /* XXX(hch): still racy */
	shost->dma_channel = 0xff;

	/* These three are default values which can be overridden */
	shost->max_channel = 0;
	shost->max_id = 8;
	shost->max_lun = 8;

	/* Give each shost a default transportt if the driver
	 * doesn't yet support Transport Attributes */
	if (!shost->transportt) 
		shost->transportt = &blank_transport_template;

	/*
	 * All drivers right now should be able to handle 12 byte
	 * commands.  Every so often there are requests for 16 byte
	 * commands, but individual low-level drivers need to certify that
	 * they actually do something sensible with such commands.
	 */
	shost->max_cmd_len = 12;
	shost->hostt = sht;
	shost->this_id = sht->this_id;
	shost->can_queue = sht->can_queue;
	shost->sg_tablesize = sht->sg_tablesize;
	shost->cmd_per_lun = sht->cmd_per_lun;
	shost->unchecked_isa_dma = sht->unchecked_isa_dma;
	shost->use_clustering = sht->use_clustering;

	if (sht->max_host_blocked)
		shost->max_host_blocked = sht->max_host_blocked;
	else
		shost->max_host_blocked = SCSI_DEFAULT_HOST_BLOCKED;

	/*
	 * If the driver imposes no hard sector transfer limit, start at
	 * machine infinity initially.
	 */
	if (sht->max_sectors)
		shost->max_sectors = sht->max_sectors;
	else
		shost->max_sectors = SCSI_DEFAULT_MAX_SECTORS;

	/*
	 * assume a 4GB boundary, if not set
	 */
	if (sht->dma_boundary)
		shost->dma_boundary = sht->dma_boundary;
	else
		shost->dma_boundary = 0xffffffff;

	rval = scsi_setup_command_freelist(shost);
	if (rval)
		goto fail_kfree;

	device_initialize(&shost->shost_gendev);
	snprintf(shost->shost_gendev.bus_id, BUS_ID_SIZE, "host%d",
		shost->host_no);
	shost->shost_gendev.release = scsi_host_dev_release;

	class_device_initialize(&shost->shost_classdev);
	shost->shost_classdev.dev = &shost->shost_gendev;
	shost->shost_classdev.class = &shost_class;
	snprintf(shost->shost_classdev.class_id, BUS_ID_SIZE, "host%d",
		  shost->host_no);

	shost->eh_notify = &complete;
	rval = kernel_thread(scsi_error_handler, shost, 0);
	if (rval < 0)
		goto fail_destroy_freelist;
	wait_for_completion(&complete);
	shost->eh_notify = NULL;
	scsi_proc_hostdir_add(shost->hostt);
	return shost;

 fail_destroy_freelist:
	scsi_destroy_command_freelist(shost);
 fail_kfree:
	kfree(shost);
	return NULL;
}

struct Scsi_Host *scsi_register(struct scsi_host_template *sht, int privsize)
{
	struct Scsi_Host *shost = scsi_host_alloc(sht, privsize);

	if (!sht->detect) {
		printk(KERN_WARNING "scsi_register() called on new-style "
				    "template for driver %s\n", sht->name);
		dump_stack();
	}

	if (shost)
		list_add_tail(&shost->sht_legacy_list, &sht->legacy_hosts);
	return shost;
}

void scsi_unregister(struct Scsi_Host *shost)
{
	list_del(&shost->sht_legacy_list);
	scsi_host_put(shost);
}

/**
 * scsi_host_lookup - get a reference to a Scsi_Host by host no
 *
 * @hostnum:	host number to locate
 *
 * Return value:
 *	A pointer to located Scsi_Host or NULL.
 **/
struct Scsi_Host *scsi_host_lookup(unsigned short hostnum)
{
	struct class *class = &shost_class;
	struct class_device *cdev;
	struct Scsi_Host *shost = ERR_PTR(-ENXIO), *p;

	down_read(&class->subsys.rwsem);
	list_for_each_entry(cdev, &class->children, node) {
		p = class_to_shost(cdev);
		if (p->host_no == hostnum) {
			shost = scsi_host_get(p);
			break;
		}
	}
	up_read(&class->subsys.rwsem);

	return shost;
}

/**
 * scsi_host_get - inc a Scsi_Host ref count
 * @shost:	Pointer to Scsi_Host to inc.
 **/
struct Scsi_Host *scsi_host_get(struct Scsi_Host *shost)
{
	if (test_bit(SHOST_DEL, &shost->shost_state) ||
		!get_device(&shost->shost_gendev))
		return NULL;
	return shost;
}

/**
 * scsi_host_put - dec a Scsi_Host ref count
 * @shost:	Pointer to Scsi_Host to dec.
 **/
void scsi_host_put(struct Scsi_Host *shost)
{
	put_device(&shost->shost_gendev);
}

int scsi_init_hosts(void)
{
	return class_register(&shost_class);
}

void scsi_exit_hosts(void)
{
	class_unregister(&shost_class);
}
