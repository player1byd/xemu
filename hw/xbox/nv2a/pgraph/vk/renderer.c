/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "qemu/error-report.h"
#include "ui/xemu-gpu-info.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-tweaks.h"
#include "failpoint.h"
#include "renderer.h"
#include "hybrid-ready.h"

#include "gloffscreen.h"

static void pgraph_vk_hybrid_service_timer_fired(void *opaque)
{
    NV2AState *d = opaque;
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (!r) {
        return;
    }
    qatomic_set(&r->hybrid_prewarm_service_pending, true);
    pgraph_vk_hybrid_worker_notify(d);
}

void pgraph_vk_hybrid_schedule_service(PGRAPHState *pg, int64_t deadline_us)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r || !r->hybrid_service_timer) {
        return;
    }
    int64_t delay_us = MAX(deadline_us - g_get_monotonic_time(), 0);
    int64_t expiry_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                        delay_us * 1000;
    if (!timer_pending(r->hybrid_service_timer) ||
        expiry_ns < timer_expire_time_ns(r->hybrid_service_timer)) {
        timer_mod_ns(r->hybrid_service_timer, expiry_ns);
    }
}

#if HAVE_EXTERNAL_MEMORY
static GloContext *g_gl_context;

static bool current_gl_context_matches_device(
    const PGRAPHVkDeviceRecord *device)
{
    GLint device_count = 0;
    uint8_t driver_uuid[PGRAPH_VK_DEVICE_UUID_SIZE];
    bool has_external_memory =
        epoxy_has_gl_extension("GL_EXT_memory_object");
#ifdef WIN32
    bool has_platform_handle =
        epoxy_has_gl_extension("GL_EXT_memory_object_win32");
#else
    bool has_platform_handle =
        epoxy_has_gl_extension("GL_EXT_memory_object_fd");
#endif

    if (!has_external_memory || !has_platform_handle) {
        return false;
    }

    while (glGetError() != GL_NO_ERROR) {
    }
    glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &device_count);
    if (glGetError() != GL_NO_ERROR || device_count <= 0 ||
        device_count > 32) {
        return false;
    }

    uint8_t (*device_uuids)[PGRAPH_VK_DEVICE_UUID_SIZE] =
        g_malloc_n(device_count, sizeof(*device_uuids));
    for (GLint i = 0; i < device_count; i++) {
        glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, i, device_uuids[i]);
    }
    glGetUnsignedBytevEXT(GL_DRIVER_UUID_EXT, driver_uuid);

    bool query_succeeded = glGetError() == GL_NO_ERROR;
    bool matches = query_succeeded &&
        pgraph_vk_shared_presentation_supported(
            device, has_external_memory, has_platform_handle,
            device_uuids, device_count, driver_uuid);
    g_free(device_uuids);
    return matches;
}
#endif

static void early_context_init(void)
{
#if HAVE_EXTERNAL_MEMORY
    g_gl_context = glo_context_create();
#endif
}

static void pgraph_vk_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->vk_renderer_state = (PGRAPHVkState *)g_malloc0(sizeof(PGRAPHVkState));
    pgraph_vk_failpoint_init();

#if HAVE_EXTERNAL_MEMORY
    glo_set_current(g_gl_context);
#endif

    pgraph_vk_debug_init();

    pgraph_vk_init_instance(pg, errp);
    if (*errp) {
        return;
    }
    pg->vk_renderer_state->hybrid_service_timer = timer_new_ns(
        QEMU_CLOCK_REALTIME, pgraph_vk_hybrid_service_timer_fired, d);

#if HAVE_EXTERNAL_MEMORY
    pg->vk_renderer_state->display.shared_presentation =
        current_gl_context_matches_device(
            &pg->vk_renderer_state->selected_device);
    fprintf(stderr, "Vulkan presentation transport: %s\n",
            pg->vk_renderer_state->display.shared_presentation ?
                "shared external memory" : "host copy");
#endif

    pgraph_vk_perf_init(pg->vk_renderer_state);
    const char *hybrid_trace_path = g_getenv("XEMU_VK_HYBRID_TRACE");
    if (hybrid_trace_path && hybrid_trace_path[0]) {
        pg->vk_renderer_state->hybrid_trace =
            pgraph_vk_hybrid_trace_open(hybrid_trace_path, 50000);
        if (!pg->vk_renderer_state->hybrid_trace) {
            error_report("nv2a/vk: could not open hybrid trace output");
        }
    }
    pgraph_vk_init_command_buffers(pg);
    pgraph_vk_init_buffers(d);
    pgraph_vk_init_surfaces(pg);
    pgraph_vk_init_shaders(pg);
    pgraph_vk_init_pipelines(pg);
    pgraph_vk_init_textures(pg);
    pgraph_vk_init_reports(pg);
    pgraph_vk_init_compute(pg);
    pgraph_vk_init_display(pg);

    qatomic_set(&pg->vk_renderer_state->hybrid_prewarm_service_pending,
                pg->vk_renderer_state->hybrid_prewarm.enabled);

    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    pgraph_vk_clear_vertex_ram_stale(pg->vk_renderer_state);

    pgraph_vk_determine_gpu_properties(d);
    xemu_vulkan_ubershader_publish_runtime(
        true,
        !pg->vk_renderer_state->ubershader_runtime_enabled ||
        (pg->vk_renderer_state->hybrid_compiler_initialized &&
         pg->vk_renderer_state->hybrid_pipeline_builder_initialized));
}

static void pgraph_vk_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    timer_del(pg->vk_renderer_state->hybrid_service_timer);

    /* Finish recorded draws before destroying their cached pipelines. */
    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    pgraph_vk_finalize_display(pg);
    pgraph_vk_finalize_compute(pg);
    pgraph_vk_finalize_reports(pg);
    pgraph_vk_finalize_textures(pg);
    pgraph_vk_finalize_pipelines(pg);
    pgraph_vk_finalize_shaders(pg);
    pgraph_vk_finalize_surfaces(pg);
    pgraph_vk_finalize_buffers(d);
    pgraph_vk_finalize_command_buffers(pg);
    pgraph_vk_perf_finalize(pg->vk_renderer_state);
    pgraph_vk_hybrid_trace_close(pg->vk_renderer_state->hybrid_trace);
    pgraph_vk_finalize_instance(pg);
    pgraph_vk_failpoint_report();

    timer_free(pg->vk_renderer_state->hybrid_service_timer);
    pg->vk_renderer_state->hybrid_service_timer = NULL;

    g_free(pg->vk_renderer_state);
    pg->vk_renderer_state = NULL;
}

static void pgraph_vk_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    pgraph_vk_invalidate_blend_constants(pg);
    pgraph_vk_surface_flush(d);
    pgraph_vk_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    pgraph_vk_clear_vertex_ram_stale(pg->vk_renderer_state);
    for (int i = 0; i < 4; i++) {
        pg->texture_dirty[i] = true;
    }

    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_vk_sync(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    pgraph_vk_render_display(pg);

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_vk_process_pending(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending) ||
        qatomic_read(&r->spirv_cache_writeback_pending) ||
        qatomic_read(&r->hybrid_prewarm_service_pending) ||
        (r->hybrid_compiler_initialized &&
         pgraph_vk_hybrid_compiler_has_result(&r->hybrid_compiler)) ||
        (r->hybrid_pipeline_builder_initialized &&
         pgraph_vk_hybrid_pipeline_builder_has_result(
             &r->hybrid_pipeline_builder))
    ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        qatomic_set(&r->hybrid_completion_kick_pending, false);
        pgraph_vk_hybrid_owner_budget_begin(r);
        qatomic_set(&r->hybrid_prewarm_service_pending, false);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_vk_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_vk_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_vk_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_vk_flush(d);
        }
        if (r->hybrid_compiler_initialized) {
            pgraph_vk_process_hybrid_completions(&d->pgraph);
            /* A published fallback module may unblock a retained family.
             * Give it one bounded service pass without waiting for a flip. */
            pgraph_vk_process_fallback_families(&d->pgraph);
        }
        if (r->hybrid_pipeline_builder_initialized &&
            pgraph_vk_hybrid_pipeline_builder_has_result(
                &r->hybrid_pipeline_builder)) {
            pgraph_vk_process_hybrid_pipeline_completions(&d->pgraph);
        }
        /* Start learned preparation before the first flip and continue one
         * bounded candidate at each existing renderer service opportunity. */
        pgraph_vk_process_hybrid_prewarm(&d->pgraph);
        pgraph_vk_process_fallback_families(&d->pgraph);
        if (qatomic_read(&r->spirv_cache_writeback_pending)) {
            pgraph_vk_writeback_pipeline_cache(&d->pgraph);
            pgraph_vk_process_spirv_cache_writeback(&d->pgraph);
            qatomic_set(&r->spirv_cache_writeback_pending, false);
            qemu_event_set(&r->spirv_cache_writeback_complete);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_vk_flip_stall(NV2AState *d)
{
    pgraph_vk_hybrid_owner_budget_begin(d->pgraph.vk_renderer_state);
    pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_FLIP_STALL);
    pgraph_vk_process_fallback_families(&d->pgraph);
    pgraph_vk_process_hybrid_prewarm(&d->pgraph);
    pgraph_vk_process_fallback_families(&d->pgraph);
    pgraph_vk_perf_frame(d->pgraph.vk_renderer_state);
    pgraph_vk_hybrid_trace_frame(
        d->pgraph.vk_renderer_state->hybrid_trace);
    pgraph_vk_debug_frame_terminator();
}

static void pgraph_vk_pre_savevm_trigger(NV2AState *d)
{
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_savevm_wait(NV2AState *d)
{
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_shutdown_trigger(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (r->hybrid_compiler_initialized) {
        /* The worker owns no Vulkan objects and cannot publish after stop. */
        pgraph_vk_hybrid_compiler_stop(&r->hybrid_compiler);
    }

    if (!r->spirv_cache_writeback_complete_initialized ||
        !r->spirv_cache_session_eligible ||
        !g_config.perf.cache_shaders ||
        r->spirv_cache_writeback_requested ||
        qatomic_read(&r->spirv_cache_writeback_pending)) {
        return;
    }
    qemu_event_reset(&r->spirv_cache_writeback_complete);
    r->spirv_cache_writeback_requested = true;
    qatomic_set(&r->spirv_cache_writeback_pending, true);
}

static void pgraph_vk_pre_shutdown_wait(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (r->spirv_cache_writeback_complete_initialized &&
        r->spirv_cache_writeback_requested) {
        qemu_event_wait(&r->spirv_cache_writeback_complete);
        r->spirv_cache_writeback_requested = false;
    }
}

static int pgraph_vk_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_perf_record_framebuffer_acquire(r);
    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    assert(surface->color);

    surface->frame_time = pg->frame_time;

    pgraph_vk_perf_record_valid_sync_request(r);
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    qemu_event_set(&pg->renderer_switch_progress);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);

#if HAVE_EXTERNAL_MEMORY
    if (r->display.shared_presentation) {
        return r->display.gl_texture_id;
    }

    PGRAPHVkDisplayState *display = &r->display;
    PGRAPHVkHostCopyUploadState *upload = &display->host_copy.upload;
    if (display->host_copy.gl_texture_id &&
        !pgraph_vk_host_copy_upload_needed(
            upload, display->completed_output_generation,
            display->width, display->height)) {
        pgraph_vk_perf_record_host_copy_result(r, true, 0);
        return display->host_copy.gl_texture_id;
    }

    if (!display->host_copy.gl_texture_id) {
        glGenTextures(1, &display->host_copy.gl_texture_id);
        glBindTexture(GL_TEXTURE_2D, display->host_copy.gl_texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    } else {
        glBindTexture(GL_TEXTURE_2D, display->host_copy.gl_texture_id);
    }
    GLint unpack_alignment;
    GLint unpack_row_length;
    GLint unpack_skip_rows;
    GLint unpack_skip_pixels;
    GLint unpack_image_height;
    GLint unpack_skip_images;
    GLint unpack_buffer_binding;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
    glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &unpack_image_height);
    glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &unpack_skip_images);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer_binding);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    if (!upload->valid || upload->width != display->width ||
        upload->height != display->height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, display->width,
                     display->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     display->host_copy.mapped);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, display->width,
                        display->height, GL_RGBA, GL_UNSIGNED_BYTE,
                        display->host_copy.mapped);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer_binding);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, unpack_image_height);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, unpack_skip_images);
    assert(glGetError() == GL_NO_ERROR);
    pgraph_vk_host_copy_mark_uploaded(
        upload, display->completed_output_generation,
        display->width, display->height);
    pgraph_vk_perf_record_host_copy_result(
        r, false, (uint64_t)display->width * display->height * 4);
    if (!r->display.presentation_reported) {
        xemu_gpu_info_record_presentation(
            XEMU_GPU_PRESENTATION_HOST_COPY,
            (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER));
        r->display.presentation_reported = true;
    }
    return display->host_copy.gl_texture_id;
#else
    if (!pgraph_vk_wait_for_surface_download(surface)) {
        error_report("Vulkan framebuffer readback failed");
        abort();
    }
    return 0;
#endif
}

static PGRAPHRenderer pgraph_vk_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_VULKAN,
    .name = "Vulkan",
    .ops = {
        .init = pgraph_vk_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_vk_finalize,
        .clear_report_value = pgraph_vk_clear_report_value,
        .clear_surface = pgraph_vk_clear_surface,
        .draw_begin = pgraph_vk_draw_begin,
        .draw_end = pgraph_vk_draw_end,
        .flip_stall = pgraph_vk_flip_stall,
        .flush_draw = pgraph_vk_flush_draw,
        .get_report = pgraph_vk_get_report,
        .image_blit = pgraph_vk_image_blit,
        .pre_savevm_trigger = pgraph_vk_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_vk_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_vk_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_vk_pre_shutdown_wait,
        .process_pending = pgraph_vk_process_pending,
        .process_pending_reports = pgraph_vk_process_pending_reports,
        .surface_update = pgraph_vk_surface_update,
        .set_surface_scale_factor = pgraph_vk_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_vk_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_vk_get_framebuffer_surface,
        .get_gpu_properties = pgraph_vk_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_vk_renderer);
}

void pgraph_vk_check_memory_budget(PGRAPHState *pg)
{
#if 0 // FIXME
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties const *props;
    vmaGetMemoryProperties(r->allocator, &props);

    g_autofree VmaBudget *budgets = g_malloc_n(props->memoryHeapCount, sizeof(VmaBudget));
    vmaGetHeapBudgets(r->allocator, budgets);

    const float budget_threshold = 0.8;
    bool near_budget = false;

    for (int i = 0; i < props->memoryHeapCount; i++) {
        VmaBudget *b = &budgets[i];
        float use_to_budget_ratio =
            (double)b->statistics.allocationBytes / (double)b->budget;
        NV2A_VK_DPRINTF("Heap %d: used %lu/%lu MiB (%.2f%%)", i,
                        b->statistics.allocationBytes / (1024 * 1024),
                        b->budget / (1024 * 1024), use_to_budget_ratio * 100);
        near_budget |= use_to_budget_ratio > budget_threshold;
    }

    // If any heaps are near budget, free up some resources
    if (near_budget) {
        pgraph_vk_trim_texture_cache(pg);
    }
#endif

#if 0
    char *s;
    vmaBuildStatsString(r->allocator, &s, VK_TRUE);
    puts(s);
    vmaFreeStatsString(r->allocator, s);
#endif
}
