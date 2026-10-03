/*
 * NV2A Vulkan hybrid slow-frame attribution tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "glib/gstdio.h"

#include "hw/xbox/nv2a/pgraph/vk/hybrid-trace.h"

static char *new_trace_path(void)
{
    char *path = NULL;
    int fd = g_file_open_tmp("xemu-hybrid-trace-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return path;
}

static void test_only_slow_frames_are_written(void)
{
    g_autofree char *path = new_trace_path();
    PGRAPHVkHybridTrace *trace =
        pgraph_vk_hybrid_trace_open(path, UINT64_MAX);
    g_autofree char *contents = NULL;

    g_assert_nonnull(trace);
    pgraph_vk_hybrid_trace_draw(trace);
    pgraph_vk_hybrid_trace_record(
        trace, VK_HYBRID_TRACE_PIPELINE_PROBE, 1, 7, 8, 9,
        10, 11, 12, 13);
    pgraph_vk_hybrid_trace_frame(trace);
    pgraph_vk_hybrid_trace_close(trace);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "hybrid-trace-v1"));
    g_assert_null(strstr(contents, "\nframe,"));
    g_unlink(path);
}

static void test_slow_frame_ring_reports_overflow(void)
{
    g_autofree char *path = new_trace_path();
    PGRAPHVkHybridTrace *trace = pgraph_vk_hybrid_trace_open(path, 0);
    g_autofree char *contents = NULL;
    unsigned int event_count = 0;
    unsigned int dropped = 0;

    g_assert_nonnull(trace);
    pgraph_vk_hybrid_trace_draw(trace);
    for (unsigned int i = 0; i < 16385; i++) {
        pgraph_vk_hybrid_trace_record(
            trace, VK_HYBRID_TRACE_PIPELINE_PROBE, 1, 7, 8, 9,
            i, 0, 0, 0);
    }
    pgraph_vk_hybrid_trace_frame(trace);
    pgraph_vk_hybrid_trace_close(trace);

    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    const char *frame = strstr(contents, "\nframe,");
    g_assert_nonnull(frame);
    g_assert_cmpint(sscanf(frame, "\nframe,%*u,%*u,%u,%u",
                           &event_count, &dropped), ==, 2);
    g_assert_cmpuint(event_count, ==, 16384);
    g_assert_cmpuint(dropped, ==, 1);
    g_unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/vk/hybrid-trace/slow-only",
                    test_only_slow_frames_are_written);
    g_test_add_func("/xbox/vk/hybrid-trace/overflow",
                    test_slow_frame_ring_reports_overflow);
    return g_test_run();
}
