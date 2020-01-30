/* 
 * File...........: linux/drivers/s390/block/dasd_fba.c
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * (C) IBM Corporation, IBM Deutschland Entwicklung GmbH, 1999,2000
 *
 * $Revision: 1.34 $
 */

#include <linux/config.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <asm/debug.h>

#include <linux/slab.h>
#include <linux/hdreg.h>	/* HDIO_GETGEO			    */
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/init.h>

#include <asm/idals.h>
#include <asm/ebcdic.h>
#include <asm/io.h>
#include <asm/todclk.h>
#include <asm/ccwdev.h>

#include "dasd_int.h"
#include "dasd_fba.h"

#ifdef PRINTK_HEADER
#undef PRINTK_HEADER
#endif				/* PRINTK_HEADER */
#define PRINTK_HEADER "dasd(fba):"

#define DASD_FBA_CCW_WRITE 0x41
#define DASD_FBA_CCW_READ 0x42
#define DASD_FBA_CCW_LOCATE 0x43
#define DASD_FBA_CCW_DEFINE_EXTENT 0x63

MODULE_LICENSE("GPL");

static struct dasd_discipline dasd_fba_discipline;

struct dasd_fba_private {
	struct dasd_fba_characteristics rdc_data;
};

static struct ccw_device_id dasd_fba_ids[] = {
	{ CCW_DEVICE_DEVTYPE (0x6310, 0, 0x9336, 0), driver_info: 0x1},
	{ CCW_DEVICE_DEVTYPE (0x3880, 0, 0x3370, 0), driver_info: 0x2},
	{ /* end of list */ },
};

MODULE_DEVICE_TABLE(ccw, dasd_fba_ids);

static struct ccw_driver dasd_fba_driver; /* see below */
static int
dasd_fba_probe(struct ccw_device *cdev)
{
	int ret;

	ret = dasd_generic_probe (cdev, &dasd_fba_discipline);
	if (ret)
		return ret;
	ccw_device_set_options(cdev, CCWDEV_DO_PATHGROUP);
	return 0;
}

static int
dasd_fba_set_online(struct ccw_device *cdev)
{
	return dasd_generic_set_online (cdev, &dasd_fba_discipline);
}

static struct ccw_driver dasd_fba_driver = {
	.name        = "dasd-fba",
	.owner       = THIS_MODULE,
	.ids         = dasd_fba_ids,
	.probe       = dasd_fba_probe,
	.remove      = dasd_generic_remove,
	.set_offline = dasd_generic_set_offline,
	.set_online  = dasd_fba_set_online,
	.notify      = dasd_generic_notify,
};

static inline void
define_extent(struct ccw1 * ccw, struct DE_fba_data *data, int rw,
	      int blksize, int beg, int nr)
{
	ccw->cmd_code = DASD_FBA_CCW_DEFINE_EXTENT;
	ccw->flags = 0;
	ccw->count = 16;
	ccw->cda = (__u32) __pa(data);
	memset(data, 0, sizeof (struct DE_fba_data));
	if (rw == WRITE)
		(data->mask).perm = 0x0;
	else if (rw == READ)
		(data->mask).perm = 0x1;
	else
		data->mask.perm = 0x2;
	data->blk_size = blksize;
	data->ext_loc = beg;
	data->ext_end = nr - 1;
}

static inline void
locate_record(struct ccw1 * ccw, struct LO_fba_data *data, int rw,
	      int block_nr, int block_ct)
{
	ccw->cmd_code = DASD_FBA_CCW_LOCATE;
	ccw->flags = 0;
	ccw->count = 8;
	ccw->cda = (__u32) __pa(data);
	memset(data, 0, sizeof (struct LO_fba_data));
	if (rw == WRITE)
		data->operation.cmd = 0x5;
	else if (rw == READ)
		data->operation.cmd = 0x6;
	else
		data->operation.cmd = 0x8;
	data->blk_nr = block_nr;
	data->blk_ct = block_ct;
}

static int
dasd_fba_check_characteristics(struct dasd_device *device)
{
	struct dasd_fba_private *private;
	struct ccw_device *cdev = device->cdev;	
	void *rdc_data;
	int rc;

	private = (struct dasd_fba_private *) device->private;
	if (private == NULL) {
		private = kmalloc(sizeof(struct dasd_fba_private), GFP_KERNEL);
		if (private == NULL) {
			MESSAGE(KERN_WARNING, "%s",
				"memory allocation failed for private data");
			return -ENOMEM;
		}
		device->private = (void *) private;
	}
	/* Read Device Characteristics */
	rdc_data = (void *) &(private->rdc_data);
	rc = read_dev_chars(device->cdev, &rdc_data, 32);
	if (rc) {
		MESSAGE(KERN_WARNING,
			"Read device characteristics returned error %d", rc);
		return rc;
	}

	DEV_MESSAGE(KERN_INFO, device,
		    "%04X/%02X(CU:%04X/%02X) %dMB at(%d B/blk)",
		    cdev->id.dev_type,
		    cdev->id.dev_model,
		    cdev->id.cu_type,
		    cdev->id.cu_model,
		    ((private->rdc_data.blk_bdsa *
		      (private->rdc_data.blk_size >> 9)) >> 11),
		    private->rdc_data.blk_size);
	return 0;
}

static int
dasd_fba_do_analysis(struct dasd_device *device)
{
	struct dasd_fba_private *private;
	int sb, rc;

	private = (struct dasd_fba_private *) device->private;
	rc = dasd_check_blocksize(private->rdc_data.blk_size);
	if (rc) {
		DEV_MESSAGE(KERN_INFO, device, "unknown blocksize %d",
			    private->rdc_data.blk_size);
		return rc;
	}
	device->blocks = private->rdc_data.blk_bdsa;
	device->bp_block = private->rdc_data.blk_size;
	device->s2b_shift = 0;	/* bits to shift 512 to get a block */
	for (sb = 512; sb < private->rdc_data.blk_size; sb = sb << 1)
		device->s2b_shift++;
	return 0;
}

static int
dasd_fba_fill_geometry(struct dasd_device *device, struct hd_geometry *geo)
{
	if (dasd_check_blocksize(device->bp_block) != 0)
		return -EINVAL;
	geo->cylinders = (device->blocks << device->s2b_shift) >> 10;
	geo->heads = 16;
	geo->sectors = 128 >> device->s2b_shift;
	return 0;
}

static dasd_era_t
dasd_fba_examine_error(struct dasd_ccw_req * cqr, struct irb * irb)
{
	struct dasd_device *device;
	struct ccw_device *cdev;

	device = (struct dasd_device *) cqr->device;
	if (irb->scsw.cstat == 0x00 &&
	    irb->scsw.dstat == (DEV_STAT_CHN_END | DEV_STAT_DEV_END))
		return dasd_era_none;
	
	cdev = device->cdev;
	switch (cdev->id.dev_type) {
	case 0x3370:
		return dasd_3370_erp_examine(cqr, irb);
	case 0x9336:
		return dasd_9336_erp_examine(cqr, irb);
	default:
		return dasd_era_recover;
	}
}

static dasd_erp_fn_t
dasd_fba_erp_action(struct dasd_ccw_req * cqr)
{
	return dasd_default_erp_action;
}

static dasd_erp_fn_t
dasd_fba_erp_postaction(struct dasd_ccw_req * cqr)
{
	if (cqr->function == dasd_default_erp_action)
		return dasd_default_erp_postaction;

	MESSAGE(KERN_WARNING, "unknown ERP action %p", cqr->function);

	return NULL;
}

static struct dasd_ccw_req *
dasd_fba_build_cp(struct dasd_device * device, struct request *req)
{
	struct dasd_fba_private *private;
	unsigned long *idaws;
	struct LO_fba_data *LO_data;
	struct dasd_ccw_req *cqr;
	struct ccw1 *ccw;
	struct bio *bio;
	struct bio_vec *bv;
	char *dst;
	int count, cidaw, cplength, datasize;
	sector_t recid, first_rec, last_rec;
	unsigned int blksize, off;
	unsigned char cmd;
	int i;

	private = (struct dasd_fba_private *) device->private;
	if (rq_data_dir(req) == READ) {
		cmd = DASD_FBA_CCW_READ;
	} else if (rq_data_dir(req) == WRITE) {
		cmd = DASD_FBA_CCW_WRITE;
	} else
		return ERR_PTR(-EINVAL);
	blksize = device->bp_block;
	/* Calculate record id of first and last block. */
	first_rec = req->sector >> device->s2b_shift;
	last_rec = (req->sector + req->nr_sectors - 1) >> device->s2b_shift;
	/* Check struct bio and count the number of blocks for the request. */
	count = 0;
	cidaw = 0;
	rq_for_each_bio(bio, req) {
		bio_for_each_segment(bv, bio, i) {
			if (bv->bv_len & (blksize - 1))
				/* Fba can only do full blocks. */
				return ERR_PTR(-EINVAL);
			count += bv->bv_len >> (device->s2b_shift + 9);
#if defined(CONFIG_ARCH_S390X)
			if (idal_is_needed (page_address(bv->bv_page), bv->bv_len))
				cidaw += bv->bv_len / blksize;
#endif
		}
	}
	/* Paranoia. */
	if (count != last_rec - first_rec + 1)
		return ERR_PTR(-EINVAL);
	/* 1x define extent + 1x locate record + number of blocks */
	cplength = 2 + count;
	/* 1x define extent + 1x locate record */
	datasize = sizeof(struct DE_fba_data) + sizeof(struct LO_fba_data) +
		cidaw * sizeof(unsigned long);
	/*
	 * Find out number of additional locate record ccws if the device
	 * can't do data chaining.
	 */
	if (private->rdc_data.mode.bits.data_chain == 0) {
		cplength += count - 1;
		datasize += (count - 1)*sizeof(struct LO_fba_data);
	}
	/* Allocate the ccw request. */
	cqr = dasd_smalloc_request(dasd_fba_discipline.name,
				   cplength, datasize, device);
	if (IS_ERR(cqr))
		return cqr;
	ccw = cqr->cpaddr;
	/* First ccw is define extent. */
	define_extent(ccw++, cqr->data, rq_data_dir(req),
		      device->bp_block, req->sector, req->nr_sectors);
	/* Build locate_record + read/write ccws. */
	idaws = (unsigned long *) (cqr->data + sizeof(struct DE_fba_data));
	LO_data = (struct LO_fba_data *) (idaws + cidaw);
	/* Locate record for all blocks for smart devices. */
	if (private->rdc_data.mode.bits.data_chain != 0) {
		ccw[-1].flags |= CCW_FLAG_CC;
		locate_record(ccw++, LO_data++, rq_data_dir(req), 0, count);
	}
	recid = first_rec;
	rq_for_each_bio(bio, req) bio_for_each_segment(bv, bio, i) {
		dst = page_address(bv->bv_page) + bv->bv_offset;
		for (off = 0; off < bv->bv_len; off += blksize) {
			/* Locate record for stupid devices. */
			if (private->rdc_data.mode.bits.data_chain == 0) {
				ccw[-1].flags |= CCW_FLAG_CC;
				locate_record(ccw, LO_data++,
					      rq_data_dir(req),
					      recid - first_rec, 1);
				ccw->flags = CCW_FLAG_CC;
				ccw++;
			} else {
				if (recid > first_rec)
					ccw[-1].flags |= CCW_FLAG_DC;
				else
					ccw[-1].flags |= CCW_FLAG_CC;
			}
			ccw->cmd_code = cmd;
			ccw->count = device->bp_block;
			if (idal_is_needed(dst, blksize)) {
				ccw->cda = (__u32)(addr_t) idaws;
				ccw->flags = CCW_FLAG_IDA;
				idaws = idal_create_words(idaws, dst, blksize);
			} else {
				ccw->cda = (__u32)(addr_t) dst;
				ccw->flags = 0;
			}
			ccw++;
			dst += blksize;
			recid++;
		}
	}
	cqr->device = device;
	cqr->expires = 5 * 60 * HZ;	/* 5 minutes */
	cqr->status = DASD_CQR_FILLED;
	return cqr;
}

static int
dasd_fba_fill_info(struct dasd_device * device,
		   struct dasd_information2_t * info)
{
	info->label_block = 1;
	info->FBA_layout = 1;
	info->format = DASD_FORMAT_LDL;
	info->characteristics_size = sizeof(struct dasd_fba_characteristics);
	memcpy(info->characteristics,
	       &((struct dasd_fba_private *) device->private)->rdc_data,
	       sizeof (struct dasd_fba_characteristics));
	info->confdata_size = 0;
	return 0;
}

static void
dasd_fba_dump_sense(struct dasd_device *device, struct dasd_ccw_req * req,
		    struct irb *irb)
{
	char *page;

	page = (char *) get_zeroed_page(GFP_KERNEL);
	if (page == NULL) {
		MESSAGE(KERN_ERR, "%s", "No memory to dump sense data");
		return;
	}
	sprintf(page, KERN_WARNING PRINTK_HEADER
		"device %s: I/O status report:\n",
		device->cdev->dev.bus_id);

	MESSAGE(KERN_ERR, "Sense data:\n%s", page);

	free_page((unsigned long) page);
}

/*
 * max_blocks is dependent on the amount of storage that is available
 * in the static io buffer for each device. Currently each device has
 * 8192 bytes (=2 pages). For 64 bit one dasd_mchunkt_t structure has
 * 24 bytes, the struct dasd_ccw_req has 136 bytes and each block can use
 * up to 16 bytes (8 for the ccw and 8 for the idal pointer). In
 * addition we have one define extent ccw + 16 bytes of data and a 
 * locate record ccw for each block (stupid devices!) + 16 bytes of data.
 * That makes:
 * (8192 - 24 - 136 - 8 - 16) / 40 = 200.2 blocks at maximum.
 * We want to fit two into the available memory so that we can immediately
 * start the next request if one finishes off. That makes 100.1 blocks
 * for one request. Give a little safety and the result is 96.
 */
static struct dasd_discipline dasd_fba_discipline = {
	.owner = THIS_MODULE,
	.name = "FBA ",
	.ebcname = "FBA ",
	.max_blocks = 96,
	.check_device = dasd_fba_check_characteristics,
	.do_analysis = dasd_fba_do_analysis,
	.fill_geometry = dasd_fba_fill_geometry,
	.start_IO = dasd_start_IO,
	.term_IO = dasd_term_IO,
	.examine_error = dasd_fba_examine_error,
	.erp_action = dasd_fba_erp_action,
	.erp_postaction = dasd_fba_erp_postaction,
	.build_cp = dasd_fba_build_cp,
	.dump_sense = dasd_fba_dump_sense,
	.fill_info = dasd_fba_fill_info,
};

static int __init
dasd_fba_init(void)
{
	int ret;

	ASCEBC(dasd_fba_discipline.ebcname, 4);

	ret = ccw_driver_register(&dasd_fba_driver);
	if (ret)
		return ret;

	dasd_generic_auto_online(&dasd_fba_driver);
	return 0;
}

static void __exit
dasd_fba_cleanup(void)
{
	ccw_driver_unregister(&dasd_fba_driver);
}

module_init(dasd_fba_init);
module_exit(dasd_fba_cleanup);

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 4 
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -4
 * c-argdecl-indent: 4
 * c-label-offset: -4
 * c-continued-statement-offset: 4
 * c-continued-brace-offset: 0
 * indent-tabs-mode: 1
 * tab-width: 8
 * End:
 */
