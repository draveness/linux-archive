/*
 * net/core/ethtool.c - Ethtool ioctl handler
 * Copyright (c) 2003 Matthew Wilcox <matthew@wil.cx>
 *
 * This file is where we call all the ethtool_ops commands to get
 * the information ethtool needs.  We fall back to calling do_ioctl()
 * for drivers which haven't been converted to ethtool_ops yet.
 *
 * It's GPL, stupid.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <asm/uaccess.h>

/* 
 * Some useful ethtool_ops methods that're device independent.
 * If we find that all drivers want to do the same thing here,
 * we can turn these into dev_() function calls.
 */

u32 ethtool_op_get_link(struct net_device *dev)
{
	return netif_carrier_ok(dev) ? 1 : 0;
}

u32 ethtool_op_get_tx_csum(struct net_device *dev)
{
	return (dev->features & NETIF_F_IP_CSUM) != 0;
}

int ethtool_op_set_tx_csum(struct net_device *dev, u32 data)
{
	if (data)
		dev->features |= NETIF_F_IP_CSUM;
	else
		dev->features &= ~NETIF_F_IP_CSUM;

	return 0;
}

u32 ethtool_op_get_sg(struct net_device *dev)
{
	return (dev->features & NETIF_F_SG) != 0;
}

int ethtool_op_set_sg(struct net_device *dev, u32 data)
{
	if (data)
		dev->features |= NETIF_F_SG;
	else
		dev->features &= ~NETIF_F_SG;

	return 0;
}

u32 ethtool_op_get_tso(struct net_device *dev)
{
	return (dev->features & NETIF_F_TSO) != 0;
}

int ethtool_op_set_tso(struct net_device *dev, u32 data)
{
	if (data)
		dev->features |= NETIF_F_TSO;
	else
		dev->features &= ~NETIF_F_TSO;

	return 0;
}

/* Handlers for each ethtool command */

static int ethtool_get_settings(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_cmd cmd = { ETHTOOL_GSET };
	int err;

	if (!dev->ethtool_ops->get_settings)
		return -EOPNOTSUPP;

	err = dev->ethtool_ops->get_settings(dev, &cmd);
	if (err < 0)
		return err;

	if (copy_to_user(useraddr, &cmd, sizeof(cmd)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_settings(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_cmd cmd;

	if (!dev->ethtool_ops->set_settings)
		return -EOPNOTSUPP;

	if (copy_from_user(&cmd, useraddr, sizeof(cmd)))
		return -EFAULT;

	return dev->ethtool_ops->set_settings(dev, &cmd);
}

static int ethtool_get_drvinfo(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_drvinfo info;
	struct ethtool_ops *ops = dev->ethtool_ops;

	if (!ops->get_drvinfo)
		return -EOPNOTSUPP;

	memset(&info, 0, sizeof(info));
	info.cmd = ETHTOOL_GDRVINFO;
	ops->get_drvinfo(dev, &info);

	if (ops->self_test_count)
		info.testinfo_len = ops->self_test_count(dev);
	if (ops->get_stats_count)
		info.n_stats = ops->get_stats_count(dev);
	if (ops->get_regs_len)
		info.regdump_len = ops->get_regs_len(dev);
	if (ops->get_eeprom_len)
		info.eedump_len = ops->get_eeprom_len(dev);

	if (copy_to_user(useraddr, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static int ethtool_get_regs(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_regs regs;
	struct ethtool_ops *ops = dev->ethtool_ops;
	void *regbuf;
	int reglen, ret;

	if (!ops->get_regs || !ops->get_regs_len)
		return -EOPNOTSUPP;

	if (copy_from_user(&regs, useraddr, sizeof(regs)))
		return -EFAULT;

	reglen = ops->get_regs_len(dev);
	if (regs.len > reglen)
		regs.len = reglen;

	regbuf = kmalloc(reglen, GFP_USER);
	if (!regbuf)
		return -ENOMEM;

	ops->get_regs(dev, &regs, regbuf);

	ret = -EFAULT;
	if (copy_to_user(useraddr, &regs, sizeof(regs)))
		goto out;
	useraddr += offsetof(struct ethtool_regs, data);
	if (copy_to_user(useraddr, regbuf, regs.len))
		goto out;
	ret = 0;

 out:
	kfree(regbuf);
	return ret;
}

static int ethtool_get_wol(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_wolinfo wol = { ETHTOOL_GWOL };

	if (!dev->ethtool_ops->get_wol)
		return -EOPNOTSUPP;

	dev->ethtool_ops->get_wol(dev, &wol);

	if (copy_to_user(useraddr, &wol, sizeof(wol)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_wol(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_wolinfo wol;

	if (!dev->ethtool_ops->set_wol)
		return -EOPNOTSUPP;

	if (copy_from_user(&wol, useraddr, sizeof(wol)))
		return -EFAULT;

	return dev->ethtool_ops->set_wol(dev, &wol);
}

static int ethtool_get_msglevel(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata = { ETHTOOL_GMSGLVL };

	if (!dev->ethtool_ops->get_msglevel)
		return -EOPNOTSUPP;

	edata.data = dev->ethtool_ops->get_msglevel(dev);

	if (copy_to_user(useraddr, &edata, sizeof(edata)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_msglevel(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata;

	if (!dev->ethtool_ops->set_msglevel)
		return -EOPNOTSUPP;

	if (copy_from_user(&edata, useraddr, sizeof(edata)))
		return -EFAULT;

	dev->ethtool_ops->set_msglevel(dev, edata.data);
	return 0;
}

static int ethtool_nway_reset(struct net_device *dev)
{
	if (!dev->ethtool_ops->nway_reset)
		return -EOPNOTSUPP;

	return dev->ethtool_ops->nway_reset(dev);
}

static int ethtool_get_link(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_value edata = { ETHTOOL_GLINK };

	if (!dev->ethtool_ops->get_link)
		return -EOPNOTSUPP;

	edata.data = dev->ethtool_ops->get_link(dev);

	if (copy_to_user(useraddr, &edata, sizeof(edata)))
		return -EFAULT;
	return 0;
}

static int ethtool_get_eeprom(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_eeprom eeprom;
	struct ethtool_ops *ops = dev->ethtool_ops;
	u8 *data;
	int ret;

	if (!ops->get_eeprom || !ops->get_eeprom_len)
		return -EOPNOTSUPP;

	if (copy_from_user(&eeprom, useraddr, sizeof(eeprom)))
		return -EFAULT;

	/* Check for wrap and zero */
	if (eeprom.offset + eeprom.len <= eeprom.offset)
		return -EINVAL;

	/* Check for exceeding total eeprom len */
	if (eeprom.offset + eeprom.len > ops->get_eeprom_len(dev))
		return -EINVAL;

	data = kmalloc(eeprom.len, GFP_USER);
	if (!data)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(data, useraddr + sizeof(eeprom), eeprom.len))
		goto out;

	ret = ops->get_eeprom(dev, &eeprom, data);
	if (ret)
		goto out;

	ret = -EFAULT;
	if (copy_to_user(useraddr, &eeprom, sizeof(eeprom)))
		goto out;
	if (copy_to_user(useraddr + sizeof(eeprom), data, eeprom.len))
		goto out;
	ret = 0;

 out:
	kfree(data);
	return ret;
}

static int ethtool_set_eeprom(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_eeprom eeprom;
	struct ethtool_ops *ops = dev->ethtool_ops;
	u8 *data;
	int ret;

	if (!ops->set_eeprom || !ops->get_eeprom_len)
		return -EOPNOTSUPP;

	if (copy_from_user(&eeprom, useraddr, sizeof(eeprom)))
		return -EFAULT;

	/* Check for wrap and zero */
	if (eeprom.offset + eeprom.len <= eeprom.offset)
		return -EINVAL;

	/* Check for exceeding total eeprom len */
	if (eeprom.offset + eeprom.len > ops->get_eeprom_len(dev))
		return -EINVAL;

	data = kmalloc(eeprom.len, GFP_USER);
	if (!data)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(data, useraddr + sizeof(eeprom), eeprom.len))
		goto out;

	ret = ops->set_eeprom(dev, &eeprom, data);
	if (ret)
		goto out;

	if (copy_to_user(useraddr + sizeof(eeprom), data, eeprom.len))
		ret = -EFAULT;

 out:
	kfree(data);
	return ret;
}

static int ethtool_get_coalesce(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_coalesce coalesce = { ETHTOOL_GCOALESCE };

	if (!dev->ethtool_ops->get_coalesce)
		return -EOPNOTSUPP;

	dev->ethtool_ops->get_coalesce(dev, &coalesce);

	if (copy_to_user(useraddr, &coalesce, sizeof(coalesce)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_coalesce(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_coalesce coalesce;

	if (!dev->ethtool_ops->get_coalesce)
		return -EOPNOTSUPP;

	if (copy_from_user(&coalesce, useraddr, sizeof(coalesce)))
		return -EFAULT;

	return dev->ethtool_ops->set_coalesce(dev, &coalesce);
}

static int ethtool_get_ringparam(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_ringparam ringparam = { ETHTOOL_GRINGPARAM };

	if (!dev->ethtool_ops->get_ringparam)
		return -EOPNOTSUPP;

	dev->ethtool_ops->get_ringparam(dev, &ringparam);

	if (copy_to_user(useraddr, &ringparam, sizeof(ringparam)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_ringparam(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_ringparam ringparam;

	if (!dev->ethtool_ops->set_ringparam)
		return -EOPNOTSUPP;

	if (copy_from_user(&ringparam, useraddr, sizeof(ringparam)))
		return -EFAULT;

	return dev->ethtool_ops->set_ringparam(dev, &ringparam);
}

static int ethtool_get_pauseparam(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_pauseparam pauseparam = { ETHTOOL_GPAUSEPARAM };

	if (!dev->ethtool_ops->get_pauseparam)
		return -EOPNOTSUPP;

	dev->ethtool_ops->get_pauseparam(dev, &pauseparam);

	if (copy_to_user(useraddr, &pauseparam, sizeof(pauseparam)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_pauseparam(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_pauseparam pauseparam;

	if (!dev->ethtool_ops->get_pauseparam)
		return -EOPNOTSUPP;

	if (copy_from_user(&pauseparam, useraddr, sizeof(pauseparam)))
		return -EFAULT;

	return dev->ethtool_ops->set_pauseparam(dev, &pauseparam);
}

static int ethtool_get_rx_csum(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata = { ETHTOOL_GRXCSUM };

	if (!dev->ethtool_ops->get_rx_csum)
		return -EOPNOTSUPP;

	edata.data = dev->ethtool_ops->get_rx_csum(dev);

	if (copy_to_user(useraddr, &edata, sizeof(edata)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_rx_csum(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata;

	if (!dev->ethtool_ops->set_rx_csum)
		return -EOPNOTSUPP;

	if (copy_from_user(&edata, useraddr, sizeof(edata)))
		return -EFAULT;

	dev->ethtool_ops->set_rx_csum(dev, edata.data);
	return 0;
}

static int ethtool_get_tx_csum(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata = { ETHTOOL_GTXCSUM };

	if (!dev->ethtool_ops->get_tx_csum)
		return -EOPNOTSUPP;

	edata.data = dev->ethtool_ops->get_tx_csum(dev);

	if (copy_to_user(useraddr, &edata, sizeof(edata)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_tx_csum(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata;

	if (!dev->ethtool_ops->set_tx_csum)
		return -EOPNOTSUPP;

	if (copy_from_user(&edata, useraddr, sizeof(edata)))
		return -EFAULT;

	return dev->ethtool_ops->set_tx_csum(dev, edata.data);
}

static int ethtool_get_sg(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata = { ETHTOOL_GSG };

	if (!dev->ethtool_ops->get_sg)
		return -EOPNOTSUPP;

	edata.data = dev->ethtool_ops->get_sg(dev);

	if (copy_to_user(useraddr, &edata, sizeof(edata)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_sg(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata;

	if (!dev->ethtool_ops->set_sg)
		return -EOPNOTSUPP;

	if (copy_from_user(&edata, useraddr, sizeof(edata)))
		return -EFAULT;

	return dev->ethtool_ops->set_sg(dev, edata.data);
}

static int ethtool_get_tso(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata = { ETHTOOL_GTSO };

	if (!dev->ethtool_ops->get_tso)
		return -EOPNOTSUPP;

	edata.data = dev->ethtool_ops->get_tso(dev);

	if (copy_to_user(useraddr, &edata, sizeof(edata)))
		return -EFAULT;
	return 0;
}

static int ethtool_set_tso(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_value edata;

	if (!dev->ethtool_ops->set_tso)
		return -EOPNOTSUPP;

	if (copy_from_user(&edata, useraddr, sizeof(edata)))
		return -EFAULT;

	return dev->ethtool_ops->set_tso(dev, edata.data);
}

static int ethtool_self_test(struct net_device *dev, char __user *useraddr)
{
	struct ethtool_test test;
	struct ethtool_ops *ops = dev->ethtool_ops;
	u64 *data;
	int ret;

	if (!ops->self_test || !ops->self_test_count)
		return -EOPNOTSUPP;

	if (copy_from_user(&test, useraddr, sizeof(test)))
		return -EFAULT;

	test.len = ops->self_test_count(dev);
	data = kmalloc(test.len * sizeof(u64), GFP_USER);
	if (!data)
		return -ENOMEM;

	ops->self_test(dev, &test, data);

	ret = -EFAULT;
	if (copy_to_user(useraddr, &test, sizeof(test)))
		goto out;
	useraddr += sizeof(test);
	if (copy_to_user(useraddr, data, test.len * sizeof(u64)))
		goto out;
	ret = 0;

 out:
	kfree(data);
	return ret;
}

static int ethtool_get_strings(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_gstrings gstrings;
	struct ethtool_ops *ops = dev->ethtool_ops;
	u8 *data;
	int ret;

	if (!ops->get_strings)
		return -EOPNOTSUPP;

	if (copy_from_user(&gstrings, useraddr, sizeof(gstrings)))
		return -EFAULT;

	switch (gstrings.string_set) {
	case ETH_SS_TEST:
		if (!ops->self_test_count)
			return -EOPNOTSUPP;
		gstrings.len = ops->self_test_count(dev);
		break;
	case ETH_SS_STATS:
		if (!ops->get_stats_count)
			return -EOPNOTSUPP;
		gstrings.len = ops->get_stats_count(dev);
		break;
	default:
		return -EINVAL;
	}

	data = kmalloc(gstrings.len * ETH_GSTRING_LEN, GFP_USER);
	if (!data)
		return -ENOMEM;

	ops->get_strings(dev, gstrings.string_set, data);

	ret = -EFAULT;
	if (copy_to_user(useraddr, &gstrings, sizeof(gstrings)))
		goto out;
	useraddr += sizeof(gstrings);
	if (copy_to_user(useraddr, data, gstrings.len * ETH_GSTRING_LEN))
		goto out;
	ret = 0;

 out:
	kfree(data);
	return ret;
}

static int ethtool_phys_id(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_value id;

	if (!dev->ethtool_ops->phys_id)
		return -EOPNOTSUPP;

	if (copy_from_user(&id, useraddr, sizeof(id)))
		return -EFAULT;

	return dev->ethtool_ops->phys_id(dev, id.data);
}

static int ethtool_get_stats(struct net_device *dev, void __user *useraddr)
{
	struct ethtool_stats stats;
	struct ethtool_ops *ops = dev->ethtool_ops;
	u64 *data;
	int ret;

	if (!ops->get_ethtool_stats || !ops->get_stats_count)
		return -EOPNOTSUPP;

	if (copy_from_user(&stats, useraddr, sizeof(stats)))
		return -EFAULT;

	stats.n_stats = ops->get_stats_count(dev);
	data = kmalloc(stats.n_stats * sizeof(u64), GFP_USER);
	if (!data)
		return -ENOMEM;

	ops->get_ethtool_stats(dev, &stats, data);

	ret = -EFAULT;
	if (copy_to_user(useraddr, &stats, sizeof(stats)))
		goto out;
	useraddr += sizeof(stats);
	if (copy_to_user(useraddr, data, stats.n_stats * sizeof(u64)))
		goto out;
	ret = 0;

 out:
	kfree(data);
	return ret;
}

/* The main entry point in this file.  Called from net/core/dev.c */

int dev_ethtool(struct ifreq *ifr)
{
	struct net_device *dev = __dev_get_by_name(ifr->ifr_name);
	void __user *useraddr = ifr->ifr_data;
	u32 ethcmd;
	int rc;

	/*
	 * XXX: This can be pushed down into the ethtool_* handlers that
	 * need it.  Keep existing behaviour for the moment.
	 */
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (!dev || !netif_device_present(dev))
		return -ENODEV;

	if (!dev->ethtool_ops)
		goto ioctl;

	if (copy_from_user(&ethcmd, useraddr, sizeof (ethcmd)))
		return -EFAULT;

	if(dev->ethtool_ops->begin)
		if ((rc = dev->ethtool_ops->begin(dev)) < 0)
			return rc;

	switch (ethcmd) {
	case ETHTOOL_GSET:
		rc = ethtool_get_settings(dev, useraddr);
		break;
	case ETHTOOL_SSET:
		rc = ethtool_set_settings(dev, useraddr);
		break;
	case ETHTOOL_GDRVINFO:
		rc = ethtool_get_drvinfo(dev, useraddr);

		break;
	case ETHTOOL_GREGS:
		rc = ethtool_get_regs(dev, useraddr);
		break;
	case ETHTOOL_GWOL:
		rc = ethtool_get_wol(dev, useraddr);
		break;
	case ETHTOOL_SWOL:
		rc = ethtool_set_wol(dev, useraddr);
		break;
	case ETHTOOL_GMSGLVL:
		rc = ethtool_get_msglevel(dev, useraddr);
		break;
	case ETHTOOL_SMSGLVL:
		rc = ethtool_set_msglevel(dev, useraddr);
		break;
	case ETHTOOL_NWAY_RST:
		rc = ethtool_nway_reset(dev);
		break;
	case ETHTOOL_GLINK:
		rc = ethtool_get_link(dev, useraddr);
		break;
	case ETHTOOL_GEEPROM:
		rc = ethtool_get_eeprom(dev, useraddr);
		break;
	case ETHTOOL_SEEPROM:
		rc = ethtool_set_eeprom(dev, useraddr);
		break;
	case ETHTOOL_GCOALESCE:
		rc = ethtool_get_coalesce(dev, useraddr);
		break;
	case ETHTOOL_SCOALESCE:
		rc = ethtool_set_coalesce(dev, useraddr);
		break;
	case ETHTOOL_GRINGPARAM:
		rc = ethtool_get_ringparam(dev, useraddr);
		break;
	case ETHTOOL_SRINGPARAM:
		rc = ethtool_set_ringparam(dev, useraddr);
		break;
	case ETHTOOL_GPAUSEPARAM:
		rc = ethtool_get_pauseparam(dev, useraddr);
		break;
	case ETHTOOL_SPAUSEPARAM:
		rc = ethtool_set_pauseparam(dev, useraddr);
		break;
	case ETHTOOL_GRXCSUM:
		rc = ethtool_get_rx_csum(dev, useraddr);
		break;
	case ETHTOOL_SRXCSUM:
		rc = ethtool_set_rx_csum(dev, useraddr);
		break;
	case ETHTOOL_GTXCSUM:
		rc = ethtool_get_tx_csum(dev, useraddr);
		break;
	case ETHTOOL_STXCSUM:
		rc = ethtool_set_tx_csum(dev, useraddr);
		break;
	case ETHTOOL_GSG:
		rc = ethtool_get_sg(dev, useraddr);
		break;
	case ETHTOOL_SSG:
		rc = ethtool_set_sg(dev, useraddr);
		break;
	case ETHTOOL_GTSO:
		rc = ethtool_get_tso(dev, useraddr);
		break;
	case ETHTOOL_STSO:
		rc = ethtool_set_tso(dev, useraddr);
		break;
	case ETHTOOL_TEST:
		rc = ethtool_self_test(dev, useraddr);
		break;
	case ETHTOOL_GSTRINGS:
		rc = ethtool_get_strings(dev, useraddr);
		break;
	case ETHTOOL_PHYS_ID:
		rc = ethtool_phys_id(dev, useraddr);
		break;
	case ETHTOOL_GSTATS:
		rc = ethtool_get_stats(dev, useraddr);
		break;
	default:
		rc =  -EOPNOTSUPP;
	}
	
	if(dev->ethtool_ops->complete)
		dev->ethtool_ops->complete(dev);
	return rc;

 ioctl:
	if (dev->do_ioctl)
		return dev->do_ioctl(dev, ifr, SIOCETHTOOL);
	return -EOPNOTSUPP;
}

EXPORT_SYMBOL(dev_ethtool);
EXPORT_SYMBOL(ethtool_op_get_link);
EXPORT_SYMBOL(ethtool_op_get_sg);
EXPORT_SYMBOL(ethtool_op_get_tso);
EXPORT_SYMBOL(ethtool_op_get_tx_csum);
EXPORT_SYMBOL(ethtool_op_set_sg);
EXPORT_SYMBOL(ethtool_op_set_tso);
EXPORT_SYMBOL(ethtool_op_set_tx_csum);
