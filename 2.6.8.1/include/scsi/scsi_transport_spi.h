/* 
 *  Parallel SCSI (SPI) transport specific attributes exported to sysfs.
 *
 *  Copyright (c) 2003 Silicon Graphics, Inc.  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#ifndef SCSI_TRANSPORT_SPI_H
#define SCSI_TRANSPORT_SPI_H

#include <linux/config.h>

struct scsi_transport_template;

struct spi_transport_attrs {
	int period;		/* value in the PPR/SDTR command */
	int offset;
	unsigned int width:1;	/* 0 - narrow, 1 - wide */
	unsigned int iu:1;	/* Information Units enabled */
	unsigned int dt:1;	/* DT clocking enabled */
	unsigned int qas:1;	/* Quick Arbitration and Selection enabled */
	unsigned int wr_flow:1;	/* Write Flow control enabled */
	unsigned int rd_strm:1;	/* Read streaming enabled */
	unsigned int rti:1;	/* Retain Training Information */
	unsigned int pcomp_en:1;/* Precompensation enabled */
	/* Private Fields */
	unsigned int dv_pending:1; /* Internal flag */
	struct semaphore dv_sem; /* semaphore to serialise dv */
};

/* accessor functions */
#define spi_period(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->period)
#define spi_offset(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->offset)
#define spi_width(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->width)
#define spi_iu(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->iu)
#define spi_dt(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->dt)
#define spi_qas(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->qas)
#define spi_wr_flow(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->wr_flow)
#define spi_rd_strm(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->rd_strm)
#define spi_rti(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->rti)
#define spi_pcomp_en(x)	(((struct spi_transport_attrs *)&(x)->transport_data)->pcomp_en)

/* The functions by which the transport class and the driver communicate */
struct spi_function_template {
	void	(*get_period)(struct scsi_device *);
	void	(*set_period)(struct scsi_device *, int);
	void	(*get_offset)(struct scsi_device *);
	void	(*set_offset)(struct scsi_device *, int);
	void	(*get_width)(struct scsi_device *);
	void	(*set_width)(struct scsi_device *, int);
	void	(*get_iu)(struct scsi_device *);
	void	(*set_iu)(struct scsi_device *, int);
	void	(*get_dt)(struct scsi_device *);
	void	(*set_dt)(struct scsi_device *, int);
	void	(*get_qas)(struct scsi_device *);
	void	(*set_qas)(struct scsi_device *, int);
	void	(*get_wr_flow)(struct scsi_device *);
	void	(*set_wr_flow)(struct scsi_device *, int);
	void	(*get_rd_strm)(struct scsi_device *);
	void	(*set_rd_strm)(struct scsi_device *, int);
	void	(*get_rti)(struct scsi_device *);
	void	(*set_rti)(struct scsi_device *, int);
	void	(*get_pcomp_en)(struct scsi_device *);
	void	(*set_pcomp_en)(struct scsi_device *, int);
	/* The driver sets these to tell the transport class it
	 * wants the attributes displayed in sysfs.  If the show_ flag
	 * is not set, the attribute will be private to the transport
	 * class */
	unsigned long	show_period:1;
	unsigned long	show_offset:1;
	unsigned long	show_width:1;
	unsigned long	show_iu:1;
	unsigned long	show_dt:1;
	unsigned long	show_qas:1;
	unsigned long	show_wr_flow:1;
	unsigned long	show_rd_strm:1;
	unsigned long	show_rti:1;
	unsigned long	show_pcomp_en:1;
};

struct scsi_transport_template *spi_attach_transport(struct spi_function_template *);
void spi_release_transport(struct scsi_transport_template *);
void spi_schedule_dv_device(struct scsi_device *);
void spi_dv_device(struct scsi_device *);

#endif /* SCSI_TRANSPORT_SPI_H */
