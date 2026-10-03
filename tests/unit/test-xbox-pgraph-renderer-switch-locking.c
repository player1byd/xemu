/*
 * NV2A renderer-switch lock ordering tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"

#include "hw/xbox/nv2a/pgraph/renderer-switch.h"

typedef struct TestSwitch {
    PGRAPHRendererSwitchCoordinator coordinator;
    PGRAPHRendererSwitchOps ops;

    QemuMutex pfifo_lock;
    QemuMutex pgraph_lock;
    QemuMutex renderer_lock;
    QemuCond framebuffer_released;
    QemuEvent switch_progress;
    QemuEvent switch_complete;

    QemuMutex state_lock;
    QemuCond state_changed;
    bool framebuffer_in_use;
    int handoff_pending;
    bool flush_pending;
    bool sync_pending;
    bool acquisition_entered;
    bool allow_sync_publish;
    bool second_acquisition_started;
    bool second_acquisition_finished;
    unsigned int process_pending_count;
    unsigned int completion_count;
    char order[16];
    size_t order_length;
} TestSwitch;

static void append_order(TestSwitch *test, char event)
{
    qemu_mutex_lock(&test->state_lock);
    g_assert_cmpuint(test->order_length, <, sizeof(test->order) - 1);
    test->order[test->order_length++] = event;
    test->order[test->order_length] = '\0';
    qemu_cond_broadcast(&test->state_changed);
    qemu_mutex_unlock(&test->state_lock);
}

static void wait_for_state(TestSwitch *test, const bool *state)
{
    qemu_mutex_lock(&test->state_lock);
    while (!*state) {
        qemu_cond_wait(&test->state_changed, &test->state_lock);
    }
    qemu_mutex_unlock(&test->state_lock);
}

static void publish_flush(void *opaque)
{
    TestSwitch *test = opaque;

    /* The production phase owns PGRAPH here, but never PFIFO. */
    g_assert_cmpint(qemu_mutex_trylock(&test->pfifo_lock), ==, 0);
    qemu_mutex_unlock(&test->pfifo_lock);
    test->flush_pending = true;
}

static bool flush_is_pending(void *opaque)
{
    TestSwitch *test = opaque;

    return test->flush_pending;
}

static void process_pending(void *opaque)
{
    TestSwitch *test = opaque;

    /* Backend process_pending() enters with PFIFO, never PGRAPH. */
    g_assert_cmpint(qemu_mutex_trylock(&test->pgraph_lock), ==, 0);
    qemu_mutex_unlock(&test->pgraph_lock);

    qemu_mutex_lock(&test->state_lock);
    test->process_pending_count++;
    test->flush_pending = false;
    if (!test->allow_sync_publish) {
        test->allow_sync_publish = true;
    }
    if (test->sync_pending) {
        test->sync_pending = false;
        qemu_cond_broadcast(&test->state_changed);
    }
    qemu_cond_broadcast(&test->state_changed);
    qemu_mutex_unlock(&test->state_lock);
}

static void finalize_renderer(void *opaque)
{
    TestSwitch *test = opaque;

    /* Finalize may own PGRAPH, but must not own PFIFO. */
    g_assert_cmpint(qemu_mutex_trylock(&test->pfifo_lock), ==, 0);
    qemu_mutex_unlock(&test->pfifo_lock);
    append_order(test, 'F');
}

static void init_renderer(void *opaque)
{
    TestSwitch *test = opaque;

    /* Init follows finalize with the same lock contract. */
    g_assert_cmpint(qemu_mutex_trylock(&test->pfifo_lock), ==, 0);
    qemu_mutex_unlock(&test->pfifo_lock);
    append_order(test, 'I');
}

static void complete_switch(void *opaque)
{
    TestSwitch *test = opaque;

    qemu_mutex_lock(&test->state_lock);
    test->completion_count++;
    qemu_mutex_unlock(&test->state_lock);
    append_order(test, 'C');
}

static void test_switch_init(TestSwitch *test)
{
    *test = (TestSwitch) { 0 };
    qemu_mutex_init(&test->pfifo_lock);
    qemu_mutex_init(&test->pgraph_lock);
    qemu_mutex_init(&test->renderer_lock);
    qemu_cond_init(&test->framebuffer_released);
    qemu_event_init(&test->switch_progress, false);
    qemu_event_init(&test->switch_complete, false);
    qemu_mutex_init(&test->state_lock);
    qemu_cond_init(&test->state_changed);

    test->coordinator = (PGRAPHRendererSwitchCoordinator) {
        .pfifo_lock = &test->pfifo_lock,
        .pgraph_lock = &test->pgraph_lock,
        .renderer_lock = &test->renderer_lock,
        .framebuffer_released = &test->framebuffer_released,
        .switch_progress = &test->switch_progress,
        .switch_complete = &test->switch_complete,
        .framebuffer_in_use = &test->framebuffer_in_use,
        .handoff_pending = &test->handoff_pending,
    };
    test->ops = (PGRAPHRendererSwitchOps) {
        .publish_flush = publish_flush,
        .flush_is_pending = flush_is_pending,
        .process_pending = process_pending,
        .finalize_renderer = finalize_renderer,
        .init_renderer = init_renderer,
        .complete = complete_switch,
    };
}

static void test_switch_destroy(TestSwitch *test)
{
    qemu_cond_destroy(&test->state_changed);
    qemu_mutex_destroy(&test->state_lock);
    qemu_event_destroy(&test->switch_complete);
    qemu_event_destroy(&test->switch_progress);
    qemu_cond_destroy(&test->framebuffer_released);
    qemu_mutex_destroy(&test->renderer_lock);
    qemu_mutex_destroy(&test->pgraph_lock);
    qemu_mutex_destroy(&test->pfifo_lock);
}

static void *acquire_with_sync(void *opaque)
{
    TestSwitch *test = opaque;

    pgraph_renderer_switch_framebuffer_acquire_begin(&test->coordinator);

    qemu_mutex_lock(&test->state_lock);
    test->acquisition_entered = true;
    qemu_cond_broadcast(&test->state_changed);
    qemu_mutex_unlock(&test->state_lock);

    /* Publish after the switch's first service pass to test late progress. */
    wait_for_state(test, &test->allow_sync_publish);

    qemu_mutex_lock(&test->pfifo_lock);
    qemu_mutex_lock(&test->state_lock);
    test->sync_pending = true;
    qemu_cond_broadcast(&test->state_changed);
    qemu_mutex_unlock(&test->state_lock);
    qemu_event_set(&test->switch_progress);
    qemu_mutex_unlock(&test->pfifo_lock);

    qemu_mutex_lock(&test->state_lock);
    while (test->sync_pending) {
        qemu_cond_wait(&test->state_changed, &test->state_lock);
    }
    qemu_mutex_unlock(&test->state_lock);

    pgraph_renderer_switch_framebuffer_acquire_end(&test->coordinator);
    pgraph_renderer_switch_framebuffer_release(&test->coordinator);
    return NULL;
}

static void test_acquisition_sync_progresses_before_handoff(void)
{
    TestSwitch test;
    QemuThread acquisition;

    test_switch_init(&test);
    qemu_mutex_lock(&test.pfifo_lock);
    qemu_thread_create(&acquisition, "renderer-acquire", acquire_with_sync,
                       &test, QEMU_THREAD_JOINABLE);
    wait_for_state(&test, &test.acquisition_entered);

    pgraph_renderer_switch_run(&test.coordinator, &test.ops, &test);
    qemu_mutex_unlock(&test.pfifo_lock);
    qemu_thread_join(&acquisition);

    g_assert_false(test.sync_pending);
    g_assert_false(test.flush_pending);
    g_assert_false(test.framebuffer_in_use);
    g_assert_cmpuint(test.process_pending_count, ==, 2);
    g_assert_cmpuint(test.completion_count, ==, 1);
    g_assert_cmpstr(test.order, ==, "FIC");
    test_switch_destroy(&test);
}

static void *run_switch(void *opaque)
{
    TestSwitch *test = opaque;

    qemu_mutex_lock(&test->pfifo_lock);
    pgraph_renderer_switch_run(&test->coordinator, &test->ops, test);
    qemu_mutex_unlock(&test->pfifo_lock);
    return NULL;
}

static void *acquire_after_handoff(void *opaque)
{
    TestSwitch *test = opaque;

    qemu_mutex_lock(&test->state_lock);
    test->second_acquisition_started = true;
    qemu_cond_broadcast(&test->state_changed);
    qemu_mutex_unlock(&test->state_lock);

    pgraph_renderer_switch_framebuffer_acquire_begin(&test->coordinator);
    append_order(test, 'A');
    pgraph_renderer_switch_framebuffer_acquire_end(&test->coordinator);
    pgraph_renderer_switch_framebuffer_release(&test->coordinator);

    qemu_mutex_lock(&test->state_lock);
    test->second_acquisition_finished = true;
    qemu_cond_broadcast(&test->state_changed);
    qemu_mutex_unlock(&test->state_lock);
    return NULL;
}

static void test_lease_drains_and_new_acquisition_waits(void)
{
    TestSwitch test;
    QemuThread switch_thread;
    QemuThread acquisition_thread;

    test_switch_init(&test);
    pgraph_renderer_switch_framebuffer_acquire_begin(&test.coordinator);
    pgraph_renderer_switch_framebuffer_acquire_end(&test.coordinator);

    qemu_thread_create(&switch_thread, "renderer-switch", run_switch, &test,
                       QEMU_THREAD_JOINABLE);

    qemu_mutex_lock(&test.state_lock);
    while (test.process_pending_count == 0) {
        qemu_cond_wait(&test.state_changed, &test.state_lock);
    }
    qemu_mutex_unlock(&test.state_lock);

    qemu_thread_create(&acquisition_thread, "renderer-reacquire",
                       acquire_after_handoff, &test, QEMU_THREAD_JOINABLE);
    wait_for_state(&test, &test.second_acquisition_started);

    pgraph_renderer_switch_framebuffer_release(&test.coordinator);
    qemu_thread_join(&switch_thread);
    qemu_thread_join(&acquisition_thread);

    g_assert_true(test.second_acquisition_finished);
    g_assert_cmpstr(test.order, ==, "FICA");
    g_assert_cmpuint(test.completion_count, ==, 1);
    g_assert_cmpint(qatomic_read(&test.handoff_pending), ==, 0);

    /* A following switch must not inherit handoff or completion state. */
    qemu_event_reset(&test.switch_complete);
    qemu_mutex_lock(&test.pfifo_lock);
    pgraph_renderer_switch_run(&test.coordinator, &test.ops, &test);
    qemu_mutex_unlock(&test.pfifo_lock);
    g_assert_cmpuint(test.completion_count, ==, 2);
    g_assert_cmpstr(test.order, ==, "FICAFIC");
    test_switch_destroy(&test);
}

static void *wait_for_switch_completion(void *opaque)
{
    TestSwitch *test = opaque;

    qemu_event_wait(&test->switch_complete);
    append_order(test, 'W');
    return NULL;
}

static void test_vcpu_waiter_observes_installed_renderer(void)
{
    TestSwitch test;
    QemuThread waiter;

    test_switch_init(&test);
    qemu_thread_create(&waiter, "renderer-vcpu-wait",
                       wait_for_switch_completion, &test,
                       QEMU_THREAD_JOINABLE);

    qemu_mutex_lock(&test.pfifo_lock);
    pgraph_renderer_switch_run(&test.coordinator, &test.ops, &test);
    qemu_mutex_unlock(&test.pfifo_lock);
    qemu_thread_join(&waiter);

    g_assert_cmpstr(test.order, ==, "FICW");
    g_assert_cmpuint(test.completion_count, ==, 1);
    g_assert_cmpint(qatomic_read(&test.handoff_pending), ==, 0);
    test_switch_destroy(&test);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/pgraph/renderer-switch/acquisition-sync",
                    test_acquisition_sync_progresses_before_handoff);
    g_test_add_func("/xbox/pgraph/renderer-switch/framebuffer-handoff",
                    test_lease_drains_and_new_acquisition_waits);
    g_test_add_func("/xbox/pgraph/renderer-switch/vcpu-waiter",
                    test_vcpu_waiter_observes_installed_renderer);
    return g_test_run();
}
