/*
 * NV2A Vulkan hybrid worker-to-PFIFO wake tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/vk/renderer.h"

typedef struct HybridWakeTest {
    NV2AState *d;
    QemuMutex lock;
    QemuCond ready;
    bool waiter_ready;
    bool woke;
} HybridWakeTest;

static void *pfifo_waiter(void *opaque)
{
    HybridWakeTest *test = opaque;

    qemu_mutex_lock(&test->d->pfifo.lock);
    qemu_mutex_lock(&test->lock);
    test->waiter_ready = true;
    qemu_cond_broadcast(&test->ready);
    qemu_mutex_unlock(&test->lock);

    while (!test->d->pfifo.fifo_kick) {
        qemu_cond_wait(&test->d->pfifo.fifo_cond,
                       &test->d->pfifo.lock);
    }
    test->woke = true;
    qemu_mutex_unlock(&test->d->pfifo.lock);
    return NULL;
}

static void test_completion_wakes_sleeping_pfifo_owner(void)
{
    NV2AState *d = g_new0(NV2AState, 1);
    PGRAPHVkState *r = g_new0(PGRAPHVkState, 1);
    HybridWakeTest test = { .d = d };
    QemuThread thread;

    d->pgraph.vk_renderer_state = r;
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_mutex_init(&test.lock);
    qemu_cond_init(&test.ready);

    qemu_thread_create(&thread, "hybrid-pfifo-wait", pfifo_waiter,
                       &test, QEMU_THREAD_JOINABLE);
    qemu_mutex_lock(&test.lock);
    while (!test.waiter_ready) {
        qemu_cond_wait(&test.ready, &test.lock);
    }
    qemu_mutex_unlock(&test.lock);

    /* No draw, flip, or UI event participates in this wake. */
    pgraph_vk_hybrid_worker_notify(d);
    qemu_thread_join(&thread);

    g_assert_true(test.woke);
    g_assert_true(d->pfifo.fifo_kick);
    g_assert_cmpint(qatomic_read(&r->hybrid_completion_kick_pending), ==, 1);

    /* Completion notifications are coalesced until the owner services them. */
    d->pfifo.fifo_kick = false;
    pgraph_vk_hybrid_worker_notify(d);
    g_assert_false(d->pfifo.fifo_kick);

    /*
     * Worker stop/join precedes renderer destruction; late no-owner calls are
     * harmless and do not touch the PFIFO synchronization state.
     */
    d->pgraph.vk_renderer_state = NULL;
    pgraph_vk_hybrid_worker_notify(d);

    qemu_cond_destroy(&test.ready);
    qemu_mutex_destroy(&test.lock);
    qemu_cond_destroy(&d->pfifo.fifo_cond);
    qemu_mutex_destroy(&d->pfifo.lock);
    g_free(r);
    g_free(d);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/hybrid-wake/sleeping-pfifo-owner",
                    test_completion_wakes_sleeping_pfifo_owner);
    return g_test_run();
}
