/*
 * devices.c
 * (C) Copyright 1999 Randy Dunlap.
 * (C) Copyright 1999,2000 Thomas Sailer <sailer@ife.ee.ethz.ch>. (proc file per device)
 * (C) Copyright 1999 Deti Fliegl (new USB architecture)
 *
 * $id$
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *************************************************************
 *
 * <mountpoint>/devices contains USB topology, device, config, class,
 * interface, & endpoint data.
 *
 * I considered using /proc/bus/usb/devices/device# for each device
 * as it is attached or detached, but I didn't like this for some
 * reason -- maybe it's just too deep of a directory structure.
 * I also don't like looking in multiple places to gather and view
 * the data.  Having only one file for ./devices also prevents race
 * conditions that could arise if a program was reading device info
 * for devices that are being removed (unplugged).  (That is, the
 * program may find a directory for devnum_12 then try to open it,
 * but it was just unplugged, so the directory is now deleted.
 * But programs would just have to be prepared for situations like
 * this in any plug-and-play environment.)
 *
 * 1999-12-16: Thomas Sailer <sailer@ife.ee.ethz.ch>
 *   Converted the whole proc stuff to real
 *   read methods. Now not the whole device list needs to fit
 *   into one page, only the device list for one bus.
 *   Added a poll method to /proc/bus/usb/devices, to wake
 *   up an eventual usbd
 * 2000-01-04: Thomas Sailer <sailer@ife.ee.ethz.ch>
 *   Turned into its own filesystem
 * 2000-07-05: Ashley Montanaro <ashley@compsoc.man.ac.uk>
 *   Converted file reading routine to dump to buffer once
 *   per device, not per bus
 *
 * $Id: devices.c,v 1.5 2000/01/11 13:58:21 tom Exp $
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/usb.h>
#include <linux/smp_lock.h>
#include <linux/usbdevice_fs.h>
#include <asm/uaccess.h>

#define MAX_TOPO_LEVEL		6

/* Define ALLOW_SERIAL_NUMBER if you want to see the serial number of devices */
#define ALLOW_SERIAL_NUMBER

static char *format_topo =
/* T:  Bus=dd Lev=dd Prnt=dd Port=dd Cnt=dd Dev#=ddd Spd=ddd MxCh=dd */
  "T:  Bus=%2.2d Lev=%2.2d Prnt=%2.2d Port=%2.2d Cnt=%2.2d Dev#=%3d Spd=%3s MxCh=%2d\n";

static char *format_string_manufacturer =
/* S:  Manufacturer=xxxx */
  "S:  Manufacturer=%.100s\n";

static char *format_string_product =
/* S:  Product=xxxx */
  "S:  Product=%.100s\n";

#ifdef ALLOW_SERIAL_NUMBER
static char *format_string_serialnumber =
/* S:  SerialNumber=xxxx */
  "S:  SerialNumber=%.100s\n";
#endif

static char *format_bandwidth =
/* B:  Alloc=ddd/ddd us (xx%), #Int=ddd, #Iso=ddd */
  "B:  Alloc=%3d/%3d us (%2d%%), #Int=%3d, #Iso=%3d\n";
  
static char *format_device1 =
/* D:  Ver=xx.xx Cls=xx(sssss) Sub=xx Prot=xx MxPS=dd #Cfgs=dd */
  "D:  Ver=%2x.%02x Cls=%02x(%-5s) Sub=%02x Prot=%02x MxPS=%2d #Cfgs=%3d\n";

static char *format_device2 =
/* P:  Vendor=xxxx ProdID=xxxx Rev=xx.xx */
  "P:  Vendor=%04x ProdID=%04x Rev=%2x.%02x\n";

static char *format_config =
/* C:  #Ifs=dd Cfg#=dd Atr=xx MPwr=dddmA */
  "C:%c #Ifs=%2d Cfg#=%2d Atr=%02x MxPwr=%3dmA\n";
  
static char *format_iface =
/* I:  If#=dd Alt=dd #EPs=dd Cls=xx(sssss) Sub=xx Prot=xx Driver=xxxx*/
  "I:  If#=%2d Alt=%2d #EPs=%2d Cls=%02x(%-5s) Sub=%02x Prot=%02x Driver=%s\n";

static char *format_endpt =
/* E:  Ad=xx(s) Atr=xx(ssss) MxPS=dddd Ivl=dddms */
  "E:  Ad=%02x(%c) Atr=%02x(%-4s) MxPS=%4d Ivl=%3dms\n";


/*
 * Need access to the driver and USB bus lists.
 * extern struct list_head usb_driver_list;
 * extern struct list_head usb_bus_list;
 * However, these will come from functions that return ptrs to each of them.
 */

static DECLARE_WAIT_QUEUE_HEAD(deviceconndiscwq);
static unsigned int conndiscevcnt = 0;

/* this struct stores the poll state for <mountpoint>/devices pollers */
struct usb_device_status {
	unsigned int lastev;
};

struct class_info {
	int class;
	char *class_name;
};

static const struct class_info clas_info[] =
{					/* max. 5 chars. per name string */
	{USB_CLASS_PER_INTERFACE,	">ifc"},
	{USB_CLASS_AUDIO,		"audio"},
	{USB_CLASS_COMM,		"comm."},
	{USB_CLASS_HID,			"HID"},
	{USB_CLASS_HUB,			"hub"},
	{USB_CLASS_PHYSICAL,		"PID"},
	{USB_CLASS_PRINTER,		"print"},
	{USB_CLASS_MASS_STORAGE,	"stor."},
	{USB_CLASS_DATA,		"data"},
	{USB_CLASS_APP_SPEC,		"app."},
	{USB_CLASS_VENDOR_SPEC,		"vend."},
	{-1,				"unk."}		/* leave as last */
};

/*****************************************************************/

void usbdevfs_conn_disc_event(void)
{
	wake_up(&deviceconndiscwq);
	conndiscevcnt++;
}

static const char *class_decode(const int class)
{
	int ix;

	for (ix = 0; clas_info[ix].class != -1; ix++)
		if (clas_info[ix].class == class)
			break;
	return (clas_info[ix].class_name);
}

static char *usb_dump_endpoint_descriptor(char *start, char *end, const struct usb_endpoint_descriptor *desc)
{
	char *EndpointType [4] = {"Ctrl", "Isoc", "Bulk", "Int."};

	if (start > end)
		return start;
	start += sprintf(start, format_endpt, desc->bEndpointAddress,
			 (desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_CONTROL
			 	? 'B' :			/* bidirectional */
			 (desc->bEndpointAddress & USB_DIR_IN) ? 'I' : 'O',
			 desc->bmAttributes, EndpointType[desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK],
			 desc->wMaxPacketSize, desc->bInterval);
	return start;
}

static char *usb_dump_endpoint(char *start, char *end, const struct usb_endpoint_descriptor *endpoint)
{
	return usb_dump_endpoint_descriptor(start, end, endpoint);
}

static char *usb_dump_interface_descriptor(char *start, char *end, const struct usb_interface *iface, int setno)
{
	struct usb_interface_descriptor *desc = &iface->altsetting[setno];

	if (start > end)
		return start;
	start += sprintf(start, format_iface,
			 desc->bInterfaceNumber,
			 desc->bAlternateSetting,
			 desc->bNumEndpoints,
			 desc->bInterfaceClass,
			 class_decode(desc->bInterfaceClass),
			 desc->bInterfaceSubClass,
			 desc->bInterfaceProtocol,
			 iface->driver ? iface->driver->name : "(none)");
	return start;
}

static char *usb_dump_interface(char *start, char *end, const struct usb_interface *iface, int setno)
{
	struct usb_interface_descriptor *desc = &iface->altsetting[setno];
	int i;

	start = usb_dump_interface_descriptor(start, end, iface, setno);
	for (i = 0; i < desc->bNumEndpoints; i++) {
		if (start > end)
			return start;
		start = usb_dump_endpoint(start, end, desc->endpoint + i);
	}
	return start;
}

/* TBD:
 * 0. TBDs
 * 1. marking active config and ifaces (code lists all, but should mark
 *    which ones are active, if any)
 * 2. add <halted> status to each endpoint line
 */

static char *usb_dump_config_descriptor(char *start, char *end, const struct usb_config_descriptor *desc, int active)
{
	if (start > end)
		return start;
	start += sprintf(start, format_config,
			 active ? '*' : ' ',	/* mark active/actual/current cfg. */
			 desc->bNumInterfaces,
			 desc->bConfigurationValue,
			 desc->bmAttributes,
			 desc->MaxPower * 2);
	return start;
}

static char *usb_dump_config(char *start, char *end, const struct usb_config_descriptor *config, int active)
{
	int i, j;
	struct usb_interface *interface;

	if (start > end)
		return start;
	if (!config)		/* getting these some in 2.3.7; none in 2.3.6 */
		return start + sprintf(start, "(null Cfg. desc.)\n");
	start = usb_dump_config_descriptor(start, end, config, active);
	for (i = 0; i < config->bNumInterfaces; i++) {
		interface = config->interface + i;
		if (!interface)
			break;
		for (j = 0; j < interface->num_altsetting; j++) {
			if (start > end)
				return start;
			start = usb_dump_interface(start, end, interface, j);
		}
	}
	return start;
}

/*
 * Dump the different USB descriptors.
 */
static char *usb_dump_device_descriptor(char *start, char *end, const struct usb_device_descriptor *desc)
{
	if (start > end)
		return start;
	start += sprintf (start, format_device1,
			  desc->bcdUSB >> 8, desc->bcdUSB & 0xff,
			  desc->bDeviceClass,
			  class_decode (desc->bDeviceClass),
			  desc->bDeviceSubClass,
			  desc->bDeviceProtocol,
			  desc->bMaxPacketSize0,
			  desc->bNumConfigurations);
	if (start > end)
		return start;
	start += sprintf(start, format_device2,
			 desc->idVendor, desc->idProduct,
			 desc->bcdDevice >> 8, desc->bcdDevice & 0xff);
	return start;
}

/*
 * Dump the different strings that this device holds.
 */
static char *usb_dump_device_strings (char *start, char *end, struct usb_device *dev)
{
	char *buf;

	if (start > end)
		return start;
	buf = kmalloc(128, GFP_KERNEL);
	if (!buf)
		return start;
	if (dev->descriptor.iManufacturer) {
		if (usb_string(dev, dev->descriptor.iManufacturer, buf, 128) > 0)
			start += sprintf(start, format_string_manufacturer, buf);
	}				
	if (start > end)
		goto out;
	if (dev->descriptor.iProduct) {
		if (usb_string(dev, dev->descriptor.iProduct, buf, 128) > 0)
			start += sprintf(start, format_string_product, buf);
	}
	if (start > end)
		goto out;
#ifdef ALLOW_SERIAL_NUMBER
	if (dev->descriptor.iSerialNumber) {
		if (usb_string(dev, dev->descriptor.iSerialNumber, buf, 128) > 0)
			start += sprintf(start, format_string_serialnumber, buf);
	}
#endif
 out:
	kfree(buf);
	return start;
}

static char *usb_dump_desc(char *start, char *end, struct usb_device *dev)
{
	int i;

	if (start > end)
		return start;
		
	start = usb_dump_device_descriptor(start, end, &dev->descriptor);

	if (start > end)
		return start;
	
	start = usb_dump_device_strings (start, end, dev);
	
	for (i = 0; i < dev->descriptor.bNumConfigurations; i++) {
		if (start > end)
			return start;
		start = usb_dump_config(start, end, dev->config + i,
					(dev->config + i) == dev->actconfig); /* active ? */
	}
	return start;
}


#ifdef PROC_EXTRA /* TBD: may want to add this code later */

static char *usb_dump_hub_descriptor(char *start, char *end, const struct usb_hub_descriptor * desc)
{
	int leng = USB_DT_HUB_NONVAR_SIZE;
	unsigned char *ptr = (unsigned char *)desc;

	if (start > end)
		return start;
	start += sprintf(start, "Interface:");
	while (leng && start <= end) {
		start += sprintf(start, " %02x", *ptr);
		ptr++; leng--;
	}
	*start++ = '\n';
	return start;
}

static char *usb_dump_string(char *start, char *end, const struct usb_device *dev, char *id, int index)
{
	if (start > end)
		return start;
	start += sprintf(start, "Interface:");
	if (index <= dev->maxstring && dev->stringindex && dev->stringindex[index])
		start += sprintf(start, "%s: %.100s ", id, dev->stringindex[index]);
	return start;
}

#endif /* PROC_EXTRA */

/*****************************************************************/

/* This is a recursive function. Parameters:
 * buffer - the user-space buffer to write data into
 * nbytes - the maximum number of bytes to write
 * skip_bytes - the number of bytes to skip before writing anything
 * file_offset - the offset into the devices file on completion
 */
static ssize_t usb_device_dump(char **buffer, size_t *nbytes, loff_t *skip_bytes, loff_t *file_offset,
				struct usb_device *usbdev, struct usb_bus *bus, int level, int index, int count)
{
	int chix;
	int ret, cnt = 0;
	int parent_devnum = 0;
	char *pages_start, *data_end;
	unsigned int length;
	ssize_t total_written = 0;
	
	/* don't bother with anything else if we're not writing any data */
	if (*nbytes <= 0)
		return 0;
	
	if (level > MAX_TOPO_LEVEL)
		return total_written;
	/* allocate 2^1 pages = 8K (on i386); should be more than enough for one device */
        if (!(pages_start = (char*) __get_free_pages(GFP_KERNEL,1)))
                return -ENOMEM;
		
	if (usbdev->parent && usbdev->parent->devnum != -1)
		parent_devnum = usbdev->parent->devnum;
	/*
	 * So the root hub's parent is 0 and any device that is
	 * plugged into the root hub has a parent of 0.
	 */
	data_end = pages_start + sprintf(pages_start, format_topo, bus->busnum, level, parent_devnum, index, count,
					usbdev->devnum, usbdev->slow ? "1.5" : "12 ", usbdev->maxchild);
	/*
	 * level = topology-tier level;
	 * parent_devnum = parent device number;
	 * index = parent's connector number;
	 * count = device count at this level
	 */
	/* If this is the root hub, display the bandwidth information */
	if (level == 0)
		data_end += sprintf(data_end, format_bandwidth, bus->bandwidth_allocated,
				FRAME_TIME_MAX_USECS_ALLOC,
				(100 * bus->bandwidth_allocated + FRAME_TIME_MAX_USECS_ALLOC / 2) / FRAME_TIME_MAX_USECS_ALLOC,
			         bus->bandwidth_int_reqs, bus->bandwidth_isoc_reqs);
	
	data_end = usb_dump_desc(data_end, pages_start + (2 * PAGE_SIZE) - 256, usbdev);
	
	if (data_end > (pages_start + (2 * PAGE_SIZE) - 256))
		data_end += sprintf(data_end, "(truncated)\n");
	
	length = data_end - pages_start;
	/* if we can start copying some data to the user */
	if (length > *skip_bytes) {
		length -= *skip_bytes;
		if (length > *nbytes)
			length = *nbytes;
		if (copy_to_user(*buffer, pages_start + *skip_bytes, length)) {
			free_pages((unsigned long)pages_start, 1);
			
			if (total_written == 0)
				return -EFAULT;
			return total_written;
		}
		*nbytes -= length;
		*file_offset += length;
		total_written += length;
		*buffer += length;
		*skip_bytes = 0;
	} else
		*skip_bytes -= length;
	
	free_pages((unsigned long)pages_start, 1);
	
	/* Now look at all of this device's children. */
	for (chix = 0; chix < usbdev->maxchild; chix++) {
		if (usbdev->children[chix]) {
			ret = usb_device_dump(buffer, nbytes, skip_bytes, file_offset, usbdev->children[chix],
					bus, level + 1, chix, ++cnt);
			if (ret == -EFAULT)
				return total_written;
			total_written += ret;
		}
	}
	return total_written;
}

static ssize_t usb_device_read(struct file *file, char *buf, size_t nbytes, loff_t *ppos)
{
	struct list_head *buslist;
	struct usb_bus *bus;
	ssize_t ret, total_written = 0;
	loff_t skip_bytes = *ppos;

	if (*ppos < 0)
		return -EINVAL;
	if (nbytes <= 0)
		return 0;
	if (!access_ok(VERIFY_WRITE, buf, nbytes))
		return -EFAULT;

	/* enumerate busses */
	for (buslist = usb_bus_list.next; buslist != &usb_bus_list; buslist = buslist->next) {
		/* print devices for this bus */
		bus = list_entry(buslist, struct usb_bus, bus_list);
		/* recurse through all children of the root hub */
		ret = usb_device_dump(&buf, &nbytes, &skip_bytes, ppos, bus->root_hub, bus, 0, 0, 0);
		if (ret < 0)
			return ret;
		total_written += ret;
	}
	return total_written;
}

/* Kernel lock for "lastev" protection */
static unsigned int usb_device_poll(struct file *file, struct poll_table_struct *wait)
{
	struct usb_device_status *st = (struct usb_device_status *)file->private_data;
	unsigned int mask = 0;

	lock_kernel();
	if (!st) {
		st = kmalloc(sizeof(struct usb_device_status), GFP_KERNEL);
		if (!st) {
			unlock_kernel();
			return POLLIN;
		}
		/*
		 * need to prevent the module from being unloaded, since
		 * proc_unregister does not call the release method and
		 * we would have a memory leak
		 */
		st->lastev = conndiscevcnt;
		file->private_data = st;
		mask = POLLIN;
	}
	if (file->f_mode & FMODE_READ)
                poll_wait(file, &deviceconndiscwq, wait);
	if (st->lastev != conndiscevcnt)
		mask |= POLLIN;
	st->lastev = conndiscevcnt;
	unlock_kernel();
	return mask;
}

static int usb_device_open(struct inode *inode, struct file *file)
{
        file->private_data = NULL;
        return 0;
}

static int usb_device_release(struct inode *inode, struct file *file)
{
	if (file->private_data) {
		kfree(file->private_data);
		file->private_data = NULL;
	}

        return 0;
}

static loff_t usb_device_lseek(struct file * file, loff_t offset, int orig)
{
	switch (orig) {
	case 0:
		file->f_pos = offset;
		return file->f_pos;

	case 1:
		file->f_pos += offset;
		return file->f_pos;

	case 2:
		return -EINVAL;

	default:
		return -EINVAL;
	}
}

struct file_operations usbdevfs_devices_fops = {
	llseek:		usb_device_lseek,
	read:		usb_device_read,
	poll:		usb_device_poll,
	open:		usb_device_open,
	release:	usb_device_release,
};
