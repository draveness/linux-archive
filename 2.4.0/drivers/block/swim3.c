/*
 * Driver for the SWIM3 (Super Woz Integrated Machine 3)
 * floppy controller found on Power Macintoshes.
 *
 * Copyright (C) 1996 Paul Mackerras.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/*
 * TODO:
 * handle 2 drives
 * handle GCR disks
 */

#include <linux/config.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/fd.h>
#include <linux/ioctl.h>
#include <asm/io.h>
#include <asm/dbdma.h>
#include <asm/prom.h>
#include <asm/uaccess.h>
#include <asm/mediabay.h>
#include <asm/feature.h>

#define MAJOR_NR	FLOPPY_MAJOR
#include <linux/blk.h>

static int floppy_blocksizes[2] = {512,512};
static int floppy_sizes[2] = {2880,2880};

#define MAX_FLOPPIES	2

enum swim_state {
	idle,
	locating,
	seeking,
	settling,
	do_transfer,
	jogging,
	available,
	revalidating,
	ejecting
};

#define REG(x)	unsigned char x; char x ## _pad[15];

/*
 * The names for these registers mostly represent speculation on my part.
 * It will be interesting to see how close they are to the names Apple uses.
 */
struct swim3 {
	REG(data);
	REG(timer);		/* counts down at 1MHz */
	REG(error);
	REG(mode);
	REG(select);		/* controls CA0, CA1, CA2 and LSTRB signals */
	REG(setup);
	REG(control);		/* writing bits clears them */
	REG(status);		/* writing bits sets them in control */
	REG(intr);
	REG(nseek);		/* # tracks to seek */
	REG(ctrack);		/* current track number */
	REG(csect);		/* current sector number */
	REG(gap3);		/* size of gap 3 in track format */
	REG(sector);		/* sector # to read or write */
	REG(nsect);		/* # sectors to read or write */
	REG(intr_enable);
};

#define control_bic	control
#define control_bis	status

/* Bits in select register */
#define CA_MASK		7
#define LSTRB		8

/* Bits in control register */
#define DO_SEEK		0x80
#define FORMAT		0x40
#define SELECT		0x20
#define WRITE_SECTORS	0x10
#define DO_ACTION	0x08
#define DRIVE2_ENABLE	0x04
#define DRIVE_ENABLE	0x02
#define INTR_ENABLE	0x01

/* Bits in status register */
#define FIFO_1BYTE	0x80
#define FIFO_2BYTE	0x40
#define ERROR		0x20
#define DATA		0x08
#define RDDATA		0x04
#define INTR_PENDING	0x02
#define MARK_BYTE	0x01

/* Bits in intr and intr_enable registers */
#define ERROR_INTR	0x20
#define DATA_CHANGED	0x10
#define TRANSFER_DONE	0x08
#define SEEN_SECTOR	0x04
#define SEEK_DONE	0x02
#define TIMER_DONE	0x01

/* Bits in error register */
#define ERR_DATA_CRC	0x80
#define ERR_ADDR_CRC	0x40
#define ERR_OVERRUN	0x04
#define ERR_UNDERRUN	0x01

/* Bits in setup register */
#define S_SW_RESET	0x80
#define S_GCR_WRITE	0x40
#define S_IBM_DRIVE	0x20
#define S_TEST_MODE	0x10
#define S_FCLK_DIV2	0x08
#define S_GCR		0x04
#define S_COPY_PROT	0x02
#define S_INV_WDATA	0x01

/* Select values for swim3_action */
#define SEEK_POSITIVE	0
#define SEEK_NEGATIVE	4
#define STEP		1
#define MOTOR_ON	2
#define MOTOR_OFF	6
#define INDEX		3
#define EJECT		7
#define SETMFM		9
#define SETGCR		13

/* Select values for swim3_select and swim3_readbit */
#define STEP_DIR	0
#define STEPPING	1
#define MOTOR_ON	2
#define RELAX		3	/* also eject in progress */
#define READ_DATA_0	4
#define TWOMEG_DRIVE	5
#define SINGLE_SIDED	6
#define DRIVE_PRESENT	7
#define DISK_IN		8
#define WRITE_PROT	9
#define TRACK_ZERO	10
#define TACHO		11
#define READ_DATA_1	12
#define MFM_MODE	13
#define SEEK_COMPLETE	14
#define ONEMEG_MEDIA	15

/* Definitions of values used in writing and formatting */
#define DATA_ESCAPE	0x99
#define GCR_SYNC_EXC	0x3f
#define GCR_SYNC_CONV	0x80
#define GCR_FIRST_MARK	0xd5
#define GCR_SECOND_MARK	0xaa
#define GCR_ADDR_MARK	"\xd5\xaa\x00"
#define GCR_DATA_MARK	"\xd5\xaa\x0b"
#define GCR_SLIP_BYTE	"\x27\xaa"
#define GCR_SELF_SYNC	"\x3f\xbf\x1e\x34\x3c\x3f"

#define DATA_99		"\x99\x99"
#define MFM_ADDR_MARK	"\x99\xa1\x99\xa1\x99\xa1\x99\xfe"
#define MFM_INDEX_MARK	"\x99\xc2\x99\xc2\x99\xc2\x99\xfc"
#define MFM_GAP_LEN	12

struct floppy_state {
	enum swim_state	state;
	volatile struct swim3 *swim3;	/* hardware registers */
	struct dbdma_regs *dma;	/* DMA controller registers */
	int	swim3_intr;	/* interrupt number for SWIM3 */
	int	dma_intr;	/* interrupt number for DMA channel */
	int	cur_cyl;	/* cylinder head is on, or -1 */
	int	cur_sector;	/* last sector we saw go past */
	int	req_cyl;	/* the cylinder for the current r/w request */
	int	head;		/* head number ditto */
	int	req_sector;	/* sector number ditto */
	int	scount;		/* # sectors we're transferring at present */
	int	retries;
	int	secpercyl;	/* disk geometry information */
	int	secpertrack;
	int	total_secs;
	int	write_prot;	/* 1 if write-protected, 0 if not, -1 dunno */
	struct dbdma_cmd *dma_cmd;
	int	ref_count;
	int	expect_cyl;
	struct timer_list timeout;
	int	timeout_pending;
	int	ejected;
	wait_queue_head_t wait;
	int	wanted;
	struct device_node*	media_bay; /* NULL when not in bay */
	char	dbdma_cmd_space[5 * sizeof(struct dbdma_cmd)];
};

static struct floppy_state floppy_states[MAX_FLOPPIES];
static int floppy_count = 0;

static unsigned short write_preamble[] = {
	0x4e4e, 0x4e4e, 0x4e4e, 0x4e4e, 0x4e4e,	/* gap field */
	0, 0, 0, 0, 0, 0,			/* sync field */
	0x99a1, 0x99a1, 0x99a1, 0x99fb,		/* data address mark */
	0x990f					/* no escape for 512 bytes */
};

static unsigned short write_postamble[] = {
	0x9904,					/* insert CRC */
	0x4e4e, 0x4e4e,
	0x9908,					/* stop writing */
	0, 0, 0, 0, 0, 0
};

static void swim3_select(struct floppy_state *fs, int sel);
static void swim3_action(struct floppy_state *fs, int action);
static int swim3_readbit(struct floppy_state *fs, int bit);
static void do_fd_request(request_queue_t * q);
static void start_request(struct floppy_state *fs);
static void set_timeout(struct floppy_state *fs, int nticks,
			void (*proc)(unsigned long));
static void scan_track(struct floppy_state *fs);
static void seek_track(struct floppy_state *fs, int n);
static void init_dma(struct dbdma_cmd *cp, int cmd, void *buf, int count);
static void setup_transfer(struct floppy_state *fs);
static void act(struct floppy_state *fs);
static void scan_timeout(unsigned long data);
static void seek_timeout(unsigned long data);
static void xfer_timeout(unsigned long data);
static void swim3_interrupt(int irq, void *dev_id, struct pt_regs *regs);
/*static void fd_dma_interrupt(int irq, void *dev_id, struct pt_regs *regs);*/
static int grab_drive(struct floppy_state *fs, enum swim_state state,
		      int interruptible);
static void release_drive(struct floppy_state *fs);
static int fd_eject(struct floppy_state *fs);
static int floppy_ioctl(struct inode *inode, struct file *filp,
			unsigned int cmd, unsigned long param);
static int floppy_open(struct inode *inode, struct file *filp);
static int floppy_release(struct inode *inode, struct file *filp);
static int floppy_check_change(kdev_t dev);
static int floppy_revalidate(kdev_t dev);
static int swim3_add_device(struct device_node *swims);
int swim3_init(void);

#ifndef CONFIG_PMAC_PBOOK
static inline int check_media_bay(struct device_node *which_bay, int what)
{
	return 1;
}
#endif

static void swim3_select(struct floppy_state *fs, int sel)
{
	volatile struct swim3 *sw = fs->swim3;

	out_8(&sw->select, RELAX);
	if (sel & 8)
		out_8(&sw->control_bis, SELECT);
	else
		out_8(&sw->control_bic, SELECT);
	out_8(&sw->select, sel & CA_MASK);
}

static void swim3_action(struct floppy_state *fs, int action)
{
	volatile struct swim3 *sw = fs->swim3;

	swim3_select(fs, action);
	udelay(1);
	out_8(&sw->select, sw->select | LSTRB);
	udelay(2);
	out_8(&sw->select, sw->select & ~LSTRB);
	udelay(1);
	out_8(&sw->select, RELAX);
}

static int swim3_readbit(struct floppy_state *fs, int bit)
{
	volatile struct swim3 *sw = fs->swim3;
	int stat;

	swim3_select(fs, bit);
	udelay(10);
	stat = in_8(&sw->status);
	out_8(&sw->select, RELAX);
	return (stat & DATA) == 0;
}

static void do_fd_request(request_queue_t * q)
{
	int i;
	for(i=0;i<floppy_count;i++)
	{
		if (floppy_states[i].media_bay &&
			check_media_bay(floppy_states[i].media_bay, MB_FD))
			continue;
		start_request(&floppy_states[i]);
	}
	sti();
}

static void start_request(struct floppy_state *fs)
{
	unsigned long x;

	if (fs->state == idle && fs->wanted) {
		fs->state = available;
		wake_up(&fs->wait);
		return;
	}
	while (!QUEUE_EMPTY && fs->state == idle) {
		if (MAJOR(CURRENT->rq_dev) != MAJOR_NR)
			panic(DEVICE_NAME ": request list destroyed");
		if (CURRENT->bh && !buffer_locked(CURRENT->bh))
			panic(DEVICE_NAME ": block not locked");
#if 0
		printk("do_fd_req: dev=%x cmd=%d sec=%ld nr_sec=%ld buf=%p\n",
		       kdev_t_to_nr(CURRENT->rq_dev), CURRENT->cmd,
		       CURRENT->sector, CURRENT->nr_sectors, CURRENT->buffer);
		printk("           rq_status=%d errors=%d current_nr_sectors=%ld\n",
		       CURRENT->rq_status, CURRENT->errors, CURRENT->current_nr_sectors);
#endif

		if (CURRENT->sector < 0 || CURRENT->sector >= fs->total_secs) {
			end_request(0);
			continue;
		}
		if (CURRENT->current_nr_sectors == 0) {
			end_request(1);
			continue;
		}
		if (fs->ejected) {
			end_request(0);
			continue;
		}

		if (CURRENT->cmd == WRITE) {
			if (fs->write_prot < 0)
				fs->write_prot = swim3_readbit(fs, WRITE_PROT);
			if (fs->write_prot) {
				end_request(0);
				continue;
			}
		}

		fs->req_cyl = CURRENT->sector / fs->secpercyl;
		x = CURRENT->sector % fs->secpercyl;
		fs->head = x / fs->secpertrack;
		fs->req_sector = x % fs->secpertrack + 1;
		fs->state = do_transfer;
		fs->retries = 0;

		act(fs);
	}
}

static void set_timeout(struct floppy_state *fs, int nticks,
			void (*proc)(unsigned long))
{
	unsigned long flags;

	save_flags(flags); cli();
	if (fs->timeout_pending)
		del_timer(&fs->timeout);
	fs->timeout.expires = jiffies + nticks;
	fs->timeout.function = proc;
	fs->timeout.data = (unsigned long) fs;
	add_timer(&fs->timeout);
	fs->timeout_pending = 1;
	restore_flags(flags);
}

static inline void scan_track(struct floppy_state *fs)
{
	volatile struct swim3 *sw = fs->swim3;
	int xx;

	swim3_select(fs, READ_DATA_0);
	xx = sw->intr;		/* clear SEEN_SECTOR bit */
	out_8(&sw->control_bis, DO_ACTION);
	/* enable intr when track found */
	out_8(&sw->intr_enable, ERROR_INTR | SEEN_SECTOR);
	set_timeout(fs, HZ, scan_timeout);	/* enable timeout */
}

static inline void seek_track(struct floppy_state *fs, int n)
{
	volatile struct swim3 *sw = fs->swim3;

	if (n >= 0) {
		swim3_action(fs, SEEK_POSITIVE);
		sw->nseek = n;
	} else {
		swim3_action(fs, SEEK_NEGATIVE);
		sw->nseek = -n;
	}
	fs->expect_cyl = (fs->cur_cyl > 0)? fs->cur_cyl + n: -1;
	swim3_select(fs, STEP);
	out_8(&sw->control_bis, DO_SEEK);
	/* enable intr when seek finished */
	out_8(&sw->intr_enable, ERROR_INTR | SEEK_DONE);
	set_timeout(fs, HZ/2, seek_timeout);	/* enable timeout */
}

static inline void init_dma(struct dbdma_cmd *cp, int cmd,
			    void *buf, int count)
{
	st_le16(&cp->req_count, count);
	st_le16(&cp->command, cmd);
	st_le32(&cp->phy_addr, virt_to_bus(buf));
	cp->xfer_status = 0;
}

static inline void setup_transfer(struct floppy_state *fs)
{
	int n;
	volatile struct swim3 *sw = fs->swim3;
	struct dbdma_cmd *cp = fs->dma_cmd;
	struct dbdma_regs *dr = fs->dma;

	if (CURRENT->current_nr_sectors <= 0) {
		printk(KERN_ERR "swim3: transfer 0 sectors?\n");
		return;
	}
	if (CURRENT->cmd == WRITE)
		n = 1;
	else {
		n = fs->secpertrack - fs->req_sector + 1;
		if (n > CURRENT->current_nr_sectors)
			n = CURRENT->current_nr_sectors;
	}
	fs->scount = n;
	swim3_select(fs, fs->head? READ_DATA_1: READ_DATA_0);
	out_8(&sw->sector, fs->req_sector);
	out_8(&sw->nsect, n);
	out_8(&sw->gap3, 0);
	st_le32(&dr->cmdptr, virt_to_bus(cp));
	if (CURRENT->cmd == WRITE) {
		/* Set up 3 dma commands: write preamble, data, postamble */
		init_dma(cp, OUTPUT_MORE, write_preamble, sizeof(write_preamble));
		++cp;
		init_dma(cp, OUTPUT_MORE, CURRENT->buffer, 512);
		++cp;
		init_dma(cp, OUTPUT_MORE, write_postamble, sizeof(write_postamble));
	} else {
		init_dma(cp, INPUT_MORE, CURRENT->buffer, n * 512);
	}
	++cp;
	out_le16(&cp->command, DBDMA_STOP);
	out_le32(&dr->control, (RUN << 16) | RUN);
	out_8(&sw->control_bis,
	      (CURRENT->cmd == WRITE? WRITE_SECTORS: 0) | DO_ACTION);
	/* enable intr when transfer complete */
	out_8(&sw->intr_enable, ERROR_INTR | TRANSFER_DONE);
	set_timeout(fs, 2*HZ, xfer_timeout);	/* enable timeout */
}

static void act(struct floppy_state *fs)
{
	volatile struct swim3 *sw = fs->swim3;

	for (;;) {
		switch (fs->state) {
		case idle:
			return;		/* XXX shouldn't get here */

		case locating:
			if (swim3_readbit(fs, TRACK_ZERO)) {
				fs->cur_cyl = 0;
				if (fs->req_cyl == 0)
					fs->state = do_transfer;
				else
					fs->state = seeking;
				break;
			}
			scan_track(fs);
			return;

		case seeking:
			if (fs->cur_cyl < 0) {
				fs->expect_cyl = -1;
				fs->state = locating;
				break;
			}
			if (fs->req_cyl == fs->cur_cyl) {
				printk("whoops, seeking 0\n");
				fs->state = do_transfer;
				break;
			}
			seek_track(fs, fs->req_cyl - fs->cur_cyl);
			return;

		case settling:
			/* wait for SEEK_COMPLETE to become true */
			swim3_select(fs, SEEK_COMPLETE);
			udelay(10);
			out_8(&sw->intr_enable, ERROR_INTR | DATA_CHANGED);
			in_8(&sw->intr);	/* clear DATA_CHANGED */
			if (in_8(&sw->status) & DATA) {
				/* seek_complete is not yet true */
				set_timeout(fs, HZ/2, seek_timeout);
				return;
			}
			out_8(&sw->intr_enable, 0);
			in_8(&sw->intr);
			fs->state = locating;
			break;

		case do_transfer:
			if (fs->cur_cyl != fs->req_cyl) {
				if (fs->retries > 5) {
					end_request(0);
					fs->state = idle;
					return;
				}
				fs->state = seeking;
				break;
			}
			setup_transfer(fs);
			return;

		case jogging:
			seek_track(fs, -5);
			return;

		default:
			printk(KERN_ERR"swim3: unknown state %d\n", fs->state);
			return;
		}
	}
}

static void scan_timeout(unsigned long data)
{
	struct floppy_state *fs = (struct floppy_state *) data;
	volatile struct swim3 *sw = fs->swim3;

	fs->timeout_pending = 0;
	out_8(&sw->control_bic, DO_ACTION);
	out_8(&sw->select, RELAX);
	out_8(&sw->intr_enable, 0);
	fs->cur_cyl = -1;
	if (fs->retries > 5) {
		end_request(0);
		fs->state = idle;
		start_request(fs);
	} else {
		fs->state = jogging;
		act(fs);
	}
}

static void seek_timeout(unsigned long data)
{
	struct floppy_state *fs = (struct floppy_state *) data;
	volatile struct swim3 *sw = fs->swim3;

	fs->timeout_pending = 0;
	if (fs->state == settling) {
		printk(KERN_ERR "swim3: MSI sel=%x ctrl=%x stat=%x intr=%x ie=%x\n",
		       sw->select, sw->control, sw->status, sw->intr, sw->intr_enable);
	}
	out_8(&sw->control_bic, DO_SEEK);
	out_8(&sw->select, RELAX);
	out_8(&sw->intr_enable, 0);
	if (fs->state == settling && swim3_readbit(fs, SEEK_COMPLETE)) {
		/* printk(KERN_DEBUG "swim3: missed settling interrupt\n"); */
		fs->state = locating;
		act(fs);
		return;
	}
	printk(KERN_ERR "swim3: seek timeout\n");
	end_request(0);
	fs->state = idle;
	start_request(fs);
}

static void xfer_timeout(unsigned long data)
{
	struct floppy_state *fs = (struct floppy_state *) data;
	volatile struct swim3 *sw = fs->swim3;
	struct dbdma_regs *dr = fs->dma;
	struct dbdma_cmd *cp = fs->dma_cmd;
	unsigned long s;

	fs->timeout_pending = 0;
	st_le32(&dr->control, RUN << 16);
	out_8(&sw->intr_enable, 0);
	out_8(&sw->control_bic, WRITE_SECTORS | DO_ACTION);
	out_8(&sw->select, RELAX);
	if (CURRENT->cmd == WRITE)
		++cp;
	if (ld_le16(&cp->xfer_status) != 0)
		s = fs->scount - ((ld_le16(&cp->res_count) + 511) >> 9);
	else
		s = 0;
	CURRENT->sector += s;
	CURRENT->current_nr_sectors -= s;
	printk(KERN_ERR "swim3: timeout %sing sector %ld\n",
	       (CURRENT->cmd==WRITE? "writ": "read"), CURRENT->sector);
	end_request(0);
	fs->state = idle;
	start_request(fs);
}

static void swim3_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct floppy_state *fs = (struct floppy_state *) dev_id;
	volatile struct swim3 *sw = fs->swim3;
	int intr, err, n;
	int stat, resid;
	struct dbdma_regs *dr;
	struct dbdma_cmd *cp;

	err = in_8(&sw->error);
	intr = in_8(&sw->intr);
#if 0
	printk("swim3 intr state=%d intr=%x err=%x\n", fs->state, intr, err);
#endif
	if ((intr & ERROR_INTR) && fs->state != do_transfer)
		printk(KERN_ERR "swim3_interrupt, state=%d, cmd=%x, intr=%x, err=%x\n",
		       fs->state, CURRENT->cmd, intr, err);
	switch (fs->state) {
	case locating:
		if (intr & SEEN_SECTOR) {
			out_8(&sw->control_bic, DO_ACTION);
			out_8(&sw->select, RELAX);
			out_8(&sw->intr_enable, 0);
			del_timer(&fs->timeout);
			fs->timeout_pending = 0;
			if (sw->ctrack == 0xff) {
				printk(KERN_ERR "swim3: seen sector but cyl=ff?\n");
				fs->cur_cyl = -1;
				if (fs->retries > 5) {
					end_request(0);
					fs->state = idle;
					start_request(fs);
				} else {
					fs->state = jogging;
					act(fs);
				}
				break;
			}
			fs->cur_cyl = sw->ctrack;
			fs->cur_sector = sw->csect;
			if (fs->expect_cyl != -1 && fs->expect_cyl != fs->cur_cyl)
				printk(KERN_ERR "swim3: expected cyl %d, got %d\n",
				       fs->expect_cyl, fs->cur_cyl);
			fs->state = do_transfer;
			act(fs);
		}
		break;
	case seeking:
	case jogging:
		if (sw->nseek == 0) {
			out_8(&sw->control_bic, DO_SEEK);
			out_8(&sw->select, RELAX);
			out_8(&sw->intr_enable, 0);
			del_timer(&fs->timeout);
			fs->timeout_pending = 0;
			if (fs->state == seeking)
				++fs->retries;
			fs->state = settling;
			act(fs);
		}
		break;
	case settling:
		out_8(&sw->intr_enable, 0);
		del_timer(&fs->timeout);
		fs->timeout_pending = 0;
		act(fs);
		break;
	case do_transfer:
		if ((intr & (ERROR_INTR | TRANSFER_DONE)) == 0)
			break;
		dr = fs->dma;
		cp = fs->dma_cmd;
		st_le32(&dr->control, RUN << 16);
		out_8(&sw->intr_enable, 0);
		out_8(&sw->control_bic, WRITE_SECTORS | DO_ACTION);
		out_8(&sw->select, RELAX);
		del_timer(&fs->timeout);
		fs->timeout_pending = 0;
		if (CURRENT->cmd == WRITE)
			++cp;
		stat = ld_le16(&cp->xfer_status);
		resid = ld_le16(&cp->res_count);
		if (intr & ERROR_INTR) {
			n = fs->scount - 1 - resid / 512;
			if (n > 0) {
				CURRENT->sector += n;
				CURRENT->current_nr_sectors -= n;
				CURRENT->buffer += n * 512;
				fs->req_sector += n;
			}
			if (fs->retries < 5) {
				++fs->retries;
				act(fs);
			} else {
				printk("swim3: error %sing block %ld (err=%x)\n",
				       CURRENT->cmd == WRITE? "writ": "read",
				       CURRENT->sector, err);
				end_request(0);
				fs->state = idle;
			}
		} else {
			if ((stat & ACTIVE) == 0 || resid != 0) {
				/* musta been an error */
				printk(KERN_ERR "swim3: fd dma: stat=%x resid=%d\n", stat, resid);
				printk(KERN_ERR "  state=%d, cmd=%x, intr=%x, err=%x\n",
				       fs->state, CURRENT->cmd, intr, err);
				end_request(0);
				fs->state = idle;
				start_request(fs);
				break;
			}
			CURRENT->sector += fs->scount;
			CURRENT->current_nr_sectors -= fs->scount;
			CURRENT->buffer += fs->scount * 512;
			if (CURRENT->current_nr_sectors <= 0) {
				end_request(1);
				fs->state = idle;
			} else {
				fs->req_sector += fs->scount;
				if (fs->req_sector > fs->secpertrack) {
					fs->req_sector -= fs->secpertrack;
					if (++fs->head > 1) {
						fs->head = 0;
						++fs->req_cyl;
					}
				}
				act(fs);
			}
		}
		if (fs->state == idle)
			start_request(fs);
		break;
	default:
		printk(KERN_ERR "swim3: don't know what to do in state %d\n", fs->state);
	}
}

/*
static void fd_dma_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
}
*/

static int grab_drive(struct floppy_state *fs, enum swim_state state,
		      int interruptible)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	if (fs->state != idle) {
		++fs->wanted;
		while (fs->state != available) {
			if (interruptible && signal_pending(current)) {
				--fs->wanted;
				restore_flags(flags);
				return -EINTR;
			}
			interruptible_sleep_on(&fs->wait);
		}
		--fs->wanted;
	}
	fs->state = state;
	restore_flags(flags);
	return 0;
}

static void release_drive(struct floppy_state *fs)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	fs->state = idle;
	start_request(fs);
	restore_flags(flags);
}

static int fd_eject(struct floppy_state *fs)
{
	int err, n;

	err = grab_drive(fs, ejecting, 1);
	if (err)
		return err;
	swim3_action(fs, EJECT);
	for (n = 2*HZ; n > 0; --n) {
		if (swim3_readbit(fs, RELAX))
			break;
		if (signal_pending(current)) {
			err = -EINTR;
			break;
		}
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(1);
	}
	fs->ejected = 1;
	release_drive(fs);
	return err;
}

int swim3_fd_eject(int devnum)
{
	if (devnum >= floppy_count)
		return -ENODEV;
	/* Do not check this - this function should ONLY be called early
	 * in the boot process! */
	/* if (floppy_states[devnum].ref_count != 1) return -EBUSY; */
	return fd_eject(&floppy_states[devnum]);
}

static struct floppy_struct floppy_type =
	{ 2880,18,2,80,0,0x1B,0x00,0xCF,0x6C,NULL };	/*  7 1.44MB 3.5"   */

static int floppy_ioctl(struct inode *inode, struct file *filp,
			unsigned int cmd, unsigned long param)
{
	struct floppy_state *fs;
	int err;
	int devnum = MINOR(inode->i_rdev);

	if (devnum >= floppy_count)
		return -ENODEV;
		
	if ((cmd & 0x80) && !suser())
		return -EPERM;

	fs = &floppy_states[devnum];

	if (fs->media_bay && check_media_bay(fs->media_bay, MB_FD))
		return -ENXIO;

	switch (cmd) {
	case FDEJECT:
		if (fs->ref_count != 1)
			return -EBUSY;
		err = fd_eject(fs);
		return err;
	case FDGETPRM:
	        err = copy_to_user((void *) param, (void *) &floppy_type,
				   sizeof(struct floppy_struct));
		return err;
	}
	return -ENOIOCTLCMD;
}

static int floppy_open(struct inode *inode, struct file *filp)
{
	struct floppy_state *fs;
	volatile struct swim3 *sw;
	int n, err;
	int devnum = MINOR(inode->i_rdev);

	if (devnum >= floppy_count)
		return -ENODEV;
	if (filp == 0)
		return -EIO;
		
	fs = &floppy_states[devnum];
	sw = fs->swim3;
	err = 0;
	if (fs->ref_count == 0) {
		if (fs->media_bay && check_media_bay(fs->media_bay, MB_FD))
			return -ENXIO;
		out_8(&sw->mode, 0x95);
		out_8(&sw->control_bic, 0xff);
		out_8(&sw->setup, S_IBM_DRIVE | S_FCLK_DIV2);
		udelay(10);
		out_8(&sw->intr_enable, 0);
		out_8(&sw->control_bis, DRIVE_ENABLE | INTR_ENABLE);
		swim3_action(fs, MOTOR_ON);
		fs->write_prot = -1;
		fs->cur_cyl = -1;
		for (n = HZ; n > 0; --n) {
			if (swim3_readbit(fs, SEEK_COMPLETE))
				break;
			if (signal_pending(current)) {
				err = -EINTR;
				break;
			}
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(1);
		}
		if (err == 0 && (swim3_readbit(fs, SEEK_COMPLETE) == 0
				 || swim3_readbit(fs, DISK_IN) == 0))
			err = -ENXIO;
		swim3_action(fs, 9);

	} else if (fs->ref_count == -1 || filp->f_flags & O_EXCL)
		return -EBUSY;

	if (err == 0 && (filp->f_flags & O_NDELAY) == 0
	    && (filp->f_mode & 3)) {
		check_disk_change(inode->i_rdev);
		if (fs->ejected)
			err = -ENXIO;
	}

	if (err == 0 && (filp->f_mode & 2)) {
		if (fs->write_prot < 0)
			fs->write_prot = swim3_readbit(fs, WRITE_PROT);
		if (fs->write_prot)
			err = -EROFS;
	}

	if (err) {
		if (fs->ref_count == 0) {
			swim3_action(fs, MOTOR_OFF);
			out_8(&sw->control_bic, DRIVE_ENABLE | INTR_ENABLE);
		}
		return err;
	}

	if (filp->f_flags & O_EXCL)
		fs->ref_count = -1;
	else
		++fs->ref_count;

	return 0;
}

static int floppy_release(struct inode *inode, struct file *filp)
{
	struct floppy_state *fs;
	volatile struct swim3 *sw;
	int devnum = MINOR(inode->i_rdev);

	if (devnum >= floppy_count)
		return -ENODEV;

	fs = &floppy_states[devnum];
	sw = fs->swim3;
	if (fs->ref_count > 0 && --fs->ref_count == 0) {
		swim3_action(fs, MOTOR_OFF);
		out_8(&sw->control_bic, 0xff);
	}
	return 0;
}

static int floppy_check_change(kdev_t dev)
{
	struct floppy_state *fs;
	int devnum = MINOR(dev);

	if (MAJOR(dev) != MAJOR_NR || (devnum >= floppy_count))
		return 0;
		
	fs = &floppy_states[devnum];
	return fs->ejected;
}

static int floppy_revalidate(kdev_t dev)
{
	struct floppy_state *fs;
	volatile struct swim3 *sw;
	int ret, n;
	int devnum = MINOR(dev);

	if (MAJOR(dev) != MAJOR_NR || (devnum >= floppy_count))
		return 0;

	fs = &floppy_states[devnum];

	if (fs->media_bay && check_media_bay(fs->media_bay, MB_FD))
		return -ENXIO;

	sw = fs->swim3;
	grab_drive(fs, revalidating, 0);
	out_8(&sw->intr_enable, 0);
	out_8(&sw->control_bis, DRIVE_ENABLE | INTR_ENABLE);
	swim3_action(fs, MOTOR_ON);
	fs->write_prot = -1;
	fs->cur_cyl = -1;
	for (n = HZ; n > 0; --n) {
		if (swim3_readbit(fs, SEEK_COMPLETE))
			break;
		if (signal_pending(current))
			break;
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(1);
	}
	ret = swim3_readbit(fs, SEEK_COMPLETE) == 0
		|| swim3_readbit(fs, DISK_IN) == 0;
	if (ret)
		swim3_action(fs, MOTOR_OFF);
	else {
		fs->ejected = 0;
		swim3_action(fs, 9);
	}

	release_drive(fs);
	return ret;
}

static void floppy_off(unsigned int nr)
{
}

static struct block_device_operations floppy_fops = {
	open:			floppy_open,
	release:		floppy_release,
	ioctl:			floppy_ioctl,
	check_media_change:	floppy_check_change,
	revalidate:		floppy_revalidate,
};

int swim3_init(void)
{
	struct device_node *swim;

	swim = find_devices("floppy");
	while (swim && (floppy_count < MAX_FLOPPIES))
	{
		swim3_add_device(swim);
		swim = swim->next;
	}

	swim = find_devices("swim3");
	while (swim && (floppy_count < MAX_FLOPPIES))
	{
		swim3_add_device(swim);
		swim = swim->next;
	}

	if (floppy_count > 0)
	{
		if (register_blkdev(MAJOR_NR, "fd", &floppy_fops)) {
			printk(KERN_ERR "Unable to get major %d for floppy\n",
			       MAJOR_NR);
			return -EBUSY;
		}
		blk_init_queue(BLK_DEFAULT_QUEUE(MAJOR_NR), DEVICE_REQUEST);
		blksize_size[MAJOR_NR] = floppy_blocksizes;
		blk_size[MAJOR_NR] = floppy_sizes;
	}

	return 0;
}

static int swim3_add_device(struct device_node *swim)
{
	struct device_node *mediabay;
	struct floppy_state *fs = &floppy_states[floppy_count];
	
	if (swim->n_addrs < 2)
	{
		printk(KERN_INFO "swim3: expecting 2 addrs (n_addrs:%d, n_intrs:%d)\n",
		       swim->n_addrs, swim->n_intrs);
		return -EINVAL;
	}

	if (swim->n_intrs < 2)
	{
		printk(KERN_INFO "swim3: expecting 2 intrs (n_addrs:%d, n_intrs:%d)\n",
		       swim->n_addrs, swim->n_intrs);
		return -EINVAL;
	}

	mediabay = (strcasecmp(swim->parent->type, "media-bay") == 0) ? swim->parent : NULL;
	if (mediabay == NULL)
		feature_set(swim, FEATURE_SWIM3_enable);
	
	memset(fs, 0, sizeof(*fs));
	fs->state = idle;
	fs->swim3 = (volatile struct swim3 *) ioremap(swim->addrs[0].address, 0x200);
	fs->dma = (struct dbdma_regs *) ioremap(swim->addrs[1].address, 0x200);
	fs->swim3_intr = swim->intrs[0].line;
	fs->dma_intr = swim->intrs[1].line;
	fs->cur_cyl = -1;
	fs->cur_sector = -1;
	fs->secpercyl = 36;
	fs->secpertrack = 18;
	fs->total_secs = 2880;
	fs->media_bay = mediabay;
	init_waitqueue_head(&fs->wait);

	fs->dma_cmd = (struct dbdma_cmd *) DBDMA_ALIGN(fs->dbdma_cmd_space);
	memset(fs->dma_cmd, 0, 2 * sizeof(struct dbdma_cmd));
	st_le16(&fs->dma_cmd[1].command, DBDMA_STOP);

	if (request_irq(fs->swim3_intr, swim3_interrupt, 0, "SWIM3", fs)) {
		printk(KERN_ERR "Couldn't get irq %d for SWIM3\n", fs->swim3_intr);
		feature_clear(swim, FEATURE_SWIM3_enable);
		return -EBUSY;
	}
/*
	if (request_irq(fs->dma_intr, fd_dma_interrupt, 0, "SWIM3-dma", fs)) {
		printk(KERN_ERR "Couldn't get irq %d for SWIM3 DMA",
		       fs->dma_intr);
		feature_clear(swim, FEATURE_SWIM3_enable);
		return -EBUSY;
	}
*/

	init_timer(&fs->timeout);

	do_floppy = NULL;

	printk(KERN_INFO "fd%d: SWIM3 floppy controller %s\n", floppy_count,
		mediabay ? "in media bay" : "");

	floppy_count++;
	
	return 0;
}
