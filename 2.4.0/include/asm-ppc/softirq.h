#ifdef __KERNEL__
#ifndef __ASM_SOFTIRQ_H
#define __ASM_SOFTIRQ_H

#include <asm/atomic.h>
#include <asm/hardirq.h>

#define local_bh_disable()	do { local_bh_count(smp_processor_id())++; barrier(); } while (0)
#define local_bh_enable()	do { barrier(); local_bh_count(smp_processor_id())--; } while (0)

#define in_softirq() (local_bh_count(smp_processor_id()) != 0)

#endif	/* __ASM_SOFTIRQ_H */
#endif /* __KERNEL__ */
