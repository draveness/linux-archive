/*
 * Software WEP encryption implementation
 * Copyright 2002, Jouni Malinen <jkmaline@cc.hut.fi>
 * Copyright 2003, Instant802 Networks, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/random.h>
#include <linux/compiler.h>
#include <linux/crc32.h>
#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <asm/scatterlist.h>

#include <net/mac80211.h>
#include "ieee80211_i.h"
#include "wep.h"


int ieee80211_wep_init(struct ieee80211_local *local)
{
	/* start WEP IV from a random value */
	get_random_bytes(&local->wep_iv, WEP_IV_LEN);

	local->wep_tx_tfm = crypto_alloc_blkcipher("ecb(arc4)", 0,
						CRYPTO_ALG_ASYNC);
	if (IS_ERR(local->wep_tx_tfm))
		return -ENOMEM;

	local->wep_rx_tfm = crypto_alloc_blkcipher("ecb(arc4)", 0,
						CRYPTO_ALG_ASYNC);
	if (IS_ERR(local->wep_rx_tfm)) {
		crypto_free_blkcipher(local->wep_tx_tfm);
		return -ENOMEM;
	}

	return 0;
}

void ieee80211_wep_free(struct ieee80211_local *local)
{
	crypto_free_blkcipher(local->wep_tx_tfm);
	crypto_free_blkcipher(local->wep_rx_tfm);
}

static inline int ieee80211_wep_weak_iv(u32 iv, int keylen)
{
	/* Fluhrer, Mantin, and Shamir have reported weaknesses in the
	 * key scheduling algorithm of RC4. At least IVs (KeyByte + 3,
	 * 0xff, N) can be used to speedup attacks, so avoid using them. */
	if ((iv & 0xff00) == 0xff00) {
		u8 B = (iv >> 16) & 0xff;
		if (B >= 3 && B < 3 + keylen)
			return 1;
	}
	return 0;
}


void ieee80211_wep_get_iv(struct ieee80211_local *local,
			  struct ieee80211_key *key, u8 *iv)
{
	local->wep_iv++;
	if (ieee80211_wep_weak_iv(local->wep_iv, key->keylen))
		local->wep_iv += 0x0100;

	if (!iv)
		return;

	*iv++ = (local->wep_iv >> 16) & 0xff;
	*iv++ = (local->wep_iv >> 8) & 0xff;
	*iv++ = local->wep_iv & 0xff;
	*iv++ = key->keyidx << 6;
}


u8 * ieee80211_wep_add_iv(struct ieee80211_local *local,
			  struct sk_buff *skb,
			  struct ieee80211_key *key)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	u16 fc;
	int hdrlen;
	u8 *newhdr;

	fc = le16_to_cpu(hdr->frame_control);
	fc |= IEEE80211_FCTL_PROTECTED;
	hdr->frame_control = cpu_to_le16(fc);

	if ((skb_headroom(skb) < WEP_IV_LEN ||
	     skb_tailroom(skb) < WEP_ICV_LEN)) {
		I802_DEBUG_INC(local->tx_expand_skb_head);
		if (unlikely(pskb_expand_head(skb, WEP_IV_LEN, WEP_ICV_LEN,
					      GFP_ATOMIC)))
			return NULL;
	}

	hdrlen = ieee80211_get_hdrlen(fc);
	newhdr = skb_push(skb, WEP_IV_LEN);
	memmove(newhdr, newhdr + WEP_IV_LEN, hdrlen);
	ieee80211_wep_get_iv(local, key, newhdr + hdrlen);
	return newhdr + hdrlen;
}


void ieee80211_wep_remove_iv(struct ieee80211_local *local,
			     struct sk_buff *skb,
			     struct ieee80211_key *key)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	u16 fc;
	int hdrlen;

	fc = le16_to_cpu(hdr->frame_control);
	hdrlen = ieee80211_get_hdrlen(fc);
	memmove(skb->data + WEP_IV_LEN, skb->data, hdrlen);
	skb_pull(skb, WEP_IV_LEN);
}


/* Perform WEP encryption using given key. data buffer must have tailroom
 * for 4-byte ICV. data_len must not include this ICV. Note: this function
 * does _not_ add IV. data = RC4(data | CRC32(data)) */
void ieee80211_wep_encrypt_data(struct crypto_blkcipher *tfm, u8 *rc4key,
				size_t klen, u8 *data, size_t data_len)
{
	struct blkcipher_desc desc = { .tfm = tfm };
	struct scatterlist sg;
	__le32 *icv;

	icv = (__le32 *)(data + data_len);
	*icv = cpu_to_le32(~crc32_le(~0, data, data_len));

	crypto_blkcipher_setkey(tfm, rc4key, klen);
	sg.page = virt_to_page(data);
	sg.offset = offset_in_page(data);
	sg.length = data_len + WEP_ICV_LEN;
	crypto_blkcipher_encrypt(&desc, &sg, &sg, sg.length);
}


/* Perform WEP encryption on given skb. 4 bytes of extra space (IV) in the
 * beginning of the buffer 4 bytes of extra space (ICV) in the end of the
 * buffer will be added. Both IV and ICV will be transmitted, so the
 * payload length increases with 8 bytes.
 *
 * WEP frame payload: IV + TX key idx, RC4(data), ICV = RC4(CRC32(data))
 */
int ieee80211_wep_encrypt(struct ieee80211_local *local, struct sk_buff *skb,
			  struct ieee80211_key *key)
{
	u32 klen;
	u8 *rc4key, *iv;
	size_t len;

	if (!key || key->alg != ALG_WEP)
		return -1;

	klen = 3 + key->keylen;
	rc4key = kmalloc(klen, GFP_ATOMIC);
	if (!rc4key)
		return -1;

	iv = ieee80211_wep_add_iv(local, skb, key);
	if (!iv) {
		kfree(rc4key);
		return -1;
	}

	len = skb->len - (iv + WEP_IV_LEN - skb->data);

	/* Prepend 24-bit IV to RC4 key */
	memcpy(rc4key, iv, 3);

	/* Copy rest of the WEP key (the secret part) */
	memcpy(rc4key + 3, key->key, key->keylen);

	/* Add room for ICV */
	skb_put(skb, WEP_ICV_LEN);

	ieee80211_wep_encrypt_data(local->wep_tx_tfm, rc4key, klen,
				   iv + WEP_IV_LEN, len);

	kfree(rc4key);

	return 0;
}


/* Perform WEP decryption using given key. data buffer includes encrypted
 * payload, including 4-byte ICV, but _not_ IV. data_len must not include ICV.
 * Return 0 on success and -1 on ICV mismatch. */
int ieee80211_wep_decrypt_data(struct crypto_blkcipher *tfm, u8 *rc4key,
			       size_t klen, u8 *data, size_t data_len)
{
	struct blkcipher_desc desc = { .tfm = tfm };
	struct scatterlist sg;
	__le32 crc;

	crypto_blkcipher_setkey(tfm, rc4key, klen);
	sg.page = virt_to_page(data);
	sg.offset = offset_in_page(data);
	sg.length = data_len + WEP_ICV_LEN;
	crypto_blkcipher_decrypt(&desc, &sg, &sg, sg.length);

	crc = cpu_to_le32(~crc32_le(~0, data, data_len));
	if (memcmp(&crc, data + data_len, WEP_ICV_LEN) != 0)
		/* ICV mismatch */
		return -1;

	return 0;
}


/* Perform WEP decryption on given skb. Buffer includes whole WEP part of
 * the frame: IV (4 bytes), encrypted payload (including SNAP header),
 * ICV (4 bytes). skb->len includes both IV and ICV.
 *
 * Returns 0 if frame was decrypted successfully and ICV was correct and -1 on
 * failure. If frame is OK, IV and ICV will be removed, i.e., decrypted payload
 * is moved to the beginning of the skb and skb length will be reduced.
 */
int ieee80211_wep_decrypt(struct ieee80211_local *local, struct sk_buff *skb,
			  struct ieee80211_key *key)
{
	u32 klen;
	u8 *rc4key;
	u8 keyidx;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	u16 fc;
	int hdrlen;
	size_t len;
	int ret = 0;

	fc = le16_to_cpu(hdr->frame_control);
	if (!(fc & IEEE80211_FCTL_PROTECTED))
		return -1;

	hdrlen = ieee80211_get_hdrlen(fc);

	if (skb->len < 8 + hdrlen)
		return -1;

	len = skb->len - hdrlen - 8;

	keyidx = skb->data[hdrlen + 3] >> 6;

	if (!key || keyidx != key->keyidx || key->alg != ALG_WEP)
		return -1;

	klen = 3 + key->keylen;

	rc4key = kmalloc(klen, GFP_ATOMIC);
	if (!rc4key)
		return -1;

	/* Prepend 24-bit IV to RC4 key */
	memcpy(rc4key, skb->data + hdrlen, 3);

	/* Copy rest of the WEP key (the secret part) */
	memcpy(rc4key + 3, key->key, key->keylen);

	if (ieee80211_wep_decrypt_data(local->wep_rx_tfm, rc4key, klen,
				       skb->data + hdrlen + WEP_IV_LEN,
				       len)) {
		printk(KERN_DEBUG "WEP decrypt failed (ICV)\n");
		ret = -1;
	}

	kfree(rc4key);

	/* Trim ICV */
	skb_trim(skb, skb->len - WEP_ICV_LEN);

	/* Remove IV */
	memmove(skb->data + WEP_IV_LEN, skb->data, hdrlen);
	skb_pull(skb, WEP_IV_LEN);

	return ret;
}


int ieee80211_wep_get_keyidx(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	u16 fc;
	int hdrlen;

	fc = le16_to_cpu(hdr->frame_control);
	if (!(fc & IEEE80211_FCTL_PROTECTED))
		return -1;

	hdrlen = ieee80211_get_hdrlen(fc);

	if (skb->len < 8 + hdrlen)
		return -1;

	return skb->data[hdrlen + 3] >> 6;
}


u8 * ieee80211_wep_is_weak_iv(struct sk_buff *skb, struct ieee80211_key *key)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	u16 fc;
	int hdrlen;
	u8 *ivpos;
	u32 iv;

	fc = le16_to_cpu(hdr->frame_control);
	if (!(fc & IEEE80211_FCTL_PROTECTED))
		return NULL;

	hdrlen = ieee80211_get_hdrlen(fc);
	ivpos = skb->data + hdrlen;
	iv = (ivpos[0] << 16) | (ivpos[1] << 8) | ivpos[2];

	if (ieee80211_wep_weak_iv(iv, key->keylen))
		return ivpos;

	return NULL;
}
