/*
 * NV2A renderer-switch lock coordination
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_RENDERER_SWITCH_H
#define HW_XBOX_NV2A_PGRAPH_RENDERER_SWITCH_H

#include "qemu/thread.h"

typedef struct PGRAPHRendererSwitchCoordinator {
    QemuMutex *pfifo_lock;
    QemuMutex *pgraph_lock;
    QemuMutex *renderer_lock;
    QemuCond *framebuffer_released;
    QemuEvent *switch_progress;
    QemuEvent *switch_complete;
    bool *framebuffer_in_use;
    int *handoff_pending;
} PGRAPHRendererSwitchCoordinator;

typedef struct PGRAPHRendererSwitchOps {
    void (*publish_flush)(void *opaque);
    bool (*flush_is_pending)(void *opaque);
    void (*process_pending)(void *opaque);
    void (*finalize_renderer)(void *opaque);
    void (*init_renderer)(void *opaque);
    void (*complete)(void *opaque);
} PGRAPHRendererSwitchOps;

/*
 * Renderer-switch lock order:
 *
 *   - backend process_pending enters with PFIFO and without PGRAPH;
 *   - PGRAPH is never held while PFIFO is acquired;
 *   - framebuffer and switch waits hold neither PFIFO nor PGRAPH;
 *   - renderer replacement holds renderer_lock and PGRAPH, without PFIFO.
 *
 * The switch-complete event must be reset before pgraph_renderer_switch_run.
 */
/* The caller enters and returns with PFIFO locked. */
void pgraph_renderer_switch_run(
    PGRAPHRendererSwitchCoordinator *coordinator,
    const PGRAPHRendererSwitchOps *ops, void *opaque);

/* acquire_begin returns with renderer_lock held; acquire_end releases it. */
void pgraph_renderer_switch_framebuffer_acquire_begin(
    PGRAPHRendererSwitchCoordinator *coordinator);
void pgraph_renderer_switch_framebuffer_acquire_end(
    PGRAPHRendererSwitchCoordinator *coordinator);
void pgraph_renderer_switch_framebuffer_release(
    PGRAPHRendererSwitchCoordinator *coordinator);

#endif
