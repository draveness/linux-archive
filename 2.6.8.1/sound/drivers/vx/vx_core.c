/*
 * Driver for Digigram VX soundcards
 *
 * Hardware core part
 *
 * Copyright (c) 2002 by Takashi Iwai <tiwai@suse.de>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <sound/driver.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/asoundef.h>
#include <sound/info.h>
#include <asm/io.h>
#include <sound/vx_core.h>
#include "vx_cmd.h"

MODULE_AUTHOR("Takashi Iwai <tiwai@suse.de>");
MODULE_DESCRIPTION("Common routines for Digigram VX drivers");
MODULE_LICENSE("GPL");


/*
 * snd_vx_delay - delay for the specified time
 * @xmsec: the time to delay in msec
 */
void snd_vx_delay(vx_core_t *chip, int xmsec)
{
	if (! in_interrupt() && xmsec >= 1000 / HZ) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout((xmsec * HZ + 999) / 1000);
	} else {
		mdelay(xmsec);
	}
}

/*
 * vx_check_reg_bit - wait for the specified bit is set/reset on a register
 * @reg: register to check
 * @mask: bit mask
 * @bit: resultant bit to be checked
 * @time: time-out of loop in msec
 *
 * returns zero if a bit matches, or a negative error code.
 */
int snd_vx_check_reg_bit(vx_core_t *chip, int reg, int mask, int bit, int time)
{
	unsigned long end_time = jiffies + (time * HZ + 999) / 1000;
#ifdef CONFIG_SND_DEBUG
	static char *reg_names[VX_REG_MAX] = {
		"ICR", "CVR", "ISR", "IVR", "RXH", "RXM", "RXL",
		"DMA", "CDSP", "RFREQ", "RUER/V2", "DATA", "MEMIRQ",
		"ACQ", "BIT0", "BIT1", "MIC0", "MIC1", "MIC2",
		"MIC3", "INTCSR", "CNTRL", "GPIOC",
		"LOFREQ", "HIFREQ", "CSUER", "RUER"
	};
#endif
	do {
		if ((snd_vx_inb(chip, reg) & mask) == bit)
			return 0;
		//snd_vx_delay(chip, 10);
	} while (time_after_eq(end_time, jiffies));
	snd_printd(KERN_DEBUG "vx_check_reg_bit: timeout, reg=%s, mask=0x%x, val=0x%x\n", reg_names[reg], mask, snd_vx_inb(chip, reg));
	return -EIO;
}

/*
 * vx_send_irq_dsp - set command irq bit
 * @num: the requested IRQ type, IRQ_XXX
 *
 * this triggers the specified IRQ request
 * returns 0 if successful, or a negative error code.
 * 
 */
static int vx_send_irq_dsp(vx_core_t *chip, int num)
{
	int nirq;

	/* wait for Hc = 0 */
	if (snd_vx_check_reg_bit(chip, VX_CVR, CVR_HC, 0, 200) < 0)
		return -EIO;

	nirq = num;
	if (vx_has_new_dsp(chip))
		nirq += VXP_IRQ_OFFSET;
	vx_outb(chip, CVR, (nirq >> 1) | CVR_HC);
	return 0;
}


/*
 * vx_reset_chk - reset CHK bit on ISR
 *
 * returns 0 if successful, or a negative error code.
 */
static int vx_reset_chk(vx_core_t *chip)
{
	/* Reset irq CHK */
	if (vx_send_irq_dsp(chip, IRQ_RESET_CHK) < 0)
		return -EIO;
	/* Wait until CHK = 0 */
	if (vx_check_isr(chip, ISR_CHK, 0, 200) < 0)
		return -EIO;
	return 0;
}

/*
 * vx_transfer_end - terminate message transfer
 * @cmd: IRQ message to send (IRQ_MESS_XXX_END)
 *
 * returns 0 if successful, or a negative error code.
 * the error code can be VX-specific, retrieved via vx_get_error().
 * NB: call with spinlock held!
 */
static int vx_transfer_end(vx_core_t *chip, int cmd)
{
	int err;

	if ((err = vx_reset_chk(chip)) < 0)
		return err;

	/* irq MESS_READ/WRITE_END */
	if ((err = vx_send_irq_dsp(chip, cmd)) < 0)
		return err;

	/* Wait CHK = 1 */
	if ((err = vx_wait_isr_bit(chip, ISR_CHK)) < 0)
		return err;

	/* If error, Read RX */
	if ((err = vx_inb(chip, ISR)) & ISR_ERR) {
		if ((err = vx_wait_for_rx_full(chip)) < 0) {
			snd_printd(KERN_DEBUG "transfer_end: error in rx_full\n");
			return err;
		}
		err = vx_inb(chip, RXH) << 16;
		err |= vx_inb(chip, RXM) << 8;
		err |= vx_inb(chip, RXL);
		snd_printd(KERN_DEBUG "transfer_end: error = 0x%x\n", err);
		return -(VX_ERR_MASK | err);
	}
	return 0;
}

/*
 * vx_read_status - return the status rmh
 * @rmh: rmh record to store the status
 *
 * returns 0 if successful, or a negative error code.
 * the error code can be VX-specific, retrieved via vx_get_error().
 * NB: call with spinlock held!
 */
static int vx_read_status(vx_core_t *chip, struct vx_rmh *rmh)
{
	int i, err, val, size;

	/* no read necessary? */
	if (rmh->DspStat == RMH_SSIZE_FIXED && rmh->LgStat == 0)
		return 0;

	/* Wait for RX full (with timeout protection)
	 * The first word of status is in RX
	 */
	err = vx_wait_for_rx_full(chip);
	if (err < 0)
		return err;

	/* Read RX */
	val = vx_inb(chip, RXH) << 16;
	val |= vx_inb(chip, RXM) << 8;
	val |= vx_inb(chip, RXL);

	/* If status given by DSP, let's decode its size */
	switch (rmh->DspStat) {
	case RMH_SSIZE_ARG:
		size = val & 0xff;
		rmh->Stat[0] = val & 0xffff00;
		rmh->LgStat = size + 1;
		break;
	case RMH_SSIZE_MASK:
		/* Let's count the arg numbers from a mask */
		rmh->Stat[0] = val;
		size = 0;
		while (val) {
			if (val & 0x01)
				size++;
			val >>= 1;
		}
		rmh->LgStat = size + 1;
		break;
	default:
		/* else retrieve the status length given by the driver */
		size = rmh->LgStat;
		rmh->Stat[0] = val;  /* Val is the status 1st word */
		size--;              /* hence adjust remaining length */
		break;
        }

	if (size < 1)
		return 0;
	snd_assert(size <= SIZE_MAX_STATUS, return -EINVAL);

	for (i = 1; i <= size; i++) {
		/* trigger an irq MESS_WRITE_NEXT */
		err = vx_send_irq_dsp(chip, IRQ_MESS_WRITE_NEXT);
		if (err < 0)
			return err;
		/* Wait for RX full (with timeout protection) */
		err = vx_wait_for_rx_full(chip);
		if (err < 0)
			return err;
		rmh->Stat[i] = vx_inb(chip, RXH) << 16;
		rmh->Stat[i] |= vx_inb(chip, RXM) <<  8;
		rmh->Stat[i] |= vx_inb(chip, RXL);
	}

	return vx_transfer_end(chip, IRQ_MESS_WRITE_END);
}


#define MASK_MORE_THAN_1_WORD_COMMAND   0x00008000
#define MASK_1_WORD_COMMAND             0x00ff7fff

/*
 * vx_send_msg_nolock - send a DSP message and read back the status
 * @rmh: the rmh record to send and receive
 *
 * returns 0 if successful, or a negative error code.
 * the error code can be VX-specific, retrieved via vx_get_error().
 * 
 * this function doesn't call spinlock at all.
 */
int vx_send_msg_nolock(vx_core_t *chip, struct vx_rmh *rmh)
{
	int i, err;
	
	if (chip->chip_status & VX_STAT_IS_STALE)
		return -EBUSY;

	if ((err = vx_reset_chk(chip)) < 0) {
		snd_printd(KERN_DEBUG "vx_send_msg: vx_reset_chk error\n");
		return err;
	}

#if 0
	printk(KERN_DEBUG "rmh: cmd = 0x%06x, length = %d, stype = %d\n",
	       rmh->Cmd[0], rmh->LgCmd, rmh->DspStat);
	if (rmh->LgCmd > 1) {
		printk(KERN_DEBUG "  ");
		for (i = 1; i < rmh->LgCmd; i++)
			printk("0x%06x ", rmh->Cmd[i]);
		printk("\n");
	}
#endif
	/* Check bit M is set according to length of the command */
	if (rmh->LgCmd > 1)
		rmh->Cmd[0] |= MASK_MORE_THAN_1_WORD_COMMAND;
	else
		rmh->Cmd[0] &= MASK_1_WORD_COMMAND;

	/* Wait for TX empty */
	if ((err = vx_wait_isr_bit(chip, ISR_TX_EMPTY)) < 0) {
		snd_printd(KERN_DEBUG "vx_send_msg: wait tx empty error\n");
		return err;
	}

	/* Write Cmd[0] */
	vx_outb(chip, TXH, (rmh->Cmd[0] >> 16) & 0xff);
	vx_outb(chip, TXM, (rmh->Cmd[0] >> 8) & 0xff);
	vx_outb(chip, TXL, rmh->Cmd[0] & 0xff);

	/* Trigger irq MESSAGE */
	if ((err = vx_send_irq_dsp(chip, IRQ_MESSAGE)) < 0) {
		snd_printd(KERN_DEBUG "vx_send_msg: send IRQ_MESSAGE error\n");
		return err;
	}

	/* Wait for CHK = 1 */
	if ((err = vx_wait_isr_bit(chip, ISR_CHK)) < 0)
		return err;

	/* If error, get error value from RX */
	if (vx_inb(chip, ISR) & ISR_ERR) {
		if ((err = vx_wait_for_rx_full(chip)) < 0) {
			snd_printd(KERN_DEBUG "vx_send_msg: rx_full read error\n");
			return err;
		}
		err = vx_inb(chip, RXH) << 16;
		err |= vx_inb(chip, RXM) << 8;
		err |= vx_inb(chip, RXL);
		snd_printd(KERN_DEBUG "msg got error = 0x%x at cmd[0]\n", err);
		err = -(VX_ERR_MASK | err);
		return err;
	}

	/* Send the other words */
	if (rmh->LgCmd > 1) {
		for (i = 1; i < rmh->LgCmd; i++) {
			/* Wait for TX ready */
			if ((err = vx_wait_isr_bit(chip, ISR_TX_READY)) < 0) {
				snd_printd(KERN_DEBUG "vx_send_msg: tx_ready error\n");
				return err;
			}

			/* Write Cmd[i] */
			vx_outb(chip, TXH, (rmh->Cmd[i] >> 16) & 0xff);
			vx_outb(chip, TXM, (rmh->Cmd[i] >> 8) & 0xff);
			vx_outb(chip, TXL, rmh->Cmd[i] & 0xff);

			/* Trigger irq MESS_READ_NEXT */
			if ((err = vx_send_irq_dsp(chip, IRQ_MESS_READ_NEXT)) < 0) {
				snd_printd(KERN_DEBUG "vx_send_msg: IRQ_READ_NEXT error\n");
				return err;
			}
		}
		/* Wait for TX empty */
		if ((err = vx_wait_isr_bit(chip, ISR_TX_READY)) < 0) {
			snd_printd(KERN_DEBUG "vx_send_msg: TX_READY error\n");
			return err;
		}
		/* End of transfer */
		err = vx_transfer_end(chip, IRQ_MESS_READ_END);
		if (err < 0)
			return err;
	}

	return vx_read_status(chip, rmh);
}


/*
 * vx_send_msg - send a DSP message with spinlock
 * @rmh: the rmh record to send and receive
 *
 * returns 0 if successful, or a negative error code.
 * see vx_send_msg_nolock().
 */
int vx_send_msg(vx_core_t *chip, struct vx_rmh *rmh)
{
	unsigned long flags;
	int err;

	spin_lock_irqsave(&chip->lock, flags);
	err = vx_send_msg_nolock(chip, rmh);
	spin_unlock_irqrestore(&chip->lock, flags);
	return err;
}


/*
 * vx_send_rih_nolock - send an RIH to xilinx
 * @cmd: the command to send
 *
 * returns 0 if successful, or a negative error code.
 * the error code can be VX-specific, retrieved via vx_get_error().
 *
 * this function doesn't call spinlock at all.
 *
 * unlike RMH, no command is sent to DSP.
 */
int vx_send_rih_nolock(vx_core_t *chip, int cmd)
{
	int err;

	if (chip->chip_status & VX_STAT_IS_STALE)
		return -EBUSY;

#if 0
	printk(KERN_DEBUG "send_rih: cmd = 0x%x\n", cmd);
#endif
	if ((err = vx_reset_chk(chip)) < 0)
		return err;
	/* send the IRQ */
	if ((err = vx_send_irq_dsp(chip, cmd)) < 0)
		return err;
	/* Wait CHK = 1 */
	if ((err = vx_wait_isr_bit(chip, ISR_CHK)) < 0)
		return err;
	/* If error, read RX */
	if (vx_inb(chip, ISR) & ISR_ERR) {
		if ((err = vx_wait_for_rx_full(chip)) < 0)
			return err;
		err = vx_inb(chip, RXH) << 16;
		err |= vx_inb(chip, RXM) << 8;
		err |= vx_inb(chip, RXL);
		return -(VX_ERR_MASK | err);
	}
	return 0;
}


/*
 * vx_send_rih - send an RIH with spinlock
 * @cmd: the command to send
 *
 * see vx_send_rih_nolock().
 */
int vx_send_rih(vx_core_t *chip, int cmd)
{
	unsigned long flags;
	int err;

	spin_lock_irqsave(&chip->lock, flags);
	err = vx_send_rih_nolock(chip, cmd);
	spin_unlock_irqrestore(&chip->lock, flags);
	return err;
}

#define END_OF_RESET_WAIT_TIME		500	/* us */

/**
 * snd_vx_boot_xilinx - boot up the xilinx interface
 * @boot: the boot record to load
 */
int snd_vx_load_boot_image(vx_core_t *chip, const snd_hwdep_dsp_image_t *boot)
{
	unsigned int i;
	int no_fillup = vx_has_new_dsp(chip);

	/* check the length of boot image */
	snd_assert(boot->length > 0, return -EINVAL);
	snd_assert(boot->length % 3 == 0, return -EINVAL);
	snd_assert(boot->image, return -EINVAL);
#if 0
	{
		/* more strict check */
		unsigned int c = ((u32)boot->image[0] << 16) | ((u32)boot->image[1] << 8) | boot->image[2];
		snd_assert(boot->length == (c + 2) * 3, return -EINVAL);
	}
#endif

	/* reset dsp */
	vx_reset_dsp(chip);
	
	udelay(END_OF_RESET_WAIT_TIME); /* another wait? */

	/* download boot strap */
	for (i = 0; i < 0x600; i += 3) {
		if (i >= boot->length) {
			if (no_fillup)
				break;
			if (vx_wait_isr_bit(chip, ISR_TX_EMPTY) < 0) {
				snd_printk(KERN_ERR "dsp boot failed at %d\n", i);
				return -EIO;
			}
			vx_outb(chip, TXH, 0);
			vx_outb(chip, TXM, 0);
			vx_outb(chip, TXL, 0);
		} else {
			unsigned char image[3];
			if (copy_from_user(image, boot->image + i, 3))
				return -EFAULT;
			if (vx_wait_isr_bit(chip, ISR_TX_EMPTY) < 0) {
				snd_printk(KERN_ERR "dsp boot failed at %d\n", i);
				return -EIO;
			}
			vx_outb(chip, TXH, image[0]);
			vx_outb(chip, TXM, image[1]);
			vx_outb(chip, TXL, image[2]);
		}
	}
	return 0;
}

/*
 * vx_test_irq_src - query the source of interrupts
 *
 * called from irq handler only
 */
static int vx_test_irq_src(vx_core_t *chip, unsigned int *ret)
{
	int err;

	vx_init_rmh(&chip->irq_rmh, CMD_TEST_IT);
	spin_lock(&chip->lock);
	err = vx_send_msg_nolock(chip, &chip->irq_rmh);
	if (err < 0)
		*ret = 0;
	else
		*ret = chip->irq_rmh.Stat[0];
	spin_unlock(&chip->lock);
	return err;
}


/*
 * vx_interrupt - soft irq handler
 */
static void vx_interrupt(unsigned long private_data)
{
	vx_core_t *chip = snd_magic_cast(vx_core_t, (void*)private_data, return);
	unsigned int events;
		
	if (chip->chip_status & VX_STAT_IS_STALE)
		return;

	if (vx_test_irq_src(chip, &events) < 0)
		return;
    
#if 0
	if (events & 0x000800)
		printk(KERN_ERR "DSP Stream underrun ! IRQ events = 0x%x\n", events);
#endif
	// printk(KERN_DEBUG "IRQ events = 0x%x\n", events);

	/* We must prevent any application using this DSP
	 * and block any further request until the application
	 * either unregisters or reloads the DSP
	 */
	if (events & FATAL_DSP_ERROR) {
		snd_printk(KERN_ERR "vx_core: fatal DSP error!!\n");
		return;
	}

	/* The start on time code conditions are filled (ie the time code
	 * received by the board is equal to one of those given to it).
	 */
	if (events & TIME_CODE_EVENT_PENDING)
		; /* so far, nothing to do yet */

	/* The frequency has changed on the board (UER mode). */
	if (events & FREQUENCY_CHANGE_EVENT_PENDING)
		vx_change_frequency(chip);

	/* update the pcm streams */
	vx_pcm_update_intr(chip, events);
}


/**
 * snd_vx_irq_handler - interrupt handler
 */
irqreturn_t snd_vx_irq_handler(int irq, void *dev, struct pt_regs *regs)
{
	vx_core_t *chip = snd_magic_cast(vx_core_t, dev, return IRQ_NONE);

	if (! (chip->chip_status & VX_STAT_CHIP_INIT) ||
	    (chip->chip_status & VX_STAT_IS_STALE))
		return IRQ_NONE;
	if (! vx_test_and_ack(chip))
		tasklet_hi_schedule(&chip->tq);
	return IRQ_HANDLED;
}


/*
 */
static void vx_reset_board(vx_core_t *chip, int cold_reset)
{
	snd_assert(chip->ops->reset_board, return);

	/* current source, later sync'ed with target */
	chip->audio_source = VX_AUDIO_SRC_LINE;
	if (cold_reset) {
		chip->audio_source_target = chip->audio_source;
		chip->clock_source = INTERNAL_QUARTZ;
		chip->freq = 48000;
		chip->uer_detected = VX_UER_MODE_NOT_PRESENT;
		chip->uer_bits = SNDRV_PCM_DEFAULT_CON_SPDIF;
	}

	chip->ops->reset_board(chip, cold_reset);

	vx_reset_codec(chip, cold_reset);

	vx_set_internal_clock(chip, chip->freq);

	/* Reset the DSP */
	vx_reset_dsp(chip);

	if (vx_is_pcmcia(chip)) {
		/* Acknowledge any pending IRQ and reset the MEMIRQ flag. */
		vx_test_and_ack(chip);
		vx_validate_irq(chip, 1);
	}

	/* init CBits */
	vx_set_iec958_status(chip, chip->uer_bits);
}


/*
 * proc interface
 */

static void vx_proc_read(snd_info_entry_t *entry, snd_info_buffer_t *buffer)
{
	vx_core_t *chip = snd_magic_cast(vx_core_t, entry->private_data, return);
	static char *audio_src_vxp[] = { "Line", "Mic", "Digital" };
	static char *audio_src_vx2[] = { "Analog", "Analog", "Digital" };
	static char *clock_src[] = { "Internal", "External" };
	static char *uer_type[] = { "Consumer", "Professional", "Not Present" };
	
	snd_iprintf(buffer, "%s\n", chip->card->longname);
	snd_iprintf(buffer, "DSP audio info:");
	if (chip->audio_info & VX_AUDIO_INFO_REAL_TIME)
		snd_iprintf(buffer, " realtime");
	if (chip->audio_info & VX_AUDIO_INFO_OFFLINE)
		snd_iprintf(buffer, " offline");
	if (chip->audio_info & VX_AUDIO_INFO_MPEG1)
		snd_iprintf(buffer, " mpeg1");
	if (chip->audio_info & VX_AUDIO_INFO_MPEG2)
		snd_iprintf(buffer, " mpeg2");
	if (chip->audio_info & VX_AUDIO_INFO_LINEAR_8)
		snd_iprintf(buffer, " linear8");
	if (chip->audio_info & VX_AUDIO_INFO_LINEAR_16)
		snd_iprintf(buffer, " linear16");
	if (chip->audio_info & VX_AUDIO_INFO_LINEAR_24)
		snd_iprintf(buffer, " linear24");
	snd_iprintf(buffer, "\n");
	snd_iprintf(buffer, "Input Source: %s\n", vx_is_pcmcia(chip) ?
		    audio_src_vxp[chip->audio_source] :
		    audio_src_vx2[chip->audio_source]);
	snd_iprintf(buffer, "Clock Source: %s\n", clock_src[chip->clock_source]);
	snd_iprintf(buffer, "Frequency: %d\n", chip->freq);
	snd_iprintf(buffer, "Detected Frequency: %d\n", chip->freq_detected);
	snd_iprintf(buffer, "Detected UER type: %s\n", uer_type[chip->uer_detected]);
	snd_iprintf(buffer, "Min/Max/Cur IBL: %d/%d/%d (granularity=%d)\n",
		    chip->ibl.min_size, chip->ibl.max_size, chip->ibl.size,
		    chip->ibl.granularity);
}

static void vx_proc_init(vx_core_t *chip)
{
	snd_info_entry_t *entry;

	if (! snd_card_proc_new(chip->card, "vx-status", &entry))
		snd_info_set_text_ops(entry, chip, 1024, vx_proc_read);
}


/**
 * snd_vx_dsp_boot - load the DSP boot
 */
int snd_vx_dsp_boot(vx_core_t *chip, const snd_hwdep_dsp_image_t *boot)
{
	int err;
	int cold_reset = !(chip->chip_status & VX_STAT_DEVICE_INIT);

	vx_reset_board(chip, cold_reset);
	vx_validate_irq(chip, 0);

	if ((err = snd_vx_load_boot_image(chip, boot)) < 0)
		return err;
	snd_vx_delay(chip, 10);

	return 0;
}

/**
 * snd_vx_dsp_load - load the DSP image
 */
int snd_vx_dsp_load(vx_core_t *chip, const snd_hwdep_dsp_image_t *dsp)
{
	unsigned int i;
	int err;
	unsigned int csum = 0;
	unsigned char image[3], *cptr;

	snd_assert(dsp->length % 3 == 0, return -EINVAL);

	vx_toggle_dac_mute(chip, 1);

	/* Transfert data buffer from PC to DSP */
	for (i = 0; i < dsp->length; i += 3) {
		if (copy_from_user(image, dsp->image + i, 3))
			return -EFAULT;
		/* Wait DSP ready for a new read */
		if ((err = vx_wait_isr_bit(chip, ISR_TX_EMPTY)) < 0) {
			printk("dsp loading error at position %d\n", i);
			return err;
		}
		cptr = image;
		csum ^= *cptr;
		csum = (csum >> 24) | (csum << 8);
		vx_outb(chip, TXH, *cptr++);
		csum ^= *cptr;
		csum = (csum >> 24) | (csum << 8);
		vx_outb(chip, TXM, *cptr++);
		csum ^= *cptr;
		csum = (csum >> 24) | (csum << 8);
		vx_outb(chip, TXL, *cptr++);
	}
	snd_printdd(KERN_DEBUG "checksum = 0x%08x\n", csum);

	snd_vx_delay(chip, 200);

	if ((err = vx_wait_isr_bit(chip, ISR_CHK)) < 0)
		return err;

	vx_toggle_dac_mute(chip, 0);

	vx_test_and_ack(chip);
	vx_validate_irq(chip, 1);

	return 0;
}

/**
 * snd_vx_create - constructor for vx_core_t
 * @hw: hardware specific record
 *
 * this function allocates the instance and prepare for the hardware
 * initialization.
 *
 * return the instance pointer if successful, NULL in error.
 */
vx_core_t *snd_vx_create(snd_card_t *card, struct snd_vx_hardware *hw,
			 struct snd_vx_ops *ops,
			 int extra_size)
{
	vx_core_t *chip;

	snd_assert(card && hw && ops, return NULL);

	chip = snd_magic_kcalloc(vx_core_t, extra_size, GFP_KERNEL);
	if (! chip) {
		snd_printk(KERN_ERR "vx_core: no memory\n");
		return NULL;
	}
	spin_lock_init(&chip->lock);
	spin_lock_init(&chip->irq_lock);
	chip->irq = -1;
	chip->hw = hw;
	chip->type = hw->type;
	chip->ops = ops;
	tasklet_init(&chip->tq, vx_interrupt, (unsigned long)chip);
	init_MUTEX(&chip->mixer_mutex);

	chip->card = card;
	card->private_data = chip;
	strcpy(card->driver, hw->name);
	sprintf(card->shortname, "Digigram %s", hw->name);

	vx_proc_init(chip);

	return chip;
}

#ifdef CONFIG_PM
/*
 * suspend
 */
void snd_vx_suspend(vx_core_t *chip)
{
	unsigned int i;

	chip->chip_status |= VX_STAT_IN_SUSPEND;
	for (i = 0; i < chip->hw->num_codecs; i++)
		snd_pcm_suspend_all(chip->pcm[i]);
	if (chip->hwdep)
		chip->hwdep->dsp_loaded = 0;
}

/*
 * resume
 */
void snd_vx_resume(vx_core_t *chip)
{
	/* clear all stuff... */
	chip->chip_status &= ~(VX_STAT_IN_SUSPEND|VX_STAT_CHIP_INIT);
}

#endif

/*
 * module entries
 */
static int __init alsa_vx_core_init(void)
{
	return 0;
}

static void __exit alsa_vx_core_exit(void)
{
}

module_init(alsa_vx_core_init)
module_exit(alsa_vx_core_exit)

/*
 * exports
 */
EXPORT_SYMBOL(snd_vx_check_reg_bit);
EXPORT_SYMBOL(snd_vx_create);
EXPORT_SYMBOL(snd_vx_hwdep_new);
EXPORT_SYMBOL(snd_vx_irq_handler);
EXPORT_SYMBOL(snd_vx_delay);
EXPORT_SYMBOL(snd_vx_dsp_boot);
EXPORT_SYMBOL(snd_vx_dsp_load);
EXPORT_SYMBOL(snd_vx_load_boot_image);
#ifdef CONFIG_PM
EXPORT_SYMBOL(snd_vx_suspend);
EXPORT_SYMBOL(snd_vx_resume);
#endif
