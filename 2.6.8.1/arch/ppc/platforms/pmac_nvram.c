/*
 *  arch/ppc/platforms/pmac_nvram.c
 *
 *  Copyright (C) 2002 Benjamin Herrenschmidt (benh@kernel.crashing.org)
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 *  Todo: - add support for the OF persistent properties
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/nvram.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/adb.h>
#include <linux/pmu.h>
#include <linux/bootmem.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <asm/sections.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/prom.h>
#include <asm/machdep.h>
#include <asm/nvram.h>

#define DEBUG

#ifdef DEBUG
#define DBG(x...) printk(x)
#else
#define DBG(x...)
#endif

#define NVRAM_SIZE		0x2000	/* 8kB of non-volatile RAM */

#define CORE99_SIGNATURE	0x5a
#define CORE99_ADLER_START	0x14

/* On Core99, nvram is either a sharp, a micron or an AMD flash */
#define SM_FLASH_STATUS_DONE	0x80
#define SM_FLASH_STATUS_ERR		0x38
#define SM_FLASH_CMD_ERASE_CONFIRM	0xd0
#define SM_FLASH_CMD_ERASE_SETUP	0x20
#define SM_FLASH_CMD_RESET		0xff
#define SM_FLASH_CMD_WRITE_SETUP	0x40
#define SM_FLASH_CMD_CLEAR_STATUS	0x50
#define SM_FLASH_CMD_READ_STATUS	0x70

/* CHRP NVRAM header */
struct chrp_header {
  u8		signature;
  u8		cksum;
  u16		len;
  char          name[12];
  u8		data[0];
};

struct core99_header {
  struct chrp_header	hdr;
  u32			adler;
  u32			generation;
  u32			reserved[2];
};

/*
 * Read and write the non-volatile RAM on PowerMacs and CHRP machines.
 */
static int nvram_naddrs;
static volatile unsigned char *nvram_addr;
static volatile unsigned char *nvram_data;
static int nvram_mult, is_core_99;
static int core99_bank = 0;
static int nvram_partitions[3];
static spinlock_t nv_lock = SPIN_LOCK_UNLOCKED;

extern int pmac_newworld;
extern int system_running;

static int (*core99_write_bank)(int bank, u8* datas);
static int (*core99_erase_bank)(int bank);

static char *nvram_image __pmacdata;


static unsigned char __pmac core99_nvram_read_byte(int addr)
{
	if (nvram_image == NULL)
		return 0xff;
	return nvram_image[addr];
}

static void __pmac core99_nvram_write_byte(int addr, unsigned char val)
{
	if (nvram_image == NULL)
		return;
	nvram_image[addr] = val;
}


static unsigned char __openfirmware direct_nvram_read_byte(int addr)
{
	return in_8(&nvram_data[(addr & (NVRAM_SIZE - 1)) * nvram_mult]);
}

static void __openfirmware direct_nvram_write_byte(int addr, unsigned char val)
{
	out_8(&nvram_data[(addr & (NVRAM_SIZE - 1)) * nvram_mult], val);
}


static unsigned char __pmac indirect_nvram_read_byte(int addr)
{
	unsigned char val;
	unsigned long flags;

	spin_lock_irqsave(&nv_lock, flags);
	out_8(nvram_addr, addr >> 5);
	val = in_8(&nvram_data[(addr & 0x1f) << 4]);
	spin_unlock_irqrestore(&nv_lock, flags);

	return val;
}

static void __pmac indirect_nvram_write_byte(int addr, unsigned char val)
{
	unsigned long flags;

	spin_lock_irqsave(&nv_lock, flags);
	out_8(nvram_addr, addr >> 5);
	out_8(&nvram_data[(addr & 0x1f) << 4], val);
	spin_unlock_irqrestore(&nv_lock, flags);
}


#ifdef CONFIG_ADB_PMU

static void __pmac pmu_nvram_complete(struct adb_request *req)
{
	if (req->arg)
		complete((struct completion *)req->arg);
}

static unsigned char __pmac pmu_nvram_read_byte(int addr)
{
	struct adb_request req;
	DECLARE_COMPLETION(req_complete); 
	
	req.arg = system_state == SYSTEM_RUNNING ? &req_complete : NULL;
	if (pmu_request(&req, pmu_nvram_complete, 3, PMU_READ_NVRAM,
			(addr >> 8) & 0xff, addr & 0xff))
		return 0xff;
	if (system_state == SYSTEM_RUNNING)
		wait_for_completion(&req_complete);
	while (!req.complete)
		pmu_poll();
	return req.reply[0];
}

static void __pmac pmu_nvram_write_byte(int addr, unsigned char val)
{
	struct adb_request req;
	DECLARE_COMPLETION(req_complete); 
	
	req.arg = system_state == SYSTEM_RUNNING ? &req_complete : NULL;
	if (pmu_request(&req, pmu_nvram_complete, 4, PMU_WRITE_NVRAM,
			(addr >> 8) & 0xff, addr & 0xff, val))
		return;
	if (system_state == SYSTEM_RUNNING)
		wait_for_completion(&req_complete);
	while (!req.complete)
		pmu_poll();
}

#endif /* CONFIG_ADB_PMU */


static u8 __pmac chrp_checksum(struct chrp_header* hdr)
{
	u8 *ptr;
	u16 sum = hdr->signature;
	for (ptr = (u8 *)&hdr->len; ptr < hdr->data; ptr++)
		sum += *ptr;
	while (sum > 0xFF)
		sum = (sum & 0xFF) + (sum>>8);
	return sum;
}

static u32 __pmac core99_calc_adler(u8 *buffer)
{
	int cnt;
	u32 low, high;

   	buffer += CORE99_ADLER_START;
	low = 1;
	high = 0;
	for (cnt=0; cnt<(NVRAM_SIZE-CORE99_ADLER_START); cnt++) {
		if ((cnt % 5000) == 0) {
			high  %= 65521UL;
			high %= 65521UL;
		}
		low += buffer[cnt];
		high += low;
	}
	low  %= 65521UL;
	high %= 65521UL;

	return (high << 16) | low;
}

static u32 __pmac core99_check(u8* datas)
{
	struct core99_header* hdr99 = (struct core99_header*)datas;

	if (hdr99->hdr.signature != CORE99_SIGNATURE) {
		DBG("Invalid signature\n");
		return 0;
	}
	if (hdr99->hdr.cksum != chrp_checksum(&hdr99->hdr)) {
		DBG("Invalid checksum\n");
		return 0;
	}
	if (hdr99->adler != core99_calc_adler(datas)) {
		DBG("Invalid adler\n");
		return 0;
	}
	return hdr99->generation;
}

static int __pmac sm_erase_bank(int bank)
{
	int stat, i;
	unsigned long timeout;

	u8* base = (u8 *)nvram_data + core99_bank*NVRAM_SIZE;

       	DBG("nvram: Sharp/Micron Erasing bank %d...\n", bank);

	out_8(base, SM_FLASH_CMD_ERASE_SETUP);
	out_8(base, SM_FLASH_CMD_ERASE_CONFIRM);
	timeout = 0;
	do {
		if (++timeout > 1000000) {
			printk(KERN_ERR "nvram: Sharp/Miron flash erase timeout !\n");
			break;
		}
		out_8(base, SM_FLASH_CMD_READ_STATUS);
		stat = in_8(base);
	} while (!(stat & SM_FLASH_STATUS_DONE));

	out_8(base, SM_FLASH_CMD_CLEAR_STATUS);
	out_8(base, SM_FLASH_CMD_RESET);

	for (i=0; i<NVRAM_SIZE; i++)
		if (base[i] != 0xff) {
			printk(KERN_ERR "nvram: Sharp/Micron flash erase failed !\n");
			return -ENXIO;
		}
	return 0;
}

static int __pmac sm_write_bank(int bank, u8* datas)
{
	int i, stat = 0;
	unsigned long timeout;

	u8* base = (u8 *)nvram_data + core99_bank*NVRAM_SIZE;

       	DBG("nvram: Sharp/Micron Writing bank %d...\n", bank);

	for (i=0; i<NVRAM_SIZE; i++) {
		out_8(base+i, SM_FLASH_CMD_WRITE_SETUP);
		udelay(1);
		out_8(base+i, datas[i]);
		timeout = 0;
		do {
			if (++timeout > 1000000) {
				printk(KERN_ERR "nvram: Sharp/Micron flash write timeout !\n");
				break;
			}
			out_8(base, SM_FLASH_CMD_READ_STATUS);
			stat = in_8(base);
		} while (!(stat & SM_FLASH_STATUS_DONE));
		if (!(stat & SM_FLASH_STATUS_DONE))
			break;
	}
	out_8(base, SM_FLASH_CMD_CLEAR_STATUS);
	out_8(base, SM_FLASH_CMD_RESET);
	for (i=0; i<NVRAM_SIZE; i++)
		if (base[i] != datas[i]) {
			printk(KERN_ERR "nvram: Sharp/Micron flash write failed !\n");
			return -ENXIO;
		}
	return 0;
}

static int __pmac amd_erase_bank(int bank)
{
	int i, stat = 0;
	unsigned long timeout;

	u8* base = (u8 *)nvram_data + core99_bank*NVRAM_SIZE;

       	DBG("nvram: AMD Erasing bank %d...\n", bank);

	/* Unlock 1 */
	out_8(base+0x555, 0xaa);
	udelay(1);
	/* Unlock 2 */
	out_8(base+0x2aa, 0x55);
	udelay(1);

	/* Sector-Erase */
	out_8(base+0x555, 0x80);
	udelay(1);
	out_8(base+0x555, 0xaa);
	udelay(1);
	out_8(base+0x2aa, 0x55);
	udelay(1);
	out_8(base, 0x30);
	udelay(1);

	timeout = 0;
	do {
		if (++timeout > 1000000) {
			printk(KERN_ERR "nvram: AMD flash erase timeout !\n");
			break;
		}
		stat = in_8(base) ^ in_8(base);
	} while (stat != 0);
	
	/* Reset */
	out_8(base, 0xf0);
	udelay(1);
	
	for (i=0; i<NVRAM_SIZE; i++)
		if (base[i] != 0xff) {
			printk(KERN_ERR "nvram: AMD flash erase failed !\n");
			return -ENXIO;
		}
	return 0;
}

static int __pmac amd_write_bank(int bank, u8* datas)
{
	int i, stat = 0;
	unsigned long timeout;

	u8* base = (u8 *)nvram_data + core99_bank*NVRAM_SIZE;

       	DBG("nvram: AMD Writing bank %d...\n", bank);

	for (i=0; i<NVRAM_SIZE; i++) {
		/* Unlock 1 */
		out_8(base+0x555, 0xaa);
		udelay(1);
		/* Unlock 2 */
		out_8(base+0x2aa, 0x55);
		udelay(1);

		/* Write single word */
		out_8(base+0x555, 0xa0);
		udelay(1);
		out_8(base+i, datas[i]);
		
		timeout = 0;
		do {
			if (++timeout > 1000000) {
				printk(KERN_ERR "nvram: AMD flash write timeout !\n");
				break;
			}
			stat = in_8(base) ^ in_8(base);
		} while (stat != 0);
		if (stat != 0)
			break;
	}

	/* Reset */
	out_8(base, 0xf0);
	udelay(1);

	for (i=0; i<NVRAM_SIZE; i++)
		if (base[i] != datas[i]) {
			printk(KERN_ERR "nvram: AMD flash write failed !\n");
			return -ENXIO;
		}
	return 0;
}

static void __init lookup_partitions(void)
{
	u8 buffer[17];
	int i, offset;
	struct chrp_header* hdr;

	if (pmac_newworld) {
		nvram_partitions[pmac_nvram_OF] = -1;
		nvram_partitions[pmac_nvram_XPRAM] = -1;
		nvram_partitions[pmac_nvram_NR] = -1;
		hdr = (struct chrp_header *)buffer;

		offset = 0;
		buffer[16] = 0;
		do {
			for (i=0;i<16;i++)
				buffer[i] = nvram_read_byte(offset+i);
			if (!strcmp(hdr->name, "common"))
				nvram_partitions[pmac_nvram_OF] = offset + 0x10;
			if (!strcmp(hdr->name, "APL,MacOS75")) {
				nvram_partitions[pmac_nvram_XPRAM] = offset + 0x10;
				nvram_partitions[pmac_nvram_NR] = offset + 0x110;
			}
			offset += (hdr->len * 0x10);
		} while(offset < NVRAM_SIZE);
	} else {
		nvram_partitions[pmac_nvram_OF] = 0x1800;
		nvram_partitions[pmac_nvram_XPRAM] = 0x1300;
		nvram_partitions[pmac_nvram_NR] = 0x1400;
	}
	DBG("nvram: OF partition at 0x%x\n", nvram_partitions[pmac_nvram_OF]);
	DBG("nvram: XP partition at 0x%x\n", nvram_partitions[pmac_nvram_XPRAM]);
	DBG("nvram: NR partition at 0x%x\n", nvram_partitions[pmac_nvram_NR]);
}

static void __pmac core99_nvram_sync(void)
{
	struct core99_header* hdr99;
	unsigned long flags;

	if (!is_core_99 || !nvram_data || !nvram_image)
		return;

	spin_lock_irqsave(&nv_lock, flags);
	if (!memcmp(nvram_image, (u8*)nvram_data + core99_bank*NVRAM_SIZE,
		NVRAM_SIZE))
		goto bail;

	DBG("Updating nvram...\n");

	hdr99 = (struct core99_header*)nvram_image;
	hdr99->generation++;
	hdr99->hdr.signature = CORE99_SIGNATURE;
	hdr99->hdr.cksum = chrp_checksum(&hdr99->hdr);
	hdr99->adler = core99_calc_adler(nvram_image);
	core99_bank = core99_bank ? 0 : 1;
	if (core99_erase_bank)
		if (core99_erase_bank(core99_bank)) {
			printk("nvram: Error erasing bank %d\n", core99_bank);
			goto bail;
		}
	if (core99_write_bank)
		if (core99_write_bank(core99_bank, nvram_image))
			printk("nvram: Error writing bank %d\n", core99_bank);
 bail:
	spin_unlock_irqrestore(&nv_lock, flags);

#ifdef DEBUG
       	mdelay(2000);
#endif
}

void __init pmac_nvram_init(void)
{
	struct device_node *dp;

	nvram_naddrs = 0;

	dp = find_devices("nvram");
	if (dp == NULL) {
		printk(KERN_ERR "Can't find NVRAM device\n");
		return;
	}
	nvram_naddrs = dp->n_addrs;
	is_core_99 = device_is_compatible(dp, "nvram,flash");
	if (is_core_99) {
		int i;
		u32 gen_bank0, gen_bank1;

		if (nvram_naddrs < 1) {
			printk(KERN_ERR "nvram: no address\n");
			return;
		}
		nvram_image = alloc_bootmem(NVRAM_SIZE);
		if (nvram_image == NULL) {
			printk(KERN_ERR "nvram: can't allocate ram image\n");
			return;
		}
		nvram_data = ioremap(dp->addrs[0].address, NVRAM_SIZE*2);
		nvram_naddrs = 1; /* Make sure we get the correct case */

		DBG("nvram: Checking bank 0...\n");

		gen_bank0 = core99_check((u8 *)nvram_data);
		gen_bank1 = core99_check((u8 *)nvram_data + NVRAM_SIZE);
		core99_bank = (gen_bank0 < gen_bank1) ? 1 : 0;

		DBG("nvram: gen0=%d, gen1=%d\n", gen_bank0, gen_bank1);
		DBG("nvram: Active bank is: %d\n", core99_bank);

		for (i=0; i<NVRAM_SIZE; i++)
			nvram_image[i] = nvram_data[i + core99_bank*NVRAM_SIZE];

		ppc_md.nvram_read_val	= core99_nvram_read_byte;
		ppc_md.nvram_write_val	= core99_nvram_write_byte;
		ppc_md.nvram_sync	= core99_nvram_sync;
		/* 
		 * Maybe we could be smarter here though making an exclusive list
		 * of known flash chips is a bit nasty as older OF didn't provide us
		 * with a useful "compatible" entry. A solution would be to really
		 * identify the chip using flash id commands and base ourselves on
		 * a list of known chips IDs
		 */
		if (device_is_compatible(dp, "amd-0137")) {
			core99_erase_bank = amd_erase_bank;
			core99_write_bank = amd_write_bank;
		} else {
			core99_erase_bank = sm_erase_bank;
			core99_write_bank = sm_write_bank;
		}
	} else if (_machine == _MACH_chrp && nvram_naddrs == 1) {
		nvram_data = ioremap(dp->addrs[0].address + isa_mem_base,
				     dp->addrs[0].size);
		nvram_mult = 1;
		ppc_md.nvram_read_val	= direct_nvram_read_byte;
		ppc_md.nvram_write_val	= direct_nvram_write_byte;
	} else if (nvram_naddrs == 1) {
		nvram_data = ioremap(dp->addrs[0].address, dp->addrs[0].size);
		nvram_mult = (dp->addrs[0].size + NVRAM_SIZE - 1) / NVRAM_SIZE;
		ppc_md.nvram_read_val	= direct_nvram_read_byte;
		ppc_md.nvram_write_val	= direct_nvram_write_byte;
	} else if (nvram_naddrs == 2) {
		nvram_addr = ioremap(dp->addrs[0].address, dp->addrs[0].size);
		nvram_data = ioremap(dp->addrs[1].address, dp->addrs[1].size);
		ppc_md.nvram_read_val	= indirect_nvram_read_byte;
		ppc_md.nvram_write_val	= indirect_nvram_write_byte;
	} else if (nvram_naddrs == 0 && sys_ctrler == SYS_CTRLER_PMU) {
#ifdef CONFIG_ADB_PMU
		nvram_naddrs = -1;
		ppc_md.nvram_read_val	= pmu_nvram_read_byte;
		ppc_md.nvram_write_val	= pmu_nvram_write_byte;
#endif /* CONFIG_ADB_PMU */
	} else {
		printk(KERN_ERR "Don't know how to access NVRAM with %d addresses\n",
		       nvram_naddrs);
	}
	lookup_partitions();
}

int __pmac pmac_get_partition(int partition)
{
	return nvram_partitions[partition];
}

u8 __pmac pmac_xpram_read(int xpaddr)
{
	int offset = nvram_partitions[pmac_nvram_XPRAM];

	if (offset < 0)
		return 0xff;

	return ppc_md.nvram_read_val(xpaddr + offset);
}

void __pmac pmac_xpram_write(int xpaddr, u8 data)
{
	int offset = nvram_partitions[pmac_nvram_XPRAM];

	if (offset < 0)
		return;

	ppc_md.nvram_write_val(xpaddr + offset, data);
}

EXPORT_SYMBOL(pmac_get_partition);
EXPORT_SYMBOL(pmac_xpram_read);
EXPORT_SYMBOL(pmac_xpram_write);
