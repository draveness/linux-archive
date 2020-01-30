/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include "linux/errno.h"
#include "linux/slab.h"
#include "asm/semaphore.h"
#include "asm/irq.h"
#include "irq_user.h"
#include "kern_util.h"
#include "os.h"
#include "xterm.h"

struct xterm_wait {
	struct semaphore sem;
	int fd;
	int pid;
	int new_fd;
};

static void xterm_interrupt(int irq, void *data, struct pt_regs *regs)
{
	struct xterm_wait *xterm = data;
	int fd;

	fd = os_rcv_fd(xterm->fd, &xterm->pid);
	if(fd == -EAGAIN)
		return;

	xterm->new_fd = fd;
	up(&xterm->sem);
}

int xterm_fd(int socket, int *pid_out)
{
	struct xterm_wait *data;
	int err, ret;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if(data == NULL){
		printk(KERN_ERR "xterm_fd : failed to allocate xterm_wait\n");
		return(-ENOMEM);
	}
	*data = ((struct xterm_wait) 
		{ .sem  	= __SEMAPHORE_INITIALIZER(data->sem, 0),
		  .fd 		= socket,
		  .pid 		= -1,
		  .new_fd 	= -1 });

	err = um_request_irq(XTERM_IRQ, socket, IRQ_READ, xterm_interrupt, 
			     SA_INTERRUPT | SA_SHIRQ | SA_SAMPLE_RANDOM, 
			     "xterm", data);
	if(err){
		printk(KERN_ERR "xterm_fd : failed to get IRQ for xterm, "
		       "err = %d\n",  err);
		return(err);
	}
	down(&data->sem);

	free_irq(XTERM_IRQ, data);

	ret = data->new_fd;
	*pid_out = data->pid;
	kfree(data);

	return(ret);
}

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
