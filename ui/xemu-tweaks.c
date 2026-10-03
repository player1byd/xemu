/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "xemu-settings.h"
#include "xemu-tweaks.h"

G_STATIC_ASSERT((int)XEMU_VK_UBERSHADER_OFF ==
                CONFIG_TWEAKS_VK_UBERSHADER_MODE_OFF);
G_STATIC_ASSERT((int)XEMU_VK_UBERSHADER_FALLBACK ==
                CONFIG_TWEAKS_VK_UBERSHADER_MODE_FALLBACK);
G_STATIC_ASSERT((int)XEMU_VK_UBERSHADER_PREWARM ==
                CONFIG_TWEAKS_VK_UBERSHADER_MODE_PREWARM);
G_STATIC_ASSERT((int)XEMU_VK_UBERSHADER_ALWAYS ==
                CONFIG_TWEAKS_VK_UBERSHADER_MODE_ALWAYS);

unsigned int xemu_tweaks_active =
    ((1u << XEMU_TWEAK_COUNT) - 1) &
    ~((1u << XEMU_TWEAK_VK_HYBRID_UBERSHADERS) |
      (1u << XEMU_TWEAK_VK_SHADER_FASTPATH) |
      (1u << XEMU_TWEAK_NV20_VERTEX_ARITHMETIC));
static int xemu_vulkan_ubershader_latched_policy =
    XEMU_VK_UBERSHADER_OFF;

typedef enum XemuVulkanUbershaderRuntimeStatus {
    XEMU_VK_UBERSHADER_RUNTIME_NO_VULKAN,
    XEMU_VK_UBERSHADER_RUNTIME_ACTIVE,
    XEMU_VK_UBERSHADER_RUNTIME_DEGRADED,
} XemuVulkanUbershaderRuntimeStatus;

static int xemu_vulkan_ubershader_runtime_status =
    XEMU_VK_UBERSHADER_RUNTIME_NO_VULKAN;
static int xemu_tweaks_renderer = XEMU_TWEAK_RENDERER_NONE;

void xemu_tweaks_publish_renderer(XemuTweakRenderer renderer)
{
    if (renderer < XEMU_TWEAK_RENDERER_NONE ||
        renderer > XEMU_TWEAK_RENDERER_VULKAN) {
        renderer = XEMU_TWEAK_RENDERER_NONE;
    }
    qatomic_set(&xemu_tweaks_renderer, renderer);
}

static bool xemu_tweak_requested(XemuTweak tweak)
{
    switch (tweak) {
    case XEMU_TWEAK_CPU_SAVING_WAIT:
        return g_config.tweaks.cpu_saving_wait;
    case XEMU_TWEAK_PGRAPH_BULK_PACKETS:
        return g_config.tweaks.pgraph_bulk_packets;
    case XEMU_TWEAK_PGRAPH_FENCE_FASTPATH:
        return g_config.tweaks.pgraph_fence_fastpath;
    case XEMU_TWEAK_VK_COLOR_DOWNLOAD_FOLDING:
        return g_config.tweaks.vk_color_download_folding;
    case XEMU_TWEAK_VK_BOUNDED_VERTEX_UPLOADS:
        return g_config.tweaks.vk_bounded_vertex_uploads;
    case XEMU_TWEAK_VK_VERTEX_COPY_SHORTCUTS:
        return g_config.tweaks.vk_vertex_copy_shortcuts;
    case XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH:
        return g_config.tweaks.vk_transient_buffer_growth;
    case XEMU_TWEAK_GL_NATIVE_S3TC:
        return g_config.tweaks.gl_native_s3tc;
    case XEMU_TWEAK_VK_HYBRID_UBERSHADERS:
        return g_config.tweaks.vk_ubershader_mode !=
               XEMU_VK_UBERSHADER_OFF;
    case XEMU_TWEAK_VK_SHADER_FASTPATH:
        return g_config.tweaks.vk_shader_fastpath;
    case XEMU_TWEAK_NV20_VERTEX_ARITHMETIC:
        return g_config.tweaks.nv20_vertex_arithmetic;
    default:
        return false;
    }
}

XemuTweakRuntimeState xemu_tweak_runtime_state(XemuTweak tweak)
{
    XemuTweakRuntimeState state = { 0 };
    XemuTweakRenderer renderer = qatomic_read(&xemu_tweaks_renderer);

    if ((unsigned int)tweak >= XEMU_TWEAK_COUNT) {
        state.reason = "Unknown Advanced setting.";
        return state;
    }

    state.requested = xemu_tweak_requested(tweak);
    state.selected = xemu_tweak_enabled(tweak);
    state.restart_pending = xemu_tweak_requires_restart(tweak) &&
                            state.requested != state.selected;

    switch (tweak) {
    case XEMU_TWEAK_CPU_SAVING_WAIT:
#ifdef _WIN32
        state.available = true;
#else
        state.reason = "This host wait route is available on Windows.";
#endif
        break;
    case XEMU_TWEAK_PGRAPH_BULK_PACKETS:
    case XEMU_TWEAK_PGRAPH_FENCE_FASTPATH:
        state.available = renderer != XEMU_TWEAK_RENDERER_NONE;
        if (!state.available) {
            state.reason = "Available when a renderer is installed.";
        }
        break;
    case XEMU_TWEAK_GL_NATIVE_S3TC:
        state.available = renderer == XEMU_TWEAK_RENDERER_OPENGL;
        if (!state.available) {
            state.reason = "Available with the OpenGL renderer.";
        }
        break;
    case XEMU_TWEAK_NV20_VERTEX_ARITHMETIC:
        state.available = renderer != XEMU_TWEAK_RENDERER_NONE;
        if (!state.available) {
            state.reason = "Available when a renderer is installed.";
        }
        break;
    case XEMU_TWEAK_VK_SHADER_FASTPATH:
        state.available = renderer == XEMU_TWEAK_RENDERER_VULKAN &&
            xemu_vulkan_ubershader_runtime_state().active !=
                XEMU_VK_UBERSHADER_OFF;
        if (!state.available) {
            state.reason = "Requires an active Vulkan ubershader mode.";
        }
        break;
    case XEMU_TWEAK_VK_HYBRID_UBERSHADERS:
        state.available = renderer == XEMU_TWEAK_RENDERER_VULKAN &&
            xemu_vulkan_ubershader_runtime_state().available;
        if (!state.available) {
            state.reason = xemu_vulkan_ubershader_runtime_state().reason;
        }
        break;
    default:
        state.available = renderer == XEMU_TWEAK_RENDERER_VULKAN;
        if (!state.available) {
            state.reason = "Available with the Vulkan renderer.";
        }
        break;
    }

    state.effective = state.selected && state.available;
    if (!state.reason) {
        state.reason = state.restart_pending ?
            "Restart xemu to apply the saved choice." :
            state.effective ? "Active for eligible work." : "Disabled.";
    }
    return state;
}

XemuVulkanUbershaderMode xemu_vulkan_ubershader_migrate_mode(
    bool mode_present, XemuVulkanUbershaderMode mode,
    bool legacy_enabled)
{
    if (mode_present) {
        return mode;
    }

    return legacy_enabled ? XEMU_VK_UBERSHADER_FALLBACK :
                            XEMU_VK_UBERSHADER_OFF;
}

bool xemu_vulkan_ubershader_mode_selectable(
    XemuVulkanUbershaderMode mode)
{
    return mode == XEMU_VK_UBERSHADER_OFF ||
           mode == XEMU_VK_UBERSHADER_FALLBACK ||
           mode == XEMU_VK_UBERSHADER_PREWARM ||
           mode == XEMU_VK_UBERSHADER_ALWAYS;
}

XemuVulkanUbershaderMode xemu_vulkan_ubershader_policy(void)
{
    return qatomic_read(&xemu_vulkan_ubershader_latched_policy);
}

XemuVulkanUbershaderRuntimeState
xemu_vulkan_ubershader_runtime_state(void)
{
    XemuVulkanUbershaderMode requested =
        (XemuVulkanUbershaderMode)g_config.tweaks.vk_ubershader_mode;
    XemuVulkanUbershaderMode policy =
        qatomic_read(&xemu_vulkan_ubershader_latched_policy);
    XemuVulkanUbershaderRuntimeStatus runtime_status =
        qatomic_read(&xemu_vulkan_ubershader_runtime_status);
    XemuVulkanUbershaderMode active =
        runtime_status == XEMU_VK_UBERSHADER_RUNTIME_ACTIVE ? policy :
        XEMU_VK_UBERSHADER_OFF;
    XemuVulkanUbershaderRuntimeState state = {
        .requested = requested,
        .policy = policy,
        .active = active,
        .available = runtime_status == XEMU_VK_UBERSHADER_RUNTIME_ACTIVE &&
                     xemu_vulkan_ubershader_mode_selectable(requested),
        .restart_pending =
            xemu_vulkan_ubershader_mode_selectable(requested) &&
            requested != policy,
    };

    if (!xemu_vulkan_ubershader_mode_selectable(requested)) {
        state.reason = "This mode is planned and is not active yet.";
    } else if (runtime_status == XEMU_VK_UBERSHADER_RUNTIME_NO_VULKAN) {
        state.reason = "Available when the Vulkan renderer is installed.";
    } else if (runtime_status == XEMU_VK_UBERSHADER_RUNTIME_DEGRADED) {
        state.reason = "Ubershader setup failed; Vulkan is using "
                       "specialized shaders.";
    } else {
        state.reason = "Active.";
    }

    return state;
}

void xemu_vulkan_ubershader_publish_runtime(
    bool vulkan_installed, bool ubershader_operational)
{
    XemuVulkanUbershaderMode policy =
        qatomic_read(&xemu_vulkan_ubershader_latched_policy);
    XemuVulkanUbershaderRuntimeStatus status;

    if (!vulkan_installed) {
        status = XEMU_VK_UBERSHADER_RUNTIME_NO_VULKAN;
    } else if (policy != XEMU_VK_UBERSHADER_OFF && !ubershader_operational) {
        status = XEMU_VK_UBERSHADER_RUNTIME_DEGRADED;
    } else {
        status = XEMU_VK_UBERSHADER_RUNTIME_ACTIVE;
    }
    qatomic_set(&xemu_vulkan_ubershader_runtime_status, status);
}

void xemu_tweaks_apply(bool startup)
{
    XemuVulkanUbershaderMode requested_mode =
        (XemuVulkanUbershaderMode)g_config.tweaks.vk_ubershader_mode;
    if (startup) {
        XemuVulkanUbershaderMode policy =
            xemu_vulkan_ubershader_mode_selectable(requested_mode) ?
                requested_mode :
                XEMU_VK_UBERSHADER_OFF;
        qatomic_set(&xemu_vulkan_ubershader_latched_policy, policy);
        qatomic_set(&xemu_vulkan_ubershader_runtime_status,
                    XEMU_VK_UBERSHADER_RUNTIME_NO_VULKAN);
    }
    bool selected[XEMU_TWEAK_COUNT] = {
        [XEMU_TWEAK_CPU_SAVING_WAIT] = g_config.tweaks.cpu_saving_wait,
        [XEMU_TWEAK_PGRAPH_BULK_PACKETS] = g_config.tweaks.pgraph_bulk_packets,
        [XEMU_TWEAK_PGRAPH_FENCE_FASTPATH] =
            g_config.tweaks.pgraph_fence_fastpath,
        [XEMU_TWEAK_VK_COLOR_DOWNLOAD_FOLDING] =
            g_config.tweaks.vk_color_download_folding,
        [XEMU_TWEAK_VK_BOUNDED_VERTEX_UPLOADS] =
            g_config.tweaks.vk_bounded_vertex_uploads,
        [XEMU_TWEAK_VK_VERTEX_COPY_SHORTCUTS] =
            g_config.tweaks.vk_vertex_copy_shortcuts,
        [XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH] =
            g_config.tweaks.vk_transient_buffer_growth,
        [XEMU_TWEAK_GL_NATIVE_S3TC] = g_config.tweaks.gl_native_s3tc,
        [XEMU_TWEAK_VK_HYBRID_UBERSHADERS] =
            qatomic_read(&xemu_vulkan_ubershader_latched_policy) !=
            XEMU_VK_UBERSHADER_OFF,
        [XEMU_TWEAK_VK_SHADER_FASTPATH] =
            g_config.tweaks.vk_shader_fastpath,
        [XEMU_TWEAK_NV20_VERTEX_ARITHMETIC] =
            g_config.tweaks.nv20_vertex_arithmetic,
    };
    unsigned int active = qatomic_read(&xemu_tweaks_active);

    for (unsigned int i = 0; i < XEMU_TWEAK_COUNT; i++) {
        if (!startup && xemu_tweak_requires_restart(i)) {
            continue;
        }
        if (selected[i]) {
            active |= 1u << i;
        } else {
            active &= ~(1u << i);
        }
    }
    qatomic_set(&xemu_tweaks_active, active);
    qemu_poll_set_cpu_saving(selected[XEMU_TWEAK_CPU_SAVING_WAIT]);
}
