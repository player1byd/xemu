/*
 * NV2A Vulkan diagnostic preparation failpoints
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_FAILPOINT_H
#define HW_XBOX_NV2A_PGRAPH_VK_FAILPOINT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum PGRAPHVkFailpoint {
    PGRAPH_VK_FAILPOINT_NONE,
    PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_MAP,
    PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_INVALIDATE,
    PGRAPH_VK_FAILPOINT_TEXTURE_STAGING_FLUSH,
    PGRAPH_VK_FAILPOINT_COUNT,
} PGRAPHVkFailpoint;

typedef struct PGRAPHVkFailpointState {
    PGRAPHVkFailpoint selected;
    uint64_t fail_after;
    uint64_t visits;
    uint64_t failures;
} PGRAPHVkFailpointState;

bool pgraph_vk_failpoint_state_configure(PGRAPHVkFailpointState *state,
                                         const char *name,
                                         uint64_t fail_after);
bool pgraph_vk_failpoint_state_should_fail(PGRAPHVkFailpointState *state,
                                           PGRAPHVkFailpoint point);

#ifdef XEMU_VK_DIAGNOSTIC_FAILURES
void pgraph_vk_failpoint_init(void);
bool pgraph_vk_failpoint_should_fail(PGRAPHVkFailpoint point);
void pgraph_vk_failpoint_report(void);
#else
#define pgraph_vk_failpoint_init() ((void)0)
#define pgraph_vk_failpoint_should_fail(point) (false)
#define pgraph_vk_failpoint_report() ((void)0)
#endif

#endif
