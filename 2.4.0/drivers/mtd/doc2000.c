
/*
 * Linux driver for Disk-On-Chip 2000 and Millennium
 * (c) 1999 Machine Vision Holdings, Inc.
 * (c) 1999, 2000 David Woodhouse <dwmw2@infradead.org>
 *
 * $Id: doc2000.c,v 1.39 2000/12/01 17:34:29 dwmw2 Exp $
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/errno.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/malloc.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/types.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/nand_ids.h>
#include <linux/mtd/doc2000.h>

#define DOC_SUPPORT_2000
#define DOC_SUPPORT_MILLENNIUM

#ifdef DOC_SUPPORT_2000
#define DoC_is_2000(doc) (doc->ChipID == DOC_ChipID_Doc2k)
#else
#define DoC_is_2000(doc) (0)
#endif

#ifdef DOC_SUPPORT_MILLENNIUM
#define DoC_is_Millennium(doc) (doc->ChipID == DOC_ChipID_DocMil)
#else
#define DoC_is_Millennium(doc) (0)
#endif

/* #define ECC_DEBUG */

/* I have no idea why some DoC chips can not use memcpy_from|to_io().
 * This may be due to the different revisions of the ASIC controller built-in or
 * simplily a QA/Bug issue. Who knows ?? If you have trouble, please uncomment
 * this:
 #undef USE_MEMCPY
*/

static int doc_read(struct mtd_info *mtd, loff_t from, size_t len,
		    size_t *retlen, u_char *buf);
static int doc_write(struct mtd_info *mtd, loff_t to, size_t len,
		     size_t *retlen, const u_char *buf);
static int doc_read_ecc(struct mtd_info *mtd, loff_t from, size_t len,
			size_t *retlen, u_char *buf, u_char *eccbuf);
static int doc_write_ecc(struct mtd_info *mtd, loff_t to, size_t len,
			 size_t *retlen, const u_char *buf, u_char *eccbuf);
static int doc_read_oob(struct mtd_info *mtd, loff_t ofs, size_t len,
			size_t *retlen, u_char *buf);
static int doc_write_oob(struct mtd_info *mtd, loff_t ofs, size_t len,
			 size_t *retlen, const u_char *buf);
static int doc_erase (struct mtd_info *mtd, struct erase_info *instr);

static struct mtd_info *doc2klist = NULL;

/* Perform the required delay cycles by reading from the appropriate register */
static void DoC_Delay(struct DiskOnChip *doc, unsigned short cycles)
{
	volatile char dummy;
	int i;
	
	for (i = 0; i < cycles; i++) {
		if (DoC_is_Millennium(doc))
			dummy = ReadDOC(doc->virtadr, NOP);
		else
			dummy = ReadDOC(doc->virtadr, DOCStatus);
	}
	
}

/* DOC_WaitReady: Wait for RDY line to be asserted by the flash chip */
static int _DoC_WaitReady(struct DiskOnChip *doc)
{
	unsigned long docptr = doc->virtadr;
	unsigned short c = 0xffff;

	DEBUG(MTD_DEBUG_LEVEL3,
	      "_DoC_WaitReady called for out-of-line wait\n");

	/* Out-of-line routine to wait for chip response */
	while (!(ReadDOC(docptr, CDSNControl) & CDSN_CTRL_FR_B) && --c)
		;

	if (c == 0)
		DEBUG(MTD_DEBUG_LEVEL2, "_DoC_WaitReady timed out.\n");

	return (c == 0);
}

static inline int DoC_WaitReady(struct DiskOnChip *doc)
{
	unsigned long docptr = doc->virtadr;
	/* This is inline, to optimise the common case, where it's ready instantly */
	int ret = 0;

	/* 4 read form NOP register should be issued in prior to the read from CDSNControl
	   see Software Requirement 11.4 item 2. */
	DoC_Delay(doc, 4);

	if (!(ReadDOC(docptr, CDSNControl) & CDSN_CTRL_FR_B))
		/* Call the out-of-line routine to wait */
		ret = _DoC_WaitReady(doc);

	/* issue 2 read from NOP register after reading from CDSNControl register
	   see Software Requirement 11.4 item 2. */
	DoC_Delay(doc, 2);

	return ret;
}

/* DoC_Command: Send a flash command to the flash chip through the CDSN Slow IO register to
   bypass the internal pipeline. Each of 4 delay cycles (read from the NOP register) is
   required after writing to CDSN Control register, see Software Requirement 11.4 item 3. */

static inline int DoC_Command(struct DiskOnChip *doc, unsigned char command,
			      unsigned char xtraflags)
{
	unsigned long docptr = doc->virtadr;

	if (DoC_is_2000(doc))
		xtraflags |= CDSN_CTRL_FLASH_IO;

	/* Assert the CLE (Command Latch Enable) line to the flash chip */
	WriteDOC(xtraflags | CDSN_CTRL_CLE | CDSN_CTRL_CE, docptr, CDSNControl);
	DoC_Delay(doc, 4);	/* Software requirement 11.4.3 for Millennium */

	if (DoC_is_Millennium(doc))
		WriteDOC(command, docptr, CDSNSlowIO);

	/* Send the command */
	WriteDOC_(command, docptr, doc->ioreg);

	/* Lower the CLE line */
	WriteDOC(xtraflags | CDSN_CTRL_CE, docptr, CDSNControl);
	DoC_Delay(doc, 4);	/* Software requirement 11.4.3 for Millennium */

	/* Wait for the chip to respond - Software requirement 11.4.1 (extended for any command) */
	return DoC_WaitReady(doc);
}

/* DoC_Address: Set the current address for the flash chip through the CDSN Slow IO register to
   bypass the internal pipeline. Each of 4 delay cycles (read from the NOP register) is
   required after writing to CDSN Control register, see Software Requirement 11.4 item 3. */

static int DoC_Address(struct DiskOnChip *doc, int numbytes, unsigned long ofs,
		       unsigned char xtraflags1, unsigned char xtraflags2)
{
	unsigned long docptr;
	int i;

	docptr = doc->virtadr;

	if (DoC_is_2000(doc))
		xtraflags1 |= CDSN_CTRL_FLASH_IO;

	/* Assert the ALE (Address Latch Enable) line to the flash chip */
	WriteDOC(xtraflags1 | CDSN_CTRL_ALE | CDSN_CTRL_CE, docptr, CDSNControl);

	DoC_Delay(doc, 4);	/* Software requirement 11.4.3 for Millennium */

	/* Send the address */
	/* Devices with 256-byte page are addressed as:
	   Column (bits 0-7), Page (bits 8-15, 16-23, 24-31)
	   * there is no device on the market with page256
	   and more than 24 bits.
	   Devices with 512-byte page are addressed as:
	   Column (bits 0-7), Page (bits 9-16, 17-24, 25-31)
	   * 25-31 is sent only if the chip support it.
	   * bit 8 changes the read command to be sent
	   (NAND_CMD_READ0 or NAND_CMD_READ1).
	 */

	if (numbytes == ADDR_COLUMN || numbytes == ADDR_COLUMN_PAGE) {
		if (DoC_is_Millennium(doc))
			WriteDOC(ofs & 0xff, docptr, CDSNSlowIO);
		WriteDOC_(ofs & 0xff, docptr, doc->ioreg);
	}

	if (doc->page256) {
		ofs = ofs >> 8;
	} else {
		ofs = ofs >> 9;
	}

	if (numbytes == ADDR_PAGE || numbytes == ADDR_COLUMN_PAGE) {
		for (i = 0; i < doc->pageadrlen; i++, ofs = ofs >> 8) {
			if (DoC_is_Millennium(doc))
				WriteDOC(ofs & 0xff, docptr, CDSNSlowIO);
			WriteDOC_(ofs & 0xff, docptr, doc->ioreg);
		}
	}

	DoC_Delay(doc, 2);	/* Needed for some slow flash chips. mf. */
	
	/* FIXME: The SlowIO's for millennium could be replaced by 
	   a single WritePipeTerm here. mf. */

	/* Lower the ALE line */
	WriteDOC(xtraflags1 | xtraflags2 | CDSN_CTRL_CE, docptr,
		 CDSNControl);

	DoC_Delay(doc, 4);	/* Software requirement 11.4.3 for Millennium */

	/* Wait for the chip to respond - Software requirement 11.4.1 */
	return DoC_WaitReady(doc);
}

/* Read a buffer from DoC, taking care of Millennium odditys */
static void DoC_ReadBuf(struct DiskOnChip *doc, u_char * buf, int len)
{
	int dummy;
	int modulus = 0xffff;
	unsigned long docptr;
	int i;

	docptr = doc->virtadr;

	if (len <= 0)
		return;

	if (DoC_is_Millennium(doc)) {
		/* Read the data via the internal pipeline through CDSN IO register,
		   see Pipelined Read Operations 11.3 */
		dummy = ReadDOC(docptr, ReadPipeInit);

		/* Millennium should use the LastDataRead register - Pipeline Reads */
		len--;

		/* This is needed for correctly ECC calculation */
		modulus = 0xff;
	}

	for (i = 0; i < len; i++)
		buf[i] = ReadDOC_(docptr, doc->ioreg + (i & modulus));

	if (DoC_is_Millennium(doc)) {
		buf[i] = ReadDOC(docptr, LastDataRead);
	}
}

/* Write a buffer to DoC, taking care of Millennium odditys */
static void DoC_WriteBuf(struct DiskOnChip *doc, const u_char * buf, int len)
{
	unsigned long docptr;
	int i;

	docptr = doc->virtadr;

	if (len <= 0)
		return;

	for (i = 0; i < len; i++)
		WriteDOC_(buf[i], docptr, doc->ioreg + i);

	if (DoC_is_Millennium(doc)) {
		WriteDOC(0x00, docptr, WritePipeTerm);
	}
}


/* DoC_SelectChip: Select a given flash chip within the current floor */

static inline int DoC_SelectChip(struct DiskOnChip *doc, int chip)
{
	unsigned long docptr = doc->virtadr;

	/* Software requirement 11.4.4 before writing DeviceSelect */
	/* Deassert the CE line to eliminate glitches on the FCE# outputs */
	WriteDOC(CDSN_CTRL_WP, docptr, CDSNControl);
	DoC_Delay(doc, 4);	/* Software requirement 11.4.3 for Millennium */

	/* Select the individual flash chip requested */
	WriteDOC(chip, docptr, CDSNDeviceSelect);
	DoC_Delay(doc, 4);

	/* Reassert the CE line */
	WriteDOC(CDSN_CTRL_CE | CDSN_CTRL_FLASH_IO | CDSN_CTRL_WP, docptr,
		 CDSNControl);
	DoC_Delay(doc, 4);	/* Software requirement 11.4.3 for Millennium */

	/* Wait for it to be ready */
	return DoC_WaitReady(doc);
}

/* DoC_SelectFloor: Select a given floor (bank of flash chips) */

static inline int DoC_SelectFloor(struct DiskOnChip *doc, int floor)
{
	unsigned long docptr = doc->virtadr;

	/* Select the floor (bank) of chips required */
	WriteDOC(floor, docptr, FloorSelect);

	/* Wait for the chip to be ready */
	return DoC_WaitReady(doc);
}

/* DoC_IdentChip: Identify a given NAND chip given {floor,chip} */

static int DoC_IdentChip(struct DiskOnChip *doc, int floor, int chip)
{
	int mfr, id, i;
	volatile char dummy;

	/* Page in the required floor/chip */
	DoC_SelectFloor(doc, floor);
	DoC_SelectChip(doc, chip);

	/* Reset the chip */
	if (DoC_Command(doc, NAND_CMD_RESET, CDSN_CTRL_WP)) {
		DEBUG(MTD_DEBUG_LEVEL2,
		      "DoC_Command (reset) for %d,%d returned true\n",
		      floor, chip);
		return 0;
	}


	/* Read the NAND chip ID: 1. Send ReadID command */
	if (DoC_Command(doc, NAND_CMD_READID, CDSN_CTRL_WP)) {
		DEBUG(MTD_DEBUG_LEVEL2,
		      "DoC_Command (ReadID) for %d,%d returned true\n",
		      floor, chip);
		return 0;
	}

	/* Read the NAND chip ID: 2. Send address byte zero */
	DoC_Address(doc, ADDR_COLUMN, 0, CDSN_CTRL_WP, 0);

	/* Read the manufacturer and device id codes from the device */

	/* CDSN Slow IO register see Software Requirement 11.4 item 5. */
	dummy = ReadDOC(doc->virtadr, CDSNSlowIO);
	DoC_Delay(doc, 2);
	mfr = ReadDOC_(doc->virtadr, doc->ioreg);

	/* CDSN Slow IO register see Software Requirement 11.4 item 5. */
	dummy = ReadDOC(doc->virtadr, CDSNSlowIO);
	DoC_Delay(doc, 2);
	id = ReadDOC_(doc->virtadr, doc->ioreg);

	/* No response - return failure */
	if (mfr == 0xff || mfr == 0)
		return 0;

	/* Check it's the same as the first chip we identified. 
	 * M-Systems say that any given DiskOnChip device should only
	 * contain _one_ type of flash part, although that's not a 
	 * hardware restriction. */
	if (doc->mfr) {
		if (doc->mfr == mfr && doc->id == id)
			return 1;	/* This is another the same the first */
		else
			printk(KERN_WARNING
			       "Flash chip at floor %d, chip %d is different:\n",
			       floor, chip);
	}

	/* Print and store the manufacturer and ID codes. */
	for (i = 0; nand_flash_ids[i].name != NULL; i++) {
		if (mfr == nand_flash_ids[i].manufacture_id &&
		    id == nand_flash_ids[i].model_id) {
			printk(KERN_INFO
			       "Flash chip found: Manufacturer ID: %2.2X, "
			       "Chip ID: %2.2X (%s)\n", mfr, id,
			       nand_flash_ids[i].name);
			if (!doc->mfr) {
				doc->mfr = mfr;
				doc->id = id;
				doc->chipshift =
				    nand_flash_ids[i].chipshift;
				doc->page256 = nand_flash_ids[i].page256;
				doc->pageadrlen =
				    nand_flash_ids[i].pageadrlen;
				doc->erasesize =
				    nand_flash_ids[i].erasesize;
				return 1;
			}
			return 0;
		}
	}


	/* We haven't fully identified the chip. Print as much as we know. */
	printk(KERN_WARNING "Unknown flash chip found: %2.2X %2.2X\n",
	       id, mfr);

	printk(KERN_WARNING "Please report to dwmw2@infradead.org\n");
	return 0;
}

/* DoC_ScanChips: Find all NAND chips present in a DiskOnChip, and identify them */

static void DoC_ScanChips(struct DiskOnChip *this)
{
	int floor, chip;
	int numchips[MAX_FLOORS];
	int maxchips = MAX_CHIPS;
	int ret = 1;

	this->numchips = 0;
	this->mfr = 0;
	this->id = 0;

	if (DoC_is_Millennium(this))
		maxchips = MAX_CHIPS_MIL;

	/* For each floor, find the number of valid chips it contains */
	for (floor = 0; floor < MAX_FLOORS; floor++) {
		ret = 1;
		numchips[floor] = 0;
		for (chip = 0; chip < maxchips && ret != 0; chip++) {

			ret = DoC_IdentChip(this, floor, chip);
			if (ret) {
				numchips[floor]++;
				this->numchips++;
			}
		}
	}

	/* If there are none at all that we recognise, bail */
	if (!this->numchips) {
		printk("No flash chips recognised.\n");
		return;
	}

	/* Allocate an array to hold the information for each chip */
	this->chips = kmalloc(sizeof(struct Nand) * this->numchips, GFP_KERNEL);
	if (!this->chips) {
		printk("No memory for allocating chip info structures\n");
		return;
	}

	ret = 0;

	/* Fill out the chip array with {floor, chipno} for each 
	 * detected chip in the device. */
	for (floor = 0; floor < MAX_FLOORS; floor++) {
		for (chip = 0; chip < numchips[floor]; chip++) {
			this->chips[ret].floor = floor;
			this->chips[ret].chip = chip;
			this->chips[ret].curadr = 0;
			this->chips[ret].curmode = 0x50;
			ret++;
		}
	}

	/* Calculate and print the total size of the device */
	this->totlen = this->numchips * (1 << this->chipshift);

	printk(KERN_INFO
	       "%d flash chips found. Total DiskOnChip size: %ld Mb\n",
	       this->numchips, this->totlen >> 20);
}


static int DoC2k_is_alias(struct DiskOnChip *doc1, struct DiskOnChip *doc2)
{
	int tmp1, tmp2, retval;
	if (doc1->physadr == doc2->physadr)
		return 1;

	/* Use the alias resolution register which was set aside for this
	 * purpose. If it's value is the same on both chips, they might
	 * be the same chip, and we write to one and check for a change in
	 * the other. It's unclear if this register is usuable in the
	 * DoC 2000 (it's in the Millennium docs), but it seems to work. */
	tmp1 = ReadDOC(doc1->virtadr, AliasResolution);
	tmp2 = ReadDOC(doc2->virtadr, AliasResolution);
	if (tmp1 != tmp2)
		return 0;

	WriteDOC((tmp1 + 1) % 0xff, doc1->virtadr, AliasResolution);
	tmp2 = ReadDOC(doc2->virtadr, AliasResolution);
	if (tmp2 == (tmp1 + 1) % 0xff)
		retval = 1;
	else
		retval = 0;

	/* Restore register contents.  May not be necessary, but do it just to
	 * be safe. */
	WriteDOC(tmp1, doc1->virtadr, AliasResolution);

	return retval;
}

static const char im_name[] = "DoC2k_init";

/* This routine is made available to other mtd code via
 * inter_module_register.  It must only be accessed through
 * inter_module_get which will bump the use count of this module.  The
 * addresses passed back in mtd are valid as long as the use count of
 * this module is non-zero, i.e. between inter_module_get and
 * inter_module_put.  Keith Owens <kaos@ocs.com.au> 29 Oct 2000.
 */
static void DoC2k_init(struct mtd_info *mtd)
{
	struct DiskOnChip *this = (struct DiskOnChip *) mtd->priv;
	struct DiskOnChip *old = NULL;

	/* We must avoid being called twice for the same device. */

	if (doc2klist)
		old = (struct DiskOnChip *) doc2klist->priv;

	while (old) {
		if (DoC2k_is_alias(old, this)) {
			printk(KERN_NOTICE
			       "Ignoring DiskOnChip 2000 at 0x%lX - already configured\n",
			       this->physadr);
			iounmap((void *) this->virtadr);
			kfree(mtd);
			return;
		}
		if (old->nextdoc)
			old = (struct DiskOnChip *) old->nextdoc->priv;
		else
			old = NULL;
	}


	switch (this->ChipID) {
	case DOC_ChipID_Doc2k:
		mtd->name = "DiskOnChip 2000";
		this->ioreg = DoC_2k_CDSN_IO;
		break;
	case DOC_ChipID_DocMil:
		mtd->name = "DiskOnChip Millennium";
		this->ioreg = DoC_Mil_CDSN_IO;
		break;
	}

	printk(KERN_NOTICE "%s found at address 0x%lX\n", mtd->name,
	       this->physadr);

	mtd->type = MTD_NANDFLASH;
	mtd->flags = MTD_CAP_NANDFLASH;
	mtd->size = 0;
	mtd->erasesize = 0;
	mtd->oobblock = 512;
	mtd->oobsize = 16;
	mtd->module = THIS_MODULE;
	mtd->erase = doc_erase;
	mtd->point = NULL;
	mtd->unpoint = NULL;
	mtd->read = doc_read;
	mtd->write = doc_write;
	mtd->read_ecc = doc_read_ecc;
	mtd->write_ecc = doc_write_ecc;
	mtd->read_oob = doc_read_oob;
	mtd->write_oob = doc_write_oob;
	mtd->sync = NULL;

	this->totlen = 0;
	this->numchips = 0;

	this->curfloor = -1;
	this->curchip = -1;

	/* Ident all the chips present. */
	DoC_ScanChips(this);

	if (!this->totlen) {
		kfree(mtd);
		iounmap((void *) this->virtadr);
	} else {
		this->nextdoc = doc2klist;
		doc2klist = mtd;
		mtd->size = this->totlen;
		mtd->erasesize = this->erasesize;
		add_mtd_device(mtd);
		return;
	}
}

static int doc_read(struct mtd_info *mtd, loff_t from, size_t len,
		    size_t * retlen, u_char * buf)
{
	/* Just a special case of doc_read_ecc */
	return doc_read_ecc(mtd, from, len, retlen, buf, NULL);
}

static int doc_read_ecc(struct mtd_info *mtd, loff_t from, size_t len,
			size_t * retlen, u_char * buf, u_char * eccbuf)
{
	struct DiskOnChip *this = (struct DiskOnChip *) mtd->priv;
	unsigned long docptr;
	struct Nand *mychip;
	unsigned char syndrome[6];
	volatile char dummy;
	int i, len256 = 0, ret=0;

	docptr = this->virtadr;

	/* Don't allow read past end of device */
	if (from >= this->totlen)
		return -EINVAL;

	/* Don't allow a single read to cross a 512-byte block boundary */
	if (from + len > ((from | 0x1ff) + 1))
		len = ((from | 0x1ff) + 1) - from;

	/* The ECC will not be calculated correctly if less than 512 is read */
	if (len != 0x200 && eccbuf)
		printk(KERN_WARNING
		       "ECC needs a full sector read (adr: %lx size %lx)\n",
		       (long) from, (long) len);

	/* printk("DoC_Read (adr: %lx size %lx)\n", (long) from, (long) len); */


	/* Find the chip which is to be used and select it */
	mychip = &this->chips[from >> (this->chipshift)];

	if (this->curfloor != mychip->floor) {
		DoC_SelectFloor(this, mychip->floor);
		DoC_SelectChip(this, mychip->chip);
	} else if (this->curchip != mychip->chip) {
		DoC_SelectChip(this, mychip->chip);
	}

	this->curfloor = mychip->floor;
	this->curchip = mychip->chip;

	DoC_Command(this,
		    (!this->page256
		     && (from & 0x100)) ? NAND_CMD_READ1 : NAND_CMD_READ0,
		    CDSN_CTRL_WP);
	DoC_Address(this, ADDR_COLUMN_PAGE, from, CDSN_CTRL_WP,
		    CDSN_CTRL_ECC_IO);

	if (eccbuf) {
		/* Prime the ECC engine */
		WriteDOC(DOC_ECC_RESET, docptr, ECCConf);
		WriteDOC(DOC_ECC_EN, docptr, ECCConf);
	} else {
		/* disable the ECC engine */
		WriteDOC(DOC_ECC_RESET, docptr, ECCConf);
		WriteDOC(DOC_ECC_DIS, docptr, ECCConf);
	}

	/* treat crossing 256-byte sector for 2M x 8bits devices */
	if (this->page256 && from + len > (from | 0xff) + 1) {
		len256 = (from | 0xff) + 1 - from;
		DoC_ReadBuf(this, buf, len256);

		DoC_Command(this, NAND_CMD_READ0, CDSN_CTRL_WP);
		DoC_Address(this, ADDR_COLUMN_PAGE, from + len256,
			    CDSN_CTRL_WP, CDSN_CTRL_ECC_IO);
	}

	DoC_ReadBuf(this, &buf[len256], len - len256);

	/* Let the caller know we completed it */
	*retlen = len;

	if (eccbuf) {
		/* Read the ECC data through the DiskOnChip ECC logic */
		/* Note: this will work even with 2M x 8bit devices as   */
		/*       they have 8 bytes of OOB per 256 page. mf.      */
		DoC_ReadBuf(this, eccbuf, 6);

		/* Flush the pipeline */
		if (DoC_is_Millennium(this)) {
			dummy = ReadDOC(docptr, ECCConf);
			dummy = ReadDOC(docptr, ECCConf);
			i = ReadDOC(docptr, ECCConf);
		} else {
			dummy = ReadDOC(docptr, 2k_ECCStatus);
			dummy = ReadDOC(docptr, 2k_ECCStatus);
			i = ReadDOC(docptr, 2k_ECCStatus);
		}

		/* Check the ECC Status */
		if (i & 0x80) {
			int nb_errors;
			/* There was an ECC error */
#ifdef ECC_DEBUG
			printk("DiskOnChip ECC Error: Read at %lx\n", (long)from);
#endif
			/* Read the ECC syndrom through the DiskOnChip ECC logic.
			   These syndrome will be all ZERO when there is no error */
			for (i = 0; i < 6; i++) {
				syndrome[i] =
				    ReadDOC(docptr, ECCSyndrome0 + i);
			}
                        nb_errors = doc_decode_ecc(buf, syndrome);

#ifdef ECC_DEBUG
			printk("Errors corrected: %x\n", nb_errors);
#endif
                        if (nb_errors < 0) {
				/* We return error, but have actually done the read. Not that
				   this can be told to user-space, via sys_read(), but at least
				   MTD-aware stuff can know about it by checking *retlen */
				ret = -EIO;
                        }
		}

#ifdef PSYCHO_DEBUG
		printk("ECC DATA at %lxB: %2.2X %2.2X %2.2X %2.2X %2.2X %2.2X\n",
			     (long)from, eccbuf[0], eccbuf[1], eccbuf[2],
			     eccbuf[3], eccbuf[4], eccbuf[5]);
#endif
		
		/* disable the ECC engine */
		WriteDOC(DOC_ECC_DIS, docptr , ECCConf);
	}

	return ret;
}

static int doc_write(struct mtd_info *mtd, loff_t to, size_t len,
		     size_t * retlen, const u_char * buf)
{
	char eccbuf[6];
	return doc_write_ecc(mtd, to, len, retlen, buf, eccbuf);
}

static int doc_write_ecc(struct mtd_info *mtd, loff_t to, size_t len,
			 size_t * retlen, const u_char * buf,
			 u_char * eccbuf)
{
	struct DiskOnChip *this = (struct DiskOnChip *) mtd->priv;
	int di; /* Yes, DI is a hangover from when I was disassembling the binary driver */
	unsigned long docptr;
	volatile char dummy;
	int len256 = 0;
	struct Nand *mychip;

	docptr = this->virtadr;

	/* Don't allow write past end of device */
	if (to >= this->totlen)
		return -EINVAL;

	/* Don't allow a single write to cross a 512-byte block boundary */
	if (to + len > ((to | 0x1ff) + 1))
		len = ((to | 0x1ff) + 1) - to;

	/* The ECC will not be calculated correctly if less than 512 is written */
	if (len != 0x200 && eccbuf)
		printk(KERN_WARNING
		       "ECC needs a full sector write (adr: %lx size %lx)\n",
		       (long) to, (long) len);

	/* printk("DoC_Write (adr: %lx size %lx)\n", (long) to, (long) len); */

	/* Find the chip which is to be used and select it */
	mychip = &this->chips[to >> (this->chipshift)];

	if (this->curfloor != mychip->floor) {
		DoC_SelectFloor(this, mychip->floor);
		DoC_SelectChip(this, mychip->chip);
	} else if (this->curchip != mychip->chip) {
		DoC_SelectChip(this, mychip->chip);
	}

	this->curfloor = mychip->floor;
	this->curchip = mychip->chip;

	/* Set device to main plane of flash */
	DoC_Command(this, NAND_CMD_RESET, CDSN_CTRL_WP);
	DoC_Command(this,
		    (!this->page256
		     && (to & 0x100)) ? NAND_CMD_READ1 : NAND_CMD_READ0,
		    CDSN_CTRL_WP);

	DoC_Command(this, NAND_CMD_SEQIN, 0);
	DoC_Address(this, ADDR_COLUMN_PAGE, to, 0, CDSN_CTRL_ECC_IO);

	if (eccbuf) {
		/* Prime the ECC engine */
		WriteDOC(DOC_ECC_RESET, docptr, ECCConf);
		WriteDOC(DOC_ECC_EN | DOC_ECC_RW, docptr, ECCConf);
	} else {
		/* disable the ECC engine */
		WriteDOC(DOC_ECC_RESET, docptr, ECCConf);
		WriteDOC(DOC_ECC_DIS, docptr, ECCConf);
	}

	/* treat crossing 256-byte sector for 2M x 8bits devices */
	if (this->page256 && to + len > (to | 0xff) + 1) {
		len256 = (to | 0xff) + 1 - to;
		DoC_WriteBuf(this, buf, len256);

		DoC_Command(this, NAND_CMD_PAGEPROG, 0);

		DoC_Command(this, NAND_CMD_STATUS, CDSN_CTRL_WP);
		/* There's an implicit DoC_WaitReady() in DoC_Command */

		dummy = ReadDOC(docptr, CDSNSlowIO);
		DoC_Delay(this, 2);

		if (ReadDOC_(docptr, this->ioreg) & 1) {
			printk("Error programming flash\n");
			/* Error in programming */
			*retlen = 0;
			return -EIO;
		}

		DoC_Command(this, NAND_CMD_SEQIN, 0);
		DoC_Address(this, ADDR_COLUMN_PAGE, to + len256, 0,
			    CDSN_CTRL_ECC_IO);
	}

	DoC_WriteBuf(this, &buf[len256], len - len256);

	if (eccbuf) {
		WriteDOC(CDSN_CTRL_ECC_IO | CDSN_CTRL_CE, docptr,
			 CDSNControl);

		if (DoC_is_Millennium(this)) {
			WriteDOC(0, docptr, NOP);
			WriteDOC(0, docptr, NOP);
			WriteDOC(0, docptr, NOP);
		} else {
			WriteDOC_(0, docptr, this->ioreg);
			WriteDOC_(0, docptr, this->ioreg);
			WriteDOC_(0, docptr, this->ioreg);
		}

		/* Read the ECC data through the DiskOnChip ECC logic */
		for (di = 0; di < 6; di++) {
			eccbuf[di] = ReadDOC(docptr, ECCSyndrome0 + di);
		}

		/* Reset the ECC engine */
		WriteDOC(DOC_ECC_DIS, docptr, ECCConf);

#ifdef PSYCHO_DEBUG
		printk
		    ("OOB data at %lx is %2.2X %2.2X %2.2X %2.2X %2.2X %2.2X\n",
		     (long) to, eccbuf[0], eccbuf[1], eccbuf[2], eccbuf[3],
		     eccbuf[4], eccbuf[5]);
#endif
	}

	DoC_Command(this, NAND_CMD_PAGEPROG, 0);

	DoC_Command(this, NAND_CMD_STATUS, CDSN_CTRL_WP);
	/* There's an implicit DoC_WaitReady() in DoC_Command */

	dummy = ReadDOC(docptr, CDSNSlowIO);
	DoC_Delay(this, 2);

	if (ReadDOC_(docptr, this->ioreg) & 1) {
		printk("Error programming flash\n");
		/* Error in programming */
		*retlen = 0;
		return -EIO;
	}

	/* Let the caller know we completed it */
	*retlen = len;
		
	if (eccbuf) {
		unsigned char x[8];
		size_t dummy;

		/* Write the ECC data to flash */
		for (di=0; di<6; di++)
			x[di] = eccbuf[di];
		
		x[6]=0x55;
		x[7]=0x55;
		
		return doc_write_oob(mtd, to, 8, &dummy, x);
	}

	return 0;
}

static int doc_read_oob(struct mtd_info *mtd, loff_t ofs, size_t len,
			size_t * retlen, u_char * buf)
{
	struct DiskOnChip *this = (struct DiskOnChip *) mtd->priv;
	int len256 = 0;
	unsigned long docptr;
	struct Nand *mychip;

	docptr = this->virtadr;

	mychip = &this->chips[ofs >> this->chipshift];

	if (this->curfloor != mychip->floor) {
		DoC_SelectFloor(this, mychip->floor);
		DoC_SelectChip(this, mychip->chip);
	} else if (this->curchip != mychip->chip) {
		DoC_SelectChip(this, mychip->chip);
	}
	this->curfloor = mychip->floor;
	this->curchip = mychip->chip;

	/* update address for 2M x 8bit devices. OOB starts on the second */
	/* page to maintain compatibility with doc_read_ecc. */
	if (this->page256) {
		if (!(ofs & 0x8))
			ofs += 0x100;
		else
			ofs -= 0x8;
	}

	DoC_Command(this, NAND_CMD_READOOB, CDSN_CTRL_WP);
	DoC_Address(this, ADDR_COLUMN_PAGE, ofs, CDSN_CTRL_WP, 0);

	/* treat crossing 8-byte OOB data for 2M x 8bit devices */
	/* Note: datasheet says it should automaticaly wrap to the */
	/*       next OOB block, but it didn't work here. mf.      */
	if (this->page256 && ofs + len > (ofs | 0x7) + 1) {
		len256 = (ofs | 0x7) + 1 - ofs;
		DoC_ReadBuf(this, buf, len256);

		DoC_Command(this, NAND_CMD_READOOB, CDSN_CTRL_WP);
		DoC_Address(this, ADDR_COLUMN_PAGE, ofs & (~0x1ff),
			    CDSN_CTRL_WP, 0);
	}

	DoC_ReadBuf(this, &buf[len256], len - len256);

	*retlen = len;
	return 0;

}

static int doc_write_oob(struct mtd_info *mtd, loff_t ofs, size_t len,
			 size_t * retlen, const u_char * buf)
{
	struct DiskOnChip *this = (struct DiskOnChip *) mtd->priv;
	int len256 = 0;
	unsigned long docptr = this->virtadr;
	struct Nand *mychip = &this->chips[ofs >> this->chipshift];
	int dummy;

	//      printk("doc_write_oob(%lx, %d): %2.2X %2.2X %2.2X %2.2X ... %2.2X %2.2X .. %2.2X %2.2X\n",(long)ofs, len,
	//   buf[0], buf[1], buf[2], buf[3], buf[8], buf[9], buf[14],buf[15]);

	/* Find the chip which is to be used and select it */
	if (this->curfloor != mychip->floor) {
		DoC_SelectFloor(this, mychip->floor);
		DoC_SelectChip(this, mychip->chip);
	} else if (this->curchip != mychip->chip) {
		DoC_SelectChip(this, mychip->chip);
	}
	this->curfloor = mychip->floor;
	this->curchip = mychip->chip;

	/* disable the ECC engine */
	WriteDOC (DOC_ECC_RESET, docptr, ECCConf);
	WriteDOC (DOC_ECC_DIS, docptr, ECCConf);

	/* Reset the chip, see Software Requirement 11.4 item 1. */
	DoC_Command(this, NAND_CMD_RESET, CDSN_CTRL_WP);

	/* issue the Read2 command to set the pointer to the Spare Data Area. */
	DoC_Command(this, NAND_CMD_READOOB, CDSN_CTRL_WP);

	/* update address for 2M x 8bit devices. OOB starts on the second */
	/* page to maintain compatibility with doc_read_ecc. */
	if (this->page256) {
		if (!(ofs & 0x8))
			ofs += 0x100;
		else
			ofs -= 0x8;
	}

	/* issue the Serial Data In command to initial the Page Program process */
	DoC_Command(this, NAND_CMD_SEQIN, 0);
	DoC_Address(this, ADDR_COLUMN_PAGE, ofs, 0, 0);

	/* treat crossing 8-byte OOB data for 2M x 8bit devices */
	/* Note: datasheet says it should automaticaly wrap to the */
	/*       next OOB block, but it didn't work here. mf.      */
	if (this->page256 && ofs + len > (ofs | 0x7) + 1) {
		len256 = (ofs | 0x7) + 1 - ofs;
		DoC_WriteBuf(this, buf, len256);

		DoC_Command(this, NAND_CMD_PAGEPROG, 0);
		DoC_Command(this, NAND_CMD_STATUS, 0);
		/* DoC_WaitReady() is implicit in DoC_Command */

		dummy = ReadDOC(docptr, CDSNSlowIO);
		DoC_Delay(this, 2);

		if (ReadDOC_(docptr, this->ioreg) & 1) {
			printk("Error programming oob data\n");
			/* There was an error */
			*retlen = 0;
			return -EIO;
		}
		DoC_Command(this, NAND_CMD_SEQIN, 0);
		DoC_Address(this, ADDR_COLUMN_PAGE, ofs & (~0x1ff), 0, 0);
	}

	DoC_WriteBuf(this, &buf[len256], len - len256);

	DoC_Command(this, NAND_CMD_PAGEPROG, 0);
	DoC_Command(this, NAND_CMD_STATUS, 0);
	/* DoC_WaitReady() is implicit in DoC_Command */

	dummy = ReadDOC(docptr, CDSNSlowIO);
	DoC_Delay(this, 2);

	if (ReadDOC_(docptr, this->ioreg) & 1) {
		printk("Error programming oob data\n");
		/* There was an error */
		*retlen = 0;
		return -EIO;
	}

	*retlen = len;
	return 0;

}

int doc_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct DiskOnChip *this = (struct DiskOnChip *) mtd->priv;
	unsigned long ofs = instr->addr;
	unsigned long len = instr->len;
	unsigned long docptr;
	struct Nand *mychip;

	if (len != mtd->erasesize)
		printk(KERN_WARNING "Erase not right size (%lx != %lx)n",
		       len, mtd->erasesize);

	docptr = this->virtadr;

	mychip = &this->chips[ofs >> this->chipshift];

	if (this->curfloor != mychip->floor) {
		DoC_SelectFloor(this, mychip->floor);
		DoC_SelectChip(this, mychip->chip);
	} else if (this->curchip != mychip->chip) {
		DoC_SelectChip(this, mychip->chip);
	}
	this->curfloor = mychip->floor;
	this->curchip = mychip->chip;

	instr->state = MTD_ERASE_PENDING;

	DoC_Command(this, NAND_CMD_ERASE1, 0);
	DoC_Address(this, ADDR_PAGE, ofs, 0, 0);
	DoC_Command(this, NAND_CMD_ERASE2, 0);

	instr->state = MTD_ERASING;

	DoC_Command(this, NAND_CMD_STATUS, CDSN_CTRL_WP);

	if (ReadDOC_(docptr, this->ioreg) & 1) {
		printk("Error writing\n");
		/* There was an error */
		instr->state = MTD_ERASE_FAILED;
	} else
		instr->state = MTD_ERASE_DONE;

	if (instr->callback)
		instr->callback(instr);

	return 0;
}


/****************************************************************************
 *
 * Module stuff
 *
 ****************************************************************************/

#if LINUX_VERSION_CODE < 0x20212 && defined(MODULE)
#define cleanup_doc2000 cleanup_module
#define init_doc2000 init_module
#endif

int __init init_doc2000(void)
{
       inter_module_register(im_name, THIS_MODULE, &DoC2k_init);
       return 0;
}

static void __exit cleanup_doc2000(void)
{
	struct mtd_info *mtd;
	struct DiskOnChip *this;

	while ((mtd = doc2klist)) {
		this = (struct DiskOnChip *) mtd->priv;
		doc2klist = this->nextdoc;

		del_mtd_device(mtd);

		iounmap((void *) this->virtadr);
		kfree(this->chips);
		kfree(mtd);
	}
	inter_module_unregister(im_name);
}

module_exit(cleanup_doc2000);
module_init(init_doc2000);
