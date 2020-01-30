/*
 * firmware_class.c - Multi purpose firmware loading support
 *
 * Copyright (c) 2003 Manuel Estrada Sainz <ranty@debian.org>
 *
 * Please see Documentation/firmware_class/ for more information.
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <asm/hardirq.h>
#include <linux/bitops.h>
#include <asm/semaphore.h>

#include <linux/firmware.h>
#include "base.h"

MODULE_AUTHOR("Manuel Estrada Sainz <ranty@debian.org>");
MODULE_DESCRIPTION("Multi purpose firmware loading support");
MODULE_LICENSE("GPL");

enum {
	FW_STATUS_LOADING,
	FW_STATUS_DONE,
	FW_STATUS_ABORT,
	FW_STATUS_READY,
};

static int loading_timeout = 10;	/* In seconds */

/* fw_lock could be moved to 'struct firmware_priv' but since it is just
 * guarding for corner cases a global lock should be OK */
static DECLARE_MUTEX(fw_lock);

struct firmware_priv {
	char fw_id[FIRMWARE_NAME_MAX];
	struct completion completion;
	struct bin_attribute attr_data;
	struct firmware *fw;
	unsigned long status;
	int alloc_size;
	struct timer_list timeout;
};

static inline void
fw_load_abort(struct firmware_priv *fw_priv)
{
	set_bit(FW_STATUS_ABORT, &fw_priv->status);
	wmb();
	complete(&fw_priv->completion);
}

static ssize_t
firmware_timeout_show(struct class *class, char *buf)
{
	return sprintf(buf, "%d\n", loading_timeout);
}

/**
 * firmware_timeout_store:
 * Description:
 *	Sets the number of seconds to wait for the firmware.  Once
 *	this expires an error will be return to the driver and no
 *	firmware will be provided.
 *
 *	Note: zero means 'wait for ever'
 *
 **/
static ssize_t
firmware_timeout_store(struct class *class, const char *buf, size_t count)
{
	loading_timeout = simple_strtol(buf, NULL, 10);
	return count;
}

static CLASS_ATTR(timeout, 0644, firmware_timeout_show, firmware_timeout_store);

static void  fw_class_dev_release(struct class_device *class_dev);
int firmware_class_hotplug(struct class_device *dev, char **envp,
			   int num_envp, char *buffer, int buffer_size);

static struct class firmware_class = {
	.name		= "firmware",
	.hotplug	= firmware_class_hotplug,
	.release	= fw_class_dev_release,
};

int
firmware_class_hotplug(struct class_device *class_dev, char **envp,
		       int num_envp, char *buffer, int buffer_size)
{
	struct firmware_priv *fw_priv = class_get_devdata(class_dev);
	int i = 0;
	char *scratch = buffer;

	if (!test_bit(FW_STATUS_READY, &fw_priv->status))
		return -ENODEV;

	if (buffer_size < (FIRMWARE_NAME_MAX + 10))
		return -ENOMEM;
	if (num_envp < 1)
		return -ENOMEM;

	envp[i++] = scratch;
	scratch += sprintf(scratch, "FIRMWARE=%s", fw_priv->fw_id) + 1;
	return 0;
}

static ssize_t
firmware_loading_show(struct class_device *class_dev, char *buf)
{
	struct firmware_priv *fw_priv = class_get_devdata(class_dev);
	int loading = test_bit(FW_STATUS_LOADING, &fw_priv->status);
	return sprintf(buf, "%d\n", loading);
}

/**
 * firmware_loading_store: - loading control file
 * Description:
 *	The relevant values are:
 *
 *	 1: Start a load, discarding any previous partial load.
 *	 0: Conclude the load and handle the data to the driver code.
 *	-1: Conclude the load with an error and discard any written data.
 **/
static ssize_t
firmware_loading_store(struct class_device *class_dev,
		       const char *buf, size_t count)
{
	struct firmware_priv *fw_priv = class_get_devdata(class_dev);
	int loading = simple_strtol(buf, NULL, 10);

	switch (loading) {
	case 1:
		down(&fw_lock);
		vfree(fw_priv->fw->data);
		fw_priv->fw->data = NULL;
		fw_priv->fw->size = 0;
		fw_priv->alloc_size = 0;
		set_bit(FW_STATUS_LOADING, &fw_priv->status);
		up(&fw_lock);
		break;
	case 0:
		if (test_bit(FW_STATUS_LOADING, &fw_priv->status)) {
			complete(&fw_priv->completion);
			clear_bit(FW_STATUS_LOADING, &fw_priv->status);
			break;
		}
		/* fallthrough */
	default:
		printk(KERN_ERR "%s: unexpected value (%d)\n", __FUNCTION__,
		       loading);
		/* fallthrough */
	case -1:
		fw_load_abort(fw_priv);
		break;
	}

	return count;
}

static CLASS_DEVICE_ATTR(loading, 0644,
			firmware_loading_show, firmware_loading_store);

static ssize_t
firmware_data_read(struct kobject *kobj,
		   char *buffer, loff_t offset, size_t count)
{
	struct class_device *class_dev = to_class_dev(kobj);
	struct firmware_priv *fw_priv = class_get_devdata(class_dev);
	struct firmware *fw;
	ssize_t ret_count = count;

	down(&fw_lock);
	fw = fw_priv->fw;
	if (test_bit(FW_STATUS_DONE, &fw_priv->status)) {
		ret_count = -ENODEV;
		goto out;
	}
	if (offset > fw->size) {
		ret_count = 0;
		goto out;
	}
	if (offset + ret_count > fw->size)
		ret_count = fw->size - offset;

	memcpy(buffer, fw->data + offset, ret_count);
out:
	up(&fw_lock);
	return ret_count;
}
static int
fw_realloc_buffer(struct firmware_priv *fw_priv, int min_size)
{
	u8 *new_data;

	if (min_size <= fw_priv->alloc_size)
		return 0;

	new_data = vmalloc(fw_priv->alloc_size + PAGE_SIZE);
	if (!new_data) {
		printk(KERN_ERR "%s: unable to alloc buffer\n", __FUNCTION__);
		/* Make sure that we don't keep incomplete data */
		fw_load_abort(fw_priv);
		return -ENOMEM;
	}
	fw_priv->alloc_size += PAGE_SIZE;
	if (fw_priv->fw->data) {
		memcpy(new_data, fw_priv->fw->data, fw_priv->fw->size);
		vfree(fw_priv->fw->data);
	}
	fw_priv->fw->data = new_data;
	BUG_ON(min_size > fw_priv->alloc_size);
	return 0;
}

/**
 * firmware_data_write:
 *
 * Description:
 *
 *	Data written to the 'data' attribute will be later handled to
 *	the driver as a firmware image.
 **/
static ssize_t
firmware_data_write(struct kobject *kobj,
		    char *buffer, loff_t offset, size_t count)
{
	struct class_device *class_dev = to_class_dev(kobj);
	struct firmware_priv *fw_priv = class_get_devdata(class_dev);
	struct firmware *fw;
	ssize_t retval;

	down(&fw_lock);
	fw = fw_priv->fw;
	if (test_bit(FW_STATUS_DONE, &fw_priv->status)) {
		retval = -ENODEV;
		goto out;
	}
	retval = fw_realloc_buffer(fw_priv, offset + count);
	if (retval)
		goto out;

	memcpy(fw->data + offset, buffer, count);

	fw->size = max_t(size_t, offset + count, fw->size);
	retval = count;
out:
	up(&fw_lock);
	return retval;
}
static struct bin_attribute firmware_attr_data_tmpl = {
	.attr = {.name = "data", .mode = 0644, .owner = THIS_MODULE},
	.size = 0,
	.read = firmware_data_read,
	.write = firmware_data_write,
};

static void
fw_class_dev_release(struct class_device *class_dev)
{
	struct firmware_priv *fw_priv = class_get_devdata(class_dev);

	kfree(fw_priv);
	kfree(class_dev);

	module_put(THIS_MODULE);
}

static void
firmware_class_timeout(u_long data)
{
	struct firmware_priv *fw_priv = (struct firmware_priv *) data;
	fw_load_abort(fw_priv);
}

static inline void
fw_setup_class_device_id(struct class_device *class_dev, struct device *dev)
{
	/* XXX warning we should watch out for name collisions */
	strlcpy(class_dev->class_id, dev->bus_id, BUS_ID_SIZE);
}

static int
fw_register_class_device(struct class_device **class_dev_p,
			 const char *fw_name, struct device *device)
{
	int retval;
	struct firmware_priv *fw_priv = kmalloc(sizeof (struct firmware_priv),
						GFP_KERNEL);
	struct class_device *class_dev = kmalloc(sizeof (struct class_device),
						 GFP_KERNEL);

	*class_dev_p = NULL;

	if (!fw_priv || !class_dev) {
		printk(KERN_ERR "%s: kmalloc failed\n", __FUNCTION__);
		retval = -ENOMEM;
		goto error_kfree;
	}
	memset(fw_priv, 0, sizeof (*fw_priv));
	memset(class_dev, 0, sizeof (*class_dev));

	init_completion(&fw_priv->completion);
	fw_priv->attr_data = firmware_attr_data_tmpl;
	strlcpy(fw_priv->fw_id, fw_name, FIRMWARE_NAME_MAX);

	fw_priv->timeout.function = firmware_class_timeout;
	fw_priv->timeout.data = (u_long) fw_priv;
	init_timer(&fw_priv->timeout);

	fw_setup_class_device_id(class_dev, device);
	class_dev->dev = device;
	class_dev->class = &firmware_class;
	class_set_devdata(class_dev, fw_priv);
	retval = class_device_register(class_dev);
	if (retval) {
		printk(KERN_ERR "%s: class_device_register failed\n",
		       __FUNCTION__);
		goto error_kfree;
	}
	*class_dev_p = class_dev;
	return 0;

error_kfree:
	kfree(fw_priv);
	kfree(class_dev);
	return retval;
}

static int
fw_setup_class_device(struct firmware *fw, struct class_device **class_dev_p,
		      const char *fw_name, struct device *device)
{
	struct class_device *class_dev;
	struct firmware_priv *fw_priv;
	int retval;

	*class_dev_p = NULL;
	retval = fw_register_class_device(&class_dev, fw_name, device);
	if (retval)
		goto out;

	/* Need to pin this module until class device is destroyed */
	__module_get(THIS_MODULE);

	fw_priv = class_get_devdata(class_dev);

	fw_priv->fw = fw;
	retval = sysfs_create_bin_file(&class_dev->kobj, &fw_priv->attr_data);
	if (retval) {
		printk(KERN_ERR "%s: sysfs_create_bin_file failed\n",
		       __FUNCTION__);
		goto error_unreg;
	}

	retval = class_device_create_file(class_dev,
					  &class_device_attr_loading);
	if (retval) {
		printk(KERN_ERR "%s: class_device_create_file failed\n",
		       __FUNCTION__);
		goto error_unreg;
	}

	set_bit(FW_STATUS_READY, &fw_priv->status);
	*class_dev_p = class_dev;
	goto out;

error_unreg:
	class_device_unregister(class_dev);
out:
	return retval;
}

/**
 * request_firmware: - request firmware to hotplug and wait for it
 * Description:
 *	@firmware will be used to return a firmware image by the name
 *	of @name for device @device.
 *
 *	Should be called from user context where sleeping is allowed.
 *
 *	@name will be use as $FIRMWARE in the hotplug environment and
 *	should be distinctive enough not to be confused with any other
 *	firmware image for this or any other device.
 **/
int
request_firmware(const struct firmware **firmware_p, const char *name,
		 struct device *device)
{
	struct class_device *class_dev;
	struct firmware_priv *fw_priv;
	struct firmware *firmware;
	int retval;

	if (!firmware_p)
		return -EINVAL;

	*firmware_p = firmware = kmalloc(sizeof (struct firmware), GFP_KERNEL);
	if (!firmware) {
		printk(KERN_ERR "%s: kmalloc(struct firmware) failed\n",
		       __FUNCTION__);
		retval = -ENOMEM;
		goto out;
	}
	memset(firmware, 0, sizeof (*firmware));

	retval = fw_setup_class_device(firmware, &class_dev, name, device);
	if (retval)
		goto error_kfree_fw;

	fw_priv = class_get_devdata(class_dev);

	if (loading_timeout) {
		fw_priv->timeout.expires = jiffies + loading_timeout * HZ;
		add_timer(&fw_priv->timeout);
	}

	kobject_hotplug("add", &class_dev->kobj);
	wait_for_completion(&fw_priv->completion);
	set_bit(FW_STATUS_DONE, &fw_priv->status);

	del_timer_sync(&fw_priv->timeout);

	down(&fw_lock);
	if (!fw_priv->fw->size || test_bit(FW_STATUS_ABORT, &fw_priv->status)) {
		retval = -ENOENT;
		release_firmware(fw_priv->fw);
		*firmware_p = NULL;
	}
	fw_priv->fw = NULL;
	up(&fw_lock);
	class_device_unregister(class_dev);
	goto out;

error_kfree_fw:
	kfree(firmware);
out:
	return retval;
}

/**
 * release_firmware: - release the resource associated with a firmware image
 **/
void
release_firmware(const struct firmware *fw)
{
	if (fw) {
		vfree(fw->data);
		kfree(fw);
	}
}

/**
 * register_firmware: - provide a firmware image for later usage
 *
 * Description:
 *	Make sure that @data will be available by requesting firmware @name.
 *
 *	Note: This will not be possible until some kind of persistence
 *	is available.
 **/
void
register_firmware(const char *name, const u8 *data, size_t size)
{
	/* This is meaningless without firmware caching, so until we
	 * decide if firmware caching is reasonable just leave it as a
	 * noop */
}

/* Async support */
struct firmware_work {
	struct work_struct work;
	struct module *module;
	const char *name;
	struct device *device;
	void *context;
	void (*cont)(const struct firmware *fw, void *context);
};

static int
request_firmware_work_func(void *arg)
{
	struct firmware_work *fw_work = arg;
	const struct firmware *fw;
	if (!arg) {
		WARN_ON(1);
		return 0;
	}
	daemonize("%s/%s", "firmware", fw_work->name);
	request_firmware(&fw, fw_work->name, fw_work->device);
	fw_work->cont(fw, fw_work->context);
	release_firmware(fw);
	module_put(fw_work->module);
	kfree(fw_work);
	return 0;
}

/**
 * request_firmware_nowait:
 *
 * Description:
 *	Asynchronous variant of request_firmware() for contexts where
 *	it is not possible to sleep.
 *
 *	@cont will be called asynchronously when the firmware request is over.
 *
 *	@context will be passed over to @cont.
 *
 *	@fw may be %NULL if firmware request fails.
 *
 **/
int
request_firmware_nowait(
	struct module *module,
	const char *name, struct device *device, void *context,
	void (*cont)(const struct firmware *fw, void *context))
{
	struct firmware_work *fw_work = kmalloc(sizeof (struct firmware_work),
						GFP_ATOMIC);
	int ret;

	if (!fw_work)
		return -ENOMEM;
	if (!try_module_get(module)) {
		kfree(fw_work);
		return -EFAULT;
	}

	*fw_work = (struct firmware_work) {
		.module = module,
		.name = name,
		.device = device,
		.context = context,
		.cont = cont,
	};

	ret = kernel_thread(request_firmware_work_func, fw_work,
			    CLONE_FS | CLONE_FILES);

	if (ret < 0) {
		fw_work->cont(NULL, fw_work->context);
		return ret;
	}
	return 0;
}

static int __init
firmware_class_init(void)
{
	int error;
	error = class_register(&firmware_class);
	if (error) {
		printk(KERN_ERR "%s: class_register failed\n", __FUNCTION__);
		return error;
	}
	error = class_create_file(&firmware_class, &class_attr_timeout);
	if (error) {
		printk(KERN_ERR "%s: class_create_file failed\n",
		       __FUNCTION__);
		class_unregister(&firmware_class);
	}
	return error;

}
static void __exit
firmware_class_exit(void)
{
	class_unregister(&firmware_class);
}

module_init(firmware_class_init);
module_exit(firmware_class_exit);

EXPORT_SYMBOL(release_firmware);
EXPORT_SYMBOL(request_firmware);
EXPORT_SYMBOL(request_firmware_nowait);
EXPORT_SYMBOL(register_firmware);
EXPORT_SYMBOL(firmware_class);
