/*
 * This is a module which is used for queueing IPv4 packets and
 * communicating with userspace via netlink.
 *
 * (C) 2000-2002 James Morris <jmorris@intercode.com.au>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * 2000-03-27: Simplified code (thanks to Andi Kleen for clues).
 * 2000-05-20: Fixed notifier problems (following Miguel Freitas' report).
 * 2000-06-19: Fixed so nfmark is copied to metadata (reported by Sebastian 
 *             Zander).
 * 2000-08-01: Added Nick Williams' MAC support.
 * 2002-06-25: Code cleanup.
 *
 */
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/ip.h>
#include <linux/notifier.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4/ip_queue.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netlink.h>
#include <linux/spinlock.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/security.h>
#include <net/sock.h>
#include <net/route.h>

#define IPQ_QMAX_DEFAULT 1024
#define IPQ_PROC_FS_NAME "ip_queue"
#define NET_IPQ_QMAX 2088
#define NET_IPQ_QMAX_NAME "ip_queue_maxlen"

struct ipq_rt_info {
	__u8 tos;
	__u32 daddr;
	__u32 saddr;
};

struct ipq_queue_entry {
	struct list_head list;
	struct nf_info *info;
	struct sk_buff *skb;
	struct ipq_rt_info rt_info;
};

typedef int (*ipq_cmpfn)(struct ipq_queue_entry *, unsigned long);

static unsigned char copy_mode = IPQ_COPY_NONE;
static unsigned int queue_maxlen = IPQ_QMAX_DEFAULT;
static rwlock_t queue_lock = RW_LOCK_UNLOCKED;
static int peer_pid;
static unsigned int copy_range;
static unsigned int queue_total;
static struct sock *ipqnl;
static LIST_HEAD(queue_list);
static DECLARE_MUTEX(ipqnl_sem);

static void
ipq_issue_verdict(struct ipq_queue_entry *entry, int verdict)
{
	nf_reinject(entry->skb, entry->info, verdict);
	kfree(entry);
}

static inline int
__ipq_enqueue_entry(struct ipq_queue_entry *entry)
{
       if (queue_total >= queue_maxlen) {
               if (net_ratelimit()) 
                       printk(KERN_WARNING "ip_queue: full at %d entries, "
                              "dropping packet(s).\n", queue_total);
               return -ENOSPC;
       }
       list_add(&entry->list, &queue_list);
       queue_total++;
       return 0;
}

/*
 * Find and return a queued entry matched by cmpfn, or return the last
 * entry if cmpfn is NULL.
 */
static inline struct ipq_queue_entry *
__ipq_find_entry(ipq_cmpfn cmpfn, unsigned long data)
{
	struct list_head *p;

	list_for_each_prev(p, &queue_list) {
		struct ipq_queue_entry *entry = (struct ipq_queue_entry *)p;
		
		if (!cmpfn || cmpfn(entry, data))
			return entry;
	}
	return NULL;
}

static inline void
__ipq_dequeue_entry(struct ipq_queue_entry *entry)
{
	list_del(&entry->list);
	queue_total--;
}

static inline struct ipq_queue_entry *
__ipq_find_dequeue_entry(ipq_cmpfn cmpfn, unsigned long data)
{
	struct ipq_queue_entry *entry;

	entry = __ipq_find_entry(cmpfn, data);
	if (entry == NULL)
		return NULL;

	__ipq_dequeue_entry(entry);
	return entry;
}


static inline void
__ipq_flush(int verdict)
{
	struct ipq_queue_entry *entry;
	
	while ((entry = __ipq_find_dequeue_entry(NULL, 0)))
		ipq_issue_verdict(entry, verdict);
}

static inline int
__ipq_set_mode(unsigned char mode, unsigned int range)
{
	int status = 0;
	
	switch(mode) {
	case IPQ_COPY_NONE:
	case IPQ_COPY_META:
		copy_mode = mode;
		copy_range = 0;
		break;
		
	case IPQ_COPY_PACKET:
		copy_mode = mode;
		copy_range = range;
		if (copy_range > 0xFFFF)
			copy_range = 0xFFFF;
		break;
		
	default:
		status = -EINVAL;

	}
	return status;
}

static inline void
__ipq_reset(void)
{
	peer_pid = 0;
	__ipq_set_mode(IPQ_COPY_NONE, 0);
	__ipq_flush(NF_DROP);
}

static struct ipq_queue_entry *
ipq_find_dequeue_entry(ipq_cmpfn cmpfn, unsigned long data)
{
	struct ipq_queue_entry *entry;
	
	write_lock_bh(&queue_lock);
	entry = __ipq_find_dequeue_entry(cmpfn, data);
	write_unlock_bh(&queue_lock);
	return entry;
}

static void
ipq_flush(int verdict)
{
	write_lock_bh(&queue_lock);
	__ipq_flush(verdict);
	write_unlock_bh(&queue_lock);
}

static struct sk_buff *
ipq_build_packet_message(struct ipq_queue_entry *entry, int *errp)
{
	unsigned char *old_tail;
	size_t size = 0;
	size_t data_len = 0;
	struct sk_buff *skb;
	struct ipq_packet_msg *pmsg;
	struct nlmsghdr *nlh;

	read_lock_bh(&queue_lock);
	
	switch (copy_mode) {
	case IPQ_COPY_META:
	case IPQ_COPY_NONE:
		size = NLMSG_SPACE(sizeof(*pmsg));
		data_len = 0;
		break;
	
	case IPQ_COPY_PACKET:
		if (copy_range == 0 || copy_range > entry->skb->len)
			data_len = entry->skb->len;
		else
			data_len = copy_range;
		
		size = NLMSG_SPACE(sizeof(*pmsg) + data_len);
		break;
	
	default:
		*errp = -EINVAL;
		read_unlock_bh(&queue_lock);
		return NULL;
	}

	read_unlock_bh(&queue_lock);

	skb = alloc_skb(size, GFP_ATOMIC);
	if (!skb)
		goto nlmsg_failure;
		
	old_tail= skb->tail;
	nlh = NLMSG_PUT(skb, 0, 0, IPQM_PACKET, size - sizeof(*nlh));
	pmsg = NLMSG_DATA(nlh);
	memset(pmsg, 0, sizeof(*pmsg));

	pmsg->packet_id       = (unsigned long )entry;
	pmsg->data_len        = data_len;
	pmsg->timestamp_sec   = entry->skb->stamp.tv_sec;
	pmsg->timestamp_usec  = entry->skb->stamp.tv_usec;
	pmsg->mark            = entry->skb->nfmark;
	pmsg->hook            = entry->info->hook;
	pmsg->hw_protocol     = entry->skb->protocol;
	
	if (entry->info->indev)
		strcpy(pmsg->indev_name, entry->info->indev->name);
	else
		pmsg->indev_name[0] = '\0';
	
	if (entry->info->outdev)
		strcpy(pmsg->outdev_name, entry->info->outdev->name);
	else
		pmsg->outdev_name[0] = '\0';
	
	if (entry->info->indev && entry->skb->dev) {
		pmsg->hw_type = entry->skb->dev->type;
		if (entry->skb->dev->hard_header_parse)
			pmsg->hw_addrlen =
				entry->skb->dev->hard_header_parse(entry->skb,
				                                   pmsg->hw_addr);
	}
	
	if (data_len)
		memcpy(pmsg->payload, entry->skb->data, data_len);
		
	nlh->nlmsg_len = skb->tail - old_tail;
	return skb;

nlmsg_failure:
	if (skb)
		kfree_skb(skb);
	*errp = -EINVAL;
	printk(KERN_ERR "ip_queue: error creating packet message\n");
	return NULL;
}

static int
ipq_enqueue_packet(struct sk_buff *skb, struct nf_info *info, void *data)
{
	int status = -EINVAL;
	struct sk_buff *nskb;
	struct ipq_queue_entry *entry;

	if (copy_mode == IPQ_COPY_NONE)
		return -EAGAIN;

	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (entry == NULL) {
		printk(KERN_ERR "ip_queue: OOM in ipq_enqueue_packet()\n");
		return -ENOMEM;
	}

	entry->info = info;
	entry->skb = skb;

	if (entry->info->hook == NF_IP_LOCAL_OUT) {
		struct iphdr *iph = skb->nh.iph;

		entry->rt_info.tos = iph->tos;
		entry->rt_info.daddr = iph->daddr;
		entry->rt_info.saddr = iph->saddr;
	}

	nskb = ipq_build_packet_message(entry, &status);
	if (nskb == NULL)
		goto err_out_free;
		
	write_lock_bh(&queue_lock);
	
	if (!peer_pid)
		goto err_out_free_nskb; 

 	/* netlink_unicast will either free the nskb or attach it to a socket */ 
	status = netlink_unicast(ipqnl, nskb, peer_pid, MSG_DONTWAIT);
	if (status < 0)
		goto err_out_unlock;
	
	status = __ipq_enqueue_entry(entry);
	if (status < 0)
		goto err_out_unlock;

	write_unlock_bh(&queue_lock);
	return status;

err_out_free_nskb:
	kfree_skb(nskb); 
	
err_out_unlock:
	write_unlock_bh(&queue_lock);

err_out_free:
	kfree(entry);
	return status;
}

static int
ipq_mangle_ipv4(ipq_verdict_msg_t *v, struct ipq_queue_entry *e)
{
	int diff;
	struct iphdr *user_iph = (struct iphdr *)v->payload;

	if (v->data_len < sizeof(*user_iph))
		return 0;
	diff = v->data_len - e->skb->len;
	if (diff < 0)
		skb_trim(e->skb, v->data_len);
	else if (diff > 0) {
		if (v->data_len > 0xFFFF)
			return -EINVAL;
		if (diff > skb_tailroom(e->skb)) {
			struct sk_buff *newskb;
			
			newskb = skb_copy_expand(e->skb,
			                         skb_headroom(e->skb),
			                         diff,
			                         GFP_ATOMIC);
			if (newskb == NULL) {
				printk(KERN_WARNING "ip_queue: OOM "
				      "in mangle, dropping packet\n");
				return -ENOMEM;
			}
			if (e->skb->sk)
				skb_set_owner_w(newskb, e->skb->sk);
			kfree_skb(e->skb);
			e->skb = newskb;
		}
		skb_put(e->skb, diff);
	}
	memcpy(e->skb->data, v->payload, v->data_len);
	e->skb->nfcache |= NFC_ALTERED;

	/*
	 * Extra routing may needed on local out, as the QUEUE target never
	 * returns control to the table.
	 */
	if (e->info->hook == NF_IP_LOCAL_OUT) {
		struct iphdr *iph = e->skb->nh.iph;

		if (!(iph->tos == e->rt_info.tos
		      && iph->daddr == e->rt_info.daddr
		      && iph->saddr == e->rt_info.saddr))
			return ip_route_me_harder(&e->skb);
	}
	return 0;
}

static inline int
id_cmp(struct ipq_queue_entry *e, unsigned long id)
{
	return (id == (unsigned long )e);
}

static int
ipq_set_verdict(struct ipq_verdict_msg *vmsg, unsigned int len)
{
	struct ipq_queue_entry *entry;

	if (vmsg->value > NF_MAX_VERDICT)
		return -EINVAL;

	entry = ipq_find_dequeue_entry(id_cmp, vmsg->id);
	if (entry == NULL)
		return -ENOENT;
	else {
		int verdict = vmsg->value;
		
		if (vmsg->data_len && vmsg->data_len == len)
			if (ipq_mangle_ipv4(vmsg, entry) < 0)
				verdict = NF_DROP;
		
		ipq_issue_verdict(entry, verdict);
		return 0;
	}
}

static int
ipq_set_mode(unsigned char mode, unsigned int range)
{
	int status;

	write_lock_bh(&queue_lock);
	status = __ipq_set_mode(mode, range);
	write_unlock_bh(&queue_lock);
	return status;
}

static int
ipq_receive_peer(struct ipq_peer_msg *pmsg,
                 unsigned char type, unsigned int len)
{
	int status = 0;

	if (len < sizeof(*pmsg))
		return -EINVAL;

	switch (type) {
	case IPQM_MODE:
		status = ipq_set_mode(pmsg->msg.mode.value,
		                      pmsg->msg.mode.range);
		break;
		
	case IPQM_VERDICT:
		if (pmsg->msg.verdict.value > NF_MAX_VERDICT)
			status = -EINVAL;
		else
			status = ipq_set_verdict(&pmsg->msg.verdict,
			                         len - sizeof(*pmsg));
			break;
	default:
		status = -EINVAL;
	}
	return status;
}

static int
dev_cmp(struct ipq_queue_entry *entry, unsigned long ifindex)
{
	if (entry->info->indev)
		if (entry->info->indev->ifindex == ifindex)
			return 1;
			
	if (entry->info->outdev)
		if (entry->info->outdev->ifindex == ifindex)
			return 1;

	return 0;
}

static void
ipq_dev_drop(int ifindex)
{
	struct ipq_queue_entry *entry;
	
	while ((entry = ipq_find_dequeue_entry(dev_cmp, ifindex)) != NULL)
		ipq_issue_verdict(entry, NF_DROP);
}

#define RCV_SKB_FAIL(err) do { netlink_ack(skb, nlh, (err)); return; } while (0)

static inline void
ipq_rcv_skb(struct sk_buff *skb)
{
	int status, type, pid, flags, nlmsglen, skblen;
	struct nlmsghdr *nlh;

	skblen = skb->len;
	if (skblen < sizeof(*nlh))
		return;

	nlh = (struct nlmsghdr *)skb->data;
	nlmsglen = nlh->nlmsg_len;
	if (nlmsglen < sizeof(*nlh) || skblen < nlmsglen)
		return;

	pid = nlh->nlmsg_pid;
	flags = nlh->nlmsg_flags;
	
	if(pid <= 0 || !(flags & NLM_F_REQUEST) || flags & NLM_F_MULTI)
		RCV_SKB_FAIL(-EINVAL);
		
	if (flags & MSG_TRUNC)
		RCV_SKB_FAIL(-ECOMM);
		
	type = nlh->nlmsg_type;
	if (type < NLMSG_NOOP || type >= IPQM_MAX)
		RCV_SKB_FAIL(-EINVAL);
		
	if (type <= IPQM_BASE)
		return;
		
	if (security_netlink_recv(skb))
		RCV_SKB_FAIL(-EPERM);
	
	write_lock_bh(&queue_lock);
	
	if (peer_pid) {
		if (peer_pid != pid) {
			write_unlock_bh(&queue_lock);
			RCV_SKB_FAIL(-EBUSY);
		}
	}
	else
		peer_pid = pid;
		
	write_unlock_bh(&queue_lock);
	
	status = ipq_receive_peer(NLMSG_DATA(nlh), type,
	                          skblen - NLMSG_LENGTH(0));
	if (status < 0)
		RCV_SKB_FAIL(status);
		
	if (flags & NLM_F_ACK)
		netlink_ack(skb, nlh, 0);
        return;
}

static void
ipq_rcv_sk(struct sock *sk, int len)
{
	do {
		struct sk_buff *skb;

		if (down_trylock(&ipqnl_sem))
			return;
			
		while ((skb = skb_dequeue(&sk->sk_receive_queue)) != NULL) {
			ipq_rcv_skb(skb);
			kfree_skb(skb);
		}
		
		up(&ipqnl_sem);

	} while (ipqnl && ipqnl->sk_receive_queue.qlen);
}

static int
ipq_rcv_dev_event(struct notifier_block *this,
                  unsigned long event, void *ptr)
{
	struct net_device *dev = ptr;

	/* Drop any packets associated with the downed device */
	if (event == NETDEV_DOWN)
		ipq_dev_drop(dev->ifindex);
	return NOTIFY_DONE;
}

static struct notifier_block ipq_dev_notifier = {
	.notifier_call	= ipq_rcv_dev_event,
};

static int
ipq_rcv_nl_event(struct notifier_block *this,
                 unsigned long event, void *ptr)
{
	struct netlink_notify *n = ptr;

	if (event == NETLINK_URELEASE &&
	    n->protocol == NETLINK_FIREWALL && n->pid) {
		write_lock_bh(&queue_lock);
		if (n->pid == peer_pid)
			__ipq_reset();
		write_unlock_bh(&queue_lock);
	}
	return NOTIFY_DONE;
}

static struct notifier_block ipq_nl_notifier = {
	.notifier_call	= ipq_rcv_nl_event,
};

static struct ctl_table_header *ipq_sysctl_header;

static ctl_table ipq_table[] = {
	{
		.ctl_name	= NET_IPQ_QMAX,
		.procname	= NET_IPQ_QMAX_NAME,
		.data		= &queue_maxlen,
		.maxlen		= sizeof(queue_maxlen),
		.mode		= 0644,
		.proc_handler	= proc_dointvec
	},
 	{ .ctl_name = 0 }
};

static ctl_table ipq_dir_table[] = {
	{
		.ctl_name	= NET_IPV4,
		.procname	= "ipv4",
		.mode		= 0555,
		.child		= ipq_table
	},
	{ .ctl_name = 0 }
};

static ctl_table ipq_root_table[] = {
	{
		.ctl_name	= CTL_NET,
		.procname	= "net",
		.mode		= 0555,
		.child		= ipq_dir_table
	},
	{ .ctl_name = 0 }
};

static int
ipq_get_info(char *buffer, char **start, off_t offset, int length)
{
	int len;

	read_lock_bh(&queue_lock);
	
	len = sprintf(buffer,
	              "Peer PID          : %d\n"
	              "Copy mode         : %hu\n"
	              "Copy range        : %u\n"
	              "Queue length      : %u\n"
	              "Queue max. length : %u\n",
	              peer_pid,
	              copy_mode,
	              copy_range,
	              queue_total,
	              queue_maxlen);

	read_unlock_bh(&queue_lock);
	
	*start = buffer + offset;
	len -= offset;
	if (len > length)
		len = length;
	else if (len < 0)
		len = 0;
	return len;
}

static int
init_or_cleanup(int init)
{
	int status = -ENOMEM;
	struct proc_dir_entry *proc;
	
	if (!init)
		goto cleanup;

	netlink_register_notifier(&ipq_nl_notifier);
	ipqnl = netlink_kernel_create(NETLINK_FIREWALL, ipq_rcv_sk);
	if (ipqnl == NULL) {
		printk(KERN_ERR "ip_queue: failed to create netlink socket\n");
		goto cleanup_netlink_notifier;
	}

	proc = proc_net_create(IPQ_PROC_FS_NAME, 0, ipq_get_info);
	if (proc)
		proc->owner = THIS_MODULE;
	else {
		printk(KERN_ERR "ip_queue: failed to create proc entry\n");
		goto cleanup_ipqnl;
	}
	
	register_netdevice_notifier(&ipq_dev_notifier);
	ipq_sysctl_header = register_sysctl_table(ipq_root_table, 0);
	
	status = nf_register_queue_handler(PF_INET, ipq_enqueue_packet, NULL);
	if (status < 0) {
		printk(KERN_ERR "ip_queue: failed to register queue handler\n");
		goto cleanup_sysctl;
	}
	return status;

cleanup:
	nf_unregister_queue_handler(PF_INET);
	synchronize_net();
	ipq_flush(NF_DROP);
	
cleanup_sysctl:
	unregister_sysctl_table(ipq_sysctl_header);
	unregister_netdevice_notifier(&ipq_dev_notifier);
	proc_net_remove(IPQ_PROC_FS_NAME);
	
cleanup_ipqnl:
	sock_release(ipqnl->sk_socket);
	down(&ipqnl_sem);
	up(&ipqnl_sem);
	
cleanup_netlink_notifier:
	netlink_unregister_notifier(&ipq_nl_notifier);
	return status;
}

static int __init init(void)
{
	
	return init_or_cleanup(1);
}

static void __exit fini(void)
{
	init_or_cleanup(0);
}

MODULE_DESCRIPTION("IPv4 packet queue handler");
MODULE_AUTHOR("James Morris <jmorris@intercode.com.au>");
MODULE_LICENSE("GPL");

module_init(init);
module_exit(fini);
