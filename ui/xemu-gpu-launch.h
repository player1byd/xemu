/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_XEMU_GPU_LAUNCH_H
#define UI_XEMU_GPU_LAUNCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "hw/xbox/nv2a/pgraph/vk/device-selection.h"

typedef enum XemuGpuSelectionSource {
    XEMU_GPU_SELECTION_SOURCE_AUTOMATIC,
    XEMU_GPU_SELECTION_SOURCE_CONFIG_UUID,
    XEMU_GPU_SELECTION_SOURCE_LEGACY,
    XEMU_GPU_SELECTION_SOURCE_CLI,
} XemuGpuSelectionSource;

typedef enum XemuGpuLaunchParseStatus {
    XEMU_GPU_LAUNCH_PARSE_OK,
    XEMU_GPU_LAUNCH_PARSE_MISSING_VALUE,
    XEMU_GPU_LAUNCH_PARSE_INVALID_DEVICE,
    XEMU_GPU_LAUNCH_PARSE_DUPLICATE_OPTION,
} XemuGpuLaunchParseStatus;

typedef struct XemuGpuLaunchRequest {
    PGRAPHVkSelectionRequest selection;
    XemuGpuSelectionSource selection_source;
    const char *info_path;
    bool list_gpus;
    bool strict;
    bool gpu_option_seen;
    bool info_option_seen;
} XemuGpuLaunchRequest;

void xemu_gpu_launch_request_init(XemuGpuLaunchRequest *request);
XemuGpuLaunchParseStatus xemu_gpu_launch_parse_early(
    int argc, char **argv, XemuGpuLaunchRequest *request);
XemuGpuLaunchParseStatus xemu_gpu_launch_apply_saved(
    XemuGpuLaunchRequest *request, const char *device_uuid,
    const char *legacy_name);
const char *xemu_gpu_launch_parse_status_string(
    XemuGpuLaunchParseStatus status);
void xemu_gpu_launch_request_set_current(
    const XemuGpuLaunchRequest *request);
const XemuGpuLaunchRequest *xemu_gpu_launch_request_get(void);
bool xemu_gpu_strict_mode(void);

#ifdef __cplusplus
}
#endif

#endif
