/*
 *  acpi_power.c - ACPI Bus Power Management ($Revision: 39 $)
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

/*
 * ACPI power-managed devices may be controlled in two ways:
 * 1. via "Device Specific (D-State) Control"
 * 2. via "Power Resource Control".
 * This module is used to manage devices relying on Power Resource Control.
 * 
 * An ACPI "power resource object" describes a software controllable power
 * plane, clock plane, or other resource used by a power managed device.
 * A device may rely on multiple power resources, and a power resource
 * may be shared by multiple devices.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>


#define _COMPONENT		ACPI_POWER_COMPONENT
ACPI_MODULE_NAME		("acpi_power")

#define ACPI_POWER_COMPONENT		0x00800000
#define ACPI_POWER_CLASS		"power_resource"
#define ACPI_POWER_DRIVER_NAME		"ACPI Power Resource Driver"
#define ACPI_POWER_DEVICE_NAME		"Power Resource"
#define ACPI_POWER_FILE_INFO		"info"
#define ACPI_POWER_FILE_STATUS		"state"
#define ACPI_POWER_RESOURCE_STATE_OFF	0x00
#define ACPI_POWER_RESOURCE_STATE_ON	0x01
#define ACPI_POWER_RESOURCE_STATE_UNKNOWN 0xFF

int acpi_power_add (struct acpi_device *device);
int acpi_power_remove (struct acpi_device *device, int type);
static int acpi_power_open_fs(struct inode *inode, struct file *file);

static struct acpi_driver acpi_power_driver = {
	.name =		ACPI_POWER_DRIVER_NAME,
	.class =	ACPI_POWER_CLASS,
	.ids =		ACPI_POWER_HID,
	.ops =		{
				.add =		acpi_power_add,
				.remove =	acpi_power_remove,
			},
};

struct acpi_power_resource
{
	acpi_handle		handle;
	acpi_bus_id		name;
	u32			system_level;
	u32			order;
	int			state;
	int			references;
};

static struct list_head		acpi_power_resource_list;

static struct file_operations acpi_power_fops = {
	.open		= acpi_power_open_fs,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* --------------------------------------------------------------------------
                             Power Resource Management
   -------------------------------------------------------------------------- */

static int
acpi_power_get_context (
	acpi_handle		handle,
	struct acpi_power_resource **resource)
{
	int			result = 0;
	struct acpi_device	*device = NULL;

	ACPI_FUNCTION_TRACE("acpi_power_get_context");

	if (!resource)
		return_VALUE(-ENODEV);

	result = acpi_bus_get_device(handle, &device);
	if (result) {
		ACPI_DEBUG_PRINT((ACPI_DB_WARN, "Error getting context [%p]\n",
			handle));
		return_VALUE(result);
	}

	*resource = (struct acpi_power_resource *) acpi_driver_data(device);
	if (!resource)
		return_VALUE(-ENODEV);

	return_VALUE(0);
}


static int
acpi_power_get_state (
	struct acpi_power_resource *resource)
{
	acpi_status		status = AE_OK;
	unsigned long		sta = 0;

	ACPI_FUNCTION_TRACE("acpi_power_get_state");

	if (!resource)
		return_VALUE(-EINVAL);

	status = acpi_evaluate_integer(resource->handle, "_STA", NULL, &sta);
	if (ACPI_FAILURE(status))
		return_VALUE(-ENODEV);

	if (sta & 0x01)
		resource->state = ACPI_POWER_RESOURCE_STATE_ON;
	else
		resource->state = ACPI_POWER_RESOURCE_STATE_OFF;

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Resource [%s] is %s\n",
		resource->name, resource->state?"on":"off"));

	return_VALUE(0);
}


static int
acpi_power_get_list_state (
	struct acpi_handle_list	*list,
	int			*state)
{
	int			result = 0;
	struct acpi_power_resource *resource = NULL;
	u32			i = 0;

	ACPI_FUNCTION_TRACE("acpi_power_get_list_state");

	if (!list || !state)
		return_VALUE(-EINVAL);

	/* The state of the list is 'on' IFF all resources are 'on'. */

	for (i=0; i<list->count; i++) {
		result = acpi_power_get_context(list->handles[i], &resource);
		if (result)
			return_VALUE(result);
		result = acpi_power_get_state(resource);
		if (result)
			return_VALUE(result);

		*state = resource->state;

		if (*state != ACPI_POWER_RESOURCE_STATE_ON)
			break;
	}

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Resource list is %s\n",
		*state?"on":"off"));

	return_VALUE(result);
}


static int
acpi_power_on (
	acpi_handle		handle)
{
	int			result = 0;
	acpi_status		status = AE_OK;
	struct acpi_device	*device = NULL;
	struct acpi_power_resource *resource = NULL;

	ACPI_FUNCTION_TRACE("acpi_power_on");

	result = acpi_power_get_context(handle, &resource);
	if (result)
		return_VALUE(result);

	resource->references++;

	if ((resource->references > 1) 
		|| (resource->state == ACPI_POWER_RESOURCE_STATE_ON)) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Resource [%s] already on\n",
			resource->name));
		return_VALUE(0);
	}

	status = acpi_evaluate_object(resource->handle, "_ON", NULL, NULL);
	if (ACPI_FAILURE(status))
		return_VALUE(-ENODEV);

	result = acpi_power_get_state(resource);
	if (result)
		return_VALUE(result);
	if (resource->state != ACPI_POWER_RESOURCE_STATE_ON)
		return_VALUE(-ENOEXEC);

	/* Update the power resource's _device_ power state */
	result = acpi_bus_get_device(resource->handle, &device);
	if (result)
		return_VALUE(result);
	device->power.state = ACPI_STATE_D0;

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Resource [%s] turned on\n",
		resource->name));

	return_VALUE(0);
}


static int
acpi_power_off_device (
	acpi_handle		handle)
{
	int			result = 0;
	acpi_status		status = AE_OK;
	struct acpi_device	*device = NULL;
	struct acpi_power_resource *resource = NULL;

	ACPI_FUNCTION_TRACE("acpi_power_off_device");

	result = acpi_power_get_context(handle, &resource);
	if (result)
		return_VALUE(result);

	if (resource->references)
		resource->references--;

	if (resource->references) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO, 
			"Resource [%s] is still in use, dereferencing\n",
			device->pnp.bus_id));
		return_VALUE(0);
	}

	if (resource->state == ACPI_POWER_RESOURCE_STATE_OFF) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Resource [%s] already off\n",
			device->pnp.bus_id));
		return_VALUE(0);
	}

	status = acpi_evaluate_object(resource->handle, "_OFF", NULL, NULL);
	if (ACPI_FAILURE(status))
		return_VALUE(-ENODEV);

	result = acpi_power_get_state(resource);
	if (result)
		return_VALUE(result);
	if (resource->state != ACPI_POWER_RESOURCE_STATE_OFF)
		return_VALUE(-ENOEXEC);

	/* Update the power resource's _device_ power state */
	result = acpi_bus_get_device(resource->handle, &device);
	if (result)
		return_VALUE(result);
	device->power.state = ACPI_STATE_D3;

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Resource [%s] turned off\n",
		resource->name));

	return_VALUE(0);
}


/* --------------------------------------------------------------------------
                             Device Power Management
   -------------------------------------------------------------------------- */

int
acpi_power_get_inferred_state (
	struct acpi_device	*device)
{
	int			result = 0;
	struct acpi_handle_list	*list = NULL;
	int			list_state = 0;
	int			i = 0;

	ACPI_FUNCTION_TRACE("acpi_power_get_inferred_state");

	if (!device)
		return_VALUE(-EINVAL);

	device->power.state = ACPI_STATE_UNKNOWN;

	/*
	 * We know a device's inferred power state when all the resources
	 * required for a given D-state are 'on'.
	 */
	for (i=ACPI_STATE_D0; i<ACPI_STATE_D3; i++) {
		list = &device->power.states[i].resources;
		if (list->count < 1)
			continue;

		result = acpi_power_get_list_state(list, &list_state);
		if (result)
			return_VALUE(result);

		if (list_state == ACPI_POWER_RESOURCE_STATE_ON) {
			device->power.state = i;
			return_VALUE(0);
		}
	}

	device->power.state = ACPI_STATE_D3;

	return_VALUE(0);
}


int
acpi_power_transition (
	struct acpi_device	*device,
	int			state)
{
	int			result = 0;
	struct acpi_handle_list	*cl = NULL;	/* Current Resources */
	struct acpi_handle_list	*tl = NULL;	/* Target Resources */
	int			i = 0;

	ACPI_FUNCTION_TRACE("acpi_power_transition");

	if (!device || (state < ACPI_STATE_D0) || (state > ACPI_STATE_D3))
		return_VALUE(-EINVAL);

	if ((device->power.state < ACPI_STATE_D0) || (device->power.state > ACPI_STATE_D3))
		return_VALUE(-ENODEV);

	cl = &device->power.states[device->power.state].resources;
	tl = &device->power.states[state].resources;

	device->power.state = ACPI_STATE_UNKNOWN;

	if (!cl->count && !tl->count) {
		result = -ENODEV;
		goto end;
	}

	/* TBD: Resources must be ordered. */

	/*
	 * First we reference all power resources required in the target list
	 * (e.g. so the device doesn't lose power while transitioning).
	 */
	for (i=0; i<tl->count; i++) {
		result = acpi_power_on(tl->handles[i]);
		if (result)
			goto end;
	}

	/*
	 * Then we dereference all power resources used in the current list.
	 */
	for (i=0; i<cl->count; i++) {
		result = acpi_power_off_device(cl->handles[i]);
		if (result)
			goto end;
	}

	/* We shouldn't change the state till all above operations succeed */
	device->power.state = state;
end:
	if (result)
		ACPI_DEBUG_PRINT((ACPI_DB_WARN, 
			"Error transitioning device [%s] to D%d\n",
			device->pnp.bus_id, state));

	return_VALUE(result);
}


/* --------------------------------------------------------------------------
                              FS Interface (/proc)
   -------------------------------------------------------------------------- */

struct proc_dir_entry		*acpi_power_dir;

static int acpi_power_seq_show(struct seq_file *seq, void *offset)
{
	struct acpi_power_resource *resource = NULL;

	ACPI_FUNCTION_TRACE("acpi_power_seq_show");

	resource = (struct acpi_power_resource *)seq->private;

	if (!resource)
		goto end;

	seq_puts(seq, "state:                   ");
	switch (resource->state) {
	case ACPI_POWER_RESOURCE_STATE_ON:
		seq_puts(seq, "on\n");
		break;
	case ACPI_POWER_RESOURCE_STATE_OFF:
		seq_puts(seq, "off\n");
		break;
	default:
		seq_puts(seq, "unknown\n");
		break;
	}

	seq_printf(seq, "system level:            S%d\n"
			"order:                   %d\n"
			"reference count:         %d\n",
			resource->system_level,
			resource->order,
			resource->references);

end:
	return 0;
}

static int acpi_power_open_fs(struct inode *inode, struct file *file)
{
	return single_open(file, acpi_power_seq_show, PDE(inode)->data);
}

static int
acpi_power_add_fs (
	struct acpi_device	*device)
{
	struct proc_dir_entry	*entry = NULL;

	ACPI_FUNCTION_TRACE("acpi_power_add_fs");

	if (!device)
		return_VALUE(-EINVAL);

	if (!acpi_device_dir(device)) {
		acpi_device_dir(device) = proc_mkdir(acpi_device_bid(device),
			acpi_power_dir);
		if (!acpi_device_dir(device))
			return_VALUE(-ENODEV);
	}

	/* 'status' [R] */
	entry = create_proc_entry(ACPI_POWER_FILE_STATUS,
		S_IRUGO, acpi_device_dir(device));
	if (!entry)
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR,
			"Unable to create '%s' fs entry\n",
			ACPI_POWER_FILE_STATUS));
	else {
		entry->proc_fops = &acpi_power_fops;
		entry->data = acpi_driver_data(device);
	}

	return_VALUE(0);
}


static int
acpi_power_remove_fs (
	struct acpi_device	*device)
{
	ACPI_FUNCTION_TRACE("acpi_power_remove_fs");

	if (acpi_device_dir(device)) {
		remove_proc_entry(ACPI_POWER_FILE_STATUS,
				  acpi_device_dir(device));
		remove_proc_entry(acpi_device_bid(device), acpi_power_dir);
		acpi_device_dir(device) = NULL;
	}

	return_VALUE(0);
}


/* --------------------------------------------------------------------------
                                Driver Interface
   -------------------------------------------------------------------------- */

int
acpi_power_add (
	struct acpi_device	*device)
{
	int			result = 0;
	acpi_status		status = AE_OK;
	struct acpi_power_resource *resource = NULL;
	union acpi_object	acpi_object;
	struct acpi_buffer	buffer = {sizeof(acpi_object), &acpi_object};

	ACPI_FUNCTION_TRACE("acpi_power_add");

	if (!device)
		return_VALUE(-EINVAL);

	resource = kmalloc(sizeof(struct acpi_power_resource), GFP_KERNEL);
	if (!resource)
		return_VALUE(-ENOMEM);
	memset(resource, 0, sizeof(struct acpi_power_resource));

	resource->handle = device->handle;
	strcpy(resource->name, device->pnp.bus_id);
	strcpy(acpi_device_name(device), ACPI_POWER_DEVICE_NAME);
	strcpy(acpi_device_class(device), ACPI_POWER_CLASS);
	acpi_driver_data(device) = resource;

	/* Evalute the object to get the system level and resource order. */
	status = acpi_evaluate_object(resource->handle, NULL, NULL, &buffer);
	if (ACPI_FAILURE(status)) {
		result = -ENODEV;
		goto end;
	}
	resource->system_level = acpi_object.power_resource.system_level;
	resource->order = acpi_object.power_resource.resource_order;

	result = acpi_power_get_state(resource);
	if (result)
		goto end;

	switch (resource->state) {
	case ACPI_POWER_RESOURCE_STATE_ON:
		device->power.state = ACPI_STATE_D0;
		break;
	case ACPI_POWER_RESOURCE_STATE_OFF:
		device->power.state = ACPI_STATE_D3;
		break;
	default:
		device->power.state = ACPI_STATE_UNKNOWN;
		break;
	}

	result = acpi_power_add_fs(device);
	if (result)
		goto end;
	
	printk(KERN_INFO PREFIX "%s [%s] (%s)\n", acpi_device_name(device),
		acpi_device_bid(device), resource->state?"on":"off");

end:
	if (result)
		kfree(resource);
	
	return_VALUE(result);
}


int
acpi_power_remove (
	struct acpi_device	*device,
	int			type)
{
	struct acpi_power_resource *resource = NULL;

	ACPI_FUNCTION_TRACE("acpi_power_remove");

	if (!device || !acpi_driver_data(device))
		return_VALUE(-EINVAL);

	resource = (struct acpi_power_resource *) acpi_driver_data(device);

	acpi_power_remove_fs(device);

	kfree(resource);

	return_VALUE(0);
}


static int __init acpi_power_init (void)
{
	int			result = 0;

	ACPI_FUNCTION_TRACE("acpi_power_init");

	if (acpi_disabled)
		return_VALUE(0);

	INIT_LIST_HEAD(&acpi_power_resource_list);

	acpi_power_dir = proc_mkdir(ACPI_POWER_CLASS, acpi_root_dir);
	if (!acpi_power_dir)
		return_VALUE(-ENODEV);

	result = acpi_bus_register_driver(&acpi_power_driver);
	if (result < 0) {
		remove_proc_entry(ACPI_POWER_CLASS, acpi_root_dir);
		return_VALUE(-ENODEV);
	}

	return_VALUE(0);
}

subsys_initcall(acpi_power_init);

