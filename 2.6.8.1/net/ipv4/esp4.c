#include <linux/config.h>
#include <linux/module.h>
#include <net/ip.h>
#include <net/xfrm.h>
#include <net/esp.h>
#include <asm/scatterlist.h>
#include <linux/crypto.h>
#include <linux/pfkeyv2.h>
#include <linux/random.h>
#include <net/icmp.h>
#include <net/udp.h>

/* decapsulation data for use when post-processing */
struct esp_decap_data {
	xfrm_address_t	saddr;
	__u16		sport;
	__u8		proto;
};

int esp_output(struct sk_buff **pskb)
{
	int err;
	struct dst_entry *dst = (*pskb)->dst;
	struct xfrm_state *x  = dst->xfrm;
	struct iphdr *top_iph;
	struct ip_esp_hdr *esph;
	struct crypto_tfm *tfm;
	struct esp_data *esp;
	struct sk_buff *trailer;
	int blksize;
	int clen;
	int alen;
	int nfrags;

	/* Strip IP+ESP header. */
	__skb_pull(*pskb, (*pskb)->h.raw - (*pskb)->data);
	/* Now skb is pure payload to encrypt */

	err = -ENOMEM;

	/* Round to block size */
	clen = (*pskb)->len;

	esp = x->data;
	alen = esp->auth.icv_trunc_len;
	tfm = esp->conf.tfm;
	blksize = (crypto_tfm_alg_blocksize(tfm) + 3) & ~3;
	clen = (clen + 2 + blksize-1)&~(blksize-1);
	if (esp->conf.padlen)
		clen = (clen + esp->conf.padlen-1)&~(esp->conf.padlen-1);

	if ((nfrags = skb_cow_data(*pskb, clen-(*pskb)->len+alen, &trailer)) < 0)
		goto error;

	/* Fill padding... */
	do {
		int i;
		for (i=0; i<clen-(*pskb)->len - 2; i++)
			*(u8*)(trailer->tail + i) = i+1;
	} while (0);
	*(u8*)(trailer->tail + clen-(*pskb)->len - 2) = (clen - (*pskb)->len)-2;
	pskb_put(*pskb, trailer, clen - (*pskb)->len);

	__skb_push(*pskb, (*pskb)->data - (*pskb)->nh.raw);
	top_iph = (*pskb)->nh.iph;
	esph = (struct ip_esp_hdr *)((*pskb)->nh.raw + top_iph->ihl*4);
	top_iph->tot_len = htons((*pskb)->len + alen);
	*(u8*)(trailer->tail - 1) = top_iph->protocol;

	/* this is non-NULL only with UDP Encapsulation */
	if (x->encap) {
		struct xfrm_encap_tmpl *encap = x->encap;
		struct udphdr *uh;
		u32 *udpdata32;

		uh = (struct udphdr *)esph;
		uh->source = encap->encap_sport;
		uh->dest = encap->encap_dport;
		uh->len = htons((*pskb)->len + alen - top_iph->ihl*4);
		uh->check = 0;

		switch (encap->encap_type) {
		default:
		case UDP_ENCAP_ESPINUDP:
			esph = (struct ip_esp_hdr *)(uh + 1);
			break;
		case UDP_ENCAP_ESPINUDP_NON_IKE:
			udpdata32 = (u32 *)(uh + 1);
			udpdata32[0] = udpdata32[1] = 0;
			esph = (struct ip_esp_hdr *)(udpdata32 + 2);
			break;
		}

		top_iph->protocol = IPPROTO_UDP;
	} else
		top_iph->protocol = IPPROTO_ESP;

	esph->spi = x->id.spi;
	esph->seq_no = htonl(++x->replay.oseq);

	if (esp->conf.ivlen)
		crypto_cipher_set_iv(tfm, esp->conf.ivec, crypto_tfm_alg_ivsize(tfm));

	do {
		struct scatterlist *sg = &esp->sgbuf[0];

		if (unlikely(nfrags > ESP_NUM_FAST_SG)) {
			sg = kmalloc(sizeof(struct scatterlist)*nfrags, GFP_ATOMIC);
			if (!sg)
				goto error;
		}
		skb_to_sgvec(*pskb, sg, esph->enc_data+esp->conf.ivlen-(*pskb)->data, clen);
		crypto_cipher_encrypt(tfm, sg, sg, clen);
		if (unlikely(sg != &esp->sgbuf[0]))
			kfree(sg);
	} while (0);

	if (esp->conf.ivlen) {
		memcpy(esph->enc_data, esp->conf.ivec, crypto_tfm_alg_ivsize(tfm));
		crypto_cipher_get_iv(tfm, esp->conf.ivec, crypto_tfm_alg_ivsize(tfm));
	}

	if (esp->auth.icv_full_len) {
		esp->auth.icv(esp, *pskb, (u8*)esph-(*pskb)->data,
		              sizeof(struct ip_esp_hdr) + esp->conf.ivlen+clen, trailer->tail);
		pskb_put(*pskb, trailer, alen);
	}

	ip_send_check(top_iph);

	err = 0;

error:
	return err;
}

/*
 * Note: detecting truncated vs. non-truncated authentication data is very
 * expensive, so we only support truncated data, which is the recommended
 * and common case.
 */
int esp_input(struct xfrm_state *x, struct xfrm_decap_state *decap, struct sk_buff *skb)
{
	struct iphdr *iph;
	struct ip_esp_hdr *esph;
	struct esp_data *esp = x->data;
	struct sk_buff *trailer;
	int blksize = crypto_tfm_alg_blocksize(esp->conf.tfm);
	int alen = esp->auth.icv_trunc_len;
	int elen = skb->len - sizeof(struct ip_esp_hdr) - esp->conf.ivlen - alen;
	int nfrags;
	int encap_len = 0;

	if (!pskb_may_pull(skb, sizeof(struct ip_esp_hdr)))
		goto out;

	if (elen <= 0 || (elen & (blksize-1)))
		goto out;

	/* If integrity check is required, do this. */
	if (esp->auth.icv_full_len) {
		u8 sum[esp->auth.icv_full_len];
		u8 sum1[alen];
		
		esp->auth.icv(esp, skb, 0, skb->len-alen, sum);

		if (skb_copy_bits(skb, skb->len-alen, sum1, alen))
			BUG();

		if (unlikely(memcmp(sum, sum1, alen))) {
			x->stats.integrity_failed++;
			goto out;
		}
	}

	if ((nfrags = skb_cow_data(skb, 0, &trailer)) < 0)
		goto out;

	skb->ip_summed = CHECKSUM_NONE;

	esph = (struct ip_esp_hdr*)skb->data;
	iph = skb->nh.iph;

	/* Get ivec. This can be wrong, check against another impls. */
	if (esp->conf.ivlen)
		crypto_cipher_set_iv(esp->conf.tfm, esph->enc_data, crypto_tfm_alg_ivsize(esp->conf.tfm));

        {
		u8 nexthdr[2];
		struct scatterlist *sg = &esp->sgbuf[0];
		u8 workbuf[60];
		int padlen;

		if (unlikely(nfrags > ESP_NUM_FAST_SG)) {
			sg = kmalloc(sizeof(struct scatterlist)*nfrags, GFP_ATOMIC);
			if (!sg)
				goto out;
		}
		skb_to_sgvec(skb, sg, sizeof(struct ip_esp_hdr) + esp->conf.ivlen, elen);
		crypto_cipher_decrypt(esp->conf.tfm, sg, sg, elen);
		if (unlikely(sg != &esp->sgbuf[0]))
			kfree(sg);

		if (skb_copy_bits(skb, skb->len-alen-2, nexthdr, 2))
			BUG();

		padlen = nexthdr[0];
		if (padlen+2 >= elen)
			goto out;

		/* ... check padding bits here. Silly. :-) */ 

		if (x->encap && decap && decap->decap_type) {
			struct esp_decap_data *encap_data;
			struct udphdr *uh = (struct udphdr *) (iph+1);

			encap_data = (struct esp_decap_data *) (decap->decap_data);
			encap_data->proto = 0;

			switch (decap->decap_type) {
			case UDP_ENCAP_ESPINUDP:
			case UDP_ENCAP_ESPINUDP_NON_IKE:
				encap_data->proto = AF_INET;
				encap_data->saddr.a4 = iph->saddr;
				encap_data->sport = uh->source;
				encap_len = (void*)esph - (void*)uh;
				break;

			default:
				goto out;
			}
		}

		iph->protocol = nexthdr[1];
		pskb_trim(skb, skb->len - alen - padlen - 2);
		memcpy(workbuf, skb->nh.raw, iph->ihl*4);
		skb->h.raw = skb_pull(skb, sizeof(struct ip_esp_hdr) + esp->conf.ivlen);
		skb->nh.raw += encap_len + sizeof(struct ip_esp_hdr) + esp->conf.ivlen;
		memcpy(skb->nh.raw, workbuf, iph->ihl*4);
		skb->nh.iph->tot_len = htons(skb->len);
	}

	return 0;

out:
	return -EINVAL;
}

int esp_post_input(struct xfrm_state *x, struct xfrm_decap_state *decap, struct sk_buff *skb)
{
  
	if (x->encap) {
		struct xfrm_encap_tmpl *encap;
		struct esp_decap_data *decap_data;

		encap = x->encap;
		decap_data = (struct esp_decap_data *)(decap->decap_data);

		/* first, make sure that the decap type == the encap type */
		if (encap->encap_type != decap->decap_type)
			return -EINVAL;

		switch (encap->encap_type) {
		default:
		case UDP_ENCAP_ESPINUDP:
		case UDP_ENCAP_ESPINUDP_NON_IKE:
			/*
			 * 1) if the NAT-T peer's IP or port changed then
			 *    advertize the change to the keying daemon.
			 *    This is an inbound SA, so just compare
			 *    SRC ports.
			 */
			if (decap_data->proto == AF_INET &&
			    (decap_data->saddr.a4 != x->props.saddr.a4 ||
			     decap_data->sport != encap->encap_sport)) {
				xfrm_address_t ipaddr;

				ipaddr.a4 = decap_data->saddr.a4;
				km_new_mapping(x, &ipaddr, decap_data->sport);
					
				/* XXX: perhaps add an extra
				 * policy check here, to see
				 * if we should allow or
				 * reject a packet from a
				 * different source
				 * address/port.
				 */
			}
		
			/*
			 * 2) ignore UDP/TCP checksums in case
			 *    of NAT-T in Transport Mode, or
			 *    perform other post-processing fixes
			 *    as per * draft-ietf-ipsec-udp-encaps-06,
			 *    section 3.1.2
			 */
			if (!x->props.mode)
				skb->ip_summed = CHECKSUM_UNNECESSARY;

			break;
		}
	}
	return 0;
}

static u32 esp4_get_max_size(struct xfrm_state *x, int mtu)
{
	struct esp_data *esp = x->data;
	u32 blksize = crypto_tfm_alg_blocksize(esp->conf.tfm);

	if (x->props.mode) {
		mtu = (mtu + 2 + blksize-1)&~(blksize-1);
	} else {
		/* The worst case. */
		mtu += 2 + blksize;
	}
	if (esp->conf.padlen)
		mtu = (mtu + esp->conf.padlen-1)&~(esp->conf.padlen-1);

	return mtu + x->props.header_len + esp->auth.icv_trunc_len;
}

void esp4_err(struct sk_buff *skb, u32 info)
{
	struct iphdr *iph = (struct iphdr*)skb->data;
	struct ip_esp_hdr *esph = (struct ip_esp_hdr*)(skb->data+(iph->ihl<<2));
	struct xfrm_state *x;

	if (skb->h.icmph->type != ICMP_DEST_UNREACH ||
	    skb->h.icmph->code != ICMP_FRAG_NEEDED)
		return;

	x = xfrm_state_lookup((xfrm_address_t *)&iph->daddr, esph->spi, IPPROTO_ESP, AF_INET);
	if (!x)
		return;
	NETDEBUG(printk(KERN_DEBUG "pmtu discovery on SA ESP/%08x/%08x\n",
			ntohl(esph->spi), ntohl(iph->daddr)));
	xfrm_state_put(x);
}

void esp_destroy(struct xfrm_state *x)
{
	struct esp_data *esp = x->data;

	if (!esp)
		return;

	if (esp->conf.tfm) {
		crypto_free_tfm(esp->conf.tfm);
		esp->conf.tfm = NULL;
	}
	if (esp->conf.ivec) {
		kfree(esp->conf.ivec);
		esp->conf.ivec = NULL;
	}
	if (esp->auth.tfm) {
		crypto_free_tfm(esp->auth.tfm);
		esp->auth.tfm = NULL;
	}
	if (esp->auth.work_icv) {
		kfree(esp->auth.work_icv);
		esp->auth.work_icv = NULL;
	}
	kfree(esp);
}

int esp_init_state(struct xfrm_state *x, void *args)
{
	struct esp_data *esp = NULL;

	/* null auth and encryption can have zero length keys */
	if (x->aalg) {
		if (x->aalg->alg_key_len > 512)
			goto error;
	}
	if (x->ealg == NULL)
		goto error;

	esp = kmalloc(sizeof(*esp), GFP_KERNEL);
	if (esp == NULL)
		return -ENOMEM;

	memset(esp, 0, sizeof(*esp));

	if (x->aalg) {
		struct xfrm_algo_desc *aalg_desc;

		esp->auth.key = x->aalg->alg_key;
		esp->auth.key_len = (x->aalg->alg_key_len+7)/8;
		esp->auth.tfm = crypto_alloc_tfm(x->aalg->alg_name, 0);
		if (esp->auth.tfm == NULL)
			goto error;
		esp->auth.icv = esp_hmac_digest;

		aalg_desc = xfrm_aalg_get_byname(x->aalg->alg_name);
		BUG_ON(!aalg_desc);

		if (aalg_desc->uinfo.auth.icv_fullbits/8 !=
		    crypto_tfm_alg_digestsize(esp->auth.tfm)) {
			NETDEBUG(printk(KERN_INFO "ESP: %s digestsize %u != %hu\n",
			       x->aalg->alg_name,
			       crypto_tfm_alg_digestsize(esp->auth.tfm),
			       aalg_desc->uinfo.auth.icv_fullbits/8));
			goto error;
		}

		esp->auth.icv_full_len = aalg_desc->uinfo.auth.icv_fullbits/8;
		esp->auth.icv_trunc_len = aalg_desc->uinfo.auth.icv_truncbits/8;

		esp->auth.work_icv = kmalloc(esp->auth.icv_full_len, GFP_KERNEL);
		if (!esp->auth.work_icv)
			goto error;
	}
	esp->conf.key = x->ealg->alg_key;
	esp->conf.key_len = (x->ealg->alg_key_len+7)/8;
	if (x->props.ealgo == SADB_EALG_NULL)
		esp->conf.tfm = crypto_alloc_tfm(x->ealg->alg_name, CRYPTO_TFM_MODE_ECB);
	else
		esp->conf.tfm = crypto_alloc_tfm(x->ealg->alg_name, CRYPTO_TFM_MODE_CBC);
	if (esp->conf.tfm == NULL)
		goto error;
	esp->conf.ivlen = crypto_tfm_alg_ivsize(esp->conf.tfm);
	esp->conf.padlen = 0;
	if (esp->conf.ivlen) {
		esp->conf.ivec = kmalloc(esp->conf.ivlen, GFP_KERNEL);
		if (unlikely(esp->conf.ivec == NULL))
			goto error;
		get_random_bytes(esp->conf.ivec, esp->conf.ivlen);
	}
	crypto_cipher_setkey(esp->conf.tfm, esp->conf.key, esp->conf.key_len);
	x->props.header_len = sizeof(struct ip_esp_hdr) + esp->conf.ivlen;
	if (x->props.mode)
		x->props.header_len += sizeof(struct iphdr);
	if (x->encap) {
		struct xfrm_encap_tmpl *encap = x->encap;

		switch (encap->encap_type) {
		default:
		case UDP_ENCAP_ESPINUDP:
			x->props.header_len += sizeof(struct udphdr);
			break;
		case UDP_ENCAP_ESPINUDP_NON_IKE:
			x->props.header_len += sizeof(struct udphdr) + 2 * sizeof(u32);
			break;
		}
	}
	x->data = esp;
	x->props.trailer_len = esp4_get_max_size(x, 0) - x->props.header_len;
	return 0;

error:
	if (esp) {
		if (esp->auth.tfm)
			crypto_free_tfm(esp->auth.tfm);
		if (esp->auth.work_icv)
			kfree(esp->auth.work_icv);
		if (esp->conf.tfm)
			crypto_free_tfm(esp->conf.tfm);
		kfree(esp);
	}
	return -EINVAL;
}

static struct xfrm_type esp_type =
{
	.description	= "ESP4",
	.owner		= THIS_MODULE,
	.proto	     	= IPPROTO_ESP,
	.init_state	= esp_init_state,
	.destructor	= esp_destroy,
	.get_max_size	= esp4_get_max_size,
	.input		= esp_input,
	.post_input	= esp_post_input,
	.output		= esp_output
};

static struct net_protocol esp4_protocol = {
	.handler	=	xfrm4_rcv,
	.err_handler	=	esp4_err,
	.no_policy	=	1,
};

static int __init esp4_init(void)
{
	struct xfrm_decap_state decap;

	if (sizeof(struct esp_decap_data)  <
	    sizeof(decap.decap_data)) {
		extern void decap_data_too_small(void);

		decap_data_too_small();
	}

	if (xfrm_register_type(&esp_type, AF_INET) < 0) {
		printk(KERN_INFO "ip esp init: can't add xfrm type\n");
		return -EAGAIN;
	}
	if (inet_add_protocol(&esp4_protocol, IPPROTO_ESP) < 0) {
		printk(KERN_INFO "ip esp init: can't add protocol\n");
		xfrm_unregister_type(&esp_type, AF_INET);
		return -EAGAIN;
	}
	return 0;
}

static void __exit esp4_fini(void)
{
	if (inet_del_protocol(&esp4_protocol, IPPROTO_ESP) < 0)
		printk(KERN_INFO "ip esp close: can't remove protocol\n");
	if (xfrm_unregister_type(&esp_type, AF_INET) < 0)
		printk(KERN_INFO "ip esp close: can't remove xfrm type\n");
}

module_init(esp4_init);
module_exit(esp4_fini);
MODULE_LICENSE("GPL");
