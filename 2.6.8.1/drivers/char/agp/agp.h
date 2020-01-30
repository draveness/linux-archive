/*
 * AGPGART
 * Copyright (C) 2002-2004 Dave Jones
 * Copyright (C) 1999 Jeff Hartmann
 * Copyright (C) 1999 Precision Insight, Inc.
 * Copyright (C) 1999 Xi Graphics, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * JEFF HARTMANN, OR ANY OTHER CONTRIBUTORS BE LIABLE FOR ANY CLAIM, 
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR 
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE 
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _AGP_BACKEND_PRIV_H
#define _AGP_BACKEND_PRIV_H 1

#include <asm/agp.h>	/* for flush_agp_cache() */

#define PFX "agpgart: "

//#define AGP_DEBUG 1
#ifdef AGP_DEBUG
#define DBG(x,y...) printk (KERN_DEBUG PFX "%s: " x "\n", __FUNCTION__ , ## y)
#else
#define DBG(x,y...) do { } while (0)
#endif

extern struct agp_bridge_data *agp_bridge;

enum aper_size_type {
	U8_APER_SIZE,
	U16_APER_SIZE,
	U32_APER_SIZE,
	LVL2_APER_SIZE,
	FIXED_APER_SIZE
};

struct gatt_mask {
	unsigned long mask;
	u32 type;
	/* totally device specific, for integrated chipsets that 
	 * might have different types of memory masks.  For other
	 * devices this will probably be ignored */
};

struct aper_size_info_8 {
	int size;
	int num_entries;
	int page_order;
	u8 size_value;
};

struct aper_size_info_16 {
	int size;
	int num_entries;
	int page_order;
	u16 size_value;
};

struct aper_size_info_32 {
	int size;
	int num_entries;
	int page_order;
	u32 size_value;
};

struct aper_size_info_lvl2 {
	int size;
	int num_entries;
	u32 size_value;
};

struct aper_size_info_fixed {
	int size;
	int num_entries;
	int page_order;
};

struct agp_bridge_driver {
	struct module *owner;
	void *aperture_sizes;
	int num_aperture_sizes;
	enum aper_size_type size_type;
	int cant_use_aperture;
	int needs_scratch_page;
	struct gatt_mask *masks;
	int (*fetch_size)(void);
	int (*configure)(void);
	void (*agp_enable)(u32);
	void (*cleanup)(void);
	void (*tlb_flush)(struct agp_memory *);
	unsigned long (*mask_memory)(unsigned long, int);
	void (*cache_flush)(void);
	int (*create_gatt_table)(void);
	int (*free_gatt_table)(void);
	int (*insert_memory)(struct agp_memory *, off_t, int);
	int (*remove_memory)(struct agp_memory *, off_t, int);
	struct agp_memory *(*alloc_by_type) (size_t, int);
	void (*free_by_type)(struct agp_memory *);
	void *(*agp_alloc_page)(void);
	void (*agp_destroy_page)(void *);
};

struct agp_bridge_data {
	struct agp_version *version;
	struct agp_bridge_driver *driver;
	struct vm_operations_struct *vm_ops;
	void *previous_size;
	void *current_size;
	void *dev_private_data;
	struct pci_dev *dev;
	u32 *gatt_table;
	u32 *gatt_table_real;
	unsigned long scratch_page;
	unsigned long scratch_page_real;
	unsigned long gart_bus_addr;
	unsigned long gatt_bus_addr;
	u32 mode;
	enum chipset_type type;
	unsigned long *key_list;
	atomic_t current_memory_agp;
	atomic_t agp_in_use;
	int max_memory_agp;	/* in number of pages */
	int aperture_size_idx;
	int capndx;
	char major_version;
	char minor_version;
};

#define OUTREG64(mmap, addr, val)	__raw_writeq((val), (mmap)+(addr))
#define OUTREG32(mmap, addr, val)	__raw_writel((val), (mmap)+(addr))
#define OUTREG16(mmap, addr, val)	__raw_writew((val), (mmap)+(addr))
#define OUTREG8(mmap, addr, val)	__raw_writeb((val), (mmap)+(addr))

#define INREG64(mmap, addr)		__raw_readq((mmap)+(addr))
#define INREG32(mmap, addr)		__raw_readl((mmap)+(addr))
#define INREG16(mmap, addr)		__raw_readw((mmap)+(addr))
#define INREG8(mmap, addr)		__raw_readb((mmap)+(addr))

#define KB(x)	((x) * 1024)
#define MB(x)	(KB (KB (x)))
#define GB(x)	(MB (KB (x)))

#define A_SIZE_8(x)	((struct aper_size_info_8 *) x)
#define A_SIZE_16(x)	((struct aper_size_info_16 *) x)
#define A_SIZE_32(x)	((struct aper_size_info_32 *) x)
#define A_SIZE_LVL2(x)	((struct aper_size_info_lvl2 *) x)
#define A_SIZE_FIX(x)	((struct aper_size_info_fixed *) x)
#define A_IDX8(bridge)	(A_SIZE_8((bridge)->driver->aperture_sizes) + i)
#define A_IDX16(bridge)	(A_SIZE_16((bridge)->driver->aperture_sizes) + i)
#define A_IDX32(bridge)	(A_SIZE_32((bridge)->driver->aperture_sizes) + i)
#define MAXKEY		(4096 * 32)

#define PGE_EMPTY(b, p)	(!(p) || (p) == (unsigned long) (b)->scratch_page)


/* Intel registers */
#define INTEL_APSIZE	0xb4
#define INTEL_ATTBASE	0xb8
#define INTEL_AGPCTRL	0xb0
#define INTEL_NBXCFG	0x50
#define INTEL_ERRSTS	0x91

/* Intel i830 registers */
#define I830_GMCH_CTRL			0x52
#define I830_GMCH_ENABLED		0x4
#define I830_GMCH_MEM_MASK		0x1
#define I830_GMCH_MEM_64M		0x1
#define I830_GMCH_MEM_128M		0
#define I830_GMCH_GMS_MASK		0x70
#define I830_GMCH_GMS_DISABLED		0x00
#define I830_GMCH_GMS_LOCAL		0x10
#define I830_GMCH_GMS_STOLEN_512	0x20
#define I830_GMCH_GMS_STOLEN_1024	0x30
#define I830_GMCH_GMS_STOLEN_8192	0x40
#define I830_RDRAM_CHANNEL_TYPE		0x03010
#define I830_RDRAM_ND(x)		(((x) & 0x20) >> 5)
#define I830_RDRAM_DDT(x)		(((x) & 0x18) >> 3)

/* This one is for I830MP w. an external graphic card */
#define INTEL_I830_ERRSTS	0x92

/* Intel 855GM/852GM registers */
#define I855_GMCH_GMS_STOLEN_0M		0x0
#define I855_GMCH_GMS_STOLEN_1M		(0x1 << 4)
#define I855_GMCH_GMS_STOLEN_4M		(0x2 << 4)
#define I855_GMCH_GMS_STOLEN_8M		(0x3 << 4)
#define I855_GMCH_GMS_STOLEN_16M	(0x4 << 4)
#define I855_GMCH_GMS_STOLEN_32M	(0x5 << 4)
#define I85X_CAPID			0x44
#define I85X_VARIANT_MASK		0x7
#define I85X_VARIANT_SHIFT		5
#define I855_GME			0x0
#define I855_GM				0x4
#define I852_GME			0x2
#define I852_GM				0x5

/* Intel i845 registers */
#define INTEL_I845_AGPM		0x51
#define INTEL_I845_ERRSTS	0xc8

/* Intel i860 registers */
#define INTEL_I860_MCHCFG	0x50
#define INTEL_I860_ERRSTS	0xc8

/* Intel i810 registers */
#define I810_GMADDR		0x10
#define I810_MMADDR		0x14
#define I810_PTE_BASE		0x10000
#define I810_PTE_MAIN_UNCACHED	0x00000000
#define I810_PTE_LOCAL		0x00000002
#define I810_PTE_VALID		0x00000001
#define I810_SMRAM_MISCC	0x70
#define I810_GFX_MEM_WIN_SIZE	0x00010000
#define I810_GFX_MEM_WIN_32M	0x00010000
#define I810_GMS		0x000000c0
#define I810_GMS_DISABLE	0x00000000
#define I810_PGETBL_CTL		0x2020
#define I810_PGETBL_ENABLED	0x00000001
#define I810_DRAM_CTL		0x3000
#define I810_DRAM_ROW_0		0x00000001
#define I810_DRAM_ROW_0_SDRAM	0x00000001

struct agp_device_ids {
	unsigned short device_id; /* first, to make table easier to read */
	enum chipset_type chipset;
	const char *chipset_name;
	int (*chipset_setup) (struct pci_dev *pdev);	/* used to override generic */
};

/* Driver registration */
struct agp_bridge_data *agp_alloc_bridge(void);
void agp_put_bridge(struct agp_bridge_data *bridge);
int agp_add_bridge(struct agp_bridge_data *bridge);
void agp_remove_bridge(struct agp_bridge_data *bridge);

/* Frontend routines. */
int agp_frontend_initialize(void);
void agp_frontend_cleanup(void);

/* Generic routines. */
void agp_generic_enable(u32 mode);
int agp_generic_create_gatt_table(void);
int agp_generic_free_gatt_table(void);
struct agp_memory *agp_create_memory(int scratch_pages);
int agp_generic_insert_memory(struct agp_memory *mem, off_t pg_start, int type);
int agp_generic_remove_memory(struct agp_memory *mem, off_t pg_start, int type);
struct agp_memory *agp_generic_alloc_by_type(size_t page_count, int type);
void agp_generic_free_by_type(struct agp_memory *curr);
void *agp_generic_alloc_page(void);
void agp_generic_destroy_page(void *addr);
void agp_free_key(int key);
int agp_num_entries(void);
u32 agp_collect_device_status(u32 mode, u32 command);
void agp_device_command(u32 command, int agp_v3);
int agp_3_5_enable(struct agp_bridge_data *bridge);
void global_cache_flush(void);
void get_agp_version(struct agp_bridge_data *bridge);
unsigned long agp_generic_mask_memory(unsigned long addr, int type);

/* generic routines for agp>=3 */
int agp3_generic_fetch_size(void);
void agp3_generic_tlbflush(struct agp_memory *mem);
int agp3_generic_configure(void);
void agp3_generic_cleanup(void);

/* aperture sizes have been standardised since v3 */
#define AGP_GENERIC_SIZES_ENTRIES 11
extern struct aper_size_info_16 agp3_generic_sizes[];


extern int agp_off;
extern int agp_try_unsupported_boot;

/* Chipset independant registers (from AGP Spec) */
#define AGP_APBASE	0x10

#define AGPSTAT		0x4
#define AGPCMD		0x8
#define AGPNISTAT	0xc
#define AGPCTRL		0x10
#define AGPAPSIZE	0x14
#define AGPNEPG		0x16
#define AGPGARTLO	0x18
#define AGPGARTHI	0x1c
#define AGPNICMD	0x20

#define AGP_MAJOR_VERSION_SHIFT	(20)
#define AGP_MINOR_VERSION_SHIFT	(16)

#define AGPSTAT_RQ_DEPTH	(0xff000000)
#define AGPSTAT_RQ_DEPTH_SHIFT	24

#define AGPSTAT_CAL_MASK	(1<<12|1<<11|1<<10)
#define AGPSTAT_ARQSZ		(1<<15|1<<14|1<<13)
#define AGPSTAT_ARQSZ_SHIFT	13

#define AGPSTAT_SBA		(1<<9)
#define AGPSTAT_AGP_ENABLE	(1<<8)
#define AGPSTAT_FW		(1<<4)
#define AGPSTAT_MODE_3_0	(1<<3)

#define AGPSTAT2_1X		(1<<0)
#define AGPSTAT2_2X		(1<<1)
#define AGPSTAT2_4X		(1<<2)

#define AGPSTAT3_RSVD		(1<<2)
#define AGPSTAT3_8X		(1<<1)
#define AGPSTAT3_4X		(1)

#define AGPCTRL_APERENB		(1<<8)
#define AGPCTRL_GTLBEN		(1<<7)

#endif	/* _AGP_BACKEND_PRIV_H */
