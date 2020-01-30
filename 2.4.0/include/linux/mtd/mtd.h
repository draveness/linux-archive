
/* $Id: mtd.h,v 1.26 2000/10/30 17:18:04 sjhill Exp $ */

#ifndef __MTD_MTD_H__
#define __MTD_MTD_H__

#ifdef __KERNEL__

#include <linux/config.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/mtd/compatmac.h>
#include <linux/module.h>
#include <linux/uio.h>

#endif /* __KERNEL__ */

struct erase_info_user {
	unsigned long start;
	unsigned long length;
};

struct mtd_oob_buf {
	loff_t start;
	ssize_t length;
	unsigned char *ptr;
};


#define MTD_CHAR_MAJOR 90
#define MTD_BLOCK_MAJOR 31
#define MAX_MTD_DEVICES 16



#define MTD_ABSENT		0
#define MTD_RAM			1
#define MTD_ROM			2
#define MTD_NORFLASH		3
#define MTD_NANDFLASH		4
#define MTD_PEROM		5
#define MTD_OTHER		14
#define MTD_UNKNOWN		15



#define MTD_CLEAR_BITS		1       // Bits can be cleared (flash)
#define MTD_SET_BITS		2       // Bits can be set
#define MTD_ERASEABLE		4       // Has an erase function
#define MTD_WRITEB_WRITEABLE	8       // Direct IO is possible
#define MTD_VOLATILE		16      // Set for RAMs
#define MTD_XIP			32	// eXecute-In-Place possible
#define MTD_OOB			64	// Out-of-band data (NAND flash)
#define MTD_ECC			128	// Device capable of automatic ECC

// Some common devices / combinations of capabilities
#define MTD_CAP_ROM		0
#define MTD_CAP_RAM		(MTD_CLEAR_BITS|MTD_SET_BITS|MTD_WRITEB_WRITEABLE)
#define MTD_CAP_NORFLASH        (MTD_CLEAR_BITS|MTD_ERASEABLE)
#define MTD_CAP_NANDFLASH       (MTD_CLEAR_BITS|MTD_ERASEABLE|MTD_OOB)
#define MTD_WRITEABLE		(MTD_CLEAR_BITS|MTD_SET_BITS)


// Types of automatic ECC/Checksum available
#define MTD_ECC_NONE		0 	// No automatic ECC available
#define MTD_ECC_RS_DiskOnChip	1	// Automatic ECC on DiskOnChip
#define MTD_ECC_SW		2	// SW ECC for Toshiba & Samsung devices

struct mtd_info_user {
	u_char type;
	u_long flags;
	u_long size;	 // Total size of the MTD
	u_long erasesize;
	u_long oobblock;  // Size of OOB blocks (e.g. 512)
	u_long oobsize;   // Amount of OOB data per block (e.g. 16)
        u_long ecctype;
        u_long eccsize;
};

#define MEMGETINFO              _IOR('M', 1, struct mtd_info_user)
#define MEMERASE                _IOW('M', 2, struct erase_info_user)
#define MEMWRITEOOB             _IOWR('M', 3, struct mtd_oob_buf)
#define MEMREADOOB              _IOWR('M', 4, struct mtd_oob_buf)
#define MEMLOCK                 _IOW('M', 5, struct erase_info_user)
#define MEMUNLOCK               _IOW('M', 6, struct erase_info_user)

#ifndef __KERNEL__

typedef struct mtd_info_user mtd_info_t;
typedef struct erase_info_user erase_info_t;

	/* User-space ioctl definitions */


#else /* __KERNEL__ */


#define MTD_ERASE_PENDING      	0x01
#define MTD_ERASING		0x02
#define MTD_ERASE_SUSPEND	0x04
#define MTD_ERASE_DONE          0x08
#define MTD_ERASE_FAILED        0x10

struct erase_info {
	struct mtd_info *mtd;
	u_long addr;
	u_long len;
	u_long time;
	u_long retries;
	u_int dev;
	u_int cell;
	void (*callback) (struct erase_info *self);
	u_long priv;
	u_char state;
	struct erase_info *next;
};


struct mtd_info {
	u_char type;
	u_long flags;
	u_long size;	 // Total size of the MTD
	u_long erasesize;
	u_long oobblock;  // Size of OOB blocks (e.g. 512)
	u_long oobsize;   // Amount of OOB data per block (e.g. 16)
        u_long ecctype;
        u_long eccsize;

	// Kernel-only stuff starts here.
	char *name;
	int index;

	u_long bank_size;

	struct module *module;
	int (*erase) (struct mtd_info *mtd, struct erase_info *instr);

	/* This stuff for eXecute-In-Place */
	int (*point) (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char **mtdbuf);

	/* We probably shouldn't allow XIP if the unpoint isn't a NULL */
	void (*unpoint) (struct mtd_info *mtd, u_char * addr);


	int (*read) (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf);
	int (*write) (struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen, const u_char *buf);

	int (*read_ecc) (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf, u_char *eccbuf);
	int (*write_ecc) (struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen, const u_char *buf, u_char *eccbuf);

	int (*read_oob) (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf);
	int (*write_oob) (struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen, const u_char *buf);

	/* iovec-based read/write methods. We need these especially for NAND flash,
	   with its limited number of write cycles per erase.
	   NB: The 'count' parameter is the number of _vectors_, each of 
	   which contains an (ofs, len) tuple.
	*/
	int (*readv) (struct mtd_info *mtd, struct iovec *vecs, unsigned long count, loff_t from, size_t *retlen);
	int (*writev) (struct mtd_info *mtd, const struct iovec *vecs, unsigned long count, loff_t to, size_t *retlen);

	/* Sync */
	void (*sync) (struct mtd_info *mtd);

	/* Chip-supported device locking */
	int (*lock) (struct mtd_info *mtd, loff_t ofs, size_t len);
	int (*unlock) (struct mtd_info *mtd, loff_t ofs, size_t len);

	/* Power Management functions */
	int (*suspend) (struct mtd_info *mtd);
	void (*resume) (struct mtd_info *mtd);

	void *priv;
};


	/* Kernel-side ioctl definitions */

extern int add_mtd_device(struct mtd_info *mtd);
extern int del_mtd_device (struct mtd_info *mtd);

extern struct mtd_info *__get_mtd_device(struct mtd_info *mtd, int num);

static inline struct mtd_info *get_mtd_device(struct mtd_info *mtd, int num)
{
	struct mtd_info *ret;
	
	ret = __get_mtd_device(mtd, num);

	if (ret && ret->module && !try_inc_mod_count(ret->module))
		return NULL;

	return ret;
}

static inline void put_mtd_device(struct mtd_info *mtd)
{
       if (mtd->module)
	       __MOD_DEC_USE_COUNT(mtd->module);
}


struct mtd_notifier {
	void (*add)(struct mtd_info *mtd);
	void (*remove)(struct mtd_info *mtd);
	struct mtd_notifier *next;
};


extern void register_mtd_user (struct mtd_notifier *new);
extern int unregister_mtd_user (struct mtd_notifier *old);


#ifndef MTDC
#define MTD_ERASE(mtd, args...) (*(mtd->erase))(mtd, args)
#define MTD_POINT(mtd, a,b,c,d) (*(mtd->point))(mtd, a,b,c, (u_char **)(d))
#define MTD_UNPOINT(mtd, arg) (*(mtd->unpoint))(mtd, (u_char *)arg)
#define MTD_READ(mtd, args...) (*(mtd->read))(mtd, args)
#define MTD_WRITE(mtd, args...) (*(mtd->write))(mtd, args)
#define MTD_READV(mtd, args...) (*(mtd->readv))(mtd, args)
#define MTD_WRITEV(mtd, args...) (*(mtd->writev))(mtd, args)
#define MTD_READECC(mtd, args...) (*(mtd->read_ecc))(mtd, args)
#define MTD_WRITEECC(mtd, args...) (*(mtd->write_ecc))(mtd, args)
#define MTD_READOOB(mtd, args...) (*(mtd->read_oob))(mtd, args)
#define MTD_WRITEOOB(mtd, args...) (*(mtd->write_oob))(mtd, args)
#define MTD_SYNC(mtd) do { if (mtd->sync) (*(mtd->sync))(mtd);  } while (0) 
#endif /* MTDC */

/*
 * Debugging macro and defines
 */
#define MTD_DEBUG_LEVEL0	(0)	/* Quiet   */
#define MTD_DEBUG_LEVEL1	(1)	/* Audible */
#define MTD_DEBUG_LEVEL2	(2)	/* Loud    */
#define MTD_DEBUG_LEVEL3	(3)	/* Noisy   */

#ifdef CONFIG_MTD_DEBUG
#define DEBUG(n, args...)			\
	if (n <=  CONFIG_MTD_DEBUG_VERBOSE) {	\
		printk(KERN_INFO args);	\
	}
#else /* CONFIG_MTD_DEBUG */
#define DEBUG(n, args...)
#endif /* CONFIG_MTD_DEBUG */

#endif /* __KERNEL__ */

#endif /* __MTD_MTD_H__ */
