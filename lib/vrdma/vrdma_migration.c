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

    LIST_INSERT_HEAD(&vrdma_mig_vqp_list, vqp_entry, entry);
    pthread_spin_lock(&vrdma_mig_vqp_list_lock);
    SPDK_NOTICELOG("vqp=0x%x", vqp->qp_idx);
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
    if (mqp->avg_depth <= MIGRATION_MQP_DEPTH_THRESH(mqp->bk_qp.qp_attr.sq_size)) {
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

    tgid_node = vrdma_find_tgid_node_by_mqp(vqp->bk_qp->poller_core, vqp->bk_qp);
    vqp->mig_ctx.mig_mqp = vrdma_find_mqp_by_depth(tgid_node, &mqp_idx);
    vrdma_desched_vq(vqp);
    vqp->mig_ctx.mig_start_ts = spdk_get_ticks();
    vrdma_mig_vqp_add_to_list(vqp);
}

void vrdma_mig_handle_sm(struct spdk_vrdma_qp *vqp)
{
    struct vrdma_tgid_node *tgid_node = NULL;
    uint8_t mqp_idx;

    if (!vqp) {
        SPDK_ERRLOG("null vqp\n");
        return;
    }
    switch (vqp->mig_ctx.mig_state) {
    case MIG_START:
        vrdma_vqp_mig_start(vqp);
        break;
    case MIG_PREPARE:
        if ((vqp->mig_ctx.mig_repost == 1) || (vqp->sq_ci == vqp->sq.comm.pre_pi)) {
            vqp->mig_ctx.mig_state = MIG_START;
            vrdma_vqp_mig_start(vqp);
        }
        break;
    case MIG_IDLE:
        if (vrdma_vqp_last_mig_expired(vqp) &&
            vrdma_check_active_mig_criteria(vqp)) {
            tgid_node = vrdma_find_tgid_node_by_mqp(vqp->bk_qp->poller_core, vqp->bk_qp);
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

int32_t vrdma_mig_set_mqp_repost_pi(struct vrdma_backend_qp *mqp)
{
    uint32_t i, mqp_sq_size, rnxt_rcv_psn;
    struct mqp_sq_meta *sq_meta = NULL;
    if (!mqp) return -1;
    rnxt_rcv_psn = mqp->mig_ctx.mig_rnxt_rcv_psn;
    SPDK_NOTICELOG("mqp.mig_rnxt_rcv_psn=%u\n", rnxt_rcv_psn);
    mqp_sq_size = mqp->bk_qp.hw_qp.sq.wqe_cnt;
    for (i = mqp->bk_qp.sq_ci; i < mqp->bk_qp.hw_qp.sq.pi; i++) {
        sq_meta = &mqp->sq_meta_buf[i & (mqp_sq_size - 1)];
        if (rnxt_rcv_psn >= sq_meta->first_psn &&
            rnxt_rcv_psn <= sq_meta->last_psn) {
            mqp->mig_ctx.mig_repost_pi = i;
            mqp->mig_ctx.mig_repost_offset= rnxt_rcv_psn - sq_meta->first_psn;
            SPDK_NOTICELOG("mqp.mig_repost_pi=%u\n", i);
            return 0;
        }
    }
    SPDK_ERRLOG("can't find the wqe including rnxt_rcv_psn\n");
    return -1;
}

static int32_t vrdma_mig_set_vqp_repost_pi(struct spdk_vrdma_qp *vqp)
{
    uint32_t i, vqp_sq_size, mqp_wqe_idx, mqp_repost_pi;
    if (!vqp) return -1;
    mqp_repost_pi = vqp->mig_ctx.mig_repost_pi;
    SPDK_NOTICELOG("mqp.mig_repost_pi=%u\n", mqp_repost_pi);
    vqp_sq_size = vqp->sq.comm.wqebb_cnt;
    for (i = vqp->sq_ci; i < vqp->sq.comm.pre_pi; i++) {
        mqp_wqe_idx = vqp->sq.meta_buff[i % vqp_sq_size].mqp_wqe_idx;
        if (mqp_wqe_idx > mqp_repost_pi) {
            vqp->mig_ctx.mig_repost_pi = mqp_wqe_idx;
            /* rollback pi and pre_pi */
            vqp->qp_pi->pi.sq_pi = vqp->sq.comm.pre_pi = mqp_wqe_idx;
            SPDK_NOTICELOG("vqp.mig_repost_pi=%u\n", i);
            return 0;
        }
    }
    SPDK_ERRLOG("can't find the wqe exceeding mqp.mig_repost_pi\n");
    return -1;
}

int
vrdma_migration_progress(struct vrdma_ctrl *ctrl)
{
    struct vrdma_mig_vqp *vqp_entry = NULL, *tmp_entry;
    struct vrdma_tgid_node *tgid_node = NULL;
    uint8_t mqp_idx;
    struct spdk_vrdma_qp *vqp = NULL;
    struct snap_pg *pg = NULL;
    struct vrdma_backend_qp *old_mqp = NULL;

    pthread_spin_lock(&vrdma_mig_vqp_list_lock);
    LIST_FOREACH_SAFE(vqp_entry, &vrdma_mig_vqp_list, entry, tmp_entry) {
        vqp = vqp_entry->vqp;
        SPDK_NOTICELOG("vqp=0x%x, mig_repost=0x%x, mig_repost_pi=%u",
                       vqp->qp_idx, vqp->mig_ctx.mig_repost, vqp->mig_ctx.mig_repost_pi);
        old_mqp = vqp->bk_qp;
        tgid_node = vrdma_find_tgid_node_by_mqp(old_mqp->poller_core,
                                                old_mqp);
        if (vqp->mig_ctx.mig_repost && (vqp->mig_ctx.mig_repost_pi == 0)
            && (old_mqp->mig_ctx.mig_lnxt_rcv_psn != VRDMA_MIG_INVALID_PSN)) {
            /* query peer mqp.next_rcv_psn only one time*/
            if (vrdma_qp_notify_remote_by_rpc(ctrl, tgid_node, old_mqp->poller_core)) {
                SPDK_ERRLOG("failed to send rpc to query mqp=0x%x "
                            "state=0x%x peer mqp.next_rcv_psn\n",
                            old_mqp->bk_qp.qpnum, old_mqp->qp_state);
                continue;
            }
            old_mqp->mig_ctx.mig_lnxt_rcv_psn = VRDMA_MIG_INVALID_PSN;
            continue;
        }
        if (vqp->mig_ctx.mig_repost && vqp->mig_ctx.mig_repost_pi) {
            vrdma_mig_set_vqp_repost_pi(vqp);
            /* ask dpa to refetch from repost_pi */
        }
        LIST_REMOVE(vqp_entry, entry);
        free(vqp_entry);

        vrdma_mqp_del_vqp_from_list(old_mqp, vqp->qp_idx);
        vrdma_mqp_add_vqp_to_list(vqp->mig_ctx.mig_mqp, vqp, vqp->qp_idx);
        pg = &ctrl->sctrl->pg_ctx.pgs[vqp->pre_bk_qp->poller_core];
        pg->id = vqp->pre_bk_qp->poller_core;
        vqp->mig_ctx.mig_state = MIG_IDLE;
        if (vrdma_sched_vq(ctrl->sctrl, vqp, pg)) {
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
                vrdma_qp_notify_remote_by_rpc(ctrl, tgid_node, mqp_idx);
            }
        }
    }
    pthread_spin_unlock(&vrdma_mig_vqp_list_lock);
    return 0;
}
