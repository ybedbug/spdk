/*
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <infiniband/verbs.h>

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "snap.h"
#include "snap_vrdma_ctrl.h"
#include "snap_vrdma_virtq.h"

#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_io_mgr.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_qp.h"
#include "vrdma_providers.h"

// #define NO_PERF_DEBUG
/* TODO: use a hash table or sorted list */
struct vrdma_lbk_qp_list_head vrdma_lbk_qp_list =
				LIST_HEAD_INITIALIZER(vrdma_lbk_qp_list);
struct vrdma_rbk_qp_list_head vrdma_rbk_qp_list =
				LIST_HEAD_INITIALIZER(vrdma_rbk_qp_list);

struct spdk_vrdma_qp *
find_spdk_vrdma_qp_by_idx(struct vrdma_ctrl *ctrl, uint32_t qp_idx)
{
	struct spdk_vrdma_qp *vqp = NULL;

	LIST_FOREACH(vqp, &ctrl->vdev->vqp_list, entry)
        if (vqp->qp_idx == qp_idx)
            break;
	return vqp;
}

struct vrdma_remote_bk_qp *
vrdma_find_rbk_qp_by_vqp(uint64_t remote_gid_ip, uint32_t remote_vqpn)
{
	struct vrdma_remote_bk_qp *rqp;

	LIST_FOREACH(rqp, &vrdma_rbk_qp_list, entry) {
	if (rqp->attr.comm.vqpn == remote_vqpn &&
		rqp->attr.comm.gid_ip == remote_gid_ip)
            return rqp;
    }
	return NULL;
}

struct vrdma_local_bk_qp *
vrdma_find_lbk_qp_by_vqp(uint64_t gid_ip, uint32_t vqp_idx)
{
	struct vrdma_local_bk_qp *lqp;

	LIST_FOREACH(lqp, &vrdma_lbk_qp_list, entry) {
	if (lqp->attr.comm.vqpn == vqp_idx &&
		lqp->attr.comm.gid_ip == gid_ip)
		return lqp;
    }
	return NULL;
}

void vrdma_del_bk_qp_list(void)
{
	struct vrdma_remote_bk_qp *rqp, *rqp_tmp;
	struct vrdma_local_bk_qp *lqp, *lqp_tmp;

	LIST_FOREACH_SAFE(rqp, &vrdma_rbk_qp_list, entry, rqp_tmp) {
		LIST_REMOVE(rqp, entry);
		free(rqp);
	}
	LIST_FOREACH_SAFE(lqp, &vrdma_lbk_qp_list, entry, lqp_tmp) {
		LIST_REMOVE(lqp, entry);
		free(lqp);
	}
}

static void vrdma_del_lbk_qp_from_list(struct vrdma_local_bk_qp *lqp)
{
	LIST_REMOVE(lqp, entry);
	free(lqp);
}

static int vrdma_add_lbk_qp_list(struct vrdma_ctrl *ctrl, uint32_t vqp_idx,
				struct vrdma_backend_qp *bk_qp)
{
	struct vrdma_local_bk_qp *lqp;
	struct vrdma_remote_bk_qp *rqp;

    lqp = calloc(1, sizeof(*lqp));
    if (!lqp) {
		SPDK_ERRLOG("Failed to allocate local qp memory for vqp %d",
			vqp_idx);
		return -1;
	}
	lqp->attr.comm.node_id = g_node_ip;
	lqp->attr.comm.dev_id = ctrl->sctrl->sdev->pci->mpci.vhca_id;
	lqp->attr.comm.vqpn = vqp_idx;
	lqp->attr.comm.gid_ip = ctrl->vdev->vrdma_sf.ip;
	memcpy(lqp->attr.comm.mac, ctrl->vdev->vrdma_sf.mac, 6);
	lqp->attr.core_id = bk_qp->poller_core;
	lqp->bk_qpn = bk_qp->bk_qp.qpnum;
	lqp->remote_gid_ip = ctrl->vdev->vrdma_sf.remote_ip;
	rqp = vrdma_find_rbk_qp_by_vqp(lqp->remote_gid_ip, bk_qp->remote_vqpn);
	if (rqp) {
		lqp->remote_qpn = rqp->bk_qpn;
		lqp->remote_node_id = rqp->attr.comm.node_id;
		lqp->remote_dev_id = rqp->attr.comm.dev_id;
	} else {
		lqp->remote_qpn = VRDMA_INVALID_QPN;
		/* Hardcode remote node according to rpc configuration message. */
		lqp->remote_node_id = g_node_rip;
		lqp->remote_dev_id = VRDMA_INVALID_DEVID;
	}
	lqp->bk_qp = bk_qp;
	bk_qp->remote_qpn = lqp->remote_qpn;
	LIST_INSERT_HEAD(&vrdma_lbk_qp_list, lqp, entry);
	SPDK_NOTICELOG("vqp %d remote_vqp %d "
		"bk_qpn 0x%x remote_qpn 0x%x remote_node_id 0x%lx "
		"remote_dev_id 0x%x remote_gid_ip 0x%lx \n",
		vqp_idx, bk_qp->remote_vqpn, lqp->bk_qpn,
		lqp->remote_qpn, lqp->remote_node_id,
		lqp->remote_dev_id, lqp->remote_gid_ip);
	return 0;
}
void vrdma_del_rbk_qp_from_list(struct vrdma_remote_bk_qp *rqp)
{
	LIST_REMOVE(rqp, entry);
	free(rqp);
}

int vrdma_add_rbk_qp_list(struct vrdma_ctrl *ctrl, uint64_t gid_ip,
		uint32_t vqp_idx, uint32_t remote_qpn,
		struct vrdma_remote_bk_qp_attr *qp_attr)
{
	struct vrdma_remote_bk_qp *rqp;
	struct vrdma_local_bk_qp *lqp;
	bool remote_ready = false;

	rqp = vrdma_find_rbk_qp_by_vqp(qp_attr->comm.gid_ip,
			qp_attr->comm.vqpn);
	if (rqp) {
		if (rqp->bk_qpn == remote_qpn &&
			!memcmp(&rqp->attr, qp_attr, sizeof(*qp_attr))) {
			SPDK_NOTICELOG("This remote vqp %d is already existed",
			qp_attr->comm.vqpn);
			return 0;
		} else {
			SPDK_NOTICELOG("Update this existed remote vqp %d old rbk_qpn 0x%x remote_qpn 0x%x",
			qp_attr->comm.vqpn, rqp->bk_qpn, remote_qpn);
			memcpy(&rqp->attr, qp_attr, sizeof(*qp_attr));
			rqp->bk_qpn = remote_qpn;
			goto update_lqp;
		}
	}
    rqp = calloc(1, sizeof(*rqp));
    if (!rqp) {
		SPDK_ERRLOG("Failed to allocate local qp memory for vqp %d",
			vqp_idx);
		return -1;
	}
	memcpy(&rqp->attr, qp_attr, sizeof(*qp_attr));
	rqp->bk_qpn = remote_qpn;
	LIST_INSERT_HEAD(&vrdma_rbk_qp_list, rqp, entry);
update_lqp:
	if (ctrl) {
		lqp = vrdma_find_lbk_qp_by_vqp(gid_ip, vqp_idx);
		if (lqp) {
			if (lqp->remote_qpn == VRDMA_INVALID_QPN) {
				lqp->remote_node_id = qp_attr->comm.node_id;
				lqp->remote_dev_id = qp_attr->comm.dev_id;
				lqp->remote_qpn = remote_qpn;
				lqp->bk_qp->remote_qpn = lqp->remote_qpn;
				lqp->remote_gid_ip = qp_attr->comm.gid_ip;
				SPDK_NOTICELOG("gid_ip 0x%lx vqp %d remote_vqp %d "
					"remote_qpn 0x%x node_id 0x%lx "
					"dev_id 0x%x remote_gid_ip 0x%lx rqp.qp_state %d "
					"modify_backend_qp_to_ready\n",
					gid_ip, vqp_idx, qp_attr->comm.vqpn, remote_qpn,
					lqp->remote_node_id, lqp->remote_dev_id, lqp->remote_gid_ip,
					rqp->attr.qp_state);
				if (rqp->attr.qp_state == SPDK_VRDMA_RPC_QP_READY)
					remote_ready = true;
				if (vrdma_modify_backend_qp_to_ready(ctrl, lqp->bk_qp, remote_ready)) {
					SPDK_ERRLOG("Failed to modify bankend qp 0x%x to ready\n",
					lqp->bk_qpn);
					return -1;
				}
				lqp->attr.qp_state = SPDK_VRDMA_RPC_QP_READY;
			} else {
				SPDK_NOTICELOG("gid_ip 0x%lx vqp %d remote_vqp %d "
					"remote_qpn 0x%x node_id 0x%lx "
					"dev_id 0x%x remote_gid_ip 0x%lx rqp.qp_state %d\n",
					gid_ip, vqp_idx, qp_attr->comm.vqpn, remote_qpn,
					lqp->remote_node_id, lqp->remote_dev_id, lqp->remote_gid_ip,
					rqp->attr.qp_state);
				if (rqp->attr.qp_state == SPDK_VRDMA_RPC_QP_READY && lqp->bk_qp)
                	set_spdk_vrdma_bk_qp_active(ctrl, lqp->bk_qp);
			}
		}
	}
	return 0;
}

struct vrdma_backend_qp *
vrdma_create_backend_qp(struct vrdma_ctrl *ctrl,
				uint32_t vqp_idx, uint32_t remote_vqpn)
{
	struct vrdma_backend_qp *qp;
	struct spdk_vrdma_qp *vqp;

	vqp = find_spdk_vrdma_qp_by_idx(ctrl, vqp_idx);
	if (!vqp) {
		SPDK_ERRLOG("Failed to find VQP %d in allocate backend QP",
			vqp_idx);
		return NULL;
	}
	qp = calloc(1, sizeof(*qp));
    if (!qp) {
		SPDK_ERRLOG("Failed to allocate backend QP memory");
		return NULL;
	}
	qp->pd = vqp->vpd->ibpd;
	qp->poller_core = spdk_env_get_current_core();
	qp->remote_qpn = VRDMA_INVALID_QPN;
	qp->rgid_rip.global.subnet_prefix = 0;
	qp->rgid_rip.global.interface_id = ctrl->vdev->vrdma_sf.remote_ip;
	qp->lgid_lip.global.subnet_prefix = 0;
	qp->lgid_lip.global.interface_id = ctrl->vdev->vrdma_sf.ip;
	qp->src_addr_idx = ctrl->vdev->vrdma_sf.gid_idx;
	memcpy(qp->dest_mac, ctrl->vdev->vrdma_sf.dest_mac, 6);
	memcpy(qp->local_mac, ctrl->vdev->vrdma_sf.mac, 6);
	qp->bk_qp.qp_attr.qp_type = SNAP_OBJ_DEVX;
	qp->bk_qp.qp_attr.sq_size = vqp->sq.comm.wqebb_cnt;
	qp->bk_qp.qp_attr.sq_max_sge = 1;
	qp->bk_qp.qp_attr.sq_max_inline_size = 256;
	qp->bk_qp.qp_attr.rq_size = vqp->rq.comm.wqebb_cnt;
	qp->bk_qp.qp_attr.rq_max_sge = 1;
	qp->bk_qp.qp_attr.is_vrdma = 1;
	if (snap_vrdma_create_qp_helper(qp->pd, &qp->bk_qp)) {
		SPDK_ERRLOG("Failed to create backend QP ");
		goto free_bk_qp;
	}
	qp->remote_vqpn = remote_vqpn;
	if (vrdma_add_lbk_qp_list(ctrl, vqp_idx, qp)) {
		SPDK_ERRLOG("Failed to add backend QP in local list");
		goto detory_bk_qp;
	}
	vqp->pre_bk_qp = qp;
	LIST_INSERT_HEAD(&ctrl->bk_qp_list, qp, entry);
	SPDK_NOTICELOG("vqpn %d remote_vqpn %d bk_qpn 0x%x sucessfully\n",
		vqp_idx, remote_vqpn, qp->bk_qp.qpnum);
	return qp;

detory_bk_qp:
	snap_vrdma_destroy_qp_helper(&qp->bk_qp);
free_bk_qp:
	free(qp);
	return NULL;
}

void
set_spdk_vrdma_bk_qp_active(struct vrdma_ctrl *ctrl,
		struct vrdma_backend_qp *pre_bk_qp)
{
	struct spdk_vrdma_qp *vqp = NULL;

	LIST_FOREACH(vqp, &ctrl->vdev->vqp_list, entry) {
        if (vqp->pre_bk_qp == pre_bk_qp && !vqp->bk_qp) {
			vqp->bk_qp = pre_bk_qp;
			SPDK_NOTICELOG("Set bk_qpn 0x%x active\n", vqp->bk_qp->bk_qp.qpnum);
			break;
		}
	}
}

int vrdma_modify_backend_qp_to_ready(struct vrdma_ctrl *ctrl,
				struct vrdma_backend_qp *bk_qp, bool remote_ready)
{
	struct snap_vrdma_bk_qp_rdy_attr rdy_attr = {0};
	struct ibv_qp_attr qp_attr = {0};
	struct snap_qp *sqp;
	int attr_mask;
	uint32_t path_mtu;

	/* Modify bankend QP to ready (rst2init + init2rtr + rtr2rts)*/
	sqp = bk_qp->bk_qp.sqp;
	qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_LOCAL_WRITE;
	attr_mask = IBV_QP_ACCESS_FLAGS;
	if (snap_vrdma_modify_bankend_qp_rst2init(sqp, &qp_attr, attr_mask)) {
		SPDK_ERRLOG("Failed to modify bankend QP reset to init");
		return -1;
	}

	attr_mask = IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
				IBV_QP_RQ_PSN | IBV_QP_MIN_RNR_TIMER |
				IBV_QP_MAX_DEST_RD_ATOMIC;
	path_mtu = ctrl->vdev->vrdma_sf.mtu < ctrl->sctrl->bar_curr->mtu ?
				ctrl->vdev->vrdma_sf.mtu : ctrl->sctrl->bar_curr->mtu;
	if (path_mtu >= 4096)
		qp_attr.path_mtu = IBV_MTU_4096;
	else if (path_mtu >= 2048)
		qp_attr.path_mtu = IBV_MTU_2048;
	else if (path_mtu >= 1024)
		qp_attr.path_mtu = IBV_MTU_1024;
	else if (path_mtu >= 512)
		qp_attr.path_mtu = IBV_MTU_512;
	else
		qp_attr.path_mtu = IBV_MTU_256;
	qp_attr.dest_qp_num = bk_qp->remote_qpn;
	if (qp_attr.dest_qp_num == VRDMA_INVALID_QPN) {
		SPDK_ERRLOG("Failed to modify bankend QP for invalid remote qpn");
		return -1;
	}
	qp_attr.rq_psn = 0;
	qp_attr.min_rnr_timer = VRDMA_MIN_RNR_TIMER;
	qp_attr.max_dest_rd_atomic = VRDMA_QP_MAX_DEST_RD_ATOMIC;
	rdy_attr.dest_mac = bk_qp->dest_mac;
	rdy_attr.rgid_rip = bk_qp->rgid_rip;
	rdy_attr.src_addr_index = bk_qp->src_addr_idx;
	if (snap_vrdma_modify_bankend_qp_init2rtr(sqp, &qp_attr, attr_mask, &rdy_attr)) {
		SPDK_ERRLOG("Failed to modify bankend QP init to RTR");
		return -1;
	}

	attr_mask = IBV_QP_SQ_PSN | IBV_QP_RETRY_CNT |
				IBV_QP_RNR_RETRY | IBV_QP_TIMEOUT |
				IBV_QP_MAX_QP_RD_ATOMIC;
	qp_attr.sq_psn = 0;
	qp_attr.retry_cnt = VRDMA_BACKEND_QP_RNR_RETRY;
	qp_attr.rnr_retry = VRDMA_BACKEND_QP_RETRY_COUNT;
	qp_attr.timeout = VRDMA_BACKEND_QP_TIMEOUT;
	qp_attr.max_rd_atomic = VRDMA_QP_MAX_RD_ATOMIC;
	if (snap_vrdma_modify_bankend_qp_rtr2rts(sqp, &qp_attr, attr_mask)) {
		SPDK_ERRLOG("Failed to modify bankend QP RTR to RTS");
		return -1;
	}
	if (remote_ready)
		set_spdk_vrdma_bk_qp_active(ctrl, bk_qp);
	SPDK_NOTICELOG("path_mtu %d dest_qp_num 0x%x min_rnr_timer %d "
	"src_addr_index %d retry_cnt %d rnr_retry %d timeout %d remote_ready %d successfully\n",
	qp_attr.path_mtu, qp_attr.dest_qp_num, qp_attr.min_rnr_timer,
	rdy_attr.src_addr_index, qp_attr.retry_cnt, qp_attr.rnr_retry,
	qp_attr.timeout, remote_ready);
	return 0;
}

void vrdma_destroy_backend_qp(struct vrdma_ctrl *ctrl, uint32_t vqp_idx)
{
	struct vrdma_local_bk_qp *lqp;
	struct vrdma_backend_qp *qp;
	struct spdk_vrdma_qp *vqp;
	struct spdk_vrdma_rpc_qp_msg msg = {0};

	SPDK_NOTICELOG("vqpn %d\n", vqp_idx);
	vqp = find_spdk_vrdma_qp_by_idx(ctrl, vqp_idx);
	if (!vqp) {
		SPDK_ERRLOG("Failed to find VQP %d in destroy backend QP",
			vqp_idx);
		return;
	}
	if (vqp->pre_bk_qp) {
		qp = vqp->pre_bk_qp;
		snap_vrdma_destroy_qp_helper(&qp->bk_qp);
		/* Send RPC to nodify remote gid/backend_qp with local gid/backend_qp */
		lqp = vrdma_find_lbk_qp_by_vqp(ctrl->vdev->vrdma_sf.ip, vqp_idx);
    	if (lqp) {
			memcpy(&msg.qp_attr, &lqp->attr.comm,
            	sizeof(struct vrdma_bk_qp_connect));
			msg.emu_manager = ctrl->emu_manager;
    		msg.bk_qpn = lqp->bk_qpn;
			msg.remote_node_id = lqp->remote_node_id;
			msg.remote_dev_id = lqp->remote_dev_id;
			msg.remote_vqpn = qp->remote_vqpn;
			msg.remote_gid_ip = lqp->remote_gid_ip;
			msg.qp_state = SPDK_VRDMA_RPC_QP_DESTROYED;
			if (spdk_vrdma_rpc_send_qp_msg(ctrl, g_vrdma_rpc.node_rip, &msg)) {
        		SPDK_ERRLOG("Fail to send local qp %d to remote qp %d to destroy\n",
            	vqp_idx, msg.remote_vqpn);
    		}
			vrdma_del_lbk_qp_from_list(lqp);
		}
		vqp->pre_bk_qp = NULL;
		vqp->bk_qp = NULL;
		LIST_REMOVE(qp, entry);
		free(qp);
	}
}

static void vrdma_vqp_rx_cb(struct snap_dma_q *q, const void *data,
									uint32_t data_len, uint32_t imm_data)
{
	uint16_t pi = be32toh(imm_data) & 0xFFFF;
	struct spdk_vrdma_qp *vqp;
	struct snap_vrdma_queue *snap_vqp;

	snap_vqp = (struct snap_vrdma_queue *)q->uctx;
	vqp = (struct spdk_vrdma_qp *)snap_vqp->ctx;
	vqp->qp_pi->pi.sq_pi = pi;
	vqp->sq.comm.num_to_parse = pi - vqp->sq.comm.pre_pi;
#ifdef NO_PERF_DEBUG
	SPDK_NOTICELOG("VRDMA: rx cb started, pi %d, num_to_parse %d\n", pi, vqp->sq.comm.num_to_parse);
#endif
	vrdma_dpa_rx_cb(vqp, VRDMA_QP_SM_OP_OK);
#ifdef NO_PERF_DEBUG
	SPDK_NOTICELOG("VRDMA: rx cb done, imm_data 0x%x\n", imm_data);
#endif
}

int vrdma_create_vq(struct vrdma_ctrl *ctrl,
				struct vrdma_admin_cmd_entry *aqe,
				struct spdk_vrdma_qp *vqp,
				struct spdk_vrdma_cq *rq_vcq,
				struct spdk_vrdma_cq *sq_vcq)
{
	struct snap_vrdma_vq_create_attr q_attr;
	uint32_t rq_buff_size, sq_buff_size, q_buff_size;
	uint16_t sq_meta_size;

	q_attr.bdev = NULL;
	q_attr.pd = ctrl->pd;
	q_attr.sq_size = VRDMA_MAX_DMA_SQ_SIZE_PER_VQP;
	q_attr.rq_size = VRDMA_MAX_DMA_RQ_SIZE_PER_VQP;
	q_attr.tx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.rx_elem_size = VRDMA_DMA_ELEM_SIZE;
	q_attr.vqpn = vqp->qp_idx;
	q_attr.rx_cb = vrdma_vqp_rx_cb;

	if (!ctrl->dpa_enabled) {
		vqp->snap_queue = ctrl->sctrl->q_ops->create(ctrl->sctrl, &q_attr);
		if (!vqp->snap_queue) {
			SPDK_ERRLOG("Failed to create qp dma queue");
			return -1;
		}
		vqp->snap_queue->ctx = vqp;
	}
	vrdma_qp_sm_init(vqp);
	vqp->rq.comm.wqebb_size =
		VRDMA_QP_WQEBB_BASE_SIZE * (aqe->req.create_qp_req.rq_wqebb_size + 1);
	vqp->rq.comm.wqebb_cnt = 1 << aqe->req.create_qp_req.log_rq_wqebb_cnt;
	rq_buff_size = vqp->rq.comm.wqebb_size * vqp->rq.comm.wqebb_cnt;
	q_buff_size = sizeof(*vqp->qp_pi) + rq_buff_size;
	vqp->sq.comm.wqebb_size =
		VRDMA_QP_WQEBB_BASE_SIZE * (aqe->req.create_qp_req.sq_wqebb_size + 1);
	vqp->sq.comm.wqebb_cnt = 1 << aqe->req.create_qp_req.log_sq_wqebb_cnt;
	sq_buff_size = vqp->sq.comm.wqebb_size * vqp->sq.comm.wqebb_cnt;
	q_buff_size += sq_buff_size;
	sq_meta_size = sizeof(struct vrdma_sq_meta) * vqp->sq.comm.wqebb_cnt;
	q_buff_size += sq_meta_size;

	vqp->qp_pi = spdk_malloc(q_buff_size, 0x10, NULL, SPDK_ENV_LCORE_ID_ANY,
                             SPDK_MALLOC_DMA);
    if (!vqp->qp_pi) {
		SPDK_ERRLOG("Failed to allocate wqe buff");
        goto destroy_dma;
    }
	vqp->rq.rq_buff = (struct vrdma_recv_wqe *)((uint8_t *)vqp->qp_pi + sizeof(*vqp->qp_pi));
	vqp->sq.sq_buff = (struct vrdma_send_wqe *)((uint8_t *)vqp->rq.rq_buff + rq_buff_size);
	vqp->sq.meta_buff = (struct vrdma_sq_meta *)((uint8_t *)vqp->sq.sq_buff + sq_buff_size);
    vqp->qp_mr = ibv_reg_mr(ctrl->pd, vqp->qp_pi, q_buff_size,
                    IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE |
                    IBV_ACCESS_LOCAL_WRITE);
    if (!vqp->qp_mr) {
		SPDK_ERRLOG("Failed to register qp_mr");
        goto free_wqe_buff;
    }
	vqp->rq.comm.wqe_buff_pa = aqe->req.create_qp_req.rq_l0_paddr;
	vqp->rq.comm.doorbell_pa = aqe->req.create_qp_req.rq_pi_paddr;
	vqp->rq.comm.log_pagesize = aqe->req.create_qp_req.log_rq_pagesize;
	vqp->rq.comm.hop = aqe->req.create_qp_req.rq_hop;
	vqp->sq.comm.wqe_buff_pa = aqe->req.create_qp_req.sq_l0_paddr;
	vqp->sq.comm.doorbell_pa = aqe->req.create_qp_req.sq_pi_paddr;
	vqp->sq.comm.log_pagesize = aqe->req.create_qp_req.log_sq_pagesize;
	vqp->sq.comm.hop = aqe->req.create_qp_req.sq_hop;

	if (ctrl->dpa_enabled) {
		SPDK_NOTICELOG("===================naliu vrdma_qp.c=================");
		SPDK_NOTICELOG("vqp %d qdb_idx %d lkey %#x rkey %#x\n",
				vqp->qp_idx, vqp->qdb_idx, vqp->qp_mr->lkey, vqp->qp_mr->rkey);
		vqp->snap_queue = vrdma_prov_vq_create(ctrl, vqp, &q_attr);
		SPDK_NOTICELOG("===naliu vrdma_create_vq...end\n");
		if (vqp->snap_queue) {
			vqp->snap_queue->ctx = vqp;
		}
	}
	SPDK_NOTICELOG("\nlizh vrdma_create_vq...done\n");
	return 0;

free_wqe_buff:
	spdk_free(vqp->qp_pi);
destroy_dma:
	ctrl->sctrl->q_ops->destroy(ctrl->sctrl, vqp->snap_queue);
	return -1;
}

bool vrdma_set_vq_flush(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
	if (ctrl->sctrl->q_ops->is_suspended(vqp->snap_queue))
		return false;
	ctrl->sctrl->q_ops->suspend(vqp->snap_queue);
	return true;
}

void vrdma_destroy_vq(struct vrdma_ctrl *ctrl,
				struct spdk_vrdma_qp *vqp)
{
	if (ctrl->sctrl)
		ctrl->sctrl->q_ops->destroy(ctrl->sctrl, vqp->snap_queue);
	if (vqp->qp_mr) {
		ibv_dereg_mr(vqp->qp_mr);
		vqp->qp_mr = NULL;
	}
	if (vqp->qp_pi) {
		spdk_free(vqp->qp_pi);
		vqp->qp_pi = NULL;
		vqp->rq.rq_buff = NULL;
		vqp->sq.sq_buff = NULL;
	}
}

bool vrdma_qp_is_suspended(struct vrdma_ctrl *ctrl, uint32_t qp_handle)
{
	struct spdk_vrdma_qp *vqp;
	
	vqp = find_spdk_vrdma_qp_by_idx(ctrl, qp_handle);
	if (!vqp) {
		SPDK_ERRLOG("Failed to find QP %d in waiting qp suspended progress",
			qp_handle);
		return false;
	}
	return ctrl->sctrl->q_ops->is_suspended(vqp->snap_queue);
}

bool vrdma_qp_is_connected_ready(struct spdk_vrdma_qp *vqp)
{
	if (vqp->qp_state > IBV_QPS_INIT && vqp->qp_state < IBV_QPS_ERR)
		return true;
	return false;
}

int vrdma_qp_notify_remote_by_rpc(struct vrdma_ctrl *ctrl, uint32_t vqpn,
		uint32_t remote_vqpn, struct vrdma_backend_qp *bk_qp)
{
	struct spdk_vrdma_rpc_qp_msg msg = {0};
    struct vrdma_local_bk_qp *lqp;

	if (bk_qp->remote_qpn != VRDMA_INVALID_QPN) {
		if (vrdma_modify_backend_qp_to_ready(ctrl, bk_qp, false)) {
			SPDK_ERRLOG("Failed to modify bankend qp 0x%x to ready\n",vqpn);
			return -1;
		}
		msg.qp_state = SPDK_VRDMA_RPC_QP_READY;
	} else {
		msg.qp_state = SPDK_VRDMA_RPC_QP_WAIT_RQPN;
	}
	/* Send RPC to nodify remote gid/backend_qp with local gid/backend_qp */
    lqp = vrdma_find_lbk_qp_by_vqp(ctrl->vdev->vrdma_sf.ip, vqpn);
    if (!lqp) {
        SPDK_ERRLOG("Fail to find local qp %d to send rpc\n", vqpn);
		return -1;
    }
	lqp->attr.qp_state = msg.qp_state;
	memcpy(&msg.qp_attr, &lqp->attr.comm,
            sizeof(struct vrdma_bk_qp_connect));
	msg.emu_manager = ctrl->emu_manager;
	msg.remote_node_id = lqp->remote_node_id;
	msg.remote_dev_id = lqp->remote_dev_id;
    msg.remote_vqpn = remote_vqpn;
	msg.remote_gid_ip = lqp->remote_gid_ip;
    msg.bk_qpn = lqp->bk_qpn;
	SPDK_NOTICELOG("vqpn %d bk_qpn 0x%x remote_qpn 0x%x remote_node_id 0x%lx "
	"remote_vqpn 0x%x gid_ip 0x%lx remote_gid_ip 0x%lx\n",
    vqpn, msg.bk_qpn, bk_qp->remote_qpn, msg.remote_node_id,
	msg.remote_vqpn, msg.qp_attr.gid_ip, msg.remote_gid_ip);
    if (spdk_vrdma_rpc_send_qp_msg(ctrl, g_vrdma_rpc.node_rip, &msg)) {
        SPDK_ERRLOG("Fail to send local qp %d to remote qp %d\n",
            vqpn, remote_vqpn);
    }
	return 0;
}
