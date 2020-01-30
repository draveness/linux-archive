/* 
   HIDP implementation for Linux Bluetooth stack (BlueZ).
   Copyright (C) 2003-2004 Marcel Holtmann <marcel@holtmann.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 as
   published by the Free Software Foundation;

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
   IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
   CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

   ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
   COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
   SOFTWARE IS DISCLAIMED.
*/

#include <linux/config.h>
#include <linux/module.h>

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/fcntl.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/ioctl.h>
#include <linux/file.h>
#include <linux/init.h>
#include <net/sock.h>

#include <linux/input.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/l2cap.h>

#include "hidp.h"

#ifndef CONFIG_BT_HIDP_DEBUG
#undef  BT_DBG
#define BT_DBG(D...)
#endif

#define VERSION "1.0"

static DECLARE_RWSEM(hidp_session_sem);
static LIST_HEAD(hidp_session_list);

static unsigned char hidp_keycode[256] = {
	  0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
	 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
	  4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
	 27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
	 65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
	105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
	 72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
	191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
	115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
	122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	 29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
	150,158,159,128,136,177,178,176,142,152,173,140
};

static struct hidp_session *__hidp_get_session(bdaddr_t *bdaddr)
{
	struct hidp_session *session;
	struct list_head *p;

	BT_DBG("");

	list_for_each(p, &hidp_session_list) {
		session = list_entry(p, struct hidp_session, list);
		if (!bacmp(bdaddr, &session->bdaddr))
			return session;
	}
	return NULL;
}

static void __hidp_link_session(struct hidp_session *session)
{
	__module_get(THIS_MODULE);
	list_add(&session->list, &hidp_session_list);
}

static void __hidp_unlink_session(struct hidp_session *session)
{
	list_del(&session->list);
	module_put(THIS_MODULE);
}

static void __hidp_copy_session(struct hidp_session *session, struct hidp_conninfo *ci)
{
	bacpy(&ci->bdaddr, &session->bdaddr);

	ci->flags = session->flags;
	ci->state = session->state;

	ci->vendor  = 0x0000;
	ci->product = 0x0000;
	ci->version = 0x0000;
	memset(ci->name, 0, 128);

	if (session->input) {
		ci->vendor  = session->input->id.vendor;
		ci->product = session->input->id.product;
		ci->version = session->input->id.version;
		if (session->input->name)
			strncpy(ci->name, session->input->name, 128);
		else
			strncpy(ci->name, "HID Boot Device", 128);
	}
}

static int hidp_input_event(struct input_dev *dev, unsigned int type, unsigned int code, int value)
{
	struct hidp_session *session = dev->private;
	struct sk_buff *skb;
	unsigned char newleds;

	BT_DBG("session %p hid %p data %p size %d", session, device, data, size);

	if (type != EV_LED)
		return -1;

	newleds = (!!test_bit(LED_KANA,    dev->led) << 3) |
		  (!!test_bit(LED_COMPOSE, dev->led) << 3) |
		  (!!test_bit(LED_SCROLLL, dev->led) << 2) |
		  (!!test_bit(LED_CAPSL,   dev->led) << 1) |
		  (!!test_bit(LED_NUML,    dev->led));

	if (session->leds == newleds)
		return 0;

	session->leds = newleds;

	if (!(skb = alloc_skb(3, GFP_ATOMIC))) {
		BT_ERR("Can't allocate memory for new frame");
		return -ENOMEM;
	}

	*skb_put(skb, 1) = 0xa2;
	*skb_put(skb, 1) = 0x01;
	*skb_put(skb, 1) = newleds;

	skb_queue_tail(&session->intr_transmit, skb);

	hidp_schedule(session);

	return 0;
}

static void hidp_input_report(struct hidp_session *session, struct sk_buff *skb)
{
	struct input_dev *dev = session->input;
	unsigned char *keys = session->keys;
	unsigned char *udata = skb->data + 1;
	signed char *sdata = skb->data + 1;
	int i, size = skb->len - 1;

	switch (skb->data[0]) {
	case 0x01:	/* Keyboard report */
		for (i = 0; i < 8; i++)
			input_report_key(dev, hidp_keycode[i + 224], (udata[0] >> i) & 1);

		for (i = 2; i < 8; i++) {
			if (keys[i] > 3 && memscan(udata + 2, keys[i], 6) == udata + 8) {
				if (hidp_keycode[keys[i]])
					input_report_key(dev, hidp_keycode[keys[i]], 0);
				else
					BT_ERR("Unknown key (scancode %#x) released.", keys[i]);
			}

			if (udata[i] > 3 && memscan(keys + 2, udata[i], 6) == keys + 8) {
				if (hidp_keycode[udata[i]])
					input_report_key(dev, hidp_keycode[udata[i]], 1);
				else
					BT_ERR("Unknown key (scancode %#x) pressed.", udata[i]);
			}
		}

		memcpy(keys, udata, 8);
		break;

	case 0x02:	/* Mouse report */
		input_report_key(dev, BTN_LEFT,   sdata[0] & 0x01);
		input_report_key(dev, BTN_RIGHT,  sdata[0] & 0x02);
		input_report_key(dev, BTN_MIDDLE, sdata[0] & 0x04);
		input_report_key(dev, BTN_SIDE,   sdata[0] & 0x08);
		input_report_key(dev, BTN_EXTRA,  sdata[0] & 0x10);

		input_report_rel(dev, REL_X, sdata[1]);
		input_report_rel(dev, REL_Y, sdata[2]);

		if (size > 3)
			input_report_rel(dev, REL_WHEEL, sdata[3]);
		break;
	}

	input_sync(dev);
}

static void hidp_idle_timeout(unsigned long arg)
{
	struct hidp_session *session = (struct hidp_session *) arg;

	atomic_inc(&session->terminate);
	hidp_schedule(session);
}

static inline void hidp_set_timer(struct hidp_session *session)
{
	if (session->idle_to > 0)
		mod_timer(&session->timer, jiffies + HZ * session->idle_to);
}

static inline void hidp_del_timer(struct hidp_session *session)
{
	if (session->idle_to > 0)
		del_timer(&session->timer);
}

static inline void hidp_send_message(struct hidp_session *session, unsigned char hdr)
{
	struct sk_buff *skb;

	BT_DBG("session %p", session);

	if (!(skb = alloc_skb(1, GFP_ATOMIC))) {
		BT_ERR("Can't allocate memory for message");
		return;
	}

	*skb_put(skb, 1) = hdr;

	skb_queue_tail(&session->ctrl_transmit, skb);

	hidp_schedule(session);
}

static inline int hidp_recv_frame(struct hidp_session *session, struct sk_buff *skb)
{
	__u8 hdr;

	BT_DBG("session %p skb %p len %d", session, skb, skb->len);

	hdr = skb->data[0];
	skb_pull(skb, 1);

	if (hdr == 0xa1) {
		hidp_set_timer(session);

		if (session->input)
			hidp_input_report(session, skb);
	} else {
		BT_DBG("Unsupported protocol header 0x%02x", hdr);
	}

	kfree_skb(skb);
	return 0;
}

static int hidp_send_frame(struct socket *sock, unsigned char *data, int len)
{
	struct kvec iv = { data, len };
	struct msghdr msg;

	BT_DBG("sock %p data %p len %d", sock, data, len);

	if (!len)
		return 0;

	memset(&msg, 0, sizeof(msg));

	return kernel_sendmsg(sock, &msg, &iv, 1, len);
}

static int hidp_process_transmit(struct hidp_session *session)
{
	struct sk_buff *skb;

	BT_DBG("session %p", session);

	while ((skb = skb_dequeue(&session->ctrl_transmit))) {
		if (hidp_send_frame(session->ctrl_sock, skb->data, skb->len) < 0) {
			skb_queue_head(&session->ctrl_transmit, skb);
			break;
		}

		hidp_set_timer(session);
		kfree_skb(skb);
	}

	while ((skb = skb_dequeue(&session->intr_transmit))) {
		if (hidp_send_frame(session->intr_sock, skb->data, skb->len) < 0) {
			skb_queue_head(&session->intr_transmit, skb);
			break;
		}

		hidp_set_timer(session);
		kfree_skb(skb);
	}

	return skb_queue_len(&session->ctrl_transmit) +
				skb_queue_len(&session->intr_transmit);
}

static int hidp_session(void *arg)
{
	struct hidp_session *session = arg;
	struct sock *ctrl_sk = session->ctrl_sock->sk;
	struct sock *intr_sk = session->intr_sock->sk;
	struct sk_buff *skb;
	int vendor = 0x0000, product = 0x0000;
	wait_queue_t ctrl_wait, intr_wait;
	unsigned long timeo = HZ;

	BT_DBG("session %p", session);

	if (session->input) {
		vendor  = session->input->id.vendor;
		product = session->input->id.product;
	}

	daemonize("khidpd_%04x%04x", vendor, product);
	set_user_nice(current, -15);
	current->flags |= PF_NOFREEZE;

	init_waitqueue_entry(&ctrl_wait, current);
	init_waitqueue_entry(&intr_wait, current);
	add_wait_queue(ctrl_sk->sk_sleep, &ctrl_wait);
	add_wait_queue(intr_sk->sk_sleep, &intr_wait);
	while (!atomic_read(&session->terminate)) {
		set_current_state(TASK_INTERRUPTIBLE);

		if (ctrl_sk->sk_state != BT_CONNECTED || intr_sk->sk_state != BT_CONNECTED)
			break;

		while ((skb = skb_dequeue(&ctrl_sk->sk_receive_queue))) {
			skb_orphan(skb);
			hidp_recv_frame(session, skb);
		}

		while ((skb = skb_dequeue(&intr_sk->sk_receive_queue))) {
			skb_orphan(skb);
			hidp_recv_frame(session, skb);
		}

		hidp_process_transmit(session);

		schedule();
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(intr_sk->sk_sleep, &intr_wait);
	remove_wait_queue(ctrl_sk->sk_sleep, &ctrl_wait);

	down_write(&hidp_session_sem);

	hidp_del_timer(session);

	if (intr_sk->sk_state != BT_CONNECTED) {
		init_waitqueue_entry(&ctrl_wait, current);
		add_wait_queue(ctrl_sk->sk_sleep, &ctrl_wait);
		while (timeo && ctrl_sk->sk_state != BT_CLOSED) {
			set_current_state(TASK_INTERRUPTIBLE);
			timeo = schedule_timeout(timeo);
		}
		set_current_state(TASK_RUNNING);
		remove_wait_queue(ctrl_sk->sk_sleep, &ctrl_wait);
		timeo = HZ;
	}

	fput(session->ctrl_sock->file);

	init_waitqueue_entry(&intr_wait, current);
	add_wait_queue(intr_sk->sk_sleep, &intr_wait);
	while (timeo && intr_sk->sk_state != BT_CLOSED) {
		set_current_state(TASK_INTERRUPTIBLE);
		timeo = schedule_timeout(timeo);
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(intr_sk->sk_sleep, &intr_wait);

	fput(session->intr_sock->file);

	__hidp_unlink_session(session);

	if (session->input) {
		input_unregister_device(session->input);
		kfree(session->input);
	}

	up_write(&hidp_session_sem);

	kfree(session);
	return 0;
}

static inline void hidp_setup_input(struct hidp_session *session, struct hidp_connadd_req *req)
{
	struct input_dev *input = session->input;
	int i;

	input->private = session;

	input->id.bustype = BUS_BLUETOOTH;
	input->id.vendor  = req->vendor;
	input->id.product = req->product;
	input->id.version = req->version;

	if (req->subclass & 0x40) {
		set_bit(EV_KEY, input->evbit);
		set_bit(EV_LED, input->evbit);
		set_bit(EV_REP, input->evbit);

		set_bit(LED_NUML,    input->ledbit);
		set_bit(LED_CAPSL,   input->ledbit);
		set_bit(LED_SCROLLL, input->ledbit);
		set_bit(LED_COMPOSE, input->ledbit);
		set_bit(LED_KANA,    input->ledbit);

		for (i = 0; i < sizeof(hidp_keycode); i++)
			set_bit(hidp_keycode[i], input->keybit);
		clear_bit(0, input->keybit);
	}

	if (req->subclass & 0x80) {
		input->evbit[0] = BIT(EV_KEY) | BIT(EV_REL);
		input->keybit[LONG(BTN_MOUSE)] = BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_MIDDLE);
		input->relbit[0] = BIT(REL_X) | BIT(REL_Y);
		input->keybit[LONG(BTN_MOUSE)] |= BIT(BTN_SIDE) | BIT(BTN_EXTRA);
		input->relbit[0] |= BIT(REL_WHEEL);
	}

	input->event = hidp_input_event;

	input_register_device(input);
}

int hidp_add_connection(struct hidp_connadd_req *req, struct socket *ctrl_sock, struct socket *intr_sock)
{
	struct hidp_session *session, *s;
	int err;

	BT_DBG("");

	if (bacmp(&bt_sk(ctrl_sock->sk)->src, &bt_sk(intr_sock->sk)->src) ||
			bacmp(&bt_sk(ctrl_sock->sk)->dst, &bt_sk(intr_sock->sk)->dst))
		return -ENOTUNIQ;

	session = kmalloc(sizeof(struct hidp_session), GFP_KERNEL);
	if (!session) 
		return -ENOMEM;
	memset(session, 0, sizeof(struct hidp_session));

	session->input = kmalloc(sizeof(struct input_dev), GFP_KERNEL);
	if (!session->input) {
		kfree(session);
		return -ENOMEM;
	}
	memset(session->input, 0, sizeof(struct input_dev));

	down_write(&hidp_session_sem);

	s = __hidp_get_session(&bt_sk(ctrl_sock->sk)->dst);
	if (s && s->state == BT_CONNECTED) {
		err = -EEXIST;
		goto failed;
	}

	bacpy(&session->bdaddr, &bt_sk(ctrl_sock->sk)->dst);

	session->ctrl_mtu = min_t(uint, l2cap_pi(ctrl_sock->sk)->omtu, l2cap_pi(ctrl_sock->sk)->imtu);
	session->intr_mtu = min_t(uint, l2cap_pi(intr_sock->sk)->omtu, l2cap_pi(intr_sock->sk)->imtu);

	BT_DBG("ctrl mtu %d intr mtu %d", session->ctrl_mtu, session->intr_mtu);

	session->ctrl_sock = ctrl_sock;
	session->intr_sock = intr_sock;
	session->state     = BT_CONNECTED;

	init_timer(&session->timer);

	session->timer.function = hidp_idle_timeout;
	session->timer.data     = (unsigned long) session;

	skb_queue_head_init(&session->ctrl_transmit);
	skb_queue_head_init(&session->intr_transmit);

	session->flags   = req->flags & (1 << HIDP_BLUETOOTH_VENDOR_ID);
	session->idle_to = req->idle_to;

	if (session->input)
		hidp_setup_input(session, req);

	__hidp_link_session(session);

	hidp_set_timer(session);

	err = kernel_thread(hidp_session, session, CLONE_KERNEL);
	if (err < 0)
		goto unlink;

	if (session->input) {
		hidp_send_message(session, 0x70);
		session->flags |= (1 << HIDP_BOOT_PROTOCOL_MODE);

		session->leds = 0xff;
		hidp_input_event(session->input, EV_LED, 0, 0);
	}

	up_write(&hidp_session_sem);
	return 0;

unlink:
	hidp_del_timer(session);

	__hidp_unlink_session(session);

	if (session->input)
		input_unregister_device(session->input);

failed:
	up_write(&hidp_session_sem);

	if (session->input)
		kfree(session->input);

	kfree(session);
	return err;
}

int hidp_del_connection(struct hidp_conndel_req *req)
{
	struct hidp_session *session;
	int err = 0;

	BT_DBG("");

	down_read(&hidp_session_sem);

	session = __hidp_get_session(&req->bdaddr);
	if (session) {
		if (req->flags & (1 << HIDP_VIRTUAL_CABLE_UNPLUG)) {
			hidp_send_message(session, 0x15);
		} else {
			/* Flush the transmit queues */
			skb_queue_purge(&session->ctrl_transmit);
			skb_queue_purge(&session->intr_transmit);

			/* Kill session thread */
			atomic_inc(&session->terminate);
			hidp_schedule(session);
		}
	} else
		err = -ENOENT;

	up_read(&hidp_session_sem);
	return err;
}

int hidp_get_connlist(struct hidp_connlist_req *req)
{
	struct list_head *p;
	int err = 0, n = 0;

	BT_DBG("");

	down_read(&hidp_session_sem);

	list_for_each(p, &hidp_session_list) {
		struct hidp_session *session;
		struct hidp_conninfo ci;

		session = list_entry(p, struct hidp_session, list);

		__hidp_copy_session(session, &ci);

		if (copy_to_user(req->ci, &ci, sizeof(ci))) {
			err = -EFAULT;
			break;
		}

		if (++n >= req->cnum)
			break;

		req->ci++;
	}
	req->cnum = n;

	up_read(&hidp_session_sem);
	return err;
}

int hidp_get_conninfo(struct hidp_conninfo *ci)
{
	struct hidp_session *session;
	int err = 0;

	down_read(&hidp_session_sem);

	session = __hidp_get_session(&ci->bdaddr);
	if (session)
		__hidp_copy_session(session, ci);
	else
		err = -ENOENT;

	up_read(&hidp_session_sem);
	return err;
}

static int __init hidp_init(void)
{
	l2cap_load();

	BT_INFO("HIDP (Human Interface Emulation) ver %s", VERSION);

	return hidp_init_sockets();
}

static void __exit hidp_exit(void)
{
	hidp_cleanup_sockets();
}

module_init(hidp_init);
module_exit(hidp_exit);

MODULE_AUTHOR("Marcel Holtmann <marcel@holtmann.org>");
MODULE_DESCRIPTION("Bluetooth HIDP ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
MODULE_ALIAS("bt-proto-6");
