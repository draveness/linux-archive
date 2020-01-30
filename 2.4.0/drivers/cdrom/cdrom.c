/* linux/drivers/cdrom/cdrom.c. 
   Copyright (c) 1996, 1997 David A. van Leeuwen.
   Copyright (c) 1997, 1998 Erik Andersen <andersee@debian.org>
   Copyright (c) 1998, 1999 Jens Axboe <axboe@image.dk>

   May be copied or modified under the terms of the GNU General Public
   License.  See linux/COPYING for more information.

   Uniform CD-ROM driver for Linux.
   See Documentation/cdrom/cdrom-standard.tex for usage information.

   The routines in the file provide a uniform interface between the
   software that uses CD-ROMs and the various low-level drivers that
   actually talk to the hardware. Suggestions are welcome.
   Patches that work are more welcome though.  ;-)

 To Do List:
 ----------------------------------

 -- Modify sysctl/proc interface. I plan on having one directory per
 drive, with entries for outputing general drive information, and sysctl
 based tunable parameters such as whether the tray should auto-close for
 that drive. Suggestions (or patches) for this welcome!


 Revision History
 ----------------------------------
 1.00  Date Unknown -- David van Leeuwen <david@tm.tno.nl>
 -- Initial version by David A. van Leeuwen. I don't have a detailed
  changelog for the 1.x series, David?

2.00  Dec  2, 1997 -- Erik Andersen <andersee@debian.org>
  -- New maintainer! As David A. van Leeuwen has been too busy to activly
  maintain and improve this driver, I am now carrying on the torch. If
  you have a problem with this driver, please feel free to contact me.

  -- Added (rudimentary) sysctl interface. I realize this is really weak
  right now, and is _very_ badly implemented. It will be improved...

  -- Modified CDROM_DISC_STATUS so that it is now incorporated into
  the Uniform CD-ROM driver via the cdrom_count_tracks function.
  The cdrom_count_tracks function helps resolve some of the false
  assumptions of the CDROM_DISC_STATUS ioctl, and is also used to check
  for the correct media type when mounting or playing audio from a CD.

  -- Remove the calls to verify_area and only use the copy_from_user and
  copy_to_user stuff, since these calls now provide their own memory
  checking with the 2.1.x kernels.

  -- Major update to return codes so that errors from low-level drivers
  are passed on through (thanks to Gerd Knorr for pointing out this
  problem).

  -- Made it so if a function isn't implemented in a low-level driver,
  ENOSYS is now returned instead of EINVAL.

  -- Simplified some complex logic so that the source code is easier to read.

  -- Other stuff I probably forgot to mention (lots of changes).

2.01 to 2.11 Dec 1997-Jan 1998
  -- TO-DO!  Write changelogs for 2.01 to 2.12.

2.12  Jan  24, 1998 -- Erik Andersen <andersee@debian.org>
  -- Fixed a bug in the IOCTL_IN and IOCTL_OUT macros.  It turns out that
  copy_*_user does not return EFAULT on error, but instead returns the number 
  of bytes not copied.  I was returning whatever non-zero stuff came back from 
  the copy_*_user functions directly, which would result in strange errors.

2.13  July 17, 1998 -- Erik Andersen <andersee@debian.org>
  -- Fixed a bug in CDROM_SELECT_SPEED where you couldn't lower the speed
  of the drive.  Thanks to Tobias Ringstr|m <tori@prosolvia.se> for pointing
  this out and providing a simple fix.
  -- Fixed the procfs-unload-module bug with the fill_inode procfs callback.
  thanks to Andrea Arcangeli
  -- Fixed it so that the /proc entry now also shows up when cdrom is
  compiled into the kernel.  Before it only worked when loaded as a module.

  2.14 August 17, 1998 -- Erik Andersen <andersee@debian.org>
  -- Fixed a bug in cdrom_media_changed and handling of reporting that
  the media had changed for devices that _don't_ implement media_changed.  
  Thanks to Grant R. Guenther <grant@torque.net> for spotting this bug.
  -- Made a few things more pedanticly correct.

2.50 Oct 19, 1998 - Jens Axboe <axboe@image.dk>
  -- New maintainers! Erik was too busy to continue the work on the driver,
  so now Chris Zwilling <chris@cloudnet.com> and Jens Axboe <axboe@image.dk>
  will do their best to follow in his footsteps
  
  2.51 Dec 20, 1998 - Jens Axboe <axboe@image.dk>
  -- Check if drive is capable of doing what we ask before blindly changing
  cdi->options in various ioctl.
  -- Added version to proc entry.
  
  2.52 Jan 16, 1999 - Jens Axboe <axboe@image.dk>
  -- Fixed an error in open_for_data where we would sometimes not return
  the correct error value. Thanks Huba Gaspar <huba@softcell.hu>.
  -- Fixed module usage count - usage was based on /proc/sys/dev
  instead of /proc/sys/dev/cdrom. This could lead to an oops when other
  modules had entries in dev. Feb 02 - real bug was in sysctl.c where
  dev would be removed even though it was used. cdrom.c just illuminated
  that bug.
  
  2.53 Feb 22, 1999 - Jens Axboe <axboe@image.dk>
  -- Fixup of several ioctl calls, in particular CDROM_SET_OPTIONS has
  been "rewritten" because capabilities and options aren't in sync. They
  should be...
  -- Added CDROM_LOCKDOOR ioctl. Locks the door and keeps it that way.
  -- Added CDROM_RESET ioctl.
  -- Added CDROM_DEBUG ioctl. Enable debug messages on-the-fly.
  -- Added CDROM_GET_CAPABILITY ioctl. This relieves userspace programs
  from parsing /proc/sys/dev/cdrom/info.
  
  2.54 Mar 15, 1999 - Jens Axboe <axboe@image.dk>
  -- Check capability mask from low level driver when counting tracks as
  per suggestion from Corey J. Scotts <cstotts@blue.weeg.uiowa.edu>.
  
  2.55 Apr 25, 1999 - Jens Axboe <axboe@image.dk>
  -- autoclose was mistakenly checked against CDC_OPEN_TRAY instead of
  CDC_CLOSE_TRAY.
  -- proc info didn't mask against capabilities mask.
  
  3.00 Aug 5, 1999 - Jens Axboe <axboe@image.dk>
  -- Unified audio ioctl handling across CD-ROM drivers. A lot of the
  code was duplicated before. Drives that support the generic packet
  interface are now being fed packets from here instead.
  -- First attempt at adding support for MMC2 commands - for DVD and
  CD-R(W) drives. Only the DVD parts are in now - the interface used is
  the same as for the audio ioctls.
  -- ioctl cleanups. if a drive couldn't play audio, it didn't get
  a change to perform device specific ioctls as well.
  -- Defined CDROM_CAN(CDC_XXX) for checking the capabilities.
  -- Put in sysctl files for autoclose, autoeject, check_media, debug,
  and lock.
  -- /proc/sys/dev/cdrom/info has been updated to also contain info about
  CD-Rx and DVD capabilities.
  -- Now default to checking media type.
  -- CDROM_SEND_PACKET ioctl added. The infrastructure was in place for
  doing this anyway, with the generic_packet addition.
  
  3.01 Aug 6, 1999 - Jens Axboe <axboe@image.dk>
  -- Fix up the sysctl handling so that the option flags get set
  correctly.
  -- Fix up ioctl handling so the device specific ones actually get
  called :).
  
  3.02 Aug 8, 1999 - Jens Axboe <axboe@image.dk>
  -- Fixed volume control on SCSI drives (or others with longer audio
  page).
  -- Fixed a couple of DVD minors. Thanks to Andrew T. Veliath
  <andrewtv@usa.net> for telling me and for having defined the various
  DVD structures and ioctls in the first place! He designed the original
  DVD patches for ide-cd and while I rearranged and unified them, the
  interface is still the same.
  
  3.03 Sep 1, 1999 - Jens Axboe <axboe@image.dk>
  -- Moved the rest of the audio ioctls from the CD-ROM drivers here. Only
  CDROMREADTOCENTRY and CDROMREADTOCHDR are left.
  -- Moved the CDROMREADxxx ioctls in here.
  -- Defined the cdrom_get_last_written and cdrom_get_next_block as ioctls
  and exported functions.
  -- Erik Andersen <andersen@xmission.com> modified all SCMD_ commands
  to now read GPCMD_ for the new generic packet interface. All low level
  drivers are updated as well.
  -- Various other cleanups.

  3.04 Sep 12, 1999 - Jens Axboe <axboe@image.dk>
  -- Fixed a couple of possible memory leaks (if an operation failed and
  we didn't free the buffer before returning the error).
  -- Integrated Uniform CD Changer handling from Richard Sharman
  <rsharman@pobox.com>.
  -- Defined CD_DVD and CD_CHANGER log levels.
  -- Fixed the CDROMREADxxx ioctls.
  -- CDROMPLAYTRKIND uses the GPCMD_PLAY_AUDIO_MSF command - too few
  drives supported it. We loose the index part, however.
  -- Small modifications to accomodate opens of /dev/hdc1, required
  for ide-cd to handle multisession discs.
  -- Export cdrom_mode_sense and cdrom_mode_select.
  -- init_cdrom_command() for setting up a cgc command.
  
  3.05 Oct 24, 1999 - Jens Axboe <axboe@image.dk>
  -- Changed the interface for CDROM_SEND_PACKET. Before it was virtually
  impossible to send the drive data in a sensible way.
  -- Lowered stack usage in mmc_ioctl(), dvd_read_disckey(), and
  dvd_read_manufact.
  -- Added setup of write mode for packet writing.
  -- Fixed CDDA ripping with cdda2wav - accept much larger requests of
  number of frames and split the reads in blocks of 8.

  3.06 Dec 13, 1999 - Jens Axboe <axboe@image.dk>
  -- Added support for changing the region of DVD drives.
  -- Added sense data to generic command.

  3.07 Feb 2, 2000 - Jens Axboe <axboe@suse.de>
  -- Do same "read header length" trick in cdrom_get_disc_info() as
  we do in cdrom_get_track_info() -- some drive don't obey specs and
  fail if they can't supply the full Mt Fuji size table.
  -- Deleted stuff related to setting up write modes. It has a different
  home now.
  -- Clear header length in mode_select unconditionally.
  -- Removed the register_disk() that was added, not needed here.

  3.08 May 1, 2000 - Jens Axboe <axboe@suse.de>
  -- Fix direction flag in setup_send_key and setup_report_key. This
  gave some SCSI adapters problems.
  -- Always return -EROFS for write opens
  -- Convert to module_init/module_exit style init and remove some
  of the #ifdef MODULE stuff
  -- Fix several dvd errors - DVD_LU_SEND_ASF should pass agid,
  DVD_HOST_SEND_RPC_STATE did not set buffer size in cdb, and
  dvd_do_auth passed uninitialized data to drive because init_cdrom_command
  did not clear a 0 sized buffer.
  
  3.09 May 12, 2000 - Jens Axboe <axboe@suse.de>
  -- Fix Video-CD on SCSI drives that don't support READ_CD command. In
  that case switch block size and issue plain READ_10 again, then switch
  back.

  3.10 Jun 10, 2000 - Jens Axboe <axboe@suse.de>
  -- Fix volume control on CD's - old SCSI-II drives now use their own
  code, as doing MODE6 stuff in here is really not my intention.
  -- Use READ_DISC_INFO for more reliable end-of-disc.

  3.11 Jun 12, 2000 - Jens Axboe <axboe@suse.de>
  -- Fix bug in getting rpc phase 2 region info.
  -- Reinstate "correct" CDROMPLAYTRKIND

   3.12 Oct 18, 2000 - Jens Axboe <axboe@suse.de>
  -- Use quiet bit on packet commands not known to work

-------------------------------------------------------------------------*/

#define REVISION "Revision: 3.12"
#define VERSION "Id: cdrom.c 3.12 2000/10/18"

/* I use an error-log mask to give fine grain control over the type of
   messages dumped to the system logs.  The available masks include: */
#define CD_NOTHING      0x0
#define CD_WARNING	0x1
#define CD_REG_UNREG	0x2
#define CD_DO_IOCTL	0x4
#define CD_OPEN		0x8
#define CD_CLOSE	0x10
#define CD_COUNT_TRACKS 0x20
#define CD_CHANGER	0x40
#define CD_DVD		0x80

/* Define this to remove _all_ the debugging messages */
/* #define ERRLOGMASK CD_NOTHING */
#define ERRLOGMASK (CD_WARNING)
/* #define ERRLOGMASK (CD_WARNING|CD_OPEN|CD_COUNT_TRACKS|CD_CLOSE) */
/* #define ERRLOGMASK (CD_WARNING|CD_REG_UNREG|CD_DO_IOCTL|CD_OPEN|CD_CLOSE|CD_COUNT_TRACKS) */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/malloc.h> 
#include <linux/cdrom.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/init.h>

#include <asm/fcntl.h>
#include <asm/segment.h>
#include <asm/uaccess.h>

/* used to tell the module to turn on full debugging messages */
static int debug;
/* used to keep tray locked at all times */
static int keeplocked;
/* default compatibility mode */
static int autoclose=1;
static int autoeject;
static int lockdoor = 1;
/* will we ever get to use this... sigh. */
static int check_media_type;
MODULE_PARM(debug, "i");
MODULE_PARM(autoclose, "i");
MODULE_PARM(autoeject, "i");
MODULE_PARM(lockdoor, "i");
MODULE_PARM(check_media_type, "i");

#if (ERRLOGMASK!=CD_NOTHING)
#define cdinfo(type, fmt, args...) \
        if ((ERRLOGMASK & type) || debug==1 ) \
            printk(KERN_INFO "cdrom: " fmt, ## args)
#else
#define cdinfo(type, fmt, args...) 
#endif

/* These are used to simplify getting data in from and back to user land */
#define IOCTL_IN(arg, type, in)					\
	if (copy_from_user(&in, (type *) arg, sizeof in))	\
		return -EFAULT;

#define IOCTL_OUT(arg, type, out) \
	if (copy_to_user((type *) arg, &out, sizeof out))	\
		return -EFAULT;

/* The (cdo->capability & ~cdi->mask & CDC_XXX) construct was used in
   a lot of places. This macro makes the code more clear. */
#define CDROM_CAN(type) (cdi->ops->capability & ~cdi->mask & (type))

/* used in the audio ioctls */
#define CHECKAUDIO if ((ret=check_for_audio_disc(cdi, cdo))) return ret

/* Not-exported routines. */
static int cdrom_open(struct inode *ip, struct file *fp);
static int cdrom_release(struct inode *ip, struct file *fp);
static int cdrom_ioctl(struct inode *ip, struct file *fp,
				unsigned int cmd, unsigned long arg);
static int cdrom_media_changed(kdev_t dev);
static int open_for_data(struct cdrom_device_info * cdi);
static int check_for_audio_disc(struct cdrom_device_info * cdi,
			 struct cdrom_device_ops * cdo);
static void sanitize_format(union cdrom_addr *addr, 
		u_char * curr, u_char requested);
static int mmc_ioctl(struct cdrom_device_info *cdi, unsigned int cmd,
		     unsigned long arg);

int cdrom_get_last_written(kdev_t dev, long *last_written);
int cdrom_get_next_writable(kdev_t dev, long *next_writable);

#ifdef CONFIG_SYSCTL
static void cdrom_sysctl_register(void);
#endif /* CONFIG_SYSCTL */ 
static struct cdrom_device_info *topCdromPtr;
static devfs_handle_t devfs_handle;

struct block_device_operations cdrom_fops =
{
	open:			cdrom_open,
	release:		cdrom_release,
	ioctl:			cdrom_ioctl,
	check_media_change:	cdrom_media_changed,
};

/* This macro makes sure we don't have to check on cdrom_device_ops
 * existence in the run-time routines below. Change_capability is a
 * hack to have the capability flags defined const, while we can still
 * change it here without gcc complaining at every line.
 */
#define ENSURE(call, bits) if (cdo->call == NULL) *change_capability &= ~(bits)

int register_cdrom(struct cdrom_device_info *cdi)
{
	static char banner_printed;
	int major = MAJOR(cdi->dev);
        struct cdrom_device_ops *cdo = cdi->ops;
        int *change_capability = (int *)&cdo->capability; /* hack */
	char vname[16];
	static unsigned int cdrom_counter;

	cdinfo(CD_OPEN, "entering register_cdrom\n"); 

	if (major < 0 || major >= MAX_BLKDEV)
		return -1;
	if (cdo->open == NULL || cdo->release == NULL)
		return -2;
	if ( !banner_printed ) {
		printk(KERN_INFO "Uniform CD-ROM driver " REVISION "\n");
		banner_printed = 1;
#ifdef CONFIG_SYSCTL
		cdrom_sysctl_register();
#endif /* CONFIG_SYSCTL */ 
	}
	ENSURE(drive_status, CDC_DRIVE_STATUS );
	ENSURE(media_changed, CDC_MEDIA_CHANGED);
	ENSURE(tray_move, CDC_CLOSE_TRAY | CDC_OPEN_TRAY);
	ENSURE(lock_door, CDC_LOCK);
	ENSURE(select_speed, CDC_SELECT_SPEED);
	ENSURE(get_last_session, CDC_MULTI_SESSION);
	ENSURE(get_mcn, CDC_MCN);
	ENSURE(reset, CDC_RESET);
	ENSURE(audio_ioctl, CDC_PLAY_AUDIO);
	ENSURE(dev_ioctl, CDC_IOCTLS);
	ENSURE(generic_packet, CDC_GENERIC_PACKET);
	cdi->mc_flags = 0;
	cdo->n_minors = 0;
        cdi->options = CDO_USE_FFLAGS;
	
	if (autoclose==1 && CDROM_CAN(CDC_CLOSE_TRAY))
		cdi->options |= (int) CDO_AUTO_CLOSE;
	if (autoeject==1 && CDROM_CAN(CDC_OPEN_TRAY))
		cdi->options |= (int) CDO_AUTO_EJECT;
	if (lockdoor==1)
		cdi->options |= (int) CDO_LOCK;
	if (check_media_type==1)
		cdi->options |= (int) CDO_CHECK_TYPE;

	if (!devfs_handle)
		devfs_handle = devfs_mk_dir (NULL, "cdroms", NULL);
	sprintf (vname, "cdrom%u", cdrom_counter++);
	if (cdi->de) {
		int pos;
		devfs_handle_t slave;
		char rname[64];

		pos = devfs_generate_path (cdi->de, rname + 3,
					   sizeof rname - 3);
		if (pos >= 0) {
			strncpy (rname + pos, "../", 3);
			devfs_mk_symlink (devfs_handle, vname,
					  DEVFS_FL_DEFAULT,
					  rname + pos, &slave, NULL);
			devfs_auto_unregister (cdi->de, slave);
		}
	}
	else {
		cdi->de =
		    devfs_register (devfs_handle, vname, DEVFS_FL_DEFAULT,
				    MAJOR (cdi->dev), MINOR (cdi->dev),
				    S_IFBLK | S_IRUGO | S_IWUGO,
				    &cdrom_fops, NULL);
	}
	cdinfo(CD_REG_UNREG, "drive \"/dev/%s\" registered\n", cdi->name);
	cdi->next = topCdromPtr; 	
	topCdromPtr = cdi;
	return 0;
}
#undef ENSURE

int unregister_cdrom(struct cdrom_device_info *unreg)
{
	struct cdrom_device_info *cdi, *prev;
	int major = MAJOR(unreg->dev);

	cdinfo(CD_OPEN, "entering unregister_cdrom\n"); 

	if (major < 0 || major >= MAX_BLKDEV)
		return -1;

	prev = NULL;
	cdi = topCdromPtr;
	while (cdi != NULL && cdi->dev != unreg->dev) {
		prev = cdi;
		cdi = cdi->next;
	}

	if (cdi == NULL)
		return -2;
	if (prev)
		prev->next = cdi->next;
	else
		topCdromPtr = cdi->next;
	cdi->ops->n_minors--;
	devfs_unregister (cdi->de);
	cdinfo(CD_REG_UNREG, "drive \"/dev/%s\" unregistered\n", cdi->name);
	return 0;
}

struct cdrom_device_info *cdrom_find_device(kdev_t dev)
{
	struct cdrom_device_info *cdi;

	cdi = topCdromPtr;
	while (cdi != NULL && cdi->dev != dev)
		cdi = cdi->next;

	return cdi;
}

/* We use the open-option O_NONBLOCK to indicate that the
 * purpose of opening is only for subsequent ioctl() calls; no device
 * integrity checks are performed.
 *
 * We hope that all cd-player programs will adopt this convention. It
 * is in their own interest: device control becomes a lot easier
 * this way.
 */
static
int cdrom_open(struct inode *ip, struct file *fp)
{
	struct cdrom_device_info *cdi;
	kdev_t dev = ip->i_rdev;
	int ret;

	cdinfo(CD_OPEN, "entering cdrom_open\n"); 
	if ((cdi = cdrom_find_device(dev)) == NULL)
		return -ENODEV;

	if ((fp->f_mode & FMODE_WRITE) && !CDROM_CAN(CDC_DVD_RAM))
		return -EROFS;

	/* if this was a O_NONBLOCK open and we should honor the flags,
	 * do a quick open without drive/disc integrity checks. */
	if ((fp->f_flags & O_NONBLOCK) && (cdi->options & CDO_USE_FFLAGS))
		ret = cdi->ops->open(cdi, 1);
	else
		ret = open_for_data(cdi);

	if (!ret) cdi->use_count++;

	cdinfo(CD_OPEN, "Use count for \"/dev/%s\" now %d\n", cdi->name, cdi->use_count);
	/* Do this on open.  Don't wait for mount, because they might
	    not be mounting, but opening with O_NONBLOCK */
	check_disk_change(dev);
	return ret;
}

static
int open_for_data(struct cdrom_device_info * cdi)
{
	int ret;
	struct cdrom_device_ops *cdo = cdi->ops;
	tracktype tracks;
	cdinfo(CD_OPEN, "entering open_for_data\n");
	/* Check if the driver can report drive status.  If it can, we
	   can do clever things.  If it can't, well, we at least tried! */
	if (cdo->drive_status != NULL) {
		ret = cdo->drive_status(cdi, CDSL_CURRENT);
		cdinfo(CD_OPEN, "drive_status=%d\n", ret); 
		if (ret == CDS_TRAY_OPEN) {
			cdinfo(CD_OPEN, "the tray is open...\n"); 
			/* can/may i close it? */
			if (CDROM_CAN(CDC_CLOSE_TRAY) &&
			    cdi->options & CDO_AUTO_CLOSE) {
				cdinfo(CD_OPEN, "trying to close the tray.\n"); 
				ret=cdo->tray_move(cdi,0);
				if (ret) {
					cdinfo(CD_OPEN, "bummer. tried to close the tray but failed.\n"); 
					/* Ignore the error from the low
					level driver.  We don't care why it
					couldn't close the tray.  We only care 
					that there is no disc in the drive, 
					since that is the _REAL_ problem here.*/
					ret=-ENOMEDIUM;
					goto clean_up_and_return;
				}
			} else {
				cdinfo(CD_OPEN, "bummer. this drive can't close the tray.\n"); 
				ret=-ENOMEDIUM;
				goto clean_up_and_return;
			}
			/* Ok, the door should be closed now.. Check again */
			ret = cdo->drive_status(cdi, CDSL_CURRENT);
			if ((ret == CDS_NO_DISC) || (ret==CDS_TRAY_OPEN)) {
				cdinfo(CD_OPEN, "bummer. the tray is still not closed.\n"); 
				cdinfo(CD_OPEN, "tray might not contain a medium.\n");
				ret=-ENOMEDIUM;
				goto clean_up_and_return;
			}
			cdinfo(CD_OPEN, "the tray is now closed.\n"); 
		}
		if (ret!=CDS_DISC_OK) {
			ret = -ENOMEDIUM;
			goto clean_up_and_return;
		}
	}
	cdrom_count_tracks(cdi, &tracks);
	if (tracks.error == CDS_NO_DISC) {
		cdinfo(CD_OPEN, "bummer. no disc.\n");
		ret=-ENOMEDIUM;
		goto clean_up_and_return;
	}
	/* CD-Players which don't use O_NONBLOCK, workman
	 * for example, need bit CDO_CHECK_TYPE cleared! */
	if (tracks.data==0) {
		if (cdi->options & CDO_CHECK_TYPE) {
		    /* give people a warning shot, now that CDO_CHECK_TYPE
		       is the default case! */
		    cdinfo(CD_OPEN, "bummer. wrong media type.\n"); 
		    cdinfo(CD_WARNING, "pid %d must open device O_NONBLOCK!\n",
					(unsigned int)current->pid); 
		    ret=-EMEDIUMTYPE;
		    goto clean_up_and_return;
		}
		else {
		    cdinfo(CD_OPEN, "wrong media type, but CDO_CHECK_TYPE not set.\n");
		}
	}

	cdinfo(CD_OPEN, "all seems well, opening the device.\n"); 

	/* all seems well, we can open the device */
	ret = cdo->open(cdi, 0); /* open for data */
	cdinfo(CD_OPEN, "opening the device gave me %d.\n", ret); 
	/* After all this careful checking, we shouldn't have problems
	   opening the device, but we don't want the device locked if 
	   this somehow fails... */
	if (ret) {
		cdinfo(CD_OPEN, "open device failed.\n"); 
		goto clean_up_and_return;
	}
	if (CDROM_CAN(CDC_LOCK) && cdi->options & CDO_LOCK) {
			cdo->lock_door(cdi, 1);
			cdinfo(CD_OPEN, "door locked.\n");
	}
	cdinfo(CD_OPEN, "device opened successfully.\n"); 
	return ret;

	/* Something failed.  Try to unlock the drive, because some drivers
	(notably ide-cd) lock the drive after every command.  This produced
	a nasty bug where after mount failed, the drive would remain locked!  
	This ensures that the drive gets unlocked after a mount fails.  This 
	is a goto to avoid bloating the driver with redundant code. */ 
clean_up_and_return:
	cdinfo(CD_WARNING, "open failed.\n"); 
	if (CDROM_CAN(CDC_LOCK) && cdi->options & CDO_LOCK) {
			cdo->lock_door(cdi, 0);
			cdinfo(CD_OPEN, "door unlocked.\n");
	}
	return ret;
}

/* This code is similar to that in open_for_data. The routine is called
   whenever an audio play operation is requested.
*/
int check_for_audio_disc(struct cdrom_device_info * cdi,
			 struct cdrom_device_ops * cdo)
{
        int ret;
	tracktype tracks;
	cdinfo(CD_OPEN, "entering check_for_audio_disc\n");
	if (!(cdi->options & CDO_CHECK_TYPE))
		return 0;
	if (cdo->drive_status != NULL) {
		ret = cdo->drive_status(cdi, CDSL_CURRENT);
		cdinfo(CD_OPEN, "drive_status=%d\n", ret); 
		if (ret == CDS_TRAY_OPEN) {
			cdinfo(CD_OPEN, "the tray is open...\n"); 
			/* can/may i close it? */
			if (CDROM_CAN(CDC_CLOSE_TRAY) &&
			    cdi->options & CDO_AUTO_CLOSE) {
				cdinfo(CD_OPEN, "trying to close the tray.\n"); 
				ret=cdo->tray_move(cdi,0);
				if (ret) {
					cdinfo(CD_OPEN, "bummer. tried to close tray but failed.\n"); 
					/* Ignore the error from the low
					level driver.  We don't care why it
					couldn't close the tray.  We only care 
					that there is no disc in the drive, 
					since that is the _REAL_ problem here.*/
					return -ENOMEDIUM;
				}
			} else {
				cdinfo(CD_OPEN, "bummer. this driver can't close the tray.\n"); 
				return -ENOMEDIUM;
			}
			/* Ok, the door should be closed now.. Check again */
			ret = cdo->drive_status(cdi, CDSL_CURRENT);
			if ((ret == CDS_NO_DISC) || (ret==CDS_TRAY_OPEN)) {
				cdinfo(CD_OPEN, "bummer. the tray is still not closed.\n"); 
				return -ENOMEDIUM;
			}	
			if (ret!=CDS_DISC_OK) {
				cdinfo(CD_OPEN, "bummer. disc isn't ready.\n"); 
				return -EIO;
			}	
			cdinfo(CD_OPEN, "the tray is now closed.\n"); 
		}	
	}
	cdrom_count_tracks(cdi, &tracks);
	if (tracks.error) 
		return(tracks.error);

	if (tracks.audio==0)
		return -EMEDIUMTYPE;

	return 0;
}


/* Admittedly, the logic below could be performed in a nicer way. */
static
int cdrom_release(struct inode *ip, struct file *fp)
{
	kdev_t dev = ip->i_rdev;
	struct cdrom_device_info *cdi = cdrom_find_device(dev);
	struct cdrom_device_ops *cdo = cdi->ops;
	int opened_for_data;

	cdinfo(CD_CLOSE, "entering cdrom_release\n"); 

	if (cdi->use_count > 0)
		cdi->use_count--;
	if (cdi->use_count == 0)
		cdinfo(CD_CLOSE, "Use count for \"/dev/%s\" now zero\n", cdi->name);
	if (cdi->use_count == 0 &&
	    cdo->capability & CDC_LOCK && !keeplocked) {
		cdinfo(CD_CLOSE, "Unlocking door!\n");
		cdo->lock_door(cdi, 0);
	}
	opened_for_data = !(cdi->options & CDO_USE_FFLAGS) ||
		!(fp && fp->f_flags & O_NONBLOCK);
	cdo->release(cdi);
	if (cdi->use_count == 0) {      /* last process that closes dev*/
		if (opened_for_data &&
		    cdi->options & CDO_AUTO_EJECT && CDROM_CAN(CDC_OPEN_TRAY))
			cdo->tray_move(cdi, 1);
	}
	return 0;
}

static int cdrom_read_mech_status(struct cdrom_device_info *cdi, 
				  struct cdrom_changer_info *buf)
{
	struct cdrom_generic_command cgc;
	struct cdrom_device_ops *cdo = cdi->ops;
	int length;

	/*
	 * Sanyo changer isn't spec compliant (doesn't use regular change
	 * LOAD_UNLOAD command, and it doesn't implement the mech status
	 * command below
	 */
	if (cdi->sanyo_slot) {
		buf->hdr.nslots = 3;
		buf->hdr.curslot = cdi->sanyo_slot == 3 ? 0 : cdi->sanyo_slot;
		for (length = 0; length < 3; length++) {
			buf->slots[length].disc_present = 1;
			buf->slots[length].change = 0;
		}
		return 0;
	}

	length = sizeof(struct cdrom_mechstat_header) +
		 cdi->capacity * sizeof(struct cdrom_slot);

	init_cdrom_command(&cgc, buf, length, CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_MECHANISM_STATUS;
	cgc.cmd[8] = (length >> 8) & 0xff;
	cgc.cmd[9] = length & 0xff;
	return cdo->generic_packet(cdi, &cgc);
}

static int cdrom_slot_status(struct cdrom_device_info *cdi, int slot)
{
	struct cdrom_changer_info info;
	int ret;

	cdinfo(CD_CHANGER, "entering cdrom_slot_status()\n"); 
	if (cdi->sanyo_slot)
		return CDS_NO_INFO;
	
	if ((ret = cdrom_read_mech_status(cdi, &info)))
		return ret;

	if (info.slots[slot].disc_present)
		return CDS_DISC_OK;
	else
		return CDS_NO_DISC;

}

/* Return the number of slots for an ATAPI/SCSI cdrom, 
 * return 1 if not a changer. 
 */
int cdrom_number_of_slots(struct cdrom_device_info *cdi) 
{
	int status;
	int nslots = 1;
	struct cdrom_changer_info info;

	cdinfo(CD_CHANGER, "entering cdrom_number_of_slots()\n"); 
	/* cdrom_read_mech_status requires a valid value for capacity: */
	cdi->capacity = 0; 

	if ((status = cdrom_read_mech_status(cdi, &info)) == 0)
		nslots = info.hdr.nslots;

	return nslots;
}


/* If SLOT < 0, unload the current slot.  Otherwise, try to load SLOT. */
static int cdrom_load_unload(struct cdrom_device_info *cdi, int slot) 
{
	struct cdrom_generic_command cgc;

	cdinfo(CD_CHANGER, "entering cdrom_load_unload()\n"); 
	if (cdi->sanyo_slot && slot < 0)
		return 0;

	init_cdrom_command(&cgc, NULL, 0, CGC_DATA_NONE);
	cgc.cmd[0] = GPCMD_LOAD_UNLOAD;
	cgc.cmd[4] = 2 + (slot >= 0);
	cgc.cmd[8] = slot;

	/* The Sanyo 3 CD changer uses byte 7 of the 
	GPCMD_TEST_UNIT_READY to command to switch CDs instead of
	using the GPCMD_LOAD_UNLOAD opcode. */
	if (cdi->sanyo_slot && -1 < slot) {
		cgc.cmd[0] = GPCMD_TEST_UNIT_READY;
		cgc.cmd[7] = slot;
		cgc.cmd[4] = cgc.cmd[8] = 0;
		cdi->sanyo_slot = slot ? slot : 3;
	}

	return cdi->ops->generic_packet(cdi, &cgc);
}

int cdrom_select_disc(struct cdrom_device_info *cdi, int slot)
{
	struct cdrom_changer_info info;
	int curslot;
	int ret;

	cdinfo(CD_CHANGER, "entering cdrom_select_disc()\n"); 
	if (!CDROM_CAN(CDC_SELECT_DISC))
		return -EDRIVE_CANT_DO_THIS;

	(void) cdi->ops->media_changed(cdi, slot);

	if (slot == CDSL_NONE) {
		/* set media changed bits, on both queues */
		cdi->mc_flags = 0x3;
		return cdrom_load_unload(cdi, -1);
	}

	if ((ret = cdrom_read_mech_status(cdi, &info)))
		return ret;

	curslot = info.hdr.curslot;

	if (cdi->use_count > 1 || keeplocked) {
		if (slot == CDSL_CURRENT) {
	    		return curslot;
		} else {
			return -EBUSY;
		}
	}

	/* Specifying CDSL_CURRENT will attempt to load the currnet slot,
	which is useful if it had been previously unloaded.
	Whether it can or not, it returns the current slot. 
	Similarly,  if slot happens to be the current one, we still
	try and load it. */
	if (slot == CDSL_CURRENT)
		slot = curslot;

	/* set media changed bits on both queues */
	cdi->mc_flags = 0x3;
	if ((ret = cdrom_load_unload(cdi, slot)))
		return ret;

	return slot;
}

/* We want to make media_changed accessible to the user through an
 * ioctl. The main problem now is that we must double-buffer the
 * low-level implementation, to assure that the VFS and the user both
 * see a medium change once.
 */

static
int media_changed(struct cdrom_device_info *cdi, int queue)
{
	unsigned int mask = (1 << (queue & 1));
	int ret = !!(cdi->mc_flags & mask);

	if (!CDROM_CAN(CDC_MEDIA_CHANGED))
	    return ret;
	/* changed since last call? */
	if (cdi->ops->media_changed(cdi, CDSL_CURRENT)) {
		cdi->mc_flags = 0x3;    /* set bit on both queues */
		ret |= 1;
	}
	cdi->mc_flags &= ~mask;         /* clear bit */
	return ret;
}

static int cdrom_media_changed(kdev_t dev)
{
	struct cdrom_device_info *cdi = cdrom_find_device(dev);
	/* This talks to the VFS, which doesn't like errors - just 1 or 0.  
	 * Returning "0" is always safe (media hasn't been changed). Do that 
	 * if the low-level cdrom driver dosn't support media changed. */ 
	if (cdi == NULL || cdi->ops->media_changed == NULL)
		return 0;
	if (!CDROM_CAN(CDC_MEDIA_CHANGED))
		return 0;
	return media_changed(cdi, 0);
}

/* badly broken, I know. Is due for a fixup anytime. */
void cdrom_count_tracks(struct cdrom_device_info *cdi, tracktype* tracks)
{
	struct cdrom_tochdr header;
	struct cdrom_tocentry entry;
	int ret, i;
	tracks->data=0;
	tracks->audio=0;
	tracks->cdi=0;
	tracks->xa=0;
	tracks->error=0;
	cdinfo(CD_COUNT_TRACKS, "entering cdrom_count_tracks\n"); 
        if (!CDROM_CAN(CDC_PLAY_AUDIO)) { 
                tracks->error=CDS_NO_INFO;
                return;
        }        
	/* Grab the TOC header so we can see how many tracks there are */
	if ((ret = cdi->ops->audio_ioctl(cdi, CDROMREADTOCHDR, &header))) {
		if (ret == -ENOMEDIUM)
			tracks->error = CDS_NO_DISC;
		else
			tracks->error = CDS_NO_INFO;
		return;
	}	
	/* check what type of tracks are on this disc */
	entry.cdte_format = CDROM_MSF;
	for (i = header.cdth_trk0; i <= header.cdth_trk1; i++) {
		entry.cdte_track  = i;
		if (cdi->ops->audio_ioctl(cdi, CDROMREADTOCENTRY, &entry)) {
			tracks->error=CDS_NO_INFO;
			return;
		}	
		if (entry.cdte_ctrl & CDROM_DATA_TRACK) {
		    if (entry.cdte_format == 0x10)
			tracks->cdi++;
		    else if (entry.cdte_format == 0x20) 
			tracks->xa++;
		    else
			tracks->data++;
		} else
		    tracks->audio++;
		cdinfo(CD_COUNT_TRACKS, "track %d: format=%d, ctrl=%d\n",
		       i, entry.cdte_format, entry.cdte_ctrl);
	}	
	cdinfo(CD_COUNT_TRACKS, "disc has %d tracks: %d=audio %d=data %d=Cd-I %d=XA\n", 
		header.cdth_trk1, tracks->audio, tracks->data, 
		tracks->cdi, tracks->xa);
}	

/* Requests to the low-level drivers will /always/ be done in the
   following format convention:

   CDROM_LBA: all data-related requests.
   CDROM_MSF: all audio-related requests.

   However, a low-level implementation is allowed to refuse this
   request, and return information in its own favorite format.

   It doesn't make sense /at all/ to ask for a play_audio in LBA
   format, or ask for multi-session info in MSF format. However, for
   backward compatibility these format requests will be satisfied, but
   the requests to the low-level drivers will be sanitized in the more
   meaningful format indicated above.
 */

static
void sanitize_format(union cdrom_addr *addr,
		     u_char * curr, u_char requested)
{
	if (*curr == requested)
		return;                 /* nothing to be done! */
	if (requested == CDROM_LBA) {
		addr->lba = (int) addr->msf.frame +
			75 * (addr->msf.second - 2 + 60 * addr->msf.minute);
	} else {                        /* CDROM_MSF */
		int lba = addr->lba;
		addr->msf.frame = lba % 75;
		lba /= 75;
		lba += 2;
		addr->msf.second = lba % 60;
		addr->msf.minute = lba / 60;
	}
	*curr = requested;
}

void init_cdrom_command(struct cdrom_generic_command *cgc, void *buf, int len,
			int type)
{
	memset(cgc, 0, sizeof(struct cdrom_generic_command));
	if (buf)
		memset(buf, 0, len);
	cgc->buffer = (char *) buf;
	cgc->buflen = len;
	cgc->data_direction = type;
	cgc->timeout = 5*HZ;
}

/* DVD handling */

#define copy_key(dest,src)	memcpy((dest), (src), sizeof(dvd_key))
#define copy_chal(dest,src)	memcpy((dest), (src), sizeof(dvd_challenge))

static void setup_report_key(struct cdrom_generic_command *cgc, unsigned agid, unsigned type)
{
	cgc->cmd[0] = GPCMD_REPORT_KEY;
	cgc->cmd[10] = type | (agid << 6);
	switch (type) {
		case 0: case 8: case 5: {
			cgc->buflen = 8;
			break;
		}
		case 1: {
			cgc->buflen = 16;
			break;
		}
		case 2: case 4: {
			cgc->buflen = 12;
			break;
		}
	}
	cgc->cmd[9] = cgc->buflen;
	cgc->data_direction = CGC_DATA_READ;
}

static void setup_send_key(struct cdrom_generic_command *cgc, unsigned agid, unsigned type)
{
	cgc->cmd[0] = GPCMD_SEND_KEY;
	cgc->cmd[10] = type | (agid << 6);
	switch (type) {
		case 1: {
			cgc->buflen = 16;
			break;
		}
		case 3: {
			cgc->buflen = 12;
			break;
		}
		case 6: {
			cgc->buflen = 8;
			break;
		}
	}
	cgc->cmd[9] = cgc->buflen;
	cgc->data_direction = CGC_DATA_WRITE;
}

static int dvd_do_auth(struct cdrom_device_info *cdi, dvd_authinfo *ai)
{
	int ret;
	u_char buf[20];
	struct cdrom_generic_command cgc;
	struct cdrom_device_ops *cdo = cdi->ops;
	rpc_state_t rpc_state;

	memset(buf, 0, sizeof(buf));
	init_cdrom_command(&cgc, buf, 0, CGC_DATA_READ);

	switch (ai->type) {
	/* LU data send */
	case DVD_LU_SEND_AGID:
		cdinfo(CD_DVD, "entering DVD_LU_SEND_AGID\n"); 
		setup_report_key(&cgc, ai->lsa.agid, 0);

		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;

		ai->lsa.agid = buf[7] >> 6;
		/* Returning data, let host change state */
		break;

	case DVD_LU_SEND_KEY1:
		cdinfo(CD_DVD, "entering DVD_LU_SEND_KEY1\n"); 
		setup_report_key(&cgc, ai->lsk.agid, 2);

		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;

		copy_key(ai->lsk.key, &buf[4]);
		/* Returning data, let host change state */
		break;

	case DVD_LU_SEND_CHALLENGE:
		cdinfo(CD_DVD, "entering DVD_LU_SEND_CHALLENGE\n"); 
		setup_report_key(&cgc, ai->lsc.agid, 1);

		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;

		copy_chal(ai->lsc.chal, &buf[4]);
		/* Returning data, let host change state */
		break;

	/* Post-auth key */
	case DVD_LU_SEND_TITLE_KEY:
		cdinfo(CD_DVD, "entering DVD_LU_SEND_TITLE_KEY\n"); 
		setup_report_key(&cgc, ai->lstk.agid, 4);
		cgc.cmd[5] = ai->lstk.lba;
		cgc.cmd[4] = ai->lstk.lba >> 8;
		cgc.cmd[3] = ai->lstk.lba >> 16;
		cgc.cmd[2] = ai->lstk.lba >> 24;

		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;

		ai->lstk.cpm = (buf[4] >> 7) & 1;
		ai->lstk.cp_sec = (buf[4] >> 6) & 1;
		ai->lstk.cgms = (buf[4] >> 4) & 3;
		copy_key(ai->lstk.title_key, &buf[5]);
		/* Returning data, let host change state */
		break;

	case DVD_LU_SEND_ASF:
		cdinfo(CD_DVD, "entering DVD_LU_SEND_ASF\n"); 
		setup_report_key(&cgc, ai->lsasf.agid, 5);
		
		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;

		ai->lsasf.asf = buf[7] & 1;
		break;

	/* LU data receive (LU changes state) */
	case DVD_HOST_SEND_CHALLENGE:
		cdinfo(CD_DVD, "entering DVD_HOST_SEND_CHALLENGE\n"); 
		setup_send_key(&cgc, ai->hsc.agid, 1);
		buf[1] = 0xe;
		copy_chal(&buf[4], ai->hsc.chal);

		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;

		ai->type = DVD_LU_SEND_KEY1;
		break;

	case DVD_HOST_SEND_KEY2:
		cdinfo(CD_DVD, "entering DVD_HOST_SEND_KEY2\n"); 
		setup_send_key(&cgc, ai->hsk.agid, 3);
		buf[1] = 0xa;
		copy_key(&buf[4], ai->hsk.key);

		if ((ret = cdo->generic_packet(cdi, &cgc))) {
			ai->type = DVD_AUTH_FAILURE;
			return ret;
		}
		ai->type = DVD_AUTH_ESTABLISHED;
		break;

	/* Misc */
	case DVD_INVALIDATE_AGID:
		cdinfo(CD_DVD, "entering DVD_INVALIDATE_AGID\n"); 
		setup_report_key(&cgc, ai->lsa.agid, 0x3f);
		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;
		break;

	/* Get region settings */
	case DVD_LU_SEND_RPC_STATE:
		cdinfo(CD_DVD, "entering DVD_LU_SEND_RPC_STATE\n");
		setup_report_key(&cgc, 0, 8);
		memset(&rpc_state, 0, sizeof(rpc_state_t));
		cgc.buffer = (char *) &rpc_state;

		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;

		ai->lrpcs.type = rpc_state.type_code;
		ai->lrpcs.vra = rpc_state.vra;
		ai->lrpcs.ucca = rpc_state.ucca;
		ai->lrpcs.region_mask = rpc_state.region_mask;
		ai->lrpcs.rpc_scheme = rpc_state.rpc_scheme;
		break;

	/* Set region settings */
	case DVD_HOST_SEND_RPC_STATE:
		cdinfo(CD_DVD, "entering DVD_HOST_SEND_RPC_STATE\n");
		setup_send_key(&cgc, 0, 6);
		buf[1] = 6;
		buf[4] = ai->hrpcs.pdrc;

		if ((ret = cdo->generic_packet(cdi, &cgc)))
			return ret;
		break;

	default:
		cdinfo(CD_WARNING, "Invalid DVD key ioctl (%d)\n", ai->type);
		return -ENOTTY;
	}

	return 0;
}

static int dvd_read_physical(struct cdrom_device_info *cdi, dvd_struct *s)
{
	int ret, i;
	u_char buf[4 + 4 * 20], *base;
	struct dvd_layer *layer;
	struct cdrom_generic_command cgc;
	struct cdrom_device_ops *cdo = cdi->ops;

	init_cdrom_command(&cgc, buf, sizeof(buf), CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_READ_DVD_STRUCTURE;
	cgc.cmd[6] = s->physical.layer_num;
	cgc.cmd[7] = s->type;
	cgc.cmd[9] = cgc.buflen & 0xff;

	if ((ret = cdo->generic_packet(cdi, &cgc)))
		return ret;

	base = &buf[4];
	layer = &s->physical.layer[0];

	/* place the data... really ugly, but at least we won't have to
	   worry about endianess in userspace or here. */
	for (i = 0; i < 4; ++i, base += 20, ++layer) {
		memset(layer, 0, sizeof(*layer));
		layer->book_version = base[0] & 0xf;
		layer->book_type = base[0] >> 4;
		layer->min_rate = base[1] & 0xf;
		layer->disc_size = base[1] >> 4;
		layer->layer_type = base[2] & 0xf;
		layer->track_path = (base[2] >> 4) & 1;
		layer->nlayers = (base[2] >> 5) & 3;
		layer->track_density = base[3] & 0xf;
		layer->linear_density = base[3] >> 4;
		layer->start_sector = base[5] << 16 | base[6] << 8 | base[7];
		layer->end_sector = base[9] << 16 | base[10] << 8 | base[11];
		layer->end_sector_l0 = base[13] << 16 | base[14] << 8 | base[15];
		layer->bca = base[16] >> 7;
	}

	return 0;
}

static int dvd_read_copyright(struct cdrom_device_info *cdi, dvd_struct *s)
{
	int ret;
	u_char buf[8];
	struct cdrom_generic_command cgc;
	struct cdrom_device_ops *cdo = cdi->ops;

	init_cdrom_command(&cgc, buf, sizeof(buf), CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_READ_DVD_STRUCTURE;
	cgc.cmd[6] = s->copyright.layer_num;
	cgc.cmd[7] = s->type;
	cgc.cmd[8] = cgc.buflen >> 8;
	cgc.cmd[9] = cgc.buflen & 0xff;

	if ((ret = cdo->generic_packet(cdi, &cgc)))
		return ret;

	s->copyright.cpst = buf[4];
	s->copyright.rmi = buf[5];

	return 0;
}

static int dvd_read_disckey(struct cdrom_device_info *cdi, dvd_struct *s)
{
	int ret, size;
	u_char *buf;
	struct cdrom_generic_command cgc;
	struct cdrom_device_ops *cdo = cdi->ops;

	size = sizeof(s->disckey.value) + 4;

	if ((buf = (u_char *) kmalloc(size, GFP_KERNEL)) == NULL)
		return -ENOMEM;

	init_cdrom_command(&cgc, buf, size, CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_READ_DVD_STRUCTURE;
	cgc.cmd[7] = s->type;
	cgc.cmd[8] = size >> 8;
	cgc.cmd[9] = size & 0xff;
	cgc.cmd[10] = s->disckey.agid << 6;

	if (!(ret = cdo->generic_packet(cdi, &cgc)))
		memcpy(s->disckey.value, &buf[4], sizeof(s->disckey.value));

	kfree(buf);
	return ret;
}

static int dvd_read_bca(struct cdrom_device_info *cdi, dvd_struct *s)
{
	int ret;
	u_char buf[4 + 188];
	struct cdrom_generic_command cgc;
	struct cdrom_device_ops *cdo = cdi->ops;

	init_cdrom_command(&cgc, buf, sizeof(buf), CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_READ_DVD_STRUCTURE;
	cgc.cmd[7] = s->type;
	cgc.cmd[9] = cgc.buflen = 0xff;

	if ((ret = cdo->generic_packet(cdi, &cgc)))
		return ret;

	s->bca.len = buf[0] << 8 | buf[1];
	if (s->bca.len < 12 || s->bca.len > 188) {
		cdinfo(CD_WARNING, "Received invalid BCA length (%d)\n", s->bca.len);
		return -EIO;
	}
	memcpy(s->bca.value, &buf[4], s->bca.len);

	return 0;
}

static int dvd_read_manufact(struct cdrom_device_info *cdi, dvd_struct *s)
{
	int ret = 0, size;
	u_char *buf;
	struct cdrom_generic_command cgc;
	struct cdrom_device_ops *cdo = cdi->ops;

	size = sizeof(s->manufact.value) + 4;

	if ((buf = (u_char *) kmalloc(size, GFP_KERNEL)) == NULL)
		return -ENOMEM;

	init_cdrom_command(&cgc, buf, size, CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_READ_DVD_STRUCTURE;
	cgc.cmd[7] = s->type;
	cgc.cmd[8] = size >> 8;
	cgc.cmd[9] = size & 0xff;

	if ((ret = cdo->generic_packet(cdi, &cgc))) {
		kfree(buf);
		return ret;
	}

	s->manufact.len = buf[0] << 8 | buf[1];
	if (s->manufact.len < 0 || s->manufact.len > 2048) {
		cdinfo(CD_WARNING, "Received invalid manufacture info length"
				   " (%d)\n", s->bca.len);
		ret = -EIO;
	} else {
		memcpy(s->manufact.value, &buf[4], s->manufact.len);
	}

	kfree(buf);
	return ret;
}

static int dvd_read_struct(struct cdrom_device_info *cdi, dvd_struct *s)
{
	switch (s->type) {
	case DVD_STRUCT_PHYSICAL:
		return dvd_read_physical(cdi, s);

	case DVD_STRUCT_COPYRIGHT:
		return dvd_read_copyright(cdi, s);

	case DVD_STRUCT_DISCKEY:
		return dvd_read_disckey(cdi, s);

	case DVD_STRUCT_BCA:
		return dvd_read_bca(cdi, s);

	case DVD_STRUCT_MANUFACT:
		return dvd_read_manufact(cdi, s);
		
	default:
		cdinfo(CD_WARNING, ": Invalid DVD structure read requested (%d)\n",
					s->type);
		return -EINVAL;
	}
}

int cdrom_mode_sense(struct cdrom_device_info *cdi,
		     struct cdrom_generic_command *cgc,
		     int page_code, int page_control)
{
	struct cdrom_device_ops *cdo = cdi->ops;

	memset(cgc->cmd, 0, sizeof(cgc->cmd));

	cgc->cmd[0] = GPCMD_MODE_SENSE_10;
	cgc->cmd[2] = page_code | (page_control << 6);
	cgc->cmd[7] = cgc->buflen >> 8;
	cgc->cmd[8] = cgc->buflen & 0xff;
	cgc->data_direction = CGC_DATA_READ;
	return cdo->generic_packet(cdi, cgc);
}

int cdrom_mode_select(struct cdrom_device_info *cdi,
		      struct cdrom_generic_command *cgc)
{
	struct cdrom_device_ops *cdo = cdi->ops;

	memset(cgc->cmd, 0, sizeof(cgc->cmd));
	memset(cgc->buffer, 0, 2);
	cgc->cmd[0] = GPCMD_MODE_SELECT_10;
	cgc->cmd[1] = 0x10;		/* PF */
	cgc->cmd[7] = cgc->buflen >> 8;
	cgc->cmd[8] = cgc->buflen & 0xff;
	cgc->data_direction = CGC_DATA_WRITE;
	return cdo->generic_packet(cdi, cgc);
}

static int cdrom_read_subchannel(struct cdrom_device_info *cdi,
				 struct cdrom_subchnl *subchnl, int mcn)
{
	struct cdrom_device_ops *cdo = cdi->ops;
	struct cdrom_generic_command cgc;
	char buffer[32];
	int ret;

	init_cdrom_command(&cgc, buffer, 16, CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_READ_SUBCHANNEL;
	cgc.cmd[1] = 2;     /* MSF addressing */
	cgc.cmd[2] = 0x40;  /* request subQ data */
	cgc.cmd[3] = mcn ? 2 : 1;
	cgc.cmd[8] = 16;

	if ((ret = cdo->generic_packet(cdi, &cgc)))
		return ret;

	subchnl->cdsc_audiostatus = cgc.buffer[1];
	subchnl->cdsc_format = CDROM_MSF;
	subchnl->cdsc_ctrl = cgc.buffer[5] & 0xf;
	subchnl->cdsc_trk = cgc.buffer[6];
	subchnl->cdsc_ind = cgc.buffer[7];

	subchnl->cdsc_reladdr.msf.minute = cgc.buffer[13];
	subchnl->cdsc_reladdr.msf.second = cgc.buffer[14];
	subchnl->cdsc_reladdr.msf.frame = cgc.buffer[15];
	subchnl->cdsc_absaddr.msf.minute = cgc.buffer[9];
	subchnl->cdsc_absaddr.msf.second = cgc.buffer[10];
	subchnl->cdsc_absaddr.msf.frame = cgc.buffer[11];

	return 0;
}

/*
 * Specific READ_10 interface
 */
static int cdrom_read_cd(struct cdrom_device_info *cdi,
			 struct cdrom_generic_command *cgc, int lba,
			 int blocksize, int nblocks)
{
	struct cdrom_device_ops *cdo = cdi->ops;

	memset(&cgc->cmd, 0, sizeof(cgc->cmd));
	cgc->cmd[0] = GPCMD_READ_10;
	cgc->cmd[2] = (lba >> 24) & 0xff;
	cgc->cmd[3] = (lba >> 16) & 0xff;
	cgc->cmd[4] = (lba >>  8) & 0xff;
	cgc->cmd[5] = lba & 0xff;
	cgc->cmd[6] = (nblocks >> 16) & 0xff;
	cgc->cmd[7] = (nblocks >>  8) & 0xff;
	cgc->cmd[8] = nblocks & 0xff;
	cgc->buflen = blocksize * nblocks;
	return cdo->generic_packet(cdi, cgc);
}

/* very generic interface for reading the various types of blocks */
static int cdrom_read_block(struct cdrom_device_info *cdi,
			    struct cdrom_generic_command *cgc,
			    int lba, int nblocks, int format, int blksize)
{
	struct cdrom_device_ops *cdo = cdi->ops;

	memset(&cgc->cmd, 0, sizeof(cgc->cmd));
	cgc->cmd[0] = GPCMD_READ_CD;
	/* expected sector size - cdda,mode1,etc. */
	cgc->cmd[1] = format << 2;
	/* starting address */
	cgc->cmd[2] = (lba >> 24) & 0xff;
	cgc->cmd[3] = (lba >> 16) & 0xff;
	cgc->cmd[4] = (lba >>  8) & 0xff;
	cgc->cmd[5] = lba & 0xff;
	/* number of blocks */
	cgc->cmd[6] = (nblocks >> 16) & 0xff;
	cgc->cmd[7] = (nblocks >>  8) & 0xff;
	cgc->cmd[8] = nblocks & 0xff;
	cgc->buflen = blksize * nblocks;
	
	/* set the header info returned */
	switch (blksize) {
	case CD_FRAMESIZE_RAW0	: cgc->cmd[9] = 0x58; break;
	case CD_FRAMESIZE_RAW1	: cgc->cmd[9] = 0x78; break;
	case CD_FRAMESIZE_RAW	: cgc->cmd[9] = 0xf8; break;
	default			: cgc->cmd[9] = 0x10;
	}
	
	return cdo->generic_packet(cdi, cgc);
}

/* Just about every imaginable ioctl is supported in the Uniform layer
 * these days. ATAPI / SCSI specific code now mainly resides in
 * mmc_ioct().
 */
static int cdrom_ioctl(struct inode *ip, struct file *fp, unsigned int cmd,
		       unsigned long arg)
{
	kdev_t dev = ip->i_rdev;
	struct cdrom_device_info *cdi = cdrom_find_device(dev);
	struct cdrom_device_ops *cdo = cdi->ops;
	int ret;

	/* the first few commands do not deal with audio drive_info, but
	   only with routines in cdrom device operations. */
	switch (cmd) {
	case CDROMMULTISESSION: {
		struct cdrom_multisession ms_info;
		u_char requested_format;
		cdinfo(CD_DO_IOCTL, "entering CDROMMULTISESSION\n"); 
                if (!(cdo->capability & CDC_MULTI_SESSION))
                        return -ENOSYS;
		IOCTL_IN(arg, struct cdrom_multisession, ms_info);
		requested_format = ms_info.addr_format;
		if (!((requested_format == CDROM_MSF) ||
			(requested_format == CDROM_LBA)))
				return -EINVAL;
		ms_info.addr_format = CDROM_LBA;
		if ((ret=cdo->get_last_session(cdi, &ms_info)))
			return ret;
		sanitize_format(&ms_info.addr, &ms_info.addr_format,
				requested_format);
		IOCTL_OUT(arg, struct cdrom_multisession, ms_info);
		cdinfo(CD_DO_IOCTL, "CDROMMULTISESSION successful\n"); 
		return 0;
		}

	case CDROMEJECT: {
		cdinfo(CD_DO_IOCTL, "entering CDROMEJECT\n"); 
		if (!CDROM_CAN(CDC_OPEN_TRAY))
			return -ENOSYS;
		if (cdi->use_count != 1 || keeplocked)
			return -EBUSY;
		if (CDROM_CAN(CDC_LOCK))
			if ((ret=cdo->lock_door(cdi, 0)))
				return ret;

		return cdo->tray_move(cdi, 1);
		}

	case CDROMCLOSETRAY: {
		cdinfo(CD_DO_IOCTL, "entering CDROMCLOSETRAY\n"); 
		if (!CDROM_CAN(CDC_CLOSE_TRAY))
			return -ENOSYS;
		return cdo->tray_move(cdi, 0);
		}

	case CDROMEJECT_SW: {
		cdinfo(CD_DO_IOCTL, "entering CDROMEJECT_SW\n"); 
		if (!CDROM_CAN(CDC_OPEN_TRAY))
			return -ENOSYS;
		if (keeplocked)
			return -EBUSY;
		cdi->options &= ~(CDO_AUTO_CLOSE | CDO_AUTO_EJECT);
		if (arg)
			cdi->options |= CDO_AUTO_CLOSE | CDO_AUTO_EJECT;
		return 0;
		}

	case CDROM_MEDIA_CHANGED: {
		struct cdrom_changer_info info;

		cdinfo(CD_DO_IOCTL, "entering CDROM_MEDIA_CHANGED\n"); 
		if (!CDROM_CAN(CDC_MEDIA_CHANGED))
			return -ENOSYS;

		/* cannot select disc or select current disc */
		if (!CDROM_CAN(CDC_SELECT_DISC) || arg == CDSL_CURRENT)
			return media_changed(cdi, 1);

		if ((unsigned int)arg >= cdi->capacity)
			return -EINVAL;

		if ((ret = cdrom_read_mech_status(cdi, &info)))
			return ret;

		return info.slots[arg].change;
		}

	case CDROM_SET_OPTIONS: {
		cdinfo(CD_DO_IOCTL, "entering CDROM_SET_OPTIONS\n"); 
		/* options need to be in sync with capability. too late for
		   that, so we have to check each one separately... */
		switch (arg) {
		case CDO_USE_FFLAGS:
		case CDO_CHECK_TYPE:
			break;
		case CDO_LOCK:
			if (!CDROM_CAN(CDC_LOCK))
				return -ENOSYS;
			break;
		case 0:
			return cdi->options;
		/* default is basically CDO_[AUTO_CLOSE|AUTO_EJECT] */
		default:
			if (!CDROM_CAN(arg))
				return -ENOSYS;
		}
		cdi->options |= (int) arg;
		return cdi->options;
		}

	case CDROM_CLEAR_OPTIONS: {
		cdinfo(CD_DO_IOCTL, "entering CDROM_CLEAR_OPTIONS\n"); 
		cdi->options &= ~(int) arg;
		return cdi->options;
		}

	case CDROM_SELECT_SPEED: {
		cdinfo(CD_DO_IOCTL, "entering CDROM_SELECT_SPEED\n"); 
		if (!CDROM_CAN(CDC_SELECT_SPEED))
			return -ENOSYS;
		return cdo->select_speed(cdi, arg);
		}

	case CDROM_SELECT_DISC: {
		cdinfo(CD_DO_IOCTL, "entering CDROM_SELECT_DISC\n"); 
		if (!CDROM_CAN(CDC_SELECT_DISC))
			return -ENOSYS;

                if ((arg != CDSL_CURRENT) && (arg != CDSL_NONE))
			if ((int)arg >= cdi->capacity)
				return -EINVAL;

		/* cdo->select_disc is a hook to allow a driver-specific
		 * way of seleting disc.  However, since there is no
		 * equiv hook for cdrom_slot_status this may not 
		 * actually be useful...
		 */
		if (cdo->select_disc != NULL)
			return cdo->select_disc(cdi, arg);

		/* no driver specific select_disc(), call our own */
		cdinfo(CD_CHANGER, "Using generic cdrom_select_disc()\n"); 
		return cdrom_select_disc(cdi, arg);
		}

	case CDROMRESET: {
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		cdinfo(CD_DO_IOCTL, "entering CDROM_RESET\n");
		if (!CDROM_CAN(CDC_RESET))
			return -ENOSYS;
		return cdo->reset(cdi);
		}

	case CDROM_LOCKDOOR: {
		cdinfo(CD_DO_IOCTL, "%socking door.\n", arg ? "L" : "Unl");
		if (!CDROM_CAN(CDC_LOCK))
			return -EDRIVE_CANT_DO_THIS;
		keeplocked = arg ? 1 : 0;
		/* don't unlock the door on multiple opens,but allow root
		 * to do so */
		if ((cdi->use_count != 1) && !arg && !capable(CAP_SYS_ADMIN))
			return -EBUSY;
		return cdo->lock_door(cdi, arg);
		}

	case CDROM_DEBUG: {
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		cdinfo(CD_DO_IOCTL, "%sabling debug.\n", arg ? "En" : "Dis");
		debug = arg ? 1 : 0;
		return debug;
		}

	case CDROM_GET_CAPABILITY: {
		cdinfo(CD_DO_IOCTL, "entering CDROM_GET_CAPABILITY\n");
		return (cdo->capability & ~cdi->mask);
		}

/* The following function is implemented, although very few audio
 * discs give Universal Product Code information, which should just be
 * the Medium Catalog Number on the box.  Note, that the way the code
 * is written on the CD is /not/ uniform across all discs!
 */
	case CDROM_GET_MCN: {
		struct cdrom_mcn mcn;
		cdinfo(CD_DO_IOCTL, "entering CDROM_GET_MCN\n"); 
		if (!(cdo->capability & CDC_MCN))
			return -ENOSYS;
		if ((ret=cdo->get_mcn(cdi, &mcn)))
			return ret;
		IOCTL_OUT(arg, struct cdrom_mcn, mcn);
		cdinfo(CD_DO_IOCTL, "CDROM_GET_MCN successful\n"); 
		return 0;
		}

	case CDROM_DRIVE_STATUS: {
		cdinfo(CD_DO_IOCTL, "entering CDROM_DRIVE_STATUS\n"); 
		if (!(cdo->capability & CDC_DRIVE_STATUS))
			return -ENOSYS;
		if (!CDROM_CAN(CDC_SELECT_DISC))
			return cdo->drive_status(cdi, CDSL_CURRENT);
                if ((arg == CDSL_CURRENT) || (arg == CDSL_NONE)) 
			return cdo->drive_status(cdi, CDSL_CURRENT);
		if (((int)arg >= cdi->capacity))
			return -EINVAL;
		return cdrom_slot_status(cdi, arg);
		}

	/* Ok, this is where problems start.  The current interface for the
	   CDROM_DISC_STATUS ioctl is flawed.  It makes the false assumption
	   that CDs are all CDS_DATA_1 or all CDS_AUDIO, etc.  Unfortunatly,
	   while this is often the case, it is also very common for CDs to
	   have some tracks with data, and some tracks with audio.  Just 
	   because I feel like it, I declare the following to be the best
	   way to cope.  If the CD has ANY data tracks on it, it will be
	   returned as a data CD.  If it has any XA tracks, I will return
	   it as that.  Now I could simplify this interface by combining these 
	   returns with the above, but this more clearly demonstrates
	   the problem with the current interface.  Too bad this wasn't 
	   designed to use bitmasks...         -Erik 

	   Well, now we have the option CDS_MIXED: a mixed-type CD. 
	   User level programmers might feel the ioctl is not very useful.
	   					---david
	*/
	case CDROM_DISC_STATUS: {
		tracktype tracks;
		cdinfo(CD_DO_IOCTL, "entering CDROM_DISC_STATUS\n"); 
		cdrom_count_tracks(cdi, &tracks);
		if (tracks.error) 
			return(tracks.error);

		/* Policy mode on */
		if (tracks.audio > 0) {
			if (tracks.data==0 && tracks.cdi==0 && tracks.xa==0) 
				return CDS_AUDIO;
			else
				return CDS_MIXED;
		}
		if (tracks.cdi > 0) return CDS_XA_2_2;
		if (tracks.xa > 0) return CDS_XA_2_1;
		if (tracks.data > 0) return CDS_DATA_1;
		/* Policy mode off */

		cdinfo(CD_WARNING,"This disc doesn't have any tracks I recognize!\n");
		return CDS_NO_INFO;
		}

	case CDROM_CHANGER_NSLOTS: {
		cdinfo(CD_DO_IOCTL, "entering CDROM_CHANGER_NSLOTS\n"); 
		return cdi->capacity;
		}
	}

	/* use the ioctls that are implemented through the generic_packet()
	   interface. this may look at bit funny, but if -ENOTTY is
	   returned that particular ioctl is not implemented and we
	   let it go through the device specific ones. */
	if (CDROM_CAN(CDC_GENERIC_PACKET)) {
		ret = mmc_ioctl(cdi, cmd, arg);
		if (ret != -ENOTTY) {
			return ret;
		}
	}

	/* note: most of the cdinfo() calls are commented out here,
	   because they fill up the sys log when CD players poll
	   the drive. */
	switch (cmd) {
	case CDROMSUBCHNL: {
		struct cdrom_subchnl q;
		u_char requested, back;
		if (!CDROM_CAN(CDC_PLAY_AUDIO))
			return -ENOSYS;
		/* cdinfo(CD_DO_IOCTL,"entering CDROMSUBCHNL\n");*/ 
		IOCTL_IN(arg, struct cdrom_subchnl, q);
		requested = q.cdsc_format;
		if (!((requested == CDROM_MSF) ||
		      (requested == CDROM_LBA)))
			return -EINVAL;
		q.cdsc_format = CDROM_MSF;
		if ((ret=cdo->audio_ioctl(cdi, cmd, &q)))
			return ret;
		back = q.cdsc_format; /* local copy */
		sanitize_format(&q.cdsc_absaddr, &back, requested);
		sanitize_format(&q.cdsc_reladdr, &q.cdsc_format, requested);
		IOCTL_OUT(arg, struct cdrom_subchnl, q);
		/* cdinfo(CD_DO_IOCTL, "CDROMSUBCHNL successful\n"); */ 
		return 0;
		}
	case CDROMREADTOCHDR: {
		struct cdrom_tochdr header;
		if (!CDROM_CAN(CDC_PLAY_AUDIO))
			return -ENOSYS;
		/* cdinfo(CD_DO_IOCTL, "entering CDROMREADTOCHDR\n"); */ 
		IOCTL_IN(arg, struct cdrom_tochdr, header);
		if ((ret=cdo->audio_ioctl(cdi, cmd, &header)))
			return ret;
		IOCTL_OUT(arg, struct cdrom_tochdr, header);
		/* cdinfo(CD_DO_IOCTL, "CDROMREADTOCHDR successful\n"); */ 
		return 0;
		}
	case CDROMREADTOCENTRY: {
		struct cdrom_tocentry entry;
		u_char requested_format;
		if (!CDROM_CAN(CDC_PLAY_AUDIO))
			return -ENOSYS;
		/* cdinfo(CD_DO_IOCTL, "entering CDROMREADTOCENTRY\n"); */ 
		IOCTL_IN(arg, struct cdrom_tocentry, entry);
		requested_format = entry.cdte_format;
		if (!((requested_format == CDROM_MSF) || 
			(requested_format == CDROM_LBA)))
				return -EINVAL;
		/* make interface to low-level uniform */
		entry.cdte_format = CDROM_MSF;
		if ((ret=cdo->audio_ioctl(cdi, cmd, &entry)))
			return ret;
		sanitize_format(&entry.cdte_addr,
		&entry.cdte_format, requested_format);
		IOCTL_OUT(arg, struct cdrom_tocentry, entry);
		/* cdinfo(CD_DO_IOCTL, "CDROMREADTOCENTRY successful\n"); */ 
		return 0;
		}
	case CDROMPLAYMSF: {
		struct cdrom_msf msf;
		if (!CDROM_CAN(CDC_PLAY_AUDIO))
			return -ENOSYS;
		cdinfo(CD_DO_IOCTL, "entering CDROMPLAYMSF\n"); 
		IOCTL_IN(arg, struct cdrom_msf, msf);
		return cdo->audio_ioctl(cdi, cmd, &msf);
		}
	case CDROMPLAYTRKIND: {
		struct cdrom_ti ti;
		if (!CDROM_CAN(CDC_PLAY_AUDIO))
			return -ENOSYS;
		cdinfo(CD_DO_IOCTL, "entering CDROMPLAYTRKIND\n"); 
		IOCTL_IN(arg, struct cdrom_ti, ti);
		CHECKAUDIO;
		return cdo->audio_ioctl(cdi, cmd, &ti);
		}
	case CDROMVOLCTRL: {
		struct cdrom_volctrl volume;
		if (!CDROM_CAN(CDC_PLAY_AUDIO))
			return -ENOSYS;
		cdinfo(CD_DO_IOCTL, "entering CDROMVOLCTRL\n"); 
		IOCTL_IN(arg, struct cdrom_volctrl, volume);
		return cdo->audio_ioctl(cdi, cmd, &volume);
		}
	case CDROMVOLREAD: {
		struct cdrom_volctrl volume;
		if (!CDROM_CAN(CDC_PLAY_AUDIO))
			return -ENOSYS;
		cdinfo(CD_DO_IOCTL, "entering CDROMVOLREAD\n"); 
		if ((ret=cdo->audio_ioctl(cdi, cmd, &volume)))
			return ret;
		IOCTL_OUT(arg, struct cdrom_volctrl, volume);
		return 0;
		}
	case CDROMSTART:
	case CDROMSTOP:
	case CDROMPAUSE:
	case CDROMRESUME: {
		if (!CDROM_CAN(CDC_PLAY_AUDIO))
			return -ENOSYS;
		cdinfo(CD_DO_IOCTL, "doing audio ioctl (start/stop/pause/resume)\n"); 
		CHECKAUDIO;
		return cdo->audio_ioctl(cdi, cmd, NULL);
		}
	} /* switch */

	/* do the device specific ioctls */
	if (CDROM_CAN(CDC_IOCTLS))
		return cdo->dev_ioctl(cdi, cmd, arg);
	
	return -ENOSYS;
}

static inline
int msf_to_lba(char m, char s, char f)
{
	return (((m * CD_SECS) + s) * CD_FRAMES + f) - CD_MSF_OFFSET;
}

/*
 * Required when we need to use READ_10 to issue other than 2048 block
 * reads
 */
static int cdrom_switch_blocksize(struct cdrom_device_info *cdi, int size)
{
	struct cdrom_device_ops *cdo = cdi->ops;
	struct cdrom_generic_command cgc;
	struct modesel_head mh;

	memset(&mh, 0, sizeof(mh));
	mh.block_desc_length = 0x08;
	mh.block_length_med = (size >> 8) & 0xff;
	mh.block_length_lo = size & 0xff;

	memset(&cgc, 0, sizeof(cgc));
	cgc.cmd[0] = 0x15;
	cgc.cmd[1] = 1 << 4;
	cgc.cmd[4] = 12;
	cgc.buflen = sizeof(mh);
	cgc.buffer = (char *) &mh;
	cgc.data_direction = CGC_DATA_WRITE;
	mh.block_desc_length = 0x08;
	mh.block_length_med = (size >> 8) & 0xff;
	mh.block_length_lo = size & 0xff;

	return cdo->generic_packet(cdi, &cgc);
}

static int cdrom_do_cmd(struct cdrom_device_info *cdi,
			struct cdrom_generic_command *cgc)
{
	struct request_sense *usense, sense;
	unsigned char *ubuf;
	int ret;

	if (cgc->data_direction == CGC_DATA_UNKNOWN)
		return -EINVAL;

	if (cgc->buflen < 0 || cgc->buflen >= 131072)
		return -EINVAL;

	if ((ubuf = cgc->buffer)) {
		cgc->buffer = kmalloc(cgc->buflen, GFP_KERNEL);
		if (cgc->buffer == NULL)
			return -ENOMEM;
	}

	usense = cgc->sense;
	cgc->sense = &sense;
	if (usense && !access_ok(VERIFY_WRITE, usense, sizeof(*usense)))
		return -EFAULT;

	if (cgc->data_direction == CGC_DATA_READ) {
		if (!access_ok(VERIFY_READ, ubuf, cgc->buflen))
			return -EFAULT;
	} else if (cgc->data_direction == CGC_DATA_WRITE) {
		if (copy_from_user(cgc->buffer, ubuf, cgc->buflen)) {
			kfree(cgc->buffer);
			return -EFAULT;
		}
	}

	ret = cdi->ops->generic_packet(cdi, cgc);
	__copy_to_user(usense, cgc->sense, sizeof(*usense));
	if (!ret && cgc->data_direction == CGC_DATA_READ)
		__copy_to_user(ubuf, cgc->buffer, cgc->buflen);
	kfree(cgc->buffer);
	return ret;
}

static int mmc_ioctl(struct cdrom_device_info *cdi, unsigned int cmd,
		     unsigned long arg)
{		
	struct cdrom_device_ops *cdo = cdi->ops;
	struct cdrom_generic_command cgc;
	kdev_t dev = cdi->dev;
	char buffer[32];
	int ret = 0;

	memset(&cgc, 0, sizeof(cgc));

	/* build a unified command and queue it through
	   cdo->generic_packet() */
	switch (cmd) {
	case CDROMREADRAW:
	case CDROMREADMODE1:
	case CDROMREADMODE2: {
		struct cdrom_msf msf;
		int blocksize = 0, format = 0, lba;
		
		switch (cmd) {
		case CDROMREADRAW:
			blocksize = CD_FRAMESIZE_RAW;
			break;
		case CDROMREADMODE1:
			blocksize = CD_FRAMESIZE;
			format = 2;
			break;
		case CDROMREADMODE2:
			blocksize = CD_FRAMESIZE_RAW0;
			break;
		}
		IOCTL_IN(arg, struct cdrom_msf, msf);
		lba = msf_to_lba(msf.cdmsf_min0,msf.cdmsf_sec0,msf.cdmsf_frame0);
		/* FIXME: we need upper bound checking, too!! */
		if (lba < 0)
			return -EINVAL;
		cgc.buffer = (char *) kmalloc(blocksize, GFP_KERNEL);
		if (cgc.buffer == NULL)
			return -ENOMEM;
		cgc.data_direction = CGC_DATA_READ;
		ret = cdrom_read_block(cdi, &cgc, lba, 1, format, blocksize);
		if (ret) {
			/*
			 * SCSI-II devices are not required to support
			 * READ_CD, so let's try switching block size
			 */
			/* FIXME: switch back again... */
			if ((ret = cdrom_switch_blocksize(cdi, blocksize))) {
				kfree(cgc.buffer);
				return ret;
			}
			cgc.sense = NULL;
			ret = cdrom_read_cd(cdi, &cgc, lba, blocksize, 1);
			ret |= cdrom_switch_blocksize(cdi, blocksize);
		}
		if (!ret && copy_to_user((char *)arg, cgc.buffer, blocksize))
			ret = -EFAULT;
		kfree(cgc.buffer);
		return ret;
		}
	case CDROMREADAUDIO: {
		struct cdrom_read_audio ra;
		int lba;

		IOCTL_IN(arg, struct cdrom_read_audio, ra);

		if (ra.addr_format == CDROM_MSF)
			lba = msf_to_lba(ra.addr.msf.minute,
					 ra.addr.msf.second,
					 ra.addr.msf.frame);
		else if (ra.addr_format == CDROM_LBA)
			lba = ra.addr.lba;
		else
			return -EINVAL;

		/* FIXME: we need upper bound checking, too!! */
		if (lba < 0 || ra.nframes <= 0)
			return -EINVAL;

		if ((cgc.buffer = (char *) kmalloc(CD_FRAMESIZE_RAW, GFP_KERNEL)) == NULL)
			return -ENOMEM;

		if (!access_ok(VERIFY_WRITE, ra.buf, ra.nframes*CD_FRAMESIZE_RAW)) {
			kfree(cgc.buffer);
			return -EFAULT;
		}
		cgc.data_direction = CGC_DATA_READ;
		while (ra.nframes > 0) {
			ret = cdrom_read_block(cdi, &cgc, lba, 1, 1, CD_FRAMESIZE_RAW);
			if (ret) break;
			__copy_to_user(ra.buf, cgc.buffer, CD_FRAMESIZE_RAW);
			ra.buf += CD_FRAMESIZE_RAW;
			ra.nframes--;
			lba++;
		}
		kfree(cgc.buffer);
		return ret;
		}
	case CDROMSUBCHNL: {
		struct cdrom_subchnl q;
		u_char requested, back;
		IOCTL_IN(arg, struct cdrom_subchnl, q);
		requested = q.cdsc_format;
		if (!((requested == CDROM_MSF) ||
		      (requested == CDROM_LBA)))
			return -EINVAL;
		q.cdsc_format = CDROM_MSF;
		if ((ret = cdrom_read_subchannel(cdi, &q, 0)))
			return ret;
		back = q.cdsc_format; /* local copy */
		sanitize_format(&q.cdsc_absaddr, &back, requested);
		sanitize_format(&q.cdsc_reladdr, &q.cdsc_format, requested);
		IOCTL_OUT(arg, struct cdrom_subchnl, q);
		/* cdinfo(CD_DO_IOCTL, "CDROMSUBCHNL successful\n"); */ 
		return 0;
		}
	case CDROMPLAYMSF: {
		struct cdrom_msf msf;
		cdinfo(CD_DO_IOCTL, "entering CDROMPLAYMSF\n");
		IOCTL_IN(arg, struct cdrom_msf, msf);
		cgc.cmd[0] = GPCMD_PLAY_AUDIO_MSF;
		cgc.cmd[3] = msf.cdmsf_min0;
		cgc.cmd[4] = msf.cdmsf_sec0;
		cgc.cmd[5] = msf.cdmsf_frame0;
		cgc.cmd[6] = msf.cdmsf_min1;
		cgc.cmd[7] = msf.cdmsf_sec1;
		cgc.cmd[8] = msf.cdmsf_frame1;
		cgc.data_direction = CGC_DATA_NONE;
		return cdo->generic_packet(cdi, &cgc);
		}
	case CDROMPLAYBLK: {
		struct cdrom_blk blk;
		cdinfo(CD_DO_IOCTL, "entering CDROMPLAYBLK\n");
		IOCTL_IN(arg, struct cdrom_blk, blk);
		cgc.cmd[0] = GPCMD_PLAY_AUDIO_10;
		cgc.cmd[2] = (blk.from >> 24) & 0xff;
		cgc.cmd[3] = (blk.from >> 16) & 0xff;
		cgc.cmd[4] = (blk.from >>  8) & 0xff;
		cgc.cmd[5] = blk.from & 0xff;
		cgc.cmd[7] = (blk.len >> 8) & 0xff;
		cgc.cmd[8] = blk.len & 0xff;
		cgc.data_direction = CGC_DATA_NONE;
		return cdo->generic_packet(cdi, &cgc);
		}
	case CDROMVOLCTRL:
	case CDROMVOLREAD: {
		struct cdrom_volctrl volctrl;
		char mask[32];
		unsigned short offset;
		cdinfo(CD_DO_IOCTL, "entering CDROMVOLUME\n");

		IOCTL_IN(arg, struct cdrom_volctrl, volctrl);

		cgc.buffer = buffer;
		cgc.buflen = 24;
		if ((ret = cdrom_mode_sense(cdi, &cgc, GPMODE_AUDIO_CTL_PAGE, 0)))
		    return ret;
		
		/* some drives have longer pages, adjust and reread. */
		if (buffer[1] > cgc.buflen) {
			cgc.buflen = buffer[1] + 2;
			if ((ret = cdrom_mode_sense(cdi, &cgc, 
					GPMODE_AUDIO_CTL_PAGE, 0))) 
			    return ret;
		}
		
		/* get the offset from the length of the page. length
		   is measure from byte 2 an on, thus the 14. */
		offset = buffer[1] - 14;

		/* now we have the current volume settings. if it was only
		   a CDROMVOLREAD, return these values */
		if (cmd == CDROMVOLREAD) {
			volctrl.channel0 = buffer[offset+9];
			volctrl.channel1 = buffer[offset+11];
			volctrl.channel2 = buffer[offset+13];
			volctrl.channel3 = buffer[offset+15];
			IOCTL_OUT(arg, struct cdrom_volctrl, volctrl);
			return 0;
		}
		
		/* get the volume mask */
		cgc.buffer = mask;
		if ((ret = cdrom_mode_sense(cdi, &cgc, 
				GPMODE_AUDIO_CTL_PAGE, 1)))
			return ret;

		buffer[offset+9] = volctrl.channel0 & mask[offset+9];
		buffer[offset+11] = volctrl.channel1 & mask[offset+11];
		buffer[offset+13] = volctrl.channel2 & mask[offset+13];
		buffer[offset+15] = volctrl.channel3 & mask[offset+15];

		/* set volume */
		cgc.buffer = buffer;
		return cdrom_mode_select(cdi, &cgc);
		}

	case CDROMSTART:
	case CDROMSTOP: {
		cdinfo(CD_DO_IOCTL, "entering CDROMSTART/CDROMSTOP\n"); 
		cgc.cmd[0] = GPCMD_START_STOP_UNIT;
		cgc.cmd[1] = 1;
		cgc.cmd[4] = (cmd == CDROMSTART) ? 1 : 0;
		cgc.data_direction = CGC_DATA_NONE;
		return cdo->generic_packet(cdi, &cgc);
		}

	case CDROMPAUSE:
	case CDROMRESUME: {
		cdinfo(CD_DO_IOCTL, "entering CDROMPAUSE/CDROMRESUME\n"); 
		cgc.cmd[0] = GPCMD_PAUSE_RESUME;
		cgc.cmd[8] = (cmd == CDROMRESUME) ? 1 : 0;
		cgc.data_direction = CGC_DATA_NONE;
		return cdo->generic_packet(cdi, &cgc);
		}

	case DVD_READ_STRUCT: {
		dvd_struct *s;
		int size = sizeof(dvd_struct);
		if (!CDROM_CAN(CDC_DVD))
			return -ENOSYS;
		if ((s = (dvd_struct *) kmalloc(size, GFP_KERNEL)) == NULL)
			return -ENOMEM;
		cdinfo(CD_DO_IOCTL, "entering DVD_READ_STRUCT\n"); 
		if (copy_from_user(s, (dvd_struct *)arg, size)) {
			kfree(s);
			return -EFAULT;
		}
		if ((ret = dvd_read_struct(cdi, s))) {
			kfree(s);
			return ret;
		}
		if (copy_to_user((dvd_struct *)arg, s, size))
			ret = -EFAULT;
		kfree(s);
		return ret;
		}

	case DVD_AUTH: {
		dvd_authinfo ai;
		if (!CDROM_CAN(CDC_DVD))
			return -ENOSYS;
		cdinfo(CD_DO_IOCTL, "entering DVD_AUTH\n"); 
		IOCTL_IN(arg, dvd_authinfo, ai);
		if ((ret = dvd_do_auth (cdi, &ai)))
			return ret;
		IOCTL_OUT(arg, dvd_authinfo, ai);
		return 0;
		}

	case CDROM_SEND_PACKET: {
		if (!CDROM_CAN(CDC_GENERIC_PACKET))
			return -ENOSYS;
		cdinfo(CD_DO_IOCTL, "entering CDROM_SEND_PACKET\n"); 
		IOCTL_IN(arg, struct cdrom_generic_command, cgc);
		return cdrom_do_cmd(cdi, &cgc);
		}
	case CDROM_NEXT_WRITABLE: {
		long next = 0;
		cdinfo(CD_DO_IOCTL, "entering CDROM_NEXT_WRITABLE\n"); 
		if ((ret = cdrom_get_next_writable(dev, &next)))
			return ret;
		IOCTL_OUT(arg, long, next);
		return 0;
		}
	case CDROM_LAST_WRITTEN: {
		long last = 0;
		cdinfo(CD_DO_IOCTL, "entering CDROM_LAST_WRITTEN\n"); 
		if ((ret = cdrom_get_last_written(dev, &last)))
			return ret;
		IOCTL_OUT(arg, long, last);
		return 0;
		}
	} /* switch */

	return -ENOTTY;
}

int cdrom_get_track_info(kdev_t dev, __u16 track, __u8 type,
			 track_information *ti)
{
        struct cdrom_device_info *cdi = cdrom_find_device(dev);
	struct cdrom_device_ops *cdo = cdi->ops;
	struct cdrom_generic_command cgc;
	int ret;

	init_cdrom_command(&cgc, ti, 8, CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_READ_TRACK_RZONE_INFO;
	cgc.cmd[1] = type & 3;
	cgc.cmd[4] = (track & 0xff00) >> 8;
	cgc.cmd[5] = track & 0xff;
	cgc.cmd[8] = 8;
	cgc.quiet = 1;

	if ((ret = cdo->generic_packet(cdi, &cgc)))
		return ret;
	
	cgc.cmd[8] = cgc.buflen = be16_to_cpu(ti->track_information_length) +
		     sizeof(ti->track_information_length);
	return cdo->generic_packet(cdi, &cgc);
}

/* requires CD R/RW */
int cdrom_get_disc_info(kdev_t dev, disc_information *di)
{
	struct cdrom_device_info *cdi = cdrom_find_device(dev);
	struct cdrom_device_ops *cdo = cdi->ops;
	struct cdrom_generic_command cgc;
	int ret;

	/* set up command and get the disc info */
	init_cdrom_command(&cgc, di, sizeof(*di), CGC_DATA_READ);
	cgc.cmd[0] = GPCMD_READ_DISC_INFO;
	cgc.cmd[8] = cgc.buflen = 2;
	cgc.quiet = 1;

	if ((ret = cdo->generic_packet(cdi, &cgc)))
		return ret;

	/* not all drives have the same disc_info length, so requeue
	 * packet with the length the drive tells us it can supply
	 */
	cgc.buflen = be16_to_cpu(di->disc_information_length) +
		     sizeof(di->disc_information_length);

	if (cgc.buflen > sizeof(disc_information))
		cgc.buflen = sizeof(disc_information);

	cgc.cmd[8] = cgc.buflen;
	return cdo->generic_packet(cdi, &cgc);
}


/* return the last written block on the CD-R media. this is for the udf
   file system. */
int cdrom_get_last_written(kdev_t dev, long *last_written)
{	
	struct cdrom_device_info *cdi = cdrom_find_device(dev);
	struct cdrom_tocentry toc;
	disc_information di;
	track_information ti;
	__u32 last_track;
	int ret = -1;

	if (!CDROM_CAN(CDC_GENERIC_PACKET))
		goto use_toc;

	if ((ret = cdrom_get_disc_info(dev, &di)))
		goto use_toc;

	last_track = (di.last_track_msb << 8) | di.last_track_lsb;
	if ((ret = cdrom_get_track_info(dev, last_track, 1, &ti)))
		goto use_toc;

	/* if this track is blank, try the previous. */
	if (ti.blank) {
		last_track--;
		if ((ret = cdrom_get_track_info(dev, last_track, 1, &ti)))
			goto use_toc;
	}

	/* if last recorded field is valid, return it. */
	if (ti.lra_v) {
		*last_written = be32_to_cpu(ti.last_rec_address);
	} else {
		/* make it up instead */
		*last_written = be32_to_cpu(ti.track_start) +
				be32_to_cpu(ti.track_size);
		if (ti.free_blocks)
			*last_written -= (be32_to_cpu(ti.free_blocks) + 7);
	}
	return 0;

	/* this is where we end up if the drive either can't do a
	   GPCMD_READ_DISC_INFO or GPCMD_READ_TRACK_RZONE_INFO or if
	   it fails. then we return the toc contents. */
use_toc:
	toc.cdte_format = CDROM_MSF;
	toc.cdte_track = CDROM_LEADOUT;
	if (cdi->ops->audio_ioctl(cdi, CDROMREADTOCENTRY, &toc))
		return ret;
	sanitize_format(&toc.cdte_addr, &toc.cdte_format, CDROM_LBA);
	*last_written = toc.cdte_addr.lba;
	return 0;
}

/* return the next writable block. also for udf file system. */
int cdrom_get_next_writable(kdev_t dev, long *next_writable)
{
	struct cdrom_device_info *cdi = cdrom_find_device(dev);
	disc_information di;
	track_information ti;
	__u16 last_track;
	int ret = -1;

	if (!CDROM_CAN(CDC_GENERIC_PACKET))
		goto use_last_written;

	if ((ret = cdrom_get_disc_info(dev, &di)))
		goto use_last_written;

	last_track = (di.last_track_msb << 8) | di.last_track_lsb;
	if ((ret = cdrom_get_track_info(dev, last_track, 1, &ti)))
		goto use_last_written;

        /* if this track is blank, try the previous. */
	if (ti.blank) {
		last_track--;
		if ((ret = cdrom_get_track_info(dev, last_track, 1, &ti)))
			goto use_last_written;
	}

	/* if next recordable address field is valid, use it. */
	if (ti.nwa_v)
		*next_writable = be32_to_cpu(ti.next_writable);
	else
		goto use_last_written;

	return 0;

use_last_written:
	if ((ret = cdrom_get_last_written(dev, next_writable))) {
		*next_writable = 0;
		return ret;
	} else {
		*next_writable += 7;
		return 0;
	}
}

EXPORT_SYMBOL(cdrom_get_disc_info);
EXPORT_SYMBOL(cdrom_get_track_info);
EXPORT_SYMBOL(cdrom_get_next_writable);
EXPORT_SYMBOL(cdrom_get_last_written);
EXPORT_SYMBOL(cdrom_count_tracks);
EXPORT_SYMBOL(register_cdrom);
EXPORT_SYMBOL(unregister_cdrom);
EXPORT_SYMBOL(cdrom_fops);
EXPORT_SYMBOL(cdrom_number_of_slots);
EXPORT_SYMBOL(cdrom_select_disc);
EXPORT_SYMBOL(cdrom_mode_select);
EXPORT_SYMBOL(cdrom_mode_sense);
EXPORT_SYMBOL(init_cdrom_command);
EXPORT_SYMBOL(cdrom_find_device);

#ifdef CONFIG_SYSCTL

#define CDROM_STR_SIZE 1000

struct cdrom_sysctl_settings {
	char	info[CDROM_STR_SIZE];	/* general info */
	int	autoclose;		/* close tray upon mount, etc */
	int	autoeject;		/* eject on umount */
	int	debug;			/* turn on debugging messages */
	int	lock;			/* lock the door on device open */
	int	check;			/* check media type */
} cdrom_sysctl_settings;

int cdrom_sysctl_info(ctl_table *ctl, int write, struct file * filp,
                           void *buffer, size_t *lenp)
{
        int pos;
	struct cdrom_device_info *cdi;
	char *info = cdrom_sysctl_settings.info;
	
	if (!*lenp || (filp->f_pos && !write)) {
		*lenp = 0;
		return 0;
	}

	pos = sprintf(info, "CD-ROM information, " VERSION "\n");
	
	pos += sprintf(info+pos, "\ndrive name:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%s", cdi->name);

	pos += sprintf(info+pos, "\ndrive speed:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", cdi->speed);

	pos += sprintf(info+pos, "\ndrive # of slots:");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", cdi->capacity);

	pos += sprintf(info+pos, "\nCan close tray:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_CLOSE_TRAY) != 0);

	pos += sprintf(info+pos, "\nCan open tray:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_OPEN_TRAY) != 0);

	pos += sprintf(info+pos, "\nCan lock tray:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_LOCK) != 0);

	pos += sprintf(info+pos, "\nCan change speed:");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_SELECT_SPEED) != 0);

	pos += sprintf(info+pos, "\nCan select disk:");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_SELECT_DISC) != 0);

	pos += sprintf(info+pos, "\nCan read multisession:");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_MULTI_SESSION) != 0);

	pos += sprintf(info+pos, "\nCan read MCN:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_MCN) != 0);

	pos += sprintf(info+pos, "\nReports media changed:");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_MEDIA_CHANGED) != 0);

	pos += sprintf(info+pos, "\nCan play audio:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_PLAY_AUDIO) != 0);

	pos += sprintf(info+pos, "\nCan write CD-R:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_CD_R) != 0);

	pos += sprintf(info+pos, "\nCan write CD-RW:");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_CD_RW) != 0);

	pos += sprintf(info+pos, "\nCan read DVD:\t");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_DVD) != 0);

	pos += sprintf(info+pos, "\nCan write DVD-R:");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_DVD_R) != 0);

	pos += sprintf(info+pos, "\nCan write DVD-RAM:");
	for (cdi=topCdromPtr;cdi!=NULL;cdi=cdi->next)
	    pos += sprintf(info+pos, "\t%d", CDROM_CAN(CDC_DVD_RAM) != 0);

	strcpy(info+pos,"\n\n");
		
        return proc_dostring(ctl, write, filp, buffer, lenp);
}

/* Unfortunately, per device settings are not implemented through
   procfs/sysctl yet. When they are, this will naturally disappear. For now
   just update all drives. Later this will become the template on which
   new registered drives will be based. */
void cdrom_update_settings(void)
{
	struct cdrom_device_info *cdi;

	for (cdi = topCdromPtr; cdi != NULL; cdi = cdi->next) {
		if (autoclose && CDROM_CAN(CDC_CLOSE_TRAY))
			cdi->options |= CDO_AUTO_CLOSE;
		else if (!autoclose)
			cdi->options &= ~CDO_AUTO_CLOSE;
		if (autoeject && CDROM_CAN(CDC_OPEN_TRAY))
			cdi->options |= CDO_AUTO_EJECT;
		else if (!autoeject)
			cdi->options &= ~CDO_AUTO_EJECT;
		if (lockdoor && CDROM_CAN(CDC_LOCK))
			cdi->options |= CDO_LOCK;
		else if (!lockdoor)
			cdi->options &= ~CDO_LOCK;
		if (check_media_type)
			cdi->options |= CDO_CHECK_TYPE;
		else
			cdi->options &= ~CDO_CHECK_TYPE;
	}
}

static int cdrom_sysctl_handler(ctl_table *ctl, int write, struct file * filp,
				void *buffer, size_t *lenp)
{
	int *valp = ctl->data;
	int val = *valp;
	int ret;
	
	ret = proc_dointvec(ctl, write, filp, buffer, lenp);

	if (write && *valp != val) {
	
		/* we only care for 1 or 0. */
		if (*valp)
			*valp = 1;
		else
			*valp = 0;

		switch (ctl->ctl_name) {
		case DEV_CDROM_AUTOCLOSE: {
			if (valp == &cdrom_sysctl_settings.autoclose)
				autoclose = cdrom_sysctl_settings.autoclose;
			break;
			}
		case DEV_CDROM_AUTOEJECT: {
			if (valp == &cdrom_sysctl_settings.autoeject)
				autoeject = cdrom_sysctl_settings.autoeject;
			break;
			}
		case DEV_CDROM_DEBUG: {
			if (valp == &cdrom_sysctl_settings.debug)
				debug = cdrom_sysctl_settings.debug;
			break;
			}
		case DEV_CDROM_LOCK: {
			if (valp == &cdrom_sysctl_settings.lock)
				lockdoor = cdrom_sysctl_settings.lock;
			break;
			}
		case DEV_CDROM_CHECK_MEDIA: {
			if (valp == &cdrom_sysctl_settings.check)
				check_media_type = cdrom_sysctl_settings.check;
			break;
			}
		}
		/* update the option flags according to the changes. we
		   don't have per device options through sysctl yet,
		   but we will have and then this will disappear. */
		cdrom_update_settings();
	}

        return ret;
}

/* Place files in /proc/sys/dev/cdrom */
ctl_table cdrom_table[] = {
	{DEV_CDROM_INFO, "info", &cdrom_sysctl_settings.info, 
		CDROM_STR_SIZE, 0444, NULL, &cdrom_sysctl_info},
	{DEV_CDROM_AUTOCLOSE, "autoclose", &cdrom_sysctl_settings.autoclose,
		sizeof(int), 0644, NULL, &cdrom_sysctl_handler },
	{DEV_CDROM_AUTOEJECT, "autoeject", &cdrom_sysctl_settings.autoeject,
		sizeof(int), 0644, NULL, &cdrom_sysctl_handler },
	{DEV_CDROM_DEBUG, "debug", &cdrom_sysctl_settings.debug,
		sizeof(int), 0644, NULL, &cdrom_sysctl_handler },
	{DEV_CDROM_LOCK, "lock", &cdrom_sysctl_settings.lock,
		sizeof(int), 0644, NULL, &cdrom_sysctl_handler },
	{DEV_CDROM_CHECK_MEDIA, "check_media", &cdrom_sysctl_settings.check,
		sizeof(int), 0644, NULL, &cdrom_sysctl_handler },
	{0}
	};

ctl_table cdrom_cdrom_table[] = {
	{DEV_CDROM, "cdrom", NULL, 0, 0555, cdrom_table},
	{0}
	};

/* Make sure that /proc/sys/dev is there */
ctl_table cdrom_root_table[] = {
#ifdef CONFIG_PROC_FS
	{CTL_DEV, "dev", NULL, 0, 0555, cdrom_cdrom_table},
#endif /* CONFIG_PROC_FS */
	{0}
	};
static struct ctl_table_header *cdrom_sysctl_header;

static void cdrom_sysctl_register(void)
{
	static int initialized;

	if (initialized == 1)
		return;

	cdrom_sysctl_header = register_sysctl_table(cdrom_root_table, 1);
	cdrom_root_table->child->de->owner = THIS_MODULE;

	/* set the defaults */
	cdrom_sysctl_settings.autoclose = autoclose;
	cdrom_sysctl_settings.autoeject = autoeject;
	cdrom_sysctl_settings.debug = debug;
	cdrom_sysctl_settings.lock = lockdoor;
	cdrom_sysctl_settings.check = check_media_type;

	initialized = 1;
}

static void cdrom_sysctl_unregister(void)
{
	unregister_sysctl_table(cdrom_sysctl_header);
}

#endif /* CONFIG_SYSCTL */

static int __init cdrom_init(void)
{
#ifdef CONFIG_SYSCTL
	cdrom_sysctl_register();
#endif
	devfs_handle = devfs_mk_dir(NULL, "cdroms", NULL);
	return 0;
}

static void __exit cdrom_exit(void)
{
	printk(KERN_INFO "Uniform CD-ROM driver unloaded\n");
#ifdef CONFIG_SYSCTL
	cdrom_sysctl_unregister();
#endif
	devfs_unregister(devfs_handle);
}

module_init(cdrom_init);
module_exit(cdrom_exit);
