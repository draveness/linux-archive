#ifndef _IPT_LOG_H
#define _IPT_LOG_H

#define IPT_LOG_TCPSEQ		0x01	/* Log TCP sequence numbers */
#define IPT_LOG_TCPOPT		0x02	/* Log TCP options */
#define IPT_LOG_IPOPT		0x04	/* Log IP options */
#define IPT_LOG_MASK		0x07

struct ipt_log_info {
	unsigned char level;
	unsigned char logflags;
	char prefix[30];
};

#endif /*_IPT_LOG_H*/
