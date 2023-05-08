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
#include "snap.h"
#include "snap_vrdma_ctrl.h"
#include "snap_vrdma_virtq.h"

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/bit_array.h"
#include "spdk/barrier.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/vrdma_controller.h"
#include "spdk/vrdma_migration.h"

pthread_spinlock_t vrdma_mig_vqp_list_lock;
LIST_HEAD(, vrdma_mig_vqp) vrdma_mig_vqp_list;
bool is_vrdma_vqp_migration_enable(void);
int vrdma_vqp_migration_enable = 0;

bool
is_vrdma_vqp_migration_enable(void)
{
    return vrdma_vqp_migration_enable == 1 ? 1 : 0;
}

int
vrdma_mig_vqp_add_to_list(struct spdk_vrdma_qp *vqp)
{
    struct vrdma_mig_vqp *vqp_entry = NULL;
    vqp_entry = calloc(1, sizeof(struct vrdma_mig_vqp));
    if (!vqp_entry) {
        SPDK_ERRLOG("Failed to allocate vrdma_mig_vqp memory");
        return -1;
    }
    vqp_entry->vqp = vqp;

    pthread_spin_lock(&vrdma_mig_vqp_list_lock);
    LIST_INSERT_HEAD(&vrdma_mig_vqp_list, vqp_entry, entry);
    pthread_spin_unlock(&vrdma_mig_vqp_list_lock);
    return 0;
}

void vrdma_mig_mqp_depth_sampling(struct vrdma_backend_qp *mqp)
{
    uint32_t i, total_depth;

    if (!mqp) return;
    mqp->sample_depth[mqp->sample_curr] = mqp->bk_qp.hw_qp.sq.pi - mqp->bk_qp.sq_ci;
    mqp->sample_curr = (mqp->sample_curr + 1) % MQP_DEPTH_SAMPLE_NUM;
    if (!mqp->sample_depth[MQP_DEPTH_SAMPLE_NUM-1]) {
        for (i = 0, total_depth = 0; i < MQP_DEPTH_SAMPLE_NUM; i++) {
            total_depth += mqp->sample_depth[i];
        }
        mqp->avg_depth = total_depth/MQP_DEPTH_SAMPLE_NUM;
    }
}

#define MIGRATION_MQP_DEPTH_THRESH(sq_size) ((sq_size)>>1 + (sq_size)>>2)
static bool vrdma_check_active_mig_criteria(struct spdk_vrdma_qp *vqp)
{
    struct vrdma_backend_qp *mqp = NULL;

    if (vqp == NULL)
        return false;
    mqp = vqp->bk_qp;
    if (!mqp || mqp->avg_depth <= MIGRATION_MQP_DEPTH_THRESH(mqp->bk_qp.qp_attr.sq_size)) {
        return false;
    } else {
        return true;
    }
}

static bool vrdma_vqp_last_mig_expired(struct spdk_vrdma_qp *vqp)
{
    if (vqp->mig_ctx.mig_start_ts + VRDMA_MIG_INTERVAL_MIN * spdk_get_ticks_hz() <
        spdk_get_ticks()) {
        return true;
    } else {
        return false;
    }
}

static void
vrdma_vqp_mig_start(struct spdk_vrdma_qp *vqp)
{
    struct vrdma_tgid_node *tgid_node = NULL;
    uint8_t mqp_idx;

    tgid_node = vqp->bk_qp->tgid_node;
    vqp->mig_ctx.mig_mqp = vrdma_find_mqp_by_depth(tgid_node, &mqp_idx);
    SPDK_NOTICELOG("new mqp=%p, idx=%u", vqp->mig_ctx.mig_mqp, mqp_idx);
    vrdma_desched_vq_nolock(vqp);
    vqp->mig_ctx.mig_start_ts = spdk_get_ticks();
    vrdma_mig_vqp_add_to_list(vqp);
}

void vrdma_mig_handle_sm(struct spdk_vrdma_qp *vqp)
{
    struct vrdma_tgid_node *tgid_node = NULL;
    struct vrdma_vqp *vqp_entry = NULL;
    uint8_t mqp_idx;
    int ret;

    if (!vqp) {
        SPDK_ERRLOG("null vqp\n");
        return;
    }
    switch (vqp->mig_ctx.mig_state) {
    case MIG_START:
        if (vqp->bk_qp->qp_state == IBV_QPS_ERR) {
            /* the 1st vqp found mqp err will move all vqp to mig list */
            pthread_spin_lock(&vqp->bk_qp->vqp_list_lock);
            LIST_FOREACH(vqp_entry, &vqp->bk_qp->vqp_list, entry) {
                vqp_entry->vqp->mig_ctx.mig_state = MIG_START;
                vqp_entry->vqp->mig_ctx.mig_repost = MIG_REPOST_SET;
                ret = vrdma_dpa_set_vq_stop_fetch(vqp_entry->vqp);
                SPDK_NOTICELOG("vrdma_dpa_set_vq_stop_fetch ret=%d", ret);
                vrdma_vqp_mig_start(vqp_entry->vqp);
            }
            pthread_spin_unlock(&vqp->bk_qp->vqp_list_lock);
        }
        break;
    case MIG_PREPARE:
        if (vqp->sq_ci == vqp->sq.comm.pre_pi) {
            vqp->mig_ctx.mig_state = MIG_START;
            vrdma_vqp_mig_start(vqp);
        }
        break;
    case MIG_IDLE:
        if (vrdma_vqp_last_mig_expired(vqp) &&
            vrdma_check_active_mig_criteria(vqp)) {
            tgid_node = vqp->bk_qp->tgid_node;
            vqp->mig_ctx.mig_mqp = vrdma_find_mqp_by_depth(tgid_node, &mqp_idx);
            if (vqp->mig_ctx.mig_mqp != vqp->bk_qp) {
                vqp->mig_ctx.mig_state = MIG_PREPARE;
            } else {
                SPDK_ERRLOG("can't find a more leisurely mqp!\n");
            }
        }
        break;
    default:
        SPDK_ERRLOG("should not have such case\n");
        break;
    }
    return;
}

static int32_t 
vrdma_mig_handle_rnxt_rcv_psn(struct vrdma_ctrl *ctrl,
                              struct vrdma_tgid_node *tgid_node,
                              struct vrdma_backend_qp *mqp)
{
    if (mqp->mig_ctx.mig_rnxt_rcv_psn_state == MIG_REQ_NULL) {
        mqp->mig_ctx.mig_rnxt_rcv_psn_state = MIG_REQ_SENT;
        /* query peer mqp.next_rcv_psn only one time*/
        if (vrdma_qp_notify_remote_by_rpc(ctrl, tgid_node, mqp->poller_core)) {
            SPDK_ERRLOG("failed to send rpc to query mqp=0x%x "
                    "state=0x%x peer mqp.next_rcv_psn\n",
                    mqp->bk_qp.qpnum, mqp->qp_state);
            return 1;
        }
    }
    if (mqp->mig_ctx.mig_rnxt_rcv_psn_state == MIG_REQ_SENT) {
        SPDK_NOTICELOG("waiting for mig_rnxt_rcv_psn response msg");
        return 2;
    }
    /* inform dpa */
    if (mqp->mig_ctx.mig_rnxt_rcv_psn_state == MIG_RESP_RCV) {
        vrdma_mig_set_repost_pi(mqp);
    }
    return 0;
}

void vrdma_mig_set_mqp_pmtu(struct vrdma_backend_qp *mqp,
                            struct ibv_qp_attr *qp_attr)
{
    switch (qp_attr->path_mtu) {
    case IBV_MTU_4096:
        mqp->mig_ctx.mig_pmtu = 4096;
        break;
    case IBV_MTU_2048:
        mqp->mig_ctx.mig_pmtu = 2048;
        break;
    case IBV_MTU_1024:
        mqp->mig_ctx.mig_pmtu = 1024;
        break;
    case IBV_MTU_512:
        mqp->mig_ctx.mig_pmtu = 512;
        break;
    case IBV_MTU_256:
        mqp->mig_ctx.mig_pmtu = 256;
        break;
    default:
        SPDK_ERRLOG("invalid path mtu=%u\n", qp_attr->path_mtu);
        break;
    }
    return;
}

int32_t vrdma_mig_set_repost_pi(struct vrdma_backend_qp *mqp)
{
    uint32_t i, mqp_sq_size, rnxt_rcv_psn;
    uint16_t mqp_pi, mqp_ci;
    struct mqp_sq_meta *sq_meta = NULL;
    struct vrdma_vqp *vqp_entry = NULL;

    if (!mqp) return -1;
    mqp_pi = mqp->bk_qp.hw_qp.sq.pi;
    mqp_ci = mqp->bk_qp.sq_ci;
    mqp_sq_size = mqp->bk_qp.hw_qp.sq.wqe_cnt;
    if (vrdma_vq_rollback(mqp_ci, mqp_pi, mqp_sq_size)) {
        mqp_pi += mqp_sq_size;
    }
    if (mqp_ci == 0) {
        mqp_pi += mqp_sq_size;
    }
    rnxt_rcv_psn = mqp->mig_ctx.mig_rnxt_rcv_psn;
    SPDK_NOTICELOG("mqp.mig_rnxt_rcv_psn=%u, sq_ci=%u, sq_pi=%u\n",
                   rnxt_rcv_psn, mqp_ci, mqp_pi);
    /* 1: find the 1st wqe and its vqp that need repost */
    for (i = mqp_ci - 1; i < mqp_pi; i++) {
        sq_meta = &mqp->sq_meta_buf[i & (mqp_sq_size - 1)];
        SPDK_NOTICELOG("sq_meta[%u] 1st psn=%u last_psn=%u\n",
                       i, sq_meta->first_psn, sq_meta->last_psn);
        if (rnxt_rcv_psn >= sq_meta->first_psn &&
            rnxt_rcv_psn <= sq_meta->last_psn) {
            SPDK_NOTICELOG("found mqp mig_repost_pi=%u, repost_offset=%u vqp=%u twqe=%u\n",
                           i, rnxt_rcv_psn - sq_meta->first_psn, sq_meta->vqp->qp_idx,
                           sq_meta->twqe_idx);
            /* first vqp may have some offset, following vqp has offset = 0 */
            sq_meta->vqp->mig_ctx.mig_repost_pi = sq_meta->twqe_idx;
            sq_meta->vqp->mig_ctx.mig_repost_offset = rnxt_rcv_psn - sq_meta->first_psn;
            /* rollback pi and pre_pi */
            //vrdma_dpa_vq_is_stopped(sq_meta->vqp);
            sq_meta->vqp->qp_pi->pi.sq_pi = sq_meta->twqe_idx;
            sq_meta->vqp->sq.comm.pre_pi = sq_meta->twqe_idx;
            sq_meta->vqp->mig_ctx.mig_repost = MIG_REPOST_START;
            vrdma_dpa_set_vq_repost_pi(sq_meta->vqp, sq_meta->vqp->mig_ctx.mig_repost_pi);
            SPDK_NOTICELOG("vqp=%u, mig_repost=%u vrdma_dpa_set_vq_repost_pi=%u",
                           sq_meta->vqp->qp_idx, sq_meta->vqp->mig_ctx.mig_repost,
                           sq_meta->vqp->mig_ctx.mig_repost_pi);
            break;
        }
    }
    if (i == mqp->bk_qp.hw_qp.sq.pi) {
        SPDK_ERRLOG("can't find the wqe including rnxt_rcv_psn\n");
        return -1;
    }
    /* 2: set other vqp.mig_ctx.mig_repost_pi per meta_buf */
    for (i = i + 1; i < mqp_pi; i++) {
        sq_meta = &mqp->sq_meta_buf[i & (mqp_sq_size - 1)];
        if (sq_meta->vqp->mig_ctx.mig_repost == MIG_REPOST_SET) {
            sq_meta->vqp->mig_ctx.mig_repost_pi = sq_meta->twqe_idx;
            /* rollback pi and pre_pi */
            //vrdma_dpa_vq_is_stopped(sq_meta->vqp);
            sq_meta->vqp->qp_pi->pi.sq_pi = sq_meta->twqe_idx;
            sq_meta->vqp->sq.comm.pre_pi = sq_meta->twqe_idx;
            sq_meta->vqp->mig_ctx.mig_repost = MIG_REPOST_START;
            vrdma_dpa_set_vq_repost_pi(sq_meta->vqp, sq_meta->vqp->mig_ctx.mig_repost_pi);
            SPDK_NOTICELOG("vqp=%u, mig_repost=%u vrdma_dpa_set_vq_repost_pi=%u",
                           sq_meta->vqp->qp_idx, sq_meta->vqp->mig_ctx.mig_repost,
                           sq_meta->vqp->mig_ctx.mig_repost_pi);
        }
    }
    /* 3: clear repost flag of left vqp which is not in meta_buf */
    pthread_spin_lock(&mqp->vqp_list_lock);
    LIST_FOREACH(vqp_entry, &mqp->vqp_list, entry) {
        if (vqp_entry->vqp->mig_ctx.mig_repost == MIG_REPOST_SET) {
            vqp_entry->vqp->mig_ctx.mig_repost = MIG_REPOST_INIT;
            /* has to inform dpa to start working */
            vrdma_dpa_set_vq_repost_pi(sq_meta->vqp, sq_meta->vqp->sq.comm.pre_pi);
            SPDK_NOTICELOG("vqp=%u, mig_repost=%u vrdma_dpa_set_vq_repost_pi=%u",
                           sq_meta->vqp->qp_idx, sq_meta->vqp->mig_ctx.mig_repost,
                           sq_meta->vqp->mig_ctx.mig_repost_pi);
        }
    }
    pthread_spin_unlock(&mqp->vqp_list_lock);
    return 0;
}

void
vrdma_mig_reassemble_wqe(struct vrdma_send_wqe *wqe,
                         uint32_t mig_repost_offset,
                         uint32_t pmtu)
{
    uint64_t offset_bytes = mig_repost_offset * pmtu;
    uint64_t total_bytes = 0, sgl_offset;
    int16_t i, j, start_sgl_idx = -1;

    wqe->rdma_rw.remote_addr += offset_bytes;
    for (i = 0; i < wqe->meta.sge_num; i++) {
        total_bytes += wqe->sgl[i].buf_length;
        if (total_bytes < offset_bytes)
            continue;
        if (total_bytes == offset_bytes) {
            sgl_offset = 0;
            start_sgl_idx = i + 1;
            break;
        }
        if (total_bytes > offset_bytes) {
            sgl_offset = offset_bytes - total_bytes;
            start_sgl_idx = i;
            break;
        }
    }
    if (start_sgl_idx == -1) {
        SPDK_ERRLOG("failed to find start_sgl_idx");
        return;
    }

    wqe->sgl[0].buf_length = wqe->sgl[start_sgl_idx].buf_length - sgl_offset;
    wqe->sgl[0].lkey = wqe->sgl[start_sgl_idx].lkey;
    if (sgl_offset < UINT_MAX) {
        wqe->sgl[0].buf_addr_lo += sgl_offset;
    } else {
        wqe->sgl[0].buf_addr_lo += sgl_offset - UINT_MAX;
        wqe->sgl[0].buf_addr_hi += 1;
    }
    for (j = 1, i = start_sgl_idx + 1; i < wqe->meta.sge_num; j++, i++) {
        memcpy(&wqe->sgl[j], &wqe->sgl[i], sizeof(wqe->sgl[j]));
    }
    wqe->meta.sge_num -= start_sgl_idx;
}

int
vrdma_migration_progress(struct vrdma_ctrl *ctrl)
{
    struct vrdma_mig_vqp *vqp_entry = NULL, *tmp_entry;
    uint8_t mqp_idx;
    struct spdk_vrdma_qp *vqp = NULL;
    struct snap_pg *pg = NULL;
    struct vrdma_backend_qp *old_mqp = NULL;
    struct vrdma_tgid_node *tgid_node = NULL;

    pthread_spin_lock(&vrdma_mig_vqp_list_lock);
    LIST_FOREACH_SAFE(vqp_entry, &vrdma_mig_vqp_list, entry, tmp_entry) {
        vqp = vqp_entry->vqp;
        SPDK_NOTICELOG("vqp=0x%x, mig_repost=0x%x",
                       vqp->qp_idx, vqp->mig_ctx.mig_repost);
        old_mqp = vqp->bk_qp;
        tgid_node = old_mqp->tgid_node;

        if (vqp->mig_ctx.mig_repost) {
            if (vrdma_mig_handle_rnxt_rcv_psn(tgid_node->ctrl, tgid_node, old_mqp)) {
                SPDK_NOTICELOG("rnxt_rcv_psn is not ready, waiting\n");
                continue;
            }
        }
        LIST_REMOVE(vqp_entry, entry);
        free(vqp_entry);

        vrdma_mqp_del_vqp_from_list(old_mqp, vqp->qp_idx);
        vrdma_mqp_add_vqp_to_list(vqp->mig_ctx.mig_mqp, vqp, vqp->qp_idx);
        pg = &tgid_node->ctrl->sctrl->pg_ctx.pgs[vqp->pre_bk_qp->poller_core];
        pg->id = vqp->pre_bk_qp->poller_core;
        vqp->mig_ctx.mig_state = MIG_IDLE;
        if (vrdma_sched_vq(tgid_node->ctrl->sctrl, vqp, pg)) {
            SPDK_ERRLOG("vqp=%u failed to join poller group \n", vqp->qp_idx);
            return -1;
        }
        if ((old_mqp->qp_state == IBV_QPS_ERR) && !old_mqp->vqp_cnt) {
            mqp_idx = old_mqp->poller_core;
            tgid_node->src_udp[mqp_idx].mqp = NULL;
            vrdma_destroy_backend_qp(&old_mqp);
            if (!vrdma_create_backend_qp(tgid_node, mqp_idx)) {
                SPDK_ERRLOG("failed to create new mqp at idx=%u\n", mqp_idx);
            } else {
                vrdma_modify_backend_qp_to_init(tgid_node->src_udp[mqp_idx].mqp);
                vrdma_qp_notify_remote_by_rpc(tgid_node->ctrl, tgid_node, mqp_idx);
            }
        }
    }
    pthread_spin_unlock(&vrdma_mig_vqp_list_lock);
    return 0;
}
