/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_DEBUG_H
#define HW_XBOX_NV2A_PGRAPH_VK_DEBUG_H

#include <stdbool.h>

extern int nv2a_vk_dgroup_indent;
extern bool nv2a_vk_text_debug_enabled;

#ifdef __GNUC__
__attribute__((format(gnu_printf, 1, 2)))
#endif
void pgraph_vk_text_debug_printf(const char *format, ...);

#define NV2A_VK_DPRINTF(fmt, ...)                                       \
    do {                                                                \
        if (unlikely(nv2a_vk_text_debug_enabled)) {                     \
            pgraph_vk_text_debug_printf(                                \
                "%*s" fmt "\n", nv2a_vk_dgroup_indent, "",           \
                ##__VA_ARGS__);                                         \
        }                                                               \
    } while (0)

#define NV2A_VK_DGROUP_BEGIN(fmt, ...)                                  \
    do {                                                                \
        if (unlikely(nv2a_vk_text_debug_enabled)) {                     \
            pgraph_vk_text_debug_printf(                                \
                "%*s" fmt "\n", nv2a_vk_dgroup_indent, "",           \
                ##__VA_ARGS__);                                         \
            nv2a_vk_dgroup_indent++;                                    \
        }                                                               \
    } while (0)

#define NV2A_VK_DGROUP_END(...)                         \
    do {                                                \
        if (unlikely(nv2a_vk_text_debug_enabled)) {     \
            nv2a_vk_dgroup_indent--;                    \
            assert(nv2a_vk_dgroup_indent >= 0);         \
        }                                               \
    } while (0)

#define VK_CHECK(x)                                           \
    do {                                                      \
        VkResult vk_result = (x);                             \
        if (vk_result != VK_SUCCESS) {                        \
            fprintf(stderr, "vk_result = %d\n", vk_result);   \
        }                                                     \
        assert(vk_result == VK_SUCCESS && "vk check failed"); \
    } while (0)

void pgraph_vk_debug_frame_terminator(void);

#endif
