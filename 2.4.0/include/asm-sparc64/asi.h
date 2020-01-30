/* $Id: asi.h,v 1.1 1996/11/20 12:59:45 davem Exp $ */
#ifndef _SPARC64_ASI_H
#define _SPARC64_ASI_H

/* asi.h:  Address Space Identifier values for the V9.
 *
 * Copyright (C) 1996 David S. Miller (davem@caip.rutgers.edu)
 */

/* V9 Architecture mandary ASIs. */
#define ASI_N			0x04 /* Nucleus					*/
#define ASI_NL			0x0c /* Nucleus, little endian			*/
#define ASI_AIUP		0x10 /* Primary, user				*/
#define ASI_AIUS		0x11 /* Secondary, user				*/
#define ASI_AIUPL		0x18 /* Primary, user, little endian		*/
#define ASI_AIUSL		0x19 /* Secondary, user, little endian		*/
#define ASI_P			0x80 /* Primary, implicit			*/
#define ASI_S			0x81 /* Secondary, implicit			*/
#define ASI_PNF			0x82 /* Primary, no fault			*/
#define ASI_SNF			0x83 /* Secondary, no fault			*/
#define ASI_PL			0x88 /* Primary, implicit, little endian	*/
#define ASI_SL			0x89 /* Secondary, implicit, little endian	*/
#define ASI_PNFL		0x8a /* Primary, no fault, little endian	*/
#define ASI_SNFL		0x8b /* Secondary, no fault, little endian	*/

/* SpitFire extended ASIs. */
#define ASI_PHYS_USE_EC		0x14 /* PADDR, E-cachable			*/
#define ASI_PHYS_BYPASS_EC_E	0x15 /* PADDR, E-cachable, E-bit		*/
#define ASI_PHYS_USE_EC_L	0x1c /* PADDR, E-cachable, little endian	*/
#define ASI_PHYS_BYPASS_EC_E_L	0x1d /* PADDR, E-cachable, E-bit, little endian	*/
#define ASI_NUCLEUS_QUAD_LDD	0x24 /* Cachable, qword load			*/
#define ASI_NUCLEUS_QUAD_LDD_L	0x2c /* Cachable, qword load, little endian 	*/
#define ASI_LSU_CONTROL		0x45 /* Load-store control unit			*/
#define ASI_DCACHE_DATA		0x46 /* Data cache data-ram diag access		*/
#define ASI_DCACHE_TAG		0x47 /* Data cache tag/valid ram diag access	*/
#define ASI_INTR_DISPATCH_STAT	0x48 /* IRQ vector dispatch status		*/
#define ASI_INTR_RECEIVE	0x49 /* IRQ vector receive status		*/
#define ASI_UPA_CONFIG		0x4a /* UPA config space			*/
#define ASI_ESTATE_ERROR_EN	0x4b /* E-cache error enable space		*/
#define ASI_AFSR		0x4c /* Async fault status register		*/
#define ASI_AFAR		0x4d /* Async fault address register		*/
#define ASI_EC_TAG_DATA		0x4e /* E-cache tag/valid ram diag access	*/
#define ASI_IMMU		0x50 /* Insn-MMU main register space		*/
#define ASI_IMMU_TSB_8KB_PTR	0x51 /* Insn-MMU 8KB TSB pointer register	*/
#define ASI_IMMU_TSB_64KB_PTR	0x52 /* Insn-MMU 64KB TSB pointer register	*/
#define ASI_ITLB_DATA_IN	0x54 /* Insn-MMU TLB data in register		*/
#define ASI_ITLB_DATA_ACCESS	0x55 /* Insn-MMU TLB data access register	*/
#define ASI_ITLB_TAG_READ	0x56 /* Insn-MMU TLB tag read register		*/
#define ASI_IMMU_DEMAP		0x57 /* Insn-MMU TLB demap			*/
#define ASI_DMMU		0x58 /* Data-MMU main register space		*/
#define ASI_DMMU_TSB_8KB_PTR	0x59 /* Data-MMU 8KB TSB pointer register	*/
#define ASI_DMMU_TSB_64KB_PTR	0x5a /* Data-MMU 16KB TSB pointer register	*/
#define ASI_DMMU_TSB_DIRECT_PTR	0x5b /* Data-MMU TSB direct pointer register	*/
#define ASI_DTLB_DATA_IN	0x5c /* Data-MMU TLB data in register		*/
#define ASI_DTLB_DATA_ACCESS	0x5d /* Data-MMU TLB data access register	*/
#define ASI_DTLB_TAG_READ	0x5e /* Data-MMU TLB tag read register		*/
#define ASI_DMMU_DEMAP		0x5f /* Data-MMU TLB demap			*/
#define ASI_IC_INSTR		0x66 /* Insn cache instrucion ram diag access	*/
#define ASI_IC_TAG		0x67 /* Insn cache tag/valid ram diag access	*/
#define ASI_IC_PRE_DECODE	0x6e /* Insn cache pre-decode ram diag access	*/
#define ASI_IC_NEXT_FIELD	0x6f /* Insn cache next-field ram diag access	*/
#define ASI_BLK_AIUP		0x70 /* Primary, user, block load/store		*/
#define ASI_BLK_AIUS		0x71 /* Secondary, user, block load/store	*/
#define ASI_EC_W		0x76 /* E-cache diag write access		*/
#define ASI_UDB_ERROR_W		0x77 /* External UDB error registers write	*/
#define ASI_UDB_CONTROL_W	0x77 /* External UDB control registers write	*/
#define ASI_UDB_INTR_W		0x77 /* External UDB IRQ vector dispatch write	*/
#define ASI_BLK_AIUPL		0x78 /* Primary, user, little, blk ld/st	*/
#define ASI_BLK_AIUSL		0x79 /* Secondary, user, little, blk ld/st	*/
#define ASI_EC_R		0x7e /* E-cache diag read access		*/
#define ASI_UDBH_ERROR_R	0x7f /* External UDB error registers read hi	*/
#define ASI_UDBL_ERROR_R	0x7f /* External UDB error registers read low	*/
#define ASI_UDBH_CONTROL_R	0x7f /* External UDB control registers read hi	*/
#define ASI_UDBL_CONTROL_R	0x7f /* External UDB control registers read low	*/
#define ASI_UDB_INTR_R		0x7f /* External UDB IRQ vector dispatch read	*/
#define ASI_PST8_P		0xc0 /* Primary, 8 8-bit, partial		*/
#define ASI_PST8_S		0xc1 /* Secondary, 8 8-bit, partial		*/
#define ASI_PST16_P		0xc2 /* Primary, 4 16-bit, partial		*/
#define ASI_PST16_S		0xc3 /* Seconary, 4 16-bit, partial		*/
#define ASI_PST32_P		0xc4 /* Primary, 2 32-bit, partial		*/
#define ASI_PST32_S		0xc5 /* Secondary, 2 32-bit, partial		*/
#define ASI_PST8_PL		0xc8 /* Primary, 8 8-bit, partial, little	*/
#define ASI_PST8_SL		0xc9 /* Secondary, 8 8-bit, partial, little	*/
#define ASI_PST16_PL		0xca /* Primary, 4 16-bit, partial, little	*/
#define ASI_PST16_SL		0xcb /* Seconary, 4 16-bit, partial, little	*/
#define ASI_PST32_PL		0xcc /* Primary, 2 32-bit, partial, little	*/
#define ASI_PST32_SL		0xcd /* Secondary, 2 32-bit, partial, little	*/
#define ASI_FL8_P		0xd0 /* Primary, 1 8-bit, fpu ld/st		*/
#define ASI_FL8_S		0xd1 /* Secondary, 1 8-bit, fpu ld/st		*/
#define ASI_FL16_P		0xd2 /* Primary, 1 16-bit, fpu ld/st		*/
#define ASI_FL16_S		0xd3 /* Secondary, 1 16-bit, fpu ld/st		*/
#define ASI_FL8_PL		0xd8 /* Primary, 1 8-bit, fpu ld/st, little	*/
#define ASI_FL8_SL		0xd9 /* Secondary, 1 8-bit, fpu ld/st, little	*/
#define ASI_FL16_PL		0xda /* Primary, 1 16-bit, fpu ld/st, little	*/
#define ASI_FL16_SL		0xdb /* Secondary, 1 16-bit, fpu ld/st, little	*/
#define ASI_BLK_COMMIT_P	0xe0 /* Primary, blk store commit		*/
#define ASI_BLK_COMMIT_S	0xe1 /* Secondary, blk store commit		*/
#define ASI_BLK_P		0xf0 /* Primary, blk ld/st			*/
#define ASI_BLK_S		0xf1 /* Secondary, blk ld/st			*/
#define ASI_BLK_PL		0xf8 /* Primary, blk ld/st, little		*/
#define ASI_BLK_SL		0xf9 /* Secondary, blk ld/st, little		*/

#endif /* _SPARC64_ASI_H */
