/* drivers/atm/zatm.c - ZeitNet ZN122x device driver */
 
/* Written 1995-2000 by Werner Almesberger, EPFL LRC/ICA */


#include <linux/config.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/atm.h>
#include <linux/atmdev.h>
#include <linux/sonet.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/delay.h>
#include <linux/ioport.h> /* for request_region */
#include <linux/uio.h>
#include <linux/init.h>
#include <linux/atm_zatm.h>
#include <linux/capability.h>
#include <linux/bitops.h>
#include <asm/byteorder.h>
#include <asm/system.h>
#include <asm/string.h>
#include <asm/io.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#include "uPD98401.h"
#include "uPD98402.h"
#include "zeprom.h"
#include "zatm.h"


/*
 * TODO:
 *
 * Minor features
 *  - support 64 kB SDUs (will have to use multibuffer batches then :-( )
 *  - proper use of CDV, credit = max(1,CDVT*PCR)
 *  - AAL0
 *  - better receive timestamps
 *  - OAM
 */

#if 0
#define DPRINTK(format,args...) printk(KERN_DEBUG format,##args)
#else
#define DPRINTK(format,args...)
#endif


#ifndef CONFIG_ATM_ZATM_DEBUG


#define NULLCHECK(x)

#define EVENT(s,a,b)


static void event_dump(void)
{
}


#else


/* 
 * NULL pointer checking
 */

#define NULLCHECK(x) \
  if ((unsigned long) (x) < 0x30) printk(KERN_CRIT #x "==0x%x\n", (int) (x))

/*
 * Very extensive activity logging. Greatly improves bug detection speed but
 * costs a few Mbps if enabled.
 */

#define EV 64

static const char *ev[EV];
static unsigned long ev_a[EV],ev_b[EV];
static int ec = 0;


static void EVENT(const char *s,unsigned long a,unsigned long b)
{
	ev[ec] = s; 
	ev_a[ec] = a;
	ev_b[ec] = b;
	ec = (ec+1) % EV;
}


static void event_dump(void)
{
	int n,i;

	printk(KERN_NOTICE "----- event dump follows -----\n");
	for (n = 0; n < EV; n++) {
		i = (ec+n) % EV;
		printk(KERN_NOTICE);
		printk(ev[i] ? ev[i] : "(null)",ev_a[i],ev_b[i]);
	}
	printk(KERN_NOTICE "----- event dump ends here -----\n");
}


#endif /* CONFIG_ATM_ZATM_DEBUG */


#define RING_BUSY	1	/* indication from do_tx that PDU has to be
				   backlogged */

static struct atm_dev *zatm_boards = NULL;
static unsigned long dummy[2] = {0,0};


#define zin_n(r) inl(zatm_dev->base+r*4)
#define zin(r) inl(zatm_dev->base+uPD98401_##r*4)
#define zout(v,r) outl(v,zatm_dev->base+uPD98401_##r*4)
#define zwait while (zin(CMR) & uPD98401_BUSY)

/* RX0, RX1, TX0, TX1 */
static const int mbx_entries[NR_MBX] = { 1024,1024,1024,1024 };
static const int mbx_esize[NR_MBX] = { 16,16,4,4 }; /* entry size in bytes */

#define MBX_SIZE(i) (mbx_entries[i]*mbx_esize[i])


/*-------------------------------- utilities --------------------------------*/


static void zpokel(struct zatm_dev *zatm_dev,u32 value,u32 addr)
{
	zwait;
	zout(value,CER);
	zout(uPD98401_IND_ACC | uPD98401_IA_BALL |
	    (uPD98401_IA_TGT_CM << uPD98401_IA_TGT_SHIFT) | addr,CMR);
}


static u32 zpeekl(struct zatm_dev *zatm_dev,u32 addr)
{
	zwait;
	zout(uPD98401_IND_ACC | uPD98401_IA_BALL | uPD98401_IA_RW |
	  (uPD98401_IA_TGT_CM << uPD98401_IA_TGT_SHIFT) | addr,CMR);
	zwait;
	return zin(CER);
}


/*------------------------------- free lists --------------------------------*/


/*
 * Free buffer head structure:
 *   [0] pointer to buffer (for SAR)
 *   [1] buffer descr link pointer (for SAR)
 *   [2] back pointer to skb (for poll_rx)
 *   [3] data
 *   ...
 */

struct rx_buffer_head {
	u32		buffer;	/* pointer to buffer (for SAR) */
	u32		link;	/* buffer descriptor link pointer (for SAR) */
	struct sk_buff	*skb;	/* back pointer to skb (for poll_rx) */
};


static void refill_pool(struct atm_dev *dev,int pool)
{
	struct zatm_dev *zatm_dev;
	struct sk_buff *skb;
	struct rx_buffer_head *first;
	unsigned long flags;
	int align,offset,free,count,size;

	EVENT("refill_pool\n",0,0);
	zatm_dev = ZATM_DEV(dev);
	size = (64 << (pool <= ZATM_AAL5_POOL_BASE ? 0 :
	    pool-ZATM_AAL5_POOL_BASE))+sizeof(struct rx_buffer_head);
	if (size < PAGE_SIZE) {
		align = 32; /* for 32 byte alignment */
		offset = sizeof(struct rx_buffer_head);
	}
	else {
		align = 4096;
		offset = zatm_dev->pool_info[pool].offset+
		    sizeof(struct rx_buffer_head);
	}
	size += align;
	save_flags(flags);
	cli();
	free = zpeekl(zatm_dev,zatm_dev->pool_base+2*pool) &
	    uPD98401_RXFP_REMAIN;
	restore_flags(flags);
	if (free >= zatm_dev->pool_info[pool].low_water) return;
	EVENT("starting ... POOL: 0x%x, 0x%x\n",
	    zpeekl(zatm_dev,zatm_dev->pool_base+2*pool),
	    zpeekl(zatm_dev,zatm_dev->pool_base+2*pool+1));
	EVENT("dummy: 0x%08lx, 0x%08lx\n",dummy[0],dummy[1]);
	count = 0;
	first = NULL;
	while (free < zatm_dev->pool_info[pool].high_water) {
		struct rx_buffer_head *head;

		skb = alloc_skb(size,GFP_ATOMIC);
		if (!skb) {
			printk(KERN_WARNING DEV_LABEL "(Itf %d): got no new "
			    "skb (%d) with %d free\n",dev->number,size,free);
			break;
		}
		skb_reserve(skb,(unsigned char *) ((((unsigned long) skb->data+
		    align+offset-1) & ~(unsigned long) (align-1))-offset)-
		    skb->data);
		head = (struct rx_buffer_head *) skb->data;
		skb_reserve(skb,sizeof(struct rx_buffer_head));
		if (!first) first = head;
		count++;
		head->buffer = virt_to_bus(skb->data);
		head->link = 0;
		head->skb = skb;
		EVENT("enq skb 0x%08lx/0x%08lx\n",(unsigned long) skb,
		    (unsigned long) head);
		cli();
		if (zatm_dev->last_free[pool])
			((struct rx_buffer_head *) (zatm_dev->last_free[pool]->
			    data))[-1].link = virt_to_bus(head);
		zatm_dev->last_free[pool] = skb;
		skb_queue_tail(&zatm_dev->pool[pool],skb);
		restore_flags(flags);
		free++;
	}
	if (first) {
		cli();
		zwait;
		zout(virt_to_bus(first),CER);
		zout(uPD98401_ADD_BAT | (pool << uPD98401_POOL_SHIFT) | count,
		    CMR);
		restore_flags(flags);
		EVENT ("POOL: 0x%x, 0x%x\n",
		    zpeekl(zatm_dev,zatm_dev->pool_base+2*pool),
		    zpeekl(zatm_dev,zatm_dev->pool_base+2*pool+1));
		EVENT("dummy: 0x%08lx, 0x%08lx\n",dummy[0],dummy[1]);
	}
}


static void drain_free(struct atm_dev *dev,int pool)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&ZATM_DEV(dev)->pool[pool]))) kfree_skb(skb);
}


static int pool_index(int max_pdu)
{
	int i;

	if (max_pdu % ATM_CELL_PAYLOAD)
		printk(KERN_ERR DEV_LABEL ": driver error in pool_index: "
		    "max_pdu is %d\n",max_pdu);
	if (max_pdu > 65536) return -1;
	for (i = 0; (64 << i) < max_pdu; i++);
	return i+ZATM_AAL5_POOL_BASE;
}


/* use_pool isn't reentrant */


static void use_pool(struct atm_dev *dev,int pool)
{
	struct zatm_dev *zatm_dev;
	unsigned long flags;
	int size;

	zatm_dev = ZATM_DEV(dev);
	if (!(zatm_dev->pool_info[pool].ref_count++)) {
		skb_queue_head_init(&zatm_dev->pool[pool]);
		size = pool-ZATM_AAL5_POOL_BASE;
		if (size < 0) size = 0; /* 64B... */
		else if (size > 10) size = 10; /* ... 64kB */
		save_flags(flags);
		cli();
		zpokel(zatm_dev,((zatm_dev->pool_info[pool].low_water/4) <<
		    uPD98401_RXFP_ALERT_SHIFT) |
		    (1 << uPD98401_RXFP_BTSZ_SHIFT) |
		    (size << uPD98401_RXFP_BFSZ_SHIFT),
		    zatm_dev->pool_base+pool*2);
		zpokel(zatm_dev,(unsigned long) dummy,zatm_dev->pool_base+
		    pool*2+1);
		restore_flags(flags);
		zatm_dev->last_free[pool] = NULL;
		refill_pool(dev,pool);
	}
	DPRINTK("pool %d: %d\n",pool,zatm_dev->pool_info[pool].ref_count);
}


static void unuse_pool(struct atm_dev *dev,int pool)
{
	if (!(--ZATM_DEV(dev)->pool_info[pool].ref_count))
		drain_free(dev,pool);
}


static void zatm_feedback(struct atm_vcc *vcc,struct sk_buff *skb,
    unsigned long start,unsigned long dest,int len)
{
	struct zatm_pool_info *pool;
	unsigned long offset,flags;

	DPRINTK("start 0x%08lx dest 0x%08lx len %d\n",start,dest,len);
	if (len < PAGE_SIZE) return;
	pool = &ZATM_DEV(vcc->dev)->pool_info[ZATM_VCC(vcc)->pool];
	offset = (dest-start) & (PAGE_SIZE-1);
	save_flags(flags);
	cli();
	if (!offset || pool->offset == offset) {
		pool->next_cnt = 0;
		restore_flags(flags);
		return;
	}
	if (offset != pool->next_off) {
		pool->next_off = offset;
		pool->next_cnt = 0;
		restore_flags(flags);
		return;
	}
	if (++pool->next_cnt >= pool->next_thres) {
		pool->offset = pool->next_off;
		pool->next_cnt = 0;
	}
	restore_flags(flags);
}


/*----------------------- high-precision timestamps -------------------------*/


#ifdef CONFIG_ATM_ZATM_EXACT_TS

static struct timer_list sync_timer;


/*
 * Note: the exact time is not normalized, i.e. tv_usec can be > 1000000.
 * This must be handled by higher layers.
 */

static inline struct timeval exact_time(struct zatm_dev *zatm_dev,u32 ticks)
{
	struct timeval tmp;

	tmp = zatm_dev->last_time;
	tmp.tv_usec += ((s64) (ticks-zatm_dev->last_clk)*
	    (s64) zatm_dev->factor) >> TIMER_SHIFT;
	return tmp;
}


static void zatm_clock_sync(unsigned long dummy)
{
	struct atm_dev *atm_dev;
	struct zatm_dev *zatm_dev;

	for (atm_dev = zatm_boards; atm_dev; atm_dev = zatm_dev->more) {
		unsigned long flags,interval;
		int diff;
		struct timeval now,expected;
		u32 ticks;

		zatm_dev = ZATM_DEV(atm_dev);
		save_flags(flags);
		cli();
		ticks = zpeekl(zatm_dev,uPD98401_TSR);
		do_gettimeofday(&now);
		restore_flags(flags);
		expected = exact_time(zatm_dev,ticks);
		diff = 1000000*(expected.tv_sec-now.tv_sec)+
		    (expected.tv_usec-now.tv_usec);
		zatm_dev->timer_history[zatm_dev->th_curr].real = now;
		zatm_dev->timer_history[zatm_dev->th_curr].expected = expected;
		zatm_dev->th_curr = (zatm_dev->th_curr+1) &
		    (ZATM_TIMER_HISTORY_SIZE-1);
		interval = 1000000*(now.tv_sec-zatm_dev->last_real_time.tv_sec)
		    +(now.tv_usec-zatm_dev->last_real_time.tv_usec);
		if (diff >= -ADJ_REP_THRES && diff <= ADJ_REP_THRES)
			zatm_dev->timer_diffs = 0;
		else
#ifndef AGGRESSIVE_DEBUGGING
			if (++zatm_dev->timer_diffs >= ADJ_MSG_THRES)
#endif
			{
			zatm_dev->timer_diffs = 0;
			printk(KERN_INFO DEV_LABEL ": TSR update after %ld us:"
			    " calculation differed by %d us\n",interval,diff);
#ifdef AGGRESSIVE_DEBUGGING
			printk(KERN_DEBUG "  %d.%08d -> %d.%08d (%lu)\n",
			    zatm_dev->last_real_time.tv_sec,
			    zatm_dev->last_real_time.tv_usec,
			    now.tv_sec,now.tv_usec,interval);
			printk(KERN_DEBUG "  %u -> %u (%d)\n",
			    zatm_dev->last_clk,ticks,ticks-zatm_dev->last_clk);
			printk(KERN_DEBUG "  factor %u\n",zatm_dev->factor);
#endif
		}
		if (diff < -ADJ_IGN_THRES || diff > ADJ_IGN_THRES) {
		    /* filter out any major changes (e.g. time zone setup and
		       such) */
			zatm_dev->last_time = now;
			zatm_dev->factor =
			    (1000 << TIMER_SHIFT)/(zatm_dev->khz+1);
		}
		else {
			zatm_dev->last_time = expected;
			/*
			 * Is the accuracy of udelay really only about 1:300 on
			 * a 90 MHz Pentium ? Well, the following line avoids
			 * the problem, but ...
			 *
			 * What it does is simply:
			 *
			 * zatm_dev->factor = (interval << TIMER_SHIFT)/
			 *     (ticks-zatm_dev->last_clk);
			 */
#define S(x) #x		/* "stringification" ... */
#define SX(x) S(x)
			asm("movl %2,%%ebx\n\t"
			    "subl %3,%%ebx\n\t"
			    "xorl %%edx,%%edx\n\t"
			    "shldl $" SX(TIMER_SHIFT) ",%1,%%edx\n\t"
			    "shl $" SX(TIMER_SHIFT) ",%1\n\t"
			    "divl %%ebx\n\t"
			    : "=a" (zatm_dev->factor)
			    : "0" (interval-diff),"g" (ticks),
			      "g" (zatm_dev->last_clk)
			    : "ebx","edx","cc");
#undef S
#undef SX
#ifdef AGGRESSIVE_DEBUGGING
			printk(KERN_DEBUG "  (%ld << %d)/(%u-%u) = %u\n",
			    interval,TIMER_SHIFT,ticks,zatm_dev->last_clk,
			    zatm_dev->factor);
#endif
		}
		zatm_dev->last_real_time = now;
		zatm_dev->last_clk = ticks;
	}
	mod_timer(&sync_timer,sync_timer.expires+POLL_INTERVAL*HZ);
}


static void __init zatm_clock_init(struct zatm_dev *zatm_dev)
{
	static int start_timer = 1;
	unsigned long flags;

	zatm_dev->factor = (1000 << TIMER_SHIFT)/(zatm_dev->khz+1);
	zatm_dev->timer_diffs = 0;
	memset(zatm_dev->timer_history,0,sizeof(zatm_dev->timer_history));
	zatm_dev->th_curr = 0;
	save_flags(flags);
	cli();
	do_gettimeofday(&zatm_dev->last_time);
	zatm_dev->last_clk = zpeekl(zatm_dev,uPD98401_TSR);
	if (start_timer) {
		start_timer = 0;
		init_timer(&sync_timer);
		sync_timer.expires = jiffies+POLL_INTERVAL*HZ;
		sync_timer.function = zatm_clock_sync;
		add_timer(&sync_timer);
	}
	restore_flags(flags);
}


#endif


/*----------------------------------- RX ------------------------------------*/


#if 0
static void exception(struct atm_vcc *vcc)
{
   static int count = 0;
   struct zatm_dev *zatm_dev = ZATM_DEV(vcc->dev);
   struct zatm_vcc *zatm_vcc = ZATM_VCC(vcc);
   unsigned long *qrp;
   int i;

   if (count++ > 2) return;
   for (i = 0; i < 8; i++)
	printk("TX%d: 0x%08lx\n",i,
	  zpeekl(zatm_dev,zatm_vcc->tx_chan*VC_SIZE/4+i));
   for (i = 0; i < 5; i++)
	printk("SH%d: 0x%08lx\n",i,
	  zpeekl(zatm_dev,uPD98401_IM(zatm_vcc->shaper)+16*i));
   qrp = (unsigned long *) zpeekl(zatm_dev,zatm_vcc->tx_chan*VC_SIZE/4+
     uPD98401_TXVC_QRP);
   printk("qrp=0x%08lx\n",(unsigned long) qrp);
   for (i = 0; i < 4; i++) printk("QRP[%d]: 0x%08lx",i,qrp[i]);
}
#endif


static const char *err_txt[] = {
	"No error",
	"RX buf underflow",
	"RX FIFO overrun",
	"Maximum len violation",
	"CRC error",
	"User abort",
	"Length violation",
	"T1 error",
	"Deactivated",
	"???",
	"???",
	"???",
	"???",
	"???",
	"???",
	"???"
};


static void poll_rx(struct atm_dev *dev,int mbx)
{
	struct zatm_dev *zatm_dev;
	unsigned long pos;
	u32 x;
	int error;

	EVENT("poll_rx\n",0,0);
	zatm_dev = ZATM_DEV(dev);
	pos = (zatm_dev->mbx_start[mbx] & ~0xffffUL) | zin(MTA(mbx));
	while (x = zin(MWA(mbx)), (pos & 0xffff) != x) {
		u32 *here;
		struct sk_buff *skb;
		struct atm_vcc *vcc;
		int cells,size,chan;

		EVENT("MBX: host 0x%lx, nic 0x%x\n",pos,x);
		here = (u32 *) pos;
		if (((pos += 16) & 0xffff) == zatm_dev->mbx_end[mbx])
			pos = zatm_dev->mbx_start[mbx];
		cells = here[0] & uPD98401_AAL5_SIZE;
#if 0
printk("RX IND: 0x%x, 0x%x, 0x%x, 0x%x\n",here[0],here[1],here[2],here[3]);
{
unsigned long *x;
		printk("POOL: 0x%08x, 0x%08x\n",zpeekl(zatm_dev,
		      zatm_dev->pool_base),
		      zpeekl(zatm_dev,zatm_dev->pool_base+1));
		x = (unsigned long *) here[2];
		printk("[0..3] = 0x%08lx, 0x%08lx, 0x%08lx, 0x%08lx\n",
		    x[0],x[1],x[2],x[3]);
}
#endif
		error = 0;
		if (here[3] & uPD98401_AAL5_ERR) {
			error = (here[3] & uPD98401_AAL5_ES) >>
			    uPD98401_AAL5_ES_SHIFT;
			if (error == uPD98401_AAL5_ES_DEACT ||
			    error == uPD98401_AAL5_ES_FREE) continue;
		}
EVENT("error code 0x%x/0x%x\n",(here[3] & uPD98401_AAL5_ES) >>
  uPD98401_AAL5_ES_SHIFT,error);
		skb = ((struct rx_buffer_head *) bus_to_virt(here[2]))->skb;
#ifdef CONFIG_ATM_ZATM_EXACT_TS
		skb->stamp = exact_time(zatm_dev,here[1]);
#else
		skb->stamp = xtime;
#endif
#if 0
printk("[-3..0] 0x%08lx 0x%08lx 0x%08lx 0x%08lx\n",((unsigned *) skb->data)[-3],
  ((unsigned *) skb->data)[-2],((unsigned *) skb->data)[-1],
  ((unsigned *) skb->data)[0]);
#endif
		EVENT("skb 0x%lx, here 0x%lx\n",(unsigned long) skb,
		    (unsigned long) here);
#if 0
printk("dummy: 0x%08lx, 0x%08lx\n",dummy[0],dummy[1]);
#endif
		size = error ? 0 : ntohs(((u16 *) skb->data)[cells*
		    ATM_CELL_PAYLOAD/sizeof(u16)-3]);
		EVENT("got skb 0x%lx, size %d\n",(unsigned long) skb,size);
		chan = (here[3] & uPD98401_AAL5_CHAN) >>
		    uPD98401_AAL5_CHAN_SHIFT;
		if (chan < zatm_dev->chans && zatm_dev->rx_map[chan]) {
			vcc = zatm_dev->rx_map[chan];
			if (skb == zatm_dev->last_free[ZATM_VCC(vcc)->pool])
				zatm_dev->last_free[ZATM_VCC(vcc)->pool] = NULL;
			skb_unlink(skb);
		}
		else {
			printk(KERN_ERR DEV_LABEL "(itf %d): RX indication "
			    "for non-existing channel\n",dev->number);
			size = 0;
			vcc = NULL;
			event_dump();
		}
		if (error) {
			static unsigned long silence = 0;
			static int last_error = 0;

			if (error != last_error ||
			    time_after(jiffies, silence)  || silence == 0){
				printk(KERN_WARNING DEV_LABEL "(itf %d): "
				    "chan %d error %s\n",dev->number,chan,
				    err_txt[error]);
				last_error = error;
				silence = (jiffies+2*HZ)|1;
			}
			size = 0;
		}
		if (size && (size > cells*ATM_CELL_PAYLOAD-ATM_AAL5_TRAILER ||
		    size <= (cells-1)*ATM_CELL_PAYLOAD-ATM_AAL5_TRAILER)) {
			printk(KERN_ERR DEV_LABEL "(itf %d): size %d with %d "
			    "cells\n",dev->number,size,cells);
			size = 0;
			event_dump();
		}
		if (size > ATM_MAX_AAL5_PDU) {
			printk(KERN_ERR DEV_LABEL "(itf %d): size too big "
			    "(%d)\n",dev->number,size);
			size = 0;
			event_dump();
		}
		if (!size) {
			dev_kfree_skb_irq(skb);
			if (vcc) atomic_inc(&vcc->stats->rx_err);
			continue;
		}
		if (!atm_charge(vcc,skb->truesize)) {
			dev_kfree_skb_irq(skb);
			continue;
		}
		skb->len = size;
		ATM_SKB(skb)->vcc = vcc;
		vcc->push(vcc,skb);
		atomic_inc(&vcc->stats->rx);
	}
	zout(pos & 0xffff,MTA(mbx));
#if 0 /* probably a stupid idea */
	refill_pool(dev,zatm_vcc->pool);
		/* maybe this saves us a few interrupts */
#endif
}


static int open_rx_first(struct atm_vcc *vcc)
{
	struct zatm_dev *zatm_dev;
	struct zatm_vcc *zatm_vcc;
	unsigned long flags;
	unsigned short chan;
	int cells;

	DPRINTK("open_rx_first (0x%x)\n",inb_p(0xc053));
	zatm_dev = ZATM_DEV(vcc->dev);
	zatm_vcc = ZATM_VCC(vcc);
	zatm_vcc->rx_chan = 0;
	if (vcc->qos.rxtp.traffic_class == ATM_NONE) return 0;
	if (vcc->qos.aal == ATM_AAL5) {
		if (vcc->qos.rxtp.max_sdu > 65464)
			vcc->qos.rxtp.max_sdu = 65464;
			/* fix this - we may want to receive 64kB SDUs
			   later */
		cells = (vcc->qos.rxtp.max_sdu+ATM_AAL5_TRAILER+
		    ATM_CELL_PAYLOAD-1)/ATM_CELL_PAYLOAD;
		zatm_vcc->pool = pool_index(cells*ATM_CELL_PAYLOAD);
	}
	else {
		cells = 1;
		zatm_vcc->pool = ZATM_AAL0_POOL;
	}
	if (zatm_vcc->pool < 0) return -EMSGSIZE;
	save_flags(flags);
	cli();
	zwait;
	zout(uPD98401_OPEN_CHAN,CMR);
	zwait;
	DPRINTK("0x%x 0x%x\n",zin(CMR),zin(CER));
	chan = (zin(CMR) & uPD98401_CHAN_ADDR) >> uPD98401_CHAN_ADDR_SHIFT;
	restore_flags(flags);
	DPRINTK("chan is %d\n",chan);
	if (!chan) return -EAGAIN;
	use_pool(vcc->dev,zatm_vcc->pool);
	DPRINTK("pool %d\n",zatm_vcc->pool);
	/* set up VC descriptor */
	cli();
	zpokel(zatm_dev,zatm_vcc->pool << uPD98401_RXVC_POOL_SHIFT,
	    chan*VC_SIZE/4);
	zpokel(zatm_dev,uPD98401_RXVC_OD | (vcc->qos.aal == ATM_AAL5 ?
	    uPD98401_RXVC_AR : 0) | cells,chan*VC_SIZE/4+1);
	zpokel(zatm_dev,0,chan*VC_SIZE/4+2);
	zatm_vcc->rx_chan = chan;
	zatm_dev->rx_map[chan] = vcc;
	restore_flags(flags);
	return 0;
}


static int open_rx_second(struct atm_vcc *vcc)
{
	struct zatm_dev *zatm_dev;
	struct zatm_vcc *zatm_vcc;
	unsigned long flags;
	int pos,shift;

	DPRINTK("open_rx_second (0x%x)\n",inb_p(0xc053));
	zatm_dev = ZATM_DEV(vcc->dev);
	zatm_vcc = ZATM_VCC(vcc);
	if (!zatm_vcc->rx_chan) return 0;
	save_flags(flags);
	cli();
	/* should also handle VPI @@@ */
	pos = vcc->vci >> 1;
	shift = (1-(vcc->vci & 1)) << 4;
	zpokel(zatm_dev,(zpeekl(zatm_dev,pos) & ~(0xffff << shift)) |
	    ((zatm_vcc->rx_chan | uPD98401_RXLT_ENBL) << shift),pos);
	restore_flags(flags);
	return 0;
}


static void close_rx(struct atm_vcc *vcc)
{
	struct zatm_dev *zatm_dev;
	struct zatm_vcc *zatm_vcc;
	unsigned long flags;
	int pos,shift;

	zatm_vcc = ZATM_VCC(vcc);
	zatm_dev = ZATM_DEV(vcc->dev);
	if (!zatm_vcc->rx_chan) return;
	DPRINTK("close_rx\n");
	/* disable receiver */
	save_flags(flags);
	if (vcc->vpi != ATM_VPI_UNSPEC && vcc->vci != ATM_VCI_UNSPEC) {
		cli();
		pos = vcc->vci >> 1;
		shift = (1-(vcc->vci & 1)) << 4;
		zpokel(zatm_dev,zpeekl(zatm_dev,pos) & ~(0xffff << shift),pos);
		zwait;
		zout(uPD98401_NOP,CMR);
		zwait;
		zout(uPD98401_NOP,CMR);
		restore_flags(flags);
	}
	cli();
	zwait;
	zout(uPD98401_DEACT_CHAN | uPD98401_CHAN_RT | (zatm_vcc->rx_chan <<
	    uPD98401_CHAN_ADDR_SHIFT),CMR);
	zwait;
	udelay(10); /* why oh why ... ? */
	zout(uPD98401_CLOSE_CHAN | uPD98401_CHAN_RT | (zatm_vcc->rx_chan <<
	    uPD98401_CHAN_ADDR_SHIFT),CMR);
	zwait;
	if (!(zin(CMR) & uPD98401_CHAN_ADDR))
		printk(KERN_CRIT DEV_LABEL "(itf %d): can't close RX channel "
		    "%d\n",vcc->dev->number,zatm_vcc->rx_chan);
	restore_flags(flags);
	zatm_dev->rx_map[zatm_vcc->rx_chan] = NULL;
	zatm_vcc->rx_chan = 0;
	unuse_pool(vcc->dev,zatm_vcc->pool);
}


static int start_rx(struct atm_dev *dev)
{
	struct zatm_dev *zatm_dev;
	int size,i;

DPRINTK("start_rx\n");
	zatm_dev = ZATM_DEV(dev);
	size = sizeof(struct atm_vcc *)*zatm_dev->chans;
	zatm_dev->rx_map = (struct atm_vcc **) kmalloc(size,GFP_KERNEL);
	if (!zatm_dev->rx_map) return -ENOMEM;
	memset(zatm_dev->rx_map,0,size);
	/* set VPI/VCI split (use all VCIs and give what's left to VPIs) */
	zpokel(zatm_dev,(1 << dev->ci_range.vci_bits)-1,uPD98401_VRR);
	/* prepare free buffer pools */
	for (i = 0; i <= ZATM_LAST_POOL; i++) {
		zatm_dev->pool_info[i].ref_count = 0;
		zatm_dev->pool_info[i].rqa_count = 0;
		zatm_dev->pool_info[i].rqu_count = 0;
		zatm_dev->pool_info[i].low_water = LOW_MARK;
		zatm_dev->pool_info[i].high_water = HIGH_MARK;
		zatm_dev->pool_info[i].offset = 0;
		zatm_dev->pool_info[i].next_off = 0;
		zatm_dev->pool_info[i].next_cnt = 0;
		zatm_dev->pool_info[i].next_thres = OFF_CNG_THRES;
	}
	return 0;
}


/*----------------------------------- TX ------------------------------------*/


static int do_tx(struct sk_buff *skb)
{
	struct atm_vcc *vcc;
	struct zatm_dev *zatm_dev;
	struct zatm_vcc *zatm_vcc;
	u32 *dsc;
	unsigned long flags;

	EVENT("do_tx\n",0,0);
	DPRINTK("sending skb %p\n",skb);
	vcc = ATM_SKB(skb)->vcc;
	zatm_dev = ZATM_DEV(vcc->dev);
	zatm_vcc = ZATM_VCC(vcc);
	EVENT("iovcnt=%d\n",ATM_SKB(skb)->iovcnt,0);
	save_flags(flags);
	cli();
	if (!ATM_SKB(skb)->iovcnt) {
		if (zatm_vcc->txing == RING_ENTRIES-1) {
			restore_flags(flags);
			return RING_BUSY;
		}
		zatm_vcc->txing++;
		dsc = zatm_vcc->ring+zatm_vcc->ring_curr;
		zatm_vcc->ring_curr = (zatm_vcc->ring_curr+RING_WORDS) &
		    (RING_ENTRIES*RING_WORDS-1);
		dsc[1] = 0;
		dsc[2] = skb->len;
		dsc[3] = virt_to_bus(skb->data);
		mb();
		dsc[0] = uPD98401_TXPD_V | uPD98401_TXPD_DP | uPD98401_TXPD_SM
		    | (vcc->qos.aal == ATM_AAL5 ? uPD98401_TXPD_AAL5 : 0 |
		    (ATM_SKB(skb)->atm_options & ATM_ATMOPT_CLP ?
		    uPD98401_CLPM_1 : uPD98401_CLPM_0));
		EVENT("dsc (0x%lx)\n",(unsigned long) dsc,0);
	}
	else {
printk("NONONONOO!!!!\n");
		dsc = NULL;
#if 0
		u32 *put;
		int i;

		dsc = (u32 *) kmalloc(uPD98401_TXPD_SIZE*2+
		    uPD98401_TXBD_SIZE*ATM_SKB(skb)->iovcnt,GFP_ATOMIC);
		if (!dsc) {
			if (vcc->pop) vcc->pop(vcc,skb);
			else dev_kfree_skb_irq(skb);
			return -EAGAIN;
		}
		/* @@@ should check alignment */
		put = dsc+8;
		dsc[0] = uPD98401_TXPD_V | uPD98401_TXPD_DP |
		    (vcc->aal == ATM_AAL5 ? uPD98401_TXPD_AAL5 : 0 |
		    (ATM_SKB(skb)->atm_options & ATM_ATMOPT_CLP ?
		    uPD98401_CLPM_1 : uPD98401_CLPM_0));
		dsc[1] = 0;
		dsc[2] = ATM_SKB(skb)->iovcnt*uPD98401_TXBD_SIZE;
		dsc[3] = virt_to_bus(put);
		for (i = 0; i < ATM_SKB(skb)->iovcnt; i++) {
			*put++ = ((struct iovec *) skb->data)[i].iov_len;
			*put++ = virt_to_bus(((struct iovec *)
			    skb->data)[i].iov_base);
		}
		put[-2] |= uPD98401_TXBD_LAST;
#endif
	}
	ZATM_PRV_DSC(skb) = dsc;
	skb_queue_tail(&zatm_vcc->tx_queue,skb);
	DPRINTK("QRP=0x%08lx\n",zpeekl(zatm_dev,zatm_vcc->tx_chan*VC_SIZE/4+
	  uPD98401_TXVC_QRP));
	zwait;
	zout(uPD98401_TX_READY | (zatm_vcc->tx_chan <<
	    uPD98401_CHAN_ADDR_SHIFT),CMR);
	restore_flags(flags);
	EVENT("done\n",0,0);
	return 0;
}


static inline void dequeue_tx(struct atm_vcc *vcc)
{
	struct zatm_vcc *zatm_vcc;
	struct sk_buff *skb;

	EVENT("dequeue_tx\n",0,0);
	zatm_vcc = ZATM_VCC(vcc);
	skb = skb_dequeue(&zatm_vcc->tx_queue);
	if (!skb) {
		printk(KERN_CRIT DEV_LABEL "(itf %d): dequeue_tx but not "
		    "txing\n",vcc->dev->number);
		return;
	}
#if 0 /* @@@ would fail on CLP */
if (*ZATM_PRV_DSC(skb) != (uPD98401_TXPD_V | uPD98401_TXPD_DP |
  uPD98401_TXPD_SM | uPD98401_TXPD_AAL5)) printk("@#*$!!!!  (%08x)\n",
  *ZATM_PRV_DSC(skb));
#endif
	*ZATM_PRV_DSC(skb) = 0; /* mark as invalid */
	zatm_vcc->txing--;
	if (vcc->pop) vcc->pop(vcc,skb);
	else dev_kfree_skb_irq(skb);
	while ((skb = skb_dequeue(&zatm_vcc->backlog)))
		if (do_tx(skb) == RING_BUSY) {
			skb_queue_head(&zatm_vcc->backlog,skb);
			break;
		}
	atomic_inc(&vcc->stats->tx);
	wake_up(&zatm_vcc->tx_wait);
}


static void poll_tx(struct atm_dev *dev,int mbx)
{
	struct zatm_dev *zatm_dev;
	unsigned long pos;
	u32 x;

	EVENT("poll_tx\n",0,0);
	zatm_dev = ZATM_DEV(dev);
	pos = (zatm_dev->mbx_start[mbx] & ~0xffffUL) | zin(MTA(mbx));
	while (x = zin(MWA(mbx)), (pos & 0xffff) != x) {
		int chan;

#if 1
		u32 data,*addr;

		EVENT("MBX: host 0x%lx, nic 0x%x\n",pos,x);
		addr = (u32 *) pos;
		data = *addr;
		chan = (data & uPD98401_TXI_CONN) >> uPD98401_TXI_CONN_SHIFT;
		EVENT("addr = 0x%lx, data = 0x%08x,",(unsigned long) addr,
		    data);
		EVENT("chan = %d\n",chan,0);
#else
NO !
		chan = (zatm_dev->mbx_start[mbx][pos >> 2] & uPD98401_TXI_CONN)
		>> uPD98401_TXI_CONN_SHIFT;
#endif
		if (chan < zatm_dev->chans && zatm_dev->tx_map[chan])
			dequeue_tx(zatm_dev->tx_map[chan]);
		else {
			printk(KERN_CRIT DEV_LABEL "(itf %d): TX indication "
			    "for non-existing channel %d\n",dev->number,chan);
			event_dump();
		}
		if (((pos += 4) & 0xffff) == zatm_dev->mbx_end[mbx])
			pos = zatm_dev->mbx_start[mbx];
	}
	zout(pos & 0xffff,MTA(mbx));
}


/*
 * BUG BUG BUG: Doesn't handle "new-style" rate specification yet.
 */

static int alloc_shaper(struct atm_dev *dev,int *pcr,int min,int max,int ubr)
{
	struct zatm_dev *zatm_dev;
	unsigned long flags;
	unsigned long i,m,c;
	int shaper;

	DPRINTK("alloc_shaper (min = %d, max = %d)\n",min,max);
	zatm_dev = ZATM_DEV(dev);
	if (!zatm_dev->free_shapers) return -EAGAIN;
	for (shaper = 0; !((zatm_dev->free_shapers >> shaper) & 1); shaper++);
	zatm_dev->free_shapers &= ~1 << shaper;
	if (ubr) {
		c = 5;
		i = m = 1;
		zatm_dev->ubr_ref_cnt++;
		zatm_dev->ubr = shaper;
	}
	else {
		if (min) {
			if (min <= 255) {
				i = min;
				m = ATM_OC3_PCR;
			}
			else {
				i = 255;
				m = ATM_OC3_PCR*255/min;
			}
		}
		else {
			if (max > zatm_dev->tx_bw) max = zatm_dev->tx_bw;
			if (max <= 255) {
				i = max;
				m = ATM_OC3_PCR;
			}
			else {
				i = 255;
				m = (ATM_OC3_PCR*255+max-1)/max;
			}
		}
		if (i > m) {
			printk(KERN_CRIT DEV_LABEL "shaper algorithm botched "
			    "[%d,%d] -> i=%ld,m=%ld\n",min,max,i,m);
			m = i;
		}
		*pcr = i*ATM_OC3_PCR/m;
		c = 20; /* @@@ should use max_cdv ! */
		if ((min && *pcr < min) || (max && *pcr > max)) return -EINVAL;
		if (zatm_dev->tx_bw < *pcr) return -EAGAIN;
		zatm_dev->tx_bw -= *pcr;
	}
	save_flags(flags);
	cli();
	DPRINTK("i = %d, m = %d, PCR = %d\n",i,m,*pcr);
	zpokel(zatm_dev,(i << uPD98401_IM_I_SHIFT) | m,uPD98401_IM(shaper));
	zpokel(zatm_dev,c << uPD98401_PC_C_SHIFT,uPD98401_PC(shaper));
	zpokel(zatm_dev,0,uPD98401_X(shaper));
	zpokel(zatm_dev,0,uPD98401_Y(shaper));
	zpokel(zatm_dev,uPD98401_PS_E,uPD98401_PS(shaper));
	restore_flags(flags);
	return shaper;
}


static void dealloc_shaper(struct atm_dev *dev,int shaper)
{
	struct zatm_dev *zatm_dev;
	unsigned long flags;

	zatm_dev = ZATM_DEV(dev);
	if (shaper == zatm_dev->ubr) {
		if (--zatm_dev->ubr_ref_cnt) return;
		zatm_dev->ubr = -1;
	}
	save_flags(flags);
	cli();
	zpokel(zatm_dev,zpeekl(zatm_dev,uPD98401_PS(shaper)) & ~uPD98401_PS_E,
	    uPD98401_PS(shaper));
	restore_flags(flags);
	zatm_dev->free_shapers |= 1 << shaper;
}


static void close_tx(struct atm_vcc *vcc)
{
	struct zatm_dev *zatm_dev;
	struct zatm_vcc *zatm_vcc;
	unsigned long flags;
	int chan;
struct sk_buff *skb;
int once = 1;

	zatm_vcc = ZATM_VCC(vcc);
	zatm_dev = ZATM_DEV(vcc->dev);
	chan = zatm_vcc->tx_chan;
	if (!chan) return;
	DPRINTK("close_tx\n");
	save_flags(flags);
	cli();
	while (skb_peek(&zatm_vcc->backlog)) {
if (once) {
printk("waiting for backlog to drain ...\n");
event_dump();
once = 0;
}
		sleep_on(&zatm_vcc->tx_wait);
	}
once = 1;
	while ((skb = skb_peek(&zatm_vcc->tx_queue))) {
if (once) {
printk("waiting for TX queue to drain ... %p\n",skb);
event_dump();
once = 0;
}
		DPRINTK("waiting for TX queue to drain ... %p\n",skb);
		sleep_on(&zatm_vcc->tx_wait);
	}
#if 0
	zwait;
	zout(uPD98401_DEACT_CHAN | (chan << uPD98401_CHAN_ADDR_SHIFT),CMR);
#endif
	zwait;
	zout(uPD98401_CLOSE_CHAN | (chan << uPD98401_CHAN_ADDR_SHIFT),CMR);
	zwait;
	if (!(zin(CMR) & uPD98401_CHAN_ADDR))
		printk(KERN_CRIT DEV_LABEL "(itf %d): can't close TX channel "
		    "%d\n",vcc->dev->number,chan);
	restore_flags(flags);
	zatm_vcc->tx_chan = 0;
	zatm_dev->tx_map[chan] = NULL;
	if (zatm_vcc->shaper != zatm_dev->ubr) {
		zatm_dev->tx_bw += vcc->qos.txtp.min_pcr;
		dealloc_shaper(vcc->dev,zatm_vcc->shaper);
	}
	if (zatm_vcc->ring) kfree(zatm_vcc->ring);
}


static int open_tx_first(struct atm_vcc *vcc)
{
	struct zatm_dev *zatm_dev;
	struct zatm_vcc *zatm_vcc;
	unsigned long flags;
	u32 *loop;
	unsigned short chan;
	int pcr,unlimited;

	DPRINTK("open_tx_first\n");
	zatm_dev = ZATM_DEV(vcc->dev);
	zatm_vcc = ZATM_VCC(vcc);
	zatm_vcc->tx_chan = 0;
	if (vcc->qos.txtp.traffic_class == ATM_NONE) return 0;
	save_flags(flags);
	cli();
	zwait;
	zout(uPD98401_OPEN_CHAN,CMR);
	zwait;
	DPRINTK("0x%x 0x%x\n",zin(CMR),zin(CER));
	chan = (zin(CMR) & uPD98401_CHAN_ADDR) >> uPD98401_CHAN_ADDR_SHIFT;
	restore_flags(flags);
	DPRINTK("chan is %d\n",chan);
	if (!chan) return -EAGAIN;
	unlimited = vcc->qos.txtp.traffic_class == ATM_UBR &&
	    (!vcc->qos.txtp.max_pcr || vcc->qos.txtp.max_pcr == ATM_MAX_PCR ||
	    vcc->qos.txtp.max_pcr >= ATM_OC3_PCR);
	if (unlimited && zatm_dev->ubr != -1) zatm_vcc->shaper = zatm_dev->ubr;
	else {
		if (unlimited) vcc->qos.txtp.max_sdu = ATM_MAX_AAL5_PDU;
		if ((zatm_vcc->shaper = alloc_shaper(vcc->dev,&pcr,
		    vcc->qos.txtp.min_pcr,vcc->qos.txtp.max_pcr,unlimited))
		    < 0) {
			close_tx(vcc);
			return zatm_vcc->shaper;
		}
		if (pcr > ATM_OC3_PCR) pcr = ATM_OC3_PCR;
		vcc->qos.txtp.min_pcr = vcc->qos.txtp.max_pcr = pcr;
	}
	zatm_vcc->tx_chan = chan;
	skb_queue_head_init(&zatm_vcc->tx_queue);
	init_waitqueue_head(&zatm_vcc->tx_wait);
	/* initialize ring */
	zatm_vcc->ring = kmalloc(RING_SIZE,GFP_KERNEL);
	if (!zatm_vcc->ring) return -ENOMEM;
	memset(zatm_vcc->ring,0,RING_SIZE);
	loop = zatm_vcc->ring+RING_ENTRIES*RING_WORDS;
	loop[0] = uPD98401_TXPD_V;
	loop[1] = loop[2] = 0;
	loop[3] = virt_to_bus(zatm_vcc->ring);
	zatm_vcc->ring_curr = 0;
	zatm_vcc->txing = 0;
	skb_queue_head_init(&zatm_vcc->backlog);
	zpokel(zatm_dev,virt_to_bus(zatm_vcc->ring),
	    chan*VC_SIZE/4+uPD98401_TXVC_QRP);
	return 0;
}


static int open_tx_second(struct atm_vcc *vcc)
{
	struct zatm_dev *zatm_dev;
	struct zatm_vcc *zatm_vcc;
	unsigned long flags;

	DPRINTK("open_tx_second\n");
	zatm_dev = ZATM_DEV(vcc->dev);
	zatm_vcc = ZATM_VCC(vcc);
	if (!zatm_vcc->tx_chan) return 0;
	save_flags(flags);
	/* set up VC descriptor */
	cli();
	zpokel(zatm_dev,0,zatm_vcc->tx_chan*VC_SIZE/4);
	zpokel(zatm_dev,uPD98401_TXVC_L | (zatm_vcc->shaper <<
	    uPD98401_TXVC_SHP_SHIFT) | (vcc->vpi << uPD98401_TXVC_VPI_SHIFT) |
	    vcc->vci,zatm_vcc->tx_chan*VC_SIZE/4+1);
	zpokel(zatm_dev,0,zatm_vcc->tx_chan*VC_SIZE/4+2);
	restore_flags(flags);
	zatm_dev->tx_map[zatm_vcc->tx_chan] = vcc;
	return 0;
}


static int start_tx(struct atm_dev *dev)
{
	struct zatm_dev *zatm_dev;
	int i;

	DPRINTK("start_tx\n");
	zatm_dev = ZATM_DEV(dev);
	zatm_dev->tx_map = (struct atm_vcc **) kmalloc(sizeof(struct atm_vcc *)*
	    zatm_dev->chans,GFP_KERNEL);
	if (!zatm_dev->tx_map) return -ENOMEM;
	zatm_dev->tx_bw = ATM_OC3_PCR;
	zatm_dev->free_shapers = (1 << NR_SHAPERS)-1;
	zatm_dev->ubr = -1;
	zatm_dev->ubr_ref_cnt = 0;
	/* initialize shapers */
	for (i = 0; i < NR_SHAPERS; i++) zpokel(zatm_dev,0,uPD98401_PS(i));
	return 0;
}


/*------------------------------- interrupts --------------------------------*/


static void zatm_int(int irq,void *dev_id,struct pt_regs *regs)
{
	struct atm_dev *dev;
	struct zatm_dev *zatm_dev;
	u32 reason;

	dev = dev_id;
	zatm_dev = ZATM_DEV(dev);
	while ((reason = zin(GSR))) {
		EVENT("reason 0x%x\n",reason,0);
		if (reason & uPD98401_INT_PI) {
			EVENT("PHY int\n",0,0);
			dev->phy->interrupt(dev);
		}
		if (reason & uPD98401_INT_RQA) {
			unsigned long pools;
			int i;

			pools = zin(RQA);
			EVENT("RQA (0x%08x)\n",pools,0);
			for (i = 0; pools; i++) {
				if (pools & 1) {
					refill_pool(dev,i);
					zatm_dev->pool_info[i].rqa_count++;
				}
				pools >>= 1;
			}
		}
		if (reason & uPD98401_INT_RQU) {
			unsigned long pools;
			int i;
			pools = zin(RQU);
			printk(KERN_WARNING DEV_LABEL "(itf %d): RQU 0x%08lx\n",
			    dev->number,pools);
			event_dump();
			for (i = 0; pools; i++) {
				if (pools & 1) {
					refill_pool(dev,i);
					zatm_dev->pool_info[i].rqu_count++;
				}
				pools >>= 1;
			}
		}
		/* don't handle RD */
		if (reason & uPD98401_INT_SPE)
			printk(KERN_ALERT DEV_LABEL "(itf %d): system parity "
			    "error at 0x%08x\n",dev->number,zin(ADDR));
		if (reason & uPD98401_INT_CPE)
			printk(KERN_ALERT DEV_LABEL "(itf %d): control memory "
			    "parity error at 0x%08x\n",dev->number,zin(ADDR));
		if (reason & uPD98401_INT_SBE) {
			printk(KERN_ALERT DEV_LABEL "(itf %d): system bus "
			    "error at 0x%08x\n",dev->number,zin(ADDR));
			event_dump();
		}
		/* don't handle IND */
		if (reason & uPD98401_INT_MF) {
			printk(KERN_CRIT DEV_LABEL "(itf %d): mailbox full "
			    "(0x%x)\n",dev->number,(reason & uPD98401_INT_MF)
			    >> uPD98401_INT_MF_SHIFT);
			event_dump();
			    /* @@@ should try to recover */
		}
		if (reason & uPD98401_INT_MM) {
			if (reason & 1) poll_rx(dev,0);
			if (reason & 2) poll_rx(dev,1);
			if (reason & 4) poll_tx(dev,2);
			if (reason & 8) poll_tx(dev,3);
		}
		/* @@@ handle RCRn */
	}
}


/*----------------------------- (E)EPROM access -----------------------------*/


static void __init eprom_set(struct zatm_dev *zatm_dev,unsigned long value,
    unsigned short cmd)
{
	int error;

	if ((error = pci_write_config_dword(zatm_dev->pci_dev,cmd,value)))
		printk(KERN_ERR DEV_LABEL ": PCI write failed (0x%02x)\n",
		    error);
}


static unsigned long __init eprom_get(struct zatm_dev *zatm_dev,
    unsigned short cmd)
{
	unsigned int value;
	int error;

	if ((error = pci_read_config_dword(zatm_dev->pci_dev,cmd,&value)))
		printk(KERN_ERR DEV_LABEL ": PCI read failed (0x%02x)\n",
		    error);
	return value;
}


static void __init eprom_put_bits(struct zatm_dev *zatm_dev,
    unsigned long data,int bits,unsigned short cmd)
{
	unsigned long value;
	int i;

	for (i = bits-1; i >= 0; i--) {
		value = ZEPROM_CS | (((data >> i) & 1) ? ZEPROM_DI : 0);
		eprom_set(zatm_dev,value,cmd);
		eprom_set(zatm_dev,value | ZEPROM_SK,cmd);
		eprom_set(zatm_dev,value,cmd);
	}
}


static void __init eprom_get_byte(struct zatm_dev *zatm_dev,
    unsigned char *byte,unsigned short cmd)
{
	int i;

	*byte = 0;
	for (i = 8; i; i--) {
		eprom_set(zatm_dev,ZEPROM_CS,cmd);
		eprom_set(zatm_dev,ZEPROM_CS | ZEPROM_SK,cmd);
		*byte <<= 1;
		if (eprom_get(zatm_dev,cmd) & ZEPROM_DO) *byte |= 1;
		eprom_set(zatm_dev,ZEPROM_CS,cmd);
	}
}


static unsigned char __init eprom_try_esi(struct atm_dev *dev,
    unsigned short cmd,int offset,int swap)
{
	unsigned char buf[ZEPROM_SIZE];
	struct zatm_dev *zatm_dev;
	int i;

	zatm_dev = ZATM_DEV(dev);
	for (i = 0; i < ZEPROM_SIZE; i += 2) {
		eprom_set(zatm_dev,ZEPROM_CS,cmd); /* select EPROM */
		eprom_put_bits(zatm_dev,ZEPROM_CMD_READ,ZEPROM_CMD_LEN,cmd);
		eprom_put_bits(zatm_dev,i >> 1,ZEPROM_ADDR_LEN,cmd);
		eprom_get_byte(zatm_dev,buf+i+swap,cmd);
		eprom_get_byte(zatm_dev,buf+i+1-swap,cmd);
		eprom_set(zatm_dev,0,cmd); /* deselect EPROM */
	}
	memcpy(dev->esi,buf+offset,ESI_LEN);
	return memcmp(dev->esi,"\0\0\0\0\0",ESI_LEN); /* assumes ESI_LEN == 6 */
}


static void __init eprom_get_esi(struct atm_dev *dev)
{
	if (eprom_try_esi(dev,ZEPROM_V1_REG,ZEPROM_V1_ESI_OFF,1)) return;
	(void) eprom_try_esi(dev,ZEPROM_V2_REG,ZEPROM_V2_ESI_OFF,0);
}


/*--------------------------------- entries ---------------------------------*/


static int __init zatm_init(struct atm_dev *dev)
{
	struct zatm_dev *zatm_dev;
	struct pci_dev *pci_dev;
	unsigned short command;
	unsigned char revision;
	int error,i,last;
	unsigned long t0,t1,t2;

	DPRINTK(">zatm_init\n");
	zatm_dev = ZATM_DEV(dev);
	pci_dev = zatm_dev->pci_dev;
	zatm_dev->base = pci_resource_start(pci_dev, 0);
	zatm_dev->irq = pci_dev->irq;
	if ((error = pci_read_config_word(pci_dev,PCI_COMMAND,&command)) ||
	    (error = pci_read_config_byte(pci_dev,PCI_REVISION_ID,&revision))) {
		printk(KERN_ERR DEV_LABEL "(itf %d): init error 0x%02x\n",
		    dev->number,error);
		return -EINVAL;
	}
	if ((error = pci_write_config_word(pci_dev,PCI_COMMAND,
	    command | PCI_COMMAND_IO | PCI_COMMAND_MASTER))) {
		printk(KERN_ERR DEV_LABEL "(itf %d): can't enable IO (0x%02x)"
		    "\n",dev->number,error);
		return -EIO;
	}
	eprom_get_esi(dev);
	printk(KERN_NOTICE DEV_LABEL "(itf %d): rev.%d,base=0x%x,irq=%d,",
	    dev->number,revision,zatm_dev->base,zatm_dev->irq);
	/* reset uPD98401 */
	zout(0,SWR);
	while (!(zin(GSR) & uPD98401_INT_IND));
	zout(uPD98401_GMR_ONE /*uPD98401_BURST4*/,GMR);
	last = MAX_CRAM_SIZE;
	for (i = last-RAM_INCREMENT; i >= 0; i -= RAM_INCREMENT) {
		zpokel(zatm_dev,0x55555555,i);
		if (zpeekl(zatm_dev,i) != 0x55555555) last = i;
		else {
			zpokel(zatm_dev,0xAAAAAAAA,i);
			if (zpeekl(zatm_dev,i) != 0xAAAAAAAA) last = i;
			else zpokel(zatm_dev,i,i);
		}
	}
	for (i = 0; i < last; i += RAM_INCREMENT)
		if (zpeekl(zatm_dev,i) != i) break;
	zatm_dev->mem = i << 2;
	while (i) zpokel(zatm_dev,0,--i);
	/* reset again to rebuild memory pointers */
	zout(0,SWR);
	while (!(zin(GSR) & uPD98401_INT_IND));
	zout(uPD98401_GMR_ONE | uPD98401_BURST8 | uPD98401_BURST4 |
	    uPD98401_BURST2 | uPD98401_GMR_PM | uPD98401_GMR_DR,GMR);
	/* TODO: should shrink allocation now */
	printk("mem=%dkB,%s (",zatm_dev->mem >> 10,zatm_dev->copper ? "UTP" :
	    "MMF");
	for (i = 0; i < ESI_LEN; i++)
		printk("%02X%s",dev->esi[i],i == ESI_LEN-1 ? ")\n" : "-");
	do {
		unsigned long flags;

		save_flags(flags);
		cli();
		t0 = zpeekl(zatm_dev,uPD98401_TSR);
		udelay(10);
		t1 = zpeekl(zatm_dev,uPD98401_TSR);
		udelay(1010);
		t2 = zpeekl(zatm_dev,uPD98401_TSR);
		restore_flags(flags);
	}
	while (t0 > t1 || t1 > t2); /* loop if wrapping ... */
	zatm_dev->khz = t2-2*t1+t0;
	printk(KERN_NOTICE DEV_LABEL "(itf %d): uPD98401 %d.%d at %d.%03d "
	    "MHz\n",dev->number,
	    (zin(VER) & uPD98401_MAJOR) >> uPD98401_MAJOR_SHIFT,
            zin(VER) & uPD98401_MINOR,zatm_dev->khz/1000,zatm_dev->khz % 1000);
#ifdef CONFIG_ATM_ZATM_EXACT_TS
	zatm_clock_init(zatm_dev);
#endif
	return uPD98402_init(dev);
}


static int __init zatm_start(struct atm_dev *dev)
{
	struct zatm_dev *zatm_dev;
	unsigned long curr;
	int pools,vccs,rx;
	int error,i,ld;

	DPRINTK("zatm_start\n");
	zatm_dev = ZATM_DEV(dev);
	if (request_irq(zatm_dev->irq,&zatm_int,SA_SHIRQ,DEV_LABEL,dev)) {
		printk(KERN_ERR DEV_LABEL "(itf %d): IRQ%d is already in use\n",
		    dev->number,zatm_dev->irq);
		return -EAGAIN;
	}
	request_region(zatm_dev->base,uPD98401_PORTS,DEV_LABEL);
	/* define memory regions */
	pools = NR_POOLS;
	if (NR_SHAPERS*SHAPER_SIZE > pools*POOL_SIZE)
		pools = NR_SHAPERS*SHAPER_SIZE/POOL_SIZE;
	vccs = (zatm_dev->mem-NR_SHAPERS*SHAPER_SIZE-pools*POOL_SIZE)/
	    (2*VC_SIZE+RX_SIZE);
	ld = -1;
	for (rx = 1; rx < vccs; rx <<= 1) ld++;
	dev->ci_range.vpi_bits = 0; /* @@@ no VPI for now */
	dev->ci_range.vci_bits = ld;
	dev->link_rate = ATM_OC3_PCR;
	zatm_dev->chans = vccs; /* ??? */
	curr = rx*RX_SIZE/4;
	DPRINTK("RX pool 0x%08lx\n",curr);
	zpokel(zatm_dev,curr,uPD98401_PMA); /* receive pool */
	zatm_dev->pool_base = curr;
	curr += pools*POOL_SIZE/4;
	DPRINTK("Shapers 0x%08lx\n",curr);
	zpokel(zatm_dev,curr,uPD98401_SMA); /* shapers */
	curr += NR_SHAPERS*SHAPER_SIZE/4;
	DPRINTK("Free    0x%08lx\n",curr);
	zpokel(zatm_dev,curr,uPD98401_TOS); /* free pool */
	printk(KERN_INFO DEV_LABEL "(itf %d): %d shapers, %d pools, %d RX, "
	    "%ld VCs\n",dev->number,NR_SHAPERS,pools,rx,
	    (zatm_dev->mem-curr*4)/VC_SIZE);
	/* create mailboxes */
	for (i = 0; i < NR_MBX; i++) zatm_dev->mbx_start[i] = 0;
	for (i = 0; i < NR_MBX; i++)
		if (mbx_entries[i]) {
			unsigned long here;

			here = (unsigned long) kmalloc(2*MBX_SIZE(i),
			    GFP_KERNEL);
			if (!here) return -ENOMEM;
			if ((here^(here+MBX_SIZE(i))) & ~0xffffUL)/* paranoia */
				here = (here & ~0xffffUL)+0x10000;
			if ((here^virt_to_bus((void *) here)) & 0xffff) {
				printk(KERN_ERR DEV_LABEL "(itf %d): system "
				    "bus incompatible with driver\n",
				    dev->number);
				kfree((void *) here);
				return -ENODEV;
			}
			DPRINTK("mbx@0x%08lx-0x%08lx\n",here,here+MBX_SIZE(i));
			zatm_dev->mbx_start[i] = here;
			zatm_dev->mbx_end[i] = (here+MBX_SIZE(i)) & 0xffff;
			zout(virt_to_bus((void *) here) >> 16,MSH(i));
			zout(virt_to_bus((void *) here),MSL(i));
			zout((here+MBX_SIZE(i)) & 0xffff,MBA(i));
			zout(here & 0xffff,MTA(i));
			zout(here & 0xffff,MWA(i));
		}
	error = start_tx(dev);
	if (error) return error;
	error = start_rx(dev);
	if (error) return error;
	error = dev->phy->start(dev);
	if (error) return error;
	zout(0xffffffff,IMR); /* enable interrupts */
	/* enable TX & RX */
	zout(zin(GMR) | uPD98401_GMR_SE | uPD98401_GMR_RE,GMR);
	return 0;
}


static void zatm_close(struct atm_vcc *vcc)
{
        DPRINTK(">zatm_close\n");
        if (!ZATM_VCC(vcc)) return;
	clear_bit(ATM_VF_READY,&vcc->flags);
        close_rx(vcc);
	EVENT("close_tx\n",0,0);
        close_tx(vcc);
        DPRINTK("zatm_close: done waiting\n");
        /* deallocate memory */
        kfree(ZATM_VCC(vcc));
        ZATM_VCC(vcc) = NULL;
	clear_bit(ATM_VF_ADDR,&vcc->flags);
}


static int zatm_open(struct atm_vcc *vcc,short vpi,int vci)
{
	struct zatm_dev *zatm_dev;
	struct zatm_vcc *zatm_vcc;
	int error;

	DPRINTK(">zatm_open\n");
	zatm_dev = ZATM_DEV(vcc->dev);
	if (!test_bit(ATM_VF_PARTIAL,&vcc->flags)) ZATM_VCC(vcc) = NULL;
	error = atm_find_ci(vcc,&vpi,&vci);
	if (error) return error;
	vcc->vpi = vpi;
	vcc->vci = vci;
	if (vci != ATM_VPI_UNSPEC && vpi != ATM_VCI_UNSPEC)
		set_bit(ATM_VF_ADDR,&vcc->flags);
	if (vcc->qos.aal != ATM_AAL5) return -EINVAL; /* @@@ AAL0 */
	DPRINTK(DEV_LABEL "(itf %d): open %d.%d\n",vcc->dev->number,vcc->vpi,
	    vcc->vci);
	if (!test_bit(ATM_VF_PARTIAL,&vcc->flags)) {
		zatm_vcc = kmalloc(sizeof(struct zatm_vcc),GFP_KERNEL);
		if (!zatm_vcc) {
			clear_bit(ATM_VF_ADDR,&vcc->flags);
			return -ENOMEM;
		}
		ZATM_VCC(vcc) = zatm_vcc;
		ZATM_VCC(vcc)->tx_chan = 0; /* for zatm_close after open_rx */
		if ((error = open_rx_first(vcc))) {
	                zatm_close(vcc);
	                return error;
	        }
		if ((error = open_tx_first(vcc))) {
			zatm_close(vcc);
			return error;
	        }
	}
	if (vci == ATM_VPI_UNSPEC || vpi == ATM_VCI_UNSPEC) return 0;
	if ((error = open_rx_second(vcc))) {
		zatm_close(vcc);
		return error;
        }
	if ((error = open_tx_second(vcc))) {
		zatm_close(vcc);
		return error;
        }
	set_bit(ATM_VF_READY,&vcc->flags);
        return 0;
}


static int zatm_change_qos(struct atm_vcc *vcc,struct atm_qos *qos,int flags)
{
	printk("Not yet implemented\n");
	return -ENOSYS;
	/* @@@ */
}


static int zatm_ioctl(struct atm_dev *dev,unsigned int cmd,void *arg)
{
	struct zatm_dev *zatm_dev;
	unsigned long flags;

	zatm_dev = ZATM_DEV(dev);
	switch (cmd) {
		case ZATM_GETPOOLZ:
			if (!capable(CAP_NET_ADMIN)) return -EPERM;
			/* fall through */
		case ZATM_GETPOOL:
			{
				struct zatm_pool_info info;
				int pool;

				if (get_user(pool,
				    &((struct zatm_pool_req *) arg)->pool_num))
					return -EFAULT;
				if (pool < 0 || pool > ZATM_LAST_POOL)
					return -EINVAL;
				save_flags(flags);
				cli();
				info = zatm_dev->pool_info[pool];
				if (cmd == ZATM_GETPOOLZ) {
					zatm_dev->pool_info[pool].rqa_count = 0;
					zatm_dev->pool_info[pool].rqu_count = 0;
				}
				restore_flags(flags);
				return copy_to_user(
				    &((struct zatm_pool_req *) arg)->info,
				    &info,sizeof(info)) ? -EFAULT : 0;
			}
		case ZATM_SETPOOL:
			{
				struct zatm_pool_info info;
				int pool;

				if (!capable(CAP_NET_ADMIN)) return -EPERM;
				if (get_user(pool,
				    &((struct zatm_pool_req *) arg)->pool_num))
					return -EFAULT;
				if (pool < 0 || pool > ZATM_LAST_POOL)
					return -EINVAL;
				if (copy_from_user(&info,
				    &((struct zatm_pool_req *) arg)->info,
				    sizeof(info))) return -EFAULT;
				if (!info.low_water)
					info.low_water = zatm_dev->
					    pool_info[pool].low_water;
				if (!info.high_water)
					info.high_water = zatm_dev->
					    pool_info[pool].high_water;
				if (!info.next_thres)
					info.next_thres = zatm_dev->
					    pool_info[pool].next_thres;
				if (info.low_water >= info.high_water ||
				    info.low_water < 0)
					return -EINVAL;
				save_flags(flags);
				cli();
				zatm_dev->pool_info[pool].low_water =
				    info.low_water;
				zatm_dev->pool_info[pool].high_water =
				    info.high_water;
				zatm_dev->pool_info[pool].next_thres =
				    info.next_thres;
				restore_flags(flags);
				return 0;
			}
#ifdef CONFIG_ATM_ZATM_EXACT_TS
		case ZATM_GETTHIST:
			{
				int i;

				save_flags(flags);
				cli();
				for (i = 0; i < ZATM_TIMER_HISTORY_SIZE; i++) {
					if (!copy_to_user(
					    (struct zatm_t_hist *) arg+i,
					    &zatm_dev->timer_history[
					    (zatm_dev->th_curr+i) &
					    (ZATM_TIMER_HISTORY_SIZE-1)],
					    sizeof(struct zatm_t_hist)))
						 continue;
					restore_flags(flags);
					return -EFAULT;
				}
				restore_flags(flags);
				return 0;
			}
#endif
		default:
        		if (!dev->phy->ioctl) return -ENOIOCTLCMD;
		        return dev->phy->ioctl(dev,cmd,arg);
	}
}


static int zatm_getsockopt(struct atm_vcc *vcc,int level,int optname,
    void *optval,int optlen)
{
	return -EINVAL;
}


static int zatm_setsockopt(struct atm_vcc *vcc,int level,int optname,
    void *optval,int optlen)
{
	return -EINVAL;
}


#if 0
static int zatm_sg_send(struct atm_vcc *vcc,unsigned long start,
    unsigned long size)
{
	return vcc->aal == ATM_AAL5;
	   /* @@@ should check size and maybe alignment*/
}
#endif


static int zatm_send(struct atm_vcc *vcc,struct sk_buff *skb)
{
	int error;

	EVENT(">zatm_send 0x%lx\n",(unsigned long) skb,0);
	if (!ZATM_VCC(vcc)->tx_chan || !test_bit(ATM_VF_READY,&vcc->flags)) {
		if (vcc->pop) vcc->pop(vcc,skb);
		else dev_kfree_skb(skb);
		return -EINVAL;
	}
	if (!skb) {
		printk(KERN_CRIT "!skb in zatm_send ?\n");
		if (vcc->pop) vcc->pop(vcc,skb);
		return -EINVAL;
	}
	ATM_SKB(skb)->vcc = vcc;
	error = do_tx(skb);
	if (error != RING_BUSY) return error;
	skb_queue_tail(&ZATM_VCC(vcc)->backlog,skb);
	return 0;
}


static void zatm_phy_put(struct atm_dev *dev,unsigned char value,
    unsigned long addr)
{
	struct zatm_dev *zatm_dev;

	zatm_dev = ZATM_DEV(dev);
	zwait;
	zout(value,CER);
	zout(uPD98401_IND_ACC | uPD98401_IA_B0 |
	    (uPD98401_IA_TGT_PHY << uPD98401_IA_TGT_SHIFT) | addr,CMR);
}


static unsigned char zatm_phy_get(struct atm_dev *dev,unsigned long addr)
{
	struct zatm_dev *zatm_dev;

	zatm_dev = ZATM_DEV(dev);
	zwait;
	zout(uPD98401_IND_ACC | uPD98401_IA_B0 | uPD98401_IA_RW |
	  (uPD98401_IA_TGT_PHY << uPD98401_IA_TGT_SHIFT) | addr,CMR);
	zwait;
	return zin(CER) & 0xff;
}


static const struct atmdev_ops ops = {
	open:		zatm_open,
	close:		zatm_close,
	ioctl:		zatm_ioctl,
	getsockopt:	zatm_getsockopt,
	setsockopt:	zatm_setsockopt,
	send:		zatm_send,
	/*zatm_sg_send*/
	phy_put:	zatm_phy_put,
	phy_get:	zatm_phy_get,
	feedback:	zatm_feedback,
	change_qos:	zatm_change_qos,
};


int __init zatm_detect(void)
{
	struct atm_dev *dev;
	struct zatm_dev *zatm_dev;
	int devs,type;

	zatm_dev = (struct zatm_dev *) kmalloc(sizeof(struct zatm_dev),
	    GFP_KERNEL);
	if (!zatm_dev) return -ENOMEM;
	devs = 0;
	for (type = 0; type < 2; type++) {
		struct pci_dev *pci_dev;

		pci_dev = NULL;
		while ((pci_dev = pci_find_device(PCI_VENDOR_ID_ZEITNET,type ?
		    PCI_DEVICE_ID_ZEITNET_1225 : PCI_DEVICE_ID_ZEITNET_1221,
		    pci_dev))) {
			if (pci_enable_device(pci_dev)) break;
			dev = atm_dev_register(DEV_LABEL,&ops,-1,NULL);
			if (!dev) break;
			zatm_dev->pci_dev = pci_dev;
			ZATM_DEV(dev) = zatm_dev;
			zatm_dev->copper = type;
			if (zatm_init(dev) || zatm_start(dev)) {
				atm_dev_deregister(dev);
				break;
			}
			zatm_dev->more = zatm_boards;
			zatm_boards = dev;
			devs++;
			zatm_dev = (struct zatm_dev *) kmalloc(sizeof(struct
			    zatm_dev),GFP_KERNEL);
			if (!zatm_dev) break;
		}
	}
	return devs;
}


#ifdef MODULE
 
int init_module(void)
{
	if (!zatm_detect()) {
		printk(KERN_ERR DEV_LABEL ": no adapter found\n");
		return -ENXIO;
	}
	MOD_INC_USE_COUNT;
	return 0;
}
 
 
void cleanup_module(void)
{
	/*
	 * Well, there's no way to get rid of the driver yet, so we don't
	 * have to clean up, right ? :-)
	 */
}
 
#endif
