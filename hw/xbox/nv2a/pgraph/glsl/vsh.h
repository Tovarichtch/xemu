/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2015 espes
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

#ifndef HW_XBOX_NV2A_PGRAPH_GLSL_VSH_H
#define HW_XBOX_NV2A_PGRAPH_GLSL_VSH_H

#include "common.h"
#include "hw/xbox/nv2a/pgraph/vsh_regs.h"

typedef struct PGRAPHState PGRAPHState;

typedef struct FixedFunctionVshState {
    bool normalization;
    bool texture_matrix_enable[4];
    enum VshTexgen texgen[4][4];
    enum VshFoggen foggen;
    enum VshSkinning skinning;
    bool lighting;
    /* When set, per-light modes come from the lightMode uniform (as the
     * hardware reads them from CSV0_D) instead of being baked in: one shader
     * covers every light configuration of this family. */
    bool light_dynamic;
    /* Same idea for the texgen modes and texture matrix enables (CSV1_A/B):
     * they come from the texgenMode/texMatEnable uniforms. */
    bool texgen_dynamic;
    /* CSV0_C wholesale: lighting master, local_eye, normalization, the four
     * material colour sources and the specular flags all come from the
     * csv0cCtl uniform; lights are always the dynamic loop. */
    bool csv0c_dynamic;
    enum VshLight light[NV2A_MAX_LIGHTS];
    enum MaterialColorSource emission_src;
    enum MaterialColorSource ambient_src;
    enum MaterialColorSource diffuse_src;
    enum MaterialColorSource specular_src;
    bool local_eye;
    /* NV097_SET_TWO_SIDE_LIGHT_EN: back-face colours are lit with the
     * negated normal and the back light/material registers instead of
     * passing the vertex back colours through. */
    bool two_sided;
} FixedFunctionVshState;

typedef struct ProgrammableVshState {
    uint32_t program_data[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][VSH_TOKEN_SIZE];
    int program_length;
} ProgrammableVshState;

typedef struct {
    uint16_t compressed_attrs;
    uint16_t uniform_attrs;
    uint16_t swizzle_attrs;
    /* Never set; kept because the shader seeds store VshState as it is. */
    bool const_attrs_dynamic;

    bool fog_enable;
    enum VshFogMode fog_mode;
    /* Fog enable/mode/foggen come from the fogCtl uniform (CONTROL_3 and
     * CSV0_D register fields) instead of being baked into the key. */
    bool fog_dynamic;

    bool specular_enable;
    bool separate_specular;
    bool ignore_specular_alpha;

    bool point_params_enable;

    bool smooth_shading;
    bool z_perspective;

    bool is_fixed_function;
    FixedFunctionVshState fixed_function;
    ProgrammableVshState programmable;
} VshState;

void pgraph_glsl_set_vsh_state(PGRAPHState *pg, VshState *state);

bool pgraph_glsl_vsh_dynamic_pending_take(VshState *out);
void pgraph_glsl_vsh_dynamic_ready(const VshState *state);

#define VSH_UNIFORM_DECL_X(S, DECL)                          \
    DECL(S, c, vec4, NV2A_VERTEXSHADER_CONSTANTS)            \
    DECL(S, clipRange, vec4, 1)                              \
    DECL(S, csv0cCtl, uint, 1)                               \
    DECL(S, fogCtl, uint, 1)                                 \
    DECL(S, fogParam, vec2, 1)                               \
    DECL(S, inlineValue, vec4, NV2A_VERTEXSHADER_ATTRIBUTES) \
    DECL(S, lightInfiniteDirection, vec3, NV2A_MAX_LIGHTS)   \
    DECL(S, lightInfiniteHalfVector, vec3, NV2A_MAX_LIGHTS)  \
    DECL(S, lightLocalAttenuation, vec3, NV2A_MAX_LIGHTS)    \
    DECL(S, lightLocalPosition, vec3, NV2A_MAX_LIGHTS)       \
    DECL(S, lightMode, uint, 1)                              \
    DECL(S, texgenMode, uint, 4)                             \
    DECL(S, texMatEnable, uint, 1)                           \
    DECL(S, ltc1, vec4, NV2A_LTC1_COUNT)                     \
    DECL(S, ltctxa, vec4, NV2A_LTCTXA_COUNT)                 \
    DECL(S, ltctxb, vec4, NV2A_LTCTXB_COUNT)                 \
    DECL(S, material_alpha, float, 1)                        \
    DECL(S, pointParams, float, 8)                           \
    DECL(S, pointSizeReg, float, 1)                          \
    DECL(S, specularPower, float, 1)                         \
    DECL(S, pointScale, float, 1)                            \
    DECL(S, surfaceSize, vec2, 1)

DECL_UNIFORM_TYPES(VshUniform, VSH_UNIFORM_DECL_X)

typedef struct GenVshGlslOptions {
    bool vulkan;
    /* Explicit locations on the stage interface (separable programs). */
    bool locations;
    bool prefix_outputs;
    bool use_push_constants_for_uniform_attrs;
    int ubo_binding;
} GenVshGlslOptions;

MString *pgraph_glsl_gen_vsh(const VshState *state,
                             GenVshGlslOptions glsl_opts);

void pgraph_glsl_set_vsh_uniform_values(PGRAPHState *pg, const VshState *state,
                                        const VshUniformLocs locs,
                                        VshUniformValues *values);

#endif
