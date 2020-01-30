/*
 *  drivers/s390/cio/device.c
 *  bus driver for ccw devices
 *   $Revision: 1.120 $
 *
 *    Copyright (C) 2002 IBM Deutschland Entwicklung GmbH,
 *			 IBM Corporation
 *    Author(s): Arnd Bergmann (arndb@de.ibm.com)
 *		 Cornelia Huck (cohuck@de.ibm.com)
 *		 Martin Schwidefsky (schwidefsky@de.ibm.com)
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/device.h>
#include <linux/workqueue.h>

#include <asm/ccwdev.h>
#include <asm/cio.h>

#include "cio.h"
#include "css.h"
#include "device.h"
#include "ioasm.h"

/******************* bus type handling ***********************/

/* The Linux driver model distinguishes between a bus type and
 * the bus itself. Of course we only have one channel
 * subsystem driver and one channel system per machine, but
 * we still use the abstraction. T.R. says it's a good idea. */
static int
ccw_bus_match (struct device * dev, struct device_driver * drv)
{
	struct ccw_device *cdev = to_ccwdev(dev);
	struct ccw_driver *cdrv = to_ccwdrv(drv);
	const struct ccw_device_id *ids = cdrv->ids, *found;

	if (!ids)
		return 0;

	found = ccw_device_id_match(ids, &cdev->id);
	if (!found)
		return 0;

	cdev->id.driver_info = found->driver_info;

	return 1;
}

/*
 * Hotplugging interface for ccw devices.
 * Heavily modeled on pci and usb hotplug.
 */
static int
ccw_hotplug (struct device *dev, char **envp, int num_envp,
	     char *buffer, int buffer_size)
{
	struct ccw_device *cdev = to_ccwdev(dev);
	int i = 0;
	int length = 0;

	if (!cdev)
		return -ENODEV;

	if (cdev->private->state == DEV_STATE_NOT_OPER)
		return -ENODEV;

	/* what we want to pass to /sbin/hotplug */

	envp[i++] = buffer;
	length += scnprintf(buffer, buffer_size - length, "CU_TYPE=%04X",
			   cdev->id.cu_type);
	if ((buffer_size - length <= 0) || (i >= num_envp))
		return -ENOMEM;
	++length;
	buffer += length;

	envp[i++] = buffer;
	length += scnprintf(buffer, buffer_size - length, "CU_MODEL=%02X",
			   cdev->id.cu_model);
	if ((buffer_size - length <= 0) || (i >= num_envp))
		return -ENOMEM;
	++length;
	buffer += length;

	/* The next two can be zero, that's ok for us */
	envp[i++] = buffer;
	length += scnprintf(buffer, buffer_size - length, "DEV_TYPE=%04X",
			   cdev->id.dev_type);
	if ((buffer_size - length <= 0) || (i >= num_envp))
		return -ENOMEM;
	++length;
	buffer += length;

	envp[i++] = buffer;
	length += scnprintf(buffer, buffer_size - length, "DEV_MODEL=%02X",
			   cdev->id.dev_model);
	if ((buffer_size - length <= 0) || (i >= num_envp))
		return -ENOMEM;

	envp[i] = 0;

	return 0;
}

struct bus_type ccw_bus_type = {
	.name  = "ccw",
	.match = &ccw_bus_match,
	.hotplug = &ccw_hotplug,
};

static int io_subchannel_probe (struct device *);
static int io_subchannel_remove (struct device *);
void io_subchannel_irq (struct device *);
static int io_subchannel_notify(struct device *, int);
static void io_subchannel_verify(struct device *);
static void io_subchannel_ioterm(struct device *);
static void io_subchannel_shutdown(struct device *);

struct css_driver io_subchannel_driver = {
	.subchannel_type = SUBCHANNEL_TYPE_IO,
	.drv = {
		.name = "io_subchannel",
		.bus  = &css_bus_type,
		.probe = &io_subchannel_probe,
		.remove = &io_subchannel_remove,
		.shutdown = &io_subchannel_shutdown,
	},
	.irq = io_subchannel_irq,
	.notify = io_subchannel_notify,
	.verify = io_subchannel_verify,
	.termination = io_subchannel_ioterm,
};

struct workqueue_struct *ccw_device_work;
struct workqueue_struct *ccw_device_notify_work;
static wait_queue_head_t ccw_device_init_wq;
static atomic_t ccw_device_init_count;

static int __init
init_ccw_bus_type (void)
{
	int ret;

	init_waitqueue_head(&ccw_device_init_wq);
	atomic_set(&ccw_device_init_count, 0);

	ccw_device_work = create_singlethread_workqueue("cio");
	if (!ccw_device_work)
		return -ENOMEM; /* FIXME: better errno ? */
	ccw_device_notify_work = create_singlethread_workqueue("cio_notify");
	if (!ccw_device_notify_work) {
		ret = -ENOMEM; /* FIXME: better errno ? */
		goto out_err;
	}
	slow_path_wq = create_singlethread_workqueue("kslowcrw");
	if (!slow_path_wq) {
		ret = -ENOMEM; /* FIXME: better errno ? */
		goto out_err;
	}
	if ((ret = bus_register (&ccw_bus_type)))
		goto out_err;

	if ((ret = driver_register(&io_subchannel_driver.drv)))
		goto out_err;

	wait_event(ccw_device_init_wq,
		   atomic_read(&ccw_device_init_count) == 0);
	flush_workqueue(ccw_device_work);
	return 0;
out_err:
	if (ccw_device_work)
		destroy_workqueue(ccw_device_work);
	if (ccw_device_notify_work)
		destroy_workqueue(ccw_device_notify_work);
	if (slow_path_wq)
		destroy_workqueue(slow_path_wq);
	return ret;
}

static void __exit
cleanup_ccw_bus_type (void)
{
	driver_unregister(&io_subchannel_driver.drv);
	bus_unregister(&ccw_bus_type);
	destroy_workqueue(ccw_device_notify_work);
	destroy_workqueue(ccw_device_work);
}

subsys_initcall(init_ccw_bus_type);
module_exit(cleanup_ccw_bus_type);

/************************ device handling **************************/

/*
 * A ccw_device has some interfaces in sysfs in addition to the
 * standard ones.
 * The following entries are designed to export the information which
 * resided in 2.4 in /proc/subchannels. Subchannel and device number
 * are obvious, so they don't have an entry :)
 * TODO: Split chpids and pimpampom up? Where is "in use" in the tree?
 */
static ssize_t
chpids_show (struct device * dev, char * buf)
{
	struct subchannel *sch = to_subchannel(dev);
	struct ssd_info *ssd = &sch->ssd_info;
	ssize_t ret = 0;
	int chp;

	for (chp = 0; chp < 8; chp++)
		ret += sprintf (buf+ret, "%02x ", ssd->chpid[chp]);

	ret += sprintf (buf+ret, "\n");
	return min((ssize_t)PAGE_SIZE, ret);
}

static ssize_t
pimpampom_show (struct device * dev, char * buf)
{
	struct subchannel *sch = to_subchannel(dev);
	struct pmcw *pmcw = &sch->schib.pmcw;

	return sprintf (buf, "%02x %02x %02x\n",
			pmcw->pim, pmcw->pam, pmcw->pom);
}

static ssize_t
devtype_show (struct device *dev, char *buf)
{
	struct ccw_device *cdev = to_ccwdev(dev);
	struct ccw_device_id *id = &(cdev->id);

	if (id->dev_type != 0)
		return sprintf(buf, "%04x/%02x\n",
				id->dev_type, id->dev_model);
	else
		return sprintf(buf, "n/a\n");
}

static ssize_t
cutype_show (struct device *dev, char *buf)
{
	struct ccw_device *cdev = to_ccwdev(dev);
	struct ccw_device_id *id = &(cdev->id);

	return sprintf(buf, "%04x/%02x\n",
		       id->cu_type, id->cu_model);
}

static ssize_t
online_show (struct device *dev, char *buf)
{
	struct ccw_device *cdev = to_ccwdev(dev);

	return sprintf(buf, cdev->online ? "1\n" : "0\n");
}

static void
ccw_device_remove_disconnected(struct ccw_device *cdev)
{
	struct subchannel *sch;
	/*
	 * Forced offline in disconnected state means
	 * 'throw away device'.
	 */
	sch = to_subchannel(cdev->dev.parent);
	device_unregister(&sch->dev);
	/* Reset intparm to zeroes. */
	sch->schib.pmcw.intparm = 0;
	cio_modify(sch);
	put_device(&sch->dev);
}

int
ccw_device_set_offline(struct ccw_device *cdev)
{
	int ret;

	if (!cdev)
		return -ENODEV;
	if (!cdev->online || !cdev->drv)
		return -EINVAL;

	if (cdev->drv->set_offline) {
		ret = cdev->drv->set_offline(cdev);
		if (ret != 0)
			return ret;
	}
	cdev->online = 0;
	spin_lock_irq(cdev->ccwlock);
	ret = ccw_device_offline(cdev);
	spin_unlock_irq(cdev->ccwlock);
	if (ret == 0)
		wait_event(cdev->private->wait_q, dev_fsm_final_state(cdev));
	else {
		pr_debug("ccw_device_offline returned %d, device %s\n",
			 ret, cdev->dev.bus_id);
		cdev->online = 1;
	}
 	return ret;
}

int
ccw_device_set_online(struct ccw_device *cdev)
{
	int ret;

	if (!cdev)
		return -ENODEV;
	if (cdev->online || !cdev->drv)
		return -EINVAL;

	spin_lock_irq(cdev->ccwlock);
	ret = ccw_device_online(cdev);
	spin_unlock_irq(cdev->ccwlock);
	if (ret == 0)
		wait_event(cdev->private->wait_q, dev_fsm_final_state(cdev));
	else {
		pr_debug("ccw_device_online returned %d, device %s\n",
			 ret, cdev->dev.bus_id);
		return ret;
	}
	if (cdev->private->state != DEV_STATE_ONLINE)
		return -ENODEV;
	if (!cdev->drv->set_online || cdev->drv->set_online(cdev) == 0) {
		cdev->online = 1;
		return 0;
	}
	spin_lock_irq(cdev->ccwlock);
	ret = ccw_device_offline(cdev);
	spin_unlock_irq(cdev->ccwlock);
	if (ret == 0)
		wait_event(cdev->private->wait_q, dev_fsm_final_state(cdev));
	else 
		pr_debug("ccw_device_offline returned %d, device %s\n",
			 ret, cdev->dev.bus_id);
	return (ret = 0) ? -ENODEV : ret;
}

static ssize_t
online_store (struct device *dev, const char *buf, size_t count)
{
	struct ccw_device *cdev = to_ccwdev(dev);
	int i, force, ret;
	char *tmp;

	if (atomic_compare_and_swap(0, 1, &cdev->private->onoff))
		return -EAGAIN;

	if (cdev->drv && !try_module_get(cdev->drv->owner)) {
		atomic_set(&cdev->private->onoff, 0);
		return -EINVAL;
	}
	if (!strncmp(buf, "force\n", count)) {
		force = 1;
		i = 1;
	} else {
		force = 0;
		i = simple_strtoul(buf, &tmp, 16);
	}
	if (i == 1) {
		/* Do device recognition, if needed. */
		if (cdev->id.cu_type == 0) {
			ret = ccw_device_recognition(cdev);
			if (ret) {
				printk(KERN_WARNING"Couldn't start recognition "
				       "for device %s (ret=%d)\n",
				       cdev->dev.bus_id, ret);
				goto out;
			}
			wait_event(cdev->private->wait_q,
				   cdev->private->flags.recog_done);
		}
		if (cdev->drv && cdev->drv->set_online)
			ccw_device_set_online(cdev);
	} else if (i == 0) {
		if (cdev->private->state == DEV_STATE_DISCONNECTED)
			ccw_device_remove_disconnected(cdev);
		else if (cdev->drv && cdev->drv->set_offline)
			ccw_device_set_offline(cdev);
	}
	if (force && cdev->private->state == DEV_STATE_BOXED) {
		ret = ccw_device_stlck(cdev);
		if (ret) {
			printk(KERN_WARNING"ccw_device_stlck for device %s "
			       "returned %d!\n", cdev->dev.bus_id, ret);
			goto out;
		}
		/* Do device recognition, if needed. */
		if (cdev->id.cu_type == 0) {
			cdev->private->state = DEV_STATE_NOT_OPER;
			ret = ccw_device_recognition(cdev);
			if (ret) {
				printk(KERN_WARNING"Couldn't start recognition "
				       "for device %s (ret=%d)\n",
				       cdev->dev.bus_id, ret);
				goto out;
			}
			wait_event(cdev->private->wait_q,
				   cdev->private->flags.recog_done);
		}
		if (cdev->drv && cdev->drv->set_online)
			ccw_device_set_online(cdev);
	}
	out:
	if (cdev->drv)
		module_put(cdev->drv->owner);
	atomic_set(&cdev->private->onoff, 0);
	return count;
}

static ssize_t
available_show (struct device *dev, char *buf)
{
	struct ccw_device *cdev = to_ccwdev(dev);
	struct subchannel *sch;

	switch (cdev->private->state) {
	case DEV_STATE_BOXED:
		return sprintf(buf, "boxed\n");
	case DEV_STATE_DISCONNECTED:
	case DEV_STATE_DISCONNECTED_SENSE_ID:
	case DEV_STATE_NOT_OPER:
		sch = to_subchannel(dev->parent);
		if (!sch->lpm)
			return sprintf(buf, "no path\n");
		else
			return sprintf(buf, "no device\n");
	default:
		/* All other states considered fine. */
		return sprintf(buf, "good\n");
	}
}

static DEVICE_ATTR(chpids, 0444, chpids_show, NULL);
static DEVICE_ATTR(pimpampom, 0444, pimpampom_show, NULL);
static DEVICE_ATTR(devtype, 0444, devtype_show, NULL);
static DEVICE_ATTR(cutype, 0444, cutype_show, NULL);
static DEVICE_ATTR(online, 0644, online_show, online_store);
extern struct device_attribute dev_attr_cmb_enable;
static DEVICE_ATTR(availability, 0444, available_show, NULL);

static struct attribute * subch_attrs[] = {
	&dev_attr_chpids.attr,
	&dev_attr_pimpampom.attr,
	NULL,
};

static struct attribute_group subch_attr_group = {
	.attrs = subch_attrs,
};

static inline int
subchannel_add_files (struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &subch_attr_group);
}

static struct attribute * ccwdev_attrs[] = {
	&dev_attr_devtype.attr,
	&dev_attr_cutype.attr,
	&dev_attr_online.attr,
	&dev_attr_cmb_enable.attr,
	&dev_attr_availability.attr,
	NULL,
};

static struct attribute_group ccwdev_attr_group = {
	.attrs = ccwdev_attrs,
};

static inline int
device_add_files (struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &ccwdev_attr_group);
}

static inline void
device_remove_files(struct device *dev)
{
	sysfs_remove_group(&dev->kobj, &ccwdev_attr_group);
}

/* this is a simple abstraction for device_register that sets the
 * correct bus type and adds the bus specific files */
int
ccw_device_register(struct ccw_device *cdev)
{
	struct device *dev = &cdev->dev;
	int ret;

	dev->bus = &ccw_bus_type;

	if ((ret = device_add(dev)))
		return ret;

	if ((ret = device_add_files(dev)))
		device_del(dev);

	return ret;
}

static struct ccw_device *
get_disc_ccwdev_by_devno(unsigned int devno, struct ccw_device *sibling)
{
	struct ccw_device *cdev;
	struct list_head *entry;
	struct device *dev;

	if (!get_bus(&ccw_bus_type))
		return NULL;
	down_read(&ccw_bus_type.subsys.rwsem);
	cdev = NULL;
	list_for_each(entry, &ccw_bus_type.devices.list) {
		dev = get_device(container_of(entry,
					      struct device, bus_list));
		if (!dev)
			continue;
		cdev = to_ccwdev(dev);
		if ((cdev->private->state == DEV_STATE_DISCONNECTED) &&
		    (cdev->private->devno == devno) &&
		    (!strncmp(cdev->dev.bus_id, sibling->dev.bus_id,
			      BUS_ID_SIZE))) {
			cdev->private->state = DEV_STATE_NOT_OPER;
			break;
		}
		put_device(dev);
		cdev = NULL;
	}
	up_read(&ccw_bus_type.subsys.rwsem);
	put_bus(&ccw_bus_type);

	return cdev;
}

void
ccw_device_do_unreg_rereg(void *data)
{
	struct ccw_device *cdev;
	struct subchannel *sch;
	int need_rename;

	cdev = (struct ccw_device *)data;
	sch = to_subchannel(cdev->dev.parent);
	if (cdev->private->devno != sch->schib.pmcw.dev) {
		/*
		 * The device number has changed. This is usually only when
		 * a device has been detached under VM and then re-appeared
		 * on another subchannel because of a different attachment
		 * order than before. Ideally, we should should just switch
		 * subchannels, but unfortunately, this is not possible with
		 * the current implementation.
		 * Instead, we search for the old subchannel for this device
		 * number and deregister so there are no collisions with the
		 * newly registered ccw_device.
		 * FIXME: Find another solution so the block layer doesn't
		 *        get possibly sick...
		 */
		struct ccw_device *other_cdev;

		need_rename = 1;
		other_cdev = get_disc_ccwdev_by_devno(sch->schib.pmcw.dev,
						      cdev);
		if (other_cdev) {
			struct subchannel *other_sch;

			other_sch = to_subchannel(other_cdev->dev.parent);
			if (get_device(&other_sch->dev)) {
				stsch(other_sch->irq, &other_sch->schib);
				if (other_sch->schib.pmcw.dnv) {
					other_sch->schib.pmcw.intparm = 0;
					cio_modify(other_sch);
				}
				device_unregister(&other_sch->dev);
			}
		}
		cdev->private->devno = sch->schib.pmcw.dev;
	} else
		need_rename = 0;
	device_remove_files(&cdev->dev);
	device_del(&cdev->dev);
	if (need_rename)
		snprintf (cdev->dev.bus_id, BUS_ID_SIZE, "0.0.%04x",
			  sch->schib.pmcw.dev);
	if (device_add(&cdev->dev)) {
		put_device(&cdev->dev);
		return;
	}
	if (device_add_files(&cdev->dev))
		device_unregister(&cdev->dev);
}

static void
ccw_device_release(struct device *dev)
{
	struct ccw_device *cdev;

	cdev = to_ccwdev(dev);
	kfree(cdev->private);
	kfree(cdev);
}

/*
 * Register recognized device.
 */
static void
io_subchannel_register(void *data)
{
	struct ccw_device *cdev;
	struct subchannel *sch;
	int ret;

	cdev = (struct ccw_device *) data;
	sch = to_subchannel(cdev->dev.parent);

	if (!list_empty(&sch->dev.children)) {
		bus_rescan_devices(&ccw_bus_type);
		goto out;
	}
	/* make it known to the system */
	ret = ccw_device_register(cdev);
	if (ret) {
		printk (KERN_WARNING "%s: could not register %s\n",
			__func__, cdev->dev.bus_id);
		put_device(&cdev->dev);
		sch->dev.driver_data = 0;
		kfree (cdev->private);
		kfree (cdev);
		goto out;
	}

	ret = subchannel_add_files(cdev->dev.parent);
	if (ret)
		printk(KERN_WARNING "%s: could not add attributes to %s\n",
		       __func__, sch->dev.bus_id);
	put_device(&cdev->dev);
out:
	cdev->private->flags.recog_done = 1;
	put_device(&sch->dev);
	wake_up(&cdev->private->wait_q);
}

void
ccw_device_call_sch_unregister(void *data)
{
	struct ccw_device *cdev = data;
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	device_unregister(&sch->dev);
	/* Reset intparm to zeroes. */
	sch->schib.pmcw.intparm = 0;
	cio_modify(sch);
	put_device(&cdev->dev);
	put_device(&sch->dev);
}

/*
 * subchannel recognition done. Called from the state machine.
 */
void
io_subchannel_recog_done(struct ccw_device *cdev)
{
	struct subchannel *sch;

	if (css_init_done == 0) {
		cdev->private->flags.recog_done = 1;
		return;
	}
	switch (cdev->private->state) {
	case DEV_STATE_NOT_OPER:
		cdev->private->flags.recog_done = 1;
		/* Remove device found not operational. */
		if (!get_device(&cdev->dev))
			break;
		sch = to_subchannel(cdev->dev.parent);
		INIT_WORK(&cdev->private->kick_work,
			  ccw_device_call_sch_unregister, (void *) cdev);
		queue_work(slow_path_wq, &cdev->private->kick_work);
		break;
	case DEV_STATE_BOXED:
		/* Device did not respond in time. */
	case DEV_STATE_OFFLINE:
		/* 
		 * We can't register the device in interrupt context so
		 * we schedule a work item.
		 */
		if (!get_device(&cdev->dev))
			break;
		INIT_WORK(&cdev->private->kick_work,
			  io_subchannel_register, (void *) cdev);
		queue_work(ccw_device_work, &cdev->private->kick_work);
		break;
	}
	if (atomic_dec_and_test(&ccw_device_init_count))
		wake_up(&ccw_device_init_wq);
}

static int
io_subchannel_recog(struct ccw_device *cdev, struct subchannel *sch)
{
	int rc;

	sch->dev.driver_data = cdev;
	sch->driver = &io_subchannel_driver;
	cdev->ccwlock = &sch->lock;
	*cdev->private = (struct ccw_device_private) {
		.devno	= sch->schib.pmcw.dev,
		.irq	= sch->irq,
		.state	= DEV_STATE_NOT_OPER,
		.cmb_list = LIST_HEAD_INIT(cdev->private->cmb_list),
	};
	init_waitqueue_head(&cdev->private->wait_q);
	init_timer(&cdev->private->timer);

	/* Set an initial name for the device. */
	snprintf (cdev->dev.bus_id, BUS_ID_SIZE, "0.0.%04x",
		  sch->schib.pmcw.dev);

	/* Increase counter of devices currently in recognition. */
	atomic_inc(&ccw_device_init_count);

	/* Start async. device sensing. */
	spin_lock_irq(&sch->lock);
	rc = ccw_device_recognition(cdev);
	spin_unlock_irq(&sch->lock);
	if (rc) {
		if (atomic_dec_and_test(&ccw_device_init_count))
			wake_up(&ccw_device_init_wq);
	}
	return rc;
}

static int
io_subchannel_probe (struct device *pdev)
{
	struct subchannel *sch;
	struct ccw_device *cdev;
	int rc;

	sch = to_subchannel(pdev);
	if (sch->dev.driver_data) {
		/*
		 * This subchannel already has an associated ccw_device.
		 * Register it and exit. This happens for all early
		 * device, e.g. the console.
		 */
		cdev = sch->dev.driver_data;
		device_initialize(&cdev->dev);
		ccw_device_register(cdev);
		subchannel_add_files(&sch->dev);
		/*
		 * Check if the device is already online. If it is
		 * the reference count needs to be corrected
		 * (see ccw_device_online and css_init_done for the
		 * ugly details).
		 */
		if (cdev->private->state != DEV_STATE_NOT_OPER &&
		    cdev->private->state != DEV_STATE_OFFLINE &&
		    cdev->private->state != DEV_STATE_BOXED)
			get_device(&cdev->dev);
		return 0;
	}
	cdev  = kmalloc (sizeof(*cdev), GFP_KERNEL);
	if (!cdev)
		return -ENOMEM;
	memset(cdev, 0, sizeof(struct ccw_device));
	cdev->private = kmalloc(sizeof(struct ccw_device_private), 
				GFP_KERNEL | GFP_DMA);
	if (!cdev->private) {
		kfree(cdev);
		return -ENOMEM;
	}
	memset(cdev->private, 0, sizeof(struct ccw_device_private));
	atomic_set(&cdev->private->onoff, 0);
	cdev->dev = (struct device) {
		.parent = pdev,
		.release = ccw_device_release,
	};
	/* Do first half of device_register. */
	device_initialize(&cdev->dev);

	if (!get_device(&sch->dev)) {
		if (cdev->dev.release)
			cdev->dev.release(&cdev->dev);
		return -ENODEV;
	}

	rc = io_subchannel_recog(cdev, to_subchannel(pdev));
	if (rc) {
		sch->dev.driver_data = 0;
		if (cdev->dev.release)
			cdev->dev.release(&cdev->dev);
	}

	return rc;
}

static int
io_subchannel_remove (struct device *dev)
{
	struct ccw_device *cdev;

	if (!dev->driver_data)
		return 0;
	cdev = dev->driver_data;
	/* Set ccw device to not operational and drop reference. */
	cdev->private->state = DEV_STATE_NOT_OPER;
	/*
	 * Careful here. Our ccw device might be yet unregistered when
	 * de-registering its subchannel (machine check during device
	 * recognition). Better look if the subchannel has children.
	 */
	if (!list_empty(&dev->children))
		device_unregister(&cdev->dev);
	dev->driver_data = NULL;
	return 0;
}

static int
io_subchannel_notify(struct device *dev, int event)
{
	struct ccw_device *cdev;

	cdev = dev->driver_data;
	if (!cdev)
		return 0;
	if (!cdev->drv)
		return 0;
	if (!cdev->online)
		return 0;
	return cdev->drv->notify ? cdev->drv->notify(cdev, event) : 0;
}

static void
io_subchannel_verify(struct device *dev)
{
	struct ccw_device *cdev;

	cdev = dev->driver_data;
	if (cdev)
		dev_fsm_event(cdev, DEV_EVENT_VERIFY);
}

static void
io_subchannel_ioterm(struct device *dev)
{
	struct ccw_device *cdev;

	cdev = dev->driver_data;
	if (!cdev)
		return;
	cdev->private->state = DEV_STATE_CLEAR_VERIFY;
	if (cdev->handler)
		cdev->handler(cdev, cdev->private->intparm,
			      ERR_PTR(-EIO));
}

static void
io_subchannel_shutdown(struct device *dev)
{
	struct subchannel *sch;
	struct ccw_device *cdev;
	int ret;

	sch = to_subchannel(dev);
	cdev = dev->driver_data;

	if (cio_is_console(sch->irq))
		return;
	if (!sch->schib.pmcw.ena)
		/* Nothing to do. */
		return;
	ret = cio_disable_subchannel(sch);
	if (ret != -EBUSY)
		/* Subchannel is disabled, we're done. */
		return;
	cdev->private->state = DEV_STATE_QUIESCE;
	if (cdev->handler)
		cdev->handler(cdev, cdev->private->intparm,
			      ERR_PTR(-EIO));
	ret = ccw_device_cancel_halt_clear(cdev);
	if (ret == -EBUSY) {
		ccw_device_set_timeout(cdev, HZ/10);
		wait_event(cdev->private->wait_q, dev_fsm_final_state(cdev));
	}
	cio_disable_subchannel(sch);
}

#ifdef CONFIG_CCW_CONSOLE
static struct ccw_device console_cdev;
static struct ccw_device_private console_private;
static int console_cdev_in_use;

static int
ccw_device_console_enable (struct ccw_device *cdev, struct subchannel *sch)
{
	int rc;

	/* Initialize the ccw_device structure. */
	cdev->dev = (struct device) {
		.parent = &sch->dev,
	};
	/* Initialize the subchannel structure */
	sch->dev.parent = &css_bus_device;
	sch->dev.bus = &css_bus_type;

	rc = io_subchannel_recog(cdev, sch);
	if (rc)
		return rc;

	/* Now wait for the async. recognition to come to an end. */
	spin_lock_irq(cdev->ccwlock);
	while (!dev_fsm_final_state(cdev))
		wait_cons_dev();
	rc = -EIO;
	if (cdev->private->state != DEV_STATE_OFFLINE)
		goto out_unlock;
	ccw_device_online(cdev);
	while (!dev_fsm_final_state(cdev))
		wait_cons_dev();
	if (cdev->private->state != DEV_STATE_ONLINE)
		goto out_unlock;
	rc = 0;
out_unlock:
	spin_unlock_irq(cdev->ccwlock);
	return 0;
}

struct ccw_device *
ccw_device_probe_console(void)
{
	struct subchannel *sch;
	int ret;

	if (xchg(&console_cdev_in_use, 1) != 0)
		return NULL;
	sch = cio_probe_console();
	if (IS_ERR(sch)) {
		console_cdev_in_use = 0;
		return (void *) sch;
	}
	memset(&console_cdev, 0, sizeof(struct ccw_device));
	memset(&console_private, 0, sizeof(struct ccw_device_private));
	console_cdev.private = &console_private;
	ret = ccw_device_console_enable(&console_cdev, sch);
	if (ret) {
		cio_release_console();
		console_cdev_in_use = 0;
		return ERR_PTR(ret);
	}
	console_cdev.online = 1;
	return &console_cdev;
}
#endif

/*
 * get ccw_device matching the busid, but only if owned by cdrv
 */
struct ccw_device *
get_ccwdev_by_busid(struct ccw_driver *cdrv, const char *bus_id)
{
	struct device *d, *dev;
	struct device_driver *drv;

	drv = get_driver(&cdrv->driver);
	if (!drv)
		return 0;

	down_read(&drv->bus->subsys.rwsem);

	dev = NULL;
	list_for_each_entry(d, &drv->devices, driver_list) {
		dev = get_device(d);

		if (dev && !strncmp(bus_id, dev->bus_id, BUS_ID_SIZE))
			break;
		else if (dev) {
			put_device(dev);
			dev = NULL;
		}
	}
	up_read(&drv->bus->subsys.rwsem);
	put_driver(drv);

	return dev ? to_ccwdev(dev) : 0;
}

/************************** device driver handling ************************/

/* This is the implementation of the ccw_driver class. The probe, remove
 * and release methods are initially very similar to the device_driver
 * implementations, with the difference that they have ccw_device
 * arguments.
 *
 * A ccw driver also contains the information that is needed for
 * device matching.
 */
static int
ccw_device_probe (struct device *dev)
{
	struct ccw_device *cdev = to_ccwdev(dev);
	struct ccw_driver *cdrv = to_ccwdrv(dev->driver);
	int ret;

	cdev->drv = cdrv; /* to let the driver call _set_online */

	ret = cdrv->probe ? cdrv->probe(cdev) : -ENODEV;

	if (ret) {
		cdev->drv = 0;
		return ret;
	}

	return 0;
}

static int
ccw_device_remove (struct device *dev)
{
	struct ccw_device *cdev = to_ccwdev(dev);
	struct ccw_driver *cdrv = cdev->drv;
	int ret;

	pr_debug("removing device %s\n", cdev->dev.bus_id);
	if (cdrv->remove)
		cdrv->remove(cdev);
	if (cdev->online) {
		cdev->online = 0;
		spin_lock_irq(cdev->ccwlock);
		ret = ccw_device_offline(cdev);
		spin_unlock_irq(cdev->ccwlock);
		if (ret == 0)
			wait_event(cdev->private->wait_q,
				   dev_fsm_final_state(cdev));
		else
			//FIXME: we can't fail!
			pr_debug("ccw_device_offline returned %d, device %s\n",
				 ret, cdev->dev.bus_id);
	}
	ccw_device_set_timeout(cdev, 0);
	cdev->drv = 0;
	return 0;
}

int
ccw_driver_register (struct ccw_driver *cdriver)
{
	struct device_driver *drv = &cdriver->driver;

	drv->bus = &ccw_bus_type;
	drv->name = cdriver->name;
	drv->probe = ccw_device_probe;
	drv->remove = ccw_device_remove;

	return driver_register(drv);
}

void
ccw_driver_unregister (struct ccw_driver *cdriver)
{
	driver_unregister(&cdriver->driver);
}

MODULE_LICENSE("GPL");
EXPORT_SYMBOL(ccw_device_set_online);
EXPORT_SYMBOL(ccw_device_set_offline);
EXPORT_SYMBOL(ccw_driver_register);
EXPORT_SYMBOL(ccw_driver_unregister);
EXPORT_SYMBOL(get_ccwdev_by_busid);
EXPORT_SYMBOL(ccw_bus_type);
EXPORT_SYMBOL(ccw_device_work);
EXPORT_SYMBOL(ccw_device_notify_work);
