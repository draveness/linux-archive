/*
 * /dev/nvram driver for Power Macintosh.
 */

#define NVRAM_VERSION "1.0"

#include <linux/module.h>

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/fcntl.h>
#include <linux/nvram.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <asm/nvram.h>

#define NVRAM_SIZE	8192

static long long nvram_llseek(struct file *file, loff_t offset, int origin)
{
	switch (origin) {
	case 1:
		offset += file->f_pos;
		break;
	case 2:
		offset += NVRAM_SIZE;
		break;
	}
	if (offset < 0)
		return -EINVAL;
	file->f_pos = offset;
	return file->f_pos;
}

static ssize_t read_nvram(struct file *file, char *buf,
			  size_t count, loff_t *ppos)
{
	unsigned int i;
	char *p = buf;

	if (verify_area(VERIFY_WRITE, buf, count))
		return -EFAULT;
	if (*ppos >= NVRAM_SIZE)
		return 0;
	for (i = *ppos; count > 0 && i < NVRAM_SIZE; ++i, ++p, --count)
		if (__put_user(nvram_read_byte(i), p))
			return -EFAULT;
	*ppos = i;
	return p - buf;
}

static ssize_t write_nvram(struct file *file, const char *buf,
			   size_t count, loff_t *ppos)
{
	unsigned int i;
	const char *p = buf;
	char c;

	if (verify_area(VERIFY_READ, buf, count))
		return -EFAULT;
	if (*ppos >= NVRAM_SIZE)
		return 0;
	for (i = *ppos; count > 0 && i < NVRAM_SIZE; ++i, ++p, --count) {
		if (__get_user(c, p))
			return -EFAULT;
		nvram_write_byte(c, i);
	}
	*ppos = i;
	return p - buf;
}

static int nvram_ioctl(struct inode *inode, struct file *file,
	unsigned int cmd, unsigned long arg)
{
	switch(cmd) {
		case PMAC_NVRAM_GET_OFFSET:
		{
			int part, offset;
			if (copy_from_user(&part,(void*)arg,sizeof(part))!=0)
				return -EFAULT;
			if (part < pmac_nvram_OF || part > pmac_nvram_NR)
				return -EINVAL;
			offset = pmac_get_partition(part);
			if (copy_to_user((void*)arg,&offset,sizeof(offset))!=0)
				return -EFAULT;
			break;
		}

		default:
			return -EINVAL;
	}

	return 0;
}

struct file_operations nvram_fops = {
	owner:		THIS_MODULE,
	llseek:		nvram_llseek,
	read:		read_nvram,
	write:		write_nvram,
	ioctl:		nvram_ioctl,
};

static struct miscdevice nvram_dev = {
	NVRAM_MINOR,
	"nvram",
	&nvram_fops
};

int __init nvram_init(void)
{
	printk(KERN_INFO "Macintosh non-volatile memory driver v%s\n",
		NVRAM_VERSION);
	misc_register(&nvram_dev);
	return 0;
}

void __exit nvram_cleanup(void)
{
        misc_deregister( &nvram_dev );
}

module_init(nvram_init);
module_exit(nvram_cleanup);
