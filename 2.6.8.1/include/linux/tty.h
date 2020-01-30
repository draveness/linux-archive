#ifndef _LINUX_TTY_H
#define _LINUX_TTY_H

/*
 * 'tty.h' defines some structures used by tty_io.c and some defines.
 */

/*
 * These constants are also useful for user-level apps (e.g., VC
 * resizing).
 */
#define MIN_NR_CONSOLES 1       /* must be at least 1 */
#define MAX_NR_CONSOLES	63	/* serial lines start at 64 */
#define MAX_NR_USER_CONSOLES 63	/* must be root to allocate above this */
		/* Note: the ioctl VT_GETSTATE does not work for
		   consoles 16 and higher (since it returns a short) */

#ifdef __KERNEL__
#include <linux/config.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/termios.h>
#include <linux/workqueue.h>
#include <linux/tty_driver.h>
#include <linux/tty_ldisc.h>

#include <asm/system.h>


/*
 * (Note: the *_driver.minor_start values 1, 64, 128, 192 are
 * hardcoded at present.)
 */
#define NR_PTYS	CONFIG_LEGACY_PTY_COUNT   /* Number of legacy ptys */
#define NR_UNIX98_PTY_DEFAULT	4096      /* Default maximum for Unix98 ptys */
#define NR_UNIX98_PTY_MAX	(1 << MINORBITS) /* Absolute limit */
#define NR_LDISCS		16

/*
 * These are set up by the setup-routine at boot-time:
 */

struct screen_info {
	u8  orig_x;		/* 0x00 */
	u8  orig_y;		/* 0x01 */
	u16 dontuse1;		/* 0x02 -- EXT_MEM_K sits here */
	u16 orig_video_page;	/* 0x04 */
	u8  orig_video_mode;	/* 0x06 */
	u8  orig_video_cols;	/* 0x07 */
	u16 unused2;		/* 0x08 */
	u16 orig_video_ega_bx;	/* 0x0a */
	u16 unused3;		/* 0x0c */
	u8  orig_video_lines;	/* 0x0e */
	u8  orig_video_isVGA;	/* 0x0f */
	u16 orig_video_points;	/* 0x10 */

	/* VESA graphic mode -- linear frame buffer */
	u16 lfb_width;		/* 0x12 */
	u16 lfb_height;		/* 0x14 */
	u16 lfb_depth;		/* 0x16 */
	u32 lfb_base;		/* 0x18 */
	u32 lfb_size;		/* 0x1c */
	u16 dontuse2, dontuse3;	/* 0x20 -- CL_MAGIC and CL_OFFSET here */
	u16 lfb_linelength;	/* 0x24 */
	u8  red_size;		/* 0x26 */
	u8  red_pos;		/* 0x27 */
	u8  green_size;		/* 0x28 */
	u8  green_pos;		/* 0x29 */
	u8  blue_size;		/* 0x2a */
	u8  blue_pos;		/* 0x2b */
	u8  rsvd_size;		/* 0x2c */
	u8  rsvd_pos;		/* 0x2d */
	u16 vesapm_seg;		/* 0x2e */
	u16 vesapm_off;		/* 0x30 */
	u16 pages;		/* 0x32 */
	u16 vesa_attributes;	/* 0x34 */
				/* 0x36 -- 0x3f reserved for future expansion */
};

extern struct screen_info screen_info;

#define ORIG_X			(screen_info.orig_x)
#define ORIG_Y			(screen_info.orig_y)
#define ORIG_VIDEO_MODE		(screen_info.orig_video_mode)
#define ORIG_VIDEO_COLS 	(screen_info.orig_video_cols)
#define ORIG_VIDEO_EGA_BX	(screen_info.orig_video_ega_bx)
#define ORIG_VIDEO_LINES	(screen_info.orig_video_lines)
#define ORIG_VIDEO_ISVGA	(screen_info.orig_video_isVGA)
#define ORIG_VIDEO_POINTS       (screen_info.orig_video_points)

#define VIDEO_TYPE_MDA		0x10	/* Monochrome Text Display	*/
#define VIDEO_TYPE_CGA		0x11	/* CGA Display 			*/
#define VIDEO_TYPE_EGAM		0x20	/* EGA/VGA in Monochrome Mode	*/
#define VIDEO_TYPE_EGAC		0x21	/* EGA in Color Mode		*/
#define VIDEO_TYPE_VGAC		0x22	/* VGA+ in Color Mode		*/
#define VIDEO_TYPE_VLFB		0x23	/* VESA VGA in graphic mode	*/

#define VIDEO_TYPE_PICA_S3	0x30	/* ACER PICA-61 local S3 video	*/
#define VIDEO_TYPE_MIPS_G364	0x31    /* MIPS Magnum 4000 G364 video  */
#define VIDEO_TYPE_SGI          0x33    /* Various SGI graphics hardware */

#define VIDEO_TYPE_TGAC		0x40	/* DEC TGA */

#define VIDEO_TYPE_SUN          0x50    /* Sun frame buffer. */
#define VIDEO_TYPE_SUNPCI       0x51    /* Sun PCI based frame buffer. */

#define VIDEO_TYPE_PMAC		0x60	/* PowerMacintosh frame buffer. */

/*
 * This character is the same as _POSIX_VDISABLE: it cannot be used as
 * a c_cc[] character, but indicates that a particular special character
 * isn't in use (eg VINTR has no character etc)
 */
#define __DISABLED_CHAR '\0'

/*
 * This is the flip buffer used for the tty driver.  The buffer is
 * located in the tty structure, and is used as a high speed interface
 * between the tty driver and the tty line discipline.
 */
#define TTY_FLIPBUF_SIZE 512

struct tty_flip_buffer {
	struct work_struct		work;
	struct semaphore pty_sem;
	char		*char_buf_ptr;
	unsigned char	*flag_buf_ptr;
	int		count;
	int		buf_num;
	unsigned char	char_buf[2*TTY_FLIPBUF_SIZE];
	char		flag_buf[2*TTY_FLIPBUF_SIZE];
	unsigned char	slop[4]; /* N.B. bug overwrites buffer by 1 */
};
/*
 * The pty uses char_buf and flag_buf as a contiguous buffer
 */
#define PTY_BUF_SIZE	4*TTY_FLIPBUF_SIZE

/*
 * When a break, frame error, or parity error happens, these codes are
 * stuffed into the flags buffer.
 */
#define TTY_NORMAL	0
#define TTY_BREAK	1
#define TTY_FRAME	2
#define TTY_PARITY	3
#define TTY_OVERRUN	4

#define INTR_CHAR(tty) ((tty)->termios->c_cc[VINTR])
#define QUIT_CHAR(tty) ((tty)->termios->c_cc[VQUIT])
#define ERASE_CHAR(tty) ((tty)->termios->c_cc[VERASE])
#define KILL_CHAR(tty) ((tty)->termios->c_cc[VKILL])
#define EOF_CHAR(tty) ((tty)->termios->c_cc[VEOF])
#define TIME_CHAR(tty) ((tty)->termios->c_cc[VTIME])
#define MIN_CHAR(tty) ((tty)->termios->c_cc[VMIN])
#define SWTC_CHAR(tty) ((tty)->termios->c_cc[VSWTC])
#define START_CHAR(tty) ((tty)->termios->c_cc[VSTART])
#define STOP_CHAR(tty) ((tty)->termios->c_cc[VSTOP])
#define SUSP_CHAR(tty) ((tty)->termios->c_cc[VSUSP])
#define EOL_CHAR(tty) ((tty)->termios->c_cc[VEOL])
#define REPRINT_CHAR(tty) ((tty)->termios->c_cc[VREPRINT])
#define DISCARD_CHAR(tty) ((tty)->termios->c_cc[VDISCARD])
#define WERASE_CHAR(tty) ((tty)->termios->c_cc[VWERASE])
#define LNEXT_CHAR(tty)	((tty)->termios->c_cc[VLNEXT])
#define EOL2_CHAR(tty) ((tty)->termios->c_cc[VEOL2])

#define _I_FLAG(tty,f)	((tty)->termios->c_iflag & (f))
#define _O_FLAG(tty,f)	((tty)->termios->c_oflag & (f))
#define _C_FLAG(tty,f)	((tty)->termios->c_cflag & (f))
#define _L_FLAG(tty,f)	((tty)->termios->c_lflag & (f))

#define I_IGNBRK(tty)	_I_FLAG((tty),IGNBRK)
#define I_BRKINT(tty)	_I_FLAG((tty),BRKINT)
#define I_IGNPAR(tty)	_I_FLAG((tty),IGNPAR)
#define I_PARMRK(tty)	_I_FLAG((tty),PARMRK)
#define I_INPCK(tty)	_I_FLAG((tty),INPCK)
#define I_ISTRIP(tty)	_I_FLAG((tty),ISTRIP)
#define I_INLCR(tty)	_I_FLAG((tty),INLCR)
#define I_IGNCR(tty)	_I_FLAG((tty),IGNCR)
#define I_ICRNL(tty)	_I_FLAG((tty),ICRNL)
#define I_IUCLC(tty)	_I_FLAG((tty),IUCLC)
#define I_IXON(tty)	_I_FLAG((tty),IXON)
#define I_IXANY(tty)	_I_FLAG((tty),IXANY)
#define I_IXOFF(tty)	_I_FLAG((tty),IXOFF)
#define I_IMAXBEL(tty)	_I_FLAG((tty),IMAXBEL)
#define I_IUTF8(tty)	_I_FLAG((tty),IUTF8)

#define O_OPOST(tty)	_O_FLAG((tty),OPOST)
#define O_OLCUC(tty)	_O_FLAG((tty),OLCUC)
#define O_ONLCR(tty)	_O_FLAG((tty),ONLCR)
#define O_OCRNL(tty)	_O_FLAG((tty),OCRNL)
#define O_ONOCR(tty)	_O_FLAG((tty),ONOCR)
#define O_ONLRET(tty)	_O_FLAG((tty),ONLRET)
#define O_OFILL(tty)	_O_FLAG((tty),OFILL)
#define O_OFDEL(tty)	_O_FLAG((tty),OFDEL)
#define O_NLDLY(tty)	_O_FLAG((tty),NLDLY)
#define O_CRDLY(tty)	_O_FLAG((tty),CRDLY)
#define O_TABDLY(tty)	_O_FLAG((tty),TABDLY)
#define O_BSDLY(tty)	_O_FLAG((tty),BSDLY)
#define O_VTDLY(tty)	_O_FLAG((tty),VTDLY)
#define O_FFDLY(tty)	_O_FLAG((tty),FFDLY)

#define C_BAUD(tty)	_C_FLAG((tty),CBAUD)
#define C_CSIZE(tty)	_C_FLAG((tty),CSIZE)
#define C_CSTOPB(tty)	_C_FLAG((tty),CSTOPB)
#define C_CREAD(tty)	_C_FLAG((tty),CREAD)
#define C_PARENB(tty)	_C_FLAG((tty),PARENB)
#define C_PARODD(tty)	_C_FLAG((tty),PARODD)
#define C_HUPCL(tty)	_C_FLAG((tty),HUPCL)
#define C_CLOCAL(tty)	_C_FLAG((tty),CLOCAL)
#define C_CIBAUD(tty)	_C_FLAG((tty),CIBAUD)
#define C_CRTSCTS(tty)	_C_FLAG((tty),CRTSCTS)

#define L_ISIG(tty)	_L_FLAG((tty),ISIG)
#define L_ICANON(tty)	_L_FLAG((tty),ICANON)
#define L_XCASE(tty)	_L_FLAG((tty),XCASE)
#define L_ECHO(tty)	_L_FLAG((tty),ECHO)
#define L_ECHOE(tty)	_L_FLAG((tty),ECHOE)
#define L_ECHOK(tty)	_L_FLAG((tty),ECHOK)
#define L_ECHONL(tty)	_L_FLAG((tty),ECHONL)
#define L_NOFLSH(tty)	_L_FLAG((tty),NOFLSH)
#define L_TOSTOP(tty)	_L_FLAG((tty),TOSTOP)
#define L_ECHOCTL(tty)	_L_FLAG((tty),ECHOCTL)
#define L_ECHOPRT(tty)	_L_FLAG((tty),ECHOPRT)
#define L_ECHOKE(tty)	_L_FLAG((tty),ECHOKE)
#define L_FLUSHO(tty)	_L_FLAG((tty),FLUSHO)
#define L_PENDIN(tty)	_L_FLAG((tty),PENDIN)
#define L_IEXTEN(tty)	_L_FLAG((tty),IEXTEN)

struct device;
/*
 * Where all of the state associated with a tty is kept while the tty
 * is open.  Since the termios state should be kept even if the tty
 * has been closed --- for things like the baud rate, etc --- it is
 * not stored here, but rather a pointer to the real state is stored
 * here.  Possible the winsize structure should have the same
 * treatment, but (1) the default 80x24 is usually right and (2) it's
 * most often used by a windowing system, which will set the correct
 * size each time the window is created or resized anyway.
 * 						- TYT, 9/14/92
 */
struct tty_struct {
	int	magic;
	struct tty_driver *driver;
	int index;
	struct tty_ldisc ldisc;
	struct termios *termios, *termios_locked;
	char name[64];
	int pgrp;
	int session;
	unsigned long flags;
	int count;
	struct winsize winsize;
	unsigned char stopped:1, hw_stopped:1, flow_stopped:1, packet:1;
	unsigned char low_latency:1, warned:1;
	unsigned char ctrl_status;

	struct tty_struct *link;
	struct fasync_struct *fasync;
	struct tty_flip_buffer flip;
	int max_flip_cnt;
	int alt_speed;		/* For magic substitution of 38400 bps */
	wait_queue_head_t write_wait;
	wait_queue_head_t read_wait;
	struct work_struct hangup_work;
	void *disc_data;
	void *driver_data;
	struct list_head tty_files;

#define N_TTY_BUF_SIZE 4096
	
	/*
	 * The following is data for the N_TTY line discipline.  For
	 * historical reasons, this is included in the tty structure.
	 */
	unsigned int column;
	unsigned char lnext:1, erasing:1, raw:1, real_raw:1, icanon:1;
	unsigned char closing:1;
	unsigned short minimum_to_wake;
	unsigned long overrun_time;
	int num_overrun;
	unsigned long process_char_map[256/(8*sizeof(unsigned long))];
	char *read_buf;
	int read_head;
	int read_tail;
	int read_cnt;
	unsigned long read_flags[N_TTY_BUF_SIZE/(8*sizeof(unsigned long))];
	int canon_data;
	unsigned long canon_head;
	unsigned int canon_column;
	struct semaphore atomic_read;
	struct semaphore atomic_write;
	spinlock_t read_lock;
	/* If the tty has a pending do_SAK, queue it here - akpm */
	struct work_struct SAK_work;
};

/* tty magic number */
#define TTY_MAGIC		0x5401

/*
 * These bits are used in the flags field of the tty structure.
 * 
 * So that interrupts won't be able to mess up the queues,
 * copy_to_cooked must be atomic with respect to itself, as must
 * tty->write.  Thus, you must use the inline functions set_bit() and
 * clear_bit() to make things atomic.
 */
#define TTY_THROTTLED 0
#define TTY_IO_ERROR 1
#define TTY_OTHER_CLOSED 2
#define TTY_EXCLUSIVE 3
#define TTY_DEBUG 4
#define TTY_DO_WRITE_WAKEUP 5
#define TTY_PUSH 6
#define TTY_CLOSING 7
#define TTY_DONT_FLIP 8
#define TTY_HW_COOK_OUT 14
#define TTY_HW_COOK_IN 15
#define TTY_PTY_LOCK 16
#define TTY_NO_WRITE_SPLIT 17

#define TTY_WRITE_FLUSH(tty) tty_write_flush((tty))

extern void tty_write_flush(struct tty_struct *);

extern struct termios tty_std_termios;
extern struct tty_ldisc ldiscs[];
extern int fg_console, last_console, want_console;

extern int kmsg_redirect;

extern void console_init(void);
extern int vcs_init(void);

extern int tty_paranoia_check(struct tty_struct *tty, struct inode *inode,
			      const char *routine);
extern char *tty_name(struct tty_struct *tty, char *buf);
extern void tty_wait_until_sent(struct tty_struct * tty, long timeout);
extern int tty_check_change(struct tty_struct * tty);
extern void stop_tty(struct tty_struct * tty);
extern void start_tty(struct tty_struct * tty);
extern int tty_register_ldisc(int disc, struct tty_ldisc *new_ldisc);
extern int tty_register_driver(struct tty_driver *driver);
extern int tty_unregister_driver(struct tty_driver *driver);
extern void tty_register_device(struct tty_driver *driver, unsigned index, struct device *dev);
extern void tty_unregister_device(struct tty_driver *driver, unsigned index);
extern int tty_read_raw_data(struct tty_struct *tty, unsigned char *bufp,
			     int buflen);
extern void tty_write_message(struct tty_struct *tty, char *msg);

extern int is_orphaned_pgrp(int pgrp);
extern int is_ignored(int sig);
extern int tty_signal(int sig, struct tty_struct *tty);
extern void tty_hangup(struct tty_struct * tty);
extern void tty_vhangup(struct tty_struct * tty);
extern void tty_unhangup(struct file *filp);
extern int tty_hung_up_p(struct file * filp);
extern void do_SAK(struct tty_struct *tty);
extern void disassociate_ctty(int priv);
extern void tty_flip_buffer_push(struct tty_struct *tty);
extern int tty_get_baud_rate(struct tty_struct *tty);
extern int tty_termios_baud_rate(struct termios *termios);

struct semaphore;
extern struct semaphore tty_sem;

/* n_tty.c */
extern struct tty_ldisc tty_ldisc_N_TTY;

/* tty_ioctl.c */
extern int n_tty_ioctl(struct tty_struct * tty, struct file * file,
		       unsigned int cmd, unsigned long arg);

/* serial.c */

extern void serial_console_init(void);
 
/* pcxx.c */

extern int pcxe_open(struct tty_struct *tty, struct file *filp);

/* printk.c */

extern void console_print(const char *);

/* vt.c */

extern int vt_ioctl(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg);

static inline dev_t tty_devnum(struct tty_struct *tty)
{
	return MKDEV(tty->driver->major, tty->driver->minor_start) + tty->index;
}

#endif /* __KERNEL__ */
#endif
