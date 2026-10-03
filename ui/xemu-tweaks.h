/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XEMU_TWEAKS_H
#define XEMU_TWEAKS_H

#include <stdbool.h>
#include "qemu/atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XemuTweak {
    XEMU_TWEAK_CPU_SAVING_WAIT,
    XEMU_TWEAK_PGRAPH_BULK_PACKETS,
    XEMU_TWEAK_PGRAPH_FENCE_FASTPATH,
    XEMU_TWEAK_VK_COLOR_DOWNLOAD_FOLDING,
    XEMU_TWEAK_VK_BOUNDED_VERTEX_UPLOADS,
    XEMU_TWEAK_VK_VERTEX_COPY_SHORTCUTS,
    XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH,
    XEMU_TWEAK_GL_NATIVE_S3TC,
    XEMU_TWEAK_VK_HYBRID_UBERSHADERS,
    XEMU_TWEAK_VK_SHADER_FASTPATH,
    XEMU_TWEAK_NV20_VERTEX_ARITHMETIC,
    XEMU_TWEAK_COUNT,
} XemuTweak;

#ifdef __cplusplus
static_assert(XEMU_TWEAK_COUNT < sizeof(unsigned int) * 8,
              "Advanced tweak mask is full");
#else
_Static_assert(XEMU_TWEAK_COUNT < sizeof(unsigned int) * 8,
               "Advanced tweak mask is full");
#endif

typedef enum XemuTweakRenderer {
    XEMU_TWEAK_RENDERER_NONE,
    XEMU_TWEAK_RENDERER_OPENGL,
    XEMU_TWEAK_RENDERER_VULKAN,
} XemuTweakRenderer;

typedef struct XemuTweakRuntimeState {
    bool requested;
    bool selected;
    bool effective;
    bool available;
    bool restart_pending;
    const char *reason;
} XemuTweakRuntimeState;

typedef enum XemuVulkanUbershaderMode {
    XEMU_VK_UBERSHADER_OFF = 0,
    XEMU_VK_UBERSHADER_FALLBACK,
    XEMU_VK_UBERSHADER_PREWARM,
    XEMU_VK_UBERSHADER_ALWAYS,
} XemuVulkanUbershaderMode;

typedef struct XemuVulkanUbershaderRuntimeState {
    XemuVulkanUbershaderMode requested;
    XemuVulkanUbershaderMode policy;
    XemuVulkanUbershaderMode active;
    bool available;
    bool restart_pending;
    const char *reason;
} XemuVulkanUbershaderRuntimeState;

/* Workers read this snapshot, never the UI-owned mutable g_config. */
extern unsigned int xemu_tweaks_active;

static inline bool xemu_tweak_enabled(XemuTweak tweak)
{
    return (qatomic_read(&xemu_tweaks_active) & (1u << tweak)) != 0;
}

static inline bool xemu_tweak_requires_restart(XemuTweak tweak)
{
    return tweak == XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH ||
           tweak == XEMU_TWEAK_GL_NATIVE_S3TC ||
           tweak == XEMU_TWEAK_VK_HYBRID_UBERSHADERS;
}

/* UI thread only. startup=true is only valid before workers are created. */
void xemu_tweaks_apply(bool startup);
/* Renderer lifecycle publication and UI-only status query. Neither is hot-path. */
void xemu_tweaks_publish_renderer(XemuTweakRenderer renderer);
XemuTweakRuntimeState xemu_tweak_runtime_state(XemuTweak tweak);
XemuVulkanUbershaderMode xemu_vulkan_ubershader_migrate_mode(
    bool mode_present, XemuVulkanUbershaderMode mode,
    bool legacy_enabled);
bool xemu_vulkan_ubershader_mode_selectable(
    XemuVulkanUbershaderMode mode);
XemuVulkanUbershaderMode xemu_vulkan_ubershader_policy(void);
XemuVulkanUbershaderRuntimeState
xemu_vulkan_ubershader_runtime_state(void);
/* Renderer lifecycle publication; never called from the draw path. */
void xemu_vulkan_ubershader_publish_runtime(
    bool vulkan_installed, bool ubershader_operational);

#ifdef __cplusplus
}
#endif
#endif
