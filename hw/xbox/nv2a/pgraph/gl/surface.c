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

#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "debug.h"
#include "renderer.h"

static void surface_download(NV2AState *d, SurfaceBinding *surface, bool force);
static bool check_surface_compatibility(SurfaceBinding *s1, SurfaceBinding *s2,
                                        bool strict);
static void surface_download_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       bool swizzle, bool flip, bool downscale,
                                       uint8_t *pixels);
static void surface_get_dimensions(PGRAPHState *pg, unsigned int *width, unsigned int *height);
static void fb_cache_drop_tex(PGRAPHGLState *r, GLuint tex);
static void fb_multi_drop_tex(PGRAPHGLState *r, GLuint tex);
static void bind_current_surface(NV2AState *d);
static void fb_bind(PGRAPHGLState *r, GLuint fbo);
static void surface_evict_eager(NV2AState *d, SurfaceBinding *s);
static void park_owed(NV2AState *d, SurfaceBinding *surface);
/* What a create found owed over its range (parked_settle_for_create). */
typedef enum {
    SETTLED_NOTHING,   /* nothing carried */
    SETTLED_RAM,       /* RAM refreshed for the create's upload */
    SETTLED_TEXTURE,   /* the texture holds the content, ahead of RAM */
} SettleResult;

static SettleResult parked_settle_for_create(NV2AState *d,
                                             SurfaceBinding *neu);

/* Clear only while no surface or parked binding is ahead of RAM: a CPU read
 * of VRAM then leaves the access callback before taking the mutex. */
static int surfaces_any_dirty = 1;

/* Retired surface textures (r->texpool), kept for the next binding with the
 * same storage: a buffer alternating between two views is recreated every
 * frame, and fresh storage each time stalls the driver (vk/surface.c:
 * invalid_surfaces). Handed back only at the same address and storage: a
 * recycled texture still holds its last owner's pixels, the image of that
 * same range. */
static GLuint texpool_take(PGRAPHGLState *r, hwaddr addr, unsigned int w,
                           unsigned int h, GLint fmt)
{
    for (unsigned i = 0; i < r->texpool_n; i++) {
        if (r->texpool[i].addr == addr && r->texpool[i].w == w &&
            r->texpool[i].h == h && r->texpool[i].fmt == fmt) {
            GLuint tex = r->texpool[i].tex;
            r->texpool[i] = r->texpool[--r->texpool_n];
            return tex;
        }
    }
    return 0;
}

static void texpool_put(PGRAPHGLState *r, GLuint tex, hwaddr addr,
                        unsigned int w, unsigned int h, GLint fmt)
{
    if (!tex || !w || !h) {
        glDeleteTextures(1, &tex);
        return;
    }
    if (r->texpool_n == ARRAY_SIZE(r->texpool)) {
        glDeleteTextures(1, &r->texpool[0].tex);
        r->texpool[0] = r->texpool[--r->texpool_n];
    }
    r->texpool[r->texpool_n].tex = tex;
    r->texpool[r->texpool_n].addr = addr;
    r->texpool[r->texpool_n].w = w;
    r->texpool[r->texpool_n].h = h;
    r->texpool[r->texpool_n].fmt = fmt;
    r->texpool_n++;
}

/* Deferred eviction writebacks: an evicted binding whose writeback waits
 * parks in r->parked, texture alive, writeback_owed set while RAM is behind
 * it. The first consumer of that RAM settles the debt: the texture upload of
 * the range (gl/texture.c), a CPU access, the drain before a snapshot or a
 * scale change, or a binding created over it. A range a real consumer read
 * is learnt live, and its later evictions write back eagerly. */
#define PARKED_MAX 8

void pgraph_gl_set_surface_scale_factor(NV2AState *d, unsigned int scale)
{
    /* External texture binds below desync the per-unit shadow. */
    pgraph_gl_tex_shadow_invalidate(d->pgraph.gl_renderer_state);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, true);
    qemu_mutex_unlock(&d->pfifo.lock);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&r->dirty_surfaces_download_complete);
    qatomic_set(&r->download_dirty_surfaces_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&r->dirty_surfaces_download_complete);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.flush_complete);

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, false);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

unsigned int pgraph_gl_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor;
}

void pgraph_gl_reload_surface_scale_factor(PGRAPHState *pg)
{
    int factor = g_config.display.quality.surface_scale;
    pg->surface_scale_factor = factor < 1 ? 1 : factor;
}

// FIXME: Move to common
static bool framebuffer_dirty(PGRAPHState *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed || (!pg->surface_shape.color_format
            && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

/* The surface is ahead of RAM: a CPU read of it has to reach the download
 * path again, past the any-dirty shortcut and a disarmed read watch. */
void pgraph_gl_surface_mark_ahead(SurfaceBinding *s)
{
    qatomic_set(&surfaces_any_dirty, 1);
    if (s->watch_read_off) {
        s->watch_read_off = false;
        mem_access_callback_set_flags(qemu_get_cpu(0), s->access_cb,
                                      BP_MEM_READ | BP_MEM_WRITE);
    }
}

void pgraph_gl_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n",
                 color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));
    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (color || zeta) {
        if (r->color_binding) {
            pgraph_gl_surface_mark_ahead(r->color_binding);
        }
        if (r->zeta_binding) {
            pgraph_gl_surface_mark_ahead(r->zeta_binding);
        }
    }
    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;

    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;

    }
}

static void init_render_to_texture(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    const char *vs =
        "#version 330\n"
        "void main()\n"
        "{\n"
        "    float x = -1.0 + float((gl_VertexID & 1) << 2);\n"
        "    float y = -1.0 + float((gl_VertexID & 2) << 1);\n"
        "    gl_Position = vec4(x, y, 0, 1);\n"
        "}\n";
    const char *fs =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "uniform vec2 surface_size;\n"
        "layout(location = 0) out vec4 out_Color;\n"
        "void main()\n"
        "{\n"
        "    vec2 texCoord = gl_FragCoord.xy / textureSize(tex, 0).xy;\n"
        "    out_Color.rgba = texture(tex, texCoord);\n"
        "}\n";

    r->s2t_rndr.prog = pgraph_gl_compile_shader(vs, fs);
    r->s2t_rndr.tex_loc = glGetUniformLocation(r->s2t_rndr.prog, "tex");
    r->s2t_rndr.surface_size_loc = glGetUniformLocation(r->s2t_rndr.prog,
                                                    "surface_size");

    /* Depth surface sampled as a raw A8R8G8B8 texture, the Z buffer's bits
     * read as colour (OutRun 2 reflections), packed on the GPU like the slow
     * path's texel: Z24S8 read back as UNSIGNED_INT_24_8 and re-uploaded as
     * BGRA/8_8_8_8_REV gives RGBA8 = (d[15:8], d[7:0], stencil, d[23:16]).
     * HEURISTIC: the stencil byte reads as 0, as GL 3.3 cannot sample it. */
    const char *fs_pack =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "layout(location = 0) out vec4 out_Color;\n"
        "void main()\n"
        "{\n"
        "    vec2 uv = gl_FragCoord.xy / textureSize(tex, 0).xy;\n"
        "    uint d = uint(texture(tex, uv).r * 16777215.0 + 0.5);\n"
        "    out_Color = vec4(float((d >> 8) & 0xFFu),\n"
        "                     float( d       & 0xFFu),\n"
        "                     0.0,\n"
        "                     float((d >> 16) & 0xFFu)) / 255.0;\n"
        "}\n";
    r->s2t_rndr.pack_prog = pgraph_gl_compile_shader(vs, fs_pack);
    r->s2t_rndr.pack_tex_loc = glGetUniformLocation(r->s2t_rndr.pack_prog, "tex");

    /* Scaled depth surface -> native-1x depth texture, as the guest samples it
     * (a scaled one shifts precise depth compares: OutRun 2's car shadow).
     * texelFetch at gl_FragCoord * scale takes each block's top-left texel,
     * like surface_copy_shrink_row; surface_shrink_on_gpu takes its centre. */
    const char *fs_depth =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "uniform int uscale;\n"
        "void main()\n"
        "{\n"
        "    gl_FragDepth = texelFetch(tex,\n"
        "        ivec2(gl_FragCoord.xy) * uscale, 0).r;\n"
        "}\n";
    /* The inverse of fs_pack, same byte order: OutRun 2 writes the scene depth
     * as colour into a 160x120 buffer, then binds it as the next pass's zeta
     * (the game's function at 0x00076014:
     * SetRenderTarget(B, 0), draw, SetRenderTarget(A, B)). The stencil byte
     * cannot be written from a fragment shader in core GL: it is lost. */
    const char *fs_unpack =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "void main()\n"
        "{\n"
        "    vec2 uv = gl_FragCoord.xy / textureSize(tex, 0).xy;\n"
        "    vec4 c = floor(texture(tex, uv) * 255.0 + 0.5);\n"
        "    uint d = (uint(c.a) << 16) | (uint(c.r) << 8) | uint(c.g);\n"
        "    gl_FragDepth = float(d) / 16777215.0;\n"
        "}\n";
    r->s2t_rndr.unpack_prog = pgraph_gl_compile_shader(vs, fs_unpack);
    r->s2t_rndr.unpack_tex_loc =
        glGetUniformLocation(r->s2t_rndr.unpack_prog, "tex");

    r->s2t_rndr.depth_prog = pgraph_gl_compile_shader(vs, fs_depth);
    r->s2t_rndr.depth_tex_loc =
        glGetUniformLocation(r->s2t_rndr.depth_prog, "tex");
    r->s2t_rndr.depth_scale_loc =
        glGetUniformLocation(r->s2t_rndr.depth_prog, "uscale");

    glGenVertexArrays(1, &r->s2t_rndr.vao);
    glBindVertexArray(r->s2t_rndr.vao);
    glGenBuffers(1, &r->s2t_rndr.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->s2t_rndr.vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STATIC_DRAW);
    glGenFramebuffers(1, &r->s2t_rndr.fbo);
}

static void finalize_render_to_texture(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    glDeleteProgram(r->s2t_rndr.prog);
    r->s2t_rndr.prog = 0;

    glDeleteVertexArrays(1, &r->s2t_rndr.vao);
    r->s2t_rndr.vao = 0;

    glDeleteBuffers(1, &r->s2t_rndr.vbo);
    r->s2t_rndr.vbo = 0;

    glDeleteFramebuffers(1, &r->s2t_rndr.fbo);
    r->s2t_rndr.fbo = 0;

    glDeleteProgram(r->s2t_rndr.pack_prog);
    glDeleteProgram(r->s2t_rndr.unpack_prog);
    glDeleteProgram(r->s2t_rndr.depth_prog);
    r->s2t_rndr.pack_prog = r->s2t_rndr.unpack_prog = 0;
    r->s2t_rndr.depth_prog = 0;
}

static bool surface_to_texture_can_fastpath(SurfaceBinding *surface,
                                            TextureShape *shape)
{
    // FIXME: Better checks/handling on formats and surface-texture compat

    int surface_fmt = surface->shape.color_format;
    int texture_fmt = shape->color_format;

    if (!surface->color) {
        // FIXME: Support zeta to color
        return false;
    }

    switch (surface_fmt) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8: switch(texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8: return true;
        default: break;
        }
        break;
    default: break;
    }

    trace_nv2a_pgraph_surface_texture_compat_failed(
        surface_fmt, texture_fmt);
    return false;
}

/* xemu's own fullscreen pass: none of the game's raster state applies (the
 * draw state cache is told it changed). */
static void s2t_pass_state(PGRAPHGLState *r)
{
    pgraph_gl_draw_state_invalidate(r);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

/* After an own pass: the game's target, vertex array and program again,
 * and its cost segment if the pass suspended one. */
static void s2t_pass_end(NV2AState *d, bool cost_suspended)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    bind_current_surface(d);
    glBindVertexArray(r->gl_vertex_array);
    glUseProgram(r->shader_binding ? r->shader_binding->gl_program : 0);
    if (cost_suspended) {
        pgraph_gl_own_draw_resume(d);
    }
}

/* Sample the surface bound on GL_TEXTURE_2D as stored: the depth of a
 * depth-stencil image, no depth compare, point sampled. */
static void tex_sample_raw(const SurfaceBinding *surface)
{
    if (surface->fmt.gl_attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE,
                        GL_DEPTH_COMPONENT);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static void render_surface_to(NV2AState *d, SurfaceBinding *surface,
                              int texture_unit, GLuint gl_target,
                              GLuint gl_texture, unsigned int width,
                              unsigned int height, bool pack)
{
    /* External texture binds below desync the per-unit shadow. */
    pgraph_gl_tex_shadow_invalidate(d->pgraph.gl_renderer_state);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* Keep this emulation-only blit out of the zpass count and the cost
     * model's segment: the silicon samples the framebuffer in place and
     * rasterises nothing here (see pgraph_gl_own_draw_suspend). */
    bool cost_suspended = pgraph_gl_own_draw_suspend(d);

    glActiveTexture(GL_TEXTURE0 + texture_unit);
    fb_bind(r, r->s2t_rndr.fbo);

    GLenum draw_buffers[1] = { GL_COLOR_ATTACHMENT0 };
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, gl_target,
                           gl_texture, 0);
    glDrawBuffers(1, draw_buffers);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    assert(glGetError() == GL_NO_ERROR);

    float color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glBindTexture(GL_TEXTURE_2D, surface->gl_buffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, color);
    if (pack) {
        tex_sample_raw(surface);
    }

    GLuint prog = pack ? r->s2t_rndr.pack_prog : r->s2t_rndr.prog;
    glBindVertexArray(r->s2t_rndr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->s2t_rndr.vbo);
    glUseProgram(prog);
    glProgramUniform1i(prog,
                       pack ? r->s2t_rndr.pack_tex_loc : r->s2t_rndr.tex_loc,
                       texture_unit);
    if (!pack) {
        glProgramUniform2f(r->s2t_rndr.prog,
                           r->s2t_rndr.surface_size_loc, width, height);
    }

    glViewport(0, 0, width, height);
    s2t_pass_state(r);
    glColorMask(true, true, true, true);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, gl_target, 0,
                           0);
    /* A cube face is attached by its face target but bound as a cube. */
    GLenum bind_target = (gl_target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
                          gl_target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) ?
                             GL_TEXTURE_CUBE_MAP : gl_target;
    glBindTexture(bind_target, gl_texture);
    s2t_pass_end(d, cost_suspended);
}

static void render_surface_to_texture_slow(NV2AState *d,
                                           SurfaceBinding *surface,
                                           TextureBinding *texture,
                                           TextureShape *texture_shape,
                                           int texture_unit)
{
    PGRAPHState *pg = &d->pgraph;

    const ColorFormatInfo *f = &kelvin_color_format_gl_map[texture_shape->color_format];
    assert(texture_shape->color_format < ARRAY_SIZE(kelvin_color_format_gl_map));
    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX_FALLBACK);

    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glBindTexture(texture->gl_target, texture->gl_texture);

    unsigned int width = surface->width,
                 height = surface->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    size_t bufsize = width * height * surface->fmt.bytes_per_pixel;

    uint8_t *buf = g_malloc(bufsize);
    surface_download_to_buffer(d, surface, false, false, false, buf);

    width = texture_shape->width;
    height = texture_shape->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    glTexImage2D(texture->gl_target, 0, f->gl_internal_format, width, height, 0,
                 f->gl_format, f->gl_type, buf);
    texture->s2t_width = width;
    texture->s2t_height = height;
    texture->s2t_format = f->gl_internal_format;
    g_free(buf);
    glBindTexture(texture->gl_target, texture->gl_texture);
}

/* Note: This function is intended to be called before PGRAPH configures GL
 * state for rendering; it will configure GL state here but only restore a
 * couple of items.
 */
void pgraph_gl_render_surface_to_texture(NV2AState *d, SurfaceBinding *surface,
                                      TextureBinding *texture,
                                      TextureShape *texture_shape,
                                      int texture_unit)
{
    /* External texture binds below desync the per-unit shadow. */
    pgraph_gl_tex_shadow_invalidate(d->pgraph.gl_renderer_state);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    const ColorFormatInfo *f =
        &kelvin_color_format_gl_map[texture_shape->color_format];
    assert(texture_shape->color_format < ARRAY_SIZE(kelvin_color_format_gl_map));

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    /* surface_to_texture_can_fastpath refuses every depth surface: only colour
     * takes this gate, and depth goes to its GPU paths below (native-1x depth,
     * or the packing shader for raw Z read as A8R8G8B8). */
    if (surface->color &&
        !surface_to_texture_can_fastpath(surface, texture_shape)) {
        render_surface_to_texture_slow(d, surface, texture,
                                              texture_shape, texture_unit);
        return;
    }

    if (!surface->color && (f->gl_format == GL_DEPTH_STENCIL ||
                            f->gl_format == GL_DEPTH_COMPONENT)) {
        /* Native-1x depth texture through fs_depth, on the GPU. A8R8G8B8 (raw
         * Z bits as colour) falls through to the packing shader. */
        unsigned int nw = texture_shape->width, nh = texture_shape->height;

        glActiveTexture(GL_TEXTURE0 + texture_unit);
        glBindTexture(texture->gl_target, texture->gl_texture);
        glTexParameteri(texture->gl_target, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        if (texture->s2t_width != nw || texture->s2t_height != nh ||
            texture->s2t_format != GL_DEPTH_COMPONENT24) {
            glTexImage2D(texture->gl_target, 0, GL_DEPTH_COMPONENT24, nw, nh, 0,
                         GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
            texture->s2t_width = nw;
            texture->s2t_height = nh;
            texture->s2t_format = GL_DEPTH_COMPONENT24;
        }
        glBindTexture(texture->gl_target, 0);

        bool cost_suspended = pgraph_gl_own_draw_suspend(d);
        fb_bind(r, r->s2t_rndr.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, texture->gl_texture, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);

        /* Source: the depth surface, sampled as depth, point-sampled. */
        glActiveTexture(GL_TEXTURE0 + texture_unit);
        glBindTexture(GL_TEXTURE_2D, surface->gl_buffer);
        tex_sample_raw(surface);

        glUseProgram(r->s2t_rndr.depth_prog);
        glProgramUniform1i(r->s2t_rndr.depth_prog, r->s2t_rndr.depth_tex_loc,
                           texture_unit);
        glProgramUniform1i(r->s2t_rndr.depth_prog, r->s2t_rndr.depth_scale_loc,
                           pg->surface_scale_factor);
        glBindVertexArray(r->s2t_rndr.vao);
        glViewport(0, 0, nw, nh);
        s2t_pass_state(r);
        glColorMask(false, false, false, false);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, 0, 0);
        texture->scale = 1;
        glActiveTexture(GL_TEXTURE0 + texture_unit);
        glBindTexture(texture->gl_target, texture->gl_texture);
        s2t_pass_end(d, cost_suspended);
        return;
    }

    unsigned int width = texture_shape->width, height = texture_shape->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glBindTexture(texture->gl_target, texture->gl_texture);
    glTexParameteri(texture->gl_target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(texture->gl_target, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(texture->gl_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    if (texture->s2t_width != width || texture->s2t_height != height ||
        texture->s2t_format != (GLint)f->gl_internal_format) {
        glTexImage2D(texture->gl_target, 0, f->gl_internal_format, width,
                     height, 0, f->gl_format, f->gl_type, NULL);
        texture->s2t_width = width;
        texture->s2t_height = height;
        texture->s2t_format = f->gl_internal_format;
    }
    glBindTexture(texture->gl_target, 0);
    render_surface_to(d, surface, texture_unit, texture->gl_target,
                             texture->gl_texture, width, height,
                             !surface->color);
    glBindTexture(texture->gl_target, texture->gl_texture);
    glUseProgram(
        r->shader_binding ? r->shader_binding->gl_program : 0);
}

/* A cubemap whose six faces are render surfaces, converted face by face on
 * the GPU like the 2D path: the NV2A samples them in place. Without GPU
 * boost each face is read back through RAM instead. */
void pgraph_gl_render_cube_faces_to_texture(NV2AState *d,
                                            SurfaceBinding *faces[6],
                                            TextureBinding *texture,
                                            TextureShape *shape,
                                            int texture_unit)
{
    pgraph_gl_tex_shadow_invalidate(d->pgraph.gl_renderer_state);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    const ColorFormatInfo *f = &kelvin_color_format_gl_map[shape->color_format];
    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    unsigned int width = faces[0]->width, height = faces[0]->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture->gl_texture);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    if (texture->s2t_width != width || texture->s2t_height != height ||
        texture->s2t_format != (GLint)f->gl_internal_format) {
        for (int i = 0; i < 6; i++) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0,
                         f->gl_internal_format, width, height, 0,
                         f->gl_format, f->gl_type, NULL);
        }
        texture->s2t_width = width;
        texture->s2t_height = height;
        texture->s2t_format = f->gl_internal_format;
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    for (int i = 0; i < 6; i++) {
        render_surface_to(d, faces[i], texture_unit,
                          GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
                          texture->gl_texture, width, height, false);
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture->gl_texture);
    glUseProgram(r->shader_binding ? r->shader_binding->gl_program : 0);
}

bool pgraph_gl_check_surface_to_texture_compatibility(
    const SurfaceBinding *surface,
    const TextureShape *shape)
{
    // FIXME: Better checks/handling on formats and surface-texture compat

    if ((!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height) {
        return false;
    }

    int surface_fmt = surface->shape.color_format;
    int texture_fmt = shape->color_format;

    if (!surface->color) {
        /* Converted on the GPU for these reads (see
         * pgraph_gl_render_surface_to_texture); any other goes through RAM. */
        switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED:
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED:
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT:
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED:
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT:
            return true;  /* native-1x depth downsample */
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
            /* raw Z24S8 bits -> RGBA via packing shader */
            return surface->fmt.bytes_per_pixel == 4;
        default:
            return false;
        }
    }

    if (shape->cubemap) {
        // FIXME: Support rendering surface to cubemap face
        return false;
    }

    if (shape->levels > 1) {
        // FIXME: Support rendering surface to mip levels
        return false;
    }

    switch (surface_fmt) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8: switch(texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8: return true;
        default: break;
        }
        break;
    default:
        break;
    }

    trace_nv2a_pgraph_surface_texture_compat_failed(
        surface_fmt, texture_fmt);
    return false;
}

static bool check_surface_overlaps_range(const SurfaceBinding *surface,
                                         hwaddr range_start, hwaddr range_len)
{
    hwaddr surface_end = surface->vram_addr + surface->size;
    hwaddr range_end = range_start + range_len;
    return !(surface->vram_addr >= range_end || range_start >= surface_end);
}

static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;
    if (!write && !qatomic_read(&surfaces_any_dirty)) {
        return;
    }
    qemu_mutex_lock(&d->pgraph.lock);

    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    bool wait_for_downloads = false;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!check_surface_overlaps_range(surface, addr, len)) {
            continue;
        }

        hwaddr offset = addr - surface->vram_addr;

        if (write) {
            trace_nv2a_pgraph_surface_cpu_write(surface->vram_addr, offset);
        } else {
            trace_nv2a_pgraph_surface_cpu_read(surface->vram_addr, offset);
        }

        if (surface->draw_dirty) {
            surface->download_pending = true;
            surface->cpu_reads++;
            wait_for_downloads = true;
        } else if (!write) {
            /* Clean: stop watching reads, each of which is a fault; writes
             * stay watched. */
            if (!surface->watch_read_off) {
                surface->watch_read_off = true;
                mem_access_callback_set_flags(qemu_get_cpu(0),
                                              surface->access_cb,
                                              BP_MEM_WRITE);
            }
        }

        if (write) {
            surface->upload_pending = true;
            /* One write is enough to know a re-upload is needed: stop
             * watching until the upload consumes it (RAM is the truth). */
            if (!surface->watch_read_off) {
                surface->watch_read_off = true;
                mem_access_callback_set_flags(qemu_get_cpu(0),
                                              surface->access_cb, 0);
            }
        }
    }

    /* RAM a parked binding still owes: queue its writeback on the render
     * thread (no GL here) and learn the range live. A write waits too, so it
     * lands on top of the written-back image. */
    SurfaceBinding *parked;
    QTAILQ_FOREACH(parked, &r->parked, entry) {
        if (parked->writeback_owed &&
            check_surface_overlaps_range(parked, addr, len)) {
            parked->download_pending = true;
            wait_for_downloads = true;
            pgraph_writeback_learn_live(&d->pgraph, parked->vram_addr,
                                        parked->size);
        }
    }

    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait_for_downloads) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->downloads_complete);
        qatomic_set(&r->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        /* A DMA write from the main loop arrives here holding the BQL: let
         * go of it while waiting, as a reset does, so the main loop (the
         * pgraph interrupt bottom half) runs meanwhile. */
        bool bql = bql_locked();
        if (bql) {
            bql_unlock();
        }
        qemu_event_wait(&r->downloads_complete);
        if (bql) {
            bql_lock();
        }
    }
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (tcg_enabled()) {
        if (surface->width && surface->height) {
            surface->access_cb = mem_access_callback_insert(
                qemu_get_cpu(0), d->vram, surface->vram_addr, surface->size,
                &surface_access_callback, d);
        } else {
            surface->access_cb = NULL;
        }
    }
}

static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding const *surface)
{
    if (tcg_enabled()) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
    }
}

static bool check_surfaces_overlap(const SurfaceBinding *surface,
                                   const SurfaceBinding *other_surface)
{
    return check_surface_overlaps_range(surface, other_surface->vram_addr,
                                        other_surface->size);
}

static void invalidate_overlapping_surfaces(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding *other_surface, *next_surface;
    QTAILQ_FOREACH_SAFE(other_surface, &r->surfaces, entry, next_surface) {
        if (check_surfaces_overlap(surface, other_surface)) {
            trace_nv2a_pgraph_surface_evict_overlapping(
                other_surface->vram_addr, other_surface->width, other_surface->height,
                other_surface->pitch);
            if (!pgraph_writeback_dead(d, other_surface->vram_addr,
                                       other_surface->size,
                                       other_surface->cpu_reads)) {
                surface_evict_eager(d, other_surface);
            } else {
                pgraph_writeback_record_discard(&d->pgraph,
                                                other_surface->vram_addr,
                                                other_surface->size);
                park_owed(d, other_surface);
            }
        }
    }
}

static SurfaceBinding *surface_put(NV2AState *d, hwaddr addr,
                                   SurfaceBinding *surface_in)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    assert(pgraph_gl_surface_get(d, addr) == NULL);

    invalidate_overlapping_surfaces(d, surface_in);
    SurfaceBinding *surface_out = g_malloc(sizeof(SurfaceBinding));
    assert(surface_out != NULL);
    *surface_out = *surface_in;
    surface_out->disp_ref = NULL; /* the display never saw it */
    surface_out->cpu_reads = 0;
    surface_out->cpu_reads_last_flip = 0;
    surface_out->dl_pbo = 0;
    surface_out->dl_pbo_size = 0;
    surface_out->dl_fence = NULL;
    surface_out->watch_read_off = false;
    surface_out->writeback_owed = false;

    SettleResult settled = parked_settle_for_create(d, surface_out);
    if (settled == SETTLED_RAM) {
        /* The owed writeback just refreshed this range's RAM: the create
         * upload must read it (this also voids the skipped upload of a
         * discarded range). */
        surface_out->upload_pending = true;
    } else if (settled == SETTLED_TEXTURE) {
        /* Fed on the GPU: the texture already holds the content, RAM
         * does not. */
        surface_out->upload_pending = false;
        surface_out->draw_dirty = true;
        surface_out->draw_time = pg->draw_time;
        pgraph_gl_surface_mark_ahead(surface_out);
    }

    register_cpu_access_callback(d, surface_out);

    QTAILQ_INSERT_TAIL(&r->surfaces, surface_out, entry);
    qatomic_set(&r->have_surfaces, true);

    return surface_out;
}

/* A new binding's texture, from the pool or made at its scaled size, left
 * bound on the active unit (the per-unit shadow is told). */
static void surface_alloc_texture(PGRAPHState *pg, SurfaceBinding *entry)
{
    unsigned int tw = entry->width ? entry->width : 1;
    unsigned int th = entry->height ? entry->height : 1;
    pgraph_apply_scaling_factor(pg, &tw, &th);
    entry->tex_w = tw;
    entry->tex_h = th;
    entry->tex_fmt = entry->fmt.gl_internal_format;
    entry->gl_buffer = texpool_take(pg->gl_renderer_state, entry->vram_addr,
                                    tw, th, entry->tex_fmt);
    bool reused = entry->gl_buffer != 0;
    if (!reused) {
        glGenTextures(1, &entry->gl_buffer);
    }
    glBindTexture(GL_TEXTURE_2D, entry->gl_buffer);
    pgraph_gl_tex_shadow_invalidate(pg->gl_renderer_state);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    if (!reused) {
        glTexImage2D(GL_TEXTURE_2D, 0, entry->fmt.gl_internal_format, tw, th,
                     0, entry->fmt.gl_format, entry->fmt.gl_type, NULL);
    }
}

/* A surface for a 2D blit destination that has none, so the GPU copy serves
 * the blit. The caller asks only when its copy writes every pixel, so nothing
 * comes from RAM, and marks the result draw_dirty. */
SurfaceBinding *pgraph_gl_surface_create_blit_dest(NV2AState *d, hwaddr addr,
                                                   const SurfaceBinding *src,
                                                   unsigned int pitch,
                                                   unsigned int width,
                                                   unsigned int height)
{
    PGRAPHState *pg = &d->pgraph;

    SurfaceBinding entry;
    memset(&entry, 0, sizeof(entry));
    entry.shape = src->shape;
    entry.fmt = src->fmt;
    entry.color = true;
    entry.swizzle = false;
    entry.vram_addr = addr;
    entry.width = width;
    entry.height = height;
    entry.pitch = pitch;
    entry.size = (size_t)height * pitch;
    entry.dma_addr = src->dma_addr;
    entry.dma_len = src->dma_len;
    entry.frame_time = pg->frame_time;
    entry.draw_time = pg->draw_time;
    entry.upload_pending = false;
    entry.draw_dirty = false;
    entry.cleared = false;

    surface_alloc_texture(pg, &entry);
    glBindTexture(GL_TEXTURE_2D, 0);

    return surface_put(d, addr, &entry);
}

SurfaceBinding *pgraph_gl_surface_get(NV2AState *d, hwaddr addr)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface->vram_addr == addr) {
            /* Move to front: a few addresses take most lookups. Callers do
             * not iterate the list across this call. */
            if (surface != QTAILQ_FIRST(&r->surfaces)) {
                QTAILQ_REMOVE(&r->surfaces, surface, entry);
                QTAILQ_INSERT_HEAD(&r->surfaces, surface, entry);
            }
            return surface;
        }
    }

    return NULL;
}

SurfaceBinding *pgraph_gl_surface_get_within(NV2AState *d, hwaddr addr)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* Bindings may overlap in memory; an exact base-address match is
     * always the surface the scanout means. */
    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (addr == surface->vram_addr) {
            return surface;
        }
    }
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (addr >= surface->vram_addr &&
            addr < (surface->vram_addr + surface->size)) {
            return surface;
        }
    }

    return NULL;
}

/* The async read-back's fence and buffer, if any. */
static void surface_drop_readback(SurfaceBinding *surface)
{
    if (surface->dl_fence) {
        glDeleteSync(surface->dl_fence);
        surface->dl_fence = NULL;
    }
    if (surface->dl_pbo) {
        glDeleteBuffers(1, &surface->dl_pbo);
        surface->dl_pbo = 0;
    }
}

/* Take a binding out of the valid list: unbound, and forgotten by the
 * target memo and the framebuffer caches. */
static void surface_detach(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    if (surface == r->color_binding) {
        assert(d->pgraph.surface_color.buffer_dirty);
        pgraph_gl_unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        assert(d->pgraph.surface_zeta.buffer_dirty);
        pgraph_gl_unbind_surface(d, false);
    }
    pgraph_rt_memo_forget(&r->rt_memo, surface);
    fb_cache_drop_tex(r, surface->gl_buffer);
    fb_multi_drop_tex(r, surface->gl_buffer);
    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    qatomic_set(&r->have_surfaces, !QTAILQ_EMPTY(&r->surfaces));
}

void pgraph_gl_surface_invalidate(NV2AState *d, SurfaceBinding *surface)
{
    surface_detach(d, surface);
    unregister_cpu_access_callback(d, surface);
    surface_drop_readback(surface);
    pgraph_gl_display_surface_gone(d, surface);
    g_free(surface);
}

static void surface_evict_old(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* HEURISTIC: 60 frames, not upstream's 5, so a surface reused every few
     * frames (an A/B ping-pong) lives; eviction only guards against leaks. */
    const int surface_age_limit = 60;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= surface_age_limit) {
            trace_nv2a_pgraph_surface_evict_reason("old", s->vram_addr);
            surface_evict_eager(d, s);
        }
    }
}

static bool check_surface_compatibility(SurfaceBinding *s1, SurfaceBinding *s2,
                                        bool strict)
{
    bool format_compatible =
        (s1->color == s2->color) &&
        (s1->fmt.gl_attachment == s2->fmt.gl_attachment) &&
        (s1->fmt.gl_internal_format == s2->fmt.gl_internal_format) &&
        (s1->pitch == s2->pitch);
    if (!format_compatible) {
        return false;
    }

    if (!strict) {
        return (s1->width >= s2->width) && (s1->height >= s2->height);
    } else {
        return (s1->width == s2->width) && (s1->height == s2->height);
    }
}

static void surface_download_if_dirty_impl(NV2AState *d,
                                           SurfaceBinding *surface)
{
    if (surface->draw_dirty) {
        surface_download(d, surface, true);
    }
}

/* Bind through the shadow: a same-name glBindFramebuffer is not free
 * (driver revalidation), and the render path rebinds every draw. */
static void fb_bind(PGRAPHGLState *r, GLuint fbo)
{
    if (r->fb_bound != fbo) {
        /* Start the work on the target being left, as the NV2A runs its
         * pushbuffer continuously: a batching driver would make each report
         * read wait for the whole frame (MEASURED, AMD Windows). */
        if (r->work_since_flush) {
            glFlush();
            r->work_since_flush = false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        r->fb_bound = fbo;
        r->fb_draw_idx = -1;
    }
}

/* One framebuffer per depth buffer, with up to 4 colour attachments chosen by
 * glDrawBuffer: MEASURED far cheaper than a framebuffer bind when a game
 * switches colour targets over one depth buffer. *idx gets the attachment to
 * select (-1 = attachment 0 of a private FBO). */
static GLuint fb_multi_get(PGRAPHGLState *r, GLuint color_tex,
                           GLuint zeta_tex, GLenum zeta_attach,
                           unsigned w, unsigned h, int *idx)
{
    *idx = -1;
    for (unsigned i = 0; i < r->fb_multi_n; i++) {
        if (r->fb_multi[i].zeta_tex != zeta_tex ||
            r->fb_multi[i].zeta_attach != zeta_attach ||
            r->fb_multi[i].w != w || r->fb_multi[i].h != h) {
            continue;
        }
        for (unsigned c = 0; c < r->fb_multi[i].n_color; c++) {
            if (r->fb_multi[i].color_tex[c] == color_tex) {
                *idx = c;
                return r->fb_multi[i].fbo;
            }
        }
        if (r->fb_multi[i].n_color < 4) {
            unsigned c = r->fb_multi[i].n_color;
            fb_bind(r, r->fb_multi[i].fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + c,
                                   GL_TEXTURE_2D, color_tex, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
                GL_FRAMEBUFFER_COMPLETE) {
                glFramebufferTexture2D(GL_FRAMEBUFFER,
                                       GL_COLOR_ATTACHMENT0 + c,
                                       GL_TEXTURE_2D, 0, 0);
                return 0;
            }
            r->fb_multi[i].color_tex[c] = color_tex;
            r->fb_multi[i].n_color = c + 1;
            *idx = c;
            return r->fb_multi[i].fbo;
        }
        return 0;   /* full: fall back to the private pair */
    }
    if (r->fb_multi_n == ARRAY_SIZE(r->fb_multi)) {
        return 0;
    }
    unsigned i = r->fb_multi_n;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    fb_bind(r, fbo);
    if (zeta_tex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, zeta_attach, GL_TEXTURE_2D,
                               zeta_tex, 0);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, color_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbo);
        r->fb_bound = 0;
        return 0;
    }
    r->fb_multi[i].zeta_tex = zeta_tex;
    r->fb_multi[i].zeta_attach = zeta_attach;
    r->fb_multi[i].fbo = fbo;
    r->fb_multi[i].color_tex[0] = color_tex;
    r->fb_multi[i].n_color = 1;
    r->fb_multi[i].w = w;
    r->fb_multi[i].h = h;
    r->fb_multi_n = i + 1;
    *idx = 0;
    return fbo;
}

/* A colour or depth texture is going away: drop every multi framebuffer
 * that references it. */
static void fb_multi_drop_tex(PGRAPHGLState *r, GLuint tex)
{
    for (unsigned i = 0; i < r->fb_multi_n;) {
        bool hit = r->fb_multi[i].zeta_tex == tex;
        for (unsigned c = 0; !hit && c < r->fb_multi[i].n_color; c++) {
            hit = r->fb_multi[i].color_tex[c] == tex;
        }
        if (hit) {
            if (r->fb_bound == r->fb_multi[i].fbo) {
                r->fb_bound = 0;
                r->fb_draw_idx = -1;
            }
            glDeleteFramebuffers(1, &r->fb_multi[i].fbo);
            memmove(&r->fb_multi[i], &r->fb_multi[i + 1],
                    (r->fb_multi_n - i - 1) * sizeof(r->fb_multi[0]));
            r->fb_multi_n--;
        } else {
            i++;
        }
    }
}

/* Return the cached FBO for this attachment pair, building it on first
 * use. Keyed on texture names (+ zeta attachment point, which varies
 * with the depth format); the color attachment point is constant. */
static GLuint fb_cache_get(PGRAPHGLState *r, GLuint color_tex,
                           GLenum color_attach, GLuint zeta_tex,
                           GLenum zeta_attach)
{
    for (unsigned i = 0; i < r->fb_cache_n; i++) {
        if (r->fb_cache[i].color_tex == color_tex &&
            r->fb_cache[i].zeta_tex == zeta_tex &&
            r->fb_cache[i].zeta_attach == zeta_attach) {
            return r->fb_cache[i].fbo;
        }
    }
    if (r->fb_cache_n == ARRAY_SIZE(r->fb_cache)) {
        if (r->fb_bound == r->fb_cache[0].fbo) {
            r->fb_bound = 0;
        }
        glDeleteFramebuffers(1, &r->fb_cache[0].fbo);
        memmove(&r->fb_cache[0], &r->fb_cache[1],
                (r->fb_cache_n - 1) * sizeof(r->fb_cache[0]));
        r->fb_cache_n--;
    }
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    fb_bind(r, fbo);
    if (color_tex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, color_attach, GL_TEXTURE_2D,
                               color_tex, 0);
    }
    if (zeta_tex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, zeta_attach, GL_TEXTURE_2D,
                               zeta_tex, 0);
    }
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
           GL_FRAMEBUFFER_COMPLETE);
    r->fb_cache[r->fb_cache_n].color_tex = color_tex;
    r->fb_cache[r->fb_cache_n].zeta_tex = zeta_tex;
    r->fb_cache[r->fb_cache_n].zeta_attach = zeta_attach;
    r->fb_cache[r->fb_cache_n].fbo = fbo;
    r->fb_cache_n++;
    return fbo;
}

/* A surface texture is being destroyed: drop every cached FBO that
 * references it (a deleted attachment would leave the FBO incomplete). */
static void fb_cache_drop_tex(PGRAPHGLState *r, GLuint tex)
{
    for (unsigned i = 0; i < r->fb_cache_n;) {
        if (r->fb_cache[i].color_tex == tex ||
            r->fb_cache[i].zeta_tex == tex) {
            if (r->fb_bound == r->fb_cache[i].fbo) {
                r->fb_bound = 0; /* GL unbinds a deleted FBO */
            }
            glDeleteFramebuffers(1, &r->fb_cache[i].fbo);
            memmove(&r->fb_cache[i], &r->fb_cache[i + 1],
                    (r->fb_cache_n - i - 1) * sizeof(r->fb_cache[0]));
            r->fb_cache_n--;
        } else {
            i++;
        }
    }
}

static void bind_current_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!r->color_binding && !r->zeta_binding) {
        fb_bind(r, r->gl_framebuffer);
        return;
    }
    if (r->color_binding) {
        pgraph_gl_display_surface_reuse(d, r->color_binding);
    }
    GLuint fbo = 0;
    int idx = -1;
    if (r->color_binding && r->zeta_binding &&
        r->color_binding->fmt.gl_attachment == GL_COLOR_ATTACHMENT0) {
        fbo = fb_multi_get(r, r->color_binding->gl_buffer,
                           r->zeta_binding->gl_buffer,
                           r->zeta_binding->fmt.gl_attachment,
                           r->color_binding->tex_w, r->color_binding->tex_h,
                           &idx);
    }
    if (!fbo) {
        idx = -1;
        fbo = fb_cache_get(
            r, r->color_binding ? r->color_binding->gl_buffer : 0,
            r->color_binding ? r->color_binding->fmt.gl_attachment : 0,
            r->zeta_binding ? r->zeta_binding->gl_buffer : 0,
            r->zeta_binding ? r->zeta_binding->fmt.gl_attachment : 0);
    }
    fb_bind(r, fbo);
    int want = idx < 0 ? 0 : idx;
    if (r->fb_draw_idx != want) {
        glDrawBuffer(GL_COLOR_ATTACHMENT0 + want);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + want);
        r->fb_draw_idx = want;
    }
}

static void surface_copy_shrink_row(uint8_t *out, uint8_t *in,
                                    unsigned int width,
                                    unsigned int bytes_per_pixel,
                                    unsigned int factor)
{
    if (bytes_per_pixel == 4) {
        for (unsigned int x = 0; x < width; x++) {
            *(uint32_t *)out = *(uint32_t *)in;
            out += 4;
            in += 4 * factor;
        }
    } else if (bytes_per_pixel == 2) {
        for (unsigned int x = 0; x < width; x++) {
            *(uint16_t *)out = *(uint16_t *)in;
            out += 2;
            in += 2 * factor;
        }
    } else {
        for (unsigned int x = 0; x < width; x++) {
            memcpy(out, in, bytes_per_pixel);
            out += bytes_per_pixel;
            in += bytes_per_pixel * factor;
        }
    }
}

/* Destroy a parked binding (render thread only: GL calls). */
static void parked_destroy(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    unregister_cpu_access_callback(d, surface);
    surface_drop_readback(surface);
    if (surface->disp_ref == NULL) {
        texpool_put(r, surface->gl_buffer, surface->vram_addr, surface->tex_w,
                    surface->tex_h, surface->tex_fmt);
        surface->gl_buffer = 0;
    }
    pgraph_gl_display_surface_gone(d, surface);
    QTAILQ_REMOVE(&r->parked, surface, entry);
    r->parked_n--;
    g_free(surface);
}

/* Settle the owed RAM copy, leaving the learning to the callers that consume
 * the range: the prune and the settle before a snapshot are not consumers,
 * and learning from them would turn a dead ping-pong eager. */
static void parked_materialize(NV2AState *d, SurfaceBinding *surface)
{
    pgraph_gl_surface_download_if_dirty(d, surface);
    surface->writeback_owed = false;
}

/* A newer parking of the exact same allocation replaces the owed content:
 * drop the older entry. */
static void parked_supersede_owed(NV2AState *d, hwaddr addr, size_t size)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->parked, entry, next) {
        if (s->writeback_owed && s->vram_addr == addr && s->size == size) {
            s->writeback_owed = false;
            parked_destroy(d, s);
        }
    }
}

/* The texture upload (gl/texture.c) is about to read RAM a parked binding
 * owes: materialize and learn. Render thread, so GL is safe. */
void pgraph_gl_parked_materialize_range(NV2AState *d, hwaddr start,
                                        size_t size)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->parked, entry, next) {
        if (s->writeback_owed && check_surface_overlaps_range(s, start, size)) {
            parked_materialize(d, s);
            pgraph_writeback_learn_live(&d->pgraph, s->vram_addr, s->size);
            parked_destroy(d, s);
        }
    }
}

/* Feed a re-created binding straight from the parked read-back: the PBO holds
 * the bytes the RAM round trip would carry (same allocation, size and format),
 * taken on the GPU in command order, with no fence wait, map or swizzle. The
 * new binding is dirty from birth: RAM stays behind it as usual. */
static bool parked_feed_from_readback(NV2AState *d, SurfaceBinding *parked,
                                      SurfaceBinding *neu)
{
    PGRAPHState *pg = &d->pgraph;

    /* A depth read back at 1x (dl_native) holds no scaled image to feed. */
    if (parked->dl_native ||
        !parked->dl_fence || parked->dl_draw_time != parked->draw_time ||
        parked->width != neu->width || parked->height != neu->height ||
        parked->fmt.bytes_per_pixel != neu->fmt.bytes_per_pixel ||
        !neu->width || !neu->height) {
        return false;
    }
    unsigned int sf = pg->surface_scale_factor;
    unsigned int width = neu->width, height = neu->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    GLint last_texture_binding, prev_alignment, prev_row_length;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture_binding);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_length);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, parked->dl_pbo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,
                  sf * parked->pitch / neu->fmt.bytes_per_pixel);
    glBindTexture(GL_TEXTURE_2D, neu->gl_buffer);
    glTexImage2D(GL_TEXTURE_2D, 0, neu->fmt.gl_internal_format, width, height,
                 0, neu->fmt.gl_format, neu->fmt.gl_type, NULL);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_length);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, last_texture_binding);
    return true;
}

/* One buffer, two views: OutRun 2 writes the scene depth as colour into a
 * 160x120 target, then binds that memory as the next pass's zeta, and never
 * Locks it (the game's function at 0x00076014).
 * A shader pass reinterprets the bits in place, with no trip through a buffer.
 * The shaders pack Z24S8 against 32-bit colour, so both views must be 4 bytes
 * per pixel (GL keeps a float Z24 as fixed, as its upload does). */
static bool alias_convert(NV2AState *d, SurfaceBinding *src,
                          SurfaceBinding *dst)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!src->gl_buffer || !dst->gl_buffer || src->swizzle || dst->swizzle ||
        src->fmt.bytes_per_pixel != 4 || dst->fmt.bytes_per_pixel != 4) {
        return false;
    }
    bool to_depth = !dst->color && src->color;
    bool to_color = dst->color && !src->color;
    if (!to_depth && !to_color) {
        return false;
    }
    if (to_depth && dst->fmt.gl_attachment != GL_DEPTH_STENCIL_ATTACHMENT &&
        dst->fmt.gl_attachment != GL_DEPTH_ATTACHMENT) {
        return false;
    }
    if (to_color && dst->fmt.gl_attachment != GL_COLOR_ATTACHMENT0) {
        return false;
    }
    unsigned int w = dst->width, h = dst->height;
    pgraph_apply_scaling_factor(pg, &w, &h);

    pgraph_gl_tex_shadow_invalidate(r);
    bool cost_suspended = pgraph_gl_own_draw_suspend(d);

    fb_bind(r, r->s2t_rndr.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, dst->fmt.gl_attachment,
                           GL_TEXTURE_2D, dst->gl_buffer, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src->gl_buffer);
    tex_sample_raw(src);

    GLuint prog = to_depth ? r->s2t_rndr.unpack_prog : r->s2t_rndr.pack_prog;
    glUseProgram(prog);
    glProgramUniform1i(prog, to_depth ? r->s2t_rndr.unpack_tex_loc
                                      : r->s2t_rndr.pack_tex_loc, 0);
    glBindVertexArray(r->s2t_rndr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->s2t_rndr.vbo);
    glViewport(0, 0, w, h);
    s2t_pass_state(r);
    if (to_depth) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        glColorMask(false, false, false, false);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);
    } else {
        GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, bufs);
        glColorMask(true, true, true, true);
        glDisable(GL_DEPTH_TEST);
    }
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
              GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glFramebufferTexture2D(GL_FRAMEBUFFER, dst->fmt.gl_attachment,
                           GL_TEXTURE_2D, 0, 0);
    if (to_depth) {
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    s2t_pass_end(d, cost_suspended);
    return ok;
}

/* A binding created over parked content of the same allocation and layout
 * consumes it (OutRun 2 composes through a colour and a depth view of one
 * buffer). Across a format change the content is reinterpreted on the GPU;
 * otherwise it is fed from the read-back or written back for the create's
 * upload, and the range learnt live. Other layouts stay parked: a RAM round
 * trip would only carry garbage across. */
static SettleResult parked_settle_for_create(NV2AState *d,
                                             SurfaceBinding *neu)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    /* Only the latest overlapping entry holds the range's last GPU write (the
     * list is insertion-ordered): an older one was overwritten in between. */
    SurfaceBinding *s, *latest = NULL;
    QTAILQ_FOREACH(s, &r->parked, entry) {
        if (s->writeback_owed &&
            check_surface_overlaps_range(s, neu->vram_addr, neu->size)) {
            latest = s;
        }
    }
    if (!latest || latest->vram_addr != neu->vram_addr ||
        latest->size != neu->size || latest->pitch != neu->pitch ||
        latest->swizzle != neu->swizzle) {
        return SETTLED_NOTHING; /* no owed content, or incompatible */
    }
    if (latest->fmt.gl_internal_format != neu->fmt.gl_internal_format &&
        alias_convert(d, latest, neu)) {
        /* Reinterpreted on the GPU, see above. */
        parked_destroy(d, latest);
        return SETTLED_TEXTURE;
    }
    if (latest->fmt.gl_internal_format != neu->fmt.gl_internal_format) {
        /* HEURISTIC: a format change alias_convert refuses drops the owed
         * content, not written back; each view keeps its own pooled image. */
        parked_destroy(d, latest);
        return SETTLED_NOTHING;
    }
    SettleResult how = SETTLED_RAM;
    if (parked_feed_from_readback(d, latest, neu)) {
        how = SETTLED_TEXTURE;
    } else {
        parked_materialize(d, latest);
    }
    pgraph_writeback_learn_live(&d->pgraph, latest->vram_addr, latest->size);
    parked_destroy(d, latest);
    return how;
}

/* pgraph_gl_surface_invalidate, but parking: the texture and the CPU access
 * callback stay alive for a writeback on demand. */
static void park_owed(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    surface_detach(d, surface);

    parked_supersede_owed(d, surface->vram_addr, surface->size);
    /* Owed only when the GPU content is newer: downloading a clean image over
     * RAM the guest wrote since would corrupt it. */
    surface->writeback_owed = surface->draw_dirty;
    QTAILQ_INSERT_TAIL(&r->parked, surface, entry);
    r->parked_n++;

    if (r->parked_n > PARKED_MAX) { /* anti-leak prune */
        SurfaceBinding *old = QTAILQ_FIRST(&r->parked);
        if (old->writeback_owed) {
            parked_materialize(d, old);
        }
        parked_destroy(d, old);
    }
}

/* Shrink a scaled surface on the GPU into a native-size scratch target left
 * bound for reading: only the guest's pixel count crosses the bus, not scale^2
 * times it, and the NVIDIA driver converts a depth read-back on the CPU
 * (MEASURED). Point sampled: each guest pixel gets the texel at its centre,
 * where the hardware draws at 1x, and a nearest depth-stencil blit copies both
 * as stored. The surface must be attached to gl_fb_util, bound for reading. */
static bool surface_shrink_on_gpu(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHGLState *r = pg->gl_renderer_state;
    bool zeta = !surface->color;
    GLuint *fbo = zeta ? &r->dl_zshrink_fbo : &r->dl_shrink_fbo;
    GLuint *tex = zeta ? &r->dl_zshrink_tex : &r->dl_shrink_tex;
    unsigned *tw = zeta ? &r->dl_zshrink_w : &r->dl_shrink_w;
    unsigned *th = zeta ? &r->dl_zshrink_h : &r->dl_shrink_h;
    GLint *tf = zeta ? &r->dl_zshrink_fmt : &r->dl_shrink_fmt;
    unsigned w = surface->width, h = surface->height, sw = w, sh = h;
    GLbitfield mask;

    if (pg->surface_scale_factor == 1) {
        return false;
    }
    if (zeta) {
        mask = GL_DEPTH_BUFFER_BIT;
        if (surface->fmt.gl_attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            mask |= GL_STENCIL_BUFFER_BIT;
        }
    } else if (surface->fmt.gl_attachment == GL_COLOR_ATTACHMENT0) {
        mask = GL_COLOR_BUFFER_BIT;
    } else {
        return false;
    }

    if (!*fbo) {
        glGenFramebuffers(1, fbo);
        glGenTextures(1, tex);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *fbo);
    if (*tw < w || *th < h || *tf != (GLint)surface->fmt.gl_internal_format) {
        GLint prev_tex;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, surface->fmt.gl_internal_format, w, h,
                     0, surface->fmt.gl_format, surface->fmt.gl_type, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, prev_tex);
        if (zeta) {
            /* A depth-only format after a depth-stencil one, or back:
             * no stale half of the other attachment may stay behind. */
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                                   GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                   0, 0);
        }
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, surface->fmt.gl_attachment,
                               GL_TEXTURE_2D, *tex, 0);
        *tw = w;
        *th = h;
        *tf = surface->fmt.gl_internal_format;
    }
    pgraph_apply_scaling_factor(pg, &sw, &sh);
    /* xemu's own copy: the scissor a draw left enabled would clip it (the
     * draw state cache is told it changed). */
    pgraph_gl_draw_state_invalidate(r);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, w, h, mask, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, *fbo);
    r->fb_bound = 0; /* GL_FRAMEBUFFER shadow is stale after this */
    return true;
}

static void surface_download_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       bool swizzle, bool flip, bool downscale,
                                       uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;

    swizzle &= surface->swizzle;
    downscale &= (pg->surface_scale_factor != 1);

    if (!surface->width || !surface->height) {
        return;
    }

    trace_nv2a_pgraph_surface_download(
        surface->color ? "COLOR" : "ZETA",
        surface->swizzle ? "sz" : "lin", surface->vram_addr,
        surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    /* Read through the scratch FBO; the render pair stays untouched. */
    PGRAPHGLState *r = pg->gl_renderer_state;
    fb_bind(r, r->gl_fb_util);
    glFramebufferTexture2D(GL_FRAMEBUFFER, surface->fmt.gl_attachment,
                           GL_TEXTURE_2D, surface->gl_buffer, 0);

    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    /* Read surface into memory */
    uint8_t *gl_read_buf = pixels;

    uint8_t *swizzle_buf = pixels;
    if (swizzle) {
        /* FIXME: Allocate big buffer up front and re-alloc if necessary.
         * FIXME: Consider swizzle in shader
         */
        assert(pg->surface_scale_factor == 1 || downscale);
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
    }

    bool gpu_shrink = downscale && surface_shrink_on_gpu(pg, surface);
    if (downscale && !gpu_shrink) {
        pg->scale_buf = (uint8_t *)g_realloc(
            pg->scale_buf, pg->surface_scale_factor * pg->surface_scale_factor *
                               surface->size);
        gl_read_buf = pg->scale_buf;
    }

    unsigned int rd_scale = gpu_shrink ? 1 : pg->surface_scale_factor;
    glo_readpixels(
        surface->fmt.gl_format, surface->fmt.gl_type, surface->fmt.bytes_per_pixel,
        rd_scale * surface->pitch,
        rd_scale * surface->width,
        rd_scale * surface->height, flip, gl_read_buf);

    if (downscale && !gpu_shrink) {
        assert(surface->pitch >= (surface->width * surface->fmt.bytes_per_pixel));
        uint8_t *out = swizzle_buf, *in = pg->scale_buf;
        for (unsigned int y = 0; y < surface->height; y++) {
            surface_copy_shrink_row(out, in, surface->width,
                                    surface->fmt.bytes_per_pixel,
                                    pg->surface_scale_factor);
            in += surface->pitch * pg->surface_scale_factor *
                  pg->surface_scale_factor;
            out += surface->pitch;
        }
    }

    if (swizzle) {
        swizzle_rect(swizzle_buf, surface->width, surface->height, pixels,
                     surface->pitch, surface->fmt.bytes_per_pixel);
        g_free(swizzle_buf);
    }

    /* Release the texture from the scratch FBO, restore the render pair */
    if (gpu_shrink) {
        glBindFramebuffer(GL_FRAMEBUFFER, r->gl_fb_util);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, surface->fmt.gl_attachment,
                           GL_TEXTURE_2D, 0, 0);
    r->fb_bound = r->gl_fb_util;
    bind_current_surface(d);
}

/* Read the surface back into its PBO without the driver sync a blocking read
 * pays; the fence tells when the pixels are there, usually before a read. */
static void surface_download_issue(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    if (surface->dl_fence) {
        /* Never harvested: nothing read it, the new one replaces it. */
        glDeleteSync(surface->dl_fence);
        surface->dl_fence = NULL;
    }
    unsigned int sf = pg->surface_scale_factor;
    size_t need = (size_t)sf * sf * surface->pitch * surface->height;
    if (!surface->dl_pbo) {
        glGenBuffers(1, &surface->dl_pbo);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, surface->dl_pbo);
    if (surface->dl_pbo_size < need) {
        glBufferData(GL_PIXEL_PACK_BUFFER, need, NULL, GL_STREAM_READ);
        surface->dl_pbo_size = need;
    }
    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
    fb_bind(r, r->gl_fb_util);
    glFramebufferTexture2D(GL_FRAMEBUFFER, surface->fmt.gl_attachment,
                           GL_TEXTURE_2D, surface->gl_buffer, 0);
    /* A depth surface is shrunk on the GPU first (surface_shrink_on_gpu);
     * colour keeps the full read here. */
    surface->dl_native = !surface->color && surface_shrink_on_gpu(pg, surface);
    unsigned int rs = surface->dl_native ? 1 : sf;
    glo_readpixels(surface->fmt.gl_format, surface->fmt.gl_type,
                   surface->fmt.bytes_per_pixel, rs * surface->pitch,
                   rs * surface->width, rs * surface->height, false, NULL);
    if (surface->dl_native) {
        glBindFramebuffer(GL_FRAMEBUFFER, r->gl_fb_util);
        r->fb_bound = r->gl_fb_util;
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, surface->fmt.gl_attachment,
                           GL_TEXTURE_2D, 0, 0);
    bind_current_surface(d);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    surface->dl_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    surface->dl_draw_time = surface->draw_time;
}

/* Collect a pending read-back into guest RAM. False when there is none
 * or it is stale (drawn again since): read back the usual way. */
static bool surface_download_harvest(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    if (!surface->dl_fence) {
        return false;
    }
    GLsync fence = surface->dl_fence;
    surface->dl_fence = NULL;
    if (surface->dl_draw_time != surface->draw_time) {
        glDeleteSync(fence);
        return false;
    }
    glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, (GLuint64)5000000000);
    glDeleteSync(fence);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, surface->dl_pbo);
    uint8_t *raw = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                    surface->dl_pbo_size, GL_MAP_READ_BIT);
    if (!raw) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return false;
    }
    unsigned int sf = pg->surface_scale_factor;
    uint8_t *pixels = d->vram_ptr + surface->vram_addr;
    uint8_t *dst = pixels;
    uint8_t *swizzle_buf = NULL;
    if (surface->swizzle) {
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        dst = swizzle_buf;
    }
    if (sf != 1 && !surface->dl_native) {
        assert(surface->pitch >= (surface->width * surface->fmt.bytes_per_pixel));
        uint8_t *out = dst, *in = raw;
        for (unsigned int y = 0; y < surface->height; y++) {
            surface_copy_shrink_row(out, in, surface->width,
                                    surface->fmt.bytes_per_pixel, sf);
            in += surface->pitch * sf * sf;
            out += surface->pitch;
        }
    } else {
        memcpy(dst, raw, (size_t)surface->pitch * surface->height);
    }
    if (surface->swizzle) {
        swizzle_rect(swizzle_buf, surface->width, surface->height, pixels,
                     surface->pitch, surface->fmt.bytes_per_pixel);
        g_free(swizzle_buf);
    }
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

/* Evict with a writeback, without waiting when possible: the binding parks
 * with its read-back in flight for the first consumer to harvest. The scanout
 * blocks, as the display reads RAM directly once the surface is gone. */
static void surface_evict_eager(NV2AState *d, SurfaceBinding *s)
{
    hwaddr scan = d->pcrtc.start;
    bool scanout = scan >= s->vram_addr && scan < s->vram_addr + s->size;
    if (s->draw_dirty && !scanout) {
        surface_download_issue(d, s);
        park_owed(d, s);
        s->download_pending = true;
        return;
    }
    pgraph_gl_surface_download_if_dirty(d, s);
    pgraph_gl_surface_invalidate(d, s);
}

static void surface_download(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force) || !surface->width ||
        !surface->height) {
        return;
    }

    /* FIXME: Respect write enable at last TOU? */

    /* An async read-back was counted when issued. */
    bool harvested = surface_download_harvest(d, surface);
    if (!harvested) {
        nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
        surface_download_to_buffer(d, surface, true, false, true,
                                   d->vram_ptr + surface->vram_addr);
    }

    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_NV2A_TEX);
    pgraph_writeback_clear_discard(&d->pgraph, surface->vram_addr,
                                   surface->pitch * surface->height);

    surface->download_pending = false;
    surface->draw_dirty = false;
}

void pgraph_gl_surfaces_refresh_any_dirty(NV2AState *d)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    SurfaceBinding *s;
    int any = 0;
    QTAILQ_FOREACH (s, &r->surfaces, entry) {
        if (s->draw_dirty) {
            any = 1;
            break;
        }
    }
    /* A parked binding still owing its writeback is ahead of RAM too. */
    QTAILQ_FOREACH (s, &r->parked, entry) {
        if (s->writeback_owed) {
            any = 1;
            break;
        }
    }
    qatomic_set(&surfaces_any_dirty, any);
}

void pgraph_gl_process_pending_downloads(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        surface_download(d, surface, false);
    }
    /* Settle the parked writebacks marked pending; free those owing nothing. */
    SurfaceBinding *parked, *next;
    QTAILQ_FOREACH_SAFE(parked, &r->parked, entry, next) {
        if (parked->writeback_owed && parked->download_pending) {
            surface_download(d, parked, false);
            parked->writeback_owed = false;
        }
        if (!parked->writeback_owed) {
            parked_destroy(d, parked);
        }
    }

    qatomic_set(&r->downloads_pending, false);
    qemu_event_set(&r->downloads_complete);
}

void pgraph_gl_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        pgraph_gl_surface_download_if_dirty(d, surface);
    }
    /* Settle every parked writeback: a snapshot or a scale change needs
     * guest RAM exact. */
    SurfaceBinding *parked, *next;
    QTAILQ_FOREACH_SAFE(parked, &r->parked, entry, next) {
        if (parked->writeback_owed) {
            parked_materialize(d, parked);
        }
        parked_destroy(d, parked);
    }

    qatomic_set(&r->download_dirty_surfaces_pending, false);
    qemu_event_set(&r->dirty_surfaces_download_complete);
}

static void surface_copy_expand_row(uint8_t *out, uint8_t *in,
                                    unsigned int width,
                                    unsigned int bytes_per_pixel,
                                    unsigned int factor)
{
    if (bytes_per_pixel == 4) {
        for (unsigned int x = 0; x < width; x++) {
            for (unsigned int i = 0; i < factor; i++) {
                *(uint32_t *)out = *(uint32_t *)in;
                out += bytes_per_pixel;
            }
            in += bytes_per_pixel;
        }
    } else if (bytes_per_pixel == 2) {
        for (unsigned int x = 0; x < width; x++) {
            for (unsigned int i = 0; i < factor; i++) {
                *(uint16_t *)out = *(uint16_t *)in;
                out += bytes_per_pixel;
            }
            in += bytes_per_pixel;
        }
    } else {
        for (unsigned int x = 0; x < width; x++) {
            for (unsigned int i = 0; i < factor; i++) {
                memcpy(out, in, bytes_per_pixel);
                out += bytes_per_pixel;
            }
            in += bytes_per_pixel;
        }
    }
}

static void surface_copy_expand(uint8_t *out, uint8_t *in, unsigned int width,
                                unsigned int height,
                                unsigned int bytes_per_pixel,
                                unsigned int factor)
{
    size_t out_pitch = width * bytes_per_pixel * factor;

    for (unsigned int y = 0; y < height; y++) {
        surface_copy_expand_row(out, in, width, bytes_per_pixel, factor);
        uint8_t *row_in = out;
        for (unsigned int i = 1; i < factor; i++) {
            out += out_pitch;
            memcpy(out, row_in, out_pitch);
        }
        in += width * bytes_per_pixel;
        out += out_pitch;
    }
}

/* Grow a native-size image into a scaled surface on the GPU: only the
 * guest's pixel count crosses the bus, not scale^2 times it, and the CPU no
 * longer writes the scaled copy (at 10x a 640x480 frame, 123 MB for each
 * upload). Point sampled, so each guest pixel fills its scale x scale block
 * as the CPU expand did, and a nearest depth-stencil blit copies both as
 * stored. The surface keeps the scaled storage it was created with;
 * anything else stays on the CPU path. gl_fb_util is bound on entry. */
static bool surface_grow_on_gpu(PGRAPHState *pg, SurfaceBinding *surface,
                                const uint8_t *pixels)
{
    PGRAPHGLState *r = pg->gl_renderer_state;
    bool zeta = !surface->color;
    GLuint *fbo = zeta ? &r->ul_zgrow_fbo : &r->ul_grow_fbo;
    GLuint *tex = zeta ? &r->ul_zgrow_tex : &r->ul_grow_tex;
    unsigned *tw = zeta ? &r->ul_zgrow_w : &r->ul_grow_w;
    unsigned *th = zeta ? &r->ul_zgrow_h : &r->ul_grow_h;
    GLint *tf = zeta ? &r->ul_zgrow_fmt : &r->ul_grow_fmt;
    unsigned w = surface->width, h = surface->height, sw = w, sh = h;
    GLbitfield mask;

    pgraph_apply_scaling_factor(pg, &sw, &sh);
    if (pg->surface_scale_factor == 1 || surface->tex_w != sw ||
        surface->tex_h != sh ||
        surface->tex_fmt != (GLint)surface->fmt.gl_internal_format) {
        return false;
    }
    if (zeta) {
        mask = GL_DEPTH_BUFFER_BIT;
        if (surface->fmt.gl_attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            mask |= GL_STENCIL_BUFFER_BIT;
        }
    } else if (surface->fmt.gl_attachment == GL_COLOR_ATTACHMENT0) {
        mask = GL_COLOR_BUFFER_BIT;
    } else {
        return false;
    }

    if (!*fbo) {
        glGenFramebuffers(1, fbo);
        glGenTextures(1, tex);
    }
    GLint prev_tex, prev_alignment;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT,
                  (w * surface->fmt.bytes_per_pixel) % 4 ? 1 : 4);
    glBindTexture(GL_TEXTURE_2D, *tex);
    bool fresh = *tw != w || *th != h ||
                 *tf != (GLint)surface->fmt.gl_internal_format;
    if (fresh) {
        glTexImage2D(GL_TEXTURE_2D, 0, surface->fmt.gl_internal_format, w, h,
                     0, surface->fmt.gl_format, surface->fmt.gl_type, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, surface->fmt.gl_format,
                        surface->fmt.gl_type, pixels);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
    glBindTexture(GL_TEXTURE_2D, prev_tex);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, *fbo);
    if (fresh) {
        if (zeta) {
            /* A depth-only format after a depth-stencil one, or back:
             * no stale half of the other attachment may stay behind. */
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                                   GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                   0, 0);
        }
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, surface->fmt.gl_attachment,
                               GL_TEXTURE_2D, *tex, 0);
        *tw = w;
        *th = h;
        *tf = surface->fmt.gl_internal_format;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->gl_fb_util);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, surface->fmt.gl_attachment,
                           GL_TEXTURE_2D, surface->gl_buffer, 0);
    /* xemu's own copy: the scissor a draw left enabled would clip it, and
     * none of the game's write masks apply (the draw state cache is told
     * it changed). */
    pgraph_gl_draw_state_invalidate(r);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glBlitFramebuffer(0, 0, w, h, 0, 0, sw, sh, mask, GL_NEAREST);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, surface->fmt.gl_attachment,
                           GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, r->gl_fb_util);
    r->fb_bound = r->gl_fb_util;
    return true;
}

static void upload_surface_data_impl(NV2AState *d, SurfaceBinding *surface,
                                bool force)
{
    if (!(surface->upload_pending || force)) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);
    pgraph_gl_display_surface_reuse(d, surface);

    trace_nv2a_pgraph_surface_upload(
                 surface->color ? "COLOR" : "ZETA",
                 surface->swizzle ? "sz" : "lin", surface->vram_addr,
                 surface->width, surface->height, surface->pitch,
                 surface->fmt.bytes_per_pixel);

    PGRAPHState *pg = &d->pgraph;

    surface->upload_pending = false;
    if (surface->watch_read_off) {
        surface->watch_read_off = false;
        mem_access_callback_set_flags(qemu_get_cpu(0), surface->access_cb,
                                      BP_MEM_READ | BP_MEM_WRITE);
    }
    surface->draw_time = pg->draw_time;

    if (!surface->width || !surface->height) {
        return;
    }

    // FIXME: Don't query GL for texture binding
    GLint last_texture_binding;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture_binding);

    /* Park on the scratch FBO while the surface texture is redefined;
     * the render pair FBO is restored at the end of the upload. */
    fb_bind(pg->gl_renderer_state, pg->gl_renderer_state->gl_fb_util);

    uint8_t *data = d->vram_ptr;
    uint8_t *buf = data + surface->vram_addr;

    if (surface->swizzle) {
        buf = (uint8_t*)g_malloc(surface->size);
        unswizzle_rect(data + surface->vram_addr,
                       surface->width, surface->height,
                       buf,
                       surface->pitch,
                       surface->fmt.bytes_per_pixel);
    }

    /* FIXME: Replace this scaling */

    // This is VRAM so we can't do this inplace!
    uint8_t *optimal_buf = buf;
    unsigned int optimal_pitch = surface->width * surface->fmt.bytes_per_pixel;

    if (surface->pitch != optimal_pitch) {
        optimal_buf = (uint8_t *)g_malloc(surface->height * optimal_pitch);

        uint8_t *src = buf;
        uint8_t *dst = optimal_buf;
        unsigned int irow;
        for (irow = 0; irow < surface->height; irow++) {
            memcpy(dst, src, optimal_pitch);
            src += surface->pitch;
            dst += optimal_pitch;
        }
    }

    uint8_t *gl_read_buf = optimal_buf;
    unsigned int width = surface->width, height = surface->height;

    if (surface_grow_on_gpu(pg, surface, optimal_buf)) {
        if (optimal_buf != buf) {
            g_free(optimal_buf);
        }
        if (surface->swizzle) {
            g_free(buf);
        }
        glBindTexture(GL_TEXTURE_2D, last_texture_binding);
        bind_current_surface(d);
        return;
    }

    if (pg->surface_scale_factor > 1) {
        pgraph_apply_scaling_factor(pg, &width, &height);
        pg->scale_buf = (uint8_t *)g_realloc(
            pg->scale_buf, width * height * surface->fmt.bytes_per_pixel);
        gl_read_buf = pg->scale_buf;
        uint8_t *out = gl_read_buf, *in = optimal_buf;
        surface_copy_expand(out, in, surface->width, surface->height,
                            surface->fmt.bytes_per_pixel,
                            d->pgraph.surface_scale_factor);
    }

    int prev_unpack_alignment;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_alignment);
    if (unlikely((width * surface->fmt.bytes_per_pixel) % 4 != 0)) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    } else {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    glBindTexture(GL_TEXTURE_2D, surface->gl_buffer);
    glTexImage2D(GL_TEXTURE_2D, 0, surface->fmt.gl_internal_format, width,
                 height, 0, surface->fmt.gl_format, surface->fmt.gl_type,
                 gl_read_buf);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_alignment);
    if (optimal_buf != buf) {
        g_free(optimal_buf);
    }
    if (surface->swizzle) {
        g_free(buf);
    }

    // Rebind previous framebuffer binding
    glBindTexture(GL_TEXTURE_2D, last_texture_binding);

    bind_current_surface(d);
}

static void compare_surfaces(SurfaceBinding *s1, SurfaceBinding *s2)
{
    #define DO_CMP(fld) \
        if (s1->fld != s2->fld) \
            trace_nv2a_pgraph_surface_compare_mismatch( \
                #fld, (long int)s1->fld, (long int)s2->fld);
    DO_CMP(shape.clip_x)
    DO_CMP(shape.clip_width)
    DO_CMP(shape.clip_y)
    DO_CMP(shape.clip_height)
    DO_CMP(gl_buffer)
    DO_CMP(fmt.bytes_per_pixel)
    DO_CMP(fmt.gl_attachment)
    DO_CMP(fmt.gl_internal_format)
    DO_CMP(fmt.gl_format)
    DO_CMP(fmt.gl_type)
    DO_CMP(color)
    DO_CMP(swizzle)
    DO_CMP(vram_addr)
    DO_CMP(width)
    DO_CMP(height)
    DO_CMP(pitch)
    DO_CMP(size)
    DO_CMP(dma_addr)
    DO_CMP(dma_len)
    DO_CMP(frame_time)
    DO_CMP(draw_time)
    #undef DO_CMP
}

static void populate_surface_binding_entry_sized(NV2AState *d, bool color,
                                                 unsigned int width,
                                                 unsigned int height,
                                                 SurfaceBinding *entry)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    Surface *surface;
    hwaddr dma_address;
    SurfaceFormatInfo fmt;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        assert(pg->surface_shape.color_format != 0);
        assert(pg->surface_shape.color_format <
               ARRAY_SIZE(kelvin_surface_color_format_gl_map));
        fmt = kelvin_surface_color_format_gl_map[pg->surface_shape.color_format];
        if (fmt.bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            abort();
        }
    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        assert(pg->surface_shape.zeta_format != 0);
        assert(pg->surface_shape.zeta_format <
               ARRAY_SIZE(kelvin_surface_zeta_float_format_gl_map));
        const SurfaceFormatInfo *map =
            pg->surface_shape.z_format ? kelvin_surface_zeta_float_format_gl_map :
                                         kelvin_surface_zeta_fixed_format_gl_map;
        fmt = map[pg->surface_shape.zeta_format];
    }

    DMAObject dma = nv_dma_load(d, dma_address);
    /* There's a bunch of bugs that could cause us to hit this function
     * at the wrong time and get a invalid dma object.
     * Check that it's sane. */
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    // assert(dma.address + surface->offset != 0);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);
    assert(surface->pitch % fmt.bytes_per_pixel == 0);
    assert((dma.address & ~0x07FFFFFF) == 0);

    entry->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                   r->color_binding->shape;
    entry->gl_buffer = 0;
    entry->fmt = fmt;
    entry->color = color;
    entry->swizzle =
        (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    entry->vram_addr = dma.address + surface->offset;
    entry->width = width;
    entry->height = height;
    entry->pitch = surface->pitch;
    entry->size = height * MAX(surface->pitch, width * fmt.bytes_per_pixel);
    entry->upload_pending = true;
    entry->download_pending = false;
    entry->draw_dirty = false;
    entry->dma_addr = dma.address;
    entry->dma_len = dma.limit;
    entry->frame_time = pg->frame_time;
    entry->draw_time = pg->draw_time;
    entry->cleared = false;
}

static void populate_surface_binding_entry(NV2AState *d, bool color,
                                                  SurfaceBinding *entry)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    unsigned int width, height;

    if (color || !r->color_binding) {
        surface_get_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        /* Since we determine surface dimensions based on the clipping
         * rectangle, make sure to include the surface offset as well.
         */
        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
        pgraph_bound_surface_to_format(pg, &width, &height);
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    populate_surface_binding_entry_sized(d, color, width, height, entry);
}

/* A zeta memo swap skips the full pass, which keeps the zeta binding
 * covering the color target (the sanity check of pgraph_gl_surface_update).
 * A color swap marks the zeta dirty, and its re-resolve checks that. */
static bool rt_memo_zeta_covers(PGRAPHGLState *r, SurfaceBinding *z)
{
    SurfaceBinding *c = r->color_binding;
    return !c || (c->width <= z->width && c->height <= z->height);
}

static void update_surface_part(NV2AState *d, bool upload, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* Render-target switch memo: if the target registers match a
     * remembered resolution and the shape generation is unchanged,
     * swap bindings without the populate/compat pass. */
    if (upload && tcg_enabled()) {
        Surface *sf = color ? &pg->surface_color : &pg->surface_zeta;
        if (sf->buffer_dirty) {
            hwaddr dma = color ? pg->dma_color : pg->dma_zeta;
            for (int i = 0; i < 2; i++) {
                SurfaceBinding *m = pgraph_rt_memo_match(
                    &r->rt_memo, color, i, dma, sf->offset, sf->pitch);
                if (m && m != (color ? r->zeta_binding : r->color_binding) &&
                    (color || rt_memo_zeta_covers(r, m))) {
                    pgraph_set_surface_binding_dim(pg, m->width, m->height,
                                                   &m->shape);
                    if (color) {
                        r->color_binding = m;
                        pg->surface_zeta.buffer_dirty = true;
                    } else {
                        r->zeta_binding = m;
                    }
                    bind_current_surface(d);
                    sf->buffer_dirty = false;
                    return;
                }
            }
        }
    }

    SurfaceBinding entry;
    populate_surface_binding_entry(d, color, &entry);

    Surface *surface = color ? &pg->surface_color : &pg->surface_zeta;

    bool mem_dirty = !tcg_enabled() && memory_region_test_and_clear_dirty(
                                           d->vram, entry.vram_addr, entry.size,
                                           DIRTY_MEMORY_NV2A);

    if (upload && (surface->buffer_dirty || mem_dirty)) {
        pgraph_gl_unbind_surface(d, color);

        SurfaceBinding *found = pgraph_gl_surface_get(d, entry.vram_addr);
        if (found != NULL) {
            /* FIXME: Support same color/zeta surface target? In the mean time,
             * if the surface we just found is currently bound, just unbind it.
             */
            SurfaceBinding *other = (color ? r->zeta_binding
                                           : r->color_binding);
            if (found == other) {
                NV2A_UNIMPLEMENTED("Same color & zeta surface offset");
                pgraph_gl_unbind_surface(d, !color);
            }
        }

        trace_nv2a_pgraph_surface_target(
            color ? "COLOR" : "ZETA", entry.vram_addr,
            entry.swizzle ? "sz" : "ln",
            pg->surface_shape.anti_aliasing,
            pg->surface_shape.clip_x,
            pg->surface_shape.clip_width, pg->surface_shape.clip_y,
            pg->surface_shape.clip_height);

        bool should_create = true;

        if (found != NULL) {
            bool is_compatible =
                check_surface_compatibility(found, &entry, false);

#define TRACE_ARGS found->vram_addr, found->width, found->height, \
            found->swizzle ? "sz" : "ln", \
            found->shape.anti_aliasing, found->shape.clip_x, \
            found->shape.clip_width, found->shape.clip_y, \
            found->shape.clip_height, found->pitch
            if (found->color) {
                trace_nv2a_pgraph_surface_match_color(TRACE_ARGS);
            } else {
                trace_nv2a_pgraph_surface_match_zeta(TRACE_ARGS);
            }
#undef TRACE_ARGS

            assert(!(entry.swizzle && pg->clearing));

            if (found->swizzle != entry.swizzle) {
                /* Clears should only be done on linear surfaces. Avoid
                 * synchronization by allowing (1) a surface marked swizzled to
                 * be cleared under the assumption the entire surface is
                 * destined to be cleared and (2) a fully cleared linear surface
                 * to be marked swizzled. Strictly match size to avoid
                 * pathological cases.
                 */
                is_compatible &= (pg->clearing || found->cleared) &&
                    check_surface_compatibility(found, &entry, true);
                if (is_compatible) {
                    trace_nv2a_pgraph_surface_migrate_type(
                        entry.swizzle ? "swizzled" : "linear");
                }
            }

            if (is_compatible && color &&
                !check_surface_compatibility(found, &entry, true)) {
                SurfaceBinding zeta_entry;
                populate_surface_binding_entry_sized(
                    d, !color, found->width, found->height, &zeta_entry);
                hwaddr color_end = found->vram_addr + found->size;
                hwaddr zeta_end = zeta_entry.vram_addr + zeta_entry.size;
                is_compatible &= found->vram_addr >= zeta_end ||
                                 zeta_entry.vram_addr >= color_end;
            }

            if (is_compatible && !color && r->color_binding) {
                /* A zeta covering the colour target stays usable: a smaller
                 * pass touches the same rows and columns as on hardware (same
                 * pitch), and GL renders to the attachments' intersection. */
                is_compatible &= (found->width >= r->color_binding->width) &&
                                 (found->height >= r->color_binding->height);
            }

            if (is_compatible) {
                /* FIXME: Refactor */
                /* A larger zeta keeps the colour's render size. */
                if (color || !r->color_binding ||
                    (found->width == r->color_binding->width &&
                     found->height == r->color_binding->height)) {
                    pgraph_set_surface_binding_dim(pg, found->width,
                                                   found->height,
                                                   &found->shape);
                }
                found->upload_pending |= mem_dirty;
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
            } else {
                trace_nv2a_pgraph_surface_evict_reason(
                    "incompatible", found->vram_addr);
                compare_surfaces(found, &entry);
                /* The same range re-declared parks the old view with its
                 * debt, for the create below (parked_settle_for_create). */
                bool superseded = found->vram_addr == entry.vram_addr &&
                                  found->size == entry.size;
                if (superseded ||
                    pgraph_writeback_dead(d, found->vram_addr, found->size,
                                          found->cpu_reads)) {
                    pgraph_writeback_record_discard(&d->pgraph,
                                                    found->vram_addr,
                                                    found->size);
                    park_owed(d, found);
                } else {
                    surface_evict_eager(d, found);
                }
            }
        }

        if (should_create) {
            /* Left bound on the active unit. */
            surface_alloc_texture(pg, &entry);
            NV2A_GL_DLABEL(GL_TEXTURE, entry.gl_buffer,
                           "%s format: %0X, width: %d, height: %d "
                           "(addr %" HWADDR_PRIx ")",
                           color ? "color" : "zeta",
                           color ? pg->surface_shape.color_format
                                 : pg->surface_shape.zeta_format,
                           entry.width, entry.height, surface->offset);
            if (pgraph_writeback_discarded(&d->pgraph, entry.vram_addr,
                                           entry.size) &&
                !memory_region_test_and_clear_dirty(d->vram, entry.vram_addr,
                                                    entry.size,
                                                    DIRTY_MEMORY_NV2A)) {
                /* Dead-upload skip: see pgraph/writeback.h. */
                entry.upload_pending = false;
            }
            found = surface_put(d, entry.vram_addr, &entry);

            /* FIXME: Refactor */
            pgraph_set_surface_binding_dim(pg, entry.width, entry.height,
                                           &entry.shape);

            if (color && r->zeta_binding && (r->zeta_binding->width != entry.width || r->zeta_binding->height != entry.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

#define TRACE_ARGS found->vram_addr, found->width, found->height, \
                   found->swizzle ? "sz" : "ln", found->shape.anti_aliasing, \
                   found->shape.clip_x, found->shape.clip_width, \
                   found->shape.clip_y, found->shape.clip_height, found->pitch

        if (color) {
            if (should_create) {
                trace_nv2a_pgraph_surface_create_color(TRACE_ARGS);
            } else {
                trace_nv2a_pgraph_surface_hit_color(TRACE_ARGS);
            }

            r->color_binding = found;
        } else {
            if (should_create) {
                trace_nv2a_pgraph_surface_create_zeta(TRACE_ARGS);
            } else {
                trace_nv2a_pgraph_surface_hit_zeta(TRACE_ARGS);
            }
            r->zeta_binding = found;
        }
#undef TRACE_ARGS

        bind_current_surface(d);

        SurfaceBinding *bound = color ? r->color_binding : r->zeta_binding;
        if (bound) {
            pgraph_rt_memo_store(&r->rt_memo, color,
                                 color ? pg->dma_color : pg->dma_zeta,
                                 surface->offset, surface->pitch, bound);
        }

        surface->buffer_dirty = false;
    }

    if (!upload && surface->draw_dirty) {
        if (!tcg_enabled()) {
            /* FIXME: Cannot monitor for reads/writes; flush now */
            surface_download(d,
                             color ? r->color_binding :
                                     r->zeta_binding,
                             true);
        }

        surface->write_enabled_cache = false;
        surface->draw_dirty = false;
    }
}

void pgraph_gl_unbind_surface(NV2AState *d, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* No GL work: the pair FBOs are cached, and the caller rebinds
     * (pgraph_gl_surface_update) to keep the bound FBO in step. */
    if (color) {
        r->color_binding = NULL;
    } else {
        r->zeta_binding = NULL;
    }
}

static void touch_bound_surface(NV2AState *d, SurfaceBinding *s, bool upload,
                                bool swizzle)
{
    s->frame_time = d->pgraph.frame_time;
    if (upload) {
        pgraph_gl_upload_surface_data(d, s, false);
        s->draw_time = d->pgraph.draw_time;
        s->swizzle = swizzle;
    }
}

/* The bound surfaces serve this frame, and on upload this draw, with their
 * RAM content brought in. */
static void touch_bound_surfaces(NV2AState *d, bool upload)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (upload) {
        pg->draw_time++;
    }
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (r->color_binding) {
        touch_bound_surface(d, r->color_binding, upload, swizzle);
    }
    if (r->zeta_binding) {
        touch_bound_surface(d, r->zeta_binding, upload, swizzle);
    }
}

void pgraph_gl_surface_update(NV2AState *d, bool upload, bool color_write,
                           bool zeta_write)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    color_write = color_write &&
            (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write && (pg->clearing || pgraph_zeta_write_enabled(pg));

    if (upload) {
        bool fb_dirty = framebuffer_dirty(pg);

        /* Target registers, buffers, write masks and surface type unchanged:
         * the full path would be a no-op. TCG only: KVM must run the
         * dirty-memory test. */
        if (tcg_enabled() && !fb_dirty &&
            !pg->surface_color.buffer_dirty &&
            !pg->surface_zeta.buffer_dirty &&
            color_write == r->last_color_write &&
            zeta_write == r->last_zeta_write &&
            pg->surface_type == r->last_surface_type &&
            (!color_write || r->color_binding) &&
            (!zeta_write || r->zeta_binding)) {
            touch_bound_surfaces(d, true);
            return;
        }
        r->last_color_write = color_write;
        r->last_zeta_write = zeta_write;
        r->last_surface_type = pg->surface_type;

        if (fb_dirty) {
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
            r->rt_memo.shape_gen++;
        }

        if (pg->surface_color.buffer_dirty) {
            pgraph_gl_unbind_surface(d, true);
        }

        if (color_write) {
            update_surface_part(d, true, true);
        }

        if (pg->surface_zeta.buffer_dirty) {
            pgraph_gl_unbind_surface(d, false);
        }

        if (zeta_write) {
            update_surface_part(d, true, false);
        }

        /* An unbind no update_surface_part follows (that buffer's writes off)
         * leaves the previous pair's FBO bound, and GL clips rendering to the
         * smallest attachment: a stale small zeta would crop a colour write
         * the silicon bounds by the surface clip alone. */
        bind_current_surface(d);
    } else {
        if ((color_write || pg->surface_color.write_enabled_cache)
            && pg->surface_color.draw_dirty) {
            update_surface_part(d, false, true);
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache)
            && pg->surface_zeta.draw_dirty) {
            update_surface_part(d, false, false);
        }
    }

    touch_bound_surfaces(d, upload);

    // Sanity check: the zeta binding must cover the color target (it may
    // be larger when a big depth allocation is reused for a smaller pass).
    if (r->color_binding && r->zeta_binding) {
        assert((r->color_binding->width <= r->zeta_binding->width)
               && (r->color_binding->height <= r->zeta_binding->height));
    }

    surface_evict_old(d);
}

// FIXME: Move to common
static void surface_get_dimensions(PGRAPHState *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

void pgraph_gl_init_surfaces(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    pgraph_gl_reload_surface_scale_factor(pg);
    glGenFramebuffers(1, &r->gl_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, r->gl_framebuffer);
    r->fb_bound = r->gl_framebuffer;
    glGenFramebuffers(1, &r->gl_fb_util);
    QTAILQ_INIT(&r->surfaces);
    QTAILQ_INIT(&r->parked);
    r->downloads_pending = false;
    qemu_event_init(&r->downloads_complete, false);
    qemu_event_init(&r->dirty_surfaces_download_complete, false);

    init_render_to_texture(pg);
}

static void flush_surfaces(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* Clear last surface shape to force recreation of buffers at next draw */
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));
    pgraph_gl_unbind_surface(d, true);
    pgraph_gl_unbind_surface(d, false);

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        // FIXME: We should download all surfaces to ram, but need to
        //        investigate corruption issue
        // pgraph_gl_surface_download_if_dirty(d, s);
        pgraph_gl_surface_invalidate(d, s);
    }

    /* After a snapshot load or a reset RAM is authoritative: what the GPU
     * drew before must not be written over it, nor reused in its stead.
     * The pool's names also die with the context at a renderer switch. */
    QTAILQ_FOREACH_SAFE(s, &r->parked, entry, next) {
        s->writeback_owed = false;
        parked_destroy(d, s);
    }
    pgraph_writeback_reset(&d->pgraph);
    for (unsigned i = 0; i < r->texpool_n; i++) {
        glDeleteTextures(1, &r->texpool[i].tex);
    }
    r->texpool_n = 0;
}

void pgraph_gl_finalize_surfaces(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHGLState *r = pg->gl_renderer_state;

    flush_surfaces(d);
    glDeleteFramebuffers(1, &r->gl_framebuffer);
    r->gl_framebuffer = 0;
    glDeleteFramebuffers(1, &r->gl_fb_util);
    r->gl_fb_util = 0;
    for (unsigned i = 0; i < r->fb_cache_n; i++) {
        glDeleteFramebuffers(1, &r->fb_cache[i].fbo);
    }
    r->fb_cache_n = 0;
    for (unsigned i = 0; i < r->fb_multi_n; i++) {
        glDeleteFramebuffers(1, &r->fb_multi[i].fbo);
    }
    r->fb_multi_n = 0;
    glDeleteFramebuffers(1, &r->dl_shrink_fbo);
    glDeleteTextures(1, &r->dl_shrink_tex);
    glDeleteFramebuffers(1, &r->dl_zshrink_fbo);
    glDeleteTextures(1, &r->dl_zshrink_tex);
    r->dl_shrink_fbo = r->dl_shrink_tex = 0;
    r->dl_zshrink_fbo = r->dl_zshrink_tex = 0;
    glDeleteFramebuffers(1, &r->ul_grow_fbo);
    glDeleteTextures(1, &r->ul_grow_tex);
    glDeleteFramebuffers(1, &r->ul_zgrow_fbo);
    glDeleteTextures(1, &r->ul_zgrow_tex);
    r->ul_grow_fbo = r->ul_grow_tex = 0;
    r->ul_zgrow_fbo = r->ul_zgrow_tex = 0;

    finalize_render_to_texture(pg);
}

void pgraph_gl_surface_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    bool update_surface = (r->color_binding || r->zeta_binding);

    flush_surfaces(d);

    pgraph_gl_reload_surface_scale_factor(pg);

    if (update_surface) {
        pgraph_gl_surface_update(d, true, true, true);
    }
}

/* Flip-time pre-download of a surface the CPU read last frame, not waiting
 * when read-backs are asynchronous; it stays dirty until a read harvests it. */
void pgraph_gl_surface_predownload(NV2AState *d, SurfaceBinding *surface)
{
    if (!surface->draw_dirty) {
        return;
    }
    surface_download_issue(d, surface);
}

void pgraph_gl_surface_download_if_dirty(NV2AState *d,
                                         SurfaceBinding *surface)
{
    if (!surface->draw_dirty) {
        return;
    }
    surface_download_if_dirty_impl(d, surface);
}

void pgraph_gl_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force)
{
    if (!(surface->upload_pending || force)) {
        return;
    }
    upload_surface_data_impl(d, surface, force);
}
