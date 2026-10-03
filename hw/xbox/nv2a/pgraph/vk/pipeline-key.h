/*
 * Vulkan graphics pipeline identity helpers.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_VK_PIPELINE_KEY_H
#define HW_XBOX_NV2A_PGRAPH_VK_PIPELINE_KEY_H

#include "hw/xbox/nv2a/pgraph/vk/renderer.h"

static inline void pgraph_vk_pipeline_key_set_shader(
    PipelineKey *key, const ShaderState *state,
    PGRAPHVkFragmentRoute route)
{
    key->fragment_route = route;
    key->shader_state = *state;
    if (route == PGRAPH_VK_FRAGMENT_UBERSHADER) {
        pgraph_vk_canonicalize_uber_combiner_state(
            &key->shader_state.psh);
    }
}

/* These register values are consumed through fragment uniforms and do not
 * change the generated shader or fixed Vulkan pipeline recipe. */
static inline void pgraph_vk_pipeline_key_canonicalize_uniform_regs(
    PipelineKey *key)
{
    key->regs[1] &= ~(uint32_t)NV_PGRAPH_CONTROL_0_ALPHAREF;
    key->regs[6] = 0;
    key->regs[7] = 0;
}

#endif
