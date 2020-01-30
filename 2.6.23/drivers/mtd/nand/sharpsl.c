/*
 * drivers/mtd/nand/sharpsl.c
 *
 *  Copyright (C) 2004 Richard Purdie
 *
 *  $Id: sharpsl.c,v 1.7 2005/11/07 11:14:31 gleixner Exp $
 *
 *  Based on Sharp's NAND driver sharp_sl.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/genhd.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/nand_ecc.h>
#include <linux/mtd/partitions.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <asm/hardware.h>
#include <asm/mach-types.h>

static void __iomem *sharpsl_io_base;
static int sharpsl_phys_base = 0x0C000000;

/* register offset */
#define ECCLPLB	 	sharpsl_io_base+0x00	/* line parity 7 - 0 bit */
#define ECCLPUB	 	sharpsl_io_base+0x04	/* line parity 15 - 8 bit */
#define ECCCP	   	sharpsl_io_base+0x08	/* column parity 5 - 0 bit */
#define ECCCNTR	 	sharpsl_io_base+0x0C	/* ECC byte counter */
#define ECCCLRR	 	sharpsl_io_base+0x10	/* cleare ECC */
#define FLASHIO	 	sharpsl_io_base+0x14	/* Flash I/O */
#define FLASHCTL	sharpsl_io_base+0x18	/* Flash Control */

/* Flash control bit */
#define FLRYBY		(1 << 5)
#define FLCE1		(1 << 4)
#define FLWP		(1 << 3)
#define FLALE		(1 << 2)
#define FLCLE		(1 << 1)
#define FLCE0		(1 << 0)

/*
 * MTD structure for SharpSL
 */
static struct mtd_info *sharpsl_mtd = NULL;

/*
 * Define partitions for flash device
 */
#define DEFAULT_NUM_PARTITIONS 3

static int nr_partitions;
static struct mtd_partition sharpsl_nand_default_partition_info[] = {
	{
	 .name = "System Area",
	 .offset = 0,
	 .size = 7 * 1024 * 1024,
	 },
	{
	 .name = "Root Filesystem",
	 .offset = 7 * 1024 * 1024,
	 .size = 30 * 1024 * 1024,
	 },
	{
	 .name = "Home Filesystem",
	 .offset = MTDPART_OFS_APPEND,
	 .size = MTDPART_SIZ_FULL,
	 },
};

/*
 *	hardware specific access to control-lines
 *	ctrl:
 *	NAND_CNE: bit 0 -> ! bit 0 & 4
 *	NAND_CLE: bit 1 -> bit 1
 *	NAND_ALE: bit 2 -> bit 2
 *
 */
static void sharpsl_nand_hwcontrol(struct mtd_info *mtd, int cmd,
				   unsigned int ctrl)
{
	struct nand_chip *chip = mtd->priv;

	if (ctrl & NAND_CTRL_CHANGE) {
		unsigned char bits = ctrl & 0x07;

		bits |= (ctrl & 0x01) << 4;

		bits ^= 0x11;

		writeb((readb(FLASHCTL) & ~0x17) | bits, FLASHCTL);
	}

	if (cmd != NAND_CMD_NONE)
		writeb(cmd, chip->IO_ADDR_W);
}

static uint8_t scan_ff_pattern[] = { 0xff, 0xff };

static struct nand_bbt_descr sharpsl_bbt = {
	.options = 0,
	.offs = 4,
	.len = 2,
	.pattern = scan_ff_pattern
};

static struct nand_bbt_descr sharpsl_akita_bbt = {
	.options = 0,
	.offs = 4,
	.len = 1,
	.pattern = scan_ff_pattern
};

static struct nand_ecclayout akita_oobinfo = {
	.eccbytes = 24,
	.eccpos = {
		   0x5, 0x1, 0x2, 0x3, 0x6, 0x7, 0x15, 0x11,
		   0x12, 0x13, 0x16, 0x17, 0x25, 0x21, 0x22, 0x23,
		   0x26, 0x27, 0x35, 0x31, 0x32, 0x33, 0x36, 0x37},
	.oobfree = {{0x08, 0x09}}
};

static int sharpsl_nand_dev_ready(struct mtd_info *mtd)
{
	return !((readb(FLASHCTL) & FLRYBY) == 0);
}

static void sharpsl_nand_enable_hwecc(struct mtd_info *mtd, int mode)
{
	writeb(0, ECCCLRR);
}

static int sharpsl_nand_calculate_ecc(struct mtd_info *mtd, const u_char * dat, u_char * ecc_code)
{
	ecc_code[0] = ~readb(ECCLPUB);
	ecc_code[1] = ~readb(ECCLPLB);
	ecc_code[2] = (~readb(ECCCP) << 2) | 0x03;
	return readb(ECCCNTR) != 0;
}

#ifdef CONFIG_MTD_PARTITIONS
const char *part_probes[] = { "cmdlinepart", NULL };
#endif

/*
 * Main initialization routine
 */
static int __init sharpsl_nand_init(void)
{
	struct nand_chip *this;
	struct mtd_partition *sharpsl_partition_info;
	int err = 0;

	/* Allocate memory for MTD device structure and private data */
	sharpsl_mtd = kmalloc(sizeof(struct mtd_info) + sizeof(struct nand_chip), GFP_KERNEL);
	if (!sharpsl_mtd) {
		printk("Unable to allocate SharpSL NAND MTD device structure.\n");
		return -ENOMEM;
	}

	/* map physical adress */
	sharpsl_io_base = ioremap(sharpsl_phys_base, 0x1000);
	if (!sharpsl_io_base) {
		printk("ioremap to access Sharp SL NAND chip failed\n");
		kfree(sharpsl_mtd);
		return -EIO;
	}

	/* Get pointer to private data */
	this = (struct nand_chip *)(&sharpsl_mtd[1]);

	/* Initialize structures */
	memset(sharpsl_mtd, 0, sizeof(struct mtd_info));
	memset(this, 0, sizeof(struct nand_chip));

	/* Link the private data with the MTD structure */
	sharpsl_mtd->priv = this;
	sharpsl_mtd->owner = THIS_MODULE;

	/*
	 * PXA initialize
	 */
	writeb(readb(FLASHCTL) | FLWP, FLASHCTL);

	/* Set address of NAND IO lines */
	this->IO_ADDR_R = FLASHIO;
	this->IO_ADDR_W = FLASHIO;
	/* Set address of hardware control function */
	this->cmd_ctrl = sharpsl_nand_hwcontrol;
	this->dev_ready = sharpsl_nand_dev_ready;
	/* 15 us command delay time */
	this->chip_delay = 15;
	/* set eccmode using hardware ECC */
	this->ecc.mode = NAND_ECC_HW;
	this->ecc.size = 256;
	this->ecc.bytes = 3;
	this->badblock_pattern = &sharpsl_bbt;
	if (machine_is_akita() || machine_is_borzoi()) {
		this->badblock_pattern = &sharpsl_akita_bbt;
		this->ecc.layout = &akita_oobinfo;
	}
	this->ecc.hwctl = sharpsl_nand_enable_hwecc;
	this->ecc.calculate = sharpsl_nand_calculate_ecc;
	this->ecc.correct = nand_correct_data;

	/* Scan to find existence of the device */
	err = nand_scan(sharpsl_mtd, 1);
	if (err) {
		iounmap(sharpsl_io_base);
		kfree(sharpsl_mtd);
		return err;
	}

	/* Register the partitions */
	sharpsl_mtd->name = "sharpsl-nand";
	nr_partitions = parse_mtd_partitions(sharpsl_mtd, part_probes, &sharpsl_partition_info, 0);

	if (nr_partitions <= 0) {
		nr_partitions = DEFAULT_NUM_PARTITIONS;
		sharpsl_partition_info = sharpsl_nand_default_partition_info;
		if (machine_is_poodle()) {
			sharpsl_partition_info[1].size = 22 * 1024 * 1024;
		} else if (machine_is_corgi() || machine_is_shepherd()) {
			sharpsl_partition_info[1].size = 25 * 1024 * 1024;
		} else if (machine_is_husky()) {
			sharpsl_partition_info[1].size = 53 * 1024 * 1024;
		} else if (machine_is_spitz()) {
			sharpsl_partition_info[1].size = 5 * 1024 * 1024;
		} else if (machine_is_akita()) {
			sharpsl_partition_info[1].size = 58 * 1024 * 1024;
		} else if (machine_is_borzoi()) {
			sharpsl_partition_info[1].size = 32 * 1024 * 1024;
		}
	}

	add_mtd_partitions(sharpsl_mtd, sharpsl_partition_info, nr_partitions);

	/* Return happy */
	return 0;
}

module_init(sharpsl_nand_init);

/*
 * Clean up routine
 */
static void __exit sharpsl_nand_cleanup(void)
{
	/* Release resources, unregister device */
	nand_release(sharpsl_mtd);

	iounmap(sharpsl_io_base);

	/* Free the MTD device structure */
	kfree(sharpsl_mtd);
}

module_exit(sharpsl_nand_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard Purdie <rpurdie@rpsys.net>");
MODULE_DESCRIPTION("Device specific logic for NAND flash on Sharp SL-C7xx Series");
