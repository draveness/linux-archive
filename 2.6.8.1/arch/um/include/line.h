/* 
 * Copyright (C) 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __LINE_H__
#define __LINE_H__

#include "linux/list.h"
#include "linux/workqueue.h"
#include "linux/tty.h"
#include "asm/semaphore.h"
#include "chan_user.h"
#include "mconsole_kern.h"

struct line_driver {
	char *name;
	char *devfs_name;
	short major;
	short minor_start;
	short type;
	short subtype;
	int read_irq;
	char *read_irq_name;
	int write_irq;
	char *write_irq_name;
	char *symlink_from;
	char *symlink_to;
	struct mc_device mc;
};

struct line {
	char *init_str;
	int init_pri;
	struct list_head chan_list;
	int valid;
	int count;
	struct tty_struct *tty;
	struct semaphore sem;
	char *buffer;
	char *head;
	char *tail;
	int sigio;
	struct work_struct task;
	struct line_driver *driver;
	int have_irq;
};

#define LINE_INIT(str, d) \
	{ init_str :	str, \
	  init_pri :	INIT_STATIC, \
	  chan_list : 	{ }, \
	  valid :	1, \
	  count :	0, \
	  tty :		NULL, \
	  sem : 	{ }, \
	  buffer :	NULL, \
	  head :	NULL, \
	  tail :	NULL, \
	  sigio :	0, \
 	  driver :	d, \
          have_irq :	0 }

struct lines {
	int num;
};

#define LINES_INIT(n) {  num :		n }

extern void line_interrupt(int irq, void *data, struct pt_regs *unused);
extern void line_write_interrupt(int irq, void *data, struct pt_regs *unused);
extern void line_close(struct line *lines, struct tty_struct *tty);
extern int line_open(struct line *lines, struct tty_struct *tty, 
		     struct chan_opts *opts);
extern int line_setup(struct line *lines, int num, char *init, 
		      int all_allowed);
extern int line_write(struct line *line, struct tty_struct *tty, int from_user,
		      const char *buf, int len);
extern int line_write_room(struct tty_struct *tty);
extern char *add_xterm_umid(char *base);
extern int line_setup_irq(int fd, int input, int output, void *data);
extern void line_close_chan(struct line *line);
extern void line_disable(struct line *line, int current_irq);
extern struct tty_driver * line_register_devfs(struct lines *set, 
				struct line_driver *line_driver, 
				struct tty_operations *driver,
				struct line *lines,
				int nlines);
extern void lines_init(struct line *lines, int nlines);
extern void close_lines(struct line *lines, int nlines);
extern int line_config(struct line *lines, int num, char *str);
extern int line_remove(struct line *lines, int num, char *str);
extern int line_get_config(char *dev, struct line *lines, int num, char *str, 
			   int size, char **error_out);

#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
