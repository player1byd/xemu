/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "device-selection.h"

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool pgraph_vk_device_uuid_parse(
    const char *text, uint8_t uuid[PGRAPH_VK_DEVICE_UUID_SIZE])
{
    uint8_t parsed[PGRAPH_VK_DEVICE_UUID_SIZE];

    if (text == NULL || strlen(text) != PGRAPH_VK_DEVICE_UUID_SIZE * 2) {
        return false;
    }

    for (size_t i = 0; i < PGRAPH_VK_DEVICE_UUID_SIZE; i++) {
        int high = hex_value(text[i * 2]);
        int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        parsed[i] = high << 4 | low;
    }

    memcpy(uuid, parsed, sizeof(parsed));
    return true;
}

void pgraph_vk_device_uuid_format(
    const uint8_t uuid[PGRAPH_VK_DEVICE_UUID_SIZE],
    char text[PGRAPH_VK_DEVICE_UUID_STRING_SIZE])
{
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < PGRAPH_VK_DEVICE_UUID_SIZE; i++) {
        text[i * 2] = hex[uuid[i] >> 4];
        text[i * 2 + 1] = hex[uuid[i] & 0xf];
    }
    text[PGRAPH_VK_DEVICE_UUID_SIZE * 2] = '\0';
}

static PGRAPHVkSelectionResult selection_result(PGRAPHVkSelectionStatus status,
                                                size_t index)
{
    return (PGRAPHVkSelectionResult) {
        .status = status,
        .index = index,
    };
}

static PGRAPHVkSelectionResult resolve_uuid(
    const PGRAPHVkDeviceRecord *devices, size_t count,
    const PGRAPHVkSelectionRequest *request)
{
    size_t match = 0;
    size_t matches = 0;

    for (size_t i = 0; i < count; i++) {
        if (!memcmp(devices[i].device_uuid, request->device_uuid,
                    PGRAPH_VK_DEVICE_UUID_SIZE)) {
            match = i;
            matches++;
        }
    }

    if (matches == 0) {
        return selection_result(PGRAPH_VK_SELECTION_NOT_FOUND, 0);
    }
    if (matches > 1) {
        return selection_result(PGRAPH_VK_SELECTION_AMBIGUOUS, 0);
    }
    if (!devices[match].renderer_supported) {
        return selection_result(PGRAPH_VK_SELECTION_UNSUPPORTED, match);
    }
    return selection_result(PGRAPH_VK_SELECTION_OK, match);
}

static PGRAPHVkSelectionResult resolve_legacy_name(
    const PGRAPHVkDeviceRecord *devices, size_t count,
    const PGRAPHVkSelectionRequest *request)
{
    size_t match = 0;
    size_t matches = 0;

    if (request->legacy_name == NULL || request->legacy_name[0] == '\0') {
        return selection_result(PGRAPH_VK_SELECTION_NOT_FOUND, 0);
    }

    for (size_t i = 0; i < count; i++) {
        if (!strcmp(devices[i].name, request->legacy_name)) {
            match = i;
            matches++;
        }
    }

    if (matches == 0) {
        return selection_result(PGRAPH_VK_SELECTION_NOT_FOUND, 0);
    }
    if (matches > 1) {
        return selection_result(PGRAPH_VK_SELECTION_AMBIGUOUS, 0);
    }
    if (!devices[match].renderer_supported) {
        return selection_result(PGRAPH_VK_SELECTION_UNSUPPORTED, match);
    }
    return selection_result(PGRAPH_VK_SELECTION_OK, match);
}

static PGRAPHVkSelectionResult resolve_automatic(
    const PGRAPHVkDeviceRecord *devices, size_t count,
    const PGRAPHVkSelectionRequest *request)
{
    for (size_t i = 0; i < count; i++) {
        if (devices[i].renderer_supported &&
            devices[i].type != PGRAPH_VK_DEVICE_TYPE_CPU) {
            return selection_result(PGRAPH_VK_SELECTION_OK, i);
        }
    }

    if (request->allow_software) {
        for (size_t i = 0; i < count; i++) {
            if (devices[i].renderer_supported) {
                return selection_result(PGRAPH_VK_SELECTION_OK, i);
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (devices[i].renderer_supported &&
            devices[i].type == PGRAPH_VK_DEVICE_TYPE_CPU) {
            return selection_result(PGRAPH_VK_SELECTION_NO_HARDWARE, i);
        }
    }
    return selection_result(PGRAPH_VK_SELECTION_UNSUPPORTED, 0);
}

PGRAPHVkSelectionResult pgraph_vk_resolve_device(
    const PGRAPHVkDeviceRecord *devices, size_t count,
    const PGRAPHVkSelectionRequest *request)
{
    if (count == 0) {
        return selection_result(PGRAPH_VK_SELECTION_NO_DEVICES, 0);
    }

    switch (request->kind) {
    case PGRAPH_VK_SELECTION_UUID:
        return resolve_uuid(devices, count, request);
    case PGRAPH_VK_SELECTION_LEGACY_NAME:
        return resolve_legacy_name(devices, count, request);
    case PGRAPH_VK_SELECTION_AUTOMATIC:
        return resolve_automatic(devices, count, request);
    default:
        return selection_result(PGRAPH_VK_SELECTION_NOT_FOUND, 0);
    }
}

bool pgraph_vk_shared_presentation_supported(
    const PGRAPHVkDeviceRecord *device,
    bool has_external_memory,
    bool has_platform_handle,
    const uint8_t (*context_device_uuids)[PGRAPH_VK_DEVICE_UUID_SIZE],
    size_t context_device_count,
    const uint8_t context_driver_uuid[PGRAPH_VK_DEVICE_UUID_SIZE])
{
    if (!has_external_memory || !has_platform_handle || device == NULL ||
        context_device_uuids == NULL ||
        context_driver_uuid == NULL || context_device_count == 0 ||
        memcmp(device->driver_uuid, context_driver_uuid,
               PGRAPH_VK_DEVICE_UUID_SIZE)) {
        return false;
    }

    for (size_t i = 0; i < context_device_count; i++) {
        if (!memcmp(device->device_uuid, context_device_uuids[i],
                    PGRAPH_VK_DEVICE_UUID_SIZE)) {
            return true;
        }
    }
    return false;
}

const char *pgraph_vk_selection_status_string(PGRAPHVkSelectionStatus status)
{
    switch (status) {
    case PGRAPH_VK_SELECTION_OK:
        return "selected";
    case PGRAPH_VK_SELECTION_NO_DEVICES:
        return "no Vulkan devices found";
    case PGRAPH_VK_SELECTION_NOT_FOUND:
        return "requested device not found";
    case PGRAPH_VK_SELECTION_AMBIGUOUS:
        return "requested device identity is ambiguous";
    case PGRAPH_VK_SELECTION_UNSUPPORTED:
        return "requested device is not renderer-compatible";
    case PGRAPH_VK_SELECTION_NO_HARDWARE:
        return "no renderer-compatible hardware device found";
    default:
        return "unknown selection result";
    }
}
