/*
 * NV2A Vulkan text-debug macro tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>

#include "qemu/log.h"
#include "hw/xbox/nv2a/pgraph/vk/debug.h"

int nv2a_vk_dgroup_indent;
bool nv2a_vk_text_debug_enabled;

void pgraph_vk_text_debug_printf(const char *format, ...)
{
    FILE *logfile = qemu_log_trylock();

    g_assert_nonnull(logfile);

    va_list args;
    va_start(args, format);
    vfprintf(logfile, format, args);
    va_end(args);

    qemu_log_unlock(logfile);
}

static unsigned int format_evaluations;

static int evaluated_value(void)
{
    format_evaluations++;
    return 7;
}

int main(int argc, char **argv)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    size_t length;
    int fd;
    int mask;

    g_test_init(&argc, &argv, NULL);

    fd = g_file_open_tmp("xemu-nv2a-log-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    g_assert_true(qemu_set_log_filename_flags(path, 0, NULL));
    NV2A_VK_DGROUP_BEGIN("disabled group %d", evaluated_value());
    NV2A_VK_DPRINTF("disabled detail %d", evaluated_value());
    g_assert_cmpint(nv2a_vk_dgroup_indent, ==, 0);
    NV2A_VK_DGROUP_END();
    g_assert_cmpint(nv2a_vk_dgroup_indent, ==, 0);
    g_assert_cmpuint(format_evaluations, ==, 0);

    mask = qemu_str_to_log_mask("nv2a");
    g_assert_cmpint(mask, !=, 0);
    g_assert_true(qemu_set_log(mask, NULL));
    nv2a_vk_text_debug_enabled = true;

    NV2A_VK_DGROUP_BEGIN("enabled group %d", evaluated_value());
    g_assert_cmpint(nv2a_vk_dgroup_indent, ==, 1);
    NV2A_VK_DPRINTF("enabled detail %d", evaluated_value());
    NV2A_VK_DGROUP_END();
    g_assert_cmpint(nv2a_vk_dgroup_indent, ==, 0);
    g_assert_cmpuint(format_evaluations, ==, 2);

    g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
    g_assert_cmpuint(length, >, 0);
    g_assert_nonnull(strstr(contents, "enabled group 7"));
    g_assert_nonnull(strstr(contents, " enabled detail 7"));
    g_assert_null(strstr(contents, "disabled"));

    return 0;
}
