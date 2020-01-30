/*
 * MosChips MCS7830 based USB 2.0 Ethernet Devices
 *
 * based on usbnet.c, asix.c and the vendor provided mcs7830 driver
 *
 * Copyright (C) 2006 Arnd Bergmann <arnd@arndb.de>
 * Copyright (C) 2003-2005 David Hollis <dhollis@davehollis.com>
 * Copyright (C) 2005 Phil Chang <pchang23@sbcglobal.net>
 * Copyright (c) 2002-2003 TiVo Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/crc32.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/init.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/usb.h>

#include "usbnet.h"

/* requests */
#define MCS7830_RD_BMREQ	(USB_DIR_IN  | USB_TYPE_VENDOR | \
				 USB_RECIP_DEVICE)
#define MCS7830_WR_BMREQ	(USB_DIR_OUT | USB_TYPE_VENDOR | \
				 USB_RECIP_DEVICE)
#define MCS7830_RD_BREQ		0x0E
#define MCS7830_WR_BREQ		0x0D

#define MCS7830_CTRL_TIMEOUT	1000
#define MCS7830_MAX_MCAST	64

#define MCS7830_VENDOR_ID	0x9710
#define MCS7830_PRODUCT_ID	0x7830

#define MCS7830_MII_ADVERTISE	(ADVERTISE_PAUSE_CAP | ADVERTISE_100FULL | \
				 ADVERTISE_100HALF | ADVERTISE_10FULL | \
				 ADVERTISE_10HALF | ADVERTISE_CSMA)

/* HIF_REG_XX coressponding index value */
enum {
	HIF_REG_MULTICAST_HASH			= 0x00,
	HIF_REG_PACKET_GAP1			= 0x08,
	HIF_REG_PACKET_GAP2			= 0x09,
	HIF_REG_PHY_DATA			= 0x0a,
	HIF_REG_PHY_CMD1			= 0x0c,
	   HIF_REG_PHY_CMD1_READ		= 0x40,
	   HIF_REG_PHY_CMD1_WRITE		= 0x20,
	   HIF_REG_PHY_CMD1_PHYADDR		= 0x01,
	HIF_REG_PHY_CMD2			= 0x0d,
	   HIF_REG_PHY_CMD2_PEND_FLAG_BIT	= 0x80,
	   HIF_REG_PHY_CMD2_READY_FLAG_BIT	= 0x40,
	HIF_REG_CONFIG				= 0x0e,
	   HIF_REG_CONFIG_CFG			= 0x80,
	   HIF_REG_CONFIG_SPEED100		= 0x40,
	   HIF_REG_CONFIG_FULLDUPLEX_ENABLE	= 0x20,
	   HIF_REG_CONFIG_RXENABLE		= 0x10,
	   HIF_REG_CONFIG_TXENABLE		= 0x08,
	   HIF_REG_CONFIG_SLEEPMODE		= 0x04,
	   HIF_REG_CONFIG_ALLMULTICAST		= 0x02,
	   HIF_REG_CONFIG_PROMISCIOUS		= 0x01,
	HIF_REG_ETHERNET_ADDR			= 0x0f,
	HIF_REG_22				= 0x15,
	HIF_REG_PAUSE_THRESHOLD			= 0x16,
	   HIF_REG_PAUSE_THRESHOLD_DEFAULT	= 0,
};

struct mcs7830_data {
	u8 multi_filter[8];
	u8 config;
};

static const char driver_name[] = "MOSCHIP usb-ethernet driver";

static int mcs7830_get_reg(struct usbnet *dev, u16 index, u16 size, void *data)
{
	struct usb_device *xdev = dev->udev;
	int ret;

	ret = usb_control_msg(xdev, usb_rcvctrlpipe(xdev, 0), MCS7830_RD_BREQ,
			      MCS7830_RD_BMREQ, 0x0000, index, data,
			      size, msecs_to_jiffies(MCS7830_CTRL_TIMEOUT));
	return ret;
}

static int mcs7830_set_reg(struct usbnet *dev, u16 index, u16 size, void *data)
{
	struct usb_device *xdev = dev->udev;
	int ret;

	ret = usb_control_msg(xdev, usb_sndctrlpipe(xdev, 0), MCS7830_WR_BREQ,
			      MCS7830_WR_BMREQ, 0x0000, index, data,
			      size, msecs_to_jiffies(MCS7830_CTRL_TIMEOUT));
	return ret;
}

static void mcs7830_async_cmd_callback(struct urb *urb)
{
	struct usb_ctrlrequest *req = (struct usb_ctrlrequest *)urb->context;

	if (urb->status < 0)
		printk(KERN_DEBUG "mcs7830_async_cmd_callback() failed with %d",
			urb->status);

	kfree(req);
	usb_free_urb(urb);
}

static void mcs7830_set_reg_async(struct usbnet *dev, u16 index, u16 size, void *data)
{
	struct usb_ctrlrequest *req;
	int ret;
	struct urb *urb;

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		dev_dbg(&dev->udev->dev, "Error allocating URB "
				"in write_cmd_async!");
		return;
	}

	req = kmalloc(sizeof *req, GFP_ATOMIC);
	if (!req) {
		dev_err(&dev->udev->dev, "Failed to allocate memory for "
				"control request");
		goto out;
	}
	req->bRequestType = MCS7830_WR_BMREQ;
	req->bRequest = MCS7830_WR_BREQ;
	req->wValue = 0;
	req->wIndex = cpu_to_le16(index);
	req->wLength = cpu_to_le16(size);

	usb_fill_control_urb(urb, dev->udev,
			     usb_sndctrlpipe(dev->udev, 0),
			     (void *)req, data, size,
			     mcs7830_async_cmd_callback, req);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err(&dev->udev->dev, "Error submitting the control "
				"message: ret=%d", ret);
		goto out;
	}
	return;
out:
	kfree(req);
	usb_free_urb(urb);
}

static int mcs7830_get_address(struct usbnet *dev)
{
	int ret;
	ret = mcs7830_get_reg(dev, HIF_REG_ETHERNET_ADDR, ETH_ALEN,
				   dev->net->dev_addr);
	if (ret < 0)
		return ret;
	return 0;
}

static int mcs7830_read_phy(struct usbnet *dev, u8 index)
{
	int ret;
	int i;
	__le16 val;

	u8 cmd[2] = {
		HIF_REG_PHY_CMD1_READ | HIF_REG_PHY_CMD1_PHYADDR,
		HIF_REG_PHY_CMD2_PEND_FLAG_BIT | index,
	};

	mutex_lock(&dev->phy_mutex);
	/* write the MII command */
	ret = mcs7830_set_reg(dev, HIF_REG_PHY_CMD1, 2, cmd);
	if (ret < 0)
		goto out;

	/* wait for the data to become valid, should be within < 1ms */
	for (i = 0; i < 10; i++) {
		ret = mcs7830_get_reg(dev, HIF_REG_PHY_CMD1, 2, cmd);
		if ((ret < 0) || (cmd[1] & HIF_REG_PHY_CMD2_READY_FLAG_BIT))
			break;
		ret = -EIO;
		msleep(1);
	}
	if (ret < 0)
		goto out;

	/* read actual register contents */
	ret = mcs7830_get_reg(dev, HIF_REG_PHY_DATA, 2, &val);
	if (ret < 0)
		goto out;
	ret = le16_to_cpu(val);
	dev_dbg(&dev->udev->dev, "read PHY reg %02x: %04x (%d tries)\n",
		index, val, i);
out:
	mutex_unlock(&dev->phy_mutex);
	return ret;
}

static int mcs7830_write_phy(struct usbnet *dev, u8 index, u16 val)
{
	int ret;
	int i;
	__le16 le_val;

	u8 cmd[2] = {
		HIF_REG_PHY_CMD1_WRITE | HIF_REG_PHY_CMD1_PHYADDR,
		HIF_REG_PHY_CMD2_PEND_FLAG_BIT | (index & 0x1F),
	};

	mutex_lock(&dev->phy_mutex);

	/* write the new register contents */
	le_val = cpu_to_le16(val);
	ret = mcs7830_set_reg(dev, HIF_REG_PHY_DATA, 2, &le_val);
	if (ret < 0)
		goto out;

	/* write the MII command */
	ret = mcs7830_set_reg(dev, HIF_REG_PHY_CMD1, 2, cmd);
	if (ret < 0)
		goto out;

	/* wait for the command to be accepted by the PHY */
	for (i = 0; i < 10; i++) {
		ret = mcs7830_get_reg(dev, HIF_REG_PHY_CMD1, 2, cmd);
		if ((ret < 0) || (cmd[1] & HIF_REG_PHY_CMD2_READY_FLAG_BIT))
			break;
		ret = -EIO;
		msleep(1);
	}
	if (ret < 0)
		goto out;

	ret = 0;
	dev_dbg(&dev->udev->dev, "write PHY reg %02x: %04x (%d tries)\n",
		index, val, i);
out:
	mutex_unlock(&dev->phy_mutex);
	return ret;
}

/*
 * This algorithm comes from the original mcs7830 version 1.4 driver,
 * not sure if it is needed.
 */
static int mcs7830_set_autoneg(struct usbnet *dev, int ptrUserPhyMode)
{
	int ret;
	/* Enable all media types */
	ret = mcs7830_write_phy(dev, MII_ADVERTISE, MCS7830_MII_ADVERTISE);

	/* First reset BMCR */
	if (!ret)
		ret = mcs7830_write_phy(dev, MII_BMCR, 0x0000);
	/* Enable Auto Neg */
	if (!ret)
		ret = mcs7830_write_phy(dev, MII_BMCR, BMCR_ANENABLE);
	/* Restart Auto Neg (Keep the Enable Auto Neg Bit Set) */
	if (!ret)
		ret = mcs7830_write_phy(dev, MII_BMCR,
				BMCR_ANENABLE | BMCR_ANRESTART	);
	return ret < 0 ? : 0;
}


/*
 * if we can read register 22, the chip revision is C or higher
 */
static int mcs7830_get_rev(struct usbnet *dev)
{
	u8 dummy[2];
	int ret;
	ret = mcs7830_get_reg(dev, HIF_REG_22, 2, dummy);
	if (ret > 0)
		return 2; /* Rev C or later */
	return 1; /* earlier revision */
}

/*
 * On rev. C we need to set the pause threshold
 */
static void mcs7830_rev_C_fixup(struct usbnet *dev)
{
	u8 pause_threshold = HIF_REG_PAUSE_THRESHOLD_DEFAULT;
	int retry;

	for (retry = 0; retry < 2; retry++) {
		if (mcs7830_get_rev(dev) == 2) {
			dev_info(&dev->udev->dev, "applying rev.C fixup\n");
			mcs7830_set_reg(dev, HIF_REG_PAUSE_THRESHOLD,
					1, &pause_threshold);
		}
		msleep(1);
	}
}

static int mcs7830_init_dev(struct usbnet *dev)
{
	int ret;
	int retry;

	/* Read MAC address from EEPROM */
	ret = -EINVAL;
	for (retry = 0; retry < 5 && ret; retry++)
		ret = mcs7830_get_address(dev);
	if (ret) {
		dev_warn(&dev->udev->dev, "Cannot read MAC address\n");
		goto out;
	}

	/* Set up PHY */
	ret = mcs7830_set_autoneg(dev, 0);
	if (ret) {
		dev_info(&dev->udev->dev, "Cannot set autoneg\n");
		goto out;
	}

	mcs7830_rev_C_fixup(dev);
	ret = 0;
out:
	return ret;
}

static int mcs7830_mdio_read(struct net_device *netdev, int phy_id,
			     int location)
{
	struct usbnet *dev = netdev->priv;
	return mcs7830_read_phy(dev, location);
}

static void mcs7830_mdio_write(struct net_device *netdev, int phy_id,
				int location, int val)
{
	struct usbnet *dev = netdev->priv;
	mcs7830_write_phy(dev, location, val);
}

static int mcs7830_ioctl(struct net_device *net, struct ifreq *rq, int cmd)
{
	struct usbnet *dev = netdev_priv(net);
	return generic_mii_ioctl(&dev->mii, if_mii(rq), cmd, NULL);
}

/* credits go to asix_set_multicast */
static void mcs7830_set_multicast(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);
	struct mcs7830_data *data = (struct mcs7830_data *)&dev->data;

	data->config = HIF_REG_CONFIG_TXENABLE;

	/* this should not be needed, but it doesn't work otherwise */
	data->config |= HIF_REG_CONFIG_ALLMULTICAST;

	if (net->flags & IFF_PROMISC) {
		data->config |= HIF_REG_CONFIG_PROMISCIOUS;
	} else if (net->flags & IFF_ALLMULTI
		   || net->mc_count > MCS7830_MAX_MCAST) {
		data->config |= HIF_REG_CONFIG_ALLMULTICAST;
	} else if (net->mc_count == 0) {
		/* just broadcast and directed */
	} else {
		/* We use the 20 byte dev->data
		 * for our 8 byte filter buffer
		 * to avoid allocating memory that
		 * is tricky to free later */
		struct dev_mc_list *mc_list = net->mc_list;
		u32 crc_bits;
		int i;

		memset(data->multi_filter, 0, sizeof data->multi_filter);

		/* Build the multicast hash filter. */
		for (i = 0; i < net->mc_count; i++) {
			crc_bits = ether_crc(ETH_ALEN, mc_list->dmi_addr) >> 26;
			data->multi_filter[crc_bits >> 3] |= 1 << (crc_bits & 7);
			mc_list = mc_list->next;
		}

		mcs7830_set_reg_async(dev, HIF_REG_MULTICAST_HASH,
				sizeof data->multi_filter,
				data->multi_filter);
	}

	mcs7830_set_reg_async(dev, HIF_REG_CONFIG, 1, &data->config);
}

static int mcs7830_get_regs_len(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);

	switch (mcs7830_get_rev(dev)) {
	case 1:
		return 21;
	case 2:
		return 32;
	}
	return 0;
}

static void mcs7830_get_drvinfo(struct net_device *net, struct ethtool_drvinfo *drvinfo)
{
	usbnet_get_drvinfo(net, drvinfo);
	drvinfo->regdump_len = mcs7830_get_regs_len(net);
}

static void mcs7830_get_regs(struct net_device *net, struct ethtool_regs *regs, void *data)
{
	struct usbnet *dev = netdev_priv(net);

	regs->version = mcs7830_get_rev(dev);
	mcs7830_get_reg(dev, 0, regs->len, data);
}

static struct ethtool_ops mcs7830_ethtool_ops = {
	.get_drvinfo		= mcs7830_get_drvinfo,
	.get_regs_len		= mcs7830_get_regs_len,
	.get_regs		= mcs7830_get_regs,

	/* common usbnet calls */
	.get_link		= usbnet_get_link,
	.get_msglevel		= usbnet_get_msglevel,
	.set_msglevel		= usbnet_set_msglevel,
	.get_settings		= usbnet_get_settings,
	.set_settings		= usbnet_set_settings,
	.nway_reset		= usbnet_nway_reset,
};

static int mcs7830_bind(struct usbnet *dev, struct usb_interface *udev)
{
	struct net_device *net = dev->net;
	int ret;

	ret = mcs7830_init_dev(dev);
	if (ret)
		goto out;

	net->do_ioctl = mcs7830_ioctl;
	net->ethtool_ops = &mcs7830_ethtool_ops;
	net->set_multicast_list = mcs7830_set_multicast;
	mcs7830_set_multicast(net);

	/* reserve space for the status byte on rx */
	dev->rx_urb_size = ETH_FRAME_LEN + 1;

	dev->mii.mdio_read = mcs7830_mdio_read;
	dev->mii.mdio_write = mcs7830_mdio_write;
	dev->mii.dev = net;
	dev->mii.phy_id_mask = 0x3f;
	dev->mii.reg_num_mask = 0x1f;
	dev->mii.phy_id = *((u8 *) net->dev_addr + 1);

	ret = usbnet_get_endpoints(dev, udev);
out:
	return ret;
}

/* The chip always appends a status bytes that we need to strip */
static int mcs7830_rx_fixup(struct usbnet *dev, struct sk_buff *skb)
{
	u8 status;

	if (skb->len == 0) {
		dev_err(&dev->udev->dev, "unexpected empty rx frame\n");
		return 0;
	}

	skb_trim(skb, skb->len - 1);
	status = skb->data[skb->len];

	if (status != 0x20)
		dev_dbg(&dev->udev->dev, "rx fixup status %x\n", status);

	return skb->len > 0;
}

static const struct driver_info moschip_info = {
	.description	= "MOSCHIP 7830 usb-NET adapter",
	.bind		= mcs7830_bind,
	.rx_fixup	= mcs7830_rx_fixup,
	.flags		= FLAG_ETHER,
	.in		= 1,
	.out		= 2,
};

static const struct usb_device_id products[] = {
	{
		USB_DEVICE(MCS7830_VENDOR_ID, MCS7830_PRODUCT_ID),
		.driver_info = (unsigned long) &moschip_info,
	},
	{},
};
MODULE_DEVICE_TABLE(usb, products);

static struct usb_driver mcs7830_driver = {
	.name = driver_name,
	.id_table = products,
	.probe = usbnet_probe,
	.disconnect = usbnet_disconnect,
	.suspend = usbnet_suspend,
	.resume = usbnet_resume,
};

static int __init mcs7830_init(void)
{
	return usb_register(&mcs7830_driver);
}
module_init(mcs7830_init);

static void __exit mcs7830_exit(void)
{
	usb_deregister(&mcs7830_driver);
}
module_exit(mcs7830_exit);

MODULE_DESCRIPTION("USB to network adapter MCS7830)");
MODULE_LICENSE("GPL");
