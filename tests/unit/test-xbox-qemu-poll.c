/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Focused Windows tests for xemu's short-deadline polling path. */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/timer.h"

#include <windows.h>

#define SHORT_TIMEOUT_NS 500000
#define READY_TIMEOUT_NS 1200000

typedef struct NestedPollApcContext {
    HANDLE outer_event;
    HANDLE nested_events[MAXIMUM_WAIT_OBJECTS];
    GPollFD nested_fds[MAXIMUM_WAIT_OBJECTS];
    bool invoked;
    int nested_result;
} NestedPollApcContext;

static ULONG_PTR pointer_to_apc_parameter(gpointer pointer)
{
    return (ULONG_PTR)(uintptr_t)pointer;
}

static GPollFD pollfd_from_handle(HANDLE handle)
{
    return (GPollFD) {
        .fd = (gintptr)handle,
        .events = G_IO_IN,
    };
}

static int64_t poll_elapsed_us(GPollFD *fds, guint nfds, int64_t timeout,
                               int *result)
{
    int64_t start = g_get_monotonic_time();

    *result = qemu_poll_ns(fds, nfds, timeout);
    return g_get_monotonic_time() - start;
}

static void assert_short_timeout(GPollFD *fds, guint nfds)
{
    int64_t elapsed_us;
    int result;

    elapsed_us = poll_elapsed_us(fds, nfds, SHORT_TIMEOUT_NS, &result);
    g_assert_cmpint(result, ==, 0);
    g_assert_cmpint(elapsed_us, >=, 50);
    g_assert_cmpint(elapsed_us, <, 100000);
}

static void test_timeout_only(void)
{
    GPollFD unused = { 0 };

    assert_short_timeout(&unused, 0);
}

static void test_ready_handle_then_timeout(void)
{
    HANDLE event = CreateEventW(NULL, TRUE, TRUE, NULL);
    GPollFD fd;
    int64_t elapsed_us;
    int result;

    g_assert_nonnull(event);
    fd = pollfd_from_handle(event);
    elapsed_us = poll_elapsed_us(&fd, 1, READY_TIMEOUT_NS, &result);
    g_assert_cmpint(result, ==, 1);
    g_assert_cmpint(fd.revents, ==, G_IO_IN);
    /* A pre-signaled handle must interrupt rather than consume the deadline. */
    g_assert_cmpint(elapsed_us, <, 1000);

    g_assert_true(ResetEvent(event));
    fd.revents = 0;
    assert_short_timeout(&fd, 1);
    g_assert_cmpint(fd.revents, ==, 0);
    CloseHandle(event);
}

static void test_auto_reset_handle(void)
{
    HANDLE event = CreateEventW(NULL, FALSE, TRUE, NULL);
    GPollFD fd;
    int result;

    g_assert_nonnull(event);
    fd = pollfd_from_handle(event);
    result = qemu_poll_ns(&fd, 1, READY_TIMEOUT_NS);
    g_assert_cmpint(result, ==, 1);
    g_assert_cmpint(fd.revents, ==, G_IO_IN);
    g_assert_cmpint(WaitForSingleObject(event, 0), ==, WAIT_TIMEOUT);
    CloseHandle(event);
}

static void test_poll_error_clears_revents(void)
{
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE restricted_event = NULL;
    GPollFD fd;
    int result;

    g_assert_nonnull(event);
    g_assert_true(DuplicateHandle(GetCurrentProcess(), event,
                                  GetCurrentProcess(), &restricted_event,
                                  EVENT_MODIFY_STATE, FALSE, 0));
    g_assert_nonnull(restricted_event);
    CloseHandle(event);

    fd = pollfd_from_handle(restricted_event);
    fd.revents = G_IO_IN;
    g_test_expect_message("GLib", G_LOG_LEVEL_WARNING,
                          "*WaitForMultipleObjectsEx failed*");
    result = qemu_poll_ns(&fd, 1, SHORT_TIMEOUT_NS);
    g_test_assert_expected_messages();
    g_assert_cmpint(result, <, 0);
    g_assert_cmpint(fd.revents, ==, 0);
    CloseHandle(restricted_event);
}

static void test_raced_timer_does_not_leak(void)
{
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    GPollFD fd;

    g_assert_nonnull(event);
    fd = pollfd_from_handle(event);

    /*
     * Repeatedly make the original event and a near-immediate timer ready.
     * The following timeout must never consume a timer signal left behind by
     * the preceding poll.
     */
    for (unsigned int i = 0; i < 256; i++) {
        int result;

        g_assert_true(SetEvent(event));
        fd.revents = 0;
        result = qemu_poll_ns(&fd, 1, 100);
        g_assert_cmpint(result, ==, 1);
        g_assert_cmpint(fd.revents, ==, G_IO_IN);

        g_assert_true(ResetEvent(event));
        fd.revents = 0;
        assert_short_timeout(&fd, 1);
    }
    CloseHandle(event);
}

static void test_multiple_ready_handles(void)
{
    HANDLE events[2];
    GPollFD fds[2];
    int result;

    for (unsigned int i = 0; i < G_N_ELEMENTS(events); i++) {
        events[i] = CreateEventW(NULL, TRUE, TRUE, NULL);
        g_assert_nonnull(events[i]);
        fds[i] = pollfd_from_handle(events[i]);
    }

    result = qemu_poll_ns(fds, G_N_ELEMENTS(fds), READY_TIMEOUT_NS);
    g_assert_cmpint(result, ==, 2);
    for (unsigned int i = 0; i < G_N_ELEMENTS(events); i++) {
        g_assert_cmpint(fds[i].revents, ==, G_IO_IN);
        CloseHandle(events[i]);
    }
}

static void test_windows_handle_boundary(void)
{
    HANDLE events[MAXIMUM_WAIT_OBJECTS];
    GPollFD fds[MAXIMUM_WAIT_OBJECTS];
    int result;

    for (unsigned int i = 0; i < G_N_ELEMENTS(events); i++) {
        events[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_assert_nonnull(events[i]);
        fds[i] = pollfd_from_handle(events[i]);
    }

    /* qemu_poll_ns appends its private timer, taking this above 64 handles. */
    g_assert_true(SetEvent(events[G_N_ELEMENTS(events) - 1]));
    result = qemu_poll_ns(fds, G_N_ELEMENTS(fds), READY_TIMEOUT_NS);
    g_assert_cmpint(result, ==, 1);
    for (unsigned int i = 0; i < G_N_ELEMENTS(events); i++) {
        g_assert_cmpint(fds[i].revents, ==,
                        i == G_N_ELEMENTS(events) - 1 ? G_IO_IN : 0);
        CloseHandle(events[i]);
    }
}

static void test_windows_handle_boundary_timer_only(void)
{
    HANDLE events[MAXIMUM_WAIT_OBJECTS];
    GPollFD fds[MAXIMUM_WAIT_OBJECTS];
    int result;

    for (unsigned int i = 0; i < G_N_ELEMENTS(events); i++) {
        events[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_assert_nonnull(events[i]);
        fds[i] = pollfd_from_handle(events[i]);
    }

    result = qemu_poll_ns(fds, G_N_ELEMENTS(fds), SHORT_TIMEOUT_NS);
    g_assert_cmpint(result, ==, 0);
    for (unsigned int i = 0; i < G_N_ELEMENTS(events); i++) {
        g_assert_cmpint(fds[i].revents, ==, 0);
        CloseHandle(events[i]);
    }
}

static VOID CALLBACK nested_poll_apc(ULONG_PTR parameter)
{
    NestedPollApcContext *ctx = (NestedPollApcContext *)parameter;

    ctx->invoked = true;
    ctx->nested_result = qemu_poll_ns(ctx->nested_fds,
                                      G_N_ELEMENTS(ctx->nested_fds),
                                      100000);
    SetEvent(ctx->outer_event);
}

static void test_nested_alertable_wait(void)
{
    NestedPollApcContext ctx = { 0 };
    HANDLE current_thread;
    GPollFD outer_fd;
    int result;

    ctx.outer_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_assert_nonnull(ctx.outer_event);
    for (unsigned int i = 0; i < G_N_ELEMENTS(ctx.nested_events); i++) {
        ctx.nested_events[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_assert_nonnull(ctx.nested_events[i]);
        ctx.nested_fds[i] = pollfd_from_handle(ctx.nested_events[i]);
    }

    current_thread = OpenThread(THREAD_SET_CONTEXT, FALSE,
                                GetCurrentThreadId());
    g_assert_nonnull(current_thread);
    g_assert_cmpuint(QueueUserAPC(nested_poll_apc, current_thread,
                                 pointer_to_apc_parameter(&ctx)), !=, 0);

    outer_fd = pollfd_from_handle(ctx.outer_event);
    result = qemu_poll_ns(&outer_fd, 1, READY_TIMEOUT_NS);
    g_assert_true(ctx.invoked);
    g_assert_cmpint(ctx.nested_result, ==, 0);

    /* GLib may report the APC as a spurious wake before reporting the event. */
    if (result == 0) {
        outer_fd.revents = 0;
        result = qemu_poll_ns(&outer_fd, 1, READY_TIMEOUT_NS);
    }
    g_assert_cmpint(result, ==, 1);
    g_assert_cmpint(outer_fd.revents, ==, G_IO_IN);

    CloseHandle(current_thread);
    CloseHandle(ctx.outer_event);
    for (unsigned int i = 0; i < G_N_ELEMENTS(ctx.nested_events); i++) {
        CloseHandle(ctx.nested_events[i]);
    }
}

static void *concurrent_timeout_thread(void *opaque)
{
    unsigned int count = GPOINTER_TO_UINT(opaque);

    for (unsigned int i = 0; i < count; i++) {
        if (qemu_poll_ns(NULL, 0, 100000) != 0) {
            return GINT_TO_POINTER(1);
        }
    }
    return NULL;
}

static void test_concurrent_thread_lifetimes(void)
{
    QemuThread threads[4];

    for (unsigned int i = 0; i < G_N_ELEMENTS(threads); i++) {
        qemu_thread_create(&threads[i], "qemu-poll-test",
                           concurrent_timeout_thread,
                           GUINT_TO_POINTER(32), QEMU_THREAD_JOINABLE);
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(threads); i++) {
        g_assert_null(qemu_thread_join(&threads[i]));
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_assert_false(qemu_poll_get_cpu_saving());
    qemu_poll_set_cpu_saving(true);
    g_test_add_func("/xbox/qemu-poll/timeout-only", test_timeout_only);
    g_test_add_func("/xbox/qemu-poll/ready-then-timeout",
                    test_ready_handle_then_timeout);
    g_test_add_func("/xbox/qemu-poll/auto-reset-handle",
                    test_auto_reset_handle);
    g_test_add_func("/xbox/qemu-poll/error-clears-revents",
                    test_poll_error_clears_revents);
    g_test_add_func("/xbox/qemu-poll/raced-timer-does-not-leak",
                    test_raced_timer_does_not_leak);
    g_test_add_func("/xbox/qemu-poll/multiple-ready-handles",
                    test_multiple_ready_handles);
    g_test_add_func("/xbox/qemu-poll/windows-handle-boundary",
                    test_windows_handle_boundary);
    g_test_add_func("/xbox/qemu-poll/windows-handle-boundary-timer-only",
                    test_windows_handle_boundary_timer_only);
    g_test_add_func("/xbox/qemu-poll/nested-alertable-wait",
                    test_nested_alertable_wait);
    g_test_add_func("/xbox/qemu-poll/concurrent-thread-lifetimes",
                    test_concurrent_thread_lifetimes);
    return g_test_run();
}
