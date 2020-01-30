/* Kernel module to match various things tied to sockets associated with
   locally generated outgoing packets.

   Copyright (C) 2000 Marc Boucher
 */
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/file.h>
#include <net/sock.h>

#include <linux/netfilter_ipv4/ipt_owner.h>
#include <linux/netfilter_ipv4/ip_tables.h>

static int
match_pid(const struct sk_buff *skb, pid_t pid)
{
	struct task_struct *p;
	struct files_struct *files;
	int i;

	read_lock(&tasklist_lock);
	p = find_task_by_pid(pid);
	if (!p)
		goto out;
	task_lock(p);
	files = p->files;
	if(files) {
		read_lock(&files->file_lock);
		for (i=0; i < files->max_fds; i++) {
			if (fcheck_files(files, i) == skb->sk->socket->file) {
				read_unlock(&files->file_lock);
				task_unlock(p);
				read_unlock(&tasklist_lock);
				return 1;
			}
		}
		read_unlock(&files->file_lock);
	}
	task_unlock(p);
out:
	read_unlock(&tasklist_lock);
	return 0;
}

static int
match_sid(const struct sk_buff *skb, pid_t sid)
{
	struct task_struct *p;
	struct file *file = skb->sk->socket->file;
	int i, found=0;

	read_lock(&tasklist_lock);
	for_each_task(p) {
		struct files_struct *files;
		if (p->session != sid)
			continue;

		task_lock(p);
		files = p->files;
		if (files) {
			read_lock(&files->file_lock);
			for (i=0; i < files->max_fds; i++) {
				if (fcheck_files(files, i) == file) {
					found = 1;
					break;
				}
			}
			read_unlock(&files->file_lock);
		}
		task_unlock(p);
		if(found)
			break;
	}
	read_unlock(&tasklist_lock);

	return found;
}

static int
match(const struct sk_buff *skb,
      const struct net_device *in,
      const struct net_device *out,
      const void *matchinfo,
      int offset,
      const void *hdr,
      u_int16_t datalen,
      int *hotdrop)
{
	const struct ipt_owner_info *info = matchinfo;

	if (!skb->sk || !skb->sk->socket || !skb->sk->socket->file)
		return 0;

	if(info->match & IPT_OWNER_UID) {
		if((skb->sk->socket->file->f_uid != info->uid) ^
		    !!(info->invert & IPT_OWNER_UID))
			return 0;
	}

	if(info->match & IPT_OWNER_GID) {
		if((skb->sk->socket->file->f_gid != info->gid) ^
		    !!(info->invert & IPT_OWNER_GID))
			return 0;
	}

	if(info->match & IPT_OWNER_PID) {
		if (!match_pid(skb, info->pid) ^
		    !!(info->invert & IPT_OWNER_PID))
			return 0;
	}

	if(info->match & IPT_OWNER_SID) {
		if (!match_sid(skb, info->sid) ^
		    !!(info->invert & IPT_OWNER_SID))
			return 0;
	}

	return 1;
}

static int
checkentry(const char *tablename,
           const struct ipt_ip *ip,
           void *matchinfo,
           unsigned int matchsize,
           unsigned int hook_mask)
{
        if (hook_mask
            & ~((1 << NF_IP_LOCAL_OUT) | (1 << NF_IP_POST_ROUTING))) {
                printk("ipt_owner: only valid for LOCAL_OUT or POST_ROUTING.\n");
                return 0;
        }

	if (matchsize != IPT_ALIGN(sizeof(struct ipt_owner_info)))
		return 0;

	return 1;
}

static struct ipt_match owner_match
= { { NULL, NULL }, "owner", &match, &checkentry, NULL, THIS_MODULE };

static int __init init(void)
{
	return ipt_register_match(&owner_match);
}

static void __exit fini(void)
{
	ipt_unregister_match(&owner_match);
}

module_init(init);
module_exit(fini);
