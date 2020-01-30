/*
 * Copyright (c) 2006, 2007 Cisco Systems, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef MLX4_DEVICE_H
#define MLX4_DEVICE_H

#include <linux/pci.h>
#include <linux/completion.h>
#include <linux/radix-tree.h>

#include <asm/atomic.h>

enum {
	MLX4_FLAG_MSI_X		= 1 << 0,
	MLX4_FLAG_OLD_PORT_CMDS	= 1 << 1,
};

enum {
	MLX4_MAX_PORTS		= 2
};

enum {
	MLX4_DEV_CAP_FLAG_RC		= 1 <<  0,
	MLX4_DEV_CAP_FLAG_UC		= 1 <<  1,
	MLX4_DEV_CAP_FLAG_UD		= 1 <<  2,
	MLX4_DEV_CAP_FLAG_SRQ		= 1 <<  6,
	MLX4_DEV_CAP_FLAG_IPOIB_CSUM	= 1 <<  7,
	MLX4_DEV_CAP_FLAG_BAD_PKEY_CNTR	= 1 <<  8,
	MLX4_DEV_CAP_FLAG_BAD_QKEY_CNTR	= 1 <<  9,
	MLX4_DEV_CAP_FLAG_MEM_WINDOW	= 1 << 16,
	MLX4_DEV_CAP_FLAG_APM		= 1 << 17,
	MLX4_DEV_CAP_FLAG_ATOMIC	= 1 << 18,
	MLX4_DEV_CAP_FLAG_RAW_MCAST	= 1 << 19,
	MLX4_DEV_CAP_FLAG_UD_AV_PORT	= 1 << 20,
	MLX4_DEV_CAP_FLAG_UD_MCAST	= 1 << 21
};

enum mlx4_event {
	MLX4_EVENT_TYPE_COMP		   = 0x00,
	MLX4_EVENT_TYPE_PATH_MIG	   = 0x01,
	MLX4_EVENT_TYPE_COMM_EST	   = 0x02,
	MLX4_EVENT_TYPE_SQ_DRAINED	   = 0x03,
	MLX4_EVENT_TYPE_SRQ_QP_LAST_WQE	   = 0x13,
	MLX4_EVENT_TYPE_SRQ_LIMIT	   = 0x14,
	MLX4_EVENT_TYPE_CQ_ERROR	   = 0x04,
	MLX4_EVENT_TYPE_WQ_CATAS_ERROR	   = 0x05,
	MLX4_EVENT_TYPE_EEC_CATAS_ERROR	   = 0x06,
	MLX4_EVENT_TYPE_PATH_MIG_FAILED	   = 0x07,
	MLX4_EVENT_TYPE_WQ_INVAL_REQ_ERROR = 0x10,
	MLX4_EVENT_TYPE_WQ_ACCESS_ERROR	   = 0x11,
	MLX4_EVENT_TYPE_SRQ_CATAS_ERROR	   = 0x12,
	MLX4_EVENT_TYPE_LOCAL_CATAS_ERROR  = 0x08,
	MLX4_EVENT_TYPE_PORT_CHANGE	   = 0x09,
	MLX4_EVENT_TYPE_EQ_OVERFLOW	   = 0x0f,
	MLX4_EVENT_TYPE_ECC_DETECT	   = 0x0e,
	MLX4_EVENT_TYPE_CMD		   = 0x0a
};

enum {
	MLX4_PORT_CHANGE_SUBTYPE_DOWN	= 1,
	MLX4_PORT_CHANGE_SUBTYPE_ACTIVE	= 4
};

enum {
	MLX4_PERM_LOCAL_READ	= 1 << 10,
	MLX4_PERM_LOCAL_WRITE	= 1 << 11,
	MLX4_PERM_REMOTE_READ	= 1 << 12,
	MLX4_PERM_REMOTE_WRITE	= 1 << 13,
	MLX4_PERM_ATOMIC	= 1 << 14
};

enum {
	MLX4_OPCODE_NOP			= 0x00,
	MLX4_OPCODE_SEND_INVAL		= 0x01,
	MLX4_OPCODE_RDMA_WRITE		= 0x08,
	MLX4_OPCODE_RDMA_WRITE_IMM	= 0x09,
	MLX4_OPCODE_SEND		= 0x0a,
	MLX4_OPCODE_SEND_IMM		= 0x0b,
	MLX4_OPCODE_LSO			= 0x0e,
	MLX4_OPCODE_RDMA_READ		= 0x10,
	MLX4_OPCODE_ATOMIC_CS		= 0x11,
	MLX4_OPCODE_ATOMIC_FA		= 0x12,
	MLX4_OPCODE_ATOMIC_MASK_CS	= 0x14,
	MLX4_OPCODE_ATOMIC_MASK_FA	= 0x15,
	MLX4_OPCODE_BIND_MW		= 0x18,
	MLX4_OPCODE_FMR			= 0x19,
	MLX4_OPCODE_LOCAL_INVAL		= 0x1b,
	MLX4_OPCODE_CONFIG_CMD		= 0x1f,

	MLX4_RECV_OPCODE_RDMA_WRITE_IMM	= 0x00,
	MLX4_RECV_OPCODE_SEND		= 0x01,
	MLX4_RECV_OPCODE_SEND_IMM	= 0x02,
	MLX4_RECV_OPCODE_SEND_INVAL	= 0x03,

	MLX4_CQE_OPCODE_ERROR		= 0x1e,
	MLX4_CQE_OPCODE_RESIZE		= 0x16,
};

enum {
	MLX4_STAT_RATE_OFFSET	= 5
};

struct mlx4_caps {
	u64			fw_ver;
	int			num_ports;
	int			vl_cap[MLX4_MAX_PORTS + 1];
	int			mtu_cap[MLX4_MAX_PORTS + 1];
	int			gid_table_len[MLX4_MAX_PORTS + 1];
	int			pkey_table_len[MLX4_MAX_PORTS + 1];
	int			local_ca_ack_delay;
	int			num_uars;
	int			bf_reg_size;
	int			bf_regs_per_page;
	int			max_sq_sg;
	int			max_rq_sg;
	int			num_qps;
	int			max_wqes;
	int			max_sq_desc_sz;
	int			max_rq_desc_sz;
	int			max_qp_init_rdma;
	int			max_qp_dest_rdma;
	int			reserved_qps;
	int			sqp_start;
	int			num_srqs;
	int			max_srq_wqes;
	int			max_srq_sge;
	int			reserved_srqs;
	int			num_cqs;
	int			max_cqes;
	int			reserved_cqs;
	int			num_eqs;
	int			reserved_eqs;
	int			num_mpts;
	int			num_mtt_segs;
	int			fmr_reserved_mtts;
	int			reserved_mtts;
	int			reserved_mrws;
	int			reserved_uars;
	int			num_mgms;
	int			num_amgms;
	int			reserved_mcgs;
	int			num_qp_per_mgm;
	int			num_pds;
	int			reserved_pds;
	int			mtt_entry_sz;
	u32			max_msg_sz;
	u32			page_size_cap;
	u32			flags;
	u16			stat_rate_support;
	u8			port_width_cap[MLX4_MAX_PORTS + 1];
};

struct mlx4_buf_list {
	void		       *buf;
	dma_addr_t		map;
};

struct mlx4_buf {
	union {
		struct mlx4_buf_list	direct;
		struct mlx4_buf_list   *page_list;
	} u;
	int			nbufs;
	int			npages;
	int			page_shift;
};

struct mlx4_mtt {
	u32			first_seg;
	int			order;
	int			page_shift;
};

struct mlx4_mr {
	struct mlx4_mtt		mtt;
	u64			iova;
	u64			size;
	u32			key;
	u32			pd;
	u32			access;
	int			enabled;
};

struct mlx4_uar {
	unsigned long		pfn;
	int			index;
};

struct mlx4_cq {
	void (*comp)		(struct mlx4_cq *);
	void (*event)		(struct mlx4_cq *, enum mlx4_event);

	struct mlx4_uar	       *uar;

	u32			cons_index;

	__be32		       *set_ci_db;
	__be32		       *arm_db;
	int			arm_sn;

	int			cqn;

	atomic_t		refcount;
	struct completion	free;
};

struct mlx4_qp {
	void (*event)		(struct mlx4_qp *, enum mlx4_event);

	int			qpn;

	atomic_t		refcount;
	struct completion	free;
};

struct mlx4_srq {
	void (*event)		(struct mlx4_srq *, enum mlx4_event);

	int			srqn;
	int			max;
	int			max_gs;
	int			wqe_shift;

	atomic_t		refcount;
	struct completion	free;
};

struct mlx4_av {
	__be32			port_pd;
	u8			reserved1;
	u8			g_slid;
	__be16			dlid;
	u8			reserved2;
	u8			gid_index;
	u8			stat_rate;
	u8			hop_limit;
	__be32			sl_tclass_flowlabel;
	u8			dgid[16];
};

struct mlx4_dev {
	struct pci_dev	       *pdev;
	unsigned long		flags;
	struct mlx4_caps	caps;
	struct radix_tree_root	qp_table_tree;
};

struct mlx4_init_port_param {
	int			set_guid0;
	int			set_node_guid;
	int			set_si_guid;
	u16			mtu;
	int			port_width_cap;
	u16			vl_cap;
	u16			max_gid;
	u16			max_pkey;
	u64			guid0;
	u64			node_guid;
	u64			si_guid;
};

int mlx4_buf_alloc(struct mlx4_dev *dev, int size, int max_direct,
		   struct mlx4_buf *buf);
void mlx4_buf_free(struct mlx4_dev *dev, int size, struct mlx4_buf *buf);

int mlx4_pd_alloc(struct mlx4_dev *dev, u32 *pdn);
void mlx4_pd_free(struct mlx4_dev *dev, u32 pdn);

int mlx4_uar_alloc(struct mlx4_dev *dev, struct mlx4_uar *uar);
void mlx4_uar_free(struct mlx4_dev *dev, struct mlx4_uar *uar);

int mlx4_mtt_init(struct mlx4_dev *dev, int npages, int page_shift,
		  struct mlx4_mtt *mtt);
void mlx4_mtt_cleanup(struct mlx4_dev *dev, struct mlx4_mtt *mtt);
u64 mlx4_mtt_addr(struct mlx4_dev *dev, struct mlx4_mtt *mtt);

int mlx4_mr_alloc(struct mlx4_dev *dev, u32 pd, u64 iova, u64 size, u32 access,
		  int npages, int page_shift, struct mlx4_mr *mr);
void mlx4_mr_free(struct mlx4_dev *dev, struct mlx4_mr *mr);
int mlx4_mr_enable(struct mlx4_dev *dev, struct mlx4_mr *mr);
int mlx4_write_mtt(struct mlx4_dev *dev, struct mlx4_mtt *mtt,
		   int start_index, int npages, u64 *page_list);
int mlx4_buf_write_mtt(struct mlx4_dev *dev, struct mlx4_mtt *mtt,
		       struct mlx4_buf *buf);

int mlx4_cq_alloc(struct mlx4_dev *dev, int nent, struct mlx4_mtt *mtt,
		  struct mlx4_uar *uar, u64 db_rec, struct mlx4_cq *cq);
void mlx4_cq_free(struct mlx4_dev *dev, struct mlx4_cq *cq);

int mlx4_qp_alloc(struct mlx4_dev *dev, int sqpn, struct mlx4_qp *qp);
void mlx4_qp_free(struct mlx4_dev *dev, struct mlx4_qp *qp);

int mlx4_srq_alloc(struct mlx4_dev *dev, u32 pdn, struct mlx4_mtt *mtt,
		   u64 db_rec, struct mlx4_srq *srq);
void mlx4_srq_free(struct mlx4_dev *dev, struct mlx4_srq *srq);
int mlx4_srq_arm(struct mlx4_dev *dev, struct mlx4_srq *srq, int limit_watermark);
int mlx4_srq_query(struct mlx4_dev *dev, struct mlx4_srq *srq, int *limit_watermark);

int mlx4_INIT_PORT(struct mlx4_dev *dev, int port);
int mlx4_CLOSE_PORT(struct mlx4_dev *dev, int port);

int mlx4_multicast_attach(struct mlx4_dev *dev, struct mlx4_qp *qp, u8 gid[16]);
int mlx4_multicast_detach(struct mlx4_dev *dev, struct mlx4_qp *qp, u8 gid[16]);

#endif /* MLX4_DEVICE_H */
