/*
 *			Simple traffic shaper for Linux NET3.
 *
 *	(c) Copyright 1996 Alan Cox <alan@redhat.com>, All Rights Reserved.
 *				http://www.redhat.com
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *	
 *	Neither Alan Cox nor CymruNet Ltd. admit liability nor provide 
 *	warranty for any of this software. This material is provided 
 *	"AS-IS" and at no charge.	
 *
 *	
 *	Algorithm:
 *
 *	Queue Frame:
 *		Compute time length of frame at regulated speed
 *		Add frame to queue at appropriate point
 *		Adjust time length computation for followup frames
 *		Any frame that falls outside of its boundaries is freed
 *
 *	We work to the following constants
 *
 *		SHAPER_QLEN	Maximum queued frames
 *		SHAPER_LATENCY	Bounding latency on a frame. Leaving this latency
 *				window drops the frame. This stops us queueing 
 *				frames for a long time and confusing a remote
 *				host.
 *		SHAPER_MAXSLIP	Maximum time a priority frame may jump forward.
 *				That bounds the penalty we will inflict on low
 *				priority traffic.
 *		SHAPER_BURST	Time range we call "now" in order to reduce
 *				system load. The more we make this the burstier
 *				the behaviour, the better local performance you
 *				get through packet clustering on routers and the
 *				worse the remote end gets to judge rtts.
 *
 *	This is designed to handle lower speed links ( < 200K/second or so). We
 *	run off a 100-150Hz base clock typically. This gives us a resolution at
 *	200Kbit/second of about 2Kbit or 256 bytes. Above that our timer
 *	resolution may start to cause much more burstiness in the traffic. We
 *	could avoid a lot of that by calling kick_shaper() at the end of the 
 *	tied device transmissions. If you run above about 100K second you 
 *	may need to tune the supposed speed rate for the right values.
 *
 *	BUGS:
 *		Downing the interface under the shaper before the shaper
 *		will render your machine defunct. Don't for now shape over
 *		PPP or SLIP therefore!
 *		This will be fixed in BETA4
 *
 * Update History :
 *
 *              bh_atomic() SMP races fixes and rewritten the locking code to
 *              be SMP safe and irq-mask friendly.
 *              NOTE: we can't use start_bh_atomic() in kick_shaper()
 *              because it's going to be recalled from an irq handler,
 *              and synchronize_bh() is a nono if called from irq context.
 *						1999  Andrea Arcangeli
 *
 *              Device statistics (tx_pakets, tx_bytes,
 *              tx_drops: queue_over_time and collisions: max_queue_exceded)
 *                               1999/06/18 Jordi Murgo <savage@apostols.org>
 *
 *		Use skb->cb for private data.
 *				 2000/03 Andi Kleen
 */
 
#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <net/dst.h>
#include <net/arp.h>
#include <linux/if_shaper.h>

struct shaper_cb { 
	__u32		shapelatency;		/* Latency on frame */
	__u32		shapeclock;		/* Time it should go out */
	__u32		shapelen;		/* Frame length in clocks */
	__u32		shapestamp;		/* Stamp for shaper    */
	__u16		shapepend;		/* Pending */
}; 
#define SHAPERCB(skb) ((struct shaper_cb *) ((skb)->cb))

int sh_debug;		/* Debug flag */

#define SHAPER_BANNER	"CymruNet Traffic Shaper BETA 0.04 for Linux 2.1\n"

/*
 *	Locking
 */
 
static int shaper_lock(struct shaper *sh)
{
	/*
	 *	Lock in an interrupt must fail
	 */
	while (test_and_set_bit(0, &sh->locked))
	{
		if (!in_interrupt())
			sleep_on(&sh->wait_queue);
		else
			return 0;
			
	}
	return 1;
}

static void shaper_kick(struct shaper *sh);

static void shaper_unlock(struct shaper *sh)
{
	clear_bit(0, &sh->locked);
	wake_up(&sh->wait_queue);
	shaper_kick(sh);
}

/*
 *	Compute clocks on a buffer
 */
  
static int shaper_clocks(struct shaper *shaper, struct sk_buff *skb)
{
 	int t=skb->len/shaper->bytespertick;
 	return t;
}

/*
 *	Set the speed of a shaper. We compute this in bytes per tick since
 *	thats how the machine wants to run. Quoted input is in bits per second
 *	as is traditional (note not BAUD). We assume 8 bit bytes. 
 */
  
static void shaper_setspeed(struct shaper *shaper, int bitspersec)
{
	shaper->bitspersec=bitspersec;
	shaper->bytespertick=(bitspersec/HZ)/8;
	if(!shaper->bytespertick)
		shaper->bytespertick++;
}

/*
 *	Throw a frame at a shaper.
 */
  
static int shaper_qframe(struct shaper *shaper, struct sk_buff *skb)
{
 	struct sk_buff *ptr;
   
 	/*
 	 *	Get ready to work on this shaper. Lock may fail if its
 	 *	an interrupt and locked.
 	 */
 	 
 	if(!shaper_lock(shaper))
 		return -1;
 	ptr=shaper->sendq.prev;
 	
 	/*
 	 *	Set up our packet details
 	 */
 	 
 	SHAPERCB(skb)->shapelatency=0;
 	SHAPERCB(skb)->shapeclock=shaper->recovery;
 	if(time_before(SHAPERCB(skb)->shapeclock, jiffies))
 		SHAPERCB(skb)->shapeclock=jiffies;
 	skb->priority=0;	/* short term bug fix */
 	SHAPERCB(skb)->shapestamp=jiffies;
 	
 	/*
 	 *	Time slots for this packet.
 	 */
 	 
 	SHAPERCB(skb)->shapelen= shaper_clocks(shaper,skb);
 	
#ifdef SHAPER_COMPLEX /* and broken.. */

 	while(ptr && ptr!=(struct sk_buff *)&shaper->sendq)
 	{
 		if(ptr->pri<skb->pri 
 			&& jiffies - SHAPERCB(ptr)->shapeclock < SHAPER_MAXSLIP)
 		{
 			struct sk_buff *tmp=ptr->prev;

 			/*
 			 *	It goes before us therefore we slip the length
 			 *	of the new frame.
 			 */

 			SHAPERCB(ptr)->shapeclock+=SHAPERCB(skb)->shapelen;
 			SHAPERCB(ptr)->shapelatency+=SHAPERCB(skb)->shapelen;

 			/*
 			 *	The packet may have slipped so far back it
 			 *	fell off.
 			 */
 			if(SHAPERCB(ptr)->shapelatency > SHAPER_LATENCY)
 			{
 				skb_unlink(ptr);
 				dev_kfree_skb(ptr);
 			}
 			ptr=tmp;
 		}
 		else
 			break;
 	}
 	if(ptr==NULL || ptr==(struct sk_buff *)&shaper->sendq)
 		skb_queue_head(&shaper->sendq,skb);
 	else
 	{
 		struct sk_buff *tmp;
 		/*
 		 *	Set the packet clock out time according to the
 		 *	frames ahead. Im sure a bit of thought could drop
 		 *	this loop.
 		 */
 		for(tmp=skb_peek(&shaper->sendq); tmp!=NULL && tmp!=ptr; tmp=tmp->next)
 			SHAPERCB(skb)->shapeclock+=tmp->shapelen;
 		skb_append(ptr,skb);
 	}
#else
	{
		struct sk_buff *tmp;
		/*
		 *	Up our shape clock by the time pending on the queue
		 *	(Should keep this in the shaper as a variable..)
		 */
		for(tmp=skb_peek(&shaper->sendq); tmp!=NULL && 
			tmp!=(struct sk_buff *)&shaper->sendq; tmp=tmp->next)
			SHAPERCB(skb)->shapeclock+=SHAPERCB(tmp)->shapelen;
		/*
		 *	Queue over time. Spill packet.
		 */
		if(SHAPERCB(skb)->shapeclock-jiffies > SHAPER_LATENCY) {
			dev_kfree_skb(skb);
			shaper->stats.tx_dropped++;
		} else
			skb_queue_tail(&shaper->sendq, skb);
	}
#endif 	
	if(sh_debug)
 		printk("Frame queued.\n");
 	if(skb_queue_len(&shaper->sendq)>SHAPER_QLEN)
 	{
 		ptr=skb_dequeue(&shaper->sendq);
                dev_kfree_skb(ptr);
                shaper->stats.collisions++;
 	}
 	shaper_unlock(shaper);
 	return 0;
}

/*
 *	Transmit from a shaper
 */
 
static void shaper_queue_xmit(struct shaper *shaper, struct sk_buff *skb)
{
	struct sk_buff *newskb=skb_clone(skb, GFP_ATOMIC);
	if(sh_debug)
		printk("Kick frame on %p\n",newskb);
	if(newskb)
	{
		newskb->dev=shaper->dev;
		newskb->priority=2;
		if(sh_debug)
			printk("Kick new frame to %s, %d\n",
				shaper->dev->name,newskb->priority);
		dev_queue_xmit(newskb);

                shaper->stats.tx_bytes+=newskb->len;
		shaper->stats.tx_packets++;

                if(sh_debug)
			printk("Kicked new frame out.\n");
		dev_kfree_skb(skb);
	}
}

/*
 *	Timer handler for shaping clock
 */
 
static void shaper_timer(unsigned long data)
{
	struct shaper *sh=(struct shaper *)data;
	shaper_kick(sh);
}

/*
 *	Kick a shaper queue and try and do something sensible with the 
 *	queue. 
 */

static void shaper_kick(struct shaper *shaper)
{
	struct sk_buff *skb;
	
	/*
	 *	Shaper unlock will kick
	 */
	 
	if (test_and_set_bit(0, &shaper->locked))
	{
		if(sh_debug)
			printk("Shaper locked.\n");
		mod_timer(&shaper->timer, jiffies);
		return;
	}

		
	/*
	 *	Walk the list (may be empty)
	 */
	 
	while((skb=skb_peek(&shaper->sendq))!=NULL)
	{
		/*
		 *	Each packet due to go out by now (within an error
		 *	of SHAPER_BURST) gets kicked onto the link 
		 */
		 
		if(sh_debug)
			printk("Clock = %d, jiffies = %ld\n", SHAPERCB(skb)->shapeclock, jiffies);
		if(time_before_eq(SHAPERCB(skb)->shapeclock - jiffies, SHAPER_BURST))
		{
			/*
			 *	Pull the frame and get interrupts back on.
			 */
			 
			skb_unlink(skb);
			if (shaper->recovery < 
			    SHAPERCB(skb)->shapeclock + SHAPERCB(skb)->shapelen)
				shaper->recovery = SHAPERCB(skb)->shapeclock + SHAPERCB(skb)->shapelen;
			/*
			 *	Pass on to the physical target device via
			 *	our low level packet thrower.
			 */
			
			SHAPERCB(skb)->shapepend=0;
			shaper_queue_xmit(shaper, skb);	/* Fire */
		}
		else
			break;
	}

	/*
	 *	Next kick.
	 */
	 
	if(skb!=NULL)
		mod_timer(&shaper->timer, SHAPERCB(skb)->shapeclock);

	clear_bit(0, &shaper->locked);
}


/*
 *	Flush the shaper queues on a closedown
 */
 
static void shaper_flush(struct shaper *shaper)
{
	struct sk_buff *skb;
 	if(!shaper_lock(shaper))
	{
		printk(KERN_ERR "shaper: shaper_flush() called by an irq!\n");
 		return;
	}
	while((skb=skb_dequeue(&shaper->sendq))!=NULL)
		dev_kfree_skb(skb);
	shaper_unlock(shaper);
}

/*
 *	Bring the interface up. We just disallow this until a 
 *	bind.
 */

static int shaper_open(struct net_device *dev)
{
	struct shaper *shaper=dev->priv;
	
	/*
	 *	Can't open until attached.
	 *	Also can't open until speed is set, or we'll get
	 *	a division by zero.
	 */
	 
	if(shaper->dev==NULL)
		return -ENODEV;
	if(shaper->bitspersec==0)
		return -EINVAL;
	return 0;
}

/*
 *	Closing a shaper flushes the queues.
 */
 
static int shaper_close(struct net_device *dev)
{
	struct shaper *shaper=dev->priv;
	shaper_flush(shaper);
	del_timer_sync(&shaper->timer);
	return 0;
}

/*
 *	Revectored calls. We alter the parameters and call the functions
 *	for our attached device. This enables us to bandwidth allocate after
 *	ARP and other resolutions and not before.
 */


static int shaper_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct shaper *sh=dev->priv;
	return shaper_qframe(sh, skb);
}

static struct net_device_stats *shaper_get_stats(struct net_device *dev)
{
     	struct shaper *sh=dev->priv;
	return &sh->stats;
}

static int shaper_header(struct sk_buff *skb, struct net_device *dev, 
	unsigned short type, void *daddr, void *saddr, unsigned len)
{
	struct shaper *sh=dev->priv;
	int v;
	if(sh_debug)
		printk("Shaper header\n");
	skb->dev=sh->dev;
	v=sh->hard_header(skb,sh->dev,type,daddr,saddr,len);
	skb->dev=dev;
	return v;
}

static int shaper_rebuild_header(struct sk_buff *skb)
{
	struct shaper *sh=skb->dev->priv;
	struct net_device *dev=skb->dev;
	int v;
	if(sh_debug)
		printk("Shaper rebuild header\n");
	skb->dev=sh->dev;
	v=sh->rebuild_header(skb);
	skb->dev=dev;
	return v;
}

#if 0
static int shaper_cache(struct neighbour *neigh, struct hh_cache *hh)
{
	struct shaper *sh=neigh->dev->priv;
	struct net_device *tmp;
	int ret;
	if(sh_debug)
		printk("Shaper header cache bind\n");
	tmp=neigh->dev;
	neigh->dev=sh->dev;
	ret=sh->hard_header_cache(neigh,hh);
	neigh->dev=tmp;
	return ret;
}

static void shaper_cache_update(struct hh_cache *hh, struct net_device *dev,
	unsigned char *haddr)
{
	struct shaper *sh=dev->priv;
	if(sh_debug)
		printk("Shaper cache update\n");
	sh->header_cache_update(hh, sh->dev, haddr);
}
#endif

#ifdef CONFIG_INET

static int shaper_neigh_setup(struct neighbour *n)
{
#ifdef CONFIG_INET
	if (n->nud_state == NUD_NONE) {
		n->ops = &arp_broken_ops;
		n->output = n->ops->output;
	}
#endif	
	return 0;
}

static int shaper_neigh_setup_dev(struct net_device *dev, struct neigh_parms *p)
{
#ifdef CONFIG_INET
	if (p->tbl->family == AF_INET) {
		p->neigh_setup = shaper_neigh_setup;
		p->ucast_probes = 0;
		p->mcast_probes = 0;
	}
#endif	
	return 0;
}

#else /* !(CONFIG_INET) */

static int shaper_neigh_setup_dev(struct net_device *dev, struct neigh_parms *p)
{
	return 0;
}

#endif

static int shaper_attach(struct net_device *shdev, struct shaper *sh, struct net_device *dev)
{
	sh->dev = dev;
	sh->hard_start_xmit=dev->hard_start_xmit;
	sh->get_stats=dev->get_stats;
	if(dev->hard_header)
	{
		sh->hard_header=dev->hard_header;
		shdev->hard_header = shaper_header;
	}
	else
		shdev->hard_header = NULL;
		
	if(dev->rebuild_header)
	{
		sh->rebuild_header	= dev->rebuild_header;
		shdev->rebuild_header	= shaper_rebuild_header;
	}
	else
		shdev->rebuild_header	= NULL;
	
#if 0
	if(dev->hard_header_cache)
	{
		sh->hard_header_cache	= dev->hard_header_cache;
		shdev->hard_header_cache= shaper_cache;
	}
	else
	{
		shdev->hard_header_cache= NULL;
	}
			
	if(dev->header_cache_update)
	{
		sh->header_cache_update	= dev->header_cache_update;
		shdev->header_cache_update = shaper_cache_update;
	}
	else
		shdev->header_cache_update= NULL;
#else
	shdev->header_cache_update = NULL;
	shdev->hard_header_cache = NULL;
#endif
	shdev->neigh_setup = shaper_neigh_setup_dev;
	
	shdev->hard_header_len=dev->hard_header_len;
	shdev->type=dev->type;
	shdev->addr_len=dev->addr_len;
	shdev->mtu=dev->mtu;
	sh->bitspersec=0;
	return 0;
}

static int shaper_ioctl(struct net_device *dev,  struct ifreq *ifr, int cmd)
{
	struct shaperconf *ss= (struct shaperconf *)&ifr->ifr_data;
	struct shaper *sh=dev->priv;
	
	if(ss->ss_cmd == SHAPER_SET_DEV || ss->ss_cmd == SHAPER_SET_SPEED)
	{
		if(!capable(CAP_NET_ADMIN))
			return -EPERM;
	}
	
	switch(ss->ss_cmd)
	{
		case SHAPER_SET_DEV:
		{
			struct net_device *them=__dev_get_by_name(ss->ss_name);
			if(them==NULL)
				return -ENODEV;
			if(sh->dev)
				return -EBUSY;
			return shaper_attach(dev,dev->priv, them);
		}
		case SHAPER_GET_DEV:
			if(sh->dev==NULL)
				return -ENODEV;
			strcpy(ss->ss_name, sh->dev->name);
			return 0;
		case SHAPER_SET_SPEED:
			shaper_setspeed(sh,ss->ss_speed);
			return 0;
		case SHAPER_GET_SPEED:
			ss->ss_speed=sh->bitspersec;
			return 0;
		default:
			return -EINVAL;
	}
}

static void shaper_init_priv(struct net_device *dev)
{
	struct shaper *sh = dev->priv;

	skb_queue_head_init(&sh->sendq);
	init_timer(&sh->timer);
	sh->timer.function=shaper_timer;
	sh->timer.data=(unsigned long)sh;
	init_waitqueue_head(&sh->wait_queue);
}

/*
 *	Add a shaper device to the system
 */
 
static int __init shaper_probe(struct net_device *dev)
{
	/*
	 *	Set up the shaper.
	 */

	SET_MODULE_OWNER(dev);

	shaper_init_priv(dev);

	dev->open		= shaper_open;
	dev->stop		= shaper_close;
	dev->hard_start_xmit 	= shaper_start_xmit;
	dev->get_stats 		= shaper_get_stats;
	dev->set_multicast_list = NULL;
	
	/*
	 *	Intialise the packet queues
	 */
	 
	dev_init_buffers(dev);
	
	/*
	 *	Handlers for when we attach to a device.
	 */

	dev->hard_header 	= shaper_header;
	dev->rebuild_header 	= shaper_rebuild_header;
#if 0
	dev->hard_header_cache	= shaper_cache;
	dev->header_cache_update= shaper_cache_update;
#endif
	dev->neigh_setup	= shaper_neigh_setup_dev;
	dev->do_ioctl		= shaper_ioctl;
	dev->hard_header_len	= 0;
	dev->type		= ARPHRD_ETHER;	/* initially */
	dev->set_mac_address	= NULL;
	dev->mtu		= 1500;
	dev->addr_len		= 0;
	dev->tx_queue_len	= 10;
	dev->flags		= 0;
		
	/*
	 *	Shaper is ok
	 */	
	 
	return 0;
}
 
static int shapers = 1;
#ifdef MODULE

MODULE_PARM(shapers, "i");

#else /* MODULE */

static int __init set_num_shapers(char *str)
{
	shapers = simple_strtol(str, NULL, 0);
	return 1;
}

__setup("shapers=", set_num_shapers);

#endif /* MODULE */

static struct net_device *devs;

static int __init shaper_init(void)
{
	int i, err;
	size_t alloc_size;
	struct shaper *sp;
	unsigned int shapers_registered = 0;

	if (shapers < 1)
		return -ENODEV;

	alloc_size = (sizeof(*devs) * shapers) +
		     (sizeof(struct shaper) * shapers);
	devs = kmalloc(alloc_size, GFP_KERNEL);
	if (!devs)
		return -ENOMEM;
	memset(devs, 0, alloc_size);
	sp = (struct shaper *) &devs[shapers];

	for (i = 0; i < shapers; i++) {
		err = dev_alloc_name(&devs[i], "shaper%d");
		if (err < 0)
			break;
		devs[i].init = shaper_probe;
		devs[i].priv = &sp[i];
		if (register_netdev(&devs[i]))
			break;
		shapers_registered++;
	}

	if (!shapers_registered) {
		kfree(devs);
		devs = NULL;
	}

	return (shapers_registered ? 0 : -ENODEV);
}

static void __exit shaper_exit (void)
{
	kfree(devs);
	devs = NULL;
}

module_init(shaper_init);
module_exit(shaper_exit);

