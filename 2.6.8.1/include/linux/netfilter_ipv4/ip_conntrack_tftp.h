#ifndef _IP_CT_TFTP
#define _IP_CT_TFTP

#define TFTP_PORT 69

struct tftphdr {
	u_int16_t opcode;
};

#define TFTP_OPCODE_READ	1
#define TFTP_OPCODE_WRITE	2
#define TFTP_OPCODE_DATA	3
#define TFTP_OPCODE_ACK		4
#define TFTP_OPCODE_ERROR	5

#endif /* _IP_CT_TFTP */
