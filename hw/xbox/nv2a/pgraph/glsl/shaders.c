/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2025 Matt Borgerson
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
#include "shaders.h"

/* Bumped when a family table changes how a state is built (a family turning
 * dynamic, a module adopted): the dirty fast path must then rebuild. */
unsigned pgraph_glsl_dynamic_gen;

ShaderState pgraph_glsl_get_shader_state(PGRAPHState *pg)
{
    pg->program_data_dirty = false; /* fixme */

    ShaderState state;

    // We will hash it, so make sure any padding is zeroed
    memset(&state, 0, sizeof(ShaderState));

    pgraph_glsl_set_vsh_state(pg, &state.vsh);
    pgraph_glsl_set_geom_state(pg, &state.geom);
    pgraph_glsl_set_psh_state(pg, &state.psh);

    return state;
}

/* Registers the dirty map flags whole are compared on their _SHADER_FIELDS,
 * snapshotted here where the map is cleared, i.e. where the compared state
 * was built. */
void pgraph_glsl_snapshot_shader_regs(PGRAPHState *pg)
{
    pg->shader_reg_snap[0] = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                             NV_PGRAPH_SETUPRASTER_SHADER_FIELDS;
    for (int i = 0; i < 4; i++) {
        pg->shader_reg_snap[1 + i] =
            pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4) &
            NV_PGRAPH_TEXFMT0_SHADER_FIELDS;
        pg->shader_reg_snap[5 + i] =
            pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4) &
            NV_PGRAPH_TEXFILTER0_SHADER_FIELDS;
    }
}

static bool shader_reg_dirty(PGRAPHState *pg, unsigned int reg, uint32_t fields,
                             int slot)
{
    if (!pgraph_is_reg_dirty(pg, reg)) {
        return false;
    }
    return (pgraph_reg_r(pg, reg) & fields) != pg->shader_reg_snap[slot];
}

bool pgraph_glsl_check_shader_state_dirty(PGRAPHState *pg,
                                          const ShaderState *state)
{
    if (pg->program_data_dirty) {
        return true;
    }

    unsigned int regs[] = {
        NV_PGRAPH_COMBINECTL,      NV_PGRAPH_COMBINESPECFOG0,
        NV_PGRAPH_COMBINESPECFOG1, NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_3,       NV_PGRAPH_CSV0_C,
        NV_PGRAPH_CSV0_D,          NV_PGRAPH_CSV1_A,
        NV_PGRAPH_CSV1_B,          NV_PGRAPH_POINTSIZE,
        NV_PGRAPH_SHADERCLIPMODE,
        NV_PGRAPH_SHADERCTL,       NV_PGRAPH_SHADERPROG,
        NV_PGRAPH_SHADOWCTL,       NV_PGRAPH_ZCOMPRESSOCCLUDE,
    };
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (pgraph_is_reg_dirty(pg, regs[i])) {
            return true;
        }
    }

    if (shader_reg_dirty(pg, NV_PGRAPH_SETUPRASTER,
                         NV_PGRAPH_SETUPRASTER_SHADER_FIELDS, 0)) {
        return true;
    }

    {
        int num_stages = pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL) & 0xFF;
        for (int i = 0; i < num_stages; i++) {
            if (pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINEALPHAI0 + i * 4) ||
                pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINEALPHAO0 + i * 4) ||
                pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINECOLORI0 + i * 4) ||
                pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINECOLORO0 + i * 4)) {
                return true;
            }
        }
    }

    /* The key holds two-sided lighting only while lighting is on. */
    if (state->vsh.is_fixed_function &&
        !state->vsh.fixed_function.csv0c_dynamic &&
        (pg->two_side_light_en && state->vsh.fixed_function.lighting) !=
            state->vsh.fixed_function.two_sided) {
        return true;
    }

    if (pg->uniform_attrs != state->vsh.uniform_attrs ||
        pg->swizzle_attrs != state->vsh.swizzle_attrs ||
        pg->compressed_attrs != state->vsh.compressed_attrs ||
        pg->primitive_mode != state->geom.primitive_mode ||
        pg->surface_shape.zeta_format != state->psh.surface_zeta_format) {
        return true;
    }

    for (int i = 0; i < 4; i++) {
        if (pgraph_is_reg_dirty(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) ||
            shader_reg_dirty(pg, NV_PGRAPH_TEXFILTER0 + i * 4,
                             NV_PGRAPH_TEXFILTER0_SHADER_FIELDS, 5 + i) ||
            shader_reg_dirty(pg, NV_PGRAPH_TEXFMT0 + i * 4,
                             NV_PGRAPH_TEXFMT0_SHADER_FIELDS, 1 + i)) {
            return true;
        }

        /* Dynamic texgen zeroes the binding's enables (they live in the
         * texMatEnable uniform): comparing them would flag every draw. */
        if (!state->vsh.fixed_function.texgen_dynamic &&
            pg->texture_matrix_enable[i] !=
                state->vsh.fixed_function.texture_matrix_enable[i]) {
            return true;
        }
    }

    return false;
}
