/*
 * linux/drivers/char/q40_keyb.c
 *
 */

#include <linux/config.h>

#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/keyboard.h>
#include <linux/signal.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/kbd_ll.h>
#include <linux/kbd_kern.h>
#include <linux/delay.h>
#include <linux/sysrq.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/malloc.h>

#include <asm/keyboard.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/q40_master.h>
#include <asm/irq.h>
#include <asm/q40ints.h>

/* Some configuration switches are present in the include file... */

#define KBD_REPORT_ERR

/* Simple translation table for the SysRq keys */

#define SYSRQ_KEY 0x54

#ifdef CONFIG_MAGIC_SYSRQ
unsigned char q40kbd_sysrq_xlate[128] =
	"\000\0331234567890-=\177\t"			/* 0x00 - 0x0f */
	"qwertyuiop[]\r\000as"				/* 0x10 - 0x1f */
	"dfghjkl;'`\000\\zxcv"				/* 0x20 - 0x2f */
	"bnm,./\000*\000 \000\201\202\203\204\205"	/* 0x30 - 0x3f */
	"\206\207\210\211\212\000\000789-456+1"		/* 0x40 - 0x4f */
	"230\177\000\000\213\214\000\000\000\000\000\000\000\000\000\000" /* 0x50 - 0x5f */
	"\r\000/";					/* 0x60 - 0x6f */
#endif

/* Q40 uses AT scancodes - no way to change it. so we have to translate ..*/
/* 0x00 means not a valid entry or no conversion known                    */

unsigned static char q40cl[256] =
{/* 0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   a,   b,   c,   d,   e,   f, */
 0x00,0x43,0x00,0x3f,0x3d,0x3b,0x3c,0x58,0x00,0x44,0x42,0x40,0x3e,0x0f,0x29,0x00,     /* 0x00 - 0x0f */
 0x00,0x38,0x2a,0x00,0x1d,0x10,0x02,0x00,0x00,0x00,0x2c,0x1f,0x1e,0x11,0x03,0x00,     /* 0x10 - 0x1f */
 0x00,0x2e,0x2d,0x20,0x12,0x05,0x04,0x00,0x21,0x39,0x2f,0x21,0x14,0x13,0x06,0x00,     /* 0x20 - 0x2f  'f' is at 0x2b, what is 0x28 ???*/
 0x00,0x31,0x30,0x23,0x22,0x15,0x07,0x00,0x24,0x00,0x32,0x24,0x16,0x08,0x09,0x00,     /* 0x30 - 0x3f */
 0x00,0x33,0x25,0x17,0x18,0x0b,0x0a,0x00,0x00,0x34,0x35,0x26,0x27,0x19,0x0c,0x00,     /* 0x40 - 0x4f */
 0x00,0x00,0x28,0x00,0x1a,0x0d,0x00,0x00,0x3a,0x36,0x1c,0x1b,0x00,0x2b,0x00,0x00,     /* 0x50 - 0x5f*/
 0x00,0x56,0x00,0x00,0x00,0x00,0x0e,0x00,0x00,0x4f,0x00,0x4b,0x47,0x00,0x00,0x00,     /* 0x60 - 0x6f */
 0x52,0x53,0x50,0x4c,0x4d,0x48,0x01,0x45,0x57,0x4e,0x51,0x4a,0x37,0x49,0x46,0x00,     /* 0x70 - 0x7f */
 0x00,0x00,0x00,0x41,0x37,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0x80 - 0x8f  0x84/0x37 is SySrq*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0x90 - 0x9f */
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xa0 - 0xaf */
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xb0 - 0xbf */
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xc0 - 0xcf */
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xd0 - 0xdf */
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xe0 - 0xef */
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xf0 - 0xff */
};

/* another table, AT 0xe0 codes to PC 0xe0 codes, 
   0xff special entry for SysRq - DROPPED right now  */
static unsigned char q40ecl[]=
{/* 0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   a,   b,   c,   d,   e,   f, */
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0x00 - 0x0f*/
 0x00,0x38,0x2a,0x00,0x1d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0x10 - 0x1f */
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0x20 - 0x2f*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0x30 - 0x3f*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x35,0x00,0x00,0x00,0x00,0x00,     /* 0x40 - 0x4f*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x36,0x1c,0x00,0x00,0x00,0x00,0x00,     /* 0x50 - 0x5f*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4f,0x00,0x4b,0x47,0x00,0x00,0x00,     /* 0x60 - 0x6f*/
 0x52,0x53,0x50,0x00,0x4d,0x48,0x00,0x00,0x00,0x00,0x51,0x00,0x00,0x49,0x00,0x00,     /* 0x70 - 0x7f*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0x80 - 0x8f*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0x90 - 0x9f*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xa0 - 0xaf*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xb0 - 0xbf*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xc0 - 0xcf*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xd0 - 0xdf*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,     /* 0xe0 - 0xef*/
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00      /* 0xf0 - 0xff*/
};


static spinlock_t kbd_controller_lock = SPIN_LOCK_UNLOCKED;


/*
 * Translation of escaped scancodes to keycodes.
 * This is now user-settable.
 * The keycodes 1-88,96-111,119 are fairly standard, and
 * should probably not be changed - changing might confuse X.
 * X also interprets scancode 0x5d (KEY_Begin).
 *
 * For 1-88 keycode equals scancode.
 */

#define E0_KPENTER 96
#define E0_RCTRL   97
#define E0_KPSLASH 98
#define E0_PRSCR   99
#define E0_RALT    100
#define E0_BREAK   101  /* (control-pause) */
#define E0_HOME    102
#define E0_UP      103
#define E0_PGUP    104
#define E0_LEFT    105
#define E0_RIGHT   106
#define E0_END     107
#define E0_DOWN    108
#define E0_PGDN    109
#define E0_INS     110
#define E0_DEL     111

#define E1_PAUSE   119

/*
 * The keycodes below are randomly located in 89-95,112-118,120-127.
 * They could be thrown away (and all occurrences below replaced by 0),
 * but that would force many users to use the `setkeycodes' utility, where
 * they needed not before. It does not matter that there are duplicates, as
 * long as no duplication occurs for any single keyboard.
 */
#define SC_LIM 89

#define FOCUS_PF1 85           /* actual code! */
#define FOCUS_PF2 89
#define FOCUS_PF3 90
#define FOCUS_PF4 91
#define FOCUS_PF5 92
#define FOCUS_PF6 93
#define FOCUS_PF7 94
#define FOCUS_PF8 95
#define FOCUS_PF9 120
#define FOCUS_PF10 121
#define FOCUS_PF11 122
#define FOCUS_PF12 123

#define JAP_86     124
/* tfj@olivia.ping.dk:
 * The four keys are located over the numeric keypad, and are
 * labelled A1-A4. It's an rc930 keyboard, from
 * Regnecentralen/RC International, Now ICL.
 * Scancodes: 59, 5a, 5b, 5c.
 */
#define RGN1 124
#define RGN2 125
#define RGN3 126
#define RGN4 127

static unsigned char high_keys[128 - SC_LIM] = {
  RGN1, RGN2, RGN3, RGN4, 0, 0, 0,                   /* 0x59-0x5f */
  0, 0, 0, 0, 0, 0, 0, 0,                            /* 0x60-0x67 */
  0, 0, 0, 0, 0, FOCUS_PF11, 0, FOCUS_PF12,          /* 0x68-0x6f */
  0, 0, 0, FOCUS_PF2, FOCUS_PF9, 0, 0, FOCUS_PF3,    /* 0x70-0x77 */
  FOCUS_PF4, FOCUS_PF5, FOCUS_PF6, FOCUS_PF7,        /* 0x78-0x7b */
  FOCUS_PF8, JAP_86, FOCUS_PF10, 0                   /* 0x7c-0x7f */
};

/* BTC */
#define E0_MACRO   112
/* LK450 */
#define E0_F13     113
#define E0_F14     114
#define E0_HELP    115
#define E0_DO      116
#define E0_F17     117
#define E0_KPMINPLUS 118
/*
 * My OmniKey generates e0 4c for  the "OMNI" key and the
 * right alt key does nada. [kkoller@nyx10.cs.du.edu]
 */
#define E0_OK	124
/*
 * New microsoft keyboard is rumoured to have
 * e0 5b (left window button), e0 5c (right window button),
 * e0 5d (menu button). [or: LBANNER, RBANNER, RMENU]
 * [or: Windows_L, Windows_R, TaskMan]
 */
#define E0_MSLW	125
#define E0_MSRW	126
#define E0_MSTM	127

/* this can be changed using setkeys : */
static unsigned char e0_keys[128] = {
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x00-0x07 */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x08-0x0f */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x10-0x17 */
  0, 0, 0, 0, E0_KPENTER, E0_RCTRL, 0, 0,	      /* 0x18-0x1f */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x20-0x27 */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x28-0x2f */
  0, 0, 0, 0, 0, E0_KPSLASH, 0, E0_PRSCR,	      /* 0x30-0x37 */
  E0_RALT, 0, 0, 0, 0, E0_F13, E0_F14, E0_HELP,	      /* 0x38-0x3f */
  E0_DO, E0_F17, 0, 0, 0, 0, E0_BREAK, E0_HOME,	      /* 0x40-0x47 */
  E0_UP, E0_PGUP, 0, E0_LEFT, E0_OK, E0_RIGHT, E0_KPMINPLUS, E0_END,/* 0x48-0x4f */
  E0_DOWN, E0_PGDN, E0_INS, E0_DEL, 0, 0, 0, 0,	      /* 0x50-0x57 */
  0, 0, 0, E0_MSLW, E0_MSRW, E0_MSTM, 0, 0,	      /* 0x58-0x5f */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x60-0x67 */
  0, 0, 0, 0, 0, 0, 0, E0_MACRO,		      /* 0x68-0x6f */
  0, 0, 0, 0, 0, 0, 0, 0,			      /* 0x70-0x77 */
  0, 0, 0, 0, 0, 0, 0, 0			      /* 0x78-0x7f */
};

static unsigned int prev_scancode = 0;   /* remember E0, E1 */

int q40kbd_setkeycode(unsigned int scancode, unsigned int keycode)
{
	if (scancode < SC_LIM || scancode > 255 || keycode > 127)
	  return -EINVAL;
	if (scancode < 128)
	  high_keys[scancode - SC_LIM] = keycode;
	else
	  e0_keys[scancode - 128] = keycode;
	return 0;
}

int q40kbd_getkeycode(unsigned int scancode)
{
	return
	  (scancode < SC_LIM || scancode > 255) ? -EINVAL :
	  (scancode < 128) ? high_keys[scancode - SC_LIM] :
	    e0_keys[scancode - 128];
}


#define disable_keyboard()	
#define enable_keyboard()	




int q40kbd_translate(unsigned char scancode, unsigned char *keycode,
		    char raw_mode)
{
  	if (scancode == 0xe0 || scancode == 0xe1) {
		prev_scancode = scancode;
		return 0;
 	}

	if (prev_scancode) {
	  /*
	   * usually it will be 0xe0, but a Pause key generates
	   * e1 1d 45 e1 9d c5 when pressed, and nothing when released
	   */
	  if (prev_scancode != 0xe0) {
	      if (prev_scancode == 0xe1 && scancode == 0x1d) {
		  prev_scancode = 0x100;
		  return 0;
	      } else if (prev_scancode == 0x100 && scancode == 0x45) {
		  *keycode = E1_PAUSE;
		  prev_scancode = 0;
	      } else {
#ifdef KBD_REPORT_UNKN
		  if (!raw_mode)
		    printk(KERN_INFO "keyboard: unknown e1 escape sequence\n");
#endif
		  prev_scancode = 0;
		  return 0;
	      }
	  } else {
	      prev_scancode = 0;
	      /*
	       *  The keyboard maintains its own internal caps lock and
	       *  num lock statuses. In caps lock mode E0 AA precedes make
	       *  code and E0 2A follows break code. In num lock mode,
	       *  E0 2A precedes make code and E0 AA follows break code.
	       *  We do our own book-keeping, so we will just ignore these.
	       */
	      /*
	       *  For my keyboard there is no caps lock mode, but there are
	       *  both Shift-L and Shift-R modes. The former mode generates
	       *  E0 2A / E0 AA pairs, the latter E0 B6 / E0 36 pairs.
	       *  So, we should also ignore the latter. - aeb@cwi.nl
	       */
	      if (scancode == 0x2a || scancode == 0x36)
		return 0;

	      if (e0_keys[scancode])
		*keycode = e0_keys[scancode];
	      else {
#ifdef KBD_REPORT_UNKN
		  if (!raw_mode)
		    printk(KERN_INFO "keyboard: unknown scancode e0 %02x\n",
			   scancode);
#endif
		  return 0;
	      }
	  }
	} else if (scancode >= SC_LIM) {
	    /* This happens with the FOCUS 9000 keyboard
	       Its keys PF1..PF12 are reported to generate
	       55 73 77 78 79 7a 7b 7c 74 7e 6d 6f
	       Moreover, unless repeated, they do not generate
	       key-down events, so we have to zero up_flag below */
	    /* Also, Japanese 86/106 keyboards are reported to
	       generate 0x73 and 0x7d for \ - and \ | respectively. */
	    /* Also, some Brazilian keyboard is reported to produce
	       0x73 and 0x7e for \ ? and KP-dot, respectively. */

	  *keycode = high_keys[scancode - SC_LIM];

	  if (!*keycode) {
	      if (!raw_mode) {
#ifdef KBD_REPORT_UNKN
		  printk(KERN_INFO "keyboard: unrecognized scancode (%02x)"
			 " - ignored\n", scancode);
#endif
	      }
	      return 0;
	  }
 	} else
	  *keycode = scancode;
 	return 1;
}

char q40kbd_unexpected_up(unsigned char keycode)
{
	/* unexpected, but this can happen: maybe this was a key release for a
	   FOCUS 9000 PF key; if we want to see it, we have to clear up_flag */
	if (keycode >= SC_LIM || keycode == 85)
	    return 0;
	else
	    return 0200;
}

static int keyup=0;
static int qprev=0;

static void keyboard_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned char status;

	spin_lock(&kbd_controller_lock);
	kbd_pt_regs = regs;

	status = IRQ_KEYB_MASK & master_inb(INTERRUPT_REG);
	if (status ) 
	  {
	    unsigned char scancode,qcode;
	    
	    qcode = master_inb(KEYCODE_REG);
	    
	    if (qcode != 0xf0)
	      {
		if (qcode == 0xe0)
		  {
		    qprev=0xe0;
		    handle_scancode(qprev , 1);
		    goto exit;
		  }
		
		scancode=qprev ? q40ecl[qcode] : q40cl[qcode];
#if 0
/* next line is last resort to hanlde some oddities */
		if (qprev && !scancode) scancode=q40cl[qcode];
#endif
		qprev=0;
		if (!scancode)
		  {
		    printk("unknown scancode %x\n",qcode);
		    goto exit;
		  }
		if (scancode==0xff)  /* SySrq */
		  scancode=SYSRQ_KEY;

		handle_scancode(scancode, ! keyup );
		keyup=0;
		tasklet_schedule(&keyboard_tasklet);
	      }
	    else
	      keyup=1;
	  }
exit:
	spin_unlock(&kbd_controller_lock);
	master_outb(-1,KEYBOARD_UNLOCK_REG); /* keyb ints reenabled herewith */
}


#define KBD_NO_DATA	(-1)	/* No data */
#define KBD_BAD_DATA	(-2)	/* Parity or other error */

static int __init kbd_read_input(void)
{
	int retval = KBD_NO_DATA;
	unsigned char status;

	status = IRQ_KEYB_MASK & master_inb(INTERRUPT_REG);
	if (status) {
		unsigned char data = master_inb(KEYCODE_REG);

		retval = data;
		master_outb(-1,KEYBOARD_UNLOCK_REG);
	}
	return retval;
}

extern void q40kbd_leds(unsigned char leds)
{ /* nothing can be done */ }

static void __init kbd_clear_input(void)
{
	int maxread = 100;	/* Random number */

	do {
		if (kbd_read_input() == KBD_NO_DATA)
			break;
	} while (--maxread);
}


void __init q40kbd_init_hw(void)
{
#if 0
	/* Get the keyboard controller registers (incomplete decode) */
	request_region(0x60, 16, "keyboard");
#endif
	/* Flush any pending input. */
	kbd_clear_input();

	/* Ok, finally allocate the IRQ, and off we go.. */
	request_irq(Q40_IRQ_KEYBOARD, keyboard_interrupt, 0, "keyboard", NULL);
	master_outb(-1,KEYBOARD_UNLOCK_REG);
	master_outb(1,KEY_IRQ_ENABLE_REG);

}

