/*
 *  linux/drivers/char/tty_ioctl.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 * Modified by Fred N. van Kempen, 01/29/93, to add line disciplines
 * which can be dynamically activated and de-activated by the line
 * discipline handling modules (like SLIP).
 */

#include <linux/types.h>
#include <linux/termios.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/tty.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/mm.h>

#include <asm/io.h>
#include <asm/bitops.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#undef TTY_DEBUG_WAIT_UNTIL_SENT

#undef	DEBUG
#ifdef DEBUG
# define	PRINTK(x)	printk (x)
#else
# define	PRINTK(x)	/**/
#endif

/*
 * Internal flag options for termios setting behavior
 */
#define TERMIOS_FLUSH	1
#define TERMIOS_WAIT	2
#define TERMIOS_TERMIO	4

void tty_wait_until_sent(struct tty_struct * tty, long timeout)
{
	DECLARE_WAITQUEUE(wait, current);

#ifdef TTY_DEBUG_WAIT_UNTIL_SENT
	char buf[64];
	
	printk("%s wait until sent...\n", tty_name(tty, buf));
#endif
	if (!tty->driver.chars_in_buffer)
		return;
	add_wait_queue(&tty->write_wait, &wait);
	if (!timeout)
		timeout = MAX_SCHEDULE_TIMEOUT;
	do {
#ifdef TTY_DEBUG_WAIT_UNTIL_SENT
		printk("waiting %s...(%d)\n", tty_name(tty, buf),
		       tty->driver.chars_in_buffer(tty));
#endif
		set_current_state(TASK_INTERRUPTIBLE);
		if (signal_pending(current))
			goto stop_waiting;
		if (!tty->driver.chars_in_buffer(tty))
			break;
		timeout = schedule_timeout(timeout);
	} while (timeout);
	if (tty->driver.wait_until_sent)
		tty->driver.wait_until_sent(tty, timeout);
stop_waiting:
	current->state = TASK_RUNNING;
	remove_wait_queue(&tty->write_wait, &wait);
}

static void unset_locked_termios(struct termios *termios,
				 struct termios *old,
				 struct termios *locked)
{
	int	i;
	
#define NOSET_MASK(x,y,z) (x = ((x) & ~(z)) | ((y) & (z)))

	if (!locked) {
		printk("Warning?!? termios_locked is NULL.\n");
		return;
	}

	NOSET_MASK(termios->c_iflag, old->c_iflag, locked->c_iflag);
	NOSET_MASK(termios->c_oflag, old->c_oflag, locked->c_oflag);
	NOSET_MASK(termios->c_cflag, old->c_cflag, locked->c_cflag);
	NOSET_MASK(termios->c_lflag, old->c_lflag, locked->c_lflag);
	termios->c_line = locked->c_line ? old->c_line : termios->c_line;
	for (i=0; i < NCCS; i++)
		termios->c_cc[i] = locked->c_cc[i] ?
			old->c_cc[i] : termios->c_cc[i];
}

static void change_termios(struct tty_struct * tty, struct termios * new_termios)
{
	int canon_change;
	struct termios old_termios = *tty->termios;

	cli();
	*tty->termios = *new_termios;
	unset_locked_termios(tty->termios, &old_termios, tty->termios_locked);
	canon_change = (old_termios.c_lflag ^ tty->termios->c_lflag) & ICANON;
	if (canon_change) {
		memset(&tty->read_flags, 0, sizeof tty->read_flags);
		tty->canon_head = tty->read_tail;
		tty->canon_data = 0;
		tty->erasing = 0;
	}
	sti();
	if (canon_change && !L_ICANON(tty) && tty->read_cnt)
		/* Get characters left over from canonical mode. */
		wake_up_interruptible(&tty->read_wait);

	/* see if packet mode change of state */

	if (tty->link && tty->link->packet) {
		int old_flow = ((old_termios.c_iflag & IXON) &&
				(old_termios.c_cc[VSTOP] == '\023') &&
				(old_termios.c_cc[VSTART] == '\021'));
		int new_flow = (I_IXON(tty) &&
				STOP_CHAR(tty) == '\023' &&
				START_CHAR(tty) == '\021');
		if (old_flow != new_flow) {
			tty->ctrl_status &= ~(TIOCPKT_DOSTOP | TIOCPKT_NOSTOP);
			if (new_flow)
				tty->ctrl_status |= TIOCPKT_DOSTOP;
			else
				tty->ctrl_status |= TIOCPKT_NOSTOP;
			wake_up_interruptible(&tty->link->read_wait);
		}
	}

	if (tty->driver.set_termios)
		(*tty->driver.set_termios)(tty, &old_termios);

	if (tty->ldisc.set_termios)
		(*tty->ldisc.set_termios)(tty, &old_termios);
}

static int set_termios(struct tty_struct * tty, unsigned long arg, int opt)
{
	struct termios tmp_termios;
	int retval;

	retval = tty_check_change(tty);
	if (retval)
		return retval;

	if (opt & TERMIOS_TERMIO) {
		memcpy(&tmp_termios, tty->termios, sizeof(struct termios));
		if (user_termio_to_kernel_termios(&tmp_termios, (struct termio *) arg))
			return -EFAULT;
	} else {
		if (user_termios_to_kernel_termios(&tmp_termios, (struct termios *) arg))
			return -EFAULT;
	}

	if ((opt & TERMIOS_FLUSH) && tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);

	if (opt & TERMIOS_WAIT) {
		tty_wait_until_sent(tty, 0);
		if (signal_pending(current))
			return -EINTR;
	}

	change_termios(tty, &tmp_termios);
	return 0;
}

static int get_termio(struct tty_struct * tty, struct termio * termio)
{
	if (kernel_termios_to_user_termio(termio, tty->termios))
		return -EFAULT;
	return 0;
}

static unsigned long inq_canon(struct tty_struct * tty)
{
	int nr, head, tail;

	if (!tty->canon_data || !tty->read_buf)
		return 0;
	head = tty->canon_head;
	tail = tty->read_tail;
	nr = (head - tail) & (N_TTY_BUF_SIZE-1);
	/* Skip EOF-chars.. */
	while (head != tail) {
		if (test_bit(tail, &tty->read_flags) &&
		    tty->read_buf[tail] == __DISABLED_CHAR)
			nr--;
		tail = (tail+1) & (N_TTY_BUF_SIZE-1);
	}
	return nr;
}

#ifdef TIOCGETP
/*
 * These are deprecated, but there is limited support..
 *
 * The "sg_flags" translation is a joke..
 */
static int get_sgflags(struct tty_struct * tty)
{
	int flags = 0;

	if (!(tty->termios->c_lflag & ICANON)) {
		if (tty->termios->c_lflag & ISIG)
			flags |= 0x02;		/* cbreak */
		else
			flags |= 0x20;		/* raw */
	}
	if (tty->termios->c_lflag & ECHO)
		flags |= 0x08;			/* echo */
	if (tty->termios->c_oflag & OPOST)
		if (tty->termios->c_oflag & ONLCR)
			flags |= 0x10;		/* crmod */
	return flags;
}

static int get_sgttyb(struct tty_struct * tty, struct sgttyb * sgttyb)
{
	struct sgttyb tmp;

	tmp.sg_ispeed = 0;
	tmp.sg_ospeed = 0;
	tmp.sg_erase = tty->termios->c_cc[VERASE];
	tmp.sg_kill = tty->termios->c_cc[VKILL];
	tmp.sg_flags = get_sgflags(tty);
	if (copy_to_user(sgttyb, &tmp, sizeof(tmp)))
		return -EFAULT;
	return 0;
}

static void set_sgflags(struct termios * termios, int flags)
{
	termios->c_iflag = ICRNL | IXON;
	termios->c_oflag = 0;
	termios->c_lflag = ISIG | ICANON;
	if (flags & 0x02) {	/* cbreak */
		termios->c_iflag = 0;
		termios->c_lflag &= ~ICANON;
	}
	if (flags & 0x08) {		/* echo */
		termios->c_lflag |= ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
	}
	if (flags & 0x10) {		/* crmod */
		termios->c_oflag |= OPOST | ONLCR;
	}
	if (flags & 0x20) {	/* raw */
		termios->c_iflag = 0;
		termios->c_lflag &= ~(ISIG | ICANON);
	}
	if (!(termios->c_lflag & ICANON)) {
		termios->c_cc[VMIN] = 1;
		termios->c_cc[VTIME] = 0;
	}
}

static int set_sgttyb(struct tty_struct * tty, struct sgttyb * sgttyb)
{
	int retval;
	struct sgttyb tmp;
	struct termios termios;

	retval = tty_check_change(tty);
	if (retval)
		return retval;
	termios =  *tty->termios;
	if (copy_from_user(&tmp, sgttyb, sizeof(tmp)))
		return -EFAULT;
	termios.c_cc[VERASE] = tmp.sg_erase;
	termios.c_cc[VKILL] = tmp.sg_kill;
	set_sgflags(&termios, tmp.sg_flags);
	change_termios(tty, &termios);
	return 0;
}
#endif

#ifdef TIOCGETC
static int get_tchars(struct tty_struct * tty, struct tchars * tchars)
{
	struct tchars tmp;

	tmp.t_intrc = tty->termios->c_cc[VINTR];
	tmp.t_quitc = tty->termios->c_cc[VQUIT];
	tmp.t_startc = tty->termios->c_cc[VSTART];
	tmp.t_stopc = tty->termios->c_cc[VSTOP];
	tmp.t_eofc = tty->termios->c_cc[VEOF];
	tmp.t_brkc = tty->termios->c_cc[VEOL2];	/* what is brkc anyway? */
	if (copy_to_user(tchars, &tmp, sizeof(tmp)))
		return -EFAULT;
	return 0;
}

static int set_tchars(struct tty_struct * tty, struct tchars * tchars)
{
	struct tchars tmp;

	if (copy_from_user(&tmp, tchars, sizeof(tmp)))
		return -EFAULT;
	tty->termios->c_cc[VINTR] = tmp.t_intrc;
	tty->termios->c_cc[VQUIT] = tmp.t_quitc;
	tty->termios->c_cc[VSTART] = tmp.t_startc;
	tty->termios->c_cc[VSTOP] = tmp.t_stopc;
	tty->termios->c_cc[VEOF] = tmp.t_eofc;
	tty->termios->c_cc[VEOL2] = tmp.t_brkc;	/* what is brkc anyway? */
	return 0;
}
#endif

#ifdef TIOCGLTC
static int get_ltchars(struct tty_struct * tty, struct ltchars * ltchars)
{
	struct ltchars tmp;

	tmp.t_suspc = tty->termios->c_cc[VSUSP];
	tmp.t_dsuspc = tty->termios->c_cc[VSUSP];	/* what is dsuspc anyway? */
	tmp.t_rprntc = tty->termios->c_cc[VREPRINT];
	tmp.t_flushc = tty->termios->c_cc[VEOL2];	/* what is flushc anyway? */
	tmp.t_werasc = tty->termios->c_cc[VWERASE];
	tmp.t_lnextc = tty->termios->c_cc[VLNEXT];
	if (copy_to_user(ltchars, &tmp, sizeof(tmp)))
		return -EFAULT;
	return 0;
}

static int set_ltchars(struct tty_struct * tty, struct ltchars * ltchars)
{
	struct ltchars tmp;

	if (copy_from_user(&tmp, ltchars, sizeof(tmp)))
		return -EFAULT;

	tty->termios->c_cc[VSUSP] = tmp.t_suspc;
	tty->termios->c_cc[VEOL2] = tmp.t_dsuspc;	/* what is dsuspc anyway? */
	tty->termios->c_cc[VREPRINT] = tmp.t_rprntc;
	tty->termios->c_cc[VEOL2] = tmp.t_flushc;	/* what is flushc anyway? */
	tty->termios->c_cc[VWERASE] = tmp.t_werasc;
	tty->termios->c_cc[VLNEXT] = tmp.t_lnextc;
	return 0;
}
#endif

/*
 * Send a high priority character to the tty.
 */
void send_prio_char(struct tty_struct *tty, char ch)
{
	int	was_stopped = tty->stopped;

	if (tty->driver.send_xchar) {
		tty->driver.send_xchar(tty, ch);
		return;
	}
	if (was_stopped)
		start_tty(tty);
	tty->driver.write(tty, 0, &ch, 1);
	if (was_stopped)
		stop_tty(tty);
}

int n_tty_ioctl(struct tty_struct * tty, struct file * file,
		       unsigned int cmd, unsigned long arg)
{
	struct tty_struct * real_tty;
	int retval;

	if (tty->driver.type == TTY_DRIVER_TYPE_PTY &&
	    tty->driver.subtype == PTY_TYPE_MASTER)
		real_tty = tty->link;
	else
		real_tty = tty;

	switch (cmd) {
#ifdef TIOCGETP
		case TIOCGETP:
			return get_sgttyb(real_tty, (struct sgttyb *) arg);
		case TIOCSETP:
		case TIOCSETN:
			return set_sgttyb(real_tty, (struct sgttyb *) arg);
#endif
#ifdef TIOCGETC
		case TIOCGETC:
			return get_tchars(real_tty, (struct tchars *) arg);
		case TIOCSETC:
			return set_tchars(real_tty, (struct tchars *) arg);
#endif
#ifdef TIOCGLTC
		case TIOCGLTC:
			return get_ltchars(real_tty, (struct ltchars *) arg);
		case TIOCSLTC:
			return set_ltchars(real_tty, (struct ltchars *) arg);
#endif
		case TCGETS:
			if (kernel_termios_to_user_termios((struct termios *)arg, real_tty->termios))
				return -EFAULT;
			return 0;
		case TCSETSF:
			return set_termios(real_tty, arg,  TERMIOS_FLUSH);
		case TCSETSW:
			return set_termios(real_tty, arg, TERMIOS_WAIT);
		case TCSETS:
			return set_termios(real_tty, arg, 0);
		case TCGETA:
			return get_termio(real_tty,(struct termio *) arg);
		case TCSETAF:
			return set_termios(real_tty, arg, TERMIOS_FLUSH | TERMIOS_TERMIO);
		case TCSETAW:
			return set_termios(real_tty, arg, TERMIOS_WAIT | TERMIOS_TERMIO);
		case TCSETA:
			return set_termios(real_tty, arg, TERMIOS_TERMIO);
		case TCXONC:
			retval = tty_check_change(tty);
			if (retval)
				return retval;
			switch (arg) {
			case TCOOFF:
				if (!tty->flow_stopped) {
					tty->flow_stopped = 1;
					stop_tty(tty);
				}
				break;
			case TCOON:
				if (tty->flow_stopped) {
					tty->flow_stopped = 0;
					start_tty(tty);
				}
				break;
			case TCIOFF:
				if (STOP_CHAR(tty) != __DISABLED_CHAR)
					send_prio_char(tty, STOP_CHAR(tty));
				break;
			case TCION:
				if (START_CHAR(tty) != __DISABLED_CHAR)
					send_prio_char(tty, START_CHAR(tty));
				break;
			default:
				return -EINVAL;
			}
			return 0;
		case TCFLSH:
			retval = tty_check_change(tty);
			if (retval)
				return retval;
			switch (arg) {
			case TCIFLUSH:
				if (tty->ldisc.flush_buffer)
					tty->ldisc.flush_buffer(tty);
				break;
			case TCIOFLUSH:
				if (tty->ldisc.flush_buffer)
					tty->ldisc.flush_buffer(tty);
				/* fall through */
			case TCOFLUSH:
				if (tty->driver.flush_buffer)
					tty->driver.flush_buffer(tty);
				break;
			default:
				return -EINVAL;
			}
			return 0;
		case TIOCOUTQ:
			return put_user(tty->driver.chars_in_buffer ?
					tty->driver.chars_in_buffer(tty) : 0,
					(int *) arg);
		case TIOCINQ:
			retval = tty->read_cnt;
			if (L_ICANON(tty))
				retval = inq_canon(tty);
			return put_user(retval, (unsigned int *) arg);
		case TIOCGLCKTRMIOS:
			if (kernel_termios_to_user_termios((struct termios *)arg, real_tty->termios_locked))
				return -EFAULT;
			return 0;

		case TIOCSLCKTRMIOS:
			if (!suser())
				return -EPERM;
			if (user_termios_to_kernel_termios(real_tty->termios_locked, (struct termios *) arg))
				return -EFAULT;
			return 0;

		case TIOCPKT:
		{
			int pktmode;

			if (tty->driver.type != TTY_DRIVER_TYPE_PTY ||
			    tty->driver.subtype != PTY_TYPE_MASTER)
				return -ENOTTY;
			retval = get_user(pktmode, (int *) arg);
			if (retval)
				return retval;
			if (pktmode) {
				if (!tty->packet) {
					tty->packet = 1;
					tty->link->ctrl_status = 0;
				}
			} else
				tty->packet = 0;
			return 0;
		}
		case TIOCGSOFTCAR:
			return put_user(C_CLOCAL(tty) ? 1 : 0, (int *) arg);
		case TIOCSSOFTCAR:
			retval = get_user(arg, (unsigned int *) arg);
			if (retval)
				return retval;
			tty->termios->c_cflag =
				((tty->termios->c_cflag & ~CLOCAL) |
				 (arg ? CLOCAL : 0));
			return 0;
		default:
			return -ENOIOCTLCMD;
		}
}
