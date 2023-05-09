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

__FLEXIO_ENTRY_POINT_START

flexio_dev_arg_unpack_func_t vrdma_dpa_rpc_unpack_func;
uint64_t vrdma_dpa_rpc_unpack_func(void *arg_buf, void *func)
{
	uint64_t arg1 = *(uint64_t *)arg_buf;
	flexio_dev_rpc_handler_t *vrdma_rpc_handler = func;
	(*vrdma_rpc_handler)(arg1);
	return 0;
}

#if 0
flexio_dev_rpc_handler_t test_dpa_flexio_work;
uint64_t test_dpa_flexio_work(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	uint64_t res;
	TRACEVAL(arg1);
	TRACEVAL(arg2);
	TRACEVAL(arg3);

	res = arg1 + arg2 + arg3;

	// flexio_dev_print("DPA says %lu + %lu + %lu is %lu\n", arg1, arg2, arg3, res);
	printf("DPA says %lu + %lu + %lu is %lu\n", arg1, arg2, arg3, res);
	return res;
}

static inline int
vrdma_dpa_get_free_vqp_slot(struct vrdma_dpa_event_handler_ctx *ehctx)
{
	int i;

	for (i = 0; i < VQP_PER_THREAD; i++) {
		if (!ehctx->vqp_ctx[i].valid) {
			return i;
		}
	}
	
	return -1;
}
#endif

flexio_dev_rpc_handler_t vrdma_qp_rpc_handler;
uint64_t vrdma_qp_rpc_handler(uint64_t arg1)
{

	struct vrdma_dpa_event_handler_ctx *ectx;
	struct vrdma_dpa_vqp_ctx *vqp_ctx;
	struct flexio_dev_thread_ctx *dtctx;
	uint16_t free_idx;
	uint16_t valid = 0;
	
#ifdef VRDMA_RPC_TIMEOUT_ISSUE_DEBUG
	printf("\n vrdma_qp_rpc_handler start\n");
#endif
	flexio_dev_get_thread_ctx(&dtctx);
	vqp_ctx = (struct vrdma_dpa_vqp_ctx *)arg1;
	ectx = (struct vrdma_dpa_event_handler_ctx *)vqp_ctx->eh_ctx_daddr;
	vrdma_debug_count_set(ectx, 0);

	/* for vqp migration repost */
	if (vqp_ctx->mctx.field & (1 << VRDMA_DPA_VQP_MOD_REPOST_PI_BIT)) {
		vqp_ctx->sq_last_fetch_start = vqp_ctx->mctx.repost_pi;
		vqp_ctx->mctx.field &= ~(1 << VRDMA_DPA_VQP_MOD_REPOST_PI_BIT | 
								1 << VRDMA_DPA_VQP_MOD_STOP_FETCH_BIT);
		flexio_dev_db_ctx_arm(dtctx, ectx->guest_db_cq_ctx.cqn,
				      		vqp_ctx->emu_db_to_cq_id);
		flexio_dev_db_ctx_force_trigger(dtctx, ectx->guest_db_cq_ctx.cqn,
						vqp_ctx->emu_db_to_cq_id);
		return 0;
	}

	/* for vqp migration stop fetch */
	if (vqp_ctx->mctx.field & (1 << VRDMA_DPA_VQP_MOD_STOP_FETCH_BIT)) {
		flexio_dev_db_ctx_arm(dtctx, ectx->guest_db_cq_ctx.cqn,
				      		vqp_ctx->emu_db_to_cq_id);
		flexio_dev_db_ctx_force_trigger(dtctx, ectx->guest_db_cq_ctx.cqn,
						vqp_ctx->emu_db_to_cq_id);
		return 0;
	}

	/* insert new added vqp to thread ctx */
	free_idx = vqp_ctx->free_idx;
	if (!(vqp_ctx->free_idx < VQP_PER_THREAD)) {
		printf("vrdma_qp_rpc_handler wrong vqp slot %d\n", vqp_ctx->free_idx);
		return 0;
	}
	switch (vqp_ctx->state) {
			case VRDMA_DPA_VQ_STATE_RDY:
				valid = 1;
				break;
			case VRDMA_DPA_VQ_STATE_SUSPEND:
			case VRDMA_DPA_VQ_STATE_INIT:
			case VRDMA_DPA_VQ_STATE_ERR:
				valid = 0;
				break;
			default:
				break;
	}
	
	spin_lock(&ectx->vqp_array_lock);
#if 0
	free_idx = vrdma_dpa_get_free_vqp_slot(ectx);
	if (free_idx == -1) {
		printf("\n vrdma_qp_rpc_handler can not find free slot\n");
		spin_lock(&ectx->vqp_array_lock);
		return 0;
	}
#endif
	fence_rw();
	ectx->vqp_ctx[free_idx].emu_db_handle = vqp_ctx->emu_db_to_cq_id;
	ectx->vqp_ctx[free_idx].valid = valid;
	ectx->vqp_ctx[free_idx].vqp_ctx_handle = (flexio_uintptr_t)vqp_ctx;
	ectx->vqp_count++;
	ectx->dma_qp.state = VRDMA_DPA_VQ_STATE_RDY;
	ectx->vqp_ctx_hdl[vqp_ctx->emu_db_to_cq_id].vqp_ctx_handle = (flexio_uintptr_t)vqp_ctx;
	ectx->vqp_ctx_hdl[vqp_ctx->emu_db_to_cq_id].valid = valid;
	spin_unlock(&ectx->vqp_array_lock);
	//vrdma_debug_value_set(ectx, 7, vqp_ctx->emu_db_to_cq_id);
	flexio_dev_outbox_config(dtctx, ectx->emu_outbox);
	flexio_dev_db_ctx_arm(dtctx, ectx->guest_db_cq_ctx.cqn,
				      vqp_ctx->emu_db_to_cq_id);
	flexio_dev_cq_arm(dtctx, ectx->guest_db_cq_ctx.ci,
				  		ectx->guest_db_cq_ctx.cqn);

#ifdef VRDMA_RPC_TIMEOUT_ISSUE_DEBUG
	printf("\n vrdma_qp_rpc_handler cqn: %#x, emu_db_to_cq_id %d,"
		"guest_db_cq_ctx.ci %d, vqp cnt %d\n", ectx->guest_db_cq_ctx.cqn,
		vqp_ctx->emu_db_to_cq_id, ectx->guest_db_cq_ctx.ci,
		ectx->vqp_count);
#endif

#if 0

	flexio_dev_db_ctx_force_trigger(dtctx, ectx->guest_db_cq_ctx.cqn,
						vqp_ctx->emu_db_to_cq_id);

	if (ectx->vqp_count == 1) {
		flexio_dev_db_ctx_arm(dtctx, ectx->guest_db_cq_ctx.cqn,
				      vqp_ctx->emu_db_to_cq_id);
		flexio_dev_cq_arm(dtctx, ectx->guest_db_cq_ctx.ci,
				  		ectx->guest_db_cq_ctx.cqn);
		flexio_dev_db_ctx_force_trigger(dtctx, ectx->guest_db_cq_ctx.cqn,
						vqp_ctx->emu_db_to_cq_id);
	}
#endif
	vrdma_debug_count_set(ectx, 1);
#ifdef VRDMA_RPC_TIMEOUT_ISSUE_DEBUG
	printf("\n vrdma_qp_rpc_handler end\n");
#endif
	return 0;
}

flexio_dev_rpc_handler_t vrdma_dev2host_copy_handler;
uint64_t vrdma_dev2host_copy_handler(uint64_t arg1)
{
	struct vrdma_window_dev_config *window_cfg;
	struct vrdma_dpa_event_handler_ctx *ehctx;
	struct vrdma_dpa_vq_data *host_data;
	struct flexio_dev_thread_ctx *dtctx;

	window_cfg = (struct vrdma_window_dev_config *)arg1;
	ehctx = (struct vrdma_dpa_event_handler_ctx *)window_cfg->heap_memory;
	flexio_dev_get_thread_ctx(&dtctx);

	/* get window mkey*/
	flexio_dev_window_mkey_config(dtctx, window_cfg->mkey);

	/* acquire dev ptr to host memory */
	flexio_dev_window_ptr_acquire(dtctx, (flexio_uintptr_t)window_cfg->haddr,
				      (flexio_uintptr_t *)&host_data);

	memcpy(&host_data->ehctx, ehctx, sizeof(host_data->ehctx));
	return 0;
}

__FLEXIO_ENTRY_POINT_END
