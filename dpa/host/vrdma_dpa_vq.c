/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include <math.h>
#include <libflexio/flexio.h>
#include "snap-rdma/vrdma/snap_vrdma_ctrl.h"
// #include "snap-rdma/src/snap_vrdma.h"
// #include "snap-rdma/src/mlx5_ifc.h"
#include "lib/vrdma/vrdma_providers.h"
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include "vrdma_dpa_vq.h"
#include "vrdma_dpa.h"
#include "vrdma_dpa_mm.h"
#include "../vrdma_dpa_common.h"

static char *vrdma_vq_rpc_handler[] = {
	[VRDMA_DPA_VQ_QP] = "vrdma_qp_rpc_handler",
};

struct vrdma_dpa_thread_ctx *g_dpa_threads = NULL;

static int vrdma_dpa_get_hart_to_use(struct vrdma_dpa_ctx *dpa_ctx)
{
	uint8_t hart_num = dpa_ctx->core_count * VRDMA_MAX_HARTS_PER_CORE
	                 + dpa_ctx->hart_count;
	if (dpa_ctx->core_count < VRDMA_MAX_CORES_AVAILABLE - 1) {
		dpa_ctx->core_count++;
	} else {
		dpa_ctx->core_count = 1;
		dpa_ctx->hart_count = (dpa_ctx->hart_count + 1) &
				      (VRDMA_MAX_HARTS_PER_CORE - 1);
	}
	return hart_num;
}

int vrdma_dpa_vq_pup_func_register(struct vrdma_dpa_ctx *dpa_ctx)
{
	flexio_func_t *host_stub_func_qp_rpc;
	flexio_func_t *host_stub_func_stats_rpc;
	int err;

	host_stub_func_qp_rpc = calloc(1, sizeof(flexio_func_t));
	if (!host_stub_func_qp_rpc) {
		log_error("Failed to alloc RQ RPC stub func, err(%d)", errno);
		return -ENOMEM;
	}

	err = flexio_func_pup_register(dpa_ctx->app,
				       vrdma_vq_rpc_handler[VRDMA_DPA_VQ_QP],
				       VRDMA_DPA_RPC_UNPACK_FUNC,
				       host_stub_func_qp_rpc, sizeof(uint64_t),
				       vrdma_dpa_rpc_pack_func);
	if (err) {
		log_error("Failed to register RQ RPC func, err(%d)", err);
		goto err_qp_rpc;
	}
	dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP] = host_stub_func_qp_rpc;

	host_stub_func_stats_rpc = calloc(1, sizeof(flexio_func_t));
	if (!host_stub_func_stats_rpc) {
		log_error("Failed to alloc stats RPC stub func, err(%d)", errno);
		err = -ENOMEM;
		goto err_stats_alloc;
	}

	err = flexio_func_pup_register(dpa_ctx->app, "vrdma_dev2host_copy_handler",
				       VRDMA_DPA_RPC_UNPACK_FUNC,
				       host_stub_func_stats_rpc, sizeof(uint64_t),
				       vrdma_dpa_rpc_pack_func);
	if (err) {
		log_error("Failed to register stats PRC func, err(%d)", err);
		goto err_stats_rpc;
	}
	dpa_ctx->dev2host_copy_func = host_stub_func_stats_rpc;

	return 0;
err_stats_rpc:
	free(host_stub_func_stats_rpc);
err_stats_alloc:
	dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP] = NULL;
err_qp_rpc:
	free(host_stub_func_qp_rpc);
	return err;
}

void vrdma_dpa_vq_pup_func_deregister(struct vrdma_dpa_ctx *dpa_ctx)
{
	free(dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP]);
	dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP] = NULL;
	free(dpa_ctx->dev2host_copy_func);
	dpa_ctx->dev2host_copy_func = NULL;
}

#if 0
/* we need state modify*/
static int vrdma_dpa_vq_state_modify(struct vrdma_dpa_thread_ctx *dpa_thread,
				enum vrdma_dpa_vq_state state)
{
	uint64_t rpc_ret;
	char *dst_addr;
	int value;
	int err;

	/* Just update the state. placed it as last field in struct */
	value = state;
	dst_addr = (char *)dpa_thread->heap_memory +
		   offsetof(struct vrdma_dpa_event_handler_ctx,
			    dma_qp.state);

	err = flexio_host2dev_memcpy(dpa_vq->dpa_ctx->flexio_process,
				     &value, sizeof(value),
				     (uintptr_t)dst_addr);
	if (err) {
		log_error("Failed to copy vq_state to dev, err(%d)", err);
		return err;
	}

	if (state == VRDMA_DPA_VQ_STATE_RDY) {
		err = flexio_process_call(dpa_vq->dpa_ctx->flexio_process,
					  dpa_vq->dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP],
					  &rpc_ret, dpa_vq->heap_memory);
		if (err) {
			log_error("Failed to call rpc, err(%d), rpc_ret(%ld)",
				  err, rpc_ret);
			if (flexio_err_status(dpa_vq->dpa_ctx->flexio_process)) {
				flexio_coredump_create(dpa_vq->dpa_ctx->flexio_process, "/images/flexio.core");
			}
		}
	}

 	return err;
}


static int vrdma_dpa_vq_modify(struct vrdma_prov_vq *vq, uint64_t mask,
				 uint32_t state)
{
	struct vrdma_dpa_vq *dpa_vq = vq->dpa_q;
	int err = 0;

	// if (mask & ~VIRTNET_DPA_ALLOWED_MASK) {
	// 	log_error("Mask %#lx is not supported", mask);
	// 	return -EINVAL;
	// }

	/*Todo: later check*/
	// if (mask & SNAP_VIRTIO_NET_QUEUE_MOD_STATE) {
		err = vrdma_dpa_vq_state_modify(dpa_vq, state);
		if (err)
			log_error("Failed to modify vq(%d) state to (%d), err(%d)",
				  dpa_vq->idx, state, err);
	// }

	return err;
}
#endif

static int vrdma_event_handler_create(struct vrdma_dpa_ctx *dpa_ctx,
				      const char *handler,
				      flexio_func_t *handler_func,
				      struct flexio_event_handler **event_handler_ptr)
{
	struct flexio_event_handler_attr attr = {};
	int err;

	err = flexio_func_register(dpa_ctx->app, handler, &handler_func);
	if (err) {
		log_error("Failed to register function, err(%d)", err);
		return err;
	}

	attr.host_stub_func = handler_func;
	attr.affinity.type = FLEXIO_AFFINITY_STRICT;
	attr.affinity.id = vrdma_dpa_get_hart_to_use(dpa_ctx);

	err = flexio_event_handler_create(dpa_ctx->flexio_process, &attr,
					  					dpa_ctx->db_outbox,
					  					event_handler_ptr);
	if (err) {
		log_error("Failed to create event_handler %s, hart (%d), err(%d)", handler, attr.affinity.id, err);
		return err;
	}
	//log_notice("%s use %d hart", handler, attr.affinity.id);
	return 0;
}

static int vrdma_dpa_handler_ctx_init(struct vrdma_dpa_thread_ctx *dpa_thread)
{
	
	int err = 0;
	
	err = vrdma_dpa_mm_zalloc(dpa_thread->dpa_ctx->flexio_process,
					   	sizeof(struct vrdma_dpa_event_handler_ctx),
					   	&dpa_thread->heap_memory);
	
	if (err) {
		log_error("Failed to allocate dev buf, err(%d)", err);
	}
	return err;
}

static int vrdma_dpa_db_handler_init(struct vrdma_dpa_ctx *dpa_ctx,
												struct vrdma_dpa_handler *dpa_handler,
												const char *db_handler,
												const char *rq_dma_q_handler)
{
	int err = 0;

	err = vrdma_event_handler_create(dpa_ctx, db_handler, 
					dpa_handler->db_handler_func,
					&dpa_handler->db_handler);

	if (err) {
		return err;
	}
#if 0
	err = vrdma_event_handler_create(dpa_ctx, rq_dma_q_handler,
					dpa_handler->rq_dma_q_handler_func,
					&dpa_handler->rq_dma_q_handler);

	if (err) {
		goto err_rq_dma_q_handler_create;
	}
#endif
	return 0;
err_rq_dma_q_handler_create:
	flexio_event_handler_destroy(dpa_handler->db_handler);
	return err;
}

static void vrdma_dpa_db_handler_uninit(struct vrdma_dpa_handler *dpa_handler)
{
	//flexio_event_handler_destroy(dpa_handler->rq_dma_q_handler);
	flexio_event_handler_destroy(dpa_handler->db_handler);
}

static int vrdma_dpa_db_cq_create(struct flexio_process *process,
			     struct ibv_context *emu_ibv_ctx,
			     struct flexio_event_handler *event_handler,
			     struct vrdma_dpa_cq *dpa_cq,
			     uint32_t emu_uar_id)
{
	struct flexio_cq_attr cq_attr = {};
	int err;

	err = vrdma_dpa_mm_cq_alloc(process, BIT_ULL(VRDMA_DB_CQ_LOG_DEPTH),
				    			dpa_cq);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		return err;
	}

	cq_attr.log_cq_depth = VRDMA_DB_CQ_LOG_DEPTH;
	cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD;
	cq_attr.thread = flexio_event_handler_get_thread(event_handler);
	cq_attr.uar_id = emu_uar_id;
	cq_attr.cq_dbr_daddr = dpa_cq->cq_dbr_daddr;
	cq_attr.cq_ring_qmem.daddr  = dpa_cq->cq_ring_daddr;
	cq_attr.always_armed = 1;
	cq_attr.overrun_ignore = 1;
	err = flexio_cq_create(process, emu_ibv_ctx, &cq_attr, &dpa_cq->cq);
	if (err) {
		log_error("Failed to create cq, err(%d)", err);
		goto err_cq_create;
	}
	dpa_cq->cq_num = flexio_cq_get_cq_num(dpa_cq->cq);
	dpa_cq->log_cq_size = cq_attr.log_cq_depth;
	return 0;
err_cq_create:
	vrdma_dpa_mm_cq_free(process, dpa_cq);
	return err;
}

static void vrdma_dpa_db_cq_destroy(struct vrdma_dpa_handler *dpa_handler,
											struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	if (!dpa_handler) {
		return;
	}
	if (dpa_handler->db_cq.cq) {
		flexio_cq_destroy(dpa_handler->db_cq.cq);
	}
	vrdma_dpa_mm_cq_free(emu_dev_ctx->flexio_process,
			       &dpa_handler->db_cq);
}

#define VRDMA_QP_SND_RCV_PSN		0x4242
static int vrdma_dpa_dma_q_create(struct vrdma_dpa_dma_qp *dpa_dma_qp,
				    struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
				    struct vrdma_dpa_dma_qp_attr *attr,
				    uint32_t rqcq_num,
				    uint32_t sqcq_num)

{
	struct flexio_qp_attr_opt_param_mask qp_mask = {};
	struct flexio_qp_attr qp_attr = {};
	int err;

	qp_attr.transport_type = FLEXIO_QPC_ST_RC;
	qp_attr.log_sq_depth = log2(attr->tx_qsize);//VRDMA_DPA_CVQ_SQ_DEPTH;
	qp_attr.log_rq_depth = log2(attr->rx_qsize);//VRDMA_DPA_CVQ_RQ_DEPTH;
	qp_attr.uar_id = emu_dev_ctx->sf_uar->page_id;
	qp_attr.sq_cqn = sqcq_num;
	qp_attr.rq_cqn = rqcq_num;
	qp_attr.pd = attr->sf_pd;
	qp_attr.qp_access_mask = IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE;
	qp_attr.ops_flag = FLEXIO_QP_WR_RDMA_WRITE | FLEXIO_QP_WR_RDMA_READ |
			   FLEXIO_QP_WR_ATOMIC_CMP_AND_SWAP;
	dpa_dma_qp->buff_daddr =
		vrdma_dpa_mm_qp_buff_alloc(emu_dev_ctx->flexio_process,
					     attr->rx_qsize,
					     &dpa_dma_qp->rq_daddr,
					     attr->tx_qsize,
					     &dpa_dma_qp->sq_daddr);
	if (!dpa_dma_qp->buff_daddr) {
		log_error("Failed to alloc qp buff, err(%d)", errno);
		return errno;
	}

	dpa_dma_qp->dbr_daddr =
					vrdma_dpa_mm_dbr_alloc(emu_dev_ctx->flexio_process);
	if (!dpa_dma_qp->dbr_daddr) {
		log_error("Failed to alloc qp_dbr, err(%d)", errno);
		err = errno;
		goto err_alloc_dbr;
	}

	/* prepare rx ring*/
	err = vrdma_dpa_mm_zalloc(emu_dev_ctx->flexio_process,
				   attr->rx_qsize * attr->rx_elem_size,
				   &dpa_dma_qp->rx_wqe_buff);
	if (err) {
		log_error("Failed to allocate dev buffer, err(%d)", err);
		goto err_rx_wqe_buf_alloc;
	}

	err = vrdma_dpa_mkey_create(emu_dev_ctx, &qp_attr,
				    attr->rx_qsize * attr->rx_elem_size,
					dpa_dma_qp->rx_wqe_buff,
				    &dpa_dma_qp->rqd_mkey);
	if (err) {
		log_error("Failed to create rx mkey, err(%d)", err);
		goto err_rqd_mkey_create;
	}

	err = vrdma_dpa_init_qp_rx_ring(emu_dev_ctx, dpa_dma_qp,
					  attr->rx_qsize,
					  sizeof(struct mlx5_wqe_data_seg),
					  attr->rx_elem_size,
					  flexio_mkey_get_id(dpa_dma_qp->rqd_mkey));
	if (err) {
		log_error("Failed to init QP Rx, err(%d)", err);
		goto err_init_qp_rx_ring;
	}

	/* prepare tx ring */
	err = vrdma_dpa_mm_zalloc(emu_dev_ctx->flexio_process,
				   attr->tx_qsize * attr->tx_elem_size,
				   &dpa_dma_qp->tx_wqe_buff);
	if (err) {
		log_error("Failed to allocate tx dev buffer, err(%d)", err);
		goto err_tx_wqe_buff_alloc;
	}

	err = vrdma_dpa_mkey_create(emu_dev_ctx, &qp_attr,
				      attr->tx_qsize * attr->tx_elem_size,
					  dpa_dma_qp->tx_wqe_buff,
				      &dpa_dma_qp->sqd_mkey);
	if (err) {
		log_error("Failed to create tx mkey, err(%d)", err);
		goto err_sqd_mkey_create;
	}

	qp_attr.qp_wq_buff_qmem.memtype = FLEXIO_MEMTYPE_DPA;
	qp_attr.qp_wq_buff_qmem.daddr = dpa_dma_qp->buff_daddr;
	qp_attr.qp_wq_dbr_qmem.daddr = dpa_dma_qp->dbr_daddr;
	err = flexio_qp_create(emu_dev_ctx->flexio_process, attr->sf_ib_ctx,
			       &qp_attr, &dpa_dma_qp->qp);
	if (err) {
		log_error("Failed to create QP, err (%d)", err);
		goto err_qp_create;
	}

	dpa_dma_qp->qp_num = flexio_qp_get_qp_num(dpa_dma_qp->qp);
	dpa_dma_qp->log_rq_depth = qp_attr.log_rq_depth;
	dpa_dma_qp->log_sq_depth = qp_attr.log_sq_depth;

	/* connect qp dev and qp host */
	qp_attr.remote_qp_num = attr->tisn_or_qpn;
	qp_attr.qp_access_mask = qp_attr.qp_access_mask;
	qp_attr.fl = 1;
	qp_attr.min_rnr_nak_timer = 0x12;
	qp_attr.path_mtu = 0x3;
	qp_attr.retry_count = 0x7;
	qp_attr.vhca_port_num = 0x1;
	/*todo: why we need this?*/
	qp_attr.next_send_psn = VRDMA_QP_SND_RCV_PSN;
	qp_attr.next_rcv_psn = VRDMA_QP_SND_RCV_PSN;
	qp_attr.next_state = FLEXIO_QP_STATE_INIT;
	err = flexio_qp_modify(dpa_dma_qp->qp, &qp_attr, &qp_mask);
	if (err) {
		log_error("Failed to modify DEV QP to INIT state, err(%d)", err);
		goto err_qp_ready;
	}

	qp_attr.next_state = FLEXIO_QP_STATE_RTR;
	err = flexio_qp_modify(dpa_dma_qp->qp, &qp_attr, &qp_mask);
	if (err) {
		log_error("Failed to modify DEV QP to RTR state, err(%d)", err);
		goto err_qp_ready;
	}

	qp_attr.next_state = FLEXIO_QP_STATE_RTS;
	err = flexio_qp_modify(dpa_dma_qp->qp, &qp_attr, &qp_mask);
	if (err) {
		log_error("Failed to modify DEV QP to RTS state, err(%d)", err);
		goto err_qp_ready;
	}

	return 0;
err_qp_ready:
	flexio_qp_destroy(dpa_dma_qp->qp);
err_qp_create:
	vrdma_dpa_mkey_destroy(dpa_dma_qp->sqd_mkey);
err_sqd_mkey_create:
	vrdma_dpa_mm_free(emu_dev_ctx->flexio_process,
						dpa_dma_qp->tx_wqe_buff);
err_tx_wqe_buff_alloc:
err_init_qp_rx_ring:
	vrdma_dpa_mkey_destroy(dpa_dma_qp->rqd_mkey);
err_rqd_mkey_create:
	vrdma_dpa_mm_free(emu_dev_ctx->flexio_process,
                 dpa_dma_qp->rx_wqe_buff);
err_rx_wqe_buf_alloc:
	vrdma_dpa_mm_free(emu_dev_ctx->flexio_process,
			    dpa_dma_qp->dbr_daddr);
err_alloc_dbr:
	vrdma_dpa_mm_qp_buff_free(emu_dev_ctx->flexio_process,
				    dpa_dma_qp->buff_daddr);
	return err;
}

static int
_vrdma_dpa_dma_q_cq_create(struct flexio_process *process,
			     struct ibv_context *ibv_ctx,
			     struct flexio_event_handler *event_handler,
			     struct vrdma_dpa_cq *rq_dpacq,
			     struct vrdma_dpa_cq *sq_dpacq,
			     struct snap_vrdma_vq_create_attr *q_attr,
			     struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	struct flexio_cq_attr cq_attr = {};
	int err;

	/* QP RQ_CQ */
	err = vrdma_dpa_mm_cq_alloc(process, q_attr->rq_size, rq_dpacq);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		return err;
	}

	//log_notice("rx_qsize %d", q_attr->rq_size);
	cq_attr.log_cq_depth = log2(q_attr->rq_size);
	cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_NON_DPA_CQ;
	//cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD;
	//cq_attr.thread = flexio_event_handler_get_thread(event_handler);
	cq_attr.uar_base_addr = emu_dev_ctx->sf_uar->base_addr;
	cq_attr.uar_id = emu_dev_ctx->sf_uar->page_id;
	cq_attr.cq_dbr_daddr = rq_dpacq->cq_dbr_daddr;
	cq_attr.cq_ring_qmem.daddr = rq_dpacq->cq_ring_daddr;
	err = flexio_cq_create(process, ibv_ctx, &cq_attr, &rq_dpacq->cq);
	if (err) {
		log_error("Failed to create dma_q rqcq, err(%d)", err);
		goto err_rqcq_create;
	}

	/* QP SQ_CQ */
	err = vrdma_dpa_mm_cq_alloc(process, q_attr->sq_size, sq_dpacq);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		goto err_rqcq_mem_alloc;
	}

	//log_notice("tx_qsize %d", q_attr->sq_size);
	cq_attr.log_cq_depth = log2(q_attr->sq_size);
	cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_NON_DPA_CQ;
	cq_attr.uar_base_addr = emu_dev_ctx->sf_uar->base_addr;
	cq_attr.uar_id = emu_dev_ctx->sf_uar->page_id;
	cq_attr.cq_dbr_daddr = sq_dpacq->cq_dbr_daddr;
	cq_attr.cq_ring_qmem.daddr = sq_dpacq->cq_ring_daddr;
	//log_notice("_vrdma_dpa_dma_q_cq_create");
	err = flexio_cq_create(process, ibv_ctx, &cq_attr, &sq_dpacq->cq);
	if (err) {
		log_error("\nFailed to create dma_q sqcq, err(%d)\n", err);
		goto err_sqcq_create;
	}

	return 0;
err_sqcq_create:
	vrdma_dpa_mm_cq_free(process, sq_dpacq);
err_rqcq_mem_alloc:
	flexio_cq_destroy(rq_dpacq->cq);
err_rqcq_create:
	vrdma_dpa_mm_cq_free(process, rq_dpacq);
	return err;
}

static int
vrdma_dpa_dma_q_cq_create(struct vrdma_dpa_dma_qp *dpa_dma_qp,
			    					struct vrdma_dpa_handler *dpa_handler,
			    					struct ibv_context *ibv_ctx,
			   						struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			    					struct snap_vrdma_vq_create_attr *q_attr)
{
	int err;

	err = _vrdma_dpa_dma_q_cq_create(emu_dev_ctx->flexio_process, ibv_ctx,
					   dpa_handler->rq_dma_q_handler,
					   &dpa_dma_qp->dma_q_rqcq,
					   &dpa_dma_qp->dma_q_sqcq,
					   q_attr,
					   emu_dev_ctx);
	if (err) {
		log_error("Failed to create db_cq, err(%d)", err);
		return err;
	}

	dpa_dma_qp->dma_q_rqcq.cq_num = 
					flexio_cq_get_cq_num(dpa_dma_qp->dma_q_rqcq.cq);
	dpa_dma_qp->dma_q_sqcq.cq_num = 
					flexio_cq_get_cq_num(dpa_dma_qp->dma_q_sqcq.cq);
	dpa_dma_qp->dma_q_rqcq.log_cq_size = log2(q_attr->rq_size);
	dpa_dma_qp->dma_q_sqcq.log_cq_size = log2(q_attr->sq_size);
	return 0;
}

static void
vrdma_dpa_dma_q_cq_destroy(struct vrdma_dpa_dma_qp *dpa_dma_qp,
									struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	if (!dpa_dma_qp) {
		return;
	}
	if (dpa_dma_qp->dma_q_sqcq.cq) {
		flexio_cq_destroy(dpa_dma_qp->dma_q_sqcq.cq);
	}
	vrdma_dpa_mm_cq_free(emu_dev_ctx->flexio_process, &dpa_dma_qp->dma_q_sqcq);
	if (dpa_dma_qp->dma_q_rqcq.cq) {
		flexio_cq_destroy(dpa_dma_qp->dma_q_rqcq.cq);
	}
	vrdma_dpa_mm_cq_free(emu_dev_ctx->flexio_process, &dpa_dma_qp->dma_q_rqcq);
}

static int
vrdma_dpa_thread_ctx_init(struct vrdma_dpa_thread_ctx *thread,
				  struct vrdma_prov_thread_init_attr *attr)
{
	struct vrdma_dpa_event_handler_ctx *eh_data;
	struct vrdma_dpa_ctx *dpa_ctx;
	struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx;
	uint32_t dbcq_num;
	int err;

	if (!thread || !attr) {
		log_error("input parameter is NULL");
		return -1;
	}
	eh_data = calloc(1, sizeof(*eh_data));
	if (!eh_data) {
		log_error("Failed to allocate memory");
		return -ENOMEM;
	}

	dpa_ctx = thread->dpa_ctx;
	emu_dev_ctx = thread->emu_dev_ctx;
	eh_data->dbg_signature = DBG_EVENT_HANDLER_CHECK;
	spin_init(&eh_data->vqp_array_lock);
	eh_data->vqp_count = 0;
	eh_data->emu_crossing_mkey = attr->emu_mkey;

	/*prepare db handler cq ctx*/
	dbcq_num = flexio_cq_get_cq_num(thread->dpa_handler->db_cq.cq);
	eh_data->guest_db_cq_ctx.cqn = dbcq_num;
	eh_data->guest_db_cq_ctx.ring =
		(struct flexio_dev_cqe64 *)thread->dpa_handler->db_cq.cq_ring_daddr;
	eh_data->guest_db_cq_ctx.dbr = (uint32_t *)thread->dpa_handler->db_cq.cq_dbr_daddr;
	eh_data->guest_db_cq_ctx.cqe = eh_data->guest_db_cq_ctx.ring;
	eh_data->guest_db_cq_ctx.hw_owner_bit = 0;
	eh_data->guest_db_cq_ctx.cq_depth = POW2(thread->dpa_handler->db_cq.log_cq_size);

	/*prepare msix handler qp.rq.cq ctx*/
	eh_data->msix_cq_ctx.cqn = flexio_cq_get_cq_num(thread->dpa_dma_qp->dma_q_rqcq.cq);
	eh_data->msix_cq_ctx.ring =
		(struct flexio_dev_cqe64 *)thread->dpa_dma_qp->dma_q_rqcq.cq_ring_daddr;
	eh_data->msix_cq_ctx.dbr = (uint32_t *)thread->dpa_dma_qp->dma_q_rqcq.cq_dbr_daddr;
	eh_data->msix_cq_ctx.cqe = eh_data->msix_cq_ctx.ring;
	eh_data->msix_cq_ctx.cq_depth = POW2(thread->dpa_dma_qp->dma_q_rqcq.log_cq_size);
	eh_data->msix_cq_ctx.hw_owner_bit = 1;

	/*prepare dma_qp address*/
	eh_data->dma_qp.hw_sq_size = attr->dma_tx_qsize;
	eh_data->dma_qp.qp_rqcq = thread->dpa_dma_qp->dma_q_rqcq;
	eh_data->dma_qp.qp_sq_buff = thread->dpa_dma_qp->sq_daddr;
	eh_data->dma_qp.qp_rq_buff = thread->dpa_dma_qp->rq_daddr;
	eh_data->dma_qp.qp_num = flexio_qp_get_qp_num(thread->dpa_dma_qp->qp);
	eh_data->dma_qp.dbr_daddr = thread->dpa_dma_qp->dbr_daddr;

	/*prepare dma_sqcq_ctx*/
	eh_data->dma_sqcq_ctx.cqn = flexio_cq_get_cq_num(thread->dpa_dma_qp->dma_q_sqcq.cq);
	eh_data->dma_sqcq_ctx.ring =
		(struct flexio_dev_cqe64 *)thread->dpa_dma_qp->dma_q_sqcq.cq_ring_daddr;
	eh_data->dma_sqcq_ctx.dbr = (uint32_t *)thread->dpa_dma_qp->dma_q_sqcq.cq_dbr_daddr;
	eh_data->dma_sqcq_ctx.cqe = eh_data->dma_sqcq_ctx.ring;
	eh_data->dma_sqcq_ctx.cq_depth = POW2(thread->dpa_dma_qp->dma_q_sqcq.log_cq_size);
	eh_data->dma_sqcq_ctx.hw_owner_bit = 0;
	//log_error("eh_data->dma_sqcq_ctx.cq_depth %d", eh_data->dma_sqcq_ctx.cq_depth);
	/* Update other pointers */
	eh_data->dma_qp.state = VRDMA_DPA_VQ_STATE_INIT;
	eh_data->emu_outbox = flexio_outbox_get_id(dpa_ctx->db_outbox);
	eh_data->sf_outbox = flexio_outbox_get_id(emu_dev_ctx->db_sf_outbox);

	eh_data->window_id = flexio_window_get_id(dpa_ctx->window);
	eh_data->ce_set_threshold = attr->dma_tx_qsize/2;
	//log_notice("ce_set_threshold %d", eh_data->ce_set_threshold);
	// eh_data->vq_depth = attr->common.size;

	/*virtnet use host msix to send msix,but vrdma don't need, vrdma get cqn from dma rq.wqe*/
	// if (attr->msix_vector != 0xFFFF)
	// 	eh_data->msix.cqn =
	// 		emu_dev_ctx->msix[attr->msix_vector].cqn;

	err = flexio_host2dev_memcpy(dpa_ctx->flexio_process,
				     eh_data,
				     sizeof(*eh_data),
				     thread->heap_memory);
	if (err)
		log_error("Failed to copy ctx to dev, err(%d)", err);

	thread->eh_ctx = eh_data;
	return err;
}

static void vrdma_dpa_thread_dump_attribute(struct vrdma_dpa_thread_ctx *dpa_thread,
										struct snap_vrdma_vq_create_attr *q_attr,
										struct vrdma_prov_thread_init_attr *attr)
{
	return;
	log_notice("\n====================dump dma qp parameter======================");
	log_notice("\ntx_qsize %#x, rx_qsize %#x, tx_elem_size %#x, rx_elem_size %#x\n",
		   q_attr->sq_size, q_attr->rq_size,
		   q_attr->tx_elem_size, q_attr->rx_elem_size);

	log_notice("\n====================dump dpa thread info=====================");
	log_notice("\ndpa thread index %d, emu_vhca_id %#x, sf_vhca_id %#x\n"
			"hw_dbcq %#x\n"
			"sw_qp : %#x sqcq %#x rqcq %#x,\ndpa qp: %#x sqcq %#x rqcq %#x\n",
			dpa_thread->thread_idx, attr->emu_vhca_id, attr->sf_vhca_id,
			dpa_thread->dpa_handler->db_cq.cq_num,
		 	dpa_thread->sw_dma_qp->dma_q->sw_qp.dv_qp.hw_qp.qp_num,
		 	dpa_thread->sw_dma_qp->dma_q->sw_qp.dv_tx_cq.cq_num,
		 	dpa_thread->sw_dma_qp->dma_q->sw_qp.dv_rx_cq.cq_num,
			dpa_thread->dpa_dma_qp->qp_num,
		 	dpa_thread->dpa_dma_qp->dma_q_sqcq.cq_num,
		 	dpa_thread->dpa_dma_qp->dma_q_rqcq.cq_num);
}

static void vrdma_dpa_vq_dump_attribute(struct vrdma_prov_vqp_init_attr *attr,
												struct vrdma_dpa_thread_ctx *dpa_thread)
{
	return;
	log_notice("\n====================dump virtq && dma qp info=====================");
		log_notice("\nemu_vhca_id %#x, sf_vhca_id %#x, vq_idx %#x, vq_qdb_idx %#x\n"
				"hw_dbcq %#x\n"
				"sw_qp : %#x sqcq %#x rqcq %#x,\ndpa qp: %#x sqcq %#x rqcq %#x\n",
				attr->emu_vhca_id, attr->sf_vhca_id,
				attr->vq_idx, attr->qdb_idx,
				dpa_thread->dpa_handler->db_cq.cq_num,
				dpa_thread->sw_dma_qp->dma_q->sw_qp.dv_qp.hw_qp.qp_num,
				dpa_thread->sw_dma_qp->dma_q->sw_qp.dv_tx_cq.cq_num,
				dpa_thread->sw_dma_qp->dma_q->sw_qp.dv_rx_cq.cq_num,
				dpa_thread->dpa_dma_qp->qp_num,
				dpa_thread->dpa_dma_qp->dma_q_sqcq.cq_num,
				dpa_thread->dpa_dma_qp->dma_q_rqcq.cq_num);

	log_notice("\n====================dump vqp info in host==========================");
	log_notice("\nrq_wqe_buff_pa %#lx, rq_pi_paddr %#lx, rq_wqebb_cnt %#x,"
			"rq_wqebb_size %#x,\nsq_wqe_buff_pa %#lx, sq_pi_paddr %#lx,"
			"sq_wqebb_cnt %#x, sq_wqebb_size %#x,\nemu_crossing_mkey %#x,"
			"sf_crossing_mkey %#x\n",
			attr->host_vq_ctx.rq_wqe_buff_pa, attr->host_vq_ctx.rq_pi_paddr,
			attr->host_vq_ctx.rq_wqebb_cnt, attr->host_vq_ctx.rq_wqebb_size,
			attr->host_vq_ctx.sq_wqe_buff_pa, attr->host_vq_ctx.sq_pi_paddr,
			attr->host_vq_ctx.sq_wqebb_cnt, attr->host_vq_ctx.sq_wqebb_size,
			attr->host_vq_ctx.emu_crossing_mkey, attr->host_vq_ctx.sf_crossing_mkey);

	log_notice("\n====================dump vqp info in arm==========================");
	log_notice("\nrq_buff_addr %#lx, sq_buff_addr %#lx, rq_lkey %#x, sq_lkey %#x\n",
			attr->arm_vq_ctx.rq_buff_addr, attr->arm_vq_ctx.sq_buff_addr,
			attr->arm_vq_ctx.rq_lkey, attr->arm_vq_ctx.sq_lkey);
	
}

static inline void 
vrdma_set_dpa_vqp_attr(struct vrdma_prov_vqp_init_attr *attr,
								struct vrdma_ctrl *ctrl,
								struct spdk_vrdma_qp *vqp)
{
	attr->qdb_idx = vqp->qdb_idx;
	attr->vq_idx = vqp->qp_idx;
	attr->emu_vhca_id  = ctrl->sctrl->sdev->pci->mpci.vhca_id;
	attr->emu_db_handler = vqp->dpa_vqp.emu_db_to_cq_id;
	attr->sf_vhca_id  = ctrl->sf_vhca_id;

	/*prepare host wr parameters*/
	attr->host_vq_ctx.rq_wqe_buff_pa = vqp->rq.comm.wqe_buff_pa;
	attr->host_vq_ctx.rq_pi_paddr    = vqp->rq.comm.doorbell_pa;
	attr->host_vq_ctx.rq_wqebb_cnt   = vqp->rq.comm.wqebb_cnt;
	attr->host_vq_ctx.rq_wqebb_size  = vqp->rq.comm.wqebb_size;
	attr->host_vq_ctx.sq_wqe_buff_pa = vqp->sq.comm.wqe_buff_pa;
	attr->host_vq_ctx.sq_pi_paddr    = vqp->sq.comm.doorbell_pa;
	attr->host_vq_ctx.sq_wqebb_cnt   = vqp->sq.comm.wqebb_cnt;
	attr->host_vq_ctx.sq_wqebb_size  = vqp->sq.comm.wqebb_size;
	attr->host_vq_ctx.emu_crossing_mkey = ctrl->sctrl->xmkey->mkey;
	attr->host_vq_ctx.sf_crossing_mkey  = ctrl->sctrl->xmkey->mkey;

	/*prepare arm wr parameters*/
	attr->arm_vq_ctx.rq_buff_addr = (uint64_t)vqp->rq.rq_buff;
	attr->arm_vq_ctx.sq_buff_addr = (uint64_t)vqp->sq.sq_buff;
	attr->arm_vq_ctx.sq_pi_addr = (uint64_t)&vqp->qp_pi->pi.sq_pi;
	attr->arm_vq_ctx.rq_lkey      = vqp->qp_mr->lkey;
	attr->arm_vq_ctx.sq_lkey      = vqp->qp_mr->lkey;

}

static int 
vrdma_dpa_find_free_vqp_slot(struct vrdma_dpa_event_handler_ctx *eh_data,
										uint16_t *vqp_idx)
{
	uint16_t free_idx = 0;

	for (free_idx = 0; free_idx < VQP_PER_THREAD; free_idx++) {
		if (!eh_data->vqp_ctx[free_idx].valid) {
			*vqp_idx = free_idx;
			return 0;
		}
	}
	return -1;
}

static int
vrdma_dpa_vq_ctx_init(const struct vrdma_dpa_thread_ctx *dpa_thread,
				  			struct vrdma_prov_vqp_init_attr *attr,
				  			struct spdk_vrdma_qp *vqp)
{
	struct vrdma_dpa_vqp_ctx *eh_qp_data;
	uint16_t slot_idx = 0;
	flexio_status err;
	uint64_t rpc_ret;
	int ret = 0;

	ret = vrdma_dpa_find_free_vqp_slot(dpa_thread->eh_ctx, &slot_idx);
	if (ret) {
		log_error("no free vqp slot in thread");
		goto out;
	}

	/*prepare host and arm wr&pi address which used for rdma write*/
	eh_qp_data = calloc(1, sizeof(*eh_qp_data));
	if (!eh_qp_data) {
		log_error("ctx data malloc failed, no more memory");
		ret = -1;
		goto out;
	}
	eh_qp_data->emu_db_to_cq_id = attr->emu_db_handler;
	eh_qp_data->vq_index = attr->vq_idx;
	eh_qp_data->host_vq_ctx = attr->host_vq_ctx;
	eh_qp_data->arm_vq_ctx	= attr->arm_vq_ctx;
	eh_qp_data->state = VRDMA_DPA_VQ_STATE_RDY;
	eh_qp_data->eh_ctx_daddr = dpa_thread->heap_memory;
	eh_qp_data->free_idx = slot_idx;
	vqp->dpa_vqp.ctx_idx = slot_idx;
	dpa_thread->eh_ctx->vqp_ctx[slot_idx].valid = 1;
	dpa_thread->eh_ctx->vqp_ctx_hdl[attr->emu_db_handler].vqp_ctx_handle = vqp->dpa_vqp.dpa_heap_memory;
	dpa_thread->eh_ctx->vqp_ctx_hdl[attr->emu_db_handler].valid = 1;

	err = flexio_host2dev_memcpy(dpa_thread->dpa_ctx->flexio_process,
				     			eh_qp_data, sizeof(*eh_qp_data),
				     			vqp->dpa_vqp.dpa_heap_memory);
	if (err) {
		log_error("Failed to copy vqp ctx to dev, err(%d)", err);
		ret = -1;
		goto free_memory;
	}

	err = flexio_process_call(dpa_thread->dpa_ctx->flexio_process,
					  dpa_thread->dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP],
					  &rpc_ret, vqp->dpa_vqp.dpa_heap_memory);
	if (err) {
		log_error("Failed to call rpc, err(%d), rpc_ret(%ld)",
				  err, rpc_ret);
		if (flexio_err_status(dpa_thread->dpa_ctx->flexio_process)) {
			flexio_coredump_create(dpa_thread->dpa_ctx->flexio_process, "/images/flexio.core");
		}
		ret = -1;
		goto free_memory;
	}

	//log_info("vqp call rpc, vqp dpa_handle %lx, vqp idx %d\n",
	//			  vqp->dpa_vqp.dpa_heap_memory, vqp->qp_idx);

free_memory:
	free(eh_qp_data);
out:
	return ret;
}

static int
vrdma_dpa_vq_ctx_release(struct spdk_vrdma_qp *vqp)
{
	struct vrdma_dpa_vqp_ctx *eh_qp_data;
	struct vrdma_dpa_thread_ctx *dpa_thread;;
	flexio_status err;
	uint64_t rpc_ret;
	int ret = 0;

	if (!vqp || vqp->dpa_vqp.dpa_thread) {
		return;
	}
	dpa_thread = vqp->dpa_vqp.dpa_thread;
	eh_qp_data = calloc(1, sizeof(*eh_qp_data));
	if (!eh_qp_data) {
		log_error("ctx data malloc failed, no more memory");
		ret = -1;
		goto out;
	}
	eh_qp_data->state = VRDMA_DPA_VQ_STATE_SUSPEND;
	dpa_thread->eh_ctx->vqp_ctx[vqp->dpa_vqp.ctx_idx].valid = 0;

	err = flexio_host2dev_memcpy(dpa_thread->dpa_ctx->flexio_process,
				     			&eh_qp_data->state, sizeof(eh_qp_data->state),
				     			vqp->dpa_vqp.dpa_heap_memory);
	if (err) {
		log_error("Failed to copy vqp ctx to dev, err(%d)", err);
		ret = -1;
		goto free_memory;
	}

	err = flexio_process_call(dpa_thread->dpa_ctx->flexio_process,
					  dpa_thread->dpa_ctx->vq_rpc_func[VRDMA_DPA_VQ_QP],
					  &rpc_ret, vqp->dpa_vqp.dpa_heap_memory);
	if (err) {
		log_error("Failed to call rpc, err(%d), rpc_ret(%ld)",
				  err, rpc_ret);
		if (flexio_err_status(dpa_thread->dpa_ctx->flexio_process)) {
			flexio_coredump_create(dpa_thread->dpa_ctx->flexio_process, "/images/flexio.core");
		}
		ret = -1;
		goto free_memory;
	}

	//log_info("vqp call rpc, vqp dpa_handle %lx, vqp idx %d\n",
	//			  vqp->dpa_vqp.dpa_heap_memory, vqp->qp_idx);

free_memory:
	free(eh_qp_data);
out:
	return ret;
}

static int vrdma_dpa_vqp_map_to_thread(struct vrdma_ctrl *ctrl, struct spdk_vrdma_qp *vqp,
											struct vrdma_dpa_thread_ctx *dpa_thread)
{
	struct vrdma_prov_vqp_init_attr init_attr = {};
	int ret = 0;
	
	ret = vrdma_dpa_mm_zalloc(dpa_thread->dpa_ctx->flexio_process,
					   	sizeof(struct vrdma_dpa_vqp_ctx),
					   	&(vqp->dpa_vqp.dpa_heap_memory));
	if (ret) {
		log_error("Failed to allocate dev buf, err(%d)", ret);
		return ret;
	}

	vqp->dpa_vqp.devx_emu_db_to_cq_ctx =
			mlx_devx_emu_db_to_cq_map(ctrl->emu_ctx, ctrl->sctrl->sdev->pci->mpci.vhca_id,
										vqp->qdb_idx, 
										flexio_cq_get_cq_num(dpa_thread->dpa_handler->db_cq.cq),
										&vqp->dpa_vqp.emu_db_to_cq_id);
	if (!vqp->dpa_vqp.devx_emu_db_to_cq_ctx || vqp->dpa_vqp.emu_db_to_cq_id >= MAX_VQP_NUM) {
		log_error("Failed to map cq_to_db, vhca_id %d, qdb_idx%d, cqn %d, emu_db_to_cq_id %d",
			   		ctrl->sctrl->sdev->pci->mpci.vhca_id, vqp->qdb_idx,
			   		flexio_cq_get_cq_num(dpa_thread->dpa_handler->db_cq.cq), 
			   		vqp->dpa_vqp.emu_db_to_cq_id);
		ret = -1;
		goto err_vqp_map;
	}
	
	memset(&init_attr, 0x0, sizeof(init_attr));
	vrdma_set_dpa_vqp_attr(&init_attr, ctrl, vqp);
	vrdma_dpa_vq_ctx_init(dpa_thread, &init_attr, vqp);
	vrdma_dpa_vq_dump_attribute(&init_attr, dpa_thread);
	vqp->dpa_vqp.dpa_thread = dpa_thread;
	dpa_thread->attached_vqp_num++;

	log_info("vqpn %d dpa addr %lx, succeed to map db_cq, vhca_id %d, qdb_idx  %d, cqn %d, emu ctx id %d",
			   		vqp->qp_idx, vqp->dpa_vqp.dpa_heap_memory, ctrl->sctrl->sdev->pci->mpci.vhca_id, vqp->qdb_idx,
			   		flexio_cq_get_cq_num(dpa_thread->dpa_handler->db_cq.cq), 
			   		vqp->dpa_vqp.emu_db_to_cq_id);
	return 0;

err_vqp_map:
	vrdma_dpa_mm_free(dpa_thread->dpa_ctx->flexio_process, 
						vqp->dpa_vqp.dpa_heap_memory);
	return ret;

}

static struct vrdma_dpa_handler *
vrdma_dpa_handler_create(struct vrdma_ctrl *ctrl, struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
									struct spdk_vrdma_qp *vqp,
									struct snap_vrdma_vq_create_attr *q_attr)
{
	struct vrdma_dpa_handler *dpa_handler;
	struct vrdma_dpa_ctx *dpa_ctx = emu_dev_ctx->dpa_ctx;
	struct vrdma_msix_init_attr msix_attr = {};
	int err = 0;

	dpa_handler = calloc(1, sizeof(*dpa_handler));
	if (!dpa_handler) {
		log_error("create dpa handler: no memory\n");
		return NULL;
	}
	
	err = vrdma_dpa_db_handler_init(dpa_ctx, dpa_handler, 
									"vrdma_db_handler", "vrdma_msix_handler");
	if (err) {
		log_error("Failed to init db handler, err(%d)", err);
		goto err_vq_init;
	}
	
	err = vrdma_dpa_db_cq_create(emu_dev_ctx->flexio_process, ctrl->emu_ctx,
								dpa_handler->db_handler, 
								&dpa_handler->db_cq,
								dpa_ctx->emu_uar->page_id);
	if (err) {
		log_error("Failed to create db_cq, err(%d)", err);
		goto err_db_cq_create;
	}
	
	msix_attr.emu_ib_ctx  = ctrl->emu_ctx;
	msix_attr.emu_vhca_id = ctrl->sctrl->sdev->pci->mpci.vhca_id;
	msix_attr.sf_ib_ctx   = ctrl->emu_ctx;
	msix_attr.sf_vhca_id  = ctrl->sf_vhca_id;
	msix_attr.msix_vector = vqp->sq_vcq->veq->vector_idx;
	msix_attr.cq_size	  = q_attr->sq_size;
	
	err = vrdma_dpa_msix_create(emu_dev_ctx, &msix_attr,
						  ctrl->sctrl->bar_curr->num_msix);
	if (err) {
		log_error("Failed to create vq msix, err(%d)", err);
		goto err_sq_msix_create;
	}
	dpa_handler->sq_msix_vector = vqp->sq_vcq->veq->vector_idx;
	
	if (vqp->sq_vcq->veq->vector_idx != vqp->rq_vcq->veq->vector_idx) {
		msix_attr.msix_vector = vqp->rq_vcq->veq->vector_idx;
		err = vrdma_dpa_msix_create(emu_dev_ctx, &msix_attr,
									ctrl->sctrl->bar_curr->num_msix);
		if (err) {
			log_error("Failed to create vq msix, err(%d)", err);
			goto err_rq_msix_create;
		}
	}
	dpa_handler->rq_msix_vector = vqp->rq_vcq->veq->vector_idx;
	return dpa_handler;

err_rq_msix_create:
	vrdma_dpa_msix_destroy(dpa_handler->sq_msix_vector, emu_dev_ctx);
err_sq_msix_create:
	vrdma_dpa_db_cq_destroy(dpa_handler, emu_dev_ctx);
err_db_cq_create:
	vrdma_dpa_db_handler_uninit(dpa_handler);
err_vq_init:
	free(dpa_handler);
	return NULL;
}

static void
vrdma_dpa_handler_destroy(struct vrdma_dpa_handler *dpa_handler,
									struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	if (!dpa_handler) {
		return;
	}
	
	vrdma_dpa_msix_destroy(dpa_handler->sq_msix_vector, emu_dev_ctx);
	if (dpa_handler->sq_msix_vector != dpa_handler->rq_msix_vector) {
		vrdma_dpa_msix_destroy(dpa_handler->rq_msix_vector, emu_dev_ctx);
	}
	vrdma_dpa_db_cq_destroy(dpa_handler, emu_dev_ctx);
	vrdma_dpa_db_handler_uninit(dpa_handler);
	free(dpa_handler);
}

static struct snap_vrdma_queue *
vrdma_sw_dma_qp_create(struct vrdma_ctrl *ctrl, struct spdk_vrdma_qp *vqp,
								struct snap_vrdma_vq_create_attr* q_attr)
{
	struct snap_vrdma_queue *sw_dma_qp;
	struct snap_dma_q_create_attr rdma_qp_create_attr = {};

	sw_dma_qp = calloc(1, sizeof(*sw_dma_qp));
	if (!sw_dma_qp) {
		log_error("create queue %d: no memory\n", q_attr->vqpn);
		return NULL;
	}

	/* create dma_qp in arm */	
	rdma_qp_create_attr.tx_qsize = q_attr->sq_size;
	rdma_qp_create_attr.rx_qsize = q_attr->rq_size;
	rdma_qp_create_attr.tx_elem_size = q_attr->tx_elem_size;
	rdma_qp_create_attr.rx_elem_size = q_attr->rx_elem_size;
	rdma_qp_create_attr.rx_cb = q_attr->rx_cb;
	rdma_qp_create_attr.uctx = sw_dma_qp;
	rdma_qp_create_attr.mode = SNAP_DMA_Q_MODE_DV;
	rdma_qp_create_attr.use_devx = true;

	sw_dma_qp->dma_q = snap_dma_ep_create(ctrl->pd, &rdma_qp_create_attr);
	if (!sw_dma_qp->dma_q) {
		log_error("Failed creating SW QP\n");
		free(sw_dma_qp);
		return NULL;
	}
	return sw_dma_qp;
}

static void
vrdma_sw_dma_qp_destroy(struct snap_vrdma_queue *sw_dma_qp)
{
	if (!sw_dma_qp) {
		return;
	}
	if (sw_dma_qp->dma_q) {
		snap_dma_ep_destroy(sw_dma_qp->dma_q);
	}
	free(sw_dma_qp);
}

static struct vrdma_dpa_dma_qp *
vrdma_dpa_dma_qp_create(struct vrdma_ctrl *ctrl, struct spdk_vrdma_qp *vqp,
								struct vrdma_dpa_thread_ctx *dpa_thread,
								struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
								struct snap_vrdma_vq_create_attr* q_attr)
{
	struct vrdma_dpa_dma_qp *dpa_dma_qp;
	struct vrdma_dpa_dma_qp_attr attr = {};
	uint32_t qpsq_cqnum;
	uint32_t qprq_cqnum;
	int err;
	
	dpa_dma_qp = calloc(1, sizeof(*dpa_dma_qp));
	if (!dpa_dma_qp) {
		log_error("create queue %d: no memory\n", q_attr->vqpn);
		return NULL;
	}
	err = vrdma_dpa_dma_q_cq_create(dpa_dma_qp, dpa_thread->dpa_handler, ctrl->emu_ctx,
									emu_dev_ctx, q_attr);
	if (err) {
		log_error("Failed creating dma_q cq, err(%d)", err);
		goto err_dma_q_cq_create;
	}

	qprq_cqnum = flexio_cq_get_cq_num(dpa_dma_qp->dma_q_rqcq.cq);
	qpsq_cqnum = flexio_cq_get_cq_num(dpa_dma_qp->dma_q_sqcq.cq);
	attr.tisn_or_qpn = dpa_thread->sw_dma_qp->dma_q->sw_qp.qp->devx_qp.devx.id;
	attr.sf_pd = ctrl->pd;
	attr.tx_qsize     = q_attr->sq_size;
	attr.rx_qsize     = q_attr->rq_size;
	attr.tx_elem_size = q_attr->tx_elem_size;
	attr.rx_elem_size = q_attr->rx_elem_size;
	err = vrdma_dpa_dma_q_create(dpa_dma_qp, emu_dev_ctx, &attr, 
				       			qprq_cqnum, qpsq_cqnum);
	if (err) {
		log_error("Failed to create QP, err(%d)", err);
		goto err_dma_q_create;
	}
	return dpa_dma_qp;
	
err_dma_q_create:
	vrdma_dpa_dma_q_cq_destroy(dpa_dma_qp, emu_dev_ctx);
err_dma_q_cq_create:
	free(dpa_dma_qp);
	return NULL;
}

static void
vrdma_dpa_dma_qp_destroy(struct vrdma_dpa_dma_qp *dpa_dma_qp,
									struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	if (!dpa_dma_qp) {
		return;
	}
	if (dpa_dma_qp->qp)
		flexio_qp_destroy(dpa_dma_qp->qp);
	// vrdma_dpa_mkey_destroy(dpa_vq);
	if (dpa_dma_qp->sqd_mkey)
		flexio_device_mkey_destroy(dpa_dma_qp->sqd_mkey);
	vrdma_dpa_mm_free(emu_dev_ctx->flexio_process, dpa_dma_qp->tx_wqe_buff);
	if (dpa_dma_qp->rqd_mkey)
		flexio_device_mkey_destroy(dpa_dma_qp->rqd_mkey);
	vrdma_dpa_mm_free(emu_dev_ctx->flexio_process, dpa_dma_qp->rx_wqe_buff);
	vrdma_dpa_mm_free(emu_dev_ctx->flexio_process, dpa_dma_qp->dbr_daddr);
	vrdma_dpa_mm_qp_buff_free(emu_dev_ctx->flexio_process, dpa_dma_qp->buff_daddr);
	vrdma_dpa_dma_q_cq_destroy(dpa_dma_qp, emu_dev_ctx);
	free(dpa_dma_qp);
	
}

static inline void 
vrdma_set_dpa_thread_attr(struct vrdma_prov_thread_init_attr *attr,
								struct snap_vrdma_vq_create_attr *q_attr,
								struct vrdma_ctrl *ctrl)
{
	attr->dma_tx_qsize = q_attr->sq_size;
	attr->dma_tx_elem_size = q_attr->tx_elem_size;
	attr->dma_rx_qsize = q_attr->rq_size;
	attr->dma_rx_elem_size = q_attr->rx_elem_size;

	attr->emu_ib_ctx  = ctrl->emu_ctx;
	attr->emu_pd      = ctrl->pd;
	attr->emu_mkey    = ctrl->sctrl->xmkey->mkey;
	attr->emu_vhca_id = ctrl->sctrl->sdev->pci->mpci.vhca_id;

	attr->sf_ib_ctx   = ctrl->emu_ctx;
	attr->sf_pd       = ctrl->pd;
	attr->sf_mkey     = ctrl->sctrl->xmkey->mkey;
	attr->sf_vhca_id  = ctrl->sf_vhca_id;

	attr->num_msix = ctrl->sctrl->bar_curr->num_msix;
}

static struct vrdma_dpa_thread_ctx *
vrdma_dpa_thread_ctx_get_create(struct vrdma_ctrl *ctrl, struct spdk_vrdma_qp *vqp,
		    							struct snap_vrdma_vq_create_attr *q_attr)
{
	struct vrdma_prov_thread_init_attr attr = {};
	struct vrdma_dpa_thread_ctx *dpa_thread;
	int rc;

	dpa_thread = &g_dpa_threads[vqp->qp_idx % MAX_DPA_THREAD];
	//log_info("vqp index %d, dpa thread(%x) attached vqp num %d\n", 
	//		vqp->qp_idx, dpa_thread, dpa_thread->attached_vqp_num);
	if (dpa_thread->attached_vqp_num) {
		return dpa_thread;
	}

	vrdma_set_dpa_thread_attr(&attr, q_attr, ctrl);
	memset(dpa_thread, 0, sizeof(*dpa_thread));
	dpa_thread->thread_idx = vqp->qp_idx % MAX_DPA_THREAD;
	dpa_thread->dpa_ctx = ctrl->dpa_ctx;
	dpa_thread->emu_dev_ctx = ctrl->dpa_emu_dev_ctx;
	dpa_thread->sf_mkey = ctrl->sctrl->xmkey->mkey;
	dpa_thread->emu_mkey = ctrl->sctrl->xmkey->mkey;
	vrdma_dpa_handler_ctx_init(dpa_thread);
	dpa_thread->dpa_handler = vrdma_dpa_handler_create(ctrl, dpa_thread->emu_dev_ctx, vqp, q_attr);
	if (!dpa_thread->dpa_handler) {
		log_error("Failed to create dpa handler");
		return NULL;
	}
	dpa_thread->sw_dma_qp = vrdma_sw_dma_qp_create(ctrl, vqp, q_attr);
	if (!dpa_thread->sw_dma_qp) {
		log_error("Failed to create sw dma qp");
		goto err_create_sw_dma_qp;
	}
	dpa_thread->dpa_dma_qp = vrdma_dpa_dma_qp_create(ctrl, vqp, dpa_thread,
													dpa_thread->emu_dev_ctx, q_attr);
	if (!dpa_thread->dpa_dma_qp) {
		log_error("Failed to create dpa dma qp");
		goto err_create_dpa_dma_qp;
	}

	rc = vrdma_dpa_thread_ctx_init(dpa_thread, &attr);
	if (rc) {
		log_error("Failed to init event handler, err(%d)", rc);
		goto err_thread_ctx_init;
	}

	rc = flexio_event_handler_run(dpa_thread->dpa_handler->db_handler, dpa_thread->heap_memory);
	if (rc) {
		log_error("Failed to run event handler, err(%d)", rc);
		goto err_db_handler_run;
	}
#if 0
	rc = flexio_event_handler_run(dpa_thread->dpa_handler->rq_dma_q_handler, dpa_thread->heap_memory);
	if (rc) {
		log_error("Failed to run event handler, err(%d)", rc);
		goto err_rq_dma_q_handler_run;
	}
#endif

	/* Connect SW_QP to remote DPA qpn */
	rc = snap_dma_ep_connect_remote_qpn(dpa_thread->sw_dma_qp->dma_q,
										dpa_thread->dpa_dma_qp->qp_num);
	if (rc) {
		log_error("Failed to connect to remote qpn %d, err(%d)", 
					dpa_thread->dpa_dma_qp->qp_num, rc);
		goto err_ep_connect;
	}

	/* Post recv buffers on SW_QP */
	rc = snap_dma_q_post_recv(dpa_thread->sw_dma_qp->dma_q);
	if (rc)
		goto err_post_recv;

	vrdma_dpa_thread_dump_attribute(dpa_thread, q_attr, &attr);
	dpa_thread->sw_dma_qp->ctrl = ctrl->sctrl;
	dpa_thread->sw_dma_qp->idx = q_attr->vqpn; 
	/* Todo: later need confirm, because in snap_vrdma_vq_create: virtq->pd = q_attr->pd;*/
	dpa_thread->sw_dma_qp->pd = ctrl->pd;
	dpa_thread->sw_dma_qp->dma_mkey = ctrl->sctrl->xmkey->mkey;
	
	return dpa_thread;

err_post_recv:
err_ep_connect:
err_rq_dma_q_handler_run:
err_db_handler_run:
err_thread_ctx_init:
	vrdma_dpa_dma_qp_destroy(dpa_thread->dpa_dma_qp, dpa_thread->emu_dev_ctx);
err_create_dpa_dma_qp:
	vrdma_sw_dma_qp_destroy(dpa_thread->sw_dma_qp);
err_create_sw_dma_qp:
	vrdma_dpa_handler_destroy(dpa_thread->dpa_handler, dpa_thread->emu_dev_ctx);

	return NULL;
}

static void
vrdma_dpa_thread_ctx_destroy(struct vrdma_dpa_thread_ctx *dpa_thread)
{
	vrdma_dpa_dma_qp_destroy(dpa_thread->dpa_dma_qp, dpa_thread->emu_dev_ctx);
	vrdma_sw_dma_qp_destroy(dpa_thread->sw_dma_qp);
	vrdma_dpa_handler_destroy(dpa_thread->dpa_handler, dpa_thread->emu_dev_ctx);
	free(dpa_thread->eh_ctx);
	vrdma_dpa_mm_free(dpa_thread->dpa_ctx->flexio_process,
			    		dpa_thread->heap_memory);
}

static void vrdma_dpa_vqp_destroy(struct spdk_vrdma_qp *vqp)
{
	struct vrdma_dpa_thread_ctx *dpa_thread;

	if (!vqp || !vqp->dpa_vqp.dpa_thread) {
		return;
	}
	dpa_thread = vqp->dpa_vqp.dpa_thread;
	if (mlx_devx_emu_db_to_cq_unmap(vqp->dpa_vqp.devx_emu_db_to_cq_ctx)) {
		SPDK_ERRLOG("vqp index %d unmap db cq failed, dbr idx %d\n", 
					vqp->qp_idx, vqp->dpa_vqp.emu_db_to_cq_id);
		return;
	}
	vrdma_dpa_vq_ctx_release(vqp);
	vrdma_dpa_mm_free(dpa_thread->dpa_ctx->flexio_process, 
						vqp->dpa_vqp.dpa_heap_memory);
	vqp->dpa_vqp.dpa_thread->attached_vqp_num--;
	if (!dpa_thread->attached_vqp_num) {
		vrdma_dpa_thread_ctx_destroy(dpa_thread);
		memset(vqp->dpa_vqp.dpa_thread, 0, sizeof(*dpa_thread));
	}
}

static void generate_access_key(unsigned int seed, uint32_t *buf)
{
	int i;

	srand(seed);
	for (i = 0; i < VRDMA_ALIAS_ACCESS_KEY_NUM_DWORD; i++)
		buf[i] = (rand() % (UINT32_MAX - 1)) + 1;
}

static struct mlx5dv_devx_obj *
vrdma_create_alias_eq(struct ibv_context *emu_mgr_ibv_ctx,
			struct ibv_context *sf_ibv_ctx,
			uint16_t emu_mgr_vhca_id, uint32_t eqn,
			uint32_t *alias_eqn)
{
	uint32_t access_key[VRDMA_ALIAS_ACCESS_KEY_NUM_DWORD] = {};
	struct vrdma_allow_other_vhca_access_attr attr;
	struct mlx5dv_devx_obj *alias_obj = NULL;
	struct vrdma_alias_attr alias_attr;
	int i, err;

	attr.type = MLX5_OBJ_TYPE_EMULATED_DEV_EQ;
	attr.obj_id = eqn;
	generate_access_key(eqn, access_key);
	for (i = 0; i < VRDMA_ALIAS_ACCESS_KEY_NUM_DWORD; i++)
		attr.access_key_be[i] = htobe32(access_key[i]);

	err = mlx_devx_allow_other_vhca_access(emu_mgr_ibv_ctx, &attr);
	if (err) {
		log_error("Failed to allow access to object, err(%d)", err);
		return 0;
	}

	alias_attr.orig_vhca_id = emu_mgr_vhca_id;
	alias_attr.type = MLX5_OBJ_TYPE_EMULATED_DEV_EQ;
	alias_attr.orig_obj_id = eqn;
	for (i = 0; i < VRDMA_ALIAS_ACCESS_KEY_NUM_DWORD; i++)
		alias_attr.access_key_be[i] = htobe32(access_key[i]);

	alias_obj = mlx_devx_create_alias_obj(sf_ibv_ctx, &alias_attr, alias_eqn);
	return alias_obj;
}

static int
vrdma_dpa_alias_cq_create(struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			    struct vrdma_msix_init_attr *attr,
			    uint32_t alias_eqn)
{
	struct flexio_cq_attr cq_attr = {};
	int err;

	err = vrdma_dpa_mm_cq_alloc(emu_dev_ctx->flexio_process, log2(attr->cq_size),
			  &emu_dev_ctx->msix[attr->msix_vector].alias_cq);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		return err;
	}

	cq_attr.element_type = FLEXIO_CQ_ELEMENT_TYPE_DPA_MSIX_EMULATED_CQ;
	cq_attr.emulated_eqn = alias_eqn;
	cq_attr.uar_id = emu_dev_ctx->sf_uar->page_id;
	cq_attr.uar_base_addr = emu_dev_ctx->sf_uar->base_addr;
	cq_attr.cq_dbr_daddr =
		emu_dev_ctx->msix[attr->msix_vector].alias_cq.cq_dbr_daddr;
	cq_attr.cq_ring_qmem.daddr  =
		emu_dev_ctx->msix[attr->msix_vector].alias_cq.cq_ring_daddr;

	err = flexio_cq_create(emu_dev_ctx->flexio_process, attr->sf_ib_ctx, &cq_attr,
			&emu_dev_ctx->msix[attr->msix_vector].alias_cq.cq);
	if (err) {
		log_error("Failed to create alias_cq msix(%#x), err(%d)",
				attr->msix_vector, err);
		goto err_alias_cq_create;
	}

	emu_dev_ctx->msix[attr->msix_vector].cqn =
		flexio_cq_get_cq_num(emu_dev_ctx->msix[attr->msix_vector].alias_cq.cq);
	return 0;

err_alias_cq_create:
	vrdma_dpa_mm_cq_free(emu_dev_ctx->flexio_process,
			&emu_dev_ctx->msix[attr->msix_vector].alias_cq);
	return err;
}

static void
vrdma_dpa_alias_cq_destroy(struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			     uint16_t msix_vector)
{
	if (emu_dev_ctx->msix[msix_vector].cqn) {
		flexio_cq_destroy(emu_dev_ctx->msix[msix_vector].alias_cq.cq);
		vrdma_dpa_mm_cq_free(emu_dev_ctx->flexio_process,
				&emu_dev_ctx->msix[msix_vector].alias_cq);
	}
}

int vrdma_dpa_msix_create(struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx,
			    struct vrdma_msix_init_attr *attr,
			    int max_msix)
{
	uint32_t eqn = 0, alias_eqn = 0;
	int err;

	/* msix vector could be 0xFFFF for traffic vq when using
	 * dpdk based applications/driver. Bypass map creation in
	 * such case.
	 */
	if (attr->msix_vector == 0xFFFF) {
		log_notice("msix_vector %d",attr->msix_vector);
		return 0;
	}

	if (attr->msix_vector > max_msix) {
		log_error("Msix vector (%d) is out of range, max(%d)",
			  attr->msix_vector, max_msix);
		return -EINVAL;
	}

	/* If msix is already created, reuse it for this VQ as well */
	if (emu_dev_ctx->msix[attr->msix_vector].eqn &&
	    emu_dev_ctx->msix[attr->msix_vector].cqn) {
		log_notice("msix %#x, (reuse) eqn %#0x, cqn %#0x",
			  attr->msix_vector,
			  emu_dev_ctx->msix[attr->msix_vector].eqn,
			  emu_dev_ctx->msix[attr->msix_vector].cqn);
		atomic32_inc(&emu_dev_ctx->msix[attr->msix_vector].msix_refcount);
		return 0;
	}
	/* alias_cq->alias_eq->eq->msix_vector. */
	emu_dev_ctx->msix[attr->msix_vector].obj =
		mlx_devx_create_eq(attr->emu_ib_ctx, attr->emu_vhca_id,
				   attr->msix_vector, &eqn);
	if (!emu_dev_ctx->msix[attr->msix_vector].obj) {
		log_error("Failed to create devx eq, errno(%d)", errno);
		return -errno;
	}

	emu_dev_ctx->msix[attr->msix_vector].alias_eq_obj =
		vrdma_create_alias_eq(attr->emu_ib_ctx, attr->sf_ib_ctx,
					emu_dev_ctx->dpa_ctx->emu_mgr_vhca_id,
					eqn, &alias_eqn);
	if (!emu_dev_ctx->msix[attr->msix_vector].alias_eq_obj) {
		log_error("Failed to create devx alias eq, errno(%d)", errno);
		err = -errno;
		goto err_alias_eq_create;
	}

	emu_dev_ctx->msix[attr->msix_vector].eqn = alias_eqn;
	err = vrdma_dpa_alias_cq_create(emu_dev_ctx, attr, alias_eqn);
	if (err) {
		log_error("Failed to alloc cq memory, err(%d)", err);
		goto err_alias_cq_create;
	}
	atomic32_inc(&emu_dev_ctx->msix[attr->msix_vector].msix_refcount);

	//log_notice("msix %#x, devx_eqn %#x, alias_eqn %#x, alias_cqn %#x",
	//	  attr->msix_vector, eqn,
	//	  alias_eqn,
	//	  emu_dev_ctx->msix[attr->msix_vector].cqn);

	return 0;

err_alias_cq_create:
	mlx_devx_destroy_eq(emu_dev_ctx->msix[attr->msix_vector].alias_eq_obj);
err_alias_eq_create:
	mlx_devx_destroy_eq(emu_dev_ctx->msix[attr->msix_vector].obj);
	return err;
}


void vrdma_dpa_msix_destroy(uint16_t msix_vector,
			      struct vrdma_dpa_emu_dev_ctx *emu_dev_ctx)
{
	if ((msix_vector == 0xFFFF) ||
	    !atomic32_dec_and_test(&emu_dev_ctx->msix[msix_vector].msix_refcount))
		return;

	//log_notice("Destroy msix %#x, alias_eqn %#x, alias_cqn %#x",
	//	  msix_vector,
	//	  emu_dev_ctx->msix[msix_vector].eqn,
	//	  emu_dev_ctx->msix[msix_vector].cqn);

	vrdma_dpa_alias_cq_destroy(emu_dev_ctx, msix_vector);
	mlx_devx_destroy_eq(emu_dev_ctx->msix[msix_vector].alias_eq_obj);
	mlx_devx_destroy_eq(emu_dev_ctx->msix[msix_vector].obj);
	memset(&emu_dev_ctx->msix[msix_vector], 0,
	       sizeof(struct vrdma_dpa_msix));
}

static void vrdma_dpa_vq_dbg_stats_query(struct vrdma_dpa_thread_ctx *dpa_thread)
{
	struct vrdma_window_dev_config window_cfg = {};
	struct vrdma_dpa_vq_data *host_data;
	struct vrdma_dpa_ctx *dpa_ctx;
	flexio_uintptr_t dest_addr;
	uint64_t func_ret;
	int err;
	
	dpa_ctx = dpa_thread->dpa_ctx;
	host_data = dpa_ctx->vq_data;

	window_cfg.mkey = dpa_ctx->vq_data_mr->lkey;
	window_cfg.haddr = (uintptr_t)host_data;
	window_cfg.heap_memory = dpa_thread->heap_memory;
	err = flexio_copy_from_host(dpa_ctx->flexio_process,
				    &window_cfg,
				    sizeof(window_cfg),
				    &dest_addr);
	if (err) {
		log_error("Failed to copy from host, err(%d", err);
		return;
	}

	err = flexio_process_call(dpa_ctx->flexio_process,
				  dpa_ctx->dev2host_copy_func,
				  &func_ret, (uint64_t)dest_addr);
	if (err) {
		log_error("Failed to flexio_process_call, err(%d)", err);
		if (flexio_err_status(dpa_ctx->flexio_process)) {
			flexio_coredump_create(dpa_ctx->flexio_process, "/images/flexio.core");
		}
		return;
	}
	log_notice("dpa_qp debug count:");

	log_notice("counter[0] into rpc num: %d", host_data->ehctx.debug_data.counter[0]);
	log_notice("counter[1] out from rpc num: %d", host_data->ehctx.debug_data.counter[1]);
	log_notice("counter[2] into db num: %d", host_data->ehctx.debug_data.counter[2]);
	log_notice("counter[3] out db num: %d", host_data->ehctx.debug_data.counter[3]);
	log_notice("counter[4] qp exceeds budget: %d", host_data->ehctx.debug_data.counter[4]);
	log_notice("counter[5] get cqe num: %d", host_data->ehctx.debug_data.counter[5]);
	log_notice("counter[6] fetch qp num: %d", host_data->ehctx.debug_data.counter[6]);
	log_notice("counter[7] set dma qp ce bit num: %d", host_data->ehctx.debug_data.counter[7]);

	log_notice("value[0] total fetched wqes: %d", host_data->ehctx.debug_data.value[0]);
	log_notice("value[1] non dbr cqe num: %d", host_data->ehctx.debug_data.value[1]);
	log_notice("value[2] hw_qp_sq_pi: %d", host_data->ehctx.debug_data.value[2]);
	log_notice("value[3] guest_db_cq_ctx.cqn: %d", host_data->ehctx.debug_data.value[3]);
	log_notice("value[4] guest_db_cq_ctx.ci: %d", host_data->ehctx.debug_data.value[4]);
	log_notice("value[5] total_handled_wqe > 8k: %d", host_data->ehctx.debug_data.value[5]);
	log_notice("value[6] null_db_cqe_cnt > 6: %d", host_data->ehctx.debug_data.value[6]);
	log_notice("value[7] last emu ctx id: %d", host_data->ehctx.debug_data.value[7]);
	
}

static uint32_t vrdma_dpa_emu_db_to_cq_ctx_get_id(struct spdk_vrdma_qp *vqp)
{
	return vqp->dpa_vqp.emu_db_to_cq_id;
}

struct vrdma_vq_ops vrdma_dpa_vq_ops = {
	.ctx_get_create = vrdma_dpa_thread_ctx_get_create,
	.map_thread = vrdma_dpa_vqp_map_to_thread,
	// .modify = vrdma_dpa_vq_modify,
	.destroy = vrdma_dpa_vqp_destroy,
	.dbg_stats_query = vrdma_dpa_vq_dbg_stats_query,
	.get_emu_db_to_cq_id = vrdma_dpa_emu_db_to_cq_ctx_get_id,
};

