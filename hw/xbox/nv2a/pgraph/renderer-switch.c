/*
 * NV2A renderer-switch lock coordination
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"

#include "renderer-switch.h"

static void process_pending_without_pgraph(
    PGRAPHRendererSwitchCoordinator *coordinator,
    const PGRAPHRendererSwitchOps *ops, void *opaque)
{
    qemu_mutex_lock(coordinator->pfifo_lock);
    ops->process_pending(opaque);
    qemu_mutex_unlock(coordinator->pfifo_lock);
}

void pgraph_renderer_switch_run(
    PGRAPHRendererSwitchCoordinator *coordinator,
    const PGRAPHRendererSwitchOps *ops, void *opaque)
{
    /*
     * Close admission before dropping PFIFO. An acquisition that passed the
     * gate before this store is allowed to finish; the retry loop below keeps
     * servicing any synchronization request it publishes.
     */
    qatomic_set(coordinator->handoff_pending, 1);
    qemu_mutex_unlock(coordinator->pfifo_lock);

    qemu_mutex_lock(coordinator->pgraph_lock);
    ops->publish_flush(opaque);
    qemu_mutex_unlock(coordinator->pgraph_lock);

    while (qemu_mutex_trylock(coordinator->renderer_lock) != 0) {
        /*
         * Reset before servicing pending work. A framebuffer acquisition may
         * publish a new synchronization request at any point while it owns
         * renderer_lock; publishing that request or releasing the lock sets
         * this event. This order prevents both a lost wakeup and a busy-spin.
         */
        qemu_event_reset(coordinator->switch_progress);
        process_pending_without_pgraph(coordinator, ops, opaque);
        if (qemu_mutex_trylock(coordinator->renderer_lock) == 0) {
            break;
        }
        qemu_event_wait(coordinator->switch_progress);
    }

    while (ops->flush_is_pending(opaque)) {
        process_pending_without_pgraph(coordinator, ops, opaque);
    }

    while (*coordinator->framebuffer_in_use) {
        qemu_cond_wait(coordinator->framebuffer_released,
                       coordinator->renderer_lock);
    }

    qemu_mutex_lock(coordinator->pgraph_lock);
    ops->finalize_renderer(opaque);
    ops->init_renderer(opaque);
    qemu_mutex_unlock(coordinator->pgraph_lock);
    qemu_mutex_unlock(coordinator->renderer_lock);
    qemu_event_set(coordinator->switch_progress);

    qemu_mutex_lock(coordinator->pfifo_lock);
    ops->complete(opaque);
    qatomic_set(coordinator->handoff_pending, 0);
    qemu_event_set(coordinator->switch_complete);
}

void pgraph_renderer_switch_framebuffer_acquire_begin(
    PGRAPHRendererSwitchCoordinator *coordinator)
{
    for (;;) {
        while (qatomic_read(coordinator->handoff_pending)) {
            qemu_event_wait(coordinator->switch_complete);
        }

        qemu_mutex_lock(coordinator->renderer_lock);
        if (!qatomic_read(coordinator->handoff_pending)) {
            break;
        }
        qemu_mutex_unlock(coordinator->renderer_lock);
    }

    assert(!*coordinator->framebuffer_in_use);
    *coordinator->framebuffer_in_use = true;
}

void pgraph_renderer_switch_framebuffer_acquire_end(
    PGRAPHRendererSwitchCoordinator *coordinator)
{
    qemu_mutex_unlock(coordinator->renderer_lock);
    qemu_event_set(coordinator->switch_progress);
}

void pgraph_renderer_switch_framebuffer_release(
    PGRAPHRendererSwitchCoordinator *coordinator)
{
    qemu_mutex_lock(coordinator->renderer_lock);
    assert(*coordinator->framebuffer_in_use);
    *coordinator->framebuffer_in_use = false;
    qemu_cond_broadcast(coordinator->framebuffer_released);
    qemu_mutex_unlock(coordinator->renderer_lock);
    qemu_event_set(coordinator->switch_progress);
}
