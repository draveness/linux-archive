/*
 * linux/drivers/char/misc.c
 *
 * Generic misc open routine by Johan Myreen
 *
 * Based on code from Linus
 *
 * Teemu Rantanen's Microsoft Busmouse support and Derrick Cole's
 *   changes incorporated into 0.97pl4
 *   by Peter Cervasio (pete%q106fm.uucp@wupost.wustl.edu) (08SEP92)
 *   See busmouse.c for particulars.
 *
 * Made things a lot mode modular - easy to compile in just one or two
 * of the misc drivers, as they are now completely independent. Linus.
 *
 * Support for loadable modules. 8-Sep-95 Philip Blundell <pjb27@cam.ac.uk>
 *
 * Fixed a failing symbol register to free the device registration
 *		Alan Cox <alan@lxorguk.ukuu.org.uk> 21-Jan-96
 *
 * Dynamic minors and /proc/mice by Alessandro Rubini. 26-Mar-96
 *
 * Renamed to misc and miscdevice to be more accurate. Alan Cox 26-Mar-96
 *
 * Handling of mouse minor numbers for kerneld:
 *  Idea by Jacques Gelinas <jack@solucorp.qc.ca>,
 *  adapted by Bjorn Ekwall <bj0rn@blox.se>
 *  corrected by Alan Cox <alan@lxorguk.ukuu.org.uk>
 *
 * Changes for kmod (from kerneld):
 *	Cyrus Durgin <cider@speakeasy.org>
 *
 * Added devfs support. Richard Gooch <rgooch@atnf.csiro.au>  10-Jan-1998
 */

#include <linux/module.h>
#include <linux/config.h>

#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/kmod.h>

/*
 * Head entry for the doubly linked miscdevice list
 */
static LIST_HEAD(misc_list);
static DECLARE_MUTEX(misc_sem);

/*
 * Assigned numbers, used for dynamic minors
 */
#define DYNAMIC_MINORS 64 /* like dynamic majors */
static unsigned char misc_minors[DYNAMIC_MINORS / 8];

extern int rtc_DP8570A_init(void);
extern int rtc_MK48T08_init(void);
extern int pmu_device_init(void);
extern int tosh_init(void);
extern int i8k_init(void);

#ifdef CONFIG_PROC_FS
static void *misc_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct miscdevice *p;
	loff_t off = 0;

	down(&misc_sem);
	list_for_each_entry(p, &misc_list, list) {
		if (*pos == off++) 
			return p;
	}
	return NULL;
}

static void *misc_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct list_head *n = ((struct miscdevice *)v)->list.next;

	++*pos;

	return (n != &misc_list) ? list_entry(n, struct miscdevice, list)
		 : NULL;
}

static void misc_seq_stop(struct seq_file *seq, void *v)
{
	up(&misc_sem);
}

static int misc_seq_show(struct seq_file *seq, void *v)
{
	const struct miscdevice *p = v;

	seq_printf(seq, "%3i %s\n", p->minor, p->name ? p->name : "");
	return 0;
}


static struct seq_operations misc_seq_ops = {
	.start = misc_seq_start,
	.next  = misc_seq_next,
	.stop  = misc_seq_stop,
	.show  = misc_seq_show,
};

static int misc_seq_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &misc_seq_ops);
}

static struct file_operations misc_proc_fops = {
	.owner	 = THIS_MODULE,
	.open    = misc_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};
#endif

static int misc_open(struct inode * inode, struct file * file)
{
	int minor = iminor(inode);
	struct miscdevice *c;
	int err = -ENODEV;
	struct file_operations *old_fops, *new_fops = NULL;
	
	down(&misc_sem);
	
	list_for_each_entry(c, &misc_list, list) {
		if (c->minor == minor) {
			new_fops = fops_get(c->fops);		
			break;
		}
	}
		
	if (!new_fops) {
		up(&misc_sem);
		request_module("char-major-%d-%d", MISC_MAJOR, minor);
		down(&misc_sem);

		list_for_each_entry(c, &misc_list, list) {
			if (c->minor == minor) {
				new_fops = fops_get(c->fops);
				break;
			}
		}
		if (!new_fops)
			goto fail;
	}

	err = 0;
	old_fops = file->f_op;
	file->f_op = new_fops;
	if (file->f_op->open) {
		err=file->f_op->open(inode,file);
		if (err) {
			fops_put(file->f_op);
			file->f_op = fops_get(old_fops);
		}
	}
	fops_put(old_fops);
fail:
	up(&misc_sem);
	return err;
}

/* 
 * TODO for 2.7:
 *  - add a struct class_device to struct miscdevice and make all usages of
 *    them dynamic.
 */
static struct class_simple *misc_class;

static struct file_operations misc_fops = {
	.owner		= THIS_MODULE,
	.open		= misc_open,
};


/**
 *	misc_register	-	register a miscellaneous device
 *	@misc: device structure
 *	
 *	Register a miscellaneous device with the kernel. If the minor
 *	number is set to %MISC_DYNAMIC_MINOR a minor number is assigned
 *	and placed in the minor field of the structure. For other cases
 *	the minor number requested is used.
 *
 *	The structure passed is linked into the kernel and may not be
 *	destroyed until it has been unregistered.
 *
 *	A zero is returned on success and a negative errno code for
 *	failure.
 */
 
int misc_register(struct miscdevice * misc)
{
	struct miscdevice *c;
	struct class_device *class;
	dev_t dev;
	int err;
	
	down(&misc_sem);
	list_for_each_entry(c, &misc_list, list) {
		if (c->minor == misc->minor) {
			up(&misc_sem);
			return -EBUSY;
		}
	}

	if (misc->minor == MISC_DYNAMIC_MINOR) {
		int i = DYNAMIC_MINORS;
		while (--i >= 0)
			if ( (misc_minors[i>>3] & (1 << (i&7))) == 0)
				break;
		if (i<0)
		{
			up(&misc_sem);
			return -EBUSY;
		}
		misc->minor = i;
	}

	if (misc->minor < DYNAMIC_MINORS)
		misc_minors[misc->minor >> 3] |= 1 << (misc->minor & 7);
	if (misc->devfs_name[0] == '\0') {
		snprintf(misc->devfs_name, sizeof(misc->devfs_name),
				"misc/%s", misc->name);
	}
	dev = MKDEV(MISC_MAJOR, misc->minor);

	class = class_simple_device_add(misc_class, dev,
					misc->dev, misc->name);
	if (IS_ERR(class)) {
		err = PTR_ERR(class);
		goto out;
	}

	err = devfs_mk_cdev(dev, S_IFCHR|S_IRUSR|S_IWUSR|S_IRGRP, 
			    misc->devfs_name);
	if (err) {
		class_simple_device_remove(dev);
		goto out;
	}

	/*
	 * Add it to the front, so that later devices can "override"
	 * earlier defaults
	 */
	list_add(&misc->list, &misc_list);
 out:
	up(&misc_sem);
	return err;
}

/**
 *	misc_deregister - unregister a miscellaneous device
 *	@misc: device to unregister
 *
 *	Unregister a miscellaneous device that was previously
 *	successfully registered with misc_register(). Success
 *	is indicated by a zero return, a negative errno code
 *	indicates an error.
 */

int misc_deregister(struct miscdevice * misc)
{
	int i = misc->minor;

	if (list_empty(&misc->list))
		return -EINVAL;

	down(&misc_sem);
	list_del(&misc->list);
	class_simple_device_remove(MKDEV(MISC_MAJOR, misc->minor));
	devfs_remove(misc->devfs_name);
	if (i < DYNAMIC_MINORS && i>0) {
		misc_minors[i>>3] &= ~(1 << (misc->minor & 7));
	}
	up(&misc_sem);
	return 0;
}

EXPORT_SYMBOL(misc_register);
EXPORT_SYMBOL(misc_deregister);

static int __init misc_init(void)
{
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry *ent;

	ent = create_proc_entry("misc", 0, NULL);
	if (ent)
		ent->proc_fops = &misc_proc_fops;
#endif
	misc_class = class_simple_create(THIS_MODULE, "misc");
	if (IS_ERR(misc_class))
		return PTR_ERR(misc_class);
#ifdef CONFIG_MVME16x
	rtc_MK48T08_init();
#endif
#ifdef CONFIG_BVME6000
	rtc_DP8570A_init();
#endif
#ifdef CONFIG_PMAC_PBOOK
	pmu_device_init();
#endif
#ifdef CONFIG_TOSHIBA
	tosh_init();
#endif
#ifdef CONFIG_I8K
	i8k_init();
#endif
	if (register_chrdev(MISC_MAJOR,"misc",&misc_fops)) {
		printk("unable to get major %d for misc devices\n",
		       MISC_MAJOR);
		class_simple_destroy(misc_class);
		return -EIO;
	}
	return 0;
}
subsys_initcall(misc_init);
