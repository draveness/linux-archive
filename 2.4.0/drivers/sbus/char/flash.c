/* $Id: flash.c,v 1.20 2000/11/08 04:57:49 davem Exp $
 * flash.c: Allow mmap access to the OBP Flash, for OBP updates.
 *
 * Copyright (C) 1997  Eddie C. Dost  (ecd@skynet.be)
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/malloc.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/smp_lock.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/io.h>
#include <asm/sbus.h>
#include <asm/ebus.h>

static struct {
	unsigned long read_base;	/* Physical read address */
	unsigned long write_base;	/* Physical write address */
	unsigned long read_size;	/* Size of read area */
	unsigned long write_size;	/* Size of write area */
	unsigned long busy;		/* In use? */
} flash;

#define FLASH_MINOR	152

static int
flash_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long addr;
	unsigned long size;

	lock_kernel();
	if (flash.read_base == flash.write_base) {
		addr = flash.read_base;
		size = flash.read_size;
	} else {
		if ((vma->vm_flags & VM_READ) &&
		    (vma->vm_flags & VM_WRITE)) {
			unlock_kernel();
			return -EINVAL;
		}
		if (vma->vm_flags & VM_READ) {
			addr = flash.read_base;
			size = flash.read_size;
		} else if (vma->vm_flags & VM_WRITE) {
			addr = flash.write_base;
			size = flash.write_size;
		} else {
			unlock_kernel();
			return -ENXIO;
		}
	}
	unlock_kernel();

	if ((vma->vm_pgoff << PAGE_SHIFT) > size)
		return -ENXIO;
	addr += (vma->vm_pgoff << PAGE_SHIFT);

	if (vma->vm_end - (vma->vm_start + (vma->vm_pgoff << PAGE_SHIFT)) > size)
		size = vma->vm_end - (vma->vm_start + (vma->vm_pgoff << PAGE_SHIFT));

	pgprot_val(vma->vm_page_prot) &= ~(_PAGE_CACHE);
	pgprot_val(vma->vm_page_prot) |= _PAGE_E;
	vma->vm_flags |= (VM_SHM | VM_LOCKED);

	if (remap_page_range(vma->vm_start, addr, size, vma->vm_page_prot))
		return -EAGAIN;
		
	return 0;
}

static long long
flash_llseek(struct file *file, long long offset, int origin)
{
	switch (origin) {
		case 0:
			file->f_pos = offset;
			break;
		case 1:
			file->f_pos += offset;
			if (file->f_pos > flash.read_size)
				file->f_pos = flash.read_size;
			break;
		case 2:
			file->f_pos = flash.read_size;
			break;
		default:
			return -EINVAL;
	}
	return file->f_pos;
}

static ssize_t
flash_read(struct file * file, char * buf,
	   size_t count, loff_t *ppos)
{
	unsigned long p = file->f_pos;
	
	if (count > flash.read_size - p)
		count = flash.read_size - p;

	if (copy_to_user(buf, flash.read_base + p, count) < 0)
		return -EFAULT;

	file->f_pos += count;
	return count;
}

static int
flash_open(struct inode *inode, struct file *file)
{
	if (test_and_set_bit(0, (void *)&flash.busy) != 0)
		return -EBUSY;

	return 0;
}

static int
flash_release(struct inode *inode, struct file *file)
{
	lock_kernel();
	flash.busy = 0;
	unlock_kernel();
	return 0;
}

static struct file_operations flash_fops = {
	/* no write to the Flash, use mmap
	 * and play flash dependent tricks.
	 */
	owner:		THIS_MODULE,
	llseek:		flash_llseek,
	read:		flash_read,
	mmap:		flash_mmap,
	open:		flash_open,
	release:	flash_release,
};

static struct miscdevice flash_dev = { FLASH_MINOR, "flash", &flash_fops };

EXPORT_NO_SYMBOLS;

static int __init flash_init(void)
{
	struct sbus_bus *sbus;
	struct sbus_dev *sdev = 0;
	struct linux_ebus *ebus;
	struct linux_ebus_device *edev = 0;
	struct linux_prom_registers regs[2];
	int len, err, nregs;

	for_all_sbusdev(sdev, sbus) {
		if (!strcmp(sdev->prom_name, "flashprom")) {
			if (sdev->reg_addrs[0].phys_addr == sdev->reg_addrs[1].phys_addr) {
				flash.read_base = ((unsigned long)sdev->reg_addrs[0].phys_addr) |
					(((unsigned long)sdev->reg_addrs[0].which_io)<<32UL);
				flash.read_size = sdev->reg_addrs[0].reg_size;
				flash.write_base = flash.read_base;
				flash.write_size = flash.read_size;
			} else {
				flash.read_base = ((unsigned long)sdev->reg_addrs[0].phys_addr) |
					(((unsigned long)sdev->reg_addrs[0].which_io)<<32UL);
				flash.read_size = sdev->reg_addrs[0].reg_size;
				flash.write_base = ((unsigned long)sdev->reg_addrs[1].phys_addr) |
					(((unsigned long)sdev->reg_addrs[1].which_io)<<32UL);
				flash.write_size = sdev->reg_addrs[1].reg_size;
			}
			flash.busy = 0;
			break;
		}
	}
	if (!sdev) {
#ifdef CONFIG_PCI
		for_each_ebus(ebus) {
			for_each_ebusdev(edev, ebus) {
				if (!strcmp(edev->prom_name, "flashprom"))
					goto ebus_done;
			}
		}
	ebus_done:
		if (!edev)
			return -ENODEV;

		len = prom_getproperty(edev->prom_node, "reg", (void *)regs, sizeof(regs));
		if ((len % sizeof(regs[0])) != 0) {
			printk("flash: Strange reg property size %d\n", len);
			return -ENODEV;
		}

		nregs = len / sizeof(regs[0]);

		flash.read_base = edev->resource[0].start;
		flash.read_size = regs[0].reg_size;

		if (nregs == 1) {
			flash.write_base = edev->resource[0].start;
			flash.write_size = regs[0].reg_size;
		} else if (nregs == 2) {
			flash.write_base = edev->resource[1].start;
			flash.write_size = regs[1].reg_size;
		} else {
			printk("flash: Strange number of regs %d\n", nregs);
			return -ENODEV;
		}

		flash.busy = 0;

#else
		return -ENODEV;
#endif
	}

	printk("OBP Flash: RD %lx[%lx] WR %lx[%lx]\n",
	       flash.read_base, flash.read_size,
	       flash.write_base, flash.write_size);

	err = misc_register(&flash_dev);
	if (err) {
		printk(KERN_ERR "flash: unable to get misc minor\n");
		return err;
	}

	return 0;
}

static void __exit flash_cleanup(void)
{
	misc_deregister(&flash_dev);
}

module_init(flash_init);
module_exit(flash_cleanup);
