/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/xemu-gpu-launch.h"

#include <assert.h>
#include <string.h>

static void test_cli_uuid_is_process_local_and_removed(void)
{
    char *argv[] = {
        "xemu", "-gpu", "uuid:00112233445566778899AABBCCDDEEFF",
        "-gpu-strict", "-config_path", "config.toml",
    };
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);

    assert(xemu_gpu_launch_parse_early(6, argv, &request) ==
           XEMU_GPU_LAUNCH_PARSE_OK);
    assert(request.selection_source == XEMU_GPU_SELECTION_SOURCE_CLI);
    assert(request.selection.kind == PGRAPH_VK_SELECTION_UUID);
    assert(request.strict);
    assert(argv[1] == NULL && argv[2] == NULL && argv[3] == NULL);
    assert(!strcmp(argv[4], "-config_path"));
    assert(!strcmp(argv[5], "config.toml"));
}

static void test_cli_auto_and_listing_options(void)
{
    char *argv[] = {
        "xemu", "-list-gpus", "-gpu", "auto", "-gpu-info", "gpu.json",
    };
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);

    assert(xemu_gpu_launch_parse_early(6, argv, &request) ==
           XEMU_GPU_LAUNCH_PARSE_OK);
    assert(request.list_gpus);
    assert(request.selection.kind == PGRAPH_VK_SELECTION_AUTOMATIC);
    assert(request.selection_source == XEMU_GPU_SELECTION_SOURCE_CLI);
    assert(!strcmp(request.info_path, "gpu.json"));
    for (size_t i = 1; i < 6; i++) {
        assert(argv[i] == NULL);
    }
}

static void test_cli_rejects_missing_malformed_and_conflicting_values(void)
{
    char *missing[] = { "xemu", "-gpu" };
    char *malformed[] = { "xemu", "-gpu", "uuid:not-a-uuid" };
    char *name[] = { "xemu", "-gpu", "AMD Radeon" };
    char *duplicate[] = { "xemu", "-gpu", "auto", "-gpu", "auto" };
    char *info_missing[] = { "xemu", "-gpu-info" };

    struct {
        int argc;
        char **argv;
        XemuGpuLaunchParseStatus expected;
    } cases[] = {
        { 2, missing, XEMU_GPU_LAUNCH_PARSE_MISSING_VALUE },
        { 3, malformed, XEMU_GPU_LAUNCH_PARSE_INVALID_DEVICE },
        { 3, name, XEMU_GPU_LAUNCH_PARSE_INVALID_DEVICE },
        { 5, duplicate, XEMU_GPU_LAUNCH_PARSE_DUPLICATE_OPTION },
        { 2, info_missing, XEMU_GPU_LAUNCH_PARSE_MISSING_VALUE },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        XemuGpuLaunchRequest request;
        xemu_gpu_launch_request_init(&request);
        assert(xemu_gpu_launch_parse_early(cases[i].argc, cases[i].argv,
                                           &request) == cases[i].expected);
    }
}

static void test_saved_uuid_precedes_legacy_name_without_mutation(void)
{
    const char saved_uuid[] = "00112233445566778899aabbccddeeff";
    const char saved_name[] = "Saved GPU";
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);

    assert(xemu_gpu_launch_apply_saved(&request, saved_uuid, saved_name) ==
           XEMU_GPU_LAUNCH_PARSE_OK);
    assert(request.selection_source == XEMU_GPU_SELECTION_SOURCE_CONFIG_UUID);
    assert(request.selection.kind == PGRAPH_VK_SELECTION_UUID);
    assert(!strcmp(saved_uuid, "00112233445566778899aabbccddeeff"));
    assert(!strcmp(saved_name, "Saved GPU"));
}

static void test_saved_legacy_and_automatic_migration(void)
{
    XemuGpuLaunchRequest legacy;
    xemu_gpu_launch_request_init(&legacy);
    assert(xemu_gpu_launch_apply_saved(&legacy, "", "Legacy GPU") ==
           XEMU_GPU_LAUNCH_PARSE_OK);
    assert(legacy.selection_source == XEMU_GPU_SELECTION_SOURCE_LEGACY);
    assert(legacy.selection.kind == PGRAPH_VK_SELECTION_LEGACY_NAME);
    assert(!strcmp(legacy.selection.legacy_name, "Legacy GPU"));

    XemuGpuLaunchRequest automatic;
    xemu_gpu_launch_request_init(&automatic);
    assert(xemu_gpu_launch_apply_saved(&automatic, "", "") ==
           XEMU_GPU_LAUNCH_PARSE_OK);
    assert(automatic.selection_source == XEMU_GPU_SELECTION_SOURCE_AUTOMATIC);
    assert(automatic.selection.kind == PGRAPH_VK_SELECTION_AUTOMATIC);
}

static void test_cli_precedence_ignores_saved_values(void)
{
    char *argv[] = { "xemu", "-gpu", "auto" };
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);
    assert(xemu_gpu_launch_parse_early(3, argv, &request) ==
           XEMU_GPU_LAUNCH_PARSE_OK);

    assert(xemu_gpu_launch_apply_saved(
               &request, "00112233445566778899aabbccddeeff", "Legacy GPU") ==
           XEMU_GPU_LAUNCH_PARSE_OK);
    assert(request.selection_source == XEMU_GPU_SELECTION_SOURCE_CLI);
    assert(request.selection.kind == PGRAPH_VK_SELECTION_AUTOMATIC);
}

static void test_invalid_saved_uuid_is_reported(void)
{
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);
    assert(xemu_gpu_launch_apply_saved(&request, "bad", "Legacy GPU") ==
           XEMU_GPU_LAUNCH_PARSE_INVALID_DEVICE);
}

static void test_current_request_exposes_strict_policy(void)
{
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);
    assert(!xemu_gpu_strict_mode());

    request.strict = true;
    xemu_gpu_launch_request_set_current(&request);
    assert(xemu_gpu_launch_request_get()->strict);
    assert(xemu_gpu_strict_mode());

    request.strict = false;
    xemu_gpu_launch_request_set_current(&request);
}

int main(void)
{
    test_cli_uuid_is_process_local_and_removed();
    test_cli_auto_and_listing_options();
    test_cli_rejects_missing_malformed_and_conflicting_values();
    test_saved_uuid_precedes_legacy_name_without_mutation();
    test_saved_legacy_and_automatic_migration();
    test_cli_precedence_ignores_saved_values();
    test_invalid_saved_uuid_is_reported();
    test_current_request_exposes_strict_policy();
    return 0;
}
