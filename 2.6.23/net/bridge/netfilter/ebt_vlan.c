/*
 * Description: EBTables 802.1Q match extension kernelspace module.
 * Authors: Nick Fedchik <nick@fedchik.org.ua>
 *          Bart De Schuymer <bdschuym@pandora.be>
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

#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netfilter_bridge/ebtables.h>
#include <linux/netfilter_bridge/ebt_vlan.h>

static int debug;
#define MODULE_VERS "0.6"

module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "debug=1 is turn on debug messages");
MODULE_AUTHOR("Nick Fedchik <nick@fedchik.org.ua>");
MODULE_DESCRIPTION("802.1Q match module (ebtables extension), v"
		   MODULE_VERS);
MODULE_LICENSE("GPL");


#define DEBUG_MSG(args...) if (debug) printk (KERN_DEBUG "ebt_vlan: " args)
#define INV_FLAG(_inv_flag_) (info->invflags & _inv_flag_) ? "!" : ""
#define GET_BITMASK(_BIT_MASK_) info->bitmask & _BIT_MASK_
#define SET_BITMASK(_BIT_MASK_) info->bitmask |= _BIT_MASK_
#define EXIT_ON_MISMATCH(_MATCH_,_MASK_) {if (!((info->_MATCH_ == _MATCH_)^!!(info->invflags & _MASK_))) return EBT_NOMATCH;}

static int
ebt_filter_vlan(const struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		const void *data, unsigned int datalen)
{
	struct ebt_vlan_info *info = (struct ebt_vlan_info *) data;
	struct vlan_hdr _frame, *fp;

	unsigned short TCI;	/* Whole TCI, given from parsed frame */
	unsigned short id;	/* VLAN ID, given from frame TCI */
	unsigned char prio;	/* user_priority, given from frame TCI */
	/* VLAN encapsulated Type/Length field, given from orig frame */
	__be16 encap;

	fp = skb_header_pointer(skb, 0, sizeof(_frame), &_frame);
	if (fp == NULL)
		return EBT_NOMATCH;

	/* Tag Control Information (TCI) consists of the following elements:
	 * - User_priority. The user_priority field is three bits in length,
	 * interpreted as a binary number.
	 * - Canonical Format Indicator (CFI). The Canonical Format Indicator
	 * (CFI) is a single bit flag value. Currently ignored.
	 * - VLAN Identifier (VID). The VID is encoded as
	 * an unsigned binary number. */
	TCI = ntohs(fp->h_vlan_TCI);
	id = TCI & VLAN_VID_MASK;
	prio = (TCI >> 13) & 0x7;
	encap = fp->h_vlan_encapsulated_proto;

	/* Checking VLAN Identifier (VID) */
	if (GET_BITMASK(EBT_VLAN_ID))
		EXIT_ON_MISMATCH(id, EBT_VLAN_ID);

	/* Checking user_priority */
	if (GET_BITMASK(EBT_VLAN_PRIO))
		EXIT_ON_MISMATCH(prio, EBT_VLAN_PRIO);

	/* Checking Encapsulated Proto (Length/Type) field */
	if (GET_BITMASK(EBT_VLAN_ENCAP))
		EXIT_ON_MISMATCH(encap, EBT_VLAN_ENCAP);

	return EBT_MATCH;
}

static int
ebt_check_vlan(const char *tablename,
	       unsigned int hooknr,
	       const struct ebt_entry *e, void *data, unsigned int datalen)
{
	struct ebt_vlan_info *info = (struct ebt_vlan_info *) data;

	/* Parameters buffer overflow check */
	if (datalen != EBT_ALIGN(sizeof(struct ebt_vlan_info))) {
		DEBUG_MSG
		    ("passed size %d is not eq to ebt_vlan_info (%Zd)\n",
		     datalen, sizeof(struct ebt_vlan_info));
		return -EINVAL;
	}

	/* Is it 802.1Q frame checked? */
	if (e->ethproto != htons(ETH_P_8021Q)) {
		DEBUG_MSG
		    ("passed entry proto %2.4X is not 802.1Q (8100)\n",
		     (unsigned short) ntohs(e->ethproto));
		return -EINVAL;
	}

	/* Check for bitmask range
	 * True if even one bit is out of mask */
	if (info->bitmask & ~EBT_VLAN_MASK) {
		DEBUG_MSG("bitmask %2X is out of mask (%2X)\n",
			  info->bitmask, EBT_VLAN_MASK);
		return -EINVAL;
	}

	/* Check for inversion flags range */
	if (info->invflags & ~EBT_VLAN_MASK) {
		DEBUG_MSG("inversion flags %2X is out of mask (%2X)\n",
			  info->invflags, EBT_VLAN_MASK);
		return -EINVAL;
	}

	/* Reserved VLAN ID (VID) values
	 * -----------------------------
	 * 0 - The null VLAN ID.
	 * 1 - The default Port VID (PVID)
	 * 0x0FFF - Reserved for implementation use.
	 * if_vlan.h: VLAN_GROUP_ARRAY_LEN 4096. */
	if (GET_BITMASK(EBT_VLAN_ID)) {
		if (!!info->id) { /* if id!=0 => check vid range */
			if (info->id > VLAN_GROUP_ARRAY_LEN) {
				DEBUG_MSG
				    ("id %d is out of range (1-4096)\n",
				     info->id);
				return -EINVAL;
			}
			/* Note: This is valid VLAN-tagged frame point.
			 * Any value of user_priority are acceptable,
			 * but should be ignored according to 802.1Q Std.
			 * So we just drop the prio flag. */
			info->bitmask &= ~EBT_VLAN_PRIO;
		}
		/* Else, id=0 (null VLAN ID)  => user_priority range (any?) */
	}

	if (GET_BITMASK(EBT_VLAN_PRIO)) {
		if ((unsigned char) info->prio > 7) {
			DEBUG_MSG("prio %d is out of range (0-7)\n",
			     info->prio);
			return -EINVAL;
		}
	}
	/* Check for encapsulated proto range - it is possible to be
	 * any value for u_short range.
	 * if_ether.h:  ETH_ZLEN        60   -  Min. octets in frame sans FCS */
	if (GET_BITMASK(EBT_VLAN_ENCAP)) {
		if ((unsigned short) ntohs(info->encap) < ETH_ZLEN) {
			DEBUG_MSG
			    ("encap frame length %d is less than minimal\n",
			     ntohs(info->encap));
			return -EINVAL;
		}
	}

	return 0;
}

static struct ebt_match filter_vlan = {
	.name		= EBT_VLAN_MATCH,
	.match		= ebt_filter_vlan,
	.check		= ebt_check_vlan,
	.me		= THIS_MODULE,
};

static int __init ebt_vlan_init(void)
{
	DEBUG_MSG("ebtables 802.1Q extension module v"
		  MODULE_VERS "\n");
	DEBUG_MSG("module debug=%d\n", !!debug);
	return ebt_register_match(&filter_vlan);
}

static void __exit ebt_vlan_fini(void)
{
	ebt_unregister_match(&filter_vlan);
}

module_init(ebt_vlan_init);
module_exit(ebt_vlan_fini);
