/* -*- linux-c -*- */

/* 
 * Driver for USB Rio 500
 *
 * Cesar Miquel (miquel@df.uba.ar)
 * 
 * based on hp_scanner.c by David E. Nelson (dnelson@jump.net)
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Based upon mouse.c (Brad Keryan) and printer.c (Michael Gee).
 *
 * */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/malloc.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/smp_lock.h>

#include "rio500_usb.h"

#define RIO_MINOR   64

/* stall/wait timeout for rio */
#define NAK_TIMEOUT (HZ)

#define IBUF_SIZE 0x1000

/* Size of the rio buffer */
#define OBUF_SIZE 0x10000

struct rio_usb_data {
        struct usb_device *rio_dev;     /* init: probe_rio */
        unsigned int ifnum;             /* Interface number of the USB device */
        int isopen;                     /* nz if open */
        int present;                    /* Device is present on the bus */
        char *obuf, *ibuf;              /* transfer buffers */
        char bulk_in_ep, bulk_out_ep;   /* Endpoint assignments */
        wait_queue_head_t wait_q;       /* for timeouts */
	struct semaphore lock;          /* general race avoidance */
};

static struct rio_usb_data rio_instance;

static int open_rio(struct inode *inode, struct file *file)
{
	struct rio_usb_data *rio = &rio_instance;

	lock_kernel();

	if (rio->isopen || !rio->present) {
		unlock_kernel();
		return -EBUSY;
	}
	rio->isopen = 1;

	init_waitqueue_head(&rio->wait_q);

	MOD_INC_USE_COUNT;

	unlock_kernel();

	info("Rio opened.");

	return 0;
}

static int close_rio(struct inode *inode, struct file *file)
{
	struct rio_usb_data *rio = &rio_instance;

	rio->isopen = 0;

	MOD_DEC_USE_COUNT;

	info("Rio closed.");
	return 0;
}

static int
ioctl_rio(struct inode *inode, struct file *file, unsigned int cmd,
	  unsigned long arg)
{
	struct RioCommand rio_cmd;
	struct rio_usb_data *rio = &rio_instance;
	void *data;
	unsigned char *buffer;
	int result, requesttype;
	int retries;
	int retval;

        /* Sanity check to make sure rio is connected, powered, etc */
        if ( rio == NULL ||
             rio->present == 0 ||
             rio->rio_dev == NULL )
          return -1;

	switch (cmd) {
	case RIO_RECV_COMMAND:
		data = (void *) arg;
		if (data == NULL)
			break;
		if (copy_from_user(&rio_cmd, data, sizeof(struct RioCommand))) {
			retval = -EFAULT;
			goto err_out;
		}
		if (rio_cmd.length > PAGE_SIZE) {
			retval = -EINVAL;
			goto err_out;
		}
		buffer = (unsigned char *) __get_free_page(GFP_KERNEL);
		if (buffer == NULL)
			return -ENOMEM;
		if (copy_from_user(buffer, rio_cmd.buffer, rio_cmd.length)) {
			retval = -EFAULT;
			free_page((unsigned long) buffer);
			goto err_out;
		}

		requesttype = rio_cmd.requesttype | USB_DIR_IN |
		    USB_TYPE_VENDOR | USB_RECIP_DEVICE;
		dbg
		    ("sending command:reqtype=%0x req=%0x value=%0x index=%0x len=%0x",
		     requesttype, rio_cmd.request, rio_cmd.value,
		     rio_cmd.index, rio_cmd.length);
		/* Send rio control message */
		retries = 3;
		down(&(rio->lock));
		while (retries) {
			result = usb_control_msg(rio->rio_dev,
						 usb_rcvctrlpipe(rio-> rio_dev, 0),
						 rio_cmd.request,
						 requesttype,
						 rio_cmd.value,
						 rio_cmd.index, buffer,
						 rio_cmd.length,
						 rio_cmd.timeout);
			if (result == -ETIMEDOUT)
				retries--;
			else if (result < 0) {
				err("Error executing ioctrl. code = %d",
				     le32_to_cpu(result));
				retries = 0;
			} else {
				dbg("Executed ioctl. Result = %d (data=%04x)",
				     le32_to_cpu(result),
				     le32_to_cpu(*((long *) buffer)));
				if (copy_to_user(rio_cmd.buffer, buffer,
						 rio_cmd.length)) {
					up(&(rio->lock));
					free_page((unsigned long) buffer);
					retval = -EFAULT;
					goto err_out;
				}
				retries = 0;
			}

			/* rio_cmd.buffer contains a raw stream of single byte
			   data which has been returned from rio.  Data is
			   interpreted at application level.  For data that
			   will be cast to data types longer than 1 byte, data
			   will be little_endian and will potentially need to
			   be swapped at the app level */

		}
		up(&(rio->lock));
		free_page((unsigned long) buffer);
		break;

	case RIO_SEND_COMMAND:
		data = (void *) arg;
		if (data == NULL)
			break;
		if (copy_from_user(&rio_cmd, data, sizeof(struct RioCommand)))
			return -EFAULT;
		if (rio_cmd.length > PAGE_SIZE)
			return -EINVAL;
		buffer = (unsigned char *) __get_free_page(GFP_KERNEL);
		if (buffer == NULL)
			return -ENOMEM;
		if (copy_from_user(buffer, rio_cmd.buffer, rio_cmd.length)) {
			free_page((unsigned long)buffer);
			return -EFAULT;
		}

		requesttype = rio_cmd.requesttype | USB_DIR_OUT |
		    USB_TYPE_VENDOR | USB_RECIP_DEVICE;
		dbg("sending command: reqtype=%0x req=%0x value=%0x index=%0x len=%0x",
		     requesttype, rio_cmd.request, rio_cmd.value,
		     rio_cmd.index, rio_cmd.length);
		/* Send rio control message */
		retries = 3;
		down(&(rio->lock));
		while (retries) {
			result = usb_control_msg(rio->rio_dev,
						 usb_sndctrlpipe(rio-> rio_dev, 0),
						 rio_cmd.request,
						 requesttype,
						 rio_cmd.value,
						 rio_cmd.index, buffer,
						 rio_cmd.length,
						 rio_cmd.timeout);
			if (result == -ETIMEDOUT)
				retries--;
			else if (result < 0) {
				err("Error executing ioctrl. code = %d",
				     le32_to_cpu(result));
				retries = 0;
			} else {
				dbg("Executed ioctl. Result = %d",
				       le32_to_cpu(result));
				retries = 0;

			}

		}
		up(&(rio->lock));
		free_page((unsigned long) buffer);
		break;

	default:
		return -ENOIOCTLCMD;
		break;
	}

	return 0;

err_out:
	return retval;
}

static ssize_t
write_rio(struct file *file, const char *buffer,
	  size_t count, loff_t * ppos)
{
	struct rio_usb_data *rio = &rio_instance;

	unsigned long copy_size;
	unsigned long bytes_written = 0;
	unsigned int partial;

	int result = 0;
	int maxretry;
	int errn = 0;

        /* Sanity check to make sure rio is connected, powered, etc */
        if ( rio == NULL ||
             rio->present == 0 ||
             rio->rio_dev == NULL )
          return -1;

	down(&(rio->lock));

	do {
		unsigned long thistime;
		char *obuf = rio->obuf;

		thistime = copy_size =
		    (count >= OBUF_SIZE) ? OBUF_SIZE : count;
		if (copy_from_user(rio->obuf, buffer, copy_size)) {
			errn = -EFAULT;
			goto error;
		}
		maxretry = 5;
		while (thistime) {
			if (!rio->rio_dev) {
				errn = -ENODEV;
				goto error;
			}
			if (signal_pending(current)) {
				up(&(rio->lock));
				return bytes_written ? bytes_written : -EINTR;
			}

			result = usb_bulk_msg(rio->rio_dev,
					 usb_sndbulkpipe(rio->rio_dev, 2),
					 obuf, thistime, &partial, 5 * HZ);

			dbg("write stats: result:%d thistime:%lu partial:%u",
			     result, thistime, partial);

			if (result == USB_ST_TIMEOUT) {	/* NAK - so hold for a while */
				if (!maxretry--) {
					errn = -ETIME;
					goto error;
				}
				interruptible_sleep_on_timeout(&rio-> wait_q, NAK_TIMEOUT);
				continue;
			} else if (!result & partial) {
				obuf += partial;
				thistime -= partial;
			} else
				break;
		};
		if (result) {
			err("Write Whoops - %x", result);
			errn = -EIO;
			goto error;
		}
		bytes_written += copy_size;
		count -= copy_size;
		buffer += copy_size;
	} while (count > 0);

	up(&(rio->lock));

	return bytes_written ? bytes_written : -EIO;

error:
	up(&(rio->lock));
	return errn;
}

static ssize_t
read_rio(struct file *file, char *buffer, size_t count, loff_t * ppos)
{
	struct rio_usb_data *rio = &rio_instance;
	ssize_t read_count;
	unsigned int partial;
	int this_read;
	int result;
	int maxretry = 10;
	char *ibuf = rio->ibuf;

        /* Sanity check to make sure rio is connected, powered, etc */
        if ( rio == NULL ||
             rio->present == 0 ||
             rio->rio_dev == NULL )
          return -1;

	read_count = 0;

	down(&(rio->lock));

	while (count > 0) {
		if (signal_pending(current)) {
			up(&(rio->lock));
			return read_count ? read_count : -EINTR;
		}
		if (!rio->rio_dev) {
			up(&(rio->lock));
			return -ENODEV;
		}
		this_read = (count >= IBUF_SIZE) ? IBUF_SIZE : count;

		result = usb_bulk_msg(rio->rio_dev,
				      usb_rcvbulkpipe(rio->rio_dev, 1),
				      ibuf, this_read, &partial,
				      (int) (HZ * 8));

		dbg(KERN_DEBUG "read stats: result:%d this_read:%u partial:%u",
		       result, this_read, partial);

		if (partial) {
			count = this_read = partial;
		} else if (result == USB_ST_TIMEOUT || result == 15) {	/* FIXME: 15 ??? */
			if (!maxretry--) {
				up(&(rio->lock));
				err("read_rio: maxretry timeout");
				return -ETIME;
			}
			interruptible_sleep_on_timeout(&rio->wait_q,
						       NAK_TIMEOUT);
			continue;
		} else if (result != USB_ST_DATAUNDERRUN) {
			up(&(rio->lock));
			err("Read Whoops - result:%u partial:%u this_read:%u",
			     result, partial, this_read);
			return -EIO;
		} else {
			unlock_kernel();
			return (0);
		}

		if (this_read) {
			if (copy_to_user(buffer, ibuf, this_read)) {
				up(&(rio->lock));
				return -EFAULT;
			}
			count -= this_read;
			read_count += this_read;
			buffer += this_read;
		}
	}
	up(&(rio->lock));
	return read_count;
}

static void *probe_rio(struct usb_device *dev, unsigned int ifnum,
		       const struct usb_device_id *id)
{
	struct rio_usb_data *rio = &rio_instance;

	info("USB Rio found at address %d", dev->devnum);

	rio->present = 1;
	rio->rio_dev = dev;

	if (!(rio->obuf = (char *) kmalloc(OBUF_SIZE, GFP_KERNEL))) {
		err("probe_rio: Not enough memory for the output buffer");
		return NULL;
	}
	dbg("probe_rio: obuf address:%p", rio->obuf);

	if (!(rio->ibuf = (char *) kmalloc(IBUF_SIZE, GFP_KERNEL))) {
		err("probe_rio: Not enough memory for the input buffer");
		kfree(rio->obuf);
		return NULL;
	}
	dbg("probe_rio: ibuf address:%p", rio->ibuf);

	init_MUTEX(&(rio->lock));

	return rio;
}

static void disconnect_rio(struct usb_device *dev, void *ptr)
{
	struct rio_usb_data *rio = (struct rio_usb_data *) ptr;

	if (rio->isopen) {
		rio->isopen = 0;
		/* better let it finish - the release will do whats needed */
		rio->rio_dev = NULL;
		return;
	}
	kfree(rio->ibuf);
	kfree(rio->obuf);

	info("USB Rio disconnected.");

	rio->present = 0;
}

static struct
file_operations usb_rio_fops = {
	read:		read_rio,
	write:		write_rio,
	ioctl:		ioctl_rio,
	open:		open_rio,
	release:	close_rio,
};

static struct usb_device_id rio_table [] = {
	{ USB_DEVICE(0x0841, 1) }, 		/* Rio 500 */
	{ }					/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, rio_table);

static struct usb_driver rio_driver = {
	name:		"rio500",
	probe:		probe_rio,
	disconnect:	disconnect_rio,
	fops:		&usb_rio_fops,
	minor:		RIO_MINOR,
	id_table:	rio_table,
};

int usb_rio_init(void)
{
	if (usb_register(&rio_driver) < 0)
		return -1;

	info("USB Rio support registered.");
	return 0;
}


void usb_rio_cleanup(void)
{
	struct rio_usb_data *rio = &rio_instance;

	rio->present = 0;
	usb_deregister(&rio_driver);


}

module_init(usb_rio_init);
module_exit(usb_rio_cleanup);

MODULE_AUTHOR("Cesar Miquel <miquel@df.uba.ar>");
MODULE_DESCRIPTION("USB Rio 500 driver");
