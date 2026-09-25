/*
 * QEMU Geforce NV2A implementation
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

#ifndef HW_XBOX_NV2A_PGRAPH_SURFACE_H
#define HW_XBOX_NV2A_PGRAPH_SURFACE_H

typedef struct SurfaceShape {
    unsigned int z_format;
    unsigned int color_format;
    unsigned int zeta_format;
    unsigned int log_width, log_height;
    unsigned int clip_x, clip_y;
    unsigned int clip_width, clip_height;
    unsigned int anti_aliasing;
} SurfaceShape;

/* How the render target registers last resolved, for the surface update fast
 * path of the renderers: switching back to a target is a binding swap, not a
 * full pass. Invalidated by a shape change (shape_gen) and when the binding
 * dies. */
typedef struct PGRAPHRtMemoSlot {
    hwaddr dma, offset;
    unsigned int pitch;
    unsigned int shape_gen;
    void *binding;
} PGRAPHRtMemoSlot;

typedef struct PGRAPHRtMemo {
    PGRAPHRtMemoSlot slot[2][2]; /* [is_color][slot] */
    unsigned int next[2];
    unsigned int shape_gen;
} PGRAPHRtMemo;

/* Slot i's binding when it resolved these registers under this shape. */
static inline void *pgraph_rt_memo_match(const PGRAPHRtMemo *m, bool color,
                                         int i, hwaddr dma, hwaddr offset,
                                         unsigned int pitch)
{
    const PGRAPHRtMemoSlot *s = &m->slot[color][i];
    if (s->binding && s->dma == dma && s->offset == offset &&
        s->pitch == pitch && s->shape_gen == m->shape_gen) {
        return s->binding;
    }
    return NULL;
}

/* Remember how these registers resolved (two slots, a new entry replacing
 * them in turn). */
static inline void pgraph_rt_memo_store(PGRAPHRtMemo *m, bool color,
                                        hwaddr dma, hwaddr offset,
                                        unsigned int pitch, void *binding)
{
    int i;
    if (m->slot[color][0].binding == binding) {
        i = 0;
    } else if (m->slot[color][1].binding == binding) {
        i = 1;
    } else {
        i = m->next[color] & 1;
        m->next[color]++;
    }
    m->slot[color][i] = (PGRAPHRtMemoSlot){
        .dma = dma,
        .offset = offset,
        .pitch = pitch,
        .shape_gen = m->shape_gen,
        .binding = binding,
    };
}

static inline void pgraph_rt_memo_forget(PGRAPHRtMemo *m, const void *binding)
{
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 2; i++) {
            if (m->slot[c][i].binding == binding) {
                m->slot[c][i].binding = NULL;
            }
        }
    }
}

#endif
