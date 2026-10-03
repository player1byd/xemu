/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "hw/xbox/mcpx/apu/vp/resample.h"

#include <samplerate.h>

typedef struct TestSamples {
    float *samples;
    long frames;
    long offset;
    long callback_frames;
    long callback_calls;
    int channels;
} TestSamples;

static long sample_callback(void *opaque, float **data)
{
    TestSamples *samples = opaque;

    samples->callback_calls++;
    long remaining = samples->frames - samples->offset;
    if (remaining == 0) {
        *data = NULL;
        return 0;
    }

    long frames = samples->callback_frames > 0 ?
                      MIN(remaining, samples->callback_frames) :
                      remaining;
    *data = &samples->samples[samples->offset * samples->channels];
    samples->offset += frames;
    return frames;
}

static void test_pack_mono(void)
{
    const float stereo[][2] = {
        { 0.25f, -0.5f },
        { 0.75f, -1.0f },
        { -0.125f, 1.0f },
    };
    float mono[ARRAY_SIZE(stereo)] = { 0 };

    mcpx_apu_pack_mono_samples(stereo, mono, ARRAY_SIZE(stereo));

    for (int i = 0; i < ARRAY_SIZE(stereo); i++) {
        g_assert_cmpfloat(mono[i], ==, stereo[i][0]);
    }
}

static void test_expand_mono(void)
{
    const float mono[] = { 0.25f, 0.75f, -0.125f };
    float stereo[ARRAY_SIZE(mono)][2] = { 0 };

    mcpx_apu_expand_mono_samples(mono, stereo, ARRAY_SIZE(mono));

    for (int i = 0; i < ARRAY_SIZE(mono); i++) {
        g_assert_cmpfloat(stereo[i][0], ==, mono[i]);
        g_assert_cmpfloat(stereo[i][1], ==, mono[i]);
    }
}

static void test_sinc_mono_matches_duplicated_stereo(void)
{
    enum { INPUT_FRAMES = 256, OUTPUT_FRAMES = 96 };
    float mono_input[INPUT_FRAMES];
    float stereo_input[INPUT_FRAMES][2];
    float mono_output[OUTPUT_FRAMES];
    float stereo_output[OUTPUT_FRAMES][2];

    for (int i = 0; i < INPUT_FRAMES; i++) {
        mono_input[i] = sinf(i * 0.071f) * 0.75f + cosf(i * 0.019f) * 0.2f;
        stereo_input[i][0] = mono_input[i];
        stereo_input[i][1] = mono_input[i];
    }

    TestSamples mono_samples = {
        .samples = mono_input,
        .frames = INPUT_FRAMES,
        .channels = 1,
    };
    TestSamples stereo_samples = {
        .samples = (float *)stereo_input,
        .frames = INPUT_FRAMES,
        .channels = 2,
    };
    int mono_err;
    int stereo_err;
    SRC_STATE *mono = src_callback_new(sample_callback, SRC_SINC_FASTEST, 1,
                                       &mono_err, &mono_samples);
    SRC_STATE *stereo = src_callback_new(sample_callback, SRC_SINC_FASTEST, 2,
                                         &stereo_err, &stereo_samples);

    g_assert_nonnull(mono);
    g_assert_cmpint(mono_err, ==, 0);
    g_assert_nonnull(stereo);
    g_assert_cmpint(stereo_err, ==, 0);

    long mono_count = src_callback_read(mono, 0.83, OUTPUT_FRAMES, mono_output);
    long stereo_count = src_callback_read(stereo, 0.83, OUTPUT_FRAMES,
                                          (float *)stereo_output);

    g_assert_cmpint(mono_count, ==, OUTPUT_FRAMES);
    g_assert_cmpint(stereo_count, ==, OUTPUT_FRAMES);
    for (int i = 0; i < OUTPUT_FRAMES; i++) {
        g_assert_cmpfloat_with_epsilon(mono_output[i], stereo_output[i][0],
                                       1e-7f);
        g_assert_cmpfloat_with_epsilon(stereo_output[i][0],
                                       stereo_output[i][1], 1e-7f);
    }

    src_delete(mono);
    src_delete(stereo);
}

static void run_sinc_streaming_equivalence(long callback_frames)
{
    enum {
        INPUT_FRAMES = 2048,
        OUTPUT_FRAMES = 32,
        OUTPUT_BLOCKS = 16,
    };
    static const double rates[] = { 0.55, 0.83, 1.0, 1.19, 1.71 };
    float mono_input[INPUT_FRAMES];
    float stereo_input[INPUT_FRAMES][2];
    float mono_output[OUTPUT_FRAMES];
    float stereo_output[OUTPUT_FRAMES][2];

    for (int i = 0; i < INPUT_FRAMES; i++) {
        mono_input[i] = sinf(i * 0.071f) * 0.75f +
                        cosf(i * 0.019f) * 0.2f;
        stereo_input[i][0] = mono_input[i];
        stereo_input[i][1] = mono_input[i];
    }

    TestSamples mono_samples = {
        .samples = mono_input,
        .frames = INPUT_FRAMES,
        .callback_frames = callback_frames,
        .channels = 1,
    };
    TestSamples stereo_samples = {
        .samples = (float *)stereo_input,
        .frames = INPUT_FRAMES,
        .callback_frames = callback_frames,
        .channels = 2,
    };
    int mono_err;
    int stereo_err;
    SRC_STATE *mono = src_callback_new(sample_callback, SRC_SINC_FASTEST, 1,
                                       &mono_err, &mono_samples);
    SRC_STATE *stereo = src_callback_new(sample_callback, SRC_SINC_FASTEST, 2,
                                         &stereo_err, &stereo_samples);

    g_assert_nonnull(mono);
    g_assert_cmpint(mono_err, ==, 0);
    g_assert_nonnull(stereo);
    g_assert_cmpint(stereo_err, ==, 0);

    for (int block = 0; block < OUTPUT_BLOCKS; block++) {
        double rate = rates[block % ARRAY_SIZE(rates)];
        long mono_count = src_callback_read(mono, rate, OUTPUT_FRAMES,
                                            mono_output);
        long stereo_count = src_callback_read(stereo, rate, OUTPUT_FRAMES,
                                              (float *)stereo_output);

        g_assert_cmpint(mono_count, ==, stereo_count);
        g_assert_cmpint(mono_count, ==, OUTPUT_FRAMES);
        g_assert_cmpint(mono_samples.offset, ==, stereo_samples.offset);
        g_assert_cmpint(mono_samples.callback_calls, ==,
                        stereo_samples.callback_calls);
        for (int i = 0; i < mono_count; i++) {
            g_assert_cmpfloat_with_epsilon(mono_output[i],
                                           stereo_output[i][0], 1e-7f);
            g_assert_cmpfloat_with_epsilon(stereo_output[i][0],
                                           stereo_output[i][1], 1e-7f);
        }
    }

    src_delete(mono);
    src_delete(stereo);
}

static void test_sinc_streaming_mono_matches_duplicated_stereo(void)
{
    static const long callback_frames[] = { 1, 7, 17, 31, 32, 33, 64 };

    for (int i = 0; i < ARRAY_SIZE(callback_frames); i++) {
        run_sinc_streaming_equivalence(callback_frames[i]);
    }
}

static void test_sinc_channel_change_at_stream_reset(void)
{
    enum { INPUT_FRAMES = 256, OUTPUT_FRAMES = 32 };
    float mono_input[INPUT_FRAMES];
    float stereo_input[INPUT_FRAMES][2];
    float recreated_output[OUTPUT_FRAMES][2];
    float fresh_output[OUTPUT_FRAMES][2];

    for (int i = 0; i < INPUT_FRAMES; i++) {
        mono_input[i] = sinf(i * 0.071f) * 0.75f;
        stereo_input[i][0] = sinf(i * 0.037f) * 0.5f;
        stereo_input[i][1] = cosf(i * 0.053f) * 0.4f;
    }

    TestSamples mono_samples = {
        .samples = mono_input,
        .frames = INPUT_FRAMES,
        .callback_frames = 17,
        .channels = 1,
    };
    int err;
    SRC_STATE *resampler = src_callback_new(
        sample_callback, SRC_SINC_FASTEST, 1, &err, &mono_samples);
    g_assert_nonnull(resampler);
    g_assert_cmpint(err, ==, 0);

    float warm_output[OUTPUT_FRAMES];
    g_assert_cmpint(src_callback_read(resampler, 0.83, OUTPUT_FRAMES,
                                     warm_output),
                    ==, OUTPUT_FRAMES);
    g_assert_cmpint(mono_samples.offset, >, 0);
    src_delete(resampler);

    TestSamples recreated_samples = {
        .samples = (float *)stereo_input,
        .frames = INPUT_FRAMES,
        .callback_frames = 17,
        .channels = 2,
    };
    TestSamples fresh_samples = recreated_samples;
    resampler = src_callback_new(sample_callback, SRC_SINC_FASTEST, 2, &err,
                                 &recreated_samples);
    g_assert_nonnull(resampler);
    g_assert_cmpint(err, ==, 0);
    SRC_STATE *fresh = src_callback_new(sample_callback, SRC_SINC_FASTEST, 2,
                                        &err, &fresh_samples);
    g_assert_nonnull(fresh);
    g_assert_cmpint(err, ==, 0);

    long recreated_count = src_callback_read(
        resampler, 1.19, OUTPUT_FRAMES, (float *)recreated_output);
    long fresh_count = src_callback_read(fresh, 1.19, OUTPUT_FRAMES,
                                         (float *)fresh_output);

    g_assert_cmpint(recreated_count, ==, fresh_count);
    g_assert_cmpint(recreated_count, ==, OUTPUT_FRAMES);
    g_assert_cmpint(recreated_samples.offset, ==, fresh_samples.offset);
    g_assert_cmpint(recreated_samples.callback_calls, ==,
                    fresh_samples.callback_calls);
    for (int i = 0; i < recreated_count; i++) {
        g_assert_cmpfloat_with_epsilon(recreated_output[i][0],
                                       fresh_output[i][0], 0.0000001f);
        g_assert_cmpfloat_with_epsilon(recreated_output[i][1],
                                       fresh_output[i][1], 0.0000001f);
    }

    src_delete(resampler);
    src_delete(fresh);
}

static void test_full_reset_discards_resampler_history(void)
{
    enum { INPUT_FRAMES = 512, OUTPUT_FRAMES = 32 };
    float signal_a[INPUT_FRAMES * 2];
    float signal_b[INPUT_FRAMES * 2];
    float reset_output[OUTPUT_FRAMES * 2];
    float fresh_output[OUTPUT_FRAMES * 2];

    for (int channels = 1; channels <= 2; channels++) {
        for (int frame = 0; frame < INPUT_FRAMES; frame++) {
            for (int channel = 0; channel < channels; channel++) {
                signal_a[frame * channels + channel] =
                    0.75f + 0.2f * sinf(frame * 0.071f + channel);
                signal_b[frame * channels + channel] =
                    -0.75f + 0.2f * cosf(frame * 0.053f + channel);
            }
        }

        TestSamples old_samples = {
            .samples = signal_a,
            .frames = INPUT_FRAMES,
            .callback_frames = 17,
            .channels = channels,
        };
        int err;
        SRC_STATE *resampler = src_callback_new(
            sample_callback, SRC_SINC_FASTEST, channels, &err, &old_samples);
        int resampler_channels = channels;
        g_assert_nonnull(resampler);
        g_assert_cmpint(err, ==, 0);

        float warm_output[OUTPUT_FRAMES * 2];
        g_assert_cmpint(src_callback_read(resampler, 0.55, OUTPUT_FRAMES,
                                         warm_output),
                        ==, OUTPUT_FRAMES);
        g_assert_cmpint(old_samples.offset, >, 0);

        mcpx_apu_resampler_destroy(&resampler, &resampler_channels);
        g_assert_null(resampler);
        g_assert_cmpint(resampler_channels, ==, 0);

        TestSamples reset_samples = {
            .samples = signal_b,
            .frames = INPUT_FRAMES,
            .callback_frames = 17,
            .channels = channels,
        };
        TestSamples fresh_samples = reset_samples;
        resampler = src_callback_new(sample_callback, SRC_SINC_FASTEST,
                                     channels, &err, &reset_samples);
        resampler_channels = channels;
        g_assert_nonnull(resampler);
        g_assert_cmpint(err, ==, 0);
        SRC_STATE *fresh = src_callback_new(sample_callback, SRC_SINC_FASTEST,
                                            channels, &err, &fresh_samples);
        g_assert_nonnull(fresh);
        g_assert_cmpint(err, ==, 0);

        long reset_count = src_callback_read(
            resampler, 0.83, OUTPUT_FRAMES, reset_output);
        long fresh_count = src_callback_read(fresh, 0.83, OUTPUT_FRAMES,
                                             fresh_output);

        g_assert_cmpint(reset_count, ==, fresh_count);
        g_assert_cmpint(reset_count, ==, OUTPUT_FRAMES);
        g_assert_cmpint(reset_samples.offset, ==, fresh_samples.offset);
        g_assert_cmpint(reset_samples.callback_calls, ==,
                        fresh_samples.callback_calls);
        for (int i = 0; i < reset_count * channels; i++) {
            g_assert_cmpfloat_with_epsilon(reset_output[i], fresh_output[i],
                                           0.0000001f);
        }

        mcpx_apu_resampler_destroy(&resampler, &resampler_channels);
        src_delete(fresh);
    }

    SRC_STATE *resampler = NULL;
    int resampler_channels = 2;
    mcpx_apu_resampler_destroy(&resampler, &resampler_channels);
    g_assert_cmpint(resampler_channels, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/mcpx/apu/resampler/pack-mono", test_pack_mono);
    g_test_add_func("/mcpx/apu/resampler/expand-mono", test_expand_mono);
    g_test_add_func("/mcpx/apu/resampler/sinc-equivalence",
                    test_sinc_mono_matches_duplicated_stereo);
    g_test_add_func("/mcpx/apu/resampler/sinc-streaming-equivalence",
                    test_sinc_streaming_mono_matches_duplicated_stereo);
    g_test_add_func("/mcpx/apu/resampler/channel-change-at-stream-reset",
                    test_sinc_channel_change_at_stream_reset);
    g_test_add_func("/mcpx/apu/resampler/full-reset-discards-history",
                    test_full_reset_discards_resampler_history);
    return g_test_run();
}
