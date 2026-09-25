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

#ifndef HW_XBOX_NV2A_PGRAPH_WRITEBACK_H
#define HW_XBOX_NV2A_PGRAPH_WRITEBACK_H

#include "exec/hwaddr.h"

/*
 * HEURISTIC. An evicted surface whose writeback no reader
 * is known to need is not written back: the renderer parks its binding,
 * owing the writeback to the first reader of that RAM (Virtua Cop 3
 * retargets one depth buffer between a linear and a swizzled layout every
 * frame; eager writebacks would read it back every pass). Only a real reader
 * learns a range live, and its later evictions write back eagerly. A surface
 * created over a range whose content was dropped unwritten skips uploading
 * that stale RAM, unless the guest wrote it since (the NV2A dirty bits,
 * consumed at the create): it starts from its pooled image, or undefined,
 * where RAM is stale anyway. The renderers keep the parked bindings; the
 * guest RAM ranges below are theirs in common.
 */

#define PGRAPH_WB_LIVE_RANGES 16
#define PGRAPH_WB_DISCARD_SPANS 8

typedef struct PGRAPHWritebackRange {
    hwaddr addr;
    size_t size;
} PGRAPHWritebackRange;

typedef struct PGRAPHWritebackState {
    PGRAPHWritebackRange live[PGRAPH_WB_LIVE_RANGES];
    unsigned int live_w;
    PGRAPHWritebackRange discarded[PGRAPH_WB_DISCARD_SPANS];
    unsigned int discarded_w;
} PGRAPHWritebackState;

typedef struct NV2AState NV2AState;
typedef struct PGRAPHState PGRAPHState;

/* Whether the eviction writeback of a surface would be dead: never read by
 * the CPU, its range not learnt live, not the scanout. */
bool pgraph_writeback_dead(NV2AState *d, hwaddr addr, size_t size,
                           unsigned int cpu_reads);
/* A reader consumed an owed writeback of this range. */
void pgraph_writeback_learn_live(PGRAPHState *pg, hwaddr addr, size_t size);
/* The content of this range was dropped without being written back. */
void pgraph_writeback_record_discard(PGRAPHState *pg, hwaddr addr,
                                     size_t size);
/* Whether a dropped span covers this range. */
bool pgraph_writeback_discarded(PGRAPHState *pg, hwaddr addr, size_t size);
/* A real writeback rewrote RAM host side, which the CPU dirty test of the
 * upload skip cannot see: the dropped spans it overlaps are disarmed. */
void pgraph_writeback_clear_discard(PGRAPHState *pg, hwaddr addr,
                                    size_t size);
/* After a snapshot load or a reset RAM is authoritative: forget what was
 * learnt of the old one. */
void pgraph_writeback_reset(PGRAPHState *pg);

#endif
