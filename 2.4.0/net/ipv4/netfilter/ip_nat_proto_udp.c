#include <linux/types.h>
#include <linux/init.h>
#include <linux/netfilter.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/if.h>

#include <linux/netfilter_ipv4/ip_nat.h>
#include <linux/netfilter_ipv4/ip_nat_rule.h>
#include <linux/netfilter_ipv4/ip_nat_protocol.h>

static int
udp_in_range(const struct ip_conntrack_tuple *tuple,
	     enum ip_nat_manip_type maniptype,
	     const union ip_conntrack_manip_proto *min,
	     const union ip_conntrack_manip_proto *max)
{
	u_int16_t port;

	if (maniptype == IP_NAT_MANIP_SRC)
		port = tuple->src.u.udp.port;
	else
		port = tuple->dst.u.udp.port;

	return ntohs(port) >= ntohs(min->udp.port)
		&& ntohs(port) <= ntohs(max->udp.port);
}

static int
udp_unique_tuple(struct ip_conntrack_tuple *tuple,
		 const struct ip_nat_range *range,
		 enum ip_nat_manip_type maniptype,
		 const struct ip_conntrack *conntrack)
{
	static u_int16_t port = 0, *portptr;
	unsigned int range_size, min, i;

	if (maniptype == IP_NAT_MANIP_SRC)
		portptr = &tuple->src.u.udp.port;
	else
		portptr = &tuple->dst.u.udp.port;

	/* If no range specified... */
	if (!(range->flags & IP_NAT_RANGE_PROTO_SPECIFIED)) {
		/* If it's dst rewrite, can't change port */
		if (maniptype == IP_NAT_MANIP_DST)
			return 0;

		if (ntohs(*portptr) < 1024) {
			/* Loose convention: >> 512 is credential passing */
			if (ntohs(*portptr)<512) {
				min = 1;
				range_size = 511 - min + 1;
			} else {
				min = 600;
				range_size = 1023 - min + 1;
			}
		} else {
			min = 1024;
			range_size = 65535 - 1024 + 1;
		}
	} else {
		min = ntohs(range->min.udp.port);
		range_size = ntohs(range->max.udp.port) - min + 1;
	}

	for (i = 0; i < range_size; i++, port++) {
		*portptr = htons(min + port % range_size);
		if (!ip_nat_used_tuple(tuple, conntrack))
			return 1;
	}
	return 0;
}

static void
udp_manip_pkt(struct iphdr *iph, size_t len,
	      const struct ip_conntrack_manip *manip,
	      enum ip_nat_manip_type maniptype)
{
	struct udphdr *hdr = (struct udphdr *)((u_int32_t *)iph + iph->ihl);
	u_int32_t oldip;
	u_int16_t *portptr;

	if (maniptype == IP_NAT_MANIP_SRC) {
		/* Get rid of src ip and src pt */
		oldip = iph->saddr;
		portptr = &hdr->source;
	} else {
		/* Get rid of dst ip and dst pt */
		oldip = iph->daddr;
		portptr = &hdr->dest;
	}
	if (hdr->check) /* 0 is a special case meaning no checksum */
		hdr->check = ip_nat_cheat_check(~oldip, manip->ip,
					ip_nat_cheat_check(*portptr ^ 0xFFFF,
							   manip->u.udp.port,
							   hdr->check));
	*portptr = manip->u.udp.port;
}

static unsigned int
udp_print(char *buffer,
	  const struct ip_conntrack_tuple *match,
	  const struct ip_conntrack_tuple *mask)
{
	unsigned int len = 0;

	if (mask->src.u.udp.port)
		len += sprintf(buffer + len, "srcpt=%u ",
			       ntohs(match->src.u.udp.port));


	if (mask->dst.u.udp.port)
		len += sprintf(buffer + len, "dstpt=%u ",
			       ntohs(match->dst.u.udp.port));

	return len;
}

static unsigned int
udp_print_range(char *buffer, const struct ip_nat_range *range)
{
	if (range->min.udp.port != 0 || range->max.udp.port != 0xFFFF) {
		if (range->min.udp.port == range->max.udp.port)
			return sprintf(buffer, "port %u ",
				       ntohs(range->min.udp.port));
		else
			return sprintf(buffer, "ports %u-%u ",
				       ntohs(range->min.udp.port),
				       ntohs(range->max.udp.port));
	}
	else return 0;
}

struct ip_nat_protocol ip_nat_protocol_udp
= { { NULL, NULL }, "UDP", IPPROTO_UDP,
    udp_manip_pkt,
    udp_in_range,
    udp_unique_tuple,
    udp_print,
    udp_print_range
};
