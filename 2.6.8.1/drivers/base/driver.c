/*
 * driver.c - centralized device driver management
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/config.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "base.h"

#define to_dev(node) container_of(node, struct device, driver_list)
#define to_drv(obj) container_of(obj, struct device_driver, kobj)

/**
 *	driver_create_file - create sysfs file for driver.
 *	@drv:	driver.
 *	@attr:	driver attribute descriptor.
 */

int driver_create_file(struct device_driver * drv, struct driver_attribute * attr)
{
	int error;
	if (get_driver(drv)) {
		error = sysfs_create_file(&drv->kobj, &attr->attr);
		put_driver(drv);
	} else
		error = -EINVAL;
	return error;
}


/**
 *	driver_remove_file - remove sysfs file for driver.
 *	@drv:	driver.
 *	@attr:	driver attribute descriptor.
 */

void driver_remove_file(struct device_driver * drv, struct driver_attribute * attr)
{
	if (get_driver(drv)) {
		sysfs_remove_file(&drv->kobj, &attr->attr);
		put_driver(drv);
	}
}


/**
 *	get_driver - increment driver reference count.
 *	@drv:	driver.
 */
struct device_driver * get_driver(struct device_driver * drv)
{
	return drv ? to_drv(kobject_get(&drv->kobj)) : NULL;
}


/**
 *	put_driver - decrement driver's refcount.
 *	@drv:	driver.
 */
void put_driver(struct device_driver * drv)
{
	kobject_put(&drv->kobj);
}


/**
 *	driver_register - register driver with bus
 *	@drv:	driver to register
 *
 *	We pass off most of the work to the bus_add_driver() call,
 *	since most of the things we have to do deal with the bus
 *	structures.
 *
 *	The one interesting aspect is that we initialize @drv->unload_sem
 *	to a locked state here. It will be unlocked when the driver
 *	reference count reaches 0.
 */
int driver_register(struct device_driver * drv)
{
	INIT_LIST_HEAD(&drv->devices);
	init_MUTEX_LOCKED(&drv->unload_sem);
	return bus_add_driver(drv);
}


/**
 *	driver_unregister - remove driver from system.
 *	@drv:	driver.
 *
 *	Again, we pass off most of the work to the bus-level call.
 *
 *	Though, once that is done, we attempt to take @drv->unload_sem.
 *	This will block until the driver refcount reaches 0, and it is
 *	released. Only modular drivers will call this function, and we
 *	have to guarantee that it won't complete, letting the driver
 *	unload until all references are gone.
 */

void driver_unregister(struct device_driver * drv)
{
	bus_remove_driver(drv);
	down(&drv->unload_sem);
	up(&drv->unload_sem);
}

/**
 *	driver_find - locate driver on a bus by its name.
 *	@name:	name of the driver.
 *	@bus:	bus to scan for the driver.
 *
 *	Call kset_find_obj() to iterate over list of drivers on
 *	a bus to find driver by name. Return driver if found.
 *
 *	Note that kset_find_obj increments driver's reference count.
 */
struct device_driver *driver_find(const char *name, struct bus_type *bus)
{
	struct kobject *k = kset_find_obj(&bus->drivers, name);
	if (k)
		return to_drv(k);
	return NULL;
}

EXPORT_SYMBOL(driver_register);
EXPORT_SYMBOL(driver_unregister);
EXPORT_SYMBOL(get_driver);
EXPORT_SYMBOL(put_driver);
EXPORT_SYMBOL(driver_find);

EXPORT_SYMBOL(driver_create_file);
EXPORT_SYMBOL(driver_remove_file);
