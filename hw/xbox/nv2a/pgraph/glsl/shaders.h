/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2025 Matt Borgerson
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

#ifndef HW_XBOX_NV2A_PGRAPH_GLSL_SHADERS_H
#define HW_XBOX_NV2A_PGRAPH_GLSL_SHADERS_H

#include "vsh.h"
#include "geom.h"
#include "psh.h"

typedef struct ShaderState {
    VshState vsh;
    GeomState geom;
    PshState psh;
} ShaderState;

typedef struct PGRAPHState PGRAPHState;

extern unsigned pgraph_glsl_dynamic_gen;
ShaderState pgraph_glsl_get_shader_state(PGRAPHState *pg);

/* The dirty map flags a register whole, but only some of a register's
 * fields are read when a shader state is built: a write landing outside
 * them cannot change the generated code. These are exactly the fields
 * the generators read -- keep them in step with vsh.c/geom.c/psh.c. */
#define NV_PGRAPH_SETUPRASTER_SHADER_FIELDS       \
    (NV_PGRAPH_SETUPRASTER_FRONTFACEMODE |        \
     NV_PGRAPH_SETUPRASTER_BACKFACEMODE |         \
     NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE |    \
     NV_PGRAPH_SETUPRASTER_Z_FORMAT |             \
     NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE)
#define NV_PGRAPH_TEXFMT0_SHADER_FIELDS \
    (NV_PGRAPH_TEXFMT0_CUBEMAPENABLE |  \
     NV_PGRAPH_TEXFMT0_BORDER_SOURCE |  \
     NV_PGRAPH_TEXFMT0_DIMENSIONALITY | \
     NV_PGRAPH_TEXFMT0_COLOR)
#define NV_PGRAPH_TEXFILTER0_SHADER_FIELDS \
    (NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL | NV_PGRAPH_TEXFILTER0_MIN)

bool pgraph_glsl_check_shader_state_dirty(PGRAPHState *pg,
                                          const ShaderState *state);

#endif
