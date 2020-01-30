/*
 *	$Id: proc.c,v 1.13 1998/05/12 07:36:07 mj Exp $
 *
 *	Procfs interface for the PCI bus.
 *
 *	Copyright (c) 1997--1999 Martin Mares <mj@ucw.cz>
 */

#include <linux/init.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/smp_lock.h>

#include <asm/uaccess.h>
#include <asm/byteorder.h>

static int proc_initialized;	/* = 0 */

static loff_t
proc_bus_pci_lseek(struct file *file, loff_t off, int whence)
{
	loff_t new = -1;
	struct inode *inode = file->f_dentry->d_inode;

	down(&inode->i_sem);
	switch (whence) {
	case 0:
		new = off;
		break;
	case 1:
		new = file->f_pos + off;
		break;
	case 2:
		new = inode->i_size + off;
		break;
	}
	if (new < 0 || new > inode->i_size)
		new = -EINVAL;
	else
		file->f_pos = new;
	up(&inode->i_sem);
	return new;
}

static ssize_t
proc_bus_pci_read(struct file *file, char __user *buf, size_t nbytes, loff_t *ppos)
{
	const struct inode *ino = file->f_dentry->d_inode;
	const struct proc_dir_entry *dp = PDE(ino);
	struct pci_dev *dev = dp->data;
	unsigned int pos = *ppos;
	unsigned int cnt, size;

	/*
	 * Normal users can read only the standardized portion of the
	 * configuration space as several chips lock up when trying to read
	 * undefined locations (think of Intel PIIX4 as a typical example).
	 */

	if (capable(CAP_SYS_ADMIN))
		size = dev->cfg_size;
	else if (dev->hdr_type == PCI_HEADER_TYPE_CARDBUS)
		size = 128;
	else
		size = 64;

	if (pos >= size)
		return 0;
	if (nbytes >= size)
		nbytes = size;
	if (pos + nbytes > size)
		nbytes = size - pos;
	cnt = nbytes;

	if (!access_ok(VERIFY_WRITE, buf, cnt))
		return -EINVAL;

	if ((pos & 1) && cnt) {
		unsigned char val;
		pci_read_config_byte(dev, pos, &val);
		__put_user(val, buf);
		buf++;
		pos++;
		cnt--;
	}

	if ((pos & 3) && cnt > 2) {
		unsigned short val;
		pci_read_config_word(dev, pos, &val);
		__put_user(cpu_to_le16(val), (unsigned short __user *) buf);
		buf += 2;
		pos += 2;
		cnt -= 2;
	}

	while (cnt >= 4) {
		unsigned int val;
		pci_read_config_dword(dev, pos, &val);
		__put_user(cpu_to_le32(val), (unsigned int __user *) buf);
		buf += 4;
		pos += 4;
		cnt -= 4;
	}

	if (cnt >= 2) {
		unsigned short val;
		pci_read_config_word(dev, pos, &val);
		__put_user(cpu_to_le16(val), (unsigned short __user *) buf);
		buf += 2;
		pos += 2;
		cnt -= 2;
	}

	if (cnt) {
		unsigned char val;
		pci_read_config_byte(dev, pos, &val);
		__put_user(val, buf);
		buf++;
		pos++;
		cnt--;
	}

	*ppos = pos;
	return nbytes;
}

static ssize_t
proc_bus_pci_write(struct file *file, const char __user *buf, size_t nbytes, loff_t *ppos)
{
	const struct inode *ino = file->f_dentry->d_inode;
	const struct proc_dir_entry *dp = PDE(ino);
	struct pci_dev *dev = dp->data;
	int pos = *ppos;
	int size = dev->cfg_size;
	int cnt;

	if (pos >= size)
		return 0;
	if (nbytes >= size)
		nbytes = size;
	if (pos + nbytes > size)
		nbytes = size - pos;
	cnt = nbytes;

	if (!access_ok(VERIFY_READ, buf, cnt))
		return -EINVAL;

	if ((pos & 1) && cnt) {
		unsigned char val;
		__get_user(val, buf);
		pci_write_config_byte(dev, pos, val);
		buf++;
		pos++;
		cnt--;
	}

	if ((pos & 3) && cnt > 2) {
		unsigned short val;
		__get_user(val, (unsigned short __user *) buf);
		pci_write_config_word(dev, pos, le16_to_cpu(val));
		buf += 2;
		pos += 2;
		cnt -= 2;
	}

	while (cnt >= 4) {
		unsigned int val;
		__get_user(val, (unsigned int __user *) buf);
		pci_write_config_dword(dev, pos, le32_to_cpu(val));
		buf += 4;
		pos += 4;
		cnt -= 4;
	}

	if (cnt >= 2) {
		unsigned short val;
		__get_user(val, (unsigned short __user *) buf);
		pci_write_config_word(dev, pos, le16_to_cpu(val));
		buf += 2;
		pos += 2;
		cnt -= 2;
	}

	if (cnt) {
		unsigned char val;
		__get_user(val, buf);
		pci_write_config_byte(dev, pos, val);
		buf++;
		pos++;
		cnt--;
	}

	*ppos = pos;
	return nbytes;
}

struct pci_filp_private {
	enum pci_mmap_state mmap_state;
	int write_combine;
};

static int proc_bus_pci_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	const struct proc_dir_entry *dp = PDE(inode);
	struct pci_dev *dev = dp->data;
#ifdef HAVE_PCI_MMAP
	struct pci_filp_private *fpriv = file->private_data;
#endif /* HAVE_PCI_MMAP */
	int ret = 0;

	switch (cmd) {
	case PCIIOC_CONTROLLER:
		ret = pci_domain_nr(dev->bus);
		break;

#ifdef HAVE_PCI_MMAP
	case PCIIOC_MMAP_IS_IO:
		fpriv->mmap_state = pci_mmap_io;
		break;

	case PCIIOC_MMAP_IS_MEM:
		fpriv->mmap_state = pci_mmap_mem;
		break;

	case PCIIOC_WRITE_COMBINE:
		if (arg)
			fpriv->write_combine = 1;
		else
			fpriv->write_combine = 0;
		break;

#endif /* HAVE_PCI_MMAP */

	default:
		ret = -EINVAL;
		break;
	};

	return ret;
}

#ifdef HAVE_PCI_MMAP
static int proc_bus_pci_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_dentry->d_inode;
	const struct proc_dir_entry *dp = PDE(inode);
	struct pci_dev *dev = dp->data;
	struct pci_filp_private *fpriv = file->private_data;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	ret = pci_mmap_page_range(dev, vma,
				  fpriv->mmap_state,
				  fpriv->write_combine);
	if (ret < 0)
		return ret;

	return 0;
}

static int proc_bus_pci_open(struct inode *inode, struct file *file)
{
	struct pci_filp_private *fpriv = kmalloc(sizeof(*fpriv), GFP_KERNEL);

	if (!fpriv)
		return -ENOMEM;

	fpriv->mmap_state = pci_mmap_io;
	fpriv->write_combine = 0;

	file->private_data = fpriv;

	return 0;
}

static int proc_bus_pci_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;

	return 0;
}
#endif /* HAVE_PCI_MMAP */

static struct file_operations proc_bus_pci_operations = {
	.llseek		= proc_bus_pci_lseek,
	.read		= proc_bus_pci_read,
	.write		= proc_bus_pci_write,
	.ioctl		= proc_bus_pci_ioctl,
#ifdef HAVE_PCI_MMAP
	.open		= proc_bus_pci_open,
	.release	= proc_bus_pci_release,
	.mmap		= proc_bus_pci_mmap,
#ifdef HAVE_ARCH_PCI_GET_UNMAPPED_AREA
	.get_unmapped_area = get_pci_unmapped_area,
#endif /* HAVE_ARCH_PCI_GET_UNMAPPED_AREA */
#endif /* HAVE_PCI_MMAP */
};

#if BITS_PER_LONG == 32
#define LONG_FORMAT "\t%08lx"
#else
#define LONG_FORMAT "\t%16lx"
#endif

/* iterator */
static void *pci_seq_start(struct seq_file *m, loff_t *pos)
{
	struct pci_dev *dev = NULL;
	loff_t n = *pos;

	dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev);
	while (n--) {
		dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev);
		if (dev == NULL)
			goto exit;
	}
exit:
	return dev;
}

static void *pci_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct pci_dev *dev = v;

	(*pos)++;
	dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev);
	return dev;
}

static void pci_seq_stop(struct seq_file *m, void *v)
{
	if (v) {
		struct pci_dev *dev = v;
		pci_dev_put(dev);
	}
}

static int show_device(struct seq_file *m, void *v)
{
	const struct pci_dev *dev = v;
	const struct pci_driver *drv;
	int i;

	if (dev == NULL)
		return 0;

	drv = pci_dev_driver(dev);
	seq_printf(m, "%02x%02x\t%04x%04x\t%x",
			dev->bus->number,
			dev->devfn,
			dev->vendor,
			dev->device,
			dev->irq);
	/* Here should be 7 and not PCI_NUM_RESOURCES as we need to preserve compatibility */
	for(i=0; i<7; i++)
		seq_printf(m, LONG_FORMAT,
			dev->resource[i].start |
			(dev->resource[i].flags & PCI_REGION_FLAG_MASK));
	for(i=0; i<7; i++)
		seq_printf(m, LONG_FORMAT,
			dev->resource[i].start < dev->resource[i].end ?
			dev->resource[i].end - dev->resource[i].start + 1 : 0);
	seq_putc(m, '\t');
	if (drv)
		seq_printf(m, "%s", drv->name);
	seq_putc(m, '\n');
	return 0;
}

static struct seq_operations proc_bus_pci_devices_op = {
	.start	= pci_seq_start,
	.next	= pci_seq_next,
	.stop	= pci_seq_stop,
	.show	= show_device
};

struct proc_dir_entry *proc_bus_pci_dir;

int pci_proc_attach_device(struct pci_dev *dev)
{
	struct pci_bus *bus = dev->bus;
	struct proc_dir_entry *de, *e;
	char name[16];

	if (!proc_initialized)
		return -EACCES;

	if (!(de = bus->procdir)) {
		if (pci_name_bus(name, bus))
			return -EEXIST;
		de = bus->procdir = proc_mkdir(name, proc_bus_pci_dir);
		if (!de)
			return -ENOMEM;
	}
	sprintf(name, "%02x.%x", PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));
	e = dev->procent = create_proc_entry(name, S_IFREG | S_IRUGO | S_IWUSR, de);
	if (!e)
		return -ENOMEM;
	e->proc_fops = &proc_bus_pci_operations;
	e->data = dev;
	e->size = dev->cfg_size;

	return 0;
}

int pci_proc_detach_device(struct pci_dev *dev)
{
	struct proc_dir_entry *e;

	if ((e = dev->procent)) {
		if (atomic_read(&e->count))
			return -EBUSY;
		remove_proc_entry(e->name, dev->bus->procdir);
		dev->procent = NULL;
	}
	return 0;
}

int pci_proc_attach_bus(struct pci_bus* bus)
{
	struct proc_dir_entry *de = bus->procdir;

	if (!proc_initialized)
		return -EACCES;

	if (!de) {
		char name[16];
		sprintf(name, "%02x", bus->number);
		de = bus->procdir = proc_mkdir(name, proc_bus_pci_dir);
		if (!de)
			return -ENOMEM;
	}
	return 0;
}

int pci_proc_detach_bus(struct pci_bus* bus)
{
	struct proc_dir_entry *de = bus->procdir;
	if (de)
		remove_proc_entry(de->name, proc_bus_pci_dir);
	return 0;
}

#ifdef CONFIG_PCI_LEGACY_PROC

/*
 *  Backward compatible /proc/pci interface.
 */

/*
 * Convert some of the configuration space registers of the device at
 * address (bus,devfn) into a string (possibly several lines each).
 * The configuration string is stored starting at buf[len].  If the
 * string would exceed the size of the buffer (SIZE), 0 is returned.
 */
static int show_dev_config(struct seq_file *m, void *v)
{
	struct pci_dev *dev = v;
	struct pci_dev *first_dev;
	struct pci_driver *drv;
	u32 class_rev;
	unsigned char latency, min_gnt, max_lat, *class;
	int reg;

	first_dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, NULL);
	if (dev == first_dev)
		seq_puts(m, "PCI devices found:\n");
	pci_dev_put(first_dev);

	drv = pci_dev_driver(dev);

	pci_read_config_dword(dev, PCI_CLASS_REVISION, &class_rev);
	pci_read_config_byte (dev, PCI_LATENCY_TIMER, &latency);
	pci_read_config_byte (dev, PCI_MIN_GNT, &min_gnt);
	pci_read_config_byte (dev, PCI_MAX_LAT, &max_lat);
	seq_printf(m, "  Bus %2d, device %3d, function %2d:\n",
	       dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));
	class = pci_class_name(class_rev >> 16);
	if (class)
		seq_printf(m, "    %s", class);
	else
		seq_printf(m, "    Class %04x", class_rev >> 16);
#ifdef CONFIG_PCI_NAMES
	seq_printf(m, ": %s", dev->pretty_name);
#else
	seq_printf(m, ": PCI device %04x:%04x", dev->vendor, dev->device);
#endif
	seq_printf(m, " (rev %d).\n", class_rev & 0xff);

	if (dev->irq)
		seq_printf(m, "      IRQ %d.\n", dev->irq);

	if (latency || min_gnt || max_lat) {
		seq_printf(m, "      Master Capable.  ");
		if (latency)
			seq_printf(m, "Latency=%d.  ", latency);
		else
			seq_puts(m, "No bursts.  ");
		if (min_gnt)
			seq_printf(m, "Min Gnt=%d.", min_gnt);
		if (max_lat)
			seq_printf(m, "Max Lat=%d.", max_lat);
		seq_putc(m, '\n');
	}

	for (reg = 0; reg < 6; reg++) {
		struct resource *res = dev->resource + reg;
		unsigned long base, end, flags;

		base = res->start;
		end = res->end;
		flags = res->flags;
		if (!end)
			continue;

		if (flags & PCI_BASE_ADDRESS_SPACE_IO) {
			seq_printf(m, "      I/O at 0x%lx [0x%lx].\n",
				base, end);
		} else {
			const char *pref, *type = "unknown";

			if (flags & PCI_BASE_ADDRESS_MEM_PREFETCH)
				pref = "P";
			else
				pref = "Non-p";
			switch (flags & PCI_BASE_ADDRESS_MEM_TYPE_MASK) {
			      case PCI_BASE_ADDRESS_MEM_TYPE_32:
				type = "32 bit"; break;
			      case PCI_BASE_ADDRESS_MEM_TYPE_1M:
				type = "20 bit"; break;
			      case PCI_BASE_ADDRESS_MEM_TYPE_64:
				type = "64 bit"; break;
			}
			seq_printf(m, "      %srefetchable %s memory at "
				       "0x%lx [0x%lx].\n", pref, type,
				       base,
				       end);
		}
	}
	return 0;
}

static struct seq_operations proc_pci_op = {
	.start	= pci_seq_start,
	.next	= pci_seq_next,
	.stop	= pci_seq_stop,
	.show	= show_dev_config
};

static int proc_pci_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &proc_pci_op);
}
static struct file_operations proc_pci_operations = {
	.open		= proc_pci_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static void legacy_proc_init(void)
{
	struct proc_dir_entry * entry = create_proc_entry("pci", 0, NULL);
	if (entry)
		entry->proc_fops = &proc_pci_operations;
}

#else

static void legacy_proc_init(void)
{

}

#endif /* CONFIG_PCI_LEGACY_PROC */

static int proc_bus_pci_dev_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &proc_bus_pci_devices_op);
}
static struct file_operations proc_bus_pci_dev_operations = {
	.open		= proc_bus_pci_dev_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __init pci_proc_init(void)
{
	struct proc_dir_entry *entry;
	struct pci_dev *dev = NULL;
	proc_bus_pci_dir = proc_mkdir("pci", proc_bus);
	entry = create_proc_entry("devices", 0, proc_bus_pci_dir);
	if (entry)
		entry->proc_fops = &proc_bus_pci_dev_operations;
	proc_initialized = 1;
	while ((dev = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
		pci_proc_attach_device(dev);
	}
	legacy_proc_init();
	return 0;
}

__initcall(pci_proc_init);

#ifdef CONFIG_HOTPLUG
EXPORT_SYMBOL(pci_proc_attach_device);
EXPORT_SYMBOL(pci_proc_attach_bus);
EXPORT_SYMBOL(pci_proc_detach_bus);
EXPORT_SYMBOL(proc_bus_pci_dir);
#endif

