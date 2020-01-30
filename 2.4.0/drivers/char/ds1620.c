/*
 * linux/drivers/char/ds1620.c: Dallas Semiconductors DS1620
 *   thermometer driver (as used in the Rebel.com NetWinder)
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/miscdevice.h>
#include <linux/smp_lock.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/capability.h>
#include <linux/init.h>

#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/uaccess.h>
#include <asm/therm.h>

#ifdef CONFIG_PROC_FS
/* define for /proc interface */
#define THERM_USE_PROC
#endif

/* Definitions for DS1620 chip */
#define THERM_START_CONVERT	0xee
#define THERM_RESET		0xaf
#define THERM_READ_CONFIG	0xac
#define THERM_READ_TEMP		0xaa
#define THERM_READ_TL		0xa2
#define THERM_READ_TH		0xa1
#define THERM_WRITE_CONFIG	0x0c
#define THERM_WRITE_TL		0x02
#define THERM_WRITE_TH		0x01

#define CFG_CPU			2
#define CFG_1SHOT		1

static const char *fan_state[] = { "off", "on", "on (hardwired)" };

/*
 * Start of NetWinder specifics
 *  Note!  We have to hold the gpio lock with IRQs disabled over the
 *  whole of our transaction to the Dallas chip, since there is a
 *  chance that the WaveArtist driver could touch these bits to
 *  enable or disable the speaker.
 */
extern spinlock_t gpio_lock;
extern unsigned int system_rev;

static inline void netwinder_ds1620_set_clk(int clk)
{
	gpio_modify_op(GPIO_DSCLK, clk ? GPIO_DSCLK : 0);
}

static inline void netwinder_ds1620_set_data(int dat)
{
	gpio_modify_op(GPIO_DATA, dat ? GPIO_DATA : 0);
}

static inline int netwinder_ds1620_get_data(void)
{
	return gpio_read() & GPIO_DATA;
}

static inline void netwinder_ds1620_set_data_dir(int dir)
{
	gpio_modify_io(GPIO_DATA, dir ? GPIO_DATA : 0);
}

static inline void netwinder_ds1620_reset(void)
{
	cpld_modify(CPLD_DS_ENABLE, 0);
	cpld_modify(CPLD_DS_ENABLE, CPLD_DS_ENABLE);
}

static inline void netwinder_lock(unsigned long *flags)
{
	spin_lock_irqsave(&gpio_lock, *flags);
}

static inline void netwinder_unlock(unsigned long *flags)
{
	spin_unlock_irqrestore(&gpio_lock, *flags);
}

static inline void netwinder_set_fan(int i)
{
	unsigned long flags;

	spin_lock_irqsave(&gpio_lock, flags);
	gpio_modify_op(GPIO_FAN, i ? GPIO_FAN : 0);
	spin_unlock_irqrestore(&gpio_lock, flags);
}

static inline int netwinder_get_fan(void)
{
	if ((system_rev & 0xf000) == 0x4000)
		return FAN_ALWAYS_ON;

	return (gpio_read() & GPIO_FAN) ? FAN_ON : FAN_OFF;
}

/*
 * End of NetWinder specifics
 */

static void ds1620_send_bits(int nr, int value)
{
	int i;

	for (i = 0; i < nr; i++) {
		netwinder_ds1620_set_data(value & 1);
		netwinder_ds1620_set_clk(0);
		udelay(1);
		netwinder_ds1620_set_clk(1);
		udelay(1);

		value >>= 1;
	}
}

static unsigned int ds1620_recv_bits(int nr)
{
	unsigned int value = 0, mask = 1;
	int i;

	netwinder_ds1620_set_data(0);

	for (i = 0; i < nr; i++) {
		netwinder_ds1620_set_clk(0);
		udelay(1);

		if (netwinder_ds1620_get_data())
			value |= mask;

		mask <<= 1;

		netwinder_ds1620_set_clk(1);
		udelay(1);
	}

	return value;
}

static void ds1620_out(int cmd, int bits, int value)
{
	unsigned long flags;

	netwinder_lock(&flags);
	netwinder_ds1620_set_clk(1);
	netwinder_ds1620_set_data_dir(0);
	netwinder_ds1620_reset();

	udelay(1);

	ds1620_send_bits(8, cmd);
	if (bits)
		ds1620_send_bits(bits, value);

	udelay(1);

	netwinder_ds1620_reset();
	netwinder_unlock(&flags);

	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(2);
}

static unsigned int ds1620_in(int cmd, int bits)
{
	unsigned long flags;
	unsigned int value;

	netwinder_lock(&flags);
	netwinder_ds1620_set_clk(1);
	netwinder_ds1620_set_data_dir(0);
	netwinder_ds1620_reset();

	udelay(1);

	ds1620_send_bits(8, cmd);

	netwinder_ds1620_set_data_dir(1);
	value = ds1620_recv_bits(bits);

	netwinder_ds1620_reset();
	netwinder_unlock(&flags);

	return value;
}

static int cvt_9_to_int(unsigned int val)
{
	if (val & 0x100)
		val |= 0xfffffe00;

	return val;
}

static void ds1620_write_state(struct therm *therm)
{
	ds1620_out(THERM_WRITE_CONFIG, 8, CFG_CPU);
	ds1620_out(THERM_WRITE_TL, 9, therm->lo);
	ds1620_out(THERM_WRITE_TH, 9, therm->hi);
	ds1620_out(THERM_START_CONVERT, 0, 0);
}

static void ds1620_read_state(struct therm *therm)
{
	therm->lo = cvt_9_to_int(ds1620_in(THERM_READ_TL, 9));
	therm->hi = cvt_9_to_int(ds1620_in(THERM_READ_TH, 9));
}

static ssize_t
ds1620_read(struct file *file, char *buf, size_t count, loff_t *ptr)
{
	signed int cur_temp;
	signed char cur_temp_degF;

	/* Can't seek (pread) on this device */
	if (ptr != &file->f_pos)
		return -ESPIPE;

	cur_temp = cvt_9_to_int(ds1620_in(THERM_READ_TEMP, 9)) >> 1;

	/* convert to Fahrenheit, as per wdt.c */
	cur_temp_degF = (cur_temp * 9) / 5 + 32;

	if (copy_to_user(buf, &cur_temp_degF, 1))
		return -EFAULT;

	return 1;
}

static int
ds1620_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	struct therm therm;
	int i;

	switch(cmd) {
	case CMD_SET_THERMOSTATE:
	case CMD_SET_THERMOSTATE2:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;

		if (cmd == CMD_SET_THERMOSTATE) {
			if (get_user(therm.hi, (int *)arg))
				return -EFAULT;
			therm.lo = therm.hi - 3;
		} else {
			if (copy_from_user(&therm, (void *)arg, sizeof(therm)))
				return -EFAULT;
		}

		therm.lo <<= 1;
		therm.hi <<= 1;

		ds1620_write_state(&therm);
		break;

	case CMD_GET_THERMOSTATE:
	case CMD_GET_THERMOSTATE2:
		ds1620_read_state(&therm);

		therm.lo >>= 1;
		therm.hi >>= 1;

		if (cmd == CMD_GET_THERMOSTATE) {
			if (put_user(therm.hi, (int *)arg))
				return -EFAULT;
		} else {
			if (copy_to_user((void *)arg, &therm, sizeof(therm)))
				return -EFAULT;
		}
		break;

	case CMD_GET_TEMPERATURE:
	case CMD_GET_TEMPERATURE2:
		i = cvt_9_to_int(ds1620_in(THERM_READ_TEMP, 9));

		if (cmd == CMD_GET_TEMPERATURE)
			i >>= 1;

		return put_user(i, (int *)arg) ? -EFAULT : 0;

	case CMD_GET_STATUS:
		i = ds1620_in(THERM_READ_CONFIG, 8) & 0xe3;

		return put_user(i, (int *)arg) ? -EFAULT : 0;

	case CMD_GET_FAN:
		i = netwinder_get_fan();

		return put_user(i, (int *)arg) ? -EFAULT : 0;

	case CMD_SET_FAN:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;

		if (get_user(i, (int *)arg))
			return -EFAULT;

		netwinder_set_fan(i);
		break;
		
	default:
		return -ENOIOCTLCMD;
	}

	return 0;
}

#ifdef THERM_USE_PROC
static int
proc_therm_ds1620_read(char *buf, char **start, off_t offset,
		       int len, int *eof, void *unused)
{
	struct therm th;
	int temp;

	ds1620_read_state(&th);
	temp =  cvt_9_to_int(ds1620_in(THERM_READ_TEMP, 9));

	len = sprintf(buf, "Thermostat: HI %i.%i, LOW %i.%i; "
		      "temperature: %i.%i C, fan %s\n",
		      th.hi >> 1, th.hi & 1 ? 5 : 0,
		      th.lo >> 1, th.lo & 1 ? 5 : 0,
		      temp  >> 1, temp  & 1 ? 5 : 0,
		      fan_state[netwinder_get_fan()]);

	return len;
}

static struct proc_dir_entry *proc_therm_ds1620;
#endif

static struct file_operations ds1620_fops = {
	owner:		THIS_MODULE,
	read:		ds1620_read,
	ioctl:		ds1620_ioctl,
};

static struct miscdevice ds1620_miscdev = {
	TEMP_MINOR,
	"temp",
	&ds1620_fops
};

int __init ds1620_init(void)
{
	int ret;
	struct therm th, th_start;

	if (!machine_is_netwinder())
		return -ENODEV;

	ds1620_out(THERM_RESET, 0, 0);
	ds1620_out(THERM_WRITE_CONFIG, 8, CFG_CPU);
	ds1620_out(THERM_START_CONVERT, 0, 0);

	/*
	 * Trigger the fan to start by setting
	 * temperature high point low.  This kicks
	 * the fan into action.
	 */
	ds1620_read_state(&th);
	th_start.lo = 0;
	th_start.hi = 1;
	ds1620_write_state(&th_start);

	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(2*HZ);

	ds1620_write_state(&th);

	ret = misc_register(&ds1620_miscdev);
	if (ret < 0)
		return ret;

#ifdef THERM_USE_PROC
	proc_therm_ds1620 = create_proc_entry("therm", 0, 0);
	if (proc_therm_ds1620)
		proc_therm_ds1620->read_proc = proc_therm_ds1620_read;
	else
		printk(KERN_ERR "therm: unable to register /proc/therm\n");
#endif

	ds1620_read_state(&th);
	ret = cvt_9_to_int(ds1620_in(THERM_READ_TEMP, 9));

	printk(KERN_INFO "Thermostat: high %i.%i, low %i.%i, "
	       "current %i.%i C, fan %s.\n",
	       th.hi >> 1, th.hi & 1 ? 5 : 0,
	       th.lo >> 1, th.lo & 1 ? 5 : 0,
	       ret   >> 1, ret   & 1 ? 5 : 0,
	       fan_state[netwinder_get_fan()]);

	return 0;
}

void __exit ds1620_exit(void)
{
#ifdef THERM_USE_PROC
	remove_proc_entry("therm", NULL);
#endif
	misc_deregister(&ds1620_miscdev);
}

module_init(ds1620_init);
module_exit(ds1620_exit);
