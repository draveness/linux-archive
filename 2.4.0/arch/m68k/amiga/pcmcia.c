/*
** asm-m68k/pcmcia.c -- Amiga Linux PCMCIA support
**                      most information was found by disassembling card.resource
**                      I'm still looking for an official doc !
**
** Copyright 1997 by Alain Malek
**
** This file is subject to the terms and conditions of the GNU General Public
** License.  See the file COPYING in the main directory of this archive
** for more details.
**
** Created: 12/10/97 by Alain Malek
*/

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <asm/amigayle.h>
#include <asm/amipcmcia.h>

/* gayle config byte for program voltage and access speed */
static u_char cfg_byte = GAYLE_CFG_0V|GAYLE_CFG_150NS;

void pcmcia_reset(void)
{
	unsigned long reset_start_time = jiffies;
	unsigned char b;

	gayle_reset = 0x00;
	while (time_before(jiffies, reset_start_time + 1*HZ/100));
	b = gayle_reset;
}


/* copy a tuple, including tuple header. return nb bytes copied */
/* be carefull as this may trigger a GAYLE_IRQ_WR interrupt ! */

int pcmcia_copy_tuple(unsigned char tuple_id, void *tuple, int max_len)
{
	unsigned char id, *dest;
	int cnt, pos, len;

	dest = tuple;
	pos = 0;

	id = gayle_attribute[pos];

	while((id != CISTPL_END) && (pos < 0x10000)) {
		len = (int)gayle_attribute[pos+2] + 2;
		if (id == tuple_id) {
			len = (len > max_len)?max_len:len;
			for (cnt = 0; cnt < len; cnt++) {
				*dest++ = gayle_attribute[pos+(cnt<<1)];
			}

			return len;
		}
		pos += len<<1;
		id = gayle_attribute[pos];
	}

	return 0;
}

void pcmcia_program_voltage(int voltage)
{
	u_char v;

	switch (voltage) {
	case PCMCIA_0V:
		v = GAYLE_CFG_0V;
		break;
	case PCMCIA_5V:
		v = GAYLE_CFG_5V;
		break;
	case PCMCIA_12V:
		v = GAYLE_CFG_12V;
		break;
	default:
		v = GAYLE_CFG_0V;
	}

	cfg_byte = (cfg_byte & 0xfc) | v;
	gayle.config = cfg_byte;

}

void pcmcia_access_speed(int speed)
{
	u_char s;

	if (speed <= PCMCIA_SPEED_100NS)
		s = GAYLE_CFG_100NS;
	else if (speed <= PCMCIA_SPEED_150NS)
		s = GAYLE_CFG_150NS;
	else if (speed <= PCMCIA_SPEED_250NS)
		s = GAYLE_CFG_250NS;
	else
		s = GAYLE_CFG_720NS;

	cfg_byte = (cfg_byte & 0xf3) | s;
	gayle.config = cfg_byte;
}

void pcmcia_write_enable(void)
{
	gayle.cardstatus = GAYLE_CS_WR|GAYLE_CS_DA;
}

void pcmcia_write_disable(void)
{
	gayle.cardstatus = 0;
}
