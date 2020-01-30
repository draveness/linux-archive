/*
 * Copyright 2002-2004, Instant802 Networks, Inc.
 * Copyright 2005, Devicescape Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/netdevice.h>

#include <net/mac80211.h>
#include "ieee80211_key.h"
#include "tkip.h"
#include "wep.h"


/* TKIP key mixing functions */


#define PHASE1_LOOP_COUNT 8


/* 2-byte by 2-byte subset of the full AES S-box table; second part of this
 * table is identical to first part but byte-swapped */
static const u16 tkip_sbox[256] =
{
	0xC6A5, 0xF884, 0xEE99, 0xF68D, 0xFF0D, 0xD6BD, 0xDEB1, 0x9154,
	0x6050, 0x0203, 0xCEA9, 0x567D, 0xE719, 0xB562, 0x4DE6, 0xEC9A,
	0x8F45, 0x1F9D, 0x8940, 0xFA87, 0xEF15, 0xB2EB, 0x8EC9, 0xFB0B,
	0x41EC, 0xB367, 0x5FFD, 0x45EA, 0x23BF, 0x53F7, 0xE496, 0x9B5B,
	0x75C2, 0xE11C, 0x3DAE, 0x4C6A, 0x6C5A, 0x7E41, 0xF502, 0x834F,
	0x685C, 0x51F4, 0xD134, 0xF908, 0xE293, 0xAB73, 0x6253, 0x2A3F,
	0x080C, 0x9552, 0x4665, 0x9D5E, 0x3028, 0x37A1, 0x0A0F, 0x2FB5,
	0x0E09, 0x2436, 0x1B9B, 0xDF3D, 0xCD26, 0x4E69, 0x7FCD, 0xEA9F,
	0x121B, 0x1D9E, 0x5874, 0x342E, 0x362D, 0xDCB2, 0xB4EE, 0x5BFB,
	0xA4F6, 0x764D, 0xB761, 0x7DCE, 0x527B, 0xDD3E, 0x5E71, 0x1397,
	0xA6F5, 0xB968, 0x0000, 0xC12C, 0x4060, 0xE31F, 0x79C8, 0xB6ED,
	0xD4BE, 0x8D46, 0x67D9, 0x724B, 0x94DE, 0x98D4, 0xB0E8, 0x854A,
	0xBB6B, 0xC52A, 0x4FE5, 0xED16, 0x86C5, 0x9AD7, 0x6655, 0x1194,
	0x8ACF, 0xE910, 0x0406, 0xFE81, 0xA0F0, 0x7844, 0x25BA, 0x4BE3,
	0xA2F3, 0x5DFE, 0x80C0, 0x058A, 0x3FAD, 0x21BC, 0x7048, 0xF104,
	0x63DF, 0x77C1, 0xAF75, 0x4263, 0x2030, 0xE51A, 0xFD0E, 0xBF6D,
	0x814C, 0x1814, 0x2635, 0xC32F, 0xBEE1, 0x35A2, 0x88CC, 0x2E39,
	0x9357, 0x55F2, 0xFC82, 0x7A47, 0xC8AC, 0xBAE7, 0x322B, 0xE695,
	0xC0A0, 0x1998, 0x9ED1, 0xA37F, 0x4466, 0x547E, 0x3BAB, 0x0B83,
	0x8CCA, 0xC729, 0x6BD3, 0x283C, 0xA779, 0xBCE2, 0x161D, 0xAD76,
	0xDB3B, 0x6456, 0x744E, 0x141E, 0x92DB, 0x0C0A, 0x486C, 0xB8E4,
	0x9F5D, 0xBD6E, 0x43EF, 0xC4A6, 0x39A8, 0x31A4, 0xD337, 0xF28B,
	0xD532, 0x8B43, 0x6E59, 0xDAB7, 0x018C, 0xB164, 0x9CD2, 0x49E0,
	0xD8B4, 0xACFA, 0xF307, 0xCF25, 0xCAAF, 0xF48E, 0x47E9, 0x1018,
	0x6FD5, 0xF088, 0x4A6F, 0x5C72, 0x3824, 0x57F1, 0x73C7, 0x9751,
	0xCB23, 0xA17C, 0xE89C, 0x3E21, 0x96DD, 0x61DC, 0x0D86, 0x0F85,
	0xE090, 0x7C42, 0x71C4, 0xCCAA, 0x90D8, 0x0605, 0xF701, 0x1C12,
	0xC2A3, 0x6A5F, 0xAEF9, 0x69D0, 0x1791, 0x9958, 0x3A27, 0x27B9,
	0xD938, 0xEB13, 0x2BB3, 0x2233, 0xD2BB, 0xA970, 0x0789, 0x33A7,
	0x2DB6, 0x3C22, 0x1592, 0xC920, 0x8749, 0xAAFF, 0x5078, 0xA57A,
	0x038F, 0x59F8, 0x0980, 0x1A17, 0x65DA, 0xD731, 0x84C6, 0xD0B8,
	0x82C3, 0x29B0, 0x5A77, 0x1E11, 0x7BCB, 0xA8FC, 0x6DD6, 0x2C3A,
};


static inline u16 Mk16(u8 x, u8 y)
{
	return ((u16) x << 8) | (u16) y;
}


static inline u8 Hi8(u16 v)
{
	return v >> 8;
}


static inline u8 Lo8(u16 v)
{
	return v & 0xff;
}


static inline u16 Hi16(u32 v)
{
	return v >> 16;
}


static inline u16 Lo16(u32 v)
{
	return v & 0xffff;
}


static inline u16 RotR1(u16 v)
{
	return (v >> 1) | ((v & 0x0001) << 15);
}


static inline u16 tkip_S(u16 val)
{
	u16 a = tkip_sbox[Hi8(val)];

	return tkip_sbox[Lo8(val)] ^ Hi8(a) ^ (Lo8(a) << 8);
}



/* P1K := Phase1(TA, TK, TSC)
 * TA = transmitter address (48 bits)
 * TK = dot11DefaultKeyValue or dot11KeyMappingValue (128 bits)
 * TSC = TKIP sequence counter (48 bits, only 32 msb bits used)
 * P1K: 80 bits
 */
static void tkip_mixing_phase1(const u8 *ta, const u8 *tk, u32 tsc_IV32,
			       u16 *p1k)
{
	int i, j;

	p1k[0] = Lo16(tsc_IV32);
	p1k[1] = Hi16(tsc_IV32);
	p1k[2] = Mk16(ta[1], ta[0]);
	p1k[3] = Mk16(ta[3], ta[2]);
	p1k[4] = Mk16(ta[5], ta[4]);

	for (i = 0; i < PHASE1_LOOP_COUNT; i++) {
		j = 2 * (i & 1);
		p1k[0] += tkip_S(p1k[4] ^ Mk16(tk[ 1 + j], tk[ 0 + j]));
		p1k[1] += tkip_S(p1k[0] ^ Mk16(tk[ 5 + j], tk[ 4 + j]));
		p1k[2] += tkip_S(p1k[1] ^ Mk16(tk[ 9 + j], tk[ 8 + j]));
		p1k[3] += tkip_S(p1k[2] ^ Mk16(tk[13 + j], tk[12 + j]));
		p1k[4] += tkip_S(p1k[3] ^ Mk16(tk[ 1 + j], tk[ 0 + j])) + i;
	}
}


static void tkip_mixing_phase2(const u16 *p1k, const u8 *tk, u16 tsc_IV16,
			       u8 *rc4key)
{
	u16 ppk[6];
	int i;

	ppk[0] = p1k[0];
	ppk[1] = p1k[1];
	ppk[2] = p1k[2];
	ppk[3] = p1k[3];
	ppk[4] = p1k[4];
	ppk[5] = p1k[4] + tsc_IV16;

	ppk[0] += tkip_S(ppk[5] ^ Mk16(tk[ 1], tk[ 0]));
	ppk[1] += tkip_S(ppk[0] ^ Mk16(tk[ 3], tk[ 2]));
	ppk[2] += tkip_S(ppk[1] ^ Mk16(tk[ 5], tk[ 4]));
	ppk[3] += tkip_S(ppk[2] ^ Mk16(tk[ 7], tk[ 6]));
	ppk[4] += tkip_S(ppk[3] ^ Mk16(tk[ 9], tk[ 8]));
	ppk[5] += tkip_S(ppk[4] ^ Mk16(tk[11], tk[10]));
	ppk[0] +=  RotR1(ppk[5] ^ Mk16(tk[13], tk[12]));
	ppk[1] +=  RotR1(ppk[0] ^ Mk16(tk[15], tk[14]));
	ppk[2] +=  RotR1(ppk[1]);
	ppk[3] +=  RotR1(ppk[2]);
	ppk[4] +=  RotR1(ppk[3]);
	ppk[5] +=  RotR1(ppk[4]);

	rc4key[0] = Hi8(tsc_IV16);
	rc4key[1] = (Hi8(tsc_IV16) | 0x20) & 0x7f;
	rc4key[2] = Lo8(tsc_IV16);
	rc4key[3] = Lo8((ppk[5] ^ Mk16(tk[1], tk[0])) >> 1);

	for (i = 0; i < 6; i++) {
		rc4key[4 + 2 * i] = Lo8(ppk[i]);
		rc4key[5 + 2 * i] = Hi8(ppk[i]);
	}
}


/* Add TKIP IV and Ext. IV at @pos. @iv0, @iv1, and @iv2 are the first octets
 * of the IV. Returns pointer to the octet following IVs (i.e., beginning of
 * the packet payload). */
u8 * ieee80211_tkip_add_iv(u8 *pos, struct ieee80211_key *key,
			   u8 iv0, u8 iv1, u8 iv2)
{
	*pos++ = iv0;
	*pos++ = iv1;
	*pos++ = iv2;
	*pos++ = (key->keyidx << 6) | (1 << 5) /* Ext IV */;
	*pos++ = key->u.tkip.iv32 & 0xff;
	*pos++ = (key->u.tkip.iv32 >> 8) & 0xff;
	*pos++ = (key->u.tkip.iv32 >> 16) & 0xff;
	*pos++ = (key->u.tkip.iv32 >> 24) & 0xff;
	return pos;
}


void ieee80211_tkip_gen_phase1key(struct ieee80211_key *key, u8 *ta,
				  u16 *phase1key)
{
	tkip_mixing_phase1(ta, &key->key[ALG_TKIP_TEMP_ENCR_KEY],
			   key->u.tkip.iv32, phase1key);
}

void ieee80211_tkip_gen_rc4key(struct ieee80211_key *key, u8 *ta,
			       u8 *rc4key)
{
	/* Calculate per-packet key */
	if (key->u.tkip.iv16 == 0 || !key->u.tkip.tx_initialized) {
		/* IV16 wrapped around - perform TKIP phase 1 */
		tkip_mixing_phase1(ta, &key->key[ALG_TKIP_TEMP_ENCR_KEY],
				   key->u.tkip.iv32, key->u.tkip.p1k);
		key->u.tkip.tx_initialized = 1;
	}

	tkip_mixing_phase2(key->u.tkip.p1k, &key->key[ALG_TKIP_TEMP_ENCR_KEY],
			   key->u.tkip.iv16, rc4key);
}

/* Encrypt packet payload with TKIP using @key. @pos is a pointer to the
 * beginning of the buffer containing payload. This payload must include
 * headroom of eight octets for IV and Ext. IV and taildroom of four octets
 * for ICV. @payload_len is the length of payload (_not_ including extra
 * headroom and tailroom). @ta is the transmitter addresses. */
void ieee80211_tkip_encrypt_data(struct crypto_blkcipher *tfm,
				 struct ieee80211_key *key,
				 u8 *pos, size_t payload_len, u8 *ta)
{
	u8 rc4key[16];

	ieee80211_tkip_gen_rc4key(key, ta, rc4key);
	pos = ieee80211_tkip_add_iv(pos, key, rc4key[0], rc4key[1], rc4key[2]);
	ieee80211_wep_encrypt_data(tfm, rc4key, 16, pos, payload_len);
}


/* Decrypt packet payload with TKIP using @key. @pos is a pointer to the
 * beginning of the buffer containing IEEE 802.11 header payload, i.e.,
 * including IV, Ext. IV, real data, Michael MIC, ICV. @payload_len is the
 * length of payload, including IV, Ext. IV, MIC, ICV.  */
int ieee80211_tkip_decrypt_data(struct crypto_blkcipher *tfm,
				struct ieee80211_key *key,
				u8 *payload, size_t payload_len, u8 *ta,
				int only_iv, int queue)
{
	u32 iv32;
	u32 iv16;
	u8 rc4key[16], keyid, *pos = payload;
	int res;

	if (payload_len < 12)
		return -1;

	iv16 = (pos[0] << 8) | pos[2];
	keyid = pos[3];
	iv32 = pos[4] | (pos[5] << 8) | (pos[6] << 16) | (pos[7] << 24);
	pos += 8;
#ifdef CONFIG_TKIP_DEBUG
	{
		int i;
		printk(KERN_DEBUG "TKIP decrypt: data(len=%zd)", payload_len);
		for (i = 0; i < payload_len; i++)
			printk(" %02x", payload[i]);
		printk("\n");
		printk(KERN_DEBUG "TKIP decrypt: iv16=%04x iv32=%08x\n",
		       iv16, iv32);
	}
#endif /* CONFIG_TKIP_DEBUG */

	if (!(keyid & (1 << 5)))
		return TKIP_DECRYPT_NO_EXT_IV;

	if ((keyid >> 6) != key->keyidx)
		return TKIP_DECRYPT_INVALID_KEYIDX;

	if (key->u.tkip.rx_initialized[queue] &&
	    (iv32 < key->u.tkip.iv32_rx[queue] ||
	     (iv32 == key->u.tkip.iv32_rx[queue] &&
	      iv16 <= key->u.tkip.iv16_rx[queue]))) {
#ifdef CONFIG_TKIP_DEBUG
		printk(KERN_DEBUG "TKIP replay detected for RX frame from "
		       MAC_FMT " (RX IV (%04x,%02x) <= prev. IV (%04x,%02x)\n",
		       MAC_ARG(ta),
		       iv32, iv16, key->u.tkip.iv32_rx[queue],
		       key->u.tkip.iv16_rx[queue]);
#endif /* CONFIG_TKIP_DEBUG */
		return TKIP_DECRYPT_REPLAY;
	}

	if (only_iv) {
		res = TKIP_DECRYPT_OK;
		key->u.tkip.rx_initialized[queue] = 1;
		goto done;
	}

	if (!key->u.tkip.rx_initialized[queue] ||
	    key->u.tkip.iv32_rx[queue] != iv32) {
		key->u.tkip.rx_initialized[queue] = 1;
		/* IV16 wrapped around - perform TKIP phase 1 */
		tkip_mixing_phase1(ta, &key->key[ALG_TKIP_TEMP_ENCR_KEY],
				   iv32, key->u.tkip.p1k_rx[queue]);
#ifdef CONFIG_TKIP_DEBUG
		{
			int i;
			printk(KERN_DEBUG "TKIP decrypt: Phase1 TA=" MAC_FMT
			       " TK=", MAC_ARG(ta));
			for (i = 0; i < 16; i++)
				printk("%02x ",
				       key->key[ALG_TKIP_TEMP_ENCR_KEY + i]);
			printk("\n");
			printk(KERN_DEBUG "TKIP decrypt: P1K=");
			for (i = 0; i < 5; i++)
				printk("%04x ", key->u.tkip.p1k_rx[queue][i]);
			printk("\n");
		}
#endif /* CONFIG_TKIP_DEBUG */
	}

	tkip_mixing_phase2(key->u.tkip.p1k_rx[queue],
			   &key->key[ALG_TKIP_TEMP_ENCR_KEY],
			   iv16, rc4key);
#ifdef CONFIG_TKIP_DEBUG
	{
		int i;
		printk(KERN_DEBUG "TKIP decrypt: Phase2 rc4key=");
		for (i = 0; i < 16; i++)
			printk("%02x ", rc4key[i]);
		printk("\n");
	}
#endif /* CONFIG_TKIP_DEBUG */

	res = ieee80211_wep_decrypt_data(tfm, rc4key, 16, pos, payload_len - 12);
 done:
	if (res == TKIP_DECRYPT_OK) {
		/* FIX: these should be updated only after Michael MIC has been
		 * verified */
		/* Record previously received IV */
		key->u.tkip.iv32_rx[queue] = iv32;
		key->u.tkip.iv16_rx[queue] = iv16;
	}

	return res;
}


