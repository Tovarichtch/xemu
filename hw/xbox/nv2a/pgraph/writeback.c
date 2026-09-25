/*
 * Geforce NV2A PGRAPH lazy surface writebacks, shared by the renderers
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "writeback.h"

static bool wb_range_overlaps(const PGRAPHWritebackRange *r, hwaddr addr,
                              size_t size)
{
    return r->size && !(addr >= r->addr + r->size || r->addr >= addr + size);
}

static bool writeback_range_is_live(PGRAPHState *pg, hwaddr addr, size_t size)
{
    for (unsigned int i = 0; i < PGRAPH_WB_LIVE_RANGES; i++) {
        if (wb_range_overlaps(&pg->writeback.live[i], addr, size)) {
            return true;
        }
    }
    return false;
}

bool pgraph_writeback_dead(NV2AState *d, hwaddr addr, size_t size,
                           unsigned int cpu_reads)
{
    if (cpu_reads != 0) {
        return false;
    }
    if (writeback_range_is_live(&d->pgraph, addr, size)) {
        return false; /* a consumer read this range before */
    }
    hwaddr scan = d->pcrtc.start;
    if (scan >= addr && scan < addr + size) {
        return false; /* the displayed framebuffer -- must be preserved */
    }
    return true;
}

void pgraph_writeback_learn_live(PGRAPHState *pg, hwaddr addr, size_t size)
{
    PGRAPHWritebackState *wb = &pg->writeback;

    for (unsigned int i = 0; i < PGRAPH_WB_LIVE_RANGES; i++) {
        if (wb->live[i].size && wb->live[i].addr == addr &&
            wb->live[i].size == size) {
            return; /* already known */
        }
    }
    wb->live[wb->live_w % PGRAPH_WB_LIVE_RANGES] =
        (PGRAPHWritebackRange){ .addr = addr, .size = size };
    wb->live_w++;
}

void pgraph_writeback_record_discard(PGRAPHState *pg, hwaddr addr,
                                     size_t size)
{
    PGRAPHWritebackState *wb = &pg->writeback;

    wb->discarded[wb->discarded_w % PGRAPH_WB_DISCARD_SPANS] =
        (PGRAPHWritebackRange){ .addr = addr, .size = size };
    wb->discarded_w++;
}

bool pgraph_writeback_discarded(PGRAPHState *pg, hwaddr addr, size_t size)
{
    for (unsigned int i = 0; i < PGRAPH_WB_DISCARD_SPANS; i++) {
        const PGRAPHWritebackRange *s = &pg->writeback.discarded[i];
        if (s->size && addr >= s->addr && addr + size <= s->addr + s->size) {
            return true;
        }
    }
    return false;
}

void pgraph_writeback_clear_discard(PGRAPHState *pg, hwaddr addr,
                                    size_t size)
{
    for (unsigned int i = 0; i < PGRAPH_WB_DISCARD_SPANS; i++) {
        if (wb_range_overlaps(&pg->writeback.discarded[i], addr, size)) {
            pg->writeback.discarded[i].size = 0;
        }
    }
}

void pgraph_writeback_reset(PGRAPHState *pg)
{
    memset(&pg->writeback, 0, sizeof(pg->writeback));
}
