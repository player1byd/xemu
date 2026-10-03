/*
 * Bounded worker for immutable Vulkan graphics-pipeline recipes
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "hw/xbox/nv2a/pgraph/vk/hybrid-pipeline-builder.h"

/* These limits cover the ordinary NV2A pipeline recipe. Reject a future
 * extension rather than retaining an unowned pointer in a queued job. */
#define MAX_STAGES 3
#define MAX_STAGE_NAME 64
#define MAX_VERTEX_BINDINGS 16
#define MAX_VERTEX_ATTRIBUTES 32
#define MAX_VIEWPORTS 4
#define MAX_COLOR_ATTACHMENTS 4
#define MAX_DYNAMIC_STATES 16
#define MAX_SAMPLE_MASK_WORDS 2

typedef struct PipelineRecipe {
    VkGraphicsPipelineCreateInfo info;
    VkPipelineShaderStageCreateInfo stages[MAX_STAGES];
    char stage_names[MAX_STAGES][MAX_STAGE_NAME];
    VkPipelineVertexInputStateCreateInfo vertex;
    VkVertexInputBindingDescription bindings[MAX_VERTEX_BINDINGS];
    VkVertexInputAttributeDescription attributes[MAX_VERTEX_ATTRIBUTES];
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkPipelineViewportStateCreateInfo viewport;
    VkViewport viewports[MAX_VIEWPORTS];
    VkRect2D scissors[MAX_VIEWPORTS];
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkSampleMask sample_masks[MAX_SAMPLE_MASK_WORDS];
    VkPipelineDepthStencilStateCreateInfo depth_stencil;
    VkPipelineColorBlendStateCreateInfo blend;
    VkPipelineColorBlendAttachmentState attachments[MAX_COLOR_ATTACHMENTS];
    VkPipelineDynamicStateCreateInfo dynamic;
    VkDynamicState dynamic_states[MAX_DYNAMIC_STATES];
} PipelineRecipe;

typedef struct PipelineJob {
    struct PipelineJob *next;
    PGRAPHVkHybridPipelineBuildRequest request;
    PGRAPHVkHybridPipelineBuildResult result;
    PipelineRecipe recipe;
} PipelineJob;

typedef struct PipelineBuilderState {
    QemuMutex lock;
    QemuCond work_ready;
    QemuThread worker;
    PGRAPHVkHybridPipelineBuilderConfig config;
    PipelineJob *pending_head;
    PipelineJob *pending_tail;
    PipelineJob *result_head;
    PipelineJob *result_tail;
    int result_available;
    PipelineJob *active;
    size_t outstanding;
    uint64_t min_generation;
    bool stopping;
    bool joined;
} PipelineBuilderState;

static PipelineJob *list_pop_highest_priority(PipelineJob **head,
                                              PipelineJob **tail)
{
    PipelineJob *job = *head;
    if (!job) {
        return NULL;
    }
    PipelineJob *best = job;
    PipelineJob *best_prev = NULL;
    PipelineJob *prev = job;
    for (PipelineJob *candidate = job->next; candidate;
         candidate = candidate->next) {
        if (candidate->request.priority > best->request.priority) {
            best = candidate;
            best_prev = prev;
        }
        prev = candidate;
    }
    if (best_prev) {
        best_prev->next = best->next;
    } else {
        *head = best->next;
    }
    if (*tail == best) {
        *tail = best_prev;
    }
    best->next = NULL;
    return best;
}

static PipelineJob *result_take_identity(PipelineBuilderState *state,
                                         uint64_t generation,
                                         uint64_t ticket)
{
    PipelineJob *previous = NULL;
    for (PipelineJob *job = state->result_head; job; job = job->next) {
        if (job->request.generation != generation ||
            job->request.ticket != ticket) {
            previous = job;
            continue;
        }
        if (previous) {
            previous->next = job->next;
        } else {
            state->result_head = job->next;
        }
        if (state->result_tail == job) {
            state->result_tail = previous;
        }
        job->next = NULL;
        return job;
    }
    return NULL;
}

static bool recipe_copy(PipelineRecipe *dst,
                        const VkGraphicsPipelineCreateInfo *src)
{
    if (!src || src->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
        src->pNext || !src->pStages || !src->stageCount ||
        src->stageCount > MAX_STAGES || src->pTessellationState ||
        !src->pVertexInputState || !src->pInputAssemblyState ||
        !src->pViewportState || !src->pRasterizationState ||
        !src->pMultisampleState || !src->pColorBlendState ||
        !src->pDynamicState || src->basePipelineHandle != VK_NULL_HANDLE) {
        return false;
    }

    dst->info = *src;
    dst->info.pStages = dst->stages;
    for (uint32_t i = 0; i < src->stageCount; i++) {
        const VkPipelineShaderStageCreateInfo *stage = &src->pStages[i];
        if (stage->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
            stage->pNext || stage->pSpecializationInfo || !stage->pName) {
            return false;
        }
        size_t len = strnlen(stage->pName, MAX_STAGE_NAME);
        if (len == MAX_STAGE_NAME) {
            return false;
        }
        dst->stages[i] = *stage;
        memcpy(dst->stage_names[i], stage->pName, len + 1);
        dst->stages[i].pName = dst->stage_names[i];
    }

    const VkPipelineVertexInputStateCreateInfo *vertex = src->pVertexInputState;
    if (vertex->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO ||
        vertex->pNext ||
        vertex->vertexBindingDescriptionCount > MAX_VERTEX_BINDINGS ||
        vertex->vertexAttributeDescriptionCount > MAX_VERTEX_ATTRIBUTES ||
        (vertex->vertexBindingDescriptionCount &&
         !vertex->pVertexBindingDescriptions) ||
        (vertex->vertexAttributeDescriptionCount &&
         !vertex->pVertexAttributeDescriptions)) {
        return false;
    }
    dst->vertex = *vertex;
    dst->info.pVertexInputState = &dst->vertex;
    if (vertex->vertexBindingDescriptionCount) {
        memcpy(dst->bindings, vertex->pVertexBindingDescriptions,
               vertex->vertexBindingDescriptionCount * sizeof(dst->bindings[0]));
        dst->vertex.pVertexBindingDescriptions = dst->bindings;
    } else {
        dst->vertex.pVertexBindingDescriptions = NULL;
    }
    if (vertex->vertexAttributeDescriptionCount) {
        memcpy(dst->attributes, vertex->pVertexAttributeDescriptions,
               vertex->vertexAttributeDescriptionCount * sizeof(dst->attributes[0]));
        dst->vertex.pVertexAttributeDescriptions = dst->attributes;
    } else {
        dst->vertex.pVertexAttributeDescriptions = NULL;
    }

    if (src->pInputAssemblyState->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
        src->pInputAssemblyState->pNext) {
        return false;
    }
    dst->assembly = *src->pInputAssemblyState;
    dst->info.pInputAssemblyState = &dst->assembly;

    const VkPipelineViewportStateCreateInfo *viewport = src->pViewportState;
    if (viewport->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO ||
        viewport->pNext || viewport->viewportCount > MAX_VIEWPORTS ||
        viewport->scissorCount > MAX_VIEWPORTS) {
        return false;
    }
    dst->viewport = *viewport;
    dst->info.pViewportState = &dst->viewport;
    if (viewport->pViewports) {
        memcpy(dst->viewports, viewport->pViewports,
               viewport->viewportCount * sizeof(dst->viewports[0]));
        dst->viewport.pViewports = dst->viewports;
    }
    if (viewport->pScissors) {
        memcpy(dst->scissors, viewport->pScissors,
               viewport->scissorCount * sizeof(dst->scissors[0]));
        dst->viewport.pScissors = dst->scissors;
    }

    if (src->pRasterizationState->sType !=
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
        src->pRasterizationState->pNext) {
        return false;
    }
    dst->raster = *src->pRasterizationState;
    dst->info.pRasterizationState = &dst->raster;

    const VkPipelineMultisampleStateCreateInfo *multi = src->pMultisampleState;
    if (multi->sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
        multi->pNext || !multi->rasterizationSamples ||
        multi->rasterizationSamples > 64) {
        return false;
    }
    dst->multisample = *multi;
    dst->info.pMultisampleState = &dst->multisample;
    if (multi->pSampleMask) {
        uint32_t words = (multi->rasterizationSamples + 31) / 32;
        if (words > MAX_SAMPLE_MASK_WORDS) {
            return false;
        }
        memcpy(dst->sample_masks, multi->pSampleMask,
               words * sizeof(dst->sample_masks[0]));
        dst->multisample.pSampleMask = dst->sample_masks;
    }

    if (src->pDepthStencilState) {
        if (src->pDepthStencilState->sType !=
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
            src->pDepthStencilState->pNext) {
            return false;
        }
        dst->depth_stencil = *src->pDepthStencilState;
        dst->info.pDepthStencilState = &dst->depth_stencil;
    }

    const VkPipelineColorBlendStateCreateInfo *blend = src->pColorBlendState;
    if (blend->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO ||
        blend->pNext || blend->attachmentCount > MAX_COLOR_ATTACHMENTS ||
        (blend->attachmentCount && !blend->pAttachments)) {
        return false;
    }
    dst->blend = *blend;
    dst->info.pColorBlendState = &dst->blend;
    if (blend->attachmentCount) {
        memcpy(dst->attachments, blend->pAttachments,
               blend->attachmentCount * sizeof(dst->attachments[0]));
        dst->blend.pAttachments = dst->attachments;
    } else {
        dst->blend.pAttachments = NULL;
    }

    const VkPipelineDynamicStateCreateInfo *dynamic = src->pDynamicState;
    if (dynamic->sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO ||
        dynamic->pNext || dynamic->dynamicStateCount > MAX_DYNAMIC_STATES ||
        (dynamic->dynamicStateCount && !dynamic->pDynamicStates)) {
        return false;
    }
    dst->dynamic = *dynamic;
    dst->info.pDynamicState = &dst->dynamic;
    if (dynamic->dynamicStateCount) {
        memcpy(dst->dynamic_states, dynamic->pDynamicStates,
               dynamic->dynamicStateCount * sizeof(dst->dynamic_states[0]));
        dst->dynamic.pDynamicStates = dst->dynamic_states;
    } else {
        dst->dynamic.pDynamicStates = NULL;
    }
    return true;
}

static void job_destroy(PipelineBuilderState *state, PipelineJob *job)
{
    if (job->result.pipeline != VK_NULL_HANDLE) {
        state->config.destroy(state->config.opaque, job->result.device,
                              job->result.pipeline);
    }
    g_free(job);
}

static void list_destroy(PipelineBuilderState *state, PipelineJob *head)
{
    while (head) {
        PipelineJob *next = head->next;
        job_destroy(state, head);
        head = next;
    }
}

static void *pipeline_worker(void *opaque)
{
    PipelineBuilderState *state = opaque;
    for (;;) {
        qemu_mutex_lock(&state->lock);
        while (!state->stopping && !state->pending_head) {
            qemu_cond_wait(&state->work_ready, &state->lock);
        }
        if (state->stopping) {
            qemu_mutex_unlock(&state->lock);
            break;
        }
        PipelineJob *job = list_pop_highest_priority(&state->pending_head,
                                                      &state->pending_tail);
        state->active = job;
        bool stale = job->request.generation < state->min_generation;
        qemu_mutex_unlock(&state->lock);

        if (!stale) {
            job->result.started_us = g_get_monotonic_time();
            job->result.vk_result = state->config.create(
                state->config.opaque, job->request.device,
                job->request.cache, &job->recipe.info,
                &job->result.pipeline);
            job->result.finished_us = g_get_monotonic_time();
        }

        qemu_mutex_lock(&state->lock);
        state->active = NULL;
        stale = state->stopping ||
                job->request.generation < state->min_generation;
        bool notify = false;
        if (stale) {
            state->outstanding--;
        } else {
            if (state->result_tail) {
                state->result_tail->next = job;
            } else {
                state->result_head = job;
            }
            state->result_tail = job;
            qatomic_set(&state->result_available, 1);
            notify = true;
        }
        qemu_mutex_unlock(&state->lock);
        if (notify && state->config.notify) {
            state->config.notify(state->config.notify_opaque);
        }
        if (stale) {
            job_destroy(state, job);
        }
    }
    return NULL;
}

bool pgraph_vk_hybrid_pipeline_builder_init(
    PGRAPHVkHybridPipelineBuilder *builder,
    const PGRAPHVkHybridPipelineBuilderConfig *config)
{
    if (!builder || builder->state || !config || !config->max_jobs ||
        !config->create || !config->destroy) {
        return false;
    }
    PipelineBuilderState *state = g_new0(PipelineBuilderState, 1);
    state->config = *config;
    qemu_mutex_init(&state->lock);
    qemu_cond_init(&state->work_ready);
    qemu_thread_create(&state->worker, "vk-hybrid-pipeline", pipeline_worker,
                       state, QEMU_THREAD_JOINABLE);
    builder->state = state;
    return true;
}

PGRAPHVkHybridPipelineSubmitResult pgraph_vk_hybrid_pipeline_builder_submit(
    PGRAPHVkHybridPipelineBuilder *builder,
    const PGRAPHVkHybridPipelineBuildRequest *request)
{
    PipelineBuilderState *state = builder ? builder->state : NULL;
    if (!state || !request || !request->ticket || !request->create_info) {
        return PGRAPH_VK_HYBRID_PIPELINE_UNSUPPORTED_RECIPE;
    }
    PipelineJob *job = g_new0(PipelineJob, 1);
    if (!recipe_copy(&job->recipe, request->create_info)) {
        g_free(job);
        return PGRAPH_VK_HYBRID_PIPELINE_UNSUPPORTED_RECIPE;
    }
    job->request = *request;
    job->request.create_info = &job->recipe.info;
    job->result = (PGRAPHVkHybridPipelineBuildResult) {
        .generation = request->generation,
        .ticket = request->ticket,
        .key_hash = request->key_hash,
        .priority = request->priority,
        .device = request->device,
        .pipeline = VK_NULL_HANDLE,
        .vk_result = VK_NOT_READY,
        .submitted_us = g_get_monotonic_time(),
        .destroy = state->config.destroy,
        .destroy_opaque = state->config.opaque,
    };
    qemu_mutex_lock(&state->lock);
    PGRAPHVkHybridPipelineSubmitResult status = PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED;
    if (state->stopping || request->generation < state->min_generation) {
        status = PGRAPH_VK_HYBRID_PIPELINE_STOPPED;
    } else if (state->outstanding >= state->config.max_jobs) {
        status = PGRAPH_VK_HYBRID_PIPELINE_QUEUE_FULL;
    } else {
        if (state->pending_tail) {
            state->pending_tail->next = job;
        } else {
            state->pending_head = job;
        }
        state->pending_tail = job;
        state->outstanding++;
        qemu_cond_signal(&state->work_ready);
    }
    qemu_mutex_unlock(&state->lock);
    if (status != PGRAPH_VK_HYBRID_PIPELINE_ACCEPTED) {
        g_free(job);
    }
    return status;
}

bool pgraph_vk_hybrid_pipeline_builder_promote(
    PGRAPHVkHybridPipelineBuilder *builder, uint64_t generation,
    uint64_t ticket, PGRAPHVkHybridPriority priority)
{
    PipelineBuilderState *state = builder ? builder->state : NULL;
    if (!state || !ticket) {
        return false;
    }
    bool found = false;
    qemu_mutex_lock(&state->lock);
    PipelineJob *lists[] = {
        state->pending_head, state->active, state->result_head,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(lists) && !found; i++) {
        for (PipelineJob *job = lists[i]; job; job = job->next) {
            if (job->request.generation == generation &&
                job->request.ticket == ticket) {
                if (priority > job->request.priority) {
                    job->request.priority = priority;
                    job->result.priority = priority;
                }
                found = true;
                break;
            }
        }
    }
    qemu_mutex_unlock(&state->lock);
    return found;
}

bool pgraph_vk_hybrid_pipeline_builder_take_result(
    PGRAPHVkHybridPipelineBuilder *builder,
    PGRAPHVkHybridPipelineBuildResult *result)
{
    PipelineBuilderState *state = builder ? builder->state : NULL;
    if (!state || !result) {
        return false;
    }
    qemu_mutex_lock(&state->lock);
    PipelineJob *job = list_pop_highest_priority(&state->result_head,
                                                  &state->result_tail);
    if (job) {
        if (!state->result_head) {
            qatomic_set(&state->result_available, 0);
        }
        state->outstanding--;
        *result = job->result;
        job->result.pipeline = VK_NULL_HANDLE;
    }
    bool found = job != NULL;
    qemu_mutex_unlock(&state->lock);
    g_free(job);
    return found;
}

bool pgraph_vk_hybrid_pipeline_builder_take_result_for(
    PGRAPHVkHybridPipelineBuilder *builder, uint64_t generation,
    uint64_t ticket, PGRAPHVkHybridPipelineBuildResult *result)
{
    PipelineBuilderState *state = builder ? builder->state : NULL;
    if (!state || !ticket || !result) {
        return false;
    }
    qemu_mutex_lock(&state->lock);
    PipelineJob *job = result_take_identity(state, generation, ticket);
    if (!job) {
        qemu_mutex_unlock(&state->lock);
        return false;
    }
    if (!state->result_head) {
        qatomic_set(&state->result_available, 0);
    }
    state->outstanding--;
    *result = job->result;
    job->result.pipeline = VK_NULL_HANDLE;
    qemu_mutex_unlock(&state->lock);
    g_free(job);
    return true;
}

bool pgraph_vk_hybrid_pipeline_builder_has_result(
    const PGRAPHVkHybridPipelineBuilder *builder)
{
    const PipelineBuilderState *state = builder ? builder->state : NULL;
    return state && qatomic_read(&state->result_available);
}

void pgraph_vk_hybrid_pipeline_builder_cancel_before_generation(
    PGRAPHVkHybridPipelineBuilder *builder, uint64_t generation)
{
    PipelineBuilderState *state = builder ? builder->state : NULL;
    if (!state) {
        return;
    }
    PipelineJob *discard = NULL;
    qemu_mutex_lock(&state->lock);
    if (generation > state->min_generation) {
        state->min_generation = generation;
    }
    PipelineJob **lists[] = { &state->pending_head, &state->result_head };
    PipelineJob **tails[] = { &state->pending_tail, &state->result_tail };
    for (size_t i = 0; i < G_N_ELEMENTS(lists); i++) {
        PipelineJob **cursor = lists[i];
        *tails[i] = NULL;
        while (*cursor) {
            PipelineJob *job = *cursor;
            if (job->request.generation < state->min_generation) {
                *cursor = job->next;
                job->next = discard;
                discard = job;
                state->outstanding--;
            } else {
                *tails[i] = job;
                cursor = &job->next;
            }
        }
    }
    qatomic_set(&state->result_available, state->result_head != NULL);
    qemu_mutex_unlock(&state->lock);
    list_destroy(state, discard);
}

void pgraph_vk_hybrid_pipeline_build_result_destroy(
    PGRAPHVkHybridPipelineBuilder *builder,
    PGRAPHVkHybridPipelineBuildResult *result)
{
    (void)builder;
    if (result && result->pipeline != VK_NULL_HANDLE) {
        assert(result->destroy);
        result->destroy(result->destroy_opaque, result->device,
                        result->pipeline);
        result->pipeline = VK_NULL_HANDLE;
    }
}

void pgraph_vk_hybrid_pipeline_builder_stop(
    PGRAPHVkHybridPipelineBuilder *builder)
{
    PipelineBuilderState *state = builder ? builder->state : NULL;
    if (!state) {
        return;
    }
    qemu_mutex_lock(&state->lock);
    PipelineJob *pending = NULL;
    PipelineJob *results = NULL;
    if (!state->stopping) {
        state->stopping = true;
        pending = state->pending_head;
        results = state->result_head;
        state->pending_head = state->pending_tail = NULL;
        state->result_head = state->result_tail = NULL;
        qatomic_set(&state->result_available, 0);
        for (PipelineJob *job = pending; job; job = job->next) {
            state->outstanding--;
        }
        for (PipelineJob *job = results; job; job = job->next) {
            state->outstanding--;
        }
        qemu_cond_broadcast(&state->work_ready);
    }
    qemu_mutex_unlock(&state->lock);
    list_destroy(state, pending);
    list_destroy(state, results);
}

void pgraph_vk_hybrid_pipeline_builder_join(
    PGRAPHVkHybridPipelineBuilder *builder)
{
    PipelineBuilderState *state = builder ? builder->state : NULL;
    if (!state || state->joined) {
        return;
    }
    pgraph_vk_hybrid_pipeline_builder_stop(builder);
    qemu_thread_join(&state->worker);
    state->joined = true;
}

void pgraph_vk_hybrid_pipeline_builder_destroy(
    PGRAPHVkHybridPipelineBuilder *builder)
{
    PipelineBuilderState *state = builder ? builder->state : NULL;
    if (!state) {
        return;
    }
    pgraph_vk_hybrid_pipeline_builder_join(builder);
    qemu_cond_destroy(&state->work_ready);
    qemu_mutex_destroy(&state->lock);
    g_free(state);
    builder->state = NULL;
}
