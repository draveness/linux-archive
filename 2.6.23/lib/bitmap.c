/*
 * lib/bitmap.c
 * Helper functions for bitmap.h.
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 */
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <asm/uaccess.h>

/*
 * bitmaps provide an array of bits, implemented using an an
 * array of unsigned longs.  The number of valid bits in a
 * given bitmap does _not_ need to be an exact multiple of
 * BITS_PER_LONG.
 *
 * The possible unused bits in the last, partially used word
 * of a bitmap are 'don't care'.  The implementation makes
 * no particular effort to keep them zero.  It ensures that
 * their value will not affect the results of any operation.
 * The bitmap operations that return Boolean (bitmap_empty,
 * for example) or scalar (bitmap_weight, for example) results
 * carefully filter out these unused bits from impacting their
 * results.
 *
 * These operations actually hold to a slightly stronger rule:
 * if you don't input any bitmaps to these ops that have some
 * unused bits set, then they won't output any set unused bits
 * in output bitmaps.
 *
 * The byte ordering of bitmaps is more natural on little
 * endian architectures.  See the big-endian headers
 * include/asm-ppc64/bitops.h and include/asm-s390/bitops.h
 * for the best explanations of this ordering.
 */

int __bitmap_empty(const unsigned long *bitmap, int bits)
{
	int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap[k])
			return 0;

	if (bits % BITS_PER_LONG)
		if (bitmap[k] & BITMAP_LAST_WORD_MASK(bits))
			return 0;

	return 1;
}
EXPORT_SYMBOL(__bitmap_empty);

int __bitmap_full(const unsigned long *bitmap, int bits)
{
	int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (~bitmap[k])
			return 0;

	if (bits % BITS_PER_LONG)
		if (~bitmap[k] & BITMAP_LAST_WORD_MASK(bits))
			return 0;

	return 1;
}
EXPORT_SYMBOL(__bitmap_full);

int __bitmap_equal(const unsigned long *bitmap1,
		const unsigned long *bitmap2, int bits)
{
	int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap1[k] != bitmap2[k])
			return 0;

	if (bits % BITS_PER_LONG)
		if ((bitmap1[k] ^ bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits))
			return 0;

	return 1;
}
EXPORT_SYMBOL(__bitmap_equal);

void __bitmap_complement(unsigned long *dst, const unsigned long *src, int bits)
{
	int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		dst[k] = ~src[k];

	if (bits % BITS_PER_LONG)
		dst[k] = ~src[k] & BITMAP_LAST_WORD_MASK(bits);
}
EXPORT_SYMBOL(__bitmap_complement);

/**
 * __bitmap_shift_right - logical right shift of the bits in a bitmap
 *   @dst : destination bitmap
 *   @src : source bitmap
 *   @shift : shift by this many bits
 *   @bits : bitmap size, in bits
 *
 * Shifting right (dividing) means moving bits in the MS -> LS bit
 * direction.  Zeros are fed into the vacated MS positions and the
 * LS bits shifted off the bottom are lost.
 */
void __bitmap_shift_right(unsigned long *dst,
			const unsigned long *src, int shift, int bits)
{
	int k, lim = BITS_TO_LONGS(bits), left = bits % BITS_PER_LONG;
	int off = shift/BITS_PER_LONG, rem = shift % BITS_PER_LONG;
	unsigned long mask = (1UL << left) - 1;
	for (k = 0; off + k < lim; ++k) {
		unsigned long upper, lower;

		/*
		 * If shift is not word aligned, take lower rem bits of
		 * word above and make them the top rem bits of result.
		 */
		if (!rem || off + k + 1 >= lim)
			upper = 0;
		else {
			upper = src[off + k + 1];
			if (off + k + 1 == lim - 1 && left)
				upper &= mask;
		}
		lower = src[off + k];
		if (left && off + k == lim - 1)
			lower &= mask;
		dst[k] = upper << (BITS_PER_LONG - rem) | lower >> rem;
		if (left && k == lim - 1)
			dst[k] &= mask;
	}
	if (off)
		memset(&dst[lim - off], 0, off*sizeof(unsigned long));
}
EXPORT_SYMBOL(__bitmap_shift_right);


/**
 * __bitmap_shift_left - logical left shift of the bits in a bitmap
 *   @dst : destination bitmap
 *   @src : source bitmap
 *   @shift : shift by this many bits
 *   @bits : bitmap size, in bits
 *
 * Shifting left (multiplying) means moving bits in the LS -> MS
 * direction.  Zeros are fed into the vacated LS bit positions
 * and those MS bits shifted off the top are lost.
 */

void __bitmap_shift_left(unsigned long *dst,
			const unsigned long *src, int shift, int bits)
{
	int k, lim = BITS_TO_LONGS(bits), left = bits % BITS_PER_LONG;
	int off = shift/BITS_PER_LONG, rem = shift % BITS_PER_LONG;
	for (k = lim - off - 1; k >= 0; --k) {
		unsigned long upper, lower;

		/*
		 * If shift is not word aligned, take upper rem bits of
		 * word below and make them the bottom rem bits of result.
		 */
		if (rem && k > 0)
			lower = src[k - 1];
		else
			lower = 0;
		upper = src[k];
		if (left && k == lim - 1)
			upper &= (1UL << left) - 1;
		dst[k + off] = lower  >> (BITS_PER_LONG - rem) | upper << rem;
		if (left && k + off == lim - 1)
			dst[k + off] &= (1UL << left) - 1;
	}
	if (off)
		memset(dst, 0, off*sizeof(unsigned long));
}
EXPORT_SYMBOL(__bitmap_shift_left);

void __bitmap_and(unsigned long *dst, const unsigned long *bitmap1,
				const unsigned long *bitmap2, int bits)
{
	int k;
	int nr = BITS_TO_LONGS(bits);

	for (k = 0; k < nr; k++)
		dst[k] = bitmap1[k] & bitmap2[k];
}
EXPORT_SYMBOL(__bitmap_and);

void __bitmap_or(unsigned long *dst, const unsigned long *bitmap1,
				const unsigned long *bitmap2, int bits)
{
	int k;
	int nr = BITS_TO_LONGS(bits);

	for (k = 0; k < nr; k++)
		dst[k] = bitmap1[k] | bitmap2[k];
}
EXPORT_SYMBOL(__bitmap_or);

void __bitmap_xor(unsigned long *dst, const unsigned long *bitmap1,
				const unsigned long *bitmap2, int bits)
{
	int k;
	int nr = BITS_TO_LONGS(bits);

	for (k = 0; k < nr; k++)
		dst[k] = bitmap1[k] ^ bitmap2[k];
}
EXPORT_SYMBOL(__bitmap_xor);

void __bitmap_andnot(unsigned long *dst, const unsigned long *bitmap1,
				const unsigned long *bitmap2, int bits)
{
	int k;
	int nr = BITS_TO_LONGS(bits);

	for (k = 0; k < nr; k++)
		dst[k] = bitmap1[k] & ~bitmap2[k];
}
EXPORT_SYMBOL(__bitmap_andnot);

int __bitmap_intersects(const unsigned long *bitmap1,
				const unsigned long *bitmap2, int bits)
{
	int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap1[k] & bitmap2[k])
			return 1;

	if (bits % BITS_PER_LONG)
		if ((bitmap1[k] & bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits))
			return 1;
	return 0;
}
EXPORT_SYMBOL(__bitmap_intersects);

int __bitmap_subset(const unsigned long *bitmap1,
				const unsigned long *bitmap2, int bits)
{
	int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap1[k] & ~bitmap2[k])
			return 0;

	if (bits % BITS_PER_LONG)
		if ((bitmap1[k] & ~bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits))
			return 0;
	return 1;
}
EXPORT_SYMBOL(__bitmap_subset);

int __bitmap_weight(const unsigned long *bitmap, int bits)
{
	int k, w = 0, lim = bits/BITS_PER_LONG;

	for (k = 0; k < lim; k++)
		w += hweight_long(bitmap[k]);

	if (bits % BITS_PER_LONG)
		w += hweight_long(bitmap[k] & BITMAP_LAST_WORD_MASK(bits));

	return w;
}
EXPORT_SYMBOL(__bitmap_weight);

/*
 * Bitmap printing & parsing functions: first version by Bill Irwin,
 * second version by Paul Jackson, third by Joe Korty.
 */

#define CHUNKSZ				32
#define nbits_to_hold_value(val)	fls(val)
#define unhex(c)			(isdigit(c) ? (c - '0') : (toupper(c) - 'A' + 10))
#define BASEDEC 10		/* fancier cpuset lists input in decimal */

/**
 * bitmap_scnprintf - convert bitmap to an ASCII hex string.
 * @buf: byte buffer into which string is placed
 * @buflen: reserved size of @buf, in bytes
 * @maskp: pointer to bitmap to convert
 * @nmaskbits: size of bitmap, in bits
 *
 * Exactly @nmaskbits bits are displayed.  Hex digits are grouped into
 * comma-separated sets of eight digits per set.
 */
int bitmap_scnprintf(char *buf, unsigned int buflen,
	const unsigned long *maskp, int nmaskbits)
{
	int i, word, bit, len = 0;
	unsigned long val;
	const char *sep = "";
	int chunksz;
	u32 chunkmask;

	chunksz = nmaskbits & (CHUNKSZ - 1);
	if (chunksz == 0)
		chunksz = CHUNKSZ;

	i = ALIGN(nmaskbits, CHUNKSZ) - CHUNKSZ;
	for (; i >= 0; i -= CHUNKSZ) {
		chunkmask = ((1ULL << chunksz) - 1);
		word = i / BITS_PER_LONG;
		bit = i % BITS_PER_LONG;
		val = (maskp[word] >> bit) & chunkmask;
		len += scnprintf(buf+len, buflen-len, "%s%0*lx", sep,
			(chunksz+3)/4, val);
		chunksz = CHUNKSZ;
		sep = ",";
	}
	return len;
}
EXPORT_SYMBOL(bitmap_scnprintf);

/**
 * __bitmap_parse - convert an ASCII hex string into a bitmap.
 * @buf: pointer to buffer containing string.
 * @buflen: buffer size in bytes.  If string is smaller than this
 *    then it must be terminated with a \0.
 * @is_user: location of buffer, 0 indicates kernel space
 * @maskp: pointer to bitmap array that will contain result.
 * @nmaskbits: size of bitmap, in bits.
 *
 * Commas group hex digits into chunks.  Each chunk defines exactly 32
 * bits of the resultant bitmask.  No chunk may specify a value larger
 * than 32 bits (%-EOVERFLOW), and if a chunk specifies a smaller value
 * then leading 0-bits are prepended.  %-EINVAL is returned for illegal
 * characters and for grouping errors such as "1,,5", ",44", "," and "".
 * Leading and trailing whitespace accepted, but not embedded whitespace.
 */
int __bitmap_parse(const char *buf, unsigned int buflen,
		int is_user, unsigned long *maskp,
		int nmaskbits)
{
	int c, old_c, totaldigits, ndigits, nchunks, nbits;
	u32 chunk;
	const char __user *ubuf = buf;

	bitmap_zero(maskp, nmaskbits);

	nchunks = nbits = totaldigits = c = 0;
	do {
		chunk = ndigits = 0;

		/* Get the next chunk of the bitmap */
		while (buflen) {
			old_c = c;
			if (is_user) {
				if (__get_user(c, ubuf++))
					return -EFAULT;
			}
			else
				c = *buf++;
			buflen--;
			if (isspace(c))
				continue;

			/*
			 * If the last character was a space and the current
			 * character isn't '\0', we've got embedded whitespace.
			 * This is a no-no, so throw an error.
			 */
			if (totaldigits && c && isspace(old_c))
				return -EINVAL;

			/* A '\0' or a ',' signal the end of the chunk */
			if (c == '\0' || c == ',')
				break;

			if (!isxdigit(c))
				return -EINVAL;

			/*
			 * Make sure there are at least 4 free bits in 'chunk'.
			 * If not, this hexdigit will overflow 'chunk', so
			 * throw an error.
			 */
			if (chunk & ~((1UL << (CHUNKSZ - 4)) - 1))
				return -EOVERFLOW;

			chunk = (chunk << 4) | unhex(c);
			ndigits++; totaldigits++;
		}
		if (ndigits == 0)
			return -EINVAL;
		if (nchunks == 0 && chunk == 0)
			continue;

		__bitmap_shift_left(maskp, maskp, CHUNKSZ, nmaskbits);
		*maskp |= chunk;
		nchunks++;
		nbits += (nchunks == 1) ? nbits_to_hold_value(chunk) : CHUNKSZ;
		if (nbits > nmaskbits)
			return -EOVERFLOW;
	} while (buflen && c == ',');

	return 0;
}
EXPORT_SYMBOL(__bitmap_parse);

/**
 * bitmap_parse_user()
 *
 * @ubuf: pointer to user buffer containing string.
 * @ulen: buffer size in bytes.  If string is smaller than this
 *    then it must be terminated with a \0.
 * @maskp: pointer to bitmap array that will contain result.
 * @nmaskbits: size of bitmap, in bits.
 *
 * Wrapper for __bitmap_parse(), providing it with user buffer.
 *
 * We cannot have this as an inline function in bitmap.h because it needs
 * linux/uaccess.h to get the access_ok() declaration and this causes
 * cyclic dependencies.
 */
int bitmap_parse_user(const char __user *ubuf,
			unsigned int ulen, unsigned long *maskp,
			int nmaskbits)
{
	if (!access_ok(VERIFY_READ, ubuf, ulen))
		return -EFAULT;
	return __bitmap_parse((const char *)ubuf, ulen, 1, maskp, nmaskbits);
}
EXPORT_SYMBOL(bitmap_parse_user);

/*
 * bscnl_emit(buf, buflen, rbot, rtop, bp)
 *
 * Helper routine for bitmap_scnlistprintf().  Write decimal number
 * or range to buf, suppressing output past buf+buflen, with optional
 * comma-prefix.  Return len of what would be written to buf, if it
 * all fit.
 */
static inline int bscnl_emit(char *buf, int buflen, int rbot, int rtop, int len)
{
	if (len > 0)
		len += scnprintf(buf + len, buflen - len, ",");
	if (rbot == rtop)
		len += scnprintf(buf + len, buflen - len, "%d", rbot);
	else
		len += scnprintf(buf + len, buflen - len, "%d-%d", rbot, rtop);
	return len;
}

/**
 * bitmap_scnlistprintf - convert bitmap to list format ASCII string
 * @buf: byte buffer into which string is placed
 * @buflen: reserved size of @buf, in bytes
 * @maskp: pointer to bitmap to convert
 * @nmaskbits: size of bitmap, in bits
 *
 * Output format is a comma-separated list of decimal numbers and
 * ranges.  Consecutively set bits are shown as two hyphen-separated
 * decimal numbers, the smallest and largest bit numbers set in
 * the range.  Output format is compatible with the format
 * accepted as input by bitmap_parselist().
 *
 * The return value is the number of characters which would be
 * generated for the given input, excluding the trailing '\0', as
 * per ISO C99.
 */
int bitmap_scnlistprintf(char *buf, unsigned int buflen,
	const unsigned long *maskp, int nmaskbits)
{
	int len = 0;
	/* current bit is 'cur', most recently seen range is [rbot, rtop] */
	int cur, rbot, rtop;

	rbot = cur = find_first_bit(maskp, nmaskbits);
	while (cur < nmaskbits) {
		rtop = cur;
		cur = find_next_bit(maskp, nmaskbits, cur+1);
		if (cur >= nmaskbits || cur > rtop + 1) {
			len = bscnl_emit(buf, buflen, rbot, rtop, len);
			rbot = cur;
		}
	}
	return len;
}
EXPORT_SYMBOL(bitmap_scnlistprintf);

/**
 * bitmap_parselist - convert list format ASCII string to bitmap
 * @bp: read nul-terminated user string from this buffer
 * @maskp: write resulting mask here
 * @nmaskbits: number of bits in mask to be written
 *
 * Input format is a comma-separated list of decimal numbers and
 * ranges.  Consecutively set bits are shown as two hyphen-separated
 * decimal numbers, the smallest and largest bit numbers set in
 * the range.
 *
 * Returns 0 on success, -errno on invalid input strings.
 * Error values:
 *    %-EINVAL: second number in range smaller than first
 *    %-EINVAL: invalid character in string
 *    %-ERANGE: bit number specified too large for mask
 */
int bitmap_parselist(const char *bp, unsigned long *maskp, int nmaskbits)
{
	unsigned a, b;

	bitmap_zero(maskp, nmaskbits);
	do {
		if (!isdigit(*bp))
			return -EINVAL;
		b = a = simple_strtoul(bp, (char **)&bp, BASEDEC);
		if (*bp == '-') {
			bp++;
			if (!isdigit(*bp))
				return -EINVAL;
			b = simple_strtoul(bp, (char **)&bp, BASEDEC);
		}
		if (!(a <= b))
			return -EINVAL;
		if (b >= nmaskbits)
			return -ERANGE;
		while (a <= b) {
			set_bit(a, maskp);
			a++;
		}
		if (*bp == ',')
			bp++;
	} while (*bp != '\0' && *bp != '\n');
	return 0;
}
EXPORT_SYMBOL(bitmap_parselist);

/**
 * bitmap_pos_to_ord(buf, pos, bits)
 *	@buf: pointer to a bitmap
 *	@pos: a bit position in @buf (0 <= @pos < @bits)
 *	@bits: number of valid bit positions in @buf
 *
 * Map the bit at position @pos in @buf (of length @bits) to the
 * ordinal of which set bit it is.  If it is not set or if @pos
 * is not a valid bit position, map to -1.
 *
 * If for example, just bits 4 through 7 are set in @buf, then @pos
 * values 4 through 7 will get mapped to 0 through 3, respectively,
 * and other @pos values will get mapped to 0.  When @pos value 7
 * gets mapped to (returns) @ord value 3 in this example, that means
 * that bit 7 is the 3rd (starting with 0th) set bit in @buf.
 *
 * The bit positions 0 through @bits are valid positions in @buf.
 */
static int bitmap_pos_to_ord(const unsigned long *buf, int pos, int bits)
{
	int i, ord;

	if (pos < 0 || pos >= bits || !test_bit(pos, buf))
		return -1;

	i = find_first_bit(buf, bits);
	ord = 0;
	while (i < pos) {
		i = find_next_bit(buf, bits, i + 1);
	     	ord++;
	}
	BUG_ON(i != pos);

	return ord;
}

/**
 * bitmap_ord_to_pos(buf, ord, bits)
 *	@buf: pointer to bitmap
 *	@ord: ordinal bit position (n-th set bit, n >= 0)
 *	@bits: number of valid bit positions in @buf
 *
 * Map the ordinal offset of bit @ord in @buf to its position in @buf.
 * Value of @ord should be in range 0 <= @ord < weight(buf), else
 * results are undefined.
 *
 * If for example, just bits 4 through 7 are set in @buf, then @ord
 * values 0 through 3 will get mapped to 4 through 7, respectively,
 * and all other @ord values return undefined values.  When @ord value 3
 * gets mapped to (returns) @pos value 7 in this example, that means
 * that the 3rd set bit (starting with 0th) is at position 7 in @buf.
 *
 * The bit positions 0 through @bits are valid positions in @buf.
 */
static int bitmap_ord_to_pos(const unsigned long *buf, int ord, int bits)
{
	int pos = 0;

	if (ord >= 0 && ord < bits) {
		int i;

		for (i = find_first_bit(buf, bits);
		     i < bits && ord > 0;
		     i = find_next_bit(buf, bits, i + 1))
	     		ord--;
		if (i < bits && ord == 0)
			pos = i;
	}

	return pos;
}

/**
 * bitmap_remap - Apply map defined by a pair of bitmaps to another bitmap
 *	@dst: remapped result
 *	@src: subset to be remapped
 *	@old: defines domain of map
 *	@new: defines range of map
 *	@bits: number of bits in each of these bitmaps
 *
 * Let @old and @new define a mapping of bit positions, such that
 * whatever position is held by the n-th set bit in @old is mapped
 * to the n-th set bit in @new.  In the more general case, allowing
 * for the possibility that the weight 'w' of @new is less than the
 * weight of @old, map the position of the n-th set bit in @old to
 * the position of the m-th set bit in @new, where m == n % w.
 *
 * If either of the @old and @new bitmaps are empty, or if @src and
 * @dst point to the same location, then this routine copies @src
 * to @dst.
 *
 * The positions of unset bits in @old are mapped to themselves
 * (the identify map).
 *
 * Apply the above specified mapping to @src, placing the result in
 * @dst, clearing any bits previously set in @dst.
 *
 * For example, lets say that @old has bits 4 through 7 set, and
 * @new has bits 12 through 15 set.  This defines the mapping of bit
 * position 4 to 12, 5 to 13, 6 to 14 and 7 to 15, and of all other
 * bit positions unchanged.  So if say @src comes into this routine
 * with bits 1, 5 and 7 set, then @dst should leave with bits 1,
 * 13 and 15 set.
 */
void bitmap_remap(unsigned long *dst, const unsigned long *src,
		const unsigned long *old, const unsigned long *new,
		int bits)
{
	int oldbit, w;

	if (dst == src)		/* following doesn't handle inplace remaps */
		return;
	bitmap_zero(dst, bits);

	w = bitmap_weight(new, bits);
	for (oldbit = find_first_bit(src, bits);
	     oldbit < bits;
	     oldbit = find_next_bit(src, bits, oldbit + 1)) {
	     	int n = bitmap_pos_to_ord(old, oldbit, bits);
		if (n < 0 || w == 0)
			set_bit(oldbit, dst);	/* identity map */
		else
			set_bit(bitmap_ord_to_pos(new, n % w, bits), dst);
	}
}
EXPORT_SYMBOL(bitmap_remap);

/**
 * bitmap_bitremap - Apply map defined by a pair of bitmaps to a single bit
 *	@oldbit: bit position to be mapped
 *	@old: defines domain of map
 *	@new: defines range of map
 *	@bits: number of bits in each of these bitmaps
 *
 * Let @old and @new define a mapping of bit positions, such that
 * whatever position is held by the n-th set bit in @old is mapped
 * to the n-th set bit in @new.  In the more general case, allowing
 * for the possibility that the weight 'w' of @new is less than the
 * weight of @old, map the position of the n-th set bit in @old to
 * the position of the m-th set bit in @new, where m == n % w.
 *
 * The positions of unset bits in @old are mapped to themselves
 * (the identify map).
 *
 * Apply the above specified mapping to bit position @oldbit, returning
 * the new bit position.
 *
 * For example, lets say that @old has bits 4 through 7 set, and
 * @new has bits 12 through 15 set.  This defines the mapping of bit
 * position 4 to 12, 5 to 13, 6 to 14 and 7 to 15, and of all other
 * bit positions unchanged.  So if say @oldbit is 5, then this routine
 * returns 13.
 */
int bitmap_bitremap(int oldbit, const unsigned long *old,
				const unsigned long *new, int bits)
{
	int w = bitmap_weight(new, bits);
	int n = bitmap_pos_to_ord(old, oldbit, bits);
	if (n < 0 || w == 0)
		return oldbit;
	else
		return bitmap_ord_to_pos(new, n % w, bits);
}
EXPORT_SYMBOL(bitmap_bitremap);

/*
 * Common code for bitmap_*_region() routines.
 *	bitmap: array of unsigned longs corresponding to the bitmap
 *	pos: the beginning of the region
 *	order: region size (log base 2 of number of bits)
 *	reg_op: operation(s) to perform on that region of bitmap
 *
 * Can set, verify and/or release a region of bits in a bitmap,
 * depending on which combination of REG_OP_* flag bits is set.
 *
 * A region of a bitmap is a sequence of bits in the bitmap, of
 * some size '1 << order' (a power of two), aligned to that same
 * '1 << order' power of two.
 *
 * Returns 1 if REG_OP_ISFREE succeeds (region is all zero bits).
 * Returns 0 in all other cases and reg_ops.
 */

enum {
	REG_OP_ISFREE,		/* true if region is all zero bits */
	REG_OP_ALLOC,		/* set all bits in region */
	REG_OP_RELEASE,		/* clear all bits in region */
};

static int __reg_op(unsigned long *bitmap, int pos, int order, int reg_op)
{
	int nbits_reg;		/* number of bits in region */
	int index;		/* index first long of region in bitmap */
	int offset;		/* bit offset region in bitmap[index] */
	int nlongs_reg;		/* num longs spanned by region in bitmap */
	int nbitsinlong;	/* num bits of region in each spanned long */
	unsigned long mask;	/* bitmask for one long of region */
	int i;			/* scans bitmap by longs */
	int ret = 0;		/* return value */

	/*
	 * Either nlongs_reg == 1 (for small orders that fit in one long)
	 * or (offset == 0 && mask == ~0UL) (for larger multiword orders.)
	 */
	nbits_reg = 1 << order;
	index = pos / BITS_PER_LONG;
	offset = pos - (index * BITS_PER_LONG);
	nlongs_reg = BITS_TO_LONGS(nbits_reg);
	nbitsinlong = min(nbits_reg,  BITS_PER_LONG);

	/*
	 * Can't do "mask = (1UL << nbitsinlong) - 1", as that
	 * overflows if nbitsinlong == BITS_PER_LONG.
	 */
	mask = (1UL << (nbitsinlong - 1));
	mask += mask - 1;
	mask <<= offset;

	switch (reg_op) {
	case REG_OP_ISFREE:
		for (i = 0; i < nlongs_reg; i++) {
			if (bitmap[index + i] & mask)
				goto done;
		}
		ret = 1;	/* all bits in region free (zero) */
		break;

	case REG_OP_ALLOC:
		for (i = 0; i < nlongs_reg; i++)
			bitmap[index + i] |= mask;
		break;

	case REG_OP_RELEASE:
		for (i = 0; i < nlongs_reg; i++)
			bitmap[index + i] &= ~mask;
		break;
	}
done:
	return ret;
}

/**
 * bitmap_find_free_region - find a contiguous aligned mem region
 *	@bitmap: array of unsigned longs corresponding to the bitmap
 *	@bits: number of bits in the bitmap
 *	@order: region size (log base 2 of number of bits) to find
 *
 * Find a region of free (zero) bits in a @bitmap of @bits bits and
 * allocate them (set them to one).  Only consider regions of length
 * a power (@order) of two, aligned to that power of two, which
 * makes the search algorithm much faster.
 *
 * Return the bit offset in bitmap of the allocated region,
 * or -errno on failure.
 */
int bitmap_find_free_region(unsigned long *bitmap, int bits, int order)
{
	int pos;		/* scans bitmap by regions of size order */

	for (pos = 0; pos < bits; pos += (1 << order))
		if (__reg_op(bitmap, pos, order, REG_OP_ISFREE))
			break;
	if (pos == bits)
		return -ENOMEM;
	__reg_op(bitmap, pos, order, REG_OP_ALLOC);
	return pos;
}
EXPORT_SYMBOL(bitmap_find_free_region);

/**
 * bitmap_release_region - release allocated bitmap region
 *	@bitmap: array of unsigned longs corresponding to the bitmap
 *	@pos: beginning of bit region to release
 *	@order: region size (log base 2 of number of bits) to release
 *
 * This is the complement to __bitmap_find_free_region() and releases
 * the found region (by clearing it in the bitmap).
 *
 * No return value.
 */
void bitmap_release_region(unsigned long *bitmap, int pos, int order)
{
	__reg_op(bitmap, pos, order, REG_OP_RELEASE);
}
EXPORT_SYMBOL(bitmap_release_region);

/**
 * bitmap_allocate_region - allocate bitmap region
 *	@bitmap: array of unsigned longs corresponding to the bitmap
 *	@pos: beginning of bit region to allocate
 *	@order: region size (log base 2 of number of bits) to allocate
 *
 * Allocate (set bits in) a specified region of a bitmap.
 *
 * Return 0 on success, or %-EBUSY if specified region wasn't
 * free (not all bits were zero).
 */
int bitmap_allocate_region(unsigned long *bitmap, int pos, int order)
{
	if (!__reg_op(bitmap, pos, order, REG_OP_ISFREE))
		return -EBUSY;
	__reg_op(bitmap, pos, order, REG_OP_ALLOC);
	return 0;
}
EXPORT_SYMBOL(bitmap_allocate_region);
