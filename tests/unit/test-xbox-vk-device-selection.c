/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/xbox/nv2a/pgraph/vk/device-selection.h"

#include <assert.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const uint8_t uuid_a[PGRAPH_VK_DEVICE_UUID_SIZE] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

static const uint8_t uuid_b[PGRAPH_VK_DEVICE_UUID_SIZE] = {
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
};

static const uint8_t driver_a[PGRAPH_VK_DEVICE_UUID_SIZE] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static const uint8_t driver_b[PGRAPH_VK_DEVICE_UUID_SIZE] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
};

static PGRAPHVkDeviceRecord make_device(const char *name, const uint8_t *uuid,
                                        PGRAPHVkDeviceType type,
                                        bool supported)
{
    PGRAPHVkDeviceRecord record = {
        .type = type,
        .renderer_supported = supported,
    };
    assert(strlen(name) < sizeof(record.name));
    strcpy(record.name, name);
    memcpy(record.device_uuid, uuid, PGRAPH_VK_DEVICE_UUID_SIZE);
    return record;
}

static void test_uuid_round_trip(void)
{
    char formatted[PGRAPH_VK_DEVICE_UUID_STRING_SIZE];
    uint8_t parsed[PGRAPH_VK_DEVICE_UUID_SIZE];

    pgraph_vk_device_uuid_format(uuid_a, formatted);
    assert(!strcmp(formatted, "00112233445566778899aabbccddeeff"));
    assert(pgraph_vk_device_uuid_parse(
        "00112233445566778899AABBCCDDEEFF", parsed));
    assert(!memcmp(parsed, uuid_a, sizeof(uuid_a)));
}

static void test_uuid_rejects_malformed_values(void)
{
    static const char *invalid[] = {
        "", "0011", "00112233445566778899aabbccddeeff00",
        "00112233445566778899aabbccddeezz",
        "00112233-4455-6677-8899-aabbccddeeff",
    };
    uint8_t parsed[PGRAPH_VK_DEVICE_UUID_SIZE];

    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        memset(parsed, 0xa5, sizeof(parsed));
        assert(!pgraph_vk_device_uuid_parse(invalid[i], parsed));
        const uint8_t unchanged[PGRAPH_VK_DEVICE_UUID_SIZE] = {
            [0 ... PGRAPH_VK_DEVICE_UUID_SIZE - 1] = 0xa5,
        };
        assert(!memcmp(parsed, unchanged, sizeof(unchanged)));
    }
}

static void test_automatic_prefers_first_supported_hardware(void)
{
    const PGRAPHVkDeviceRecord devices[] = {
        make_device("CPU", uuid_a, PGRAPH_VK_DEVICE_TYPE_CPU, true),
        make_device("AMD", uuid_b, PGRAPH_VK_DEVICE_TYPE_INTEGRATED, true),
    };
    const PGRAPHVkSelectionRequest request = {
        .kind = PGRAPH_VK_SELECTION_AUTOMATIC,
    };
    PGRAPHVkSelectionResult result =
        pgraph_vk_resolve_device(devices, ARRAY_SIZE(devices), &request);

    assert(result.status == PGRAPH_VK_SELECTION_OK);
    assert(result.index == 1);
}

static void test_exact_uuid_does_not_fall_back_from_unsupported_device(void)
{
    const PGRAPHVkDeviceRecord devices[] = {
        make_device("Requested", uuid_a, PGRAPH_VK_DEVICE_TYPE_DISCRETE, false),
        make_device("Other", uuid_b, PGRAPH_VK_DEVICE_TYPE_INTEGRATED, true),
    };
    PGRAPHVkSelectionRequest request = {
        .kind = PGRAPH_VK_SELECTION_UUID,
    };
    memcpy(request.device_uuid, uuid_a, sizeof(request.device_uuid));

    PGRAPHVkSelectionResult result =
        pgraph_vk_resolve_device(devices, ARRAY_SIZE(devices), &request);
    assert(result.status == PGRAPH_VK_SELECTION_UNSUPPORTED);
}

static void test_exact_uuid_ignores_enumeration_order(void)
{
    const PGRAPHVkDeviceRecord devices[] = {
        make_device("NVIDIA", uuid_b, PGRAPH_VK_DEVICE_TYPE_DISCRETE, true),
        make_device("AMD", uuid_a, PGRAPH_VK_DEVICE_TYPE_INTEGRATED, true),
    };
    PGRAPHVkSelectionRequest request = {
        .kind = PGRAPH_VK_SELECTION_UUID,
    };
    memcpy(request.device_uuid, uuid_a, sizeof(request.device_uuid));

    PGRAPHVkSelectionResult result =
        pgraph_vk_resolve_device(devices, ARRAY_SIZE(devices), &request);
    assert(result.status == PGRAPH_VK_SELECTION_OK);
    assert(result.index == 1);
}

static void test_exact_uuid_rejects_missing_and_duplicate_matches(void)
{
    const PGRAPHVkDeviceRecord missing[] = {
        make_device("NVIDIA", uuid_b, PGRAPH_VK_DEVICE_TYPE_DISCRETE, true),
    };
    const PGRAPHVkDeviceRecord duplicate[] = {
        make_device("GPU 0", uuid_a, PGRAPH_VK_DEVICE_TYPE_DISCRETE, true),
        make_device("GPU 1", uuid_a, PGRAPH_VK_DEVICE_TYPE_DISCRETE, true),
    };
    PGRAPHVkSelectionRequest request = {
        .kind = PGRAPH_VK_SELECTION_UUID,
    };
    memcpy(request.device_uuid, uuid_a, sizeof(request.device_uuid));

    assert(pgraph_vk_resolve_device(missing, ARRAY_SIZE(missing),
                                    &request).status ==
           PGRAPH_VK_SELECTION_NOT_FOUND);
    assert(pgraph_vk_resolve_device(duplicate, ARRAY_SIZE(duplicate),
                                    &request).status ==
           PGRAPH_VK_SELECTION_AMBIGUOUS);
}

static void test_legacy_name_rejects_ambiguity(void)
{
    const PGRAPHVkDeviceRecord devices[] = {
        make_device("Same GPU", uuid_a, PGRAPH_VK_DEVICE_TYPE_DISCRETE, true),
        make_device("Same GPU", uuid_b, PGRAPH_VK_DEVICE_TYPE_DISCRETE, true),
    };
    const PGRAPHVkSelectionRequest request = {
        .kind = PGRAPH_VK_SELECTION_LEGACY_NAME,
        .legacy_name = "Same GPU",
    };

    assert(pgraph_vk_resolve_device(devices, ARRAY_SIZE(devices),
                                    &request).status ==
           PGRAPH_VK_SELECTION_AMBIGUOUS);
}

static void test_software_requires_explicit_permission(void)
{
    const PGRAPHVkDeviceRecord devices[] = {
        make_device("Software", uuid_a, PGRAPH_VK_DEVICE_TYPE_CPU, true),
    };
    PGRAPHVkSelectionRequest request = {
        .kind = PGRAPH_VK_SELECTION_AUTOMATIC,
    };

    assert(pgraph_vk_resolve_device(devices, ARRAY_SIZE(devices),
                                    &request).status ==
           PGRAPH_VK_SELECTION_NO_HARDWARE);
    request.allow_software = true;
    assert(pgraph_vk_resolve_device(devices, ARRAY_SIZE(devices),
                                    &request).status ==
           PGRAPH_VK_SELECTION_OK);
}

static void test_shared_presentation_requires_extensions_device_and_driver(void)
{
    PGRAPHVkDeviceRecord device =
        make_device("Vulkan GPU", uuid_a, PGRAPH_VK_DEVICE_TYPE_INTEGRATED,
                    true);
    memcpy(device.driver_uuid, driver_a, sizeof(device.driver_uuid));

    uint8_t matching_devices[][PGRAPH_VK_DEVICE_UUID_SIZE] = {
        { 0 },
        { 0 },
    };
    memcpy(matching_devices[0], uuid_b, sizeof(uuid_b));
    memcpy(matching_devices[1], uuid_a, sizeof(uuid_a));

    assert(pgraph_vk_shared_presentation_supported(
        &device, true, true, matching_devices, ARRAY_SIZE(matching_devices),
        driver_a));
    assert(!pgraph_vk_shared_presentation_supported(
        &device, false, true, matching_devices, ARRAY_SIZE(matching_devices),
        driver_a));
    assert(!pgraph_vk_shared_presentation_supported(
        &device, true, false, matching_devices, ARRAY_SIZE(matching_devices),
        driver_a));
    assert(!pgraph_vk_shared_presentation_supported(
        &device, true, true, matching_devices, ARRAY_SIZE(matching_devices),
        driver_b));

    uint8_t other_device[][PGRAPH_VK_DEVICE_UUID_SIZE] = {
        { 0 },
    };
    memcpy(other_device[0], uuid_b, sizeof(uuid_b));
    assert(!pgraph_vk_shared_presentation_supported(
        &device, true, true, other_device, ARRAY_SIZE(other_device), driver_a));
    assert(!pgraph_vk_shared_presentation_supported(
        &device, true, true, NULL, 0, driver_a));
}

int main(void)
{
    test_uuid_round_trip();
    test_uuid_rejects_malformed_values();
    test_automatic_prefers_first_supported_hardware();
    test_exact_uuid_does_not_fall_back_from_unsupported_device();
    test_exact_uuid_ignores_enumeration_order();
    test_exact_uuid_rejects_missing_and_duplicate_matches();
    test_legacy_name_rejects_ambiguity();
    test_software_requires_explicit_permission();
    test_shared_presentation_requires_extensions_device_and_driver();
    return 0;
}
