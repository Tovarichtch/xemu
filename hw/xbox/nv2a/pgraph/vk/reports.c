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

void pgraph_vk_init_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QSIMPLEQ_INIT(&r->report_queue);
    r->num_queries_in_flight = 0;
    r->queries_submitted = 0;
    r->queries_read = 0;
    r->max_queries_in_flight = 1024;
    r->new_query_needed = false;
    r->query_in_flight = false;
    r->zpass_pixel_count_result = 0;

    VkQueryPoolCreateInfo pool_create_info = (VkQueryPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_OCCLUSION,
        .queryCount = r->max_queries_in_flight,
    };
    VK_CHECK(
        vkCreateQueryPool(r->device, &pool_create_info, NULL, &r->query_pool));

    /* Cost-model counters per segment (pgraph/cost.c): shader invocations,
     * and pass counts while the game's own occlusion query is closed (one
     * active query of a type per command buffer). Made whenever supported:
     * the model is a live UI toggle. */
    r->cost_stats_pool = VK_NULL_HANDLE;
    r->cost_occl_pool = VK_NULL_HANDLE;
    r->cost_pending_slot = -1;
    if (r->enabled_physical_device_features.pipelineStatisticsQuery) {
        VkQueryPoolCreateInfo stats_create_info = (VkQueryPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
            .queryCount = XEMU_COST_SEGS,
            .pipelineStatistics =
                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT,
        };
        VK_CHECK(vkCreateQueryPool(r->device, &stats_create_info, NULL,
                                   &r->cost_stats_pool));
        VkQueryPoolCreateInfo occl_create_info = (VkQueryPoolCreateInfo){
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_OCCLUSION,
            .queryCount = XEMU_COST_SEGS,
        };
        VK_CHECK(vkCreateQueryPool(r->device, &occl_create_info, NULL,
                                   &r->cost_occl_pool));
    }
}

void pgraph_vk_finalize_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    vkDestroyQueryPool(r->device, r->query_pool, NULL);
    if (r->cost_stats_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(r->device, r->cost_stats_pool, NULL);
        vkDestroyQueryPool(r->device, r->cost_occl_pool, NULL);
    }
}

void pgraph_vk_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueryReport *report = g_malloc(sizeof(QueryReport)); // FIXME: Pre-allocate
    report->clear = true;
    report->parameter = 0;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

void pgraph_vk_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);
    assert(type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);

    QueryReport *report = g_malloc(sizeof(QueryReport)); // FIXME: Pre-allocate
    report->clear = false;
    report->parameter = parameter;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;

    /* No finish here: a guest that waits for the value stops feeding the
     * pusher, which runs dry into the STALLED finish of
     * process_pending_reports, as on GL. Outside a recording, what is
     * already available is served now. */
    if (!r->in_command_buffer) {
        pgraph_vk_process_pending_reports_internal(d);
    }
}

void pgraph_vk_process_pending_reports_internal(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Processing queries");

    /* The hardware writes a report when the GPU reaches it; the pusher
     * never waits. Slots are read at availability only (no WAIT) and
     * counted per report window, up to the slot the method was issued at:
     * a later slot would count pixels drawn after the test, and a clear
     * served in the same pass would wipe the next test's. A report
     * still running stays queued (the pfifo loop polls, reports_pending)
     * and the guest reads the previous value meanwhile, as on the hardware. */

    const int base = r->queries_read;
    int unread = r->queries_submitted - base;
    g_autofree uint64_t *results = NULL;
    int avail = 0;
    if (unread > 0) {
        results = g_malloc_n(unread, 2 * sizeof(uint64_t));
        VkResult result = vkGetQueryPoolResults(
            r->device, r->query_pool, base, unread,
            (size_t)unread * 2 * sizeof(uint64_t), results,
            2 * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        assert(result == VK_SUCCESS || result == VK_NOT_READY);
        while (avail < unread && results[2 * avail + 1] != 0) {
            avail++;
        }
    }
    const int limit = base + avail;

    const int result_divisor =
        pg->surface_scale_factor * pg->surface_scale_factor;
    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL &&
           report->query_count <= limit) {
        while (r->queries_read < report->query_count) {
            r->zpass_pixel_count_result +=
                results[2 * (r->queries_read - base)];
            r->queries_read++;
        }
        if (report->clear) {
            NV2A_VK_DPRINTF("Cleared");
            r->zpass_pixel_count_result = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(
                d, report->parameter,
                r->zpass_pixel_count_result / result_divisor);
        }
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }
    if (QSIMPLEQ_EMPTY(&r->report_queue)) {
        /* No report waits: every counted slot belongs to the next window. */
        while (r->queries_read < limit) {
            r->zpass_pixel_count_result +=
                results[2 * (r->queries_read - base)];
            r->queries_read++;
        }
    }
    pg->reports_pending = !QSIMPLEQ_EMPTY(&r->report_queue);

    /* Every slot counted and nothing queued or open: the pool starts over
     * (a slot is reset before its reuse, begin_query). */
    if (!pg->reports_pending && !r->query_in_flight &&
        r->queries_read == r->num_queries_in_flight) {
        r->num_queries_in_flight = 0;
        r->queries_submitted = 0;
        r->queries_read = 0;
    }

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    if (*dma_get == *dma_put && r->in_command_buffer) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_STALLED);
        return; /* the finish served what was available */
    }
    /* Poll while a report waits on submitted slots, and serve one whose
     * window is already counted (a clear issued after the last draw). */
    QueryReport *head = QSIMPLEQ_FIRST(&r->report_queue);
    if (head && (r->queries_read < r->queries_submitted ||
                 head->query_count <= r->queries_read)) {
        pgraph_vk_process_pending_reports_internal(d);
    }
}
