/* $Id: act2000_isa.c,v 1.11 2000/11/12 16:32:06 kai Exp $
 *
 * ISDN lowlevel-module for the IBM ISDN-S0 Active 2000 (ISA-Version).
 *
 * Copyright 1998 by Fritz Elfert (fritz@isdn4linux.de)
 * Thanks to Friedemann Baitinger and IBM Germany
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */

#define __NO_VERSION__
#include "act2000.h"
#include "act2000_isa.h"
#include "capi.h"

static act2000_card *irq2card_map[16] =
{
	0, 0, 0, 0, 0, 0, 0, 0,	0, 0, 0, 0, 0, 0, 0, 0
};

static int act2000_isa_irqs[] =
{
        3, 5, 7, 10, 11, 12, 15
};
#define ISA_NRIRQS (sizeof(act2000_isa_irqs)/sizeof(int))

static void
act2000_isa_delay(long t)
{
        sti();
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(t);
        sti();
}

/*
 * Reset Controller, then try to read the Card's signature.
 + Return:
 *   1 = Signature found.
 *   0 = Signature not found.
 */
static int
act2000_isa_reset(unsigned short portbase)
{
        unsigned char reg;
        int i;
        int found;
        int serial = 0;

        found = 0;
        if ((reg = inb(portbase + ISA_COR)) != 0xff) {
                outb(reg | ISA_COR_RESET, portbase + ISA_COR);
                mdelay(10);
                outb(reg, portbase + ISA_COR);
                mdelay(10);

                for (i = 0; i < 16; i++) {
                        if (inb(portbase + ISA_ISR) & ISA_ISR_SERIAL)
                                serial |= 0x10000;
                        serial >>= 1;
                }
                if (serial == ISA_SER_ID)
                        found++;
        }
        return found;
}

int
act2000_isa_detect(unsigned short portbase)
{
        int ret = 0;
        unsigned long flags;

        save_flags(flags);
        cli();
        if (!check_region(portbase, ISA_REGION))
                ret = act2000_isa_reset(portbase);
        restore_flags(flags);
        return ret;
}

static void
act2000_isa_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
        act2000_card *card = irq2card_map[irq];
        u_char istatus;

        if (!card) {
                printk(KERN_WARNING
                       "act2000: Spurious interrupt!\n");
                return;
        }
        istatus = (inb(ISA_PORT_ISR) & 0x07);
        if (istatus & ISA_ISR_OUT) {
                /* RX fifo has data */
		istatus &= ISA_ISR_OUT_MASK;
		outb(0, ISA_PORT_SIS);
		act2000_isa_receive(card);
		outb(ISA_SIS_INT, ISA_PORT_SIS);
        }
        if (istatus & ISA_ISR_ERR) {
                /* Error Interrupt */
		istatus &= ISA_ISR_ERR_MASK;
                printk(KERN_WARNING "act2000: errIRQ\n");
        }
	if (istatus)
		printk(KERN_DEBUG "act2000: ?IRQ %d %02x\n", irq, istatus);
}

static void
act2000_isa_select_irq(act2000_card * card)
{
	unsigned char reg;

	reg = (inb(ISA_PORT_COR) & ~ISA_COR_IRQOFF) | ISA_COR_PERR;
	switch (card->irq) {
		case 3:
			reg = ISA_COR_IRQ03;
			break;
		case 5:
			reg = ISA_COR_IRQ05;
			break;
		case 7:
			reg = ISA_COR_IRQ07;
			break;
		case 10:
			reg = ISA_COR_IRQ10;
			break;
		case 11:
			reg = ISA_COR_IRQ11;
			break;
		case 12:
			reg = ISA_COR_IRQ12;
			break;
		case 15:
			reg = ISA_COR_IRQ15;
			break;
	}
	outb(reg, ISA_PORT_COR);
}

static void
act2000_isa_enable_irq(act2000_card * card)
{
	act2000_isa_select_irq(card);
	/* Enable READ irq */
	outb(ISA_SIS_INT, ISA_PORT_SIS);
}

/*
 * Install interrupt handler, enable irq on card.
 * If irq is -1, choose next free irq, else irq is given explicitely.
 */
int
act2000_isa_config_irq(act2000_card * card, short irq)
{
        int i;
        unsigned long flags;

        if (card->flags & ACT2000_FLAGS_IVALID) {
                free_irq(card->irq, NULL);
                irq2card_map[card->irq] = NULL;
        }
        card->flags &= ~ACT2000_FLAGS_IVALID;
        outb(ISA_COR_IRQOFF, ISA_PORT_COR);
        if (!irq)
                return 0;
        save_flags(flags);
        cli();
        if (irq == -1) {
                /* Auto select */
                for (i = 0; i < ISA_NRIRQS; i++) {
                        if (!request_irq(act2000_isa_irqs[i], &act2000_isa_interrupt, 0, card->regname, NULL)) {
                                card->irq = act2000_isa_irqs[i];
                                irq2card_map[card->irq] = card;
                                card->flags |= ACT2000_FLAGS_IVALID;
                                break;
                        }
                }
        } else {
                /* Fixed irq */
                if (!request_irq(irq, &act2000_isa_interrupt, 0, card->regname, NULL)) {
                        card->irq = irq;
                        irq2card_map[card->irq] = card;
			card->flags |= ACT2000_FLAGS_IVALID;
                }
        }
        restore_flags(flags);
        if (!card->flags & ACT2000_FLAGS_IVALID) {
                printk(KERN_WARNING
                       "act2000: Could not request irq\n");
                return -EBUSY;
        } else {
		act2000_isa_select_irq(card);
                /* Disable READ and WRITE irq */
                outb(0, ISA_PORT_SIS);
                outb(0, ISA_PORT_SOS);
        }
        return 0;
}

int
act2000_isa_config_port(act2000_card * card, unsigned short portbase)
{
        if (card->flags & ACT2000_FLAGS_PVALID) {
                release_region(card->port, ISA_REGION);
                card->flags &= ~ACT2000_FLAGS_PVALID;
        }
        if (!check_region(portbase, ISA_REGION)) {
                request_region(portbase, ACT2000_PORTLEN, card->regname);
                card->port = portbase;
                card->flags |= ACT2000_FLAGS_PVALID;
                return 0;
        }
        return -EBUSY;
}

/*
 * Release ressources, used by an adaptor.
 */
void
act2000_isa_release(act2000_card * card)
{
        unsigned long flags;

        save_flags(flags);
        cli();
        if (card->flags & ACT2000_FLAGS_IVALID) {
                free_irq(card->irq, NULL);
                irq2card_map[card->irq] = NULL;
        }
        card->flags &= ~ACT2000_FLAGS_IVALID;
        if (card->flags & ACT2000_FLAGS_PVALID)
                release_region(card->port, ISA_REGION);
        card->flags &= ~ACT2000_FLAGS_PVALID;
        restore_flags(flags);
}

static int
act2000_isa_writeb(act2000_card * card, u_char data)
{
        u_char timeout = 40;

        while (timeout) {
                if (inb(ISA_PORT_SOS) & ISA_SOS_READY) {
                        outb(data, ISA_PORT_SDO);
                        return 0;
                } else {
                        timeout--;
                        udelay(10);
                }
        }
        return 1;
}

static int
act2000_isa_readb(act2000_card * card, u_char * data)
{
        u_char timeout = 40;

        while (timeout) {
                if (inb(ISA_PORT_SIS) & ISA_SIS_READY) {
                        *data = inb(ISA_PORT_SDI);
                        return 0;
                } else {
                        timeout--;
                        udelay(10);
                }
        }
        return 1;
}

void
act2000_isa_receive(act2000_card *card)
{
	u_char c;

        if (test_and_set_bit(ACT2000_LOCK_RX, (void *) &card->ilock) != 0)
		return;
	while (!act2000_isa_readb(card, &c)) {
		if (card->idat.isa.rcvidx < 8) {
                        card->idat.isa.rcvhdr[card->idat.isa.rcvidx++] = c;
			if (card->idat.isa.rcvidx == 8) {
				int valid = actcapi_chkhdr(card, (actcapi_msghdr *)&card->idat.isa.rcvhdr);

				if (valid) {
					card->idat.isa.rcvlen = ((actcapi_msghdr *)&card->idat.isa.rcvhdr)->len;
					card->idat.isa.rcvskb = dev_alloc_skb(card->idat.isa.rcvlen);
					if (card->idat.isa.rcvskb == NULL) {
						card->idat.isa.rcvignore = 1;
						printk(KERN_WARNING
						       "act2000_isa_receive: no memory\n");
						test_and_clear_bit(ACT2000_LOCK_RX, (void *) &card->ilock);
						return;
					}
					memcpy(skb_put(card->idat.isa.rcvskb, 8), card->idat.isa.rcvhdr, 8);
					card->idat.isa.rcvptr = skb_put(card->idat.isa.rcvskb, card->idat.isa.rcvlen - 8);
				} else {
					card->idat.isa.rcvidx = 0;
					printk(KERN_WARNING
					       "act2000_isa_receive: Invalid CAPI msg\n");
					{
						int i; __u8 *p; __u8 *c; __u8 tmp[30];
						for (i = 0, p = (__u8 *)&card->idat.isa.rcvhdr, c = tmp; i < 8; i++)
							c += sprintf(c, "%02x ", *(p++));
						printk(KERN_WARNING "act2000_isa_receive: %s\n", tmp);
					}
				}
			}
		} else {
			if (!card->idat.isa.rcvignore)
				*card->idat.isa.rcvptr++ = c;
			if (++card->idat.isa.rcvidx >= card->idat.isa.rcvlen) {
				if (!card->idat.isa.rcvignore) {
					skb_queue_tail(&card->rcvq, card->idat.isa.rcvskb);
					act2000_schedule_rx(card);
				}
				card->idat.isa.rcvidx = 0;
				card->idat.isa.rcvlen = 8;
				card->idat.isa.rcvignore = 0;
				card->idat.isa.rcvskb = NULL;
				card->idat.isa.rcvptr = card->idat.isa.rcvhdr;
			}
		}
	}
	if (!(card->flags & ACT2000_FLAGS_IVALID)) {
		/* In polling mode, schedule myself */
		if ((card->idat.isa.rcvidx) &&
		    (card->idat.isa.rcvignore ||
		     (card->idat.isa.rcvidx < card->idat.isa.rcvlen)))
			act2000_schedule_poll(card);
	}
	test_and_clear_bit(ACT2000_LOCK_RX, (void *) &card->ilock);
}

void
act2000_isa_send(act2000_card * card)
{
	unsigned long flags;
	struct sk_buff *skb;
	actcapi_msg *msg;
	int l;

        if (test_and_set_bit(ACT2000_LOCK_TX, (void *) &card->ilock) != 0)
		return;
	while (1) {
		save_flags(flags);
		cli();
		if (!(card->sbuf)) {
			if ((card->sbuf = skb_dequeue(&card->sndq))) {
				card->ack_msg = card->sbuf->data;
				msg = (actcapi_msg *)card->sbuf->data;
				if ((msg->hdr.cmd.cmd == 0x86) &&
				    (msg->hdr.cmd.subcmd == 0)   ) {
					/* Save flags in message */
					card->need_b3ack = msg->msg.data_b3_req.flags;
					msg->msg.data_b3_req.flags = 0;
				}
			}
		}
		restore_flags(flags);
		if (!(card->sbuf)) {
			/* No more data to send */
			test_and_clear_bit(ACT2000_LOCK_TX, (void *) &card->ilock);
			return;
		}
		skb = card->sbuf;
		l = 0;
		while (skb->len) {
			if (act2000_isa_writeb(card, *(skb->data))) {
				/* Fifo is full, but more data to send */
				test_and_clear_bit(ACT2000_LOCK_TX, (void *) &card->ilock);
				/* Schedule myself */
				act2000_schedule_tx(card);
				return;
			}
			skb_pull(skb, 1);
			l++;
		}
		msg = (actcapi_msg *)card->ack_msg;
		if ((msg->hdr.cmd.cmd == 0x86) &&
		    (msg->hdr.cmd.subcmd == 0)   ) {
			/*
			 * If it's user data, reset data-ptr
			 * and put skb into ackq.
			 */
			skb->data = card->ack_msg;
			/* Restore flags in message */
			msg->msg.data_b3_req.flags = card->need_b3ack;
			skb_queue_tail(&card->ackq, skb);
		} else
			dev_kfree_skb(skb);
		card->sbuf = NULL;
	}
}

/*
 * Get firmware ID, check for 'ISDN' signature.
 */
static int
act2000_isa_getid(act2000_card * card)
{

        act2000_fwid fid;
        u_char *p = (u_char *) & fid;
        int count = 0;

        while (1) {
                if (count > 510)
                        return -EPROTO;
                if (act2000_isa_readb(card, p++))
                        break;
                count++;
        }
        if (count <= 20) {
                printk(KERN_WARNING "act2000: No Firmware-ID!\n");
                return -ETIME;
        }
        *p = '\0';
        fid.revlen[0] = '\0';
        if (strcmp(fid.isdn, "ISDN")) {
                printk(KERN_WARNING "act2000: Wrong Firmware-ID!\n");
                return -EPROTO;
        }
	if ((p = strchr(fid.revision, '\n')))
		*p = '\0';
        printk(KERN_INFO "act2000: Firmware-ID: %s\n", fid.revision);
	if (card->flags & ACT2000_FLAGS_IVALID) {
		printk(KERN_DEBUG "Enabling Interrupts ...\n");
		act2000_isa_enable_irq(card);
	}
        return 0;
}

/*
 * Download microcode into card, check Firmware signature.
 */
int
act2000_isa_download(act2000_card * card, act2000_ddef * cb)
{
        int length;
        int ret;
        int l;
        int c;
        long timeout;
        u_char *b;
        u_char *p;
        u_char *buf;
        act2000_ddef cblock;

        if (!act2000_isa_reset(card->port))
                return -ENXIO;
        act2000_isa_delay(HZ / 2);
        if ((ret = verify_area(VERIFY_READ, (void *) cb, sizeof(cblock))))
                return ret;
        copy_from_user(&cblock, (char *) cb, sizeof(cblock));
        length = cblock.length;
        p = cblock.buffer;
        if ((ret = verify_area(VERIFY_READ, (void *) p, length)))
                return ret;
        buf = (u_char *) kmalloc(1024, GFP_KERNEL);
        if (!buf)
                return -ENOMEM;
        timeout = 0;
        while (length) {
                l = (length > 1024) ? 1024 : length;
                c = 0;
                b = buf;
                copy_from_user(buf, p, l);
                while (c < l) {
                        if (act2000_isa_writeb(card, *b++)) {
                                printk(KERN_WARNING
                                       "act2000: loader timed out"
                                       " len=%d c=%d\n", length, c);
                                kfree(buf);
                                return -ETIME;
                        }
                        c++;
                }
                length -= l;
                p += l;
        }
        kfree(buf);
        act2000_isa_delay(HZ / 2);
        return (act2000_isa_getid(card));
}
