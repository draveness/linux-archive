#ifndef _IEEE1394_TRANSACTIONS_H
#define _IEEE1394_TRANSACTIONS_H

#include "ieee1394_core.h"


/*
 * Get and free transaction labels.
 */
int hpsb_get_tlabel(struct hpsb_packet *packet);
void hpsb_free_tlabel(struct hpsb_packet *packet);

struct hpsb_packet *hpsb_make_readpacket(struct hpsb_host *host, nodeid_t node,
					 u64 addr, size_t length);
struct hpsb_packet *hpsb_make_lockpacket(struct hpsb_host *host, nodeid_t node,
                                         u64 addr, int extcode, quadlet_t *data,
					 quadlet_t arg);
struct hpsb_packet *hpsb_make_lock64packet(struct hpsb_host *host, nodeid_t node,
                                          u64 addr, int extcode, octlet_t *data,
					  octlet_t arg);
struct hpsb_packet *hpsb_make_phypacket(struct hpsb_host *host,
                                        quadlet_t data) ;
struct hpsb_packet *hpsb_make_isopacket(struct hpsb_host *host,
					int length, int channel,
					int tag, int sync);
struct hpsb_packet *hpsb_make_writepacket (struct hpsb_host *host, nodeid_t node,
					   u64 addr, quadlet_t *buffer, size_t length);
struct hpsb_packet *hpsb_make_streampacket(struct hpsb_host *host, u8 *buffer,
                                           int length, int channel, int tag, int sync);

/*
 * hpsb_packet_success - Make sense of the ack and reply codes and
 * return more convenient error codes:
 * 0           success
 * -EBUSY      node is busy, try again
 * -EAGAIN     error which can probably resolved by retry
 * -EREMOTEIO  node suffers from an internal error
 * -EACCES     this transaction is not allowed on requested address
 * -EINVAL     invalid address at node
 */
int hpsb_packet_success(struct hpsb_packet *packet);


/*
 * The generic read, write and lock functions.  All recognize the local node ID
 * and act accordingly.  Read and write automatically use quadlet commands if
 * length == 4 and and block commands otherwise (however, they do not yet
 * support lengths that are not a multiple of 4).  You must explicitly specifiy
 * the generation for which the node ID is valid, to avoid sending packets to
 * the wrong nodes when we race with a bus reset.
 */
int hpsb_read(struct hpsb_host *host, nodeid_t node, unsigned int generation,
	      u64 addr, quadlet_t *buffer, size_t length);
int hpsb_write(struct hpsb_host *host, nodeid_t node, unsigned int generation,
	       u64 addr, quadlet_t *buffer, size_t length);
int hpsb_lock(struct hpsb_host *host, nodeid_t node, unsigned int generation,
	      u64 addr, int extcode, quadlet_t *data, quadlet_t arg);
int hpsb_lock64(struct hpsb_host *host, nodeid_t node, unsigned int generation,
		u64 addr, int extcode, octlet_t *data, octlet_t arg);
int hpsb_send_gasp(struct hpsb_host *host, int channel, unsigned int generation,
                   quadlet_t *buffer, size_t length, u32 specifier_id,
                   unsigned int version);

#endif /* _IEEE1394_TRANSACTIONS_H */
