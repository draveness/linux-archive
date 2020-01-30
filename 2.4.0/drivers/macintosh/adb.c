/*
 * Device driver for the Apple Desktop Bus
 * and the /dev/adb device on macintoshes.
 *
 * Copyright (C) 1996 Paul Mackerras.
 *
 * Modified to declare controllers as structures, added
 * client notification of bus reset and handles PowerBook
 * sleep, by Benjamin Herrenschmidt.
 *
 * To do:
 *
 * - /proc/adb to list the devices and infos
 * - more /dev/adb to allow userland to receive the
 *   flow of auto-polling datas from a given device.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/malloc.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/adb.h>
#include <linux/cuda.h>
#include <linux/pmu.h>
#include <linux/notifier.h>
#include <linux/wait.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#ifdef CONFIG_PPC
#include <asm/prom.h>
#include <asm/hydra.h>
#endif

EXPORT_SYMBOL(adb_controller);
EXPORT_SYMBOL(adb_client_list);

extern struct adb_driver via_macii_driver;
extern struct adb_driver via_maciisi_driver;
extern struct adb_driver via_cuda_driver;
extern struct adb_driver adb_iop_driver;
extern struct adb_driver via_pmu_driver;
extern struct adb_driver macio_adb_driver;

static struct adb_driver *adb_driver_list[] = {
#ifdef CONFIG_ADB_MACII
	&via_macii_driver,
#endif
#ifdef CONFIG_ADB_MACIISI
	&via_maciisi_driver,
#endif
#ifdef CONFIG_ADB_CUDA
	&via_cuda_driver,
#endif
#ifdef CONFIG_ADB_IOP
	&adb_iop_driver,
#endif
#ifdef CONFIG_ADB_PMU
	&via_pmu_driver,
#endif
#ifdef CONFIG_ADB_MACIO
	&macio_adb_driver,
#endif
	NULL
};

struct adb_driver *adb_controller;
struct notifier_block *adb_client_list = NULL;
static int adb_got_sleep = 0;
static int adb_inited = 0;

#ifdef CONFIG_PMAC_PBOOK
static int adb_notify_sleep(struct pmu_sleep_notifier *self, int when);
static struct pmu_sleep_notifier adb_sleep_notifier = {
	adb_notify_sleep,
	SLEEP_LEVEL_ADB,
};
#endif

static int adb_scan_bus(void);

static struct adb_handler {
	void (*handler)(unsigned char *, int, struct pt_regs *, int);
	int original_address;
	int handler_id;
} adb_handler[16];

#if 0
static void printADBreply(struct adb_request *req)
{
        int i;

        printk("adb reply (%d)", req->reply_len);
        for(i = 0; i < req->reply_len; i++)
                printk(" %x", req->reply[i]);
        printk("\n");

}
#endif

static int adb_scan_bus(void)
{
	int i, highFree=0, noMovement;
	int devmask = 0;
	struct adb_request req;
	
	/* assumes adb_handler[] is all zeroes at this point */
	for (i = 1; i < 16; i++) {
		/* see if there is anything at address i */
		adb_request(&req, NULL, ADBREQ_SYNC | ADBREQ_REPLY, 1,
                            (i << 4) | 0xf);
		if (req.reply_len > 1)
			/* one or more devices at this address */
			adb_handler[i].original_address = i;
		else if (i > highFree)
			highFree = i;
	}

	/* Note we reset noMovement to 0 each time we move a device */
	for (noMovement = 1; noMovement < 2 && highFree > 0; noMovement++) {
		for (i = 1; i < 16; i++) {
			if (adb_handler[i].original_address == 0)
				continue;
			/*
			 * Send a "talk register 3" command to address i
			 * to provoke a collision if there is more than
			 * one device at this address.
			 */
			adb_request(&req, NULL, ADBREQ_SYNC | ADBREQ_REPLY, 1,
				    (i << 4) | 0xf);
			/*
			 * Move the device(s) which didn't detect a
			 * collision to address `highFree'.  Hopefully
			 * this only moves one device.
			 */
			adb_request(&req, NULL, ADBREQ_SYNC, 3,
				    (i<< 4) | 0xb, (highFree | 0x60), 0xfe);
			/*
			 * See if anybody actually moved. This is suggested
			 * by HW TechNote 01:
			 *
			 * http://developer.apple.com/technotes/hw/hw_01.html
			 */
			adb_request(&req, NULL, ADBREQ_SYNC | ADBREQ_REPLY, 1,
				    (highFree << 4) | 0xf);
			if (req.reply_len <= 1) continue;
			/*
			 * Test whether there are any device(s) left
			 * at address i.
			 */
			adb_request(&req, NULL, ADBREQ_SYNC | ADBREQ_REPLY, 1,
				    (i << 4) | 0xf);
			if (req.reply_len > 1) {
				/*
				 * There are still one or more devices
				 * left at address i.  Register the one(s)
				 * we moved to `highFree', and find a new
				 * value for highFree.
				 */
				adb_handler[highFree].original_address =
					adb_handler[i].original_address;
				while (highFree > 0 &&
				       adb_handler[highFree].original_address)
					highFree--;
				if (highFree <= 0)
					break;

				noMovement = 0;
			}
			else {
				/*
				 * No devices left at address i; move the
				 * one(s) we moved to `highFree' back to i.
				 */
				adb_request(&req, NULL, ADBREQ_SYNC, 3,
					    (highFree << 4) | 0xb,
					    (i | 0x60), 0xfe);
			}
		}	
	}

	/* Now fill in the handler_id field of the adb_handler entries. */
	printk(KERN_DEBUG "adb devices:");
	for (i = 1; i < 16; i++) {
		if (adb_handler[i].original_address == 0)
			continue;
		adb_request(&req, NULL, ADBREQ_SYNC | ADBREQ_REPLY, 1,
			    (i << 4) | 0xf);
		adb_handler[i].handler_id = req.reply[2];
		printk(" [%d]: %d %x", i, adb_handler[i].original_address,
		       adb_handler[i].handler_id);
		devmask |= 1 << i;
	}
	printk("\n");
	return devmask;
}

int __init adb_init(void)
{
	struct adb_driver *driver;
	int i;

#ifdef CONFIG_PPC
	if ( (_machine != _MACH_chrp) && (_machine != _MACH_Pmac) )
		return 0;
#endif
#ifdef CONFIG_MAC
	if (!MACH_IS_MAC)
		return 0;
#endif

	/* xmon may do early-init */
	if (adb_inited)
		return 0;
	adb_inited = 1;
		
	adb_controller = NULL;

	i = 0;
	while ((driver = adb_driver_list[i++]) != NULL) {
		if (!driver->probe()) {
			adb_controller = driver;
			break;
		}
	}
	if ((adb_controller == NULL) || adb_controller->init()) {
		printk(KERN_WARNING "Warning: no ADB interface detected\n");
	} else {
#ifdef CONFIG_PMAC_PBOOK
		pmu_register_sleep_notifier(&adb_sleep_notifier);
#endif /* CONFIG_PMAC_PBOOK */

		adb_reset_bus();
	}
	return 0;
}

__initcall(adb_init);

#ifdef CONFIG_PMAC_PBOOK
/*
 * notify clients before sleep and reset bus afterwards
 */
int
adb_notify_sleep(struct pmu_sleep_notifier *self, int when)
{
	int ret;
	
	switch (when) {
	case PBOOK_SLEEP_REQUEST:
		adb_got_sleep = 1;
		if (adb_controller->autopoll)
			adb_controller->autopoll(0);
		ret = notifier_call_chain(&adb_client_list, ADB_MSG_POWERDOWN, NULL);
		if (ret & NOTIFY_STOP_MASK)
			return PBOOK_SLEEP_REFUSE;
		break;
	case PBOOK_SLEEP_REJECT:
		if (adb_got_sleep) {
			adb_got_sleep = 0;
			adb_reset_bus();
		}
		break;
		
	case PBOOK_SLEEP_NOW:
		break;
	case PBOOK_WAKE:
		adb_reset_bus();
		adb_got_sleep = 0;
		break;
	}
	return PBOOK_SLEEP_OK;
}
#endif /* CONFIG_PMAC_PBOOK */

int
adb_reset_bus(void)
{
	int ret, nret, devs;
	unsigned long flags;
	
	if (adb_controller == NULL)
		return -ENXIO;
		
	if (adb_controller->autopoll)
		adb_controller->autopoll(0);

	nret = notifier_call_chain(&adb_client_list, ADB_MSG_PRE_RESET, NULL);
	if (nret & NOTIFY_STOP_MASK) {
		if (adb_controller->autopoll)
			adb_controller->autopoll(devs);
		return -EBUSY;
	}

	save_flags(flags);
	cli();
	memset(adb_handler, 0, sizeof(adb_handler));
	restore_flags(flags);
	
	if (adb_controller->reset_bus)
		ret = adb_controller->reset_bus();
	else
		ret = 0;

	if (!ret) {
		devs = adb_scan_bus();
		if (adb_controller->autopoll)
			adb_controller->autopoll(devs);
	}

	nret = notifier_call_chain(&adb_client_list, ADB_MSG_POST_RESET, NULL);
	if (nret & NOTIFY_STOP_MASK)
		return -EBUSY;
	
	return ret;
}

void
adb_poll(void)
{
	if ((adb_controller == NULL)||(adb_controller->poll == NULL))
		return;
	adb_controller->poll();
}


int
adb_request(struct adb_request *req, void (*done)(struct adb_request *),
	    int flags, int nbytes, ...)
{
	va_list list;
	int i;
	struct adb_request sreq;

	if ((adb_controller == NULL) || (adb_controller->send_request == NULL))
		return -ENXIO;
	if (nbytes < 1)
		return -EINVAL;
	
	if (req == NULL) {
		req = &sreq;
		flags |= ADBREQ_SYNC;
	}
	req->nbytes = nbytes+1;
	req->done = done;
	req->reply_expected = flags & ADBREQ_REPLY;
	req->data[0] = ADB_PACKET;
	va_start(list, nbytes);
	for (i = 0; i < nbytes; ++i)
		req->data[i+1] = va_arg(list, int);
	va_end(list);

	if (flags & ADBREQ_NOSEND)
		return 0;

	return adb_controller->send_request(req, flags & ADBREQ_SYNC);
}

 /* Ultimately this should return the number of devices with
    the given default id.
    And it does it now ! Note: changed behaviour: This function
    will now register if default_id _and_ handler_id both match
    but handler_id can be left to 0 to match with default_id only.
    When handler_id is set, this function will try to adjust
    the handler_id id it doesn't match. */
int
adb_register(int default_id, int handler_id, struct adb_ids *ids,
	     void (*handler)(unsigned char *, int, struct pt_regs *, int))
{
	int i;

	ids->nids = 0;
	for (i = 1; i < 16; i++) {
		if ((adb_handler[i].original_address == default_id) &&
		    (!handler_id || (handler_id == adb_handler[i].handler_id) || 
		    adb_try_handler_change(i, handler_id))) {
			if (adb_handler[i].handler != 0) {
				printk(KERN_ERR
				       "Two handlers for ADB device %d\n",
				       default_id);
				continue;
			}
			adb_handler[i].handler = handler;
			ids->id[ids->nids++] = i;
		}
	}
	return ids->nids;
}

int
adb_unregister(int index)
{
	if (!adb_handler[index].handler)
		return -ENODEV;
	adb_handler[index].handler = 0;
	return 0;
}

void
adb_input(unsigned char *buf, int nb, struct pt_regs *regs, int autopoll)
{
	int i, id;
	static int dump_adb_input = 0;

	/* We skip keystrokes and mouse moves when the sleep process
	 * has been started. We stop autopoll, but this is another security
	 */
	if (adb_got_sleep)
		return;
		
	id = buf[0] >> 4;
	if (dump_adb_input) {
		printk(KERN_INFO "adb packet: ");
		for (i = 0; i < nb; ++i)
			printk(" %x", buf[i]);
		printk(", id = %d\n", id);
	}
	if (adb_handler[id].handler != 0) {
		(*adb_handler[id].handler)(buf, nb, regs, autopoll);
	}
}

/* Try to change handler to new_id. Will return 1 if successful */
int
adb_try_handler_change(int address, int new_id)
{
	struct adb_request req;

	if (adb_handler[address].handler_id == new_id)
	    return 1;
	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	    ADB_WRITEREG(address, 3), address | 0x20, new_id);
	adb_request(&req, NULL, ADBREQ_SYNC | ADBREQ_REPLY, 1,
	    ADB_READREG(address, 3));
	if (req.reply_len < 2)
	    return 0;
	if (req.reply[2] != new_id)
	    return 0;
	adb_handler[address].handler_id = req.reply[2];

	return 1;
}

int
adb_get_infos(int address, int *original_address, int *handler_id)
{
	*original_address = adb_handler[address].original_address;
	*handler_id = adb_handler[address].handler_id;
	
	return (*original_address != 0);
}


/*
 * /dev/adb device driver.
 */

#define ADB_MAJOR	56	/* major number for /dev/adb */

extern void adbdev_init(void);

struct adbdev_state {
	spinlock_t	lock;
	atomic_t	n_pending;
	struct adb_request *completed;
  	wait_queue_head_t wait_queue;
	int		inuse;
};

static void adb_write_done(struct adb_request *req)
{
	struct adbdev_state *state = (struct adbdev_state *) req->arg;
	unsigned long flags;

	if (!req->complete) {
		req->reply_len = 0;
		req->complete = 1;
	}
	spin_lock_irqsave(&state->lock, flags);
	atomic_dec(&state->n_pending);
	if (!state->inuse) {
		kfree(req);
		if (atomic_read(&state->n_pending) == 0) {
			spin_unlock_irqrestore(&state->lock, flags);
			kfree(state);
			return;
		}
	} else {
		struct adb_request **ap = &state->completed;
		while (*ap != NULL)
			ap = &(*ap)->next;
		req->next = NULL;
		*ap = req;
		wake_up_interruptible(&state->wait_queue);
	}
	spin_unlock_irqrestore(&state->lock, flags);
}

static int adb_open(struct inode *inode, struct file *file)
{
	struct adbdev_state *state;

	if (MINOR(inode->i_rdev) > 0 || adb_controller == NULL)
		return -ENXIO;
	state = kmalloc(sizeof(struct adbdev_state), GFP_KERNEL);
	if (state == 0)
		return -ENOMEM;
	file->private_data = state;
	spin_lock_init(&state->lock);
	atomic_set(&state->n_pending, 0);
	state->completed = NULL;
	init_waitqueue_head(&state->wait_queue);
	state->inuse = 1;

	return 0;
}

static int adb_release(struct inode *inode, struct file *file)
{
	struct adbdev_state *state = file->private_data;
	unsigned long flags;

	lock_kernel();
	if (state) {
		file->private_data = NULL;
		spin_lock_irqsave(&state->lock, flags);
		if (atomic_read(&state->n_pending) == 0
		    && state->completed == NULL) {
			spin_unlock_irqrestore(&state->lock, flags);
			kfree(state);
		} else {
			state->inuse = 0;
			spin_unlock_irqrestore(&state->lock, flags);
		}
	}
	unlock_kernel();
	return 0;
}

static long long adb_lseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}

static ssize_t adb_read(struct file *file, char *buf,
			size_t count, loff_t *ppos)
{
	int ret;
	struct adbdev_state *state = file->private_data;
	struct adb_request *req;
	wait_queue_t wait = __WAITQUEUE_INITIALIZER(wait,current);
	unsigned long flags;

	if (count < 2)
		return -EINVAL;
	if (count > sizeof(req->reply))
		count = sizeof(req->reply);
	ret = verify_area(VERIFY_WRITE, buf, count);
	if (ret)
		return ret;

	req = NULL;
	add_wait_queue(&state->wait_queue, &wait);
	current->state = TASK_INTERRUPTIBLE;

	for (;;) {
		spin_lock_irqsave(&state->lock, flags);
		req = state->completed;
		if (req != NULL)
			state->completed = req->next;
		else if (atomic_read(&state->n_pending) == 0)
			ret = -EIO;
		spin_unlock_irqrestore(&state->lock, flags);
		if (req != NULL || ret != 0)
			break;
		
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			break;
		}
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
		schedule();
	}

	current->state = TASK_RUNNING;
	remove_wait_queue(&state->wait_queue, &wait);

	if (ret)
		return ret;

	ret = req->reply_len;
	if (ret > count)
		ret = count;
	if (ret > 0 && copy_to_user(buf, req->reply, ret))
		ret = -EFAULT;

	kfree(req);
	return ret;
}

static ssize_t adb_write(struct file *file, const char *buf,
			 size_t count, loff_t *ppos)
{
	int ret/*, i*/;
	struct adbdev_state *state = file->private_data;
	struct adb_request *req;

	if (count < 2 || count > sizeof(req->data))
		return -EINVAL;
	ret = verify_area(VERIFY_READ, buf, count);
	if (ret)
		return ret;

	req = (struct adb_request *) kmalloc(sizeof(struct adb_request),
					     GFP_KERNEL);
	if (req == NULL)
		return -ENOMEM;

	req->nbytes = count;
	req->done = adb_write_done;
	req->arg = (void *) state;
	req->complete = 0;
	
	ret = -EFAULT;
	if (copy_from_user(req->data, buf, count))
		goto out;

	atomic_inc(&state->n_pending);
	if (adb_controller == NULL) return -ENXIO;

	/* Special case for ADB_BUSRESET request, all others are sent to
	   the controller */
	if ((req->data[0] == ADB_PACKET)&&(count > 1)
		&&(req->data[1] == ADB_BUSRESET)) {
		ret = adb_reset_bus();
		atomic_dec(&state->n_pending);
		goto out;
	} else {	
		req->reply_expected = ((req->data[1] & 0xc) == 0xc);

		if (adb_controller && adb_controller->send_request)
			ret = adb_controller->send_request(req, 0);
		else
			ret = -ENXIO;
	}

	if (ret != 0) {
		atomic_dec(&state->n_pending);
		goto out;
	}
	return count;

out:
	kfree(req);
	return ret;
}

static struct file_operations adb_fops = {
	llseek:		adb_lseek,
	read:		adb_read,
	write:		adb_write,
	open:		adb_open,
	release:	adb_release,
};

void adbdev_init()
{
#ifdef CONFIG_PPC
	if ( (_machine != _MACH_chrp) && (_machine != _MACH_Pmac) )
		return;
#endif
#ifdef CONFIG_MAC
	if (!MACH_IS_MAC)
		return;
#endif

	if (devfs_register_chrdev(ADB_MAJOR, "adb", &adb_fops))
		printk(KERN_ERR "adb: unable to get major %d\n", ADB_MAJOR);
	else
		devfs_register (NULL, "adb", DEVFS_FL_DEFAULT,
				ADB_MAJOR, 0,
				S_IFCHR | S_IRUSR | S_IWUSR,
				&adb_fops, NULL);
}
