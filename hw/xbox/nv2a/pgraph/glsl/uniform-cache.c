/*
 * Geforce NV2A PGRAPH uniform tracking, shared by the renderers
 *
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
#include "uniform-cache.h"

const int8_t pgraph_vsh_uniform_section[VshUniform__COUNT] = {
    [VshUniform_c] = UNISEC_VSHC,
    [VshUniform_ltctxa] = UNISEC_LTCTX,
    [VshUniform_ltctxb] = UNISEC_LTCTX,
    [VshUniform_ltc1] = UNISEC_LTCTX,
    [VshUniform_lightInfiniteDirection] = UNISEC_LIGHT,
    [VshUniform_lightInfiniteHalfVector] = UNISEC_LIGHT,
    [VshUniform_lightLocalAttenuation] = UNISEC_LIGHT,
    [VshUniform_lightLocalPosition] = UNISEC_LIGHT,
    [VshUniform_pointParams] = UNISEC_PP,
    /* Only from NV_PGRAPH_FOGPARAM0/1. */
    [VshUniform_fogParam] = UNISEC_FOGP,
};

/* The shared setter fills the live fields through the binding's locs and
 * the tracked sections through 0, a clean section masked to -1. */
void pgraph_vsh_uniform_source_refresh(PGRAPHState *pg, VshUniformSource *src,
                                       const VshState *state,
                                       const VshUniformLocs binding_locs)
{
    bool ff = state->is_fixed_function;
    bool dirty[UNISEC__COUNT] = { false };

    if (!src->valid) {
        /* First fill: every section, whatever its write marker. */
        dirty[UNISEC_VSHC] = true;
        dirty[UNISEC_PP] = true;
        dirty[UNISEC_FOGP] = true;
    } else {
        dirty[UNISEC_VSHC] = memchr(pg->vsh_constants_dirty, 1,
                                    sizeof(pg->vsh_constants_dirty)) != NULL;
        dirty[UNISEC_PP] = pg->point_params_dirty;
        dirty[UNISEC_FOGP] = pg->fog_params_dirty;
    }
    /* ltctx/light only fill under a fixed-function state (the shared
     * setter gates them); leave them pending until an FF draw. */
    if (ff && !src->ff_valid) {
        dirty[UNISEC_LTCTX] = true;
        dirty[UNISEC_LIGHT] = true;
    } else if (ff) {
        dirty[UNISEC_LTCTX] =
            memchr(pg->ltctxa_dirty, 1, sizeof(pg->ltctxa_dirty)) ||
            memchr(pg->ltctxb_dirty, 1, sizeof(pg->ltctxb_dirty)) ||
            memchr(pg->ltc1_dirty, 1, sizeof(pg->ltc1_dirty));
        dirty[UNISEC_LIGHT] = pg->light_dirty;
    }

    VshUniformLocs fill_locs;
    for (int i = 0; i < VshUniform__COUNT; i++) {
        int s = pgraph_vsh_uniform_section[i];
        fill_locs[i] = s == UNISEC_LIVE ? binding_locs[i] :
                       dirty[s] ? 0 : -1;
    }
    pgraph_glsl_set_vsh_uniform_values(pg, state, fill_locs, &src->values);

    if (dirty[UNISEC_VSHC]) {
        src->gen[UNISEC_VSHC]++;
        memset(pg->vsh_constants_dirty, 0, sizeof(pg->vsh_constants_dirty));
    }
    if (dirty[UNISEC_PP]) {
        src->gen[UNISEC_PP]++;
        pg->point_params_dirty = false;
    }
    if (dirty[UNISEC_FOGP]) {
        src->gen[UNISEC_FOGP]++;
        pg->fog_params_dirty = false;
    }
    if (dirty[UNISEC_LTCTX]) {
        src->gen[UNISEC_LTCTX]++;
        memset(pg->ltctxa_dirty, 0, sizeof(pg->ltctxa_dirty));
        memset(pg->ltctxb_dirty, 0, sizeof(pg->ltctxb_dirty));
        memset(pg->ltc1_dirty, 0, sizeof(pg->ltc1_dirty));
    }
    if (dirty[UNISEC_LIGHT]) {
        src->gen[UNISEC_LIGHT]++;
        pg->light_dirty = false;
    }
    src->valid = true;
    if (ff) {
        src->ff_valid = true;
    }
}

/* Open addressing over 8 slots from a Knuth multiplicative hash; a key
 * that finds none is uploaded in full every time. */
int uniform_cache_slot(UniformCache *c, uintptr_t key)
{
    unsigned int h =
        (unsigned int)((key >> 4) ^ key) * 2654435761u % UNIFORM_CACHE_SLOTS;
    for (unsigned int i = 0; i < 8; i++) {
        unsigned int s = (h + i) % UNIFORM_CACHE_SLOTS;
        if (c->e[s].key == key) {
            return s;
        }
        if (c->e[s].key == 0) {
            c->e[s].key = key;
            c->e[s].valid = false;
            c->e[s].sec_valid = false;
            return s;
        }
    }
    return -1;
}

void uniform_cache_forget(UniformCache *c, uintptr_t key)
{
    for (unsigned int i = 0; i < UNIFORM_CACHE_SLOTS; i++) {
        if (c->e[i].key == key) {
            c->e[i].key = 0;
            c->e[i].valid = false;
            c->e[i].sec_valid = false;
        }
    }
}

void uniform_cache_invalidate(UniformCache *c)
{
    for (unsigned int i = 0; i < UNIFORM_CACHE_SLOTS; i++) {
        c->e[i].key = 0;
        c->e[i].valid = false;
        c->e[i].sec_valid = false;
    }
}

bool uniform_cache_apply(UniformCache *c, int slot,
                         const unsigned int *src_gen, const UniformInfo *info,
                         const int *locs, const void *values, size_t count,
                         const int8_t *sections, UniformUploadFn upload,
                         void *opaque)
{
    UniformCacheEntry *e = slot >= 0 ? &c->e[slot] : NULL;
    char *cache = e ? (char *)&e->v : NULL;
    bool cache_valid = e && e->valid;
    bool changed = false;

    /* A section this program already holds at the current generation
     * needs neither compare nor upload. */
    bool sec_skip[UNISEC__COUNT] = { false };
    if (sections && cache_valid && e->sec_valid) {
        for (int s = UNISEC_LIVE + 1; s < UNISEC__COUNT; s++) {
            sec_skip[s] = e->sec_gen[s] == src_gen[s];
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (locs[i] == -1) {
            continue;
        }
        if (sections && sec_skip[(int)sections[i]]) {
            continue;
        }
        const void *value = (const char *)values + info[i].val_offs;
        size_t sz = info[i].size * info[i].count;
        if (cache_valid && memcmp(cache + info[i].val_offs, value, sz) == 0) {
            continue;
        }
        if (cache) {
            memcpy(cache + info[i].val_offs, value, sz);
        }
        upload(opaque, &info[i], locs[i], value);
        changed = true;
    }

    if (!e) {
        return true;
    }
    e->valid = true;
    if (sections) {
        memcpy(e->sec_gen, src_gen, sizeof(e->sec_gen));
        e->sec_valid = true;
    }
    return changed;
}
