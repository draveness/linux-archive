/* net/atm/addr.c - Local ATM address registry */

/* Written 1995-2000 by Werner Almesberger, EPFL LRC/ICA */


#include <linux/atm.h>
#include <linux/atmdev.h>
#include <linux/sched.h>
#include <asm/uaccess.h>

#include "signaling.h"
#include "addr.h"


static int check_addr(struct sockaddr_atmsvc *addr)
{
	int i;

	if (addr->sas_family != AF_ATMSVC) return -EAFNOSUPPORT;
	if (!*addr->sas_addr.pub)
		return *addr->sas_addr.prv ? 0 : -EINVAL;
	for (i = 1; i < ATM_E164_LEN+1; i++) /* make sure it's \0-terminated */
		if (!addr->sas_addr.pub[i]) return 0;
	return -EINVAL;
}


static int identical(struct sockaddr_atmsvc *a,struct sockaddr_atmsvc *b)
{
	if (*a->sas_addr.prv)
		if (memcmp(a->sas_addr.prv,b->sas_addr.prv,ATM_ESA_LEN))
			return 0;
	if (!*a->sas_addr.pub) return !*b->sas_addr.pub;
	if (!*b->sas_addr.pub) return 0;
	return !strcmp(a->sas_addr.pub,b->sas_addr.pub);
}


/*
 * Avoid modification of any list of local interfaces while reading it
 * (which may involve page faults and therefore rescheduling)
 */

static DECLARE_MUTEX(local_lock);
extern  spinlock_t atm_dev_lock;

static void notify_sigd(struct atm_dev *dev)
{
	struct sockaddr_atmpvc pvc;

	pvc.sap_addr.itf = dev->number;
	sigd_enq(NULL,as_itf_notify,NULL,&pvc,NULL);
}


void reset_addr(struct atm_dev *dev)
{
	struct atm_dev_addr *this;

	down(&local_lock);
	spin_lock (&atm_dev_lock);		
	while (dev->local) {
		this = dev->local;
		dev->local = this->next;
		kfree(this);
	}
	up(&local_lock);
	spin_unlock (&atm_dev_lock);
	notify_sigd(dev);
}


int add_addr(struct atm_dev *dev,struct sockaddr_atmsvc *addr)
{
	struct atm_dev_addr **walk;
	int error;

	error = check_addr(addr);
	if (error) return error;
	down(&local_lock);
	for (walk = &dev->local; *walk; walk = &(*walk)->next)
		if (identical(&(*walk)->addr,addr)) {
			up(&local_lock);
			return -EEXIST;
		}
	*walk = kmalloc(sizeof(struct atm_dev_addr),GFP_KERNEL);
	if (!*walk) {
		up(&local_lock);
		return -ENOMEM;
	}
	(*walk)->addr = *addr;
	(*walk)->next = NULL;
	up(&local_lock);
	notify_sigd(dev);
	return 0;
}


int del_addr(struct atm_dev *dev,struct sockaddr_atmsvc *addr)
{
	struct atm_dev_addr **walk,*this;
	int error;

	error = check_addr(addr);
	if (error) return error;
	down(&local_lock);
	for (walk = &dev->local; *walk; walk = &(*walk)->next)
		if (identical(&(*walk)->addr,addr)) break;
	if (!*walk) {
		up(&local_lock);
		return -ENOENT;
	}
	this = *walk;
	*walk = this->next;
	kfree(this);
	up(&local_lock);
	notify_sigd(dev);
	return 0;
}


int get_addr(struct atm_dev *dev,struct sockaddr_atmsvc *u_buf,int size)
{
	struct atm_dev_addr *walk;
	int total;

	down(&local_lock);
	total = 0;
	for (walk = dev->local; walk; walk = walk->next) {
		total += sizeof(struct sockaddr_atmsvc);
		if (total > size) {
			up(&local_lock);
			return -E2BIG;
		}
		if (copy_to_user(u_buf,&walk->addr,
		    sizeof(struct sockaddr_atmsvc))) {
			up(&local_lock);
			return -EFAULT;
		}
		u_buf++;
	}
	up(&local_lock);
	return total;
}
