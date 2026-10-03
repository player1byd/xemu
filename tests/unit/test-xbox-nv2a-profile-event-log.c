/*
 * NV2A opt-in event log tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>

#include "qemu/log.h"
#include "hw/xbox/nv2a/debug.h"

static void assert_occurs_once(const char *contents, const char *needle)
{
    const char *first = strstr(contents, needle);

    g_assert_nonnull(first);
    g_assert_null(strstr(first + strlen(needle), needle));
}

static void test_events_are_written_once(void)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    size_t length;
    int fd;

    fd = g_file_open_tmp("xemu-event-log-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    g_assert_true(qemu_set_log_filename_flags(path, 0, NULL));
    nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_GPU_SUBMIT);
    g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
    g_assert_cmpuint(length, ==, 0);
    g_clear_pointer(&contents, g_free);

    g_assert_true(qemu_set_log(qemu_str_to_log_mask("nv2a"), NULL));

    nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_GPU_SUBMIT);
    nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_GPU_SUBMIT);
    nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_SHADER_COMPILE);
    nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_SHADER_COMPILE);
    nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_READBACK);
    nv2a_profile_log_event_once(NV2A_PROFILE_EVENT_READBACK);

    g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
    g_assert_cmpuint(length, >, 0);
    assert_occurs_once(contents, "\"event\":\"gpu_submit\"");
    assert_occurs_once(contents, "\"event\":\"shader_compile\"");
    assert_occurs_once(contents, "\"event\":\"readback\"");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nv2a/profile/event-log-once",
                    test_events_are_written_once);

    return g_test_run();
}
