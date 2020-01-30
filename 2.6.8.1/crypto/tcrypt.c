/* 
 * Quick & dirty crypto testing module.
 *
 * This will only exist until we have a better testing mechanism
 * (e.g. a char device).
 *
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 * Copyright (c) 2002 Jean-Francois Dive <jef@linuxbe.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 * 14 - 09 - 2003 
 *	Rewritten by Kartikey Mahendra Bhatt
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/scatterlist.h>
#include <linux/string.h>
#include <linux/crypto.h>
#include <linux/highmem.h>
#include "tcrypt.h"

/*
 * Need to kmalloc() memory for testing kmap().
 */
#define TVMEMSIZE	4096
#define XBUFSIZE	32768

/*
 * Indexes into the xbuf to simulate cross-page access.
 */
#define IDX1		37
#define IDX2		32400
#define IDX3		1
#define IDX4		8193
#define IDX5		22222
#define IDX6		17101
#define IDX7		27333
#define IDX8		3000

/*
* Used by test_cipher()
*/
#define ENCRYPT 1
#define DECRYPT 0
#define MODE_ECB 1
#define MODE_CBC 0

static unsigned int IDX[8] = { IDX1, IDX2, IDX3, IDX4, IDX5, IDX6, IDX7, IDX8 };

static int mode;
static char *xbuf;
static char *tvmem;

static char *check[] = {
	"des", "md5", "des3_ede", "rot13", "sha1", "sha256", "blowfish",
	"twofish", "serpent", "sha384", "sha512", "md4", "aes", "cast6", 
	"arc4", "michael_mic", "deflate", "crc32c", "tea", "xtea", NULL
};

static void
hexdump(unsigned char *buf, unsigned int len)
{
	while (len--)
		printk("%02x", *buf++);

	printk("\n");
}

static void 
test_hash (char * algo, struct hash_testvec * template, unsigned int tcount)
{
	char *p; 
        unsigned int i, j, k, temp;
        struct scatterlist sg[8];
        char result[64];
        struct crypto_tfm *tfm;
        struct hash_testvec *hash_tv;
        unsigned int tsize;
	 
        printk("\ntesting %s\n", algo);

	tsize = sizeof (struct hash_testvec);
	tsize *= tcount;
	
	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize, TVMEMSIZE);
		return;
	}

	memcpy(tvmem, template, tsize);
	hash_tv = (void *) tvmem;
	tfm = crypto_alloc_tfm(algo, 0);
	if (tfm == NULL) {
		printk("failed to load transform for %s\n", algo);
		return;
	}

	for (i = 0; i < tcount; i++) {
		printk ("test %u:\n", i + 1);
		memset (result, 0, 64);

		p = hash_tv[i].plaintext;
		sg[0].page = virt_to_page (p);
		sg[0].offset = offset_in_page (p);
		sg[0].length = hash_tv[i].psize;

		crypto_digest_init (tfm);
		if (tfm->crt_u.digest.dit_setkey) {
			crypto_digest_setkey (tfm, hash_tv[i].key,
					      hash_tv[i].ksize);
		}
		crypto_digest_update (tfm, sg, 1);
		crypto_digest_final (tfm, result);

		hexdump (result, crypto_tfm_alg_digestsize (tfm));
		printk("%s\n",
			memcmp(result, hash_tv[i].digest,
				crypto_tfm_alg_digestsize(tfm)) ? "fail" :
			"pass");
	}

	printk ("testing %s across pages\n", algo);

	/* setup the dummy buffer first */
        memset(xbuf, 0, XBUFSIZE);

	j = 0;
	for (i = 0; i < tcount; i++) {
		if (hash_tv[i].np) {
			j++;
			printk ("test %u:\n", j);
			memset (result, 0, 64);

			temp = 0;
			for (k = 0; k < hash_tv[i].np; k++) {
				memcpy (&xbuf[IDX[k]], hash_tv[i].plaintext + temp, 
						hash_tv[i].tap[k]);	
				temp += hash_tv[i].tap[k];
				p = &xbuf[IDX[k]];
				sg[k].page = virt_to_page (p);
				sg[k].offset = offset_in_page (p);
				sg[k].length = hash_tv[i].tap[k];
			}

			crypto_digest_digest (tfm, sg, hash_tv[i].np, result);
			
			hexdump (result, crypto_tfm_alg_digestsize (tfm));
			printk("%s\n",
				memcmp(result, hash_tv[i].digest,
					crypto_tfm_alg_digestsize(tfm)) ? "fail" :
				"pass");
		}
	}
	
	crypto_free_tfm (tfm);
}


#ifdef CONFIG_CRYPTO_HMAC

static void
test_hmac(char *algo, struct hmac_testvec * template, unsigned int tcount)
{
	char *p;
	unsigned int i, j, k, temp;
	struct scatterlist sg[8];
	char result[64];
	struct crypto_tfm *tfm;
	struct hmac_testvec *hmac_tv;
	unsigned int tsize, klen;

	tfm = crypto_alloc_tfm(algo, 0);
	if (tfm == NULL) {
		printk("failed to load transform for %s\n", algo);
		return;
	}

	printk("\ntesting hmac_%s\n", algo);
	
	tsize = sizeof (struct hmac_testvec);
	tsize *= tcount;
	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize,
		       TVMEMSIZE);
		goto out;
	}

	memcpy(tvmem, template, tsize);
	hmac_tv = (void *) tvmem;

	for (i = 0; i < tcount; i++) {
		printk("test %u:\n", i + 1);
		memset(result, 0, sizeof (result));

		p = hmac_tv[i].plaintext;
		klen = hmac_tv[i].ksize;
		sg[0].page = virt_to_page(p);
		sg[0].offset = offset_in_page(p);
		sg[0].length = hmac_tv[i].psize;

		crypto_hmac(tfm, hmac_tv[i].key, &klen, sg, 1, result);

		hexdump(result, crypto_tfm_alg_digestsize(tfm));
		printk("%s\n",
		       memcmp(result, hmac_tv[i].digest,
			      crypto_tfm_alg_digestsize(tfm)) ? "fail" :
		       "pass");
	}

	printk("\ntesting hmac_%s across pages\n", algo);

	memset(xbuf, 0, XBUFSIZE);
	
	j = 0;
	for (i = 0; i < tcount; i++) {
		if (hmac_tv[i].np) {
			j++;
			printk ("test %u:\n",j);
			memset (result, 0, 64);

			temp = 0;
			klen = hmac_tv[i].ksize;
			for (k = 0; k < hmac_tv[i].np; k++) {
				memcpy (&xbuf[IDX[k]], hmac_tv[i].plaintext + temp, 
						hmac_tv[i].tap[k]);	
				temp += hmac_tv[i].tap[k];
				p = &xbuf[IDX[k]];
				sg[k].page = virt_to_page (p);
				sg[k].offset = offset_in_page (p);
				sg[k].length = hmac_tv[i].tap[k];
			}

			crypto_hmac(tfm, hmac_tv[i].key, &klen, sg, hmac_tv[i].np, 
					result);
			hexdump(result, crypto_tfm_alg_digestsize(tfm));
			
			printk("%s\n",
				memcmp(result, hmac_tv[i].digest,
					crypto_tfm_alg_digestsize(tfm)) ? "fail" : 
				"pass");
		}
	}
out:
	crypto_free_tfm(tfm);
}

#endif	/* CONFIG_CRYPTO_HMAC */

void
test_cipher(char * algo, int mode, int enc, struct cipher_testvec * template, unsigned int tcount)
{
	unsigned int ret, i, j, k, temp;
	unsigned int tsize;
	char *p, *q;
	struct crypto_tfm *tfm;
	char *key;
	struct cipher_testvec *cipher_tv;
	struct scatterlist sg[8];
	char e[11], m[4];

	if (enc == ENCRYPT)
	        strncpy(e, "encryption", 11);
	else
        	strncpy(e, "decryption", 11);
	if (mode == MODE_ECB)
        	strncpy(m, "ECB", 4);
	else
        	strncpy(m, "CBC", 4);

	printk("\ntesting %s %s %s \n", algo, m, e);

	tsize = sizeof (struct cipher_testvec);	
	tsize *= tcount;
	
	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize,
		       TVMEMSIZE);
		return;
	}

	memcpy(tvmem, template, tsize);
	cipher_tv = (void *) tvmem;

	if (mode) 
		tfm = crypto_alloc_tfm (algo, 0);
	else 
		tfm = crypto_alloc_tfm (algo, CRYPTO_TFM_MODE_CBC);
	
	if (tfm == NULL) {
		printk("failed to load transform for %s %s\n", algo, m);
		return;
	}
	
	j = 0;
	for (i = 0; i < tcount; i++) {
		if (!(cipher_tv[i].np)) {
			j++;	
			printk("test %u (%d bit key):\n",
			j, cipher_tv[i].klen * 8);

			tfm->crt_flags = 0;
			if (cipher_tv[i].wk) 
				tfm->crt_flags |= CRYPTO_TFM_REQ_WEAK_KEY;
			key = cipher_tv[i].key;
	
			ret = crypto_cipher_setkey(tfm, key, cipher_tv[i].klen);
			if (ret) {
				printk("setkey() failed flags=%x\n", tfm->crt_flags);
	
				if (!cipher_tv[i].fail)
					goto out;
			}	

			p = cipher_tv[i].input;
			sg[0].page = virt_to_page(p);
			sg[0].offset = offset_in_page(p);
			sg[0].length = cipher_tv[i].ilen;
	
			if (!mode) {
				crypto_cipher_set_iv(tfm, cipher_tv[i].iv,
					crypto_tfm_alg_ivsize (tfm));
			}
		
			if (enc)
				ret = crypto_cipher_encrypt(tfm, sg, sg, cipher_tv[i].ilen);
			else
				ret = crypto_cipher_decrypt(tfm, sg, sg, cipher_tv[i].ilen);
			
				
			if (ret) {
				printk("%s () failed flags=%x\n", e, tfm->crt_flags);
				goto out;
			}	
	
			q = kmap(sg[0].page) + sg[0].offset;
			hexdump(q, cipher_tv[i].rlen);
	
			printk("%s\n", 
				memcmp(q, cipher_tv[i].result, cipher_tv[i].rlen) ? "fail" : 
			"pass");
		}
	}
	
	printk("\ntesting %s %s %s across pages (chunking) \n", algo, m, e);
	memset(xbuf, 0, XBUFSIZE);
	
	j = 0;
	for (i = 0; i < tcount; i++) {
		if (cipher_tv[i].np) {
			j++;				
			printk("test %u (%d bit key):\n",
			j, cipher_tv[i].klen * 8);

			tfm->crt_flags = 0;			
			if (cipher_tv[i].wk) 
				tfm->crt_flags |= CRYPTO_TFM_REQ_WEAK_KEY;
			key = cipher_tv[i].key;
			
			ret = crypto_cipher_setkey(tfm, key, cipher_tv[i].klen);		
			if (ret) {
				printk("setkey() failed flags=%x\n", tfm->crt_flags);
				
				if (!cipher_tv[i].fail)
					goto out;
			}

			temp = 0;
			for (k = 0; k < cipher_tv[i].np; k++) {
				memcpy (&xbuf[IDX[k]], cipher_tv[i].input + temp, 
						cipher_tv[i].tap[k]);	
				temp += cipher_tv[i].tap[k];
				p = &xbuf[IDX[k]];
				sg[k].page = virt_to_page (p);
				sg[k].offset = offset_in_page (p);
				sg[k].length = cipher_tv[i].tap[k];
			}
			
			if (!mode) {
				crypto_cipher_set_iv(tfm, cipher_tv[i].iv,
						crypto_tfm_alg_ivsize (tfm));
			}
			
			if (enc)
				ret = crypto_cipher_encrypt(tfm, sg, sg, cipher_tv[i].ilen);
			else
				ret = crypto_cipher_decrypt(tfm, sg, sg, cipher_tv[i].ilen);
			
			if (ret) {
				printk("%s () failed flags=%x\n", e, tfm->crt_flags);
				goto out;
			}

			temp = 0;
			for (k = 0; k < cipher_tv[i].np; k++) {
				printk("page %u\n", k);
				q = kmap(sg[k].page) + sg[k].offset;
				hexdump(q, cipher_tv[i].tap[k]);
				printk("%s\n", 
					memcmp(q, cipher_tv[i].result + temp, 
						cipher_tv[i].tap[k]) ? "fail" : 
					"pass");
				temp += cipher_tv[i].tap[k];
			}
		}
	}

out:
	crypto_free_tfm(tfm);
}

static void
test_deflate(void)
{
	unsigned int i;
	char result[COMP_BUF_SIZE];
	struct crypto_tfm *tfm;
	struct comp_testvec *tv;
	unsigned int tsize;

	printk("\ntesting deflate compression\n");

	tsize = sizeof (deflate_comp_tv_template);
	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize,
		       TVMEMSIZE);
		return;
	}

	memcpy(tvmem, deflate_comp_tv_template, tsize);
	tv = (void *) tvmem;

	tfm = crypto_alloc_tfm("deflate", 0);
	if (tfm == NULL) {
		printk("failed to load transform for deflate\n");
		return;
	}

	for (i = 0; i < DEFLATE_COMP_TEST_VECTORS; i++) {
		int ilen, ret, dlen = COMP_BUF_SIZE;
		
		printk("test %u:\n", i + 1);
		memset(result, 0, sizeof (result));

		ilen = tv[i].inlen;
		ret = crypto_comp_compress(tfm, tv[i].input,
		                           ilen, result, &dlen);
		if (ret) {
			printk("fail: ret=%d\n", ret);
			continue;
		}
		hexdump(result, dlen);
		printk("%s (ratio %d:%d)\n",
		       memcmp(result, tv[i].output, dlen) ? "fail" : "pass",
		       ilen, dlen);
	}

	printk("\ntesting deflate decompression\n");

	tsize = sizeof (deflate_decomp_tv_template);
	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize,
		       TVMEMSIZE);
		goto out;
	}

	memcpy(tvmem, deflate_decomp_tv_template, tsize);
	tv = (void *) tvmem;

	for (i = 0; i < DEFLATE_DECOMP_TEST_VECTORS; i++) {
		int ilen, ret, dlen = COMP_BUF_SIZE;
		
		printk("test %u:\n", i + 1);
		memset(result, 0, sizeof (result));

		ilen = tv[i].inlen;
		ret = crypto_comp_decompress(tfm, tv[i].input,
		                             ilen, result, &dlen);
		if (ret) {
			printk("fail: ret=%d\n", ret);
			continue;
		}
		hexdump(result, dlen);
		printk("%s (ratio %d:%d)\n",
		       memcmp(result, tv[i].output, dlen) ? "fail" : "pass",
		       ilen, dlen);
	}
out:
	crypto_free_tfm(tfm);
}

static void
test_crc32c(void)
{
#define NUMVEC 6
#define VECSIZE 40

	int i, j, pass;
	u32 crc;
	u8 b, test_vec[NUMVEC][VECSIZE];
	static u32 vec_results[NUMVEC] = {
		0x0e2c157f, 0xe980ebf6, 0xde74bded,
		0xd579c862, 0xba979ad0, 0x2b29d913
	};
	static u32 tot_vec_results = 0x24c5d375;
	
	struct scatterlist sg[NUMVEC];
	struct crypto_tfm *tfm;
	char *fmtdata = "testing crc32c initialized to %08x: %s\n";
#define SEEDTESTVAL 0xedcba987
	u32 seed;

	printk("\ntesting crc32c\n");

	tfm = crypto_alloc_tfm("crc32c", 0);
	if (tfm == NULL) {
		printk("failed to load transform for crc32c\n");
		return;
	}
	
	crypto_digest_init(tfm);
	crypto_digest_final(tfm, (u8*)&crc);
	printk(fmtdata, crc, (crc == 0) ? "pass" : "ERROR");
	
	/*
	 * stuff test_vec with known values, simple incrementing
	 * byte values.
	 */
	b = 0;
	for (i = 0; i < NUMVEC; i++) {
		for (j = 0; j < VECSIZE; j++) 
			test_vec[i][j] = ++b;
		sg[i].page = virt_to_page(test_vec[i]);
		sg[i].offset = offset_in_page(test_vec[i]);
		sg[i].length = VECSIZE;
	}

	seed = SEEDTESTVAL;
	(void)crypto_digest_setkey(tfm, (const u8*)&seed, sizeof(u32));
	crypto_digest_final(tfm, (u8*)&crc);
	printk("testing crc32c setkey returns %08x : %s\n", crc, (crc == (SEEDTESTVAL ^ ~(u32)0)) ?
	       "pass" : "ERROR");
	
	printk("testing crc32c using update/final:\n");

	pass = 1;		    /* assume all is well */
	
	for (i = 0; i < NUMVEC; i++) {
		seed = ~(u32)0;
		(void)crypto_digest_setkey(tfm, (const u8*)&seed, sizeof(u32));
		crypto_digest_update(tfm, &sg[i], 1);
		crypto_digest_final(tfm, (u8*)&crc);
		if (crc == vec_results[i]) {
			printk(" %08x:OK", crc);
		} else {
			printk(" %08x:BAD, wanted %08x\n", crc, vec_results[i]);
			pass = 0;
		}
	}

	printk("\ntesting crc32c using incremental accumulator:\n");
	crc = 0;
	for (i = 0; i < NUMVEC; i++) {
		seed = (crc ^ ~(u32)0);
		(void)crypto_digest_setkey(tfm, (const u8*)&seed, sizeof(u32));
		crypto_digest_update(tfm, &sg[i], 1);
		crypto_digest_final(tfm, (u8*)&crc);
	}
	if (crc == tot_vec_results) {
		printk(" %08x:OK", crc);
	} else {
		printk(" %08x:BAD, wanted %08x\n", crc, tot_vec_results);
		pass = 0;
	}

	printk("\ntesting crc32c using digest:\n");
	seed = ~(u32)0;
	(void)crypto_digest_setkey(tfm, (const u8*)&seed, sizeof(u32));
	crypto_digest_digest(tfm, sg, NUMVEC, (u8*)&crc);
	if (crc == tot_vec_results) {
		printk(" %08x:OK", crc);
	} else {
		printk(" %08x:BAD, wanted %08x\n", crc, tot_vec_results);
		pass = 0;
	}
	
	printk("\n%s\n", pass ? "pass" : "ERROR");

	crypto_free_tfm(tfm);
	printk("crc32c test complete\n");
}

static void
test_available(void)
{
	char **name = check;
	
	while (*name) {
		printk("alg %s ", *name);
		printk((crypto_alg_available(*name, 0)) ?
			"found\n" : "not found\n");
		name++;
	}	
}

static void
do_test(void)
{
	switch (mode) {

	case 0:
		test_hash("md5", md5_tv_template, MD5_TEST_VECTORS);
		
		test_hash("sha1", sha1_tv_template, SHA1_TEST_VECTORS);
		
		//DES
		test_cipher ("des", MODE_ECB, ENCRYPT, des_enc_tv_template, DES_ENC_TEST_VECTORS);
                test_cipher ("des", MODE_ECB, DECRYPT, des_dec_tv_template, DES_DEC_TEST_VECTORS);
                test_cipher ("des", MODE_CBC, ENCRYPT, des_cbc_enc_tv_template, DES_CBC_ENC_TEST_VECTORS);
                test_cipher ("des", MODE_CBC, DECRYPT, des_cbc_dec_tv_template, DES_CBC_DEC_TEST_VECTORS);
	
		//DES3_EDE
		test_cipher ("des3_ede", MODE_ECB, ENCRYPT, des3_ede_enc_tv_template, DES3_EDE_ENC_TEST_VECTORS);
                test_cipher ("des3_ede", MODE_ECB, DECRYPT, des3_ede_dec_tv_template, DES3_EDE_DEC_TEST_VECTORS);
		
		test_hash("md4", md4_tv_template, MD4_TEST_VECTORS);
		
		test_hash("sha256", sha256_tv_template, SHA256_TEST_VECTORS);
		
		//BLOWFISH
		test_cipher ("blowfish", MODE_ECB, ENCRYPT, bf_enc_tv_template, BF_ENC_TEST_VECTORS);
		test_cipher ("blowfish", MODE_ECB, DECRYPT, bf_dec_tv_template, BF_DEC_TEST_VECTORS);
		test_cipher ("blowfish", MODE_CBC, ENCRYPT, bf_cbc_enc_tv_template, BF_CBC_ENC_TEST_VECTORS);
		test_cipher ("blowfish", MODE_CBC, DECRYPT, bf_cbc_dec_tv_template, BF_CBC_DEC_TEST_VECTORS);
		
		//TWOFISH
		test_cipher ("twofish", MODE_ECB, ENCRYPT, tf_enc_tv_template, TF_ENC_TEST_VECTORS);
		test_cipher ("twofish", MODE_ECB, DECRYPT, tf_dec_tv_template, TF_DEC_TEST_VECTORS);
		test_cipher ("twofish", MODE_CBC, ENCRYPT, tf_cbc_enc_tv_template, TF_CBC_ENC_TEST_VECTORS);
		test_cipher ("twofish", MODE_CBC, DECRYPT, tf_cbc_dec_tv_template, TF_CBC_DEC_TEST_VECTORS);
		
		//SERPENT
		test_cipher ("serpent", MODE_ECB, ENCRYPT, serpent_enc_tv_template, SERPENT_ENC_TEST_VECTORS);
		test_cipher ("serpent", MODE_ECB, DECRYPT, serpent_dec_tv_template, SERPENT_DEC_TEST_VECTORS);
		
		//AES
		test_cipher ("aes", MODE_ECB, ENCRYPT, aes_enc_tv_template, AES_ENC_TEST_VECTORS);
		test_cipher ("aes", MODE_ECB, DECRYPT, aes_dec_tv_template, AES_DEC_TEST_VECTORS);

		//CAST5
		test_cipher ("cast5", MODE_ECB, ENCRYPT, cast5_enc_tv_template, CAST5_ENC_TEST_VECTORS);
		test_cipher ("cast5", MODE_ECB, DECRYPT, cast5_dec_tv_template, CAST5_DEC_TEST_VECTORS);
		
		//CAST6
		test_cipher ("cast6", MODE_ECB, ENCRYPT, cast6_enc_tv_template, CAST6_ENC_TEST_VECTORS);
		test_cipher ("cast6", MODE_ECB, DECRYPT, cast6_dec_tv_template, CAST6_DEC_TEST_VECTORS);

		//ARC4
		test_cipher ("arc4", MODE_ECB, ENCRYPT, arc4_enc_tv_template, ARC4_ENC_TEST_VECTORS);
		test_cipher ("arc4", MODE_ECB, DECRYPT, arc4_dec_tv_template, ARC4_DEC_TEST_VECTORS);

		//TEA
		test_cipher ("tea", MODE_ECB, ENCRYPT, tea_enc_tv_template, TEA_ENC_TEST_VECTORS);
		test_cipher ("tea", MODE_ECB, DECRYPT, tea_dec_tv_template, TEA_DEC_TEST_VECTORS);


		//XTEA
		test_cipher ("xtea", MODE_ECB, ENCRYPT, xtea_enc_tv_template, XTEA_ENC_TEST_VECTORS);
		test_cipher ("xtea", MODE_ECB, DECRYPT, xtea_dec_tv_template, XTEA_DEC_TEST_VECTORS);

		//KHAZAD
		test_cipher ("khazad", MODE_ECB, ENCRYPT, khazad_enc_tv_template, KHAZAD_ENC_TEST_VECTORS);
		test_cipher ("khazad", MODE_ECB, DECRYPT, khazad_dec_tv_template, KHAZAD_DEC_TEST_VECTORS);

		test_hash("sha384", sha384_tv_template, SHA384_TEST_VECTORS);
		test_hash("sha512", sha512_tv_template, SHA512_TEST_VECTORS);
		test_deflate();
		test_crc32c();
#ifdef CONFIG_CRYPTO_HMAC
		test_hmac("md5", hmac_md5_tv_template, HMAC_MD5_TEST_VECTORS);
		test_hmac("sha1", hmac_sha1_tv_template, HMAC_SHA1_TEST_VECTORS);		
		test_hmac("sha256", hmac_sha256_tv_template, HMAC_SHA256_TEST_VECTORS);
#endif		

		test_hash("michael_mic", michael_mic_tv_template, MICHAEL_MIC_TEST_VECTORS);
		break;

	case 1:
		test_hash("md5", md5_tv_template, MD5_TEST_VECTORS);
		break;

	case 2:
		test_hash("sha1", sha1_tv_template, SHA1_TEST_VECTORS);
		break;

	case 3:
		test_cipher ("des", MODE_ECB, ENCRYPT, des_enc_tv_template, DES_ENC_TEST_VECTORS);
		test_cipher ("des", MODE_ECB, DECRYPT, des_dec_tv_template, DES_DEC_TEST_VECTORS);
		test_cipher ("des", MODE_CBC, ENCRYPT, des_cbc_enc_tv_template, DES_CBC_ENC_TEST_VECTORS);
		test_cipher ("des", MODE_CBC, DECRYPT, des_cbc_dec_tv_template, DES_CBC_DEC_TEST_VECTORS);
		break;

	case 4:
		test_cipher ("des3_ede", MODE_ECB, ENCRYPT, des3_ede_enc_tv_template, DES3_EDE_ENC_TEST_VECTORS);
                test_cipher ("des3_ede", MODE_ECB, DECRYPT, des3_ede_dec_tv_template, DES3_EDE_DEC_TEST_VECTORS);
		break;

	case 5:
		test_hash("md4", md4_tv_template, MD4_TEST_VECTORS);
		break;
		
	case 6:
		test_hash("sha256", sha256_tv_template, SHA256_TEST_VECTORS);
		break;
	
	case 7:
		test_cipher ("blowfish", MODE_ECB, ENCRYPT, bf_enc_tv_template, BF_ENC_TEST_VECTORS);
		test_cipher ("blowfish", MODE_ECB, DECRYPT, bf_dec_tv_template, BF_DEC_TEST_VECTORS);
		test_cipher ("blowfish", MODE_CBC, ENCRYPT, bf_cbc_enc_tv_template, BF_CBC_ENC_TEST_VECTORS);
		test_cipher ("blowfish", MODE_CBC, DECRYPT, bf_cbc_dec_tv_template, BF_CBC_DEC_TEST_VECTORS);
		break;

	case 8:
		test_cipher ("twofish", MODE_ECB, ENCRYPT, tf_enc_tv_template, TF_ENC_TEST_VECTORS);
		test_cipher ("twofish", MODE_ECB, DECRYPT, tf_dec_tv_template, TF_DEC_TEST_VECTORS);
		test_cipher ("twofish", MODE_CBC, ENCRYPT, tf_cbc_enc_tv_template, TF_CBC_ENC_TEST_VECTORS);
		test_cipher ("twofish", MODE_CBC, DECRYPT, tf_cbc_dec_tv_template, TF_CBC_DEC_TEST_VECTORS);
		break;
		
	case 9:
		break;

	case 10:
		test_cipher ("aes", MODE_ECB, ENCRYPT, aes_enc_tv_template, AES_ENC_TEST_VECTORS);
		test_cipher ("aes", MODE_ECB, DECRYPT, aes_dec_tv_template, AES_DEC_TEST_VECTORS);	
		break;

	case 11:
		test_hash("sha384", sha384_tv_template, SHA384_TEST_VECTORS);
		break;
		
	case 12:
		test_hash("sha512", sha512_tv_template, SHA512_TEST_VECTORS);
		break;

	case 13:
		test_deflate();
		break;

	case 14:
		test_cipher ("cast5", MODE_ECB, ENCRYPT, cast5_enc_tv_template, CAST5_ENC_TEST_VECTORS);
		test_cipher ("cast5", MODE_ECB, DECRYPT, cast5_dec_tv_template, CAST5_DEC_TEST_VECTORS);
		break;

	case 15:
		test_cipher ("cast6", MODE_ECB, ENCRYPT, cast6_enc_tv_template, CAST6_ENC_TEST_VECTORS);
		test_cipher ("cast6", MODE_ECB, DECRYPT, cast6_dec_tv_template, CAST6_DEC_TEST_VECTORS);
		break;

	case 16:
		test_cipher ("arc4", MODE_ECB, ENCRYPT, arc4_enc_tv_template, ARC4_ENC_TEST_VECTORS);
		test_cipher ("arc4", MODE_ECB, DECRYPT, arc4_dec_tv_template, ARC4_DEC_TEST_VECTORS);
		break;

	case 17:
		test_hash("michael_mic", michael_mic_tv_template, MICHAEL_MIC_TEST_VECTORS);
		break;

	case 18:
		test_crc32c();
		break;

	case 19:
		test_cipher ("tea", MODE_ECB, ENCRYPT, tea_enc_tv_template, TEA_ENC_TEST_VECTORS);
		test_cipher ("tea", MODE_ECB, DECRYPT, tea_dec_tv_template, TEA_DEC_TEST_VECTORS);
		break;

	case 20:
		test_cipher ("xtea", MODE_ECB, ENCRYPT, xtea_enc_tv_template, XTEA_ENC_TEST_VECTORS);
		test_cipher ("xtea", MODE_ECB, DECRYPT, xtea_dec_tv_template, XTEA_DEC_TEST_VECTORS);
		break;

	case 21:
		test_cipher ("khazad", MODE_ECB, ENCRYPT, khazad_enc_tv_template, KHAZAD_ENC_TEST_VECTORS);
		test_cipher ("khazad", MODE_ECB, DECRYPT, khazad_dec_tv_template, KHAZAD_DEC_TEST_VECTORS);
		break;

#ifdef CONFIG_CRYPTO_HMAC
	case 100:
		test_hmac("md5", hmac_md5_tv_template, HMAC_MD5_TEST_VECTORS);
		break;
		
	case 101:
		test_hmac("sha1", hmac_sha1_tv_template, HMAC_SHA1_TEST_VECTORS);		
		break;
	
	case 102:
		test_hmac("sha256", hmac_sha256_tv_template, HMAC_SHA256_TEST_VECTORS);
		break;

#endif

	case 1000:
		test_available();
		break;
		
	default:
		/* useful for debugging */
		printk("not testing anything\n");
		break;
	}
}

static int __init
init(void)
{
	tvmem = kmalloc(TVMEMSIZE, GFP_KERNEL);
	if (tvmem == NULL)
		return -ENOMEM;

	xbuf = kmalloc(XBUFSIZE, GFP_KERNEL);
	if (xbuf == NULL) {
		kfree(tvmem);
		return -ENOMEM;
	}

	do_test();

	kfree(xbuf);
	kfree(tvmem);
	return 0;
}

/*
 * If an init function is provided, an exit function must also be provided
 * to allow module unload.
 */
static void __exit fini(void) { }

module_init(init);
module_exit(fini);

MODULE_PARM(mode, "i");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Quick & dirty crypto testing module");
MODULE_AUTHOR("James Morris <jmorris@intercode.com.au>");
