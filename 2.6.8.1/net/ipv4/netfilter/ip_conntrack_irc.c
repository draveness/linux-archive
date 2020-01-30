/* IRC extension for IP connection tracking, Version 1.21
 * (C) 2000-2002 by Harald Welte <laforge@gnumonks.org>
 * based on RR's ip_conntrack_ftp.c	
 *
 * ip_conntrack_irc.c,v 1.21 2002/02/05 14:49:26 laforge Exp
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 **
 *	Module load syntax:
 * 	insmod ip_conntrack_irc.o ports=port1,port2,...port<MAX_PORTS>
 *			    max_dcc_channels=n dcc_timeout=secs
 *	
 * 	please give the ports of all IRC servers You wish to connect to.
 *	If You don't specify ports, the default will be port 6667.
 *	With max_dcc_channels you can define the maximum number of not
 *	yet answered DCC channels per IRC session (default 8).
 *	With dcc_timeout you can specify how long the system waits for 
 *	an expected DCC channel (default 300 seconds).
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/ip.h>
#include <net/checksum.h>
#include <net/tcp.h>

#include <linux/netfilter_ipv4/lockhelp.h>
#include <linux/netfilter_ipv4/ip_conntrack_helper.h>
#include <linux/netfilter_ipv4/ip_conntrack_irc.h>

#define MAX_PORTS 8
static int ports[MAX_PORTS];
static int ports_c;
static int max_dcc_channels = 8;
static unsigned int dcc_timeout = 300;
/* This is slow, but it's simple. --RR */
static char irc_buffer[65536];

MODULE_AUTHOR("Harald Welte <laforge@netfilter.org>");
MODULE_DESCRIPTION("IRC (DCC) connection tracking helper");
MODULE_LICENSE("GPL");
MODULE_PARM(ports, "1-" __MODULE_STRING(MAX_PORTS) "i");
MODULE_PARM_DESC(ports, "port numbers of IRC servers");
MODULE_PARM(max_dcc_channels, "i");
MODULE_PARM_DESC(max_dcc_channels, "max number of expected DCC channels per IRC session");
MODULE_PARM(dcc_timeout, "i");
MODULE_PARM_DESC(dcc_timeout, "timeout on for unestablished DCC channels");

static char *dccprotos[] = { "SEND ", "CHAT ", "MOVE ", "TSEND ", "SCHAT " };
#define MINMATCHLEN	5

DECLARE_LOCK(ip_irc_lock);
struct module *ip_conntrack_irc = THIS_MODULE;

#if 0
#define DEBUGP(format, args...) printk(KERN_DEBUG "%s:%s:" format, \
                                       __FILE__, __FUNCTION__ , ## args)
#else
#define DEBUGP(format, args...)
#endif

int parse_dcc(char *data, char *data_end, u_int32_t * ip, u_int16_t * port,
	      char **ad_beg_p, char **ad_end_p)
/* tries to get the ip_addr and port out of a dcc command
   return value: -1 on failure, 0 on success 
	data		pointer to first byte of DCC command data
	data_end	pointer to last byte of dcc command data
	ip		returns parsed ip of dcc command
	port		returns parsed port of dcc command
	ad_beg_p	returns pointer to first byte of addr data
	ad_end_p	returns pointer to last byte of addr data */
{

	/* at least 12: "AAAAAAAA P\1\n" */
	while (*data++ != ' ')
		if (data > data_end - 12)
			return -1;

	*ad_beg_p = data;
	*ip = simple_strtoul(data, &data, 10);

	/* skip blanks between ip and port */
	while (*data == ' ') {
		if (data >= data_end) 
			return -1;
		data++;
	}

	*port = simple_strtoul(data, &data, 10);
	*ad_end_p = data;

	return 0;
}

static int help(struct sk_buff *skb,
		struct ip_conntrack *ct, enum ip_conntrack_info ctinfo)
{
	unsigned int dataoff;
	struct tcphdr tcph;
	char *data, *data_limit;
	int dir = CTINFO2DIR(ctinfo);
	struct ip_conntrack_expect *exp;
	struct ip_ct_irc_expect *exp_irc_info = NULL;

	u_int32_t dcc_ip;
	u_int16_t dcc_port;
	int i;
	char *addr_beg_p, *addr_end_p;

	DEBUGP("entered\n");

	/* If packet is coming from IRC server */
	if (dir == IP_CT_DIR_REPLY)
		return NF_ACCEPT;

	/* Until there's been traffic both ways, don't look in packets. */
	if (ctinfo != IP_CT_ESTABLISHED
	    && ctinfo != IP_CT_ESTABLISHED + IP_CT_IS_REPLY) {
		DEBUGP("Conntrackinfo = %u\n", ctinfo);
		return NF_ACCEPT;
	}

	/* Not a full tcp header? */
	if (skb_copy_bits(skb, skb->nh.iph->ihl*4, &tcph, sizeof(tcph)) != 0)
		return NF_ACCEPT;

	/* No data? */
	dataoff = skb->nh.iph->ihl*4 + tcph.doff*4;
	if (dataoff >= skb->len)
		return NF_ACCEPT;

	LOCK_BH(&ip_irc_lock);
	skb_copy_bits(skb, dataoff, irc_buffer, skb->len - dataoff);

	data = irc_buffer;
	data_limit = irc_buffer + skb->len - dataoff;

	/* strlen("\1DCC SENT t AAAAAAAA P\1\n")=24
	 * 5+MINMATCHLEN+strlen("t AAAAAAAA P\1\n")=14 */
	while (data < (data_limit - (19 + MINMATCHLEN))) {
		if (memcmp(data, "\1DCC ", 5)) {
			data++;
			continue;
		}

		data += 5;
		/* we have at least (19+MINMATCHLEN)-5 bytes valid data left */

		DEBUGP("DCC found in master %u.%u.%u.%u:%u %u.%u.%u.%u:%u...\n",
			NIPQUAD(iph->saddr), ntohs(tcph.source),
			NIPQUAD(iph->daddr), ntohs(tcph.dest));

		for (i = 0; i < ARRAY_SIZE(dccprotos); i++) {
			if (memcmp(data, dccprotos[i], strlen(dccprotos[i]))) {
				/* no match */
				continue;
			}

			DEBUGP("DCC %s detected\n", dccprotos[i]);
			data += strlen(dccprotos[i]);
			/* we have at least 
			 * (19+MINMATCHLEN)-5-dccprotos[i].matchlen bytes valid
			 * data left (== 14/13 bytes) */
			if (parse_dcc((char *)data, data_limit, &dcc_ip,
				       &dcc_port, &addr_beg_p, &addr_end_p)) {
				/* unable to parse */
				DEBUGP("unable to parse dcc command\n");
				continue;
			}
			DEBUGP("DCC bound ip/port: %u.%u.%u.%u:%u\n",
				HIPQUAD(dcc_ip), dcc_port);

			/* dcc_ip can be the internal OR external (NAT'ed) IP
			 * Tiago Sousa <mirage@kaotik.org> */
			if (ct->tuplehash[dir].tuple.src.ip != htonl(dcc_ip)
			    && ct->tuplehash[IP_CT_DIR_REPLY].tuple.dst.ip != htonl(dcc_ip)) {
				if (net_ratelimit())
					printk(KERN_WARNING
						"Forged DCC command from "
						"%u.%u.%u.%u: %u.%u.%u.%u:%u\n",
				NIPQUAD(ct->tuplehash[dir].tuple.src.ip),
						HIPQUAD(dcc_ip), dcc_port);

				continue;
			}

			exp = ip_conntrack_expect_alloc();
			if (exp == NULL)
				goto out;

			exp_irc_info = &exp->help.exp_irc_info;

			/* save position of address in dcc string,
			 * necessary for NAT */
			DEBUGP("tcph->seq = %u\n", tcph.seq);
			exp->seq = ntohl(tcph.seq) + (addr_beg_p - irc_buffer);
			exp_irc_info->len = (addr_end_p - addr_beg_p);
			exp_irc_info->port = dcc_port;
			DEBUGP("wrote info seq=%u (ofs=%u), len=%d\n",
				exp->seq, (addr_end_p - _data), exp_irc_info->len);

			exp->tuple = ((struct ip_conntrack_tuple)
				{ { 0, { 0 } },
				  { ct->tuplehash[dir].tuple.src.ip, { .tcp = { htons(dcc_port) } },
				    IPPROTO_TCP }});
			exp->mask = ((struct ip_conntrack_tuple)
				{ { 0, { 0 } },
				  { 0xFFFFFFFF, { .tcp = { 0xFFFF } }, 0xFFFF }});

			exp->expectfn = NULL;

			DEBUGP("expect_related %u.%u.%u.%u:%u-%u.%u.%u.%u:%u\n",
				NIPQUAD(exp->tuple.src.ip),
				ntohs(exp->tuple.src.u.tcp.port),
				NIPQUAD(exp->tuple.dst.ip),
				ntohs(exp->tuple.dst.u.tcp.port));

			ip_conntrack_expect_related(exp, ct);

			goto out;
		} /* for .. NUM_DCCPROTO */
	} /* while data < ... */

 out:
	UNLOCK_BH(&ip_irc_lock);
	return NF_ACCEPT;
}

static struct ip_conntrack_helper irc_helpers[MAX_PORTS];
static char irc_names[MAX_PORTS][10];

static void fini(void);

static int __init init(void)
{
	int i, ret;
	struct ip_conntrack_helper *hlpr;
	char *tmpname;

	if (max_dcc_channels < 1) {
		printk("ip_conntrack_irc: max_dcc_channels must be a positive integer\n");
		return -EBUSY;
	}
	if (dcc_timeout < 0) {
		printk("ip_conntrack_irc: dcc_timeout must be a positive integer\n");
		return -EBUSY;
	}
	
	/* If no port given, default to standard irc port */
	if (ports[0] == 0)
		ports[0] = IRC_PORT;

	for (i = 0; (i < MAX_PORTS) && ports[i]; i++) {
		hlpr = &irc_helpers[i];
		hlpr->tuple.src.u.tcp.port = htons(ports[i]);
		hlpr->tuple.dst.protonum = IPPROTO_TCP;
		hlpr->mask.src.u.tcp.port = 0xFFFF;
		hlpr->mask.dst.protonum = 0xFFFF;
		hlpr->max_expected = max_dcc_channels;
		hlpr->timeout = dcc_timeout;
		hlpr->flags = IP_CT_HELPER_F_REUSE_EXPECT;
		hlpr->me = ip_conntrack_irc;
		hlpr->help = help;

		tmpname = &irc_names[i][0];
		if (ports[i] == IRC_PORT)
			sprintf(tmpname, "irc");
		else
			sprintf(tmpname, "irc-%d", i);
		hlpr->name = tmpname;

		DEBUGP("port #%d: %d\n", i, ports[i]);

		ret = ip_conntrack_helper_register(hlpr);

		if (ret) {
			printk("ip_conntrack_irc: ERROR registering port %d\n",
				ports[i]);
			fini();
			return -EBUSY;
		}
		ports_c++;
	}
	return 0;
}

/* This function is intentionally _NOT_ defined as __exit, because 
 * it is needed by the init function */
static void fini(void)
{
	int i;
	for (i = 0; i < ports_c; i++) {
		DEBUGP("unregistering port %d\n",
		       ports[i]);
		ip_conntrack_helper_unregister(&irc_helpers[i]);
	}
}

PROVIDES_CONNTRACK(irc);
EXPORT_SYMBOL(ip_irc_lock);

module_init(init);
module_exit(fini);
