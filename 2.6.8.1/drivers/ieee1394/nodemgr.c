/*
 * Node information (ConfigROM) collection and management.
 *
 * Copyright (C) 2000		Andreas E. Bombe
 *               2001-2003	Ben Collins <bcollins@debian.net>
 *
 * This code is licensed under the GPL.  See the file COPYING in the root
 * directory of the kernel sources for details.
 */

#include <linux/kernel.h>
#include <linux/config.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/interrupt.h>
#include <linux/kmod.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/moduleparam.h>
#include <linux/suspend.h>
#include <asm/atomic.h>

#include "ieee1394_types.h"
#include "ieee1394.h"
#include "hosts.h"
#include "ieee1394_transactions.h"
#include "highlevel.h"
#include "csr.h"
#include "nodemgr.h"

static int ignore_drivers = 0;
module_param(ignore_drivers, int, 0444);
MODULE_PARM_DESC(ignore_drivers, "Disable automatic probing for drivers.");

struct nodemgr_csr_info {
	struct hpsb_host *host;
	nodeid_t nodeid;
	unsigned int generation;
};


static char *nodemgr_find_oui_name(int oui)
{
#ifdef CONFIG_IEEE1394_OUI_DB
	extern struct oui_list_struct {
		int oui;
		char *name;
	} oui_list[];
	int i;

	for (i = 0; oui_list[i].name; i++)
		if (oui_list[i].oui == oui)
			return oui_list[i].name;
#endif
	return NULL;
}


static int nodemgr_bus_read(struct csr1212_csr *csr, u64 addr, u16 length,
                            void *buffer, void *__ci)
{
	struct nodemgr_csr_info *ci = (struct nodemgr_csr_info*)__ci;
	int i, ret = 0;

	for (i = 0; i < 3; i++) {
		ret = hpsb_read(ci->host, ci->nodeid, ci->generation, addr,
				buffer, length);
		if (!ret)
			break;

		set_current_state(TASK_INTERRUPTIBLE);
		if (schedule_timeout (HZ/3))
			return -EINTR;
	}

	return ret;
}

static int nodemgr_get_max_rom(quadlet_t *bus_info_data, void *__ci)
{
	return (CSR1212_BE32_TO_CPU(bus_info_data[2]) >> 8) & 0x3;
}

static struct csr1212_bus_ops nodemgr_csr_ops = {
	.bus_read =	nodemgr_bus_read,
	.get_max_rom =	nodemgr_get_max_rom
};


/*
 * Basically what we do here is start off retrieving the bus_info block.
 * From there will fill in some info about the node, verify it is of IEEE
 * 1394 type, and that the crc checks out ok. After that we start off with
 * the root directory, and subdirectories. To do this, we retrieve the
 * quadlet header for a directory, find out the length, and retrieve the
 * complete directory entry (be it a leaf or a directory). We then process
 * it and add the info to our structure for that particular node.
 *
 * We verify CRC's along the way for each directory/block/leaf. The entire
 * node structure is generic, and simply stores the information in a way
 * that's easy to parse by the protocol interface.
 */

/*
 * The nodemgr relies heavily on the Driver Model for device callbacks and
 * driver/device mappings. The old nodemgr used to handle all this itself,
 * but now we are much simpler because of the LDM.
 */

static DECLARE_MUTEX(nodemgr_serialize);

struct host_info {
	struct hpsb_host *host;
	struct list_head list;
	struct completion exited;
	struct semaphore reset_sem;
	int pid;
	char daemon_name[15];
	int kill_me;
};

static int nodemgr_bus_match(struct device * dev, struct device_driver * drv);
static int nodemgr_hotplug(struct class_device *cdev, char **envp, int num_envp,
			   char *buffer, int buffer_size);
static void nodemgr_resume_ne(struct node_entry *ne);
static void nodemgr_remove_ne(struct node_entry *ne);
static struct node_entry *find_entry_by_guid(u64 guid);

struct bus_type ieee1394_bus_type = {
	.name		= "ieee1394",
	.match		= nodemgr_bus_match,
};

static void host_cls_release(struct class_device *class_dev)
{
	put_device(&container_of((class_dev), struct hpsb_host, class_dev)->device);
}

struct class hpsb_host_class = {
	.name		= "ieee1394_host",
	.release	= host_cls_release,
};

static void ne_cls_release(struct class_device *class_dev)
{
	put_device(&container_of((class_dev), struct node_entry, class_dev)->device);
}

struct class nodemgr_ne_class = {
	.name		= "ieee1394_node",
	.release	= ne_cls_release,
};

static void ud_cls_release(struct class_device *class_dev)
{
	put_device(&container_of((class_dev), struct unit_directory, class_dev)->device);
}

/* The name here is only so that unit directory hotplug works with old
 * style hotplug, which only ever did unit directories anyway. */
struct class nodemgr_ud_class = {
	.name		= "ieee1394",
	.release	= ud_cls_release,
	.hotplug	= nodemgr_hotplug,
};

static struct hpsb_highlevel nodemgr_highlevel;


static void nodemgr_release_ud(struct device *dev)
{
	struct unit_directory *ud = container_of(dev, struct unit_directory, device);

	if (ud->vendor_name_kv)
		csr1212_release_keyval(ud->vendor_name_kv);
	if (ud->model_name_kv)
		csr1212_release_keyval(ud->model_name_kv);

	kfree(ud);
}

static void nodemgr_release_ne(struct device *dev)
{
	struct node_entry *ne = container_of(dev, struct node_entry, device);

	if (ne->vendor_name_kv)
		csr1212_release_keyval(ne->vendor_name_kv);

	kfree(ne);
}


static void nodemgr_release_host(struct device *dev)
{
	struct hpsb_host *host = container_of(dev, struct hpsb_host, device);

	csr1212_destroy_csr(host->csr.rom);

	kfree(host);
}

static int nodemgr_ud_platform_data;

static struct device nodemgr_dev_template_ud = {
	.bus		= &ieee1394_bus_type,
	.release	= nodemgr_release_ud,
	.platform_data	= &nodemgr_ud_platform_data,
};

static struct device nodemgr_dev_template_ne = {
	.bus		= &ieee1394_bus_type,
	.release	= nodemgr_release_ne,
};

struct device nodemgr_dev_template_host = {
	.bus		= &ieee1394_bus_type,
	.release	= nodemgr_release_host,
};


#define fw_attr(class, class_type, field, type, format_string)		\
static ssize_t fw_show_##class##_##field (struct device *dev, char *buf)\
{									\
	class_type *class;						\
	class = container_of(dev, class_type, device);			\
	return sprintf(buf, format_string, (type)class->field);		\
}									\
static struct device_attribute dev_attr_##class##_##field = {		\
	.attr = {.name = __stringify(field), .mode = S_IRUGO },		\
	.show   = fw_show_##class##_##field,				\
};

#define fw_attr_td(class, class_type, td_kv)				\
static ssize_t fw_show_##class##_##td_kv (struct device *dev, char *buf)\
{									\
	int len;							\
	class_type *class = container_of(dev, class_type, device);	\
	len = (class->td_kv->value.leaf.len - 2) * sizeof(quadlet_t);	\
	memcpy(buf,							\
	       CSR1212_TEXTUAL_DESCRIPTOR_LEAF_DATA(class->td_kv),	\
	       len);							\
	while ((buf + len - 1) == '\0')					\
		len--;							\
	buf[len++] = '\n';						\
	buf[len] = '\0';						\
	return len;							\
}									\
static struct device_attribute dev_attr_##class##_##td_kv = {		\
	.attr = {.name = __stringify(td_kv), .mode = S_IRUGO },		\
	.show   = fw_show_##class##_##td_kv,				\
};


#define fw_drv_attr(field, type, format_string)			\
static ssize_t fw_drv_show_##field (struct device_driver *drv, char *buf) \
{								\
	struct hpsb_protocol_driver *driver;			\
	driver = container_of(drv, struct hpsb_protocol_driver, driver); \
	return sprintf(buf, format_string, (type)driver->field);\
}								\
static struct driver_attribute driver_attr_drv_##field = {	\
        .attr = {.name = __stringify(field), .mode = S_IRUGO },	\
        .show   = fw_drv_show_##field,				\
};


static ssize_t fw_show_ne_bus_options(struct device *dev, char *buf)
{
	struct node_entry *ne = container_of(dev, struct node_entry, device);

	return sprintf(buf, "IRMC(%d) CMC(%d) ISC(%d) BMC(%d) PMC(%d) GEN(%d) "
		       "LSPD(%d) MAX_REC(%d) MAX_ROM(%d) CYC_CLK_ACC(%d)\n",
		       ne->busopt.irmc,
		       ne->busopt.cmc, ne->busopt.isc, ne->busopt.bmc,
		       ne->busopt.pmc, ne->busopt.generation, ne->busopt.lnkspd,
		       ne->busopt.max_rec,
		       ne->busopt.max_rom,
		       ne->busopt.cyc_clk_acc);
}
static DEVICE_ATTR(bus_options,S_IRUGO,fw_show_ne_bus_options,NULL);


static ssize_t fw_show_ne_tlabels_free(struct device *dev, char *buf)
{
	struct node_entry *ne = container_of(dev, struct node_entry, device);
	return sprintf(buf, "%d\n", atomic_read(&ne->tpool->count.count) + 1);
}
static DEVICE_ATTR(tlabels_free,S_IRUGO,fw_show_ne_tlabels_free,NULL);


static ssize_t fw_show_ne_tlabels_allocations(struct device *dev, char *buf)
{
	struct node_entry *ne = container_of(dev, struct node_entry, device);
	return sprintf(buf, "%u\n", ne->tpool->allocations);
}
static DEVICE_ATTR(tlabels_allocations,S_IRUGO,fw_show_ne_tlabels_allocations,NULL);


static ssize_t fw_show_ne_tlabels_mask(struct device *dev, char *buf)
{
	struct node_entry *ne = container_of(dev, struct node_entry, device);
#if (BITS_PER_LONG <= 32)
	return sprintf(buf, "0x%08lx%08lx\n", ne->tpool->pool[0], ne->tpool->pool[1]);
#else
	return sprintf(buf, "0x%016lx\n", ne->tpool->pool[0]);
#endif
}
static DEVICE_ATTR(tlabels_mask, S_IRUGO, fw_show_ne_tlabels_mask, NULL);


static ssize_t fw_set_ignore_driver(struct device *dev, const char *buf, size_t count)
{
	struct unit_directory *ud = container_of(dev, struct unit_directory, device);
	int state = simple_strtoul(buf, NULL, 10);

	if (state == 1) {
		down_write(&dev->bus->subsys.rwsem);
		device_release_driver(dev);
		ud->ignore_driver = 1;
		up_write(&dev->bus->subsys.rwsem);
	} else if (!state)
		ud->ignore_driver = 0;

	return count;
}
static ssize_t fw_get_ignore_driver(struct device *dev, char *buf)
{
	struct unit_directory *ud = container_of(dev, struct unit_directory, device);

	return sprintf(buf, "%d\n", ud->ignore_driver);
}
static DEVICE_ATTR(ignore_driver, S_IWUSR | S_IRUGO, fw_get_ignore_driver, fw_set_ignore_driver);


static ssize_t fw_set_destroy_node(struct bus_type *bus, const char *buf, size_t count)
{
	struct node_entry *ne;
	u64 guid = (u64)simple_strtoull(buf, NULL, 16);

	ne = find_entry_by_guid(guid);

	if (ne == NULL || !ne->in_limbo)
		return -EINVAL;

	nodemgr_remove_ne(ne);

	return count;
}
static ssize_t fw_get_destroy_node(struct bus_type *bus, char *buf)
{
	return sprintf(buf, "You can destroy in_limbo nodes by writing their GUID to this file\n");
}
static BUS_ATTR(destroy_node, S_IWUSR | S_IRUGO, fw_get_destroy_node, fw_set_destroy_node);

static int nodemgr_rescan_bus_thread(void *__unused)
{
	/* No userlevel access needed */
	daemonize("kfwrescan");

	bus_rescan_devices(&ieee1394_bus_type);

	return 0;
}

static ssize_t fw_set_rescan(struct bus_type *bus, const char *buf, size_t count)
{
	int state = simple_strtoul(buf, NULL, 10);

	/* Don't wait for this, or care about errors. Root could do
	 * something stupid and spawn this a lot of times, but that's
	 * root's fault. */
	if (state == 1)
		kernel_thread(nodemgr_rescan_bus_thread, NULL, CLONE_KERNEL);

	return count;
}
static ssize_t fw_get_rescan(struct bus_type *bus, char *buf)
{
	return sprintf(buf, "You can force a rescan of the bus for "
			"drivers by writing a 1 to this file\n");
}
static BUS_ATTR(rescan, S_IWUSR | S_IRUGO, fw_get_rescan, fw_set_rescan);


static ssize_t fw_set_ignore_drivers(struct bus_type *bus, const char *buf, size_t count)
{
	int state = simple_strtoul(buf, NULL, 10);

	if (state == 1)
		ignore_drivers = 1;
	else if (!state)
		ignore_drivers = 0;

	return count;
}
static ssize_t fw_get_ignore_drivers(struct bus_type *bus, char *buf)
{
	return sprintf(buf, "%d\n", ignore_drivers);
}
static BUS_ATTR(ignore_drivers, S_IWUSR | S_IRUGO, fw_get_ignore_drivers, fw_set_ignore_drivers);


struct bus_attribute *const fw_bus_attrs[] = {
	&bus_attr_destroy_node,
	&bus_attr_rescan,
	&bus_attr_ignore_drivers,
	NULL
};


fw_attr(ne, struct node_entry, capabilities, unsigned int, "0x%06x\n")
fw_attr(ne, struct node_entry, nodeid, unsigned int, "0x%04x\n")

fw_attr(ne, struct node_entry, vendor_id, unsigned int, "0x%06x\n")
fw_attr_td(ne, struct node_entry, vendor_name_kv)
fw_attr(ne, struct node_entry, vendor_oui, const char *, "%s\n")

fw_attr(ne, struct node_entry, guid, unsigned long long, "0x%016Lx\n")
fw_attr(ne, struct node_entry, guid_vendor_id, unsigned int, "0x%06x\n")
fw_attr(ne, struct node_entry, guid_vendor_oui, const char *, "%s\n")
fw_attr(ne, struct node_entry, in_limbo, int, "%d\n");

static struct device_attribute *const fw_ne_attrs[] = {
	&dev_attr_ne_guid,
	&dev_attr_ne_guid_vendor_id,
	&dev_attr_ne_capabilities,
	&dev_attr_ne_vendor_id,
	&dev_attr_ne_nodeid,
	&dev_attr_bus_options,
	&dev_attr_tlabels_free,
	&dev_attr_tlabels_allocations,
	&dev_attr_tlabels_mask,
};



fw_attr(ud, struct unit_directory, address, unsigned long long, "0x%016Lx\n")
fw_attr(ud, struct unit_directory, length, int, "%d\n")
/* These are all dependent on the value being provided */
fw_attr(ud, struct unit_directory, vendor_id, unsigned int, "0x%06x\n")
fw_attr(ud, struct unit_directory, model_id, unsigned int, "0x%06x\n")
fw_attr(ud, struct unit_directory, specifier_id, unsigned int, "0x%06x\n")
fw_attr(ud, struct unit_directory, version, unsigned int, "0x%06x\n")
fw_attr_td(ud, struct unit_directory, vendor_name_kv)
fw_attr(ud, struct unit_directory, vendor_oui, const char *, "%s\n")
fw_attr_td(ud, struct unit_directory, model_name_kv)

static struct device_attribute *const fw_ud_attrs[] = {
	&dev_attr_ud_address,
	&dev_attr_ud_length,
	&dev_attr_ignore_driver,
};


fw_attr(host, struct hpsb_host, node_count, int, "%d\n")
fw_attr(host, struct hpsb_host, selfid_count, int, "%d\n")
fw_attr(host, struct hpsb_host, nodes_active, int, "%d\n")
fw_attr(host, struct hpsb_host, in_bus_reset, int, "%d\n")
fw_attr(host, struct hpsb_host, is_root, int, "%d\n")
fw_attr(host, struct hpsb_host, is_cycmst, int, "%d\n")
fw_attr(host, struct hpsb_host, is_irm, int, "%d\n")
fw_attr(host, struct hpsb_host, is_busmgr, int, "%d\n")

static struct device_attribute *const fw_host_attrs[] = {
	&dev_attr_host_node_count,
	&dev_attr_host_selfid_count,
	&dev_attr_host_nodes_active,
	&dev_attr_host_in_bus_reset,
	&dev_attr_host_is_root,
	&dev_attr_host_is_cycmst,
	&dev_attr_host_is_irm,
	&dev_attr_host_is_busmgr,
};


static ssize_t fw_show_drv_device_ids(struct device_driver *drv, char *buf)
{
	struct hpsb_protocol_driver *driver;
	struct ieee1394_device_id *id;
	int length = 0;
	char *scratch = buf;

        driver = container_of(drv, struct hpsb_protocol_driver, driver);

	for (id = driver->id_table; id->match_flags != 0; id++) {
		int need_coma = 0;

		if (id->match_flags & IEEE1394_MATCH_VENDOR_ID) {
			length += sprintf(scratch, "vendor_id=0x%06x", id->vendor_id);
			scratch = buf + length;
			need_coma++;
		}

		if (id->match_flags & IEEE1394_MATCH_MODEL_ID) {
			length += sprintf(scratch, "%smodel_id=0x%06x",
					  need_coma++ ? "," : "",
					  id->model_id);
			scratch = buf + length;
		}

		if (id->match_flags & IEEE1394_MATCH_SPECIFIER_ID) {
			length += sprintf(scratch, "%sspecifier_id=0x%06x",
					  need_coma++ ? "," : "",
					  id->specifier_id);
			scratch = buf + length;
		}

		if (id->match_flags & IEEE1394_MATCH_VERSION) {
			length += sprintf(scratch, "%sversion=0x%06x",
					  need_coma++ ? "," : "",
					  id->version);
			scratch = buf + length;
		}

		if (need_coma) {
			*scratch++ = '\n';
			length++;
		}
	}

	return length;
}
static DRIVER_ATTR(device_ids,S_IRUGO,fw_show_drv_device_ids,NULL);


fw_drv_attr(name, const char *, "%s\n")

static struct driver_attribute *const fw_drv_attrs[] = {
	&driver_attr_drv_name,
	&driver_attr_device_ids,
};


static void nodemgr_create_drv_files(struct hpsb_protocol_driver *driver)
{
	struct device_driver *drv = &driver->driver;
	int i;

	for (i = 0; i < ARRAY_SIZE(fw_drv_attrs); i++)
		driver_create_file(drv, fw_drv_attrs[i]);
}


static void nodemgr_remove_drv_files(struct hpsb_protocol_driver *driver)
{
	struct device_driver *drv = &driver->driver;
	int i;

	for (i = 0; i < ARRAY_SIZE(fw_drv_attrs); i++)
		driver_remove_file(drv, fw_drv_attrs[i]);
}


static void nodemgr_create_ne_dev_files(struct node_entry *ne)
{
	struct device *dev = &ne->device;
	int i;

	for (i = 0; i < ARRAY_SIZE(fw_ne_attrs); i++)
		device_create_file(dev, fw_ne_attrs[i]);
}


static void nodemgr_create_host_dev_files(struct hpsb_host *host)
{
	struct device *dev = &host->device;
	int i;

	for (i = 0; i < ARRAY_SIZE(fw_host_attrs); i++)
		device_create_file(dev, fw_host_attrs[i]);
}


static struct node_entry *find_entry_by_nodeid(struct hpsb_host *host, nodeid_t nodeid);

static void nodemgr_update_host_dev_links(struct hpsb_host *host)
{
	struct device *dev = &host->device;
	struct node_entry *ne;

	sysfs_remove_link(&dev->kobj, "irm_id");
	sysfs_remove_link(&dev->kobj, "busmgr_id");
	sysfs_remove_link(&dev->kobj, "host_id");

	if ((ne = find_entry_by_nodeid(host, host->irm_id)))
		sysfs_create_link(&dev->kobj, &ne->device.kobj, "irm_id");
	if ((ne = find_entry_by_nodeid(host, host->busmgr_id)))
		sysfs_create_link(&dev->kobj, &ne->device.kobj, "busmgr_id");
	if ((ne = find_entry_by_nodeid(host, host->node_id)))
		sysfs_create_link(&dev->kobj, &ne->device.kobj, "host_id");
}

static void nodemgr_create_ud_dev_files(struct unit_directory *ud)
{
	struct device *dev = &ud->device;
	int i;

	for (i = 0; i < ARRAY_SIZE(fw_ud_attrs); i++)
		device_create_file(dev, fw_ud_attrs[i]);

	if (ud->flags & UNIT_DIRECTORY_SPECIFIER_ID)
		device_create_file(dev, &dev_attr_ud_specifier_id);

	if (ud->flags & UNIT_DIRECTORY_VERSION)
		device_create_file(dev, &dev_attr_ud_version);

	if (ud->flags & UNIT_DIRECTORY_VENDOR_ID) {
		device_create_file(dev, &dev_attr_ud_vendor_id);
		if (ud->vendor_name_kv)
			device_create_file(dev, &dev_attr_ud_vendor_name_kv);
	}

	if (ud->flags & UNIT_DIRECTORY_MODEL_ID) {
		device_create_file(dev, &dev_attr_ud_model_id);
		if (ud->model_name_kv)
			device_create_file(dev, &dev_attr_ud_model_name_kv);
	}
}


static int nodemgr_bus_match(struct device * dev, struct device_driver * drv)
{
        struct hpsb_protocol_driver *driver;
        struct unit_directory *ud;
	struct ieee1394_device_id *id;

	/* We only match unit directories */
	if (dev->platform_data != &nodemgr_ud_platform_data)
		return 0;

	ud = container_of(dev, struct unit_directory, device);
	driver = container_of(drv, struct hpsb_protocol_driver, driver);

	if (ud->ne->in_limbo || ud->ignore_driver)
		return 0;

        for (id = driver->id_table; id->match_flags != 0; id++) {
                if ((id->match_flags & IEEE1394_MATCH_VENDOR_ID) &&
                    id->vendor_id != ud->vendor_id)
                        continue;

                if ((id->match_flags & IEEE1394_MATCH_MODEL_ID) &&
                    id->model_id != ud->model_id)
                        continue;

                if ((id->match_flags & IEEE1394_MATCH_SPECIFIER_ID) &&
                    id->specifier_id != ud->specifier_id)
                        continue;

                if ((id->match_flags & IEEE1394_MATCH_VERSION) &&
                    id->version != ud->version)
                        continue;

		return 1;
        }

	return 0;
}


static void nodemgr_remove_uds(struct node_entry *ne)
{
	struct class_device *cdev, *next;
	struct unit_directory *ud;

	list_for_each_entry_safe(cdev, next, &nodemgr_ud_class.children, node) {
		ud = container_of(cdev, struct unit_directory, class_dev);

		if (ud->ne != ne)
			continue;

		class_device_unregister(&ud->class_dev);
		device_unregister(&ud->device);
	}
}


static void nodemgr_remove_ne(struct node_entry *ne)
{
	struct device *dev = &ne->device;

	dev = get_device(&ne->device);
	if (!dev)
		return;

	HPSB_DEBUG("Node removed: ID:BUS[" NODE_BUS_FMT "]  GUID[%016Lx]",
		   NODE_BUS_ARGS(ne->host, ne->nodeid), (unsigned long long)ne->guid);

	nodemgr_remove_uds(ne);

	class_device_unregister(&ne->class_dev);
	device_unregister(dev);

	put_device(dev);
}


static void nodemgr_remove_host_dev(struct device *dev)
{
	struct device *ne_dev, *next;

	list_for_each_entry_safe(ne_dev, next, &dev->children, node)
		nodemgr_remove_ne(container_of(ne_dev, struct node_entry, device));

	sysfs_remove_link(&dev->kobj, "irm_id");
	sysfs_remove_link(&dev->kobj, "busmgr_id");
	sysfs_remove_link(&dev->kobj, "host_id");
}


static void nodemgr_update_bus_options(struct node_entry *ne)
{
#ifdef CONFIG_IEEE1394_VERBOSEDEBUG
	static const u16 mr[] = { 4, 64, 1024, 0};
#endif
	quadlet_t busoptions = be32_to_cpu(ne->csr->bus_info_data[2]);

	ne->busopt.irmc         = (busoptions >> 31) & 1;
	ne->busopt.cmc          = (busoptions >> 30) & 1;
	ne->busopt.isc          = (busoptions >> 29) & 1;
	ne->busopt.bmc          = (busoptions >> 28) & 1;
	ne->busopt.pmc          = (busoptions >> 27) & 1;
	ne->busopt.cyc_clk_acc  = (busoptions >> 16) & 0xff;
	ne->busopt.max_rec      = 1 << (((busoptions >> 12) & 0xf) + 1);
	ne->busopt.max_rom	= (busoptions >> 8) & 0x3;
	ne->busopt.generation   = (busoptions >> 4) & 0xf;
	ne->busopt.lnkspd       = busoptions & 0x7;

	HPSB_VERBOSE("NodeMgr: raw=0x%08x irmc=%d cmc=%d isc=%d bmc=%d pmc=%d "
		     "cyc_clk_acc=%d max_rec=%d max_rom=%d gen=%d lspd=%d",
		     busoptions, ne->busopt.irmc, ne->busopt.cmc,
		     ne->busopt.isc, ne->busopt.bmc, ne->busopt.pmc,
		     ne->busopt.cyc_clk_acc, ne->busopt.max_rec,
		     mr[ne->busopt.max_rom],
		     ne->busopt.generation, ne->busopt.lnkspd);
}


static struct node_entry *nodemgr_create_node(octlet_t guid, struct csr1212_csr *csr,
					      struct host_info *hi, nodeid_t nodeid,
					      unsigned int generation)
{
	struct hpsb_host *host = hi->host;
        struct node_entry *ne;

	ne = kmalloc(sizeof(struct node_entry), GFP_KERNEL);
        if (!ne) return NULL;

	memset(ne, 0, sizeof(struct node_entry));

	ne->tpool = &host->tpool[nodeid & NODE_MASK];

        ne->host = host;
        ne->nodeid = nodeid;
	ne->generation = generation;
	ne->needs_probe = 1;

        ne->guid = guid;
	ne->guid_vendor_id = (guid >> 40) & 0xffffff;
	ne->guid_vendor_oui = nodemgr_find_oui_name(ne->guid_vendor_id);
	ne->csr = csr;

	memcpy(&ne->device, &nodemgr_dev_template_ne,
	       sizeof(ne->device));
	ne->device.parent = &host->device;
	snprintf(ne->device.bus_id, BUS_ID_SIZE, "%016Lx",
		 (unsigned long long)(ne->guid));

	ne->class_dev.dev = &ne->device;
	ne->class_dev.class = &nodemgr_ne_class;
	snprintf(ne->class_dev.class_id, BUS_ID_SIZE, "%016Lx",
		 (unsigned long long)(ne->guid));

	device_register(&ne->device);
	class_device_register(&ne->class_dev);
	get_device(&ne->device);

	if (ne->guid_vendor_oui)
		device_create_file(&ne->device, &dev_attr_ne_guid_vendor_oui);
	nodemgr_create_ne_dev_files(ne);

	nodemgr_update_bus_options(ne);

	HPSB_DEBUG("%s added: ID:BUS[" NODE_BUS_FMT "]  GUID[%016Lx]",
		   (host->node_id == nodeid) ? "Host" : "Node",
		   NODE_BUS_ARGS(host, nodeid), (unsigned long long)guid);

        return ne;
}


static struct node_entry *find_entry_by_guid(u64 guid)
{
	struct class *class = &nodemgr_ne_class;
	struct class_device *cdev;
	struct node_entry *ne, *ret_ne = NULL;

	down_read(&class->subsys.rwsem);
	list_for_each_entry(cdev, &class->children, node) {
		ne = container_of(cdev, struct node_entry, class_dev);

		if (ne->guid == guid) {
			ret_ne = ne;
			break;
		}
	}
	up_read(&class->subsys.rwsem);

        return ret_ne;
}


static struct node_entry *find_entry_by_nodeid(struct hpsb_host *host, nodeid_t nodeid)
{
	struct class *class = &nodemgr_ne_class;
	struct class_device *cdev;
	struct node_entry *ne, *ret_ne = NULL;

	down_read(&class->subsys.rwsem);
	list_for_each_entry(cdev, &class->children, node) {
		ne = container_of(cdev, struct node_entry, class_dev);

		if (ne->host == host && ne->nodeid == nodeid) {
			ret_ne = ne;
			break;
		}
	}
	up_read(&class->subsys.rwsem);

	return ret_ne;
}



/* This implementation currently only scans the config rom and its
 * immediate unit directories looking for software_id and
 * software_version entries, in order to get driver autoloading working. */
static struct unit_directory *nodemgr_process_unit_directory
	(struct host_info *hi, struct node_entry *ne, struct csr1212_keyval *ud_kv,
	 unsigned int *id, struct unit_directory *parent)
{
	struct unit_directory *ud;
	struct unit_directory *ud_temp = NULL;
	struct csr1212_dentry *dentry;
	struct csr1212_keyval *kv;
	u8 last_key_id = 0;

	ud = kmalloc(sizeof(struct unit_directory), GFP_KERNEL);
	if (!ud)
		goto unit_directory_error;

	memset (ud, 0, sizeof(struct unit_directory));

	ud->ne = ne;
	ud->ignore_driver = ignore_drivers;
	ud->address = ud_kv->offset + CSR1212_CONFIG_ROM_SPACE_BASE;
	ud->ud_kv = ud_kv;
	ud->id = (*id)++;

	csr1212_for_each_dir_entry(ne->csr, kv, ud_kv, dentry) {
		switch (kv->key.id) {
		case CSR1212_KV_ID_VENDOR:
			if (kv->key.type == CSR1212_KV_TYPE_IMMEDIATE) {
				ud->vendor_id = kv->value.immediate;
				ud->flags |= UNIT_DIRECTORY_VENDOR_ID;

				if (ud->vendor_id)
					ud->vendor_oui = nodemgr_find_oui_name(ud->vendor_id);
			}
			break;

		case CSR1212_KV_ID_MODEL:
			ud->model_id = kv->value.immediate;
			ud->flags |= UNIT_DIRECTORY_MODEL_ID;
			break;

		case CSR1212_KV_ID_SPECIFIER_ID:
			ud->specifier_id = kv->value.immediate;
			ud->flags |= UNIT_DIRECTORY_SPECIFIER_ID;
			break;

		case CSR1212_KV_ID_VERSION:
			ud->version = kv->value.immediate;
			ud->flags |= UNIT_DIRECTORY_VERSION;
			break;

		case CSR1212_KV_ID_DESCRIPTOR:
			if (kv->key.type == CSR1212_KV_TYPE_LEAF &&
			    CSR1212_DESCRIPTOR_LEAF_TYPE(kv) == 0 &&
			    CSR1212_DESCRIPTOR_LEAF_SPECIFIER_ID(kv) == 0 &&
			    CSR1212_TEXTUAL_DESCRIPTOR_LEAF_WIDTH(kv) == 0 &&
			    CSR1212_TEXTUAL_DESCRIPTOR_LEAF_CHAR_SET(kv) == 0 &&
			    CSR1212_TEXTUAL_DESCRIPTOR_LEAF_LANGUAGE(kv) == 0) {
				switch (last_key_id) {
				case CSR1212_KV_ID_VENDOR:
					ud->vendor_name_kv = kv;
					csr1212_keep_keyval(kv);
					break;

				case CSR1212_KV_ID_MODEL:
					ud->model_name_kv = kv;
					csr1212_keep_keyval(kv);
					break;

				}
			} /* else if (kv->key.type == CSR1212_KV_TYPE_DIRECTORY) ... */
			break;

		case CSR1212_KV_ID_DEPENDENT_INFO:
			if (kv->key.type == CSR1212_KV_TYPE_DIRECTORY) {
				/* This should really be done in SBP2 as this is
				 * doing SBP2 specific parsing. */
				ud->flags |= UNIT_DIRECTORY_HAS_LUN_DIRECTORY;
				ud_temp = nodemgr_process_unit_directory(hi, ne, kv, id,
									 parent);

				if (ud_temp == NULL)
					break;

				/* inherit unspecified values */
				if ((ud->flags & UNIT_DIRECTORY_VENDOR_ID) &&
				    !(ud_temp->flags & UNIT_DIRECTORY_VENDOR_ID))
				{
					ud_temp->flags |=  UNIT_DIRECTORY_VENDOR_ID;
					ud_temp->vendor_id = ud->vendor_id;
					ud_temp->vendor_oui = ud->vendor_oui;
				}
				if ((ud->flags & UNIT_DIRECTORY_MODEL_ID) &&
				    !(ud_temp->flags & UNIT_DIRECTORY_MODEL_ID))
				{
					ud_temp->flags |=  UNIT_DIRECTORY_MODEL_ID;
					ud_temp->model_id = ud->model_id;
				}
				if ((ud->flags & UNIT_DIRECTORY_SPECIFIER_ID) &&
				    !(ud_temp->flags & UNIT_DIRECTORY_SPECIFIER_ID))
				{
					ud_temp->flags |=  UNIT_DIRECTORY_SPECIFIER_ID;
					ud_temp->specifier_id = ud->specifier_id;
				}
				if ((ud->flags & UNIT_DIRECTORY_VERSION) &&
				    !(ud_temp->flags & UNIT_DIRECTORY_VERSION))
				{
					ud_temp->flags |=  UNIT_DIRECTORY_VERSION;
					ud_temp->version = ud->version;
				}
			}

			break;

		default:
			break;
		}
		last_key_id = kv->key.id;
	}

	memcpy(&ud->device, &nodemgr_dev_template_ud,
	       sizeof(ud->device));

	if (parent) {
		ud->flags |= UNIT_DIRECTORY_LUN_DIRECTORY;
		ud->device.parent = &parent->device;
	} else
		ud->device.parent = &ne->device;

	snprintf(ud->device.bus_id, BUS_ID_SIZE, "%s-%u",
		 ne->device.bus_id, ud->id);

	ud->class_dev.dev = &ud->device;
	ud->class_dev.class = &nodemgr_ud_class;
	snprintf(ud->class_dev.class_id, BUS_ID_SIZE, "%s-%u",
		 ne->device.bus_id, ud->id);

	device_register(&ud->device);
	class_device_register(&ud->class_dev);
	get_device(&ud->device);

	if (ud->vendor_oui)
		device_create_file(&ud->device, &dev_attr_ud_vendor_oui);
	nodemgr_create_ud_dev_files(ud);

	return ud;

unit_directory_error:
	if (ud != NULL)
		kfree(ud);
	return NULL;
}


static void nodemgr_process_root_directory(struct host_info *hi, struct node_entry *ne)
{
	unsigned int ud_id = 0;
	struct csr1212_dentry *dentry;
	struct csr1212_keyval *kv;
	u8 last_key_id = 0;

	ne->needs_probe = 0;

	csr1212_for_each_dir_entry(ne->csr, kv, ne->csr->root_kv, dentry) {
		switch (kv->key.id) {
		case CSR1212_KV_ID_VENDOR:
			ne->vendor_id = kv->value.immediate;

			if (ne->vendor_id)
				ne->vendor_oui = nodemgr_find_oui_name(ne->vendor_id);
			break;

		case CSR1212_KV_ID_NODE_CAPABILITIES:
			ne->capabilities = kv->value.immediate;
			break;

		case CSR1212_KV_ID_UNIT:
			nodemgr_process_unit_directory(hi, ne, kv, &ud_id, NULL);
			break;

		case CSR1212_KV_ID_DESCRIPTOR:
			if (last_key_id == CSR1212_KV_ID_VENDOR) {
				if (kv->key.type == CSR1212_KV_TYPE_LEAF &&
				    CSR1212_DESCRIPTOR_LEAF_TYPE(kv) == 0 &&
				    CSR1212_DESCRIPTOR_LEAF_SPECIFIER_ID(kv) == 0 &&
				    CSR1212_TEXTUAL_DESCRIPTOR_LEAF_WIDTH(kv) == 0 &&
				    CSR1212_TEXTUAL_DESCRIPTOR_LEAF_CHAR_SET(kv) == 0 &&
				    CSR1212_TEXTUAL_DESCRIPTOR_LEAF_LANGUAGE(kv) == 0) {
					ne->vendor_name_kv = kv;
					csr1212_keep_keyval(kv);
				}
			}
			break;
		}
		last_key_id = kv->key.id;
	}

	if (ne->vendor_oui)
		device_create_file(&ne->device, &dev_attr_ne_vendor_oui);
	if (ne->vendor_name_kv)
		device_create_file(&ne->device, &dev_attr_ne_vendor_name_kv);
}

#ifdef CONFIG_HOTPLUG

static int nodemgr_hotplug(struct class_device *cdev, char **envp, int num_envp,
			   char *buffer, int buffer_size)
{
	struct unit_directory *ud;
	int i = 0;
	int length = 0;

	if (!cdev)
		return -ENODEV;

	ud = container_of(cdev, struct unit_directory, class_dev);

	if (ud->ne->in_limbo || ud->ignore_driver)
		return -ENODEV;

#define PUT_ENVP(fmt,val) 					\
do {								\
    	int printed;						\
	envp[i++] = buffer;					\
	printed = snprintf(buffer, buffer_size - length,	\
			   fmt, val);				\
	if ((buffer_size - (length+printed) <= 0) || (i >= num_envp))	\
		return -ENOMEM;					\
	length += printed+1;					\
	buffer += printed+1;					\
} while (0)

	PUT_ENVP("VENDOR_ID=%06x", ud->vendor_id);
	PUT_ENVP("MODEL_ID=%06x", ud->model_id);
	PUT_ENVP("GUID=%016Lx", (unsigned long long)ud->ne->guid);
	PUT_ENVP("SPECIFIER_ID=%06x", ud->specifier_id);
	PUT_ENVP("VERSION=%06x", ud->version);

#undef PUT_ENVP

	envp[i] = NULL;

	return 0;
}

#else

static int nodemgr_hotplug(struct class_device *cdev, char **envp, int num_envp,
			   char *buffer, int buffer_size)
{
	return -ENODEV;
}

#endif /* CONFIG_HOTPLUG */


int hpsb_register_protocol(struct hpsb_protocol_driver *driver)
{
	int ret;

	/* This will cause a probe for devices */
	ret = driver_register(&driver->driver);
	if (!ret)
		nodemgr_create_drv_files(driver);

	return ret;
}

void hpsb_unregister_protocol(struct hpsb_protocol_driver *driver)
{
	nodemgr_remove_drv_files(driver);
	/* This will subsequently disconnect all devices that our driver
	 * is attached to. */
	driver_unregister(&driver->driver);
}


/*
 * This function updates nodes that were present on the bus before the
 * reset and still are after the reset.  The nodeid and the config rom
 * may have changed, and the drivers managing this device must be
 * informed that this device just went through a bus reset, to allow
 * the to take whatever actions required.
 */
static void nodemgr_update_node(struct node_entry *ne, struct csr1212_csr *csr,
				struct host_info *hi, nodeid_t nodeid,
				unsigned int generation)
{
	if (ne->nodeid != nodeid) {
		HPSB_DEBUG("Node changed: " NODE_BUS_FMT " -> " NODE_BUS_FMT,
			   NODE_BUS_ARGS(ne->host, ne->nodeid),
			   NODE_BUS_ARGS(ne->host, nodeid));
		ne->nodeid = nodeid;
	}

	if (ne->busopt.generation != ((be32_to_cpu(csr->bus_info_data[2]) >> 4) & 0xf)) {
		kfree(ne->csr->private);
		csr1212_destroy_csr(ne->csr);
		ne->csr = csr;

		/* If the node's configrom generation has changed, we
		 * unregister all the unit directories. */
		nodemgr_remove_uds(ne);

		nodemgr_update_bus_options(ne);

		/* Mark the node as new, so it gets re-probed */
		ne->needs_probe = 1;
	}

	if (ne->in_limbo)
		nodemgr_resume_ne(ne);

	/* Mark the node current */
	ne->generation = generation;
}



static void nodemgr_node_scan_one(struct host_info *hi,
				  nodeid_t nodeid, int generation)
{
	struct hpsb_host *host = hi->host;
	struct node_entry *ne;
	octlet_t guid;
	struct csr1212_csr *csr;
	struct nodemgr_csr_info *ci;

	ci = kmalloc(sizeof(struct nodemgr_csr_info), GFP_KERNEL);
	if (!ci)
		return;

	ci->host = host;
	ci->nodeid = nodeid;
	ci->generation = generation;

	/* We need to detect when the ConfigROM's generation has changed,
	 * so we only update the node's info when it needs to be.  */

	csr = csr1212_create_csr(&nodemgr_csr_ops, 5 * sizeof(quadlet_t), ci);
	if (!csr || csr1212_parse_csr(csr) != CSR1212_SUCCESS) {
		HPSB_ERR("Error parsing configrom for node " NODE_BUS_FMT,
			 NODE_BUS_ARGS(host, nodeid));
		if (csr)
			csr1212_destroy_csr(csr);
		kfree(ci);
		return;
	}

	if (csr->bus_info_data[1] != IEEE1394_BUSID_MAGIC) {
		/* This isn't a 1394 device, but we let it slide. There
		 * was a report of a device with broken firmware which
		 * reported '2394' instead of '1394', which is obviously a
		 * mistake. One would hope that a non-1394 device never
		 * gets connected to Firewire bus. If someone does, we
		 * shouldn't be held responsible, so we'll allow it with a
		 * warning.  */
		HPSB_WARN("Node " NODE_BUS_FMT " has invalid busID magic [0x%08x]",
			  NODE_BUS_ARGS(host, nodeid), csr->bus_info_data[1]);
	}

	guid = ((u64)be32_to_cpu(csr->bus_info_data[3]) << 32) | be32_to_cpu(csr->bus_info_data[4]);
	ne = find_entry_by_guid(guid);

	if (ne && ne->host != host && ne->in_limbo) {
		/* Must have moved this device from one host to another */
		nodemgr_remove_ne(ne);
		ne = NULL;
	}

	if (!ne)
		nodemgr_create_node(guid, csr, hi, nodeid, generation);
	else
		nodemgr_update_node(ne, csr, hi, nodeid, generation);

	return;
}


static void nodemgr_node_scan(struct host_info *hi, int generation)
{
        int count;
        struct hpsb_host *host = hi->host;
        struct selfid *sid = (struct selfid *)host->topology_map;
        nodeid_t nodeid = LOCAL_BUS;

        /* Scan each node on the bus */
        for (count = host->selfid_count; count; count--, sid++) {
                if (sid->extended)
                        continue;

                if (!sid->link_active) {
                        nodeid++;
                        continue;
                }
                nodemgr_node_scan_one(hi, nodeid++, generation);
        }
}


static void nodemgr_suspend_ne(struct node_entry *ne)
{
	struct class_device *cdev;
	struct unit_directory *ud;

	HPSB_DEBUG("Node suspended: ID:BUS[" NODE_BUS_FMT "]  GUID[%016Lx]",
		   NODE_BUS_ARGS(ne->host, ne->nodeid), (unsigned long long)ne->guid);

	ne->in_limbo = 1;
	device_create_file(&ne->device, &dev_attr_ne_in_limbo);

	down_write(&ne->device.bus->subsys.rwsem);
	list_for_each_entry(cdev, &nodemgr_ud_class.children, node) {
		ud = container_of(cdev, struct unit_directory, class_dev);

		if (ud->ne != ne)
			continue;

		if (ud->device.driver &&
		    (!ud->device.driver->suspend ||
		      ud->device.driver->suspend(&ud->device, 0, 0)))
			device_release_driver(&ud->device);
	}
	up_write(&ne->device.bus->subsys.rwsem);
}


static void nodemgr_resume_ne(struct node_entry *ne)
{
	struct class_device *cdev;
	struct unit_directory *ud;

	ne->in_limbo = 0;
	device_remove_file(&ne->device, &dev_attr_ne_in_limbo);

	down_read(&ne->device.bus->subsys.rwsem);
	list_for_each_entry(cdev, &nodemgr_ud_class.children, node) {
		ud = container_of(cdev, struct unit_directory, class_dev);

		if (ud->ne != ne)
			continue;

		if (ud->device.driver && ud->device.driver->resume)
			ud->device.driver->resume(&ud->device, 0);
	}
	up_read(&ne->device.bus->subsys.rwsem);

	HPSB_DEBUG("Node resumed: ID:BUS[" NODE_BUS_FMT "]  GUID[%016Lx]",
		   NODE_BUS_ARGS(ne->host, ne->nodeid), (unsigned long long)ne->guid);
}


static void nodemgr_update_pdrv(struct node_entry *ne)
{
	struct unit_directory *ud;
	struct hpsb_protocol_driver *pdrv;
	struct class *class = &nodemgr_ud_class;
	struct class_device *cdev;

	down_read(&class->subsys.rwsem);
	list_for_each_entry(cdev, &class->children, node) {
		ud = container_of(cdev, struct unit_directory, class_dev);
		if (ud->ne != ne || !ud->device.driver)
			continue;

		pdrv = container_of(ud->device.driver, struct hpsb_protocol_driver, driver);

		if (pdrv->update && pdrv->update(ud)) {
			down_write(&ud->device.bus->subsys.rwsem);
			device_release_driver(&ud->device);
			up_write(&ud->device.bus->subsys.rwsem);
		}
	}
	up_read(&class->subsys.rwsem);
}


static void nodemgr_probe_ne(struct host_info *hi, struct node_entry *ne, int generation)
{
	struct device *dev;

	if (ne->host != hi->host || ne->in_limbo)
		return;

	dev = get_device(&ne->device);
	if (!dev)
		return;

	/* If "needs_probe", then this is either a new or changed node we
	 * rescan totally. If the generation matches for an existing node
	 * (one that existed prior to the bus reset) we send update calls
	 * down to the drivers. Otherwise, this is a dead node and we
	 * suspend it. */
	if (ne->needs_probe)
		nodemgr_process_root_directory(hi, ne);
	else if (ne->generation == generation)
		nodemgr_update_pdrv(ne);
	else
		nodemgr_suspend_ne(ne);

	put_device(dev);
}


static void nodemgr_node_probe(struct host_info *hi, int generation)
{
	struct hpsb_host *host = hi->host;
	struct class *class = &nodemgr_ne_class;
	struct class_device *cdev;

	/* Do some processing of the nodes we've probed. This pulls them
	 * into the sysfs layer if needed, and can result in processing of
	 * unit-directories, or just updating the node and it's
	 * unit-directories. */
	down_read(&class->subsys.rwsem);
	list_for_each_entry(cdev, &class->children, node)
		nodemgr_probe_ne(hi, container_of(cdev, struct node_entry, class_dev), generation);
        up_read(&class->subsys.rwsem);


	/* If we had a bus reset while we were scanning the bus, it is
	 * possible that we did not probe all nodes.  In that case, we
	 * skip the clean up for now, since we could remove nodes that
	 * were still on the bus.  The bus reset increased hi->reset_sem,
	 * so there's a bus scan pending which will do the clean up
	 * eventually.
	 *
	 * Now let's tell the bus to rescan our devices. This may seem
	 * like overhead, but the driver-model core will only scan a
	 * device for a driver when either the device is added, or when a
	 * new driver is added. A bus reset is a good reason to rescan
	 * devices that were there before.  For example, an sbp2 device
	 * may become available for login, if the host that held it was
	 * just removed.  */

	if (generation == get_hpsb_generation(host))
		bus_rescan_devices(&ieee1394_bus_type);

	return;
}

/* Because we are a 1394a-2000 compliant IRM, we need to inform all the other
 * nodes of the broadcast channel.  (Really we're only setting the validity
 * bit). Other IRM responsibilities go in here as well. */
static int nodemgr_do_irm_duties(struct hpsb_host *host, int cycles)
{
	quadlet_t bc;

	/* if irm_id == -1 then there is no IRM on this bus */
	if (!host->is_irm || host->irm_id == (nodeid_t)-1)
		return 1;

	host->csr.broadcast_channel |= 0x40000000;  /* set validity bit */

	bc = cpu_to_be32(host->csr.broadcast_channel);

	hpsb_write(host, LOCAL_BUS | ALL_NODES, get_hpsb_generation(host),
		   (CSR_REGISTER_BASE | CSR_BROADCAST_CHANNEL),
		   &bc, sizeof(quadlet_t));

	/* If there is no bus manager then we should set the root node's
	 * force_root bit to promote bus stability per the 1394
	 * spec. (8.4.2.6) */
	if (host->busmgr_id == 0xffff && host->node_count > 1)
	{
		u16 root_node = host->node_count - 1;
		struct node_entry *ne = find_entry_by_nodeid(host, root_node | LOCAL_BUS);

		if (ne && ne->busopt.cmc)
			hpsb_send_phy_config(host, root_node, -1);
		else {
			HPSB_DEBUG("The root node is not cycle master capable; "
				   "selecting a new root node and resetting...");

			if (cycles >= 5) {
				/* Oh screw it! Just leave the bus as it is */
				HPSB_DEBUG("Stopping reset loop for IRM sanity");
				return 1;
			}

			hpsb_send_phy_config(host, NODEID_TO_NODE(host->node_id), -1);
			hpsb_reset_bus(host, LONG_RESET_FORCE_ROOT);

			return 0;
		}
	}

	return 1;
}

/* We need to ensure that if we are not the IRM, that the IRM node is capable of
 * everything we can do, otherwise issue a bus reset and try to become the IRM
 * ourselves. */
static int nodemgr_check_irm_capability(struct hpsb_host *host, int cycles)
{
	quadlet_t bc;
	int status;

	if (host->is_irm)
		return 1;

	status = hpsb_read(host, LOCAL_BUS | (host->irm_id),
			   get_hpsb_generation(host),
			   (CSR_REGISTER_BASE | CSR_BROADCAST_CHANNEL),
			   &bc, sizeof(quadlet_t));

	if (status < 0 || !(be32_to_cpu(bc) & 0x80000000)) {
		/* The current irm node does not have a valid BROADCAST_CHANNEL
		 * register and we do, so reset the bus with force_root set */
		HPSB_DEBUG("Current remote IRM is not 1394a-2000 compliant, resetting...");

		if (cycles >= 5) {
			/* Oh screw it! Just leave the bus as it is */
			HPSB_DEBUG("Stopping reset loop for IRM sanity");
			return 1;
		}

		hpsb_send_phy_config(host, NODEID_TO_NODE(host->node_id), -1);
		hpsb_reset_bus(host, LONG_RESET_FORCE_ROOT);

		return 0;
	}

	return 1;
}

static int nodemgr_host_thread(void *__hi)
{
	struct host_info *hi = (struct host_info *)__hi;
	struct hpsb_host *host = hi->host;
	int reset_cycles = 0;

	/* No userlevel access needed */
	daemonize(hi->daemon_name);

	/* Setup our device-model entries */
	nodemgr_create_host_dev_files(host);

	/* Sit and wait for a signal to probe the nodes on the bus. This
	 * happens when we get a bus reset. */
	while (1) {
		unsigned int generation = 0;
		int i;

		if (down_interruptible(&hi->reset_sem) ||
		    down_interruptible(&nodemgr_serialize)) {
			if (current->flags & PF_FREEZE) {
				refrigerator(0);
				continue;
			}
			printk("NodeMgr: received unexpected signal?!\n" );
			break;
		}

		if (hi->kill_me)
			break;

		/* Pause for 1/4 second in 1/16 second intervals,
		 * to make sure things settle down. */
		for (i = 0; i < 4 ; i++) {
			set_current_state(TASK_INTERRUPTIBLE);
			if (schedule_timeout(HZ/16)) {
				up(&nodemgr_serialize);
				goto caught_signal;
			}

			/* Now get the generation in which the node ID's we collect
			 * are valid.  During the bus scan we will use this generation
			 * for the read transactions, so that if another reset occurs
			 * during the scan the transactions will fail instead of
			 * returning bogus data. */
			generation = get_hpsb_generation(host);

			/* If we get a reset before we are done waiting, then
			 * start the the waiting over again */
			while (!down_trylock(&hi->reset_sem))
				i = 0;

			/* Check the kill_me again */
			if (hi->kill_me)
				goto caught_signal;
		}

		if (!nodemgr_check_irm_capability(host, reset_cycles)) {
			reset_cycles++;
			up(&nodemgr_serialize);
			continue;
		}

		/* Scan our nodes to get the bus options and create node
		 * entries. This does not do the sysfs stuff, since that
		 * would trigger hotplug callbacks and such, which is a
		 * bad idea at this point. */
		nodemgr_node_scan(hi, generation);
		if (!nodemgr_do_irm_duties(host, reset_cycles)) {
			reset_cycles++;
			up(&nodemgr_serialize);
			continue;
		}

		reset_cycles = 0;

		/* This actually does the full probe, with sysfs
		 * registration. */
		nodemgr_node_probe(hi, generation);

		/* Update some of our sysfs symlinks */
		nodemgr_update_host_dev_links(host);

		up(&nodemgr_serialize);
	}

caught_signal:
	HPSB_VERBOSE("NodeMgr: Exiting thread");

	complete_and_exit(&hi->exited, 0);
}

struct node_entry *hpsb_guid_get_entry(u64 guid)
{
        struct node_entry *ne;

	down(&nodemgr_serialize);
        ne = find_entry_by_guid(guid);
	up(&nodemgr_serialize);

        return ne;
}

struct node_entry *hpsb_nodeid_get_entry(struct hpsb_host *host, nodeid_t nodeid)
{
	struct node_entry *ne;

	down(&nodemgr_serialize);
	ne = find_entry_by_nodeid(host, nodeid);
	up(&nodemgr_serialize);

	return ne;
}


int nodemgr_for_each_host(void *__data, int (*cb)(struct hpsb_host *, void *))
{
	struct class *class = &hpsb_host_class;
	struct class_device *cdev;
	struct hpsb_host *host;
	int error = 0;

	down_read(&class->subsys.rwsem);
	list_for_each_entry(cdev, &class->children, node) {
		host = container_of(cdev, struct hpsb_host, class_dev);

		if ((error = cb(host, __data)))
			break;
	}
	up_read(&class->subsys.rwsem);

	return error;
}

/* The following four convenience functions use a struct node_entry
 * for addressing a node on the bus.  They are intended for use by any
 * process context, not just the nodemgr thread, so we need to be a
 * little careful when reading out the node ID and generation.  The
 * thing that can go wrong is that we get the node ID, then a bus
 * reset occurs, and then we read the generation.  The node ID is
 * possibly invalid, but the generation is current, and we end up
 * sending a packet to a the wrong node.
 *
 * The solution is to make sure we read the generation first, so that
 * if a reset occurs in the process, we end up with a stale generation
 * and the transactions will fail instead of silently using wrong node
 * ID's.
 */

void hpsb_node_fill_packet(struct node_entry *ne, struct hpsb_packet *pkt)
{
        pkt->host = ne->host;
        pkt->generation = ne->generation;
	barrier();
        pkt->node_id = ne->nodeid;
}

int hpsb_node_read(struct node_entry *ne, u64 addr,
		   quadlet_t *buffer, size_t length)
{
	unsigned int generation = ne->generation;

	barrier();
	return hpsb_read(ne->host, ne->nodeid, generation,
			 addr, buffer, length);
}

int hpsb_node_write(struct node_entry *ne, u64 addr,
		    quadlet_t *buffer, size_t length)
{
	unsigned int generation = ne->generation;

	barrier();
	return hpsb_write(ne->host, ne->nodeid, generation,
			  addr, buffer, length);
}

int hpsb_node_lock(struct node_entry *ne, u64 addr,
		   int extcode, quadlet_t *data, quadlet_t arg)
{
	unsigned int generation = ne->generation;

	barrier();
	return hpsb_lock(ne->host, ne->nodeid, generation,
			 addr, extcode, data, arg);
}

static void nodemgr_add_host(struct hpsb_host *host)
{
	struct host_info *hi;

	hi = hpsb_create_hostinfo(&nodemgr_highlevel, host, sizeof(*hi));

	if (!hi) {
		HPSB_ERR ("NodeMgr: out of memory in add host");
		return;
	}

	hi->host = host;
	init_completion(&hi->exited);
        sema_init(&hi->reset_sem, 0);

	sprintf(hi->daemon_name, "knodemgrd_%d", host->id);

	hi->pid = kernel_thread(nodemgr_host_thread, hi, CLONE_KERNEL);

	if (hi->pid < 0) {
		HPSB_ERR ("NodeMgr: failed to start %s thread for %s",
			  hi->daemon_name, host->driver->name);
		hpsb_destroy_hostinfo(&nodemgr_highlevel, host);
		return;
	}

	return;
}

static void nodemgr_host_reset(struct hpsb_host *host)
{
	struct host_info *hi = hpsb_get_hostinfo(&nodemgr_highlevel, host);

	if (hi != NULL) {
		HPSB_VERBOSE("NodeMgr: Processing host reset for %s", hi->daemon_name);
		up(&hi->reset_sem);
	} else
		HPSB_ERR ("NodeMgr: could not process reset of unused host");

	return;
}

static void nodemgr_remove_host(struct hpsb_host *host)
{
	struct host_info *hi = hpsb_get_hostinfo(&nodemgr_highlevel, host);

	if (hi) {
		if (hi->pid >= 0) {
			hi->kill_me = 1;
			mb();
			up(&hi->reset_sem);
			wait_for_completion(&hi->exited);
			nodemgr_remove_host_dev(&host->device);
		}
	} else
		HPSB_ERR("NodeMgr: host %s does not exist, cannot remove",
			 host->driver->name);

	return;
}

static struct hpsb_highlevel nodemgr_highlevel = {
	.name =		"Node manager",
	.add_host =	nodemgr_add_host,
	.host_reset =	nodemgr_host_reset,
	.remove_host =	nodemgr_remove_host,
};

int init_ieee1394_nodemgr(void)
{
	int ret;

	ret = class_register(&nodemgr_ne_class);
	if (ret < 0)
		return ret;

	ret = class_register(&nodemgr_ud_class);
	if (ret < 0) {
		class_unregister(&nodemgr_ne_class);
		return ret;
	}

	hpsb_register_highlevel(&nodemgr_highlevel);

	return 0;
}

void cleanup_ieee1394_nodemgr(void)
{
        hpsb_unregister_highlevel(&nodemgr_highlevel);

	class_unregister(&nodemgr_ud_class);
	class_unregister(&nodemgr_ne_class);
}
