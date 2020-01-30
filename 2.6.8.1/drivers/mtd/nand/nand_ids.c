/*
 *  drivers/mtd/nandids.c
 *
 *  Copyright (C) 2002 Thomas Gleixner (tglx@linutronix.de)
  *
 * $Id: nand_ids.c,v 1.10 2004/05/26 13:40:12 gleixner Exp $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/module.h>
#include <linux/mtd/nand.h>
/*
*	Chip ID list
*	
*	Name. ID code, pagesize, chipsize in MegaByte, eraseblock size,
*	options
* 
* 	Pagesize; 0, 256, 512
*	0 	get this information from the extended chip ID
+	256	256 Byte page size
*	512	512 Byte page size	
*/
struct nand_flash_dev nand_flash_ids[] = {
	{"NAND 1MiB 5V 8-bit", 		0x6e, 256, 1, 0x1000, 0},
	{"NAND 2MiB 5V 8-bit", 		0x64, 256, 2, 0x1000, 0},
	{"NAND 4MiB 5V 8-bit", 		0x6b, 512, 4, 0x2000, 0},
	{"NAND 1MiB 3,3V 8-bit", 	0xe8, 256, 1, 0x1000, 0},
	{"NAND 1MiB 3,3V 8-bit", 	0xec, 256, 1, 0x1000, 0},
	{"NAND 2MiB 3,3V 8-bit", 	0xea, 256, 2, 0x1000, 0},
	{"NAND 4MiB 3,3V 8-bit", 	0xd5, 512, 4, 0x2000, 0},
	{"NAND 4MiB 3,3V 8-bit", 	0xe3, 512, 4, 0x2000, 0},
	{"NAND 4MiB 3,3V 8-bit", 	0xe5, 512, 4, 0x2000, 0},
	{"NAND 8MiB 3,3V 8-bit", 	0xd6, 512, 8, 0x2000, 0},
	
	{"NAND 8MiB 1,8V 8-bit", 	0x39, 512, 8, 0x2000, 0},
	{"NAND 8MiB 3,3V 8-bit", 	0xe6, 512, 8, 0x2000, 0},
	{"NAND 8MiB 1,8V 16-bit", 	0x49, 512, 8, 0x2000, NAND_BUSWIDTH_16},
	{"NAND 8MiB 3,3V 16-bit", 	0x59, 512, 8, 0x2000, NAND_BUSWIDTH_16},
	
	{"NAND 16MiB 1,8V 8-bit", 	0x33, 512, 16, 0x4000, 0},
	{"NAND 16MiB 3,3V 8-bit", 	0x73, 512, 16, 0x4000, 0},
	{"NAND 16MiB 1,8V 16-bit", 	0x43, 512, 16, 0x4000, NAND_BUSWIDTH_16},
	{"NAND 16MiB 3,3V 16-bit", 	0x53, 512, 16, 0x4000, NAND_BUSWIDTH_16},
	
	{"NAND 32MiB 1,8V 8-bit", 	0x35, 512, 32, 0x4000, 0},
	{"NAND 32MiB 3,3V 8-bit", 	0x75, 512, 32, 0x4000, 0},
	{"NAND 32MiB 1,8V 16-bit", 	0x45, 512, 32, 0x4000, NAND_BUSWIDTH_16},
	{"NAND 32MiB 3,3V 16-bit", 	0x55, 512, 32, 0x4000, NAND_BUSWIDTH_16},
	
	{"NAND 64MiB 1,8V 8-bit", 	0x36, 512, 64, 0x4000, 0},
	{"NAND 64MiB 3,3V 8-bit", 	0x76, 512, 64, 0x4000, 0},
	{"NAND 64MiB 1,8V 16-bit", 	0x46, 512, 64, 0x4000, NAND_BUSWIDTH_16},
	{"NAND 64MiB 3,3V 16-bit", 	0x56, 512, 64, 0x4000, NAND_BUSWIDTH_16},
	
	{"NAND 128MiB 1,8V 8-bit", 	0x78, 512, 128, 0x4000, 0},
	{"NAND 128MiB 3,3V 8-bit", 	0x79, 512, 128, 0x4000, 0},
	{"NAND 128MiB 1,8V 16-bit", 	0x72, 512, 128, 0x4000, NAND_BUSWIDTH_16},
	{"NAND 128MiB 3,3V 16-bit", 	0x74, 512, 128, 0x4000, NAND_BUSWIDTH_16},
	
	{"NAND 256MiB 3,3V 8-bit", 	0x71, 512, 256, 0x4000, 0},

	{"NAND 512MiB 3,3V 8-bit", 	0xDC, 512, 512, 0x4000, 0},
	
	/* These are the new chips with large page size. The pagesize
	* and the erasesize is determined from the extended id bytes
	*/
	/* 1 Gigabit */
	{"NAND 128MiB 1,8V 8-bit", 	0xA1, 0, 128, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 128MiB 3,3V 8-bit", 	0xF1, 0, 128, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 128MiB 1,8V 16-bit", 	0xB1, 0, 128, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},
	{"NAND 128MiB 3,3V 16-bit", 	0xC1, 0, 128, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},

	/* 2 Gigabit */
	{"NAND 256MiB 1,8V 8-bit", 	0xAA, 0, 256, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 256MiB 3,3V 8-bit", 	0xDA, 0, 256, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 256MiB 1,8V 16-bit", 	0xBA, 0, 256, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},
	{"NAND 256MiB 3,3V 16-bit", 	0xCA, 0, 256, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},
	
	/* 4 Gigabit */
	{"NAND 512MiB 1,8V 8-bit", 	0xAC, 0, 512, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 512MiB 3,3V 8-bit", 	0xDC, 0, 512, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 512MiB 1,8V 16-bit", 	0xBC, 0, 512, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},
	{"NAND 512MiB 3,3V 16-bit", 	0xCC, 0, 512, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},
	
	/* 8 Gigabit */
	{"NAND 1GiB 1,8V 8-bit", 	0xA3, 0, 1024, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 1GiB 3,3V 8-bit", 	0xD3, 0, 1024, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 1GiB 1,8V 16-bit", 	0xB3, 0, 1024, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},
	{"NAND 1GiB 3,3V 16-bit", 	0xC3, 0, 1024, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},

	/* 16 Gigabit */
	{"NAND 2GiB 1,8V 8-bit", 	0xA5, 0, 2048, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 2GiB 3,3V 8-bit", 	0xD5, 0, 2048, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_NO_AUTOINCR},
	{"NAND 2GiB 1,8V 16-bit", 	0xB5, 0, 2048, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},
	{"NAND 2GiB 3,3V 16-bit", 	0xC5, 0, 2048, 0, NAND_SAMSUNG_LP_OPTIONS | NAND_BUSWIDTH_16 | NAND_NO_AUTOINCR},

	/* Renesas AND 1 Gigabit. Those chips do not support extended id and have a strange page/block layout ! 
	 * The chosen minimum erasesize is 4 * 2 * 2048 = 16384 Byte, as those chips have an array of 4 page planes
	 * 1 block = 2 pages, but due to plane arrangement the blocks 0-3 consists of page 0 + 4,1 + 5, 2 + 6, 3 + 7
	 * Anyway JFFS2 would increase the eraseblock size so we chose a combined one which can be erased in one go
	 * There are more speed improvements for reads and writes possible, but not implemented now 
	 */
	{"AND 128MiB 3,3V 8-bit",	0x01, 2048, 128, 0x4000, NAND_IS_AND | NAND_NO_AUTOINCR | NAND_4PAGE_ARRAY},

	{NULL,}
};

/*
*	Manufacturer ID list
*/
struct nand_manufacturers nand_manuf_ids[] = {
	{NAND_MFR_TOSHIBA, "Toshiba"},
	{NAND_MFR_SAMSUNG, "Samsung"},
	{NAND_MFR_FUJITSU, "Fujitsu"},
	{NAND_MFR_NATIONAL, "National"},
	{NAND_MFR_RENESAS, "Renesas"},
	{NAND_MFR_STMICRO, "ST Micro"},
	{0x0, "Unknown"}
};

EXPORT_SYMBOL (nand_manuf_ids);
EXPORT_SYMBOL (nand_flash_ids);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Thomas Gleixner <tglx@linutronix.de>");
MODULE_DESCRIPTION ("Nand device & manufacturer ID's");
