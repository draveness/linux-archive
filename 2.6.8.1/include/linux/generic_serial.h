/*
 *  generic_serial.h
 *
 *  Copyright (C) 1998 R.E.Wolff@BitWizard.nl
 *
 *  written for the SX serial driver.
 *     Contains the code that should be shared over all the serial drivers.
 *
 *  Version 0.1 -- December, 1998.
 */

#ifndef GENERIC_SERIAL_H
#define GENERIC_SERIAL_H

struct real_driver {
  void                    (*disable_tx_interrupts) (void *);
  void                    (*enable_tx_interrupts) (void *);
  void                    (*disable_rx_interrupts) (void *);
  void                    (*enable_rx_interrupts) (void *);
  int                     (*get_CD) (void *);
  void                    (*shutdown_port) (void*);
  int                     (*set_real_termios) (void*);
  int                     (*chars_in_buffer) (void*);
  void                    (*close) (void*);
  void                    (*hungup) (void*);
  void                    (*getserial) (void*, struct serial_struct *sp);
};



struct gs_port {
  int                     magic;
  unsigned char           *xmit_buf; 
  int                     xmit_head;
  int                     xmit_tail;
  int                     xmit_cnt;
  /*  struct semaphore        port_write_sem; */
  int                     flags;
  wait_queue_head_t       open_wait;
  wait_queue_head_t       close_wait;
  int                     count;
  int                     blocked_open;
  struct tty_struct       *tty;
  unsigned long           event;
  unsigned short          closing_wait;
  int                     close_delay;
  struct real_driver      *rd;
  int                     wakeup_chars;
  int                     baud_base;
  int                     baud;
  int                     custom_divisor;
};


/* Flags */
/* Warning: serial.h defines some ASYNC_ flags, they say they are "only"
   used in serial.c, but they are also used in all other serial drivers. 
   Make sure they don't clash with these here... */
#define GS_TX_INTEN      0x00800000
#define GS_RX_INTEN      0x00400000
#define GS_ACTIVE        0x00200000



#define GS_TYPE_NORMAL   1

#define GS_DEBUG_FLUSH   0x00000001
#define GS_DEBUG_BTR     0x00000002
#define GS_DEBUG_TERMIOS 0x00000004
#define GS_DEBUG_STUFF   0x00000008
#define GS_DEBUG_CLOSE   0x00000010
#define GS_DEBUG_FLOW    0x00000020


void gs_put_char(struct tty_struct *tty, unsigned char ch);
int  gs_write(struct tty_struct *tty, int from_user, 
             const unsigned char *buf, int count);
int  gs_write_room(struct tty_struct *tty);
int  gs_chars_in_buffer(struct tty_struct *tty);
void gs_flush_buffer(struct tty_struct *tty);
void gs_flush_chars(struct tty_struct *tty);
void gs_stop(struct tty_struct *tty);
void gs_start(struct tty_struct *tty);
void gs_hangup(struct tty_struct *tty);
void gs_do_softint(void *private_);
int  gs_block_til_ready(void *port, struct file *filp);
void gs_close(struct tty_struct *tty, struct file *filp);
void gs_set_termios (struct tty_struct * tty, 
                     struct termios * old_termios);
int  gs_init_port(struct gs_port *port);
int  gs_setserial(struct gs_port *port, struct serial_struct __user *sp);
int  gs_getserial(struct gs_port *port, struct serial_struct __user *sp);
void gs_got_break(struct gs_port *port);

extern int gs_debug;

#endif
