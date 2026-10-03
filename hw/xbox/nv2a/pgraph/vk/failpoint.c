/*
 * NV2A Vulkan diagnostic preparation failpoints
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"

#include "failpoint.h"

typedef struct PGRAPHVkFailpointName {
    PGRAPHVkFailpoint point;
    const char *name;
} PGRAPHVkFailpointName;

static const PGRAPHVkFailpointName failpoint_names[] = {
    { PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_MAP, "surface-download-map" },
    { PGRAPH_VK_FAILPOINT_SURFACE_DOWNLOAD_INVALIDATE,
      "surface-download-invalidate" },
    { PGRAPH_VK_FAILPOINT_TEXTURE_STAGING_FLUSH, "texture-staging-flush" },
};

bool pgraph_vk_failpoint_state_configure(PGRAPHVkFailpointState *state,
                                         const char *name,
                                         uint64_t fail_after)
{
    memset(state, 0, sizeof(*state));

    if (!name) {
        return false;
    }

    for (size_t i = 0; i < ARRAY_SIZE(failpoint_names); i++) {
        if (!strcmp(name, failpoint_names[i].name)) {
            state->selected = failpoint_names[i].point;
            state->fail_after = fail_after;
            return true;
        }
    }
    return false;
}

bool pgraph_vk_failpoint_state_should_fail(PGRAPHVkFailpointState *state,
                                           PGRAPHVkFailpoint point)
{
    if (point == PGRAPH_VK_FAILPOINT_NONE || point != state->selected) {
        return false;
    }

    state->visits++;
    if (state->failures == 0 && state->visits > state->fail_after) {
        state->failures++;
        return true;
    }
    return false;
}

#ifdef XEMU_VK_DIAGNOSTIC_FAILURES

static PGRAPHVkFailpointState diagnostic_state;
static bool diagnostic_requested;
static bool diagnostic_valid;

static const char *pgraph_vk_failpoint_name(PGRAPHVkFailpoint point)
{
    for (size_t i = 0; i < ARRAY_SIZE(failpoint_names); i++) {
        if (failpoint_names[i].point == point) {
            return failpoint_names[i].name;
        }
    }
    return "none";
}

void pgraph_vk_failpoint_init(void)
{
    const char *name = getenv("XEMU_VK_DIAGNOSTIC_FAILPOINT");
    const char *after_text = getenv("XEMU_VK_DIAGNOSTIC_FAIL_AFTER");
    uint64_t fail_after = 0;

    memset(&diagnostic_state, 0, sizeof(diagnostic_state));
    diagnostic_requested = name && name[0];
    diagnostic_valid = false;
    if (!diagnostic_requested) {
        return;
    }

    if (after_text && after_text[0]) {
        char *end = NULL;

        errno = 0;
        fail_after = g_ascii_strtoull(after_text, &end, 10);
        if (errno || *end) {
            return;
        }
    }

    diagnostic_valid = pgraph_vk_failpoint_state_configure(
        &diagnostic_state, name, fail_after);
}

bool pgraph_vk_failpoint_should_fail(PGRAPHVkFailpoint point)
{
    bool fail = diagnostic_valid && pgraph_vk_failpoint_state_should_fail(
                                        &diagnostic_state, point);

    if (fail) {
        fprintf(stderr,
                "XEMU_VK_FAILPOINT_RESULT selected=%s phase=injected "
                "fail_after=%" PRIu64 " visits=%" PRIu64
                " failures=%" PRIu64 "\n",
                pgraph_vk_failpoint_name(diagnostic_state.selected),
                diagnostic_state.fail_after, diagnostic_state.visits,
                diagnostic_state.failures);
        fflush(stderr);
    }
    return fail;
}

void pgraph_vk_failpoint_report(void)
{
    if (!diagnostic_requested) {
        return;
    }

    fprintf(stderr,
            "XEMU_VK_FAILPOINT_RESULT selected=%s valid=%s fail_after=%" PRIu64
            " visits=%" PRIu64 " failures=%" PRIu64 "\n",
            diagnostic_valid ?
                pgraph_vk_failpoint_name(diagnostic_state.selected) :
                "invalid",
            diagnostic_valid ? "yes" : "no", diagnostic_state.fail_after,
            diagnostic_state.visits, diagnostic_state.failures);
}

#endif
