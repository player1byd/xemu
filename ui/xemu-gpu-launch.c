/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "xemu-gpu-launch.h"

static XemuGpuLaunchRequest current_request;

void xemu_gpu_launch_request_init(XemuGpuLaunchRequest *request)
{
    *request = (XemuGpuLaunchRequest) {
        .selection = {
            .kind = PGRAPH_VK_SELECTION_AUTOMATIC,
            .allow_software = true,
        },
        .selection_source = XEMU_GPU_SELECTION_SOURCE_AUTOMATIC,
    };
}

static XemuGpuLaunchParseStatus parse_gpu_value(
    XemuGpuLaunchRequest *request, const char *value)
{
    if (!strcmp(value, "auto")) {
        request->selection = (PGRAPHVkSelectionRequest) {
            .kind = PGRAPH_VK_SELECTION_AUTOMATIC,
            .allow_software = true,
        };
    } else if (!strncmp(value, "uuid:", strlen("uuid:"))) {
        request->selection = (PGRAPHVkSelectionRequest) {
            .kind = PGRAPH_VK_SELECTION_UUID,
        };
        if (!pgraph_vk_device_uuid_parse(value + strlen("uuid:"),
                                          request->selection.device_uuid)) {
            return XEMU_GPU_LAUNCH_PARSE_INVALID_DEVICE;
        }
    } else {
        return XEMU_GPU_LAUNCH_PARSE_INVALID_DEVICE;
    }
    request->selection_source = XEMU_GPU_SELECTION_SOURCE_CLI;
    return XEMU_GPU_LAUNCH_PARSE_OK;
}

XemuGpuLaunchParseStatus xemu_gpu_launch_parse_early(
    int argc, char **argv, XemuGpuLaunchRequest *request)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL) {
            continue;
        }
        if (!strcmp(argv[i], "-gpu")) {
            if (request->gpu_option_seen) {
                return XEMU_GPU_LAUNCH_PARSE_DUPLICATE_OPTION;
            }
            if (i + 1 >= argc || argv[i + 1] == NULL) {
                return XEMU_GPU_LAUNCH_PARSE_MISSING_VALUE;
            }
            XemuGpuLaunchParseStatus status =
                parse_gpu_value(request, argv[i + 1]);
            if (status != XEMU_GPU_LAUNCH_PARSE_OK) {
                return status;
            }
            request->gpu_option_seen = true;
            argv[i] = NULL;
            argv[++i] = NULL;
        } else if (!strcmp(argv[i], "-gpu-strict")) {
            request->strict = true;
            argv[i] = NULL;
        } else if (!strcmp(argv[i], "-list-gpus")) {
            request->list_gpus = true;
            argv[i] = NULL;
        } else if (!strcmp(argv[i], "-gpu-info")) {
            if (request->info_option_seen) {
                return XEMU_GPU_LAUNCH_PARSE_DUPLICATE_OPTION;
            }
            if (i + 1 >= argc || argv[i + 1] == NULL) {
                return XEMU_GPU_LAUNCH_PARSE_MISSING_VALUE;
            }
            request->info_path = argv[i + 1];
            request->info_option_seen = true;
            argv[i] = NULL;
            argv[++i] = NULL;
        }
    }

    if (request->strict &&
        request->selection.kind == PGRAPH_VK_SELECTION_AUTOMATIC) {
        request->selection.allow_software = false;
    }
    return XEMU_GPU_LAUNCH_PARSE_OK;
}

XemuGpuLaunchParseStatus xemu_gpu_launch_apply_saved(
    XemuGpuLaunchRequest *request, const char *device_uuid,
    const char *legacy_name)
{
    if (request->selection_source == XEMU_GPU_SELECTION_SOURCE_CLI) {
        return XEMU_GPU_LAUNCH_PARSE_OK;
    }

    if (device_uuid != NULL && device_uuid[0] != '\0') {
        request->selection = (PGRAPHVkSelectionRequest) {
            .kind = PGRAPH_VK_SELECTION_UUID,
        };
        if (!pgraph_vk_device_uuid_parse(device_uuid,
                                          request->selection.device_uuid)) {
            return XEMU_GPU_LAUNCH_PARSE_INVALID_DEVICE;
        }
        request->selection_source = XEMU_GPU_SELECTION_SOURCE_CONFIG_UUID;
    } else if (legacy_name != NULL && legacy_name[0] != '\0') {
        request->selection = (PGRAPHVkSelectionRequest) {
            .kind = PGRAPH_VK_SELECTION_LEGACY_NAME,
            .legacy_name = legacy_name,
        };
        request->selection_source = XEMU_GPU_SELECTION_SOURCE_LEGACY;
    } else {
        request->selection = (PGRAPHVkSelectionRequest) {
            .kind = PGRAPH_VK_SELECTION_AUTOMATIC,
            .allow_software = !request->strict,
        };
        request->selection_source = XEMU_GPU_SELECTION_SOURCE_AUTOMATIC;
    }
    return XEMU_GPU_LAUNCH_PARSE_OK;
}

const char *xemu_gpu_launch_parse_status_string(
    XemuGpuLaunchParseStatus status)
{
    switch (status) {
    case XEMU_GPU_LAUNCH_PARSE_OK:
        return "success";
    case XEMU_GPU_LAUNCH_PARSE_MISSING_VALUE:
        return "GPU option requires a value";
    case XEMU_GPU_LAUNCH_PARSE_INVALID_DEVICE:
        return "GPU must be 'auto' or 'uuid:<32 hexadecimal digits>'";
    case XEMU_GPU_LAUNCH_PARSE_DUPLICATE_OPTION:
        return "GPU option was specified more than once";
    default:
        return "unknown GPU option error";
    }
}

void xemu_gpu_launch_request_set_current(
    const XemuGpuLaunchRequest *request)
{
    current_request = *request;
}

const XemuGpuLaunchRequest *xemu_gpu_launch_request_get(void)
{
    return &current_request;
}

bool xemu_gpu_strict_mode(void)
{
    return current_request.strict;
}
