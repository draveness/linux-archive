/*
 * File...........: linux/drivers/s390/block/dasd_eckd.c
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 *		    Horst Hummel <Horst.Hummel@de.ibm.com>
 *		    Carsten Otte <Cotte@de.ibm.com>
 *		    Martin Schwidefsky <schwidefsky@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * (C) IBM Corporation, IBM Deutschland Entwicklung GmbH, 1999,2000
 *
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/hdreg.h>	/* HDIO_GETGEO			    */
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/init.h>

#include <asm/debug.h>
#include <asm/idals.h>
#include <asm/ebcdic.h>
#include <asm/io.h>
#include <asm/todclk.h>
#include <asm/uaccess.h>
#include <asm/cio.h>
#include <asm/ccwdev.h>

#include "dasd_int.h"
#include "dasd_eckd.h"

#ifdef PRINTK_HEADER
#undef PRINTK_HEADER
#endif				/* PRINTK_HEADER */
#define PRINTK_HEADER "dasd(eckd):"

#define ECKD_C0(i) (i->home_bytes)
#define ECKD_F(i) (i->formula)
#define ECKD_F1(i) (ECKD_F(i)==0x01?(i->factors.f_0x01.f1):\
		    (i->factors.f_0x02.f1))
#define ECKD_F2(i) (ECKD_F(i)==0x01?(i->factors.f_0x01.f2):\
		    (i->factors.f_0x02.f2))
#define ECKD_F3(i) (ECKD_F(i)==0x01?(i->factors.f_0x01.f3):\
		    (i->factors.f_0x02.f3))
#define ECKD_F4(i) (ECKD_F(i)==0x02?(i->factors.f_0x02.f4):0)
#define ECKD_F5(i) (ECKD_F(i)==0x02?(i->factors.f_0x02.f5):0)
#define ECKD_F6(i) (i->factor6)
#define ECKD_F7(i) (i->factor7)
#define ECKD_F8(i) (i->factor8)

MODULE_LICENSE("GPL");

static struct dasd_discipline dasd_eckd_discipline;

struct dasd_eckd_private {
	struct dasd_eckd_characteristics rdc_data;
	struct dasd_eckd_confdata conf_data;
	struct dasd_eckd_path path_data;
	struct eckd_count count_area[5];
	int init_cqr_status;
	int uses_cdl;
	struct attrib_data_t attrib;	/* e.g. cache operations */
};

/* The ccw bus type uses this table to find devices that it sends to
 * dasd_eckd_probe */
static struct ccw_device_id dasd_eckd_ids[] = {
	{ CCW_DEVICE_DEVTYPE (0x3990, 0, 0x3390, 0), .driver_info = 0x1},
	{ CCW_DEVICE_DEVTYPE (0x2105, 0, 0x3390, 0), .driver_info = 0x2},
	{ CCW_DEVICE_DEVTYPE (0x3880, 0, 0x3390, 0), .driver_info = 0x3},
	{ CCW_DEVICE_DEVTYPE (0x3990, 0, 0x3380, 0), .driver_info = 0x4},
	{ CCW_DEVICE_DEVTYPE (0x2105, 0, 0x3380, 0), .driver_info = 0x5},
	{ CCW_DEVICE_DEVTYPE (0x9343, 0, 0x9345, 0), .driver_info = 0x6},
	{ CCW_DEVICE_DEVTYPE (0x2107, 0, 0x3390, 0), .driver_info = 0x7},
	{ CCW_DEVICE_DEVTYPE (0x2107, 0, 0x3380, 0), .driver_info = 0x8},
	{ CCW_DEVICE_DEVTYPE (0x1750, 0, 0x3390, 0), .driver_info = 0x9},
	{ CCW_DEVICE_DEVTYPE (0x1750, 0, 0x3380, 0), .driver_info = 0xa},
	{ /* end of list */ },
};

MODULE_DEVICE_TABLE(ccw, dasd_eckd_ids);

static struct ccw_driver dasd_eckd_driver; /* see below */

/* initial attempt at a probe function. this can be simplified once
 * the other detection code is gone */
static int
dasd_eckd_probe (struct ccw_device *cdev)
{
	int ret;

	/* set ECKD specific ccw-device options */
	ret = ccw_device_set_options(cdev, CCWDEV_ALLOW_FORCE);
	if (ret) {
		printk(KERN_WARNING
		       "dasd_eckd_probe: could not set ccw-device options "
		       "for %s\n", cdev->dev.bus_id);
		return ret;
	}
	ret = dasd_generic_probe(cdev, &dasd_eckd_discipline);
	return ret;
}

static int
dasd_eckd_set_online(struct ccw_device *cdev)
{
	return dasd_generic_set_online(cdev, &dasd_eckd_discipline);
}

static struct ccw_driver dasd_eckd_driver = {
	.name        = "dasd-eckd",
	.owner       = THIS_MODULE,
	.ids         = dasd_eckd_ids,
	.probe       = dasd_eckd_probe,
	.remove      = dasd_generic_remove,
	.set_offline = dasd_generic_set_offline,
	.set_online  = dasd_eckd_set_online,
	.notify      = dasd_generic_notify,
};

static const int sizes_trk0[] = { 28, 148, 84 };
#define LABEL_SIZE 140

static inline unsigned int
round_up_multiple(unsigned int no, unsigned int mult)
{
	int rem = no % mult;
	return (rem ? no - rem + mult : no);
}

static inline unsigned int
ceil_quot(unsigned int d1, unsigned int d2)
{
	return (d1 + (d2 - 1)) / d2;
}

static unsigned int
recs_per_track(struct dasd_eckd_characteristics * rdc,
	       unsigned int kl, unsigned int dl)
{
	int dn, kn;

	switch (rdc->dev_type) {
	case 0x3380:
		if (kl)
			return 1499 / (15 + 7 + ceil_quot(kl + 12, 32) +
				       ceil_quot(dl + 12, 32));
		else
			return 1499 / (15 + ceil_quot(dl + 12, 32));
	case 0x3390:
		dn = ceil_quot(dl + 6, 232) + 1;
		if (kl) {
			kn = ceil_quot(kl + 6, 232) + 1;
			return 1729 / (10 + 9 + ceil_quot(kl + 6 * kn, 34) +
				       9 + ceil_quot(dl + 6 * dn, 34));
		} else
			return 1729 / (10 + 9 + ceil_quot(dl + 6 * dn, 34));
	case 0x9345:
		dn = ceil_quot(dl + 6, 232) + 1;
		if (kl) {
			kn = ceil_quot(kl + 6, 232) + 1;
			return 1420 / (18 + 7 + ceil_quot(kl + 6 * kn, 34) +
				       ceil_quot(dl + 6 * dn, 34));
		} else
			return 1420 / (18 + 7 + ceil_quot(dl + 6 * dn, 34));
	}
	return 0;
}

static int
check_XRC (struct ccw1         *de_ccw,
           struct DE_eckd_data *data,
           struct dasd_device  *device)
{
        struct dasd_eckd_private *private;
	int rc;

        private = (struct dasd_eckd_private *) device->private;
	if (!private->rdc_data.facilities.XRC_supported)
		return 0;

        /* switch on System Time Stamp - needed for XRC Support */
	data->ga_extended |= 0x08; /* switch on 'Time Stamp Valid'   */
	data->ga_extended |= 0x02; /* switch on 'Extended Parameter' */

	rc = get_sync_clock(&data->ep_sys_time);
	/* Ignore return code if sync clock is switched off. */
	if (rc == -ENOSYS || rc == -EACCES)
		rc = 0;

	de_ccw->count = sizeof (struct DE_eckd_data);
	de_ccw->flags |= CCW_FLAG_SLI;
	return rc;
}

static int
define_extent(struct ccw1 * ccw, struct DE_eckd_data * data, int trk,
	      int totrk, int cmd, struct dasd_device * device)
{
	struct dasd_eckd_private *private;
	struct ch_t geo, beg, end;
	int rc = 0;

	private = (struct dasd_eckd_private *) device->private;

	ccw->cmd_code = DASD_ECKD_CCW_DEFINE_EXTENT;
	ccw->flags = 0;
	ccw->count = 16;
	ccw->cda = (__u32) __pa(data);

	memset(data, 0, sizeof (struct DE_eckd_data));
	switch (cmd) {
	case DASD_ECKD_CCW_READ_HOME_ADDRESS:
	case DASD_ECKD_CCW_READ_RECORD_ZERO:
	case DASD_ECKD_CCW_READ:
	case DASD_ECKD_CCW_READ_MT:
	case DASD_ECKD_CCW_READ_CKD:
	case DASD_ECKD_CCW_READ_CKD_MT:
	case DASD_ECKD_CCW_READ_KD:
	case DASD_ECKD_CCW_READ_KD_MT:
	case DASD_ECKD_CCW_READ_COUNT:
		data->mask.perm = 0x1;
		data->attributes.operation = private->attrib.operation;
		break;
	case DASD_ECKD_CCW_WRITE:
	case DASD_ECKD_CCW_WRITE_MT:
	case DASD_ECKD_CCW_WRITE_KD:
	case DASD_ECKD_CCW_WRITE_KD_MT:
		data->mask.perm = 0x02;
		data->attributes.operation = private->attrib.operation;
		rc = check_XRC (ccw, data, device);
		break;
	case DASD_ECKD_CCW_WRITE_CKD:
	case DASD_ECKD_CCW_WRITE_CKD_MT:
		data->attributes.operation = DASD_BYPASS_CACHE;
		rc = check_XRC (ccw, data, device);
		break;
	case DASD_ECKD_CCW_ERASE:
	case DASD_ECKD_CCW_WRITE_HOME_ADDRESS:
	case DASD_ECKD_CCW_WRITE_RECORD_ZERO:
		data->mask.perm = 0x3;
		data->mask.auth = 0x1;
		data->attributes.operation = DASD_BYPASS_CACHE;
		rc = check_XRC (ccw, data, device);
		break;
	default:
		DEV_MESSAGE(KERN_ERR, device, "unknown opcode 0x%x", cmd);
		break;
	}

	data->attributes.mode = 0x3;	/* ECKD */

	if ((private->rdc_data.cu_type == 0x2105 ||
	     private->rdc_data.cu_type == 0x2107 ||
	     private->rdc_data.cu_type == 0x1750)
	    && !(private->uses_cdl && trk < 2))
		data->ga_extended |= 0x40; /* Regular Data Format Mode */

	geo.cyl = private->rdc_data.no_cyl;
	geo.head = private->rdc_data.trk_per_cyl;
	beg.cyl = trk / geo.head;
	beg.head = trk % geo.head;
	end.cyl = totrk / geo.head;
	end.head = totrk % geo.head;

	/* check for sequential prestage - enhance cylinder range */
	if (data->attributes.operation == DASD_SEQ_PRESTAGE ||
	    data->attributes.operation == DASD_SEQ_ACCESS) {

		if (end.cyl + private->attrib.nr_cyl < geo.cyl)
			end.cyl += private->attrib.nr_cyl;
		else
			end.cyl = (geo.cyl - 1);
	}

	data->beg_ext.cyl = beg.cyl;
	data->beg_ext.head = beg.head;
	data->end_ext.cyl = end.cyl;
	data->end_ext.head = end.head;
	return rc;
}

static void
locate_record(struct ccw1 *ccw, struct LO_eckd_data *data, int trk,
	      int rec_on_trk, int no_rec, int cmd,
	      struct dasd_device * device, int reclen)
{
	struct dasd_eckd_private *private;
	int sector;
	int dn, d;

	private = (struct dasd_eckd_private *) device->private;

	DBF_DEV_EVENT(DBF_INFO, device,
		  "Locate: trk %d, rec %d, no_rec %d, cmd %d, reclen %d",
		  trk, rec_on_trk, no_rec, cmd, reclen);

	ccw->cmd_code = DASD_ECKD_CCW_LOCATE_RECORD;
	ccw->flags = 0;
	ccw->count = 16;
	ccw->cda = (__u32) __pa(data);

	memset(data, 0, sizeof (struct LO_eckd_data));
	sector = 0;
	if (rec_on_trk) {
		switch (private->rdc_data.dev_type) {
		case 0x3390:
			dn = ceil_quot(reclen + 6, 232);
			d = 9 + ceil_quot(reclen + 6 * (dn + 1), 34);
			sector = (49 + (rec_on_trk - 1) * (10 + d)) / 8;
			break;
		case 0x3380:
			d = 7 + ceil_quot(reclen + 12, 32);
			sector = (39 + (rec_on_trk - 1) * (8 + d)) / 7;
			break;
		}
	}
	data->sector = sector;
	data->count = no_rec;
	switch (cmd) {
	case DASD_ECKD_CCW_WRITE_HOME_ADDRESS:
		data->operation.orientation = 0x3;
		data->operation.operation = 0x03;
		break;
	case DASD_ECKD_CCW_READ_HOME_ADDRESS:
		data->operation.orientation = 0x3;
		data->operation.operation = 0x16;
		break;
	case DASD_ECKD_CCW_WRITE_RECORD_ZERO:
		data->operation.orientation = 0x1;
		data->operation.operation = 0x03;
		data->count++;
		break;
	case DASD_ECKD_CCW_READ_RECORD_ZERO:
		data->operation.orientation = 0x3;
		data->operation.operation = 0x16;
		data->count++;
		break;
	case DASD_ECKD_CCW_WRITE:
	case DASD_ECKD_CCW_WRITE_MT:
	case DASD_ECKD_CCW_WRITE_KD:
	case DASD_ECKD_CCW_WRITE_KD_MT:
		data->auxiliary.last_bytes_used = 0x1;
		data->length = reclen;
		data->operation.operation = 0x01;
		break;
	case DASD_ECKD_CCW_WRITE_CKD:
	case DASD_ECKD_CCW_WRITE_CKD_MT:
		data->auxiliary.last_bytes_used = 0x1;
		data->length = reclen;
		data->operation.operation = 0x03;
		break;
	case DASD_ECKD_CCW_READ:
	case DASD_ECKD_CCW_READ_MT:
	case DASD_ECKD_CCW_READ_KD:
	case DASD_ECKD_CCW_READ_KD_MT:
		data->auxiliary.last_bytes_used = 0x1;
		data->length = reclen;
		data->operation.operation = 0x06;
		break;
	case DASD_ECKD_CCW_READ_CKD:
	case DASD_ECKD_CCW_READ_CKD_MT:
		data->auxiliary.last_bytes_used = 0x1;
		data->length = reclen;
		data->operation.operation = 0x16;
		break;
	case DASD_ECKD_CCW_READ_COUNT:
		data->operation.operation = 0x06;
		break;
	case DASD_ECKD_CCW_ERASE:
		data->length = reclen;
		data->auxiliary.last_bytes_used = 0x1;
		data->operation.operation = 0x0b;
		break;
	default:
		DEV_MESSAGE(KERN_ERR, device, "unknown opcode 0x%x", cmd);
	}
	data->seek_addr.cyl = data->search_arg.cyl =
		trk / private->rdc_data.trk_per_cyl;
	data->seek_addr.head = data->search_arg.head =
		trk % private->rdc_data.trk_per_cyl;
	data->search_arg.record = rec_on_trk;
}

/*
 * Returns 1 if the block is one of the special blocks that needs
 * to get read/written with the KD variant of the command.
 * That is DASD_ECKD_READ_KD_MT instead of DASD_ECKD_READ_MT and
 * DASD_ECKD_WRITE_KD_MT instead of DASD_ECKD_WRITE_MT.
 * Luckily the KD variants differ only by one bit (0x08) from the
 * normal variant. So don't wonder about code like:
 * if (dasd_eckd_cdl_special(blk_per_trk, recid))
 *         ccw->cmd_code |= 0x8;
 */
static inline int
dasd_eckd_cdl_special(int blk_per_trk, int recid)
{
	if (recid < 3)
		return 1;
	if (recid < blk_per_trk)
		return 0;
	if (recid < 2 * blk_per_trk)
		return 1;
	return 0;
}

/*
 * Returns the record size for the special blocks of the cdl format.
 * Only returns something useful if dasd_eckd_cdl_special is true
 * for the recid.
 */
static inline int
dasd_eckd_cdl_reclen(int recid)
{
	if (recid < 3)
		return sizes_trk0[recid];
	return LABEL_SIZE;
}

/*
 * Generate device unique id that specifies the physical device.
 */
static int
dasd_eckd_generate_uid(struct dasd_device *device, struct dasd_uid *uid)
{
	struct dasd_eckd_private *private;
	struct dasd_eckd_confdata *confdata;

	private = (struct dasd_eckd_private *) device->private;
	if (!private)
		return -ENODEV;
	confdata = &private->conf_data;
	if (!confdata)
		return -ENODEV;

	memset(uid, 0, sizeof(struct dasd_uid));
	memcpy(uid->vendor, confdata->ned1.HDA_manufacturer,
	       sizeof(uid->vendor) - 1);
	EBCASC(uid->vendor, sizeof(uid->vendor) - 1);
	memcpy(uid->serial, confdata->ned1.HDA_location,
	       sizeof(uid->serial) - 1);
	EBCASC(uid->serial, sizeof(uid->serial) - 1);
	uid->ssid = confdata->neq.subsystemID;
	if (confdata->ned2.sneq.flags == 0x40) {
		uid->alias = 1;
		uid->unit_addr = confdata->ned2.sneq.base_unit_addr;
	} else
		uid->unit_addr = confdata->ned1.unit_addr;

	return 0;
}

static struct dasd_ccw_req *dasd_eckd_build_rcd_lpm(struct dasd_device *device,
						    void *rcd_buffer,
						    struct ciw *ciw, __u8 lpm)
{
	struct dasd_ccw_req *cqr;
	struct ccw1 *ccw;

	cqr = dasd_smalloc_request("ECKD", 1 /* RCD */, ciw->count, device);

	if (IS_ERR(cqr)) {
		DEV_MESSAGE(KERN_WARNING, device, "%s",
			    "Could not allocate RCD request");
		return cqr;
	}

	ccw = cqr->cpaddr;
	ccw->cmd_code = ciw->cmd;
	ccw->cda = (__u32)(addr_t)rcd_buffer;
	ccw->count = ciw->count;

	cqr->device = device;
	cqr->expires = 10*HZ;
	cqr->lpm = lpm;
	clear_bit(DASD_CQR_FLAGS_USE_ERP, &cqr->flags);
	cqr->retries = 2;
	cqr->buildclk = get_clock();
	cqr->status = DASD_CQR_FILLED;
	return cqr;
}

static int dasd_eckd_read_conf_lpm(struct dasd_device *device,
				   void **rcd_buffer,
				   int *rcd_buffer_size, __u8 lpm)
{
	struct ciw *ciw;
	char *rcd_buf = NULL;
	int ret;
	struct dasd_ccw_req *cqr;

	/*
	 * scan for RCD command in extended SenseID data
	 */
	ciw = ccw_device_get_ciw(device->cdev, CIW_TYPE_RCD);
	if (!ciw || ciw->cmd == 0) {
		ret = -EOPNOTSUPP;
		goto out_error;
	}
	rcd_buf = kzalloc(ciw->count, GFP_KERNEL | GFP_DMA);
	if (!rcd_buf) {
		ret = -ENOMEM;
		goto out_error;
	}
	cqr = dasd_eckd_build_rcd_lpm(device, rcd_buf, ciw, lpm);
	if (IS_ERR(cqr)) {
		ret =  PTR_ERR(cqr);
		goto out_error;
	}
	ret = dasd_sleep_on(cqr);
	/*
	 * on success we update the user input parms
	 */
	dasd_sfree_request(cqr, cqr->device);
	if (ret)
		goto out_error;

	*rcd_buffer_size = ciw->count;
	*rcd_buffer = rcd_buf;
	return 0;
out_error:
	kfree(rcd_buf);
	*rcd_buffer = NULL;
	*rcd_buffer_size = 0;
	return ret;
}

static int
dasd_eckd_read_conf(struct dasd_device *device)
{
	void *conf_data;
	int conf_len, conf_data_saved;
	int rc;
	__u8 lpm;
	struct dasd_eckd_private *private;
	struct dasd_eckd_path *path_data;

	private = (struct dasd_eckd_private *) device->private;
	path_data = (struct dasd_eckd_path *) &private->path_data;
	path_data->opm = ccw_device_get_path_mask(device->cdev);
	lpm = 0x80;
	conf_data_saved = 0;

	/* get configuration data per operational path */
	for (lpm = 0x80; lpm; lpm>>= 1) {
		if (lpm & path_data->opm){
			rc = dasd_eckd_read_conf_lpm(device, &conf_data,
						     &conf_len, lpm);
			if (rc && rc != -EOPNOTSUPP) {	/* -EOPNOTSUPP is ok */
				MESSAGE(KERN_WARNING,
					"Read configuration data returned "
					"error %d", rc);
				return rc;
			}
			if (conf_data == NULL) {
				MESSAGE(KERN_WARNING, "%s", "No configuration "
					"data retrieved");
				continue;	/* no error */
			}
			if (conf_len != sizeof (struct dasd_eckd_confdata)) {
				MESSAGE(KERN_WARNING,
					"sizes of configuration data mismatch"
					"%d (read) vs %ld (expected)",
					conf_len,
					sizeof (struct dasd_eckd_confdata));
				kfree(conf_data);
				continue;	/* no error */
			}
			/* save first valid configuration data */
			if (!conf_data_saved){
				memcpy(&private->conf_data, conf_data,
				       sizeof (struct dasd_eckd_confdata));
				conf_data_saved++;
			}
			switch (((char *)conf_data)[242] & 0x07){
			case 0x02:
				path_data->npm |= lpm;
				break;
			case 0x03:
				path_data->ppm |= lpm;
				break;
			}
			kfree(conf_data);
		}
	}
	return 0;
}

/*
 * Build CP for Perform Subsystem Function - SSC.
 */
static struct dasd_ccw_req *
dasd_eckd_build_psf_ssc(struct dasd_device *device)
{
       struct dasd_ccw_req *cqr;
       struct dasd_psf_ssc_data *psf_ssc_data;
       struct ccw1 *ccw;

       cqr = dasd_smalloc_request("ECKD", 1 /* PSF */ ,
				  sizeof(struct dasd_psf_ssc_data),
				  device);

       if (IS_ERR(cqr)) {
	       DEV_MESSAGE(KERN_WARNING, device, "%s",
			   "Could not allocate PSF-SSC request");
	       return cqr;
       }
       psf_ssc_data = (struct dasd_psf_ssc_data *)cqr->data;
       psf_ssc_data->order = PSF_ORDER_SSC;
       psf_ssc_data->suborder = 0x08;

       ccw = cqr->cpaddr;
       ccw->cmd_code = DASD_ECKD_CCW_PSF;
       ccw->cda = (__u32)(addr_t)psf_ssc_data;
       ccw->count = 66;

       cqr->device = device;
       cqr->expires = 10*HZ;
       cqr->buildclk = get_clock();
       cqr->status = DASD_CQR_FILLED;
       return cqr;
}

/*
 * Perform Subsystem Function.
 * It is necessary to trigger CIO for channel revalidation since this
 * call might change behaviour of DASD devices.
 */
static int
dasd_eckd_psf_ssc(struct dasd_device *device)
{
       struct dasd_ccw_req *cqr;
       int rc;

       cqr = dasd_eckd_build_psf_ssc(device);
       if (IS_ERR(cqr))
	       return PTR_ERR(cqr);

       rc = dasd_sleep_on(cqr);
       if (!rc)
	       /* trigger CIO to reprobe devices */
	       css_schedule_reprobe();
       dasd_sfree_request(cqr, cqr->device);
       return rc;
}

/*
 * Valide storage server of current device.
 */
static int
dasd_eckd_validate_server(struct dasd_device *device, struct dasd_uid *uid)
{
	int rc;

	/* Currently PAV is the only reason to 'validate' server on LPAR */
	if (dasd_nopav || MACHINE_IS_VM)
		return 0;

	rc = dasd_eckd_psf_ssc(device);
	/* may be requested feature is not available on server,
	 * therefore just report error and go ahead */
	DEV_MESSAGE(KERN_INFO, device,
		    "PSF-SSC on storage subsystem %s.%s.%04x returned rc=%d",
		    uid->vendor, uid->serial, uid->ssid, rc);
	/* RE-Read Configuration Data */
	return dasd_eckd_read_conf(device);
}

/*
 * Check device characteristics.
 * If the device is accessible using ECKD discipline, the device is enabled.
 */
static int
dasd_eckd_check_characteristics(struct dasd_device *device)
{
	struct dasd_eckd_private *private;
	struct dasd_uid uid;
	void *rdc_data;
	int rc;

	private = (struct dasd_eckd_private *) device->private;
	if (private == NULL) {
		private = kzalloc(sizeof(struct dasd_eckd_private),
				  GFP_KERNEL | GFP_DMA);
		if (private == NULL) {
			DEV_MESSAGE(KERN_WARNING, device, "%s",
				    "memory allocation failed for private "
				    "data");
			return -ENOMEM;
		}
		device->private = (void *) private;
	}
	/* Invalidate status of initial analysis. */
	private->init_cqr_status = -1;
	/* Set default cache operations. */
	private->attrib.operation = DASD_NORMAL_CACHE;
	private->attrib.nr_cyl = 0;

	/* Read Configuration Data */
	rc = dasd_eckd_read_conf(device);
	if (rc)
		return rc;

	/* Generate device unique id and register in devmap */
	rc = dasd_eckd_generate_uid(device, &uid);
	if (rc)
		return rc;
	rc = dasd_set_uid(device->cdev, &uid);
	if (rc == 1)	/* new server found */
		rc = dasd_eckd_validate_server(device, &uid);
	if (rc)
		return rc;

	/* Read Device Characteristics */
	rdc_data = (void *) &(private->rdc_data);
	memset(rdc_data, 0, sizeof(rdc_data));
	rc = dasd_generic_read_dev_chars(device, "ECKD", &rdc_data, 64);
	if (rc)
		DEV_MESSAGE(KERN_WARNING, device,
			    "Read device characteristics returned "
			    "rc=%d", rc);

	DEV_MESSAGE(KERN_INFO, device,
		    "%04X/%02X(CU:%04X/%02X) Cyl:%d Head:%d Sec:%d",
		    private->rdc_data.dev_type,
		    private->rdc_data.dev_model,
		    private->rdc_data.cu_type,
		    private->rdc_data.cu_model.model,
		    private->rdc_data.no_cyl,
		    private->rdc_data.trk_per_cyl,
		    private->rdc_data.sec_per_trk);
	return rc;
}

static struct dasd_ccw_req *
dasd_eckd_analysis_ccw(struct dasd_device *device)
{
	struct dasd_eckd_private *private;
	struct eckd_count *count_data;
	struct LO_eckd_data *LO_data;
	struct dasd_ccw_req *cqr;
	struct ccw1 *ccw;
	int cplength, datasize;
	int i;

	private = (struct dasd_eckd_private *) device->private;

	cplength = 8;
	datasize = sizeof(struct DE_eckd_data) + 2*sizeof(struct LO_eckd_data);
	cqr = dasd_smalloc_request(dasd_eckd_discipline.name,
				   cplength, datasize, device);
	if (IS_ERR(cqr))
		return cqr;
	ccw = cqr->cpaddr;
	/* Define extent for the first 3 tracks. */
	define_extent(ccw++, cqr->data, 0, 2,
		      DASD_ECKD_CCW_READ_COUNT, device);
	LO_data = cqr->data + sizeof (struct DE_eckd_data);
	/* Locate record for the first 4 records on track 0. */
	ccw[-1].flags |= CCW_FLAG_CC;
	locate_record(ccw++, LO_data++, 0, 0, 4,
		      DASD_ECKD_CCW_READ_COUNT, device, 0);

	count_data = private->count_area;
	for (i = 0; i < 4; i++) {
		ccw[-1].flags |= CCW_FLAG_CC;
		ccw->cmd_code = DASD_ECKD_CCW_READ_COUNT;
		ccw->flags = 0;
		ccw->count = 8;
		ccw->cda = (__u32)(addr_t) count_data;
		ccw++;
		count_data++;
	}

	/* Locate record for the first record on track 2. */
	ccw[-1].flags |= CCW_FLAG_CC;
	locate_record(ccw++, LO_data++, 2, 0, 1,
		      DASD_ECKD_CCW_READ_COUNT, device, 0);
	/* Read count ccw. */
	ccw[-1].flags |= CCW_FLAG_CC;
	ccw->cmd_code = DASD_ECKD_CCW_READ_COUNT;
	ccw->flags = 0;
	ccw->count = 8;
	ccw->cda = (__u32)(addr_t) count_data;

	cqr->device = device;
	cqr->retries = 0;
	cqr->buildclk = get_clock();
	cqr->status = DASD_CQR_FILLED;
	return cqr;
}

/*
 * This is the callback function for the init_analysis cqr. It saves
 * the status of the initial analysis ccw before it frees it and kicks
 * the device to continue the startup sequence. This will call
 * dasd_eckd_do_analysis again (if the devices has not been marked
 * for deletion in the meantime).
 */
static void
dasd_eckd_analysis_callback(struct dasd_ccw_req *init_cqr, void *data)
{
	struct dasd_eckd_private *private;
	struct dasd_device *device;

	device = init_cqr->device;
	private = (struct dasd_eckd_private *) device->private;
	private->init_cqr_status = init_cqr->status;
	dasd_sfree_request(init_cqr, device);
	dasd_kick_device(device);
}

static int
dasd_eckd_start_analysis(struct dasd_device *device)
{
	struct dasd_eckd_private *private;
	struct dasd_ccw_req *init_cqr;

	private = (struct dasd_eckd_private *) device->private;
	init_cqr = dasd_eckd_analysis_ccw(device);
	if (IS_ERR(init_cqr))
		return PTR_ERR(init_cqr);
	init_cqr->callback = dasd_eckd_analysis_callback;
	init_cqr->callback_data = NULL;
	init_cqr->expires = 5*HZ;
	dasd_add_request_head(init_cqr);
	return -EAGAIN;
}

static int
dasd_eckd_end_analysis(struct dasd_device *device)
{
	struct dasd_eckd_private *private;
	struct eckd_count *count_area;
	unsigned int sb, blk_per_trk;
	int status, i;

	private = (struct dasd_eckd_private *) device->private;
	status = private->init_cqr_status;
	private->init_cqr_status = -1;
	if (status != DASD_CQR_DONE) {
		DEV_MESSAGE(KERN_WARNING, device, "%s",
			    "volume analysis returned unformatted disk");
		return -EMEDIUMTYPE;
	}

	private->uses_cdl = 1;
	/* Calculate number of blocks/records per track. */
	blk_per_trk = recs_per_track(&private->rdc_data, 0, device->bp_block);
	/* Check Track 0 for Compatible Disk Layout */
	count_area = NULL;
	for (i = 0; i < 3; i++) {
		if (private->count_area[i].kl != 4 ||
		    private->count_area[i].dl != dasd_eckd_cdl_reclen(i) - 4) {
			private->uses_cdl = 0;
			break;
		}
	}
	if (i == 3)
		count_area = &private->count_area[4];

	if (private->uses_cdl == 0) {
		for (i = 0; i < 5; i++) {
			if ((private->count_area[i].kl != 0) ||
			    (private->count_area[i].dl !=
			     private->count_area[0].dl))
				break;
		}
		if (i == 5)
			count_area = &private->count_area[0];
	} else {
		if (private->count_area[3].record == 1)
			DEV_MESSAGE(KERN_WARNING, device, "%s",
				    "Trk 0: no records after VTOC!");
	}
	if (count_area != NULL && count_area->kl == 0) {
		/* we found notthing violating our disk layout */
		if (dasd_check_blocksize(count_area->dl) == 0)
			device->bp_block = count_area->dl;
	}
	if (device->bp_block == 0) {
		DEV_MESSAGE(KERN_WARNING, device, "%s",
			    "Volume has incompatible disk layout");
		return -EMEDIUMTYPE;
	}
	device->s2b_shift = 0;	/* bits to shift 512 to get a block */
	for (sb = 512; sb < device->bp_block; sb = sb << 1)
		device->s2b_shift++;

	blk_per_trk = recs_per_track(&private->rdc_data, 0, device->bp_block);
	device->blocks = (private->rdc_data.no_cyl *
			  private->rdc_data.trk_per_cyl *
			  blk_per_trk);

	DEV_MESSAGE(KERN_INFO, device,
		    "(%dkB blks): %dkB at %dkB/trk %s",
		    (device->bp_block >> 10),
		    ((private->rdc_data.no_cyl *
		      private->rdc_data.trk_per_cyl *
		      blk_per_trk * (device->bp_block >> 9)) >> 1),
		    ((blk_per_trk * device->bp_block) >> 10),
		    private->uses_cdl ?
		    "compatible disk layout" : "linux disk layout");

	return 0;
}

static int
dasd_eckd_do_analysis(struct dasd_device *device)
{
	struct dasd_eckd_private *private;

	private = (struct dasd_eckd_private *) device->private;
	if (private->init_cqr_status < 0)
		return dasd_eckd_start_analysis(device);
	else
		return dasd_eckd_end_analysis(device);
}

static int
dasd_eckd_fill_geometry(struct dasd_device *device, struct hd_geometry *geo)
{
	struct dasd_eckd_private *private;

	private = (struct dasd_eckd_private *) device->private;
	if (dasd_check_blocksize(device->bp_block) == 0) {
		geo->sectors = recs_per_track(&private->rdc_data,
					      0, device->bp_block);
	}
	geo->cylinders = private->rdc_data.no_cyl;
	geo->heads = private->rdc_data.trk_per_cyl;
	return 0;
}

static struct dasd_ccw_req *
dasd_eckd_format_device(struct dasd_device * device,
			struct format_data_t * fdata)
{
	struct dasd_eckd_private *private;
	struct dasd_ccw_req *fcp;
	struct eckd_count *ect;
	struct ccw1 *ccw;
	void *data;
	int rpt, cyl, head;
	int cplength, datasize;
	int i;

	private = (struct dasd_eckd_private *) device->private;
	rpt = recs_per_track(&private->rdc_data, 0, fdata->blksize);
	cyl = fdata->start_unit / private->rdc_data.trk_per_cyl;
	head = fdata->start_unit % private->rdc_data.trk_per_cyl;

	/* Sanity checks. */
	if (fdata->start_unit >=
	    (private->rdc_data.no_cyl * private->rdc_data.trk_per_cyl)) {
		DEV_MESSAGE(KERN_INFO, device, "Track no %d too big!",
			    fdata->start_unit);
		return ERR_PTR(-EINVAL);
	}
	if (fdata->start_unit > fdata->stop_unit) {
		DEV_MESSAGE(KERN_INFO, device, "Track %d reached! ending.",
			    fdata->start_unit);
		return ERR_PTR(-EINVAL);
	}
	if (dasd_check_blocksize(fdata->blksize) != 0) {
		DEV_MESSAGE(KERN_WARNING, device,
			    "Invalid blocksize %d...terminating!",
			    fdata->blksize);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * fdata->intensity is a bit string that tells us what to do:
	 *   Bit 0: write record zero
	 *   Bit 1: write home address, currently not supported
	 *   Bit 2: invalidate tracks
	 *   Bit 3: use OS/390 compatible disk layout (cdl)
	 * Only some bit combinations do make sense.
	 */
	switch (fdata->intensity) {
	case 0x00:	/* Normal format */
	case 0x08:	/* Normal format, use cdl. */
		cplength = 2 + rpt;
		datasize = sizeof(struct DE_eckd_data) +
			sizeof(struct LO_eckd_data) +
			rpt * sizeof(struct eckd_count);
		break;
	case 0x01:	/* Write record zero and format track. */
	case 0x09:	/* Write record zero and format track, use cdl. */
		cplength = 3 + rpt;
		datasize = sizeof(struct DE_eckd_data) +
			sizeof(struct LO_eckd_data) +
			sizeof(struct eckd_count) +
			rpt * sizeof(struct eckd_count);
		break;
	case 0x04:	/* Invalidate track. */
	case 0x0c:	/* Invalidate track, use cdl. */
		cplength = 3;
		datasize = sizeof(struct DE_eckd_data) +
			sizeof(struct LO_eckd_data) +
			sizeof(struct eckd_count);
		break;
	default:
		DEV_MESSAGE(KERN_WARNING, device, "Invalid flags 0x%x.",
			    fdata->intensity);
		return ERR_PTR(-EINVAL);
	}
	/* Allocate the format ccw request. */
	fcp = dasd_smalloc_request(dasd_eckd_discipline.name,
				   cplength, datasize, device);
	if (IS_ERR(fcp))
		return fcp;

	data = fcp->data;
	ccw = fcp->cpaddr;

	switch (fdata->intensity & ~0x08) {
	case 0x00: /* Normal format. */
		define_extent(ccw++, (struct DE_eckd_data *) data,
			      fdata->start_unit, fdata->start_unit,
			      DASD_ECKD_CCW_WRITE_CKD, device);
		data += sizeof(struct DE_eckd_data);
		ccw[-1].flags |= CCW_FLAG_CC;
		locate_record(ccw++, (struct LO_eckd_data *) data,
			      fdata->start_unit, 0, rpt,
			      DASD_ECKD_CCW_WRITE_CKD, device,
			      fdata->blksize);
		data += sizeof(struct LO_eckd_data);
		break;
	case 0x01: /* Write record zero + format track. */
		define_extent(ccw++, (struct DE_eckd_data *) data,
			      fdata->start_unit, fdata->start_unit,
			      DASD_ECKD_CCW_WRITE_RECORD_ZERO,
			      device);
		data += sizeof(struct DE_eckd_data);
		ccw[-1].flags |= CCW_FLAG_CC;
		locate_record(ccw++, (struct LO_eckd_data *) data,
			      fdata->start_unit, 0, rpt + 1,
			      DASD_ECKD_CCW_WRITE_RECORD_ZERO, device,
			      device->bp_block);
		data += sizeof(struct LO_eckd_data);
		break;
	case 0x04: /* Invalidate track. */
		define_extent(ccw++, (struct DE_eckd_data *) data,
			      fdata->start_unit, fdata->start_unit,
			      DASD_ECKD_CCW_WRITE_CKD, device);
		data += sizeof(struct DE_eckd_data);
		ccw[-1].flags |= CCW_FLAG_CC;
		locate_record(ccw++, (struct LO_eckd_data *) data,
			      fdata->start_unit, 0, 1,
			      DASD_ECKD_CCW_WRITE_CKD, device, 8);
		data += sizeof(struct LO_eckd_data);
		break;
	}
	if (fdata->intensity & 0x01) {	/* write record zero */
		ect = (struct eckd_count *) data;
		data += sizeof(struct eckd_count);
		ect->cyl = cyl;
		ect->head = head;
		ect->record = 0;
		ect->kl = 0;
		ect->dl = 8;
		ccw[-1].flags |= CCW_FLAG_CC;
		ccw->cmd_code = DASD_ECKD_CCW_WRITE_RECORD_ZERO;
		ccw->flags = CCW_FLAG_SLI;
		ccw->count = 8;
		ccw->cda = (__u32)(addr_t) ect;
		ccw++;
	}
	if ((fdata->intensity & ~0x08) & 0x04) {	/* erase track */
		ect = (struct eckd_count *) data;
		data += sizeof(struct eckd_count);
		ect->cyl = cyl;
		ect->head = head;
		ect->record = 1;
		ect->kl = 0;
		ect->dl = 0;
		ccw[-1].flags |= CCW_FLAG_CC;
		ccw->cmd_code = DASD_ECKD_CCW_WRITE_CKD;
		ccw->flags = CCW_FLAG_SLI;
		ccw->count = 8;
		ccw->cda = (__u32)(addr_t) ect;
	} else {		/* write remaining records */
		for (i = 0; i < rpt; i++) {
			ect = (struct eckd_count *) data;
			data += sizeof(struct eckd_count);
			ect->cyl = cyl;
			ect->head = head;
			ect->record = i + 1;
			ect->kl = 0;
			ect->dl = fdata->blksize;
			/* Check for special tracks 0-1 when formatting CDL */
			if ((fdata->intensity & 0x08) &&
			    fdata->start_unit == 0) {
				if (i < 3) {
					ect->kl = 4;
					ect->dl = sizes_trk0[i] - 4;
				}
			}
			if ((fdata->intensity & 0x08) &&
			    fdata->start_unit == 1) {
				ect->kl = 44;
				ect->dl = LABEL_SIZE - 44;
			}
			ccw[-1].flags |= CCW_FLAG_CC;
			ccw->cmd_code = DASD_ECKD_CCW_WRITE_CKD;
			ccw->flags = CCW_FLAG_SLI;
			ccw->count = 8;
			ccw->cda = (__u32)(addr_t) ect;
			ccw++;
		}
	}
	fcp->device = device;
	fcp->retries = 2;	/* set retry counter to enable ERP */
	fcp->buildclk = get_clock();
	fcp->status = DASD_CQR_FILLED;
	return fcp;
}

static dasd_era_t
dasd_eckd_examine_error(struct dasd_ccw_req * cqr, struct irb * irb)
{
	struct dasd_device *device = (struct dasd_device *) cqr->device;
	struct ccw_device *cdev = device->cdev;

	if (irb->scsw.cstat == 0x00 &&
	    irb->scsw.dstat == (DEV_STAT_CHN_END | DEV_STAT_DEV_END))
		return dasd_era_none;

	switch (cdev->id.cu_type) {
	case 0x3990:
	case 0x2105:
	case 0x2107:
	case 0x1750:
		return dasd_3990_erp_examine(cqr, irb);
	case 0x9343:
		return dasd_9343_erp_examine(cqr, irb);
	case 0x3880:
	default:
		DEV_MESSAGE(KERN_WARNING, device, "%s",
			    "default (unknown CU type) - RECOVERABLE return");
		return dasd_era_recover;
	}
}

static dasd_erp_fn_t
dasd_eckd_erp_action(struct dasd_ccw_req * cqr)
{
	struct dasd_device *device = (struct dasd_device *) cqr->device;
	struct ccw_device *cdev = device->cdev;

	switch (cdev->id.cu_type) {
	case 0x3990:
	case 0x2105:
	case 0x2107:
	case 0x1750:
		return dasd_3990_erp_action;
	case 0x9343:
	case 0x3880:
	default:
		return dasd_default_erp_action;
	}
}

static dasd_erp_fn_t
dasd_eckd_erp_postaction(struct dasd_ccw_req * cqr)
{
	return dasd_default_erp_postaction;
}

static struct dasd_ccw_req *
dasd_eckd_build_cp(struct dasd_device * device, struct request *req)
{
	struct dasd_eckd_private *private;
	unsigned long *idaws;
	struct LO_eckd_data *LO_data;
	struct dasd_ccw_req *cqr;
	struct ccw1 *ccw;
	struct bio *bio;
	struct bio_vec *bv;
	char *dst;
	unsigned int blksize, blk_per_trk, off;
	int count, cidaw, cplength, datasize;
	sector_t recid, first_rec, last_rec;
	sector_t first_trk, last_trk;
	unsigned int first_offs, last_offs;
	unsigned char cmd, rcmd;
	int i;

	private = (struct dasd_eckd_private *) device->private;
	if (rq_data_dir(req) == READ)
		cmd = DASD_ECKD_CCW_READ_MT;
	else if (rq_data_dir(req) == WRITE)
		cmd = DASD_ECKD_CCW_WRITE_MT;
	else
		return ERR_PTR(-EINVAL);
	/* Calculate number of blocks/records per track. */
	blksize = device->bp_block;
	blk_per_trk = recs_per_track(&private->rdc_data, 0, blksize);
	/* Calculate record id of first and last block. */
	first_rec = first_trk = req->sector >> device->s2b_shift;
	first_offs = sector_div(first_trk, blk_per_trk);
	last_rec = last_trk =
		(req->sector + req->nr_sectors - 1) >> device->s2b_shift;
	last_offs = sector_div(last_trk, blk_per_trk);
	/* Check struct bio and count the number of blocks for the request. */
	count = 0;
	cidaw = 0;
	rq_for_each_bio(bio, req) {
		bio_for_each_segment(bv, bio, i) {
			if (bv->bv_len & (blksize - 1))
				/* Eckd can only do full blocks. */
				return ERR_PTR(-EINVAL);
			count += bv->bv_len >> (device->s2b_shift + 9);
#if defined(CONFIG_64BIT)
			if (idal_is_needed (page_address(bv->bv_page),
					    bv->bv_len))
				cidaw += bv->bv_len >> (device->s2b_shift + 9);
#endif
		}
	}
	/* Paranoia. */
	if (count != last_rec - first_rec + 1)
		return ERR_PTR(-EINVAL);
	/* 1x define extent + 1x locate record + number of blocks */
	cplength = 2 + count;
	/* 1x define extent + 1x locate record + cidaws*sizeof(long) */
	datasize = sizeof(struct DE_eckd_data) + sizeof(struct LO_eckd_data) +
		cidaw * sizeof(unsigned long);
	/* Find out the number of additional locate record ccws for cdl. */
	if (private->uses_cdl && first_rec < 2*blk_per_trk) {
		if (last_rec >= 2*blk_per_trk)
			count = 2*blk_per_trk - first_rec;
		cplength += count;
		datasize += count*sizeof(struct LO_eckd_data);
	}
	/* Allocate the ccw request. */
	cqr = dasd_smalloc_request(dasd_eckd_discipline.name,
				   cplength, datasize, device);
	if (IS_ERR(cqr))
		return cqr;
	ccw = cqr->cpaddr;
	/* First ccw is define extent. */
	if (define_extent(ccw++, cqr->data, first_trk,
			  last_trk, cmd, device) == -EAGAIN) {
		/* Clock not in sync and XRC is enabled. Try again later. */
		dasd_sfree_request(cqr, device);
		return ERR_PTR(-EAGAIN);
	}
	/* Build locate_record+read/write/ccws. */
	idaws = (unsigned long *) (cqr->data + sizeof(struct DE_eckd_data));
	LO_data = (struct LO_eckd_data *) (idaws + cidaw);
	recid = first_rec;
	if (private->uses_cdl == 0 || recid > 2*blk_per_trk) {
		/* Only standard blocks so there is just one locate record. */
		ccw[-1].flags |= CCW_FLAG_CC;
		locate_record(ccw++, LO_data++, first_trk, first_offs + 1,
			      last_rec - recid + 1, cmd, device, blksize);
	}
	rq_for_each_bio(bio, req) bio_for_each_segment(bv, bio, i) {
		dst = page_address(bv->bv_page) + bv->bv_offset;
		if (dasd_page_cache) {
			char *copy = kmem_cache_alloc(dasd_page_cache,
						      GFP_DMA | __GFP_NOWARN);
			if (copy && rq_data_dir(req) == WRITE)
				memcpy(copy + bv->bv_offset, dst, bv->bv_len);
			if (copy)
				dst = copy + bv->bv_offset;
		}
		for (off = 0; off < bv->bv_len; off += blksize) {
			sector_t trkid = recid;
			unsigned int recoffs = sector_div(trkid, blk_per_trk);
			rcmd = cmd;
			count = blksize;
			/* Locate record for cdl special block ? */
			if (private->uses_cdl && recid < 2*blk_per_trk) {
				if (dasd_eckd_cdl_special(blk_per_trk, recid)){
					rcmd |= 0x8;
					count = dasd_eckd_cdl_reclen(recid);
					if (count < blksize &&
					    rq_data_dir(req) == READ)
						memset(dst + count, 0xe5,
						       blksize - count);
				}
				ccw[-1].flags |= CCW_FLAG_CC;
				locate_record(ccw++, LO_data++,
					      trkid, recoffs + 1,
					      1, rcmd, device, count);
			}
			/* Locate record for standard blocks ? */
			if (private->uses_cdl && recid == 2*blk_per_trk) {
				ccw[-1].flags |= CCW_FLAG_CC;
				locate_record(ccw++, LO_data++,
					      trkid, recoffs + 1,
					      last_rec - recid + 1,
					      cmd, device, count);
			}
			/* Read/write ccw. */
			ccw[-1].flags |= CCW_FLAG_CC;
			ccw->cmd_code = rcmd;
			ccw->count = count;
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
	if (req->cmd_flags & REQ_FAILFAST)
		set_bit(DASD_CQR_FLAGS_FAILFAST, &cqr->flags);
	cqr->device = device;
	cqr->expires = 5 * 60 * HZ;	/* 5 minutes */
	cqr->lpm = private->path_data.ppm;
	cqr->retries = 256;
	cqr->buildclk = get_clock();
	cqr->status = DASD_CQR_FILLED;
	return cqr;
}

static int
dasd_eckd_free_cp(struct dasd_ccw_req *cqr, struct request *req)
{
	struct dasd_eckd_private *private;
	struct ccw1 *ccw;
	struct bio *bio;
	struct bio_vec *bv;
	char *dst, *cda;
	unsigned int blksize, blk_per_trk, off;
	sector_t recid;
	int i, status;

	if (!dasd_page_cache)
		goto out;
	private = (struct dasd_eckd_private *) cqr->device->private;
	blksize = cqr->device->bp_block;
	blk_per_trk = recs_per_track(&private->rdc_data, 0, blksize);
	recid = req->sector >> cqr->device->s2b_shift;
	ccw = cqr->cpaddr;
	/* Skip over define extent & locate record. */
	ccw++;
	if (private->uses_cdl == 0 || recid > 2*blk_per_trk)
		ccw++;
	rq_for_each_bio(bio, req) bio_for_each_segment(bv, bio, i) {
		dst = page_address(bv->bv_page) + bv->bv_offset;
		for (off = 0; off < bv->bv_len; off += blksize) {
			/* Skip locate record. */
			if (private->uses_cdl && recid <= 2*blk_per_trk)
				ccw++;
			if (dst) {
				if (ccw->flags & CCW_FLAG_IDA)
					cda = *((char **)((addr_t) ccw->cda));
				else
					cda = (char *)((addr_t) ccw->cda);
				if (dst != cda) {
					if (rq_data_dir(req) == READ)
						memcpy(dst, cda, bv->bv_len);
					kmem_cache_free(dasd_page_cache,
					    (void *)((addr_t)cda & PAGE_MASK));
				}
				dst = NULL;
			}
			ccw++;
			recid++;
		}
	}
out:
	status = cqr->status == DASD_CQR_DONE;
	dasd_sfree_request(cqr, cqr->device);
	return status;
}

static int
dasd_eckd_fill_info(struct dasd_device * device,
		    struct dasd_information2_t * info)
{
	struct dasd_eckd_private *private;

	private = (struct dasd_eckd_private *) device->private;
	info->label_block = 2;
	info->FBA_layout = private->uses_cdl ? 0 : 1;
	info->format = private->uses_cdl ? DASD_FORMAT_CDL : DASD_FORMAT_LDL;
	info->characteristics_size = sizeof(struct dasd_eckd_characteristics);
	memcpy(info->characteristics, &private->rdc_data,
	       sizeof(struct dasd_eckd_characteristics));
	info->confdata_size = sizeof (struct dasd_eckd_confdata);
	memcpy(info->configuration_data, &private->conf_data,
	       sizeof (struct dasd_eckd_confdata));
	return 0;
}

/*
 * SECTION: ioctl functions for eckd devices.
 */

/*
 * Release device ioctl.
 * Buils a channel programm to releases a prior reserved
 * (see dasd_eckd_reserve) device.
 */
static int
dasd_eckd_release(struct dasd_device *device)
{
	struct dasd_ccw_req *cqr;
	int rc;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	cqr = dasd_smalloc_request(dasd_eckd_discipline.name,
				   1, 32, device);
	if (IS_ERR(cqr)) {
		DEV_MESSAGE(KERN_WARNING, device, "%s",
			    "Could not allocate initialization request");
		return PTR_ERR(cqr);
	}
	cqr->cpaddr->cmd_code = DASD_ECKD_CCW_RELEASE;
        cqr->cpaddr->flags |= CCW_FLAG_SLI;
        cqr->cpaddr->count = 32;
	cqr->cpaddr->cda = (__u32)(addr_t) cqr->data;
	cqr->device = device;
	clear_bit(DASD_CQR_FLAGS_USE_ERP, &cqr->flags);
	set_bit(DASD_CQR_FLAGS_FAILFAST, &cqr->flags);
	cqr->retries = 2;	/* set retry counter to enable basic ERP */
	cqr->expires = 2 * HZ;
	cqr->buildclk = get_clock();
	cqr->status = DASD_CQR_FILLED;

	rc = dasd_sleep_on_immediatly(cqr);

	dasd_sfree_request(cqr, cqr->device);
	return rc;
}

/*
 * Reserve device ioctl.
 * Options are set to 'synchronous wait for interrupt' and
 * 'timeout the request'. This leads to a terminate IO if
 * the interrupt is outstanding for a certain time.
 */
static int
dasd_eckd_reserve(struct dasd_device *device)
{
	struct dasd_ccw_req *cqr;
	int rc;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	cqr = dasd_smalloc_request(dasd_eckd_discipline.name,
				   1, 32, device);
	if (IS_ERR(cqr)) {
		DEV_MESSAGE(KERN_WARNING, device, "%s",
			    "Could not allocate initialization request");
		return PTR_ERR(cqr);
	}
	cqr->cpaddr->cmd_code = DASD_ECKD_CCW_RESERVE;
        cqr->cpaddr->flags |= CCW_FLAG_SLI;
        cqr->cpaddr->count = 32;
	cqr->cpaddr->cda = (__u32)(addr_t) cqr->data;
	cqr->device = device;
	clear_bit(DASD_CQR_FLAGS_USE_ERP, &cqr->flags);
	set_bit(DASD_CQR_FLAGS_FAILFAST, &cqr->flags);
	cqr->retries = 2;	/* set retry counter to enable basic ERP */
	cqr->expires = 2 * HZ;
	cqr->buildclk = get_clock();
	cqr->status = DASD_CQR_FILLED;

	rc = dasd_sleep_on_immediatly(cqr);

	dasd_sfree_request(cqr, cqr->device);
	return rc;
}

/*
 * Steal lock ioctl - unconditional reserve device.
 * Buils a channel programm to break a device's reservation.
 * (unconditional reserve)
 */
static int
dasd_eckd_steal_lock(struct dasd_device *device)
{
	struct dasd_ccw_req *cqr;
	int rc;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	cqr = dasd_smalloc_request(dasd_eckd_discipline.name,
				   1, 32, device);
	if (IS_ERR(cqr)) {
		DEV_MESSAGE(KERN_WARNING, device, "%s",
			    "Could not allocate initialization request");
		return PTR_ERR(cqr);
	}
	cqr->cpaddr->cmd_code = DASD_ECKD_CCW_SLCK;
        cqr->cpaddr->flags |= CCW_FLAG_SLI;
        cqr->cpaddr->count = 32;
	cqr->cpaddr->cda = (__u32)(addr_t) cqr->data;
	cqr->device = device;
	clear_bit(DASD_CQR_FLAGS_USE_ERP, &cqr->flags);
	set_bit(DASD_CQR_FLAGS_FAILFAST, &cqr->flags);
	cqr->retries = 2;	/* set retry counter to enable basic ERP */
	cqr->expires = 2 * HZ;
	cqr->buildclk = get_clock();
	cqr->status = DASD_CQR_FILLED;

	rc = dasd_sleep_on_immediatly(cqr);

	dasd_sfree_request(cqr, cqr->device);
	return rc;
}

/*
 * Read performance statistics
 */
static int
dasd_eckd_performance(struct dasd_device *device, void __user *argp)
{
	struct dasd_psf_prssd_data *prssdp;
	struct dasd_rssd_perf_stats_t *stats;
	struct dasd_ccw_req *cqr;
	struct ccw1 *ccw;
	int rc;

	cqr = dasd_smalloc_request(dasd_eckd_discipline.name,
				   1 /* PSF */  + 1 /* RSSD */ ,
				   (sizeof (struct dasd_psf_prssd_data) +
				    sizeof (struct dasd_rssd_perf_stats_t)),
				   device);
	if (IS_ERR(cqr)) {
		DEV_MESSAGE(KERN_WARNING, device, "%s",
			    "Could not allocate initialization request");
		return PTR_ERR(cqr);
	}
	cqr->device = device;
	cqr->retries = 0;
	cqr->expires = 10 * HZ;

	/* Prepare for Read Subsystem Data */
	prssdp = (struct dasd_psf_prssd_data *) cqr->data;
	memset(prssdp, 0, sizeof (struct dasd_psf_prssd_data));
	prssdp->order = PSF_ORDER_PRSSD;
	prssdp->suborder = 0x01;	/* Perfomance Statistics */
	prssdp->varies[1] = 0x01;	/* Perf Statistics for the Subsystem */

	ccw = cqr->cpaddr;
	ccw->cmd_code = DASD_ECKD_CCW_PSF;
	ccw->count = sizeof (struct dasd_psf_prssd_data);
	ccw->flags |= CCW_FLAG_CC;
	ccw->cda = (__u32)(addr_t) prssdp;

	/* Read Subsystem Data - Performance Statistics */
	stats = (struct dasd_rssd_perf_stats_t *) (prssdp + 1);
	memset(stats, 0, sizeof (struct dasd_rssd_perf_stats_t));

	ccw++;
	ccw->cmd_code = DASD_ECKD_CCW_RSSD;
	ccw->count = sizeof (struct dasd_rssd_perf_stats_t);
	ccw->cda = (__u32)(addr_t) stats;

	cqr->buildclk = get_clock();
	cqr->status = DASD_CQR_FILLED;
	rc = dasd_sleep_on(cqr);
	if (rc == 0) {
		/* Prepare for Read Subsystem Data */
		prssdp = (struct dasd_psf_prssd_data *) cqr->data;
		stats = (struct dasd_rssd_perf_stats_t *) (prssdp + 1);
		if (copy_to_user(argp, stats,
				 sizeof(struct dasd_rssd_perf_stats_t)))
			rc = -EFAULT;
	}
	dasd_sfree_request(cqr, cqr->device);
	return rc;
}

/*
 * Get attributes (cache operations)
 * Returnes the cache attributes used in Define Extend (DE).
 */
static int
dasd_eckd_get_attrib(struct dasd_device *device, void __user *argp)
{
	struct dasd_eckd_private *private =
		(struct dasd_eckd_private *)device->private;
	struct attrib_data_t attrib = private->attrib;
	int rc;

        if (!capable(CAP_SYS_ADMIN))
                return -EACCES;
	if (!argp)
                return -EINVAL;

	rc = 0;
	if (copy_to_user(argp, (long *) &attrib,
			 sizeof (struct attrib_data_t)))
		rc = -EFAULT;

	return rc;
}

/*
 * Set attributes (cache operations)
 * Stores the attributes for cache operation to be used in Define Extend (DE).
 */
static int
dasd_eckd_set_attrib(struct dasd_device *device, void __user *argp)
{
	struct dasd_eckd_private *private =
		(struct dasd_eckd_private *)device->private;
	struct attrib_data_t attrib;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (!argp)
		return -EINVAL;

	if (copy_from_user(&attrib, argp, sizeof(struct attrib_data_t)))
		return -EFAULT;
	private->attrib = attrib;

	DEV_MESSAGE(KERN_INFO, device,
		    "cache operation mode set to %x (%i cylinder prestage)",
		    private->attrib.operation, private->attrib.nr_cyl);
	return 0;
}

static int
dasd_eckd_ioctl(struct dasd_device *device, unsigned int cmd, void __user *argp)
{
	switch (cmd) {
	case BIODASDGATTR:
		return dasd_eckd_get_attrib(device, argp);
	case BIODASDSATTR:
		return dasd_eckd_set_attrib(device, argp);
	case BIODASDPSRD:
		return dasd_eckd_performance(device, argp);
	case BIODASDRLSE:
		return dasd_eckd_release(device);
	case BIODASDRSRV:
		return dasd_eckd_reserve(device);
	case BIODASDSLCK:
		return dasd_eckd_steal_lock(device);
	default:
		return -ENOIOCTLCMD;
	}
}

/*
 * Dump the range of CCWs into 'page' buffer
 * and return number of printed chars.
 */
static int
dasd_eckd_dump_ccw_range(struct ccw1 *from, struct ccw1 *to, char *page)
{
	int len, count;
	char *datap;

	len = 0;
	while (from <= to) {
		len += sprintf(page + len, KERN_ERR PRINTK_HEADER
			       " CCW %p: %08X %08X DAT:",
			       from, ((int *) from)[0], ((int *) from)[1]);

		/* get pointer to data (consider IDALs) */
		if (from->flags & CCW_FLAG_IDA)
			datap = (char *) *((addr_t *) (addr_t) from->cda);
		else
			datap = (char *) ((addr_t) from->cda);

		/* dump data (max 32 bytes) */
		for (count = 0; count < from->count && count < 32; count++) {
			if (count % 8 == 0) len += sprintf(page + len, " ");
			if (count % 4 == 0) len += sprintf(page + len, " ");
			len += sprintf(page + len, "%02x", datap[count]);
		}
		len += sprintf(page + len, "\n");
		from++;
	}
	return len;
}

/*
 * Print sense data and related channel program.
 * Parts are printed because printk buffer is only 1024 bytes.
 */
static void
dasd_eckd_dump_sense(struct dasd_device *device, struct dasd_ccw_req * req,
		     struct irb *irb)
{
	char *page;
	struct ccw1 *first, *last, *fail, *from, *to;
	int len, sl, sct;

	page = (char *) get_zeroed_page(GFP_ATOMIC);
	if (page == NULL) {
		DEV_MESSAGE(KERN_ERR, device, " %s",
			    "No memory to dump sense data");
		return;
	}
	/* dump the sense data */
	len = sprintf(page,  KERN_ERR PRINTK_HEADER
		      " I/O status report for device %s:\n",
		      device->cdev->dev.bus_id);
	len += sprintf(page + len, KERN_ERR PRINTK_HEADER
		       " in req: %p CS: 0x%02X DS: 0x%02X\n", req,
		       irb->scsw.cstat, irb->scsw.dstat);
	len += sprintf(page + len, KERN_ERR PRINTK_HEADER
		       " device %s: Failing CCW: %p\n",
		       device->cdev->dev.bus_id,
		       (void *) (addr_t) irb->scsw.cpa);
	if (irb->esw.esw0.erw.cons) {
		for (sl = 0; sl < 4; sl++) {
			len += sprintf(page + len, KERN_ERR PRINTK_HEADER
				       " Sense(hex) %2d-%2d:",
				       (8 * sl), ((8 * sl) + 7));

			for (sct = 0; sct < 8; sct++) {
				len += sprintf(page + len, " %02x",
					       irb->ecw[8 * sl + sct]);
			}
			len += sprintf(page + len, "\n");
		}

		if (irb->ecw[27] & DASD_SENSE_BIT_0) {
			/* 24 Byte Sense Data */
			sprintf(page + len, KERN_ERR PRINTK_HEADER
				" 24 Byte: %x MSG %x, "
				"%s MSGb to SYSOP\n",
				irb->ecw[7] >> 4, irb->ecw[7] & 0x0f,
				irb->ecw[1] & 0x10 ? "" : "no");
		} else {
			/* 32 Byte Sense Data */
			sprintf(page + len, KERN_ERR PRINTK_HEADER
				" 32 Byte: Format: %x "
				"Exception class %x\n",
				irb->ecw[6] & 0x0f, irb->ecw[22] >> 4);
		}
	} else {
		sprintf(page + len, KERN_ERR PRINTK_HEADER
			" SORRY - NO VALID SENSE AVAILABLE\n");
	}
	printk("%s", page);

	/* dump the Channel Program (max 140 Bytes per line) */
	/* Count CCW and print first CCWs (maximum 1024 % 140 = 7) */
	first = req->cpaddr;
	for (last = first; last->flags & (CCW_FLAG_CC | CCW_FLAG_DC); last++);
	to = min(first + 6, last);
	len = sprintf(page,  KERN_ERR PRINTK_HEADER
		      " Related CP in req: %p\n", req);
	dasd_eckd_dump_ccw_range(first, to, page + len);
	printk("%s", page);

	/* print failing CCW area (maximum 4) */
	/* scsw->cda is either valid or zero  */
	len = 0;
	from = ++to;
	fail = (struct ccw1 *)(addr_t) irb->scsw.cpa; /* failing CCW */
	if (from <  fail - 2) {
		from = fail - 2;     /* there is a gap - print header */
		len += sprintf(page, KERN_ERR PRINTK_HEADER "......\n");
	}
	to = min(fail + 1, last);
	len += dasd_eckd_dump_ccw_range(from, to, page + len);

	/* print last CCWs (maximum 2) */
	from = max(from, ++to);
	if (from < last - 1) {
		from = last - 1;     /* there is a gap - print header */
		len += sprintf(page + len, KERN_ERR PRINTK_HEADER "......\n");
	}
	len += dasd_eckd_dump_ccw_range(from, last, page + len);
	if (len > 0)
		printk("%s", page);
	free_page((unsigned long) page);
}

/*
 * max_blocks is dependent on the amount of storage that is available
 * in the static io buffer for each device. Currently each device has
 * 8192 bytes (=2 pages). For 64 bit one dasd_mchunkt_t structure has
 * 24 bytes, the struct dasd_ccw_req has 136 bytes and each block can use
 * up to 16 bytes (8 for the ccw and 8 for the idal pointer). In
 * addition we have one define extent ccw + 16 bytes of data and one
 * locate record ccw + 16 bytes of data. That makes:
 * (8192 - 24 - 136 - 8 - 16 - 8 - 16) / 16 = 499 blocks at maximum.
 * We want to fit two into the available memory so that we can immediately
 * start the next request if one finishes off. That makes 249.5 blocks
 * for one request. Give a little safety and the result is 240.
 */
static struct dasd_discipline dasd_eckd_discipline = {
	.owner = THIS_MODULE,
	.name = "ECKD",
	.ebcname = "ECKD",
	.max_blocks = 240,
	.check_device = dasd_eckd_check_characteristics,
	.do_analysis = dasd_eckd_do_analysis,
	.fill_geometry = dasd_eckd_fill_geometry,
	.start_IO = dasd_start_IO,
	.term_IO = dasd_term_IO,
	.format_device = dasd_eckd_format_device,
	.examine_error = dasd_eckd_examine_error,
	.erp_action = dasd_eckd_erp_action,
	.erp_postaction = dasd_eckd_erp_postaction,
	.build_cp = dasd_eckd_build_cp,
	.free_cp = dasd_eckd_free_cp,
	.dump_sense = dasd_eckd_dump_sense,
	.fill_info = dasd_eckd_fill_info,
	.ioctl = dasd_eckd_ioctl,
};

static int __init
dasd_eckd_init(void)
{
	ASCEBC(dasd_eckd_discipline.ebcname, 4);
	return ccw_driver_register(&dasd_eckd_driver);
}

static void __exit
dasd_eckd_cleanup(void)
{
	ccw_driver_unregister(&dasd_eckd_driver);
}

module_init(dasd_eckd_init);
module_exit(dasd_eckd_cleanup);
