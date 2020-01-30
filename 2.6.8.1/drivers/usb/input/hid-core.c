/*
 *  USB HID support for Linux
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2001 Vojtech Pavlik <vojtech@suse.cz>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/spinlock.h>
#include <asm/unaligned.h>
#include <asm/byteorder.h>
#include <linux/input.h>

#undef DEBUG
#undef DEBUG_DATA

#include <linux/usb.h>

#include "hid.h"
#include <linux/hiddev.h>

/*
 * Version Information
 */

#define DRIVER_VERSION "v2.0"
#define DRIVER_AUTHOR "Andreas Gal, Vojtech Pavlik"
#define DRIVER_DESC "USB HID core driver"
#define DRIVER_LICENSE "GPL"

static char *hid_types[] = {"Device", "Pointer", "Mouse", "Device", "Joystick",
				"Gamepad", "Keyboard", "Keypad", "Multi-Axis Controller"};

/*
 * Register a new report for a device.
 */

static struct hid_report *hid_register_report(struct hid_device *device, unsigned type, unsigned id)
{
	struct hid_report_enum *report_enum = device->report_enum + type;
	struct hid_report *report;

	if (report_enum->report_id_hash[id])
		return report_enum->report_id_hash[id];

	if (!(report = kmalloc(sizeof(struct hid_report), GFP_KERNEL)))
		return NULL;
	memset(report, 0, sizeof(struct hid_report));

	if (id != 0)
		report_enum->numbered = 1;

	report->id = id;
	report->type = type;
	report->size = 0;
	report->device = device;
	report_enum->report_id_hash[id] = report;

	list_add_tail(&report->list, &report_enum->report_list);

	return report;
}

/*
 * Register a new field for this report.
 */

static struct hid_field *hid_register_field(struct hid_report *report, unsigned usages, unsigned values)
{
	struct hid_field *field;

	if (report->maxfield == HID_MAX_FIELDS) {
		dbg("too many fields in report");
		return NULL;
	}

	if (!(field = kmalloc(sizeof(struct hid_field) + usages * sizeof(struct hid_usage)
		+ values * sizeof(unsigned), GFP_KERNEL))) return NULL;

	memset(field, 0, sizeof(struct hid_field) + usages * sizeof(struct hid_usage)
		+ values * sizeof(unsigned));

	report->field[report->maxfield++] = field;
	field->usage = (struct hid_usage *)(field + 1);
	field->value = (unsigned *)(field->usage + usages);
	field->report = report;

	return field;
}

/*
 * Open a collection. The type/usage is pushed on the stack.
 */

static int open_collection(struct hid_parser *parser, unsigned type)
{
	struct hid_collection *collection;
	unsigned usage;

	usage = parser->local.usage[0];

	if (parser->collection_stack_ptr == HID_COLLECTION_STACK_SIZE) {
		dbg("collection stack overflow");
		return -1;
	}

	if (parser->device->maxcollection == parser->device->collection_size) {
		collection = kmalloc(sizeof(struct hid_collection) *
				     parser->device->collection_size * 2,
				     GFP_KERNEL);
		if (collection == NULL) {
			dbg("failed to reallocate collection array");
			return -1;
		}
		memcpy(collection, parser->device->collection,
		       sizeof(struct hid_collection) *
		       parser->device->collection_size);
		memset(collection + parser->device->collection_size, 0,
		       sizeof(struct hid_collection) *
		       parser->device->collection_size);
		kfree(parser->device->collection);
		parser->device->collection = collection;
		parser->device->collection_size *= 2;
	}

	parser->collection_stack[parser->collection_stack_ptr++] =
		parser->device->maxcollection;

	collection = parser->device->collection + 
		parser->device->maxcollection++;
	collection->type = type;
	collection->usage = usage;
	collection->level = parser->collection_stack_ptr - 1;
	
	if (type == HID_COLLECTION_APPLICATION)
		parser->device->maxapplication++;

	return 0;
}

/*
 * Close a collection.
 */

static int close_collection(struct hid_parser *parser)
{
	if (!parser->collection_stack_ptr) {
		dbg("collection stack underflow");
		return -1;
	}
	parser->collection_stack_ptr--;
	return 0;
}

/*
 * Climb up the stack, search for the specified collection type
 * and return the usage.
 */

static unsigned hid_lookup_collection(struct hid_parser *parser, unsigned type)
{
	int n;
	for (n = parser->collection_stack_ptr - 1; n >= 0; n--)
		if (parser->device->collection[parser->collection_stack[n]].type == type)
			return parser->device->collection[parser->collection_stack[n]].usage;
	return 0; /* we know nothing about this usage type */
}

/*
 * Add a usage to the temporary parser table.
 */

static int hid_add_usage(struct hid_parser *parser, unsigned usage)
{
	if (parser->local.usage_index >= HID_MAX_USAGES) {
		dbg("usage index exceeded");
		return -1;
	}
	parser->local.usage[parser->local.usage_index] = usage;
	parser->local.collection_index[parser->local.usage_index] =
		parser->collection_stack_ptr ? 
		parser->collection_stack[parser->collection_stack_ptr - 1] : 0;
	parser->local.usage_index++;
	return 0;
}

/*
 * Register a new field for this report.
 */

static int hid_add_field(struct hid_parser *parser, unsigned report_type, unsigned flags)
{
	struct hid_report *report;
	struct hid_field *field;
	int usages;
	unsigned offset;
	int i;

	if (!(report = hid_register_report(parser->device, report_type, parser->global.report_id))) {
		dbg("hid_register_report failed");
		return -1;
	}

	if (parser->global.logical_maximum < parser->global.logical_minimum) {
		dbg("logical range invalid %d %d", parser->global.logical_minimum, parser->global.logical_maximum);
		return -1;
	}
	usages = parser->local.usage_index;

	offset = report->size;
	report->size += parser->global.report_size * parser->global.report_count;

	if (usages < parser->global.report_count)
		usages = parser->global.report_count;

	if (usages == 0)
		return 0; /* ignore padding fields */

	if ((field = hid_register_field(report, usages, parser->global.report_count)) == NULL)
		return 0;

	field->physical = hid_lookup_collection(parser, HID_COLLECTION_PHYSICAL);
	field->logical = hid_lookup_collection(parser, HID_COLLECTION_LOGICAL);
	field->application = hid_lookup_collection(parser, HID_COLLECTION_APPLICATION);

	for (i = 0; i < usages; i++) {
		int j = i;
		/* Duplicate the last usage we parsed if we have excess values */
		if (i >= parser->local.usage_index)
			j = parser->local.usage_index - 1;
		field->usage[i].hid = parser->local.usage[j];
		field->usage[i].collection_index =
			parser->local.collection_index[j];
	}

	field->maxusage = usages;
	field->flags = flags;
	field->report_offset = offset;
	field->report_type = report_type;
	field->report_size = parser->global.report_size;
	field->report_count = parser->global.report_count;
	field->logical_minimum = parser->global.logical_minimum;
	field->logical_maximum = parser->global.logical_maximum;
	field->physical_minimum = parser->global.physical_minimum;
	field->physical_maximum = parser->global.physical_maximum;
	field->unit_exponent = parser->global.unit_exponent;
	field->unit = parser->global.unit;

	return 0;
}

/*
 * Read data value from item.
 */

static __inline__ __u32 item_udata(struct hid_item *item)
{
	switch (item->size) {
		case 1: return item->data.u8;
		case 2: return item->data.u16;
		case 4: return item->data.u32;
	}
	return 0;
}

static __inline__ __s32 item_sdata(struct hid_item *item)
{
	switch (item->size) {
		case 1: return item->data.s8;
		case 2: return item->data.s16;
		case 4: return item->data.s32;
	}
	return 0;
}

/*
 * Process a global item.
 */

static int hid_parser_global(struct hid_parser *parser, struct hid_item *item)
{
	switch (item->tag) {

		case HID_GLOBAL_ITEM_TAG_PUSH:

			if (parser->global_stack_ptr == HID_GLOBAL_STACK_SIZE) {
				dbg("global enviroment stack overflow");
				return -1;
			}

			memcpy(parser->global_stack + parser->global_stack_ptr++,
				&parser->global, sizeof(struct hid_global));
			return 0;

		case HID_GLOBAL_ITEM_TAG_POP:

			if (!parser->global_stack_ptr) {
				dbg("global enviroment stack underflow");
				return -1;
			}

			memcpy(&parser->global, parser->global_stack + --parser->global_stack_ptr,
				sizeof(struct hid_global));
			return 0;

		case HID_GLOBAL_ITEM_TAG_USAGE_PAGE:
			parser->global.usage_page = item_udata(item);
			return 0;

		case HID_GLOBAL_ITEM_TAG_LOGICAL_MINIMUM:
			parser->global.logical_minimum = item_sdata(item);
			return 0;

		case HID_GLOBAL_ITEM_TAG_LOGICAL_MAXIMUM:
			if (parser->global.logical_minimum < 0)
				parser->global.logical_maximum = item_sdata(item);
			else
				parser->global.logical_maximum = item_udata(item);
			return 0;

		case HID_GLOBAL_ITEM_TAG_PHYSICAL_MINIMUM:
			parser->global.physical_minimum = item_sdata(item);
			return 0;

		case HID_GLOBAL_ITEM_TAG_PHYSICAL_MAXIMUM:
			if (parser->global.physical_minimum < 0)
				parser->global.physical_maximum = item_sdata(item);
			else
				parser->global.physical_maximum = item_udata(item);
			return 0;

		case HID_GLOBAL_ITEM_TAG_UNIT_EXPONENT:
			parser->global.unit_exponent = item_sdata(item);
			return 0;

		case HID_GLOBAL_ITEM_TAG_UNIT:
			parser->global.unit = item_udata(item);
			return 0;

		case HID_GLOBAL_ITEM_TAG_REPORT_SIZE:
			if ((parser->global.report_size = item_udata(item)) > 32) {
				dbg("invalid report_size %d", parser->global.report_size);
				return -1;
			}
			return 0;

		case HID_GLOBAL_ITEM_TAG_REPORT_COUNT:
			if ((parser->global.report_count = item_udata(item)) > HID_MAX_USAGES) {
				dbg("invalid report_count %d", parser->global.report_count);
				return -1;
			}
			return 0;

		case HID_GLOBAL_ITEM_TAG_REPORT_ID:
			if ((parser->global.report_id = item_udata(item)) == 0) {
				dbg("report_id 0 is invalid");
				return -1;
			}
			return 0;

		default:
			dbg("unknown global tag 0x%x", item->tag);
			return -1;
	}
}

/*
 * Process a local item.
 */

static int hid_parser_local(struct hid_parser *parser, struct hid_item *item)
{
	__u32 data;
	unsigned n;

	if (item->size == 0) {
		dbg("item data expected for local item");
		return -1;
	}

	data = item_udata(item);

	switch (item->tag) {

		case HID_LOCAL_ITEM_TAG_DELIMITER:

			if (data) {
				/*
				 * We treat items before the first delimiter
				 * as global to all usage sets (branch 0).
				 * In the moment we process only these global
				 * items and the first delimiter set.
				 */
				if (parser->local.delimiter_depth != 0) {
					dbg("nested delimiters");
					return -1;
				}
				parser->local.delimiter_depth++;
				parser->local.delimiter_branch++;
			} else {
				if (parser->local.delimiter_depth < 1) {
					dbg("bogus close delimiter");
					return -1;
				}
				parser->local.delimiter_depth--;
			}
			return 1;

		case HID_LOCAL_ITEM_TAG_USAGE:

			if (parser->local.delimiter_branch > 1) {
				dbg("alternative usage ignored");
				return 0;
			}

			if (item->size <= 2)
				data = (parser->global.usage_page << 16) + data;

			return hid_add_usage(parser, data);

		case HID_LOCAL_ITEM_TAG_USAGE_MINIMUM:

			if (parser->local.delimiter_branch > 1) {
				dbg("alternative usage ignored");
				return 0;
			}

			if (item->size <= 2)
				data = (parser->global.usage_page << 16) + data;

			parser->local.usage_minimum = data;
			return 0;

		case HID_LOCAL_ITEM_TAG_USAGE_MAXIMUM:

			if (parser->local.delimiter_branch > 1) {
				dbg("alternative usage ignored");
				return 0;
			}

			if (item->size <= 2)
				data = (parser->global.usage_page << 16) + data;

			for (n = parser->local.usage_minimum; n <= data; n++)
				if (hid_add_usage(parser, n)) {
					dbg("hid_add_usage failed\n");
					return -1;
				}
			return 0;

		default:

			dbg("unknown local item tag 0x%x", item->tag);
			return 0;
	}
	return 0;
}

/*
 * Process a main item.
 */

static int hid_parser_main(struct hid_parser *parser, struct hid_item *item)
{
	__u32 data;
	int ret;

	data = item_udata(item);

	switch (item->tag) {
		case HID_MAIN_ITEM_TAG_BEGIN_COLLECTION:
			ret = open_collection(parser, data & 0xff);
			break;
		case HID_MAIN_ITEM_TAG_END_COLLECTION:
			ret = close_collection(parser);
			break;
		case HID_MAIN_ITEM_TAG_INPUT:
			ret = hid_add_field(parser, HID_INPUT_REPORT, data);
			break;
		case HID_MAIN_ITEM_TAG_OUTPUT:
			ret = hid_add_field(parser, HID_OUTPUT_REPORT, data);
			break;
		case HID_MAIN_ITEM_TAG_FEATURE:
			ret = hid_add_field(parser, HID_FEATURE_REPORT, data);
			break;
		default:
			dbg("unknown main item tag 0x%x", item->tag);
			ret = 0;
	}

	memset(&parser->local, 0, sizeof(parser->local));	/* Reset the local parser environment */

	return ret;
}

/*
 * Process a reserved item.
 */

static int hid_parser_reserved(struct hid_parser *parser, struct hid_item *item)
{
	dbg("reserved item type, tag 0x%x", item->tag);
	return 0;
}

/*
 * Free a report and all registered fields. The field->usage and
 * field->value table's are allocated behind the field, so we need
 * only to free(field) itself.
 */

static void hid_free_report(struct hid_report *report)
{
	unsigned n;

	for (n = 0; n < report->maxfield; n++)
		kfree(report->field[n]);
	kfree(report);
}

/*
 * Free a device structure, all reports, and all fields.
 */

static void hid_free_device(struct hid_device *device)
{
	unsigned i,j;

	hid_ff_exit(device);

	for (i = 0; i < HID_REPORT_TYPES; i++) {
		struct hid_report_enum *report_enum = device->report_enum + i;

		for (j = 0; j < 256; j++) {
			struct hid_report *report = report_enum->report_id_hash[j];
			if (report)
				hid_free_report(report);
		}
	}

	if (device->rdesc)
		kfree(device->rdesc);
	kfree(device);
}

/*
 * Fetch a report description item from the data stream. We support long
 * items, though they are not used yet.
 */

static u8 *fetch_item(__u8 *start, __u8 *end, struct hid_item *item)
{
	u8 b;

	if ((end - start) <= 0)
		return NULL;

	b = *start++;

	item->type = (b >> 2) & 3;
	item->tag  = (b >> 4) & 15;

	if (item->tag == HID_ITEM_TAG_LONG) {

		item->format = HID_ITEM_FORMAT_LONG;

		if ((end - start) < 2)
			return NULL;

		item->size = *start++;
		item->tag  = *start++;

		if ((end - start) < item->size) 
			return NULL;

		item->data.longdata = start;
		start += item->size;
		return start;
	} 

	item->format = HID_ITEM_FORMAT_SHORT;
	item->size = b & 3;

	switch (item->size) {

		case 0:
			return start;

		case 1:
			if ((end - start) < 1)
				return NULL;
			item->data.u8 = *start++;
			return start;

		case 2:
			if ((end - start) < 2) 
				return NULL;
			item->data.u16 = le16_to_cpu(get_unaligned((__u16*)start));
			start = (__u8 *)((__u16 *)start + 1);
			return start;

		case 3:
			item->size++;
			if ((end - start) < 4)
				return NULL;
			item->data.u32 = le32_to_cpu(get_unaligned((__u32*)start));
			start = (__u8 *)((__u32 *)start + 1);
			return start;
	}

	return NULL;
}

/*
 * Parse a report description into a hid_device structure. Reports are
 * enumerated, fields are attached to these reports.
 */

static struct hid_device *hid_parse_report(__u8 *start, unsigned size)
{
	struct hid_device *device;
	struct hid_parser *parser;
	struct hid_item item;
	__u8 *end;
	unsigned i;
	static int (*dispatch_type[])(struct hid_parser *parser,
				      struct hid_item *item) = {
		hid_parser_main,
		hid_parser_global,
		hid_parser_local,
		hid_parser_reserved
	};

	if (!(device = kmalloc(sizeof(struct hid_device), GFP_KERNEL)))
		return NULL;
	memset(device, 0, sizeof(struct hid_device));

	if (!(device->collection =kmalloc(sizeof(struct hid_collection) *
				   HID_DEFAULT_NUM_COLLECTIONS, GFP_KERNEL))) {
		kfree(device);
		return NULL;
	}
	memset(device->collection, 0, sizeof(struct hid_collection) *
	       HID_DEFAULT_NUM_COLLECTIONS);
	device->collection_size = HID_DEFAULT_NUM_COLLECTIONS;

	for (i = 0; i < HID_REPORT_TYPES; i++)
		INIT_LIST_HEAD(&device->report_enum[i].report_list);

	if (!(device->rdesc = (__u8 *)kmalloc(size, GFP_KERNEL))) {
		kfree(device->collection);
		kfree(device);
		return NULL;
	}
	memcpy(device->rdesc, start, size);
	device->rsize = size;

	if (!(parser = kmalloc(sizeof(struct hid_parser), GFP_KERNEL))) {
		kfree(device->rdesc);
		kfree(device->collection);
		kfree(device);
		return NULL;
	}
	memset(parser, 0, sizeof(struct hid_parser));
	parser->device = device;

	end = start + size;
	while ((start = fetch_item(start, end, &item)) != 0) {

		if (item.format != HID_ITEM_FORMAT_SHORT) {
			dbg("unexpected long global item");
			kfree(device->collection);
			hid_free_device(device);
			kfree(parser);
			return NULL;
		}

		if (dispatch_type[item.type](parser, &item)) {
			dbg("item %u %u %u %u parsing failed\n",
				item.format, (unsigned)item.size, (unsigned)item.type, (unsigned)item.tag);
			kfree(device->collection);
			hid_free_device(device);
			kfree(parser);
			return NULL;
		}

		if (start == end) {
			if (parser->collection_stack_ptr) {
				dbg("unbalanced collection at end of report description");
				kfree(device->collection);
				hid_free_device(device);
				kfree(parser);
				return NULL;
			}
			if (parser->local.delimiter_depth) {
				dbg("unbalanced delimiter at end of report description");
				kfree(device->collection);
				hid_free_device(device);
				kfree(parser);
				return NULL;
			}
			kfree(parser);
			return device;
		}
	}

	dbg("item fetching failed at offset %d\n", (int)(end - start));
	kfree(device->collection);
	hid_free_device(device);
	kfree(parser);
	return NULL;
}

/*
 * Convert a signed n-bit integer to signed 32-bit integer. Common
 * cases are done through the compiler, the screwed things has to be
 * done by hand.
 */

static __inline__ __s32 snto32(__u32 value, unsigned n)
{
	switch (n) {
		case 8:  return ((__s8)value);
		case 16: return ((__s16)value);
		case 32: return ((__s32)value);
	}
	return value & (1 << (n - 1)) ? value | (-1 << n) : value;
}

/*
 * Convert a signed 32-bit integer to a signed n-bit integer.
 */

static __inline__ __u32 s32ton(__s32 value, unsigned n)
{
	__s32 a = value >> (n - 1);
	if (a && a != -1)
		return value < 0 ? 1 << (n - 1) : (1 << (n - 1)) - 1;
	return value & ((1 << n) - 1);
}

/*
 * Extract/implement a data field from/to a report.
 */

static __inline__ __u32 extract(__u8 *report, unsigned offset, unsigned n)
{
	report += (offset >> 5) << 2; offset &= 31;
	return (le64_to_cpu(get_unaligned((__u64*)report)) >> offset) & ((1 << n) - 1);
}

static __inline__ void implement(__u8 *report, unsigned offset, unsigned n, __u32 value)
{
	report += (offset >> 5) << 2; offset &= 31;
	put_unaligned((get_unaligned((__u64*)report)
		& cpu_to_le64(~((((__u64) 1 << n) - 1) << offset)))
		| cpu_to_le64((__u64)value << offset), (__u64*)report);
}

/*
 * Search an array for a value.
 */

static __inline__ int search(__s32 *array, __s32 value, unsigned n)
{
	while (n--) {
		if (*array++ == value)
			return 0;
	}
	return -1;
}

static void hid_process_event(struct hid_device *hid, struct hid_field *field, struct hid_usage *usage, __s32 value, struct pt_regs *regs)
{
	hid_dump_input(usage, value);
	if (hid->claimed & HID_CLAIMED_INPUT)
		hidinput_hid_event(hid, field, usage, value, regs);
	if (hid->claimed & HID_CLAIMED_HIDDEV)
		hiddev_hid_event(hid, field, usage, value, regs);
}

/*
 * Analyse a received field, and fetch the data from it. The field
 * content is stored for next report processing (we do differential
 * reporting to the layer).
 */

static void hid_input_field(struct hid_device *hid, struct hid_field *field, __u8 *data, struct pt_regs *regs)
{
	unsigned n;
	unsigned count = field->report_count;
	unsigned offset = field->report_offset;
	unsigned size = field->report_size;
	__s32 min = field->logical_minimum;
	__s32 max = field->logical_maximum;
	__s32 *value;

	value = kmalloc(sizeof(__s32)*count, GFP_ATOMIC);
	if (!value)
		return;

	for (n = 0; n < count; n++) {

			value[n] = min < 0 ? snto32(extract(data, offset + n * size, size), size) :
						    extract(data, offset + n * size, size);

			if (!(field->flags & HID_MAIN_ITEM_VARIABLE) /* Ignore report if ErrorRollOver */
			    && value[n] >= min && value[n] <= max
			    && field->usage[value[n] - min].hid == HID_UP_KEYBOARD + 1)
				goto exit;
	}

	for (n = 0; n < count; n++) {

		if (HID_MAIN_ITEM_VARIABLE & field->flags) {

			if (field->flags & HID_MAIN_ITEM_RELATIVE) {
				if (!value[n])
					continue;
			} else {
				if (value[n] == field->value[n])
					continue;
			}	
			hid_process_event(hid, field, &field->usage[n], value[n], regs);
			continue;
		}

		if (field->value[n] >= min && field->value[n] <= max
			&& field->usage[field->value[n] - min].hid
			&& search(value, field->value[n], count))
				hid_process_event(hid, field, &field->usage[field->value[n] - min], 0, regs);

		if (value[n] >= min && value[n] <= max
			&& field->usage[value[n] - min].hid
			&& search(field->value, value[n], count))
				hid_process_event(hid, field, &field->usage[value[n] - min], 1, regs);
	}

	memcpy(field->value, value, count * sizeof(__s32));
exit:
	kfree(value);
}

static int hid_input_report(int type, struct urb *urb, struct pt_regs *regs)
{
	struct hid_device *hid = urb->context;
	struct hid_report_enum *report_enum = hid->report_enum + type;
	u8 *data = urb->transfer_buffer;
	int len = urb->actual_length;
	struct hid_report *report;
	int n, size;

	if (!len) {
		dbg("empty report");
		return -1;
	}

#ifdef DEBUG_DATA
	printk(KERN_DEBUG __FILE__ ": report (size %u) (%snumbered)\n", len, report_enum->numbered ? "" : "un");
#endif

	n = 0;				/* Normally report number is 0 */
	if (report_enum->numbered) {	/* Device uses numbered reports, data[0] is report number */
		n = *data++;
		len--;
	}

#ifdef DEBUG_DATA
	{
		int i;
		printk(KERN_DEBUG __FILE__ ": report %d (size %u) = ", n, len);
		for (i = 0; i < len; i++)
			printk(" %02x", data[i]);
		printk("\n");
	}
#endif

	if (!(report = report_enum->report_id_hash[n])) {
		dbg("undefined report_id %d received", n);
		return -1;
	}

	size = ((report->size - 1) >> 3) + 1;

	if (len < size) {
		dbg("report %d is too short, (%d < %d)", report->id, len, size);
		return -1;
	}

	if (hid->claimed & HID_CLAIMED_HIDDEV)
		hiddev_report_event(hid, report);

	for (n = 0; n < report->maxfield; n++)
		hid_input_field(hid, report->field[n], data, regs);

	if (hid->claimed & HID_CLAIMED_INPUT)
		hidinput_report_event(hid, report);

	return 0;
}

/*
 * Input interrupt completion handler.
 */

static void hid_irq_in(struct urb *urb, struct pt_regs *regs)
{
	struct hid_device	*hid = urb->context;
	int			status;

	switch (urb->status) {
	case 0:			/* success */
		hid_input_report(HID_INPUT_REPORT, urb, regs);
		break;
	case -ECONNRESET:	/* unlink */
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	default:		/* error */
		dbg("nonzero status in input irq %d", urb->status);
	}
	
	status = usb_submit_urb (urb, SLAB_ATOMIC);
	if (status)
		err ("can't resubmit intr, %s-%s/input%d, status %d",
				hid->dev->bus->bus_name, hid->dev->devpath,
				hid->ifnum, status);
}

/*
 * Output the field into the report.
 */

static void hid_output_field(struct hid_field *field, __u8 *data)
{
	unsigned count = field->report_count;
	unsigned offset = field->report_offset;
	unsigned size = field->report_size;
	unsigned n;

	for (n = 0; n < count; n++) {
		if (field->logical_minimum < 0)	/* signed values */
			implement(data, offset + n * size, size, s32ton(field->value[n], size));
		 else				/* unsigned values */
			implement(data, offset + n * size, size, field->value[n]);
	}
}

/*
 * Create a report.
 */

static void hid_output_report(struct hid_report *report, __u8 *data)
{
	unsigned n;

	if (report->id > 0)
		*data++ = report->id;

	for (n = 0; n < report->maxfield; n++)
		hid_output_field(report->field[n], data);
}

/*
 * Set a field value. The report this field belongs to has to be
 * created and transferred to the device, to set this value in the
 * device.
 */

int hid_set_field(struct hid_field *field, unsigned offset, __s32 value)
{
	unsigned size = field->report_size;

	hid_dump_input(field->usage + offset, value);

	if (offset >= field->report_count) {
		dbg("offset (%d) exceeds report_count (%d)", offset, field->report_count);
		hid_dump_field(field, 8);
		return -1;
	}
	if (field->logical_minimum < 0) {
		if (value != snto32(s32ton(value, size), size)) {
			dbg("value %d is out of range", value);
			return -1;
		}
	}
	field->value[offset] = value;
	return 0;
}

int hid_find_field(struct hid_device *hid, unsigned int type, unsigned int code, struct hid_field **field)
{
	struct hid_report_enum *report_enum = hid->report_enum + HID_OUTPUT_REPORT;
	struct list_head *list = report_enum->report_list.next;
	int i, j;

	while (list != &report_enum->report_list) {
		struct hid_report *report = (struct hid_report *) list;
		list = list->next;
		for (i = 0; i < report->maxfield; i++) {
			*field = report->field[i];
			for (j = 0; j < (*field)->maxusage; j++)
				if ((*field)->usage[j].type == type && (*field)->usage[j].code == code)
					return j;
		}
	}
	return -1;
}

/*
 * Find a report with a specified HID usage.
 */

int hid_find_report_by_usage(struct hid_device *hid, __u32 wanted_usage, struct hid_report **report, int type)
{
	struct hid_report_enum *report_enum = hid->report_enum + type;
	struct list_head *list = report_enum->report_list.next;
	int i, j;

	while (list != &report_enum->report_list) {
		*report = (struct hid_report *) list;
		list = list->next;
		for (i = 0; i < (*report)->maxfield; i++) {
			struct hid_field *field = (*report)->field[i];
			for (j = 0; j < field->maxusage; j++)
				if (field->logical == wanted_usage)
					return j;
		}
	}
	return -1;
}

#if 0
static int hid_find_field_in_report(struct hid_report *report, __u32 wanted_usage, struct hid_field **field)
{
	int i, j;

	for (i = 0; i < report->maxfield; i++) {
		*field = report->field[i];
		for (j = 0; j < (*field)->maxusage; j++)
			if ((*field)->usage[j].hid == wanted_usage)
				return j;
	}

	return -1;
}
#endif

static int hid_submit_out(struct hid_device *hid)
{
	struct hid_report *report;

	report = hid->out[hid->outtail];

	hid_output_report(report, hid->outbuf);
	hid->urbout->transfer_buffer_length = ((report->size - 1) >> 3) + 1 + (report->id > 0);
	hid->urbout->dev = hid->dev;

	dbg("submitting out urb");

	if (usb_submit_urb(hid->urbout, GFP_ATOMIC)) {
		err("usb_submit_urb(out) failed");
		return -1;
	}

	return 0;
}

static int hid_submit_ctrl(struct hid_device *hid)
{
	struct hid_report *report;
	unsigned char dir;
	int len;

	report = hid->ctrl[hid->ctrltail].report;
	dir = hid->ctrl[hid->ctrltail].dir;

	len = ((report->size - 1) >> 3) + 1 + (report->id > 0);
	if (dir == USB_DIR_OUT) {
		hid_output_report(report, hid->ctrlbuf);
		hid->urbctrl->pipe = usb_sndctrlpipe(hid->dev, 0);
		hid->urbctrl->transfer_buffer_length = len;
	} else {
		int maxpacket, padlen;

		hid->urbctrl->pipe = usb_rcvctrlpipe(hid->dev, 0);
		maxpacket = usb_maxpacket(hid->dev, hid->urbctrl->pipe, 0);
		if (maxpacket > 0) {
			padlen = (len + maxpacket - 1) / maxpacket;
			padlen *= maxpacket;
			if (padlen > HID_BUFFER_SIZE)
				padlen = HID_BUFFER_SIZE;
		} else
			padlen = 0;
		hid->urbctrl->transfer_buffer_length = padlen;
	}
	hid->urbctrl->dev = hid->dev;

	hid->cr->bRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE | dir;
	hid->cr->bRequest = (dir == USB_DIR_OUT) ? HID_REQ_SET_REPORT : HID_REQ_GET_REPORT;
	hid->cr->wValue = cpu_to_le16(((report->type + 1) << 8) | report->id);
	hid->cr->wIndex = cpu_to_le16(hid->ifnum);
	hid->cr->wLength = cpu_to_le16(len);

	dbg("submitting ctrl urb: %s wValue=0x%04x wIndex=0x%04x wLength=%u",
	    hid->cr->bRequest == HID_REQ_SET_REPORT ? "Set_Report" : "Get_Report",
	    hid->cr->wValue, hid->cr->wIndex, hid->cr->wLength);

	if (usb_submit_urb(hid->urbctrl, GFP_ATOMIC)) {
		err("usb_submit_urb(ctrl) failed");
		return -1;
	}

	return 0;
}

/*
 * Output interrupt completion handler.
 */

static void hid_irq_out(struct urb *urb, struct pt_regs *regs)
{
	struct hid_device *hid = urb->context;
	unsigned long flags;

	if (urb->status)
		warn("output irq status %d received", urb->status);

	spin_lock_irqsave(&hid->outlock, flags);

	hid->outtail = (hid->outtail + 1) & (HID_OUTPUT_FIFO_SIZE - 1);

	if (hid->outhead != hid->outtail) {
		hid_submit_out(hid);
		spin_unlock_irqrestore(&hid->outlock, flags);
		return;
	}

	clear_bit(HID_OUT_RUNNING, &hid->iofl);

	spin_unlock_irqrestore(&hid->outlock, flags);

	wake_up(&hid->wait);
}

/*
 * Control pipe completion handler.
 */

static void hid_ctrl(struct urb *urb, struct pt_regs *regs)
{
	struct hid_device *hid = urb->context;
	unsigned long flags;

	if (urb->status)
		warn("ctrl urb status %d received", urb->status);

	spin_lock_irqsave(&hid->ctrllock, flags);

	if (hid->ctrl[hid->ctrltail].dir == USB_DIR_IN) 
		hid_input_report(hid->ctrl[hid->ctrltail].report->type, urb, regs);

	hid->ctrltail = (hid->ctrltail + 1) & (HID_CONTROL_FIFO_SIZE - 1);

	if (hid->ctrlhead != hid->ctrltail) {
		hid_submit_ctrl(hid);
		spin_unlock_irqrestore(&hid->ctrllock, flags);
		return;
	}

	clear_bit(HID_CTRL_RUNNING, &hid->iofl);

	spin_unlock_irqrestore(&hid->ctrllock, flags);

	wake_up(&hid->wait);
}

void hid_submit_report(struct hid_device *hid, struct hid_report *report, unsigned char dir)
{
	int head;
	unsigned long flags;

	if ((hid->quirks & HID_QUIRK_NOGET) && dir == USB_DIR_IN)
		return;

	if (hid->urbout && dir == USB_DIR_OUT && report->type == HID_OUTPUT_REPORT) {

		spin_lock_irqsave(&hid->outlock, flags);

		if ((head = (hid->outhead + 1) & (HID_OUTPUT_FIFO_SIZE - 1)) == hid->outtail) {
			spin_unlock_irqrestore(&hid->outlock, flags);
			warn("output queue full");
			return;
		}

		hid->out[hid->outhead] = report;
		hid->outhead = head;

		if (!test_and_set_bit(HID_OUT_RUNNING, &hid->iofl))
			hid_submit_out(hid);

		spin_unlock_irqrestore(&hid->outlock, flags);
		return;
	}

	spin_lock_irqsave(&hid->ctrllock, flags);

	if ((head = (hid->ctrlhead + 1) & (HID_CONTROL_FIFO_SIZE - 1)) == hid->ctrltail) {
		spin_unlock_irqrestore(&hid->ctrllock, flags);
		warn("control queue full");
		return;
	}

	hid->ctrl[hid->ctrlhead].report = report;
	hid->ctrl[hid->ctrlhead].dir = dir;
	hid->ctrlhead = head;

	if (!test_and_set_bit(HID_CTRL_RUNNING, &hid->iofl))
		hid_submit_ctrl(hid);

	spin_unlock_irqrestore(&hid->ctrllock, flags);
}

int hid_wait_io(struct hid_device *hid)
{
	DECLARE_WAITQUEUE(wait, current);
	int timeout = 10*HZ;

	set_current_state(TASK_UNINTERRUPTIBLE);
	add_wait_queue(&hid->wait, &wait);

	while (timeout && (test_bit(HID_CTRL_RUNNING, &hid->iofl) ||
			   test_bit(HID_OUT_RUNNING, &hid->iofl)))
		timeout = schedule_timeout(timeout);

	set_current_state(TASK_RUNNING);
	remove_wait_queue(&hid->wait, &wait);

	if (!timeout) {
		dbg("timeout waiting for ctrl or out queue to clear");
		return -1;
	}

	return 0;
}

static int hid_get_class_descriptor(struct usb_device *dev, int ifnum,
		unsigned char type, void *buf, int size)
{
	return usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
		USB_REQ_GET_DESCRIPTOR, USB_RECIP_INTERFACE | USB_DIR_IN,
		(type << 8), ifnum, buf, size, HZ * USB_CTRL_GET_TIMEOUT);
}

int hid_open(struct hid_device *hid)
{
	if (hid->open++)
		return 0;

	hid->urbin->dev = hid->dev;

	if (usb_submit_urb(hid->urbin, GFP_KERNEL))
		return -EIO;

	return 0;
}

void hid_close(struct hid_device *hid)
{
	if (!--hid->open)
		usb_unlink_urb(hid->urbin);
}

/*
 * Initialize all reports
 */

void hid_init_reports(struct hid_device *hid)
{
	struct hid_report_enum *report_enum;
	struct hid_report *report;
	struct list_head *list;
	int err, ret;

	/*
	 * The Set_Idle request is supposed to affect only the
	 * "Interrupt In" pipe. Unfortunately, buggy devices such as
	 * the BTC keyboard (ID 046e:5303) the request also affects
	 * Get_Report requests on the control pipe.  In the worst
	 * case, if the device was put on idle for an indefinite
	 * amount of time (as we do below) and there are no input
	 * events to report, the Get_Report requests will just hang
	 * until we get a USB timeout.  To avoid this, we temporarily
	 * establish a minimal idle time of 1ms.  This shouldn't hurt
	 * bugfree devices and will cause a worst-case extra delay of
	 * 1ms for buggy ones.
	 */
	usb_control_msg(hid->dev, usb_sndctrlpipe(hid->dev, 0),
			HID_REQ_SET_IDLE, USB_TYPE_CLASS | USB_RECIP_INTERFACE, (1 << 8),
			hid->ifnum, NULL, 0, HZ * USB_CTRL_SET_TIMEOUT);

	report_enum = hid->report_enum + HID_INPUT_REPORT;
	list = report_enum->report_list.next;
	while (list != &report_enum->report_list) {
		report = (struct hid_report *) list;
		hid_submit_report(hid, report, USB_DIR_IN);
		list = list->next;
	}

	report_enum = hid->report_enum + HID_FEATURE_REPORT;
	list = report_enum->report_list.next;
	while (list != &report_enum->report_list) {
		report = (struct hid_report *) list;
		hid_submit_report(hid, report, USB_DIR_IN);
		list = list->next;
	}

	err = 0;
	ret = hid_wait_io(hid);
	while (ret) {
		err |= ret;
		if (test_bit(HID_CTRL_RUNNING, &hid->iofl))
			usb_unlink_urb(hid->urbctrl);
		if (test_bit(HID_OUT_RUNNING, &hid->iofl))
			usb_unlink_urb(hid->urbout);
		ret = hid_wait_io(hid);
	}

	if (err)
		warn("timeout initializing reports\n");

	report_enum = hid->report_enum + HID_INPUT_REPORT;
	list = report_enum->report_list.next;
	while (list != &report_enum->report_list) {
		report = (struct hid_report *) list;
		usb_control_msg(hid->dev, usb_sndctrlpipe(hid->dev, 0),
			HID_REQ_SET_IDLE, USB_TYPE_CLASS | USB_RECIP_INTERFACE, report->id,
			hid->ifnum, NULL, 0, HZ * USB_CTRL_SET_TIMEOUT);
		list = list->next;
	}
}

#define USB_VENDOR_ID_WACOM		0x056a
#define USB_DEVICE_ID_WACOM_PENPARTNER	0x0000
#define USB_DEVICE_ID_WACOM_GRAPHIRE	0x0010
#define USB_DEVICE_ID_WACOM_INTUOS	0x0020
#define USB_DEVICE_ID_WACOM_PL		0x0030
#define USB_DEVICE_ID_WACOM_INTUOS2	0x0040
#define USB_DEVICE_ID_WACOM_VOLITO      0x0060
#define USB_DEVICE_ID_WACOM_PTU         0x0003

#define USB_VENDOR_ID_KBGEAR            0x084e
#define USB_DEVICE_ID_KBGEAR_JAMSTUDIO  0x1001

#define USB_VENDOR_ID_AIPTEK		0x08ca
#define USB_DEVICE_ID_AIPTEK_01		0x0001
#define USB_DEVICE_ID_AIPTEK_10		0x0010
#define USB_DEVICE_ID_AIPTEK_20		0x0020
#define USB_DEVICE_ID_AIPTEK_21		0x0021
#define USB_DEVICE_ID_AIPTEK_22		0x0022
#define USB_DEVICE_ID_AIPTEK_23		0x0023
#define USB_DEVICE_ID_AIPTEK_24		0x0024

#define USB_VENDOR_ID_GRIFFIN		0x077d
#define USB_DEVICE_ID_POWERMATE		0x0410
#define USB_DEVICE_ID_SOUNDKNOB		0x04AA

#define USB_VENDOR_ID_ATEN             0x0557  
#define USB_DEVICE_ID_ATEN_UC100KM     0x2004
#define USB_DEVICE_ID_ATEN_CS124U      0x2202
#define USB_DEVICE_ID_ATEN_2PORTKVM    0x2204
#define USB_DEVICE_ID_ATEN_4PORTKVM    0x2205
#define USB_DEVICE_ID_ATEN_4PORTKVMC   0x2208

#define USB_VENDOR_ID_TOPMAX           0x0663
#define USB_DEVICE_ID_TOPMAX_COBRAPAD  0x0103

#define USB_VENDOR_ID_HAPP             0x078b
#define USB_DEVICE_ID_UGCI_DRIVING     0x0010
#define USB_DEVICE_ID_UGCI_FLYING      0x0020
#define USB_DEVICE_ID_UGCI_FIGHTING    0x0030

#define USB_VENDOR_ID_MGE              0x0463
#define USB_DEVICE_ID_MGE_UPS          0xffff
#define USB_DEVICE_ID_MGE_UPS1         0x0001

#define USB_VENDOR_ID_ONTRAK		0x0a07
#define USB_DEVICE_ID_ONTRAK_ADU100	0x0064

#define USB_VENDOR_ID_TANGTOP          0x0d3d
#define USB_DEVICE_ID_TANGTOP_USBPS2   0x0001

#define USB_VENDOR_ID_ESSENTIAL_REALITY	0x0d7f
#define USB_DEVICE_ID_ESSENTIAL_REALITY_P5	0x0100

#define USB_VENDOR_ID_A4TECH		0x09DA
#define USB_DEVICE_ID_A4TECH_WCP32PU	0x0006

#define USB_VENDOR_ID_CYPRESS		0x04b4
#define USB_DEVICE_ID_CYPRESS_MOUSE	0x0001

#define USB_VENDOR_ID_BERKSHIRE		0x0c98
#define USB_DEVICE_ID_BERKSHIRE_PCWD	0x1140

#define USB_VENDOR_ID_ALPS		0x0433
#define USB_DEVICE_ID_IBM_GAMEPAD	0x1101

#define USB_VENDOR_ID_SAITEK		0x06a3
#define USB_DEVICE_ID_SAITEK_RUMBLEPAD	0xff17

#define USB_VENDOR_ID_NEC		0x073e
#define USB_DEVICE_ID_NEC_USB_GAME_PAD	0x0301

#define USB_VENDOR_ID_CHIC		0x05fe
#define USB_DEVICE_ID_CHIC_GAMEPAD	0x0014

#define USB_VENDOR_ID_GLAB		0x06c2
#define USB_DEVICE_ID_4_PHIDGETSERVO_30	0x0038
#define USB_DEVICE_ID_1_PHIDGETSERVO_30	0x0039
#define USB_DEVICE_ID_8_8_8_IF_KIT	0x0045
#define USB_DEVICE_ID_0_0_4_IF_KIT	0x0040
#define USB_DEVICE_ID_0_8_8_IF_KIT	0x0053

#define USB_VENDOR_ID_WISEGROUP		0x0925
#define USB_DEVICE_ID_1_PHIDGETSERVO_20	0x8101
#define USB_DEVICE_ID_4_PHIDGETSERVO_20	0x8104

static struct hid_blacklist {
	__u16 idVendor;
	__u16 idProduct;
	unsigned quirks;
} hid_blacklist[] = {

	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_01, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_10, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_20, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_21, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_22, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_23, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_24, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_BERKSHIRE, USB_DEVICE_ID_BERKSHIRE_PCWD, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ESSENTIAL_REALITY, USB_DEVICE_ID_ESSENTIAL_REALITY_P5, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_KBGEAR, USB_DEVICE_ID_KBGEAR_JAMSTUDIO, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GRIFFIN, USB_DEVICE_ID_POWERMATE, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GRIFFIN, USB_DEVICE_ID_SOUNDKNOB, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_MGE, USB_DEVICE_ID_MGE_UPS, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_MGE, USB_DEVICE_ID_MGE_UPS1, HID_QUIRK_IGNORE },

	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 100, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 200, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 300, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 400, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 500, HID_QUIRK_IGNORE },

	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_PENPARTNER, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_GRAPHIRE, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_GRAPHIRE + 1, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_GRAPHIRE + 2, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_GRAPHIRE + 3, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_GRAPHIRE + 4, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS + 1, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS + 2, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS + 3, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS + 4, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_PL, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_PL + 1, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_PL + 2, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_PL + 3, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_PL + 4, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_PL + 5, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS2 + 1, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS2 + 2, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS2 + 3, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS2 + 4, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS2 + 5, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS2 + 7, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_VOLITO, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_PTU, HID_QUIRK_IGNORE },

	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_4_PHIDGETSERVO_30, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_1_PHIDGETSERVO_30, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_8_8_8_IF_KIT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_0_4_IF_KIT, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_8_8_IF_KIT, HID_QUIRK_IGNORE },

	{ USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_4_PHIDGETSERVO_20, HID_QUIRK_IGNORE },
	{ USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_1_PHIDGETSERVO_20, HID_QUIRK_IGNORE },

	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_UC100KM, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_CS124U, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_2PORTKVM, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_4PORTKVM, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_ATEN, USB_DEVICE_ID_ATEN_4PORTKVMC, HID_QUIRK_NOGET },
	{ USB_VENDOR_ID_TANGTOP, USB_DEVICE_ID_TANGTOP_USBPS2, HID_QUIRK_NOGET },

	{ USB_VENDOR_ID_A4TECH, USB_DEVICE_ID_A4TECH_WCP32PU, HID_QUIRK_2WHEEL_MOUSE_HACK_BACK },
	{ USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_MOUSE, HID_QUIRK_2WHEEL_MOUSE_HACK_EXTRA },

	{ USB_VENDOR_ID_ALPS, USB_DEVICE_ID_IBM_GAMEPAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_CHIC, USB_DEVICE_ID_CHIC_GAMEPAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_HAPP, USB_DEVICE_ID_UGCI_DRIVING, HID_QUIRK_BADPAD | HID_QUIRK_MULTI_INPUT },
	{ USB_VENDOR_ID_HAPP, USB_DEVICE_ID_UGCI_FLYING, HID_QUIRK_BADPAD | HID_QUIRK_MULTI_INPUT },
	{ USB_VENDOR_ID_HAPP, USB_DEVICE_ID_UGCI_FIGHTING, HID_QUIRK_BADPAD | HID_QUIRK_MULTI_INPUT },
	{ USB_VENDOR_ID_NEC, USB_DEVICE_ID_NEC_USB_GAME_PAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_SAITEK, USB_DEVICE_ID_SAITEK_RUMBLEPAD, HID_QUIRK_BADPAD },
	{ USB_VENDOR_ID_TOPMAX, USB_DEVICE_ID_TOPMAX_COBRAPAD, HID_QUIRK_BADPAD },

	{ 0, 0 }
};

static int hid_alloc_buffers(struct usb_device *dev, struct hid_device *hid)
{
	if (!(hid->inbuf = usb_buffer_alloc(dev, HID_BUFFER_SIZE, SLAB_ATOMIC, &hid->inbuf_dma)))
		return -1;
	if (!(hid->outbuf = usb_buffer_alloc(dev, HID_BUFFER_SIZE, SLAB_ATOMIC, &hid->outbuf_dma)))
		return -1;
	if (!(hid->cr = usb_buffer_alloc(dev, sizeof(*(hid->cr)), SLAB_ATOMIC, &hid->cr_dma)))
		return -1;
	if (!(hid->ctrlbuf = usb_buffer_alloc(dev, HID_BUFFER_SIZE, SLAB_ATOMIC, &hid->ctrlbuf_dma)))
		return -1;

	return 0;
}

static void hid_free_buffers(struct usb_device *dev, struct hid_device *hid)
{
	if (hid->inbuf)
		usb_buffer_free(dev, HID_BUFFER_SIZE, hid->inbuf, hid->inbuf_dma);
	if (hid->outbuf)
		usb_buffer_free(dev, HID_BUFFER_SIZE, hid->outbuf, hid->outbuf_dma);
	if (hid->cr)
		usb_buffer_free(dev, sizeof(*(hid->cr)), hid->cr, hid->cr_dma);
	if (hid->ctrlbuf)
		usb_buffer_free(dev, HID_BUFFER_SIZE, hid->ctrlbuf, hid->ctrlbuf_dma);
}

static struct hid_device *usb_hid_configure(struct usb_interface *intf)
{
	struct usb_host_interface *interface = intf->cur_altsetting;
	struct usb_device *dev = interface_to_usbdev (intf);
	struct hid_descriptor *hdesc;
	struct hid_device *hid;
	unsigned quirks = 0, rsize = 0;
	char *buf, *rdesc;
	int n;

	for (n = 0; hid_blacklist[n].idVendor; n++)
		if ((hid_blacklist[n].idVendor == dev->descriptor.idVendor) &&
			(hid_blacklist[n].idProduct == dev->descriptor.idProduct))
				quirks = hid_blacklist[n].quirks;

	if (quirks & HID_QUIRK_IGNORE)
		return NULL;

	if (usb_get_extra_descriptor(interface, HID_DT_HID, &hdesc) && ((!interface->desc.bNumEndpoints) ||
		usb_get_extra_descriptor(&interface->endpoint[0], HID_DT_HID, &hdesc))) {
			dbg("class descriptor not present\n");
			return NULL;
	}

	for (n = 0; n < hdesc->bNumDescriptors; n++)
		if (hdesc->desc[n].bDescriptorType == HID_DT_REPORT)
			rsize = le16_to_cpu(hdesc->desc[n].wDescriptorLength);

	if (!rsize || rsize > HID_MAX_DESCRIPTOR_SIZE) {
		dbg("weird size of report descriptor (%u)", rsize);
		return NULL;
	}

	if (!(rdesc = kmalloc(rsize, GFP_KERNEL))) {
		dbg("couldn't allocate rdesc memory");
		return NULL;
	}

	if ((n = hid_get_class_descriptor(dev, interface->desc.bInterfaceNumber, HID_DT_REPORT, rdesc, rsize)) < 0) {
		dbg("reading report descriptor failed");
		kfree(rdesc);
		return NULL;
	}

#ifdef DEBUG_DATA
	printk(KERN_DEBUG __FILE__ ": report descriptor (size %u, read %d) = ", rsize, n);
	for (n = 0; n < rsize; n++)
		printk(" %02x", (unsigned char) rdesc[n]);
	printk("\n");
#endif

	if (!(hid = hid_parse_report(rdesc, rsize))) {
		dbg("parsing report descriptor failed");
		kfree(rdesc);
		return NULL;
	}

	kfree(rdesc);
	hid->quirks = quirks;

	if (hid_alloc_buffers(dev, hid)) {
		hid_free_buffers(dev, hid);
		goto fail;
	}

	for (n = 0; n < interface->desc.bNumEndpoints; n++) {

		struct usb_endpoint_descriptor *endpoint;
		int pipe;

		endpoint = &interface->endpoint[n].desc;
		if ((endpoint->bmAttributes & 3) != 3)		/* Not an interrupt endpoint */
			continue;

		if (endpoint->bEndpointAddress & USB_DIR_IN) {
			int len;

			if (hid->urbin)
				continue;
			if (!(hid->urbin = usb_alloc_urb(0, GFP_KERNEL)))
				goto fail;
			pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
			len = usb_maxpacket(dev, pipe, 0);
			if (len > HID_BUFFER_SIZE)
				len = HID_BUFFER_SIZE;
			usb_fill_int_urb(hid->urbin, dev, pipe, hid->inbuf, len,
					 hid_irq_in, hid, endpoint->bInterval);
			hid->urbin->transfer_dma = hid->inbuf_dma;
			hid->urbin->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		} else {
			if (hid->urbout)
				continue;
			if (!(hid->urbout = usb_alloc_urb(0, GFP_KERNEL)))
				goto fail;
			pipe = usb_sndintpipe(dev, endpoint->bEndpointAddress);
			usb_fill_int_urb(hid->urbout, dev, pipe, hid->outbuf, 0,
					  hid_irq_out, hid, 1);
			hid->urbout->transfer_dma = hid->outbuf_dma;
			hid->urbout->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		}
	}

	if (!hid->urbin) {
		err("couldn't find an input interrupt endpoint");
		goto fail;
	}

	init_waitqueue_head(&hid->wait);
	
	hid->outlock = SPIN_LOCK_UNLOCKED;
	hid->ctrllock = SPIN_LOCK_UNLOCKED;

	hid->version = le16_to_cpu(hdesc->bcdHID);
	hid->country = hdesc->bCountryCode;
	hid->dev = dev;
	hid->intf = intf;
	hid->ifnum = interface->desc.bInterfaceNumber;

	hid->name[0] = 0;

	if (!(buf = kmalloc(64, GFP_KERNEL)))
		goto fail;

	if (usb_string(dev, dev->descriptor.iManufacturer, buf, 64) > 0) {
		strcat(hid->name, buf);
		if (usb_string(dev, dev->descriptor.iProduct, buf, 64) > 0)
			snprintf(hid->name, 64, "%s %s", hid->name, buf);
	} else if (usb_string(dev, dev->descriptor.iProduct, buf, 128) > 0) {
			snprintf(hid->name, 128, "%s", buf);
	} else
		snprintf(hid->name, 128, "%04x:%04x", dev->descriptor.idVendor, dev->descriptor.idProduct);

	usb_make_path(dev, buf, 64);
	snprintf(hid->phys, 64, "%s/input%d", buf,
			intf->altsetting[0].desc.bInterfaceNumber);

	if (usb_string(dev, dev->descriptor.iSerialNumber, hid->uniq, 64) <= 0)
		hid->uniq[0] = 0;

	kfree(buf);

	hid->urbctrl = usb_alloc_urb(0, GFP_KERNEL);
	if (!hid->urbctrl)
		goto fail;
	usb_fill_control_urb(hid->urbctrl, dev, 0, (void *) hid->cr,
			     hid->ctrlbuf, 1, hid_ctrl, hid);
	hid->urbctrl->setup_dma = hid->cr_dma;
	hid->urbctrl->transfer_dma = hid->ctrlbuf_dma;
	hid->urbctrl->transfer_flags |= (URB_NO_TRANSFER_DMA_MAP
				| URB_NO_SETUP_DMA_MAP);

	return hid;

fail:

	if (hid->urbin)
		usb_free_urb(hid->urbin);
	if (hid->urbout)
		usb_free_urb(hid->urbout);
	if (hid->urbctrl)
		usb_free_urb(hid->urbctrl);
	hid_free_buffers(dev, hid);
	hid_free_device(hid);

	return NULL;
}

static void hid_disconnect(struct usb_interface *intf)
{
	struct hid_device *hid = usb_get_intfdata (intf);

	if (!hid)
		return;

	usb_set_intfdata(intf, NULL);
	usb_unlink_urb(hid->urbin);
	usb_unlink_urb(hid->urbout);
	usb_unlink_urb(hid->urbctrl);

	if (hid->claimed & HID_CLAIMED_INPUT)
		hidinput_disconnect(hid);
	if (hid->claimed & HID_CLAIMED_HIDDEV)
		hiddev_disconnect(hid);

	usb_free_urb(hid->urbin);
	usb_free_urb(hid->urbctrl);
	if (hid->urbout)
		usb_free_urb(hid->urbout);

	hid_free_buffers(hid->dev, hid);
	hid_free_device(hid);
}

static int hid_probe (struct usb_interface *intf, const struct usb_device_id *id)
{
	struct hid_device *hid;
	char path[64];
	int i;
	char *c;

	dbg("HID probe called for ifnum %d",
			intf->altsetting->desc.bInterfaceNumber);

	if (!(hid = usb_hid_configure(intf)))
		return -EIO;

	hid_init_reports(hid);
	hid_dump_device(hid);

	if (!hidinput_connect(hid))
		hid->claimed |= HID_CLAIMED_INPUT;
	if (!hiddev_connect(hid))
		hid->claimed |= HID_CLAIMED_HIDDEV;

	usb_set_intfdata(intf, hid);

	if (!hid->claimed) {
		printk ("HID device not claimed by input or hiddev\n");
		hid_disconnect(intf);
		return -EIO;
	}

	printk(KERN_INFO);

	if (hid->claimed & HID_CLAIMED_INPUT)
		printk("input");
	if (hid->claimed == (HID_CLAIMED_INPUT | HID_CLAIMED_HIDDEV))
		printk(",");
	if (hid->claimed & HID_CLAIMED_HIDDEV)
		printk("hiddev%d", hid->minor);

	c = "Device";
	for (i = 0; i < hid->maxcollection; i++) {
		if (hid->collection[i].type == HID_COLLECTION_APPLICATION &&
		    (hid->collection[i].usage & HID_USAGE_PAGE) == HID_UP_GENDESK &&
		    (hid->collection[i].usage & 0xffff) < ARRAY_SIZE(hid_types)) {
			c = hid_types[hid->collection[i].usage & 0xffff];
			break;
		}
	}

	usb_make_path(interface_to_usbdev(intf), path, 63);

	printk(": USB HID v%x.%02x %s [%s] on %s\n",
		hid->version >> 8, hid->version & 0xff, c, hid->name, path);

	return 0;
}

static struct usb_device_id hid_usb_ids [] = {
	{ .match_flags = USB_DEVICE_ID_MATCH_INT_CLASS,
	    .bInterfaceClass = USB_INTERFACE_CLASS_HID },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, hid_usb_ids);

static struct usb_driver hid_driver = {
	.owner =	THIS_MODULE,
	.name =		"usbhid",
	.probe =	hid_probe,
	.disconnect =	hid_disconnect,
	.id_table =	hid_usb_ids,
};

static int __init hid_init(void)
{
	int retval;
	retval = hiddev_init();
	if (retval)
		goto hiddev_init_fail;
	retval = usb_register(&hid_driver);
	if (retval)
		goto usb_register_fail;
	info(DRIVER_VERSION ":" DRIVER_DESC);

	return 0;
usb_register_fail:
	hiddev_exit();
hiddev_init_fail:
	return retval;
}

static void __exit hid_exit(void)
{
	hiddev_exit();
	usb_deregister(&hid_driver);
}

module_init(hid_init);
module_exit(hid_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE(DRIVER_LICENSE);
