/*
 * NV2A Vulkan pending-report integration tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/vk/renderer.h"
#include "xbox-pgraph-report-test-support.h"

#define VRAM_SIZE 64
#define RAMIN_SIZE 64
#define REPORT_SIZE 16
#define CANARY 0xa5

typedef enum BoundaryEvent {
    BOUNDARY_END_QUERY,
    BOUNDARY_END_MAIN,
    BOUNDARY_BEGIN_AUX,
    BOUNDARY_FLUSH,
    BOUNDARY_BARRIER,
    BOUNDARY_END_AUX,
    BOUNDARY_RESET_FENCE,
    BOUNDARY_SUBMIT,
    BOUNDARY_WAIT,
    BOUNDARY_QUERY_RESULTS,
} BoundaryEvent;

typedef struct BoundaryTrace {
    BoundaryEvent events[16];
    size_t event_count;
    const uint8_t *watched_report;
    bool wait_saw_unpublished_report;
    bool query_saw_unpublished_report;
    bool valid;
} BoundaryTrace;

typedef struct ReportFixture {
    NV2AState d;
    PGRAPHVkState renderer;
    MemoryRegion vram_region;
    uint8_t vram[VRAM_SIZE];
    uint8_t ramin[RAMIN_SIZE];
} ReportFixture;

static void fixture_free(ReportFixture *fixture)
{
    QueryReport *report;

    while ((report = QSIMPLEQ_FIRST(&fixture->renderer.report_queue))) {
        QSIMPLEQ_REMOVE_HEAD(&fixture->renderer.report_queue, entry);
        g_free(report);
    }
    g_free(fixture);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ReportFixture, fixture_free)

int nv2a_vk_dgroup_indent;
bool nv2a_vk_text_debug_enabled;
unsigned int xemu_tweaks_active;

void pgraph_vk_text_debug_printf(const char *format, ...)
{
    (void)format;
}

PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
PFN_vkCmdEndQuery vkCmdEndQuery;
PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
PFN_vkDestroyPipeline vkDestroyPipeline;
PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
PFN_vkDestroyShaderModule vkDestroyShaderModule;
PFN_vkEndCommandBuffer vkEndCommandBuffer;
PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
PFN_vkQueueSubmit vkQueueSubmit;
PFN_vkResetFences vkResetFences;
PFN_vkWaitForFences vkWaitForFences;

static BoundaryTrace boundary_trace;
static VkCommandBuffer main_command_buffer = (VkCommandBuffer)(uintptr_t)1;
static VkCommandBuffer aux_command_buffer = (VkCommandBuffer)(uintptr_t)2;

static bool buffer_is_value(const uint8_t *buffer, size_t size, uint8_t value)
{
    for (size_t i = 0; i < size; i++) {
        if (buffer[i] != value) {
            return false;
        }
    }
    return true;
}

static bool report_matches(const uint8_t *report, uint32_t result)
{
    static const uint8_t timestamp[] = {
        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    };
    uint8_t expected[REPORT_SIZE] = { 0 };

    memcpy(expected, timestamp, sizeof(timestamp));
    stl_le_p(expected + 8, result);
    return !memcmp(report, expected, sizeof(expected));
}

static void record_boundary(BoundaryEvent event)
{
    if (boundary_trace.event_count >= ARRAY_SIZE(boundary_trace.events)) {
        boundary_trace.valid = false;
        return;
    }
    boundary_trace.events[boundary_trace.event_count++] = event;
}

static VKAPI_ATTR VkResult VKAPI_CALL test_vk_begin_command_buffer(
    VkCommandBuffer command_buffer, const VkCommandBufferBeginInfo *begin_info)
{
    boundary_trace.valid &= command_buffer == aux_command_buffer;
    boundary_trace.valid &= begin_info != NULL;
    record_boundary(BOUNDARY_BEGIN_AUX);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
test_vk_end_command_buffer(VkCommandBuffer command_buffer)
{
    if (command_buffer == main_command_buffer) {
        record_boundary(BOUNDARY_END_MAIN);
    } else if (command_buffer == aux_command_buffer) {
        record_boundary(BOUNDARY_END_AUX);
    } else {
        boundary_trace.valid = false;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL test_vk_cmd_pipeline_barrier(
    VkCommandBuffer command_buffer, VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask, VkDependencyFlags dependency_flags,
    uint32_t memory_barrier_count, const VkMemoryBarrier *memory_barriers,
    uint32_t buffer_barrier_count, const VkBufferMemoryBarrier *buffer_barriers,
    uint32_t image_barrier_count, const VkImageMemoryBarrier *image_barriers)
{
    boundary_trace.valid &= command_buffer == aux_command_buffer;
    boundary_trace.valid &= src_stage_mask == VK_PIPELINE_STAGE_HOST_BIT;
    boundary_trace.valid &=
        dst_stage_mask == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    boundary_trace.valid &= dependency_flags == 0;
    boundary_trace.valid &= memory_barrier_count == 0;
    boundary_trace.valid &= memory_barriers == NULL;
    boundary_trace.valid &= buffer_barrier_count == 1;
    boundary_trace.valid &= buffer_barriers != NULL;
    boundary_trace.valid &= image_barrier_count == 0;
    boundary_trace.valid &= image_barriers == NULL;
    record_boundary(BOUNDARY_BARRIER);
}

static VKAPI_ATTR VkResult VKAPI_CALL test_vk_reset_fences(
    VkDevice device, uint32_t fence_count, const VkFence *fences)
{
    (void)device;
    boundary_trace.valid &= fence_count == 1 && fences != NULL;
    record_boundary(BOUNDARY_RESET_FENCE);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
test_vk_queue_submit(VkQueue queue, uint32_t submit_count,
                     const VkSubmitInfo *submits, VkFence fence)
{
    (void)queue;
    (void)fence;
    boundary_trace.valid &= submit_count == 2 && submits != NULL;
    boundary_trace.valid &= submits[0].commandBufferCount == 1;
    boundary_trace.valid &= submits[1].commandBufferCount == 1;
    record_boundary(BOUNDARY_SUBMIT);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL test_vk_wait_for_fences(
    VkDevice device, uint32_t fence_count, const VkFence *fences,
    VkBool32 wait_all, uint64_t timeout)
{
    (void)device;
    boundary_trace.valid &= fence_count == 1 && fences != NULL;
    boundary_trace.valid &= wait_all == VK_TRUE && timeout == UINT64_MAX;
    boundary_trace.wait_saw_unpublished_report =
        buffer_is_value(boundary_trace.watched_report, REPORT_SIZE, CANARY);
    record_boundary(BOUNDARY_WAIT);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL test_vk_noop_cmd_copy_buffer(
    VkCommandBuffer command_buffer, VkBuffer src, VkBuffer dst,
    uint32_t region_count, const VkBufferCopy *regions)
{
    (void)command_buffer;
    (void)src;
    (void)dst;
    (void)region_count;
    (void)regions;
    boundary_trace.valid = false;
}

static VKAPI_ATTR void VKAPI_CALL test_vk_cmd_end_query(
    VkCommandBuffer command_buffer, VkQueryPool pool, uint32_t query)
{
    boundary_trace.valid &= command_buffer == main_command_buffer;
    boundary_trace.valid &= pool == (VkQueryPool)(uintptr_t)8;
    boundary_trace.valid &= query == 0;
    record_boundary(BOUNDARY_END_QUERY);
}

static VKAPI_ATTR void VKAPI_CALL
test_vk_noop_cmd_end_render_pass(VkCommandBuffer command_buffer)
{
    (void)command_buffer;
    boundary_trace.valid = false;
}

static VKAPI_ATTR void VKAPI_CALL
test_vk_noop_destroy_framebuffer(VkDevice device, VkFramebuffer framebuffer,
                                 const VkAllocationCallbacks *allocator)
{
    (void)device;
    (void)framebuffer;
    (void)allocator;
    boundary_trace.valid = false;
}

static VKAPI_ATTR VkResult VKAPI_CALL test_vk_get_query_pool_results(
    VkDevice device, VkQueryPool query_pool, uint32_t first_query,
    uint32_t query_count, size_t data_size, void *data, VkDeviceSize stride,
    VkQueryResultFlags flags)
{
    (void)device;
    boundary_trace.valid &= query_pool == (VkQueryPool)(uintptr_t)8;
    boundary_trace.valid &= first_query == 0 && query_count == 1;
    boundary_trace.valid &= data_size == sizeof(uint64_t);
    boundary_trace.valid &= data != NULL && stride == sizeof(uint64_t);
    boundary_trace.valid &=
        flags == (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    boundary_trace.query_saw_unpublished_report =
        buffer_is_value(boundary_trace.watched_report, REPORT_SIZE, CANARY);
    *(uint64_t *)data = 11;
    record_boundary(BOUNDARY_QUERY_RESULTS);
    return VK_SUCCESS;
}

VkResult vmaFlushAllocation(VmaAllocator allocator, VmaAllocation allocation,
                            VkDeviceSize offset, VkDeviceSize size)
{
    (void)allocator;
    (void)allocation;
    boundary_trace.valid &= offset == 0 && size == VK_WHOLE_SIZE;
    record_boundary(BOUNDARY_FLUSH);
    return VK_SUCCESS;
}

void vmaSetCurrentFrameIndex(VmaAllocator allocator, uint32_t frame_index)
{
    (void)allocator;
    (void)frame_index;
    boundary_trace.valid = false;
}

void pgraph_vk_check_memory_budget(PGRAPHState *pg)
{
    /*
     * The fixture's first submit cannot reach the periodic budget branch.
     * Keep this as a failing link-closure guard rather than a success stub.
     */
    (void)pg;
    boundary_trace.valid = false;
}

void pgraph_vk_fallback_family_note_pipeline_ready(
    PGRAPHVkState *r, const PipelineKey *specialized_key)
{
    /* No hybrid pipeline publication is valid in this report-only fixture. */
    (void)r;
    (void)specialized_key;
    boundary_trace.valid = false;
}

static void install_vulkan_boundaries(void)
{
    vkBeginCommandBuffer = test_vk_begin_command_buffer;
    vkCmdCopyBuffer = test_vk_noop_cmd_copy_buffer;
    vkCmdEndQuery = test_vk_cmd_end_query;
    vkCmdEndRenderPass = test_vk_noop_cmd_end_render_pass;
    vkCmdPipelineBarrier = test_vk_cmd_pipeline_barrier;
    vkDestroyFramebuffer = test_vk_noop_destroy_framebuffer;
    vkEndCommandBuffer = test_vk_end_command_buffer;
    vkGetQueryPoolResults = test_vk_get_query_pool_results;
    vkQueueSubmit = test_vk_queue_submit;
    vkResetFences = test_vk_reset_fences;
    vkWaitForFences = test_vk_wait_for_fences;
}

static void write_dma_descriptor(uint8_t *ramin, uint32_t base, uint32_t limit)
{
    uint32_t flags = 0;

    SET_MASK(flags, NV_DMA_ADJUST, base & 0xfff);
    stl_le_p(ramin, flags);
    stl_le_p(ramin + 4, limit);
    stl_le_p(ramin + 8, base & NV_DMA_ADDRESS);
}

static uint32_t report_parameter(uint32_t offset)
{
    uint32_t parameter = 0;

    SET_MASK(parameter, NV097_GET_REPORT_TYPE,
             NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);
    SET_MASK(parameter, NV097_GET_REPORT_OFFSET, offset);
    return parameter;
}

static void fixture_init(ReportFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    report_test_memory_region_set_size(&fixture->d.ramin,
                                       sizeof(fixture->ramin));
    report_test_memory_region_set_size(&fixture->vram_region,
                                       sizeof(fixture->vram));
    fixture->d.ramin_ptr = fixture->ramin;
    fixture->d.vram = &fixture->vram_region;
    fixture->d.vram_ptr = fixture->vram;
    fixture->d.pgraph.vk_renderer_state = &fixture->renderer;
    fixture->d.pgraph.surface_scale_factor = 1;
    fixture->renderer.perf.enabled = true;
    fixture->renderer.command_buffer = main_command_buffer;
    fixture->renderer.aux_command_buffer = aux_command_buffer;
    fixture->renderer.command_buffer_semaphore = (VkSemaphore)(uintptr_t)3;
    fixture->renderer.command_buffer_fence = (VkFence)(uintptr_t)4;
    fixture->renderer.queue = (VkQueue)(uintptr_t)5;
    fixture->renderer.device = (VkDevice)(uintptr_t)6;
    fixture->renderer.allocator = (VmaAllocator)(uintptr_t)7;
    fixture->renderer.query_pool = (VkQueryPool)(uintptr_t)8;
    QSIMPLEQ_INIT(&fixture->renderer.report_queue);
    memset(fixture->vram, CANARY, sizeof(fixture->vram));
    memset(&boundary_trace, 0, sizeof(boundary_trace));
    boundary_trace.valid = true;
    nv2a_vk_dgroup_indent = 0;
}

static bool queue_is_empty(const ReportFixture *fixture)
{
    return QSIMPLEQ_EMPTY(&fixture->renderer.report_queue);
}

static bool no_command_buffer_boundaries_called(void)
{
    return boundary_trace.valid && boundary_trace.event_count == 0;
}

static bool test_idle_no_command_buffer_publishes_accumulated_count(void)
{
    g_autoptr(ReportFixture) fixture = g_new0(ReportFixture, 1);

    fixture_init(fixture);
    write_dma_descriptor(fixture->ramin, 0, REPORT_SIZE - 1);
    fixture->d.pgraph.dma_report = 0;
    fixture->renderer.zpass_pixel_count_result = UINT32_C(0x2345);
    fixture->renderer.compute.descriptor_set_index = 3;
    pgraph_vk_get_report(&fixture->d, report_parameter(0));

    pgraph_vk_process_pending_reports(&fixture->d);
    return report_matches(fixture->vram, UINT32_C(0x2345)) &&
           buffer_is_value(fixture->vram + REPORT_SIZE, VRAM_SIZE - REPORT_SIZE,
                           CANARY) &&
           queue_is_empty(fixture) && !fixture->renderer.in_command_buffer &&
           fixture->renderer.num_queries_in_flight == 0 &&
           fixture->renderer.compute.descriptor_set_index == 0 &&
           fixture->renderer.perf.finish[VK_FINISH_REASON_STALLED].call_count ==
               1 &&
           no_command_buffer_boundaries_called();
}

static bool test_clear_then_report_publishes_zero(void)
{
    g_autoptr(ReportFixture) fixture = g_new0(ReportFixture, 1);

    fixture_init(fixture);
    write_dma_descriptor(fixture->ramin, 0, REPORT_SIZE - 1);
    fixture->d.pgraph.dma_report = 0;
    fixture->renderer.zpass_pixel_count_result = UINT32_C(0x3456);
    pgraph_vk_clear_report_value(&fixture->d);
    pgraph_vk_get_report(&fixture->d, report_parameter(0));

    pgraph_vk_process_pending_reports(&fixture->d);
    return report_matches(fixture->vram, 0) && queue_is_empty(fixture) &&
           fixture->renderer.zpass_pixel_count_result == 0 &&
           fixture->renderer.perf.finish[VK_FINISH_REASON_STALLED].call_count ==
               1 &&
           no_command_buffer_boundaries_called();
}

static bool test_rejection_retires_before_dma_switch_and_valid_report(void)
{
    g_autoptr(ReportFixture) fixture = g_new0(ReportFixture, 1);
    uint8_t after_valid[VRAM_SIZE];
    uint64_t finish_calls;

    fixture_init(fixture);
    write_dma_descriptor(fixture->ramin, 0, REPORT_SIZE - 2);
    write_dma_descriptor(fixture->ramin + 12, 32, REPORT_SIZE - 1);

    fixture->d.pgraph.dma_report = 0;
    fixture->renderer.zpass_pixel_count_result = 9;
    pgraph_vk_get_report(&fixture->d, report_parameter(0));
    pgraph_vk_process_pending_reports(&fixture->d);
    if (!buffer_is_value(fixture->vram, sizeof(fixture->vram), CANARY) ||
        !queue_is_empty(fixture) ||
        fixture->renderer.perf.report_retirements != 1 ||
        fixture->renderer.perf.report_write_attempts != 1) {
        return false;
    }

    /* SET_CONTEXT_DMA_REPORT drains before assigning the new DMA handle. */
    fixture->d.pgraph.dma_report = 12;
    pgraph_vk_get_report(&fixture->d, report_parameter(0));
    pgraph_vk_process_pending_reports(&fixture->d);
    if (!buffer_is_value(fixture->vram, 32, CANARY) ||
        !report_matches(fixture->vram + 32, 9) ||
        !buffer_is_value(fixture->vram + 48, 16, CANARY) ||
        !queue_is_empty(fixture)) {
        return false;
    }

    memcpy(after_valid, fixture->vram, sizeof(after_valid));
    finish_calls =
        fixture->renderer.perf.finish[VK_FINISH_REASON_STALLED].call_count;
    pgraph_vk_process_pending_reports(&fixture->d);
    return finish_calls == 2 &&
           fixture->renderer.perf.finish[VK_FINISH_REASON_STALLED].call_count ==
               finish_calls &&
           fixture->renderer.perf.report_retirements == 2 &&
           fixture->renderer.perf.report_write_attempts == 2 &&
           !memcmp(after_valid, fixture->vram, sizeof(after_valid)) &&
           no_command_buffer_boundaries_called();
}

static bool test_nonidle_fifo_defers_queue(void)
{
    g_autoptr(ReportFixture) fixture = g_new0(ReportFixture, 1);

    fixture_init(fixture);
    write_dma_descriptor(fixture->ramin, 0, REPORT_SIZE - 1);
    fixture->d.pgraph.dma_report = 0;
    fixture->d.pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = 4;
    pgraph_vk_get_report(&fixture->d, report_parameter(0));

    pgraph_vk_process_pending_reports(&fixture->d);
    if (queue_is_empty(fixture) ||
        !buffer_is_value(fixture->vram, sizeof(fixture->vram), CANARY) ||
        fixture->renderer.perf.finish[VK_FINISH_REASON_STALLED].call_count !=
            0) {
        return false;
    }

    fixture->d.pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = 0;
    pgraph_vk_process_pending_reports(&fixture->d);
    return queue_is_empty(fixture) && report_matches(fixture->vram, 0) &&
           fixture->renderer.perf.finish[VK_FINISH_REASON_STALLED].call_count ==
               1 &&
           no_command_buffer_boundaries_called();
}

static bool test_descriptor_words_are_decoded_at_retirement(void)
{
    g_autoptr(ReportFixture) fixture = g_new0(ReportFixture, 1);

    fixture_init(fixture);
    write_dma_descriptor(fixture->ramin, 0, REPORT_SIZE - 1);
    fixture->d.pgraph.dma_report = 0;
    pgraph_vk_get_report(&fixture->d, report_parameter(0));

    write_dma_descriptor(fixture->ramin, 16, REPORT_SIZE - 1);
    pgraph_vk_process_pending_reports(&fixture->d);
    return buffer_is_value(fixture->vram, 16, CANARY) &&
           report_matches(fixture->vram + 16, 0) &&
           buffer_is_value(fixture->vram + 32, 32, CANARY) &&
           queue_is_empty(fixture) && no_command_buffer_boundaries_called();
}

static bool test_active_command_buffer_waits_before_publication(void)
{
    static const BoundaryEvent expected[] = {
        BOUNDARY_END_QUERY,     BOUNDARY_END_MAIN, BOUNDARY_BEGIN_AUX,
        BOUNDARY_FLUSH,         BOUNDARY_BARRIER,  BOUNDARY_END_AUX,
        BOUNDARY_RESET_FENCE,   BOUNDARY_SUBMIT,   BOUNDARY_WAIT,
        BOUNDARY_QUERY_RESULTS,
    };
    g_autoptr(ReportFixture) fixture = g_new0(ReportFixture, 1);
    const PGRAPHVkWaitStats *finish_stats;

    fixture_init(fixture);
    install_vulkan_boundaries();
    write_dma_descriptor(fixture->ramin, 0, REPORT_SIZE - 1);
    fixture->d.pgraph.dma_report = 0;
    fixture->renderer.in_command_buffer = true;
    fixture->renderer.query_in_flight = true;
    fixture->renderer.num_queries_in_flight = 1;
    fixture->renderer.zpass_pixel_count_result = 7;
    boundary_trace.watched_report = fixture->vram;
    pgraph_vk_get_report(&fixture->d, report_parameter(0));

    pgraph_vk_process_pending_reports(&fixture->d);
    finish_stats = &fixture->renderer.perf.finish[VK_FINISH_REASON_STALLED];
    return boundary_trace.valid && boundary_trace.wait_saw_unpublished_report &&
           boundary_trace.query_saw_unpublished_report &&
           boundary_trace.event_count == ARRAY_SIZE(expected) &&
           !memcmp(boundary_trace.events, expected, sizeof(expected)) &&
           report_matches(fixture->vram, 18) && queue_is_empty(fixture) &&
           !fixture->renderer.in_command_buffer &&
           !fixture->renderer.in_aux_command_buffer &&
           !fixture->renderer.query_in_flight &&
           fixture->renderer.num_queries_in_flight == 0 &&
           fixture->renderer.submit_count == 1 &&
           fixture->renderer.report_queue_depth == 0 &&
           fixture->renderer.perf.report_stalled_finish_calls == 1 &&
           fixture->renderer.perf.report_retirements == 1 &&
           fixture->renderer.perf.report_write_attempts == 1 &&
           fixture->renderer.perf.report_query_result_calls == 1 &&
           fixture->renderer.perf.report_query_results_waited == 1 &&
           fixture->renderer.perf.report_cpu_only_retirements == 0 &&
           finish_stats->call_count == 1 && finish_stats->submit_count == 1 &&
           finish_stats->wait_count == 1;
}

static bool test_report_lifecycle_telemetry_tracks_queue_and_retirement(void)
{
    g_autoptr(ReportFixture) fixture = g_new0(ReportFixture, 1);
    QueryReport *clear_report;
    QueryReport *value_report;

    fixture_init(fixture);
    write_dma_descriptor(fixture->ramin, 0, REPORT_SIZE - 1);
    fixture->d.pgraph.dma_report = 0;
    fixture->renderer.perf.frame = 7;
    fixture->renderer.perf.submission_serial = 3;

    pgraph_vk_clear_report_value(&fixture->d);
    pgraph_vk_get_report(&fixture->d, report_parameter(0));

    clear_report = QSIMPLEQ_FIRST(&fixture->renderer.report_queue);
    value_report = QSIMPLEQ_NEXT(clear_report, entry);
    if (fixture->renderer.report_queue_depth != 2 ||
        fixture->renderer.perf.report_entries_enqueued != 2 ||
        fixture->renderer.perf.report_clears_enqueued != 1 ||
        fixture->renderer.perf.report_max_queue_depth != 2 ||
        clear_report->enqueue_frame != 7 ||
        value_report->enqueue_frame != 7) {
        return false;
    }

    fixture->renderer.perf.frame = 9;
    pgraph_vk_process_pending_reports(&fixture->d);

    return queue_is_empty(fixture) &&
           fixture->renderer.report_queue_depth == 0 &&
           fixture->renderer.perf.report_stalled_finish_calls == 1 &&
           fixture->renderer.perf.report_write_attempts == 1 &&
           fixture->renderer.perf.report_cpu_only_retirements == 2 &&
           fixture->renderer.perf.report_query_result_calls == 0 &&
           fixture->renderer.perf.report_query_results_waited == 0 &&
           fixture->renderer.perf.report_enqueue_to_retire_frames_total == 4 &&
           fixture->renderer.perf.report_enqueue_to_retire_frames_max == 2;
}

static bool test_disabled_telemetry_preserves_report_behavior(void)
{
    g_autoptr(ReportFixture) fixture = g_new0(ReportFixture, 1);

    fixture_init(fixture);
    fixture->renderer.perf.enabled = false;
    write_dma_descriptor(fixture->ramin, 0, REPORT_SIZE - 1);
    fixture->d.pgraph.dma_report = 0;
    fixture->renderer.zpass_pixel_count_result = UINT32_C(0x4567);

    pgraph_vk_get_report(&fixture->d, report_parameter(0));
    pgraph_vk_process_pending_reports(&fixture->d);

    return report_matches(fixture->vram, UINT32_C(0x4567)) &&
           queue_is_empty(fixture) &&
           fixture->renderer.perf.report_entries_enqueued == 0 &&
           fixture->renderer.perf.report_retirements == 0 &&
           fixture->renderer.perf.report_write_attempts == 0 &&
           no_command_buffer_boundaries_called();
}

int main(void)
{
    bool idle = test_idle_no_command_buffer_publishes_accumulated_count();
    bool clear = test_clear_then_report_publishes_zero();
    bool reject = test_rejection_retires_before_dma_switch_and_valid_report();
    bool nonidle = test_nonidle_fifo_defers_queue();
    bool delayed = test_descriptor_words_are_decoded_at_retirement();
    bool active = test_active_command_buffer_waits_before_publication();
    bool telemetry =
        test_report_lifecycle_telemetry_tracks_queue_and_retirement();
    bool disabled = test_disabled_telemetry_preserves_report_behavior();

    puts("TAP version 13");
    puts("1..8");
    printf("%s 1 - idle no-CB queue publishes accumulated count\n",
           idle ? "ok" : "not ok");
    printf("%s 2 - clear then report publishes zero\n",
           clear ? "ok" : "not ok");
    printf("%s 3 - rejection retires before DMA switch and valid report\n",
           reject ? "ok" : "not ok");
    printf("%s 4 - nonidle FIFO defers pending report queue\n",
           nonidle ? "ok" : "not ok");
    printf("%s 5 - descriptor words decode at retirement\n",
           delayed ? "ok" : "not ok");
    printf("%s 6 - active CB completion precedes publication\n",
           active ? "ok" : "not ok");
    printf("%s 7 - report lifecycle telemetry tracks queue and retirement\n",
           telemetry ? "ok" : "not ok");
    printf("%s 8 - disabled telemetry preserves report behavior\n",
           disabled ? "ok" : "not ok");
    return idle && clear && reject && nonidle && delayed && active &&
                   telemetry && disabled ?
               0 :
               1;
}
