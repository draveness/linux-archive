/*
 * NET3:	Token ring device handling subroutines
 * 
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Fixes:       3 Feb 97 Paul Norton <pnorton@cts.com> Minor routing fixes.
 *              Added rif table to /proc/net/tr_rif and rif timeout to
 *              /proc/sys/net/token-ring/rif_timeout.
 *              22 Jun 98 Paul Norton <p.norton@computer.org> Rearranged
 *              tr_header and tr_type_trans to handle passing IPX SNAP and
 *              802.2 through the correct layers. Eliminated tr_reformat.
 *        
 */

#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/trdevice.h>
#include <linux/skbuff.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/net.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <net/arp.h>

static void tr_add_rif_info(struct trh_hdr *trh, struct net_device *dev);
static void rif_check_expire(unsigned long dummy);

#define TR_SR_DEBUG 0

/*
 *	Each RIF entry we learn is kept this way
 */
 
struct rif_cache_s {	
	unsigned char addr[TR_ALEN];
	int iface;
	__u16 rcf;
	__u16 rseg[8];
	struct rif_cache_s *next;
	unsigned long last_used;
	unsigned char local_ring;
};

#define RIF_TABLE_SIZE 32

/*
 *	We hash the RIF cache 32 ways. We do after all have to look it
 *	up a lot.
 */
 
static struct rif_cache_s *rif_table[RIF_TABLE_SIZE];

static spinlock_t rif_lock = SPIN_LOCK_UNLOCKED;


/*
 *	Garbage disposal timer.
 */
 
static struct timer_list rif_timer;

int sysctl_tr_rif_timeout = 60*10*HZ;

static inline unsigned long rif_hash(const unsigned char *addr)
{
	unsigned long x;

	x = addr[0];
	x = (x << 2) ^ addr[1];
	x = (x << 2) ^ addr[2];
	x = (x << 2) ^ addr[3];
	x = (x << 2) ^ addr[4];
	x = (x << 2) ^ addr[5];

	x ^= x >> 8;

	return x & (RIF_TABLE_SIZE - 1);
}

/*
 *	Put the headers on a token ring packet. Token ring source routing
 *	makes this a little more exciting than on ethernet.
 */
 
int tr_header(struct sk_buff *skb, struct net_device *dev, unsigned short type,
              void *daddr, void *saddr, unsigned len) 
{
	struct trh_hdr *trh;
	int hdr_len;

	/* 
	 * Add the 802.2 SNAP header if IP as the IPv4/IPv6 code calls  
	 * dev->hard_header directly.
	 */
	if (type == ETH_P_IP || type == ETH_P_IPV6 || type == ETH_P_ARP)
	{
		struct trllc *trllc;

		hdr_len = sizeof(struct trh_hdr) + sizeof(struct trllc);
		trh = (struct trh_hdr *)skb_push(skb, hdr_len);
		trllc = (struct trllc *)(trh+1);
		trllc->dsap = trllc->ssap = EXTENDED_SAP;
		trllc->llc = UI_CMD;
		trllc->protid[0] = trllc->protid[1] = trllc->protid[2] = 0x00;
		trllc->ethertype = htons(type);
	}
	else
	{
		hdr_len = sizeof(struct trh_hdr);
		trh = (struct trh_hdr *)skb_push(skb, hdr_len);	
	}

	trh->ac=AC;
	trh->fc=LLC_FRAME;

	if(saddr)
		memcpy(trh->saddr,saddr,dev->addr_len);
	else
		memcpy(trh->saddr,dev->dev_addr,dev->addr_len);

	/*
	 *	Build the destination and then source route the frame
	 */
	 
	if(daddr) 
	{
		memcpy(trh->daddr,daddr,dev->addr_len);
		tr_source_route(skb,trh,dev);
		return(hdr_len);
	}

	return -hdr_len;
}
	
/*
 *	A neighbour discovery of some species (eg arp) has completed. We
 *	can now send the packet.
 */
 
int tr_rebuild_header(struct sk_buff *skb) 
{
	struct trh_hdr *trh=(struct trh_hdr *)skb->data;
	struct trllc *trllc=(struct trllc *)(skb->data+sizeof(struct trh_hdr));
	struct net_device *dev = skb->dev;

	/*
	 *	FIXME: We don't yet support IPv6 over token rings
	 */
	 
	if(trllc->ethertype != htons(ETH_P_IP)) {
		printk("tr_rebuild_header: Don't know how to resolve type %04X addresses ?\n",(unsigned int)htons(trllc->ethertype));
		return 0;
	}

#ifdef CONFIG_INET
	if(arp_find(trh->daddr, skb)) {
			return 1;
	}
	else 
#endif	
	{	
		tr_source_route(skb,trh,dev); 
		return 0;
	}
}
	
/*
 *	Some of this is a bit hackish. We intercept RIF information
 *	used for source routing. We also grab IP directly and don't feed
 *	it via SNAP.
 */
 
unsigned short tr_type_trans(struct sk_buff *skb, struct net_device *dev) 
{

	struct trh_hdr *trh=(struct trh_hdr *)skb->data;
	struct trllc *trllc;
	unsigned riflen=0;
	
	skb->mac.raw = skb->data;
	
       	if(trh->saddr[0] & TR_RII)
		riflen = (ntohs(trh->rcf) & TR_RCF_LEN_MASK) >> 8;

	trllc = (struct trllc *)(skb->data+sizeof(struct trh_hdr)-TR_MAXRIFLEN+riflen);

	skb_pull(skb,sizeof(struct trh_hdr)-TR_MAXRIFLEN+riflen);

	if(*trh->daddr & 0x80) 
	{
		if(!memcmp(trh->daddr,dev->broadcast,TR_ALEN)) 	
			skb->pkt_type=PACKET_BROADCAST;
		else
			skb->pkt_type=PACKET_MULTICAST;
	}
	else if ( (trh->daddr[0] & 0x01) && (trh->daddr[1] & 0x00) && (trh->daddr[2] & 0x5E))
	{
		skb->pkt_type=PACKET_MULTICAST;
	}
	else if(dev->flags & IFF_PROMISC) 
	{
		if(memcmp(trh->daddr, dev->dev_addr, TR_ALEN))
			skb->pkt_type=PACKET_OTHERHOST;
	}

	if ((skb->pkt_type != PACKET_BROADCAST) &&
	    (skb->pkt_type != PACKET_MULTICAST))
		tr_add_rif_info(trh,dev) ; 

	/*
	 * Strip the SNAP header from ARP packets since we don't 
	 * pass them through to the 802.2/SNAP layers.
	 */

	if (trllc->dsap == EXTENDED_SAP &&
	    (trllc->ethertype == ntohs(ETH_P_IP) ||
	     trllc->ethertype == ntohs(ETH_P_IPV6) ||
	     trllc->ethertype == ntohs(ETH_P_ARP)))
	{
		skb_pull(skb, sizeof(struct trllc));
		return trllc->ethertype;
	}

	return ntohs(ETH_P_802_2);
}

/*
 *	We try to do source routing... 
 */

void tr_source_route(struct sk_buff *skb,struct trh_hdr *trh,struct net_device *dev) 
{
	int slack;
	unsigned int hash;
	struct rif_cache_s *entry;
	unsigned char *olddata;
	static const unsigned char mcast_func_addr[] 
		= {0xC0,0x00,0x00,0x04,0x00,0x00};
	
	spin_lock_bh(&rif_lock);

	/*
	 *	Broadcasts are single route as stated in RFC 1042 
	 */
	if( (!memcmp(&(trh->daddr[0]),&(dev->broadcast[0]),TR_ALEN)) ||
	    (!memcmp(&(trh->daddr[0]),&(mcast_func_addr[0]), TR_ALEN))  )
	{
		trh->rcf=htons((((sizeof(trh->rcf)) << 8) & TR_RCF_LEN_MASK)  
			       | TR_RCF_FRAME2K | TR_RCF_LIMITED_BROADCAST);
		trh->saddr[0]|=TR_RII;
	}
	else 
	{
		hash = rif_hash(trh->daddr);
		/*
		 *	Walk the hash table and look for an entry
		 */
		for(entry=rif_table[hash];entry && memcmp(&(entry->addr[0]),&(trh->daddr[0]),TR_ALEN);entry=entry->next);

		/*
		 *	If we found an entry we can route the frame.
		 */
		if(entry) 
		{
#if TR_SR_DEBUG
printk("source routing for %02X:%02X:%02X:%02X:%02X:%02X\n",trh->daddr[0],
		  trh->daddr[1],trh->daddr[2],trh->daddr[3],trh->daddr[4],trh->daddr[5]);
#endif
			if(!entry->local_ring && (ntohs(entry->rcf) & TR_RCF_LEN_MASK) >> 8)
			{
				trh->rcf=entry->rcf;
				memcpy(&trh->rseg[0],&entry->rseg[0],8*sizeof(unsigned short));
				trh->rcf^=htons(TR_RCF_DIR_BIT);	
				trh->rcf&=htons(0x1fff);	/* Issam Chehab <ichehab@madge1.demon.co.uk> */

				trh->saddr[0]|=TR_RII;
#if TR_SR_DEBUG
				printk("entry found with rcf %04x\n", entry->rcf);
			}
			else
			{
				printk("entry found but without rcf length, local=%02x\n", entry->local_ring);
#endif
			}
			entry->last_used=jiffies;
		}
		else 
		{
			/*
			 *	Without the information we simply have to shout
			 *	on the wire. The replies should rapidly clean this
			 *	situation up.
			 */
			trh->rcf=htons((((sizeof(trh->rcf)) << 8) & TR_RCF_LEN_MASK)  
				       | TR_RCF_FRAME2K | TR_RCF_LIMITED_BROADCAST);
			trh->saddr[0]|=TR_RII;
#if TR_SR_DEBUG
			printk("no entry in rif table found - broadcasting frame\n");
#endif
		}
	}

	/* Compress the RIF here so we don't have to do it in the driver(s) */
	if (!(trh->saddr[0] & 0x80))
		slack = 18;
	else 
		slack = 18 - ((ntohs(trh->rcf) & TR_RCF_LEN_MASK)>>8);
	olddata = skb->data;
	spin_unlock_bh(&rif_lock);

	skb_pull(skb, slack);
	memmove(skb->data, olddata, sizeof(struct trh_hdr) - slack);
}

/*
 *	We have learned some new RIF information for our source
 *	routing.
 */
 
static void tr_add_rif_info(struct trh_hdr *trh, struct net_device *dev)
{
	unsigned int hash, rii_p = 0;
	struct rif_cache_s *entry;


	spin_lock_bh(&rif_lock);
	
	/*
	 *	Firstly see if the entry exists
	 */

       	if(trh->saddr[0] & TR_RII)
	{
		trh->saddr[0]&=0x7f;
		if (((ntohs(trh->rcf) & TR_RCF_LEN_MASK) >> 8) > 2)
		{
			rii_p = 1;
	        }
	}

	hash = rif_hash(trh->saddr);
	for(entry=rif_table[hash];entry && memcmp(&(entry->addr[0]),&(trh->saddr[0]),TR_ALEN);entry=entry->next);

	if(entry==NULL) 
	{
#if TR_SR_DEBUG
printk("adding rif_entry: addr:%02X:%02X:%02X:%02X:%02X:%02X rcf:%04X\n",
		trh->saddr[0],trh->saddr[1],trh->saddr[2],
       		trh->saddr[3],trh->saddr[4],trh->saddr[5],
		ntohs(trh->rcf));
#endif
		/*
		 *	Allocate our new entry. A failure to allocate loses
		 *	use the information. This is harmless.
		 *
		 *	FIXME: We ought to keep some kind of cache size
		 *	limiting and adjust the timers to suit.
		 */
		entry=kmalloc(sizeof(struct rif_cache_s),GFP_ATOMIC);

		if(!entry) 
		{
			printk(KERN_DEBUG "tr.c: Couldn't malloc rif cache entry !\n");
			spin_unlock_bh(&rif_lock);
			return;
		}

		memcpy(&(entry->addr[0]),&(trh->saddr[0]),TR_ALEN);
		entry->iface = dev->ifindex;
		entry->next=rif_table[hash];
		entry->last_used=jiffies;
		rif_table[hash]=entry;

		if (rii_p)
		{
			entry->rcf = trh->rcf & htons((unsigned short)~TR_RCF_BROADCAST_MASK);
			memcpy(&(entry->rseg[0]),&(trh->rseg[0]),8*sizeof(unsigned short));
			entry->local_ring = 0;
			trh->saddr[0]|=TR_RII; /* put the routing indicator back for tcpdump */
		}
		else
		{
			entry->local_ring = 1;
		}
	} 	
	else	/* Y. Tahara added */
	{ 
		/*
		 *	Update existing entries
		 */
		if (!entry->local_ring) 
		    if (entry->rcf != (trh->rcf & htons((unsigned short)~TR_RCF_BROADCAST_MASK)) &&
			 !(trh->rcf & htons(TR_RCF_BROADCAST_MASK)))
		    {
#if TR_SR_DEBUG
printk("updating rif_entry: addr:%02X:%02X:%02X:%02X:%02X:%02X rcf:%04X\n",
		trh->saddr[0],trh->saddr[1],trh->saddr[2],
		trh->saddr[3],trh->saddr[4],trh->saddr[5],
		ntohs(trh->rcf));
#endif
			    entry->rcf = trh->rcf & htons((unsigned short)~TR_RCF_BROADCAST_MASK);
        		    memcpy(&(entry->rseg[0]),&(trh->rseg[0]),8*sizeof(unsigned short));
		    }                                         
           	entry->last_used=jiffies;               
	}
	spin_unlock_bh(&rif_lock);
}

/*
 *	Scan the cache with a timer and see what we need to throw out.
 */

static void rif_check_expire(unsigned long dummy) 
{
	int i;
	unsigned long next_interval = jiffies + sysctl_tr_rif_timeout/2;

	spin_lock_bh(&rif_lock);
	
	for(i =0; i < RIF_TABLE_SIZE; i++) {
		struct rif_cache_s *entry, **pentry;
		
		pentry = rif_table+i;
		while((entry=*pentry) != NULL) {
			unsigned long expires
				= entry->last_used + sysctl_tr_rif_timeout;

			if (time_before_eq(expires, jiffies)) {
				*pentry = entry->next;
				kfree(entry);
			} else {
				pentry = &entry->next;

				if (time_before(expires, next_interval))
					next_interval = expires;
			}
		}
	}
	
	spin_unlock_bh(&rif_lock);

	mod_timer(&rif_timer, next_interval);

}

/*
 *	Generate the /proc/net information for the token ring RIF
 *	routing.
 */
 
#ifdef CONFIG_PROC_FS

static struct rif_cache_s *rif_get_idx(loff_t pos)
{
	int i;
	struct rif_cache_s *entry;
	loff_t off = 0;

	for(i = 0; i < RIF_TABLE_SIZE; i++) 
		for(entry = rif_table[i]; entry; entry = entry->next) {
			if (off == pos)
				return entry;
			++off;
		}

	return NULL;
}

static void *rif_seq_start(struct seq_file *seq, loff_t *pos)
{
	spin_lock_bh(&rif_lock);

	return *pos ? rif_get_idx(*pos - 1) : SEQ_START_TOKEN;
}

static void *rif_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	int i;
	struct rif_cache_s *ent = v;

	++*pos;

	if (v == SEQ_START_TOKEN) {
		i = -1;
		goto scan;
	}

	if (ent->next) 
		return ent->next;

	i = rif_hash(ent->addr);
 scan:
	while (++i < RIF_TABLE_SIZE) {
		if ((ent = rif_table[i]) != NULL)
			return ent;
	}
	return NULL;
}

static void rif_seq_stop(struct seq_file *seq, void *v)
{
	spin_unlock_bh(&rif_lock);
}

static int rif_seq_show(struct seq_file *seq, void *v)
{
	int j, rcf_len, segment, brdgnmb;
	struct rif_cache_s *entry = v;

	if (v == SEQ_START_TOKEN)
		seq_puts(seq,
		     "if     TR address       TTL   rcf   routing segments\n");
	else {
		struct net_device *dev = dev_get_by_index(entry->iface);
		long ttl = (long) (entry->last_used + sysctl_tr_rif_timeout)
				- (long) jiffies;

		seq_printf(seq, "%s %02X:%02X:%02X:%02X:%02X:%02X %7li ",
			   dev?dev->name:"?",
			   entry->addr[0],entry->addr[1],entry->addr[2],
			   entry->addr[3],entry->addr[4],entry->addr[5],
			   ttl/HZ);

			if (entry->local_ring)
			        seq_puts(seq, "local\n");
			else {

				seq_printf(seq, "%04X", ntohs(entry->rcf));
				rcf_len = ((ntohs(entry->rcf) & TR_RCF_LEN_MASK)>>8)-2; 
				if (rcf_len)
				        rcf_len >>= 1;
				for(j = 1; j < rcf_len; j++) {
					if(j==1) {
						segment=ntohs(entry->rseg[j-1])>>4;
						seq_printf(seq,"  %03X",segment);
					};
					segment=ntohs(entry->rseg[j])>>4;
					brdgnmb=ntohs(entry->rseg[j-1])&0x00f;
					seq_printf(seq,"-%01X-%03X",brdgnmb,segment);
				}
				seq_putc(seq, '\n');
			}
	   	}
	return 0;
}


static struct seq_operations rif_seq_ops = {
	.start = rif_seq_start,
	.next  = rif_seq_next,
	.stop  = rif_seq_stop,
	.show  = rif_seq_show,
};

static int rif_seq_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &rif_seq_ops);
}

static struct file_operations rif_seq_fops = {
	.owner	 = THIS_MODULE,
	.open    = rif_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
};

#endif

/*
 *	Called during bootup.  We don't actually have to initialise
 *	too much for this.
 */

static int __init rif_init(void)
{
	init_timer(&rif_timer);
	rif_timer.expires  = sysctl_tr_rif_timeout;
	rif_timer.data     = 0L;
	rif_timer.function = rif_check_expire;
	add_timer(&rif_timer);

	proc_net_fops_create("tr_rif", S_IRUGO, &rif_seq_fops);
	return 0;
}

module_init(rif_init);

EXPORT_SYMBOL(tr_source_route);
EXPORT_SYMBOL(tr_type_trans);
