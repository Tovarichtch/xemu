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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "hw/xbox/nv2a/pgraph/seed.h"
#include <zlib.h>
#include "data/shader_seeds.bin.h"

#define SEED_MAGIC 0x44455358u /* "XSED" */
#define SEED_VERSION 1u
#define SEED_MAX_ITEMS 65536u

typedef struct SeedHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t nsections;
    uint32_t reserved;
} SeedHeader;

typedef struct SeedSection {
    uint32_t id;
    uint32_t itemsize;
    uint32_t count;
    uint32_t reserved;
} SeedSection;

static char *seed_path(void)
{
    const char *tag = pgraph_seed_game_tag();
    if (!tag) {
        return NULL;
    }
    char *dir = g_strdup_printf("%sshader_seeds",
                                xemu_settings_get_base_path());
    qemu_mkdir(dir);
    char *path = g_strdup_printf("%s/%s.seed", dir, tag);
    g_free(dir);
    return path;
}

/* Walk the sections of a seed image; `visit` returns false to stop.
 * Returns false when the image is not a seed file at all. */
static bool seed_walk(const uint8_t *buf, size_t len,
                      bool (*visit)(const SeedSection *sec,
                                    const uint8_t *items, void *opaque),
                      void *opaque)
{
    SeedHeader hdr;
    if (len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != SEED_MAGIC || hdr.version != SEED_VERSION) {
        return false;
    }
    size_t off = sizeof(hdr);
    for (uint32_t i = 0; i < hdr.nsections; i++) {
        SeedSection sec;
        if (len - off < sizeof(sec)) {
            return false;
        }
        memcpy(&sec, buf + off, sizeof(sec));
        off += sizeof(sec);
        size_t bytes = (size_t)sec.itemsize * sec.count;
        if (sec.count > SEED_MAX_ITEMS || len - off < bytes) {
            return false;
        }
        if (!visit(&sec, buf + off, opaque)) {
            return true;
        }
        off += bytes;
    }
    return true;
}

struct SeedFind {
    int section;
    size_t itemsize;
    void *items;
    unsigned count;
};

static bool seed_find_visit(const SeedSection *sec, const uint8_t *items,
                            void *opaque)
{
    struct SeedFind *f = opaque;
    if (sec->id != (uint32_t)f->section || sec->itemsize != f->itemsize ||
        sec->count == 0) {
        return true;
    }
    f->items = g_memdup2(items, (size_t)sec->itemsize * sec->count);
    f->count = sec->count;
    return false;
}

static void *seed_find(const uint8_t *buf, size_t len, int section,
                       size_t itemsize, unsigned *count)
{
    struct SeedFind f = { .section = section, .itemsize = itemsize };
    seed_walk(buf, len, seed_find_visit, &f);
    *count = f.count;
    return f.items;
}

void *pgraph_seed_read(int section, size_t itemsize, unsigned *count)
{
    *count = 0;
    char *path = seed_path();
    if (!path) {
        return NULL;
    }
    gchar *buf = NULL;
    gsize len = 0;
    void *items = NULL;
    if (g_file_get_contents(path, &buf, &len, NULL)) {
        items = seed_find((const uint8_t *)buf, len, section, itemsize, count);
        g_free(buf);
    }
    g_free(path);
    return items;
}

/* The embedded dictionary, data/shader_seeds.bin: a header, one entry per
 * game, then the zlib-compressed seed files of those games in entry order.
 * It covers the very first play of a fresh install; a local seed learned
 * since is read alongside it by the callers. Regenerate it whenever the
 * ShaderState or ShaderModuleCacheKey layouts change: the item-size check
 * rejects a dictionary built for another size, not a layout change that
 * keeps the size. */
#define SEED_DICT_MAGIC 0x44445358u /* "XSDD" */
#define SEED_DICT_VERSION 1u

typedef struct SeedDictHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t ngames;
} SeedDictHeader;

typedef struct SeedDictEntry {
    char game[32]; /* NUL-padded */
    uint32_t raw_len;
    uint32_t z_len;
} SeedDictEntry;

void *pgraph_seed_dict_read(int section, size_t itemsize, unsigned *count)
{
    *count = 0;
    const char *tag = pgraph_seed_game_tag();
    /* An entry name holds at most 31 characters and its NUL. */
    if (!tag || strlen(tag) >= sizeof_field(SeedDictEntry, game)) {
        return NULL;
    }
    SeedDictHeader h;
    if (shader_seeds_size < sizeof(h)) {
        return NULL;
    }
    memcpy(&h, shader_seeds_data, sizeof(h));
    if (h.magic != SEED_DICT_MAGIC || h.version != SEED_DICT_VERSION ||
        h.ngames > (shader_seeds_size - sizeof(h)) / sizeof(SeedDictEntry)) {
        return NULL;
    }
    size_t off = sizeof(h) + h.ngames * sizeof(SeedDictEntry);
    for (uint32_t i = 0; i < h.ngames; i++) {
        SeedDictEntry e;
        memcpy(&e, shader_seeds_data + sizeof(h) + i * sizeof(e), sizeof(e));
        if (e.z_len > shader_seeds_size - off) {
            return NULL;
        }
        const uint8_t *z = shader_seeds_data + off;
        off += e.z_len;
        if (strncmp(e.game, tag, sizeof(e.game)) != 0) {
            continue;
        }
        if (!e.raw_len || e.raw_len > (64u << 20)) {
            return NULL;
        }
        uint8_t *raw = g_malloc(e.raw_len);
        uLongf dlen = e.raw_len;
        void *items = NULL;
        if (uncompress(raw, &dlen, z, e.z_len) == Z_OK &&
            dlen == e.raw_len) {
            items = seed_find(raw, dlen, section, itemsize, count);
        }
        g_free(raw);
        return items;
    }
    return NULL;
}

struct SeedRewrite {
    int section;
    GByteArray *out;
    uint32_t nsections;
};

static bool seed_rewrite_visit(const SeedSection *sec, const uint8_t *items,
                               void *opaque)
{
    struct SeedRewrite *w = opaque;
    if (sec->id == (uint32_t)w->section) {
        return true; /* replaced by the caller's section */
    }
    g_byte_array_append(w->out, (const uint8_t *)sec, sizeof(*sec));
    g_byte_array_append(w->out, items, (size_t)sec->itemsize * sec->count);
    w->nsections++;
    return true;
}

bool pgraph_seed_write(int section, size_t itemsize, const void *items,
                       unsigned count)
{
    char *path = seed_path();
    if (!path || !count || count > SEED_MAX_ITEMS) {
        g_free(path);
        return false;
    }
    struct SeedRewrite w = { .section = section,
                             .out = g_byte_array_new() };
    SeedHeader hdr = { .magic = SEED_MAGIC, .version = SEED_VERSION };
    g_byte_array_append(w.out, (const uint8_t *)&hdr, sizeof(hdr));

    gchar *old = NULL;
    gsize old_len = 0;
    if (g_file_get_contents(path, &old, &old_len, NULL)) {
        seed_walk((const uint8_t *)old, old_len, seed_rewrite_visit, &w);
        g_free(old);
    }
    SeedSection sec = { .id = section, .itemsize = itemsize, .count = count };
    g_byte_array_append(w.out, (const uint8_t *)&sec, sizeof(sec));
    g_byte_array_append(w.out, items, itemsize * count);
    w.nsections++;
    memcpy(w.out->data + offsetof(SeedHeader, nsections), &w.nsections,
           sizeof(w.nsections));

    bool ok = g_file_set_contents(path, (const gchar *)w.out->data,
                                  w.out->len, NULL);
    g_byte_array_free(w.out, TRUE);
    g_free(path);
    return ok;
}

GHashTable *pgraph_seed_hash_set_new(void)
{
    return g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
}

bool pgraph_seed_hash_set_add(GHashTable *set, uint64_t h)
{
    if (g_hash_table_contains(set, &h)) {
        return false;
    }
    g_hash_table_add(set, g_memdup2(&h, sizeof(h)));
    return true;
}

unsigned pgraph_seed_merge(int section, size_t itemsize, const void *items,
                           unsigned count, unsigned cap, unsigned *total)
{
    unsigned n_disk = 0;
    uint8_t *disk = pgraph_seed_read(section, itemsize, &n_disk);
    GHashTable *seen = pgraph_seed_hash_set_new();
    GByteArray *out = g_byte_array_new();
    unsigned n = 0, added = 0;

    for (unsigned i = 0; i < n_disk; i++) {
        const uint8_t *it = disk + (size_t)i * itemsize;
        if (pgraph_seed_hash_set_add(seen, fast_hash(it, itemsize))) {
            g_byte_array_append(out, it, itemsize);
            n++;
        }
    }
    for (unsigned i = 0; i < count && n < cap; i++) {
        const uint8_t *it = (const uint8_t *)items + (size_t)i * itemsize;
        if (pgraph_seed_hash_set_add(seen, fast_hash(it, itemsize))) {
            g_byte_array_append(out, it, itemsize);
            n++;
            added++;
        }
    }
    if (added && !pgraph_seed_write(section, itemsize, out->data, n)) {
        added = 0;
    }
    *total = n;
    g_byte_array_free(out, TRUE);
    g_hash_table_destroy(seen);
    g_free(disk);
    return added;
}
