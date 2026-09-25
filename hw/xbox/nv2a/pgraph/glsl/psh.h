/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2013 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
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

#ifndef HW_XBOX_NV2A_PGRAPH_GLSL_PSH_H
#define HW_XBOX_NV2A_PGRAPH_GLSL_PSH_H

#include "common.h"
#include "hw/xbox/nv2a/pgraph/psh_regs.h"

typedef struct PGRAPHState PGRAPHState;

enum PshDepthFormat {
    DEPTH_FORMAT_D24,
    DEPTH_FORMAT_D16,
    DEPTH_FORMAT_F24,
    DEPTH_FORMAT_F16,
};

typedef struct PshState {
    uint32_t combiner_control;
    uint32_t shader_stage_program;
    uint32_t other_stage_input;
    uint32_t final_inputs_0;
    uint32_t final_inputs_1;

    uint32_t rgb_inputs[8], rgb_outputs[8];
    uint32_t alpha_inputs[8], alpha_outputs[8];

    bool combiner_dynamic;
    bool alpha_dynamic;
    /* Never set; kept because the shader seeds store PshState as it is. */
    bool universal;

    bool point_sprite;
    bool rect_tex[4];
    bool snorm_tex[4];
    bool compare_mode[4][4];
    bool alphakill[4];
    int colorkey_mode[4];
    enum ConvolutionFilter conv_tex[4];
    bool tex_x8y24[4];
    int dim_tex[4];
    bool tex_cubemap[4];

    bool border_adjust[4];

    bool shadow_map[4];
    enum PshShadowDepthFunc shadow_depth_func;

    bool alpha_test;

    bool window_clip_exclusive;

    bool smooth_shading;
    bool depth_clipping;
    bool z_perspective;

    unsigned int surface_zeta_format;
    enum PshDepthFormat depth_format;
} PshState;

void pgraph_glsl_set_psh_state(PGRAPHState *pg, PshState *state);

bool pgraph_glsl_psh_dynamic_pending_take(PshState *out);
void pgraph_glsl_psh_dynamic_ready(const PshState *state);
bool pgraph_glsl_psh_canonicalize_dynamic(PshState *state);

#define PSH_UNIFORM_DECL_X(S, DECL) \
    DECL(S, alphaRef, int, 1)       \
    DECL(S, alphaFunc, int, 1)      \
    DECL(S, alphaKillMask, uint, 1) \
    DECL(S, alphaTestEnable, int, 1) \
    DECL(S, borderLogicalSize, vec3, 4) \
    DECL(S, borderInvRealSize, vec3, 4) \
    DECL(S, bumpMat, mat2, 4)       \
    DECL(S, bumpOffset, float, 4)   \
    DECL(S, bumpScale, float, 4)    \
    DECL(S, clipRange, vec4, 1)     \
    DECL(S, clipRegion, ivec4, 8)   \
    DECL(S, colorKey, uint, 4)      \
    DECL(S, colorKeyMask, uint, 4)  \
    DECL(S, combAlphaIn, uint, 8)   \
    DECL(S, combAlphaOut, uint, 8)  \
    DECL(S, combCtl, uint, 1)       \
    DECL(S, combFinal0, uint, 1)    \
    DECL(S, combFinal1, uint, 1)    \
    DECL(S, combRgbIn, uint, 8)     \
    DECL(S, combRgbOut, uint, 8)    \
    DECL(S, consts, vec4, 18)       \
    DECL(S, depthFactor, float, 1)  \
    DECL(S, depthOffset, float, 1)  \
    DECL(S, fogColor, vec4, 1)      \
    DECL(S, surfaceScale, ivec2, 1) \
    DECL(S, texScale, float, 4)     \
    DECL(S, twoSideSel, uint, 1)

DECL_UNIFORM_TYPES(PshUniform, PSH_UNIFORM_DECL_X)

typedef struct GenPshGlslOptions {
    bool vulkan;
    bool locations;
    int ubo_binding;
    int tex_binding;
} GenPshGlslOptions;

MString *pgraph_glsl_gen_psh(const PshState *state, GenPshGlslOptions opts);

void pgraph_glsl_set_psh_uniform_values(PGRAPHState *pg,
                                        const PshUniformLocs locs,
                                        PshUniformValues *values);

#endif
