/* r3964 linediscipline for linux
 *
 * -----------------------------------------------------------
 * Copyright by 
 * Philips Automation Projects
 * Kassel (Germany)
 * http://www.pap-philips.de
 * -----------------------------------------------------------
 * This software may be used and distributed according to the terms of
 * the GNU Public License, incorporated herein by reference.
 *
 * Author:
 * L. Haag
 *
 * $Log: n_r3964.c,v $
 * Revision 1.8  2000/03/23 14:14:54  dwmw2
 * Fix race in sleeping in r3964_read()
 *
 * Revision 1.7  1999/28/08 11:41:50  dwmw2
 * Port to 2.3 kernel
 *
 * Revision 1.6  1998/09/30 00:40:40  dwmw2
 * Fixed compilation on 2.0.x kernels
 * Updated to newly registered tty-ldisc number 9
 *
 * Revision 1.5  1998/09/04 21:57:36  dwmw2
 * Signal handling bug fixes, port to 2.1.x.
 *
 * Revision 1.4  1998/04/02 20:26:59  lhaag
 * select, blocking, ...
 *
 * Revision 1.3  1998/02/12 18:58:43  root
 * fixed some memory leaks
 * calculation of checksum characters
 *
 * Revision 1.2  1998/02/07 13:03:34  root
 * ioctl read_telegram
 *
 * Revision 1.1  1998/02/06 19:21:03  root
 * Initial revision
 *
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/malloc.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/string.h>   /* used in new tty drivers */
#include <linux/signal.h>   /* used in new tty drivers */
#include <linux/ioctl.h>
#include <linux/n_r3964.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <asm/uaccess.h>


//#define DEBUG_QUEUE

/* Log successfull handshake and protocol operations  */
//#define DEBUG_PROTO_S

/* Log handshake and protocol errors: */
//#define DEBUG_PROTO_E

/* Log Linediscipline operations (open, close, read, write...): */
//#define DEBUG_LDISC

/* Log module and memory operations (init, cleanup; kmalloc, kfree): */
//#define DEBUG_MODUL

/* Macro helpers for debug output: */
#define TRACE(format, args...) printk("r3964: " format "\n" , ## args);

#ifdef DEBUG_MODUL
#define TRACE_M(format, args...) printk("r3964: " format "\n" , ## args);
#else
#define TRACE_M(fmt, arg...) /**/
#endif

#ifdef DEBUG_PROTO_S
#define TRACE_PS(format, args...) printk("r3964: " format "\n" , ## args);
#else
#define TRACE_PS(fmt, arg...) /**/
#endif

#ifdef DEBUG_PROTO_E
#define TRACE_PE(format, args...) printk("r3964: " format "\n" , ## args);
#else
#define TRACE_PE(fmt, arg...) /**/
#endif

#ifdef DEBUG_LDISC
#define TRACE_L(format, args...) printk("r3964: " format "\n" , ## args);
#else
#define TRACE_L(fmt, arg...) /**/
#endif

#ifdef DEBUG_QUEUE
#define TRACE_Q(format, args...) printk("r3964: " format "\n" , ## args);
#else
#define TRACE_Q(fmt, arg...) /**/
#endif

static void on_timer_1(void*);
static void on_timer_2(void*);
static void add_tx_queue(struct r3964_info *, struct r3964_block_header *);
static void remove_from_tx_queue(struct r3964_info *pInfo, int error_code);
static void put_char(struct r3964_info *pInfo, unsigned char ch);
static void trigger_transmit(struct r3964_info *pInfo);
static void retry_transmit(struct r3964_info *pInfo);
static void transmit_block(struct r3964_info *pInfo);
static void receive_char(struct r3964_info *pInfo, const unsigned char c);
static void receive_error(struct r3964_info *pInfo, const char flag);
static void on_timeout(struct r3964_info *pInfo);
static int enable_signals(struct r3964_info *pInfo, pid_t pid, int arg);
static int read_telegram(struct r3964_info *pInfo, pid_t pid, unsigned char *buf);
static void add_msg(struct r3964_client_info *pClient, int msg_id, int arg,
             int error_code, struct r3964_block_header *pBlock);
static struct r3964_message* remove_msg(struct r3964_info *pInfo, 
             struct r3964_client_info *pClient);
static void remove_client_block(struct r3964_info *pInfo, 
                struct r3964_client_info *pClient);

static int  r3964_open(struct tty_struct *tty);
static void r3964_close(struct tty_struct *tty);
static int  r3964_read(struct tty_struct *tty, struct file *file,
                     unsigned char *buf, unsigned int nr);
static int  r3964_write(struct tty_struct * tty, struct file * file,
                      const unsigned char * buf, unsigned int nr);
static int r3964_ioctl(struct tty_struct * tty, struct file * file,
                       unsigned int cmd, unsigned long arg);
static void r3964_set_termios(struct tty_struct *tty, struct termios * old);
static unsigned int r3964_poll(struct tty_struct * tty, struct file * file,
		      struct poll_table_struct  *wait);
static void r3964_receive_buf(struct tty_struct *tty, const unsigned char *cp,
                              char *fp, int count);
static int  r3964_receive_room(struct tty_struct *tty);

static struct tty_ldisc tty_ldisc_N_R3964 = {
        TTY_LDISC_MAGIC,       /* magic */
	"R3964",               /* name */
        0,                     /* num */
        0,                     /* flags */
        r3964_open,            /* open */
        r3964_close,           /* close */
        0,                     /* flush_buffer */
        0,                     /* chars_in_buffer */
        r3964_read,            /* read */
        r3964_write,           /* write */
        r3964_ioctl,           /* ioctl */
        r3964_set_termios,     /* set_termios */
        r3964_poll,            /* poll */            
        r3964_receive_buf,     /* receive_buf */
        r3964_receive_room,    /* receive_room */
        0                      /* write_wakeup */
};



static void dump_block(const unsigned char *block, unsigned int length)
{
   unsigned int i,j;
   char linebuf[16*3+1];
   
   for(i=0;i<length;i+=16)
   {
      for(j=0;(j<16) && (j+i<length);j++)
      {
         sprintf(linebuf+3*j,"%02x ",block[i+j]);
      }
      linebuf[3*j]='\0';
      TRACE_PS("%s",linebuf);
   }
}

         


/*************************************************************
 * Driver initialisation
 *************************************************************/


/*************************************************************
 * Module support routines
 *************************************************************/

static void __exit r3964_exit(void)
{
   int status;
   
   TRACE_M ("cleanup_module()");

   status=tty_register_ldisc(N_R3964, NULL);
   
   if(status!=0)
   {
      printk(KERN_ERR "r3964: error unregistering linediscipline: %d\n", status);
   }
   else
   {
      TRACE_L("linediscipline successfully unregistered");
   }
   
}

static int __init r3964_init(void)
{
   int status;
   
   printk ("r3964: Philips r3964 Driver $Revision: 1.8 $\n");

   /*
    * Register the tty line discipline
    */
   
   status = tty_register_ldisc (N_R3964, &tty_ldisc_N_R3964);
   if (status == 0)
     {
       TRACE_L("line discipline %d registered", N_R3964);
       TRACE_L("flags=%x num=%x", tty_ldisc_N_R3964.flags, 
               tty_ldisc_N_R3964.num);
       TRACE_L("open=%x", (int)tty_ldisc_N_R3964.open);
       TRACE_L("tty_ldisc_N_R3964 = %x", (int)&tty_ldisc_N_R3964);
     }
   else
     {
       printk (KERN_ERR "r3964: error registering line discipline: %d\n", status);
     }
   return status;
}

module_init(r3964_init);
module_exit(r3964_exit);


/*************************************************************
 * Protocol implementation routines
 *************************************************************/

static void on_timer_1(void *arg)
{
   struct r3964_info *pInfo = (struct r3964_info *)arg;
  
   if(pInfo->count_down)
   {
      if(!--pInfo->count_down)
      {
         on_timeout(pInfo);
      }
   }
   queue_task(&pInfo->bh_2, &tq_timer);
}

static void on_timer_2(void *arg)
{
   struct r3964_info *pInfo = (struct r3964_info *)arg;
  
   if(pInfo->count_down)
   {
      if(!--pInfo->count_down)
      {
         on_timeout(pInfo);
      }
   }
   queue_task(&pInfo->bh_1, &tq_timer);
}

static void add_tx_queue(struct r3964_info *pInfo, struct r3964_block_header *pHeader)
{
   unsigned long flags;
   
   save_flags(flags);
   cli();

   pHeader->next = NULL;

   if(pInfo->tx_last == NULL)
   {
      pInfo->tx_first = pInfo->tx_last = pHeader;
   }
   else
   {
      pInfo->tx_last->next = pHeader;
      pInfo->tx_last = pHeader;
   }
   
   restore_flags(flags);

   TRACE_Q("add_tx_queue %x, length %d, tx_first = %x", 
          (int)pHeader, pHeader->length, (int)pInfo->tx_first );
}

static void remove_from_tx_queue(struct r3964_info *pInfo, int error_code)
{
   struct r3964_block_header *pHeader;
   unsigned long flags;
#ifdef DEBUG_QUEUE
   struct r3964_block_header *pDump;
#endif
   
   pHeader = pInfo->tx_first;

   if(pHeader==NULL)
      return;

#ifdef DEBUG_QUEUE
   printk("r3964: remove_from_tx_queue: %x, length %d - ",
          (int)pHeader, (int)pHeader->length );
   for(pDump=pHeader;pDump;pDump=pDump->next)
	 printk("%x ", (int)pDump);
   printk("\n");
#endif


   if(pHeader->owner)
   {
      if(error_code)
      {
          add_msg(pHeader->owner, R3964_MSG_ACK, 0, 
                  error_code, NULL);
      }
      else
      {
          add_msg(pHeader->owner, R3964_MSG_ACK, pHeader->length, 
                  error_code, NULL);
      }
      wake_up_interruptible (&pInfo->read_wait);
   }

   save_flags(flags);
   cli();

   pInfo->tx_first = pHeader->next;
   if(pInfo->tx_first==NULL)
   {
      pInfo->tx_last = NULL;
   }

   restore_flags(flags);

   kfree(pHeader);
   TRACE_M("remove_from_tx_queue - kfree %x",(int)pHeader);

   TRACE_Q("remove_from_tx_queue: tx_first = %x, tx_last = %x",
          (int)pInfo->tx_first, (int)pInfo->tx_last );
}

static void add_rx_queue(struct r3964_info *pInfo, struct r3964_block_header *pHeader)
{
   unsigned long flags;
   
   save_flags(flags);
   cli();

   pHeader->next = NULL;

   if(pInfo->rx_last == NULL)
   {
      pInfo->rx_first = pInfo->rx_last = pHeader;
   }
   else
   {
      pInfo->rx_last->next = pHeader;
      pInfo->rx_last = pHeader;
   }
   pInfo->blocks_in_rx_queue++;
   
   restore_flags(flags);

   TRACE_Q("add_rx_queue: %x, length = %d, rx_first = %x, count = %d",
          (int)pHeader, pHeader->length,
          (int)pInfo->rx_first, pInfo->blocks_in_rx_queue);
}

static void remove_from_rx_queue(struct r3964_info *pInfo,
                 struct r3964_block_header *pHeader)
{
   unsigned long flags;
   struct r3964_block_header *pFind;
   
   if(pHeader==NULL)
      return;

   TRACE_Q("remove_from_rx_queue: rx_first = %x, rx_last = %x, count = %d",
          (int)pInfo->rx_first, (int)pInfo->rx_last, pInfo->blocks_in_rx_queue );
   TRACE_Q("remove_from_rx_queue: %x, length %d",
          (int)pHeader, (int)pHeader->length );

   save_flags(flags);
   cli();

   if(pInfo->rx_first == pHeader)
   {
      /* Remove the first block in the linked list: */
      pInfo->rx_first = pHeader->next;
      
      if(pInfo->rx_first==NULL)
      {
         pInfo->rx_last = NULL;
      }
      pInfo->blocks_in_rx_queue--;
   }
   else 
   {
      /* Find block to remove: */
      for(pFind=pInfo->rx_first; pFind; pFind=pFind->next)
      {
         if(pFind->next == pHeader) 
         {
            /* Got it. */
            pFind->next = pHeader->next;
            pInfo->blocks_in_rx_queue--;
            if(pFind->next==NULL)
            {
               /* Oh, removed the last one! */
               pInfo->rx_last = pFind;
            }
            break;
         }
      }
   }

   restore_flags(flags);

   kfree(pHeader);
   TRACE_M("remove_from_rx_queue - kfree %x",(int)pHeader);

   TRACE_Q("remove_from_rx_queue: rx_first = %x, rx_last = %x, count = %d",
          (int)pInfo->rx_first, (int)pInfo->rx_last, pInfo->blocks_in_rx_queue );
}

static void put_char(struct r3964_info *pInfo, unsigned char ch)
{
   struct tty_struct *tty = pInfo->tty;

   if(tty==NULL)
      return;

   if(tty->driver.put_char)
   {
      tty->driver.put_char(tty, ch);
   }
   pInfo->bcc ^= ch;
}

static void flush(struct r3964_info *pInfo)
{
   struct tty_struct *tty = pInfo->tty;

   if(tty==NULL)
      return;

   if(tty->driver.flush_chars)
   {
      tty->driver.flush_chars(tty);
   }
}

static void trigger_transmit(struct r3964_info *pInfo)
{
   unsigned long flags;
   

   save_flags(flags);
   cli();

   if((pInfo->state == R3964_IDLE) && (pInfo->tx_first!=NULL))
   {
      pInfo->state = R3964_TX_REQUEST;
      pInfo->count_down = R3964_TO_QVZ;
      pInfo->nRetry=0;
      pInfo->flags &= ~R3964_ERROR;
      
      restore_flags(flags);

      TRACE_PS("trigger_transmit - sent STX");

      put_char(pInfo, STX);
      flush(pInfo);

      pInfo->bcc = 0;
   }
   else
   {
      restore_flags(flags);
   }
}

static void retry_transmit(struct r3964_info *pInfo)
{
   if(pInfo->nRetry<R3964_MAX_RETRIES)
   {
      TRACE_PE("transmission failed. Retry #%d", 
             pInfo->nRetry);
      pInfo->bcc = 0;
      put_char(pInfo, STX);
      flush(pInfo);
      pInfo->state = R3964_TX_REQUEST;
      pInfo->count_down = R3964_TO_QVZ;
      pInfo->nRetry++;
   }
   else
   {
      TRACE_PE("transmission failed after %d retries", 
             R3964_MAX_RETRIES);

      remove_from_tx_queue(pInfo, R3964_TX_FAIL);
      
      put_char(pInfo, NAK);
      flush(pInfo);
      pInfo->state = R3964_IDLE;

      trigger_transmit(pInfo);
   }
}


static void transmit_block(struct r3964_info *pInfo)
{
   struct tty_struct *tty = pInfo->tty;
   struct r3964_block_header *pBlock = pInfo->tx_first;
   int room=0;

   if((tty==NULL) || (pBlock==NULL))
   {
      return;
   }

   if(tty->driver.write_room)
      room=tty->driver.write_room(tty);

   TRACE_PS("transmit_block %x, room %d, length %d", 
          (int)pBlock, room, pBlock->length);
   
   while(pInfo->tx_position < pBlock->length)
   {
      if(room<2)
         break;
 
      if(pBlock->data[pInfo->tx_position]==DLE)
      {
         /* send additional DLE char: */
         put_char(pInfo, DLE);
      }
      put_char(pInfo, pBlock->data[pInfo->tx_position++]);
      
      room--;
   }

   if((pInfo->tx_position == pBlock->length) && (room>=3))
   {
      put_char(pInfo, DLE);
      put_char(pInfo, ETX);
      if(pInfo->flags & R3964_BCC)
      {
         put_char(pInfo, pInfo->bcc);
      }
      pInfo->state = R3964_WAIT_FOR_TX_ACK;
      pInfo->count_down = R3964_TO_QVZ;
   }
   flush(pInfo);
}

static void on_receive_block(struct r3964_info *pInfo)
{
   unsigned int length;
   struct r3964_client_info *pClient;
   struct r3964_block_header *pBlock;
   
   length=pInfo->rx_position;

   /* compare byte checksum characters: */
   if(pInfo->flags & R3964_BCC)
   {
      if(pInfo->bcc!=pInfo->last_rx)
      {
         TRACE_PE("checksum error - got %x but expected %x",
                pInfo->last_rx, pInfo->bcc);
         pInfo->flags |= R3964_CHECKSUM;
      }
   }

   /* check for errors (parity, overrun,...): */
   if(pInfo->flags & R3964_ERROR)
   {
      TRACE_PE("on_receive_block - transmission failed error %x",
             pInfo->flags & R3964_ERROR);
      
      put_char(pInfo, NAK);
      flush(pInfo);
      if(pInfo->nRetry<R3964_MAX_RETRIES)
      {
         pInfo->state=R3964_WAIT_FOR_RX_REPEAT;
         pInfo->count_down = R3964_TO_RX_PANIC;
         pInfo->nRetry++;
      }
      else
      {
         TRACE_PE("on_receive_block - failed after max retries");
         pInfo->state=R3964_IDLE;
      }
      return;
   }

   
   /* received block; submit DLE: */
   put_char(pInfo, DLE);
   flush(pInfo);
   pInfo->count_down=0;
   TRACE_PS(" rx success: got %d chars", length);

   /* prepare struct r3964_block_header: */
   pBlock = kmalloc(length+sizeof(struct r3964_block_header), GFP_KERNEL);
   TRACE_M("on_receive_block - kmalloc %x",(int)pBlock);

   if(pBlock==NULL)
      return;

   pBlock->length = length;
   pBlock->data   = ((unsigned char*)pBlock)+sizeof(struct r3964_block_header);
   pBlock->locks  = 0;
   pBlock->next   = NULL;
   pBlock->owner  = NULL;

   memcpy(pBlock->data, pInfo->rx_buf, length);

   /* queue block into rx_queue: */
   add_rx_queue(pInfo, pBlock);

   /* notify attached client processes: */
   for(pClient=pInfo->firstClient; pClient; pClient=pClient->next)
   {
      if(pClient->sig_flags & R3964_SIG_DATA)
      {
         add_msg(pClient, R3964_MSG_DATA, length, R3964_OK, pBlock);
      }
   }
   wake_up_interruptible (&pInfo->read_wait);
   
   pInfo->state = R3964_IDLE;

   trigger_transmit(pInfo);
}


static void receive_char(struct r3964_info *pInfo, const unsigned char c)
{
   switch(pInfo->state)
   {
      case R3964_TX_REQUEST:
         if(c==DLE)
         {
            TRACE_PS("TX_REQUEST - got DLE");

            pInfo->state = R3964_TRANSMITTING;
            pInfo->tx_position = 0;
            
            transmit_block(pInfo);
         }
         else if(c==STX)
         {
            if(pInfo->nRetry==0)
            {
               TRACE_PE("TX_REQUEST - init conflict");
               if(pInfo->priority == R3964_SLAVE)
               {
                  goto start_receiving;
               }
            } 
            else 
            {
               TRACE_PE("TX_REQUEST - secondary init conflict!?"
                        " Switching to SLAVE mode for next rx.");
               goto start_receiving;
            }
         }
         else
         {
            TRACE_PE("TX_REQUEST - char != DLE: %x", c);
            retry_transmit(pInfo);
         }
         break;
      case R3964_TRANSMITTING:
         if(c==NAK)
         {
            TRACE_PE("TRANSMITTING - got NAK");
            retry_transmit(pInfo);
         }
         else
         {
            TRACE_PE("TRANSMITTING - got illegal char");
 
            pInfo->state = R3964_WAIT_ZVZ_BEFORE_TX_RETRY;
            pInfo->count_down = R3964_TO_ZVZ;
         }
         break;
      case R3964_WAIT_FOR_TX_ACK:
         if(c==DLE)
         {
            TRACE_PS("WAIT_FOR_TX_ACK - got DLE");
            remove_from_tx_queue(pInfo, R3964_OK);
            
            pInfo->state = R3964_IDLE;
            trigger_transmit(pInfo);
         }
         else
         {
            retry_transmit(pInfo);
         }
         break;
      case R3964_WAIT_FOR_RX_REPEAT:
         /* FALLTROUGH */
      case R3964_IDLE:
         if(c==STX)
         {
            /* Prevent rx_queue from overflow: */
            if(pInfo->blocks_in_rx_queue >= R3964_MAX_BLOCKS_IN_RX_QUEUE)
            {
               TRACE_PE("IDLE - got STX but no space in rx_queue!");
               pInfo->state=R3964_WAIT_FOR_RX_BUF;
               pInfo->count_down = R3964_TO_NO_BUF;
               break;
            }
start_receiving:
            /* Ok, start receiving: */
            TRACE_PS("IDLE - got STX");
            pInfo->rx_position = 0;
            pInfo->last_rx = 0;
            pInfo->flags &= ~R3964_ERROR;
            pInfo->state=R3964_RECEIVING;
            pInfo->count_down = R3964_TO_ZVZ;
            pInfo->nRetry = 0;
            put_char(pInfo, DLE);
            flush(pInfo);
            pInfo->bcc = 0;
         }
         break;
      case R3964_RECEIVING:
         if(pInfo->rx_position < RX_BUF_SIZE)
         {
            pInfo->bcc ^= c;
            
            if(c==DLE)
            {
               if(pInfo->last_rx==DLE)
               {
                  pInfo->last_rx = 0;
                  goto char_to_buf;
               }
               pInfo->last_rx = DLE;
               break;
            } 
            else if((c==ETX) && (pInfo->last_rx==DLE))
            {
               if(pInfo->flags & R3964_BCC)
               {
                  pInfo->state = R3964_WAIT_FOR_BCC;
                  pInfo->count_down = R3964_TO_ZVZ;
               }
               else 
               {
                  on_receive_block(pInfo);
               }
            }
            else
            {
               pInfo->last_rx = c;
char_to_buf:
               pInfo->rx_buf[pInfo->rx_position++] = c;
               pInfo->count_down = R3964_TO_ZVZ;
            }
         }
        /* else: overflow-msg? BUF_SIZE>MTU; should not happen? */ 
         break;
      case R3964_WAIT_FOR_BCC:
         pInfo->last_rx = c;
         on_receive_block(pInfo);
         break;
   }
}

static void receive_error(struct r3964_info *pInfo, const char flag)
{
    switch (flag) 
    {
    case TTY_NORMAL:
        break;
    case TTY_BREAK:
        TRACE_PE("received break")
        pInfo->flags |= R3964_BREAK;
        break;
    case TTY_PARITY:
        TRACE_PE("parity error")
        pInfo->flags |= R3964_PARITY;
        break;
    case TTY_FRAME:
        TRACE_PE("frame error")
        pInfo->flags |= R3964_FRAME;
        break;
    case TTY_OVERRUN:
        TRACE_PE("frame overrun")
        pInfo->flags |= R3964_OVERRUN;
        break;
    default:
        TRACE_PE("receive_error - unknown flag %d", flag);
        pInfo->flags |= R3964_UNKNOWN;
        break;
    }
}

static void on_timeout(struct r3964_info *pInfo)
{
   switch(pInfo->state)
   {
      case R3964_TX_REQUEST:
         TRACE_PE("TX_REQUEST - timeout");
         retry_transmit(pInfo);
         break;
      case R3964_WAIT_ZVZ_BEFORE_TX_RETRY:
         put_char(pInfo, NAK);
         flush(pInfo);
         retry_transmit(pInfo);
         break;
      case R3964_WAIT_FOR_TX_ACK:
         TRACE_PE("WAIT_FOR_TX_ACK - timeout");
         retry_transmit(pInfo);
         break;
      case R3964_WAIT_FOR_RX_BUF:
         TRACE_PE("WAIT_FOR_RX_BUF - timeout");
         put_char(pInfo, NAK);
         flush(pInfo);
         pInfo->state=R3964_IDLE;
         break;
      case R3964_RECEIVING:
         TRACE_PE("RECEIVING - timeout after %d chars", 
                  pInfo->rx_position);
         put_char(pInfo, NAK);
         flush(pInfo);
         pInfo->state=R3964_IDLE;
         break;
      case R3964_WAIT_FOR_RX_REPEAT:
         TRACE_PE("WAIT_FOR_RX_REPEAT - timeout");
         pInfo->state=R3964_IDLE;
         break;
      case R3964_WAIT_FOR_BCC:
         TRACE_PE("WAIT_FOR_BCC - timeout");
         put_char(pInfo, NAK);
         flush(pInfo);
         pInfo->state=R3964_IDLE;
         break;
   }
}

static struct r3964_client_info *findClient(
  struct r3964_info *pInfo, pid_t pid)
{
   struct r3964_client_info *pClient;
   
   for(pClient=pInfo->firstClient; pClient; pClient=pClient->next)
   {
      if(pClient->pid == pid)
      {
         return pClient;
      }
   }
   return NULL;
}

static int enable_signals(struct r3964_info *pInfo, pid_t pid, int arg)
{
   struct r3964_client_info *pClient;
   struct r3964_client_info **ppClient;
   struct r3964_message *pMsg;
   
   if((arg & R3964_SIG_ALL)==0)
   {
      /* Remove client from client list */
      for(ppClient=&pInfo->firstClient; *ppClient; ppClient=&(*ppClient)->next)
      {
         pClient = *ppClient;
         
         if(pClient->pid == pid)
         {
            TRACE_PS("removing client %d from client list", pid);
            *ppClient = pClient->next;
            while(pClient->msg_count)
            {
               pMsg=remove_msg(pInfo, pClient);
               if(pMsg)
               {
                  kfree(pMsg);
                  TRACE_M("enable_signals - msg kfree %x",(int)pMsg);
               }
            }
            kfree(pClient);
            TRACE_M("enable_signals - kfree %x",(int)pClient);
            return 0;
         }
      }
      return -EINVAL;
   }
   else
   {
      pClient=findClient(pInfo, pid);
      if(pClient)
      {
         /* update signal options */
         pClient->sig_flags=arg;
      } 
      else 
      {
         /* add client to client list */
         pClient=kmalloc(sizeof(struct r3964_client_info), GFP_KERNEL);
         TRACE_M("enable_signals - kmalloc %x",(int)pClient);
         if(pClient==NULL)
            return -ENOMEM;

         TRACE_PS("add client %d to client list", pid);
         pClient->sig_flags=arg;
         pClient->pid = pid;
         pClient->next=pInfo->firstClient;
         pClient->first_msg = NULL;
         pClient->last_msg = NULL;
         pClient->next_block_to_read = NULL;
         pClient->msg_count = 0;
         pInfo->firstClient=pClient;
      }
   }

   return 0;
}

static int read_telegram(struct r3964_info *pInfo, pid_t pid, unsigned char *buf)
{
    struct r3964_client_info *pClient;
    struct r3964_block_header *block;

    if(!buf)
    {
        return -EINVAL;
    }

    pClient=findClient(pInfo,pid);
    if(pClient==NULL)
    {
       return -EINVAL;
    }
    
    block=pClient->next_block_to_read;
    if(!block)
    {
       return 0;
    }
    else
    {
      if (copy_to_user (buf, block->data, block->length))
	return -EFAULT;

       remove_client_block(pInfo, pClient);
       return block->length;
    }

    return -EINVAL;
}

static void add_msg(struct r3964_client_info *pClient, int msg_id, int arg,
             int error_code, struct r3964_block_header *pBlock)
{
   struct r3964_message *pMsg;
   unsigned long flags;
   
   if(pClient->msg_count<R3964_MAX_MSG_COUNT-1)
   {
queue_the_message:

      save_flags(flags);
      cli();

      pMsg = kmalloc(sizeof(struct r3964_message), GFP_KERNEL);
      TRACE_M("add_msg - kmalloc %x",(int)pMsg);
      if(pMsg==NULL)
         return;

      pMsg->msg_id = msg_id;
      pMsg->arg    = arg;
      pMsg->error_code = error_code;
      pMsg->block  = pBlock;
      pMsg->next   = NULL;
      
      if(pClient->last_msg==NULL)
      {
         pClient->first_msg=pClient->last_msg=pMsg;
      }
      else
      {
         pClient->last_msg->next = pMsg;
         pClient->last_msg=pMsg;
      }

      pClient->msg_count++;

      if(pBlock!=NULL)
      {
         pBlock->locks++;
      }
      restore_flags(flags);
   }
   else
   {
      if((pClient->last_msg->msg_id == R3964_MSG_ACK)
		 && (pClient->last_msg->error_code==R3964_OVERFLOW))
      {
         pClient->last_msg->arg++;
		 TRACE_PE("add_msg - inc prev OVERFLOW-msg");
      }
      else
      {
         msg_id = R3964_MSG_ACK;
         arg = 0;
		 error_code = R3964_OVERFLOW;
         pBlock = NULL;
		 TRACE_PE("add_msg - queue OVERFLOW-msg");
         goto queue_the_message;
      }
   }
   /* Send SIGIO signal to client process: */
   if(pClient->sig_flags & R3964_USE_SIGIO)
   {
      kill_proc(pClient->pid, SIGIO, 1);
   }
}

static struct r3964_message *remove_msg(struct r3964_info *pInfo,
                       struct r3964_client_info *pClient)
{
   struct r3964_message *pMsg=NULL;
   unsigned long flags;

   if(pClient->first_msg)
   {
      save_flags(flags);
      cli();

      pMsg = pClient->first_msg;
      pClient->first_msg = pMsg->next;
      if(pClient->first_msg==NULL)
      {
         pClient->last_msg = NULL;
      }
      
      pClient->msg_count--;
      if(pMsg->block)
      {
        remove_client_block(pInfo, pClient);
        pClient->next_block_to_read = pMsg->block;
      }
      restore_flags(flags);
   }
   return pMsg;
}

static void remove_client_block(struct r3964_info *pInfo, 
                struct r3964_client_info *pClient)
{
    struct r3964_block_header *block;

    TRACE_PS("remove_client_block PID %d", pClient->pid);

    block=pClient->next_block_to_read;
    if(block)
    {
        block->locks--;
        if(block->locks==0)
        {
            remove_from_rx_queue(pInfo, block);
        }
    }
    pClient->next_block_to_read = NULL;
}


/*************************************************************
 * Line discipline routines
 *************************************************************/

static int r3964_open(struct tty_struct *tty)
{
   struct r3964_info *pInfo;
   
   MOD_INC_USE_COUNT;

   TRACE_L("open");
   TRACE_L("tty=%x, PID=%d, disc_data=%x", 
          (int)tty, current->pid, (int)tty->disc_data);
   
   pInfo=kmalloc(sizeof(struct r3964_info), GFP_KERNEL); 
   TRACE_M("r3964_open - info kmalloc %x",(int)pInfo);

   if(!pInfo)
   {
      printk(KERN_ERR "r3964: failed to alloc info structure\n");
      return -ENOMEM;
   }

   pInfo->rx_buf = kmalloc(RX_BUF_SIZE, GFP_KERNEL);
   TRACE_M("r3964_open - rx_buf kmalloc %x",(int)pInfo->rx_buf);

   if(!pInfo->rx_buf)
   {
      printk(KERN_ERR "r3964: failed to alloc receive buffer\n");
      kfree(pInfo);
      TRACE_M("r3964_open - info kfree %x",(int)pInfo);
      return -ENOMEM;
   }
   
   pInfo->tx_buf = kmalloc(TX_BUF_SIZE, GFP_KERNEL);
   TRACE_M("r3964_open - tx_buf kmalloc %x",(int)pInfo->tx_buf);

   if(!pInfo->tx_buf)
   {
      printk(KERN_ERR "r3964: failed to alloc transmit buffer\n");
      kfree(pInfo->rx_buf);
      TRACE_M("r3964_open - rx_buf kfree %x",(int)pInfo->rx_buf);
      kfree(pInfo);
      TRACE_M("r3964_open - info kfree %x",(int)pInfo);
      return -ENOMEM;
   }

   pInfo->tty = tty;
   init_waitqueue_head (&pInfo->read_wait);
   pInfo->priority = R3964_MASTER;
   pInfo->rx_first = pInfo->rx_last = NULL;
   pInfo->tx_first = pInfo->tx_last = NULL;
   pInfo->rx_position = 0;
   pInfo->tx_position = 0;
   pInfo->last_rx = 0;
   pInfo->blocks_in_rx_queue = 0;
   pInfo->firstClient=NULL;
   pInfo->state=R3964_IDLE;
   pInfo->flags = R3964_DEBUG;
   pInfo->count_down = 0;
   pInfo->nRetry = 0;
   
   tty->disc_data = pInfo;

   /*
    * Add 'on_timer' to timer task queue
    * (will be called from timer bh)
    */
   INIT_LIST_HEAD(&pInfo->bh_1.list);
   pInfo->bh_1.sync = 0;
   pInfo->bh_1.routine = &on_timer_1;
   pInfo->bh_1.data = pInfo;
   
   INIT_LIST_HEAD(&pInfo->bh_2.list);
   pInfo->bh_2.sync = 0;
   pInfo->bh_2.routine = &on_timer_2;
   pInfo->bh_2.data = pInfo;

   queue_task(&pInfo->bh_1, &tq_timer);

   return 0;
}

static void r3964_close(struct tty_struct *tty)
{
   struct r3964_info *pInfo=(struct r3964_info*)tty->disc_data;
   struct r3964_client_info *pClient, *pNext;
   struct r3964_message *pMsg;
   struct r3964_block_header *pHeader, *pNextHeader;
   unsigned long flags;

   TRACE_L("close");

    /*
     * Make sure that our task queue isn't activated.  If it
     * is, take it out of the linked list.
     */
    spin_lock_irqsave(&tqueue_lock, flags);
    if (pInfo->bh_1.sync)
    	list_del(&pInfo->bh_1.list);
    if (pInfo->bh_2.sync)
    	list_del(&pInfo->bh_2.list);
    spin_unlock_irqrestore(&tqueue_lock, flags);

   /* Remove client-structs and message queues: */
    pClient=pInfo->firstClient;
    while(pClient)
    {
       pNext=pClient->next;
       while(pClient->msg_count)
       {
          pMsg=remove_msg(pInfo, pClient);
          if(pMsg)
          {
             kfree(pMsg);
             TRACE_M("r3964_close - msg kfree %x",(int)pMsg);
          }
       }
       kfree(pClient);
       TRACE_M("r3964_close - client kfree %x",(int)pClient);
       pClient=pNext;
    }
    /* Remove jobs from tx_queue: */
	save_flags(flags);
        cli();
	pHeader=pInfo->tx_first;
	pInfo->tx_first=pInfo->tx_last=NULL;
	restore_flags(flags);
	
    while(pHeader)
	{
	   pNextHeader=pHeader->next;
	   kfree(pHeader);
	   pHeader=pNextHeader;
	}

    /* Free buffers: */
    wake_up_interruptible(&pInfo->read_wait);
    kfree(pInfo->rx_buf);
    TRACE_M("r3964_close - rx_buf kfree %x",(int)pInfo->rx_buf);
    kfree(pInfo->tx_buf);
    TRACE_M("r3964_close - tx_buf kfree %x",(int)pInfo->tx_buf);
    kfree(pInfo);
    TRACE_M("r3964_close - info kfree %x",(int)pInfo);

    MOD_DEC_USE_COUNT;
}

static int r3964_read(struct tty_struct *tty, struct file *file,
                     unsigned char *buf, unsigned int nr)
{
   struct r3964_info *pInfo=(struct r3964_info*)tty->disc_data;
   struct r3964_client_info *pClient;
   struct r3964_message *pMsg;
   struct r3964_client_message theMsg;
   DECLARE_WAITQUEUE (wait, current);
   
   int pid = current->pid;
   int count;
   
   TRACE_L("read()");
 
   pClient=findClient(pInfo, pid);
   if(pClient)
   {
      pMsg = remove_msg(pInfo, pClient);
      if(pMsg==NULL)
      {
		 /* no messages available. */
         if (file->f_flags & O_NONBLOCK)
		 {
            return -EAGAIN;
		 }
         /* block until there is a message: */
         add_wait_queue(&pInfo->read_wait, &wait);
repeat:
         current->state = TASK_INTERRUPTIBLE;
         pMsg = remove_msg(pInfo, pClient);
	 if (!pMsg && !signal_pending(current))
		 {
            schedule();
            goto repeat;
         }
         current->state = TASK_RUNNING;
         remove_wait_queue(&pInfo->read_wait, &wait);
      }
      
      /* If we still haven't got a message, we must have been signalled */

      if (!pMsg) return -EINTR;

      /* deliver msg to client process: */
      theMsg.msg_id = pMsg->msg_id;
      theMsg.arg    = pMsg->arg;
      theMsg.error_code = pMsg->error_code;
      count = sizeof(struct r3964_client_message);

      kfree(pMsg);
      TRACE_M("r3964_read - msg kfree %x",(int)pMsg);

      if (copy_to_user(buf,&theMsg, count))
	return -EFAULT;

      TRACE_PS("read - return %d", count);
      return count;
   }
   return -EPERM;
}

static int r3964_write(struct tty_struct * tty, struct file * file,
                      const unsigned char *data, unsigned int count)
{
   struct r3964_info *pInfo=(struct r3964_info*)tty->disc_data;
   struct r3964_block_header *pHeader;
   struct r3964_client_info *pClient;
   unsigned char *new_data;
   int status;
   int pid;
   
   TRACE_L("write request, %d characters", count);
/* 
 * Verify the pointers 
 */

   if(!pInfo)
      return -EIO;

   status = verify_area (VERIFY_READ, data, count);
   if (status != 0) 
   {
      return status;
   }

/*
 * Ensure that the caller does not wish to send too much.
 */
   if (count > R3964_MTU) 
   {
      if (pInfo->flags & R3964_DEBUG)
      {
         TRACE_L (KERN_WARNING
                 "r3964_write: truncating user packet "
                 "from %u to mtu %d", count, R3964_MTU);
      }
      count = R3964_MTU;
   }
/*
 * Allocate a buffer for the data and fetch it from the user space.
 */
   new_data = kmalloc (count+sizeof(struct r3964_block_header), GFP_KERNEL);
   TRACE_M("r3964_write - kmalloc %x",(int)new_data);
   if (new_data == NULL) {
      if (pInfo->flags & R3964_DEBUG)
      {
         printk (KERN_ERR
               "r3964_write: no memory\n");
      }
      return -ENOSPC;
   }
   
   pHeader = (struct r3964_block_header *)new_data;
   pHeader->data = new_data + sizeof(struct r3964_block_header);
   pHeader->length = count;
   pHeader->locks = 0;
   pHeader->owner = NULL;
   
   pid=current->pid;
   
   pClient=findClient(pInfo, pid);
   if(pClient)
   {
      pHeader->owner = pClient;
   }

   copy_from_user (pHeader->data, data, count); /* We already verified this */

   if(pInfo->flags & R3964_DEBUG)
   {
      dump_block(pHeader->data, count);
   }

/*
 * Add buffer to transmit-queue:
 */
   add_tx_queue(pInfo, pHeader);
   trigger_transmit(pInfo);
   
   return 0;
}

static int r3964_ioctl(struct tty_struct * tty, struct file * file,
               unsigned int cmd, unsigned long arg)
{
   struct r3964_info *pInfo=(struct r3964_info*)tty->disc_data;
   if(pInfo==NULL)
      return -EINVAL;
   switch(cmd)
   {
      case R3964_ENABLE_SIGNALS:
         return enable_signals(pInfo, current->pid, arg);
      case R3964_SETPRIORITY:
         if(arg<R3964_MASTER || arg>R3964_SLAVE)
            return -EINVAL;
         pInfo->priority = arg & 0xff;
         return 0;
      case R3964_USE_BCC:
             if(arg)
            pInfo->flags |= R3964_BCC;
         else
            pInfo->flags &= ~R3964_BCC;
         return 0;
      case R3964_READ_TELEGRAM:
         return read_telegram(pInfo, current->pid, (unsigned char *)arg);
      default:
         return -ENOIOCTLCMD;
   }
}

static void r3964_set_termios(struct tty_struct *tty, struct termios * old)
{
   TRACE_L("set_termios");
}

/* Called without the kernel lock held - fine */
static unsigned int r3964_poll(struct tty_struct * tty, struct file * file,
		      struct poll_table_struct *wait)
{
   struct r3964_info *pInfo=(struct r3964_info*)tty->disc_data;
   int pid=current->pid;
   struct r3964_client_info *pClient;
   struct r3964_message *pMsg=NULL;
   unsigned int flags;
   int result = POLLOUT;

   TRACE_L("POLL");

   pClient=findClient(pInfo,pid);
   if(pClient)
     {
       poll_wait(file, &pInfo->read_wait, wait);
       save_flags(flags);
       cli();
       pMsg=pClient->first_msg;
       restore_flags(flags);
       if(pMsg)
	   result |= POLLIN | POLLRDNORM;
     }
   else
     {
       result = -EINVAL;
     }
   return result;
}

static void r3964_receive_buf(struct tty_struct *tty, const unsigned char *cp,
                              char *fp, int count)
{
   struct r3964_info *pInfo=(struct r3964_info*)tty->disc_data;
    const unsigned char *p;
    char *f, flags = 0;
    int i;

    for (i=count, p = cp, f = fp; i; i--, p++) {
        if (f)
            flags = *f++;
        if(flags==TTY_NORMAL)
        {
            receive_char(pInfo, *p);
        }
        else
        {
            receive_error(pInfo, flags);
        }
        
    }
}

static int r3964_receive_room(struct tty_struct *tty)
{
   TRACE_L("receive_room");
   return -1;
}

