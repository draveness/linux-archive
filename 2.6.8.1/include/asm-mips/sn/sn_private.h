#ifndef __ASM_SN_SN_PRIVATE_H
#define __ASM_SN_SN_PRIVATE_H

#include <asm/sn/types.h>

extern nasid_t master_nasid;

extern void cpu_node_probe(void);
extern cnodeid_t get_compact_nodeid(void);
extern void hub_rtc_init(cnodeid_t);
extern void cpu_time_init(void);
extern void per_cpu_init(void);
extern void per_hub_init(cnodeid_t cnode);
extern void install_cpu_nmi_handler(int slice);
extern void install_ipi(void);
extern void setup_replication_mask(int);
extern void replicate_kernel_text(int);
extern pfn_t node_getfirstfree(cnodeid_t);
extern void mlreset(void);

#endif /* __ASM_SN_SN_PRIVATE_H */
