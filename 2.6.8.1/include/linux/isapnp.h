/*
 *  ISA Plug & Play support
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef LINUX_ISAPNP_H
#define LINUX_ISAPNP_H

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/pnp.h>

/*
 *  Configuration registers (TODO: change by specification)
 */ 

#define ISAPNP_CFG_ACTIVATE		0x30	/* byte */
#define ISAPNP_CFG_MEM			0x40	/* 4 * dword */
#define ISAPNP_CFG_PORT			0x60	/* 8 * word */
#define ISAPNP_CFG_IRQ			0x70	/* 2 * word */
#define ISAPNP_CFG_DMA			0x74	/* 2 * byte */

/*
 *
 */

#define ISAPNP_VENDOR(a,b,c)	(((((a)-'A'+1)&0x3f)<<2)|\
				((((b)-'A'+1)&0x18)>>3)|((((b)-'A'+1)&7)<<13)|\
				((((c)-'A'+1)&0x1f)<<8))
#define ISAPNP_DEVICE(x)	((((x)&0xf000)>>8)|\
				 (((x)&0x0f00)>>8)|\
				 (((x)&0x00f0)<<8)|\
				 (((x)&0x000f)<<8))
#define ISAPNP_FUNCTION(x)	ISAPNP_DEVICE(x)

/*
 *
 */

#ifdef __KERNEL__

#define DEVICE_COUNT_COMPATIBLE 4

#define ISAPNP_ANY_ID		0xffff
#define ISAPNP_CARD_DEVS	8

#define ISAPNP_CARD_ID(_va, _vb, _vc, _device) \
		.card_vendor = ISAPNP_VENDOR(_va, _vb, _vc), .card_device = ISAPNP_DEVICE(_device)
#define ISAPNP_CARD_END \
		.card_vendor = 0, .card_device = 0
#define ISAPNP_DEVICE_ID(_va, _vb, _vc, _function) \
		{ .vendor = ISAPNP_VENDOR(_va, _vb, _vc), .function = ISAPNP_FUNCTION(_function) }

/* export used IDs outside module */
#define ISAPNP_CARD_TABLE(name) \
		MODULE_GENERIC_TABLE(isapnp_card, name)

struct isapnp_card_id {
	unsigned long driver_data;	/* data private to the driver */
	unsigned short card_vendor, card_device;
	struct {
		unsigned short vendor, function;
	} devs[ISAPNP_CARD_DEVS];	/* logical devices */
};

#define ISAPNP_DEVICE_SINGLE(_cva, _cvb, _cvc, _cdevice, _dva, _dvb, _dvc, _dfunction) \
		.card_vendor = ISAPNP_VENDOR(_cva, _cvb, _cvc), .card_device =  ISAPNP_DEVICE(_cdevice), \
		.vendor = ISAPNP_VENDOR(_dva, _dvb, _dvc), .function = ISAPNP_FUNCTION(_dfunction)
#define ISAPNP_DEVICE_SINGLE_END \
		.card_vendor = 0, .card_device = 0

struct isapnp_device_id {
	unsigned short card_vendor, card_device;
	unsigned short vendor, function;
	unsigned long driver_data;	/* data private to the driver */
};

#if defined(CONFIG_ISAPNP) || (defined(CONFIG_ISAPNP_MODULE) && defined(MODULE))

#define __ISAPNP__

/* lowlevel configuration */
int isapnp_present(void);
int isapnp_cfg_begin(int csn, int device);
int isapnp_cfg_end(void);
unsigned char isapnp_read_byte(unsigned char idx);
unsigned short isapnp_read_word(unsigned char idx);
unsigned int isapnp_read_dword(unsigned char idx);
void isapnp_write_byte(unsigned char idx, unsigned char val);
void isapnp_write_word(unsigned char idx, unsigned short val);
void isapnp_write_dword(unsigned char idx, unsigned int val);
void isapnp_wake(unsigned char csn);
void isapnp_device(unsigned char device);
void isapnp_activate(unsigned char device);
void isapnp_deactivate(unsigned char device);
void *isapnp_alloc(long size);

#ifdef CONFIG_PROC_FS
int isapnp_proc_init(void);
int isapnp_proc_done(void);
#else
static inline int isapnp_proc_init(void) { return 0; }
static inline int isapnp_proc_done(void) { return 0; }
#endif

/* init/main.c */
int isapnp_init(void);

/* compat */
struct pnp_card *pnp_find_card(unsigned short vendor,
			       unsigned short device,
			       struct pnp_card *from);
struct pnp_dev *pnp_find_dev(struct pnp_card *card,
			     unsigned short vendor,
			     unsigned short function,
			     struct pnp_dev *from);

#else /* !CONFIG_ISAPNP */

/* lowlevel configuration */
static inline int isapnp_present(void) { return 0; }
static inline int isapnp_cfg_begin(int csn, int device) { return -ENODEV; }
static inline int isapnp_cfg_end(void) { return -ENODEV; }
static inline unsigned char isapnp_read_byte(unsigned char idx) { return 0xff; }
static inline unsigned short isapnp_read_word(unsigned char idx) { return 0xffff; }
static inline unsigned int isapnp_read_dword(unsigned char idx) { return 0xffffffff; }
static inline void isapnp_write_byte(unsigned char idx, unsigned char val) { ; }
static inline void isapnp_write_word(unsigned char idx, unsigned short val) { ; }
static inline void isapnp_write_dword(unsigned char idx, unsigned int val) { ; }
static inline void isapnp_wake(unsigned char csn) { ; }
static inline void isapnp_device(unsigned char device) { ; }
static inline void isapnp_activate(unsigned char device) { ; }
static inline void isapnp_deactivate(unsigned char device) { ; }

static inline struct pnp_card *pnp_find_card(unsigned short vendor,
					     unsigned short device,
					     struct pnp_card *from) { return NULL; }
static inline struct pnp_dev *pnp_find_dev(struct pnp_card *card,
					   unsigned short vendor,
					   unsigned short function,
					   struct pnp_dev *from) { return NULL; }

#endif /* CONFIG_ISAPNP */

#endif /* __KERNEL__ */
#endif /* LINUX_ISAPNP_H */
