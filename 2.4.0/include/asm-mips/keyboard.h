/* $Id: keyboard.h,v 1.14 1999/08/19 22:56:33 ralf Exp $
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1994 - 1999 Ralf Baechle
 */
#ifndef _ASM_KEYBOARD_H
#define _ASM_KEYBOARD_H

#ifdef __KERNEL__

#include <linux/delay.h>
#include <linux/ioport.h>
#include <asm/bootinfo.h>

#define DISABLE_KBD_DURING_INTERRUPTS 0

extern int pckbd_setkeycode(unsigned int scancode, unsigned int keycode);
extern int pckbd_getkeycode(unsigned int scancode);
extern int pckbd_translate(unsigned char scancode, unsigned char *keycode,
			   char raw_mode);
extern char pckbd_unexpected_up(unsigned char keycode);
extern void pckbd_leds(unsigned char leds);
extern void pckbd_init_hw(void);
extern unsigned char pckbd_sysrq_xlate[128];
extern void kbd_forward_char (int ch);

#define kbd_setkeycode		pckbd_setkeycode
#define kbd_getkeycode		pckbd_getkeycode
#define kbd_translate		pckbd_translate
#define kbd_unexpected_up	pckbd_unexpected_up
#define kbd_leds		pckbd_leds
#define kbd_init_hw		pckbd_init_hw
#define kbd_sysrq_xlate         pckbd_sysrq_xlate

#define SYSRQ_KEY 0x54

/* Some stoneage hardware needs delays after some operations.  */
#define kbd_pause() do { } while(0)

struct kbd_ops {
	/* Keyboard driver resource allocation  */
	void (*kbd_request_region)(void);
	int (*kbd_request_irq)(void (*handler)(int, void *, struct pt_regs *));

	/* PSaux driver resource managment  */
	int (*aux_request_irq)(void (*handler)(int, void *, struct pt_regs *));
	void (*aux_free_irq)(void);

	/* Methods to access the keyboard processor's I/O registers  */
	unsigned char (*kbd_read_input)(void);
	void (*kbd_write_output)(unsigned char val);
	void (*kbd_write_command)(unsigned char val);
	unsigned char (*kbd_read_status)(void);
};

extern struct kbd_ops *kbd_ops;

/* Do the actual calls via kbd_ops vector  */
#define kbd_request_region() kbd_ops->kbd_request_region()
#define kbd_request_irq(handler) kbd_ops->kbd_request_irq(handler)

#define aux_request_irq(hand, dev_id) kbd_ops->aux_request_irq(hand)
#define aux_free_irq(dev_id) kbd_ops->aux_free_irq()

#define kbd_read_input() kbd_ops->kbd_read_input()
#define kbd_write_output(val) kbd_ops->kbd_write_output(val)
#define kbd_write_command(val) kbd_ops->kbd_write_command(val)
#define kbd_read_status() kbd_ops->kbd_read_status()

#endif /* __KERNEL */

#endif /* _ASM_KEYBOARD_H */
