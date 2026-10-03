/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/vk/device-inventory.h"
#include "ui/xemu-gpu-info.h"

#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>

static PGRAPHVkDeviceRecord make_device(const char *name, uint8_t uuid_byte,
                                        bool supported)
{
    PGRAPHVkDeviceRecord device = {
        .vendor_id = 0x1002,
        .device_id = 0x164e,
        .api_version = PGRAPH_VK_MAKE_API_VERSION(1, 3, 0),
        .driver_version = 0x12345678,
        .type = PGRAPH_VK_DEVICE_TYPE_INTEGRATED,
        .renderer_supported = supported,
        .rejection_reason = supported ? NULL : "missing required feature",
    };
    snprintf(device.name, sizeof(device.name), "%s", name);
    memset(device.device_uuid, uuid_byte, sizeof(device.device_uuid));
    memset(device.driver_uuid, uuid_byte + 1, sizeof(device.driver_uuid));
    return device;
}

static void assert_contains(const char *text, const char *expected)
{
    assert(strstr(text, expected) != NULL);
}

static void test_inventory_json(void)
{
    PGRAPHVkDeviceRecord devices[] = {
        make_device("AMD \"Accelerator\"\\GPU", 0x11, true),
        make_device("Unsupported", 0x22, false),
    };
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);
    request.list_gpus = true;

    XemuGpuInfoDocument document = {
        .state = XEMU_GPU_INFO_INVENTORY,
        .request = &request,
        .devices = devices,
        .device_count = 2,
        .requested_backend = "Vulkan",
        .presentation_mode = XEMU_GPU_PRESENTATION_UNKNOWN,
    };
    char *json = xemu_gpu_info_render_json(&document);
    assert(json != NULL);
    assert_contains(json, "\"schema_version\":1");
    assert_contains(json, "\"state\":\"inventory\"");
    assert_contains(json, "AMD \\\"Accelerator\\\"\\\\GPU");
    assert_contains(json, "\"actual_device\":null");
    assert_contains(json, "\"actual_backend\":null");
    assert_contains(json, "\"compatible\":true");
    assert_contains(json, "\"compatible\":false");
    assert_contains(json, "\"rejection_reason\":null");
    assert_contains(json, "missing required feature");
    assert_contains(json, "\"mode\":\"unknown\"");
    assert_contains(json, "\"gl_vendor\":null");
    assert_contains(json, "\"gl_renderer\":null");
    free(json);
}

static void test_initialized_json(void)
{
    PGRAPHVkDeviceRecord device = make_device("AMD GPU", 0xaa, true);
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);
    request.strict = true;
    request.selection_source = XEMU_GPU_SELECTION_SOURCE_CONFIG_UUID;
    request.selection.kind = PGRAPH_VK_SELECTION_UUID;
    memcpy(request.selection.device_uuid, device.device_uuid,
           sizeof(request.selection.device_uuid));

    XemuGpuInfoDocument document = {
        .state = XEMU_GPU_INFO_INITIALIZED,
        .request = &request,
        .devices = &device,
        .device_count = 1,
        .actual_device = &device,
        .requested_backend = "Vulkan",
        .actual_backend = "Vulkan",
        .presentation_mode = XEMU_GPU_PRESENTATION_SHARED,
        .presentation_vendor = "NVIDIA Corporation",
        .presentation_renderer = "NVIDIA GPU",
    };
    char *json = xemu_gpu_info_render_json(&document);
    assert(json != NULL);
    assert_contains(json, "\"state\":\"initialized\"");
    assert_contains(json, "\"source\":\"saved_uuid\"");
    assert_contains(json,
                    "\"selector\":\"uuid:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"");
    assert_contains(json, "\"strict\":true");
    assert_contains(json, "\"actual_device\":{");
    assert_contains(json, "\"mode\":\"shared\"");
    assert_contains(json, "\"gl_vendor\":\"NVIDIA Corporation\"");
    assert_contains(json, "\"gl_renderer\":\"NVIDIA GPU\"");
    assert_contains(json, "\"error\":null");
    free(json);
}

static void test_failed_json(void)
{
    XemuGpuLaunchRequest request;
    xemu_gpu_launch_request_init(&request);
    XemuGpuInfoDocument document = {
        .state = XEMU_GPU_INFO_FAILED,
        .request = &request,
        .requested_backend = "Vulkan",
        .fallback_used = true,
        .fallback_reason = "requested device unavailable",
        .presentation_mode = XEMU_GPU_PRESENTATION_UNKNOWN,
        .error_message = "device initialization failed",
    };
    char *json = xemu_gpu_info_render_json(&document);
    assert(json != NULL);
    assert_contains(json, "\"state\":\"failed\"");
    assert_contains(json, "\"used\":true");
    assert_contains(json, "requested device unavailable");
    assert_contains(json, "device initialization failed");
    free(json);
}

static void test_atomic_replacement(void)
{
    char directory[] = "xemu-gpu-info-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char path[512];
    char temporary[520];
    snprintf(path, sizeof(path), "%s/info.json", directory);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);

    FILE *initial = fopen(path, "wb");
    assert(initial != NULL);
    assert(fputs("old", initial) >= 0);
    assert(fclose(initial) == 0);

    char error[256];
    assert(xemu_gpu_info_write_atomic(path, "{\"new\":true}\n",
                                      error, sizeof(error)));
    FILE *result = fopen(path, "rb");
    assert(result != NULL);
    char buffer[64] = { 0 };
    assert(fread(buffer, 1, sizeof(buffer) - 1, result) == 13);
    assert(fclose(result) == 0);
    assert(strcmp(buffer, "{\"new\":true}\n") == 0);
    assert(access(temporary, F_OK) != 0);

    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
}

static void test_atomic_failure_preserves_original(void)
{
    char error[256] = { 0 };
    assert(!xemu_gpu_info_write_atomic("/does/not/exist/info.json", "{}",
                                       error, sizeof(error)));
    assert(error[0] != '\0');
}

int main(void)
{
    test_inventory_json();
    test_initialized_json();
    test_failed_json();
    test_atomic_replacement();
    test_atomic_failure_preserves_original();
    return 0;
}
