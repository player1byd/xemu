/*
 * NV2A Vulkan texture-binding transition tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hw/xbox/nv2a/pgraph/vk/texture-binding-state.h"

static bool test_disabled_stage_keeps_dirty_for_reenable(void)
{
    bool enabled = false;
    bool dirty = true;
    bool bound = false;
    bool dummy = false;

    if (!pgraph_vk_texture_stage_needs_rebind(enabled, dirty, bound, dummy)) {
        return false;
    }

    /* The first disabled bind selects the dummy without retiring guest state. */
    bound = true;
    dummy = true;
    if (pgraph_vk_texture_stage_needs_rebind(enabled, dirty, bound, dummy) ||
        !dirty) {
        return false;
    }

    enabled = true;
    if (!pgraph_vk_texture_stage_needs_rebind(enabled, dirty, bound, dummy)) {
        return false;
    }
    dummy = false;
    dirty = false;
    if (pgraph_vk_texture_stage_needs_rebind(enabled, dirty, bound, dummy)) {
        return false;
    }

    enabled = false;
    if (!pgraph_vk_texture_stage_needs_rebind(enabled, dirty, bound, dummy)) {
        return false;
    }
    dummy = true;
    return !pgraph_vk_texture_stage_needs_rebind(enabled, dirty, bound, dummy);
}

static bool test_failed_active_bind_remains_retryable(void)
{
    /* A failed bind selects the dummy and retains dirtiness. */
    return pgraph_vk_texture_stage_needs_rebind(true, true, true, true) &&
           pgraph_vk_texture_stage_needs_rebind(true, false, true, true);
}

static bool test_texture_source_identity(void)
{
    const uint64_t texture = 0x1000;
    const uint64_t palette = 0x2000;

    return pgraph_vk_texture_source_identity_matches(
               false, true, false, false,
               texture + 1, texture, 32, palette + 1, palette) &&
           pgraph_vk_texture_source_identity_matches(
               true, false, false, false,
               texture + 1, texture, 32, palette + 1, palette) &&
           pgraph_vk_texture_source_identity_matches(
               true, true, true, false,
               texture + 1, texture, 32, palette + 1, palette) &&
           pgraph_vk_texture_source_identity_matches(
               true, true, false, true,
               texture + 1, texture, 32, palette + 1, palette) &&
           pgraph_vk_texture_source_identity_matches(
               true, true, false, false,
               texture, texture, 0, palette + 1, palette) &&
           pgraph_vk_texture_source_identity_matches(
               true, true, false, false,
               texture, texture, 32, palette, palette) &&
           !pgraph_vk_texture_source_identity_matches(
               true, true, false, false,
               texture + 1, texture, 0, palette, palette) &&
           !pgraph_vk_texture_source_identity_matches(
               true, true, false, false,
               texture, texture, 32, palette + 1, palette);
}

static bool test_descriptor_identity_changes_only_for_visible_binding(void)
{
    const PGRAPHVkTextureDescriptorIdentity dummy = {
        .image_view = 1,
        .sampler = 2,
    };
    const PGRAPHVkTextureDescriptorIdentity real_a = {
        .image_view = 3,
        .sampler = 4,
    };
    const PGRAPHVkTextureDescriptorIdentity real_a_content_update = {
        .image_view = 3,
        .sampler = 4,
    };
    const PGRAPHVkTextureDescriptorIdentity view_only = {
        .image_view = 5,
        .sampler = 4,
    };
    const PGRAPHVkTextureDescriptorIdentity sampler_only = {
        .image_view = 3,
        .sampler = 6,
    };
    const PGRAPHVkTextureDescriptorIdentity real_b = {
        .image_view = 5,
        .sampler = 6,
    };

    return !pgraph_vk_texture_descriptor_identity_changed(dummy, dummy) &&
           pgraph_vk_texture_descriptor_identity_changed(real_a, dummy) &&
           pgraph_vk_texture_descriptor_identity_changed(dummy, real_a) &&
           !pgraph_vk_texture_descriptor_identity_changed(
               real_a, real_a_content_update) &&
           pgraph_vk_texture_descriptor_identity_changed(real_a, view_only) &&
           pgraph_vk_texture_descriptor_identity_changed(real_a,
                                                          sampler_only) &&
           pgraph_vk_texture_descriptor_identity_changed(real_a, real_b) &&
           pgraph_vk_texture_descriptor_identity_changed(real_b, dummy);
}

static bool test_failed_multistage_bind_detects_recovery_changes(void)
{
    const PGRAPHVkTextureDescriptorIdentity real_a = {
        .image_view = 3,
        .sampler = 4,
    };
    const PGRAPHVkTextureDescriptorIdentity real_b = {
        .image_view = 5,
        .sampler = 6,
    };
    const PGRAPHVkTextureDescriptorIdentity dummy = {
        .image_view = 1,
        .sampler = 2,
    };

    /* An earlier stage changes before a later stage fails to the dummy. The
     * draw is skipped, so the per-call change is not published. On retry the
     * recovered stage still changes from dummy to real and requires a fresh
     * descriptor publication. */
    bool failed_attempt_changed =
        pgraph_vk_texture_descriptor_identity_changed(real_a, real_b) ||
        pgraph_vk_texture_descriptor_identity_changed(real_a, dummy);
    bool recovery_attempt_changed =
        !pgraph_vk_texture_descriptor_identity_changed(real_b, real_b) &&
        pgraph_vk_texture_descriptor_identity_changed(dummy, real_a);

    return failed_attempt_changed && recovery_attempt_changed;
}

static bool test_failed_bind_retains_unpublished_descriptor_change(void)
{
    const PGRAPHVkTextureDescriptorIdentity dummy = {
        .image_view = 1,
        .sampler = 2,
    };
    const PGRAPHVkTextureDescriptorIdentity real_a = {
        .image_view = 3,
        .sampler = 4,
    };
    const PGRAPHVkTextureDescriptorIdentity real_b = {
        .image_view = 5,
        .sampler = 6,
    };
    bool publication_pending = false;

    /* Stage 0 changes before stage 1 fails while already bound to the dummy.
     * The failed draw cannot publish either stage. */
    pgraph_vk_texture_descriptor_publication_observe(
        &publication_pending, real_a, real_b);
    pgraph_vk_texture_descriptor_publication_observe(
        &publication_pending, dummy, dummy);
    if (!publication_pending) {
        return false;
    }

    /* The following draw disables stage 1 and changes no handles. The earlier
     * unpublished stage-0 change must still force a descriptor write. */
    pgraph_vk_texture_descriptor_publication_observe(
        &publication_pending, real_b, real_b);
    pgraph_vk_texture_descriptor_publication_observe(
        &publication_pending, dummy, dummy);
    if (!publication_pending) {
        return false;
    }

    pgraph_vk_texture_descriptor_publication_complete(
        &publication_pending);
    return !publication_pending;
}

int main(void)
{
    bool disabled = test_disabled_stage_keeps_dirty_for_reenable();
    bool retry = test_failed_active_bind_remains_retryable();
    bool identity = test_texture_source_identity();
    bool descriptor_identity =
        test_descriptor_identity_changes_only_for_visible_binding();
    bool recovery_changes =
        test_failed_multistage_bind_detects_recovery_changes();
    bool retained_publication =
        test_failed_bind_retains_unpublished_descriptor_change();

    puts("TAP version 13");
    puts("1..6");
    printf("%s 1 - disabled dirty state waits for re-enable\n",
           disabled ? "ok" : "not ok");
    printf("%s 2 - failed active bind remains retryable\n",
           retry ? "ok" : "not ok");
    printf("%s 3 - texture source identity gates clean reuse\n",
           identity ? "ok" : "not ok");
    printf("%s 4 - descriptor identity changes only for visible binding\n",
           descriptor_identity ? "ok" : "not ok");
    printf("%s 5 - failed multistage bind detects recovery changes\n",
           recovery_changes ? "ok" : "not ok");
    printf("%s 6 - failed bind retains unpublished descriptor change\n",
           retained_publication ? "ok" : "not ok");
    return (disabled && retry && identity && descriptor_identity &&
            recovery_changes && retained_publication) ? 0 : 1;
}
