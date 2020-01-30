/*
 * Flash memory interface rev.5 driver for the Intel
 * Flash chips used on the NetWinder.
 *
 * 20/08/2000	RMK	use __ioremap to map flash into virtual memory
 *			make a few more places use "volatile"
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/init.h>

#include <asm/hardware/dec21285.h>
#include <asm/io.h>
#include <asm/leds.h>
#include <asm/mach-types.h>
#include <asm/system.h>
#include <asm/uaccess.h>

/*****************************************************************************/
#include <asm/nwflash.h>

//#define MINIKERNEL 1          //export flash write, erase routines for MiniKernel

#ifndef MINIKERNEL
#define MSTATIC static
#else
#define MSTATIC
#endif

#define	NWFLASH_VERSION "6.3"

MSTATIC void kick_open(void);
MSTATIC int get_flash_id(void);
MSTATIC int erase_block(int nBlock);
MSTATIC int write_block(unsigned long p, const char *buf, int count);
static int open_flash(struct inode *inodep, struct file *filep);
static int flash_ioctl(struct inode *inodep, struct file *filep, unsigned int cmd, unsigned long arg);
static ssize_t flash_read(struct file *file, char *buf, size_t count, loff_t * ppos);
static ssize_t flash_write(struct file *file, const char *buf, size_t count, loff_t * ppos);
static long long flash_llseek(struct file *file, long long offset, int orig);

#define KFLASH_SIZE	1024*1024	//1 Meg
#define KFLASH_SIZE4	4*1024*1024	//4 Meg
#define KFLASH_ID	0x89A6		//Intel flash
#define KFLASH_ID4	0xB0D4		//Intel flash 4Meg

static int flashdebug;		//if set - we will display progress msgs

static int gbWriteEnable;
static int gbWriteBase64Enable;
static volatile unsigned char *FLASH_BASE;
MSTATIC int gbFlashSize = KFLASH_SIZE;

extern spinlock_t gpio_lock;

static struct file_operations flash_fops =
{
	owner:		THIS_MODULE,
	llseek:		flash_llseek,
	read:		flash_read,
	write:		flash_write,
	ioctl:		flash_ioctl,
	open:		open_flash,
};

static struct miscdevice flash_miscdev =
{
	FLASH_MINOR,
	"nwflash",
	&flash_fops
};

/*
 * the delay routine - it is often required to let the flash "breeze"...
 */
void flash_wait(int timeout)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule_timeout(timeout);
}

MSTATIC int get_flash_id(void)
{
	volatile unsigned int c1, c2;

	/*
	 * try to get flash chip ID
	 */
	kick_open();
	c2 = inb(0x80);
	*(volatile unsigned char *) (FLASH_BASE + 0x8000) = 0x90;
	udelay(15);
	c1 = *(volatile unsigned char *) FLASH_BASE;
	c2 = inb(0x80);

	/*
	 * on 4 Meg flash the second byte is actually at offset 2...
	 */
	if (c1 == 0xB0)
		c2 = *(volatile unsigned char *) (FLASH_BASE + 2);
	else
		c2 = *(volatile unsigned char *) (FLASH_BASE + 1);

	c2 += (c1 << 8);

	/*
	 * set it back to read mode
	 */
	*(volatile unsigned char *) (FLASH_BASE + 0x8000) = 0xFF;

	if (c2 == KFLASH_ID4)
		gbFlashSize = KFLASH_SIZE4;

	return c2;
}

static int open_flash(struct inode *inodep, struct file *filep)
{
	int id;

	id = get_flash_id();
	if ((id != KFLASH_ID) && (id != KFLASH_ID4)) {
		printk("Flash: incorrect ID 0x%04X.\n", id);
		return -ENXIO;
	}

	return 0;
}

static int flash_ioctl(struct inode *inodep, struct file *filep, unsigned int cmd, unsigned long arg)
{
//      printk("Flash_ioctl: cmd = 0x%X.\n",cmd);

	switch (cmd) {
	case CMD_WRITE_DISABLE:
		gbWriteBase64Enable = 0;
		gbWriteEnable = 0;
		break;

	case CMD_WRITE_ENABLE:
		gbWriteEnable = 1;
		break;

	case CMD_WRITE_BASE64K_ENABLE:
		gbWriteBase64Enable = 1;
		break;

	default:
		gbWriteBase64Enable = 0;
		gbWriteEnable = 0;
		return -EINVAL;
	}

	return 0;
}



static ssize_t flash_read(struct file *file, char *buf, size_t count, loff_t * ppos)
{
	unsigned long p = file->f_pos;
	int read;

	if (flashdebug)
		printk("Flash_dev: flash_read: offset=0x%X, buffer=0x%X, count=0x%X.\n",
		       (unsigned int) p, (unsigned int) buf, count);


	if (count < 0)
		return -EINVAL;

	if (count > gbFlashSize - p)
		count = gbFlashSize - p;

	read = 0;

	if (copy_to_user(buf, (void *)(FLASH_BASE + p), count))
		return -EFAULT;
	read += count;
	file->f_pos += read;
	return read;
}

static ssize_t flash_write(struct file *file, const char *buf, size_t count, loff_t * ppos)
{
	struct inode *inode = file->f_dentry->d_inode;
	unsigned long p = file->f_pos;
	int written;
	int nBlock, temp, rc;
	int i, j;

	if (flashdebug)
		printk("flash_write: offset=0x%lX, buffer=0x%p, count=0x%X.\n",
		       p, buf, count);

	if (!gbWriteEnable)
		return -EINVAL;

	if (p < 64 * 1024 && (!gbWriteBase64Enable))
		return -EINVAL;

	/*
	 * if byte count is -ve or to big - error!
	 */
	if (count < 0 || count > gbFlashSize - p)
		return -EINVAL;

	if (verify_area(VERIFY_READ, buf, count))
		return -EFAULT;


	/*
	 * We now should lock around writes.  Really, we shouldn't
	 * allow the flash to be opened more than once in write
	 * mode though (note that you can't stop two processes having
	 * it open even then). --rmk
	 */
	if (down_interruptible(&inode->i_sem))
		return -ERESTARTSYS;

	written = 0;

	leds_event(led_claim);
	leds_event(led_green_on);

	nBlock = (int) p >> 16;	//block # of 64K bytes

	/*
	 * # of 64K blocks to erase and write
	 */
	temp = ((int) (p + count) >> 16) - nBlock + 1;

	/*
	 * write ends at exactly 64k boundry?
	 */
	if (((int) (p + count) & 0xFFFF) == 0)
		temp -= 1;

	if (flashdebug)
		printk("FlashWrite: writing %d block(s) starting at %d.\n", temp, nBlock);

	for (; temp; temp--, nBlock++) {
		if (flashdebug)
			printk("FlashWrite: erasing block %d.\n", nBlock);

		/*
		 * first we have to erase the block(s), where we will write...
		 */
		i = 0;
		j = 0;
	  RetryBlock:
		do {
			rc = erase_block(nBlock);
			i++;
		} while (rc && i < 10);

		if (rc) {
			if (flashdebug)
				printk("FlashWrite: erase error %X. Aborting...\n", rc);

			break;
		}
		if (flashdebug)
			printk("FlashWrite: writing offset %X, from buf %X, bytes left %X.\n",
			       (unsigned int) p, (unsigned int) buf, count - written);

		/*
		 * write_block will limit write to space left in this block
		 */
		rc = write_block(p, buf, count - written);
		j++;

		/*
		 * if somehow write verify failed? Can't happen??
		 */
		if (!rc) {
			/*
			 * retry up to 10 times
			 */
			if (j < 10)
				goto RetryBlock;
			else
				/*
				 * else quit with error...
				 */
				rc = -1;

		}
		if (rc < 0) {
			if (flashdebug)
				printk("FlashWrite: write error %X. Aborting...\n", rc);
			break;
		}
		p += rc;
		buf += rc;
		written += rc;
		file->f_pos += rc;

		if (flashdebug)
			printk("FlashWrite: written 0x%X bytes OK.\n", written);
	}

	/*
	 * restore reg on exit
	 */
	leds_event(led_release);

	up(&inode->i_sem);

	return written;
}


/*
 * The memory devices use the full 32/64 bits of the offset, and so we cannot
 * check against negative addresses: they are ok. The return value is weird,
 * though, in that case (0).
 *
 * also note that seeking relative to the "end of file" isn't supported:
 * it has no meaning, so it returns -EINVAL.
 */
static long long flash_llseek(struct file *file, long long offset, int orig)
{
	if (flashdebug)
		printk("Flash_dev: flash_lseek, offset=0x%X, orig=0x%X.\n",
		       (unsigned int) offset, (unsigned int) orig);

	switch (orig) {
	case 0:
		if (offset < 0)
			return -EINVAL;

		if ((unsigned int) offset > gbFlashSize)
			return -EINVAL;

		file->f_pos = (unsigned int) offset;
		return file->f_pos;
	case 1:
		if ((file->f_pos + offset) > gbFlashSize)
			return -EINVAL;
		if ((file->f_pos + offset) < 0)
			return -EINVAL;
		file->f_pos += offset;
		return file->f_pos;
	default:
		return -EINVAL;
	}
}


/*
 * assume that main Write routine did the parameter checking...
 * so just go ahead and erase, what requested!
 */

MSTATIC int erase_block(int nBlock)
{
	volatile unsigned int c1;
	volatile unsigned char *pWritePtr;
	int temp, temp1;

	/*
	 * orange LED == erase
	 */
	leds_event(led_amber_on);

	/*
	 * reset footbridge to the correct offset 0 (...0..3)
	 */
	*CSR_ROMWRITEREG = 0;

	/*
	 * dummy ROM read
	 */
	c1 = *(volatile unsigned char *) (FLASH_BASE + 0x8000);

	kick_open();
	/*
	 * reset status if old errors
	 */
	*(volatile unsigned char *) (FLASH_BASE + 0x8000) = 0x50;

	/*
	 * erase a block...
	 * aim at the middle of a current block...
	 */
	pWritePtr = (unsigned char *) ((unsigned int) (FLASH_BASE + 0x8000 + (nBlock << 16)));
	/*
	 * dummy read
	 */
	c1 = *pWritePtr;

	kick_open();
	/*
	 * erase
	 */
	*(volatile unsigned char *) pWritePtr = 0x20;

	/*
	 * confirm
	 */
	*(volatile unsigned char *) pWritePtr = 0xD0;

	/*
	 * wait 10 ms
	 */
	flash_wait(HZ / 100);

	/*
	 * wait while erasing in process (up to 10 sec)
	 */
	temp = jiffies + 10 * HZ;
	c1 = 0;
	while (!(c1 & 0x80) && time_before(jiffies, temp)) {
		flash_wait(HZ / 100);
		/*
		 * read any address
		 */
		c1 = *(volatile unsigned char *) (pWritePtr);
		//              printk("Flash_erase: status=%X.\n",c1);
	}

	/*
	 * set flash for normal read access
	 */
	kick_open();
//      *(volatile unsigned char*)(FLASH_BASE+0x8000) = 0xFF;
	*(volatile unsigned char *) pWritePtr = 0xFF;	//back to normal operation

	/*
	 * check if erase errors were reported
	 */
	if (c1 & 0x20) {
		if (flashdebug)
			printk("Flash_erase: err at %X.\n", (unsigned int) pWritePtr);
		/*
		 * reset error
		 */
		*(volatile unsigned char *) (FLASH_BASE + 0x8000) = 0x50;

		return -2;
	}

	/*
	 * just to make sure - verify if erased OK...
	 */
	flash_wait(HZ / 100);

	pWritePtr = (unsigned char *) ((unsigned int) (FLASH_BASE + (nBlock << 16)));

	for (temp = 0; temp < 16 * 1024; temp++, pWritePtr += 4) {
		if ((temp1 = *(volatile unsigned int *) pWritePtr) != 0xFFFFFFFF) {
			if (flashdebug)
				printk("Flash_erase: verify err at %X = %X.\n",
				       (unsigned int) pWritePtr, temp1);
			return -1;
		}
	}

	return 0;

}

/*
 * write_block will limit number of bytes written to the space in this block
 */
MSTATIC int write_block(unsigned long p, const char *buf, int count)
{
	volatile unsigned int c1;
	volatile unsigned int c2;
	unsigned char *pWritePtr;
	unsigned int uAddress;
	unsigned int offset;
	unsigned int timeout;
	unsigned int timeout1;

	/*
	 * red LED == write
	 */
	leds_event(led_amber_off);
	leds_event(led_red_on);

	pWritePtr = (unsigned char *) ((unsigned int) (FLASH_BASE + p));

	/*
	 * check if write will end in this block....
	 */
	offset = p & 0xFFFF;

	if (offset + count > 0x10000)
		count = 0x10000 - offset;

	/*
	 * wait up to 30 sec for this block
	 */
	timeout = jiffies + 30 * HZ;

	for (offset = 0; offset < count; offset++, pWritePtr++) {
		uAddress = (unsigned int) pWritePtr;
		uAddress &= 0xFFFFFFFC;
		if (__get_user(c2, buf + offset))
			return -EFAULT;

	  WriteRetry:
	  	/*
	  	 * dummy read
	  	 */
		c1 = *(volatile unsigned char *) (FLASH_BASE + 0x8000);

		/*
		 * kick open the write gate
		 */
		kick_open();

		/*
		 * program footbridge to the correct offset...0..3
		 */
		*CSR_ROMWRITEREG = (unsigned int) pWritePtr & 3;

		/*
		 * write cmd
		 */
		*(volatile unsigned char *) (uAddress) = 0x40;

		/*
		 * data to write
		 */
		*(volatile unsigned char *) (uAddress) = c2;

		/*
		 * get status
		 */
		*(volatile unsigned char *) (FLASH_BASE + 0x10000) = 0x70;

		c1 = 0;

		/*
		 * wait up to 1 sec for this byte
		 */
		timeout1 = jiffies + 1 * HZ;

		/*
		 * while not ready...
		 */
		while (!(c1 & 0x80) && time_before(jiffies, timeout1))
			c1 = *(volatile unsigned char *) (FLASH_BASE + 0x8000);

		/*
		 * if timeout getting status
		 */
		if (time_after_eq(jiffies, timeout1)) {
			kick_open();
			/*
			 * reset err
			 */
			*(volatile unsigned char *) (FLASH_BASE + 0x8000) = 0x50;

			goto WriteRetry;
		}
		/*
		 * switch on read access, as a default flash operation mode
		 */
		kick_open();
		/*
		 * read access
		 */
		*(volatile unsigned char *) (FLASH_BASE + 0x8000) = 0xFF;

		/*
		 * if hardware reports an error writing, and not timeout - 
		 * reset the chip and retry
		 */
		if (c1 & 0x10) {
			kick_open();
			/*
			 * reset err
			 */
			*(volatile unsigned char *) (FLASH_BASE + 0x8000) = 0x50;

			/*
			 * before timeout?
			 */
			if (time_before(jiffies, timeout)) {
				if (flashdebug)
					printk("FlashWrite: Retrying write (addr=0x%X)...\n",
					       pWritePtr - FLASH_BASE);

				/*
				 * no LED == waiting
				 */
				leds_event(led_amber_off);
				/*
				 * wait couple ms
				 */
				flash_wait(HZ / 100);
				/*
				 * red LED == write
				 */
				leds_event(led_red_on);

				goto WriteRetry;
			} else {
				printk("Timeout in flash write! (addr=0x%X) Aborting...\n",
				       pWritePtr - FLASH_BASE);
				/*
				 * return error -2
				 */
				return -2;

			}
		}
	}

	/*
	 * green LED == read/verify
	 */
	leds_event(led_amber_off);
	leds_event(led_green_on);

	flash_wait(HZ / 100);

	pWritePtr = (unsigned char *) ((unsigned int) (FLASH_BASE + p));

	for (offset = 0; offset < count; offset++) {
		char c, c1;
		if (__get_user(c, buf))
			return -EFAULT;
		buf++;
		if ((c1 = *pWritePtr++) != c) {
			if (flashdebug)
				printk("flash write verify error at 0x%X! (%02X!=%02X) Retrying...\n",
				       (unsigned int) pWritePtr, c1, c);
			return 0;
		}
	}

	return count;
}


MSTATIC void kick_open(void)
{
	unsigned long flags;

	/*
	 * we want to write a bit pattern XXX1 to Xilinx to enable
	 * the write gate, which will be open for about the next 2ms.
	 */
	spin_lock_irqsave(&gpio_lock, flags);
	cpld_modify(1, 1);
	spin_unlock_irqrestore(&gpio_lock, flags);

	/*
	 * let the ISA bus to catch on...
	 */
	udelay(25);
}

MSTATIC int __init nwflash_init(void)
{
	int ret = -ENODEV;

	if (machine_is_netwinder()) {
		int id;

		FLASH_BASE = __ioremap(DC21285_FLASH, KFLASH_SIZE4, 0);
		if (!FLASH_BASE)
			goto out;

		id = get_flash_id();
		printk("Flash ROM driver v.%s, flash device ID 0x%04X, size %d Mb.\n",
		       NWFLASH_VERSION, id, gbFlashSize / (1024 * 1024));

		misc_register(&flash_miscdev);

		ret = 0;
	}
out:
	return ret;
}

MSTATIC void __exit nwflash_exit(void)
{
	misc_deregister(&flash_miscdev);
	iounmap((void *)FLASH_BASE);
}

EXPORT_NO_SYMBOLS;

MODULE_PARM(flashdebug, "i");

module_init(nwflash_init);
module_exit(nwflash_exit);
