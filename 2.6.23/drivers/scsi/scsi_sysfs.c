/*
 * scsi_sysfs.c
 *
 * SCSI sysfs interface routines.
 *
 * Created to pull SCSI mid layer sysfs routines into one file.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/device.h>

#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_transport.h>
#include <scsi/scsi_driver.h>

#include "scsi_priv.h"
#include "scsi_logging.h"

static const struct {
	enum scsi_device_state	value;
	char			*name;
} sdev_states[] = {
	{ SDEV_CREATED, "created" },
	{ SDEV_RUNNING, "running" },
	{ SDEV_CANCEL, "cancel" },
	{ SDEV_DEL, "deleted" },
	{ SDEV_QUIESCE, "quiesce" },
	{ SDEV_OFFLINE,	"offline" },
	{ SDEV_BLOCK,	"blocked" },
};

const char *scsi_device_state_name(enum scsi_device_state state)
{
	int i;
	char *name = NULL;

	for (i = 0; i < ARRAY_SIZE(sdev_states); i++) {
		if (sdev_states[i].value == state) {
			name = sdev_states[i].name;
			break;
		}
	}
	return name;
}

static const struct {
	enum scsi_host_state	value;
	char			*name;
} shost_states[] = {
	{ SHOST_CREATED, "created" },
	{ SHOST_RUNNING, "running" },
	{ SHOST_CANCEL, "cancel" },
	{ SHOST_DEL, "deleted" },
	{ SHOST_RECOVERY, "recovery" },
	{ SHOST_CANCEL_RECOVERY, "cancel/recovery" },
	{ SHOST_DEL_RECOVERY, "deleted/recovery", },
};
const char *scsi_host_state_name(enum scsi_host_state state)
{
	int i;
	char *name = NULL;

	for (i = 0; i < ARRAY_SIZE(shost_states); i++) {
		if (shost_states[i].value == state) {
			name = shost_states[i].name;
			break;
		}
	}
	return name;
}

static int check_set(unsigned int *val, char *src)
{
	char *last;

	if (strncmp(src, "-", 20) == 0) {
		*val = SCAN_WILD_CARD;
	} else {
		/*
		 * Doesn't check for int overflow
		 */
		*val = simple_strtoul(src, &last, 0);
		if (*last != '\0')
			return 1;
	}
	return 0;
}

static int scsi_scan(struct Scsi_Host *shost, const char *str)
{
	char s1[15], s2[15], s3[15], junk;
	unsigned int channel, id, lun;
	int res;

	res = sscanf(str, "%10s %10s %10s %c", s1, s2, s3, &junk);
	if (res != 3)
		return -EINVAL;
	if (check_set(&channel, s1))
		return -EINVAL;
	if (check_set(&id, s2))
		return -EINVAL;
	if (check_set(&lun, s3))
		return -EINVAL;
	if (shost->transportt->user_scan)
		res = shost->transportt->user_scan(shost, channel, id, lun);
	else
		res = scsi_scan_host_selected(shost, channel, id, lun, 1);
	return res;
}

/*
 * shost_show_function: macro to create an attr function that can be used to
 * show a non-bit field.
 */
#define shost_show_function(name, field, format_string)			\
static ssize_t								\
show_##name (struct class_device *class_dev, char *buf)			\
{									\
	struct Scsi_Host *shost = class_to_shost(class_dev);		\
	return snprintf (buf, 20, format_string, shost->field);		\
}

/*
 * shost_rd_attr: macro to create a function and attribute variable for a
 * read only field.
 */
#define shost_rd_attr2(name, field, format_string)			\
	shost_show_function(name, field, format_string)			\
static CLASS_DEVICE_ATTR(name, S_IRUGO, show_##name, NULL);

#define shost_rd_attr(field, format_string) \
shost_rd_attr2(field, field, format_string)

/*
 * Create the actual show/store functions and data structures.
 */

static ssize_t store_scan(struct class_device *class_dev, const char *buf,
			  size_t count)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	int res;

	res = scsi_scan(shost, buf);
	if (res == 0)
		res = count;
	return res;
};
static CLASS_DEVICE_ATTR(scan, S_IWUSR, NULL, store_scan);

static ssize_t
store_shost_state(struct class_device *class_dev, const char *buf, size_t count)
{
	int i;
	struct Scsi_Host *shost = class_to_shost(class_dev);
	enum scsi_host_state state = 0;

	for (i = 0; i < ARRAY_SIZE(shost_states); i++) {
		const int len = strlen(shost_states[i].name);
		if (strncmp(shost_states[i].name, buf, len) == 0 &&
		   buf[len] == '\n') {
			state = shost_states[i].value;
			break;
		}
	}
	if (!state)
		return -EINVAL;

	if (scsi_host_set_state(shost, state))
		return -EINVAL;
	return count;
}

static ssize_t
show_shost_state(struct class_device *class_dev, char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	const char *name = scsi_host_state_name(shost->shost_state);

	if (!name)
		return -EINVAL;

	return snprintf(buf, 20, "%s\n", name);
}

static CLASS_DEVICE_ATTR(state, S_IRUGO | S_IWUSR, show_shost_state, store_shost_state);

shost_rd_attr(unique_id, "%u\n");
shost_rd_attr(host_busy, "%hu\n");
shost_rd_attr(cmd_per_lun, "%hd\n");
shost_rd_attr(can_queue, "%hd\n");
shost_rd_attr(sg_tablesize, "%hu\n");
shost_rd_attr(unchecked_isa_dma, "%d\n");
shost_rd_attr2(proc_name, hostt->proc_name, "%s\n");

static struct class_device_attribute *scsi_sysfs_shost_attrs[] = {
	&class_device_attr_unique_id,
	&class_device_attr_host_busy,
	&class_device_attr_cmd_per_lun,
	&class_device_attr_can_queue,
	&class_device_attr_sg_tablesize,
	&class_device_attr_unchecked_isa_dma,
	&class_device_attr_proc_name,
	&class_device_attr_scan,
	&class_device_attr_state,
	NULL
};

static void scsi_device_cls_release(struct class_device *class_dev)
{
	struct scsi_device *sdev;

	sdev = class_to_sdev(class_dev);
	put_device(&sdev->sdev_gendev);
}

static void scsi_device_dev_release_usercontext(struct work_struct *work)
{
	struct scsi_device *sdev;
	struct device *parent;
	struct scsi_target *starget;
	unsigned long flags;

	sdev = container_of(work, struct scsi_device, ew.work);

	parent = sdev->sdev_gendev.parent;
	starget = to_scsi_target(parent);

	spin_lock_irqsave(sdev->host->host_lock, flags);
	starget->reap_ref++;
	list_del(&sdev->siblings);
	list_del(&sdev->same_target_siblings);
	list_del(&sdev->starved_entry);
	spin_unlock_irqrestore(sdev->host->host_lock, flags);

	if (sdev->request_queue) {
		sdev->request_queue->queuedata = NULL;
		/* user context needed to free queue */
		scsi_free_queue(sdev->request_queue);
		/* temporary expedient, try to catch use of queue lock
		 * after free of sdev */
		sdev->request_queue = NULL;
	}

	scsi_target_reap(scsi_target(sdev));

	kfree(sdev->inquiry);
	kfree(sdev);

	if (parent)
		put_device(parent);
}

static void scsi_device_dev_release(struct device *dev)
{
	struct scsi_device *sdp = to_scsi_device(dev);
	execute_in_process_context(scsi_device_dev_release_usercontext,
				   &sdp->ew);
}

static struct class sdev_class = {
	.name		= "scsi_device",
	.release	= scsi_device_cls_release,
};

/* all probing is done in the individual ->probe routines */
static int scsi_bus_match(struct device *dev, struct device_driver *gendrv)
{
	struct scsi_device *sdp = to_scsi_device(dev);
	if (sdp->no_uld_attach)
		return 0;
	return (sdp->inq_periph_qual == SCSI_INQ_PQ_CON)? 1: 0;
}

static int scsi_bus_uevent(struct device *dev, char **envp, int num_envp,
		           char *buffer, int buffer_size)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	int i = 0;
	int length = 0;

	add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
		       "MODALIAS=" SCSI_DEVICE_MODALIAS_FMT, sdev->type);
	envp[i] = NULL;
	return 0;
}

static int scsi_bus_suspend(struct device * dev, pm_message_t state)
{
	struct device_driver *drv = dev->driver;
	struct scsi_device *sdev = to_scsi_device(dev);
	int err;

	err = scsi_device_quiesce(sdev);
	if (err)
		return err;

	if (drv && drv->suspend) {
		err = drv->suspend(dev, state);
		if (err)
			return err;
	}

	return 0;
}

static int scsi_bus_resume(struct device * dev)
{
	struct device_driver *drv = dev->driver;
	struct scsi_device *sdev = to_scsi_device(dev);
	int err = 0;

	if (drv && drv->resume)
		err = drv->resume(dev);

	scsi_device_resume(sdev);

	return err;
}

struct bus_type scsi_bus_type = {
        .name		= "scsi",
        .match		= scsi_bus_match,
	.uevent		= scsi_bus_uevent,
	.suspend	= scsi_bus_suspend,
	.resume		= scsi_bus_resume,
};

int scsi_sysfs_register(void)
{
	int error;

	error = bus_register(&scsi_bus_type);
	if (!error) {
		error = class_register(&sdev_class);
		if (error)
			bus_unregister(&scsi_bus_type);
	}

	return error;
}

void scsi_sysfs_unregister(void)
{
	class_unregister(&sdev_class);
	bus_unregister(&scsi_bus_type);
}

/*
 * sdev_show_function: macro to create an attr function that can be used to
 * show a non-bit field.
 */
#define sdev_show_function(field, format_string)				\
static ssize_t								\
sdev_show_##field (struct device *dev, struct device_attribute *attr, char *buf)				\
{									\
	struct scsi_device *sdev;					\
	sdev = to_scsi_device(dev);					\
	return snprintf (buf, 20, format_string, sdev->field);		\
}									\

/*
 * sdev_rd_attr: macro to create a function and attribute variable for a
 * read only field.
 */
#define sdev_rd_attr(field, format_string)				\
	sdev_show_function(field, format_string)			\
static DEVICE_ATTR(field, S_IRUGO, sdev_show_##field, NULL);


/*
 * sdev_rd_attr: create a function and attribute variable for a
 * read/write field.
 */
#define sdev_rw_attr(field, format_string)				\
	sdev_show_function(field, format_string)				\
									\
static ssize_t								\
sdev_store_##field (struct device *dev, struct device_attribute *attr, const char *buf, size_t count)	\
{									\
	struct scsi_device *sdev;					\
	sdev = to_scsi_device(dev);					\
	snscanf (buf, 20, format_string, &sdev->field);			\
	return count;							\
}									\
static DEVICE_ATTR(field, S_IRUGO | S_IWUSR, sdev_show_##field, sdev_store_##field);

/* Currently we don't export bit fields, but we might in future,
 * so leave this code in */
#if 0
/*
 * sdev_rd_attr: create a function and attribute variable for a
 * read/write bit field.
 */
#define sdev_rw_attr_bit(field)						\
	sdev_show_function(field, "%d\n")					\
									\
static ssize_t								\
sdev_store_##field (struct device *dev, struct device_attribute *attr, const char *buf, size_t count)	\
{									\
	int ret;							\
	struct scsi_device *sdev;					\
	ret = scsi_sdev_check_buf_bit(buf);				\
	if (ret >= 0)	{						\
		sdev = to_scsi_device(dev);				\
		sdev->field = ret;					\
		ret = count;						\
	}								\
	return ret;							\
}									\
static DEVICE_ATTR(field, S_IRUGO | S_IWUSR, sdev_show_##field, sdev_store_##field);

/*
 * scsi_sdev_check_buf_bit: return 0 if buf is "0", return 1 if buf is "1",
 * else return -EINVAL.
 */
static int scsi_sdev_check_buf_bit(const char *buf)
{
	if ((buf[1] == '\0') || ((buf[1] == '\n') && (buf[2] == '\0'))) {
		if (buf[0] == '1')
			return 1;
		else if (buf[0] == '0')
			return 0;
		else 
			return -EINVAL;
	} else
		return -EINVAL;
}
#endif
/*
 * Create the actual show/store functions and data structures.
 */
sdev_rd_attr (device_blocked, "%d\n");
sdev_rd_attr (queue_depth, "%d\n");
sdev_rd_attr (type, "%d\n");
sdev_rd_attr (scsi_level, "%d\n");
sdev_rd_attr (vendor, "%.8s\n");
sdev_rd_attr (model, "%.16s\n");
sdev_rd_attr (rev, "%.4s\n");

static ssize_t
sdev_show_timeout (struct device *dev, struct device_attribute *attr, char *buf)
{
	struct scsi_device *sdev;
	sdev = to_scsi_device(dev);
	return snprintf (buf, 20, "%d\n", sdev->timeout / HZ);
}

static ssize_t
sdev_store_timeout (struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct scsi_device *sdev;
	int timeout;
	sdev = to_scsi_device(dev);
	sscanf (buf, "%d\n", &timeout);
	sdev->timeout = timeout * HZ;
	return count;
}
static DEVICE_ATTR(timeout, S_IRUGO | S_IWUSR, sdev_show_timeout, sdev_store_timeout);

static ssize_t
store_rescan_field (struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	scsi_rescan_device(dev);
	return count;
}
static DEVICE_ATTR(rescan, S_IWUSR, NULL, store_rescan_field);

static void sdev_store_delete_callback(struct device *dev)
{
	scsi_remove_device(to_scsi_device(dev));
}

static ssize_t sdev_store_delete(struct device *dev, struct device_attribute *attr, const char *buf,
				 size_t count)
{
	int rc;

	/* An attribute cannot be unregistered by one of its own methods,
	 * so we have to use this roundabout approach.
	 */
	rc = device_schedule_callback(dev, sdev_store_delete_callback);
	if (rc)
		count = rc;
	return count;
};
static DEVICE_ATTR(delete, S_IWUSR, NULL, sdev_store_delete);

static ssize_t
store_state_field(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int i;
	struct scsi_device *sdev = to_scsi_device(dev);
	enum scsi_device_state state = 0;

	for (i = 0; i < ARRAY_SIZE(sdev_states); i++) {
		const int len = strlen(sdev_states[i].name);
		if (strncmp(sdev_states[i].name, buf, len) == 0 &&
		   buf[len] == '\n') {
			state = sdev_states[i].value;
			break;
		}
	}
	if (!state)
		return -EINVAL;

	if (scsi_device_set_state(sdev, state))
		return -EINVAL;
	return count;
}

static ssize_t
show_state_field(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	const char *name = scsi_device_state_name(sdev->sdev_state);

	if (!name)
		return -EINVAL;

	return snprintf(buf, 20, "%s\n", name);
}

static DEVICE_ATTR(state, S_IRUGO | S_IWUSR, show_state_field, store_state_field);

static ssize_t
show_queue_type_field(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	const char *name = "none";

	if (sdev->ordered_tags)
		name = "ordered";
	else if (sdev->simple_tags)
		name = "simple";

	return snprintf(buf, 20, "%s\n", name);
}

static DEVICE_ATTR(queue_type, S_IRUGO, show_queue_type_field, NULL);

static ssize_t
show_iostat_counterbits(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, 20, "%d\n", (int)sizeof(atomic_t) * 8);
}

static DEVICE_ATTR(iocounterbits, S_IRUGO, show_iostat_counterbits, NULL);

#define show_sdev_iostat(field)						\
static ssize_t								\
show_iostat_##field(struct device *dev, struct device_attribute *attr, char *buf)			\
{									\
	struct scsi_device *sdev = to_scsi_device(dev);			\
	unsigned long long count = atomic_read(&sdev->field);		\
	return snprintf(buf, 20, "0x%llx\n", count);			\
}									\
static DEVICE_ATTR(field, S_IRUGO, show_iostat_##field, NULL)

show_sdev_iostat(iorequest_cnt);
show_sdev_iostat(iodone_cnt);
show_sdev_iostat(ioerr_cnt);

static ssize_t
sdev_show_modalias(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct scsi_device *sdev;
	sdev = to_scsi_device(dev);
	return snprintf (buf, 20, SCSI_DEVICE_MODALIAS_FMT "\n", sdev->type);
}
static DEVICE_ATTR(modalias, S_IRUGO, sdev_show_modalias, NULL);

/* Default template for device attributes.  May NOT be modified */
static struct device_attribute *scsi_sysfs_sdev_attrs[] = {
	&dev_attr_device_blocked,
	&dev_attr_queue_depth,
	&dev_attr_queue_type,
	&dev_attr_type,
	&dev_attr_scsi_level,
	&dev_attr_vendor,
	&dev_attr_model,
	&dev_attr_rev,
	&dev_attr_rescan,
	&dev_attr_delete,
	&dev_attr_state,
	&dev_attr_timeout,
	&dev_attr_iocounterbits,
	&dev_attr_iorequest_cnt,
	&dev_attr_iodone_cnt,
	&dev_attr_ioerr_cnt,
	&dev_attr_modalias,
	NULL
};

static ssize_t sdev_store_queue_depth_rw(struct device *dev, struct device_attribute *attr, const char *buf,
					 size_t count)
{
	int depth, retval;
	struct scsi_device *sdev = to_scsi_device(dev);
	struct scsi_host_template *sht = sdev->host->hostt;

	if (!sht->change_queue_depth)
		return -EINVAL;

	depth = simple_strtoul(buf, NULL, 0);

	if (depth < 1)
		return -EINVAL;

	retval = sht->change_queue_depth(sdev, depth);
	if (retval < 0)
		return retval;

	return count;
}

static struct device_attribute sdev_attr_queue_depth_rw =
	__ATTR(queue_depth, S_IRUGO | S_IWUSR, sdev_show_queue_depth,
	       sdev_store_queue_depth_rw);

static ssize_t sdev_store_queue_type_rw(struct device *dev, struct device_attribute *attr, const char *buf,
					size_t count)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct scsi_host_template *sht = sdev->host->hostt;
	int tag_type = 0, retval;
	int prev_tag_type = scsi_get_tag_type(sdev);

	if (!sdev->tagged_supported || !sht->change_queue_type)
		return -EINVAL;

	if (strncmp(buf, "ordered", 7) == 0)
		tag_type = MSG_ORDERED_TAG;
	else if (strncmp(buf, "simple", 6) == 0)
		tag_type = MSG_SIMPLE_TAG;
	else if (strncmp(buf, "none", 4) != 0)
		return -EINVAL;

	if (tag_type == prev_tag_type)
		return count;

	retval = sht->change_queue_type(sdev, tag_type);
	if (retval < 0)
		return retval;

	return count;
}

static struct device_attribute sdev_attr_queue_type_rw =
	__ATTR(queue_type, S_IRUGO | S_IWUSR, show_queue_type_field,
	       sdev_store_queue_type_rw);

static struct device_attribute *attr_changed_internally(
		struct Scsi_Host *shost,
		struct device_attribute * attr)
{
	if (!strcmp("queue_depth", attr->attr.name)
	    && shost->hostt->change_queue_depth)
		return &sdev_attr_queue_depth_rw;
	else if (!strcmp("queue_type", attr->attr.name)
	    && shost->hostt->change_queue_type)
		return &sdev_attr_queue_type_rw;
	return attr;
}


static struct device_attribute *attr_overridden(
		struct device_attribute **attrs,
		struct device_attribute *attr)
{
	int i;

	if (!attrs)
		return NULL;
	for (i = 0; attrs[i]; i++)
		if (!strcmp(attrs[i]->attr.name, attr->attr.name))
			return attrs[i];
	return NULL;
}

static int attr_add(struct device *dev, struct device_attribute *attr)
{
	struct device_attribute *base_attr;

	/*
	 * Spare the caller from having to copy things it's not interested in.
	 */
	base_attr = attr_overridden(scsi_sysfs_sdev_attrs, attr);
	if (base_attr) {
		/* extend permissions */
		attr->attr.mode |= base_attr->attr.mode;

		/* override null show/store with default */
		if (!attr->show)
			attr->show = base_attr->show;
		if (!attr->store)
			attr->store = base_attr->store;
	}

	return device_create_file(dev, attr);
}

/**
 * scsi_sysfs_add_sdev - add scsi device to sysfs
 * @sdev:	scsi_device to add
 *
 * Return value:
 * 	0 on Success / non-zero on Failure
 **/
int scsi_sysfs_add_sdev(struct scsi_device *sdev)
{
	int error, i;
	struct request_queue *rq = sdev->request_queue;

	if ((error = scsi_device_set_state(sdev, SDEV_RUNNING)) != 0)
		return error;

	error = device_add(&sdev->sdev_gendev);
	if (error) {
		put_device(sdev->sdev_gendev.parent);
		printk(KERN_INFO "error 1\n");
		return error;
	}
	error = class_device_add(&sdev->sdev_classdev);
	if (error) {
		printk(KERN_INFO "error 2\n");
		goto clean_device;
	}

	/* take a reference for the sdev_classdev; this is
	 * released by the sdev_class .release */
	get_device(&sdev->sdev_gendev);

	error = bsg_register_queue(rq, &sdev->sdev_gendev, NULL);

	if (error)
		sdev_printk(KERN_INFO, sdev,
			    "Failed to register bsg queue, errno=%d\n", error);

	/* we're treating error on bsg register as non-fatal, so pretend
	 * nothing went wrong */
	error = 0;

	if (sdev->host->hostt->sdev_attrs) {
		for (i = 0; sdev->host->hostt->sdev_attrs[i]; i++) {
			error = attr_add(&sdev->sdev_gendev,
					sdev->host->hostt->sdev_attrs[i]);
			if (error) {
				__scsi_remove_device(sdev);
				goto out;
			}
		}
	}
	
	for (i = 0; scsi_sysfs_sdev_attrs[i]; i++) {
		if (!attr_overridden(sdev->host->hostt->sdev_attrs,
					scsi_sysfs_sdev_attrs[i])) {
			struct device_attribute * attr = 
				attr_changed_internally(sdev->host, 
							scsi_sysfs_sdev_attrs[i]);
			error = device_create_file(&sdev->sdev_gendev, attr);
			if (error) {
				__scsi_remove_device(sdev);
				goto out;
			}
		}
	}

	transport_add_device(&sdev->sdev_gendev);
 out:
	return error;

 clean_device:
	scsi_device_set_state(sdev, SDEV_CANCEL);

	device_del(&sdev->sdev_gendev);
	transport_destroy_device(&sdev->sdev_gendev);
	put_device(&sdev->sdev_gendev);

	return error;
}

void __scsi_remove_device(struct scsi_device *sdev)
{
	struct device *dev = &sdev->sdev_gendev;

	if (scsi_device_set_state(sdev, SDEV_CANCEL) != 0)
		return;

	bsg_unregister_queue(sdev->request_queue);
	class_device_unregister(&sdev->sdev_classdev);
	transport_remove_device(dev);
	device_del(dev);
	scsi_device_set_state(sdev, SDEV_DEL);
	if (sdev->host->hostt->slave_destroy)
		sdev->host->hostt->slave_destroy(sdev);
	transport_destroy_device(dev);
	put_device(dev);
}

/**
 * scsi_remove_device - unregister a device from the scsi bus
 * @sdev:	scsi_device to unregister
 **/
void scsi_remove_device(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;

	mutex_lock(&shost->scan_mutex);
	__scsi_remove_device(sdev);
	mutex_unlock(&shost->scan_mutex);
}
EXPORT_SYMBOL(scsi_remove_device);

static void __scsi_remove_target(struct scsi_target *starget)
{
	struct Scsi_Host *shost = dev_to_shost(starget->dev.parent);
	unsigned long flags;
	struct scsi_device *sdev;

	spin_lock_irqsave(shost->host_lock, flags);
	starget->reap_ref++;
 restart:
	list_for_each_entry(sdev, &shost->__devices, siblings) {
		if (sdev->channel != starget->channel ||
		    sdev->id != starget->id ||
		    sdev->sdev_state == SDEV_DEL)
			continue;
		spin_unlock_irqrestore(shost->host_lock, flags);
		scsi_remove_device(sdev);
		spin_lock_irqsave(shost->host_lock, flags);
		goto restart;
	}
	spin_unlock_irqrestore(shost->host_lock, flags);
	scsi_target_reap(starget);
}

static int __remove_child (struct device * dev, void * data)
{
	if (scsi_is_target_device(dev))
		__scsi_remove_target(to_scsi_target(dev));
	return 0;
}

/**
 * scsi_remove_target - try to remove a target and all its devices
 * @dev: generic starget or parent of generic stargets to be removed
 *
 * Note: This is slightly racy.  It is possible that if the user
 * requests the addition of another device then the target won't be
 * removed.
 */
void scsi_remove_target(struct device *dev)
{
	struct device *rdev;

	if (scsi_is_target_device(dev)) {
		__scsi_remove_target(to_scsi_target(dev));
		return;
	}

	rdev = get_device(dev);
	device_for_each_child(dev, NULL, __remove_child);
	put_device(rdev);
}
EXPORT_SYMBOL(scsi_remove_target);

int scsi_register_driver(struct device_driver *drv)
{
	drv->bus = &scsi_bus_type;

	return driver_register(drv);
}
EXPORT_SYMBOL(scsi_register_driver);

int scsi_register_interface(struct class_interface *intf)
{
	intf->class = &sdev_class;

	return class_interface_register(intf);
}
EXPORT_SYMBOL(scsi_register_interface);


static struct class_device_attribute *class_attr_overridden(
		struct class_device_attribute **attrs,
		struct class_device_attribute *attr)
{
	int i;

	if (!attrs)
		return NULL;
	for (i = 0; attrs[i]; i++)
		if (!strcmp(attrs[i]->attr.name, attr->attr.name))
			return attrs[i];
	return NULL;
}

static int class_attr_add(struct class_device *classdev,
		struct class_device_attribute *attr)
{
	struct class_device_attribute *base_attr;

	/*
	 * Spare the caller from having to copy things it's not interested in.
	 */
	base_attr = class_attr_overridden(scsi_sysfs_shost_attrs, attr);
	if (base_attr) {
		/* extend permissions */
		attr->attr.mode |= base_attr->attr.mode;

		/* override null show/store with default */
		if (!attr->show)
			attr->show = base_attr->show;
		if (!attr->store)
			attr->store = base_attr->store;
	}

	return class_device_create_file(classdev, attr);
}

/**
 * scsi_sysfs_add_host - add scsi host to subsystem
 * @shost:     scsi host struct to add to subsystem
 * @dev:       parent struct device pointer
 **/
int scsi_sysfs_add_host(struct Scsi_Host *shost)
{
	int error, i;

	if (shost->hostt->shost_attrs) {
		for (i = 0; shost->hostt->shost_attrs[i]; i++) {
			error = class_attr_add(&shost->shost_classdev,
					shost->hostt->shost_attrs[i]);
			if (error)
				return error;
		}
	}

	for (i = 0; scsi_sysfs_shost_attrs[i]; i++) {
		if (!class_attr_overridden(shost->hostt->shost_attrs,
					scsi_sysfs_shost_attrs[i])) {
			error = class_device_create_file(&shost->shost_classdev,
					scsi_sysfs_shost_attrs[i]);
			if (error)
				return error;
		}
	}

	transport_register_device(&shost->shost_gendev);
	return 0;
}

void scsi_sysfs_device_initialize(struct scsi_device *sdev)
{
	unsigned long flags;
	struct Scsi_Host *shost = sdev->host;
	struct scsi_target  *starget = sdev->sdev_target;

	device_initialize(&sdev->sdev_gendev);
	sdev->sdev_gendev.bus = &scsi_bus_type;
	sdev->sdev_gendev.release = scsi_device_dev_release;
	sprintf(sdev->sdev_gendev.bus_id,"%d:%d:%d:%d",
		sdev->host->host_no, sdev->channel, sdev->id,
		sdev->lun);
	
	class_device_initialize(&sdev->sdev_classdev);
	sdev->sdev_classdev.dev = &sdev->sdev_gendev;
	sdev->sdev_classdev.class = &sdev_class;
	snprintf(sdev->sdev_classdev.class_id, BUS_ID_SIZE,
		 "%d:%d:%d:%d", sdev->host->host_no,
		 sdev->channel, sdev->id, sdev->lun);
	sdev->scsi_level = starget->scsi_level;
	transport_setup_device(&sdev->sdev_gendev);
	spin_lock_irqsave(shost->host_lock, flags);
	list_add_tail(&sdev->same_target_siblings, &starget->devices);
	list_add_tail(&sdev->siblings, &shost->__devices);
	spin_unlock_irqrestore(shost->host_lock, flags);
}

int scsi_is_sdev_device(const struct device *dev)
{
	return dev->release == scsi_device_dev_release;
}
EXPORT_SYMBOL(scsi_is_sdev_device);

/* A blank transport template that is used in drivers that don't
 * yet implement Transport Attributes */
struct scsi_transport_template blank_transport_template = { { { {NULL, }, }, }, };
