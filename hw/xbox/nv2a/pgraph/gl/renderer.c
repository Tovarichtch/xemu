/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "debug.h"
#include "renderer.h"
#include "qemu/fast-hash.h"
#include "ui/xemu-settings.h"

GloContext *g_nv2a_context_render;
GloContext *g_nv2a_context_display;

static void early_context_init(void)
{
    g_nv2a_context_render = glo_context_create_4_1();
    g_nv2a_context_display = glo_context_create_4_1();

    // Note: Due to use of shared contexts, this must happen after some other
    // context is created so the temporary context will not become the thread
    // context. After destroying the context, some a durable context should be
    // selected.
    GloContext *context = glo_context_create_4_1();
    pgraph_gl_determine_gpu_properties();
    glo_context_destroy(context);
    glo_set_current(g_nv2a_context_display);
}

static void pgraph_gl_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->gl_renderer_state = g_malloc0(sizeof(*pg->gl_renderer_state));
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* fire up opengl */
    glo_set_current(g_nv2a_context_render);
    if (pgraph_gpu_boost_gl() &&
        glo_check_extension("GL_ARB_parallel_shader_compile")) {
        glMaxShaderCompilerThreadsARB(16);
    }

#if DEBUG_NV2A_GL
    gl_debug_initialize();
#endif

    /* DXT textures */
    assert(glo_check_extension("GL_EXT_texture_compression_s3tc"));
    /*  Internal RGB565 texture format */
    assert(glo_check_extension("GL_ARB_ES2_compatibility"));

    glGetFloatv(GL_SMOOTH_LINE_WIDTH_RANGE, r->supported_smooth_line_width_range);
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, r->supported_aliased_line_width_range);

    pgraph_gl_init_surfaces(pg);
    pgraph_gl_init_reports(d);
    pgraph_gl_init_textures(d);
    pgraph_gl_init_buffers(d);
    pgraph_gl_init_shaders(pg);
    pgraph_gl_init_display(d);

    pgraph_gl_update_entire_memory_buffer(d);

    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->supported_extensions.texture_filter_anisotropic =
        glo_check_extension("GL_EXT_texture_filter_anisotropic");
}

static void pgraph_gl_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    glo_set_current(g_nv2a_context_render);

    pgraph_gl_finalize_surfaces(pg);
    pgraph_gl_finalize_shaders(pg);
    pgraph_gl_finalize_textures(pg);
    pgraph_gl_finalize_reports(pg);
    pgraph_gl_finalize_buffers(pg);
    pgraph_gl_finalize_display(pg);

    glDeleteSync(pg->gl_renderer_state->flip_fence); /* ignores 0 */

    glo_set_current(NULL);

    g_free(pg->gl_renderer_state);
    pg->gl_renderer_state = NULL;
}

static void pgraph_gl_flip_stall(NV2AState *d)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    NV2A_GL_DFRAME_TERMINATOR();
    pgraph_gl_shaders_flip_service(d);

    /* Close this frame's open fragment-query segment (one segment per run
     * of draws sharing a cost profile); the model reads the segments after
     * the drain below. */
    xemu_cost_frame_close(d, &pgraph_gl_cost_ops);

    /* The cost model reads its queries right after this and keeps the
     * drain. Otherwise wait for the PREVIOUS frame's fence and post this
     * frame's: the GPU stays at most one frame behind, and a driver slow to
     * glFinish (AMD Windows, MEASURED) does not stall every frame. */
    if (xemu_cost_segn) {
        glFinish();
    } else {
        if (r->flip_fence) {
            glClientWaitSync(r->flip_fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                             (GLuint64)5000000000);
            glDeleteSync(r->flip_fence);
        }
        r->flip_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
    }

    /* Surfaces the guest CPU read this frame (a blocking read that drains
     * the pipeline -- free on the real hardware's unified VRAM) are read
     * back here, at the flip, without waiting where the pack allows: the
     * next guest read finds RAM up to date and no longer blocks. Nothing
     * for games that never read back. */
    SurfaceBinding *s;
    QTAILQ_FOREACH (s, &r->surfaces, entry) {
        if (s->cpu_reads != s->cpu_reads_last_flip) {
            s->cpu_reads_last_flip = s->cpu_reads;
            pgraph_gl_surface_predownload(d, s);
        }
    }
    pgraph_gl_surfaces_refresh_any_dirty(d);
    xemu_cost_resolve_frame(d, &pgraph_gl_cost_ops,
                            d->pgraph.surface_scale_factor);
}

static void pgraph_gl_flush(NV2AState *d)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    pgraph_gl_surface_flush(d);
    /* The surface list is rebuilt: the display resolve compares surface
     * pointers, so forget the one it published from. */
    r->disp_last_surface = NULL;
    pgraph_gl_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_gl_update_entire_memory_buffer(d);
    pgraph_gl_uniform_sources_invalidate(r);
    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_gl_process_pending(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    bool scanout = qatomic_read(&d->pgraph.scanout_changed) ||
                   qatomic_read(&r->disp_check_pending);
    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) || scanout ||
        qatomic_read(&d->pgraph.flush_pending) ||
        qatomic_read(&r->shader_cache_writeback_pending)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_gl_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            /* pre-savevm: close the open visibility-test interval and serve
             * every queued report, so each GET_REPORT result is in guest
             * RAM before it is serialized. */
            if (r->gl_zpass_query_open) {
                glEndQuery(GL_SAMPLES_PASSED);
                r->gl_zpass_query_open = false;
            }
            pgraph_gl_drain_pending_reports(d);
            pgraph_gl_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            qatomic_set(&d->pgraph.scanout_changed, false);
            qatomic_set(&r->disp_check_pending, false);
            pgraph_gl_sync(d);
        } else if (scanout) {
            qatomic_set(&d->pgraph.scanout_changed, false);
            qatomic_set(&r->disp_check_pending, false);
            pgraph_gl_present_scanout(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_gl_flush(d);
        }
        if (qatomic_read(&r->shader_cache_writeback_pending)) {
            pgraph_gl_shader_write_cache_reload_list(&d->pgraph);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_gl_pre_savevm_trigger(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qatomic_set(&r->download_dirty_surfaces_pending, true);
    qemu_event_reset(&r->dirty_surfaces_download_complete);
}

static void pgraph_gl_pre_savevm_wait(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_event_wait(&r->dirty_surfaces_download_complete);
}

static void pgraph_gl_pre_shutdown_trigger(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qatomic_set(&r->shader_cache_writeback_pending, true);
    qemu_event_reset(&r->shader_cache_writeback_complete);
}

static void pgraph_gl_pre_shutdown_wait(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_event_wait(&r->shader_cache_writeback_complete);
}

static PGRAPHRenderer pgraph_gl_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_OPENGL,
    .name = "OpenGL",
    .ops = {
        .init = pgraph_gl_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_gl_finalize,
        .clear_report_value = pgraph_gl_clear_report_value,
        .clear_surface = pgraph_gl_clear_surface,
        .draw_begin = pgraph_gl_draw_begin,
        .draw_end = pgraph_gl_draw_end,
        .flip_stall = pgraph_gl_flip_stall,
        .flush_draw = pgraph_gl_flush_draw,
        .get_report = pgraph_gl_get_report,
        .image_blit = pgraph_gl_image_blit,
        .pre_savevm_trigger = pgraph_gl_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_gl_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_gl_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_gl_pre_shutdown_wait,
        .process_pending = pgraph_gl_process_pending,
        .process_pending_reports = pgraph_gl_process_pending_reports,
        .surface_update = pgraph_gl_surface_update,
        .set_surface_scale_factor = pgraph_gl_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_gl_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_gl_get_framebuffer_surface,
        .get_gpu_properties = pgraph_gl_get_gpu_properties,
    }
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_gl_renderer);
}
