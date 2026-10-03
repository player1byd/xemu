/*
 * Opt-in Vulkan wait ownership telemetry.
 *
 * No file is opened and each call is a single branch unless XEMU_VK_PERF_LOG
 * is set. Records are aggregated in memory and emitted once per guest frame.
 */

#include "renderer.h"

/*
 * Morrowind issues about 102 VERTEX_BUFFER_DIRTY submissions per guest frame.
 * Time the first eight occurrences of every reason so rare paths remain exact,
 * then one in sixteen hot occurrences. This retains roughly fourteen timing
 * samples for that hot path while removing about 86% of its clock reads.
 */
#define VK_PERF_INITIAL_TIMED_SUBMITS 8
#define VK_PERF_HOT_SAMPLE_STRIDE 16

static const char *finish_reason_names[VK_FINISH_REASON_COUNT] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = "vertex_buffer_dirty",
    [VK_FINISH_REASON_SURFACE_CREATE] = "surface_create",
    [VK_FINISH_REASON_SURFACE_DOWN] = "surface_down",
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = "need_buffer_space",
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = "framebuffer_dirty",
    [VK_FINISH_REASON_PRESENTING] = "presenting",
    [VK_FINISH_REASON_FLIP_STALL] = "flip_stall",
    [VK_FINISH_REASON_FLUSH] = "flush",
    [VK_FINISH_REASON_STALLED] = "stalled",
    [VK_FINISH_REASON_TEXTURE_DIRTY] = "texture_dirty",
};

static const char *single_time_reason_names[VK_SINGLE_TIME_REASON_COUNT] = {
    [VK_SINGLE_TIME_PVIDEO_UPLOAD] = "pvideo_upload",
    [VK_SINGLE_TIME_DISPLAY_RENDER] = "display_render",
    [VK_SINGLE_TIME_SURFACE_DOWNLOAD] = "surface_download",
    [VK_SINGLE_TIME_SURFACE_CREATE] = "surface_create",
    [VK_SINGLE_TIME_SURFACE_UPLOAD] = "surface_upload",
    [VK_SINGLE_TIME_TEXTURE_UPLOAD] = "texture_upload",
    [VK_SINGLE_TIME_DUMMY_TEXTURE_CREATE] = "dummy_texture_create",
};

static const char *cpu_region_names[VK_PERF_CPU_REGION_COUNT] = {
    [VK_PERF_CPU_DRAW_BEGIN_SURFACE_UPDATE] = "draw_begin_surface_update",
    [VK_PERF_CPU_DRAW_FLUSH] = "draw_flush",
    [VK_PERF_CPU_PIPELINE_PREPARE] = "pipeline_prepare",
    [VK_PERF_CPU_BIND_TEXTURES] = "bind_textures",
    [VK_PERF_CPU_TEXTURE_UPLOAD] = "texture_upload",
    [VK_PERF_CPU_UPDATE_DESCRIPTOR_SETS] = "update_descriptor_sets",
};

static void write_names(FILE *file, const char *key, const char **names,
                        size_t count)
{
    fprintf(file, ",\"%s\":[", key);
    for (size_t i = 0; i < count; i++) {
        fprintf(file, "%s\"%s\"", i ? "," : "", names[i]);
    }
    fputc(']', file);
}

static void write_stat_array(FILE *file, const char *key,
                             const PGRAPHVkWaitStats *stats, size_t count,
                             size_t member_offset)
{
    fprintf(file, ",\"%s\":[", key);
    for (size_t i = 0; i < count; i++) {
        const uint64_t *value = (const uint64_t *)
            ((const uint8_t *)&stats[i] + member_offset);
        fprintf(file, "%s%" PRIu64, i ? "," : "", *value);
    }
    fputc(']', file);
}

static void write_cpu_stat_array(FILE *file, const char *key,
                                 const PGRAPHVkCpuStats *stats,
                                 size_t member_offset)
{
    fprintf(file, ",\"%s\":[", key);
    for (size_t i = 0; i < VK_PERF_CPU_REGION_COUNT; i++) {
        const uint64_t *value = (const uint64_t *)
            ((const uint8_t *)&stats[i] + member_offset);
        fprintf(file, "%s%" PRIu64, i ? "," : "", *value);
    }
    fputc(']', file);
}

void pgraph_vk_perf_init(PGRAPHVkState *r)
{
    const char *path = g_getenv("XEMU_VK_PERF_LOG");
    if (path == NULL || path[0] == '\0') {
        return;
    }

    r->perf.file = qemu_fopen(path, "w");
    if (r->perf.file == NULL) {
        fprintf(stderr, "nv2a: failed to open Vulkan perf log '%s'\n", path);
        return;
    }
    r->perf.enabled = true;
    r->perf.last_flush_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    fprintf(r->perf.file,
            "{\"type\":\"schema\",\"schema_version\":8"
            ",\"features\":[\"report_lifecycle\","
            "\"descriptor_publication\",\"surface_upload\"]"
            ",\"duration_sampling\":{\"initial_per_reason_per_frame\":%u"
            ",\"hot_stride\":%u}"
            ",\"presentation_counters\":\"cumulative_totals\"",
            VK_PERF_INITIAL_TIMED_SUBMITS, VK_PERF_HOT_SAMPLE_STRIDE);
    write_names(r->perf.file, "finish_reasons", finish_reason_names,
                ARRAY_SIZE(finish_reason_names));
    write_names(r->perf.file, "single_time_callers", single_time_reason_names,
                ARRAY_SIZE(single_time_reason_names));
    write_names(r->perf.file, "cpu_regions", cpu_region_names,
                ARRAY_SIZE(cpu_region_names));
    fprintf(r->perf.file, "}\n");
}

void pgraph_vk_perf_finalize(PGRAPHVkState *r)
{
    if (r->perf.file != NULL) {
        fflush(r->perf.file);
        fclose(r->perf.file);
    }
    r->perf.file = NULL;
    r->perf.enabled = false;
}

void pgraph_vk_perf_record_finish_call(PGRAPHVkState *r, FinishReason reason)
{
    assert(reason < VK_FINISH_REASON_COUNT);
    if (r->perf.enabled) {
        r->perf.finish[reason].call_count++;
    }
}

bool pgraph_vk_perf_should_time_finish(PGRAPHVkState *r, FinishReason reason)
{
    if (!r->perf.enabled) {
        return false;
    }
    assert(reason < VK_FINISH_REASON_COUNT);
    uint64_t occurrence = r->perf.finish[reason].call_count;
    return occurrence <= VK_PERF_INITIAL_TIMED_SUBMITS ||
           occurrence % VK_PERF_HOT_SAMPLE_STRIDE == 0;
}

void pgraph_vk_perf_record_finish_submit(PGRAPHVkState *r,
                                         FinishReason reason,
                                         bool timed,
                                         uint64_t submit_cpu_us,
                                         uint64_t wait_us,
                                         uint64_t staged_bytes,
                                         uint64_t submit_info_count,
                                         uint64_t command_buffer_count)
{
    if (!r->perf.enabled) {
        return;
    }
    assert(reason < VK_FINISH_REASON_COUNT);
    PGRAPHVkWaitStats *stats = &r->perf.finish[reason];
    stats->submit_count++;
    stats->wait_count++;
    if (timed) {
        stats->timed_submit_count++;
        stats->submit_cpu_us += submit_cpu_us;
        stats->wait_us += wait_us;
    }
    r->perf.submit_info_count += submit_info_count;
    r->perf.command_buffer_count += command_buffer_count;
    r->perf.staged_bytes += staged_bytes;
    r->perf.peak_in_flight_submission_count = MAX(
        r->perf.peak_in_flight_submission_count, 1);
    r->perf.newest_submitted_serial = ++r->perf.submission_serial;
}

void pgraph_vk_perf_record_single_time_submit(PGRAPHVkState *r,
                                               SingleTimeReason reason,
                                               uint64_t submit_cpu_us,
                                               uint64_t wait_us,
                                               uint64_t staged_bytes)
{
    if (!r->perf.enabled) {
        return;
    }
    assert(reason < VK_SINGLE_TIME_REASON_COUNT);
    PGRAPHVkWaitStats *stats = &r->perf.single_time[reason];
    stats->call_count++;
    stats->submit_count++;
    stats->timed_submit_count++;
    stats->submit_cpu_us += submit_cpu_us;
    stats->wait_count++;
    stats->wait_us += wait_us;
    r->perf.submit_info_count++;
    r->perf.command_buffer_count++;
    r->perf.staged_bytes += staged_bytes;
    r->perf.peak_in_flight_submission_count = MAX(
        r->perf.peak_in_flight_submission_count, 1);
    r->perf.newest_submitted_serial = ++r->perf.submission_serial;
}

void pgraph_vk_perf_record_vertex_staging_copy(PGRAPHVkState *r,
                                                uint64_t bytes)
{
    if (r->perf.enabled) {
        r->perf.vertex_staged_bytes += bytes;
        r->perf.vertex_staging_copy_count++;
    }
}

void pgraph_vk_perf_record_vertex_direct_copy(PGRAPHVkState *r,
                                               uint64_t bytes)
{
    if (r->perf.enabled) {
        r->perf.vertex_direct_bytes += bytes;
        r->perf.vertex_direct_copy_count++;
    }
}

void pgraph_vk_perf_record_vertex_staging_growth(PGRAPHVkState *r)
{
    if (r->perf.enabled) {
        r->perf.vertex_staging_capacity_growth_count++;
    }
}

void pgraph_vk_perf_record_vertex_staging_fallback(PGRAPHVkState *r)
{
    if (r->perf.enabled) {
        r->perf.vertex_staging_fallback_finish_count++;
    }
}

void pgraph_vk_perf_record_bc_upload(PGRAPHVkState *r, bool native,
                                     uint64_t source_bytes,
                                     uint64_t staged_bytes,
                                     uint64_t prepare_cpu_us)
{
    if (!r->perf.enabled) {
        return;
    }
    if (native) {
        r->perf.native_bc_upload_count++;
        r->perf.native_bc_source_bytes += source_bytes;
        r->perf.native_bc_staged_bytes += staged_bytes;
        r->perf.native_bc_prepare_cpu_us += prepare_cpu_us;
    } else {
        r->perf.decoded_bc_upload_count++;
        r->perf.decoded_bc_source_bytes += source_bytes;
        r->perf.decoded_bc_staged_bytes += staged_bytes;
        r->perf.decoded_bc_prepare_cpu_us += prepare_cpu_us;
    }
}

void pgraph_vk_perf_record_cpu_region(PGRAPHVkState *r, PerfCpuRegion region,
                                      uint64_t cpu_us)
{
    assert(region < VK_PERF_CPU_REGION_COUNT);
    if (r->perf.enabled) {
        r->perf.cpu_regions[region].call_count++;
        r->perf.cpu_regions[region].cpu_us += cpu_us;
    }
}

void pgraph_vk_perf_record_framebuffer_acquire(PGRAPHVkState *r)
{
    if (r->perf.enabled) {
        qatomic_fetch_add(&r->perf.framebuffer_acquire_calls_total, 1);
    }
}

void pgraph_vk_perf_record_valid_sync_request(PGRAPHVkState *r)
{
    if (r->perf.enabled) {
        qatomic_fetch_add(&r->perf.valid_sync_requests_total, 1);
    }
}

void pgraph_vk_perf_record_host_copy_result(PGRAPHVkState *r, bool skipped,
                                            uint64_t uploaded_bytes)
{
    if (!r->perf.enabled) {
        return;
    }

    if (skipped) {
        qatomic_fetch_add(&r->perf.host_copy_upload_skips_total, 1);
    } else {
        qatomic_fetch_add(&r->perf.host_copy_uploads_total, 1);
        qatomic_fetch_add(&r->perf.host_copy_uploaded_bytes_total,
                          uploaded_bytes);
    }
}

void pgraph_vk_perf_frame(PGRAPHVkState *r)
{
    PGRAPHVkPerfTelemetry *perf = &r->perf;
    if (!perf->enabled) {
        return;
    }

    uint64_t submit_count = 0;
    for (size_t i = 0; i < ARRAY_SIZE(perf->finish); i++) {
        submit_count += perf->finish[i].submit_count;
    }
    for (size_t i = 0; i < ARRAY_SIZE(perf->single_time); i++) {
        submit_count += perf->single_time[i].submit_count;
    }
    double staged_bytes_per_submit = submit_count ?
        (double)perf->staged_bytes / submit_count : 0.0;
    double submit_infos_per_submit = submit_count ?
        (double)perf->submit_info_count / submit_count : 0.0;
    double command_buffers_per_submit = submit_count ?
        (double)perf->command_buffer_count / submit_count : 0.0;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    fprintf(perf->file,
            "{\"type\":\"frame\",\"schema_version\":8"
            ",\"timestamp_us\":%" PRId64 ",\"guest_frame\":%" PRIu64,
            now, ++perf->frame);
    write_stat_array(perf->file, "finish_count_per_guest_frame", perf->finish,
                     ARRAY_SIZE(perf->finish),
                     offsetof(PGRAPHVkWaitStats, call_count));
    write_stat_array(perf->file, "finish_submit_count_per_guest_frame",
                     perf->finish, ARRAY_SIZE(perf->finish),
                     offsetof(PGRAPHVkWaitStats, submit_count));
    write_stat_array(perf->file, "finish_timed_submit_count_per_guest_frame",
                     perf->finish, ARRAY_SIZE(perf->finish),
                     offsetof(PGRAPHVkWaitStats, timed_submit_count));
    write_stat_array(perf->file,
                     "finish_sampled_submit_cpu_us_per_guest_frame",
                     perf->finish, ARRAY_SIZE(perf->finish),
                     offsetof(PGRAPHVkWaitStats, submit_cpu_us));
    write_stat_array(perf->file, "fence_wait_count_per_guest_frame",
                     perf->finish, ARRAY_SIZE(perf->finish),
                     offsetof(PGRAPHVkWaitStats, wait_count));
    write_stat_array(perf->file, "finish_sampled_wait_us_per_guest_frame",
                     perf->finish,
                     ARRAY_SIZE(perf->finish),
                     offsetof(PGRAPHVkWaitStats, wait_us));
    write_stat_array(perf->file, "single_time_submit_count_per_guest_frame",
                     perf->single_time, ARRAY_SIZE(perf->single_time),
                     offsetof(PGRAPHVkWaitStats, submit_count));
    write_stat_array(perf->file,
                     "single_time_timed_submit_count_per_guest_frame",
                     perf->single_time, ARRAY_SIZE(perf->single_time),
                     offsetof(PGRAPHVkWaitStats, timed_submit_count));
    write_stat_array(perf->file,
                     "single_time_sampled_submit_cpu_us_per_guest_frame",
                     perf->single_time, ARRAY_SIZE(perf->single_time),
                     offsetof(PGRAPHVkWaitStats, submit_cpu_us));
    write_stat_array(perf->file, "queue_wait_idle_count_per_guest_frame",
                     perf->single_time, ARRAY_SIZE(perf->single_time),
                     offsetof(PGRAPHVkWaitStats, wait_count));
    write_stat_array(perf->file,
                     "single_time_sampled_wait_us_per_guest_frame",
                     perf->single_time, ARRAY_SIZE(perf->single_time),
                     offsetof(PGRAPHVkWaitStats, wait_us));
    write_cpu_stat_array(perf->file, "cpu_region_calls_per_guest_frame",
                         perf->cpu_regions,
                         offsetof(PGRAPHVkCpuStats, call_count));
    write_cpu_stat_array(perf->file, "cpu_region_us_per_guest_frame",
                         perf->cpu_regions,
                         offsetof(PGRAPHVkCpuStats, cpu_us));
    fprintf(perf->file,
            ",\"vk_queue_submit_calls_per_guest_frame\":%" PRIu64
            ",\"vk_submit_infos_per_guest_frame\":%" PRIu64
            ",\"command_buffers_per_guest_frame\":%" PRIu64
            ",\"staged_bytes_per_guest_frame\":%" PRIu64
            ",\"vertex_staged_bytes_per_guest_frame\":%" PRIu64
            ",\"vertex_staging_copies_per_guest_frame\":%" PRIu64
            ",\"vertex_direct_bytes_per_guest_frame\":%" PRIu64
            ",\"vertex_direct_copies_per_guest_frame\":%" PRIu64
            ",\"vertex_version_draws_per_guest_frame\":%" PRIu64
            ",\"vertex_version_bytes_per_guest_frame\":%" PRIu64
            ",\"vertex_version_selected_ranges_per_guest_frame\":%" PRIu64
            ",\"vertex_staging_capacity_bytes\":%zu"
            ",\"vertex_staging_capacity_growths_per_guest_frame\":%" PRIu64
            ",\"vertex_staging_fallback_finishes_per_guest_frame\":%" PRIu64
            ",\"native_bc_uploads_per_guest_frame\":%" PRIu64
            ",\"native_bc_source_bytes_per_guest_frame\":%" PRIu64
            ",\"native_bc_staged_bytes_per_guest_frame\":%" PRIu64
            ",\"native_bc_prepare_cpu_us_per_guest_frame\":%" PRIu64
            ",\"decoded_bc_uploads_per_guest_frame\":%" PRIu64
            ",\"decoded_bc_source_bytes_per_guest_frame\":%" PRIu64
            ",\"decoded_bc_staged_bytes_per_guest_frame\":%" PRIu64
            ",\"decoded_bc_prepare_cpu_us_per_guest_frame\":%" PRIu64
            ",\"staged_bytes_per_submit\":%.3f"
            ",\"submit_infos_per_submit\":%.3f"
            ",\"command_buffers_per_submit\":%.3f"
            ",\"in_flight_submission_count\":%" PRIu64
            ",\"peak_in_flight_submission_count\":%" PRIu64
            ",\"oldest_in_flight_serial\":%" PRIu64
            ",\"newest_submitted_serial\":%" PRIu64
            ",\"retirement_queue_objects\":%" PRIu64
            ",\"retirement_queue_bytes\":%" PRIu64
            ",\"report_queue_depth\":%" PRIu32
            ",\"report_entries_enqueued_per_guest_frame\":%" PRIu64
            ",\"report_clears_enqueued_per_guest_frame\":%" PRIu64
            ",\"report_max_queue_depth_per_guest_frame\":%" PRIu64
            ",\"report_stalled_finish_calls_per_guest_frame\":%" PRIu64
            ",\"report_retirements_per_guest_frame\":%" PRIu64
            ",\"report_write_attempts_per_guest_frame\":%" PRIu64
            ",\"report_query_result_calls_per_guest_frame\":%" PRIu64
            ",\"report_query_results_waited_per_guest_frame\":%" PRIu64
            ",\"report_query_result_wait_us_per_guest_frame\":%" PRIu64
            ",\"report_cpu_only_retirements_per_guest_frame\":%" PRIu64
            ",\"report_enqueue_to_retire_frames_total_per_guest_frame\":%" PRIu64
            ",\"report_enqueue_to_retire_frames_max_per_guest_frame\":%" PRIu64
            ",\"framebuffer_acquire_calls_total\":%" PRIu64
            ",\"valid_sync_requests_total\":%" PRIu64
            ",\"host_copy_uploads_total\":%" PRIu64
            ",\"host_copy_upload_skips_total\":%" PRIu64
            ",\"host_copy_uploaded_bytes_total\":%" PRIu64,
            submit_count, perf->submit_info_count, perf->command_buffer_count,
            perf->staged_bytes, perf->vertex_staged_bytes,
            perf->vertex_staging_copy_count,
            perf->vertex_direct_bytes, perf->vertex_direct_copy_count,
            perf->vertex_version_draw_count, perf->vertex_version_bytes,
            perf->vertex_version_selected_ranges,
            r->storage_buffers[BUFFER_VERTEX_RAM_STAGING].buffer_size,
            perf->vertex_staging_capacity_growth_count,
            perf->vertex_staging_fallback_finish_count,
            perf->native_bc_upload_count, perf->native_bc_source_bytes,
            perf->native_bc_staged_bytes, perf->native_bc_prepare_cpu_us,
            perf->decoded_bc_upload_count, perf->decoded_bc_source_bytes,
            perf->decoded_bc_staged_bytes, perf->decoded_bc_prepare_cpu_us,
            staged_bytes_per_submit,
            submit_infos_per_submit, command_buffers_per_submit,
            perf->in_flight_submission_count,
            perf->peak_in_flight_submission_count,
            perf->oldest_in_flight_serial, perf->newest_submitted_serial,
            perf->retirement_queue_objects, perf->retirement_queue_bytes,
            r->report_queue_depth, perf->report_entries_enqueued,
            perf->report_clears_enqueued, perf->report_max_queue_depth,
            perf->report_stalled_finish_calls, perf->report_retirements,
            perf->report_write_attempts, perf->report_query_result_calls,
            perf->report_query_results_waited,
            perf->report_query_result_wait_us,
            perf->report_cpu_only_retirements,
            perf->report_enqueue_to_retire_frames_total,
            perf->report_enqueue_to_retire_frames_max,
            qatomic_read_u64(&perf->framebuffer_acquire_calls_total),
            qatomic_read_u64(&perf->valid_sync_requests_total),
            qatomic_read_u64(&perf->host_copy_uploads_total),
            qatomic_read_u64(&perf->host_copy_upload_skips_total),
            qatomic_read_u64(&perf->host_copy_uploaded_bytes_total));

    fprintf(perf->file,
            ",\"descriptor_update_calls_per_guest_frame\":%" PRIu64
            ",\"descriptor_reuse_returns_per_guest_frame\":%" PRIu64
            ",\"descriptor_set_writes_per_guest_frame\":%" PRIu64
            ",\"descriptor_control_only_reuses_per_guest_frame\":%" PRIu64
            ",\"descriptor_texture_change_requests_per_guest_frame\":%" PRIu64
            ",\"descriptor_force_reupload_requests_per_guest_frame\":%" PRIu64
            ",\"descriptor_uniform_write_requests_per_guest_frame\":%" PRIu64
            ",\"descriptor_capacity_requests_per_guest_frame\":%" PRIu64
            ",\"uniform_capacity_requests_per_guest_frame\":%" PRIu64
            ",\"uniform_vsh_writes_per_guest_frame\":%" PRIu64
            ",\"uniform_psh_writes_per_guest_frame\":%" PRIu64
            ",\"surface_upload_attempts_per_guest_frame\":%" PRIu64
            ",\"surface_upload_color_attempts_per_guest_frame\":%" PRIu64
            ",\"surface_upload_depth_attempts_per_guest_frame\":%" PRIu64
            ",\"surface_upload_force_attempts_per_guest_frame\":%" PRIu64
            ",\"surface_upload_requested_bytes_per_guest_frame\":%" PRIu64
            ",\"surface_upload_new_causes_per_guest_frame\":%" PRIu64
            ",\"surface_upload_guest_write_causes_per_guest_frame\":%" PRIu64
            ",\"surface_upload_dirty_memory_causes_per_guest_frame\":%" PRIu64
            ",\"surface_upload_overlap_guest_write_causes_per_guest_frame\":%" PRIu64
            "}\n",
            perf->descriptor_update_calls,
            perf->descriptor_reuse_returns,
            perf->descriptor_set_writes,
            perf->descriptor_control_only_reuses,
            perf->descriptor_texture_change_requests,
            perf->descriptor_force_reupload_requests,
            perf->descriptor_uniform_write_requests,
            perf->descriptor_capacity_requests,
            perf->uniform_capacity_requests,
            perf->uniform_stage_writes[PGRAPH_UNIFORM_STAGE_VSH],
            perf->uniform_stage_writes[PGRAPH_UNIFORM_STAGE_PSH],
            perf->surface_upload_attempts,
            perf->surface_upload_color_attempts,
            perf->surface_upload_depth_attempts,
            perf->surface_upload_force_attempts,
            perf->surface_upload_requested_bytes,
            perf->surface_upload_new_causes,
            perf->surface_upload_guest_write_causes,
            perf->surface_upload_dirty_memory_causes,
            perf->surface_upload_overlap_guest_write_causes);

    if (now - perf->last_flush_us >= G_USEC_PER_SEC) {
        fflush(perf->file);
        perf->last_flush_us = now;
    }

    memset(perf->finish, 0, sizeof(perf->finish));
    memset(perf->single_time, 0, sizeof(perf->single_time));
    memset(perf->cpu_regions, 0, sizeof(perf->cpu_regions));
    perf->submit_info_count = 0;
    perf->command_buffer_count = 0;
    perf->staged_bytes = 0;
    perf->vertex_staged_bytes = 0;
    perf->vertex_staging_copy_count = 0;
    perf->vertex_direct_bytes = 0;
    perf->vertex_direct_copy_count = 0;
    perf->vertex_version_draw_count = 0;
    perf->vertex_version_bytes = 0;
    perf->vertex_version_selected_ranges = 0;
    perf->vertex_staging_capacity_growth_count = 0;
    perf->vertex_staging_fallback_finish_count = 0;
    perf->native_bc_upload_count = 0;
    perf->native_bc_source_bytes = 0;
    perf->native_bc_staged_bytes = 0;
    perf->native_bc_prepare_cpu_us = 0;
    perf->decoded_bc_upload_count = 0;
    perf->decoded_bc_source_bytes = 0;
    perf->decoded_bc_staged_bytes = 0;
    perf->decoded_bc_prepare_cpu_us = 0;
    perf->descriptor_update_calls = 0;
    perf->descriptor_reuse_returns = 0;
    perf->descriptor_set_writes = 0;
    perf->descriptor_control_only_reuses = 0;
    perf->descriptor_texture_change_requests = 0;
    perf->descriptor_force_reupload_requests = 0;
    perf->descriptor_uniform_write_requests = 0;
    perf->descriptor_capacity_requests = 0;
    perf->uniform_capacity_requests = 0;
    memset(perf->uniform_stage_writes, 0,
           sizeof(perf->uniform_stage_writes));
    perf->surface_upload_attempts = 0;
    perf->surface_upload_color_attempts = 0;
    perf->surface_upload_depth_attempts = 0;
    perf->surface_upload_force_attempts = 0;
    perf->surface_upload_requested_bytes = 0;
    perf->surface_upload_new_causes = 0;
    perf->surface_upload_guest_write_causes = 0;
    perf->surface_upload_dirty_memory_causes = 0;
    perf->surface_upload_overlap_guest_write_causes = 0;
    perf->peak_in_flight_submission_count =
        perf->in_flight_submission_count;
    perf->oldest_in_flight_serial = 0;
    perf->retirement_queue_objects = 0;
    perf->retirement_queue_bytes = 0;
    perf->report_entries_enqueued = 0;
    perf->report_clears_enqueued = 0;
    perf->report_max_queue_depth = r->report_queue_depth;
    perf->report_stalled_finish_calls = 0;
    perf->report_retirements = 0;
    perf->report_write_attempts = 0;
    perf->report_query_result_calls = 0;
    perf->report_query_results_waited = 0;
    perf->report_query_result_wait_us = 0;
    perf->report_cpu_only_retirements = 0;
    perf->report_enqueue_to_retire_frames_total = 0;
    perf->report_enqueue_to_retire_frames_max = 0;
}
