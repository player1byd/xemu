#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run production packet dispatch and buffer reservation with small API doubles.

No GPU is required. This tests Off/On control flow, not GPU rendering or timing.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


pgraph = (repo / "hw/xbox/nv2a/pgraph/pgraph.c").read_text()
draw = (repo / "hw/xbox/nv2a/pgraph/vk/draw.c").read_text()
widgets = (repo / "ui/xui/widgets.cc").read_text()
main_menu = (repo / "ui/xui/main-menu.cc").read_text()
settings = (repo / "ui/xemu-settings.cc").read_text()
migration = (repo / "ui/xemu-settings-migration.cc").read_text()
vsh = (repo / "hw/xbox/nv2a/pgraph/glsl/vsh.c").read_text()
vsh_prog = (repo / "hw/xbox/nv2a/pgraph/glsl/vsh-prog.c").read_text()
shaders = (repo / "hw/xbox/nv2a/pgraph/glsl/shaders.c").read_text()

assert "CalcTextSize(description, nullptr, false, wrap_width)" in widgets
assert "wrap_width" in widgets[widgets.index("void WidgetTitleDescription("):
                               widgets.index("void WidgetTitleDescriptionItem(")]
assert 'ChevronCombo("Vulkan ubershader mode"' in main_menu
assert "ChevronComboItemEnabled" in widgets
assert "ImGui::BeginDisabled(!enabled)" in widgets
assert "xemu_vulkan_ubershader_mode_selectable" in main_menu
assert '"Selected: %s   Active: %s"' in main_menu
assert "ubershader_state.active ==" in main_menu
assert "xemu_settings_apply_ubershader_migration" in settings
assert 'contains("vk_ubershader_mode")' in migration
assert "xemu_vulkan_ubershader_migrate_mode" in migration
assert "g_config.tweaks.vk_hybrid_ubershaders = false" in settings
assert 'PerformanceToggle("Accurate NV20 vertex arithmetic"' in main_menu
assert "XEMU_TWEAK_NV20_VERTEX_ARITHMETIC" in main_menu
assert "nv20_vertex_arithmetic" in vsh
assert "state->vsh.nv20_vertex_arithmetic" in shaders
assert "nv20_mul_bits" in vsh_prog
assert "nv20_sum_bits" in vsh_prog
assert "nv20_rcp_bits" in vsh_prog
macros = pgraph[pgraph.index("#define METHOD_HANDLER_ARG_DECL"):
                pgraph.index("#define DEF_METHOD_PROTO")]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define qemu_build_assert(test) _Static_assert(test, "atomic operand size")
#include "ui/xemu-tweaks.h"
#include "hw/xbox/nv2a/pgraph/inline-elements.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#define g_assert_not_reached() assert(false)
typedef struct StorageBuffer {
    uint64_t buffer, buffer_size, buffer_offset;
} StorageBuffer;
typedef struct PGRAPHVkState {
    StorageBuffer storage_buffers[1];
    bool in_command_buffer, in_aux_command_buffer;
} PGRAPHVkState;
typedef struct PGRAPHState {
    unsigned int inline_elements_length, inline_array_length, draw_arrays_length;
    uint32_t inline_elements[16], inline_array[16];
    uint32_t draw_arrays_count[16];
    PGRAPHVkState *vk_renderer_state;
} PGRAPHState;
typedef struct NV2AState { int unused; } NV2AState;
unsigned int xemu_tweaks_active;
static bool tracing;
static unsigned scalar_calls;
static unsigned drop_calls;
static unsigned log_calls;
static uint32_t scalar_values[16];
static bool pgraph_method_trace_enabled(void) { return tracing; }
static void pgraph_method_log(unsigned s, unsigned c, unsigned m, uint32_t p) {
    log_calls++;
}
static void pgraph_check_within_begin_end_block(PGRAPHState *pg) {}
static void pgraph_expand_draw_arrays(NV2AState *d) { assert(false); }
static void pgraph_drop_oversized_array_packet(unsigned int method,
                                                size_t packet_words) {
    drop_calls++;
}
static uint32_t ldl_le_p(const uint32_t *p) {
    const uint8_t *b = (const uint8_t *)p;
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}
typedef uint64_t VkDeviceSize;
typedef uint64_t VkDeviceAddress;
#define VK_NULL_HANDLE 0
#define VK_FINISH_REASON_NEED_BUFFER_SPACE 0
#define pgraph_vk_hybrid_trace_record(...) ((void)0)
static unsigned finishes;
static uint64_t reservation;
static uint64_t pgraph_vk_buffer_required_size(PGRAPHState *pg, int i,
                                               uint64_t size, uint64_t align) {
    return (pg->vk_renderer_state->storage_buffers[i].buffer_offset + align - 1)
           / align * align + size;
}
static bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int i,
                                          uint64_t size, uint64_t align) {
    return pgraph_vk_buffer_required_size(pg, i, size, align) <=
           pg->vk_renderer_state->storage_buffers[i].buffer_size;
}
static void pgraph_vk_finish(PGRAPHState *pg, int reason) {
    finishes++;
    pg->vk_renderer_state->storage_buffers[0].buffer_offset = 0;
    pg->vk_renderer_state->in_command_buffer = false;
}
static void pgraph_vk_ensure_buffer_pair_capacity(PGRAPHState *pg, int i,
                                                 uint64_t required) {
    assert(!pg->vk_renderer_state->in_command_buffer);
    reservation = required;
}
'''
bulk_signature = "static bool pgraph_method_array_bulk(NV2AState *d, PGRAPHState *pg,"
# Skip the forward declaration when locating the definition.
bulk = pgraph[pgraph.index(bulk_signature, pgraph.index(bulk_signature) + 1):]
code = preamble + macros + r'''
typedef void (*MethodFunc)(METHOD_HANDLER_ARG_DECL);
static void scalar(METHOD_HANDLER_ARG_DECL) {
    scalar_values[scalar_calls++] = parameter;
    *num_words_consumed = 1;
}
''' + function(bulk, bulk_signature)
code += function(pgraph, "static bool pgraph_method_array_packet_fits(")
code += function(pgraph, "static void pgraph_method_non_inc(")
code += function(draw, "static bool ensure_buffer_space(")
code += r'''
int main(void) {
    NV2AState d = {0};
    uint8_t bytes[] = {0x34,0x12,0x78,0x56, 0xbc,0x9a,0xf0,0xde};
    uint32_t words[2];
    memcpy(words, bytes, sizeof(words));
    const unsigned methods[] = {
        NV097_ARRAY_ELEMENT16, NV097_ARRAY_ELEMENT32, NV097_INLINE_ARRAY
    };
    for (unsigned m = 0; m < 3; m++) {
        for (unsigned on = 0; on < 2; on++) {
            for (unsigned trace = 0; trace < 2; trace++) {
                for (unsigned inc = 0; inc < 2; inc++) {
                    PGRAPHState pg = {0};
                    size_t consumed = 0;
                    scalar_calls = 0;
                    tracing = trace;
                    xemu_tweaks_active = on << XEMU_TWEAK_PGRAPH_BULK_PACKETS;
                    pgraph_method_non_inc(scalar, &d, &pg, 0, methods[m],
                        0x56781234, words, 2, &consumed, inc);
                    assert(consumed == (inc ? 1 : 2));
                    if (on && !trace && !inc) {
                        assert(scalar_calls == 0);
                        if (m == 0) {
                            const uint32_t expected[] = {0x1234,0x5678,0x9abc,0xdef0};
                            assert(pg.inline_elements_length == 4);
                            assert(!memcmp(pg.inline_elements, expected, sizeof(expected)));
                        } else {
                            uint32_t *out = m == 1 ? pg.inline_elements : pg.inline_array;
                            assert(out[0] == 0x56781234 && out[1] == 0xdef09abc);
                        }
                    } else {
                        assert(scalar_calls == (inc ? 1 : 2));
                        assert(scalar_values[0] == 0x56781234);
                        if (!inc) { assert(scalar_values[1] == 0xdef09abc); }
                    }
                }
            }
        }
    }
    for (unsigned m = 0; m < 3; m++) {
        const size_t values_per_word = m == 0 ? 2 : 1;
        PGRAPHState exact = {0};
        PGRAPHState over = {0};
        unsigned int *exact_length = m == 2 ?
            &exact.inline_array_length : &exact.inline_elements_length;
        unsigned int *over_length = m == 2 ?
            &over.inline_array_length : &over.inline_elements_length;
        if (m == 2) {
            *exact_length = NV2A_MAX_BATCH_LENGTH - 2 * values_per_word;
            *over_length = *exact_length + 1;
        } else {
            *exact_length = 7;
            *over_length = 7;
            exact.draw_arrays_length = 1;
            over.draw_arrays_length = 1;
            exact.draw_arrays_count[0] =
                NV2A_MAX_BATCH_LENGTH - *exact_length -
                2 * values_per_word;
            over.draw_arrays_count[0] = exact.draw_arrays_count[0] + 1;
        }
        assert(pgraph_method_array_packet_fits(&exact, methods[m], 2));
        assert(!pgraph_method_array_packet_fits(&over, methods[m], 2));

        for (unsigned on = 0; on < 2; on++) {
            for (unsigned trace = 0; trace < 2; trace++) {
                PGRAPHState pg = over;
                size_t consumed = 0;
                unsigned inline_before = pg.inline_elements_length;
                unsigned array_before = pg.inline_array_length;
                unsigned pending_before = pg.draw_arrays_length;
                uint32_t pending_count_before = pg.draw_arrays_count[0];
                scalar_calls = 0;
                drop_calls = 0;
                log_calls = 0;
                tracing = trace;
                xemu_tweaks_active =
                    on << XEMU_TWEAK_PGRAPH_BULK_PACKETS;
                pgraph_method_non_inc(scalar, &d, &pg, 0, methods[m],
                    0x56781234, words, 2, &consumed, false);
                assert(consumed == 2);
                assert(drop_calls == 1);
                assert(scalar_calls == 0);
                assert(pg.inline_elements_length == inline_before);
                assert(pg.inline_array_length == array_before);
                assert(pg.draw_arrays_length == pending_before);
                assert(pg.draw_arrays_count[0] == pending_count_before);
                assert(log_calls == trace);
            }
        }
    }
    for (unsigned on = 0; on < 2; on++) {
        PGRAPHVkState r = { .storage_buffers = {{1, 128, 120}},
                            .in_command_buffer = true };
        PGRAPHState pg = { .vk_renderer_state = &r };
        xemu_tweaks_active = on << XEMU_TWEAK_VK_TRANSIENT_BUFFER_GROWTH;
        finishes = 0;
        assert(ensure_buffer_space(&pg, 0, 32, 16));
        assert(finishes == 1);
        assert(reservation == (on ? 160 : 32));
        // Off still reserves enough for a draw larger than the whole buffer.
        r.storage_buffers[0].buffer_offset = 0;
        assert(ensure_buffer_space(&pg, 0, 256, 16));
        assert(reservation == 256);
        r.storage_buffers[0].buffer_offset = 0;
        assert(!ensure_buffer_space(&pg, 0, 16, 16));
    }
    puts("PASS: 24 valid packet modes, 12 atomic oversized rejections, and On/Off buffer reservation fallbacks");
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-tweak-paths-") as tmp:
    source = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    source.write_text(code)
    subprocess.run([args.cc, "-std=gnu11", "-fsanitize=address,undefined",
                    "-I", str(repo), "-I", str(repo / "include"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
