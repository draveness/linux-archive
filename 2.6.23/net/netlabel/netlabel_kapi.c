/*
 * NetLabel Kernel API
 *
 * This file defines the kernel API for the NetLabel system.  The NetLabel
 * system manages static and dynamic label mappings for network protocols such
 * as CIPSO and RIPSO.
 *
 * Author: Paul Moore <paul.moore@hp.com>
 *
 */

/*
 * (c) Copyright Hewlett-Packard Development Company, L.P., 2006
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY;  without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program;  if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/init.h>
#include <linux/types.h>
#include <net/ip.h>
#include <net/netlabel.h>
#include <net/cipso_ipv4.h>
#include <asm/bug.h>

#include "netlabel_domainhash.h"
#include "netlabel_unlabeled.h"
#include "netlabel_user.h"
#include "netlabel_mgmt.h"

/*
 * Security Attribute Functions
 */

/**
 * netlbl_secattr_catmap_walk - Walk a LSM secattr catmap looking for a bit
 * @catmap: the category bitmap
 * @offset: the offset to start searching at, in bits
 *
 * Description:
 * This function walks a LSM secattr category bitmap starting at @offset and
 * returns the spot of the first set bit or -ENOENT if no bits are set.
 *
 */
int netlbl_secattr_catmap_walk(struct netlbl_lsm_secattr_catmap *catmap,
			       u32 offset)
{
	struct netlbl_lsm_secattr_catmap *iter = catmap;
	u32 node_idx;
	u32 node_bit;
	NETLBL_CATMAP_MAPTYPE bitmap;

	if (offset > iter->startbit) {
		while (offset >= (iter->startbit + NETLBL_CATMAP_SIZE)) {
			iter = iter->next;
			if (iter == NULL)
				return -ENOENT;
		}
		node_idx = (offset - iter->startbit) / NETLBL_CATMAP_MAPSIZE;
		node_bit = offset - iter->startbit -
			   (NETLBL_CATMAP_MAPSIZE * node_idx);
	} else {
		node_idx = 0;
		node_bit = 0;
	}
	bitmap = iter->bitmap[node_idx] >> node_bit;

	for (;;) {
		if (bitmap != 0) {
			while ((bitmap & NETLBL_CATMAP_BIT) == 0) {
				bitmap >>= 1;
				node_bit++;
			}
			return iter->startbit +
				(NETLBL_CATMAP_MAPSIZE * node_idx) + node_bit;
		}
		if (++node_idx >= NETLBL_CATMAP_MAPCNT) {
			if (iter->next != NULL) {
				iter = iter->next;
				node_idx = 0;
			} else
				return -ENOENT;
		}
		bitmap = iter->bitmap[node_idx];
		node_bit = 0;
	}

	return -ENOENT;
}

/**
 * netlbl_secattr_catmap_walk_rng - Find the end of a string of set bits
 * @catmap: the category bitmap
 * @offset: the offset to start searching at, in bits
 *
 * Description:
 * This function walks a LSM secattr category bitmap starting at @offset and
 * returns the spot of the first cleared bit or -ENOENT if the offset is past
 * the end of the bitmap.
 *
 */
int netlbl_secattr_catmap_walk_rng(struct netlbl_lsm_secattr_catmap *catmap,
				   u32 offset)
{
	struct netlbl_lsm_secattr_catmap *iter = catmap;
	u32 node_idx;
	u32 node_bit;
	NETLBL_CATMAP_MAPTYPE bitmask;
	NETLBL_CATMAP_MAPTYPE bitmap;

	if (offset > iter->startbit) {
		while (offset >= (iter->startbit + NETLBL_CATMAP_SIZE)) {
			iter = iter->next;
			if (iter == NULL)
				return -ENOENT;
		}
		node_idx = (offset - iter->startbit) / NETLBL_CATMAP_MAPSIZE;
		node_bit = offset - iter->startbit -
			   (NETLBL_CATMAP_MAPSIZE * node_idx);
	} else {
		node_idx = 0;
		node_bit = 0;
	}
	bitmask = NETLBL_CATMAP_BIT << node_bit;

	for (;;) {
		bitmap = iter->bitmap[node_idx];
		while (bitmask != 0 && (bitmap & bitmask) != 0) {
			bitmask <<= 1;
			node_bit++;
		}

		if (bitmask != 0)
			return iter->startbit +
				(NETLBL_CATMAP_MAPSIZE * node_idx) +
				node_bit - 1;
		else if (++node_idx >= NETLBL_CATMAP_MAPCNT) {
			if (iter->next == NULL)
				return iter->startbit +	NETLBL_CATMAP_SIZE - 1;
			iter = iter->next;
			node_idx = 0;
		}
		bitmask = NETLBL_CATMAP_BIT;
		node_bit = 0;
	}

	return -ENOENT;
}

/**
 * netlbl_secattr_catmap_setbit - Set a bit in a LSM secattr catmap
 * @catmap: the category bitmap
 * @bit: the bit to set
 * @flags: memory allocation flags
 *
 * Description:
 * Set the bit specified by @bit in @catmap.  Returns zero on success,
 * negative values on failure.
 *
 */
int netlbl_secattr_catmap_setbit(struct netlbl_lsm_secattr_catmap *catmap,
				 u32 bit,
				 gfp_t flags)
{
	struct netlbl_lsm_secattr_catmap *iter = catmap;
	u32 node_bit;
	u32 node_idx;

	while (iter->next != NULL &&
	       bit >= (iter->startbit + NETLBL_CATMAP_SIZE))
		iter = iter->next;
	if (bit >= (iter->startbit + NETLBL_CATMAP_SIZE)) {
		iter->next = netlbl_secattr_catmap_alloc(flags);
		if (iter->next == NULL)
			return -ENOMEM;
		iter = iter->next;
		iter->startbit = bit & ~(NETLBL_CATMAP_SIZE - 1);
	}

	/* gcc always rounds to zero when doing integer division */
	node_idx = (bit - iter->startbit) / NETLBL_CATMAP_MAPSIZE;
	node_bit = bit - iter->startbit - (NETLBL_CATMAP_MAPSIZE * node_idx);
	iter->bitmap[node_idx] |= NETLBL_CATMAP_BIT << node_bit;

	return 0;
}

/**
 * netlbl_secattr_catmap_setrng - Set a range of bits in a LSM secattr catmap
 * @catmap: the category bitmap
 * @start: the starting bit
 * @end: the last bit in the string
 * @flags: memory allocation flags
 *
 * Description:
 * Set a range of bits, starting at @start and ending with @end.  Returns zero
 * on success, negative values on failure.
 *
 */
int netlbl_secattr_catmap_setrng(struct netlbl_lsm_secattr_catmap *catmap,
				 u32 start,
				 u32 end,
				 gfp_t flags)
{
	int ret_val = 0;
	struct netlbl_lsm_secattr_catmap *iter = catmap;
	u32 iter_max_spot;
	u32 spot;

	/* XXX - This could probably be made a bit faster by combining writes
	 * to the catmap instead of setting a single bit each time, but for
	 * right now skipping to the start of the range in the catmap should
	 * be a nice improvement over calling the individual setbit function
	 * repeatedly from a loop. */

	while (iter->next != NULL &&
	       start >= (iter->startbit + NETLBL_CATMAP_SIZE))
		iter = iter->next;
	iter_max_spot = iter->startbit + NETLBL_CATMAP_SIZE;

	for (spot = start; spot <= end && ret_val == 0; spot++) {
		if (spot >= iter_max_spot && iter->next != NULL) {
			iter = iter->next;
			iter_max_spot = iter->startbit + NETLBL_CATMAP_SIZE;
		}
		ret_val = netlbl_secattr_catmap_setbit(iter, spot, GFP_ATOMIC);
	}

	return ret_val;
}

/*
 * LSM Functions
 */

/**
 * netlbl_enabled - Determine if the NetLabel subsystem is enabled
 *
 * Description:
 * The LSM can use this function to determine if it should use NetLabel
 * security attributes in it's enforcement mechanism.  Currently, NetLabel is
 * considered to be enabled when it's configuration contains a valid setup for
 * at least one labeled protocol (i.e. NetLabel can understand incoming
 * labeled packets of at least one type); otherwise NetLabel is considered to
 * be disabled.
 *
 */
int netlbl_enabled(void)
{
	/* At some point we probably want to expose this mechanism to the user
	 * as well so that admins can toggle NetLabel regardless of the
	 * configuration */
	return (netlbl_mgmt_protocount_value() > 0 ? 1 : 0);
}

/**
 * netlbl_socket_setattr - Label a socket using the correct protocol
 * @sk: the socket to label
 * @secattr: the security attributes
 *
 * Description:
 * Attach the correct label to the given socket using the security attributes
 * specified in @secattr.  This function requires exclusive access to @sk,
 * which means it either needs to be in the process of being created or locked.
 * Returns zero on success, negative values on failure.
 *
 */
int netlbl_sock_setattr(struct sock *sk,
			const struct netlbl_lsm_secattr *secattr)
{
	int ret_val = -ENOENT;
	struct netlbl_dom_map *dom_entry;

	rcu_read_lock();
	dom_entry = netlbl_domhsh_getentry(secattr->domain);
	if (dom_entry == NULL)
		goto socket_setattr_return;
	switch (dom_entry->type) {
	case NETLBL_NLTYPE_CIPSOV4:
		ret_val = cipso_v4_sock_setattr(sk,
						dom_entry->type_def.cipsov4,
						secattr);
		break;
	case NETLBL_NLTYPE_UNLABELED:
		ret_val = 0;
		break;
	default:
		ret_val = -ENOENT;
	}

socket_setattr_return:
	rcu_read_unlock();
	return ret_val;
}

/**
 * netlbl_sock_getattr - Determine the security attributes of a sock
 * @sk: the sock
 * @secattr: the security attributes
 *
 * Description:
 * Examines the given sock to see any NetLabel style labeling has been
 * applied to the sock, if so it parses the socket label and returns the
 * security attributes in @secattr.  Returns zero on success, negative values
 * on failure.
 *
 */
int netlbl_sock_getattr(struct sock *sk, struct netlbl_lsm_secattr *secattr)
{
	int ret_val;

	ret_val = cipso_v4_sock_getattr(sk, secattr);
	if (ret_val == 0)
		return 0;

	return netlbl_unlabel_getattr(secattr);
}

/**
 * netlbl_skbuff_getattr - Determine the security attributes of a packet
 * @skb: the packet
 * @secattr: the security attributes
 *
 * Description:
 * Examines the given packet to see if a recognized form of packet labeling
 * is present, if so it parses the packet label and returns the security
 * attributes in @secattr.  Returns zero on success, negative values on
 * failure.
 *
 */
int netlbl_skbuff_getattr(const struct sk_buff *skb,
			  struct netlbl_lsm_secattr *secattr)
{
	if (CIPSO_V4_OPTEXIST(skb) &&
	    cipso_v4_skbuff_getattr(skb, secattr) == 0)
		return 0;

	return netlbl_unlabel_getattr(secattr);
}

/**
 * netlbl_skbuff_err - Handle a LSM error on a sk_buff
 * @skb: the packet
 * @error: the error code
 *
 * Description:
 * Deal with a LSM problem when handling the packet in @skb, typically this is
 * a permission denied problem (-EACCES).  The correct action is determined
 * according to the packet's labeling protocol.
 *
 */
void netlbl_skbuff_err(struct sk_buff *skb, int error)
{
	if (CIPSO_V4_OPTEXIST(skb))
		cipso_v4_error(skb, error, 0);
}

/**
 * netlbl_cache_invalidate - Invalidate all of the NetLabel protocol caches
 *
 * Description:
 * For all of the NetLabel protocols that support some form of label mapping
 * cache, invalidate the cache.  Returns zero on success, negative values on
 * error.
 *
 */
void netlbl_cache_invalidate(void)
{
	cipso_v4_cache_invalidate();
}

/**
 * netlbl_cache_add - Add an entry to a NetLabel protocol cache
 * @skb: the packet
 * @secattr: the packet's security attributes
 *
 * Description:
 * Add the LSM security attributes for the given packet to the underlying
 * NetLabel protocol's label mapping cache.  Returns zero on success, negative
 * values on error.
 *
 */
int netlbl_cache_add(const struct sk_buff *skb,
		     const struct netlbl_lsm_secattr *secattr)
{
	if ((secattr->flags & NETLBL_SECATTR_CACHE) == 0)
		return -ENOMSG;

	if (CIPSO_V4_OPTEXIST(skb))
		return cipso_v4_cache_add(skb, secattr);

	return -ENOMSG;
}

/*
 * Setup Functions
 */

/**
 * netlbl_init - Initialize NetLabel
 *
 * Description:
 * Perform the required NetLabel initialization before first use.
 *
 */
static int __init netlbl_init(void)
{
	int ret_val;

	printk(KERN_INFO "NetLabel: Initializing\n");
	printk(KERN_INFO "NetLabel:  domain hash size = %u\n",
	       (1 << NETLBL_DOMHSH_BITSIZE));
	printk(KERN_INFO "NetLabel:  protocols ="
	       " UNLABELED"
	       " CIPSOv4"
	       "\n");

	ret_val = netlbl_domhsh_init(NETLBL_DOMHSH_BITSIZE);
	if (ret_val != 0)
		goto init_failure;

	ret_val = netlbl_netlink_init();
	if (ret_val != 0)
		goto init_failure;

	ret_val = netlbl_unlabel_defconf();
	if (ret_val != 0)
		goto init_failure;
	printk(KERN_INFO "NetLabel:  unlabeled traffic allowed by default\n");

	return 0;

init_failure:
	panic("NetLabel: failed to initialize properly (%d)\n", ret_val);
}

subsys_initcall(netlbl_init);
