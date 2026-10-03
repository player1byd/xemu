/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_DEVICE_INVENTORY_H
#define HW_XBOX_NV2A_PGRAPH_VK_DEVICE_INVENTORY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device-selection.h"

typedef struct Error Error;

#define PGRAPH_VK_MAKE_API_VERSION(major, minor, patch) \
    (((major) << 22) | ((minor) << 12) | (patch))
#define PGRAPH_VK_API_VERSION_1_0 PGRAPH_VK_MAKE_API_VERSION(1, 0, 0)
#define PGRAPH_VK_API_VERSION_1_1 PGRAPH_VK_MAKE_API_VERSION(1, 1, 0)

typedef enum PGRAPHVkRequiredFeature {
    PGRAPH_VK_FEATURE_DEPTH_CLAMP = 1 << 0,
    PGRAPH_VK_FEATURE_FILL_MODE_NON_SOLID = 1 << 1,
    PGRAPH_VK_FEATURE_GEOMETRY_SHADER = 1 << 2,
    PGRAPH_VK_FEATURE_OCCLUSION_QUERY_PRECISE = 1 << 3,
    PGRAPH_VK_FEATURE_SHADER_CLIP_DISTANCE = 1 << 4,
    PGRAPH_VK_FEATURE_GEOMETRY_POINT_SIZE = 1 << 5,
} PGRAPHVkRequiredFeature;

typedef enum PGRAPHVkShaderTarget {
    PGRAPH_VK_SHADER_TARGET_VULKAN_1_1,
    PGRAPH_VK_SHADER_TARGET_VULKAN_1_2,
    PGRAPH_VK_SHADER_TARGET_VULKAN_1_3,
} PGRAPHVkShaderTarget;

#define PGRAPH_VK_REQUIRED_FEATURES \
    (PGRAPH_VK_FEATURE_DEPTH_CLAMP | \
     PGRAPH_VK_FEATURE_FILL_MODE_NON_SOLID | \
     PGRAPH_VK_FEATURE_GEOMETRY_SHADER | \
     PGRAPH_VK_FEATURE_OCCLUSION_QUERY_PRECISE | \
     PGRAPH_VK_FEATURE_SHADER_CLIP_DISTANCE | \
     PGRAPH_VK_FEATURE_GEOMETRY_POINT_SIZE)

typedef struct PGRAPHVkDeviceCapabilities {
    uint32_t api_version;
    bool has_graphics_compute_queue;
    bool has_external_memory;
    bool has_external_semaphore;
    uint32_t available_required_features;
} PGRAPHVkDeviceCapabilities;

typedef enum PGRAPHVkEnumerateStatus {
    PGRAPH_VK_ENUMERATE_SUCCESS,
    PGRAPH_VK_ENUMERATE_INCOMPLETE,
    PGRAPH_VK_ENUMERATE_FAILURE,
} PGRAPHVkEnumerateStatus;

typedef PGRAPHVkEnumerateStatus (*PGRAPHVkEnumerateDeviceTokens)(
    void *opaque, uint32_t *count, uintptr_t *tokens);

typedef enum PGRAPHVkEnumerationResult {
    PGRAPH_VK_ENUMERATION_OK,
    PGRAPH_VK_ENUMERATION_EMPTY,
    PGRAPH_VK_ENUMERATION_FAILED,
    PGRAPH_VK_ENUMERATION_UNSTABLE,
    PGRAPH_VK_ENUMERATION_ALLOCATION_FAILED,
} PGRAPHVkEnumerationResult;

PGRAPHVkEnumerationResult pgraph_vk_enumerate_device_tokens(
    PGRAPHVkEnumerateDeviceTokens enumerate, void *opaque,
    uintptr_t **tokens, size_t *count);
void pgraph_vk_device_record_check_renderer_support(
    PGRAPHVkDeviceRecord *record,
    const PGRAPHVkDeviceCapabilities *capabilities);
PGRAPHVkShaderTarget pgraph_vk_shader_target_for_api(uint32_t api_version);
bool pgraph_vk_probe_device_inventory(PGRAPHVkDeviceRecord **records,
                                      size_t *count, Error **errp);

#ifdef __cplusplus
}
#endif

#endif
