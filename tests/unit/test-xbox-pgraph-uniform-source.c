/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>

#include "hw/xbox/nv2a/pgraph/polygon-offset.h"
#include "hw/xbox/nv2a/pgraph/uniform-source.h"
#include "hw/xbox/nv2a/pgraph/uniform-stage-update.h"

static void test_stage_epochs_are_independent(void)
{
    PGRAPHUniformSourceEpochs epochs = { 0 };
    PGRAPHUniformSourceEpochs previous = epochs;

    pgraph_uniform_source_touch(&epochs, PGRAPH_UNIFORM_STAGE_MASK_PSH);

    g_assert_false(pgraph_uniform_source_stage_changed(
        &epochs, &previous, PGRAPH_UNIFORM_STAGE_VSH));
    g_assert_true(pgraph_uniform_source_stage_changed(
        &epochs, &previous, PGRAPH_UNIFORM_STAGE_PSH));
    g_assert_cmpuint(epochs.total, ==, 1);
    g_assert_cmpuint(epochs.unclassified, ==, 0);
}

static void test_both_and_unclassified_are_conservative(void)
{
    PGRAPHUniformSourceEpochs epochs = { 0 };

    pgraph_uniform_source_touch(&epochs, PGRAPH_UNIFORM_STAGE_MASK_BOTH);
    g_assert_cmpuint(epochs.stage[PGRAPH_UNIFORM_STAGE_VSH], ==, 1);
    g_assert_cmpuint(epochs.stage[PGRAPH_UNIFORM_STAGE_PSH], ==, 1);
    g_assert_cmpuint(epochs.unclassified, ==, 0);

    pgraph_uniform_source_touch_unclassified(&epochs);
    g_assert_cmpuint(epochs.stage[PGRAPH_UNIFORM_STAGE_VSH], ==, 2);
    g_assert_cmpuint(epochs.stage[PGRAPH_UNIFORM_STAGE_PSH], ==, 2);
    g_assert_cmpuint(epochs.unclassified, ==, 1);
}

static uint32_t uniform_float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void test_float_setter_preserves_signed_zero(void)
{
    PGRAPHUniformSourceEpochs epochs = { 0 };
    float value = 0.0f;

    g_assert_true(pgraph_uniform_float_bits_update(
        &value, 0x80000000, &epochs, PGRAPH_UNIFORM_STAGE_MASK_VSH));
    g_assert_cmphex(uniform_float_bits(value), ==, 0x80000000);
    g_assert_cmpuint(epochs.total, ==, 1);
    g_assert_cmpuint(epochs.stage[PGRAPH_UNIFORM_STAGE_VSH], ==, 1);
    g_assert_cmpuint(epochs.stage[PGRAPH_UNIFORM_STAGE_PSH], ==, 0);

    g_assert_false(pgraph_uniform_float_bits_update(
        &value, 0x80000000, &epochs, PGRAPH_UNIFORM_STAGE_MASK_VSH));
    g_assert_cmpuint(epochs.total, ==, 1);

    g_assert_true(pgraph_uniform_float_bits_update(
        &value, 0x00000000, &epochs, PGRAPH_UNIFORM_STAGE_MASK_VSH));
    g_assert_cmphex(uniform_float_bits(value), ==, 0x00000000);
    g_assert_cmpuint(epochs.total, ==, 2);
}

static void test_float_setter_ignores_same_nan_bits(void)
{
    PGRAPHUniformSourceEpochs epochs = { 0 };
    float value = 0.0f;

    g_assert_true(pgraph_uniform_float_bits_update(
        &value, 0x7FC01234, &epochs, PGRAPH_UNIFORM_STAGE_MASK_VSH));
    g_assert_cmphex(uniform_float_bits(value), ==, 0x7FC01234);
    g_assert_false(pgraph_uniform_float_bits_update(
        &value, 0x7FC01234, &epochs, PGRAPH_UNIFORM_STAGE_MASK_VSH));
    g_assert_cmpuint(epochs.total, ==, 1);

    g_assert_true(pgraph_uniform_float_bits_update(
        &value, 0x7FC05678, &epochs, PGRAPH_UNIFORM_STAGE_MASK_VSH));
    g_assert_cmphex(uniform_float_bits(value), ==, 0x7FC05678);
    g_assert_cmpuint(epochs.total, ==, 2);
    g_assert_cmpuint(epochs.stage[PGRAPH_UNIFORM_STAGE_VSH], ==, 2);
    g_assert_cmpuint(epochs.stage[PGRAPH_UNIFORM_STAGE_PSH], ==, 0);
}

static void assert_register_range(unsigned int first, unsigned int count,
                                  PGRAPHUniformStageMask expected)
{
    for (unsigned int i = 0; i < count; i++) {
        g_assert_cmpuint(
            pgraph_reg_uniform_stage_mask(first + i * 4, UINT32_MAX), ==,
            expected);
    }
}

static void test_register_stage_classification(void)
{
    assert_register_range(NV_PGRAPH_COMBINEFACTOR0, 8,
                          PGRAPH_UNIFORM_STAGE_MASK_PSH);
    assert_register_range(NV_PGRAPH_COMBINEFACTOR1, 8,
                          PGRAPH_UNIFORM_STAGE_MASK_PSH);
    assert_register_range(NV_PGRAPH_WINDOWCLIPX0, 8,
                          PGRAPH_UNIFORM_STAGE_MASK_PSH);
    assert_register_range(NV_PGRAPH_WINDOWCLIPY0, 8,
                          PGRAPH_UNIFORM_STAGE_MASK_PSH);
    assert_register_range(NV_PGRAPH_TEXFMT0, 4,
                          PGRAPH_UNIFORM_STAGE_MASK_PSH);

    g_assert_cmpuint(
        pgraph_reg_uniform_stage_mask(NV_PGRAPH_FOGPARAM0, UINT32_MAX), ==,
        PGRAPH_UNIFORM_STAGE_MASK_VSH);
    g_assert_cmpuint(
        pgraph_reg_uniform_stage_mask(NV_PGRAPH_ZCLIPMIN, UINT32_MAX), ==,
        PGRAPH_UNIFORM_STAGE_MASK_BOTH);
    g_assert_cmpuint(
        pgraph_reg_uniform_stage_mask(NV_PGRAPH_CONTROL_0, UINT32_MAX), ==,
        PGRAPH_UNIFORM_STAGE_MASK_PSH);
    g_assert_cmpuint(
        pgraph_reg_uniform_stage_mask(
            NV_PGRAPH_SETUPRASTER,
            NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE), ==,
        PGRAPH_UNIFORM_STAGE_MASK_PSH);
    g_assert_cmpuint(
        pgraph_reg_uniform_stage_mask(
            NV_PGRAPH_SETUPRASTER, NV_PGRAPH_SETUPRASTER_Z_FORMAT), ==,
        PGRAPH_UNIFORM_STAGE_MASK_BOTH);
    g_assert_cmpuint(
        pgraph_reg_uniform_stage_mask(NV_PGRAPH_BLEND, UINT32_MAX), ==,
        PGRAPH_UNIFORM_STAGE_MASK_NONE);
}

static void test_stage_update_decisions(void)
{
    PGRAPHUniformStageUpdateInputs inputs = { 0 };
    bool update_stage[PGRAPH_UNIFORM_STAGE_COUNT];

    inputs.source_changed[PGRAPH_UNIFORM_STAGE_PSH] = true;
    pgraph_uniform_stage_update_needs(&inputs, update_stage);
    g_assert_false(update_stage[PGRAPH_UNIFORM_STAGE_VSH]);
    g_assert_true(update_stage[PGRAPH_UNIFORM_STAGE_PSH]);

    inputs = (PGRAPHUniformStageUpdateInputs){ 0 };
    inputs.texture_bindings_changed = true;
    pgraph_uniform_stage_update_needs(&inputs, update_stage);
    g_assert_false(update_stage[PGRAPH_UNIFORM_STAGE_VSH]);
    g_assert_true(update_stage[PGRAPH_UNIFORM_STAGE_PSH]);

    inputs = (PGRAPHUniformStageUpdateInputs){ 0 };
    inputs.psh_effective_inputs_changed = true;
    pgraph_uniform_stage_update_needs(&inputs, update_stage);
    g_assert_false(update_stage[PGRAPH_UNIFORM_STAGE_VSH]);
    g_assert_true(update_stage[PGRAPH_UNIFORM_STAGE_PSH]);

    inputs = (PGRAPHUniformStageUpdateInputs){ 0 };
    inputs.vsh_rows_dirty = true;
    pgraph_uniform_stage_update_needs(&inputs, update_stage);
    g_assert_true(update_stage[PGRAPH_UNIFORM_STAGE_VSH]);
    g_assert_false(update_stage[PGRAPH_UNIFORM_STAGE_PSH]);

    inputs = (PGRAPHUniformStageUpdateInputs){ 0 };
    inputs.force_full_update = true;
    pgraph_uniform_stage_update_needs(&inputs, update_stage);
    g_assert_true(update_stage[PGRAPH_UNIFORM_STAGE_VSH]);
    g_assert_true(update_stage[PGRAPH_UNIFORM_STAGE_PSH]);
}

static uint32_t setup_raster(uint32_t mode, uint32_t enable)
{
    return (mode & NV_PGRAPH_SETUPRASTER_FRONTFACEMODE) | enable;
}

static void test_effective_polygon_offset(void)
{
    const uint32_t bias = 0x80000000;
    const uint32_t factor = 0x7FC01234;
    const uint32_t all_enables =
        NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE |
        NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE |
        NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE;
    PGRAPHPolygonOffsetUniformKey key;

    key = pgraph_polygon_offset_uniform_key(NV097_SET_BEGIN_END_OP_LINES,
                                             all_enables, bias, factor);
    g_assert_cmphex(key.offset_bits, ==, 0);
    g_assert_cmphex(key.factor_bits, ==, 0);

    const struct {
        uint32_t mode;
        uint32_t enable;
    } cases[] = {
        { NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL,
          NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE },
        { NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_LINE,
          NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE },
        { NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_POINT,
          NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE },
    };

    for (unsigned int primitive = NV097_SET_BEGIN_END_OP_TRIANGLES;
         primitive <= NV097_SET_BEGIN_END_OP_POLYGON; primitive++) {
        for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
            key = pgraph_polygon_offset_uniform_key(
                primitive,
                setup_raster(cases[i].mode, cases[i].enable), bias, factor);
            g_assert_cmphex(key.offset_bits, ==, bias);
            g_assert_cmphex(key.factor_bits, ==, factor);

            key = pgraph_polygon_offset_uniform_key(
                primitive,
                setup_raster(cases[i].mode,
                             cases[(i + 1) % G_N_ELEMENTS(cases)].enable),
                bias, factor);
            g_assert_cmphex(key.offset_bits, ==, 0);
            g_assert_cmphex(key.factor_bits, ==, 0);
        }
    }

    PGRAPHPolygonOffsetUniformKey same = { bias, factor };
    PGRAPHPolygonOffsetUniformKey changed = { bias ^ 1U, factor };
    g_assert_true(pgraph_polygon_offset_uniform_key_equal(same, same));
    g_assert_false(pgraph_polygon_offset_uniform_key_equal(same, changed));
    g_assert_true(pgraph_polygon_offset_uniform_key_changed(false, same,
                                                             same));
    g_assert_false(pgraph_polygon_offset_uniform_key_changed(true, same,
                                                              same));
    g_assert_true(pgraph_polygon_offset_uniform_key_changed(true, same,
                                                             changed));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xbox/pgraph/uniform-source/stage-epochs",
                    test_stage_epochs_are_independent);
    g_test_add_func("/xbox/pgraph/uniform-source/conservative-unknown",
                    test_both_and_unclassified_are_conservative);
    g_test_add_func("/xbox/pgraph/uniform-source/float-setter-signed-zero",
                    test_float_setter_preserves_signed_zero);
    g_test_add_func("/xbox/pgraph/uniform-source/float-setter-same-nan",
                    test_float_setter_ignores_same_nan_bits);
    g_test_add_func("/xbox/pgraph/uniform-source/register-classification",
                    test_register_stage_classification);
    g_test_add_func("/xbox/pgraph/uniform-source/stage-update-decisions",
                    test_stage_update_decisions);
    g_test_add_func("/xbox/pgraph/uniform-source/effective-polygon-offset",
                    test_effective_polygon_offset);

    return g_test_run();
}
