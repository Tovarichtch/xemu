/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 * Copyright (c) 2026 Réda Chérif-Touil
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

#include "renderer.h"
#include "ui/xemu-settings.h"

static void create_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(
        vkCreateCommandPool(r->device, &create_info, NULL, &r->command_pool));
}

static void destroy_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyCommandPool(r->device, r->command_pool, NULL);
}

static void create_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ARRAY_SIZE(r->command_buffers),
    };
    VK_CHECK(
        vkAllocateCommandBuffers(r->device, &alloc_info, r->command_buffers));

    /* [0..N) = main slots; [N..2N) = per-slot staging-sync; [2N] = the
     * standalone aux for single-time commands (own fence). */
    for (int s = 0; s < PGRAPH_VK_SLOTS; s++) {
        r->slot_main[s] = r->command_buffers[s];
        r->slot_sync[s] = r->command_buffers[PGRAPH_VK_SLOTS + s];
    }
    r->slot = 0;
    r->command_buffer = r->slot_main[0];
    r->aux_command_buffer = r->command_buffers[2 * PGRAPH_VK_SLOTS];
}

static void destroy_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeCommandBuffers(r->device, r->command_pool,
                         ARRAY_SIZE(r->command_buffers), r->command_buffers);

    r->command_buffer = VK_NULL_HANDLE;
    r->aux_command_buffer = VK_NULL_HANDLE;
}

/* Wait for the last aux submission, so the aux command buffer and the
 * staging buffers it read can be reused; nothing else uses its fence. */
void pgraph_vk_wait_for_aux(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->aux_in_flight) {
        VK_CHECK(vkWaitForFences(r->device, 1, &r->aux_fence, VK_TRUE,
                                 UINT64_MAX));

        VK_CHECK(vkResetFences(r->device, 1, &r->aux_fence));
        r->aux_in_flight = false;
    }
}

VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_wait_for_aux(pg);
    assert(!r->in_aux_command_buffer);
    r->in_aux_command_buffer = true;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &begin_info));

    return r->aux_command_buffer;
}

/* Submit without blocking: the work runs ahead of the open recording, and
 * the barrier each caller records last (a layout transition) orders later
 * commands after it. Callers that read results back use the sync variant. */
void pgraph_vk_end_single_time_commands_async(PGRAPHState *pg,
                                              VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_aux_command_buffer);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, r->aux_fence));
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_AUX);
    r->aux_in_flight = true;

    r->in_aux_command_buffer = false;
}

void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    pgraph_vk_end_single_time_commands_async(pg, cmd);
    pgraph_vk_wait_for_aux(pg);
}

/* Timestamps are defined only on a queue family with valid bits. */
static bool queue_has_timestamps(PGRAPHVkState *r)
{
    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(r->physical_device, &n, NULL);
    g_autofree VkQueueFamilyProperties *props =
        g_new(VkQueueFamilyProperties, n);
    vkGetPhysicalDeviceQueueFamilyProperties(r->physical_device, &n, props);
    return indices.queue_family >= 0 && (uint32_t)indices.queue_family < n &&
           props[indices.queue_family].timestampValidBits > 0;
}

void pgraph_vk_init_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    create_command_pool(pg);
    create_command_buffers(pg);

    /* Frame GPU timestamps (draw.c pgraph_vk_frame_gpu_*), host-reset. */
    r->frame_gpu.pool = VK_NULL_HANDLE;
    r->frame_gpu.w = 0;
    r->frame_gpu.busy_us = 0;
    if (r->host_query_reset && queue_has_timestamps(r)) {
        r->frame_gpu.capacity = 256;
        VkQueryPoolCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 2 * r->frame_gpu.capacity,
        };
        VK_CHECK(vkCreateQueryPool(r->device, &ci, NULL, &r->frame_gpu.pool));
        vkResetQueryPool(r->device, r->frame_gpu.pool, 0,
                         2 * r->frame_gpu.capacity);
        r->frame_gpu.scratch =
            g_malloc0(2 * r->frame_gpu.capacity * sizeof(uint64_t));
    }

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &r->aux_fence));
    r->aux_in_flight = false;

    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    for (int s = 0; s < PGRAPH_VK_SLOTS; s++) {
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL,
                               &r->slot_fence[s]));
        r->slot_fence_pending[s] = false;
        VK_CHECK(vkCreateSemaphore(r->device, &sem_info, NULL,
                                   &r->slot_sem[s]));
    }
}

void pgraph_vk_finalize_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_wait_all_slots(r);
    pgraph_vk_wait_for_aux(pg);
    pgraph_vk_trash_drain(r);
    g_free(r->trash);
    r->trash = NULL;
    r->trash_cap = 0;
    vkDestroyFence(r->device, r->aux_fence, NULL);
    for (int s = 0; s < PGRAPH_VK_SLOTS; s++) {
        vkDestroyFence(r->device, r->slot_fence[s], NULL);
        vkDestroySemaphore(r->device, r->slot_sem[s], NULL);
    }
    destroy_command_buffers(pg);
    if (r->frame_gpu.pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(r->device, r->frame_gpu.pool, NULL);
        r->frame_gpu.pool = VK_NULL_HANDLE;
        g_free(r->frame_gpu.scratch);
        r->frame_gpu.scratch = NULL;
    }
    destroy_command_pool(pg);
}