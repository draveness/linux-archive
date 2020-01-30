/* 
 * Copyright (C) 2001 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <stdio.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "chan_user.h"
#include "user_util.h"
#include "user.h"
#include "os.h"

struct tty_chan {
	char *dev;
	int raw;
	struct termios tt;
};

void *tty_chan_init(char *str, int device, struct chan_opts *opts)
{
	struct tty_chan *data;

	if(*str != ':'){
		printk("tty_init : channel type 'tty' must specify "
		       "a device\n");
		return(NULL);
	}
	str++;

	if((data = um_kmalloc(sizeof(*data))) == NULL) 
		return(NULL);
	*data = ((struct tty_chan) { .dev 	= str,
				     .raw 	= opts->raw });
				     
	return(data);
}

int tty_open(int input, int output, int primary, void *d, char **dev_out)
{
	struct tty_chan *data = d;
	int fd;

	fd = os_open_file(data->dev, of_set_rw(OPENFLAGS(), input, output), 0);
	if(fd < 0) return(fd);
	if(data->raw){
		tcgetattr(fd, &data->tt);
		raw(fd, 0);
	}

	*dev_out = data->dev;
	return(fd);
}

int tty_console_write(int fd, const char *buf, int n, void *d)
{
	struct tty_chan *data = d;

	return(generic_console_write(fd, buf, n, &data->tt));
}

struct chan_ops tty_ops = {
	.type		= "tty",
	.init		= tty_chan_init,
	.open		= tty_open,
	.close		= generic_close,
	.read		= generic_read,
	.write		= generic_write,
	.console_write	= tty_console_write,
	.window_size	= generic_window_size,
	.free		= generic_free,
	.winch		= 0,
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
