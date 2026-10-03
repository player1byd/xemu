/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "device-inventory.h"

#define PGRAPH_VK_ENUMERATION_ATTEMPTS 4

PGRAPHVkEnumerationResult pgraph_vk_enumerate_device_tokens(
    PGRAPHVkEnumerateDeviceTokens enumerate, void *opaque,
    uintptr_t **tokens, size_t *count)
{
    *tokens = NULL;
    *count = 0;

    for (unsigned int attempt = 0;
         attempt < PGRAPH_VK_ENUMERATION_ATTEMPTS; attempt++) {
        uint32_t queried_count = 0;
        PGRAPHVkEnumerateStatus status =
            enumerate(opaque, &queried_count, NULL);
        if (status == PGRAPH_VK_ENUMERATE_FAILURE) {
            return PGRAPH_VK_ENUMERATION_FAILED;
        }
        if (status == PGRAPH_VK_ENUMERATE_INCOMPLETE) {
            continue;
        }
        if (queried_count == 0) {
            return PGRAPH_VK_ENUMERATION_EMPTY;
        }

        uintptr_t *candidate = calloc(queried_count, sizeof(*candidate));
        if (candidate == NULL) {
            return PGRAPH_VK_ENUMERATION_ALLOCATION_FAILED;
        }

        uint32_t filled_count = queried_count;
        status = enumerate(opaque, &filled_count, candidate);
        if (status == PGRAPH_VK_ENUMERATE_SUCCESS &&
            filled_count <= queried_count) {
            *tokens = candidate;
            *count = filled_count;
            return filled_count == 0 ? PGRAPH_VK_ENUMERATION_EMPTY :
                                       PGRAPH_VK_ENUMERATION_OK;
        }
        free(candidate);

        if (status == PGRAPH_VK_ENUMERATE_FAILURE) {
            return PGRAPH_VK_ENUMERATION_FAILED;
        }
    }

    return PGRAPH_VK_ENUMERATION_UNSTABLE;
}

void pgraph_vk_device_record_check_renderer_support(
    PGRAPHVkDeviceRecord *record,
    const PGRAPHVkDeviceCapabilities *capabilities)
{
    record->renderer_supported = false;

    if (capabilities->api_version < PGRAPH_VK_API_VERSION_1_1) {
        record->rejection_reason = "Vulkan 1.1 is required";
    } else if (!capabilities->has_graphics_compute_queue) {
        record->rejection_reason =
            "combined graphics/compute queue is required";
    } else if (!capabilities->has_external_memory) {
        record->rejection_reason =
            "external memory is required for shared presentation";
    } else if (!capabilities->has_external_semaphore) {
        record->rejection_reason =
            "external semaphore is required for shared presentation";
    } else if ((capabilities->available_required_features &
                PGRAPH_VK_REQUIRED_FEATURES) != PGRAPH_VK_REQUIRED_FEATURES) {
        record->rejection_reason =
            "required physical-device feature is unavailable";
    } else {
        record->renderer_supported = true;
        record->rejection_reason = NULL;
    }
}

PGRAPHVkShaderTarget pgraph_vk_shader_target_for_api(uint32_t api_version)
{
    if (api_version >= PGRAPH_VK_MAKE_API_VERSION(1, 3, 0)) {
        return PGRAPH_VK_SHADER_TARGET_VULKAN_1_3;
    }
    if (api_version >= PGRAPH_VK_MAKE_API_VERSION(1, 2, 0)) {
        return PGRAPH_VK_SHADER_TARGET_VULKAN_1_2;
    }
    return PGRAPH_VK_SHADER_TARGET_VULKAN_1_1;
}
