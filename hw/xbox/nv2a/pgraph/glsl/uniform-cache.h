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

#ifndef HW_XBOX_NV2A_PGRAPH_GLSL_UNIFORM_CACHE_H
#define HW_XBOX_NV2A_PGRAPH_GLSL_UNIFORM_CACHE_H

#include "vsh.h"
#include "psh.h"

/*
 * The vertex shader uniforms come in sections (on GL, with its shader
 * optimizations: pgraph_gpu_boost_gl), each tracked by the write markers of
 * its method handlers (vsh_constants_dirty[], ltctx*_dirty[], light, point
 * and fog params): a clean section is not re-read, compared or uploaded. A
 * renderer keeps, per program, the values it last gave it, and uploads only
 * the fields that differ.
 */
enum {
    UNISEC_LIVE = 0, /* filled and compared every draw */
    UNISEC_VSHC,
    UNISEC_LTCTX,
    UNISEC_LIGHT,
    UNISEC_PP,
    UNISEC_FOGP,
    UNISEC__COUNT
};

/* The section of each vertex shader uniform; unlisted fields are live. */
extern const int8_t pgraph_vsh_uniform_section[VshUniform__COUNT];

/* The current vertex shader uniform values, shared by every program:
 * ltctx and light fill only under fixed function, hence ff_valid. */
typedef struct VshUniformSource {
    VshUniformValues values;
    unsigned int gen[UNISEC__COUNT];
    bool valid;
    bool ff_valid;
} VshUniformSource;

/* Refill the live fields, and the tracked sections written since. */
void pgraph_vsh_uniform_source_refresh(PGRAPHState *pg, VshUniformSource *src,
                                       const VshState *state,
                                       const VshUniformLocs binding_locs);

#define UNIFORM_CACHE_SLOTS 512

/* The values a renderer last gave one program, keyed by a GL program name
 * or a Vulkan module address (0: a free slot). */
typedef struct UniformCacheEntry {
    uintptr_t key;
    bool valid;
    bool sec_valid;
    unsigned int sec_gen[UNISEC__COUNT];
    union {
        VshUniformValues vsh;
        PshUniformValues psh;
    } v;
} UniformCacheEntry;

typedef struct UniformCache {
    UniformCacheEntry e[UNIFORM_CACHE_SLOTS];
} UniformCache;

/* The slot of key, taking a free one; -1 when its probe run is full. */
int uniform_cache_slot(UniformCache *c, uintptr_t key);
/* Key's program is gone: a later one under the same key starts empty. */
void uniform_cache_forget(UniformCache *c, uintptr_t key);
/* The sources changed behind every program (snapshot load, reset): every
 * slot is freed. */
void uniform_cache_invalidate(UniformCache *c);

typedef void (*UniformUploadFn)(void *opaque, const UniformInfo *info,
                                int loc, const void *value);

/* Give a program the fields that differ from what slot holds (every field
 * when slot is -1), calling upload for each; sections, if given, skips the
 * tracked sections the slot holds at the source generation src_gen.
 * Returns whether anything was uploaded, or true without a slot. */
bool uniform_cache_apply(UniformCache *c, int slot,
                         const unsigned int *src_gen, const UniformInfo *info,
                         const int *locs, const void *values, size_t count,
                         const int8_t *sections, UniformUploadFn upload,
                         void *opaque);

#endif
