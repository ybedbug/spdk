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

#include <libflexio-libc/string.h>
#include <libflexio-libc/stdio.h>
#include <libflexio-dev/flexio_dev.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-dev/flexio_dev_debug.h>
#include "../vrdma_dpa_common.h"
#include "vrdma_dpa_dev_com.h"
#include "vrdma_dpa_cq.h"

//#define DPA_LATENCY_TEST
//#define VRDMA_DPA_DEBUG_DETAIL
// #define DPA_COUNT

static int
get_next_qp_swqe_index(uint32_t pi, uint32_t depth)
{
	return (pi % depth);
}

static inline unsigned long
dpa_align(unsigned long val, unsigned long align)
{
	return (val + align - 1) & ~(align - 1);
}

static inline void
swqe_seg_ctrl_set_rdmaw(union flexio_dev_sqe_seg *swqe, uint32_t sq_pi,
						 uint32_t sq_number, uint32_t ce, uint16_t ds)
{
#if 0
	if (wqe_flag & VRDMA_DPA_WQE_WITH_IMM) {
		opcode = MLX5_CTRL_SEG_OPCODE_RDMA_WRITE_WITH_IMMEDIATE;
		swqe->ctrl.general_id = cpu_to_be32(imm);
	} else {
		opcode = MLX5_CTRL_SEG_OPCODE_RDMA_WRITE;
		swqe->ctrl.general_id = 0;
	}
#endif
	swqe->ctrl.general_id = 0;

	/* Fill out 1-st segment (Control) */
	swqe->ctrl.idx_opcode = cpu_to_be32(((sq_pi & 0xffff) << 8) | MLX5_CTRL_SEG_OPCODE_RDMA_WRITE);
	swqe->ctrl.qpn_ds = cpu_to_be32((sq_number << 8) | ds);
	swqe->ctrl.signature_fm_ce_se = cpu_to_be32(ce << 2);
}

static void
vrdma_dpa_process_sq_ci(struct vrdma_dpa_event_handler_ctx *ehctx)
{
	vrdma_dpa_cq_wait(&ehctx->dma_sqcq_ctx,
			    POW2MASK(ehctx->dma_sqcq_ctx.log_cq_depth),
			    &ehctx->dma_qp.hw_qp_sq_ci);

	ehctx->dma_qp.hw_qp_sq_ci++;

	flexio_dev_dbr_cq_set_ci(ehctx->dma_sqcq_ctx.dbr,
				 ehctx->dma_sqcq_ctx.ci);
}

static inline uint16_t
vrdma_dpa_set_inl_data_seg(struct flexio_dev_wqe_inline_data_seg *swqe,
										uint16_t pi)
{
	uint16_t len = sizeof(pi);
	uint32_t byte_cnt;

	byte_cnt = cpu_to_be32(len | DPA_INLINE_SEG);
	*(uint32_t *)swqe->inline_data = byte_cnt;
	*(uint16_t *)(swqe->inline_data + 4) = pi;
	return (dpa_align(len + sizeof(byte_cnt), 16) / 16);
}

static inline void 
vrdma_dev_swqe_seg_rdma_set(union flexio_dev_sqe_seg *swqe, uint32_t rkey,
						 				uint64_t raddr)
{
	swqe->rdma.raddr = cpu_to_be64(raddr);
	swqe->rdma.rkey = cpu_to_be32(rkey);
}

static inline void
vrdma_dev_swqe_seg_data_set(union flexio_dev_sqe_seg *swqe, uint32_t data_sz,
						 			uint32_t lkey, uint64_t data_addr)
{
	swqe->data.byte_count = cpu_to_be32(data_sz);
	swqe->data.lkey = cpu_to_be32(lkey);
	swqe->data.addr = cpu_to_be64(data_addr);
}

static inline uint16_t
vrdma_get_sq_free_wqe_num(struct vrdma_dpa_event_handler_ctx *ehctx)
{
	uint16_t outstanding_wqe = ehctx->dma_qp.hw_qp_sq_pi - ehctx->dma_qp.hw_qp_sq_ci;
	return (ehctx->dma_qp.hw_sq_size - outstanding_wqe);
}

static inline uint32_t
vrdma_get_ce_bits(struct vrdma_dpa_event_handler_ctx *ehctx)
{
	uint32_t ce;
	if (vrdma_get_sq_free_wqe_num(ehctx) == ehctx->ce_set_threshold) {
		ce = MLX5_CTRL_SEG_CE_CQE_ALWAYS;
		vrdma_debug_count_set(ehctx, 7);
	} else {
		ce = MLX5_CTRL_SEG_CE_CQE_ON_CQE_ERROR;
	}
	return ce;
}

#if 0
static inline unsigned long
DIV_ROUND_UP(unsigned long n, unsigned long d)
{
	return ((n) + (d) - 1) / (d);
}
#endif

static inline uint32_t vrdma_dpa_cpu_cyc_get(void)
{
	uint32_t cyc;
	asm volatile("rdcycle %0" : "=r"(cyc));
	return cyc;
}

static void
vrdma_dpa_set_dma_wqe(struct vrdma_dpa_event_handler_ctx *ehctx,
							uint32_t remote_key,
							uint64_t remote_addr,
							uint32_t local_key,
							uint64_t local_addr,
							uint32_t size, 
							uint16_t wr_pi,
							uint16_t wqe_flag)
{
	union flexio_dev_sqe_seg *swqe;
	struct flexio_dev_wqe_ctrl_seg *ctrl;
	int swqe_index;
	uint32_t ce = 0;

	ce = vrdma_get_ce_bits(ehctx);
	swqe_index = get_next_qp_swqe_index(ehctx->dma_qp.hw_qp_sq_pi,
					    				ehctx->dma_qp.hw_sq_size);
	swqe = (union flexio_dev_sqe_seg *)
			(ehctx->dma_qp.qp_sq_buff + (swqe_index * DPA_DMA_SEND_WQE_BB));
	ctrl = (struct flexio_dev_wqe_ctrl_seg *)swqe; 
	/* Fill out 2-nd segment (RDMA) */
	swqe++;
	vrdma_dev_swqe_seg_rdma_set(swqe, remote_key, remote_addr);
	/* Fill out 3-rd segment (local Data) */
	swqe++;
	if (wqe_flag & VRDMA_DPA_WQE_INLINE) {
		vrdma_dpa_set_inl_data_seg((struct flexio_dev_wqe_inline_data_seg *)swqe, wr_pi);
	} else {
		vrdma_dev_swqe_seg_data_set(swqe, size, local_key, local_addr);
	}
	/* Fill ctrl segment rdma write/rdma write immediately*/
	/* use magic num 3 is to decrease cycles in DPA */
	swqe_seg_ctrl_set_rdmaw((union flexio_dev_sqe_seg *)ctrl, ehctx->dma_qp.hw_qp_sq_pi,
							 ehctx->dma_qp.qp_num, ce, 3);
	/*pi is for each wqebb*/
	ehctx->dma_qp.hw_qp_sq_pi += 1; //DIV_ROUND_UP(3 * 16, DPA_DMA_SEND_WQE_BB);
}

#if 0
static void
vrdma_dpa_rq_wr_fetch(struct vrdma_dpa_event_handler_ctx *ehctx,
					struct vrdma_dpa_vqp_ctx *vqp_ctx,
					uint16_t rq_start_idx, uint16_t size, uint32_t vqp_idx,
					uint16_t imm_data_pi, uint16_t wqe_flag)
{
	uint32_t remote_key, local_key;
	uint64_t remote_addr, local_addr;
	uint16_t wqebb_size;

	/*notice: now both host and arm wqebb(wr) has same size and count*/
	//index = rq_start_pi % ehctx->dma_qp.host_vq_ctx.rq_wqebb_cnt;
	wqebb_size = vqp_ctx->host_vq_ctx.rq_wqebb_size;

	local_key  = vqp_ctx->host_vq_ctx.emu_crossing_mkey;
	local_addr = vqp_ctx->host_vq_ctx.rq_wqe_buff_pa +
		   			wqebb_size * rq_start_idx;

	remote_key   = vqp_ctx->arm_vq_ctx.rq_lkey;
	remote_addr  = vqp_ctx->arm_vq_ctx.rq_buff_addr +
		      		wqebb_size * rq_start_idx;

	vrdma_dpa_set_dma_wqe(ehctx, remote_key, remote_addr, local_key,
				local_addr, size * wqebb_size, imm_data_pi, wqe_flag, vqp_idx);
#ifdef VRDMA_DPA_DEBUG_DETAIL
	printf("rq: index %#x, wqebb_size %#x, size %#x, remote_key %#x, remote_addr %#lx,"
			"local_key %#x, local_addr %#lx\n imm_data_pi %#x\n",
			rq_start_idx, wqebb_size, size, remote_key, remote_addr, local_key, local_addr,
			imm_data_pi);
#endif
}
#endif

static void
vrdma_dpa_sq_wr_fetch(struct vrdma_dpa_event_handler_ctx *ehctx,
					struct vrdma_dpa_vqp_ctx *vqp_ctx,
					uint16_t sq_start_idx, uint16_t size,
					uint16_t imm_data_pi, uint16_t wqe_flag)
{
	uint32_t remote_key, local_key;
	uint64_t remote_addr, local_addr;
	uint16_t wqebb_size;

	/*notice: now both host and arm wqebb(wr) has same size and count*/
	wqebb_size = vqp_ctx->host_vq_ctx.sq_wqebb_size;

	local_key  = vqp_ctx->host_vq_ctx.emu_crossing_mkey;
	local_addr = vqp_ctx->host_vq_ctx.sq_wqe_buff_pa +
		      		wqebb_size * sq_start_idx;

	remote_key   = vqp_ctx->arm_vq_ctx.sq_lkey;
	remote_addr  = vqp_ctx->arm_vq_ctx.sq_buff_addr +
		      		wqebb_size * sq_start_idx;

	vrdma_dpa_set_dma_wqe(ehctx, remote_key, remote_addr, local_key,
				local_addr, size * wqebb_size, imm_data_pi, wqe_flag);
#ifdef VRDMA_DPA_DEBUG_DETAIL
	printf("sq: index %#x, wqebb_size %#x, size %#x, remote_key %#x, remote_addr %#lx,"
			"local_key %#x, local_addr %#lx\n imm_data_pi %#x\n",
			sq_start_idx, wqebb_size, size, remote_key, remote_addr, local_key, local_addr,
			imm_data_pi);
#endif
}

static inline void
vrdma_dpa_sq_update_pi(struct vrdma_dpa_event_handler_ctx *ehctx,
					struct vrdma_dpa_vqp_ctx *vqp_ctx, uint16_t pi,
					uint16_t wqe_flag)
{
	uint32_t remote_key;
	uint64_t remote_addr;

	remote_key   = vqp_ctx->arm_vq_ctx.sq_lkey;
	remote_addr  = vqp_ctx->arm_vq_ctx.sq_pi_addr;

	vrdma_dpa_set_dma_wqe(ehctx, remote_key, remote_addr, 
							0, 0, 0, pi, wqe_flag);
}

static inline int
vrdma_vq_dpa_rollback(uint16_t pre_pi, uint16_t pi, uint16_t q_size)
{
		if (pi % q_size == 0) {
			return 0;
		}
		return !(pi % q_size > pre_pi % q_size);
}

#if 0
static void
vrdma_dpa_rq_process(struct vrdma_dpa_event_handler_ctx *ehctx,
							struct vrdma_dpa_vqp_ctx *vqp_ctx, uint16_t pi,
							uint16_t last_pi)
{
	uint16_t fetch_size;
	uint16_t wqebb_cnt;
	uint16_t wqe_flag = 0;

	wqebb_cnt = vqp_ctx->host_vq_ctx.rq_wqebb_cnt;

	if (!vrdma_vq_dpa_rollback(last_pi, pi, wqebb_cnt)) {
		vrdma_dpa_rq_wr_fetch(ehctx, vqp_ctx, last_pi % wqebb_cnt, pi - last_pi,
								vqp_ctx->vq_index, pi, wqe_flag);
	} else {
		fetch_size = wqebb_cnt - last_pi % wqebb_cnt;
		vrdma_dpa_rq_wr_fetch(ehctx, vqp_ctx, last_pi % wqebb_cnt, fetch_size, 
								vqp_ctx->vq_index, 0, wqe_flag);
		fetch_size = pi % wqebb_cnt;
		vrdma_dpa_rq_wr_fetch(ehctx, vqp_ctx, 0, fetch_size, vqp_ctx->vq_index, pi, wqe_flag);
	}

}
#endif

static void
vrdma_dpa_sq_process(struct vrdma_dpa_event_handler_ctx *ehctx,
							struct vrdma_dpa_vqp_ctx *vqp_ctx, uint16_t pi,
							uint16_t last_pi)
{
	uint16_t fetch_size;
	uint16_t wqebb_cnt;
	uint16_t wqe_flag = 0;
		
	wqebb_cnt = vqp_ctx->host_vq_ctx.sq_wqebb_cnt;

	if (!vrdma_vq_dpa_rollback(last_pi, pi, wqebb_cnt)) {
		vrdma_dpa_sq_wr_fetch(ehctx, vqp_ctx, last_pi % wqebb_cnt, pi - last_pi,
								pi, wqe_flag);
	} else {
		fetch_size = wqebb_cnt - last_pi % wqebb_cnt;
		vrdma_dpa_sq_wr_fetch(ehctx, vqp_ctx, last_pi % wqebb_cnt, fetch_size, 
								0, wqe_flag);
		fetch_size = pi % wqebb_cnt;
		vrdma_dpa_sq_wr_fetch(ehctx, vqp_ctx, 0, fetch_size, pi, wqe_flag);
	}

	wqe_flag |= VRDMA_DPA_WQE_INLINE;
	vrdma_dpa_sq_update_pi(ehctx, vqp_ctx, pi, wqe_flag);
}

static flexio_uintptr_t
vrdma_dpa_get_vqp_ctx(struct vrdma_dpa_event_handler_ctx *ehctx,
								uint32_t emu_db_handle)
{
	uint16_t i;

	spin_lock(&ehctx->vqp_array_lock);
	for (i = 0; i < VQP_PER_THREAD; i++) {
		if (ehctx->vqp_ctx[i].valid && 
			ehctx->vqp_ctx[i].emu_db_handle == emu_db_handle) {
			spin_unlock(&ehctx->vqp_array_lock);
			return ehctx->vqp_ctx[i].vqp_ctx_handle;
		}
	}
	spin_unlock(&ehctx->vqp_array_lock);
	return (flexio_uintptr_t)NULL;
}

static void
vrdma_dpa_handle_dma_cqe(struct vrdma_dpa_event_handler_ctx *ehctx)
{
	uint16_t sq_free_wqe_num;
	
	sq_free_wqe_num = vrdma_get_sq_free_wqe_num(ehctx);
	if (sq_free_wqe_num < VRDMA_CQ_WAIT_THRESHOLD(POW2(ehctx->dma_sqcq_ctx.log_cq_depth))) {
		vrdma_dpa_process_sq_ci(ehctx);
#ifdef VRDMA_DPA_DEBUG_DETAIL
		printf("need to wait dma cqe, sq_free_wqe_num %d, dma.sq.pi %d, dma.sq.ci %d\n",
			 sq_free_wqe_num, ehctx->dma_qp.hw_qp_sq_pi, ehctx->dma_qp.hw_qp_sq_ci);
#endif
	}
}								

static int
vrdma_dpa_handle_one_vqp(struct flexio_dev_thread_ctx *dtctx,
								struct vrdma_dpa_event_handler_ctx *ehctx,
								flexio_uintptr_t vqp_daddr)
{
	struct vrdma_dpa_vqp_ctx *vqp_ctx;
	uint16_t rq_pi = 0, rq_pi_last = 0;
	uint16_t sq_pi = 0, sq_pi_last = 0;
	//uint16_t each_loop_wqe = 0;
	uint16_t total_wqe = 0;
	uint16_t wqe_loop = 0;
	uint32_t start_cycles, end_cycles;
	uint32_t avr_cycles;

	vqp_ctx = (struct vrdma_dpa_vqp_ctx *)vqp_daddr;
	//ehctx = (struct vrdma_dpa_event_handler_ctx *)vqp_ctx->eh_ctx_daddr;

#ifdef VRDMA_DPA_DEBUG
	printf("vq_idx %d, window_base_addr %#x, emu_outbox %d, emu_crossing_mkey %d\n",
			vqp_ctx->vq_index, ehctx->window_base_addr, ehctx->emu_outbox, 
			vqp_ctx->host_vq_ctx.emu_crossing_mkey);
	printf("rq_wqe_buff_pa %#lx, rq_pi_paddr %#lx, rq_wqebb_cnt %#x,"
			"rq_wqebb_size %#x, sq_wqe_buff_pa %#lx, sq_pi_paddr %#lx,"
			"sq_wqebb_cnt %#x, sq_wqebb_size %#lx, emu_crossing_mkey %#x,"
			"sf_crossing_mkey %#x\n",
			vqp_ctx->host_vq_ctx.rq_wqe_buff_pa, vqp_ctx->host_vq_ctx.rq_pi_paddr,
			vqp_ctx->host_vq_ctx.rq_wqebb_cnt, vqp_ctx->host_vq_ctx.rq_wqebb_size,
			vqp_ctx->host_vq_ctx.sq_wqe_buff_pa, vqp_ctx->host_vq_ctx.sq_pi_paddr,
			vqp_ctx->host_vq_ctx.sq_wqebb_cnt, vqp_ctx->host_vq_ctx.sq_wqebb_size,
			vqp_ctx->host_vq_ctx.emu_crossing_mkey, vqp_ctx->host_vq_ctx.sf_crossing_mkey);
#endif
	rq_pi_last = vqp_ctx->rq_last_fetch_start;
	sq_pi_last = vqp_ctx->sq_last_fetch_start;

	fence_rw();
	rq_pi = *(uint16_t*)(ehctx->window_base_addr + vqp_ctx->host_vq_ctx.rq_pi_paddr);
	sq_pi = *(uint16_t*)(ehctx->window_base_addr + vqp_ctx->host_vq_ctx.sq_pi_paddr);
	start_cycles = vrdma_dpa_cpu_cyc_get();

	//while ((rq_pi_last != rq_pi) ||
	while(sq_pi_last != sq_pi) {
	//while (1) {
#if 0
		if (rq_pi_last != rq_pi) {
			vrdma_dpa_rq_process(ehctx, vqp_ctx, rq_pi, rq_pi_last);
			total_wqe += each_loop_wqe = rq_pi - rq_pi_last;
			rq_pi_last = rq_pi;
		}
#endif
		
		//if (sq_pi_last != sq_pi) {
			vrdma_dpa_sq_process(ehctx, vqp_ctx, sq_pi, sq_pi_last);
			total_wqe += sq_pi - sq_pi_last;
			sq_pi_last = sq_pi;
		//}
		//vrdma_debug_count_set(ehctx, 6);
		//if (each_loop_wqe) {
			flexio_dev_dbr_sq_set_pi((uint32_t *)ehctx->dma_qp.dbr_daddr + 1,
								ehctx->dma_qp.hw_qp_sq_pi);
			flexio_dev_qp_sq_ring_db(dtctx, ehctx->dma_qp.hw_qp_sq_pi,
								ehctx->dma_qp.qp_num);
			//each_loop_wqe = 0;
		//}
		
		if (total_wqe > VRDMA_VQP_HANDLE_BUDGET || 
			wqe_loop++ > 32) {
			break;
		}
#if 0		
		if(handled_wqe >= VRDMA_VQP_HANDLE_BUDGET) {
			//vrdma_debug_count_set(ehctx, 4);
			flexio_dev_db_ctx_arm(dtctx, ehctx->guest_db_cq_ctx.cqn,
			      				vqp_ctx->emu_db_to_cq_id);
			flexio_dev_db_ctx_force_trigger(dtctx, ehctx->guest_db_cq_ctx.cqn,
											vqp_ctx->emu_db_to_cq_id);
			goto out;
		}
#endif
		fence_rw();
		sq_pi = *(uint16_t*)(ehctx->window_base_addr + vqp_ctx->host_vq_ctx.sq_pi_paddr);

	}
	end_cycles = vrdma_dpa_cpu_cyc_get();
	avr_cycles = (end_cycles - start_cycles) / total_wqe;
	flexio_dev_db_ctx_arm(dtctx, ehctx->guest_db_cq_ctx.cqn,
			      			vqp_ctx->emu_db_to_cq_id);
	flexio_dev_db_ctx_force_trigger(dtctx, ehctx->guest_db_cq_ctx.cqn,
											vqp_ctx->emu_db_to_cq_id);
#ifdef VRDMA_DPA_DEBUG
    if (total_wqe) {
		printf("\n handled_wqe %d, total_cyc %d, wqe_loop %d \n", 
				total_wqe, (end_cycles - start_cycles), wqe_loop);
    }
#endif

#ifdef VRDMA_DPA_DEBUG
	printf("\n rq_pi %d, sq_pi %d\n", rq_pi, sq_pi);
	printf("\n dma_qp.hw_qp_sq_pi %d\n", ehctx->dma_qp.hw_qp_sq_pi);
	printf("\n vrdma_db_handler done. cqn: %#x, emu_db_to_cq_id %d, guest_db_cq_ctx.ci %d\n",
		ehctx->guest_db_cq_ctx.cqn, vqp_ctx->emu_db_to_cq_id, ehctx->guest_db_cq_ctx.ci);
#endif
	vqp_ctx->rq_last_fetch_start = rq_pi;
	vqp_ctx->sq_last_fetch_start = sq_pi;

	return total_wqe;

#if 0
	//flexio_dev_db_ctx_arm(dtctx, ehctx->guest_db_cq_ctx.cqn, vqp_ctx->emu_db_to_cq_id);
	fence_rw();
	rq_pi = *(uint16_t*)(ehctx->window_base_addr + vqp_ctx->host_vq_ctx.rq_pi_paddr);
	sq_pi = *(uint16_t*)(ehctx->window_base_addr + vqp_ctx->host_vq_ctx.sq_pi_paddr);

	if ((rq_pi_last != rq_pi) || (sq_pi_last != sq_pi)) {
		vrdma_debug_count_set(ehctx, 4);
		flexio_dev_db_ctx_force_trigger(dtctx, ehctx->guest_db_cq_ctx.cqn,
										vqp_ctx->emu_db_to_cq_id);
	}
#endif
}

#if 0
static int 
vrdma_dpa_handle_actived_vqp(struct flexio_dev_thread_ctx * dtctx,
										struct vrdma_dpa_event_handler_ctx *ehctx)
{
	uint16_t vqp_idx;
	uint32_t handled_wqe = 0;
	flexio_uintptr_t vqp_daddr;

	for (vqp_idx = 0; vqp_idx < VQP_PER_THREAD; vqp_idx++) {
		if (ehctx->vqp_ctx[vqp_idx].valid) {
			vqp_daddr = ehctx->vqp_ctx[vqp_idx].vqp_ctx_handle;
			handled_wqe += vrdma_dpa_handle_one_vqp(dtctx, ehctx, vqp_daddr);
		}
	}
	return handled_wqe;
}
#endif

__FLEXIO_ENTRY_POINT_START
flexio_dev_event_handler_t vrdma_db_handler;
void vrdma_db_handler(flexio_uintptr_t thread_arg)
{
	struct vrdma_dpa_event_handler_ctx *ehctx;
	struct vrdma_dpa_vqp_ctx *vqp_ctx;
	struct flexio_dev_thread_ctx *dtctx;
	uint16_t total_handled_wqe = 0;
	uint16_t null_db_cqe_cnt = 0;
	uint16_t handled_cqe_num = 0;
	struct vrdma_dev_cqe64 *db_cqe;
	uint32_t emu_db_handle;
#if 0
	uint16_t vqp_idx;
	flexio_uintptr_t vqp_daddr;
#endif
	uint8_t need_to_release = 0;

	flexio_dev_get_thread_ctx(&dtctx);
	ehctx = (struct vrdma_dpa_event_handler_ctx *)thread_arg;
#ifdef VRDMA_DPA_DEBUG
	printf("%s: virtq status %d.\n", __func__, ehctx->dma_qp.state);
#endif
	if (ehctx->dma_qp.state != VRDMA_DPA_VQ_STATE_RDY) {
		printf("%s: virtq status %d is not READY.\n", __func__, ehctx->dma_qp.state);
		//goto err_state;
	}
	vrdma_debug_count_set(ehctx, 2);
	flexio_dev_outbox_config(dtctx, ehctx->emu_outbox);
	flexio_dev_window_mkey_config(dtctx,
				      ehctx->emu_crossing_mkey);
	flexio_dev_window_ptr_acquire(dtctx, 0,
		(flexio_uintptr_t *)&ehctx->window_base_addr);

	while (1)
	{
		fence_rw();
		db_cqe = (struct vrdma_dev_cqe64 *)vrdma_dpa_cqe_get(&ehctx->guest_db_cq_ctx, 
												POW2MASK(ehctx->guest_db_cq_ctx.log_cq_depth));
		if (db_cqe) {
			null_db_cqe_cnt = 0;
			handled_cqe_num++;
			vrdma_debug_count_set(ehctx, 5);
			emu_db_handle = be32_to_cpu(db_cqe->emu_db_handle);
			vqp_ctx = (struct vrdma_dpa_vqp_ctx *)vrdma_dpa_get_vqp_ctx(ehctx, emu_db_handle);
#ifdef VRDMA_DPA_DEBUG
			printf("%s: virtq emu db handler %d, vqp_idx %d.\n",
					__func__, emu_db_handle, vqp_ctx->vq_index);
#endif
			if (vqp_ctx) {
				total_handled_wqe += vrdma_dpa_handle_one_vqp(dtctx, ehctx, 
															(flexio_uintptr_t)vqp_ctx);
				
			} else {
				flexio_dev_db_ctx_force_trigger(dtctx,
												ehctx->guest_db_cq_ctx.cqn,
												emu_db_handle);
				vrdma_debug_count_set(ehctx, 6);
			}
		} else {
			null_db_cqe_cnt++;
		}
#if 0

		for (vqp_idx = 0; vqp_idx < VQP_PER_THREAD; vqp_idx++) {
			if (ehctx->vqp_ctx[vqp_idx].valid) {
				vqp_daddr = ehctx->vqp_ctx[vqp_idx].vqp_ctx_handle;
				total_handled_wqe += vrdma_dpa_handle_one_vqp(dtctx, ehctx, vqp_daddr);
			}
			if (total_handled_wqe > VRDMA_TOTAL_WQE_BUDGET) {
				need_to_release = 1;
				break;
			}
		}
#else

		if (handled_cqe_num > 8 || total_handled_wqe > VRDMA_TOTAL_WQE_BUDGET) {
			need_to_release = 1;
		}
#endif
		
		vrdma_dpa_handle_dma_cqe(ehctx);

		if (need_to_release) {
			vrdma_debug_value_add(ehctx, 5, 1);
			break;
		}
		if (null_db_cqe_cnt > 6) {
			//printf("total handled wqe %d.\n", total_handled_wqe);
			vrdma_debug_value_add(ehctx, 6, 1);
			break;
		}
	}

	vrdma_debug_value_add(ehctx, 0, total_handled_wqe);
	vrdma_debug_value_add(ehctx, 1, null_db_cqe_cnt);
	vrdma_debug_value_set(ehctx, 2, ehctx->dma_qp.hw_qp_sq_pi);
	vrdma_debug_value_set(ehctx, 3, ehctx->guest_db_cq_ctx.cqn);
	vrdma_debug_value_set(ehctx, 4, ehctx->guest_db_cq_ctx.ci);
	vrdma_debug_count_set(ehctx, 3);
#if 0
err_state:
	vrdma_dpa_db_cq_incr(&ehctx->guest_db_cq_ctx);
	flexio_dev_dbr_cq_set_ci(ehctx->guest_db_cq_ctx.dbr,
							ehctx->guest_db_cq_ctx.ci);
	flexio_dev_cq_arm(dtctx, ehctx->guest_db_cq_ctx.ci,
					ehctx->guest_db_cq_ctx.cqn);
#endif
	flexio_dev_reschedule();
}
__FLEXIO_ENTRY_POINT_END
