/*
 *	AX.25 release 037
 *
 *	This code REQUIRES 2.1.15 or higher/ NET3.038
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Most of this code is based on the SDL diagrams published in the 7th
 *	ARRL Computer Networking Conference papers. The diagrams have mistakes
 *	in them, but are mostly correct. Before you modify the code could you
 *	read the SDL diagrams as the code is not obvious and probably very
 *	easy to break;
 *
 *	History
 *	AX.25 036	Jonathan(G4KLX)	Split from ax25_subr.c.
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <net/ax25.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>

/*
 *	The null address is defined as a callsign of all spaces with an
 *	SSID of zero.
 */
ax25_address null_ax25_address = {{0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x00}};

/*
 *	ax25 -> ascii conversion
 */
char *ax2asc(ax25_address *a)
{
	static char buf[11];
	char c, *s;
	int n;

	for (n = 0, s = buf; n < 6; n++) {
		c = (a->ax25_call[n] >> 1) & 0x7F;

		if (c != ' ') *s++ = c;
	}

	*s++ = '-';

	if ((n = ((a->ax25_call[6] >> 1) & 0x0F)) > 9) {
		*s++ = '1';
		n -= 10;
	}

	*s++ = n + '0';
	*s++ = '\0';

	if (*buf == '\0' || *buf == '-')
	   return "*";

	return buf;

}

/*
 *	ascii -> ax25 conversion
 */
ax25_address *asc2ax(char *callsign)
{
	static ax25_address addr;
	char *s;
	int n;

	for (s = callsign, n = 0; n < 6; n++) {
		if (*s != '\0' && *s != '-')
			addr.ax25_call[n] = *s++;
		else
			addr.ax25_call[n] = ' ';
		addr.ax25_call[n] <<= 1;
		addr.ax25_call[n] &= 0xFE;
	}

	if (*s++ == '\0') {
		addr.ax25_call[6] = 0x00;
		return &addr;
	}

	addr.ax25_call[6] = *s++ - '0';

	if (*s != '\0') {
		addr.ax25_call[6] *= 10;
		addr.ax25_call[6] += *s++ - '0';
	}

	addr.ax25_call[6] <<= 1;
	addr.ax25_call[6] &= 0x1E;

	return &addr;
}

/*
 *	Compare two ax.25 addresses
 */
int ax25cmp(ax25_address *a, ax25_address *b)
{
	int ct = 0;

	while (ct < 6) {
		if ((a->ax25_call[ct] & 0xFE) != (b->ax25_call[ct] & 0xFE))	/* Clean off repeater bits */
			return 1;
		ct++;
	}

 	if ((a->ax25_call[ct] & 0x1E) == (b->ax25_call[ct] & 0x1E))	/* SSID without control bit */
 		return 0;

 	return 2;			/* Partial match */
}

/*
 *	Compare two AX.25 digipeater paths.
 */
int ax25digicmp(ax25_digi *digi1, ax25_digi *digi2)
{
	int i;

	if (digi1->ndigi != digi2->ndigi)
		return 1;

	if (digi1->lastrepeat != digi2->lastrepeat)
		return 1;

	for (i = 0; i < digi1->ndigi; i++)
		if (ax25cmp(&digi1->calls[i], &digi2->calls[i]) != 0)
			return 1;

	return 0;
}

/*
 *	Given an AX.25 address pull of to, from, digi list, command/response and the start of data
 *
 */
unsigned char *ax25_addr_parse(unsigned char *buf, int len, ax25_address *src, ax25_address *dest, ax25_digi *digi, int *flags, int *dama)
{
	int d = 0;

	if (len < 14) return NULL;

	if (flags != NULL) {
		*flags = 0;
		
		if (buf[6] & AX25_CBIT)
			*flags = AX25_COMMAND;
		if (buf[13] & AX25_CBIT)
			*flags = AX25_RESPONSE;
	}

	if (dama != NULL) 
		*dama = ~buf[13] & AX25_DAMA_FLAG;

	/* Copy to, from */
	if (dest != NULL)
		memcpy(dest, buf + 0, AX25_ADDR_LEN);
	if (src != NULL)
		memcpy(src,  buf + 7, AX25_ADDR_LEN);

	buf += 2 * AX25_ADDR_LEN;
	len -= 2 * AX25_ADDR_LEN;

	digi->lastrepeat = -1;
	digi->ndigi      = 0;

	while (!(buf[-1] & AX25_EBIT)) {
		if (d >= AX25_MAX_DIGIS)  return NULL;	/* Max of 6 digis */
		if (len < 7) return NULL;	/* Short packet */

		memcpy(&digi->calls[d], buf, AX25_ADDR_LEN);
		digi->ndigi = d + 1;

		if (buf[6] & AX25_HBIT) {
			digi->repeated[d] = 1;
			digi->lastrepeat  = d;
		} else {
			digi->repeated[d] = 0;
		}

		buf += AX25_ADDR_LEN;
		len -= AX25_ADDR_LEN;
		d++;
	}

	return buf;
}

/*
 *	Assemble an AX.25 header from the bits
 */
int ax25_addr_build(unsigned char *buf, ax25_address *src, ax25_address *dest, ax25_digi *d, int flag, int modulus)
{
	int len = 0;
	int ct  = 0;

	memcpy(buf, dest, AX25_ADDR_LEN);
	buf[6] &= ~(AX25_EBIT | AX25_CBIT);
	buf[6] |= AX25_SSSID_SPARE;

	if (flag == AX25_COMMAND) buf[6] |= AX25_CBIT;

	buf += AX25_ADDR_LEN;
	len += AX25_ADDR_LEN;

	memcpy(buf, src, AX25_ADDR_LEN);
	buf[6] &= ~(AX25_EBIT | AX25_CBIT);
	buf[6] &= ~AX25_SSSID_SPARE;

	if (modulus == AX25_MODULUS)
		buf[6] |= AX25_SSSID_SPARE;
	else
		buf[6] |= AX25_ESSID_SPARE;

	if (flag == AX25_RESPONSE) buf[6] |= AX25_CBIT;

	/*
	 *	Fast path the normal digiless path
	 */
	if (d == NULL || d->ndigi == 0) {
		buf[6] |= AX25_EBIT;
		return 2 * AX25_ADDR_LEN;
	}	

	buf += AX25_ADDR_LEN;
	len += AX25_ADDR_LEN;

	while (ct < d->ndigi) {
		memcpy(buf, &d->calls[ct], AX25_ADDR_LEN);

		if (d->repeated[ct])
			buf[6] |= AX25_HBIT;
		else
			buf[6] &= ~AX25_HBIT;

		buf[6] &= ~AX25_EBIT;
		buf[6] |= AX25_SSSID_SPARE;

		buf += AX25_ADDR_LEN;
		len += AX25_ADDR_LEN;
		ct++;
	}

	buf[-1] |= AX25_EBIT;

	return len;
}

int ax25_addr_size(ax25_digi *dp)
{
	if (dp == NULL)
		return 2 * AX25_ADDR_LEN;

	return AX25_ADDR_LEN * (2 + dp->ndigi);
}

/* 
 *	Reverse Digipeat List. May not pass both parameters as same struct
 */
void ax25_digi_invert(ax25_digi *in, ax25_digi *out)
{
	int ct;

	out->ndigi      = in->ndigi;
	out->lastrepeat = in->ndigi - in->lastrepeat - 2;

	/* Invert the digipeaters */
	for (ct = 0; ct < in->ndigi; ct++) {
		out->calls[ct] = in->calls[in->ndigi - ct - 1];

		if (ct <= out->lastrepeat) {
			out->calls[ct].ax25_call[6] |= AX25_HBIT;
			out->repeated[ct]            = 1;
		} else {
			out->calls[ct].ax25_call[6] &= ~AX25_HBIT;
			out->repeated[ct]            = 0;
		}
	}
}

