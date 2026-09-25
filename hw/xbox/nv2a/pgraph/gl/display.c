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

#include "qemu/osdep.h"
#include "hw/display/vga_int.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "renderer.h"

#include <math.h>

static uint8_t *convert_texture_data__CR8YB8CB8YA8(const uint8_t *data,
                                                   unsigned int width,
                                                   unsigned int height,
                                                   unsigned int pitch)
{
    uint8_t *converted_data = (uint8_t *)g_malloc(width * height * 4);
    int x, y;
    for (y = 0; y < height; y++) {
        const uint8_t *line = &data[y * pitch];
        const uint32_t row_offset = y * width;
        for (x = 0; x < width; x++) {
            uint8_t *pixel = &converted_data[(row_offset + x) * 4];
            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
    return converted_data;
}

static float pvideo_calculate_scale(unsigned int din_dout,
                                           unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

/* Display hand-off. The render thread owns the render context and the
 * surfaces; a front buffer reaches the UI thread as a ticket (DispPost:
 * texture, geometry, display parameters, the video overlay registers and a
 * fence after the frame's draws). The UI thread waits for that fence on the
 * GPU and blits the texture into one of its two display textures: neither
 * thread switches GL context, and past the first frame neither waits for
 * the other. A ticket keeps its surface texture alive (DispRef). */

/* disp_lock held. The texture and the fence are shared objects: any
 * context of the share group may delete them. */
static void disp_ref_free_locked(DispRef *ref)
{
    glDeleteTextures(1, &ref->tex);
    if (ref->presented) {
        glDeleteSync(ref->presented);
    }
    g_free(ref);
}

/* disp_lock held. */
static void disp_ref_put_locked(DispRef *ref)
{
    ref->refs--;
    if (ref->refs == 0 && ref->orphan) {
        disp_ref_free_locked(ref); /* the surface is gone */
    }
}

/* Render thread: the surface's texture is being destroyed. */
void pgraph_gl_display_surface_gone(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    DispRef *ref = surface->disp_ref;

    if (r->disp_last_surface == surface) {
        r->disp_last_surface = NULL; /* the address may be reused */
    }
    if (ref == NULL) {
        glDeleteTextures(1, &surface->gl_buffer);
        return;
    }
    surface->disp_ref = NULL;
    qemu_mutex_lock(&r->disp_lock);
    if (ref->refs == 0) {
        disp_ref_free_locked(ref);
    } else {
        ref->orphan = true; /* the last ticket deletes it */
    }
    qemu_mutex_unlock(&r->disp_lock);
}

/* Render thread, before the surface is written again: its last display blit
 * may still be queued on the UI context, so the GPU waits for it first. */
void pgraph_gl_display_surface_reuse(NV2AState *d, SurfaceBinding *surface)
{
    DispRef *ref = surface->disp_ref;

    if (ref == NULL || qatomic_read(&ref->presented) == NULL) {
        return;
    }
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    qemu_mutex_lock(&r->disp_lock);
    GLsync f = ref->presented;
    ref->presented = NULL;
    qemu_mutex_unlock(&r->disp_lock);
    if (f) {
        glWaitSync(f, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(f);
    }
}

/* UI thread state: the display program and two display textures, created
 * in the UI context on first use and kept for the process. */
static struct {
    bool ready;
    GLuint prog, vao, vbo, fbo, pvideo_tex;
    GLint tex_loc, pvideo_enable_loc, pvideo_tex_loc, pvideo_in_pos_loc,
          pvideo_pos_loc, pvideo_scale_loc, pvideo_color_key_enable_loc,
          pvideo_color_key_loc, display_size_loc, line_offset_loc;
    struct {
        GLuint tex;
        GLint internal_format;
        GLsizei width, height;
        GLenum format, type;
    } slot[2];
    int cur; /* the texture on screen, -1 = none yet */
    unsigned int cur_draw_time;
} ui;

static void ui_init(void)
{
    for (int i = 0; i < ARRAY_SIZE(ui.slot); i++) {
        memset(&ui.slot[i], 0, sizeof(ui.slot[i]));
        glGenTextures(1, &ui.slot[i].tex);
    }
    ui.cur = -1;

    const char *vs =
        "#version 330\n"
        "void main()\n"
        "{\n"
        "    float x = -1.0 + float((gl_VertexID & 1) << 2);\n"
        "    float y = -1.0 + float((gl_VertexID & 2) << 1);\n"
        "    gl_Position = vec4(x, y, 0, 1);\n"
        "}\n";
    /* FIXME: improve interlace handling, pvideo */

    const char *fs =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "uniform bool pvideo_enable;\n"
        "uniform sampler2D pvideo_tex;\n"
        "uniform vec2 pvideo_in_pos;\n"
        "uniform vec4 pvideo_pos;\n"
        "uniform vec3 pvideo_scale;\n"
        "uniform bool pvideo_color_key_enable;\n"
        "uniform vec3 pvideo_color_key;\n"
        "uniform vec2 display_size;\n"
        "uniform float line_offset;\n"
        "layout(location = 0) out vec4 out_Color;\n"
        "void main()\n"
        "{\n"
        "    vec2 texCoord = gl_FragCoord.xy/display_size;\n"
        "    float rel = display_size.y/textureSize(tex, 0).y/line_offset;\n"
        "    texCoord.y = rel*(1.0f - texCoord.y);\n"
        "    out_Color.rgba = texture(tex, texCoord);\n"
        "    if (pvideo_enable) {\n"
        "        vec2 screenCoord = gl_FragCoord.xy - 0.5;\n"
        "        vec4 output_region = vec4(pvideo_pos.xy, pvideo_pos.xy + pvideo_pos.zw);\n"
        "        bvec4 clip = bvec4(lessThan(screenCoord, output_region.xy),\n"
        "                           greaterThan(screenCoord, output_region.zw));\n"
        "        if (!any(clip) && (!pvideo_color_key_enable || out_Color.rgb == pvideo_color_key)) {\n"
        "            vec2 out_xy = (screenCoord - pvideo_pos.xy) * pvideo_scale.z;\n"
        "            vec2 in_st = (pvideo_in_pos + out_xy * pvideo_scale.xy) / textureSize(pvideo_tex, 0);\n"
        "            in_st.y *= -1.0;\n"
        "            out_Color.rgba = texture(pvideo_tex, in_st);\n"
        "        }\n"
        "    }\n"
        "}\n";

    ui.prog = pgraph_gl_compile_shader(vs, fs);
    ui.tex_loc = glGetUniformLocation(ui.prog, "tex");
    ui.pvideo_enable_loc = glGetUniformLocation(ui.prog, "pvideo_enable");
    ui.pvideo_tex_loc = glGetUniformLocation(ui.prog, "pvideo_tex");
    ui.pvideo_in_pos_loc = glGetUniformLocation(ui.prog, "pvideo_in_pos");
    ui.pvideo_pos_loc = glGetUniformLocation(ui.prog, "pvideo_pos");
    ui.pvideo_scale_loc = glGetUniformLocation(ui.prog, "pvideo_scale");
    ui.pvideo_color_key_enable_loc = glGetUniformLocation(ui.prog, "pvideo_color_key_enable");
    ui.pvideo_color_key_loc = glGetUniformLocation(ui.prog, "pvideo_color_key");
    ui.display_size_loc = glGetUniformLocation(ui.prog, "display_size");
    ui.line_offset_loc = glGetUniformLocation(ui.prog, "line_offset");

    glGenVertexArrays(1, &ui.vao);
    glBindVertexArray(ui.vao);
    glGenBuffers(1, &ui.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ui.vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STATIC_DRAW);
    glGenFramebuffers(1, &ui.fbo);
    glGenTextures(1, &ui.pvideo_tex);
    ui.ready = true;
}

static void ui_pvideo_overlay(NV2AState *d, const DispPost *p,
                              GLsizei disp_height)
{
    const DispPvideo *pv = &p->pv;

    // FIXME: This check against PVIDEO_SIZE_IN does not match HW behavior.
    // Many games seem to pass this value when initializing or tearing down
    // PVIDEO. On its own, this generally does not result in the overlay being
    // hidden, however there are certain games (e.g., Ultimate Beach Soccer)
    // that use an unknown mechanism to hide the overlay without explicitly
    // stopping it.
    // Since the value seems to be set to 0xFFFFFFFF only in cases where the
    // content is not valid, it is probably good enough to treat it as an
    // implicit stop.
    bool enabled = p->pvideo;
    glUniform1ui(ui.pvideo_enable_loc, enabled);
    if (!enabled) {
        return;
    }

    hwaddr base = pv->base;
    hwaddr limit = pv->limit;
    hwaddr offset = pv->offset;

    int in_width = GET_MASK(pv->size_in, NV_PVIDEO_SIZE_IN_WIDTH);
    int in_height = GET_MASK(pv->size_in, NV_PVIDEO_SIZE_IN_HEIGHT);

    int in_s = GET_MASK(pv->point_in, NV_PVIDEO_POINT_IN_S);
    int in_t = GET_MASK(pv->point_in, NV_PVIDEO_POINT_IN_T);

    int in_pitch = GET_MASK(pv->format, NV_PVIDEO_FORMAT_PITCH);
    int in_color = GET_MASK(pv->format, NV_PVIDEO_FORMAT_COLOR);

    unsigned int out_width = GET_MASK(pv->size_out, NV_PVIDEO_SIZE_OUT_WIDTH);
    unsigned int out_height =
        GET_MASK(pv->size_out, NV_PVIDEO_SIZE_OUT_HEIGHT);

    float scale_x = 1.0f;
    float scale_y = 1.0f;
    unsigned int ds_dx = pv->ds_dx;
    unsigned int dt_dy = pv->dt_dy;
    if (ds_dx != NV_PVIDEO_DIN_DOUT_UNITY) {
        scale_x = pvideo_calculate_scale(ds_dx, out_width);
    }
    if (dt_dy != NV_PVIDEO_DIN_DOUT_UNITY) {
        scale_y = pvideo_calculate_scale(dt_dy, out_height);
    }

    // On HW, setting NV_PVIDEO_SIZE_IN larger than NV_PVIDEO_SIZE_OUT results
    // in them being capped to the output size, content is not scaled. This is
    // particularly important as NV_PVIDEO_SIZE_IN may be set to 0xFFFFFFFF
    // during initialization or teardown.
    if (in_width > out_width) {
        in_width = floorf((float)out_width * scale_x + 0.5f);
    }
    if (in_height > out_height) {
        in_height = floorf((float)out_height * scale_y + 0.5f);
    }

    /* TODO: support other color formats */
    assert(in_color == NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8);

    unsigned int out_x = GET_MASK(pv->point_out, NV_PVIDEO_POINT_OUT_X);
    unsigned int out_y = GET_MASK(pv->point_out, NV_PVIDEO_POINT_OUT_Y);

    unsigned int color_key_enabled =
        GET_MASK(pv->format, NV_PVIDEO_FORMAT_DISPLAY);
    glUniform1ui(ui.pvideo_color_key_enable_loc, color_key_enabled);

    unsigned int color_key = pv->color_key & 0xFFFFFF;
    glUniform3f(ui.pvideo_color_key_loc,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0,
                GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0);

    assert(offset + in_pitch * in_height <= limit);
    hwaddr end = base + offset + in_pitch * in_height;
    assert(end <= memory_region_size(d->vram));

    out_x *= p->scale;
    out_y *= p->scale;
    out_width *= p->scale;
    out_height *= p->scale;

    // Translate for the GL viewport origin.
    out_y = MAX(disp_height - 1 - (int)(out_y + out_height), 0);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, ui.pvideo_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    uint8_t *tex_rgba = convert_texture_data__CR8YB8CB8YA8(
        d->vram_ptr + base + offset, in_width, in_height, in_pitch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, in_width, in_height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, tex_rgba);
    g_free(tex_rgba);
    glUniform1i(ui.pvideo_tex_loc, 1);
    glUniform2f(ui.pvideo_in_pos_loc, in_s / 16.f, in_t / 8.f);
    glUniform4f(ui.pvideo_pos_loc, out_x, out_y, out_width, out_height);
    glUniform3f(ui.pvideo_scale_loc, scale_x, scale_y, 1.0f / p->scale);
}

/* UI thread, its context current: blit a ticket into a display texture. */
static void ui_resolve(NV2AState *d, const DispPost *p, int slot)
{
    g_nv2a_stats.presented_frame_id = p->draw_time;

    unsigned int width = p->disp_width, height = p->disp_height;
    int line_offset = p->line_offset ? p->pitch / p->line_offset : 1;

    /* Adjust viewport height for interlaced mode, used only in 1080i */
    if (p->interlaced) {
        height *= 2;
    }

    /* Clamp display to surface dimensions when CRTC area exceeds the
     * framebuffer (e.g. PAL 720x576 CRTC with 640x480 render target).
     * On real hardware the TV encoder fills the overscan with black;
     * xemu has no TV encoder so we just match the surface size. */
    if (width > p->width)   width  = p->width;
    if (height > p->height) height = p->height;

    width *= p->scale;
    height *= p->scale;

    /* GPU-side: the blit queues behind the frame's draws. */
    glWaitSync(p->rendered, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(p->rendered);

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ui.fbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ui.slot[slot].tex);
    bool recreate = (
        p->internal_format != ui.slot[slot].internal_format
        || width != ui.slot[slot].width
        || height != ui.slot[slot].height
        || p->format != ui.slot[slot].format
        || p->type != ui.slot[slot].type
        );

    if (recreate) {
        /* XXX: There's apparently a bug in some Intel OpenGL drivers for
         * Windows that will leak this texture when its orphaned after use in
         * another context, apparently regardless of which thread it's created
         * or released on.
         *
         * Driver: 27.20.100.8729 9/11/2020 W10 x64
         * Track: https://community.intel.com/t5/Graphics/OpenGL-Windows-drivers-for-Intel-HD-630-leaking-GPU-memory-when/td-p/1274423
         */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        ui.slot[slot].internal_format = p->internal_format;
        ui.slot[slot].width = width;
        ui.slot[slot].height = height;
        ui.slot[slot].format = p->format;
        ui.slot[slot].type = p->type;
        glTexImage2D(GL_TEXTURE_2D, 0, ui.slot[slot].internal_format,
                     ui.slot[slot].width, ui.slot[slot].height, 0,
                     ui.slot[slot].format, ui.slot[slot].type, NULL);
    }

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, ui.slot[slot].tex, 0);
    GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, DrawBuffers);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindTexture(GL_TEXTURE_2D, p->tex);
    glBindVertexArray(ui.vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui.vbo);
    glUseProgram(ui.prog);
    glProgramUniform1i(ui.prog, ui.tex_loc, 0);
    glUniform2f(ui.display_size_loc, width, height);
    glUniform1f(ui.line_offset_loc, line_offset);
    ui_pvideo_overlay(d, p, ui.slot[slot].height);

    glViewport(0, 0, width, height);
    glColorMask(true, true, true, true);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

void pgraph_gl_init_display(NV2AState *d)
{
    struct PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    r->disp_check_pending = false;
    r->disp_last_surface = NULL;
    r->disp_post_pending = false;
    r->disp_shown = false;
    qemu_mutex_init(&r->disp_lock);
    qemu_mutex_init(&r->disp_post_lock);
}

/* Render thread: a ticket the UI thread will never resolve. */
static void disp_post_release(PGRAPHGLState *r, const DispPost *p)
{
    if (p->rendered) {
        glDeleteSync(p->rendered);
    }
    if (p->ref) {
        qemu_mutex_lock(&r->disp_lock);
        disp_ref_put_locked(p->ref);
        qemu_mutex_unlock(&r->disp_lock);
    }
}

/* Render thread: mailbox with one slot. A ticket the UI thread has not
 * taken is superseded (that frame is never presented). */
static void disp_post_ticket(PGRAPHGLState *r, const DispPost *p)
{
    qemu_mutex_lock(&r->disp_post_lock);
    if (r->disp_post_pending) {
        disp_post_release(r, &r->disp_post);
    }
    r->disp_post = *p;
    r->disp_post_pending = true;
    qemu_mutex_unlock(&r->disp_post_lock);
}

/* Render thread, after the surfaces are gone. */
void pgraph_gl_finalize_display(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (r->disp_post_pending) {
        r->disp_post_pending = false;
        disp_post_release(r, &r->disp_post);
    }
    qemu_mutex_destroy(&r->disp_post_lock);
    qemu_mutex_destroy(&r->disp_lock);
}

/* Render thread, pgraph.lock held. Post the scanout to the UI thread
 * when it changed. Returns false without a scanout surface (VGA path);
 * *fresh tells whether a ticket was posted. */
static bool post_scanout(NV2AState *d, bool *fresh)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    *fresh = false;
    SurfaceBinding *surface = pgraph_gl_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color || !surface->width ||
        !surface->height) {
        qatomic_set(&r->scanout_found, false);
        r->disp_last_surface = NULL;
        if (r->disp_shown) {
            /* The CRTC left the surfaces (a CPU-drawn frame): an empty ticket
             * sends the UI thread back to the VGA path. */
            r->disp_shown = false;
            DispPost none = { 0 };
            disp_post_ticket(r, &none);
        }
        return false;
    }
    qatomic_set(&r->scanout_found, true);
    surface->frame_time = d->pgraph.frame_time;

    /* FIXME: Sanity check surface dimensions */

    /* The last ticket already carried this very frame: nothing to do. */
    bool pvideo = (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE) &&
                  d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    int disp_width, disp_height;
    d->vga.get_resolution(&d->vga, &disp_width, &disp_height);
    bool interlaced = d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
                      NV_PRMCIO_INTERLACE_MODE_DISABLED;
    if (r->disp_last_surface != NULL &&
        surface == r->disp_last_surface &&
        surface->draw_time == r->disp_last_draw_time &&
        !surface->upload_pending && !pvideo &&
        (int)vga_display_params.line_offset == r->disp_last_line_offset &&
        disp_width == r->disp_last_width &&
        disp_height == r->disp_last_height &&
        interlaced == r->disp_last_interlaced &&
        d->pgraph.surface_scale_factor == r->disp_last_scale) {
        return true;
    }

    pgraph_gl_upload_surface_data(d, surface, !tcg_enabled());

    if (surface->disp_ref == NULL) {
        surface->disp_ref = g_new0(DispRef, 1);
        surface->disp_ref->tex = surface->gl_buffer;
    }

    DispPost p = {
        .ref = surface->disp_ref,
        .tex = surface->gl_buffer,
        .draw_time = surface->draw_time,
        .width = surface->width,
        .height = surface->height,
        .pitch = surface->pitch,
        .internal_format = surface->fmt.gl_internal_format,
        .format = surface->fmt.gl_format,
        .type = surface->fmt.gl_type,
        .line_offset = vga_display_params.line_offset,
        .disp_width = disp_width,
        .disp_height = disp_height,
        .interlaced = interlaced,
        .scale = d->pgraph.surface_scale_factor,
        .pvideo = pvideo,
    };
    if (pvideo) {
        const uint32_t *regs = d->pvideo.regs;
        p.pv = (DispPvideo){
            .buffer = regs[NV_PVIDEO_BUFFER],
            .size_in = regs[NV_PVIDEO_SIZE_IN],
            .base = regs[NV_PVIDEO_BASE],
            .limit = regs[NV_PVIDEO_LIMIT],
            .offset = regs[NV_PVIDEO_OFFSET],
            .point_in = regs[NV_PVIDEO_POINT_IN],
            .ds_dx = regs[NV_PVIDEO_DS_DX],
            .dt_dy = regs[NV_PVIDEO_DT_DY],
            .point_out = regs[NV_PVIDEO_POINT_OUT],
            .size_out = regs[NV_PVIDEO_SIZE_OUT],
            .format = regs[NV_PVIDEO_FORMAT],
            .color_key = regs[NV_PVIDEO_COLOR_KEY],
        };
    }
    /* The frame's draws are queued on the render context: the UI thread's
     * blit waits for this on the GPU, not this thread. */
    p.rendered = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    qemu_mutex_lock(&r->disp_lock);
    p.ref->refs++;
    qemu_mutex_unlock(&r->disp_lock);

    disp_post_ticket(r, &p);
    r->disp_shown = true;

    r->disp_last_surface = surface;
    r->disp_last_draw_time = surface->draw_time;
    r->disp_last_line_offset = vga_display_params.line_offset;
    r->disp_last_width = disp_width;
    r->disp_last_height = disp_height;
    r->disp_last_interlaced = interlaced;
    r->disp_last_scale = d->pgraph.surface_scale_factor;
    *fresh = true;
    return true;
}

/* The UI thread's request path: post, then wake it. */
void pgraph_gl_sync(NV2AState *d)
{
    bool fresh;
    post_scanout(d, &fresh);
    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

/* Post on a scanout change (PCRTC_START write) or on the UI thread's
 * re-check; neither waits for this thread. */
void pgraph_gl_present_scanout(NV2AState *d)
{
    bool fresh;
    post_scanout(d, &fresh);
}

/* UI thread: take the pending ticket, if any. */
static bool disp_take_post(PGRAPHGLState *r, DispPost *p)
{
    bool got = false;
    qemu_mutex_lock(&r->disp_post_lock);
    if (r->disp_post_pending) {
        *p = r->disp_post;
        r->disp_post_pending = false;
        got = true;
    }
    qemu_mutex_unlock(&r->disp_post_lock);
    return got;
}

int pgraph_gl_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* The surface list belongs to the render thread; walking it here
     * raced against insertions and evictions. Only consult the atomic
     * emptiness hint -- the render thread resolves the actual scanout on
     * its own thread and posts it. An empty list (early boot, VGA text)
     * keeps the no-kick behaviour, so this never waits on a renderer with
     * nothing to say. */
    if (!qatomic_read(&r->have_surfaces)) {
        return 0;
    }

    DispPost p;
    bool got = disp_take_post(r, &p);
    if (!got && ui.cur < 0) {
        /* Nothing on screen yet (first frames, VGA text): ask the render
         * thread for a ticket and wait for it. */
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&d->pgraph.sync_complete);
        qatomic_set(&pg->sync_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&d->pgraph.sync_complete);
        if (!qatomic_read(&r->scanout_found)) {
            return 0;
        }
        got = disp_take_post(r, &p);
    } else if (!got) {
        /* The front buffer may change without a scanout move (guest CPU
         * writes, video overlay): have the render thread look, later. */
        qatomic_set(&r->disp_check_pending, true);
        qemu_mutex_lock(&d->pfifo.lock);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
    }
    if (got && p.ref == NULL) {
        /* No front buffer among the surfaces: back to the VGA path. */
        ui.cur = -1;
        got = false;
    }
    if (got) {
        if (!ui.ready) {
            ui_init();
        }
        int slot = ui.cur < 0 ? 0 : 1 - ui.cur;
        ui_resolve(d, &p, slot);
        /* The render thread waits for this before writing the surface
         * again (pgraph_gl_display_surface_reuse). */
        GLsync presented = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        qemu_mutex_lock(&r->disp_lock);
        if (p.ref->presented) {
            glDeleteSync(p.ref->presented);
        }
        p.ref->presented = presented;
        disp_ref_put_locked(p.ref);
        qemu_mutex_unlock(&r->disp_lock);
        ui.cur = slot;
        ui.cur_draw_time = p.draw_time;
        if (p.pvideo) {
            /* The overlay is read from guest RAM at each resolve: ask
             * for the next one now, not at the next scanout move. */
            qatomic_set(&r->disp_check_pending, true);
            qemu_mutex_lock(&d->pfifo.lock);
            pfifo_kick(d);
            qemu_mutex_unlock(&d->pfifo.lock);
        }
    }
    if (ui.cur < 0) {
        return 0;
    }
    return ui.slot[ui.cur].tex;
}
