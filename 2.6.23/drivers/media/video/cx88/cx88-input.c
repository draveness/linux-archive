/*
 *
 * Device driver for GPIO attached remote control interfaces
 * on Conexant 2388x based TV/DVB cards.
 *
 * Copyright (c) 2003 Pavel Machek
 * Copyright (c) 2004 Gerd Knorr
 * Copyright (c) 2004, 2005 Chris Pascoe
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <linux/init.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "cx88.h"
#include <media/ir-common.h>

/* ---------------------------------------------------------------------- */

struct cx88_IR {
	struct cx88_core *core;
	struct input_dev *input;
	struct ir_input_state ir;
	char name[32];
	char phys[32];

	/* sample from gpio pin 16 */
	u32 sampling;
	u32 samples[16];
	int scount;
	unsigned long release;

	/* poll external decoder */
	int polling;
	struct work_struct work;
	struct timer_list timer;
	u32 gpio_addr;
	u32 last_gpio;
	u32 mask_keycode;
	u32 mask_keydown;
	u32 mask_keyup;
};

static int ir_debug = 0;
module_param(ir_debug, int, 0644);	/* debug level [IR] */
MODULE_PARM_DESC(ir_debug, "enable debug messages [IR]");

#define ir_dprintk(fmt, arg...)	if (ir_debug) \
	printk(KERN_DEBUG "%s IR: " fmt , ir->core->name , ##arg)

/* ---------------------------------------------------------------------- */

static void cx88_ir_handle_key(struct cx88_IR *ir)
{
	struct cx88_core *core = ir->core;
	u32 gpio, data, auxgpio;

	/* read gpio value */
	gpio = cx_read(ir->gpio_addr);
	switch (core->board) {
	case CX88_BOARD_NPGTECH_REALTV_TOP10FM:
		/* This board apparently uses a combination of 2 GPIO
		   to represent the keys. Additionally, the second GPIO
		   can be used for parity.

		   Example:

		   for key "5"
			gpio = 0x758, auxgpio = 0xe5 or 0xf5
		   for key "Power"
			gpio = 0x758, auxgpio = 0xed or 0xfd
		 */

		auxgpio = cx_read(MO_GP1_IO);
		/* Take out the parity part */
		gpio=(gpio & 0x7fd) + (auxgpio & 0xef);
		break;
	case CX88_BOARD_WINFAST_DTV1000:
		gpio = (gpio & 0x6ff) | ((cx_read(MO_GP1_IO) << 8) & 0x900);
		auxgpio = gpio;
		break;
	default:
		auxgpio = gpio;
	}
	if (ir->polling) {
		if (ir->last_gpio == auxgpio)
			return;
		ir->last_gpio = auxgpio;
	}

	/* extract data */
	data = ir_extract_bits(gpio, ir->mask_keycode);
	ir_dprintk("irq gpio=0x%x code=%d | %s%s%s\n",
		   gpio, data,
		   ir->polling ? "poll" : "irq",
		   (gpio & ir->mask_keydown) ? " down" : "",
		   (gpio & ir->mask_keyup) ? " up" : "");

	if (ir->core->board == CX88_BOARD_NORWOOD_MICRO) {
		u32 gpio_key = cx_read(MO_GP0_IO);

		data = (data << 4) | ((gpio_key & 0xf0) >> 4);

		ir_input_keydown(ir->input, &ir->ir, data, data);
		ir_input_nokey(ir->input, &ir->ir);

	} else if (ir->mask_keydown) {
		/* bit set on keydown */
		if (gpio & ir->mask_keydown) {
			ir_input_keydown(ir->input, &ir->ir, data, data);
		} else {
			ir_input_nokey(ir->input, &ir->ir);
		}

	} else if (ir->mask_keyup) {
		/* bit cleared on keydown */
		if (0 == (gpio & ir->mask_keyup)) {
			ir_input_keydown(ir->input, &ir->ir, data, data);
		} else {
			ir_input_nokey(ir->input, &ir->ir);
		}

	} else {
		/* can't distinguish keydown/up :-/ */
		ir_input_keydown(ir->input, &ir->ir, data, data);
		ir_input_nokey(ir->input, &ir->ir);
	}
}

static void ir_timer(unsigned long data)
{
	struct cx88_IR *ir = (struct cx88_IR *)data;

	schedule_work(&ir->work);
}

static void cx88_ir_work(struct work_struct *work)
{
	struct cx88_IR *ir = container_of(work, struct cx88_IR, work);

	cx88_ir_handle_key(ir);
	mod_timer(&ir->timer, jiffies + msecs_to_jiffies(ir->polling));
}

static void cx88_ir_start(struct cx88_core *core, struct cx88_IR *ir)
{
	if (ir->polling) {
		setup_timer(&ir->timer, ir_timer, (unsigned long)ir);
		INIT_WORK(&ir->work, cx88_ir_work);
		schedule_work(&ir->work);
	}
	if (ir->sampling) {
		core->pci_irqmask |= (1 << 18);	/* IR_SMP_INT */
		cx_write(MO_DDS_IO, 0xa80a80);	/* 4 kHz sample rate */
		cx_write(MO_DDSCFG_IO, 0x5);	/* enable */
	}
}

static void cx88_ir_stop(struct cx88_core *core, struct cx88_IR *ir)
{
	if (ir->sampling) {
		cx_write(MO_DDSCFG_IO, 0x0);
		core->pci_irqmask &= ~(1 << 18);
	}

	if (ir->polling) {
		del_timer_sync(&ir->timer);
		flush_scheduled_work();
	}
}

/* ---------------------------------------------------------------------- */

int cx88_ir_init(struct cx88_core *core, struct pci_dev *pci)
{
	struct cx88_IR *ir;
	struct input_dev *input_dev;
	IR_KEYTAB_TYPE *ir_codes = NULL;
	int ir_type = IR_TYPE_OTHER;
	int err = -ENOMEM;

	ir = kzalloc(sizeof(*ir), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!ir || !input_dev)
		goto err_out_free;

	ir->input = input_dev;

	/* detect & configure */
	switch (core->board) {
	case CX88_BOARD_DNTV_LIVE_DVB_T:
	case CX88_BOARD_KWORLD_DVB_T:
	case CX88_BOARD_KWORLD_DVB_T_CX22702:
		ir_codes = ir_codes_dntv_live_dvb_t;
		ir->gpio_addr = MO_GP1_IO;
		ir->mask_keycode = 0x1f;
		ir->mask_keyup = 0x60;
		ir->polling = 50; /* ms */
		break;
	case CX88_BOARD_TERRATEC_CINERGY_1400_DVB_T1:
		ir_codes = ir_codes_cinergy_1400;
		ir_type = IR_TYPE_PD;
		ir->sampling = 0xeb04; /* address */
		break;
	case CX88_BOARD_HAUPPAUGE:
	case CX88_BOARD_HAUPPAUGE_DVB_T1:
	case CX88_BOARD_HAUPPAUGE_NOVASE2_S1:
	case CX88_BOARD_HAUPPAUGE_NOVASPLUS_S1:
	case CX88_BOARD_HAUPPAUGE_HVR1100:
	case CX88_BOARD_HAUPPAUGE_HVR3000:
		ir_codes = ir_codes_hauppauge_new;
		ir_type = IR_TYPE_RC5;
		ir->sampling = 1;
		break;
	case CX88_BOARD_WINFAST_DTV2000H:
		ir_codes = ir_codes_winfast;
		ir->gpio_addr = MO_GP0_IO;
		ir->mask_keycode = 0x8f8;
		ir->mask_keyup = 0x100;
		ir->polling = 50; /* ms */
		break;
	case CX88_BOARD_WINFAST2000XP_EXPERT:
	case CX88_BOARD_WINFAST_DTV1000:
		ir_codes = ir_codes_winfast;
		ir->gpio_addr = MO_GP0_IO;
		ir->mask_keycode = 0x8f8;
		ir->mask_keyup = 0x100;
		ir->polling = 1; /* ms */
		break;
	case CX88_BOARD_IODATA_GVBCTV7E:
		ir_codes = ir_codes_iodata_bctv7e;
		ir->gpio_addr = MO_GP0_IO;
		ir->mask_keycode = 0xfd;
		ir->mask_keydown = 0x02;
		ir->polling = 5; /* ms */
		break;
	case CX88_BOARD_PROLINK_PLAYTVPVR:
	case CX88_BOARD_PIXELVIEW_PLAYTV_ULTRA_PRO:
		ir_codes = ir_codes_pixelview;
		ir->gpio_addr = MO_GP1_IO;
		ir->mask_keycode = 0x1f;
		ir->mask_keyup = 0x80;
		ir->polling = 1; /* ms */
		break;
	case CX88_BOARD_KWORLD_LTV883:
		ir_codes = ir_codes_pixelview;
		ir->gpio_addr = MO_GP1_IO;
		ir->mask_keycode = 0x1f;
		ir->mask_keyup = 0x60;
		ir->polling = 1; /* ms */
		break;
	case CX88_BOARD_ADSTECH_DVB_T_PCI:
		ir_codes = ir_codes_adstech_dvb_t_pci;
		ir->gpio_addr = MO_GP1_IO;
		ir->mask_keycode = 0xbf;
		ir->mask_keyup = 0x40;
		ir->polling = 50; /* ms */
		break;
	case CX88_BOARD_MSI_TVANYWHERE_MASTER:
		ir_codes = ir_codes_msi_tvanywhere;
		ir->gpio_addr = MO_GP1_IO;
		ir->mask_keycode = 0x1f;
		ir->mask_keyup = 0x40;
		ir->polling = 1; /* ms */
		break;
	case CX88_BOARD_AVERTV_303:
	case CX88_BOARD_AVERTV_STUDIO_303:
		ir_codes         = ir_codes_avertv_303;
		ir->gpio_addr    = MO_GP2_IO;
		ir->mask_keycode = 0xfb;
		ir->mask_keydown = 0x02;
		ir->polling      = 50; /* ms */
		break;
	case CX88_BOARD_DNTV_LIVE_DVB_T_PRO:
		ir_codes = ir_codes_dntv_live_dvbt_pro;
		ir_type = IR_TYPE_PD;
		ir->sampling = 0xff00; /* address */
		break;
	case CX88_BOARD_NORWOOD_MICRO:
		ir_codes         = ir_codes_norwood;
		ir->gpio_addr    = MO_GP1_IO;
		ir->mask_keycode = 0x0e;
		ir->mask_keyup   = 0x80;
		ir->polling      = 50; /* ms */
		break;
	case CX88_BOARD_NPGTECH_REALTV_TOP10FM:
		ir_codes = ir_codes_npgtech;
		ir->gpio_addr = MO_GP0_IO;
		ir->mask_keycode = 0xfa;
		ir->polling = 50; /* ms */
		break;
	}

	if (NULL == ir_codes) {
		err = -ENODEV;
		goto err_out_free;
	}

	/* init input device */
	snprintf(ir->name, sizeof(ir->name), "cx88 IR (%s)",
		 cx88_boards[core->board].name);
	snprintf(ir->phys, sizeof(ir->phys), "pci-%s/ir0", pci_name(pci));

	ir_input_init(input_dev, &ir->ir, ir_type, ir_codes);
	input_dev->name = ir->name;
	input_dev->phys = ir->phys;
	input_dev->id.bustype = BUS_PCI;
	input_dev->id.version = 1;
	if (pci->subsystem_vendor) {
		input_dev->id.vendor = pci->subsystem_vendor;
		input_dev->id.product = pci->subsystem_device;
	} else {
		input_dev->id.vendor = pci->vendor;
		input_dev->id.product = pci->device;
	}
	input_dev->dev.parent = &pci->dev;
	/* record handles to ourself */
	ir->core = core;
	core->ir = ir;

	cx88_ir_start(core, ir);

	/* all done */
	err = input_register_device(ir->input);
	if (err)
		goto err_out_stop;

	return 0;

 err_out_stop:
	cx88_ir_stop(core, ir);
	core->ir = NULL;
 err_out_free:
	input_free_device(input_dev);
	kfree(ir);
	return err;
}

int cx88_ir_fini(struct cx88_core *core)
{
	struct cx88_IR *ir = core->ir;

	/* skip detach on non attached boards */
	if (NULL == ir)
		return 0;

	cx88_ir_stop(core, ir);
	input_unregister_device(ir->input);
	kfree(ir);

	/* done */
	core->ir = NULL;
	return 0;
}

/* ---------------------------------------------------------------------- */

void cx88_ir_irq(struct cx88_core *core)
{
	struct cx88_IR *ir = core->ir;
	u32 samples, ircode;
	int i;

	if (NULL == ir)
		return;
	if (!ir->sampling)
		return;

	samples = cx_read(MO_SAMPLE_IO);
	if (0 != samples && 0xffffffff != samples) {
		/* record sample data */
		if (ir->scount < ARRAY_SIZE(ir->samples))
			ir->samples[ir->scount++] = samples;
		return;
	}
	if (!ir->scount) {
		/* nothing to sample */
		if (ir->ir.keypressed && time_after(jiffies, ir->release))
			ir_input_nokey(ir->input, &ir->ir);
		return;
	}

	/* have a complete sample */
	if (ir->scount < ARRAY_SIZE(ir->samples))
		ir->samples[ir->scount++] = samples;
	for (i = 0; i < ir->scount; i++)
		ir->samples[i] = ~ir->samples[i];
	if (ir_debug)
		ir_dump_samples(ir->samples, ir->scount);

	/* decode it */
	switch (core->board) {
	case CX88_BOARD_TERRATEC_CINERGY_1400_DVB_T1:
	case CX88_BOARD_DNTV_LIVE_DVB_T_PRO:
		ircode = ir_decode_pulsedistance(ir->samples, ir->scount, 1, 4);

		if (ircode == 0xffffffff) { /* decoding error */
			ir_dprintk("pulse distance decoding error\n");
			break;
		}

		ir_dprintk("pulse distance decoded: %x\n", ircode);

		if (ircode == 0) { /* key still pressed */
			ir_dprintk("pulse distance decoded repeat code\n");
			ir->release = jiffies + msecs_to_jiffies(120);
			break;
		}

		if ((ircode & 0xffff) != (ir->sampling & 0xffff)) { /* wrong address */
			ir_dprintk("pulse distance decoded wrong address\n");
			break;
		}

		if (((~ircode >> 24) & 0xff) != ((ircode >> 16) & 0xff)) { /* wrong checksum */
			ir_dprintk("pulse distance decoded wrong check sum\n");
			break;
		}

		ir_dprintk("Key Code: %x\n", (ircode >> 16) & 0x7f);

		ir_input_keydown(ir->input, &ir->ir, (ircode >> 16) & 0x7f, (ircode >> 16) & 0xff);
		ir->release = jiffies + msecs_to_jiffies(120);
		break;
	case CX88_BOARD_HAUPPAUGE:
	case CX88_BOARD_HAUPPAUGE_DVB_T1:
	case CX88_BOARD_HAUPPAUGE_NOVASE2_S1:
	case CX88_BOARD_HAUPPAUGE_NOVASPLUS_S1:
	case CX88_BOARD_HAUPPAUGE_HVR1100:
	case CX88_BOARD_HAUPPAUGE_HVR3000:
		ircode = ir_decode_biphase(ir->samples, ir->scount, 5, 7);
		ir_dprintk("biphase decoded: %x\n", ircode);
		if ((ircode & 0xfffff000) != 0x3000)
			break;
		ir_input_keydown(ir->input, &ir->ir, ircode & 0x3f, ircode);
		ir->release = jiffies + msecs_to_jiffies(120);
		break;
	}

	ir->scount = 0;
	return;
}

/* ---------------------------------------------------------------------- */

MODULE_AUTHOR("Gerd Knorr, Pavel Machek, Chris Pascoe");
MODULE_DESCRIPTION("input driver for cx88 GPIO-based IR remote controls");
MODULE_LICENSE("GPL");
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
