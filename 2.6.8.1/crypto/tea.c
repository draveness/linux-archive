/* 
 * Cryptographic API.
 *
 * TEA and Xtended TEA Algorithms
 *
 * The TEA and Xtended TEA algorithms were developed by David Wheeler 
 * and Roger Needham at the Computer Laboratory of Cambridge University.
 *
 * Copyright (c) 2004 Aaron Grothe ajgrothe@yahoo.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/scatterlist.h>
#include <linux/crypto.h>

#define TEA_KEY_SIZE		16
#define TEA_BLOCK_SIZE		8
#define TEA_ROUNDS		32
#define TEA_DELTA		0x9e3779b9

#define XTEA_KEY_SIZE		16
#define XTEA_BLOCK_SIZE		8
#define XTEA_ROUNDS		32
#define XTEA_DELTA		0x9e3779b9

#define u32_in(x) le32_to_cpu(*(const u32 *)(x))
#define u32_out(to, from) (*(u32 *)(to) = cpu_to_le32(from))

struct tea_ctx {
	u32 KEY[4];
};

struct xtea_ctx {
	u32 KEY[4];
};

static int tea_setkey(void *ctx_arg, const u8 *in_key,
                       unsigned int key_len, u32 *flags)
{ 

	struct tea_ctx *ctx = ctx_arg;
	
	if (key_len != 16)
	{
		*flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
		return -EINVAL;
	}

	ctx->KEY[0] = u32_in (in_key);
	ctx->KEY[1] = u32_in (in_key + 4);
	ctx->KEY[2] = u32_in (in_key + 8);
	ctx->KEY[3] = u32_in (in_key + 12);

	return 0; 

}

static void tea_encrypt(void *ctx_arg, u8 *dst, const u8 *src)
{ 
	u32 y, z, n, sum = 0;
	u32 k0, k1, k2, k3;

	struct tea_ctx *ctx = ctx_arg;

	y = u32_in (src);
	z = u32_in (src + 4);

	k0 = ctx->KEY[0];
	k1 = ctx->KEY[1];
	k2 = ctx->KEY[2];
	k3 = ctx->KEY[3];

	n = TEA_ROUNDS;

	while (n-- > 0) {
		sum += TEA_DELTA;
		y += ((z << 4) + k0) ^ (z + sum) ^ ((z >> 5) + k1);
		z += ((y << 4) + k2) ^ (y + sum) ^ ((y >> 5) + k3);
	}
	
	u32_out (dst, y);
	u32_out (dst + 4, z);
}

static void tea_decrypt(void *ctx_arg, u8 *dst, const u8 *src)
{ 
	u32 y, z, n, sum;
	u32 k0, k1, k2, k3;

	struct tea_ctx *ctx = ctx_arg;

	y = u32_in (src);
	z = u32_in (src + 4);

	k0 = ctx->KEY[0];
	k1 = ctx->KEY[1];
	k2 = ctx->KEY[2];
	k3 = ctx->KEY[3];

	sum = TEA_DELTA << 5;

	n = TEA_ROUNDS;

	while (n-- > 0) {
		z -= ((y << 4) + k2) ^ (y + sum) ^ ((y >> 5) + k3);
		y -= ((z << 4) + k0) ^ (z + sum) ^ ((z >> 5) + k1);
		sum -= TEA_DELTA;
	}
	
	u32_out (dst, y);
	u32_out (dst + 4, z);

}

static int xtea_setkey(void *ctx_arg, const u8 *in_key,
                       unsigned int key_len, u32 *flags)
{ 

	struct xtea_ctx *ctx = ctx_arg;
	
	if (key_len != 16)
	{
		*flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
		return -EINVAL;
	}

	ctx->KEY[0] = u32_in (in_key);
	ctx->KEY[1] = u32_in (in_key + 4);
	ctx->KEY[2] = u32_in (in_key + 8);
	ctx->KEY[3] = u32_in (in_key + 12);

	return 0; 

}

static void xtea_encrypt(void *ctx_arg, u8 *dst, const u8 *src)
{ 

	u32 y, z, sum = 0;
	u32 limit = XTEA_DELTA * XTEA_ROUNDS;

	struct xtea_ctx *ctx = ctx_arg;

	y = u32_in (src);
	z = u32_in (src + 4);

	while (sum != limit) {
		y += (z << 4 ^ z >> 5) + (z ^ sum) + ctx->KEY[sum&3]; 
		sum += TEA_DELTA;
		z += (y << 4 ^ y >> 5) + (y ^ sum) + ctx->KEY[sum>>11 &3]; 
	}
	
	u32_out (dst, y);
	u32_out (dst + 4, z);

}

static void xtea_decrypt(void *ctx_arg, u8 *dst, const u8 *src)
{ 

	u32 y, z, sum;
	struct tea_ctx *ctx = ctx_arg;

	y = u32_in (src);
	z = u32_in (src + 4);

	sum = XTEA_DELTA * XTEA_ROUNDS;

	while (sum) {
		z -= (y << 4 ^ y >> 5) + (y ^ sum) + ctx->KEY[sum>>11 & 3];
		sum -= XTEA_DELTA;
		y -= (z << 4 ^ z >> 5) + (z ^ sum) + ctx->KEY[sum & 3];
	}
	
	u32_out (dst, y);
	u32_out (dst + 4, z);

}

static struct crypto_alg tea_alg = {
	.cra_name		=	"tea",
	.cra_flags		=	CRYPTO_ALG_TYPE_CIPHER,
	.cra_blocksize		=	TEA_BLOCK_SIZE,
	.cra_ctxsize		=	sizeof (struct tea_ctx),
	.cra_module		=	THIS_MODULE,
	.cra_list		=	LIST_HEAD_INIT(tea_alg.cra_list),
	.cra_u			=	{ .cipher = {
	.cia_min_keysize	=	TEA_KEY_SIZE,
	.cia_max_keysize	=	TEA_KEY_SIZE,
	.cia_setkey		= 	tea_setkey,
	.cia_encrypt		=	tea_encrypt,
	.cia_decrypt		=	tea_decrypt } }
};

static struct crypto_alg xtea_alg = {
	.cra_name		=	"xtea",
	.cra_flags		=	CRYPTO_ALG_TYPE_CIPHER,
	.cra_blocksize		=	XTEA_BLOCK_SIZE,
	.cra_ctxsize		=	sizeof (struct xtea_ctx),
	.cra_module		=	THIS_MODULE,
	.cra_list		=	LIST_HEAD_INIT(xtea_alg.cra_list),
	.cra_u			=	{ .cipher = {
	.cia_min_keysize	=	XTEA_KEY_SIZE,
	.cia_max_keysize	=	XTEA_KEY_SIZE,
	.cia_setkey		= 	xtea_setkey,
	.cia_encrypt		=	xtea_encrypt,
	.cia_decrypt		=	xtea_decrypt } }
};

static int __init init(void)
{
	int ret = 0;
	
	ret = crypto_register_alg(&tea_alg);
	if (ret < 0)
		goto out;

	ret = crypto_register_alg(&xtea_alg);
	if (ret < 0) {
		crypto_unregister_alg(&tea_alg);
		goto out;
	}

out:	
	return ret;
}

static void __exit fini(void)
{
	crypto_unregister_alg(&tea_alg);
	crypto_unregister_alg(&xtea_alg);
}

MODULE_ALIAS("xtea");

module_init(init);
module_exit(fini);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEA & XTEA Cryptographic Algorithms");
