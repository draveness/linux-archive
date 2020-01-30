/*
 * AGPGART driver.
 * Copyright (C) 2002-2004 Dave Jones.
 * Copyright (C) 1999 Jeff Hartmann.
 * Copyright (C) 1999 Precision Insight, Inc.
 * Copyright (C) 1999 Xi Graphics, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * JEFF HARTMANN, OR ANY OTHER CONTRIBUTORS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * TODO:
 * - Allocate more than order 0 pages to avoid too much linear map splitting.
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/miscdevice.h>
#include <linux/pm.h>
#include <linux/agp_backend.h>
#include <linux/vmalloc.h>
#include "agp.h"

__u32 *agp_gatt_table;
int agp_memory_reserved;

/*
 * Needed by the Nforce GART driver for the time being. Would be
 * nice to do this some other way instead of needing this export.
 */
EXPORT_SYMBOL_GPL(agp_memory_reserved);

/*
 * Generic routines for handling agp_memory structures -
 * They use the basic page allocation routines to do the brunt of the work.
 */

void agp_free_key(int key)
{
	if (key < 0)
		return;

	if (key < MAXKEY)
		clear_bit(key, agp_bridge->key_list);
}
EXPORT_SYMBOL(agp_free_key);


static int agp_get_key(void)
{
	int bit;

	bit = find_first_zero_bit(agp_bridge->key_list, MAXKEY);
	if (bit < MAXKEY) {
		set_bit(bit, agp_bridge->key_list);
		return bit;
	}
	return -1;
}


struct agp_memory *agp_create_memory(int scratch_pages)
{
	struct agp_memory *new;

	new = kmalloc(sizeof(struct agp_memory), GFP_KERNEL);

	if (new == NULL)
		return NULL;

	memset(new, 0, sizeof(struct agp_memory));
	new->key = agp_get_key();

	if (new->key < 0) {
		kfree(new);
		return NULL;
	}
	new->memory = vmalloc(PAGE_SIZE * scratch_pages);

	if (new->memory == NULL) {
		agp_free_key(new->key);
		kfree(new);
		return NULL;
	}
	new->num_scratch_pages = scratch_pages;
	return new;
}
EXPORT_SYMBOL(agp_create_memory);

/**
 *	agp_free_memory - free memory associated with an agp_memory pointer.
 *
 *	@curr:		agp_memory pointer to be freed.
 *
 *	It is the only function that can be called when the backend is not owned
 *	by the caller.  (So it can free memory on client death.)
 */
void agp_free_memory(struct agp_memory *curr)
{
	size_t i;

	if ((agp_bridge->type == NOT_SUPPORTED) || (curr == NULL))
		return;

	if (curr->is_bound == TRUE)
		agp_unbind_memory(curr);

	if (curr->type != 0) {
		agp_bridge->driver->free_by_type(curr);
		return;
	}
	if (curr->page_count != 0) {
		for (i = 0; i < curr->page_count; i++) {
			agp_bridge->driver->agp_destroy_page(phys_to_virt(curr->memory[i]));
		}
	}
	agp_free_key(curr->key);
	vfree(curr->memory);
	kfree(curr);
}
EXPORT_SYMBOL(agp_free_memory);

#define ENTRIES_PER_PAGE		(PAGE_SIZE / sizeof(unsigned long))

/**
 *	agp_allocate_memory  -  allocate a group of pages of a certain type.
 *
 *	@page_count:	size_t argument of the number of pages
 *	@type:	u32 argument of the type of memory to be allocated.
 *
 *	Every agp bridge device will allow you to allocate AGP_NORMAL_MEMORY which
 *	maps to physical ram.  Any other type is device dependent.
 *
 *	It returns NULL whenever memory is unavailable.
 */
struct agp_memory *agp_allocate_memory(size_t page_count, u32 type)
{
	int scratch_pages;
	struct agp_memory *new;
	size_t i;

	if (agp_bridge->type == NOT_SUPPORTED)
		return NULL;

	if ((atomic_read(&agp_bridge->current_memory_agp) + page_count) > agp_bridge->max_memory_agp)
		return NULL;

	if (type != 0) {
		new = agp_bridge->driver->alloc_by_type(page_count, type);
		return new;
	}

	scratch_pages = (page_count + ENTRIES_PER_PAGE - 1) / ENTRIES_PER_PAGE;

	new = agp_create_memory(scratch_pages);

	if (new == NULL)
		return NULL;

	for (i = 0; i < page_count; i++) {
		void *addr = agp_bridge->driver->agp_alloc_page();

		if (addr == NULL) {
			agp_free_memory(new);
			return NULL;
		}
		new->memory[i] =
			agp_bridge->driver->mask_memory(virt_to_phys(addr), type);
		new->page_count++;
	}

	flush_agp_mappings();

	return new;
}
EXPORT_SYMBOL(agp_allocate_memory);


/* End - Generic routines for handling agp_memory structures */


static int agp_return_size(void)
{
	int current_size;
	void *temp;

	temp = agp_bridge->current_size;

	switch (agp_bridge->driver->size_type) {
	case U8_APER_SIZE:
		current_size = A_SIZE_8(temp)->size;
		break;
	case U16_APER_SIZE:
		current_size = A_SIZE_16(temp)->size;
		break;
	case U32_APER_SIZE:
		current_size = A_SIZE_32(temp)->size;
		break;
	case LVL2_APER_SIZE:
		current_size = A_SIZE_LVL2(temp)->size;
		break;
	case FIXED_APER_SIZE:
		current_size = A_SIZE_FIX(temp)->size;
		break;
	default:
		current_size = 0;
		break;
	}

	current_size -= (agp_memory_reserved / (1024*1024));
	if (current_size <0)
		current_size = 0;
	return current_size;
}


int agp_num_entries(void)
{
	int num_entries;
	void *temp;

	temp = agp_bridge->current_size;

	switch (agp_bridge->driver->size_type) {
	case U8_APER_SIZE:
		num_entries = A_SIZE_8(temp)->num_entries;
		break;
	case U16_APER_SIZE:
		num_entries = A_SIZE_16(temp)->num_entries;
		break;
	case U32_APER_SIZE:
		num_entries = A_SIZE_32(temp)->num_entries;
		break;
	case LVL2_APER_SIZE:
		num_entries = A_SIZE_LVL2(temp)->num_entries;
		break;
	case FIXED_APER_SIZE:
		num_entries = A_SIZE_FIX(temp)->num_entries;
		break;
	default:
		num_entries = 0;
		break;
	}

	num_entries -= agp_memory_reserved>>PAGE_SHIFT;
	if (num_entries<0)
		num_entries = 0;
	return num_entries;
}
EXPORT_SYMBOL_GPL(agp_num_entries);


/**
 *	agp_copy_info  -  copy bridge state information
 *
 *	@info:		agp_kern_info pointer.  The caller should insure that this pointer is valid. 
 *
 *	This function copies information about the agp bridge device and the state of
 *	the agp backend into an agp_kern_info pointer.
 */
int agp_copy_info(struct agp_kern_info *info)
{
	memset(info, 0, sizeof(struct agp_kern_info));
	if (!agp_bridge || agp_bridge->type == NOT_SUPPORTED ||
	    !agp_bridge->version) {
		info->chipset = NOT_SUPPORTED;
		return -EIO;
	}

	info->version.major = agp_bridge->version->major;
	info->version.minor = agp_bridge->version->minor;
	info->chipset = agp_bridge->type;
	info->device = agp_bridge->dev;
	info->mode = agp_bridge->mode;
	info->aper_base = agp_bridge->gart_bus_addr;
	info->aper_size = agp_return_size();
	info->max_memory = agp_bridge->max_memory_agp;
	info->current_memory = atomic_read(&agp_bridge->current_memory_agp);
	info->cant_use_aperture = agp_bridge->driver->cant_use_aperture;
	info->vm_ops = agp_bridge->vm_ops;
	info->page_mask = ~0UL;
	return 0;
}
EXPORT_SYMBOL(agp_copy_info);


/* End - Routine to copy over information structure */


/*
 * Routines for handling swapping of agp_memory into the GATT -
 * These routines take agp_memory and insert them into the GATT.
 * They call device specific routines to actually write to the GATT.
 */

/**
 *	agp_bind_memory  -  Bind an agp_memory structure into the GATT.
 *
 *	@curr:		agp_memory pointer
 *	@pg_start:	an offset into the graphics aperture translation table
 *
 *	It returns -EINVAL if the pointer == NULL.
 *	It returns -EBUSY if the area of the table requested is already in use.
 */
int agp_bind_memory(struct agp_memory *curr, off_t pg_start)
{
	int ret_val;

	if ((agp_bridge->type == NOT_SUPPORTED) || (curr == NULL))
		return -EINVAL;

	if (curr->is_bound == TRUE) {
		printk (KERN_INFO PFX "memory %p is already bound!\n", curr);
		return -EINVAL;
	}
	if (curr->is_flushed == FALSE) {
		agp_bridge->driver->cache_flush();
		curr->is_flushed = TRUE;
	}
	ret_val = agp_bridge->driver->insert_memory(curr, pg_start, curr->type);

	if (ret_val != 0)
		return ret_val;

	curr->is_bound = TRUE;
	curr->pg_start = pg_start;
	return 0;
}
EXPORT_SYMBOL(agp_bind_memory);


/**
 *	agp_unbind_memory  -  Removes an agp_memory structure from the GATT
 *
 * @curr:	agp_memory pointer to be removed from the GATT.
 *
 * It returns -EINVAL if this piece of agp_memory is not currently bound to
 * the graphics aperture translation table or if the agp_memory pointer == NULL
 */
int agp_unbind_memory(struct agp_memory *curr)
{
	int ret_val;

	if ((agp_bridge->type == NOT_SUPPORTED) || (curr == NULL))
		return -EINVAL;

	if (curr->is_bound != TRUE) {
		printk (KERN_INFO PFX "memory %p was not bound!\n", curr);
		return -EINVAL;
	}

	ret_val = agp_bridge->driver->remove_memory(curr, curr->pg_start, curr->type);

	if (ret_val != 0)
		return ret_val;

	curr->is_bound = FALSE;
	curr->pg_start = 0;
	return 0;
}
EXPORT_SYMBOL(agp_unbind_memory);

/* End - Routines for handling swapping of agp_memory into the GATT */


/* Generic Agp routines - Start */
static void agp_v2_parse_one(u32 *mode, u32 *cmd, u32 *tmp)
{
	/* disable SBA if it's not supported */
	if (!((*cmd & AGPSTAT_SBA) && (*tmp & AGPSTAT_SBA) && (*mode & AGPSTAT_SBA)))
		*cmd &= ~AGPSTAT_SBA;

	/* Set speed */
	if (!((*cmd & AGPSTAT2_4X) && (*tmp & AGPSTAT2_4X) && (*mode & AGPSTAT2_4X)))
		*cmd &= ~AGPSTAT2_4X;

	if (!((*cmd & AGPSTAT2_2X) && (*tmp & AGPSTAT2_2X) && (*mode & AGPSTAT2_2X)))
		*cmd &= ~AGPSTAT2_2X;

	if (!((*cmd & AGPSTAT2_1X) && (*tmp & AGPSTAT2_1X) && (*mode & AGPSTAT2_1X)))
		*cmd &= ~AGPSTAT2_1X;

	/* Now we know what mode it should be, clear out the unwanted bits. */
	if (*cmd & AGPSTAT2_4X)
		*cmd &= ~(AGPSTAT2_1X | AGPSTAT2_2X);	/* 4X */

	if (*cmd & AGPSTAT2_2X)
		*cmd &= ~(AGPSTAT2_1X | AGPSTAT2_4X);	/* 2X */

	if (*cmd & AGPSTAT2_1X)
		*cmd &= ~(AGPSTAT2_2X | AGPSTAT2_4X);	/* 1X */
}

/*
 * mode = requested mode.
 * cmd = PCI_AGP_STATUS from agp bridge.
 * tmp = PCI_AGP_STATUS from graphic card.
 */
static void agp_v3_parse_one(u32 *mode, u32 *cmd, u32 *tmp)
{
	u32 origcmd=*cmd, origtmp=*tmp;

	/* ARQSZ - Set the value to the maximum one.
	 * Don't allow the mode register to override values. */
	*cmd = ((*cmd & ~AGPSTAT_ARQSZ) |
		max_t(u32,(*cmd & AGPSTAT_ARQSZ),(*tmp & AGPSTAT_ARQSZ)));

	/* Calibration cycle.
	 * Don't allow the mode register to override values. */
	*cmd = ((*cmd & ~AGPSTAT_CAL_MASK) |
		min_t(u32,(*cmd & AGPSTAT_CAL_MASK),(*tmp & AGPSTAT_CAL_MASK)));

	/* SBA *must* be supported for AGP v3 */
	*cmd |= AGPSTAT_SBA;

	/*
	 * Set speed.
	 * Check for invalid speeds. This can happen when applications
	 * written before the AGP 3.0 standard pass AGP2.x modes to AGP3 hardware
	 */
	if (*mode & AGPSTAT_MODE_3_0) {
		/*
		 * Caller hasn't a clue what its doing. We are in 3.0 mode,
		 * have been passed a 3.0 mode, but with 2.x speed bits set.
		 * AGP2.x 4x -> AGP3.0 4x.
		 */
		if (*mode & AGPSTAT2_4X) {
			printk (KERN_INFO PFX "%s passes broken AGP3 flags (%x). Fixed.\n",
						current->comm, *mode);
			*mode &= ~AGPSTAT2_4X;
			*mode |= AGPSTAT3_4X;
		}
	} else {
		/*
		 * The caller doesn't know what they are doing. We are in 3.0 mode,
		 * but have been passed an AGP 2.x mode.
		 * Convert AGP 1x,2x,4x -> AGP 3.0 4x.
		 */
		printk (KERN_INFO PFX "%s passes broken AGP2 flags (%x) in AGP3 mode. Fixed.\n",
					current->comm, *mode);
		*mode &= ~(AGPSTAT2_4X | AGPSTAT2_2X | AGPSTAT2_1X);
		*mode |= AGPSTAT3_4X;
	}

	if (*mode & AGPSTAT3_8X) {
		if (!(*cmd & AGPSTAT3_8X)) {
			*cmd &= ~(AGPSTAT3_8X | AGPSTAT3_RSVD);
			*cmd |= AGPSTAT3_4X;
			printk ("%s requested AGPx8 but bridge not capable.\n", current->comm);
			return;
		}
		if (!(*tmp & AGPSTAT3_8X)) {
			*cmd &= ~(AGPSTAT3_8X | AGPSTAT3_RSVD);
			*cmd |= AGPSTAT3_4X;
			printk ("%s requested AGPx8 but graphic card not capable.\n", current->comm);
			return;
		}
		/* All set, bridge & device can do AGP x8*/
		*cmd &= ~(AGPSTAT3_4X | AGPSTAT3_RSVD);
		return;

	} else {

		/*
		 * If we didn't specify AGPx8, we can only do x4.
		 * If the hardware can't do x4, we're up shit creek, and never
		 *  should have got this far.
		 */
		*cmd &= ~(AGPSTAT3_8X | AGPSTAT3_RSVD);
		if ((*cmd & AGPSTAT3_4X) && (*tmp & AGPSTAT3_4X))
			*cmd |= AGPSTAT3_4X;
		else {
			printk (KERN_INFO PFX "Badness. Don't know which AGP mode to set. "
							"[cmd:%x tmp:%x fell back to:- cmd:%x tmp:%x]\n",
							origcmd, origtmp, *cmd, *tmp);
			if (!(*cmd & AGPSTAT3_4X))
				printk (KERN_INFO PFX "Bridge couldn't do AGP x4.\n");
			if (!(*tmp & AGPSTAT3_4X))
				printk (KERN_INFO PFX "Graphic card couldn't do AGP x4.\n");
		}
	}
}

//FIXME: This doesn't smell right.
//We need a function we pass an agp_device to.
u32 agp_collect_device_status(u32 mode, u32 cmd)
{
	struct pci_dev *device = NULL;
	u8 cap_ptr;
	u32 tmp;
	u32 agp3;

	while ((device = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, device)) != NULL) {
		cap_ptr = pci_find_capability(device, PCI_CAP_ID_AGP);
		if (!cap_ptr)
			continue;

		//FIXME: We should probably skip anything here that
		// isn't an AGP graphic card.
		/*
		 * Ok, here we have a AGP device. Disable impossible
		 * settings, and adjust the readqueue to the minimum.
		 */
		pci_read_config_dword(device, cap_ptr+PCI_AGP_STATUS, &tmp);

		/* adjust RQ depth */
		cmd = ((cmd & ~AGPSTAT_RQ_DEPTH) |
		     min_t(u32, (mode & AGPSTAT_RQ_DEPTH),
			 min_t(u32, (cmd & AGPSTAT_RQ_DEPTH), (tmp & AGPSTAT_RQ_DEPTH))));

		/* disable FW if it's not supported */
		if (!((cmd & AGPSTAT_FW) && (tmp & AGPSTAT_FW) && (mode & AGPSTAT_FW)))
			cmd &= ~AGPSTAT_FW;

		/* Check to see if we are operating in 3.0 mode */
		pci_read_config_dword(device, cap_ptr+AGPSTAT, &agp3);
		if (agp3 & AGPSTAT_MODE_3_0) {
			agp_v3_parse_one(&mode, &cmd, &tmp);
		} else {
			agp_v2_parse_one(&mode, &cmd, &tmp);
		}
	}
	return cmd;
}
EXPORT_SYMBOL(agp_collect_device_status);


void agp_device_command(u32 command, int agp_v3)
{
	struct pci_dev *device = NULL;
	int mode;

	mode = command & 0x7;
	if (agp_v3)
		mode *= 4;

	while ((device = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, device)) != NULL) {
		u8 agp = pci_find_capability(device, PCI_CAP_ID_AGP);
		if (!agp)
			continue;

		printk(KERN_INFO PFX "Putting AGP V%d device at %s into %dx mode\n",
				agp_v3 ? 3 : 2, pci_name(device), mode);
		pci_write_config_dword(device, agp + PCI_AGP_COMMAND, command);
	}
}
EXPORT_SYMBOL(agp_device_command);


void get_agp_version(struct agp_bridge_data *bridge)
{
	u32 ncapid;

	/* Exit early if already set by errata workarounds. */
	if (agp_bridge->major_version != 0)
		return;

	pci_read_config_dword(agp_bridge->dev, agp_bridge->capndx, &ncapid);
	agp_bridge->major_version = (ncapid >> AGP_MAJOR_VERSION_SHIFT) & 0xf;
	agp_bridge->minor_version = (ncapid >> AGP_MINOR_VERSION_SHIFT) & 0xf;
}
EXPORT_SYMBOL(get_agp_version);


void agp_generic_enable(u32 mode)
{
	u32 command, temp;
	u32 agp3;

	get_agp_version(agp_bridge);

	printk(KERN_INFO PFX "Found an AGP %d.%d compliant device at %s.\n",
				agp_bridge->major_version,
				agp_bridge->minor_version,
				agp_bridge->dev->slot_name);

	pci_read_config_dword(agp_bridge->dev,
		      agp_bridge->capndx + PCI_AGP_STATUS, &command);

	command = agp_collect_device_status(mode, command);
	command |= AGPSTAT_AGP_ENABLE;

	/* Do AGP version specific frobbing. */
	if(agp_bridge->major_version >= 3) {
		pci_read_config_dword(agp_bridge->dev,
			agp_bridge->capndx+AGPSTAT, &agp3);

		/* Check to see if we are operating in 3.0 mode */
		if (agp3 & AGPSTAT_MODE_3_0) {
			/* If we have 3.5, we can do the isoch stuff. */
			if (agp_bridge->minor_version >= 5)
				agp_3_5_enable(agp_bridge);
			agp_device_command(command, TRUE);
			return;
		} else {
		    /* Disable calibration cycle in RX91<1> when not in AGP3.0 mode of operation.*/
		    command &= ~(7<<10) ;
		    pci_read_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, &temp);
		    temp |= (1<<9);
		    pci_write_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, temp);

		    printk (KERN_INFO PFX "Device is in legacy mode,"
				" falling back to 2.x\n");
		}
	}

	/* AGP v<3 */
	agp_device_command(command, FALSE);
}
EXPORT_SYMBOL(agp_generic_enable);


int agp_generic_create_gatt_table(void)
{
	char *table;
	char *table_end;
	int size;
	int page_order;
	int num_entries;
	int i;
	void *temp;
	struct page *page;

	/* The generic routines can't handle 2 level gatt's */
	if (agp_bridge->driver->size_type == LVL2_APER_SIZE)
		return -EINVAL;

	table = NULL;
	i = agp_bridge->aperture_size_idx;
	temp = agp_bridge->current_size;
	size = page_order = num_entries = 0;

	if (agp_bridge->driver->size_type != FIXED_APER_SIZE) {
		do {
			switch (agp_bridge->driver->size_type) {
			case U8_APER_SIZE:
				size = A_SIZE_8(temp)->size;
				page_order =
				    A_SIZE_8(temp)->page_order;
				num_entries =
				    A_SIZE_8(temp)->num_entries;
				break;
			case U16_APER_SIZE:
				size = A_SIZE_16(temp)->size;
				page_order = A_SIZE_16(temp)->page_order;
				num_entries = A_SIZE_16(temp)->num_entries;
				break;
			case U32_APER_SIZE:
				size = A_SIZE_32(temp)->size;
				page_order = A_SIZE_32(temp)->page_order;
				num_entries = A_SIZE_32(temp)->num_entries;
				break;
				/* This case will never really happen. */
			case FIXED_APER_SIZE:
			case LVL2_APER_SIZE:
			default:
				size = page_order = num_entries = 0;
				break;
			}

			table = (char *) __get_free_pages(GFP_KERNEL,
							  page_order);

			if (table == NULL) {
				i++;
				switch (agp_bridge->driver->size_type) {
				case U8_APER_SIZE:
					agp_bridge->current_size = A_IDX8(agp_bridge);
					break;
				case U16_APER_SIZE:
					agp_bridge->current_size = A_IDX16(agp_bridge);
					break;
				case U32_APER_SIZE:
					agp_bridge->current_size = A_IDX32(agp_bridge);
					break;
					/* This case will never really happen. */
				case FIXED_APER_SIZE:
				case LVL2_APER_SIZE:
				default:
					agp_bridge->current_size =
					    agp_bridge->current_size;
					break;
				}
				temp = agp_bridge->current_size;
			} else {
				agp_bridge->aperture_size_idx = i;
			}
		} while (!table && (i < agp_bridge->driver->num_aperture_sizes));
	} else {
		size = ((struct aper_size_info_fixed *) temp)->size;
		page_order = ((struct aper_size_info_fixed *) temp)->page_order;
		num_entries = ((struct aper_size_info_fixed *) temp)->num_entries;
		table = (char *) __get_free_pages(GFP_KERNEL, page_order);
	}

	if (table == NULL)
		return -ENOMEM;

	table_end = table + ((PAGE_SIZE * (1 << page_order)) - 1);

	for (page = virt_to_page(table); page <= virt_to_page(table_end); page++)
		SetPageReserved(page);

	agp_bridge->gatt_table_real = (u32 *) table;
	agp_gatt_table = (void *)table;

	agp_bridge->driver->cache_flush();
	agp_bridge->gatt_table = ioremap_nocache(virt_to_phys(table),
					(PAGE_SIZE * (1 << page_order)));
	agp_bridge->driver->cache_flush();

	if (agp_bridge->gatt_table == NULL) {
		for (page = virt_to_page(table); page <= virt_to_page(table_end); page++)
			ClearPageReserved(page);

		free_pages((unsigned long) table, page_order);

		return -ENOMEM;
	}
	agp_bridge->gatt_bus_addr = virt_to_phys(agp_bridge->gatt_table_real);

	/* AK: bogus, should encode addresses > 4GB */
	for (i = 0; i < num_entries; i++)
		agp_bridge->gatt_table[i] = (unsigned long) agp_bridge->scratch_page;

	return 0;
}
EXPORT_SYMBOL(agp_generic_create_gatt_table);

int agp_generic_free_gatt_table(void)
{
	int page_order;
	char *table, *table_end;
	void *temp;
	struct page *page;

	temp = agp_bridge->current_size;

	switch (agp_bridge->driver->size_type) {
	case U8_APER_SIZE:
		page_order = A_SIZE_8(temp)->page_order;
		break;
	case U16_APER_SIZE:
		page_order = A_SIZE_16(temp)->page_order;
		break;
	case U32_APER_SIZE:
		page_order = A_SIZE_32(temp)->page_order;
		break;
	case FIXED_APER_SIZE:
		page_order = A_SIZE_FIX(temp)->page_order;
		break;
	case LVL2_APER_SIZE:
		/* The generic routines can't deal with 2 level gatt's */
		return -EINVAL;
		break;
	default:
		page_order = 0;
		break;
	}

	/* Do not worry about freeing memory, because if this is
	 * called, then all agp memory is deallocated and removed
	 * from the table. */

	iounmap(agp_bridge->gatt_table);
	table = (char *) agp_bridge->gatt_table_real;
	table_end = table + ((PAGE_SIZE * (1 << page_order)) - 1);

	for (page = virt_to_page(table); page <= virt_to_page(table_end); page++)
		ClearPageReserved(page);

	free_pages((unsigned long) agp_bridge->gatt_table_real, page_order);

	agp_gatt_table = NULL;
	agp_bridge->gatt_table = NULL;
	agp_bridge->gatt_table_real = NULL;
	agp_bridge->gatt_bus_addr = 0;

	return 0;
}
EXPORT_SYMBOL(agp_generic_free_gatt_table);


int agp_generic_insert_memory(struct agp_memory * mem, off_t pg_start, int type)
{
	int num_entries;
	size_t i;
	off_t j;
	void *temp;

	temp = agp_bridge->current_size;

	switch (agp_bridge->driver->size_type) {
	case U8_APER_SIZE:
		num_entries = A_SIZE_8(temp)->num_entries;
		break;
	case U16_APER_SIZE:
		num_entries = A_SIZE_16(temp)->num_entries;
		break;
	case U32_APER_SIZE:
		num_entries = A_SIZE_32(temp)->num_entries;
		break;
	case FIXED_APER_SIZE:
		num_entries = A_SIZE_FIX(temp)->num_entries;
		break;
	case LVL2_APER_SIZE:
		/* The generic routines can't deal with 2 level gatt's */
		return -EINVAL;
		break;
	default:
		num_entries = 0;
		break;
	}

	num_entries -= agp_memory_reserved/PAGE_SIZE;
	if (num_entries < 0) num_entries = 0;

	if (type != 0 || mem->type != 0) {
		/* The generic routines know nothing of memory types */
		return -EINVAL;
	}

	/* AK: could wrap */
	if ((pg_start + mem->page_count) > num_entries)
		return -EINVAL;

	j = pg_start;

	while (j < (pg_start + mem->page_count)) {
		if (!PGE_EMPTY(agp_bridge, agp_bridge->gatt_table[j])) {
			return -EBUSY;
		}
		j++;
	}

	if (mem->is_flushed == FALSE) {
		agp_bridge->driver->cache_flush();
		mem->is_flushed = TRUE;
	}

	for (i = 0, j = pg_start; i < mem->page_count; i++, j++)
		agp_bridge->gatt_table[j] =
				agp_bridge->driver->mask_memory(
						mem->memory[i], mem->type);

	agp_bridge->driver->tlb_flush(mem);
	return 0;
}
EXPORT_SYMBOL(agp_generic_insert_memory);


int agp_generic_remove_memory(struct agp_memory *mem, off_t pg_start, int type)
{
	size_t i;

	if (type != 0 || mem->type != 0) {
		/* The generic routines know nothing of memory types */
		return -EINVAL;
	}

	/* AK: bogus, should encode addresses > 4GB */
	for (i = pg_start; i < (mem->page_count + pg_start); i++) {
		agp_bridge->gatt_table[i] =
		    (unsigned long) agp_bridge->scratch_page;
	}

	agp_bridge->driver->tlb_flush(mem);
	return 0;
}
EXPORT_SYMBOL(agp_generic_remove_memory);


struct agp_memory *agp_generic_alloc_by_type(size_t page_count, int type)
{
	return NULL;
}
EXPORT_SYMBOL(agp_generic_alloc_by_type);


void agp_generic_free_by_type(struct agp_memory *curr)
{
	if (curr->memory != NULL)
		vfree(curr->memory);

	agp_free_key(curr->key);
	kfree(curr);
}
EXPORT_SYMBOL(agp_generic_free_by_type);


/*
 * Basic Page Allocation Routines -
 * These routines handle page allocation and by default they reserve the allocated
 * memory.  They also handle incrementing the current_memory_agp value, Which is checked
 * against a maximum value.
 */

void *agp_generic_alloc_page(void)
{
	struct page * page;

	page = alloc_page(GFP_KERNEL);
	if (page == NULL)
		return NULL;

	map_page_into_agp(page);

	get_page(page);
	SetPageLocked(page);
	atomic_inc(&agp_bridge->current_memory_agp);
	return page_address(page);
}
EXPORT_SYMBOL(agp_generic_alloc_page);


void agp_generic_destroy_page(void *addr)
{
	struct page *page;

	if (addr == NULL)
		return;

	page = virt_to_page(addr);
	unmap_page_from_agp(page);
	put_page(page);
	unlock_page(page);
	free_page((unsigned long)addr);
	atomic_dec(&agp_bridge->current_memory_agp);
}
EXPORT_SYMBOL(agp_generic_destroy_page);

/* End Basic Page Allocation Routines */


/**
 * agp_enable  -  initialise the agp point-to-point connection.
 *
 * @mode:	agp mode register value to configure with.
 */
void agp_enable(u32 mode)
{
	if (agp_bridge->type == NOT_SUPPORTED)
		return;
	agp_bridge->driver->agp_enable(mode);
}
EXPORT_SYMBOL(agp_enable);


#ifdef CONFIG_SMP
static void ipi_handler(void *null)
{
	flush_agp_cache();
}
#endif

void global_cache_flush(void)
{
#ifdef CONFIG_SMP
	if (on_each_cpu(ipi_handler, NULL, 1, 1) != 0)
		panic(PFX "timed out waiting for the other CPUs!\n");
#else
	flush_agp_cache();
#endif
}
EXPORT_SYMBOL(global_cache_flush);

unsigned long agp_generic_mask_memory(unsigned long addr, int type)
{
	/* memory type is ignored in the generic routine */
	if (agp_bridge->driver->masks)
		return addr | agp_bridge->driver->masks[0].mask;
	else
		return addr;
}
EXPORT_SYMBOL(agp_generic_mask_memory);

/*
 * These functions are implemented according to the AGPv3 spec,
 * which covers implementation details that had previously been
 * left open.
 */

int agp3_generic_fetch_size(void)
{
	u16 temp_size;
	int i;
	struct aper_size_info_16 *values;

	pci_read_config_word(agp_bridge->dev, agp_bridge->capndx+AGPAPSIZE, &temp_size);
	values = A_SIZE_16(agp_bridge->driver->aperture_sizes);

	for (i = 0; i < agp_bridge->driver->num_aperture_sizes; i++) {
		if (temp_size == values[i].size_value) {
			agp_bridge->previous_size =
				agp_bridge->current_size = (void *) (values + i);

			agp_bridge->aperture_size_idx = i;
			return values[i].size;
		}
	}
	return 0;
}
EXPORT_SYMBOL(agp3_generic_fetch_size);

void agp3_generic_tlbflush(struct agp_memory *mem)
{
	u32 ctrl;
	pci_read_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, &ctrl);
	pci_write_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, ctrl & ~AGPCTRL_GTLBEN);
	pci_write_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, ctrl);
}
EXPORT_SYMBOL(agp3_generic_tlbflush);

int agp3_generic_configure(void)
{
	u32 temp;
	struct aper_size_info_16 *current_size;

	current_size = A_SIZE_16(agp_bridge->current_size);

	pci_read_config_dword(agp_bridge->dev, AGP_APBASE, &temp);
	agp_bridge->gart_bus_addr = (temp & PCI_BASE_ADDRESS_MEM_MASK);

	/* set aperture size */
	pci_write_config_word(agp_bridge->dev, agp_bridge->capndx+AGPAPSIZE, current_size->size_value);
	/* set gart pointer */
	pci_write_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPGARTLO, agp_bridge->gatt_bus_addr);
	/* enable aperture and GTLB */
	pci_read_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, &temp);
	pci_write_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, temp | AGPCTRL_APERENB | AGPCTRL_GTLBEN);
	return 0;
}
EXPORT_SYMBOL(agp3_generic_configure);

void agp3_generic_cleanup(void)
{
	u32 ctrl;
	pci_read_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, &ctrl);
	pci_write_config_dword(agp_bridge->dev, agp_bridge->capndx+AGPCTRL, ctrl & ~AGPCTRL_APERENB);
}
EXPORT_SYMBOL(agp3_generic_cleanup);

struct aper_size_info_16 agp3_generic_sizes[AGP_GENERIC_SIZES_ENTRIES] =
{
	{4096, 1048576, 10,0x000},
	{2048,  524288, 9, 0x800},
	{1024,  262144, 8, 0xc00},
	{ 512,  131072, 7, 0xe00},
	{ 256,   65536, 6, 0xf00},
	{ 128,   32768, 5, 0xf20},
	{  64,   16384, 4, 0xf30},
	{  32,    8192, 3, 0xf38},
	{  16,    4096, 2, 0xf3c},
	{   8,    2048, 1, 0xf3e},
	{   4,    1024, 0, 0xf3f}
};
EXPORT_SYMBOL(agp3_generic_sizes);

