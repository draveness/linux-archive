#include <linux/module.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/fd.h>
#include <linux/tty.h>
#include <linux/suspend.h>
#include <linux/root_dev.h>
#include <linux/security.h>
#include <linux/delay.h>
#include <linux/genhd.h>
#include <linux/mount.h>
#include <linux/device.h>
#include <linux/init.h>

#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>
#include <linux/nfs_mount.h>

#include "do_mounts.h"

extern int get_filesystem_list(char * buf);

int __initdata rd_doload;	/* 1 = load RAM disk, 0 = don't load */

int root_mountflags = MS_RDONLY | MS_SILENT;
char * __initdata root_device_name;
static char __initdata saved_root_name[64];
static int __initdata root_wait;

dev_t ROOT_DEV;

static int __init load_ramdisk(char *str)
{
	rd_doload = simple_strtol(str,NULL,0) & 3;
	return 1;
}
__setup("load_ramdisk=", load_ramdisk);

static int __init readonly(char *str)
{
	if (*str)
		return 0;
	root_mountflags |= MS_RDONLY;
	return 1;
}

static int __init readwrite(char *str)
{
	if (*str)
		return 0;
	root_mountflags &= ~MS_RDONLY;
	return 1;
}

__setup("ro", readonly);
__setup("rw", readwrite);

static dev_t try_name(char *name, int part)
{
	char path[64];
	char buf[32];
	int range;
	dev_t res;
	char *s;
	int len;
	int fd;
	unsigned int maj, min;

	/* read device number from .../dev */

	sprintf(path, "/sys/block/%s/dev", name);
	fd = sys_open(path, 0, 0);
	if (fd < 0)
		goto fail;
	len = sys_read(fd, buf, 32);
	sys_close(fd);
	if (len <= 0 || len == 32 || buf[len - 1] != '\n')
		goto fail;
	buf[len - 1] = '\0';
	if (sscanf(buf, "%u:%u", &maj, &min) == 2) {
		/*
		 * Try the %u:%u format -- see print_dev_t()
		 */
		res = MKDEV(maj, min);
		if (maj != MAJOR(res) || min != MINOR(res))
			goto fail;
	} else {
		/*
		 * Nope.  Try old-style "0321"
		 */
		res = new_decode_dev(simple_strtoul(buf, &s, 16));
		if (*s)
			goto fail;
	}

	/* if it's there and we are not looking for a partition - that's it */
	if (!part)
		return res;

	/* otherwise read range from .../range */
	sprintf(path, "/sys/block/%s/range", name);
	fd = sys_open(path, 0, 0);
	if (fd < 0)
		goto fail;
	len = sys_read(fd, buf, 32);
	sys_close(fd);
	if (len <= 0 || len == 32 || buf[len - 1] != '\n')
		goto fail;
	buf[len - 1] = '\0';
	range = simple_strtoul(buf, &s, 10);
	if (*s)
		goto fail;

	/* if partition is within range - we got it */
	if (part < range)
		return res + part;
fail:
	return 0;
}

/*
 *	Convert a name into device number.  We accept the following variants:
 *
 *	1) device number in hexadecimal	represents itself
 *	2) /dev/nfs represents Root_NFS (0xff)
 *	3) /dev/<disk_name> represents the device number of disk
 *	4) /dev/<disk_name><decimal> represents the device number
 *         of partition - device number of disk plus the partition number
 *	5) /dev/<disk_name>p<decimal> - same as the above, that form is
 *	   used when disk name of partitioned disk ends on a digit.
 *
 *	If name doesn't have fall into the categories above, we return 0.
 *	Sysfs is used to check if something is a disk name - it has
 *	all known disks under bus/block/devices.  If the disk name
 *	contains slashes, name of sysfs node has them replaced with
 *	bangs.  try_name() does the actual checks, assuming that sysfs
 *	is mounted on rootfs /sys.
 */

dev_t name_to_dev_t(char *name)
{
	char s[32];
	char *p;
	dev_t res = 0;
	int part;

#ifdef CONFIG_SYSFS
	int mkdir_err = sys_mkdir("/sys", 0700);
	if (sys_mount("sysfs", "/sys", "sysfs", 0, NULL) < 0)
		goto out;
#endif

	if (strncmp(name, "/dev/", 5) != 0) {
		unsigned maj, min;

		if (sscanf(name, "%u:%u", &maj, &min) == 2) {
			res = MKDEV(maj, min);
			if (maj != MAJOR(res) || min != MINOR(res))
				goto fail;
		} else {
			res = new_decode_dev(simple_strtoul(name, &p, 16));
			if (*p)
				goto fail;
		}
		goto done;
	}
	name += 5;
	res = Root_NFS;
	if (strcmp(name, "nfs") == 0)
		goto done;
	res = Root_RAM0;
	if (strcmp(name, "ram") == 0)
		goto done;

	if (strlen(name) > 31)
		goto fail;
	strcpy(s, name);
	for (p = s; *p; p++)
		if (*p == '/')
			*p = '!';
	res = try_name(s, 0);
	if (res)
		goto done;

	while (p > s && isdigit(p[-1]))
		p--;
	if (p == s || !*p || *p == '0')
		goto fail;
	part = simple_strtoul(p, NULL, 10);
	*p = '\0';
	res = try_name(s, part);
	if (res)
		goto done;

	if (p < s + 2 || !isdigit(p[-2]) || p[-1] != 'p')
		goto fail;
	p[-1] = '\0';
	res = try_name(s, part);
done:
#ifdef CONFIG_SYSFS
	sys_umount("/sys", 0);
out:
	if (!mkdir_err)
		sys_rmdir("/sys");
#endif
	return res;
fail:
	res = 0;
	goto done;
}

static int __init root_dev_setup(char *line)
{
	strlcpy(saved_root_name, line, sizeof(saved_root_name));
	return 1;
}

__setup("root=", root_dev_setup);

static int __init rootwait_setup(char *str)
{
	if (*str)
		return 0;
	root_wait = 1;
	return 1;
}

__setup("rootwait", rootwait_setup);

static char * __initdata root_mount_data;
static int __init root_data_setup(char *str)
{
	root_mount_data = str;
	return 1;
}

static char * __initdata root_fs_names;
static int __init fs_names_setup(char *str)
{
	root_fs_names = str;
	return 1;
}

static unsigned int __initdata root_delay;
static int __init root_delay_setup(char *str)
{
	root_delay = simple_strtoul(str, NULL, 0);
	return 1;
}

__setup("rootflags=", root_data_setup);
__setup("rootfstype=", fs_names_setup);
__setup("rootdelay=", root_delay_setup);

static void __init get_fs_names(char *page)
{
	char *s = page;

	if (root_fs_names) {
		strcpy(page, root_fs_names);
		while (*s++) {
			if (s[-1] == ',')
				s[-1] = '\0';
		}
	} else {
		int len = get_filesystem_list(page);
		char *p, *next;

		page[len] = '\0';
		for (p = page-1; p; p = next) {
			next = strchr(++p, '\n');
			if (*p++ != '\t')
				continue;
			while ((*s++ = *p++) != '\n')
				;
			s[-1] = '\0';
		}
	}
	*s = '\0';
}

static int __init do_mount_root(char *name, char *fs, int flags, void *data)
{
	int err = sys_mount(name, "/root", fs, flags, data);
	if (err)
		return err;

	sys_chdir("/root");
	ROOT_DEV = current->fs->pwdmnt->mnt_sb->s_dev;
	printk("VFS: Mounted root (%s filesystem)%s.\n",
	       current->fs->pwdmnt->mnt_sb->s_type->name,
	       current->fs->pwdmnt->mnt_sb->s_flags & MS_RDONLY ? 
	       " readonly" : "");
	return 0;
}

void __init mount_block_root(char *name, int flags)
{
	char *fs_names = __getname();
	char *p;
#ifdef CONFIG_BLOCK
	char b[BDEVNAME_SIZE];
#else
	const char *b = name;
#endif

	get_fs_names(fs_names);
retry:
	for (p = fs_names; *p; p += strlen(p)+1) {
		int err = do_mount_root(name, p, flags, root_mount_data);
		switch (err) {
			case 0:
				goto out;
			case -EACCES:
				flags |= MS_RDONLY;
				goto retry;
			case -EINVAL:
				continue;
		}
	        /*
		 * Allow the user to distinguish between failed sys_open
		 * and bad superblock on root device.
		 * and give them a list of the available devices
		 */
#ifdef CONFIG_BLOCK
		__bdevname(ROOT_DEV, b);
#endif
		printk("VFS: Cannot open root device \"%s\" or %s\n",
				root_device_name, b);
		printk("Please append a correct \"root=\" boot option; here are the available partitions:\n");

		printk_all_partitions();
		panic("VFS: Unable to mount root fs on %s", b);
	}

	printk("List of all partitions:\n");
	printk_all_partitions();
	printk("No filesystem could mount root, tried: ");
	for (p = fs_names; *p; p += strlen(p)+1)
		printk(" %s", p);
	printk("\n");
#ifdef CONFIG_BLOCK
	__bdevname(ROOT_DEV, b);
#endif
	panic("VFS: Unable to mount root fs on %s", b);
out:
	putname(fs_names);
}
 
#ifdef CONFIG_ROOT_NFS
static int __init mount_nfs_root(void)
{
	void *data = nfs_root_data();

	create_dev("/dev/root", ROOT_DEV);
	if (data &&
	    do_mount_root("/dev/root", "nfs", root_mountflags, data) == 0)
		return 1;
	return 0;
}
#endif

#if defined(CONFIG_BLK_DEV_RAM) || defined(CONFIG_BLK_DEV_FD)
void __init change_floppy(char *fmt, ...)
{
	struct termios termios;
	char buf[80];
	char c;
	int fd;
	va_list args;
	va_start(args, fmt);
	vsprintf(buf, fmt, args);
	va_end(args);
	fd = sys_open("/dev/root", O_RDWR | O_NDELAY, 0);
	if (fd >= 0) {
		sys_ioctl(fd, FDEJECT, 0);
		sys_close(fd);
	}
	printk(KERN_NOTICE "VFS: Insert %s and press ENTER\n", buf);
	fd = sys_open("/dev/console", O_RDWR, 0);
	if (fd >= 0) {
		sys_ioctl(fd, TCGETS, (long)&termios);
		termios.c_lflag &= ~ICANON;
		sys_ioctl(fd, TCSETSF, (long)&termios);
		sys_read(fd, &c, 1);
		termios.c_lflag |= ICANON;
		sys_ioctl(fd, TCSETSF, (long)&termios);
		sys_close(fd);
	}
}
#endif

void __init mount_root(void)
{
#ifdef CONFIG_ROOT_NFS
	if (MAJOR(ROOT_DEV) == UNNAMED_MAJOR) {
		if (mount_nfs_root())
			return;

		printk(KERN_ERR "VFS: Unable to mount root fs via NFS, trying floppy.\n");
		ROOT_DEV = Root_FD0;
	}
#endif
#ifdef CONFIG_BLK_DEV_FD
	if (MAJOR(ROOT_DEV) == FLOPPY_MAJOR) {
		/* rd_doload is 2 for a dual initrd/ramload setup */
		if (rd_doload==2) {
			if (rd_load_disk(1)) {
				ROOT_DEV = Root_RAM1;
				root_device_name = NULL;
			}
		} else
			change_floppy("root floppy");
	}
#endif
#ifdef CONFIG_BLOCK
	create_dev("/dev/root", ROOT_DEV);
	mount_block_root("/dev/root", root_mountflags);
#endif
}

/*
 * Prepare the namespace - decide what/where to mount, load ramdisks, etc.
 */
void __init prepare_namespace(void)
{
	int is_floppy;

	if (root_delay) {
		printk(KERN_INFO "Waiting %dsec before mounting root device...\n",
		       root_delay);
		ssleep(root_delay);
	}

	/* wait for the known devices to complete their probing */
	while (driver_probe_done() != 0)
		msleep(100);

	md_run_setup();

	if (saved_root_name[0]) {
		root_device_name = saved_root_name;
		if (!strncmp(root_device_name, "mtd", 3)) {
			mount_block_root(root_device_name, root_mountflags);
			goto out;
		}
		ROOT_DEV = name_to_dev_t(root_device_name);
		if (strncmp(root_device_name, "/dev/", 5) == 0)
			root_device_name += 5;
	}

	if (initrd_load())
		goto out;

	/* wait for any asynchronous scanning to complete */
	if ((ROOT_DEV == 0) && root_wait) {
		printk(KERN_INFO "Waiting for root device %s...\n",
			saved_root_name);
		while (driver_probe_done() != 0 ||
			(ROOT_DEV = name_to_dev_t(saved_root_name)) == 0)
			msleep(100);
	}

	is_floppy = MAJOR(ROOT_DEV) == FLOPPY_MAJOR;

	if (is_floppy && rd_doload && rd_load_disk(0))
		ROOT_DEV = Root_RAM0;

	mount_root();
out:
	sys_mount(".", "/", NULL, MS_MOVE, NULL);
	sys_chroot(".");
	security_sb_post_mountroot();
}

