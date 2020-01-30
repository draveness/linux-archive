/* 
 * Copyright (C) 2001 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include "user.h"
#include "user_util.h"
#include "chan_user.h"

struct fd_chan {
	int fd;
	int raw;
	struct termios tt;
	char str[sizeof("1234567890\0")];
};

void *fd_init(char *str, int device, struct chan_opts *opts)
{
	struct fd_chan *data;
	char *end;
	int n;

	if(*str != ':'){
		printk("fd_init : channel type 'fd' must specify a file "
		       "descriptor\n");
		return(NULL);
	}
	str++;
	n = strtoul(str, &end, 0);
	if((*end != '\0') || (end == str)){
		printk("fd_init : couldn't parse file descriptor '%s'\n", str);
		return(NULL);
	}
	if((data = um_kmalloc(sizeof(*data))) == NULL) return(NULL);
	*data = ((struct fd_chan) { .fd  	= n,
				    .raw  	= opts->raw });
	return(data);
}

int fd_open(int input, int output, int primary, void *d, char **dev_out)
{
	struct fd_chan *data = d;

	if(data->raw && isatty(data->fd)){
		tcgetattr(data->fd, &data->tt);
		raw(data->fd, 0);
	}
	sprintf(data->str, "%d", data->fd);
	*dev_out = data->str;
	return(data->fd);
}

void fd_close(int fd, void *d)
{
	struct fd_chan *data = d;

	if(data->raw && isatty(fd)){
		tcsetattr(fd, TCSAFLUSH, &data->tt);
		data->raw = 0;
	}
}

int fd_console_write(int fd, const char *buf, int n, void *d)
{
	struct fd_chan *data = d;

	return(generic_console_write(fd, buf, n, &data->tt));
}

struct chan_ops fd_ops = {
	.type		= "fd",
	.init		= fd_init,
	.open		= fd_open,
	.close		= fd_close,
	.read		= generic_read,
	.write		= generic_write,
	.console_write	= fd_console_write,
	.window_size	= generic_window_size,
	.free		= generic_free,
	.winch		= 1,
};

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
