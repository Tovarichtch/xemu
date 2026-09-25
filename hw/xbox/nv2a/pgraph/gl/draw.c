/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/cost.h"
#include "debug.h"
#include "renderer.h"

/* GL queries of the cost model (cost.c): fragment-shader invocations, a
 * target the game's zpass query never uses, and SAMPLES_PASSED while the
 * game's query is closed. */
static GLuint gl_cost_segq[XEMU_COST_SEGS];
static GLuint gl_cost_segq2[XEMU_COST_SEGS];
static int xemu_cost_probe = -1;

static bool gl_cost_seg_begin(NV2AState *d, int slot, bool want_samples)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    if (!gl_cost_segq[slot]) {
        glGenQueries(1, &gl_cost_segq[slot]);
    }
    glBeginQuery(GL_FRAGMENT_SHADER_INVOCATIONS_ARB, gl_cost_segq[slot]);
    if (want_samples && !r->gl_zpass_query_open) {
        if (!gl_cost_segq2[slot]) {
            glGenQueries(1, &gl_cost_segq2[slot]);
        }
        glBeginQuery(GL_SAMPLES_PASSED, gl_cost_segq2[slot]);
        return true;
    }
    return false;
}

static void gl_cost_seg_end(NV2AState *d)
{
    glEndQuery(GL_FRAGMENT_SHADER_INVOCATIONS_ARB);
    if (xemu_cost_q2_open) {
        glEndQuery(GL_SAMPLES_PASSED);
    }
}

static void gl_cost_seg_read(NV2AState *d, int slot, uint64_t *invocations,
                             uint64_t *samples)
{
    GLuint64 v = 0;
    glGetQueryObjectui64v(gl_cost_segq[slot], GL_QUERY_RESULT, &v);
    *invocations = v;
    if (samples) {
        GLuint64 s = 0;
        glGetQueryObjectui64v(gl_cost_segq2[slot], GL_QUERY_RESULT, &s);
        *samples = s;
    }
}

const XemuCostQueryOps pgraph_gl_cost_ops = {
    .seg_begin = gl_cost_seg_begin,
    .seg_end = gl_cost_seg_end,
    .seg_read = gl_cost_seg_read,
};

static inline bool cost_probe_active(void)
{
    if (!xemu_cost_model_active()) {
        return false;
    }
    if (xemu_cost_probe < 0) {
        xemu_cost_probe =
            epoxy_has_gl_extension("GL_ARB_pipeline_statistics_query");
        if (!xemu_cost_probe) {
            fprintf(stderr, "xemu: real-hw-speed: "
                    "GL_ARB_pipeline_statistics_query missing -- "
                    "cost model running without fragments (degraded)\n");
        }
    }
    return xemu_cost_probe;
}

/* The NV2A dithers to hide quantisation on 5/6-bit channel surfaces
 * (R5G6B5, X1R5G5B5); on 8888 targets host dithering only stipples
 * gradients, so the DITHERENABLE that games leave on stays off there.
 * REASONED from D3D dither semantics; the 8888 stipple is visible on screen. */
static bool pgraph_color_surface_dithers(PGRAPHState *pg)
{
    unsigned int f = pg->surface_shape.color_format;
    return f >= NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5 &&
           f <= NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5;
}

void pgraph_gl_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_DPRINTF("---------PRE CLEAR ------\n");
    pg->clearing = true;

    GLbitfield gl_mask = 0;

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    if (write_zeta) {
        GLint gl_clear_stencil;
        GLfloat gl_clear_depth;
        pgraph_get_clear_depth_stencil_value(pg, &gl_clear_depth,
                                             &gl_clear_stencil);

        if (parameter & NV097_CLEAR_SURFACE_Z) {
            gl_mask |= GL_DEPTH_BUFFER_BIT;
            glDepthMask(GL_TRUE);
            glClearDepth(gl_clear_depth);
        }
        if (parameter & NV097_CLEAR_SURFACE_STENCIL) {
            gl_mask |= GL_STENCIL_BUFFER_BIT;
            glStencilMask(0xff);
            glClearStencil(gl_clear_stencil);
        }
    }
    if (write_color) {
        gl_mask |= GL_COLOR_BUFFER_BIT;
        glColorMask((parameter & NV097_CLEAR_SURFACE_R)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_G)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_B)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_A)
                         ? GL_TRUE : GL_FALSE);

        GLfloat rgba[4];
        pgraph_get_clear_color(pg, rgba);
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    pgraph_gl_surface_update(d, true, write_color, write_zeta);

    /* FIXME: Needs confirmation */
    unsigned int xmin =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY), NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY), NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_DPRINTF(
        "------------------CLEAR 0x%x %d,%d - %d,%d  %x---------------\n",
        parameter, xmin, ymin, xmax, ymax,
        d->pgraph.regs_[NV_PGRAPH_COLORCLEARVALUE]);

    unsigned int scissor_width = xmax - xmin + 1,
                 scissor_height = ymax - ymin + 1;
    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    NV2A_DPRINTF("Translated clear rect to %d,%d - %d,%d\n", xmin, ymin,
                 xmin + scissor_width - 1, ymin + scissor_height - 1);

    bool full_clear = !xmin && !ymin &&
                      scissor_width >= pg->surface_binding_dim.width &&
                      scissor_height >= pg->surface_binding_dim.height;

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    /* FIXME: Respect window clip?!?! */
    glEnable(GL_SCISSOR_TEST);
    glScissor(xmin, ymin, scissor_width, scissor_height);

    /* Dither */
    if ((pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
         NV_PGRAPH_CONTROL_0_DITHERENABLE) &&
        pgraph_color_surface_dithers(pg)) {
        glEnable(GL_DITHER);
    } else {
        glDisable(GL_DITHER);
    }

    glClear(gl_mask);

    glDisable(GL_SCISSOR_TEST);
    /* The masks, the scissor and the dither set here are not the cached
     * draw state's: the next draw sets its own again. */
    pgraph_gl_draw_state_invalidate(r);

    pgraph_gl_set_surface_dirty(pg, write_color, write_zeta);

    if (r->color_binding) {
        r->color_binding->cleared = full_clear && write_color;
    }
    if (r->zeta_binding) {
        r->zeta_binding->cleared = full_clear && write_zeta;
    }
    
    pg->clearing = false;
}

/* Exactly the register fields the block reads, so a write outside them
 * changes no GL call: keep them in step with the block. */
#define GLST_CONTROL_0_FIELDS                                          \
    (NV_PGRAPH_CONTROL_0_ZENABLE | NV_PGRAPH_CONTROL_0_ZFUNC |         \
     NV_PGRAPH_CONTROL_0_DITHERENABLE |                                \
     NV_PGRAPH_CONTROL_0_ZWRITEENABLE |                                \
     NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |                          \
     NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |                            \
     NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |                          \
     NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
#define GLST_CONTROL_1_FIELDS                                             \
    (NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE |                            \
     NV_PGRAPH_CONTROL_1_STENCIL_FUNC | NV_PGRAPH_CONTROL_1_STENCIL_REF | \
     NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ |                              \
     NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE)
#define GLST_CONTROL_2_FIELDS                                          \
    (NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL |                             \
     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL |                            \
     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS)
#define GLST_BLEND_FIELDS                                              \
    (NV_PGRAPH_BLEND_EQN | NV_PGRAPH_BLEND_EN |                        \
     NV_PGRAPH_BLEND_SFACTOR | NV_PGRAPH_BLEND_DFACTOR)
#define GLST_SETUPRASTER_FIELDS                                           \
    (NV_PGRAPH_SETUPRASTER_CULLENABLE | NV_PGRAPH_SETUPRASTER_CULLCTRL |  \
     NV_PGRAPH_SETUPRASTER_FRONTFACE |                                    \
     NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE |                             \
     NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE)

void pgraph_gl_draw_state_invalidate(PGRAPHGLState *r)
{
    r->draw_state_key.valid = false;
}

/* Keeps xemu's own surface conversion draws out of the game's zpass count
 * and of the cost counts (why: xemu_cost_blit_suspend in cost.c). The
 * game's query is closed; its next enabled draw opens a new one, and the
 * report sums them. Returns whether a cost segment was suspended. */
bool pgraph_gl_own_draw_suspend(NV2AState *d)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;

    if (r->gl_zpass_query_open) {
        glEndQuery(GL_SAMPLES_PASSED);
        r->gl_zpass_query_open = false;
    }
    return xemu_cost_blit_suspend(d, &pgraph_gl_cost_ops);
}

void pgraph_gl_own_draw_resume(NV2AState *d)
{
    xemu_cost_blit_resume(d, &pgraph_gl_cost_ops);
}

void pgraph_gl_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_GL_DGROUP_BEGIN("NV097_SET_BEGIN_END: 0x%x", pg->primitive_mode);

    if (cost_probe_active()) {
        xemu_cost_draw_begin(d, &pgraph_gl_cost_ops);
    }

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_gl_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        return;
    }

    assert(r->color_binding || r->zeta_binding);

    {
        /* Lines wider than the host rasterizes go through the wide-line
         * geometry shader: decided here, before the shader state is keyed,
         * from the same width and range the glLineWidth below uses. */
        bool line_prim = pg->primitive_mode == PRIM_TYPE_LINES ||
                         pg->primitive_mode == PRIM_TYPE_LINE_LOOP ||
                         pg->primitive_mode == PRIM_TYPE_LINE_STRIP;
        bool aa = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
                           NV_PGRAPH_ANTIALIASING_ENABLE);
        bool smooth = !aa && (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                              NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE);
        float lw_px = (pg->line_width / 8.0f) * pg->surface_scale_factor;
        float host_max = smooth ? r->supported_smooth_line_width_range[1]
                                : r->supported_aliased_line_width_range[1];
        r->wide_lines = line_prim && lw_px > host_max;

        pgraph_gl_bind_textures(d);
        pgraph_gl_bind_shaders(pg);
    }

    {
        uint32_t key[13] = {
            pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) & GLST_CONTROL_0_FIELDS,
            pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & GLST_CONTROL_1_FIELDS,
            pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2) & GLST_CONTROL_2_FIELDS,
            pgraph_reg_r(pg, NV_PGRAPH_BLEND) & GLST_BLEND_FIELDS,
            pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR),
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & GLST_SETUPRASTER_FIELDS,
            pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
            pg->surface_binding_dim.width,
            pg->surface_binding_dim.height,
            pg->surface_shape.clip_x | (pg->surface_shape.clip_y << 16),
            pg->surface_shape.clip_width |
                (pg->surface_shape.clip_height << 16),
            pg->surface_scale_factor,
            /* Dithering follows the colour surface's channel depth. */
            pg->surface_shape.color_format,
        };
        if (r->draw_state_key.valid &&
            !memcmp(r->draw_state_key.k, key, sizeof(key))) {
            goto gl_state_done;
        }
        memcpy(r->draw_state_key.k, key, sizeof(key));
        r->draw_state_key.valid = true;
    }
    glColorMask(mask_red, mask_green, mask_blue, mask_alpha);
    glDepthMask(!!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE));
    glStencilMask(GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE));

    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
        glEnable(GL_BLEND);
        uint32_t sfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                    NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                    NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
        glBlendFunc(pgraph_blend_factor_gl_map[sfactor],
                    pgraph_blend_factor_gl_map[dfactor]);

        uint32_t equation = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                     NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_gl_map));
        glBlendEquation(pgraph_blend_equation_gl_map[equation]);

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        float gl_blend_color[4];
        pgraph_argb_pack32_to_rgba_float(blend_color, gl_blend_color);
        glBlendColor(gl_blend_color[0], gl_blend_color[1], gl_blend_color[2],
                     gl_blend_color[3]);
    } else {
        glDisable(GL_BLEND);
    }

    /* Face culling */
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER)
            & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_gl_map));
        glCullFace(pgraph_cull_face_gl_map[cull_face]);
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }

    /* Front-face select */
    /* Winding is reverse here because clip-space y-coordinates are inverted */
    glFrontFace(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER)
                    & NV_PGRAPH_SETUPRASTER_FRONTFACE
                        ? GL_CW : GL_CCW);

    /* Polygon offset is handled in geometry and fragment shaders explicitly */
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDisable(GL_POLYGON_OFFSET_POINT);

    /* Depth testing */
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);

        uint32_t depth_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                                       NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_gl_map));
        glDepthFunc(pgraph_depth_func_gl_map[depth_func]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }

    glEnable(GL_DEPTH_CLAMP);

    /* Set first vertex convention to match Vulkan default */
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);

    if (stencil_test) {
        glEnable(GL_STENCIL_TEST);

        uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                    NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                    NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t func_mask = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_gl_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_gl_map));

        glStencilFunc(
            pgraph_stencil_func_gl_map[stencil_func],
            stencil_ref,
            func_mask);

        glStencilOp(
            pgraph_stencil_op_gl_map[op_fail],
            pgraph_stencil_op_gl_map[op_zfail],
            pgraph_stencil_op_gl_map[op_zpass]);

    } else {
        glDisable(GL_STENCIL_TEST);
    }

    /* Dither */
    bool dither = (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
                   NV_PGRAPH_CONTROL_0_DITHERENABLE) &&
                  pgraph_color_surface_dithers(pg);
    if (dither) {
        glEnable(GL_DITHER);
    } else {
        glDisable(GL_DITHER);
    }

    glEnable(GL_PROGRAM_POINT_SIZE);

    bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING), NV_PGRAPH_ANTIALIASING_ENABLE);

    /* Edge Antialiasing; the line width is posed after gl_state_done. */
    r->line_smooth_on = !anti_aliasing &&
                        (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                         NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE);
    if (r->line_smooth_on) {
        glEnable(GL_LINE_SMOOTH);
    } else {
        glDisable(GL_LINE_SMOOTH);
    }
    if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                              NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
        glEnable(GL_POLYGON_SMOOTH);
    } else {
        glDisable(GL_POLYGON_SMOOTH);
    }

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
    glViewport(0, 0, vp_width, vp_height);

    /* Surface clip */
    /* FIXME: Consider moving to PSH w/ window clip */
    unsigned int xmin = pg->surface_shape.clip_x,
                 ymin = pg->surface_shape.clip_y;

    unsigned int scissor_width = pg->surface_shape.clip_width,
                 scissor_height = pg->surface_shape.clip_height;

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);
    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    glEnable(GL_SCISSOR_TEST);
    glScissor(xmin, ymin, scissor_width, scissor_height);

gl_state_done:;
    /* NV097_SET_LINE_WIDTH is in 1/8 pixel; posed every draw, as it changes
     * too often for the cached block. The clamp is host plumbing (0 is a GL
     * error, the top is driver dependent); wider lines go to the geometry
     * shader. */
    GLfloat line_width = (pg->line_width / 8.0f) * pg->surface_scale_factor;
    const GLfloat *lw_range = r->line_smooth_on
                                  ? r->supported_smooth_line_width_range
                                  : r->supported_aliased_line_width_range;
    glLineWidth(MAX(lw_range[0], MIN(lw_range[1], line_width)));
    if (r->shader_binding && r->shader_binding->state.geom.wide_lines) {
        pgraph_gl_wide_line_uniforms(pg);
    }

    /* Visibility testing. The hardware zpass count is a running sum
     * sampled by GET_REPORT, not a per-draw event (the XDK brackets each
     * visibility test as CLEAR+ENABLE ... draws ... ENABLE(0)+REPORT):
     * one query spanning the whole interval is the exact semantic, at a
     * fraction of the driver cost of one query per draw. */
    if (pg->zpass_pixel_count_enable) {
        if (!r->gl_zpass_query_open) {
            nv2a_profile_inc_counter(NV2A_PROF_QUERY);
            r->gl_zpass_pixel_count_query_count++;
            r->gl_zpass_pixel_count_queries = (GLuint*)g_realloc(
                r->gl_zpass_pixel_count_queries,
                sizeof(GLuint) * r->gl_zpass_pixel_count_query_count);

            GLuint gl_query;
            glGenQueries(1, &gl_query);
            r->gl_zpass_pixel_count_queries[
                r->gl_zpass_pixel_count_query_count - 1] = gl_query;
            /* The game's query takes SAMPLES_PASSED: end the cost model's
             * and mark its sample count partial. */
            if (xemu_cost_q2_open) {
                glEndQuery(GL_SAMPLES_PASSED);
                xemu_cost_samples_interrupted();
            }
            glBeginQuery(GL_SAMPLES_PASSED, gl_query);
            r->gl_zpass_query_open = true;
        }
    } else if (r->gl_zpass_query_open) {
        /* The counter only accumulates enabled draws: close on the
         * transition so disabled draws stay out of the sum. */
        glEndQuery(GL_SAMPLES_PASSED);
        r->gl_zpass_query_open = false;
    }
}

void pgraph_gl_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (cost_probe_active() && !is_nop_draw) {
        xemu_cost_draw_end_tally(pg);
    }

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_GL_DGROUP_END();
        return;
    }

    pgraph_gl_flush_draw(d);
    r->work_since_flush = true;

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_gl_set_surface_dirty(pg, color_write, depth_test || stencil_test);
    NV2A_GL_DGROUP_END();
}

void pgraph_gl_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return;
    }
    assert(r->shader_binding);

    /* per-vertex cost of the current config (constant for the flush) */
    unsigned long long vcost_milli = 0, vbytes_milli = 0;
    if (cost_probe_active()) {
        vcost_milli = xemu_cost_vert_cost_milli(pg);
        vbytes_milli = xemu_cost_attr_bytes(pg) * 1000ull;
    }

    if (pg->draw_arrays_length) {
        NV2A_GL_DPRINTF(false, "Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);
        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        pgraph_gl_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                      pg->draw_arrays_max_count - 1,
                                      false, 0,
                                      pg->draw_arrays_max_count - 1, false);
        glMultiDrawArrays(r->shader_binding->gl_primitive_mode,
                          pg->draw_arrays_start,
                          pg->draw_arrays_count,
                          pg->draw_arrays_length);
        if (cost_probe_active()) {
            for (unsigned int i = 0; i < pg->draw_arrays_length; i++) {
                xemu_cost_add_verts(pg->draw_arrays_count[i], vcost_milli,
                                    vbytes_milli);
            }
        }
    } else if (pg->inline_elements_length) {
        NV2A_GL_DPRINTF(false, "Inline Elements");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i=0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }

        pgraph_gl_bind_vertex_attributes(
                d, min_element, max_element, false, 0,
                pg->inline_elements[pg->inline_elements_length - 1], true);

        VertexKey k;
        memset(&k, 0, sizeof(VertexKey));
        k.count = pg->inline_elements_length;
        k.gl_type = GL_UNSIGNED_INT;
        k.gl_normalize = GL_FALSE;
        k.stride = sizeof(uint32_t);
        uint64_t h = fast_hash((uint8_t*)pg->inline_elements,
                               pg->inline_elements_length * 4);

        LruNode *node = lru_lookup(&r->element_cache, h, &k);
        VertexLruNode *found = container_of(node, VertexLruNode, node);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, found->gl_buffer);
        if (!found->initialized) {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         pg->inline_elements_length * 4,
                         pg->inline_elements, GL_STATIC_DRAW);
            found->initialized = true;
        } else {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4_NOTDIRTY);
        }
        glDrawElementsBaseVertex(r->shader_binding->gl_primitive_mode,
                                 pg->inline_elements_length,
                                 GL_UNSIGNED_INT,
                                 (void *)0, r->draw_basevertex);
        if (cost_probe_active()) {
            xemu_cost_add_verts(pg->inline_elements_length, vcost_milli,
                                vbytes_milli);
        }
    } else if (pg->inline_buffer_length) {
        NV2A_GL_DPRINTF(false, "Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        if (pg->compressed_attrs) {
            pg->compressed_attrs = 0;
            pgraph_gl_bind_shaders(pg);
        }

        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->inline_buffer_populated) {
                nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
                glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_buffer[i]);
                glBufferData(GL_ARRAY_BUFFER,
                             pg->inline_buffer_length * sizeof(float) * 4,
                             attr->inline_buffer, GL_STREAM_DRAW);
                pgraph_gl_attr_pointer(r, i, r->gl_inline_buffer[i], 4,
                                       GL_FLOAT, GL_FALSE, 0, 0, false);
                pgraph_gl_attr_enable(r, i, true);
                attr->inline_buffer_populated = false;
                memcpy(attr->inline_value,
                       attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                       sizeof(attr->inline_value));
            } else {
                pgraph_gl_attr_enable(r, i, false);
                pgraph_gl_attr_value(r, i, attr->inline_value);
            }
        }

        glDrawArrays(r->shader_binding->gl_primitive_mode,
                     0, pg->inline_buffer_length);
        xemu_cost_add_verts(pg->inline_buffer_length, vcost_milli,
                            vbytes_milli);
    } else if (pg->inline_array_length) {
        NV2A_GL_DPRINTF(false, "Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        unsigned int index_count = pgraph_gl_bind_inline_array(d);
        glDrawArrays(r->shader_binding->gl_primitive_mode,
                     0, index_count);
        xemu_cost_add_verts(index_count, vcost_milli, vbytes_milli);
    } else {
        NV2A_GL_DPRINTF(true, "EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
}
