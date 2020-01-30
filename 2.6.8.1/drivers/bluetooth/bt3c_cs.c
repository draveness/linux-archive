/*
 *
 *  Driver for the 3Com Bluetooth PCMCIA card
 *
 *  Copyright (C) 2001-2002  Marcel Holtmann <marcel@holtmann.org>
 *                           Jose Orlando Pereira <jop@di.uminho.pt>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation;
 *
 *  Software distributed under the License is distributed on an "AS
 *  IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 *  implied. See the License for the specific language governing
 *  rights and limitations under the License.
 *
 *  The initial developer of the original code is David A. Hinds
 *  <dahinds@users.sourceforge.net>.  Portions created by David A. Hinds
 *  are Copyright (C) 1999 David A. Hinds.  All Rights Reserved.
 *
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>

#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/serial.h>
#include <linux/serial_reg.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/io.h>

#include <linux/device.h>
#include <linux/firmware.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ciscode.h>
#include <pcmcia/ds.h>
#include <pcmcia/cisreg.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>



/* ======================== Module parameters ======================== */


/* Bit map of interrupts to choose from */
static u_int irq_mask = 0xffff;
static int irq_list[4] = { -1 };

MODULE_PARM(irq_mask, "i");
MODULE_PARM(irq_list, "1-4i");

MODULE_AUTHOR("Marcel Holtmann <marcel@holtmann.org>, Jose Orlando Pereira <jop@di.uminho.pt>");
MODULE_DESCRIPTION("Bluetooth driver for the 3Com Bluetooth PCMCIA card");
MODULE_LICENSE("GPL");



/* ======================== Local structures ======================== */


typedef struct bt3c_info_t {
	dev_link_t link;
	dev_node_t node;

	struct hci_dev *hdev;

	spinlock_t lock;		/* For serializing operations */

	struct sk_buff_head txq;
	unsigned long tx_state;

	unsigned long rx_state;
	unsigned long rx_count;
	struct sk_buff *rx_skb;
} bt3c_info_t;


void bt3c_config(dev_link_t *link);
void bt3c_release(dev_link_t *link);
int bt3c_event(event_t event, int priority, event_callback_args_t *args);

static dev_info_t dev_info = "bt3c_cs";

dev_link_t *bt3c_attach(void);
void bt3c_detach(dev_link_t *);

static dev_link_t *dev_list = NULL;


/* Transmit states  */
#define XMIT_SENDING  1
#define XMIT_WAKEUP   2
#define XMIT_WAITING  8

/* Receiver states */
#define RECV_WAIT_PACKET_TYPE   0
#define RECV_WAIT_EVENT_HEADER  1
#define RECV_WAIT_ACL_HEADER    2
#define RECV_WAIT_SCO_HEADER    3
#define RECV_WAIT_DATA          4



/* ======================== Special I/O functions ======================== */


#define DATA_L   0
#define DATA_H   1
#define ADDR_L   2
#define ADDR_H   3
#define CONTROL  4


inline void bt3c_address(unsigned int iobase, unsigned short addr)
{
	outb(addr & 0xff, iobase + ADDR_L);
	outb((addr >> 8) & 0xff, iobase + ADDR_H);
}


inline void bt3c_put(unsigned int iobase, unsigned short value)
{
	outb(value & 0xff, iobase + DATA_L);
	outb((value >> 8) & 0xff, iobase + DATA_H);
}


inline void bt3c_io_write(unsigned int iobase, unsigned short addr, unsigned short value)
{
	bt3c_address(iobase, addr);
	bt3c_put(iobase, value);
}


inline unsigned short bt3c_get(unsigned int iobase)
{
	unsigned short value = inb(iobase + DATA_L);

	value |= inb(iobase + DATA_H) << 8;

	return value;
}


inline unsigned short bt3c_read(unsigned int iobase, unsigned short addr)
{
	bt3c_address(iobase, addr);

	return bt3c_get(iobase);
}



/* ======================== Interrupt handling ======================== */


static int bt3c_write(unsigned int iobase, int fifo_size, __u8 *buf, int len)
{
	int actual = 0;

	bt3c_address(iobase, 0x7080);

	/* Fill FIFO with current frame */
	while (actual < len) {
		/* Transmit next byte */
		bt3c_put(iobase, buf[actual]);
		actual++;
	}

	bt3c_io_write(iobase, 0x7005, actual);

	return actual;
}


static void bt3c_write_wakeup(bt3c_info_t *info)
{
	if (!info) {
		BT_ERR("Unknown device");
		return;
	}

	if (test_and_set_bit(XMIT_SENDING, &(info->tx_state)))
		return;

	do {
		register unsigned int iobase = info->link.io.BasePort1;
		register struct sk_buff *skb;
		register int len;

		if (!(info->link.state & DEV_PRESENT))
			break;


		if (!(skb = skb_dequeue(&(info->txq)))) {
			clear_bit(XMIT_SENDING, &(info->tx_state));
			break;
		}

		/* Send frame */
		len = bt3c_write(iobase, 256, skb->data, skb->len);

		if (len != skb->len) {
			BT_ERR("Very strange");
		}

		kfree_skb(skb);

		info->hdev->stat.byte_tx += len;

	} while (0);
}


static void bt3c_receive(bt3c_info_t *info)
{
	unsigned int iobase;
	int size = 0, avail;

	if (!info) {
		BT_ERR("Unknown device");
		return;
	}

	iobase = info->link.io.BasePort1;

	avail = bt3c_read(iobase, 0x7006);
	//printk("bt3c_cs: receiving %d bytes\n", avail);

	bt3c_address(iobase, 0x7480);
	while (size < avail) {
		size++;
		info->hdev->stat.byte_rx++;

		/* Allocate packet */
		if (info->rx_skb == NULL) {
			info->rx_state = RECV_WAIT_PACKET_TYPE;
			info->rx_count = 0;
			if (!(info->rx_skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC))) {
				BT_ERR("Can't allocate mem for new packet");
				return;
			}
		}


		if (info->rx_state == RECV_WAIT_PACKET_TYPE) {

			info->rx_skb->dev = (void *) info->hdev;
			info->rx_skb->pkt_type = inb(iobase + DATA_L);
			inb(iobase + DATA_H);
			//printk("bt3c: PACKET_TYPE=%02x\n", info->rx_skb->pkt_type);

			switch (info->rx_skb->pkt_type) {

			case HCI_EVENT_PKT:
				info->rx_state = RECV_WAIT_EVENT_HEADER;
				info->rx_count = HCI_EVENT_HDR_SIZE;
				break;

			case HCI_ACLDATA_PKT:
				info->rx_state = RECV_WAIT_ACL_HEADER;
				info->rx_count = HCI_ACL_HDR_SIZE;
				break;

			case HCI_SCODATA_PKT:
				info->rx_state = RECV_WAIT_SCO_HEADER;
				info->rx_count = HCI_SCO_HDR_SIZE;
				break;

			default:
				/* Unknown packet */
				BT_ERR("Unknown HCI packet with type 0x%02x received", info->rx_skb->pkt_type);
				info->hdev->stat.err_rx++;
				clear_bit(HCI_RUNNING, &(info->hdev->flags));

				kfree_skb(info->rx_skb);
				info->rx_skb = NULL;
				break;

			}

		} else {

			__u8 x = inb(iobase + DATA_L);

			*skb_put(info->rx_skb, 1) = x;
			inb(iobase + DATA_H);
			info->rx_count--;

			if (info->rx_count == 0) {

				int dlen;
				struct hci_event_hdr *eh;
				struct hci_acl_hdr *ah;
				struct hci_sco_hdr *sh;

				switch (info->rx_state) {

				case RECV_WAIT_EVENT_HEADER:
					eh = (struct hci_event_hdr *)(info->rx_skb->data);
					info->rx_state = RECV_WAIT_DATA;
					info->rx_count = eh->plen;
					break;

				case RECV_WAIT_ACL_HEADER:
					ah = (struct hci_acl_hdr *)(info->rx_skb->data);
					dlen = __le16_to_cpu(ah->dlen);
					info->rx_state = RECV_WAIT_DATA;
					info->rx_count = dlen;
					break;

				case RECV_WAIT_SCO_HEADER:
					sh = (struct hci_sco_hdr *)(info->rx_skb->data);
					info->rx_state = RECV_WAIT_DATA;
					info->rx_count = sh->dlen;
					break;

				case RECV_WAIT_DATA:
					hci_recv_frame(info->rx_skb);
					info->rx_skb = NULL;
					break;

				}

			}

		}

	}

	bt3c_io_write(iobase, 0x7006, 0x0000);
}


static irqreturn_t bt3c_interrupt(int irq, void *dev_inst, struct pt_regs *regs)
{
	bt3c_info_t *info = dev_inst;
	unsigned int iobase;
	int iir;

	if (!info || !info->hdev) {
		BT_ERR("Call of irq %d for unknown device", irq);
		return IRQ_NONE;
	}

	iobase = info->link.io.BasePort1;

	spin_lock(&(info->lock));

	iir = inb(iobase + CONTROL);
	if (iir & 0x80) {
		int stat = bt3c_read(iobase, 0x7001);

		if ((stat & 0xff) == 0x7f) {
			BT_ERR("Very strange (stat=0x%04x)", stat);
		} else if ((stat & 0xff) != 0xff) {
			if (stat & 0x0020) {
				int stat = bt3c_read(iobase, 0x7002) & 0x10;
				BT_INFO("%s: Antenna %s", info->hdev->name,
							stat ? "out" : "in");
			}
			if (stat & 0x0001)
				bt3c_receive(info);
			if (stat & 0x0002) {
				//BT_ERR("Ack (stat=0x%04x)", stat);
				clear_bit(XMIT_SENDING, &(info->tx_state));
				bt3c_write_wakeup(info);
			}

			bt3c_io_write(iobase, 0x7001, 0x0000);

			outb(iir, iobase + CONTROL);
		}
	}

	spin_unlock(&(info->lock));

	return IRQ_HANDLED;
}



/* ======================== HCI interface ======================== */


static int bt3c_hci_flush(struct hci_dev *hdev)
{
	bt3c_info_t *info = (bt3c_info_t *)(hdev->driver_data);

	/* Drop TX queue */
	skb_queue_purge(&(info->txq));

	return 0;
}


static int bt3c_hci_open(struct hci_dev *hdev)
{
	set_bit(HCI_RUNNING, &(hdev->flags));

	return 0;
}


static int bt3c_hci_close(struct hci_dev *hdev)
{
	if (!test_and_clear_bit(HCI_RUNNING, &(hdev->flags)))
		return 0;

	bt3c_hci_flush(hdev);

	return 0;
}


static int bt3c_hci_send_frame(struct sk_buff *skb)
{
	bt3c_info_t *info;
	struct hci_dev *hdev = (struct hci_dev *)(skb->dev);
	unsigned long flags;

	if (!hdev) {
		BT_ERR("Frame for unknown HCI device (hdev=NULL)");
		return -ENODEV;
	}

	info = (bt3c_info_t *) (hdev->driver_data);

	switch (skb->pkt_type) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	};

	/* Prepend skb with frame type */
	memcpy(skb_push(skb, 1), &(skb->pkt_type), 1);
	skb_queue_tail(&(info->txq), skb);

	spin_lock_irqsave(&(info->lock), flags);

	bt3c_write_wakeup(info);

	spin_unlock_irqrestore(&(info->lock), flags);

	return 0;
}


static void bt3c_hci_destruct(struct hci_dev *hdev)
{
}


static int bt3c_hci_ioctl(struct hci_dev *hdev, unsigned int cmd, unsigned long arg)
{
	return -ENOIOCTLCMD;
}



/* ======================== Card services HCI interaction ======================== */


static struct device *bt3c_device(void)
{
	static char *kobj_name = "bt3c";

	static struct device dev = {
		.bus_id = "pcmcia",
	};
	dev.kobj.k_name = kmalloc(strlen(kobj_name) + 1, GFP_KERNEL);
	strcpy(dev.kobj.k_name, kobj_name);
	kobject_init(&dev.kobj);

	return &dev;
}


static int bt3c_load_firmware(bt3c_info_t *info, unsigned char *firmware, int count)
{
	char *ptr = (char *) firmware;
	char b[9];
	unsigned int iobase, size, addr, fcs, tmp;
	int i, err = 0;

	iobase = info->link.io.BasePort1;

	/* Reset */
	bt3c_io_write(iobase, 0x8040, 0x0404);
	bt3c_io_write(iobase, 0x8040, 0x0400);

	udelay(1);

	bt3c_io_write(iobase, 0x8040, 0x0404);

	udelay(17);

	/* Load */
	while (count) {
		if (ptr[0] != 'S') {
			BT_ERR("Bad address in firmware");
			err = -EFAULT;
			goto error;
		}

		memset(b, 0, sizeof(b));
		memcpy(b, ptr + 2, 2);
		size = simple_strtol(b, NULL, 16);

		memset(b, 0, sizeof(b));
		memcpy(b, ptr + 4, 8);
		addr = simple_strtol(b, NULL, 16);

		memset(b, 0, sizeof(b));
		memcpy(b, ptr + (size * 2) + 2, 2);
		fcs = simple_strtol(b, NULL, 16);

		memset(b, 0, sizeof(b));
		for (tmp = 0, i = 0; i < size; i++) {
			memcpy(b, ptr + (i * 2) + 2, 2);
			tmp += simple_strtol(b, NULL, 16);
		}

		if (((tmp + fcs) & 0xff) != 0xff) {
			BT_ERR("Checksum error in firmware");
			err = -EILSEQ;
			goto error;
		}

		if (ptr[1] == '3') {
			bt3c_address(iobase, addr);

			memset(b, 0, sizeof(b));
			for (i = 0; i < (size - 4) / 2; i++) {
				memcpy(b, ptr + (i * 4) + 12, 4);
				tmp = simple_strtol(b, NULL, 16);
				bt3c_put(iobase, tmp);
			}
		}

		ptr   += (size * 2) + 6;
		count -= (size * 2) + 6;
	}

	udelay(17);

	/* Boot */
	bt3c_address(iobase, 0x3000);
	outb(inb(iobase + CONTROL) | 0x40, iobase + CONTROL);

error:
	udelay(17);

	/* Clear */
	bt3c_io_write(iobase, 0x7006, 0x0000);
	bt3c_io_write(iobase, 0x7005, 0x0000);
	bt3c_io_write(iobase, 0x7001, 0x0000);

	return err;
}


int bt3c_open(bt3c_info_t *info)
{
	const struct firmware *firmware;
	struct hci_dev *hdev;
	int err;

	spin_lock_init(&(info->lock));

	skb_queue_head_init(&(info->txq));

	info->rx_state = RECV_WAIT_PACKET_TYPE;
	info->rx_count = 0;
	info->rx_skb = NULL;

	/* Initialize HCI device */
	hdev = hci_alloc_dev();
	if (!hdev) {
		BT_ERR("Can't allocate HCI device");
		return -ENOMEM;
	}

	info->hdev = hdev;

	hdev->type = HCI_PCCARD;
	hdev->driver_data = info;

	hdev->open     = bt3c_hci_open;
	hdev->close    = bt3c_hci_close;
	hdev->flush    = bt3c_hci_flush;
	hdev->send     = bt3c_hci_send_frame;
	hdev->destruct = bt3c_hci_destruct;
	hdev->ioctl    = bt3c_hci_ioctl;

	hdev->owner = THIS_MODULE;

	/* Load firmware */
	err = request_firmware(&firmware, "BT3CPCC.bin", bt3c_device());
	if (err < 0) {
		BT_ERR("Firmware request failed");
		goto error;
	}

	err = bt3c_load_firmware(info, firmware->data, firmware->size);

	release_firmware(firmware);

	if (err < 0) {
		BT_ERR("Firmware loading failed");
		goto error;
	}

	/* Timeout before it is safe to send the first HCI packet */
	msleep(1000);

	/* Register HCI device */
	err = hci_register_dev(hdev);
	if (err < 0) {
		BT_ERR("Can't register HCI device");
		goto error;
	}

	return 0;

error:
	info->hdev = NULL;
	hci_free_dev(hdev);
	return err;
}


int bt3c_close(bt3c_info_t *info)
{
	struct hci_dev *hdev = info->hdev;

	if (!hdev)
		return -ENODEV;

	bt3c_hci_close(hdev);

	if (hci_unregister_dev(hdev) < 0)
		BT_ERR("Can't unregister HCI device %s", hdev->name);

	hci_free_dev(hdev);

	return 0;
}

dev_link_t *bt3c_attach(void)
{
	bt3c_info_t *info;
	client_reg_t client_reg;
	dev_link_t *link;
	int i, ret;

	/* Create new info device */
	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return NULL;
	memset(info, 0, sizeof(*info));

	link = &info->link;
	link->priv = info;

	link->io.Attributes1 = IO_DATA_PATH_WIDTH_8;
	link->io.NumPorts1 = 8;
	link->irq.Attributes = IRQ_TYPE_EXCLUSIVE | IRQ_HANDLE_PRESENT;
	link->irq.IRQInfo1 = IRQ_INFO2_VALID | IRQ_LEVEL_ID;

	if (irq_list[0] == -1)
		link->irq.IRQInfo2 = irq_mask;
	else
		for (i = 0; i < 4; i++)
			link->irq.IRQInfo2 |= 1 << irq_list[i];

	link->irq.Handler = bt3c_interrupt;
	link->irq.Instance = info;

	link->conf.Attributes = CONF_ENABLE_IRQ;
	link->conf.Vcc = 50;
	link->conf.IntType = INT_MEMORY_AND_IO;

	/* Register with Card Services */
	link->next = dev_list;
	dev_list = link;
	client_reg.dev_info = &dev_info;
	client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
	client_reg.EventMask =
	    CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
	    CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
	    CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
	client_reg.event_handler = &bt3c_event;
	client_reg.Version = 0x0210;
	client_reg.event_callback_args.client_data = link;

	ret = pcmcia_register_client(&link->handle, &client_reg);
	if (ret != CS_SUCCESS) {
		cs_error(link->handle, RegisterClient, ret);
		bt3c_detach(link);
		return NULL;
	}

	return link;
}


void bt3c_detach(dev_link_t *link)
{
	bt3c_info_t *info = link->priv;
	dev_link_t **linkp;
	int ret;

	/* Locate device structure */
	for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
		if (*linkp == link)
			break;

	if (*linkp == NULL)
		return;

	if (link->state & DEV_CONFIG)
		bt3c_release(link);

	if (link->handle) {
		ret = pcmcia_deregister_client(link->handle);
		if (ret != CS_SUCCESS)
			cs_error(link->handle, DeregisterClient, ret);
	}

	/* Unlink device structure, free bits */
	*linkp = link->next;

	kfree(info);
}

static int get_tuple(client_handle_t handle, tuple_t *tuple, cisparse_t *parse)
{
	int i;

	i = pcmcia_get_tuple_data(handle, tuple);
	if (i != CS_SUCCESS)
		return i;

	return pcmcia_parse_tuple(handle, tuple, parse);
}

static int first_tuple(client_handle_t handle, tuple_t *tuple, cisparse_t *parse)
{
	if (pcmcia_get_first_tuple(handle, tuple) != CS_SUCCESS)
		return CS_NO_MORE_ITEMS;
	return get_tuple(handle, tuple, parse);
}

static int next_tuple(client_handle_t handle, tuple_t *tuple, cisparse_t *parse)
{
	if (pcmcia_get_next_tuple(handle, tuple) != CS_SUCCESS)
		return CS_NO_MORE_ITEMS;
	return get_tuple(handle, tuple, parse);
}

void bt3c_config(dev_link_t *link)
{
	static ioaddr_t base[5] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8, 0x0 };
	client_handle_t handle = link->handle;
	bt3c_info_t *info = link->priv;
	tuple_t tuple;
	u_short buf[256];
	cisparse_t parse;
	cistpl_cftable_entry_t *cf = &parse.cftable_entry;
	config_info_t config;
	int i, j, try, last_ret, last_fn;

	tuple.TupleData = (cisdata_t *)buf;
	tuple.TupleOffset = 0;
	tuple.TupleDataMax = 255;
	tuple.Attributes = 0;

	/* Get configuration register information */
	tuple.DesiredTuple = CISTPL_CONFIG;
	last_ret = first_tuple(handle, &tuple, &parse);
	if (last_ret != CS_SUCCESS) {
		last_fn = ParseTuple;
		goto cs_failed;
	}
	link->conf.ConfigBase = parse.config.base;
	link->conf.Present = parse.config.rmask[0];

	/* Configure card */
	link->state |= DEV_CONFIG;
	i = pcmcia_get_configuration_info(handle, &config);
	link->conf.Vcc = config.Vcc;

	/* First pass: look for a config entry that looks normal. */
	tuple.TupleData = (cisdata_t *)buf;
	tuple.TupleOffset = 0;
	tuple.TupleDataMax = 255;
	tuple.Attributes = 0;
	tuple.DesiredTuple = CISTPL_CFTABLE_ENTRY;
	/* Two tries: without IO aliases, then with aliases */
	for (try = 0; try < 2; try++) {
		i = first_tuple(handle, &tuple, &parse);
		while (i != CS_NO_MORE_ITEMS) {
			if (i != CS_SUCCESS)
				goto next_entry;
			if (cf->vpp1.present & (1 << CISTPL_POWER_VNOM))
				link->conf.Vpp1 = link->conf.Vpp2 = cf->vpp1.param[CISTPL_POWER_VNOM] / 10000;
			if ((cf->io.nwin > 0) && (cf->io.win[0].len == 8) && (cf->io.win[0].base != 0)) {
				link->conf.ConfigIndex = cf->index;
				link->io.BasePort1 = cf->io.win[0].base;
				link->io.IOAddrLines = (try == 0) ? 16 : cf->io.flags & CISTPL_IO_LINES_MASK;
				i = pcmcia_request_io(link->handle, &link->io);
				if (i == CS_SUCCESS)
					goto found_port;
			}
next_entry:
			i = next_tuple(handle, &tuple, &parse);
		}
	}

	/* Second pass: try to find an entry that isn't picky about
	   its base address, then try to grab any standard serial port
	   address, and finally try to get any free port. */
	i = first_tuple(handle, &tuple, &parse);
	while (i != CS_NO_MORE_ITEMS) {
		if ((i == CS_SUCCESS) && (cf->io.nwin > 0) && ((cf->io.flags & CISTPL_IO_LINES_MASK) <= 3)) {
			link->conf.ConfigIndex = cf->index;
			for (j = 0; j < 5; j++) {
				link->io.BasePort1 = base[j];
				link->io.IOAddrLines = base[j] ? 16 : 3;
				i = pcmcia_request_io(link->handle, &link->io);
				if (i == CS_SUCCESS)
					goto found_port;
			}
		}
		i = next_tuple(handle, &tuple, &parse);
	}

found_port:
	if (i != CS_SUCCESS) {
		BT_ERR("No usable port range found");
		cs_error(link->handle, RequestIO, i);
		goto failed;
	}

	i = pcmcia_request_irq(link->handle, &link->irq);
	if (i != CS_SUCCESS) {
		cs_error(link->handle, RequestIRQ, i);
		link->irq.AssignedIRQ = 0;
	}

	i = pcmcia_request_configuration(link->handle, &link->conf);
	if (i != CS_SUCCESS) {
		cs_error(link->handle, RequestConfiguration, i);
		goto failed;
	}

	if (bt3c_open(info) != 0)
		goto failed;

	strcpy(info->node.dev_name, info->hdev->name);
	link->dev = &info->node;
	link->state &= ~DEV_CONFIG_PENDING;

	return;

cs_failed:
	cs_error(link->handle, last_fn, last_ret);

failed:
	bt3c_release(link);
}


void bt3c_release(dev_link_t *link)
{
	bt3c_info_t *info = link->priv;

	if (link->state & DEV_PRESENT)
		bt3c_close(info);

	link->dev = NULL;

	pcmcia_release_configuration(link->handle);
	pcmcia_release_io(link->handle, &link->io);
	pcmcia_release_irq(link->handle, &link->irq);

	link->state &= ~DEV_CONFIG;
}


int bt3c_event(event_t event, int priority, event_callback_args_t *args)
{
	dev_link_t *link = args->client_data;
	bt3c_info_t *info = link->priv;

	switch (event) {
	case CS_EVENT_CARD_REMOVAL:
		link->state &= ~DEV_PRESENT;
		if (link->state & DEV_CONFIG) {
			bt3c_close(info);
			bt3c_release(link);
		}
		break;
	case CS_EVENT_CARD_INSERTION:
		link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
		bt3c_config(link);
		break;
	case CS_EVENT_PM_SUSPEND:
		link->state |= DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_RESET_PHYSICAL:
		if (link->state & DEV_CONFIG)
			pcmcia_release_configuration(link->handle);
		break;
	case CS_EVENT_PM_RESUME:
		link->state &= ~DEV_SUSPEND;
		/* Fall through... */
	case CS_EVENT_CARD_RESET:
		if (DEV_OK(link))
			pcmcia_request_configuration(link->handle, &link->conf);
		break;
	}

	return 0;
}

static struct pcmcia_driver bt3c_driver = {
	.owner		= THIS_MODULE,
	.drv		= {
		.name	= "bt3c_cs",
	},
	.attach		= bt3c_attach,
	.detach		= bt3c_detach,
};

static int __init init_bt3c_cs(void)
{
	return pcmcia_register_driver(&bt3c_driver);
}


static void __exit exit_bt3c_cs(void)
{
	pcmcia_unregister_driver(&bt3c_driver);

	/* XXX: this really needs to move into generic code.. */
	while (dev_list != NULL)
		bt3c_detach(dev_list);
}

module_init(init_bt3c_cs);
module_exit(exit_bt3c_cs);
