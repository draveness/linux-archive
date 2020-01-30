/*
 * Copyright (C) 2001 Lennert Buytenhek (buytenh@gnu.org) and 
 * James Leu (jleu@mindspring.net).
 * Copyright (C) 2001 by various other people who didn't put their name here.
 * Licensed under the GPL.
 */

#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include "net_user.h"
#include "daemon.h"
#include "kern_util.h"
#include "user_util.h"
#include "user.h"
#include "os.h"

#define MAX_PACKET (ETH_MAX_PACKET + ETH_HEADER_OTHER)

enum request_type { REQ_NEW_CONTROL };

#define SWITCH_MAGIC 0xfeedface

struct request_v3 {
	uint32_t magic;
	uint32_t version;
	enum request_type type;
	struct sockaddr_un sock;
};

static struct sockaddr_un *new_addr(void *name, int len)
{
	struct sockaddr_un *sun;

	sun = um_kmalloc(sizeof(struct sockaddr_un));
	if(sun == NULL){
		printk("new_addr: allocation of sockaddr_un failed\n");
		return(NULL);
	}
	sun->sun_family = AF_UNIX;
	memcpy(sun->sun_path, name, len);
	return(sun);
}

static int connect_to_switch(struct daemon_data *pri)
{
	struct sockaddr_un *ctl_addr = pri->ctl_addr;
	struct sockaddr_un *local_addr = pri->local_addr;
	struct sockaddr_un *sun;
	struct request_v3 req;
	int fd, n, err;

	if((pri->control = socket(AF_UNIX, SOCK_STREAM, 0)) < 0){
		printk("daemon_open : control socket failed, errno = %d\n", 
		       errno);		
		return(-errno);
	}

	if(connect(pri->control, (struct sockaddr *) ctl_addr, 
		   sizeof(*ctl_addr)) < 0){
		printk("daemon_open : control connect failed, errno = %d\n",
		       errno);
		err = -errno;
		goto out;
	}

	if((fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0){
		printk("daemon_open : data socket failed, errno = %d\n", 
		       errno);
		err = -errno;
		goto out;
	}
	if(bind(fd, (struct sockaddr *) local_addr, sizeof(*local_addr)) < 0){
		printk("daemon_open : data bind failed, errno = %d\n", 
		       errno);
		err = -errno;
		goto out_close;
	}

	sun = um_kmalloc(sizeof(struct sockaddr_un));
	if(sun == NULL){
		printk("new_addr: allocation of sockaddr_un failed\n");
		err = -ENOMEM;
		goto out_close;
	}

	req.magic = SWITCH_MAGIC;
	req.version = SWITCH_VERSION;
	req.type = REQ_NEW_CONTROL;
	req.sock = *local_addr;
	n = write(pri->control, &req, sizeof(req));
	if(n != sizeof(req)){
		printk("daemon_open : control setup request returned %d, "
		       "errno = %d\n", n, errno);
		err = -ENOTCONN;
		goto out;		
	}

	n = read(pri->control, sun, sizeof(*sun));
	if(n != sizeof(*sun)){
		printk("daemon_open : read of data socket returned %d, "
		       "errno = %d\n", n, errno);
		err = -ENOTCONN;
		goto out_close;		
	}

	pri->data_addr = sun;
	return(fd);

 out_close:
	close(fd);
 out:
	close(pri->control);
	return(err);
}

static void daemon_user_init(void *data, void *dev)
{
	struct daemon_data *pri = data;
	struct timeval tv;
	struct {
		char zero;
		int pid;
		int usecs;
	} name;

	if(!strcmp(pri->sock_type, "unix"))
		pri->ctl_addr = new_addr(pri->ctl_sock, 
					 strlen(pri->ctl_sock) + 1);
	name.zero = 0;
	name.pid = os_getpid();
	gettimeofday(&tv, NULL);
	name.usecs = tv.tv_usec;
	pri->local_addr = new_addr(&name, sizeof(name));
	pri->dev = dev;
	pri->fd = connect_to_switch(pri);
	if(pri->fd < 0){
		kfree(pri->local_addr);
		pri->local_addr = NULL;
	}
}

static int daemon_open(void *data)
{
	struct daemon_data *pri = data;
	return(pri->fd);
}

static void daemon_remove(void *data)
{
	struct daemon_data *pri = data;

	close(pri->fd);
	close(pri->control);
	if(pri->data_addr != NULL) kfree(pri->data_addr);
	if(pri->ctl_addr != NULL) kfree(pri->ctl_addr);
	if(pri->local_addr != NULL) kfree(pri->local_addr);
}

int daemon_user_write(int fd, void *buf, int len, struct daemon_data *pri)
{
	struct sockaddr_un *data_addr = pri->data_addr;

	return(net_sendto(fd, buf, len, data_addr, sizeof(*data_addr)));
}

static int daemon_set_mtu(int mtu, void *data)
{
	return(mtu);
}

struct net_user_info daemon_user_info = {
	.init		= daemon_user_init,
	.open		= daemon_open,
	.close	 	= NULL,
	.remove	 	= daemon_remove,
	.set_mtu	= daemon_set_mtu,
	.add_address	= NULL,
	.delete_address = NULL,
	.max_packet	= MAX_PACKET - ETH_HEADER_OTHER
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
