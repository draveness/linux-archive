/*
 * IBM PowerPC Virtual I/O Infrastructure Support.
 *
 *    Copyright (c) 2003-2005 IBM Corp.
 *     Dave Engebretsen engebret@us.ibm.com
 *     Santiago Leon santil@us.ibm.com
 *     Hollis Blanchard <hollisb@us.ibm.com>
 *     Stephen Rothwell
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/types.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/kobject.h>

#include <asm/iommu.h>
#include <asm/dma.h>
#include <asm/vio.h>
#include <asm/prom.h>
#include <asm/firmware.h>
#include <asm/tce.h>
#include <asm/abs_addr.h>
#include <asm/page.h>
#include <asm/hvcall.h>
#include <asm/iseries/vio.h>
#include <asm/iseries/hv_types.h>
#include <asm/iseries/hv_lp_config.h>
#include <asm/iseries/hv_call_xm.h>
#include <asm/iseries/iommu.h>

extern struct kset devices_subsys; /* needed for vio_find_name() */

static struct vio_dev vio_bus_device  = { /* fake "parent" device */
	.name = vio_bus_device.dev.bus_id,
	.type = "",
	.dev.bus_id = "vio",
	.dev.bus = &vio_bus_type,
};

#ifdef CONFIG_PPC_ISERIES
struct device *iSeries_vio_dev = &vio_bus_device.dev;
EXPORT_SYMBOL(iSeries_vio_dev);

static struct iommu_table veth_iommu_table;
static struct iommu_table vio_iommu_table;

static void __init iommu_vio_init(void)
{
	iommu_table_getparms_iSeries(255, 0, 0xff, &veth_iommu_table);
	veth_iommu_table.it_size /= 2;
	vio_iommu_table = veth_iommu_table;
	vio_iommu_table.it_offset += veth_iommu_table.it_size;

	if (!iommu_init_table(&veth_iommu_table, -1))
		printk("Virtual Bus VETH TCE table failed.\n");
	if (!iommu_init_table(&vio_iommu_table, -1))
		printk("Virtual Bus VIO TCE table failed.\n");
}
#endif

static struct iommu_table *vio_build_iommu_table(struct vio_dev *dev)
{
#ifdef CONFIG_PPC_ISERIES
	if (firmware_has_feature(FW_FEATURE_ISERIES)) {
		if (strcmp(dev->type, "network") == 0)
			return &veth_iommu_table;
		return &vio_iommu_table;
	} else
#endif
	{
		const unsigned char *dma_window;
		struct iommu_table *tbl;
		unsigned long offset, size;

		dma_window = of_get_property(dev->dev.archdata.of_node,
					  "ibm,my-dma-window", NULL);
		if (!dma_window)
			return NULL;

		tbl = kmalloc(sizeof(*tbl), GFP_KERNEL);

		of_parse_dma_window(dev->dev.archdata.of_node, dma_window,
				    &tbl->it_index, &offset, &size);

		/* TCE table size - measured in tce entries */
		tbl->it_size = size >> IOMMU_PAGE_SHIFT;
		/* offset for VIO should always be 0 */
		tbl->it_offset = offset >> IOMMU_PAGE_SHIFT;
		tbl->it_busno = 0;
		tbl->it_type = TCE_VB;

		return iommu_init_table(tbl, -1);
	}
}

/**
 * vio_match_device: - Tell if a VIO device has a matching
 *			VIO device id structure.
 * @ids:	array of VIO device id structures to search in
 * @dev:	the VIO device structure to match against
 *
 * Used by a driver to check whether a VIO device present in the
 * system is in its list of supported devices. Returns the matching
 * vio_device_id structure or NULL if there is no match.
 */
static const struct vio_device_id *vio_match_device(
		const struct vio_device_id *ids, const struct vio_dev *dev)
{
	while (ids->type[0] != '\0') {
		if ((strncmp(dev->type, ids->type, strlen(ids->type)) == 0) &&
		    of_device_is_compatible(dev->dev.archdata.of_node,
					 ids->compat))
			return ids;
		ids++;
	}
	return NULL;
}

/*
 * Convert from struct device to struct vio_dev and pass to driver.
 * dev->driver has already been set by generic code because vio_bus_match
 * succeeded.
 */
static int vio_bus_probe(struct device *dev)
{
	struct vio_dev *viodev = to_vio_dev(dev);
	struct vio_driver *viodrv = to_vio_driver(dev->driver);
	const struct vio_device_id *id;
	int error = -ENODEV;

	if (!viodrv->probe)
		return error;

	id = vio_match_device(viodrv->id_table, viodev);
	if (id)
		error = viodrv->probe(viodev, id);

	return error;
}

/* convert from struct device to struct vio_dev and pass to driver. */
static int vio_bus_remove(struct device *dev)
{
	struct vio_dev *viodev = to_vio_dev(dev);
	struct vio_driver *viodrv = to_vio_driver(dev->driver);

	if (viodrv->remove)
		return viodrv->remove(viodev);

	/* driver can't remove */
	return 1;
}

/* convert from struct device to struct vio_dev and pass to driver. */
static void vio_bus_shutdown(struct device *dev)
{
	struct vio_dev *viodev = to_vio_dev(dev);
	struct vio_driver *viodrv = to_vio_driver(dev->driver);

	if (dev->driver && viodrv->shutdown)
		viodrv->shutdown(viodev);
}

/**
 * vio_register_driver: - Register a new vio driver
 * @drv:	The vio_driver structure to be registered.
 */
int vio_register_driver(struct vio_driver *viodrv)
{
	printk(KERN_DEBUG "%s: driver %s registering\n", __FUNCTION__,
		viodrv->driver.name);

	/* fill in 'struct driver' fields */
	viodrv->driver.bus = &vio_bus_type;

	return driver_register(&viodrv->driver);
}
EXPORT_SYMBOL(vio_register_driver);

/**
 * vio_unregister_driver - Remove registration of vio driver.
 * @driver:	The vio_driver struct to be removed form registration
 */
void vio_unregister_driver(struct vio_driver *viodrv)
{
	driver_unregister(&viodrv->driver);
}
EXPORT_SYMBOL(vio_unregister_driver);

/* vio_dev refcount hit 0 */
static void __devinit vio_dev_release(struct device *dev)
{
	/* XXX should free TCE table */
	of_node_put(dev->archdata.of_node);
	kfree(to_vio_dev(dev));
}

/**
 * vio_register_device_node: - Register a new vio device.
 * @of_node:	The OF node for this device.
 *
 * Creates and initializes a vio_dev structure from the data in
 * of_node and adds it to the list of virtual devices.
 * Returns a pointer to the created vio_dev or NULL if node has
 * NULL device_type or compatible fields.
 */
struct vio_dev * __devinit vio_register_device_node(struct device_node *of_node)
{
	struct vio_dev *viodev;
	const unsigned int *unit_address;

	/* we need the 'device_type' property, in order to match with drivers */
	if (of_node->type == NULL) {
		printk(KERN_WARNING "%s: node %s missing 'device_type'\n",
				__FUNCTION__,
				of_node->name ? of_node->name : "<unknown>");
		return NULL;
	}

	unit_address = of_get_property(of_node, "reg", NULL);
	if (unit_address == NULL) {
		printk(KERN_WARNING "%s: node %s missing 'reg'\n",
				__FUNCTION__,
				of_node->name ? of_node->name : "<unknown>");
		return NULL;
	}

	/* allocate a vio_dev for this node */
	viodev = kzalloc(sizeof(struct vio_dev), GFP_KERNEL);
	if (viodev == NULL)
		return NULL;

	viodev->irq = irq_of_parse_and_map(of_node, 0);

	snprintf(viodev->dev.bus_id, BUS_ID_SIZE, "%x", *unit_address);
	viodev->name = of_node->name;
	viodev->type = of_node->type;
	viodev->unit_address = *unit_address;
	if (firmware_has_feature(FW_FEATURE_ISERIES)) {
		unit_address = of_get_property(of_node,
				"linux,unit_address", NULL);
		if (unit_address != NULL)
			viodev->unit_address = *unit_address;
	}
	viodev->dev.archdata.of_node = of_node_get(of_node);
	viodev->dev.archdata.dma_ops = &dma_iommu_ops;
	viodev->dev.archdata.dma_data = vio_build_iommu_table(viodev);
	viodev->dev.archdata.numa_node = of_node_to_nid(of_node);

	/* init generic 'struct device' fields: */
	viodev->dev.parent = &vio_bus_device.dev;
	viodev->dev.bus = &vio_bus_type;
	viodev->dev.release = vio_dev_release;

	/* register with generic device framework */
	if (device_register(&viodev->dev)) {
		printk(KERN_ERR "%s: failed to register device %s\n",
				__FUNCTION__, viodev->dev.bus_id);
		/* XXX free TCE table */
		kfree(viodev);
		return NULL;
	}

	return viodev;
}
EXPORT_SYMBOL(vio_register_device_node);

/**
 * vio_bus_init: - Initialize the virtual IO bus
 */
static int __init vio_bus_init(void)
{
	int err;
	struct device_node *node_vroot;

#ifdef CONFIG_PPC_ISERIES
	if (firmware_has_feature(FW_FEATURE_ISERIES)) {
		iommu_vio_init();
		vio_bus_device.dev.archdata.dma_ops = &dma_iommu_ops;
		vio_bus_device.dev.archdata.dma_data = &vio_iommu_table;
		iSeries_vio_dev = &vio_bus_device.dev;
	}
#endif /* CONFIG_PPC_ISERIES */

	err = bus_register(&vio_bus_type);
	if (err) {
		printk(KERN_ERR "failed to register VIO bus\n");
		return err;
	}

	/*
	 * The fake parent of all vio devices, just to give us
	 * a nice directory
	 */
	err = device_register(&vio_bus_device.dev);
	if (err) {
		printk(KERN_WARNING "%s: device_register returned %i\n",
				__FUNCTION__, err);
		return err;
	}

	node_vroot = of_find_node_by_name(NULL, "vdevice");
	if (node_vroot) {
		struct device_node *of_node;

		/*
		 * Create struct vio_devices for each virtual device in
		 * the device tree. Drivers will associate with them later.
		 */
		for (of_node = node_vroot->child; of_node != NULL;
				of_node = of_node->sibling) {
			printk(KERN_DEBUG "%s: processing %p\n",
					__FUNCTION__, of_node);
			vio_register_device_node(of_node);
		}
		of_node_put(node_vroot);
	}

	return 0;
}
__initcall(vio_bus_init);

static ssize_t name_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_vio_dev(dev)->name);
}

static ssize_t devspec_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct device_node *of_node = dev->archdata.of_node;

	return sprintf(buf, "%s\n", of_node ? of_node->full_name : "none");
}

static struct device_attribute vio_dev_attrs[] = {
	__ATTR_RO(name),
	__ATTR_RO(devspec),
	__ATTR_NULL
};

void __devinit vio_unregister_device(struct vio_dev *viodev)
{
	device_unregister(&viodev->dev);
}
EXPORT_SYMBOL(vio_unregister_device);

static int vio_bus_match(struct device *dev, struct device_driver *drv)
{
	const struct vio_dev *vio_dev = to_vio_dev(dev);
	struct vio_driver *vio_drv = to_vio_driver(drv);
	const struct vio_device_id *ids = vio_drv->id_table;

	return (ids != NULL) && (vio_match_device(ids, vio_dev) != NULL);
}

static int vio_hotplug(struct device *dev, char **envp, int num_envp,
			char *buffer, int buffer_size)
{
	const struct vio_dev *vio_dev = to_vio_dev(dev);
	struct device_node *dn;
	const char *cp;
	int length;

	if (!num_envp)
		return -ENOMEM;

	dn = dev->archdata.of_node;
	if (!dn)
		return -ENODEV;
	cp = of_get_property(dn, "compatible", &length);
	if (!cp)
		return -ENODEV;

	envp[0] = buffer;
	length = scnprintf(buffer, buffer_size, "MODALIAS=vio:T%sS%s",
				vio_dev->type, cp);
	if ((buffer_size - length) <= 0)
		return -ENOMEM;
	envp[1] = NULL;
	return 0;
}

struct bus_type vio_bus_type = {
	.name = "vio",
	.dev_attrs = vio_dev_attrs,
	.uevent = vio_hotplug,
	.match = vio_bus_match,
	.probe = vio_bus_probe,
	.remove = vio_bus_remove,
	.shutdown = vio_bus_shutdown,
};

/**
 * vio_get_attribute: - get attribute for virtual device
 * @vdev:	The vio device to get property.
 * @which:	The property/attribute to be extracted.
 * @length:	Pointer to length of returned data size (unused if NULL).
 *
 * Calls prom.c's of_get_property() to return the value of the
 * attribute specified by @which
*/
const void *vio_get_attribute(struct vio_dev *vdev, char *which, int *length)
{
	return of_get_property(vdev->dev.archdata.of_node, which, length);
}
EXPORT_SYMBOL(vio_get_attribute);

#ifdef CONFIG_PPC_PSERIES
/* vio_find_name() - internal because only vio.c knows how we formatted the
 * kobject name
 * XXX once vio_bus_type.devices is actually used as a kset in
 * drivers/base/bus.c, this function should be removed in favor of
 * "device_find(kobj_name, &vio_bus_type)"
 */
static struct vio_dev *vio_find_name(const char *kobj_name)
{
	struct kobject *found;

	found = kset_find_obj(&devices_subsys, kobj_name);
	if (!found)
		return NULL;

	return to_vio_dev(container_of(found, struct device, kobj));
}

/**
 * vio_find_node - find an already-registered vio_dev
 * @vnode: device_node of the virtual device we're looking for
 */
struct vio_dev *vio_find_node(struct device_node *vnode)
{
	const uint32_t *unit_address;
	char kobj_name[BUS_ID_SIZE];

	/* construct the kobject name from the device node */
	unit_address = of_get_property(vnode, "reg", NULL);
	if (!unit_address)
		return NULL;
	snprintf(kobj_name, BUS_ID_SIZE, "%x", *unit_address);

	return vio_find_name(kobj_name);
}
EXPORT_SYMBOL(vio_find_node);

int vio_enable_interrupts(struct vio_dev *dev)
{
	int rc = h_vio_signal(dev->unit_address, VIO_IRQ_ENABLE);
	if (rc != H_SUCCESS)
		printk(KERN_ERR "vio: Error 0x%x enabling interrupts\n", rc);
	return rc;
}
EXPORT_SYMBOL(vio_enable_interrupts);

int vio_disable_interrupts(struct vio_dev *dev)
{
	int rc = h_vio_signal(dev->unit_address, VIO_IRQ_DISABLE);
	if (rc != H_SUCCESS)
		printk(KERN_ERR "vio: Error 0x%x disabling interrupts\n", rc);
	return rc;
}
EXPORT_SYMBOL(vio_disable_interrupts);
#endif /* CONFIG_PPC_PSERIES */
