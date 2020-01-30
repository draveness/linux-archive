/*
 * Copyright (C) 2005 Stephen Street / StreetFire Sound Labs
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/delay.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/hardware.h>
#include <asm/delay.h>
#include <asm/dma.h>

#include <asm/arch/hardware.h>
#include <asm/arch/pxa-regs.h>
#include <asm/arch/pxa2xx_spi.h>

MODULE_AUTHOR("Stephen Street");
MODULE_DESCRIPTION("PXA2xx SSP SPI Contoller");
MODULE_LICENSE("GPL");

#define MAX_BUSES 3

#define DMA_INT_MASK (DCSR_ENDINTR | DCSR_STARTINTR | DCSR_BUSERR)
#define RESET_DMA_CHANNEL (DCSR_NODESC | DMA_INT_MASK)
#define IS_DMA_ALIGNED(x) (((u32)(x)&0x07)==0)

/* for testing SSCR1 changes that require SSP restart, basically
 * everything except the service and interrupt enables */
#define SSCR1_CHANGE_MASK (SSCR1_TTELP | SSCR1_TTE | SSCR1_EBCEI | SSCR1_SCFR \
				| SSCR1_ECRA | SSCR1_ECRB | SSCR1_SCLKDIR \
				| SSCR1_RWOT | SSCR1_TRAIL | SSCR1_PINTE \
				| SSCR1_STRF | SSCR1_EFWR |SSCR1_RFT \
				| SSCR1_TFT | SSCR1_SPH | SSCR1_SPO | SSCR1_LBM)

#define DEFINE_SSP_REG(reg, off) \
static inline u32 read_##reg(void *p) { return __raw_readl(p + (off)); } \
static inline void write_##reg(u32 v, void *p) { __raw_writel(v, p + (off)); }

DEFINE_SSP_REG(SSCR0, 0x00)
DEFINE_SSP_REG(SSCR1, 0x04)
DEFINE_SSP_REG(SSSR, 0x08)
DEFINE_SSP_REG(SSITR, 0x0c)
DEFINE_SSP_REG(SSDR, 0x10)
DEFINE_SSP_REG(SSTO, 0x28)
DEFINE_SSP_REG(SSPSP, 0x2c)

#define START_STATE ((void*)0)
#define RUNNING_STATE ((void*)1)
#define DONE_STATE ((void*)2)
#define ERROR_STATE ((void*)-1)

#define QUEUE_RUNNING 0
#define QUEUE_STOPPED 1

struct driver_data {
	/* Driver model hookup */
	struct platform_device *pdev;

	/* SPI framework hookup */
	enum pxa_ssp_type ssp_type;
	struct spi_master *master;

	/* PXA hookup */
	struct pxa2xx_spi_master *master_info;

	/* DMA setup stuff */
	int rx_channel;
	int tx_channel;
	u32 *null_dma_buf;

	/* SSP register addresses */
	void *ioaddr;
	u32 ssdr_physical;

	/* SSP masks*/
	u32 dma_cr1;
	u32 int_cr1;
	u32 clear_sr;
	u32 mask_sr;

	/* Driver message queue */
	struct workqueue_struct	*workqueue;
	struct work_struct pump_messages;
	spinlock_t lock;
	struct list_head queue;
	int busy;
	int run;

	/* Message Transfer pump */
	struct tasklet_struct pump_transfers;

	/* Current message transfer state info */
	struct spi_message* cur_msg;
	struct spi_transfer* cur_transfer;
	struct chip_data *cur_chip;
	size_t len;
	void *tx;
	void *tx_end;
	void *rx;
	void *rx_end;
	int dma_mapped;
	dma_addr_t rx_dma;
	dma_addr_t tx_dma;
	size_t rx_map_len;
	size_t tx_map_len;
	u8 n_bytes;
	u32 dma_width;
	int cs_change;
	int (*write)(struct driver_data *drv_data);
	int (*read)(struct driver_data *drv_data);
	irqreturn_t (*transfer_handler)(struct driver_data *drv_data);
	void (*cs_control)(u32 command);
};

struct chip_data {
	u32 cr0;
	u32 cr1;
	u32 psp;
	u32 timeout;
	u8 n_bytes;
	u32 dma_width;
	u32 dma_burst_size;
	u32 threshold;
	u32 dma_threshold;
	u8 enable_dma;
	u8 bits_per_word;
	u32 speed_hz;
	int (*write)(struct driver_data *drv_data);
	int (*read)(struct driver_data *drv_data);
	void (*cs_control)(u32 command);
};

static void pump_messages(struct work_struct *work);

static int flush(struct driver_data *drv_data)
{
	unsigned long limit = loops_per_jiffy << 1;

	void *reg = drv_data->ioaddr;

	do {
		while (read_SSSR(reg) & SSSR_RNE) {
			read_SSDR(reg);
		}
	} while ((read_SSSR(reg) & SSSR_BSY) && limit--);
	write_SSSR(SSSR_ROR, reg);

	return limit;
}

static void null_cs_control(u32 command)
{
}

static int null_writer(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;
	u8 n_bytes = drv_data->n_bytes;

	if (((read_SSSR(reg) & 0x00000f00) == 0x00000f00)
		|| (drv_data->tx == drv_data->tx_end))
		return 0;

	write_SSDR(0, reg);
	drv_data->tx += n_bytes;

	return 1;
}

static int null_reader(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;
	u8 n_bytes = drv_data->n_bytes;

	while ((read_SSSR(reg) & SSSR_RNE)
		&& (drv_data->rx < drv_data->rx_end)) {
		read_SSDR(reg);
		drv_data->rx += n_bytes;
	}

	return drv_data->rx == drv_data->rx_end;
}

static int u8_writer(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;

	if (((read_SSSR(reg) & 0x00000f00) == 0x00000f00)
		|| (drv_data->tx == drv_data->tx_end))
		return 0;

	write_SSDR(*(u8 *)(drv_data->tx), reg);
	++drv_data->tx;

	return 1;
}

static int u8_reader(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;

	while ((read_SSSR(reg) & SSSR_RNE)
		&& (drv_data->rx < drv_data->rx_end)) {
		*(u8 *)(drv_data->rx) = read_SSDR(reg);
		++drv_data->rx;
	}

	return drv_data->rx == drv_data->rx_end;
}

static int u16_writer(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;

	if (((read_SSSR(reg) & 0x00000f00) == 0x00000f00)
		|| (drv_data->tx == drv_data->tx_end))
		return 0;

	write_SSDR(*(u16 *)(drv_data->tx), reg);
	drv_data->tx += 2;

	return 1;
}

static int u16_reader(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;

	while ((read_SSSR(reg) & SSSR_RNE)
		&& (drv_data->rx < drv_data->rx_end)) {
		*(u16 *)(drv_data->rx) = read_SSDR(reg);
		drv_data->rx += 2;
	}

	return drv_data->rx == drv_data->rx_end;
}

static int u32_writer(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;

	if (((read_SSSR(reg) & 0x00000f00) == 0x00000f00)
		|| (drv_data->tx == drv_data->tx_end))
		return 0;

	write_SSDR(*(u32 *)(drv_data->tx), reg);
	drv_data->tx += 4;

	return 1;
}

static int u32_reader(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;

	while ((read_SSSR(reg) & SSSR_RNE)
		&& (drv_data->rx < drv_data->rx_end)) {
		*(u32 *)(drv_data->rx) = read_SSDR(reg);
		drv_data->rx += 4;
	}

	return drv_data->rx == drv_data->rx_end;
}

static void *next_transfer(struct driver_data *drv_data)
{
	struct spi_message *msg = drv_data->cur_msg;
	struct spi_transfer *trans = drv_data->cur_transfer;

	/* Move to next transfer */
	if (trans->transfer_list.next != &msg->transfers) {
		drv_data->cur_transfer =
			list_entry(trans->transfer_list.next,
					struct spi_transfer,
					transfer_list);
		return RUNNING_STATE;
	} else
		return DONE_STATE;
}

static int map_dma_buffers(struct driver_data *drv_data)
{
	struct spi_message *msg = drv_data->cur_msg;
	struct device *dev = &msg->spi->dev;

	if (!drv_data->cur_chip->enable_dma)
		return 0;

	if (msg->is_dma_mapped)
		return  drv_data->rx_dma && drv_data->tx_dma;

	if (!IS_DMA_ALIGNED(drv_data->rx) || !IS_DMA_ALIGNED(drv_data->tx))
		return 0;

	/* Modify setup if rx buffer is null */
	if (drv_data->rx == NULL) {
		*drv_data->null_dma_buf = 0;
		drv_data->rx = drv_data->null_dma_buf;
		drv_data->rx_map_len = 4;
	} else
		drv_data->rx_map_len = drv_data->len;


	/* Modify setup if tx buffer is null */
	if (drv_data->tx == NULL) {
		*drv_data->null_dma_buf = 0;
		drv_data->tx = drv_data->null_dma_buf;
		drv_data->tx_map_len = 4;
	} else
		drv_data->tx_map_len = drv_data->len;

	/* Stream map the rx buffer */
	drv_data->rx_dma = dma_map_single(dev, drv_data->rx,
						drv_data->rx_map_len,
						DMA_FROM_DEVICE);
	if (dma_mapping_error(drv_data->rx_dma))
		return 0;

	/* Stream map the tx buffer */
	drv_data->tx_dma = dma_map_single(dev, drv_data->tx,
						drv_data->tx_map_len,
						DMA_TO_DEVICE);

	if (dma_mapping_error(drv_data->tx_dma)) {
		dma_unmap_single(dev, drv_data->rx_dma,
					drv_data->rx_map_len, DMA_FROM_DEVICE);
		return 0;
	}

	return 1;
}

static void unmap_dma_buffers(struct driver_data *drv_data)
{
	struct device *dev;

	if (!drv_data->dma_mapped)
		return;

	if (!drv_data->cur_msg->is_dma_mapped) {
		dev = &drv_data->cur_msg->spi->dev;
		dma_unmap_single(dev, drv_data->rx_dma,
					drv_data->rx_map_len, DMA_FROM_DEVICE);
		dma_unmap_single(dev, drv_data->tx_dma,
					drv_data->tx_map_len, DMA_TO_DEVICE);
	}

	drv_data->dma_mapped = 0;
}

/* caller already set message->status; dma and pio irqs are blocked */
static void giveback(struct driver_data *drv_data)
{
	struct spi_transfer* last_transfer;
	unsigned long flags;
	struct spi_message *msg;

	spin_lock_irqsave(&drv_data->lock, flags);
	msg = drv_data->cur_msg;
	drv_data->cur_msg = NULL;
	drv_data->cur_transfer = NULL;
	drv_data->cur_chip = NULL;
	queue_work(drv_data->workqueue, &drv_data->pump_messages);
	spin_unlock_irqrestore(&drv_data->lock, flags);

	last_transfer = list_entry(msg->transfers.prev,
					struct spi_transfer,
					transfer_list);

	if (!last_transfer->cs_change)
		drv_data->cs_control(PXA2XX_CS_DEASSERT);

	msg->state = NULL;
	if (msg->complete)
		msg->complete(msg->context);
}

static int wait_ssp_rx_stall(void *ioaddr)
{
	unsigned long limit = loops_per_jiffy << 1;

	while ((read_SSSR(ioaddr) & SSSR_BSY) && limit--)
		cpu_relax();

	return limit;
}

static int wait_dma_channel_stop(int channel)
{
	unsigned long limit = loops_per_jiffy << 1;

	while (!(DCSR(channel) & DCSR_STOPSTATE) && limit--)
		cpu_relax();

	return limit;
}

void dma_error_stop(struct driver_data *drv_data, const char *msg)
{
	void *reg = drv_data->ioaddr;

	/* Stop and reset */
	DCSR(drv_data->rx_channel) = RESET_DMA_CHANNEL;
	DCSR(drv_data->tx_channel) = RESET_DMA_CHANNEL;
	write_SSSR(drv_data->clear_sr, reg);
	write_SSCR1(read_SSCR1(reg) & ~drv_data->dma_cr1, reg);
	if (drv_data->ssp_type != PXA25x_SSP)
		write_SSTO(0, reg);
	flush(drv_data);
	write_SSCR0(read_SSCR0(reg) & ~SSCR0_SSE, reg);

	unmap_dma_buffers(drv_data);

	dev_err(&drv_data->pdev->dev, "%s\n", msg);

	drv_data->cur_msg->state = ERROR_STATE;
	tasklet_schedule(&drv_data->pump_transfers);
}

static void dma_transfer_complete(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;
	struct spi_message *msg = drv_data->cur_msg;

	/* Clear and disable interrupts on SSP and DMA channels*/
	write_SSCR1(read_SSCR1(reg) & ~drv_data->dma_cr1, reg);
	write_SSSR(drv_data->clear_sr, reg);
	DCSR(drv_data->tx_channel) = RESET_DMA_CHANNEL;
	DCSR(drv_data->rx_channel) = RESET_DMA_CHANNEL;

	if (wait_dma_channel_stop(drv_data->rx_channel) == 0)
		dev_err(&drv_data->pdev->dev,
			"dma_handler: dma rx channel stop failed\n");

	if (wait_ssp_rx_stall(drv_data->ioaddr) == 0)
		dev_err(&drv_data->pdev->dev,
			"dma_transfer: ssp rx stall failed\n");

	unmap_dma_buffers(drv_data);

	/* update the buffer pointer for the amount completed in dma */
	drv_data->rx += drv_data->len -
			(DCMD(drv_data->rx_channel) & DCMD_LENGTH);

	/* read trailing data from fifo, it does not matter how many
	 * bytes are in the fifo just read until buffer is full
	 * or fifo is empty, which ever occurs first */
	drv_data->read(drv_data);

	/* return count of what was actually read */
	msg->actual_length += drv_data->len -
				(drv_data->rx_end - drv_data->rx);

	/* Release chip select if requested, transfer delays are
	 * handled in pump_transfers */
	if (drv_data->cs_change)
		drv_data->cs_control(PXA2XX_CS_DEASSERT);

	/* Move to next transfer */
	msg->state = next_transfer(drv_data);

	/* Schedule transfer tasklet */
	tasklet_schedule(&drv_data->pump_transfers);
}

static void dma_handler(int channel, void *data)
{
	struct driver_data *drv_data = data;
	u32 irq_status = DCSR(channel) & DMA_INT_MASK;

	if (irq_status & DCSR_BUSERR) {

		if (channel == drv_data->tx_channel)
			dma_error_stop(drv_data,
					"dma_handler: "
					"bad bus address on tx channel");
		else
			dma_error_stop(drv_data,
					"dma_handler: "
					"bad bus address on rx channel");
		return;
	}

	/* PXA255x_SSP has no timeout interrupt, wait for tailing bytes */
	if ((channel == drv_data->tx_channel)
		&& (irq_status & DCSR_ENDINTR)
		&& (drv_data->ssp_type == PXA25x_SSP)) {

		/* Wait for rx to stall */
		if (wait_ssp_rx_stall(drv_data->ioaddr) == 0)
			dev_err(&drv_data->pdev->dev,
				"dma_handler: ssp rx stall failed\n");

		/* finish this transfer, start the next */
		dma_transfer_complete(drv_data);
	}
}

static irqreturn_t dma_transfer(struct driver_data *drv_data)
{
	u32 irq_status;
	void *reg = drv_data->ioaddr;

	irq_status = read_SSSR(reg) & drv_data->mask_sr;
	if (irq_status & SSSR_ROR) {
		dma_error_stop(drv_data, "dma_transfer: fifo overrun");
		return IRQ_HANDLED;
	}

	/* Check for false positive timeout */
	if ((irq_status & SSSR_TINT)
		&& (DCSR(drv_data->tx_channel) & DCSR_RUN)) {
		write_SSSR(SSSR_TINT, reg);
		return IRQ_HANDLED;
	}

	if (irq_status & SSSR_TINT || drv_data->rx == drv_data->rx_end) {

		/* Clear and disable timeout interrupt, do the rest in
		 * dma_transfer_complete */
		if (drv_data->ssp_type != PXA25x_SSP)
			write_SSTO(0, reg);

		/* finish this transfer, start the next */
		dma_transfer_complete(drv_data);

		return IRQ_HANDLED;
	}

	/* Opps problem detected */
	return IRQ_NONE;
}

static void int_error_stop(struct driver_data *drv_data, const char* msg)
{
	void *reg = drv_data->ioaddr;

	/* Stop and reset SSP */
	write_SSSR(drv_data->clear_sr, reg);
	write_SSCR1(read_SSCR1(reg) & ~drv_data->int_cr1, reg);
	if (drv_data->ssp_type != PXA25x_SSP)
		write_SSTO(0, reg);
	flush(drv_data);
	write_SSCR0(read_SSCR0(reg) & ~SSCR0_SSE, reg);

	dev_err(&drv_data->pdev->dev, "%s\n", msg);

	drv_data->cur_msg->state = ERROR_STATE;
	tasklet_schedule(&drv_data->pump_transfers);
}

static void int_transfer_complete(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;

	/* Stop SSP */
	write_SSSR(drv_data->clear_sr, reg);
	write_SSCR1(read_SSCR1(reg) & ~drv_data->int_cr1, reg);
	if (drv_data->ssp_type != PXA25x_SSP)
		write_SSTO(0, reg);

	/* Update total byte transfered return count actual bytes read */
	drv_data->cur_msg->actual_length += drv_data->len -
				(drv_data->rx_end - drv_data->rx);

	/* Release chip select if requested, transfer delays are
	 * handled in pump_transfers */
	if (drv_data->cs_change)
		drv_data->cs_control(PXA2XX_CS_DEASSERT);

	/* Move to next transfer */
	drv_data->cur_msg->state = next_transfer(drv_data);

	/* Schedule transfer tasklet */
	tasklet_schedule(&drv_data->pump_transfers);
}

static irqreturn_t interrupt_transfer(struct driver_data *drv_data)
{
	void *reg = drv_data->ioaddr;

	u32 irq_mask = (read_SSCR1(reg) & SSCR1_TIE) ?
			drv_data->mask_sr : drv_data->mask_sr & ~SSSR_TFS;

	u32 irq_status = read_SSSR(reg) & irq_mask;

	if (irq_status & SSSR_ROR) {
		int_error_stop(drv_data, "interrupt_transfer: fifo overrun");
		return IRQ_HANDLED;
	}

	if (irq_status & SSSR_TINT) {
		write_SSSR(SSSR_TINT, reg);
		if (drv_data->read(drv_data)) {
			int_transfer_complete(drv_data);
			return IRQ_HANDLED;
		}
	}

	/* Drain rx fifo, Fill tx fifo and prevent overruns */
	do {
		if (drv_data->read(drv_data)) {
			int_transfer_complete(drv_data);
			return IRQ_HANDLED;
		}
	} while (drv_data->write(drv_data));

	if (drv_data->read(drv_data)) {
		int_transfer_complete(drv_data);
		return IRQ_HANDLED;
	}

	if (drv_data->tx == drv_data->tx_end) {
		write_SSCR1(read_SSCR1(reg) & ~SSCR1_TIE, reg);
		/* PXA25x_SSP has no timeout, read trailing bytes */
		if (drv_data->ssp_type == PXA25x_SSP) {
			if (!wait_ssp_rx_stall(reg))
			{
				int_error_stop(drv_data, "interrupt_transfer: "
						"rx stall failed");
				return IRQ_HANDLED;
			}
			if (!drv_data->read(drv_data))
			{
				int_error_stop(drv_data,
						"interrupt_transfer: "
						"trailing byte read failed");
				return IRQ_HANDLED;
			}
			int_transfer_complete(drv_data);
		}
	}

	/* We did something */
	return IRQ_HANDLED;
}

static irqreturn_t ssp_int(int irq, void *dev_id)
{
	struct driver_data *drv_data = dev_id;
	void *reg = drv_data->ioaddr;

	if (!drv_data->cur_msg) {

		write_SSCR0(read_SSCR0(reg) & ~SSCR0_SSE, reg);
		write_SSCR1(read_SSCR1(reg) & ~drv_data->int_cr1, reg);
		if (drv_data->ssp_type != PXA25x_SSP)
			write_SSTO(0, reg);
		write_SSSR(drv_data->clear_sr, reg);

		dev_err(&drv_data->pdev->dev, "bad message state "
			"in interrupt handler\n");

		/* Never fail */
		return IRQ_HANDLED;
	}

	return drv_data->transfer_handler(drv_data);
}

int set_dma_burst_and_threshold(struct chip_data *chip, struct spi_device *spi,
				u8 bits_per_word, u32 *burst_code,
				u32 *threshold)
{
	struct pxa2xx_spi_chip *chip_info =
			(struct pxa2xx_spi_chip *)spi->controller_data;
	int bytes_per_word;
	int burst_bytes;
	int thresh_words;
	int req_burst_size;
	int retval = 0;

	/* Set the threshold (in registers) to equal the same amount of data
	 * as represented by burst size (in bytes).  The computation below
	 * is (burst_size rounded up to nearest 8 byte, word or long word)
	 * divided by (bytes/register); the tx threshold is the inverse of
	 * the rx, so that there will always be enough data in the rx fifo
	 * to satisfy a burst, and there will always be enough space in the
	 * tx fifo to accept a burst (a tx burst will overwrite the fifo if
	 * there is not enough space), there must always remain enough empty
	 * space in the rx fifo for any data loaded to the tx fifo.
	 * Whenever burst_size (in bytes) equals bits/word, the fifo threshold
	 * will be 8, or half the fifo;
	 * The threshold can only be set to 2, 4 or 8, but not 16, because
	 * to burst 16 to the tx fifo, the fifo would have to be empty;
	 * however, the minimum fifo trigger level is 1, and the tx will
	 * request service when the fifo is at this level, with only 15 spaces.
	 */

	/* find bytes/word */
	if (bits_per_word <= 8)
		bytes_per_word = 1;
	else if (bits_per_word <= 16)
		bytes_per_word = 2;
	else
		bytes_per_word = 4;

	/* use struct pxa2xx_spi_chip->dma_burst_size if available */
	if (chip_info)
		req_burst_size = chip_info->dma_burst_size;
	else {
		switch (chip->dma_burst_size) {
		default:
			/* if the default burst size is not set,
			 * do it now */
			chip->dma_burst_size = DCMD_BURST8;
		case DCMD_BURST8:
			req_burst_size = 8;
			break;
		case DCMD_BURST16:
			req_burst_size = 16;
			break;
		case DCMD_BURST32:
			req_burst_size = 32;
			break;
		}
	}
	if (req_burst_size <= 8) {
		*burst_code = DCMD_BURST8;
		burst_bytes = 8;
	} else if (req_burst_size <= 16) {
		if (bytes_per_word == 1) {
			/* don't burst more than 1/2 the fifo */
			*burst_code = DCMD_BURST8;
			burst_bytes = 8;
			retval = 1;
		} else {
			*burst_code = DCMD_BURST16;
			burst_bytes = 16;
		}
	} else {
		if (bytes_per_word == 1) {
			/* don't burst more than 1/2 the fifo */
			*burst_code = DCMD_BURST8;
			burst_bytes = 8;
			retval = 1;
		} else if (bytes_per_word == 2) {
			/* don't burst more than 1/2 the fifo */
			*burst_code = DCMD_BURST16;
			burst_bytes = 16;
			retval = 1;
		} else {
			*burst_code = DCMD_BURST32;
			burst_bytes = 32;
		}
	}

	thresh_words = burst_bytes / bytes_per_word;

	/* thresh_words will be between 2 and 8 */
	*threshold = (SSCR1_RxTresh(thresh_words) & SSCR1_RFT)
			| (SSCR1_TxTresh(16-thresh_words) & SSCR1_TFT);

	return retval;
}

static void pump_transfers(unsigned long data)
{
	struct driver_data *drv_data = (struct driver_data *)data;
	struct spi_message *message = NULL;
	struct spi_transfer *transfer = NULL;
	struct spi_transfer *previous = NULL;
	struct chip_data *chip = NULL;
	void *reg = drv_data->ioaddr;
	u32 clk_div = 0;
	u8 bits = 0;
	u32 speed = 0;
	u32 cr0;
	u32 cr1;
	u32 dma_thresh = drv_data->cur_chip->dma_threshold;
	u32 dma_burst = drv_data->cur_chip->dma_burst_size;

	/* Get current state information */
	message = drv_data->cur_msg;
	transfer = drv_data->cur_transfer;
	chip = drv_data->cur_chip;

	/* Handle for abort */
	if (message->state == ERROR_STATE) {
		message->status = -EIO;
		giveback(drv_data);
		return;
	}

	/* Handle end of message */
	if (message->state == DONE_STATE) {
		message->status = 0;
		giveback(drv_data);
		return;
	}

	/* Delay if requested at end of transfer*/
	if (message->state == RUNNING_STATE) {
		previous = list_entry(transfer->transfer_list.prev,
					struct spi_transfer,
					transfer_list);
		if (previous->delay_usecs)
			udelay(previous->delay_usecs);
	}

	/* Check transfer length */
	if (transfer->len > 8191)
	{
		dev_warn(&drv_data->pdev->dev, "pump_transfers: transfer "
				"length greater than 8191\n");
		message->status = -EINVAL;
		giveback(drv_data);
		return;
	}

	/* Setup the transfer state based on the type of transfer */
	if (flush(drv_data) == 0) {
		dev_err(&drv_data->pdev->dev, "pump_transfers: flush failed\n");
		message->status = -EIO;
		giveback(drv_data);
		return;
	}
	drv_data->n_bytes = chip->n_bytes;
	drv_data->dma_width = chip->dma_width;
	drv_data->cs_control = chip->cs_control;
	drv_data->tx = (void *)transfer->tx_buf;
	drv_data->tx_end = drv_data->tx + transfer->len;
	drv_data->rx = transfer->rx_buf;
	drv_data->rx_end = drv_data->rx + transfer->len;
	drv_data->rx_dma = transfer->rx_dma;
	drv_data->tx_dma = transfer->tx_dma;
	drv_data->len = transfer->len & DCMD_LENGTH;
	drv_data->write = drv_data->tx ? chip->write : null_writer;
	drv_data->read = drv_data->rx ? chip->read : null_reader;
	drv_data->cs_change = transfer->cs_change;

	/* Change speed and bit per word on a per transfer */
	cr0 = chip->cr0;
	if (transfer->speed_hz || transfer->bits_per_word) {

		bits = chip->bits_per_word;
		speed = chip->speed_hz;

		if (transfer->speed_hz)
			speed = transfer->speed_hz;

		if (transfer->bits_per_word)
			bits = transfer->bits_per_word;

		if (reg == SSP1_VIRT)
			clk_div = SSP1_SerClkDiv(speed);
		else if (reg == SSP2_VIRT)
			clk_div = SSP2_SerClkDiv(speed);
		else if (reg == SSP3_VIRT)
			clk_div = SSP3_SerClkDiv(speed);

		if (bits <= 8) {
			drv_data->n_bytes = 1;
			drv_data->dma_width = DCMD_WIDTH1;
			drv_data->read = drv_data->read != null_reader ?
						u8_reader : null_reader;
			drv_data->write = drv_data->write != null_writer ?
						u8_writer : null_writer;
		} else if (bits <= 16) {
			drv_data->n_bytes = 2;
			drv_data->dma_width = DCMD_WIDTH2;
			drv_data->read = drv_data->read != null_reader ?
						u16_reader : null_reader;
			drv_data->write = drv_data->write != null_writer ?
						u16_writer : null_writer;
		} else if (bits <= 32) {
			drv_data->n_bytes = 4;
			drv_data->dma_width = DCMD_WIDTH4;
			drv_data->read = drv_data->read != null_reader ?
						u32_reader : null_reader;
			drv_data->write = drv_data->write != null_writer ?
						u32_writer : null_writer;
		}
		/* if bits/word is changed in dma mode, then must check the
		 * thresholds and burst also */
		if (chip->enable_dma) {
			if (set_dma_burst_and_threshold(chip, message->spi,
							bits, &dma_burst,
							&dma_thresh))
				if (printk_ratelimit())
					dev_warn(&message->spi->dev,
						"pump_transfer: "
						"DMA burst size reduced to "
						"match bits_per_word\n");
		}

		cr0 = clk_div
			| SSCR0_Motorola
			| SSCR0_DataSize(bits > 16 ? bits - 16 : bits)
			| SSCR0_SSE
			| (bits > 16 ? SSCR0_EDSS : 0);
	}

	message->state = RUNNING_STATE;

	/* Try to map dma buffer and do a dma transfer if successful */
	if ((drv_data->dma_mapped = map_dma_buffers(drv_data))) {

		/* Ensure we have the correct interrupt handler */
		drv_data->transfer_handler = dma_transfer;

		/* Setup rx DMA Channel */
		DCSR(drv_data->rx_channel) = RESET_DMA_CHANNEL;
		DSADR(drv_data->rx_channel) = drv_data->ssdr_physical;
		DTADR(drv_data->rx_channel) = drv_data->rx_dma;
		if (drv_data->rx == drv_data->null_dma_buf)
			/* No target address increment */
			DCMD(drv_data->rx_channel) = DCMD_FLOWSRC
							| drv_data->dma_width
							| dma_burst
							| drv_data->len;
		else
			DCMD(drv_data->rx_channel) = DCMD_INCTRGADDR
							| DCMD_FLOWSRC
							| drv_data->dma_width
							| dma_burst
							| drv_data->len;

		/* Setup tx DMA Channel */
		DCSR(drv_data->tx_channel) = RESET_DMA_CHANNEL;
		DSADR(drv_data->tx_channel) = drv_data->tx_dma;
		DTADR(drv_data->tx_channel) = drv_data->ssdr_physical;
		if (drv_data->tx == drv_data->null_dma_buf)
			/* No source address increment */
			DCMD(drv_data->tx_channel) = DCMD_FLOWTRG
							| drv_data->dma_width
							| dma_burst
							| drv_data->len;
		else
			DCMD(drv_data->tx_channel) = DCMD_INCSRCADDR
							| DCMD_FLOWTRG
							| drv_data->dma_width
							| dma_burst
							| drv_data->len;

		/* Enable dma end irqs on SSP to detect end of transfer */
		if (drv_data->ssp_type == PXA25x_SSP)
			DCMD(drv_data->tx_channel) |= DCMD_ENDIRQEN;

		/* Fix me, need to handle cs polarity */
		drv_data->cs_control(PXA2XX_CS_ASSERT);

		/* Clear status and start DMA engine */
		cr1 = chip->cr1 | dma_thresh | drv_data->dma_cr1;
		write_SSSR(drv_data->clear_sr, reg);
		DCSR(drv_data->rx_channel) |= DCSR_RUN;
		DCSR(drv_data->tx_channel) |= DCSR_RUN;
	} else {
		/* Ensure we have the correct interrupt handler	*/
		drv_data->transfer_handler = interrupt_transfer;

		/* Fix me, need to handle cs polarity */
		drv_data->cs_control(PXA2XX_CS_ASSERT);

		/* Clear status  */
		cr1 = chip->cr1 | chip->threshold | drv_data->int_cr1;
		write_SSSR(drv_data->clear_sr, reg);
	}

	/* see if we need to reload the config registers */
	if ((read_SSCR0(reg) != cr0)
		|| (read_SSCR1(reg) & SSCR1_CHANGE_MASK) !=
			(cr1 & SSCR1_CHANGE_MASK)) {

		write_SSCR0(cr0 & ~SSCR0_SSE, reg);
		if (drv_data->ssp_type != PXA25x_SSP)
			write_SSTO(chip->timeout, reg);
		write_SSCR1(cr1, reg);
		write_SSCR0(cr0, reg);
	} else {
		if (drv_data->ssp_type != PXA25x_SSP)
			write_SSTO(chip->timeout, reg);
		write_SSCR1(cr1, reg);
	}
}

static void pump_messages(struct work_struct *work)
{
	struct driver_data *drv_data =
		container_of(work, struct driver_data, pump_messages);
	unsigned long flags;

	/* Lock queue and check for queue work */
	spin_lock_irqsave(&drv_data->lock, flags);
	if (list_empty(&drv_data->queue) || drv_data->run == QUEUE_STOPPED) {
		drv_data->busy = 0;
		spin_unlock_irqrestore(&drv_data->lock, flags);
		return;
	}

	/* Make sure we are not already running a message */
	if (drv_data->cur_msg) {
		spin_unlock_irqrestore(&drv_data->lock, flags);
		return;
	}

	/* Extract head of queue */
	drv_data->cur_msg = list_entry(drv_data->queue.next,
					struct spi_message, queue);
	list_del_init(&drv_data->cur_msg->queue);

	/* Initial message state*/
	drv_data->cur_msg->state = START_STATE;
	drv_data->cur_transfer = list_entry(drv_data->cur_msg->transfers.next,
						struct spi_transfer,
						transfer_list);

	/* prepare to setup the SSP, in pump_transfers, using the per
	 * chip configuration */
	drv_data->cur_chip = spi_get_ctldata(drv_data->cur_msg->spi);

	/* Mark as busy and launch transfers */
	tasklet_schedule(&drv_data->pump_transfers);

	drv_data->busy = 1;
	spin_unlock_irqrestore(&drv_data->lock, flags);
}

static int transfer(struct spi_device *spi, struct spi_message *msg)
{
	struct driver_data *drv_data = spi_master_get_devdata(spi->master);
	unsigned long flags;

	spin_lock_irqsave(&drv_data->lock, flags);

	if (drv_data->run == QUEUE_STOPPED) {
		spin_unlock_irqrestore(&drv_data->lock, flags);
		return -ESHUTDOWN;
	}

	msg->actual_length = 0;
	msg->status = -EINPROGRESS;
	msg->state = START_STATE;

	list_add_tail(&msg->queue, &drv_data->queue);

	if (drv_data->run == QUEUE_RUNNING && !drv_data->busy)
		queue_work(drv_data->workqueue, &drv_data->pump_messages);

	spin_unlock_irqrestore(&drv_data->lock, flags);

	return 0;
}

/* the spi->mode bits understood by this driver: */
#define MODEBITS (SPI_CPOL | SPI_CPHA)

static int setup(struct spi_device *spi)
{
	struct pxa2xx_spi_chip *chip_info = NULL;
	struct chip_data *chip;
	struct driver_data *drv_data = spi_master_get_devdata(spi->master);
	unsigned int clk_div;

	if (!spi->bits_per_word)
		spi->bits_per_word = 8;

	if (drv_data->ssp_type != PXA25x_SSP
		&& (spi->bits_per_word < 4 || spi->bits_per_word > 32)) {
		dev_err(&spi->dev, "failed setup: ssp_type=%d, bits/wrd=%d "
				"b/w not 4-32 for type non-PXA25x_SSP\n",
				drv_data->ssp_type, spi->bits_per_word);
		return -EINVAL;
	}
	else if (drv_data->ssp_type == PXA25x_SSP
			&& (spi->bits_per_word < 4
				|| spi->bits_per_word > 16)) {
		dev_err(&spi->dev, "failed setup: ssp_type=%d, bits/wrd=%d "
				"b/w not 4-16 for type PXA25x_SSP\n",
				drv_data->ssp_type, spi->bits_per_word);
		return -EINVAL;
	}

	if (spi->mode & ~MODEBITS) {
		dev_dbg(&spi->dev, "setup: unsupported mode bits %x\n",
			spi->mode & ~MODEBITS);
		return -EINVAL;
	}

	/* Only alloc on first setup */
	chip = spi_get_ctldata(spi);
	if (!chip) {
		chip = kzalloc(sizeof(struct chip_data), GFP_KERNEL);
		if (!chip) {
			dev_err(&spi->dev,
				"failed setup: can't allocate chip data\n");
			return -ENOMEM;
		}

		chip->cs_control = null_cs_control;
		chip->enable_dma = 0;
		chip->timeout = 1000;
		chip->threshold = SSCR1_RxTresh(1) | SSCR1_TxTresh(1);
		chip->dma_burst_size = drv_data->master_info->enable_dma ?
					DCMD_BURST8 : 0;
	}

	/* protocol drivers may change the chip settings, so...
	 * if chip_info exists, use it */
	chip_info = spi->controller_data;

	/* chip_info isn't always needed */
	chip->cr1 = 0;
	if (chip_info) {
		if (chip_info->cs_control)
			chip->cs_control = chip_info->cs_control;

		chip->timeout = chip_info->timeout;

		chip->threshold = (SSCR1_RxTresh(chip_info->rx_threshold) &
								SSCR1_RFT) |
				(SSCR1_TxTresh(chip_info->tx_threshold) &
								SSCR1_TFT);

		chip->enable_dma = chip_info->dma_burst_size != 0
					&& drv_data->master_info->enable_dma;
		chip->dma_threshold = 0;

		if (chip_info->enable_loopback)
			chip->cr1 = SSCR1_LBM;
	}

	/* set dma burst and threshold outside of chip_info path so that if
	 * chip_info goes away after setting chip->enable_dma, the
	 * burst and threshold can still respond to changes in bits_per_word */
	if (chip->enable_dma) {
		/* set up legal burst and threshold for dma */
		if (set_dma_burst_and_threshold(chip, spi, spi->bits_per_word,
						&chip->dma_burst_size,
						&chip->dma_threshold)) {
			dev_warn(&spi->dev, "in setup: DMA burst size reduced "
					"to match bits_per_word\n");
		}
	}

	if (drv_data->ioaddr == SSP1_VIRT)
		clk_div = SSP1_SerClkDiv(spi->max_speed_hz);
	else if (drv_data->ioaddr == SSP2_VIRT)
		clk_div = SSP2_SerClkDiv(spi->max_speed_hz);
	else if (drv_data->ioaddr == SSP3_VIRT)
		clk_div = SSP3_SerClkDiv(spi->max_speed_hz);
	else
	{
		dev_err(&spi->dev, "failed setup: unknown IO address=0x%p\n",
			drv_data->ioaddr);
		return -ENODEV;
	}
	chip->speed_hz = spi->max_speed_hz;

	chip->cr0 = clk_div
			| SSCR0_Motorola
			| SSCR0_DataSize(spi->bits_per_word > 16 ?
				spi->bits_per_word - 16 : spi->bits_per_word)
			| SSCR0_SSE
			| (spi->bits_per_word > 16 ? SSCR0_EDSS : 0);
	chip->cr1 &= ~(SSCR1_SPO | SSCR1_SPH);
	chip->cr1 |= (((spi->mode & SPI_CPHA) != 0) ? SSCR1_SPH : 0)
			| (((spi->mode & SPI_CPOL) != 0) ? SSCR1_SPO : 0);

	/* NOTE:  PXA25x_SSP _could_ use external clocking ... */
	if (drv_data->ssp_type != PXA25x_SSP)
		dev_dbg(&spi->dev, "%d bits/word, %d Hz, mode %d\n",
				spi->bits_per_word,
				(CLOCK_SPEED_HZ)
					/ (1 + ((chip->cr0 & SSCR0_SCR) >> 8)),
				spi->mode & 0x3);
	else
		dev_dbg(&spi->dev, "%d bits/word, %d Hz, mode %d\n",
				spi->bits_per_word,
				(CLOCK_SPEED_HZ/2)
					/ (1 + ((chip->cr0 & SSCR0_SCR) >> 8)),
				spi->mode & 0x3);

	if (spi->bits_per_word <= 8) {
		chip->n_bytes = 1;
		chip->dma_width = DCMD_WIDTH1;
		chip->read = u8_reader;
		chip->write = u8_writer;
	} else if (spi->bits_per_word <= 16) {
		chip->n_bytes = 2;
		chip->dma_width = DCMD_WIDTH2;
		chip->read = u16_reader;
		chip->write = u16_writer;
	} else if (spi->bits_per_word <= 32) {
		chip->cr0 |= SSCR0_EDSS;
		chip->n_bytes = 4;
		chip->dma_width = DCMD_WIDTH4;
		chip->read = u32_reader;
		chip->write = u32_writer;
	} else {
		dev_err(&spi->dev, "invalid wordsize\n");
		return -ENODEV;
	}
	chip->bits_per_word = spi->bits_per_word;

	spi_set_ctldata(spi, chip);

	return 0;
}

static void cleanup(struct spi_device *spi)
{
	struct chip_data *chip = spi_get_ctldata(spi);

	kfree(chip);
}

static int init_queue(struct driver_data *drv_data)
{
	INIT_LIST_HEAD(&drv_data->queue);
	spin_lock_init(&drv_data->lock);

	drv_data->run = QUEUE_STOPPED;
	drv_data->busy = 0;

	tasklet_init(&drv_data->pump_transfers,
			pump_transfers,	(unsigned long)drv_data);

	INIT_WORK(&drv_data->pump_messages, pump_messages);
	drv_data->workqueue = create_singlethread_workqueue(
					drv_data->master->cdev.dev->bus_id);
	if (drv_data->workqueue == NULL)
		return -EBUSY;

	return 0;
}

static int start_queue(struct driver_data *drv_data)
{
	unsigned long flags;

	spin_lock_irqsave(&drv_data->lock, flags);

	if (drv_data->run == QUEUE_RUNNING || drv_data->busy) {
		spin_unlock_irqrestore(&drv_data->lock, flags);
		return -EBUSY;
	}

	drv_data->run = QUEUE_RUNNING;
	drv_data->cur_msg = NULL;
	drv_data->cur_transfer = NULL;
	drv_data->cur_chip = NULL;
	spin_unlock_irqrestore(&drv_data->lock, flags);

	queue_work(drv_data->workqueue, &drv_data->pump_messages);

	return 0;
}

static int stop_queue(struct driver_data *drv_data)
{
	unsigned long flags;
	unsigned limit = 500;
	int status = 0;

	spin_lock_irqsave(&drv_data->lock, flags);

	/* This is a bit lame, but is optimized for the common execution path.
	 * A wait_queue on the drv_data->busy could be used, but then the common
	 * execution path (pump_messages) would be required to call wake_up or
	 * friends on every SPI message. Do this instead */
	drv_data->run = QUEUE_STOPPED;
	while (!list_empty(&drv_data->queue) && drv_data->busy && limit--) {
		spin_unlock_irqrestore(&drv_data->lock, flags);
		msleep(10);
		spin_lock_irqsave(&drv_data->lock, flags);
	}

	if (!list_empty(&drv_data->queue) || drv_data->busy)
		status = -EBUSY;

	spin_unlock_irqrestore(&drv_data->lock, flags);

	return status;
}

static int destroy_queue(struct driver_data *drv_data)
{
	int status;

	status = stop_queue(drv_data);
	/* we are unloading the module or failing to load (only two calls
	 * to this routine), and neither call can handle a return value.
	 * However, destroy_workqueue calls flush_workqueue, and that will
	 * block until all work is done.  If the reason that stop_queue
	 * timed out is that the work will never finish, then it does no
	 * good to call destroy_workqueue, so return anyway. */
	if (status != 0)
		return status;

	destroy_workqueue(drv_data->workqueue);

	return 0;
}

static int pxa2xx_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pxa2xx_spi_master *platform_info;
	struct spi_master *master;
	struct driver_data *drv_data = 0;
	struct resource *memory_resource;
	int irq;
	int status = 0;

	platform_info = dev->platform_data;

	if (platform_info->ssp_type == SSP_UNDEFINED) {
		dev_err(&pdev->dev, "undefined SSP\n");
		return -ENODEV;
	}

	/* Allocate master with space for drv_data and null dma buffer */
	master = spi_alloc_master(dev, sizeof(struct driver_data) + 16);
	if (!master) {
		dev_err(&pdev->dev, "can not alloc spi_master\n");
		return -ENOMEM;
	}
	drv_data = spi_master_get_devdata(master);
	drv_data->master = master;
	drv_data->master_info = platform_info;
	drv_data->pdev = pdev;

	master->bus_num = pdev->id;
	master->num_chipselect = platform_info->num_chipselect;
	master->cleanup = cleanup;
	master->setup = setup;
	master->transfer = transfer;

	drv_data->ssp_type = platform_info->ssp_type;
	drv_data->null_dma_buf = (u32 *)ALIGN((u32)(drv_data +
						sizeof(struct driver_data)), 8);

	/* Setup register addresses */
	memory_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!memory_resource) {
		dev_err(&pdev->dev, "memory resources not defined\n");
		status = -ENODEV;
		goto out_error_master_alloc;
	}

	drv_data->ioaddr = (void *)io_p2v((unsigned long)(memory_resource->start));
	drv_data->ssdr_physical = memory_resource->start + 0x00000010;
	if (platform_info->ssp_type == PXA25x_SSP) {
		drv_data->int_cr1 = SSCR1_TIE | SSCR1_RIE;
		drv_data->dma_cr1 = 0;
		drv_data->clear_sr = SSSR_ROR;
		drv_data->mask_sr = SSSR_RFS | SSSR_TFS | SSSR_ROR;
	} else {
		drv_data->int_cr1 = SSCR1_TIE | SSCR1_RIE | SSCR1_TINTE;
		drv_data->dma_cr1 = SSCR1_TSRE | SSCR1_RSRE | SSCR1_TINTE;
		drv_data->clear_sr = SSSR_ROR | SSSR_TINT;
		drv_data->mask_sr = SSSR_TINT | SSSR_RFS | SSSR_TFS | SSSR_ROR;
	}

	/* Attach to IRQ */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "irq resource not defined\n");
		status = -ENODEV;
		goto out_error_master_alloc;
	}

	status = request_irq(irq, ssp_int, 0, dev->bus_id, drv_data);
	if (status < 0) {
		dev_err(&pdev->dev, "can not get IRQ\n");
		goto out_error_master_alloc;
	}

	/* Setup DMA if requested */
	drv_data->tx_channel = -1;
	drv_data->rx_channel = -1;
	if (platform_info->enable_dma) {

		/* Get two DMA channels	(rx and tx) */
		drv_data->rx_channel = pxa_request_dma("pxa2xx_spi_ssp_rx",
							DMA_PRIO_HIGH,
							dma_handler,
							drv_data);
		if (drv_data->rx_channel < 0) {
			dev_err(dev, "problem (%d) requesting rx channel\n",
				drv_data->rx_channel);
			status = -ENODEV;
			goto out_error_irq_alloc;
		}
		drv_data->tx_channel = pxa_request_dma("pxa2xx_spi_ssp_tx",
							DMA_PRIO_MEDIUM,
							dma_handler,
							drv_data);
		if (drv_data->tx_channel < 0) {
			dev_err(dev, "problem (%d) requesting tx channel\n",
				drv_data->tx_channel);
			status = -ENODEV;
			goto out_error_dma_alloc;
		}

		if (drv_data->ioaddr == SSP1_VIRT) {
				DRCMRRXSSDR = DRCMR_MAPVLD
						| drv_data->rx_channel;
				DRCMRTXSSDR = DRCMR_MAPVLD
						| drv_data->tx_channel;
		} else if (drv_data->ioaddr == SSP2_VIRT) {
				DRCMRRXSS2DR = DRCMR_MAPVLD
						| drv_data->rx_channel;
				DRCMRTXSS2DR = DRCMR_MAPVLD
						| drv_data->tx_channel;
		} else if (drv_data->ioaddr == SSP3_VIRT) {
				DRCMRRXSS3DR = DRCMR_MAPVLD
						| drv_data->rx_channel;
				DRCMRTXSS3DR = DRCMR_MAPVLD
						| drv_data->tx_channel;
		} else {
			dev_err(dev, "bad SSP type\n");
			goto out_error_dma_alloc;
		}
	}

	/* Enable SOC clock */
	pxa_set_cken(platform_info->clock_enable, 1);

	/* Load default SSP configuration */
	write_SSCR0(0, drv_data->ioaddr);
	write_SSCR1(SSCR1_RxTresh(4) | SSCR1_TxTresh(12), drv_data->ioaddr);
	write_SSCR0(SSCR0_SerClkDiv(2)
			| SSCR0_Motorola
			| SSCR0_DataSize(8),
			drv_data->ioaddr);
	if (drv_data->ssp_type != PXA25x_SSP)
		write_SSTO(0, drv_data->ioaddr);
	write_SSPSP(0, drv_data->ioaddr);

	/* Initial and start queue */
	status = init_queue(drv_data);
	if (status != 0) {
		dev_err(&pdev->dev, "problem initializing queue\n");
		goto out_error_clock_enabled;
	}
	status = start_queue(drv_data);
	if (status != 0) {
		dev_err(&pdev->dev, "problem starting queue\n");
		goto out_error_clock_enabled;
	}

	/* Register with the SPI framework */
	platform_set_drvdata(pdev, drv_data);
	status = spi_register_master(master);
	if (status != 0) {
		dev_err(&pdev->dev, "problem registering spi master\n");
		goto out_error_queue_alloc;
	}

	return status;

out_error_queue_alloc:
	destroy_queue(drv_data);

out_error_clock_enabled:
	pxa_set_cken(platform_info->clock_enable, 0);

out_error_dma_alloc:
	if (drv_data->tx_channel != -1)
		pxa_free_dma(drv_data->tx_channel);
	if (drv_data->rx_channel != -1)
		pxa_free_dma(drv_data->rx_channel);

out_error_irq_alloc:
	free_irq(irq, drv_data);

out_error_master_alloc:
	spi_master_put(master);
	return status;
}

static int pxa2xx_spi_remove(struct platform_device *pdev)
{
	struct driver_data *drv_data = platform_get_drvdata(pdev);
	int irq;
	int status = 0;

	if (!drv_data)
		return 0;

	/* Remove the queue */
	status = destroy_queue(drv_data);
	if (status != 0)
		/* the kernel does not check the return status of this
		 * this routine (mod->exit, within the kernel).  Therefore
		 * nothing is gained by returning from here, the module is
		 * going away regardless, and we should not leave any more
		 * resources allocated than necessary.  We cannot free the
		 * message memory in drv_data->queue, but we can release the
		 * resources below.  I think the kernel should honor -EBUSY
		 * returns but... */
		dev_err(&pdev->dev, "pxa2xx_spi_remove: workqueue will not "
			"complete, message memory not freed\n");

	/* Disable the SSP at the peripheral and SOC level */
	write_SSCR0(0, drv_data->ioaddr);
	pxa_set_cken(drv_data->master_info->clock_enable, 0);

	/* Release DMA */
	if (drv_data->master_info->enable_dma) {
		if (drv_data->ioaddr == SSP1_VIRT) {
			DRCMRRXSSDR = 0;
			DRCMRTXSSDR = 0;
		} else if (drv_data->ioaddr == SSP2_VIRT) {
			DRCMRRXSS2DR = 0;
			DRCMRTXSS2DR = 0;
		} else if (drv_data->ioaddr == SSP3_VIRT) {
			DRCMRRXSS3DR = 0;
			DRCMRTXSS3DR = 0;
		}
		pxa_free_dma(drv_data->tx_channel);
		pxa_free_dma(drv_data->rx_channel);
	}

	/* Release IRQ */
	irq = platform_get_irq(pdev, 0);
	if (irq >= 0)
		free_irq(irq, drv_data);

	/* Disconnect from the SPI framework */
	spi_unregister_master(drv_data->master);

	/* Prevent double remove */
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static void pxa2xx_spi_shutdown(struct platform_device *pdev)
{
	int status = 0;

	if ((status = pxa2xx_spi_remove(pdev)) != 0)
		dev_err(&pdev->dev, "shutdown failed with %d\n", status);
}

#ifdef CONFIG_PM
static int suspend_devices(struct device *dev, void *pm_message)
{
	pm_message_t *state = pm_message;

	if (dev->power.power_state.event != state->event) {
		dev_warn(dev, "pm state does not match request\n");
		return -1;
	}

	return 0;
}

static int pxa2xx_spi_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct driver_data *drv_data = platform_get_drvdata(pdev);
	int status = 0;

	/* Check all childern for current power state */
	if (device_for_each_child(&pdev->dev, &state, suspend_devices) != 0) {
		dev_warn(&pdev->dev, "suspend aborted\n");
		return -1;
	}

	status = stop_queue(drv_data);
	if (status != 0)
		return status;
	write_SSCR0(0, drv_data->ioaddr);
	pxa_set_cken(drv_data->master_info->clock_enable, 0);

	return 0;
}

static int pxa2xx_spi_resume(struct platform_device *pdev)
{
	struct driver_data *drv_data = platform_get_drvdata(pdev);
	int status = 0;

	/* Enable the SSP clock */
	pxa_set_cken(drv_data->master_info->clock_enable, 1);

	/* Start the queue running */
	status = start_queue(drv_data);
	if (status != 0) {
		dev_err(&pdev->dev, "problem starting queue (%d)\n", status);
		return status;
	}

	return 0;
}
#else
#define pxa2xx_spi_suspend NULL
#define pxa2xx_spi_resume NULL
#endif /* CONFIG_PM */

static struct platform_driver driver = {
	.driver = {
		.name = "pxa2xx-spi",
		.bus = &platform_bus_type,
		.owner = THIS_MODULE,
	},
	.probe = pxa2xx_spi_probe,
	.remove = __devexit_p(pxa2xx_spi_remove),
	.shutdown = pxa2xx_spi_shutdown,
	.suspend = pxa2xx_spi_suspend,
	.resume = pxa2xx_spi_resume,
};

static int __init pxa2xx_spi_init(void)
{
	platform_driver_register(&driver);

	return 0;
}
module_init(pxa2xx_spi_init);

static void __exit pxa2xx_spi_exit(void)
{
	platform_driver_unregister(&driver);
}
module_exit(pxa2xx_spi_exit);
