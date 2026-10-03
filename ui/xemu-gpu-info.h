/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_XEMU_GPU_INFO_H
#define UI_XEMU_GPU_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "hw/xbox/nv2a/pgraph/vk/device-selection.h"
#include "ui/xemu-gpu-launch.h"

typedef enum XemuGpuInfoState {
    XEMU_GPU_INFO_INVENTORY,
    XEMU_GPU_INFO_INITIALIZED,
    XEMU_GPU_INFO_FAILED,
} XemuGpuInfoState;

typedef enum XemuGpuPresentationMode {
    XEMU_GPU_PRESENTATION_UNKNOWN,
    XEMU_GPU_PRESENTATION_SHARED,
    XEMU_GPU_PRESENTATION_HOST_COPY,
} XemuGpuPresentationMode;

typedef struct XemuGpuInfoDocument {
    XemuGpuInfoState state;
    const XemuGpuLaunchRequest *request;
    const PGRAPHVkDeviceRecord *devices;
    size_t device_count;
    const PGRAPHVkDeviceRecord *actual_device;
    const char *requested_backend;
    const char *actual_backend;
    bool fallback_used;
    const char *fallback_reason;
    XemuGpuPresentationMode presentation_mode;
    const char *presentation_vendor;
    const char *presentation_renderer;
    const char *error_message;
} XemuGpuInfoDocument;

char *xemu_gpu_info_render_json(const XemuGpuInfoDocument *document);
bool xemu_gpu_info_write_atomic(const char *path, const char *contents,
                                char *error, size_t error_size);
void xemu_gpu_info_record_inventory(const PGRAPHVkDeviceRecord *devices,
                                    size_t count);
const PGRAPHVkDeviceRecord *xemu_gpu_info_get_inventory(size_t *count);
const PGRAPHVkDeviceRecord *xemu_gpu_info_get_actual_device(void);
void xemu_gpu_info_set_actual_device(const PGRAPHVkDeviceRecord *device);
bool xemu_gpu_info_record_initialized(const PGRAPHVkDeviceRecord *device,
                                      const char *actual_backend,
                                      XemuGpuPresentationMode mode,
                                      bool fallback_used,
                                      const char *fallback_reason);
bool xemu_gpu_info_record_presentation(XemuGpuPresentationMode mode,
                                       const char *vendor,
                                       const char *renderer);
bool xemu_gpu_info_record_failure(const char *actual_backend,
                                  const char *message, bool fallback_used,
                                  const char *fallback_reason);

#ifdef __cplusplus
}
#endif

#endif
