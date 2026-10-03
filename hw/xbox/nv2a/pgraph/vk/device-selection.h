/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_DEVICE_SELECTION_H
#define HW_XBOX_NV2A_PGRAPH_VK_DEVICE_SELECTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PGRAPH_VK_DEVICE_UUID_SIZE 16
#define PGRAPH_VK_DEVICE_NAME_SIZE 256
#define PGRAPH_VK_DEVICE_UUID_STRING_SIZE \
    (PGRAPH_VK_DEVICE_UUID_SIZE * 2 + 1)

typedef enum PGRAPHVkDeviceType {
    PGRAPH_VK_DEVICE_TYPE_OTHER,
    PGRAPH_VK_DEVICE_TYPE_INTEGRATED,
    PGRAPH_VK_DEVICE_TYPE_DISCRETE,
    PGRAPH_VK_DEVICE_TYPE_VIRTUAL,
    PGRAPH_VK_DEVICE_TYPE_CPU,
} PGRAPHVkDeviceType;

typedef struct PGRAPHVkDeviceRecord {
    char name[PGRAPH_VK_DEVICE_NAME_SIZE];
    uint8_t device_uuid[PGRAPH_VK_DEVICE_UUID_SIZE];
    uint8_t driver_uuid[PGRAPH_VK_DEVICE_UUID_SIZE];
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t api_version;
    uint32_t driver_version;
    PGRAPHVkDeviceType type;
    bool renderer_supported;
    const char *rejection_reason;
} PGRAPHVkDeviceRecord;

typedef enum PGRAPHVkSelectionKind {
    PGRAPH_VK_SELECTION_AUTOMATIC,
    PGRAPH_VK_SELECTION_UUID,
    PGRAPH_VK_SELECTION_LEGACY_NAME,
} PGRAPHVkSelectionKind;

typedef struct PGRAPHVkSelectionRequest {
    PGRAPHVkSelectionKind kind;
    uint8_t device_uuid[PGRAPH_VK_DEVICE_UUID_SIZE];
    const char *legacy_name;
    bool allow_software;
} PGRAPHVkSelectionRequest;

typedef enum PGRAPHVkSelectionStatus {
    PGRAPH_VK_SELECTION_OK,
    PGRAPH_VK_SELECTION_NO_DEVICES,
    PGRAPH_VK_SELECTION_NOT_FOUND,
    PGRAPH_VK_SELECTION_AMBIGUOUS,
    PGRAPH_VK_SELECTION_UNSUPPORTED,
    PGRAPH_VK_SELECTION_NO_HARDWARE,
} PGRAPHVkSelectionStatus;

typedef struct PGRAPHVkSelectionResult {
    PGRAPHVkSelectionStatus status;
    size_t index;
} PGRAPHVkSelectionResult;

bool pgraph_vk_device_uuid_parse(
    const char *text, uint8_t uuid[PGRAPH_VK_DEVICE_UUID_SIZE]);
void pgraph_vk_device_uuid_format(
    const uint8_t uuid[PGRAPH_VK_DEVICE_UUID_SIZE],
    char text[PGRAPH_VK_DEVICE_UUID_STRING_SIZE]);
PGRAPHVkSelectionResult pgraph_vk_resolve_device(
    const PGRAPHVkDeviceRecord *devices, size_t count,
    const PGRAPHVkSelectionRequest *request);
bool pgraph_vk_shared_presentation_supported(
    const PGRAPHVkDeviceRecord *device,
    bool has_external_memory,
    bool has_platform_handle,
    const uint8_t (*context_device_uuids)[PGRAPH_VK_DEVICE_UUID_SIZE],
    size_t context_device_count,
    const uint8_t context_driver_uuid[PGRAPH_VK_DEVICE_UUID_SIZE]);
const char *pgraph_vk_selection_status_string(PGRAPHVkSelectionStatus status);

#ifdef __cplusplus
}
#endif

#endif
