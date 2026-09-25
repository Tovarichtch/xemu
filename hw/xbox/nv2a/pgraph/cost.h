/*
 * Geforce NV2A real-hardware cost model -- renderer-agnostic core
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

#ifndef HW_XBOX_NV2A_PGRAPH_COST_H
#define HW_XBOX_NV2A_PGRAPH_COST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NV2AState NV2AState;
typedef struct PGRAPHState PGRAPHState;

/* Input of the per-frame NV2A cost model: submitted vertices plus shaded
 * fragments, counted per segment (a run of draws sharing one register-derived
 * per-fragment cost profile, one backend query each) and resolved at the
 * flip. Profiles, segmentation and arithmetic live in cost.c; a backend only
 * supplies the fragment counts through these hooks. */
typedef struct XemuCostQueryOps {
    /* Begin fragment counting for segment `slot`. When `want_samples` the
     * resolver also wants a pass-count (post depth/alpha) query; return
     * whether it actually started -- false when the game's own visibility
     * query owns that counter. */
    bool (*seg_begin)(NV2AState *d, int slot, bool want_samples);
    /* End the open segment's queries. Called only while
     * xemu_cost_q_open; xemu_cost_q2_open tells whether the pass-count
     * query is live too (the flags are cleared by the caller after). */
    void (*seg_end)(NV2AState *d);
    /* Read back `slot`'s counters, raw (resolution-scaled) host values.
     * `samples` is non-NULL only when seg_begin returned true for it.
     * Called with the pipeline drained (flip stall). */
    void (*seg_read)(NV2AState *d, int slot, uint64_t *invocations,
                     uint64_t *samples);
} XemuCostQueryOps;

#define XEMU_COST_SEGS 512

extern int xemu_cost_segn;
extern bool xemu_cost_q_open;
extern bool xemu_cost_q2_open;

/* Frame accumulators consumed by the pacing model (cost.c). */
extern unsigned long long xemu_cost_vtime_milli;
extern unsigned long long xemu_cost_ftime_milli;
extern unsigned long long xemu_cost_bytes_milli;
extern unsigned long long xemu_cost_texres_run; /* unique-texture bytes seen */

/* Read from other devices: a PFB compression region is valid (pfb.c), and
 * the ATA commands issued so far (hw/ide/core.c). */
extern unsigned xemu_pfb_zcomp_on;
extern unsigned long xemu_ide_cmds;

/* Register-derived cost profile helpers. */
extern unsigned long long xemu_cost_reports;
/* At each FLIP_STALL: the real hardware speed deadline of this frame. */
void xemu_cost_flip_deadline(NV2AState *d);
long xemu_cost_vert_cost_milli(PGRAPHState *pg);
long xemu_cost_attr_bytes(PGRAPHState *pg);
long xemu_cost_draw_texbytes(PGRAPHState *pg);
void xemu_cost_frag_profile(PGRAPHState *pg, long *compute_milli,
                            long *texbytes_out, long *fb_out,
                            long *z_milli, bool *z_accept_ok);

/* The perf.real_hw_speed setting; the backend layers its capability probe
 * on top. */
bool xemu_cost_model_active(void);

/* Segmentation decision for the draw about to run: closes the open
 * segment when the profile changed, opens a new one. */
void xemu_cost_draw_begin(NV2AState *d, const XemuCostQueryOps *ops);
/* Per-draw quantities, knowable only after vertex submission. */
void xemu_cost_draw_end_tally(PGRAPHState *pg);
/* Keep xemu's own surface-to-texture blit out of the counts (the NV2A
 * has no such copy). Suspend returns whether a segment was interrupted;
 * resume reopens one with the interrupted segment's profile. */
bool xemu_cost_blit_suspend(NV2AState *d, const XemuCostQueryOps *ops);
void xemu_cost_blit_resume(NV2AState *d, const XemuCostQueryOps *ops);
/* The game's visibility query took the pass-count target: the backend
 * ended our query itself; mark the segment truncated. */
void xemu_cost_samples_interrupted(void);
/* The backend ended the segment's queries itself (e.g. at a render pass
 * boundary they cannot cross): close it so the next draw opens a fresh one;
 * billing is unchanged, the resolve sums segments linearly. */
void xemu_cost_backend_interrupt(void);
/* Close the frame's open segment (before the backend drains). */
void xemu_cost_frame_close(NV2AState *d, const XemuCostQueryOps *ops);
/* Read every segment and turn the counts into shade/DRAM cost.
 * `scale_factor` is the host resolution scale to normalise away. */
void xemu_cost_resolve_frame(NV2AState *d, const XemuCostQueryOps *ops,
                             unsigned scale_factor);

/* Vertex-side accumulation, called from the backend draw paths with the
 * per-vertex rates computed once per flush. */
static inline void xemu_cost_add_verts(unsigned long long n,
                                       unsigned long long vcost_milli,
                                       unsigned long long vbytes_milli)
{
    xemu_cost_vtime_milli += n * vcost_milli;
    xemu_cost_bytes_milli += n * vbytes_milli;
}

#endif
