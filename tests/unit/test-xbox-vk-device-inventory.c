/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/xbox/nv2a/pgraph/vk/device-inventory.h"
#include "hw/xbox/nv2a/pgraph/vk/device-selection.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct EnumerationStep {
    PGRAPHVkEnumerateStatus status;
    uint32_t count;
    uintptr_t first_token;
} EnumerationStep;

typedef struct EnumerationFixture {
    const EnumerationStep *steps;
    size_t step_count;
    size_t next_step;
} EnumerationFixture;

static PGRAPHVkEnumerateStatus enumerate_fixture(void *opaque, uint32_t *count,
                                                 uintptr_t *tokens)
{
    EnumerationFixture *fixture = opaque;
    assert(fixture->next_step < fixture->step_count);
    const EnumerationStep *step = &fixture->steps[fixture->next_step++];

    *count = step->count;
    if (tokens != NULL) {
        for (uint32_t i = 0; i < step->count; i++) {
            tokens[i] = step->first_token + i;
        }
    }
    return step->status;
}

static PGRAPHVkEnumerationResult run_steps(const EnumerationStep *steps,
                                           size_t step_count,
                                           uintptr_t **tokens,
                                           size_t *count)
{
    EnumerationFixture fixture = {
        .steps = steps,
        .step_count = step_count,
    };
    return pgraph_vk_enumerate_device_tokens(enumerate_fixture, &fixture,
                                             tokens, count);
}

static void test_successful_two_call_enumeration(void)
{
    const EnumerationStep steps[] = {
        { PGRAPH_VK_ENUMERATE_SUCCESS, 2, 0 },
        { PGRAPH_VK_ENUMERATE_SUCCESS, 2, 100 },
    };
    uintptr_t *tokens = NULL;
    size_t count = 0;

    assert(run_steps(steps, 2, &tokens, &count) ==
           PGRAPH_VK_ENUMERATION_OK);
    assert(count == 2);
    assert(tokens[0] == 100);
    assert(tokens[1] == 101);
    free(tokens);
}

static void test_zero_devices_is_distinct(void)
{
    const EnumerationStep steps[] = {
        { PGRAPH_VK_ENUMERATE_SUCCESS, 0, 0 },
    };
    uintptr_t *tokens = (uintptr_t *)1;
    size_t count = 99;

    assert(run_steps(steps, 1, &tokens, &count) ==
           PGRAPH_VK_ENUMERATION_EMPTY);
    assert(tokens == NULL);
    assert(count == 0);
}

static void test_query_failure_is_reported(void)
{
    const EnumerationStep steps[] = {
        { PGRAPH_VK_ENUMERATE_FAILURE, 0, 0 },
    };
    uintptr_t *tokens = NULL;
    size_t count = 0;

    assert(run_steps(steps, 1, &tokens, &count) ==
           PGRAPH_VK_ENUMERATION_FAILED);
}

static void test_fill_failure_is_reported(void)
{
    const EnumerationStep steps[] = {
        { PGRAPH_VK_ENUMERATE_SUCCESS, 2, 0 },
        { PGRAPH_VK_ENUMERATE_FAILURE, 2, 0 },
    };
    uintptr_t *tokens = NULL;
    size_t count = 0;

    assert(run_steps(steps, 2, &tokens, &count) ==
           PGRAPH_VK_ENUMERATION_FAILED);
    assert(tokens == NULL);
    assert(count == 0);
}

static void test_incomplete_fill_retries_from_count_query(void)
{
    const EnumerationStep steps[] = {
        { PGRAPH_VK_ENUMERATE_SUCCESS, 1, 0 },
        { PGRAPH_VK_ENUMERATE_INCOMPLETE, 1, 10 },
        { PGRAPH_VK_ENUMERATE_SUCCESS, 2, 0 },
        { PGRAPH_VK_ENUMERATE_SUCCESS, 2, 20 },
    };
    uintptr_t *tokens = NULL;
    size_t count = 0;

    assert(run_steps(steps, 4, &tokens, &count) ==
           PGRAPH_VK_ENUMERATION_OK);
    assert(count == 2);
    assert(tokens[0] == 20);
    assert(tokens[1] == 21);
    free(tokens);
}

static void test_count_shrink_is_accepted(void)
{
    const EnumerationStep steps[] = {
        { PGRAPH_VK_ENUMERATE_SUCCESS, 3, 0 },
        { PGRAPH_VK_ENUMERATE_SUCCESS, 1, 30 },
    };
    uintptr_t *tokens = NULL;
    size_t count = 0;

    assert(run_steps(steps, 2, &tokens, &count) ==
           PGRAPH_VK_ENUMERATION_OK);
    assert(count == 1);
    assert(tokens[0] == 30);
    free(tokens);
}

static void test_repeated_incomplete_is_bounded(void)
{
    const EnumerationStep steps[] = {
        { PGRAPH_VK_ENUMERATE_SUCCESS, 1, 0 },
        { PGRAPH_VK_ENUMERATE_INCOMPLETE, 1, 0 },
        { PGRAPH_VK_ENUMERATE_SUCCESS, 2, 0 },
        { PGRAPH_VK_ENUMERATE_INCOMPLETE, 2, 0 },
        { PGRAPH_VK_ENUMERATE_SUCCESS, 3, 0 },
        { PGRAPH_VK_ENUMERATE_INCOMPLETE, 3, 0 },
        { PGRAPH_VK_ENUMERATE_SUCCESS, 4, 0 },
        { PGRAPH_VK_ENUMERATE_INCOMPLETE, 4, 0 },
    };
    uintptr_t *tokens = NULL;
    size_t count = 0;

    assert(run_steps(steps, 8, &tokens, &count) ==
           PGRAPH_VK_ENUMERATION_UNSTABLE);
    assert(tokens == NULL);
    assert(count == 0);
}

static PGRAPHVkDeviceCapabilities supported_capabilities(void)
{
    return (PGRAPHVkDeviceCapabilities) {
        .api_version = PGRAPH_VK_API_VERSION_1_1,
        .has_graphics_compute_queue = true,
        .has_external_memory = true,
        .has_external_semaphore = true,
        .available_required_features = PGRAPH_VK_REQUIRED_FEATURES,
    };
}

static void test_capabilities_accept_complete_renderer_support(void)
{
    PGRAPHVkDeviceRecord record = {0};
    PGRAPHVkDeviceCapabilities capabilities = supported_capabilities();

    pgraph_vk_device_record_check_renderer_support(&record, &capabilities);
    assert(record.renderer_supported);
    assert(record.rejection_reason == NULL);
}

static void test_capabilities_reject_each_required_boundary(void)
{
    static const struct {
        enum {
            LOW_API,
            NO_QUEUE,
            NO_MEMORY,
            NO_SEMAPHORE,
            MISSING_FEATURE,
        } mutation;
        const char *reason;
    } cases[] = {
        { LOW_API, "Vulkan 1.1 is required" },
        { NO_QUEUE, "combined graphics/compute queue is required" },
        { NO_MEMORY, "external memory is required for shared presentation" },
        { NO_SEMAPHORE,
          "external semaphore is required for shared presentation" },
        { MISSING_FEATURE, "required physical-device feature is unavailable" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        PGRAPHVkDeviceRecord record = {0};
        PGRAPHVkDeviceCapabilities capabilities = supported_capabilities();
        switch (cases[i].mutation) {
        case LOW_API:
            capabilities.api_version = PGRAPH_VK_API_VERSION_1_0;
            break;
        case NO_QUEUE:
            capabilities.has_graphics_compute_queue = false;
            break;
        case NO_MEMORY:
            capabilities.has_external_memory = false;
            break;
        case NO_SEMAPHORE:
            capabilities.has_external_semaphore = false;
            break;
        case MISSING_FEATURE:
            capabilities.available_required_features &=
                ~PGRAPH_VK_FEATURE_DEPTH_CLAMP;
            break;
        }
        pgraph_vk_device_record_check_renderer_support(&record, &capabilities);
        assert(!record.renderer_supported);
        assert(!strcmp(record.rejection_reason, cases[i].reason));
    }
}

int main(void)
{
    test_successful_two_call_enumeration();
    test_zero_devices_is_distinct();
    test_query_failure_is_reported();
    test_fill_failure_is_reported();
    test_incomplete_fill_retries_from_count_query();
    test_count_shrink_is_accepted();
    test_repeated_incomplete_is_bounded();
    test_capabilities_accept_complete_renderer_support();
    test_capabilities_reject_each_required_boundary();
    assert(pgraph_vk_shader_target_for_api(
               PGRAPH_VK_MAKE_API_VERSION(1, 1, 0)) ==
           PGRAPH_VK_SHADER_TARGET_VULKAN_1_1);
    assert(pgraph_vk_shader_target_for_api(
               PGRAPH_VK_MAKE_API_VERSION(1, 2, 0)) ==
           PGRAPH_VK_SHADER_TARGET_VULKAN_1_2);
    assert(pgraph_vk_shader_target_for_api(
               PGRAPH_VK_MAKE_API_VERSION(1, 3, 0)) ==
           PGRAPH_VK_SHADER_TARGET_VULKAN_1_3);
    assert(pgraph_vk_shader_target_for_api(
               PGRAPH_VK_MAKE_API_VERSION(1, 4, 0)) ==
           PGRAPH_VK_SHADER_TARGET_VULKAN_1_3);
    return 0;
}
