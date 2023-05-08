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

#ifndef __VRDMA_MIGRATION_H__
#define __VRDMA_MIGRATION_H__

#include <stdint.h>

struct vrdma_mig_vqp {
    LIST_ENTRY(vrdma_mig_vqp) entry;
    struct spdk_vrdma_qp *vqp;
};

enum vrdma_mig_state {
    MIG_IDLE    = 0,
    MIG_PREPARE = 1,
    MIG_START   = 2,
};

enum mig_rnxt_rcv_psn_state {
    MIG_REQ_NULL   = 0,
    MIG_REQ_SENT   = 1,
    MIG_RESP_RCV   = 2,
};

struct vrdma_mqp_mig_ctx {
    uint32_t mig_pmtu;
#define PSN_MASK                   0xFFFFFF /* 24 bits */
    /* first PSN of the current executing WQE in the Send Queue */
    uint32_t msg_1st_psn;                   /* 1st psn of current wqe */
    uint32_t mig_lnxt_rcv_psn;              /* local next_rcv_psn in qpc */
    uint32_t mig_rnxt_rcv_psn_state:8;      /* 1 indicate has sent msg to peer */
    uint32_t mig_rnxt_rcv_psn;              /* remote next_rcv_psn in qpc */
};

extern pthread_spinlock_t vrdma_mig_vqp_list_lock;
extern int vrdma_vqp_migration_enable;

bool is_vrdma_vqp_migration_enable(void);
int vrdma_mig_vqp_add_to_list(struct spdk_vrdma_qp *vqp);
void vrdma_mig_mqp_depth_sampling(struct vrdma_backend_qp *mqp);
void vrdma_mig_handle_sm(struct spdk_vrdma_qp *vqp);
void vrdma_mig_set_mqp_pmtu(struct vrdma_backend_qp *mqp,
                            struct ibv_qp_attr *qp_attr);
int32_t vrdma_mig_set_repost_pi(struct vrdma_backend_qp *mqp);
void
vrdma_mig_reassemble_wqe(struct vrdma_send_wqe *wqe,
                         uint32_t mig_repost_offset,
                         uint32_t pmtu);
int vrdma_migration_progress(struct vrdma_ctrl *ctrl);
#endif
