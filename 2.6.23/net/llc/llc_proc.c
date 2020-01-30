/*
 * proc_llc.c - proc interface for LLC
 *
 * Copyright (c) 2001 by Jay Schulist <jschlst@samba.org>
 *		 2002-2003 by Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *
 * This program can be redistributed or modified under the terms of the
 * GNU General Public License as published by the Free Software Foundation.
 * This program is distributed without any warranty or implied warranty
 * of merchantability or fitness for a particular purpose.
 *
 * See the GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/errno.h>
#include <linux/seq_file.h>
#include <net/sock.h>
#include <net/llc.h>
#include <net/llc_c_ac.h>
#include <net/llc_c_ev.h>
#include <net/llc_c_st.h>
#include <net/llc_conn.h>

static void llc_ui_format_mac(struct seq_file *seq, unsigned char *mac)
{
	seq_printf(seq, "%02X:%02X:%02X:%02X:%02X:%02X",
		   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static struct sock *llc_get_sk_idx(loff_t pos)
{
	struct list_head *sap_entry;
	struct llc_sap *sap;
	struct hlist_node *node;
	struct sock *sk = NULL;

	list_for_each(sap_entry, &llc_sap_list) {
		sap = list_entry(sap_entry, struct llc_sap, node);

		read_lock_bh(&sap->sk_list.lock);
		sk_for_each(sk, node, &sap->sk_list.list) {
			if (!pos)
				goto found;
			--pos;
		}
		read_unlock_bh(&sap->sk_list.lock);
	}
	sk = NULL;
found:
	return sk;
}

static void *llc_seq_start(struct seq_file *seq, loff_t *pos)
{
	loff_t l = *pos;

	read_lock_bh(&llc_sap_list_lock);
	return l ? llc_get_sk_idx(--l) : SEQ_START_TOKEN;
}

static void *llc_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct sock* sk, *next;
	struct llc_sock *llc;
	struct llc_sap *sap;

	++*pos;
	if (v == SEQ_START_TOKEN) {
		sk = llc_get_sk_idx(0);
		goto out;
	}
	sk = v;
	next = sk_next(sk);
	if (next) {
		sk = next;
		goto out;
	}
	llc = llc_sk(sk);
	sap = llc->sap;
	read_unlock_bh(&sap->sk_list.lock);
	sk = NULL;
	for (;;) {
		if (sap->node.next == &llc_sap_list)
			break;
		sap = list_entry(sap->node.next, struct llc_sap, node);
		read_lock_bh(&sap->sk_list.lock);
		if (!hlist_empty(&sap->sk_list.list)) {
			sk = sk_head(&sap->sk_list.list);
			break;
		}
		read_unlock_bh(&sap->sk_list.lock);
	}
out:
	return sk;
}

static void llc_seq_stop(struct seq_file *seq, void *v)
{
	if (v && v != SEQ_START_TOKEN) {
		struct sock *sk = v;
		struct llc_sock *llc = llc_sk(sk);
		struct llc_sap *sap = llc->sap;

		read_unlock_bh(&sap->sk_list.lock);
	}
	read_unlock_bh(&llc_sap_list_lock);
}

static int llc_seq_socket_show(struct seq_file *seq, void *v)
{
	struct sock* sk;
	struct llc_sock *llc;

	if (v == SEQ_START_TOKEN) {
		seq_puts(seq, "SKt Mc local_mac_sap        remote_mac_sap   "
			      "    tx_queue rx_queue st uid link\n");
		goto out;
	}
	sk = v;
	llc = llc_sk(sk);

	/* FIXME: check if the address is multicast */
	seq_printf(seq, "%2X  %2X ", sk->sk_type, 0);

	if (llc->dev)
		llc_ui_format_mac(seq, llc->dev->dev_addr);
	else
		seq_printf(seq, "00:00:00:00:00:00");
	seq_printf(seq, "@%02X ", llc->sap->laddr.lsap);
	llc_ui_format_mac(seq, llc->daddr.mac);
	seq_printf(seq, "@%02X %8d %8d %2d %3d %4d\n", llc->daddr.lsap,
		   atomic_read(&sk->sk_wmem_alloc),
		   atomic_read(&sk->sk_rmem_alloc) - llc->copied_seq,
		   sk->sk_state,
		   sk->sk_socket ? SOCK_INODE(sk->sk_socket)->i_uid : -1,
		   llc->link);
out:
	return 0;
}

static char *llc_conn_state_names[] = {
	[LLC_CONN_STATE_ADM] =        "adm",
	[LLC_CONN_STATE_SETUP] =      "setup",
	[LLC_CONN_STATE_NORMAL] =     "normal",
	[LLC_CONN_STATE_BUSY] =       "busy",
	[LLC_CONN_STATE_REJ] =        "rej",
	[LLC_CONN_STATE_AWAIT] =      "await",
	[LLC_CONN_STATE_AWAIT_BUSY] = "await_busy",
	[LLC_CONN_STATE_AWAIT_REJ] =  "await_rej",
	[LLC_CONN_STATE_D_CONN]	=     "d_conn",
	[LLC_CONN_STATE_RESET] =      "reset",
	[LLC_CONN_STATE_ERROR] =      "error",
	[LLC_CONN_STATE_TEMP] =       "temp",
};

static int llc_seq_core_show(struct seq_file *seq, void *v)
{
	struct sock* sk;
	struct llc_sock *llc;

	if (v == SEQ_START_TOKEN) {
		seq_puts(seq, "Connection list:\n"
			      "dsap state      retr txw rxw pf ff sf df rs cs "
			      "tack tpfc trs tbs blog busr\n");
		goto out;
	}
	sk = v;
	llc = llc_sk(sk);

	seq_printf(seq, " %02X  %-10s %3d  %3d %3d %2d %2d %2d %2d %2d %2d "
			"%4d %4d %3d %3d %4d %4d\n",
		   llc->daddr.lsap, llc_conn_state_names[llc->state],
		   llc->retry_count, llc->k, llc->rw, llc->p_flag, llc->f_flag,
		   llc->s_flag, llc->data_flag, llc->remote_busy_flag,
		   llc->cause_flag, timer_pending(&llc->ack_timer.timer),
		   timer_pending(&llc->pf_cycle_timer.timer),
		   timer_pending(&llc->rej_sent_timer.timer),
		   timer_pending(&llc->busy_state_timer.timer),
		   !!sk->sk_backlog.tail, !!sock_owned_by_user(sk));
out:
	return 0;
}

static const struct seq_operations llc_seq_socket_ops = {
	.start  = llc_seq_start,
	.next   = llc_seq_next,
	.stop   = llc_seq_stop,
	.show   = llc_seq_socket_show,
};

static const struct seq_operations llc_seq_core_ops = {
	.start  = llc_seq_start,
	.next   = llc_seq_next,
	.stop   = llc_seq_stop,
	.show   = llc_seq_core_show,
};

static int llc_seq_socket_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &llc_seq_socket_ops);
}

static int llc_seq_core_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &llc_seq_core_ops);
}

static const struct file_operations llc_seq_socket_fops = {
	.owner		= THIS_MODULE,
	.open		= llc_seq_socket_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static const struct file_operations llc_seq_core_fops = {
	.owner		= THIS_MODULE,
	.open		= llc_seq_core_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static struct proc_dir_entry *llc_proc_dir;

int __init llc_proc_init(void)
{
	int rc = -ENOMEM;
	struct proc_dir_entry *p;

	llc_proc_dir = proc_mkdir("llc", proc_net);
	if (!llc_proc_dir)
		goto out;
	llc_proc_dir->owner = THIS_MODULE;

	p = create_proc_entry("socket", S_IRUGO, llc_proc_dir);
	if (!p)
		goto out_socket;

	p->proc_fops = &llc_seq_socket_fops;

	p = create_proc_entry("core", S_IRUGO, llc_proc_dir);
	if (!p)
		goto out_core;

	p->proc_fops = &llc_seq_core_fops;

	rc = 0;
out:
	return rc;
out_core:
	remove_proc_entry("socket", llc_proc_dir);
out_socket:
	remove_proc_entry("llc", proc_net);
	goto out;
}

void llc_proc_exit(void)
{
	remove_proc_entry("socket", llc_proc_dir);
	remove_proc_entry("core", llc_proc_dir);
	remove_proc_entry("llc", proc_net);
}
