/*
 * Routines common to all CFI-type probes.
 * (C) 2001-2003 Red Hat, Inc.
 * GPL'd
 * $Id: gen_probe.c,v 1.19 2004/07/13 22:33:32 dwmw2 Exp $
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/cfi.h>
#include <linux/mtd/gen_probe.h>

static struct mtd_info *check_cmd_set(struct map_info *, int);
static struct cfi_private *genprobe_ident_chips(struct map_info *map,
						struct chip_probe *cp);
static int genprobe_new_chip(struct map_info *map, struct chip_probe *cp,
			     struct cfi_private *cfi);

struct mtd_info *mtd_do_chip_probe(struct map_info *map, struct chip_probe *cp)
{
	struct mtd_info *mtd = NULL;
	struct cfi_private *cfi;

	/* First probe the map to see if we have CFI stuff there. */
	cfi = genprobe_ident_chips(map, cp);
	
	if (!cfi)
		return NULL;

	map->fldrv_priv = cfi;
	/* OK we liked it. Now find a driver for the command set it talks */

	mtd = check_cmd_set(map, 1); /* First the primary cmdset */
	if (!mtd)
		mtd = check_cmd_set(map, 0); /* Then the secondary */
	
	if (mtd)
		return mtd;

	printk(KERN_WARNING"gen_probe: No supported Vendor Command Set found\n");
	
	kfree(cfi->cfiq);
	kfree(cfi);
	map->fldrv_priv = NULL;
	return NULL;
}
EXPORT_SYMBOL(mtd_do_chip_probe);


static struct cfi_private *genprobe_ident_chips(struct map_info *map, struct chip_probe *cp)
{
	struct cfi_private cfi;
	struct cfi_private *retcfi;
	unsigned long *chip_map;
	int i, j, mapsize;
	int max_chips;

	memset(&cfi, 0, sizeof(cfi));

	/* Call the probetype-specific code with all permutations of 
	   interleave and device type, etc. */
	if (!genprobe_new_chip(map, cp, &cfi)) {
		/* The probe didn't like it */
		printk(KERN_WARNING "%s: Found no %s device at location zero\n",
		       cp->name, map->name);
		return NULL;
	}		

#if 0 /* Let the CFI probe routine do this sanity check. The Intel and AMD
	 probe routines won't ever return a broken CFI structure anyway,
	 because they make them up themselves.
      */
	if (cfi.cfiq->NumEraseRegions == 0) {
		printk(KERN_WARNING "Number of erase regions is zero\n");
		kfree(cfi.cfiq);
		return NULL;
	}
#endif
	cfi.chipshift = cfi.cfiq->DevSize;

	if (cfi_interleave_is_1(&cfi)) {
		;
	} else if (cfi_interleave_is_2(&cfi)) {
		cfi.chipshift++;
	} else if (cfi_interleave_is_4((&cfi))) {
		cfi.chipshift += 2;
	} else if (cfi_interleave_is_8(&cfi)) {
		cfi.chipshift += 3;
	} else {
		BUG();
	}
		
	cfi.numchips = 1;

	/* 
	 * Allocate memory for bitmap of valid chips. 
	 * Align bitmap storage size to full byte. 
	 */ 
	max_chips = map->size >> cfi.chipshift;
	mapsize = (max_chips / 8) + ((max_chips % 8) ? 1 : 0);
	chip_map = kmalloc(mapsize, GFP_KERNEL);
	if (!chip_map) {
		printk(KERN_WARNING "%s: kmalloc failed for CFI chip map\n", map->name);
		kfree(cfi.cfiq);
		return NULL;
	}
	memset (chip_map, 0, mapsize);

	set_bit(0, chip_map); /* Mark first chip valid */

	/*
	 * Now probe for other chips, checking sensibly for aliases while
	 * we're at it. The new_chip probe above should have let the first
	 * chip in read mode.
	 */

	for (i = 1; i < max_chips; i++) {
		cp->probe_chip(map, i << cfi.chipshift, chip_map, &cfi);
	}

	/*
	 * Now allocate the space for the structures we need to return to 
	 * our caller, and copy the appropriate data into them.
	 */

	retcfi = kmalloc(sizeof(struct cfi_private) + cfi.numchips * sizeof(struct flchip), GFP_KERNEL);

	if (!retcfi) {
		printk(KERN_WARNING "%s: kmalloc failed for CFI private structure\n", map->name);
		kfree(cfi.cfiq);
		kfree(chip_map);
		return NULL;
	}

	memcpy(retcfi, &cfi, sizeof(cfi));
	memset(&retcfi->chips[0], 0, sizeof(struct flchip) * cfi.numchips);

	for (i = 0, j = 0; (j < cfi.numchips) && (i < max_chips); i++) {
		if(test_bit(i, chip_map)) {
			struct flchip *pchip = &retcfi->chips[j++];

			pchip->start = (i << cfi.chipshift);
			pchip->state = FL_READY;
			init_waitqueue_head(&pchip->wq);
			spin_lock_init(&pchip->_spinlock);
			pchip->mutex = &pchip->_spinlock;
		}
	}

	kfree(chip_map);
	return retcfi;
}

	
static int genprobe_new_chip(struct map_info *map, struct chip_probe *cp,
			     struct cfi_private *cfi)
{
	int min_chips = (map_bankwidth(map)/4?:1); /* At most 4-bytes wide. */
	int max_chips = map_bankwidth(map); /* And minimum 1 */
	int nr_chips, type;

	for (nr_chips = min_chips; nr_chips <= max_chips; nr_chips <<= 1) {

		if (!cfi_interleave_supported(nr_chips))
		    continue;

		cfi->interleave = nr_chips;

		for (type = 0; type < 3; type++) {
			cfi->device_type = 1<<type;

			if (cp->probe_chip(map, 0, NULL, cfi))
				return 1;
		}
	}
	return 0;
}

typedef struct mtd_info *cfi_cmdset_fn_t(struct map_info *, int);

extern cfi_cmdset_fn_t cfi_cmdset_0001;
extern cfi_cmdset_fn_t cfi_cmdset_0002;
extern cfi_cmdset_fn_t cfi_cmdset_0020;

static inline struct mtd_info *cfi_cmdset_unknown(struct map_info *map, 
						  int primary)
{
	struct cfi_private *cfi = map->fldrv_priv;
	__u16 type = primary?cfi->cfiq->P_ID:cfi->cfiq->A_ID;
#if defined(CONFIG_MODULES) && defined(HAVE_INTER_MODULE)
	char probename[32];
	cfi_cmdset_fn_t *probe_function;

	sprintf(probename, "cfi_cmdset_%4.4X", type);
		
	probe_function = inter_module_get_request(probename, probename);

	if (probe_function) {
		struct mtd_info *mtd;

		mtd = (*probe_function)(map, primary);
		/* If it was happy, it'll have increased its own use count */
		inter_module_put(probename);
		return mtd;
	}
#endif
	printk(KERN_NOTICE "Support for command set %04X not present\n",
	       type);

	return NULL;
}

static struct mtd_info *check_cmd_set(struct map_info *map, int primary)
{
	struct cfi_private *cfi = map->fldrv_priv;
	__u16 type = primary?cfi->cfiq->P_ID:cfi->cfiq->A_ID;
	
	if (type == P_ID_NONE || type == P_ID_RESERVED)
		return NULL;

	switch(type){
		/* Urgh. Ifdefs. The version with weak symbols was
		 * _much_ nicer. Shame it didn't seem to work on
		 * anything but x86, really.
		 * But we can't rely in inter_module_get() because
		 * that'd mean we depend on link order.
		 */
#ifdef CONFIG_MTD_CFI_INTELEXT
	case 0x0001:
	case 0x0003:
		return cfi_cmdset_0001(map, primary);
#endif
#ifdef CONFIG_MTD_CFI_AMDSTD
	case 0x0002:
		return cfi_cmdset_0002(map, primary);
#endif
#ifdef CONFIG_MTD_CFI_STAA
        case 0x0020:
		return cfi_cmdset_0020(map, primary);
#endif
	}

	return cfi_cmdset_unknown(map, primary);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org>");
MODULE_DESCRIPTION("Helper routines for flash chip probe code");
