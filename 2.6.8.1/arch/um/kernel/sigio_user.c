/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <pty.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include "init.h"
#include "user.h"
#include "kern_util.h"
#include "sigio.h"
#include "helper.h"
#include "os.h"

/* Changed during early boot */
int pty_output_sigio = 0;
int pty_close_sigio = 0;

/* Used as a flag during SIGIO testing early in boot */
static int got_sigio = 0;

void __init handler(int sig)
{
	got_sigio = 1;
}

struct openpty_arg {
	int master;
	int slave;
	int err;
};

static void openpty_cb(void *arg)
{
	struct openpty_arg *info = arg;

	info->err = 0;
	if(openpty(&info->master, &info->slave, NULL, NULL, NULL))
		info->err = errno;
}

void __init check_one_sigio(void (*proc)(int, int))
{
	struct sigaction old, new;
	struct termios tt;
	struct openpty_arg pty = { .master = -1, .slave = -1 };
	int master, slave, flags;

	initial_thread_cb(openpty_cb, &pty);
	if(pty.err){
		printk("openpty failed, errno = %d\n", pty.err);
		return;
	}

	master = pty.master;
	slave = pty.slave;

	if((master == -1) || (slave == -1)){
		printk("openpty failed to allocate a pty\n");
		return;
	}

	if(tcgetattr(master, &tt) < 0)
		panic("check_sigio : tcgetattr failed, errno = %d\n", errno);
	cfmakeraw(&tt);
	if(tcsetattr(master, TCSADRAIN, &tt) < 0)
		panic("check_sigio : tcsetattr failed, errno = %d\n", errno);

	if((flags = fcntl(master, F_GETFL)) < 0)
		panic("tty_fds : fcntl F_GETFL failed, errno = %d\n", errno);

	if((fcntl(master, F_SETFL, flags | O_NONBLOCK | O_ASYNC) < 0) ||
	   (fcntl(master, F_SETOWN, os_getpid()) < 0))
		panic("check_sigio : fcntl F_SETFL or F_SETOWN failed, "
		      "errno = %d\n", errno);

	if((fcntl(slave, F_SETFL, flags | O_NONBLOCK) < 0))
		panic("check_sigio : fcntl F_SETFL failed, errno = %d\n", 
		      errno);

	if(sigaction(SIGIO, NULL, &old) < 0)
		panic("check_sigio : sigaction 1 failed, errno = %d\n", errno);
	new = old;
	new.sa_handler = handler;
	if(sigaction(SIGIO, &new, NULL) < 0)
		panic("check_sigio : sigaction 2 failed, errno = %d\n", errno);

	got_sigio = 0;
	(*proc)(master, slave);
		
	close(master);
	close(slave);

	if(sigaction(SIGIO, &old, NULL) < 0)
		panic("check_sigio : sigaction 3 failed, errno = %d\n", errno);
}

static void tty_output(int master, int slave)
{
	int n;
	char buf[512];

	printk("Checking that host ptys support output SIGIO...");

	memset(buf, 0, sizeof(buf));
	while(write(master, buf, sizeof(buf)) > 0) ;
	if(errno != EAGAIN)
		panic("check_sigio : write failed, errno = %d\n", errno);

	while(((n = read(slave, buf, sizeof(buf))) > 0) && !got_sigio) ;

	if(got_sigio){
		printk("Yes\n");
		pty_output_sigio = 1;
	}
	else if(errno == EAGAIN) printk("No, enabling workaround\n");
	else panic("check_sigio : read failed, errno = %d\n", errno);
}

static void tty_close(int master, int slave)
{
	printk("Checking that host ptys support SIGIO on close...");

	close(slave);
	if(got_sigio){
		printk("Yes\n");
		pty_close_sigio = 1;
	}
	else printk("No, enabling workaround\n");
}

void __init check_sigio(void)
{
	if(access("/dev/ptmx", R_OK) && access("/dev/ptyp0", R_OK)){
		printk("No pseudo-terminals available - skipping pty SIGIO "
		       "check\n");
		return;
	}
	check_one_sigio(tty_output);
	check_one_sigio(tty_close);
}

/* Protected by sigio_lock(), also used by sigio_cleanup, which is an 
 * exitcall.
 */
static int write_sigio_pid = -1;

/* These arrays are initialized before the sigio thread is started, and
 * the descriptors closed after it is killed.  So, it can't see them change.
 * On the UML side, they are changed under the sigio_lock.
 */
static int write_sigio_fds[2] = { -1, -1 };
static int sigio_private[2] = { -1, -1 };

struct pollfds {
	struct pollfd *poll;
	int size;
	int used;
};

/* Protected by sigio_lock().  Used by the sigio thread, but the UML thread
 * synchronizes with it.
 */
struct pollfds current_poll = {
	.poll  		= NULL,
	.size 		= 0,
	.used 		= 0
};

struct pollfds next_poll = {
	.poll  		= NULL,
	.size 		= 0,
	.used 		= 0
};

static int write_sigio_thread(void *unused)
{
	struct pollfds *fds, tmp;
	struct pollfd *p;
	int i, n, respond_fd;
	char c;

	fds = &current_poll;
	while(1){
		n = poll(fds->poll, fds->used, -1);
		if(n < 0){
			if(errno == EINTR) continue;
			printk("write_sigio_thread : poll returned %d, "
			       "errno = %d\n", n, errno);
		}
		for(i = 0; i < fds->used; i++){
			p = &fds->poll[i];
			if(p->revents == 0) continue;
			if(p->fd == sigio_private[1]){
				n = read(sigio_private[1], &c, sizeof(c));
				if(n != sizeof(c))
					printk("write_sigio_thread : "
					       "read failed, errno = %d\n",
					       errno);
				tmp = current_poll;
				current_poll = next_poll;
				next_poll = tmp;
				respond_fd = sigio_private[1];
			}
			else {
				respond_fd = write_sigio_fds[1];
				fds->used--;
				memmove(&fds->poll[i], &fds->poll[i + 1],
					(fds->used - i) * sizeof(*fds->poll));
			}

			n = write(respond_fd, &c, sizeof(c));
			if(n != sizeof(c))
				printk("write_sigio_thread : write failed, "
				       "errno = %d\n", errno);
		}
	}
}

static int need_poll(int n)
{
	if(n <= next_poll.size){
		next_poll.used = n;
		return(0);
	}
	if(next_poll.poll != NULL) kfree(next_poll.poll);
	next_poll.poll = um_kmalloc_atomic(n * sizeof(struct pollfd));
	if(next_poll.poll == NULL){
		printk("need_poll : failed to allocate new pollfds\n");
		next_poll.size = 0;
		next_poll.used = 0;
		return(-1);
	}
	next_poll.size = n;
	next_poll.used = n;
	return(0);
}

static void update_thread(void)
{
	unsigned long flags;
	int n;
	char c;

	flags = set_signals(0);
	n = write(sigio_private[0], &c, sizeof(c));
	if(n != sizeof(c)){
		printk("update_thread : write failed, errno = %d\n", errno);
		goto fail;
	}

	n = read(sigio_private[0], &c, sizeof(c));
	if(n != sizeof(c)){
		printk("update_thread : read failed, errno = %d\n", errno);
		goto fail;
	}

	set_signals(flags);
	return;
 fail:
	sigio_lock();
	if(write_sigio_pid != -1) 
		os_kill_process(write_sigio_pid, 1);
	write_sigio_pid = -1;
	close(sigio_private[0]);
	close(sigio_private[1]);	
	close(write_sigio_fds[0]);
	close(write_sigio_fds[1]);
	sigio_unlock();
	set_signals(flags);
}

int add_sigio_fd(int fd, int read)
{
	int err = 0, i, n, events;

	sigio_lock();
	for(i = 0; i < current_poll.used; i++){
		if(current_poll.poll[i].fd == fd) 
			goto out;
	}

	n = current_poll.used + 1;
	err = need_poll(n);
	if(err) 
		goto out;

	for(i = 0; i < current_poll.used; i++)
		next_poll.poll[i] = current_poll.poll[i];

	if(read) events = POLLIN;
	else events = POLLOUT;

	next_poll.poll[n - 1] = ((struct pollfd) { .fd  	= fd,
						   .events 	= events,
						   .revents 	= 0 });
	update_thread();
 out:
	sigio_unlock();
	return(err);
}

int ignore_sigio_fd(int fd)
{
	struct pollfd *p;
	int err = 0, i, n = 0;

	sigio_lock();
	for(i = 0; i < current_poll.used; i++){
		if(current_poll.poll[i].fd == fd) break;
	}
	if(i == current_poll.used)
		goto out;
	
	err = need_poll(current_poll.used - 1);
	if(err)
		goto out;

	for(i = 0; i < current_poll.used; i++){
		p = &current_poll.poll[i];
		if(p->fd != fd) next_poll.poll[n++] = current_poll.poll[i];
	}
	if(n == i){
		printk("ignore_sigio_fd : fd %d not found\n", fd);
		err = -1;
		goto out;
	}

	update_thread();
 out:
	sigio_unlock();
	return(err);
}

static int setup_initial_poll(int fd)
{
	struct pollfd *p;

	p = um_kmalloc(sizeof(struct pollfd));
	if(p == NULL){
		printk("setup_initial_poll : failed to allocate poll\n");
		return(-1);
	}
	*p = ((struct pollfd) { .fd  	= fd,
				.events 	= POLLIN,
				.revents 	= 0 });
	current_poll = ((struct pollfds) { .poll 	= p,
					   .used 	= 1,
					   .size 	= 1 });
	return(0);
}

void write_sigio_workaround(void)
{
	unsigned long stack;
	int err;

	sigio_lock();
	if(write_sigio_pid != -1)
		goto out;

	err = os_pipe(write_sigio_fds, 1, 1);
	if(err){
		printk("write_sigio_workaround - os_pipe 1 failed, "
		       "errno = %d\n", -err);
		goto out;
	}
	err = os_pipe(sigio_private, 1, 1);
	if(err){
		printk("write_sigio_workaround - os_pipe 2 failed, "
		       "errno = %d\n", -err);
		goto out_close1;
	}
	if(setup_initial_poll(sigio_private[1]))
		goto out_close2;

	write_sigio_pid = run_helper_thread(write_sigio_thread, NULL, 
					    CLONE_FILES | CLONE_VM, &stack, 0);

	if(write_sigio_pid < 0) goto out_close2;

	if(write_sigio_irq(write_sigio_fds[0])) 
		goto out_kill;

 out:
	sigio_unlock();
	return;

 out_kill:
	os_kill_process(write_sigio_pid, 1);
	write_sigio_pid = -1;
 out_close2:
	close(sigio_private[0]);
	close(sigio_private[1]);	
 out_close1:
	close(write_sigio_fds[0]);
	close(write_sigio_fds[1]);
	sigio_unlock();
}

int read_sigio_fd(int fd)
{
	int n;
	char c;

	n = read(fd, &c, sizeof(c));
	if(n != sizeof(c)){
		printk("read_sigio_fd - read failed, errno = %d\n", errno);
		return(-errno);
	}
	return(n);
}

static void sigio_cleanup(void)
{
	if(write_sigio_pid != -1)
		os_kill_process(write_sigio_pid, 1);
}

__uml_exitcall(sigio_cleanup);

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
