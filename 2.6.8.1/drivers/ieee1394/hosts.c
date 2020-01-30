/*
 * IEEE 1394 for Linux
 *
 * Low level (host adapter) management.
 *
 * Copyright (C) 1999 Andreas E. Bombe
 * Copyright (C) 1999 Emanuel Pirker
 *
 * This code is licensed under the GPL.  See the file COPYING in the root
 * directory of the kernel sources for details.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/timer.h>

#include "csr1212.h"
#include "ieee1394.h"
#include "ieee1394_types.h"
#include "hosts.h"
#include "ieee1394_core.h"
#include "highlevel.h"
#include "nodemgr.h"
#include "csr.h"
#include "config_roms.h"


static void delayed_reset_bus(void * __reset_info)
{
	struct hpsb_host *host = (struct hpsb_host*)__reset_info;
	int generation = host->csr.generation + 1;

	/* The generation field rolls over to 2 rather than 0 per IEEE
	 * 1394a-2000. */
	if (generation > 0xf || generation < 2)
		generation = 2;

	CSR_SET_BUS_INFO_GENERATION(host->csr.rom, generation);
	if (csr1212_generate_csr_image(host->csr.rom) != CSR1212_SUCCESS) {
		/* CSR image creation failed, reset generation field and do not
		 * issue a bus reset. */
		CSR_SET_BUS_INFO_GENERATION(host->csr.rom, host->csr.generation);
		return;
	}

	host->csr.generation = generation;

	host->update_config_rom = 0;
	if (host->driver->set_hw_config_rom)
		host->driver->set_hw_config_rom(host, host->csr.rom->bus_info_data);

	host->csr.gen_timestamp[host->csr.generation] = jiffies;
	hpsb_reset_bus(host, SHORT_RESET);
}

static int dummy_transmit_packet(struct hpsb_host *h, struct hpsb_packet *p)
{
        return 0;
}

static int dummy_devctl(struct hpsb_host *h, enum devctl_cmd c, int arg)
{
        return -1;
}

static int dummy_isoctl(struct hpsb_iso *iso, enum isoctl_cmd command, unsigned long arg)
{
	return -1;
}

static struct hpsb_host_driver dummy_driver = {
        .transmit_packet = dummy_transmit_packet,
        .devctl =          dummy_devctl,
	.isoctl =          dummy_isoctl
};

static int alloc_hostnum_cb(struct hpsb_host *host, void *__data)
{
	int *hostnum = __data;

	if (host->id == *hostnum)
		return 1;

	return 0;
}

/**
 * hpsb_alloc_host - allocate a new host controller.
 * @drv: the driver that will manage the host controller
 * @extra: number of extra bytes to allocate for the driver
 *
 * Allocate a &hpsb_host and initialize the general subsystem specific
 * fields.  If the driver needs to store per host data, as drivers
 * usually do, the amount of memory required can be specified by the
 * @extra parameter.  Once allocated, the driver should initialize the
 * driver specific parts, enable the controller and make it available
 * to the general subsystem using hpsb_add_host().
 *
 * Return Value: a pointer to the &hpsb_host if succesful, %NULL if
 * no memory was available.
 */
static DECLARE_MUTEX(host_num_alloc);

struct hpsb_host *hpsb_alloc_host(struct hpsb_host_driver *drv, size_t extra,
				  struct device *dev)
{
        struct hpsb_host *h;
	int i;
	int hostnum = 0;

        h = kmalloc(sizeof(struct hpsb_host) + extra, SLAB_KERNEL);
        if (!h) return NULL;
        memset(h, 0, sizeof(struct hpsb_host) + extra);

	h->csr.rom = csr1212_create_csr(&csr_bus_ops, CSR_BUS_INFO_SIZE, h);
	if (!h->csr.rom) {
		kfree(h);
		return NULL;
	}

	h->hostdata = h + 1;
        h->driver = drv;

	skb_queue_head_init(&h->pending_packet_queue);
	INIT_LIST_HEAD(&h->addr_space);

	for (i = 2; i < 16; i++)
		h->csr.gen_timestamp[i] = jiffies - 60 * HZ;

	for (i = 0; i < ARRAY_SIZE(h->tpool); i++)
		HPSB_TPOOL_INIT(&h->tpool[i]);

	atomic_set(&h->generation, 0);

	INIT_WORK(&h->delayed_reset, delayed_reset_bus, h);
	
	init_timer(&h->timeout);
	h->timeout.data = (unsigned long) h;
	h->timeout.function = abort_timedouts;
	h->timeout_interval = HZ / 20; // 50ms by default

        h->topology_map = h->csr.topology_map + 3;
        h->speed_map = (u8 *)(h->csr.speed_map + 2);

	down(&host_num_alloc);

	while (nodemgr_for_each_host(&hostnum, alloc_hostnum_cb))
		hostnum++;

	h->id = hostnum;

	memcpy(&h->device, &nodemgr_dev_template_host, sizeof(h->device));
	h->device.parent = dev;
	snprintf(h->device.bus_id, BUS_ID_SIZE, "fw-host%d", h->id);

	h->class_dev.dev = &h->device;
	h->class_dev.class = &hpsb_host_class;
	snprintf(h->class_dev.class_id, BUS_ID_SIZE, "fw-host%d", h->id);

	device_register(&h->device);
	class_device_register(&h->class_dev);
	get_device(&h->device);

	up(&host_num_alloc);

	return h;
}

int hpsb_add_host(struct hpsb_host *host)
{
	if (hpsb_default_host_entry(host))
		return -ENOMEM;

	hpsb_add_extra_config_roms(host);

	highlevel_add_host(host);

	return 0;
}

void hpsb_remove_host(struct hpsb_host *host)
{
        host->is_shutdown = 1;

	cancel_delayed_work(&host->delayed_reset);
	flush_scheduled_work();

        host->driver = &dummy_driver;

        highlevel_remove_host(host);

	hpsb_remove_extra_config_roms(host);

	class_device_unregister(&host->class_dev);
	device_unregister(&host->device);
}

int hpsb_update_config_rom_image(struct hpsb_host *host)
{
	unsigned long reset_delay;
	int next_gen = host->csr.generation + 1;

	if (!host->update_config_rom)
		return -EINVAL;

	if (next_gen > 0xf)
		next_gen = 2;

	/* Stop the delayed interrupt, we're about to change the config rom and
	 * it would be a waste to do a bus reset twice. */
	cancel_delayed_work(&host->delayed_reset);

	/* IEEE 1394a-2000 prohibits using the same generation number
	 * twice in a 60 second period. */
	if (jiffies - host->csr.gen_timestamp[next_gen] < 60 * HZ)
		/* Wait 60 seconds from the last time this generation number was
		 * used. */
		reset_delay = (60 * HZ) + host->csr.gen_timestamp[next_gen] - jiffies;
	else
		/* Wait 1 second in case some other code wants to change the
		 * Config ROM in the near future. */
		reset_delay = HZ;

	PREPARE_WORK(&host->delayed_reset, delayed_reset_bus, host);
	schedule_delayed_work(&host->delayed_reset, reset_delay);

	return 0;
}
