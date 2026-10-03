/*
 * NV2A Vulkan hybrid worker completion wake
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/vk/renderer.h"

void pgraph_vk_hybrid_worker_notify(void *opaque)
{
    NV2AState *d = opaque;
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (!r || qatomic_xchg(&r->hybrid_completion_kick_pending, true)) {
        return;
    }
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}
