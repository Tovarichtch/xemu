/*
 * Geforce NV2A PGRAPH shader seeds -- one file per game
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
#ifndef HW_XBOX_NV2A_PGRAPH_SEED_H
#define HW_XBOX_NV2A_PGRAPH_SEED_H

#include <stdbool.h>
#include <stddef.h>

/* Shader seeds: the shader families a game was seen using, so a shader is
 * (almost) never compiled inside a frame. Two sources, read together:
 *   1. the embedded dictionary (data/shader_seeds.bin, built from the seed
 *      files recorded while playing each game, so it holds their shader
 *      states, vertex programs included): what a fresh install gets on its
 *      first play;
 *   2. the local file shader_seeds/<game>.seed, keyed by the game's name (a
 *      Chihiro game's MAME name, found by its disc's identifier): the union
 *      of what was on disk and what each session drew, rewritten at shutdown
 *      by the backend that ran (GL sections 1-2, Vulkan section 3).
 * During SEGABOOT a seeded state gets its modules, its dynamic-sibling cover
 * and (Vulkan) its pipeline libraries built. An unseeded state of a known
 * family is drawn at once with its dynamic sibling while a worker compiles
 * the exact program (first meet); only a family never seen pays one
 * synchronous compile, then it is learned.
 *
 * File layout (little-endian): header { magic "XSED", version, section
 * count, 0 } then per section { id, item size, item count, 0 } + items.
 * A section whose item size differs from the running build is skipped
 * (a stale seed is inert, never harmful); unknown sections are kept
 * verbatim on rewrite. */
enum {
    PGRAPH_SEED_GL_MODULES = 1, /* ShaderModuleCacheKey (OpenGL) */
    PGRAPH_SEED_GL_STATES = 2,  /* ShaderState as the OpenGL backend keys it */
    PGRAPH_SEED_VK_STATES = 3,  /* ShaderState as the Vulkan backend keys it */
};

/* Renames the seed file `from`, a name the game's seeds had before, to `to`,
 * unless `to` exists. */
void pgraph_seed_rename(const char *from, const char *to);

/* Items of one section of the current game's local seed (NULL when the
 * game is not named yet, the file is absent or the section is stale).
 * The caller frees the result. */
void *pgraph_seed_read(int section, size_t itemsize, unsigned *count);

/* Same, from the embedded dictionary. */
void *pgraph_seed_dict_read(int section, size_t itemsize, unsigned *count);

/* Replace one section of the current game's local seed, keeping every
 * other section as it is on disk. Atomic (temporary file + rename). */
bool pgraph_seed_write(int section, size_t itemsize, const void *items,
                       unsigned count);

/* A set of 64-bit item hashes for the seed dedups: an item whose hash
 * collides with another counts as known and is left out, which only costs
 * it its warm start. */
GHashTable *pgraph_seed_hash_set_new(void);
/* Adds h; false when the set already holds it. */
bool pgraph_seed_hash_set_add(GHashTable *set, uint64_t h);

/* Merge a session's items into one section of the current game's local
 * seed: the file's items, then the new ones while the section holds fewer
 * than cap (a short session never shrinks a seed learned over a long one).
 * The file is rewritten only when an item is new. Returns the number of new
 * items written; *total gets the section's item count. */
unsigned pgraph_seed_merge(int section, size_t itemsize, const void *items,
                           unsigned count, unsigned cap, unsigned *total);

#endif
