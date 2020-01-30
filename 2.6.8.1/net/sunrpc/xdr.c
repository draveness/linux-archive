/*
 * linux/net/sunrpc/xdr.c
 *
 * Generic XDR support.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/pagemap.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/net.h>
#include <net/sock.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/msg_prot.h>

/*
 * XDR functions for basic NFS types
 */
u32 *
xdr_encode_netobj(u32 *p, const struct xdr_netobj *obj)
{
	unsigned int	quadlen = XDR_QUADLEN(obj->len);

	p[quadlen] = 0;		/* zero trailing bytes */
	*p++ = htonl(obj->len);
	memcpy(p, obj->data, obj->len);
	return p + XDR_QUADLEN(obj->len);
}

u32 *
xdr_decode_netobj_fixed(u32 *p, void *obj, unsigned int len)
{
	if (ntohl(*p++) != len)
		return NULL;
	memcpy(obj, p, len);
	return p + XDR_QUADLEN(len);
}

u32 *
xdr_decode_netobj(u32 *p, struct xdr_netobj *obj)
{
	unsigned int	len;

	if ((len = ntohl(*p++)) > XDR_MAX_NETOBJ)
		return NULL;
	obj->len  = len;
	obj->data = (u8 *) p;
	return p + XDR_QUADLEN(len);
}

/**
 * xdr_encode_opaque_fixed - Encode fixed length opaque data
 * @p - pointer to current position in XDR buffer.
 * @ptr - pointer to data to encode (or NULL)
 * @nbytes - size of data.
 *
 * Copy the array of data of length nbytes at ptr to the XDR buffer
 * at position p, then align to the next 32-bit boundary by padding
 * with zero bytes (see RFC1832).
 * Note: if ptr is NULL, only the padding is performed.
 *
 * Returns the updated current XDR buffer position
 *
 */
u32 *xdr_encode_opaque_fixed(u32 *p, const void *ptr, unsigned int nbytes)
{
	if (likely(nbytes != 0)) {
		unsigned int quadlen = XDR_QUADLEN(nbytes);
		unsigned int padding = (quadlen << 2) - nbytes;

		if (ptr != NULL)
			memcpy(p, ptr, nbytes);
		if (padding != 0)
			memset((char *)p + nbytes, 0, padding);
		p += quadlen;
	}
	return p;
}
EXPORT_SYMBOL(xdr_encode_opaque_fixed);

/**
 * xdr_encode_opaque - Encode variable length opaque data
 * @p - pointer to current position in XDR buffer.
 * @ptr - pointer to data to encode (or NULL)
 * @nbytes - size of data.
 *
 * Returns the updated current XDR buffer position
 */
u32 *xdr_encode_opaque(u32 *p, const void *ptr, unsigned int nbytes)
{
	*p++ = htonl(nbytes);
	return xdr_encode_opaque_fixed(p, ptr, nbytes);
}
EXPORT_SYMBOL(xdr_encode_opaque);

u32 *
xdr_encode_string(u32 *p, const char *string)
{
	return xdr_encode_array(p, string, strlen(string));
}

u32 *
xdr_decode_string(u32 *p, char **sp, int *lenp, int maxlen)
{
	unsigned int	len;
	char		*string;

	if ((len = ntohl(*p++)) > maxlen)
		return NULL;
	if (lenp)
		*lenp = len;
	if ((len % 4) != 0) {
		string = (char *) p;
	} else {
		string = (char *) (p - 1);
		memmove(string, p, len);
	}
	string[len] = '\0';
	*sp = string;
	return p + XDR_QUADLEN(len);
}

u32 *
xdr_decode_string_inplace(u32 *p, char **sp, int *lenp, int maxlen)
{
	unsigned int	len;

	if ((len = ntohl(*p++)) > maxlen)
		return NULL;
	*lenp = len;
	*sp = (char *) p;
	return p + XDR_QUADLEN(len);
}

void
xdr_encode_pages(struct xdr_buf *xdr, struct page **pages, unsigned int base,
		 unsigned int len)
{
	struct kvec *tail = xdr->tail;
	u32 *p;

	xdr->pages = pages;
	xdr->page_base = base;
	xdr->page_len = len;

	p = (u32 *)xdr->head[0].iov_base + XDR_QUADLEN(xdr->head[0].iov_len);
	tail->iov_base = p;
	tail->iov_len = 0;

	if (len & 3) {
		unsigned int pad = 4 - (len & 3);

		*p = 0;
		tail->iov_base = (char *)p + (len & 3);
		tail->iov_len  = pad;
		len += pad;
	}
	xdr->buflen += len;
	xdr->len += len;
}

void
xdr_inline_pages(struct xdr_buf *xdr, unsigned int offset,
		 struct page **pages, unsigned int base, unsigned int len)
{
	struct kvec *head = xdr->head;
	struct kvec *tail = xdr->tail;
	char *buf = (char *)head->iov_base;
	unsigned int buflen = head->iov_len;

	head->iov_len  = offset;

	xdr->pages = pages;
	xdr->page_base = base;
	xdr->page_len = len;

	tail->iov_base = buf + offset;
	tail->iov_len = buflen - offset;

	xdr->buflen += len;
}

/*
 * Realign the kvec if the server missed out some reply elements
 * (such as post-op attributes,...)
 * Note: This is a simple implementation that assumes that
 *            len <= iov->iov_len !!!
 *       The RPC header (assumed to be the 1st element in the iov array)
 *            is not shifted.
 */
void xdr_shift_iovec(struct kvec *iov, int nr, size_t len)
{
	struct kvec *pvec;

	for (pvec = iov + nr - 1; nr > 1; nr--, pvec--) {
		struct kvec *svec = pvec - 1;

		if (len > pvec->iov_len) {
			printk(KERN_DEBUG "RPC: Urk! Large shift of short iovec.\n");
			return;
		}
		memmove((char *)pvec->iov_base + len, pvec->iov_base,
			pvec->iov_len - len);

		if (len > svec->iov_len) {
			printk(KERN_DEBUG "RPC: Urk! Large shift of short iovec.\n");
			return;
		}
		memcpy(pvec->iov_base,
		       (char *)svec->iov_base + svec->iov_len - len, len);
	}
}

/*
 * Map a struct xdr_buf into an kvec array.
 */
int xdr_kmap(struct kvec *iov_base, struct xdr_buf *xdr, size_t base)
{
	struct kvec	*iov = iov_base;
	struct page	**ppage = xdr->pages;
	unsigned int	len, pglen = xdr->page_len;

	len = xdr->head[0].iov_len;
	if (base < len) {
		iov->iov_len = len - base;
		iov->iov_base = (char *)xdr->head[0].iov_base + base;
		iov++;
		base = 0;
	} else
		base -= len;

	if (pglen == 0)
		goto map_tail;
	if (base >= pglen) {
		base -= pglen;
		goto map_tail;
	}
	if (base || xdr->page_base) {
		pglen -= base;
		base  += xdr->page_base;
		ppage += base >> PAGE_CACHE_SHIFT;
		base &= ~PAGE_CACHE_MASK;
	}
	do {
		len = PAGE_CACHE_SIZE;
		iov->iov_base = kmap(*ppage);
		if (base) {
			iov->iov_base += base;
			len -= base;
			base = 0;
		}
		if (pglen < len)
			len = pglen;
		iov->iov_len = len;
		iov++;
		ppage++;
	} while ((pglen -= len) != 0);
map_tail:
	if (xdr->tail[0].iov_len) {
		iov->iov_len = xdr->tail[0].iov_len - base;
		iov->iov_base = (char *)xdr->tail[0].iov_base + base;
		iov++;
	}
	return (iov - iov_base);
}

void xdr_kunmap(struct xdr_buf *xdr, size_t base)
{
	struct page	**ppage = xdr->pages;
	unsigned int	pglen = xdr->page_len;

	if (!pglen)
		return;
	if (base > xdr->head[0].iov_len)
		base -= xdr->head[0].iov_len;
	else
		base = 0;

	if (base >= pglen)
		return;
	if (base || xdr->page_base) {
		pglen -= base;
		base  += xdr->page_base;
		ppage += base >> PAGE_CACHE_SHIFT;
		/* Note: The offset means that the length of the first
		 * page is really (PAGE_CACHE_SIZE - (base & ~PAGE_CACHE_MASK)).
		 * In order to avoid an extra test inside the loop,
		 * we bump pglen here, and just subtract PAGE_CACHE_SIZE... */
		pglen += base & ~PAGE_CACHE_MASK;
	}
	for (;;) {
		flush_dcache_page(*ppage);
		kunmap(*ppage);
		if (pglen <= PAGE_CACHE_SIZE)
			break;
		pglen -= PAGE_CACHE_SIZE;
		ppage++;
	}
}

void
xdr_partial_copy_from_skb(struct xdr_buf *xdr, unsigned int base,
			  skb_reader_t *desc,
			  skb_read_actor_t copy_actor)
{
	struct page	**ppage = xdr->pages;
	unsigned int	len, pglen = xdr->page_len;
	int		ret;

	len = xdr->head[0].iov_len;
	if (base < len) {
		len -= base;
		ret = copy_actor(desc, (char *)xdr->head[0].iov_base + base, len);
		if (ret != len || !desc->count)
			return;
		base = 0;
	} else
		base -= len;

	if (pglen == 0)
		goto copy_tail;
	if (base >= pglen) {
		base -= pglen;
		goto copy_tail;
	}
	if (base || xdr->page_base) {
		pglen -= base;
		base  += xdr->page_base;
		ppage += base >> PAGE_CACHE_SHIFT;
		base &= ~PAGE_CACHE_MASK;
	}
	do {
		char *kaddr;

		len = PAGE_CACHE_SIZE;
		kaddr = kmap_atomic(*ppage, KM_SKB_SUNRPC_DATA);
		if (base) {
			len -= base;
			if (pglen < len)
				len = pglen;
			ret = copy_actor(desc, kaddr + base, len);
			base = 0;
		} else {
			if (pglen < len)
				len = pglen;
			ret = copy_actor(desc, kaddr, len);
		}
		flush_dcache_page(*ppage);
		kunmap_atomic(kaddr, KM_SKB_SUNRPC_DATA);
		if (ret != len || !desc->count)
			return;
		ppage++;
	} while ((pglen -= len) != 0);
copy_tail:
	len = xdr->tail[0].iov_len;
	if (base < len)
		copy_actor(desc, (char *)xdr->tail[0].iov_base + base, len - base);
}


int
xdr_sendpages(struct socket *sock, struct sockaddr *addr, int addrlen,
		struct xdr_buf *xdr, unsigned int base, int msgflags)
{
	struct page **ppage = xdr->pages;
	unsigned int len, pglen = xdr->page_len;
	int err, ret = 0;
	ssize_t (*sendpage)(struct socket *, struct page *, int, size_t, int);

	len = xdr->head[0].iov_len;
	if (base < len || (addr != NULL && base == 0)) {
		struct kvec iov = {
			.iov_base = xdr->head[0].iov_base + base,
			.iov_len  = len - base,
		};
		struct msghdr msg = {
			.msg_name    = addr,
			.msg_namelen = addrlen,
			.msg_flags   = msgflags,
		};
		if (xdr->len > len)
			msg.msg_flags |= MSG_MORE;

		if (iov.iov_len != 0)
			err = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
		else
			err = kernel_sendmsg(sock, &msg, NULL, 0, 0);
		if (ret == 0)
			ret = err;
		else if (err > 0)
			ret += err;
		if (err != iov.iov_len)
			goto out;
		base = 0;
	} else
		base -= len;

	if (pglen == 0)
		goto copy_tail;
	if (base >= pglen) {
		base -= pglen;
		goto copy_tail;
	}
	if (base || xdr->page_base) {
		pglen -= base;
		base  += xdr->page_base;
		ppage += base >> PAGE_CACHE_SHIFT;
		base &= ~PAGE_CACHE_MASK;
	}

	sendpage = sock->ops->sendpage ? : sock_no_sendpage;
	do {
		int flags = msgflags;

		len = PAGE_CACHE_SIZE;
		if (base)
			len -= base;
		if (pglen < len)
			len = pglen;

		if (pglen != len || xdr->tail[0].iov_len != 0)
			flags |= MSG_MORE;

		/* Hmm... We might be dealing with highmem pages */
		if (PageHighMem(*ppage))
			sendpage = sock_no_sendpage;
		err = sendpage(sock, *ppage, base, len, flags);
		if (ret == 0)
			ret = err;
		else if (err > 0)
			ret += err;
		if (err != len)
			goto out;
		base = 0;
		ppage++;
	} while ((pglen -= len) != 0);
copy_tail:
	len = xdr->tail[0].iov_len;
	if (base < len) {
		struct kvec iov = {
			.iov_base = xdr->tail[0].iov_base + base,
			.iov_len  = len - base,
		};
		struct msghdr msg = {
			.msg_flags   = msgflags,
		};
		err = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
		if (ret == 0)
			ret = err;
		else if (err > 0)
			ret += err;
	}
out:
	return ret;
}


/*
 * Helper routines for doing 'memmove' like operations on a struct xdr_buf
 *
 * _shift_data_right_pages
 * @pages: vector of pages containing both the source and dest memory area.
 * @pgto_base: page vector address of destination
 * @pgfrom_base: page vector address of source
 * @len: number of bytes to copy
 *
 * Note: the addresses pgto_base and pgfrom_base are both calculated in
 *       the same way:
 *            if a memory area starts at byte 'base' in page 'pages[i]',
 *            then its address is given as (i << PAGE_CACHE_SHIFT) + base
 * Also note: pgfrom_base must be < pgto_base, but the memory areas
 * 	they point to may overlap.
 */
static void
_shift_data_right_pages(struct page **pages, size_t pgto_base,
		size_t pgfrom_base, size_t len)
{
	struct page **pgfrom, **pgto;
	char *vfrom, *vto;
	size_t copy;

	BUG_ON(pgto_base <= pgfrom_base);

	pgto_base += len;
	pgfrom_base += len;

	pgto = pages + (pgto_base >> PAGE_CACHE_SHIFT);
	pgfrom = pages + (pgfrom_base >> PAGE_CACHE_SHIFT);

	pgto_base &= ~PAGE_CACHE_MASK;
	pgfrom_base &= ~PAGE_CACHE_MASK;

	do {
		/* Are any pointers crossing a page boundary? */
		if (pgto_base == 0) {
			pgto_base = PAGE_CACHE_SIZE;
			pgto--;
		}
		if (pgfrom_base == 0) {
			pgfrom_base = PAGE_CACHE_SIZE;
			pgfrom--;
		}

		copy = len;
		if (copy > pgto_base)
			copy = pgto_base;
		if (copy > pgfrom_base)
			copy = pgfrom_base;
		pgto_base -= copy;
		pgfrom_base -= copy;

		vto = kmap_atomic(*pgto, KM_USER0);
		vfrom = kmap_atomic(*pgfrom, KM_USER1);
		memmove(vto + pgto_base, vfrom + pgfrom_base, copy);
		kunmap_atomic(vfrom, KM_USER1);
		kunmap_atomic(vto, KM_USER0);

	} while ((len -= copy) != 0);
}

/*
 * _copy_to_pages
 * @pages: array of pages
 * @pgbase: page vector address of destination
 * @p: pointer to source data
 * @len: length
 *
 * Copies data from an arbitrary memory location into an array of pages
 * The copy is assumed to be non-overlapping.
 */
static void
_copy_to_pages(struct page **pages, size_t pgbase, const char *p, size_t len)
{
	struct page **pgto;
	char *vto;
	size_t copy;

	pgto = pages + (pgbase >> PAGE_CACHE_SHIFT);
	pgbase &= ~PAGE_CACHE_MASK;

	do {
		copy = PAGE_CACHE_SIZE - pgbase;
		if (copy > len)
			copy = len;

		vto = kmap_atomic(*pgto, KM_USER0);
		memcpy(vto + pgbase, p, copy);
		kunmap_atomic(vto, KM_USER0);

		pgbase += copy;
		if (pgbase == PAGE_CACHE_SIZE) {
			pgbase = 0;
			pgto++;
		}
		p += copy;

	} while ((len -= copy) != 0);
}

/*
 * _copy_from_pages
 * @p: pointer to destination
 * @pages: array of pages
 * @pgbase: offset of source data
 * @len: length
 *
 * Copies data into an arbitrary memory location from an array of pages
 * The copy is assumed to be non-overlapping.
 */
void
_copy_from_pages(char *p, struct page **pages, size_t pgbase, size_t len)
{
	struct page **pgfrom;
	char *vfrom;
	size_t copy;

	pgfrom = pages + (pgbase >> PAGE_CACHE_SHIFT);
	pgbase &= ~PAGE_CACHE_MASK;

	do {
		copy = PAGE_CACHE_SIZE - pgbase;
		if (copy > len)
			copy = len;

		vfrom = kmap_atomic(*pgfrom, KM_USER0);
		memcpy(p, vfrom + pgbase, copy);
		kunmap_atomic(vfrom, KM_USER0);

		pgbase += copy;
		if (pgbase == PAGE_CACHE_SIZE) {
			pgbase = 0;
			pgfrom++;
		}
		p += copy;

	} while ((len -= copy) != 0);
}

/*
 * xdr_shrink_bufhead
 * @buf: xdr_buf
 * @len: bytes to remove from buf->head[0]
 *
 * Shrinks XDR buffer's header kvec buf->head[0] by 
 * 'len' bytes. The extra data is not lost, but is instead
 * moved into the inlined pages and/or the tail.
 */
void
xdr_shrink_bufhead(struct xdr_buf *buf, size_t len)
{
	struct kvec *head, *tail;
	size_t copy, offs;
	unsigned int pglen = buf->page_len;

	tail = buf->tail;
	head = buf->head;
	BUG_ON (len > head->iov_len);

	/* Shift the tail first */
	if (tail->iov_len != 0) {
		if (tail->iov_len > len) {
			copy = tail->iov_len - len;
			memmove((char *)tail->iov_base + len,
					tail->iov_base, copy);
		}
		/* Copy from the inlined pages into the tail */
		copy = len;
		if (copy > pglen)
			copy = pglen;
		offs = len - copy;
		if (offs >= tail->iov_len)
			copy = 0;
		else if (copy > tail->iov_len - offs)
			copy = tail->iov_len - offs;
		if (copy != 0)
			_copy_from_pages((char *)tail->iov_base + offs,
					buf->pages,
					buf->page_base + pglen + offs - len,
					copy);
		/* Do we also need to copy data from the head into the tail ? */
		if (len > pglen) {
			offs = copy = len - pglen;
			if (copy > tail->iov_len)
				copy = tail->iov_len;
			memcpy(tail->iov_base,
					(char *)head->iov_base +
					head->iov_len - offs,
					copy);
		}
	}
	/* Now handle pages */
	if (pglen != 0) {
		if (pglen > len)
			_shift_data_right_pages(buf->pages,
					buf->page_base + len,
					buf->page_base,
					pglen - len);
		copy = len;
		if (len > pglen)
			copy = pglen;
		_copy_to_pages(buf->pages, buf->page_base,
				(char *)head->iov_base + head->iov_len - len,
				copy);
	}
	head->iov_len -= len;
	buf->buflen -= len;
	/* Have we truncated the message? */
	if (buf->len > buf->buflen)
		buf->len = buf->buflen;
}

/*
 * xdr_shrink_pagelen
 * @buf: xdr_buf
 * @len: bytes to remove from buf->pages
 *
 * Shrinks XDR buffer's page array buf->pages by 
 * 'len' bytes. The extra data is not lost, but is instead
 * moved into the tail.
 */
void
xdr_shrink_pagelen(struct xdr_buf *buf, size_t len)
{
	struct kvec *tail;
	size_t copy;
	char *p;
	unsigned int pglen = buf->page_len;

	tail = buf->tail;
	BUG_ON (len > pglen);

	/* Shift the tail first */
	if (tail->iov_len != 0) {
		p = (char *)tail->iov_base + len;
		if (tail->iov_len > len) {
			copy = tail->iov_len - len;
			memmove(p, tail->iov_base, copy);
		} else
			buf->buflen -= len;
		/* Copy from the inlined pages into the tail */
		copy = len;
		if (copy > tail->iov_len)
			copy = tail->iov_len;
		_copy_from_pages((char *)tail->iov_base,
				buf->pages, buf->page_base + pglen - len,
				copy);
	}
	buf->page_len -= len;
	buf->buflen -= len;
	/* Have we truncated the message? */
	if (buf->len > buf->buflen)
		buf->len = buf->buflen;
}

void
xdr_shift_buf(struct xdr_buf *buf, size_t len)
{
	xdr_shrink_bufhead(buf, len);
}

/**
 * xdr_init_encode - Initialize a struct xdr_stream for sending data.
 * @xdr: pointer to xdr_stream struct
 * @buf: pointer to XDR buffer in which to encode data
 * @p: current pointer inside XDR buffer
 *
 * Note: at the moment the RPC client only passes the length of our
 *	 scratch buffer in the xdr_buf's header kvec. Previously this
 *	 meant we needed to call xdr_adjust_iovec() after encoding the
 *	 data. With the new scheme, the xdr_stream manages the details
 *	 of the buffer length, and takes care of adjusting the kvec
 *	 length for us.
 */
void xdr_init_encode(struct xdr_stream *xdr, struct xdr_buf *buf, uint32_t *p)
{
	struct kvec *iov = buf->head;

	xdr->buf = buf;
	xdr->iov = iov;
	xdr->end = (uint32_t *)((char *)iov->iov_base + iov->iov_len);
	buf->len = iov->iov_len = (char *)p - (char *)iov->iov_base;
	xdr->p = p;
}
EXPORT_SYMBOL(xdr_init_encode);

/**
 * xdr_reserve_space - Reserve buffer space for sending
 * @xdr: pointer to xdr_stream
 * @nbytes: number of bytes to reserve
 *
 * Checks that we have enough buffer space to encode 'nbytes' more
 * bytes of data. If so, update the total xdr_buf length, and
 * adjust the length of the current kvec.
 */
uint32_t * xdr_reserve_space(struct xdr_stream *xdr, size_t nbytes)
{
	uint32_t *p = xdr->p;
	uint32_t *q;

	/* align nbytes on the next 32-bit boundary */
	nbytes += 3;
	nbytes &= ~3;
	q = p + (nbytes >> 2);
	if (unlikely(q > xdr->end || q < p))
		return NULL;
	xdr->p = q;
	xdr->iov->iov_len += nbytes;
	xdr->buf->len += nbytes;
	return p;
}
EXPORT_SYMBOL(xdr_reserve_space);

/**
 * xdr_write_pages - Insert a list of pages into an XDR buffer for sending
 * @xdr: pointer to xdr_stream
 * @pages: list of pages
 * @base: offset of first byte
 * @len: length of data in bytes
 *
 */
void xdr_write_pages(struct xdr_stream *xdr, struct page **pages, unsigned int base,
		 unsigned int len)
{
	struct xdr_buf *buf = xdr->buf;
	struct kvec *iov = buf->tail;
	buf->pages = pages;
	buf->page_base = base;
	buf->page_len = len;

	iov->iov_base = (char *)xdr->p;
	iov->iov_len  = 0;
	xdr->iov = iov;

	if (len & 3) {
		unsigned int pad = 4 - (len & 3);

		BUG_ON(xdr->p >= xdr->end);
		iov->iov_base = (char *)xdr->p + (len & 3);
		iov->iov_len  += pad;
		len += pad;
		*xdr->p++ = 0;
	}
	buf->buflen += len;
	buf->len += len;
}
EXPORT_SYMBOL(xdr_write_pages);

/**
 * xdr_init_decode - Initialize an xdr_stream for decoding data.
 * @xdr: pointer to xdr_stream struct
 * @buf: pointer to XDR buffer from which to decode data
 * @p: current pointer inside XDR buffer
 */
void xdr_init_decode(struct xdr_stream *xdr, struct xdr_buf *buf, uint32_t *p)
{
	struct kvec *iov = buf->head;
	unsigned int len = iov->iov_len;

	if (len > buf->len)
		len = buf->len;
	xdr->buf = buf;
	xdr->iov = iov;
	xdr->p = p;
	xdr->end = (uint32_t *)((char *)iov->iov_base + len);
}
EXPORT_SYMBOL(xdr_init_decode);

/**
 * xdr_inline_decode - Retrieve non-page XDR data to decode
 * @xdr: pointer to xdr_stream struct
 * @nbytes: number of bytes of data to decode
 *
 * Check if the input buffer is long enough to enable us to decode
 * 'nbytes' more bytes of data starting at the current position.
 * If so return the current pointer, then update the current
 * pointer position.
 */
uint32_t * xdr_inline_decode(struct xdr_stream *xdr, size_t nbytes)
{
	uint32_t *p = xdr->p;
	uint32_t *q = p + XDR_QUADLEN(nbytes);

	if (unlikely(q > xdr->end || q < p))
		return NULL;
	xdr->p = q;
	return p;
}
EXPORT_SYMBOL(xdr_inline_decode);

/**
 * xdr_read_pages - Ensure page-based XDR data to decode is aligned at current pointer position
 * @xdr: pointer to xdr_stream struct
 * @len: number of bytes of page data
 *
 * Moves data beyond the current pointer position from the XDR head[] buffer
 * into the page list. Any data that lies beyond current position + "len"
 * bytes is moved into the XDR tail[]. The current pointer is then
 * repositioned at the beginning of the XDR tail.
 */
void xdr_read_pages(struct xdr_stream *xdr, unsigned int len)
{
	struct xdr_buf *buf = xdr->buf;
	struct kvec *iov;
	ssize_t shift;
	unsigned int end;
	int padding;

	/* Realign pages to current pointer position */
	iov  = buf->head;
	shift = iov->iov_len + (char *)iov->iov_base - (char *)xdr->p;
	if (shift > 0)
		xdr_shrink_bufhead(buf, shift);

	/* Truncate page data and move it into the tail */
	if (buf->page_len > len)
		xdr_shrink_pagelen(buf, buf->page_len - len);
	padding = (XDR_QUADLEN(len) << 2) - len;
	xdr->iov = iov = buf->tail;
	/* Compute remaining message length.  */
	end = iov->iov_len;
	shift = buf->buflen - buf->len;
	if (shift < end)
		end -= shift;
	else if (shift > 0)
		end = 0;
	/*
	 * Position current pointer at beginning of tail, and
	 * set remaining message length.
	 */
	xdr->p = (uint32_t *)((char *)iov->iov_base + padding);
	xdr->end = (uint32_t *)((char *)iov->iov_base + end);
}
EXPORT_SYMBOL(xdr_read_pages);

static struct kvec empty_iov = {.iov_base = NULL, .iov_len = 0};

void
xdr_buf_from_iov(struct kvec *iov, struct xdr_buf *buf)
{
	buf->head[0] = *iov;
	buf->tail[0] = empty_iov;
	buf->page_len = 0;
	buf->buflen = buf->len = iov->iov_len;
}

/* Sets subiov to the intersection of iov with the buffer of length len
 * starting base bytes after iov.  Indicates empty intersection by setting
 * length of subiov to zero.  Decrements len by length of subiov, sets base
 * to zero (or decrements it by length of iov if subiov is empty). */
static void
iov_subsegment(struct kvec *iov, struct kvec *subiov, int *base, int *len)
{
	if (*base > iov->iov_len) {
		subiov->iov_base = NULL;
		subiov->iov_len = 0;
		*base -= iov->iov_len;
	} else {
		subiov->iov_base = iov->iov_base + *base;
		subiov->iov_len = min(*len, (int)iov->iov_len - *base);
		*base = 0;
	}
	*len -= subiov->iov_len; 
}

/* Sets subbuf to the portion of buf of length len beginning base bytes
 * from the start of buf. Returns -1 if base of length are out of bounds. */
int
xdr_buf_subsegment(struct xdr_buf *buf, struct xdr_buf *subbuf,
			int base, int len)
{
	int i;

	subbuf->buflen = subbuf->len = len;
	iov_subsegment(buf->head, subbuf->head, &base, &len);

	if (base < buf->page_len) {
		i = (base + buf->page_base) >> PAGE_CACHE_SHIFT;
		subbuf->pages = &buf->pages[i];
		subbuf->page_base = (base + buf->page_base) & ~PAGE_CACHE_MASK;
		subbuf->page_len = min((int)buf->page_len - base, len);
		len -= subbuf->page_len;
		base = 0;
	} else {
		base -= buf->page_len;
		subbuf->page_len = 0;
	}

	iov_subsegment(buf->tail, subbuf->tail, &base, &len);
	if (base || len)
		return -1;
	return 0;
}

/* obj is assumed to point to allocated memory of size at least len: */
int
read_bytes_from_xdr_buf(struct xdr_buf *buf, int base, void *obj, int len)
{
	struct xdr_buf subbuf;
	int this_len;
	int status;

	status = xdr_buf_subsegment(buf, &subbuf, base, len);
	if (status)
		goto out;
	this_len = min(len, (int)subbuf.head[0].iov_len);
	memcpy(obj, subbuf.head[0].iov_base, this_len);
	len -= this_len;
	obj += this_len;
	this_len = min(len, (int)subbuf.page_len);
	if (this_len)
		_copy_from_pages(obj, subbuf.pages, subbuf.page_base, this_len);
	len -= this_len;
	obj += this_len;
	this_len = min(len, (int)subbuf.tail[0].iov_len);
	memcpy(obj, subbuf.tail[0].iov_base, this_len);
out:
	return status;
}

static int
read_u32_from_xdr_buf(struct xdr_buf *buf, int base, u32 *obj)
{
	u32	raw;
	int	status;

	status = read_bytes_from_xdr_buf(buf, base, &raw, sizeof(*obj));
	if (status)
		return status;
	*obj = ntohl(raw);
	return 0;
}

/* If the netobj starting offset bytes from the start of xdr_buf is contained
 * entirely in the head or the tail, set object to point to it; otherwise
 * try to find space for it at the end of the tail, copy it there, and
 * set obj to point to it. */
int
xdr_buf_read_netobj(struct xdr_buf *buf, struct xdr_netobj *obj, int offset)
{
	u32	tail_offset = buf->head[0].iov_len + buf->page_len;
	u32	obj_end_offset;

	if (read_u32_from_xdr_buf(buf, offset, &obj->len))
		goto out;
	obj_end_offset = offset + 4 + obj->len;

	if (obj_end_offset <= buf->head[0].iov_len) {
		/* The obj is contained entirely in the head: */
		obj->data = buf->head[0].iov_base + offset + 4;
	} else if (offset + 4 >= tail_offset) {
		if (obj_end_offset - tail_offset
				> buf->tail[0].iov_len)
			goto out;
		/* The obj is contained entirely in the tail: */
		obj->data = buf->tail[0].iov_base
			+ offset - tail_offset + 4;
	} else {
		/* use end of tail as storage for obj:
		 * (We don't copy to the beginning because then we'd have
		 * to worry about doing a potentially overlapping copy.
		 * This assumes the object is at most half the length of the
		 * tail.) */
		if (obj->len > buf->tail[0].iov_len)
			goto out;
		obj->data = buf->tail[0].iov_base + buf->tail[0].iov_len - 
				obj->len;
		if (read_bytes_from_xdr_buf(buf, offset + 4,
					obj->data, obj->len))
			goto out;

	}
	return 0;
out:
	return -1;
}
