/*
 * Common Flash Interface support:
 *   Intel Extended Vendor Command Set (ID 0x0001)
 *
 * (C) 2000 Red Hat. GPL'd
 *
 * $Id: cfi_cmdset_0001.c,v 1.154 2004/08/09 13:19:43 dwmw2 Exp $
 *
 * 
 * 10/10/2000	Nicolas Pitre <nico@cam.org>
 * 	- completely revamped method functions so they are aware and
 * 	  independent of the flash geometry (buswidth, interleave, etc.)
 * 	- scalability vs code size is completely set at compile-time
 * 	  (see include/linux/mtd/cfi.h for selection)
 *	- optimized write buffer method
 * 02/05/2002	Christopher Hoover <ch@hpl.hp.com>/<ch@murgatroid.com>
 *	- reworked lock/unlock/erase support for var size flash
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/byteorder.h>

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mtd/map.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/compatmac.h>
#include <linux/mtd/cfi.h>

/* #define CMDSET0001_DISABLE_ERASE_SUSPEND_ON_WRITE */

// debugging, turns off buffer write mode if set to 1
#define FORCE_WORD_WRITE 0

static int cfi_intelext_read (struct mtd_info *, loff_t, size_t, size_t *, u_char *);
//static int cfi_intelext_read_user_prot_reg (struct mtd_info *, loff_t, size_t, size_t *, u_char *);
//static int cfi_intelext_read_fact_prot_reg (struct mtd_info *, loff_t, size_t, size_t *, u_char *);
static int cfi_intelext_write_words(struct mtd_info *, loff_t, size_t, size_t *, const u_char *);
static int cfi_intelext_write_buffers(struct mtd_info *, loff_t, size_t, size_t *, const u_char *);
static int cfi_intelext_erase_varsize(struct mtd_info *, struct erase_info *);
static void cfi_intelext_sync (struct mtd_info *);
static int cfi_intelext_lock(struct mtd_info *mtd, loff_t ofs, size_t len);
static int cfi_intelext_unlock(struct mtd_info *mtd, loff_t ofs, size_t len);
static int cfi_intelext_suspend (struct mtd_info *);
static void cfi_intelext_resume (struct mtd_info *);

static void cfi_intelext_destroy(struct mtd_info *);

struct mtd_info *cfi_cmdset_0001(struct map_info *, int);

static struct mtd_info *cfi_intelext_setup (struct map_info *);
static int cfi_intelext_partition_fixup(struct map_info *, struct cfi_private **);

static int cfi_intelext_point (struct mtd_info *mtd, loff_t from, size_t len,
		     size_t *retlen, u_char **mtdbuf);
static void cfi_intelext_unpoint (struct mtd_info *mtd, u_char *addr, loff_t from,
			size_t len);


/*
 *  *********** SETUP AND PROBE BITS  ***********
 */

static struct mtd_chip_driver cfi_intelext_chipdrv = {
	.probe		= NULL, /* Not usable directly */
	.destroy	= cfi_intelext_destroy,
	.name		= "cfi_cmdset_0001",
	.module		= THIS_MODULE
};

/* #define DEBUG_LOCK_BITS */
/* #define DEBUG_CFI_FEATURES */

#ifdef DEBUG_CFI_FEATURES
static void cfi_tell_features(struct cfi_pri_intelext *extp)
{
	int i;
	printk("  Feature/Command Support:      %4.4X\n", extp->FeatureSupport);
	printk("     - Chip Erase:              %s\n", extp->FeatureSupport&1?"supported":"unsupported");
	printk("     - Suspend Erase:           %s\n", extp->FeatureSupport&2?"supported":"unsupported");
	printk("     - Suspend Program:         %s\n", extp->FeatureSupport&4?"supported":"unsupported");
	printk("     - Legacy Lock/Unlock:      %s\n", extp->FeatureSupport&8?"supported":"unsupported");
	printk("     - Queued Erase:            %s\n", extp->FeatureSupport&16?"supported":"unsupported");
	printk("     - Instant block lock:      %s\n", extp->FeatureSupport&32?"supported":"unsupported");
	printk("     - Protection Bits:         %s\n", extp->FeatureSupport&64?"supported":"unsupported");
	printk("     - Page-mode read:          %s\n", extp->FeatureSupport&128?"supported":"unsupported");
	printk("     - Synchronous read:        %s\n", extp->FeatureSupport&256?"supported":"unsupported");
	printk("     - Simultaneous operations: %s\n", extp->FeatureSupport&512?"supported":"unsupported");
	for (i=10; i<32; i++) {
		if (extp->FeatureSupport & (1<<i)) 
			printk("     - Unknown Bit %X:      supported\n", i);
	}
	
	printk("  Supported functions after Suspend: %2.2X\n", extp->SuspendCmdSupport);
	printk("     - Program after Erase Suspend: %s\n", extp->SuspendCmdSupport&1?"supported":"unsupported");
	for (i=1; i<8; i++) {
		if (extp->SuspendCmdSupport & (1<<i))
			printk("     - Unknown Bit %X:               supported\n", i);
	}
	
	printk("  Block Status Register Mask: %4.4X\n", extp->BlkStatusRegMask);
	printk("     - Lock Bit Active:      %s\n", extp->BlkStatusRegMask&1?"yes":"no");
	printk("     - Valid Bit Active:     %s\n", extp->BlkStatusRegMask&2?"yes":"no");
	for (i=2; i<16; i++) {
		if (extp->BlkStatusRegMask & (1<<i))
			printk("     - Unknown Bit %X Active: yes\n",i);
	}
	
	printk("  Vcc Logic Supply Optimum Program/Erase Voltage: %d.%d V\n", 
	       extp->VccOptimal >> 4, extp->VccOptimal & 0xf);
	if (extp->VppOptimal)
		printk("  Vpp Programming Supply Optimum Program/Erase Voltage: %d.%d V\n", 
		       extp->VppOptimal >> 4, extp->VppOptimal & 0xf);
}
#endif

#ifdef CMDSET0001_DISABLE_ERASE_SUSPEND_ON_WRITE
/* Some Intel Strata Flash prior to FPO revision C has bugs in this area */ 
static void fixup_intel_strataflash(struct map_info *map, void* param)
{
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_amdstd *extp = cfi->cmdset_priv;

	printk(KERN_WARNING "cfi_cmdset_0001: Suspend "
	                    "erase on write disabled.\n");
	extp->SuspendCmdSupport &= ~1;
}
#endif

static void fixup_st_m28w320ct(struct map_info *map, void* param)
{
	struct cfi_private *cfi = map->fldrv_priv;
	
	cfi->cfiq->BufWriteTimeoutTyp = 0;	/* Not supported */
	cfi->cfiq->BufWriteTimeoutMax = 0;	/* Not supported */
}

static void fixup_st_m28w320cb(struct map_info *map, void* param)
{
	struct cfi_private *cfi = map->fldrv_priv;
	
	/* Note this is done after the region info is endian swapped */
	cfi->cfiq->EraseRegionInfo[1] =
		(cfi->cfiq->EraseRegionInfo[1] & 0xffff0000) | 0x3e;
};

static struct cfi_fixup fixup_table[] = {
#ifdef CMDSET0001_DISABLE_ERASE_SUSPEND_ON_WRITE
	{
		CFI_MFR_ANY, CFI_ID_ANY,
		fixup_intel_strataflash, NULL
	}, 
#endif
	{
		0x0020,	/* STMicroelectronics */
		0x00ba,	/* M28W320CT */
		fixup_st_m28w320ct, NULL
	}, {
		0x0020,	/* STMicroelectronics */
		0x00bb,	/* M28W320CB */
		fixup_st_m28w320cb, NULL
	}, {
		0, 0, NULL, NULL
	}
};

/* This routine is made available to other mtd code via
 * inter_module_register.  It must only be accessed through
 * inter_module_get which will bump the use count of this module.  The
 * addresses passed back in cfi are valid as long as the use count of
 * this module is non-zero, i.e. between inter_module_get and
 * inter_module_put.  Keith Owens <kaos@ocs.com.au> 29 Oct 2000.
 */
struct mtd_info *cfi_cmdset_0001(struct map_info *map, int primary)
{
	struct cfi_private *cfi = map->fldrv_priv;
	int i;

	if (cfi->cfi_mode == CFI_MODE_CFI) {
		/* 
		 * It's a real CFI chip, not one for which the probe
		 * routine faked a CFI structure. So we read the feature
		 * table from it.
		 */
		__u16 adr = primary?cfi->cfiq->P_ADR:cfi->cfiq->A_ADR;
		struct cfi_pri_intelext *extp;

		extp = (struct cfi_pri_intelext*)cfi_read_pri(map, adr, sizeof(*extp), "Intel/Sharp");
		if (!extp)
			return NULL;
		
		/* Do some byteswapping if necessary */
		extp->FeatureSupport = le32_to_cpu(extp->FeatureSupport);
		extp->BlkStatusRegMask = le16_to_cpu(extp->BlkStatusRegMask);
		extp->ProtRegAddr = le16_to_cpu(extp->ProtRegAddr);

		/* Install our own private info structure */
		cfi->cmdset_priv = extp;	

		cfi_fixup(map, fixup_table);
			
#ifdef DEBUG_CFI_FEATURES
		/* Tell the user about it in lots of lovely detail */
		cfi_tell_features(extp);
#endif	

		if(extp->SuspendCmdSupport & 1) {
			printk(KERN_NOTICE "cfi_cmdset_0001: Erase suspend on write enabled\n");
		}
	}

	for (i=0; i< cfi->numchips; i++) {
		cfi->chips[i].word_write_time = 1<<cfi->cfiq->WordWriteTimeoutTyp;
		cfi->chips[i].buffer_write_time = 1<<cfi->cfiq->BufWriteTimeoutTyp;
		cfi->chips[i].erase_time = 1<<cfi->cfiq->BlockEraseTimeoutTyp;
		cfi->chips[i].ref_point_counter = 0;
	}		

	map->fldrv = &cfi_intelext_chipdrv;
	
	return cfi_intelext_setup(map);
}

static struct mtd_info *cfi_intelext_setup(struct map_info *map)
{
	struct cfi_private *cfi = map->fldrv_priv;
	struct mtd_info *mtd;
	unsigned long offset = 0;
	int i,j;
	unsigned long devsize = (1<<cfi->cfiq->DevSize) * cfi->interleave;

	mtd = kmalloc(sizeof(*mtd), GFP_KERNEL);
	//printk(KERN_DEBUG "number of CFI chips: %d\n", cfi->numchips);

	if (!mtd) {
		printk(KERN_ERR "Failed to allocate memory for MTD device\n");
		goto setup_err;
	}

	memset(mtd, 0, sizeof(*mtd));
	mtd->priv = map;
	mtd->type = MTD_NORFLASH;
	mtd->size = devsize * cfi->numchips;

	mtd->numeraseregions = cfi->cfiq->NumEraseRegions * cfi->numchips;
	mtd->eraseregions = kmalloc(sizeof(struct mtd_erase_region_info) 
			* mtd->numeraseregions, GFP_KERNEL);
	if (!mtd->eraseregions) { 
		printk(KERN_ERR "Failed to allocate memory for MTD erase region info\n");
		goto setup_err;
	}
	
	for (i=0; i<cfi->cfiq->NumEraseRegions; i++) {
		unsigned long ernum, ersize;
		ersize = ((cfi->cfiq->EraseRegionInfo[i] >> 8) & ~0xff) * cfi->interleave;
		ernum = (cfi->cfiq->EraseRegionInfo[i] & 0xffff) + 1;

		if (mtd->erasesize < ersize) {
			mtd->erasesize = ersize;
		}
		for (j=0; j<cfi->numchips; j++) {
			mtd->eraseregions[(j*cfi->cfiq->NumEraseRegions)+i].offset = (j*devsize)+offset;
			mtd->eraseregions[(j*cfi->cfiq->NumEraseRegions)+i].erasesize = ersize;
			mtd->eraseregions[(j*cfi->cfiq->NumEraseRegions)+i].numblocks = ernum;
		}
		offset += (ersize * ernum);
	}

	if (offset != devsize) {
		/* Argh */
		printk(KERN_WARNING "Sum of regions (%lx) != total size of set of interleaved chips (%lx)\n", offset, devsize);
		goto setup_err;
	}

	for (i=0; i<mtd->numeraseregions;i++){
		printk(KERN_DEBUG "%d: offset=0x%x,size=0x%x,blocks=%d\n",
		       i,mtd->eraseregions[i].offset,
		       mtd->eraseregions[i].erasesize,
		       mtd->eraseregions[i].numblocks);
	}

	/* Also select the correct geometry setup too */ 
	mtd->erase = cfi_intelext_erase_varsize;
	mtd->read = cfi_intelext_read;

	if (map_is_linear(map)) {
		mtd->point = cfi_intelext_point;
		mtd->unpoint = cfi_intelext_unpoint;
	}

	if ( cfi->cfiq->BufWriteTimeoutTyp && !FORCE_WORD_WRITE) {
		printk(KERN_INFO "Using buffer write method\n" );
		mtd->write = cfi_intelext_write_buffers;
	} else {
		printk(KERN_INFO "Using word write method\n" );
		mtd->write = cfi_intelext_write_words;
	}
#if 0
	mtd->read_user_prot_reg = cfi_intelext_read_user_prot_reg;
	mtd->read_fact_prot_reg = cfi_intelext_read_fact_prot_reg;
#endif
	mtd->sync = cfi_intelext_sync;
	mtd->lock = cfi_intelext_lock;
	mtd->unlock = cfi_intelext_unlock;
	mtd->suspend = cfi_intelext_suspend;
	mtd->resume = cfi_intelext_resume;
	mtd->flags = MTD_CAP_NORFLASH;
	map->fldrv = &cfi_intelext_chipdrv;
	mtd->name = map->name;

	/* This function has the potential to distort the reality
	   a bit and therefore should be called last. */
	if (cfi_intelext_partition_fixup(map, &cfi) != 0)
		goto setup_err;

	__module_get(THIS_MODULE);
	return mtd;

 setup_err:
	if(mtd) {
		if(mtd->eraseregions)
			kfree(mtd->eraseregions);
		kfree(mtd);
	}
	kfree(cfi->cmdset_priv);
	return NULL;
}

static int cfi_intelext_partition_fixup(struct map_info *map,
					struct cfi_private **pcfi)
{
	struct cfi_private *cfi = *pcfi;
	struct cfi_pri_intelext *extp = cfi->cmdset_priv;

	/*
	 * Probing of multi-partition flash ships.
	 *
	 * This is extremely crude at the moment and should probably be
	 * extracted entirely from the Intel extended query data instead.
	 * Right now a L18 flash is assumed if multiple operations is
	 * detected.
	 *
	 * To support multiple partitions when available, we simply arrange
	 * for each of them to have their own flchip structure even if they
	 * are on the same physical chip.  This means completely recreating
	 * a new cfi_private structure right here which is a blatent code
	 * layering violation, but this is still the least intrusive
	 * arrangement at this point. This can be rearranged in the future
	 * if someone feels motivated enough.  --nico
	 */
	if (extp && extp->FeatureSupport & (1 << 9)) {
		struct cfi_private *newcfi;
		struct flchip *chip;
		struct flchip_shared *shared;
		int numparts, partshift, numvirtchips, i, j;

		/*
		 * The L18 flash memory array is divided
		 * into multiple 8-Mbit partitions.
		 */
		numparts = 1 << (cfi->cfiq->DevSize - 20);
		partshift = 20 + __ffs(cfi->interleave);
		numvirtchips = cfi->numchips * numparts;

		newcfi = kmalloc(sizeof(struct cfi_private) + numvirtchips * sizeof(struct flchip), GFP_KERNEL);
		if (!newcfi)
			return -ENOMEM;
		shared = kmalloc(sizeof(struct flchip_shared) * cfi->numchips, GFP_KERNEL);
		if (!shared) {
			kfree(newcfi);
			return -ENOMEM;
		}
		memcpy(newcfi, cfi, sizeof(struct cfi_private));
		newcfi->numchips = numvirtchips;
		newcfi->chipshift = partshift;

		chip = &newcfi->chips[0];
		for (i = 0; i < cfi->numchips; i++) {
			shared[i].writing = shared[i].erasing = NULL;
			spin_lock_init(&shared[i].lock);
			for (j = 0; j < numparts; j++) {
				*chip = cfi->chips[i];
				chip->start += j << partshift;
				chip->priv = &shared[i];
				/* those should be reset too since
				   they create memory references. */
				init_waitqueue_head(&chip->wq);
				spin_lock_init(&chip->_spinlock);
				chip->mutex = &chip->_spinlock;
				chip++;
			}
		}

		printk(KERN_DEBUG "%s: %d sets of %d interleaved chips "
				  "--> %d partitions of %#x bytes\n",
				  map->name, cfi->numchips, cfi->interleave,
				  newcfi->numchips, 1<<newcfi->chipshift);

		map->fldrv_priv = newcfi;
		*pcfi = newcfi;
		kfree(cfi);
	}

	return 0;
}

/*
 *  *********** CHIP ACCESS FUNCTIONS ***********
 */

static int get_chip(struct map_info *map, struct flchip *chip, unsigned long adr, int mode)
{
	DECLARE_WAITQUEUE(wait, current);
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK = CMD(0x80), status_PWS = CMD(0x01);
	unsigned long timeo;
	struct cfi_pri_intelext *cfip = cfi->cmdset_priv;

 resettime:
	timeo = jiffies + HZ;
 retry:
	if (chip->priv && (mode == FL_WRITING || mode == FL_ERASING)) {
		/*
		 * OK. We have possibility for contension on the write/erase
		 * operations which are global to the real chip and not per
		 * partition.  So let's fight it over in the partition which
		 * currently has authority on the operation.
		 *
		 * The rules are as follows:
		 *
		 * - any write operation must own shared->writing.
		 *
		 * - any erase operation must own _both_ shared->writing and
		 *   shared->erasing.
		 *
		 * - contension arbitration is handled in the owner's context.
		 *
		 * The 'shared' struct can be read when its lock is taken.
		 * However any writes to it can only be made when the current
		 * owner's lock is also held.
		 */
		struct flchip_shared *shared = chip->priv;
		struct flchip *contender;
		spin_lock(&shared->lock);
		contender = shared->writing;
		if (contender && contender != chip) {
			/*
			 * The engine to perform desired operation on this
			 * partition is already in use by someone else.
			 * Let's fight over it in the context of the chip
			 * currently using it.  If it is possible to suspend,
			 * that other partition will do just that, otherwise
			 * it'll happily send us to sleep.  In any case, when
			 * get_chip returns success we're clear to go ahead.
			 */
			int ret = spin_trylock(contender->mutex);
			spin_unlock(&shared->lock);
			if (!ret)
				goto retry;
			spin_unlock(chip->mutex);
			ret = get_chip(map, contender, contender->start, mode);
			spin_lock(chip->mutex);
			if (ret) {
				spin_unlock(contender->mutex);
				return ret;
			}
			timeo = jiffies + HZ;
			spin_lock(&shared->lock);
		}

		/* We now own it */
		shared->writing = chip;
		if (mode == FL_ERASING)
			shared->erasing = chip;
		if (contender && contender != chip)
			spin_unlock(contender->mutex);
		spin_unlock(&shared->lock);
	}

	switch (chip->state) {

	case FL_STATUS:
		for (;;) {
			status = map_read(map, adr);
			if (map_word_andequal(map, status, status_OK, status_OK))
				break;

			/* At this point we're fine with write operations
			   in other partitions as they don't conflict. */
			if (chip->priv && map_word_andequal(map, status, status_PWS, status_PWS))
				break;

			if (time_after(jiffies, timeo)) {
				printk(KERN_ERR "Waiting for chip to be ready timed out. Status %lx\n", 
				       status.x[0]);
				return -EIO;
			}
			spin_unlock(chip->mutex);
			cfi_udelay(1);
			spin_lock(chip->mutex);
			/* Someone else might have been playing with it. */
			goto retry;
		}
				
	case FL_READY:
	case FL_CFI_QUERY:
	case FL_JEDEC_QUERY:
		return 0;

	case FL_ERASING:
		if (!(cfip->FeatureSupport & 2) ||
		    !(mode == FL_READY || mode == FL_POINT ||
		     (mode == FL_WRITING && (cfip->SuspendCmdSupport & 1))))
			goto sleep;


		/* Erase suspend */
		map_write(map, CMD(0xB0), adr);

		/* If the flash has finished erasing, then 'erase suspend'
		 * appears to make some (28F320) flash devices switch to
		 * 'read' mode.  Make sure that we switch to 'read status'
		 * mode so we get the right data. --rmk
		 */
		map_write(map, CMD(0x70), adr);
		chip->oldstate = FL_ERASING;
		chip->state = FL_ERASE_SUSPENDING;
		chip->erase_suspended = 1;
		for (;;) {
			status = map_read(map, adr);
			if (map_word_andequal(map, status, status_OK, status_OK))
			        break;

			if (time_after(jiffies, timeo)) {
				/* Urgh. Resume and pretend we weren't here.  */
				map_write(map, CMD(0xd0), adr);
				/* Make sure we're in 'read status' mode if it had finished */
				map_write(map, CMD(0x70), adr);
				chip->state = FL_ERASING;
				chip->oldstate = FL_READY;
				printk(KERN_ERR "Chip not ready after erase "
				       "suspended: status = 0x%lx\n", status.x[0]);
				return -EIO;
			}

			spin_unlock(chip->mutex);
			cfi_udelay(1);
			spin_lock(chip->mutex);
			/* Nobody will touch it while it's in state FL_ERASE_SUSPENDING.
			   So we can just loop here. */
		}
		chip->state = FL_STATUS;
		return 0;

	case FL_POINT:
		/* Only if there's no operation suspended... */
		if (mode == FL_READY && chip->oldstate == FL_READY)
			return 0;

	default:
	sleep:
		set_current_state(TASK_UNINTERRUPTIBLE);
		add_wait_queue(&chip->wq, &wait);
		spin_unlock(chip->mutex);
		schedule();
		remove_wait_queue(&chip->wq, &wait);
		spin_lock(chip->mutex);
		goto resettime;
	}
}

static void put_chip(struct map_info *map, struct flchip *chip, unsigned long adr)
{
	struct cfi_private *cfi = map->fldrv_priv;

	if (chip->priv) {
		struct flchip_shared *shared = chip->priv;
		spin_lock(&shared->lock);
		if (shared->writing == chip) {
			/* We own the ability to write, but we're done */
			shared->writing = shared->erasing;
			if (shared->writing && shared->writing != chip) {
				/* give back ownership to who we loaned it from */
				struct flchip *loaner = shared->writing;
				spin_lock(loaner->mutex);
				spin_unlock(&shared->lock);
				spin_unlock(chip->mutex);
				put_chip(map, loaner, loaner->start);
				spin_lock(chip->mutex);
				spin_unlock(loaner->mutex);
			} else {
				if (chip->oldstate != FL_ERASING) {
					shared->erasing = NULL;
					if (chip->oldstate != FL_WRITING)
						shared->writing = NULL;
				}
				spin_unlock(&shared->lock);
			}
		}
	}

	switch(chip->oldstate) {
	case FL_ERASING:
		chip->state = chip->oldstate;
		/* What if one interleaved chip has finished and the 
		   other hasn't? The old code would leave the finished
		   one in READY mode. That's bad, and caused -EROFS 
		   errors to be returned from do_erase_oneblock because
		   that's the only bit it checked for at the time.
		   As the state machine appears to explicitly allow 
		   sending the 0x70 (Read Status) command to an erasing
		   chip and expecting it to be ignored, that's what we 
		   do. */
		map_write(map, CMD(0xd0), adr);
		map_write(map, CMD(0x70), adr);
		chip->oldstate = FL_READY;
		chip->state = FL_ERASING;
		break;

	case FL_READY:
	case FL_STATUS:
	case FL_JEDEC_QUERY:
		/* We should really make set_vpp() count, rather than doing this */
		DISABLE_VPP(map);
		break;
	default:
		printk(KERN_ERR "put_chip() called with oldstate %d!!\n", chip->oldstate);
	}
	wake_up(&chip->wq);
}

static int do_point_onechip (struct map_info *map, struct flchip *chip, loff_t adr, size_t len)
{
	unsigned long cmd_addr;
	struct cfi_private *cfi = map->fldrv_priv;
	int ret = 0;

	adr += chip->start;

	/* Ensure cmd read/writes are aligned. */ 
	cmd_addr = adr & ~(map_bankwidth(map)-1); 

	spin_lock(chip->mutex);

	ret = get_chip(map, chip, cmd_addr, FL_POINT);

	if (!ret) {
		if (chip->state != FL_POINT && chip->state != FL_READY)
			map_write(map, CMD(0xff), cmd_addr);

		chip->state = FL_POINT;
		chip->ref_point_counter++;
	}
	spin_unlock(chip->mutex);

	return ret;
}

static int cfi_intelext_point (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char **mtdbuf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long ofs;
	int chipnum;
	int ret = 0;

	if (!map->virt || (from + len > mtd->size))
		return -EINVAL;
	
	*mtdbuf = (void *)map->virt + from;
	*retlen = 0;

	/* Now lock the chip(s) to POINT state */

	/* ofs: offset within the first chip that the first read should start */
	chipnum = (from >> cfi->chipshift);
	ofs = from - (chipnum << cfi->chipshift);

	while (len) {
		unsigned long thislen;

		if (chipnum >= cfi->numchips)
			break;

		if ((len + ofs -1) >> cfi->chipshift)
			thislen = (1<<cfi->chipshift) - ofs;
		else
			thislen = len;

		ret = do_point_onechip(map, &cfi->chips[chipnum], ofs, thislen);
		if (ret)
			break;

		*retlen += thislen;
		len -= thislen;
		
		ofs = 0;
		chipnum++;
	}
	return 0;
}

static void cfi_intelext_unpoint (struct mtd_info *mtd, u_char *addr, loff_t from, size_t len)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long ofs;
	int chipnum;

	/* Now unlock the chip(s) POINT state */

	/* ofs: offset within the first chip that the first read should start */
	chipnum = (from >> cfi->chipshift);
	ofs = from - (chipnum <<  cfi->chipshift);

	while (len) {
		unsigned long thislen;
		struct flchip *chip;

		chip = &cfi->chips[chipnum];
		if (chipnum >= cfi->numchips)
			break;

		if ((len + ofs -1) >> cfi->chipshift)
			thislen = (1<<cfi->chipshift) - ofs;
		else
			thislen = len;

		spin_lock(chip->mutex);
		if (chip->state == FL_POINT) {
			chip->ref_point_counter--;
			if(chip->ref_point_counter == 0)
				chip->state = FL_READY;
		} else
			printk(KERN_ERR "Warning: unpoint called on non pointed region\n"); /* Should this give an error? */

		put_chip(map, chip, chip->start);
		spin_unlock(chip->mutex);

		len -= thislen;
		ofs = 0;
		chipnum++;
	}
}

static inline int do_read_onechip(struct map_info *map, struct flchip *chip, loff_t adr, size_t len, u_char *buf)
{
	unsigned long cmd_addr;
	struct cfi_private *cfi = map->fldrv_priv;
	int ret;

	adr += chip->start;

	/* Ensure cmd read/writes are aligned. */ 
	cmd_addr = adr & ~(map_bankwidth(map)-1); 

	spin_lock(chip->mutex);
	ret = get_chip(map, chip, cmd_addr, FL_READY);
	if (ret) {
		spin_unlock(chip->mutex);
		return ret;
	}

	if (chip->state != FL_POINT && chip->state != FL_READY) {
		map_write(map, CMD(0xff), cmd_addr);

		chip->state = FL_READY;
	}

	map_copy_from(map, buf, adr, len);

	put_chip(map, chip, cmd_addr);

	spin_unlock(chip->mutex);
	return 0;
}

static int cfi_intelext_read (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long ofs;
	int chipnum;
	int ret = 0;

	/* ofs: offset within the first chip that the first read should start */
	chipnum = (from >> cfi->chipshift);
	ofs = from - (chipnum <<  cfi->chipshift);

	*retlen = 0;

	while (len) {
		unsigned long thislen;

		if (chipnum >= cfi->numchips)
			break;

		if ((len + ofs -1) >> cfi->chipshift)
			thislen = (1<<cfi->chipshift) - ofs;
		else
			thislen = len;

		ret = do_read_onechip(map, &cfi->chips[chipnum], ofs, thislen, buf);
		if (ret)
			break;

		*retlen += thislen;
		len -= thislen;
		buf += thislen;
		
		ofs = 0;
		chipnum++;
	}
	return ret;
}
#if 0
static int cfi_intelext_read_prot_reg (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf, int base_offst, int reg_sz)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_intelext *extp = cfi->cmdset_priv;
	struct flchip *chip;
	int ofs_factor = cfi->interleave * cfi->device_type;
	int count = len;
	int chip_num, offst;
	int ret;

	chip_num = ((unsigned int)from/reg_sz);
	offst = from - (reg_sz*chip_num)+base_offst;

	while (count) {
	/* Calculate which chip & protection register offset we need */

		if (chip_num >= cfi->numchips)
			goto out;

		chip = &cfi->chips[chip_num];
		
		spin_lock(chip->mutex);
		ret = get_chip(map, chip, chip->start, FL_JEDEC_QUERY);
		if (ret) {
			spin_unlock(chip->mutex);
			return (len-count)?:ret;
		}

		if (chip->state != FL_JEDEC_QUERY) {
			map_write(map, CMD(0x90), chip->start);
			chip->state = FL_JEDEC_QUERY;
		}

		while (count && ((offst-base_offst) < reg_sz)) {
			*buf = map_read8(map,(chip->start+((extp->ProtRegAddr+1)*ofs_factor)+offst));
			buf++;
			offst++;
			count--;
		}

		put_chip(map, chip, chip->start);
		spin_unlock(chip->mutex);

		/* Move on to the next chip */
		chip_num++;
		offst = base_offst;
	}
	
 out:	
	return len-count;
}
	
static int cfi_intelext_read_user_prot_reg (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_intelext *extp=cfi->cmdset_priv;
	int base_offst,reg_sz;
	
	/* Check that we actually have some protection registers */
	if(!(extp->FeatureSupport&64)){
		printk(KERN_WARNING "%s: This flash device has no protection data to read!\n",map->name);
		return 0;
	}

	base_offst=(1<<extp->FactProtRegSize);
	reg_sz=(1<<extp->UserProtRegSize);

	return cfi_intelext_read_prot_reg(mtd, from, len, retlen, buf, base_offst, reg_sz);
}

static int cfi_intelext_read_fact_prot_reg (struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	struct cfi_pri_intelext *extp=cfi->cmdset_priv;
	int base_offst,reg_sz;
	
	/* Check that we actually have some protection registers */
	if(!(extp->FeatureSupport&64)){
		printk(KERN_WARNING "%s: This flash device has no protection data to read!\n",map->name);
		return 0;
	}

	base_offst=0;
	reg_sz=(1<<extp->FactProtRegSize);

	return cfi_intelext_read_prot_reg(mtd, from, len, retlen, buf, base_offst, reg_sz);
}
#endif

static int do_write_oneword(struct map_info *map, struct flchip *chip, unsigned long adr, map_word datum)
{
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK;
	unsigned long timeo;
	int z, ret=0;

	adr += chip->start;

	/* Let's determine this according to the interleave only once */
	status_OK = CMD(0x80);

	spin_lock(chip->mutex);
	ret = get_chip(map, chip, adr, FL_WRITING);
	if (ret) {
		spin_unlock(chip->mutex);
		return ret;
	}

	ENABLE_VPP(map);
	map_write(map, CMD(0x40), adr);
	map_write(map, datum, adr);
	chip->state = FL_WRITING;

	spin_unlock(chip->mutex);
	INVALIDATE_CACHED_RANGE(map, adr, map_bankwidth(map));
	cfi_udelay(chip->word_write_time);
	spin_lock(chip->mutex);

	timeo = jiffies + (HZ/2);
	z = 0;
	for (;;) {
		if (chip->state != FL_WRITING) {
			/* Someone's suspended the write. Sleep */
			DECLARE_WAITQUEUE(wait, current);

			set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&chip->wq, &wait);
			spin_unlock(chip->mutex);
			schedule();
			remove_wait_queue(&chip->wq, &wait);
			timeo = jiffies + (HZ / 2); /* FIXME */
			spin_lock(chip->mutex);
			continue;
		}

		status = map_read(map, adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;
		
		/* OK Still waiting */
		if (time_after(jiffies, timeo)) {
			chip->state = FL_STATUS;
			printk(KERN_ERR "waiting for chip to be ready timed out in word write\n");
			ret = -EIO;
			goto out;
		}

		/* Latency issues. Drop the lock, wait a while and retry */
		spin_unlock(chip->mutex);
		z++;
		cfi_udelay(1);
		spin_lock(chip->mutex);
	}
	if (!z) {
		chip->word_write_time--;
		if (!chip->word_write_time)
			chip->word_write_time++;
	}
	if (z > 1) 
		chip->word_write_time++;

	/* Done and happy. */
	chip->state = FL_STATUS;
	/* check for lock bit */
	if (map_word_bitsset(map, status, CMD(0x02))) {
		/* clear status */
		map_write(map, CMD(0x50), adr);
		/* put back into read status register mode */
		map_write(map, CMD(0x70), adr);
		ret = -EROFS;
	}
 out:
	put_chip(map, chip, adr);
	spin_unlock(chip->mutex);

	return ret;
}


static int cfi_intelext_write_words (struct mtd_info *mtd, loff_t to , size_t len, size_t *retlen, const u_char *buf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	int ret = 0;
	int chipnum;
	unsigned long ofs;

	*retlen = 0;
	if (!len)
		return 0;

	chipnum = to >> cfi->chipshift;
	ofs = to  - (chipnum << cfi->chipshift);

	/* If it's not bus-aligned, do the first byte write */
	if (ofs & (map_bankwidth(map)-1)) {
		unsigned long bus_ofs = ofs & ~(map_bankwidth(map)-1);
		int gap = ofs - bus_ofs;
		int n;
		map_word datum;

		n = min_t(int, len, map_bankwidth(map)-gap);
		datum = map_word_ff(map);
		datum = map_word_load_partial(map, datum, buf, gap, n);

		ret = do_write_oneword(map, &cfi->chips[chipnum],
					       bus_ofs, datum);
		if (ret) 
			return ret;

		len -= n;
		ofs += n;
		buf += n;
		(*retlen) += n;

		if (ofs >> cfi->chipshift) {
			chipnum ++; 
			ofs = 0;
			if (chipnum == cfi->numchips)
				return 0;
		}
	}
	
	while(len >= map_bankwidth(map)) {
		map_word datum = map_word_load(map, buf);

		ret = do_write_oneword(map, &cfi->chips[chipnum],
				ofs, datum);
		if (ret)
			return ret;

		ofs += map_bankwidth(map);
		buf += map_bankwidth(map);
		(*retlen) += map_bankwidth(map);
		len -= map_bankwidth(map);

		if (ofs >> cfi->chipshift) {
			chipnum ++; 
			ofs = 0;
			if (chipnum == cfi->numchips)
				return 0;
		}
	}

	if (len & (map_bankwidth(map)-1)) {
		map_word datum;

		datum = map_word_ff(map);
		datum = map_word_load_partial(map, datum, buf, 0, len);

		ret = do_write_oneword(map, &cfi->chips[chipnum],
					       ofs, datum);
		if (ret) 
			return ret;
		
		(*retlen) += len;
	}

	return 0;
}


static inline int do_write_buffer(struct map_info *map, struct flchip *chip, 
				  unsigned long adr, const u_char *buf, int len)
{
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK;
	unsigned long cmd_adr, timeo;
	int wbufsize, z, ret=0, bytes, words;

	wbufsize = cfi_interleave(cfi) << cfi->cfiq->MaxBufWriteSize;
	adr += chip->start;
	cmd_adr = adr & ~(wbufsize-1);
	
	/* Let's determine this according to the interleave only once */
	status_OK = CMD(0x80);

	spin_lock(chip->mutex);
	ret = get_chip(map, chip, cmd_adr, FL_WRITING);
	if (ret) {
		spin_unlock(chip->mutex);
		return ret;
	}

	/* �4.8 of the 28FxxxJ3A datasheet says "Any time SR.4 and/or SR.5 is set
	   [...], the device will not accept any more Write to Buffer commands". 
	   So we must check here and reset those bits if they're set. Otherwise
	   we're just pissing in the wind */
	if (chip->state != FL_STATUS)
		map_write(map, CMD(0x70), cmd_adr);
	status = map_read(map, cmd_adr);
	if (map_word_bitsset(map, status, CMD(0x30))) {
		printk(KERN_WARNING "SR.4 or SR.5 bits set in buffer write (status %lx). Clearing.\n", status.x[0]);
		map_write(map, CMD(0x50), cmd_adr);
		map_write(map, CMD(0x70), cmd_adr);
	}

	ENABLE_VPP(map);
	chip->state = FL_WRITING_TO_BUFFER;

	z = 0;
	for (;;) {
		map_write(map, CMD(0xe8), cmd_adr);

		status = map_read(map, cmd_adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;

		spin_unlock(chip->mutex);
		cfi_udelay(1);
		spin_lock(chip->mutex);

		if (++z > 20) {
			/* Argh. Not ready for write to buffer */
			map_write(map, CMD(0x70), cmd_adr);
			chip->state = FL_STATUS;
			printk(KERN_ERR "Chip not ready for buffer write. Xstatus = %lx, status = %lx\n",
			       status.x[0], map_read(map, cmd_adr).x[0]);
			/* Odd. Clear status bits */
			map_write(map, CMD(0x50), cmd_adr);
			map_write(map, CMD(0x70), cmd_adr);
			ret = -EIO;
			goto out;
		}
	}

	/* Write length of data to come */
	bytes = len & (map_bankwidth(map)-1);
	words = len / map_bankwidth(map);
	map_write(map, CMD(words - !bytes), cmd_adr );

	/* Write data */
	z = 0;
	while(z < words * map_bankwidth(map)) {
		map_word datum = map_word_load(map, buf);
		map_write(map, datum, adr+z);

		z += map_bankwidth(map);
		buf += map_bankwidth(map);
	}

	if (bytes) {
		map_word datum;

		datum = map_word_ff(map);
		datum = map_word_load_partial(map, datum, buf, 0, bytes);
		map_write(map, datum, adr+z);
	}

	/* GO GO GO */
	map_write(map, CMD(0xd0), cmd_adr);
	chip->state = FL_WRITING;

	spin_unlock(chip->mutex);
	INVALIDATE_CACHED_RANGE(map, adr, len);
	cfi_udelay(chip->buffer_write_time);
	spin_lock(chip->mutex);

	timeo = jiffies + (HZ/2);
	z = 0;
	for (;;) {
		if (chip->state != FL_WRITING) {
			/* Someone's suspended the write. Sleep */
			DECLARE_WAITQUEUE(wait, current);
			set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&chip->wq, &wait);
			spin_unlock(chip->mutex);
			schedule();
			remove_wait_queue(&chip->wq, &wait);
			timeo = jiffies + (HZ / 2); /* FIXME */
			spin_lock(chip->mutex);
			continue;
		}

		status = map_read(map, cmd_adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;

		/* OK Still waiting */
		if (time_after(jiffies, timeo)) {
			chip->state = FL_STATUS;
			printk(KERN_ERR "waiting for chip to be ready timed out in bufwrite\n");
			ret = -EIO;
			goto out;
		}
		
		/* Latency issues. Drop the lock, wait a while and retry */
		spin_unlock(chip->mutex);
		cfi_udelay(1);
		z++;
		spin_lock(chip->mutex);
	}
	if (!z) {
		chip->buffer_write_time--;
		if (!chip->buffer_write_time)
			chip->buffer_write_time++;
	}
	if (z > 1) 
		chip->buffer_write_time++;

	/* Done and happy. */
 	chip->state = FL_STATUS;

	/* check for lock bit */
	if (map_word_bitsset(map, status, CMD(0x02))) {
		/* clear status */
		map_write(map, CMD(0x50), cmd_adr);
		/* put back into read status register mode */
		map_write(map, CMD(0x70), adr);
		ret = -EROFS;
	}

 out:
	put_chip(map, chip, cmd_adr);
	spin_unlock(chip->mutex);
	return ret;
}

static int cfi_intelext_write_buffers (struct mtd_info *mtd, loff_t to, 
				       size_t len, size_t *retlen, const u_char *buf)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	int wbufsize = cfi_interleave(cfi) << cfi->cfiq->MaxBufWriteSize;
	int ret = 0;
	int chipnum;
	unsigned long ofs;

	*retlen = 0;
	if (!len)
		return 0;

	chipnum = to >> cfi->chipshift;
	ofs = to  - (chipnum << cfi->chipshift);

	/* If it's not bus-aligned, do the first word write */
	if (ofs & (map_bankwidth(map)-1)) {
		size_t local_len = (-ofs)&(map_bankwidth(map)-1);
		if (local_len > len)
			local_len = len;
		ret = cfi_intelext_write_words(mtd, to, local_len,
					       retlen, buf);
		if (ret)
			return ret;
		ofs += local_len;
		buf += local_len;
		len -= local_len;

		if (ofs >> cfi->chipshift) {
			chipnum ++;
			ofs = 0;
			if (chipnum == cfi->numchips)
				return 0;
		}
	}

	while(len) {
		/* We must not cross write block boundaries */
		int size = wbufsize - (ofs & (wbufsize-1));

		if (size > len)
			size = len;
		ret = do_write_buffer(map, &cfi->chips[chipnum], 
				      ofs, buf, size);
		if (ret)
			return ret;

		ofs += size;
		buf += size;
		(*retlen) += size;
		len -= size;

		if (ofs >> cfi->chipshift) {
			chipnum ++; 
			ofs = 0;
			if (chipnum == cfi->numchips)
				return 0;
		}
	}
	return 0;
}

typedef int (*varsize_frob_t)(struct map_info *map, struct flchip *chip,
			      unsigned long adr, int len, void *thunk);

static int cfi_intelext_varsize_frob(struct mtd_info *mtd, varsize_frob_t frob,
				     loff_t ofs, size_t len, void *thunk)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	unsigned long adr;
	int chipnum, ret = 0;
	int i, first;
	struct mtd_erase_region_info *regions = mtd->eraseregions;

	if (ofs > mtd->size)
		return -EINVAL;

	if ((len + ofs) > mtd->size)
		return -EINVAL;

	/* Check that both start and end of the requested erase are
	 * aligned with the erasesize at the appropriate addresses.
	 */

	i = 0;

	/* Skip all erase regions which are ended before the start of 
	   the requested erase. Actually, to save on the calculations,
	   we skip to the first erase region which starts after the
	   start of the requested erase, and then go back one.
	*/
	
	while (i < mtd->numeraseregions && ofs >= regions[i].offset)
	       i++;
	i--;

	/* OK, now i is pointing at the erase region in which this 
	   erase request starts. Check the start of the requested
	   erase range is aligned with the erase size which is in
	   effect here.
	*/

	if (ofs & (regions[i].erasesize-1))
		return -EINVAL;

	/* Remember the erase region we start on */
	first = i;

	/* Next, check that the end of the requested erase is aligned
	 * with the erase region at that address.
	 */

	while (i<mtd->numeraseregions && (ofs + len) >= regions[i].offset)
		i++;

	/* As before, drop back one to point at the region in which
	   the address actually falls
	*/
	i--;
	
	if ((ofs + len) & (regions[i].erasesize-1))
		return -EINVAL;

	chipnum = ofs >> cfi->chipshift;
	adr = ofs - (chipnum << cfi->chipshift);

	i=first;

	while(len) {
		unsigned long chipmask;
		int size = regions[i].erasesize;

		ret = (*frob)(map, &cfi->chips[chipnum], adr, size, thunk);
		
		if (ret)
			return ret;

		adr += size;
		len -= size;

		chipmask = (1 << cfi->chipshift) - 1;
		if ((adr & chipmask) == ((regions[i].offset + size * regions[i].numblocks) & chipmask))
			i++;

		if (adr >> cfi->chipshift) {
			adr = 0;
			chipnum++;
			
			if (chipnum >= cfi->numchips)
			break;
		}
	}

	return 0;
}


static int do_erase_oneblock(struct map_info *map, struct flchip *chip,
			     unsigned long adr, int len, void *thunk)
{
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK;
	unsigned long timeo;
	int retries = 3;
	DECLARE_WAITQUEUE(wait, current);
	int ret = 0;

	adr += chip->start;

	/* Let's determine this according to the interleave only once */
	status_OK = CMD(0x80);

 retry:
	spin_lock(chip->mutex);
	ret = get_chip(map, chip, adr, FL_ERASING);
	if (ret) {
		spin_unlock(chip->mutex);
		return ret;
	}

	ENABLE_VPP(map);
	/* Clear the status register first */
	map_write(map, CMD(0x50), adr);

	/* Now erase */
	map_write(map, CMD(0x20), adr);
	map_write(map, CMD(0xD0), adr);
	chip->state = FL_ERASING;
	chip->erase_suspended = 0;

	spin_unlock(chip->mutex);
	INVALIDATE_CACHED_RANGE(map, adr, len);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((chip->erase_time*HZ)/(2*1000));
	spin_lock(chip->mutex);

	/* FIXME. Use a timer to check this, and return immediately. */
	/* Once the state machine's known to be working I'll do that */

	timeo = jiffies + (HZ*20);
	for (;;) {
		if (chip->state != FL_ERASING) {
			/* Someone's suspended the erase. Sleep */
			set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&chip->wq, &wait);
			spin_unlock(chip->mutex);
			schedule();
			remove_wait_queue(&chip->wq, &wait);
			spin_lock(chip->mutex);
			continue;
		}
		if (chip->erase_suspended) {
			/* This erase was suspended and resumed.
			   Adjust the timeout */
			timeo = jiffies + (HZ*20); /* FIXME */
			chip->erase_suspended = 0;
		}

		status = map_read(map, adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;
		
		/* OK Still waiting */
		if (time_after(jiffies, timeo)) {
			map_write(map, CMD(0x70), adr);
			chip->state = FL_STATUS;
			printk(KERN_ERR "waiting for erase at %08lx to complete timed out. Xstatus = %lx, status = %lx.\n",
			       adr, status.x[0], map_read(map, adr).x[0]);
			/* Clear status bits */
			map_write(map, CMD(0x50), adr);
			map_write(map, CMD(0x70), adr);
			DISABLE_VPP(map);
			spin_unlock(chip->mutex);
			return -EIO;
		}
		
		/* Latency issues. Drop the lock, wait a while and retry */
		spin_unlock(chip->mutex);
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
		spin_lock(chip->mutex);
	}
	
	DISABLE_VPP(map);
	ret = 0;

	/* We've broken this before. It doesn't hurt to be safe */
	map_write(map, CMD(0x70), adr);
	chip->state = FL_STATUS;
	status = map_read(map, adr);

	/* check for lock bit */
	if (map_word_bitsset(map, status, CMD(0x3a))) {
		unsigned char chipstatus = status.x[0];
		if (!map_word_equal(map, status, CMD(chipstatus))) {
			int i, w;
			for (w=0; w<map_words(map); w++) {
				for (i = 0; i<cfi_interleave(cfi); i++) {
					chipstatus |= status.x[w] >> (cfi->device_type * 8);
				}
			}
			printk(KERN_WARNING "Status is not identical for all chips: 0x%lx. Merging to give 0x%02x\n",
			       status.x[0], chipstatus);
		}
		/* Reset the error bits */
		map_write(map, CMD(0x50), adr);
		map_write(map, CMD(0x70), adr);
		
		if ((chipstatus & 0x30) == 0x30) {
			printk(KERN_NOTICE "Chip reports improper command sequence: status 0x%x\n", chipstatus);
			ret = -EIO;
		} else if (chipstatus & 0x02) {
			/* Protection bit set */
			ret = -EROFS;
		} else if (chipstatus & 0x8) {
			/* Voltage */
			printk(KERN_WARNING "Chip reports voltage low on erase: status 0x%x\n", chipstatus);
			ret = -EIO;
		} else if (chipstatus & 0x20) {
			if (retries--) {
				printk(KERN_DEBUG "Chip erase failed at 0x%08lx: status 0x%x. Retrying...\n", adr, chipstatus);
				timeo = jiffies + HZ;
				chip->state = FL_STATUS;
				spin_unlock(chip->mutex);
				goto retry;
			}
			printk(KERN_DEBUG "Chip erase failed at 0x%08lx: status 0x%x\n", adr, chipstatus);
			ret = -EIO;
		}
	}

	wake_up(&chip->wq);
	spin_unlock(chip->mutex);
	return ret;
}

int cfi_intelext_erase_varsize(struct mtd_info *mtd, struct erase_info *instr)
{
	unsigned long ofs, len;
	int ret;

	ofs = instr->addr;
	len = instr->len;

	ret = cfi_intelext_varsize_frob(mtd, do_erase_oneblock, ofs, len, NULL);
	if (ret)
		return ret;

	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);
	
	return 0;
}

static void cfi_intelext_sync (struct mtd_info *mtd)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	int i;
	struct flchip *chip;
	int ret = 0;

	for (i=0; !ret && i<cfi->numchips; i++) {
		chip = &cfi->chips[i];

		spin_lock(chip->mutex);
		ret = get_chip(map, chip, chip->start, FL_SYNCING);

		if (!ret) {
			chip->oldstate = chip->state;
			chip->state = FL_SYNCING;
			/* No need to wake_up() on this state change - 
			 * as the whole point is that nobody can do anything
			 * with the chip now anyway.
			 */
		}
		spin_unlock(chip->mutex);
	}

	/* Unlock the chips again */

	for (i--; i >=0; i--) {
		chip = &cfi->chips[i];

		spin_lock(chip->mutex);
		
		if (chip->state == FL_SYNCING) {
			chip->state = chip->oldstate;
			wake_up(&chip->wq);
		}
		spin_unlock(chip->mutex);
	}
}

#ifdef DEBUG_LOCK_BITS
static int do_printlockstatus_oneblock(struct map_info *map, struct flchip *chip,
				       unsigned long adr, int len, void *thunk)
{
	struct cfi_private *cfi = map->fldrv_priv;
	int ofs_factor = cfi->interleave * cfi->device_type;

	cfi_send_gen_cmd(0x90, 0x55, 0, map, cfi, cfi->device_type, NULL);
	printk(KERN_DEBUG "block status register for 0x%08lx is %x\n",
	       adr, cfi_read_query(map, adr+(2*ofs_factor)));
	chip->state = FL_JEDEC_QUERY;
	return 0;
}
#endif

#define DO_XXLOCK_ONEBLOCK_LOCK		((void *) 1)
#define DO_XXLOCK_ONEBLOCK_UNLOCK	((void *) 2)

static int do_xxlock_oneblock(struct map_info *map, struct flchip *chip,
			      unsigned long adr, int len, void *thunk)
{
	struct cfi_private *cfi = map->fldrv_priv;
	map_word status, status_OK;
	unsigned long timeo = jiffies + HZ;
	int ret;

	adr += chip->start;

	/* Let's determine this according to the interleave only once */
	status_OK = CMD(0x80);

	spin_lock(chip->mutex);
	ret = get_chip(map, chip, adr, FL_LOCKING);
	if (ret) {
		spin_unlock(chip->mutex);
		return ret;
	}

	ENABLE_VPP(map);
	map_write(map, CMD(0x60), adr);

	if (thunk == DO_XXLOCK_ONEBLOCK_LOCK) {
		map_write(map, CMD(0x01), adr);
		chip->state = FL_LOCKING;
	} else if (thunk == DO_XXLOCK_ONEBLOCK_UNLOCK) {
		map_write(map, CMD(0xD0), adr);
		chip->state = FL_UNLOCKING;
	} else
		BUG();

	spin_unlock(chip->mutex);
	schedule_timeout(HZ);
	spin_lock(chip->mutex);

	/* FIXME. Use a timer to check this, and return immediately. */
	/* Once the state machine's known to be working I'll do that */

	timeo = jiffies + (HZ*20);
	for (;;) {

		status = map_read(map, adr);
		if (map_word_andequal(map, status, status_OK, status_OK))
			break;
		
		/* OK Still waiting */
		if (time_after(jiffies, timeo)) {
			map_write(map, CMD(0x70), adr);
			chip->state = FL_STATUS;
			printk(KERN_ERR "waiting for unlock to complete timed out. Xstatus = %lx, status = %lx.\n",
			       status.x[0], map_read(map, adr).x[0]);
			DISABLE_VPP(map);
			spin_unlock(chip->mutex);
			return -EIO;
		}
		
		/* Latency issues. Drop the lock, wait a while and retry */
		spin_unlock(chip->mutex);
		cfi_udelay(1);
		spin_lock(chip->mutex);
	}
	
	/* Done and happy. */
	chip->state = FL_STATUS;
	put_chip(map, chip, adr);
	spin_unlock(chip->mutex);
	return 0;
}

static int cfi_intelext_lock(struct mtd_info *mtd, loff_t ofs, size_t len)
{
	int ret;

#ifdef DEBUG_LOCK_BITS
	printk(KERN_DEBUG "%s: lock status before, ofs=0x%08llx, len=0x%08X\n",
	       __FUNCTION__, ofs, len);
	cfi_intelext_varsize_frob(mtd, do_printlockstatus_oneblock,
				  ofs, len, 0);
#endif

	ret = cfi_intelext_varsize_frob(mtd, do_xxlock_oneblock, 
					ofs, len, DO_XXLOCK_ONEBLOCK_LOCK);
	
#ifdef DEBUG_LOCK_BITS
	printk(KERN_DEBUG "%s: lock status after, ret=%d\n",
	       __FUNCTION__, ret);
	cfi_intelext_varsize_frob(mtd, do_printlockstatus_oneblock,
				  ofs, len, 0);
#endif

	return ret;
}

static int cfi_intelext_unlock(struct mtd_info *mtd, loff_t ofs, size_t len)
{
	int ret;

#ifdef DEBUG_LOCK_BITS
	printk(KERN_DEBUG "%s: lock status before, ofs=0x%08llx, len=0x%08X\n",
	       __FUNCTION__, ofs, len);
	cfi_intelext_varsize_frob(mtd, do_printlockstatus_oneblock,
				  ofs, len, 0);
#endif

	ret = cfi_intelext_varsize_frob(mtd, do_xxlock_oneblock,
					ofs, len, DO_XXLOCK_ONEBLOCK_UNLOCK);
	
#ifdef DEBUG_LOCK_BITS
	printk(KERN_DEBUG "%s: lock status after, ret=%d\n",
	       __FUNCTION__, ret);
	cfi_intelext_varsize_frob(mtd, do_printlockstatus_oneblock, 
				  ofs, len, 0);
#endif
	
	return ret;
}

static int cfi_intelext_suspend(struct mtd_info *mtd)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	int i;
	struct flchip *chip;
	int ret = 0;

	for (i=0; !ret && i<cfi->numchips; i++) {
		chip = &cfi->chips[i];

		spin_lock(chip->mutex);

		switch (chip->state) {
		case FL_READY:
		case FL_STATUS:
		case FL_CFI_QUERY:
		case FL_JEDEC_QUERY:
			if (chip->oldstate == FL_READY) {
				chip->oldstate = chip->state;
				chip->state = FL_PM_SUSPENDED;
				/* No need to wake_up() on this state change - 
				 * as the whole point is that nobody can do anything
				 * with the chip now anyway.
				 */
			}
			break;
		default:
			ret = -EAGAIN;
		case FL_PM_SUSPENDED:
			break;
		}
		spin_unlock(chip->mutex);
	}

	/* Unlock the chips again */

	if (ret) {
		for (i--; i >=0; i--) {
			chip = &cfi->chips[i];
			
			spin_lock(chip->mutex);
			
			if (chip->state == FL_PM_SUSPENDED) {
				/* No need to force it into a known state here,
				   because we're returning failure, and it didn't
				   get power cycled */
				chip->state = chip->oldstate;
				wake_up(&chip->wq);
			}
			spin_unlock(chip->mutex);
		}
	} 
	
	return ret;
}

static void cfi_intelext_resume(struct mtd_info *mtd)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	int i;
	struct flchip *chip;

	for (i=0; i<cfi->numchips; i++) {
	
		chip = &cfi->chips[i];

		spin_lock(chip->mutex);
		
		/* Go to known state. Chip may have been power cycled */
		if (chip->state == FL_PM_SUSPENDED) {
			map_write(map, CMD(0xFF), cfi->chips[i].start);
			chip->state = FL_READY;
			wake_up(&chip->wq);
		}

		spin_unlock(chip->mutex);
	}
}

static void cfi_intelext_destroy(struct mtd_info *mtd)
{
	struct map_info *map = mtd->priv;
	struct cfi_private *cfi = map->fldrv_priv;
	kfree(cfi->cmdset_priv);
	kfree(cfi->cfiq);
	kfree(cfi->chips[0].priv);
	kfree(cfi);
	kfree(mtd->eraseregions);
}

static char im_name_1[]="cfi_cmdset_0001";
static char im_name_3[]="cfi_cmdset_0003";

int __init cfi_intelext_init(void)
{
	inter_module_register(im_name_1, THIS_MODULE, &cfi_cmdset_0001);
	inter_module_register(im_name_3, THIS_MODULE, &cfi_cmdset_0001);
	return 0;
}

static void __exit cfi_intelext_exit(void)
{
	inter_module_unregister(im_name_1);
	inter_module_unregister(im_name_3);
}

module_init(cfi_intelext_init);
module_exit(cfi_intelext_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org> et al.");
MODULE_DESCRIPTION("MTD chip driver for Intel/Sharp flash chips");
