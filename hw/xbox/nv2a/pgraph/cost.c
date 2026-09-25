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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/cost.h"
#include "hw/xbox/nv2a/pgraph/psh_regs.h"
#include "ui/xemu-settings.h"

/* Per-segment cost profile, latched at segment open from the registers. */
static long xemu_cost_segcm[XEMU_COST_SEGS];  /* milli-ns compute/fragment */
static long xemu_cost_segbmt[XEMU_COST_SEGS]; /* milli-bytes texels/fragment */
static long xemu_cost_segbmf[XEMU_COST_SEGS]; /* milli-bytes fb/fragment */
static long xemu_cost_segz[XEMU_COST_SEGS];   /* milli-bytes Z RMW/fragment */
static bool xemu_cost_segzacc[XEMU_COST_SEGS]; /* z-accept eligible */
/* Per-segment draw count, tallied in draw_end: draw_begin runs off
 * SET_BEGIN_END, before any vertex is submitted. */
static long xemu_cost_segnd[XEMU_COST_SEGS];
/* Per-draw texture-set bytes, summed and max'd over the segment's draws:
 * the basis of the finite texture cache cap. Tallied like segnd. */
static unsigned long long xemu_cost_segtexb[XEMU_COST_SEGS];
static unsigned long long xemu_cost_segtexbmax[XEMU_COST_SEGS];
/* Pass count (after alpha test and depth) beside the invocations: 0 absent,
 * 1 valid, 2 truncated. The game's zpass queries share its target: ours runs
 * only while theirs is closed, and a segment theirs interrupts is truncated. */
static unsigned char xemu_cost_seg2v[XEMU_COST_SEGS];
static unsigned char xemu_cost_segat[XEMU_COST_SEGS]; /* alpha test on */
int xemu_cost_segn;
bool xemu_cost_q_open;
bool xemu_cost_q2_open;
unsigned long long xemu_cost_texres_run;
static long cost_cur_cm = -1, cost_cur_bm = -1;

bool xemu_cost_model_active(void)
{
    /* live UI toggle: perf.real_hw_speed */
    return g_config.perf.real_hw_speed;
}

void xemu_cost_draw_begin(NV2AState *d, const XemuCostQueryOps *ops)
{
    PGRAPHState *pg = &d->pgraph;
    long cm, bmt, bmf, zm;
    bool zacc;
    xemu_cost_frag_profile(pg, &cm, &bmt, &bmf, &zm, &zacc);
    /* Alpha test is in the key, so segat holds for every draw of a segment. */
    bool at_key = (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
                   NV_PGRAPH_CONTROL_0_ALPHATESTENABLE) != 0;
    long bm = bmt + bmf + zm * 131 +   /* fold z into the segment key */
              (at_key ? (1l << 30) : 0);
    if (xemu_cost_q_open && (cm != cost_cur_cm || bm != cost_cur_bm) &&
        xemu_cost_segn < XEMU_COST_SEGS) {
        ops->seg_end(d);
        xemu_cost_q_open = false;
        xemu_cost_q2_open = false;
    }
    if (!xemu_cost_q_open && xemu_cost_segn < XEMU_COST_SEGS) {
        int n = xemu_cost_segn;
        xemu_cost_segnd[n] = 0;
        xemu_cost_segtexb[n] = 0;
        xemu_cost_segtexbmax[n] = 0;
        xemu_cost_segcm[n] = cm;
        xemu_cost_segbmt[n] = bmt;
        xemu_cost_segbmf[n] = bmf;
        xemu_cost_segz[n] = zm;
        xemu_cost_segzacc[n] = zacc;
        xemu_cost_segat[n] = at_key;
        bool samples = ops->seg_begin(d, n, true);
        xemu_cost_seg2v[n] = samples ? 1 : 0;
        xemu_cost_q2_open = samples;
        cost_cur_cm = cm;
        cost_cur_bm = bm;
        xemu_cost_segn++;
        xemu_cost_q_open = true;
    }
}

void xemu_cost_draw_end_tally(PGRAPHState *pg)
{
    if (xemu_cost_segn <= 0) {
        return;
    }
    int seg = xemu_cost_segn - 1;
    xemu_cost_segnd[seg]++;
    long tb = xemu_cost_draw_texbytes(pg);
    if (tb > 0) {
        xemu_cost_segtexb[seg] += (unsigned long long)tb;
        if ((unsigned long long)tb > xemu_cost_segtexbmax[seg]) {
            xemu_cost_segtexbmax[seg] = (unsigned long long)tb;
        }
    }
}

/* xemu's surface-to-texture blit is no NV2A work: the texture unit samples
 * the framebuffer in place, in unified VRAM. The blit runs inside the open
 * segment: close it, let the blit run unmeasured, reopen an identical one. */
bool xemu_cost_blit_suspend(NV2AState *d, const XemuCostQueryOps *ops)
{
    if (!xemu_cost_q_open) {
        return false;
    }
    ops->seg_end(d);
    xemu_cost_q_open = false;
    xemu_cost_q2_open = false;
    return true;
}

void xemu_cost_blit_resume(NV2AState *d, const XemuCostQueryOps *ops)
{
    int p = xemu_cost_segn - 1, n = xemu_cost_segn;
    if (xemu_cost_q_open || p < 0 || n >= XEMU_COST_SEGS) {
        return;
    }
    /* Same cost profile as the segment we interrupted: the draw that
     * triggered the blit has not changed any of it. */
    xemu_cost_segcm[n] = xemu_cost_segcm[p];
    xemu_cost_segbmt[n] = xemu_cost_segbmt[p];
    xemu_cost_segbmf[n] = xemu_cost_segbmf[p];
    xemu_cost_segz[n] = xemu_cost_segz[p];
    xemu_cost_segzacc[n] = xemu_cost_segzacc[p];
    xemu_cost_segat[n] = xemu_cost_segat[p];
    /* The per-draw tallies start at zero: the slot may hold a previous
     * frame's counts, which would disarm the finite texture cache cap. */
    xemu_cost_segnd[n] = 0;
    xemu_cost_segtexb[n] = 0;
    xemu_cost_segtexbmax[n] = 0;
    bool samples = ops->seg_begin(d, n, true);
    xemu_cost_seg2v[n] = samples ? 1 : 0;
    xemu_cost_q2_open = samples;
    xemu_cost_segn++;
    xemu_cost_q_open = true;
}

void xemu_cost_samples_interrupted(void)
{
    xemu_cost_q2_open = false;
    if (xemu_cost_segn > 0) {
        xemu_cost_seg2v[xemu_cost_segn - 1] = 2;
    }
}

void xemu_cost_backend_interrupt(void)
{
    xemu_cost_q_open = false;
    xemu_cost_q2_open = false;
}

void xemu_cost_frame_close(NV2AState *d, const XemuCostQueryOps *ops)
{
    if (xemu_cost_q_open) {
        ops->seg_end(d);
        xemu_cost_q_open = false;
        xemu_cost_q2_open = false;
    }
}

void xemu_cost_resolve_frame(NV2AState *d, const XemuCostQueryOps *ops,
                             unsigned scale_factor)
{
    if (!xemu_cost_segn) {
        return;
    }
    /* Normalise to 1x: the resolution scale multiplies host fragments
     * by scale^2 without changing what the real NV2A would pay. */
    unsigned sf = scale_factor;
    /* Pass 1: read every query and collect the frame's measured kill ratios,
     * which bill the segments whose pass count the game's own query took. */
    static uint64_t segv[XEMU_COST_SEGS], segs[XEMU_COST_SEGS];
    unsigned long long mv_at = 0, ms_at = 0, mv_z = 0, ms_z = 0;
    unsigned long long f_tex = 0, f_fb = 0;
    for (int i = 0; i < xemu_cost_segn; i++) {
        uint64_t v = 0, s = 0;
        bool has_s = xemu_cost_seg2v[i] == 1;
        ops->seg_read(d, i, &v, has_s ? &s : NULL);
        v /= sf * sf;
        segv[i] = v;
        segs[i] = (uint64_t)-1;
        if (has_s) {
            s /= sf * sf;
            if (s > v) {
                s = v;
            }
            segs[i] = s;
            if (xemu_cost_segat[i]) {
                mv_at += v;
                ms_at += s;
            } else {
                mv_z += v;
                ms_z += s;
            }
        }
    }
    for (int i = 0; i < xemu_cost_segn; i++) {
        uint64_t v = segv[i];
        uint64_t s = segs[i];
        unsigned long long cm = xemu_cost_segcm[i];
        unsigned long long bmt = xemu_cost_segbmt[i];
        unsigned long long bmf = xemu_cost_segbmf[i];
        bool at = xemu_cost_segat[i];
        if (s == (uint64_t)-1) {
            /* Unmeasured: same-frame measured ratio, per class. */
            if (at && mv_at) {
                s = v * ms_at / mv_at;
            } else if (!at && mv_z) {
                s = v * ms_z / mv_z;
            } else {
                s = v; /* nothing measured at all: full rate */
            }
        }
        unsigned long long shade = s * cm;
        unsigned long long texadd = s * bmt;
        unsigned long long fbadd = s * bmf;
        if (at) {
            /* alpha-killed: shaded and texel-fetched, no fb write */
            shade += (v - s) * cm;
            texadd += (v - s) * bmt;
        }
        /* Texture cache, per draw: a draw whose whole set fits on chip pays
         * compulsory misses only, bounded by the set's bytes, not by its
         * fragments. A segment qualifies when every draw's set fits in
         * cachekb KB and its mean fragments per draw exceed cachemag x the
         * largest set (size alone does not tell Ghost Squad's cached smoke
         * from VC3's streamed one). NV2x has dual on-chip texture caches of
         * unpublished size (GF3/GF4 docs) [HEURISTIC: 4 KB and x4 bounded by
         * those two scenes]; cacheamp, in hundredths (1.33x), covers mip chains
         * and bursts. */
        {
            const long cachekb = 4, cacheamp = 133, cachemag = 4;
            if (xemu_cost_segnd[i] > 0 &&
                xemu_cost_segtexbmax[i] > 0 &&
                xemu_cost_segtexbmax[i] <=
                    (unsigned long long)cachekb * 1024 &&
                v > (unsigned long long)xemu_cost_segnd[i] *
                        xemu_cost_segtexbmax[i] * cachemag) {
                unsigned long long cap =
                    xemu_cost_segtexb[i] * 1000ull * cacheamp / 100;
                if (texadd > cap) {
                    texadd = cap;
                }
            }
        }
        /* Z RMW by the segment's measured pass ratio: sure-pass segments
         * ride z-accept (read skipped, US6646639; planar 8:1 tiles absorb
         * the write), mixed ones pay the compressed RMW on every tested
         * fragment [HEURISTIC constants bounded by the rnndb tile ratios]. */
        {
            long zm = xemu_cost_segz[i];
            if (zm) {
                const long zacc_milli = 4000, zpassthr = 95;
                if (xemu_cost_segzacc[i] && v &&
                    s * 100 >= v * (unsigned long long)zpassthr) {
                    fbadd += s * zacc_milli;
                } else {
                    fbadd += v * zm;
                }
            }
        }
        /* Coalesced ROP stream: framebuffer RMW leaves in 8-pixel bursts
         * through the pixel cache across 4 partitions (US6075544, LMA),
         * above the flat mixed-traffic rate. 79% = 4.97/6.28 GB/s
         * [HEURISTIC, mechanism sourced; bounded by the AnandTech GF3
         * blended-fill envelope scaled to 233 MHz, ~750 Mpix/s]. */
        const long fbcoal = 79;
        fbadd = fbadd * fbcoal / 100;
        xemu_cost_ftime_milli += shade;
        /* Accumulate the FRAME, not the segment: the texel cap below is
         * a property of the frame's whole working set. */
        f_tex += texadd;
        f_fb += fbadd;
    }
    {
        /* Cap the frame's texel traffic at resamp/100 x the texture bytes
         * newly resident this frame: the small texture cache re-fetches a
         * working-set byte many times, not once [MEASURED: the cap bites
         * on the heat haze alone, the scene with the least new texture per
         * fragment, which sets the 20x]. */
        const long resamp = 2000;
        static unsigned long long prev_res;
        unsigned long long dres = xemu_cost_texres_run - prev_res;
        prev_res = xemu_cost_texres_run;
        unsigned long long cap = dres * 1000ull * resamp / 100;
        if (f_tex > cap) {
            f_tex = cap;
        }
        xemu_cost_bytes_milli += f_tex + f_fb;
    }
    xemu_cost_segn = 0;
}

/* Real hardware speed model, NV2A costs derived from the registers:
 *  - 2 vertex units at 233 MHz, ~1 instruction a cycle, running the
 *    submitted program or, in fixed-function mode, an XF ROM program
 *    ("Cheops") whose length follows CSV0_C/CSV0_D/CSV1;
 *  - 4 pixel pipes at 233 MHz, 2 texture fetches a cycle per pipe;
 *  - shared 128-bit DDR200 DRAM, 6.4 GB/s: vertex attributes, texels,
 *    framebuffer.
 * Frame time = max(vertex, pixel, bandwidth) + serialised terms (per-draw
 * setup, pipeline drain on GET_REPORT) + base. */

unsigned long long xemu_cost_vtime_milli;   /* ns*1000, vertex units   */
unsigned long long xemu_cost_ftime_milli;   /* ns*1000, pixel pipes    */
unsigned long long xemu_cost_bytes_milli;   /* bytes*1000, DRAM        */
unsigned long long xemu_cost_reports;

static long cost_instr_milli = -1;  /* milli-ns per vertex instruction */
static long cost_pxcycle_milli;     /* milli-ns per pixel-pipe cycle   */
static long cost_bp_num_v;          /* bytes_milli*num/100000 -> milli-ns */

static void xemu_cost_init_consts(void)
{
    if (cost_instr_milli >= 0) {
        return;
    }
    /* Evidence of each constant: PROVEN = public hardware fact, source
     * stated; MEASURED = read from a reproducible bench; HEURISTIC =
     * assumed, bounded as stated. 233 MHz core, 2 vertex units, 4 pixel
     * pipes, 6.4 GB/s raw (128-bit DDR200): PROVEN (NV2A specifications). */
    double mhz = 233.0;
    /* DRAM efficiency 0.80: HEURISTIC, mechanism known. The fitted parts
     * (Samsung K4D263238M-QC50, Hynix HY5DU283222AQ-5: 400 Mbps/pin x 128
     * bit, PROVEN by part number) lose 0.77-0.90 % to refresh (tRFC 70 ns
     * every tREF 7.8 us) and ~0 to row activation (a 1 KB page is one 16x16
     * tile at 32 bpp); read/write turnaround (tCDLR = 2 tCK) at NVIDIA's
     * 8-pixel write grain (US 6,075,544) leaves ~82 %. CRTC scanout =
     * pclk * bpp/8 = 0.1007 GB/s at 640x480x32@60: PROVEN by nv20_update_arb
     * ("crtc_drain_rate = pclk_freq * bpp/8"), corroborated by ATI
     * US 5,953,020 and the DEC/WRL Neon report. */
    double bw = 6.4 * 0.80 - 0.1007;
    cost_instr_milli = (long)(1000000.0 / (mhz * 2.0));
    cost_pxcycle_milli = (long)(1000000.0 / (mhz * 4.0));
    cost_bp_num_v = (long)(100000.0 / bw);
}

static long xemu_cost_bp_num(void)
{
    xemu_cost_init_consts();
    return cost_bp_num_v;
}

/* Vertex instructions executed for the current config. Program mode: the
 * uploaded program's length, walked to its FINAL bit (MEASURED-EXACT).
 * Fixed-function mode: program-equivalent counts per feature, mirroring
 * vsh-ff.c, billed at face value; the microcode ROM "is not accessible in
 * any way" (envytools docs/hw/graph/xf/isa.rst:187-188, NV10:NV41).
 * HEURISTIC: fixed-function lighting runs on a separate hard-wired unit, up
 * to 2x a comparable program (Lindholm/Kilgard/Moreton, SIGGRAPH 2001),
 * while the base rate has no stall model; face value matched frame-counted
 * real footage (VC3 helicopter crash), so the two are taken to cancel. Do
 * not scale either without a measurement that separates them. */
static long xemu_cost_vert_instr(PGRAPHState *pg)
{
    uint32_t csv0d = pgraph_reg_r(pg, NV_PGRAPH_CSV0_D);
    if (GET_MASK(csv0d, NV_PGRAPH_CSV0_D_MODE) == 2) {
        int start = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                             NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
        long n = 0;
        for (int i = start; i < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; i++) {
            n++;
            if (vsh_get_field((uint32_t *)&pg->program_data[i], FLD_FINAL)) {
                break;
            }
        }
        return n;
    }

    uint32_t csv0c = pgraph_reg_r(pg, NV_PGRAPH_CSV0_C);
    long n = 8;                             /* position + viewport */
    int skin = GET_MASK(csv0d, NV_PGRAPH_CSV0_D_SKIN);
    if (skin) {
        n += 7 * skin;                      /* extra matrices, pos+normal */
    }
    bool lighting = GET_MASK(csv0c, NV_PGRAPH_CSV0_C_LIGHTING);
    bool normalize = csv0c & NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE;
    if (lighting || normalize) {
        n += 3;                             /* normal transform */
    }
    if (normalize) {
        n += 3;                             /* DP3+RSQ+MUL */
    }
    if (lighting) {
        long nl = 4;                        /* ambient/emissive accum */
        long n_inf = 0;                     /* infinite lights, for LOCALEYE */
        uint32_t modes = GET_MASK(csv0d, NV_PGRAPH_CSV0_D_LIGHTS);
        for (int i = 0; i < NV2A_MAX_LIGHTS; i++) {
            /* Infinite: 5 instructions, PROVEN (Lindholm 2001 sec. 6.1,
             * "apply lighting and output color": DP3, DP3, MOV, LIT, DP3),
             * specular included: LIT computes it, and the D3D8 runtime in
             * vc3.xbe writes SET_SPECULAR_PARAMS once per flush, not per
             * light. Local = 5 + 9 attenuation slots, spot = local + 2 cone
             * slots: DERIVED from the vsh-ff.c chains (LIT collapses only the
             * infinite case); the shape is proven (US 6,417,851 TABLE 16A,
             * US 6,573,900 FIG. 10), the magnitudes HEURISTIC. */
            switch ((modes >> (i * 2)) & 3) {
            case 1: nl += 5; n_inf++; break; /* infinite  PROVEN */
            case 2: nl += 14; break;        /* local  DERIVED */
            case 3: nl += 16; break;        /* spot   DERIVED */
            }
        }
        if (pg->two_side_light_en) {
            nl *= 2;                        /* re-light back face */
        }
        if (GET_MASK(csv0c, NV_PGRAPH_CSV0_C_LOCALEYE)) {
            nl += 6;
            /* Local viewer: +6 per vertex for VPeye, and per infinite light
             * 4 slots (ADD, DP3, RSQ, MUL) for the half vector it can no
             * longer take precomputed (DERIVED from vsh-ff.c). */
            nl += 4 * n_inf;
        }
        n += nl;
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (pgraph_texgen_mode(pg, i, j)) {
                n += 2;                     /* texgen per component */
            }
        }
        if (pg->texture_matrix_enable[i]) {
            n += 4;                         /* 4 DP4 */
        }
    }
    if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3) & NV_PGRAPH_CONTROL_3_FOGENABLE) {
        n += 3;
    }
    return n;
}

long xemu_cost_vert_cost_milli(PGRAPHState *pg)
{
    xemu_cost_init_consts();
    return xemu_cost_vert_instr(pg) * cost_instr_milli;
}

/* DRAM bytes per vertex: sum of the active attributes. */
long xemu_cost_attr_bytes(PGRAPHState *pg)
{
    long b = 0;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *a = &pg->vertex_attributes[i];
        b += a->size * a->count;
    }
    return b;
}

/* Texel size of a texture format in milli-bytes: DXT1 packs 4 bits a
 * texel, DXT2-5 8; an unknown format counts 4 bytes. */
static long texel_milli_bytes(unsigned int fmt)
{
    if (fmt == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5) {
        return 500;
    }
    if (fmt == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ||
        fmt == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8) {
        return 1000;
    }
    if (fmt < ARRAY_SIZE(kelvin_color_format_info_map) &&
        kelvin_color_format_info_map[fmt].bytes_per_pixel) {
        return kelvin_color_format_info_map[fmt].bytes_per_pixel * 1000;
    }
    return 4000;
}

/* Bytes of the texture set the draw binds: base level (x6 cubemap, x depth
 * volume) at the format's storage rate, TEXIMAGERECT size for linear formats
 * (their BASE_SIZE reads 0). The compulsory-miss bound of a finite cache. */
long xemu_cost_draw_texbytes(PGRAPHState *pg)
{
    /* 64-bit arithmetic: long is 32 bits on Windows, and a 1024x1024
     * texture at four bytes a texel already overflows it in milli-bytes. */
    int64_t total = 0;
    for (int k = 0; k < 4; k++) {
        if (!(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + k * 4) &
              NV_PGRAPH_TEXCTL0_0_ENABLE)) {
            continue;
        }
        uint32_t f = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + k * 4);
        unsigned int fmt = GET_MASK(f, NV_PGRAPH_TEXFMT0_COLOR);
        int64_t bpt_milli = texel_milli_bytes(fmt);
        bool lin = fmt < ARRAY_SIZE(kelvin_color_format_info_map) &&
                   kelvin_color_format_info_map[fmt].linear;
        int64_t tx;
        if (lin) {
            uint32_t rect = pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + k * 4);
            tx = (int64_t)GET_MASK(rect, NV_PGRAPH_TEXIMAGERECT0_WIDTH) *
                 GET_MASK(rect, NV_PGRAPH_TEXIMAGERECT0_HEIGHT);
        } else {
            tx = ((int64_t)1 << GET_MASK(f, NV_PGRAPH_TEXFMT0_BASE_SIZE_U)) *
                 ((int64_t)1 << GET_MASK(f, NV_PGRAPH_TEXFMT0_BASE_SIZE_V));
            if (GET_MASK(f, NV_PGRAPH_TEXFMT0_DIMENSIONALITY) == 3) {
                tx <<= GET_MASK(f, NV_PGRAPH_TEXFMT0_BASE_SIZE_P);
            }
            if (f & NV_PGRAPH_TEXFMT0_CUBEMAPENABLE) {
                tx *= 6;
            }
        }
        total += tx * bpt_milli / 1000;
    }
    return (long)MIN(total, (int64_t)LONG_MAX);
}

/* Per-fragment cost of the current config: pixel-pipe cycles (milli-ns)
 * and DRAM bytes (milli-bytes), texels and framebuffer kept apart for the
 * resolver's kill-aware split. */
void xemu_cost_frag_profile(PGRAPHState *pg, long *compute_milli,
                            long *texbytes_out, long *fb_out,
                            long *z_milli, bool *z_accept_ok)
{
    xemu_cost_init_consts();
    uint32_t stageprog = pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG);
    long fetches = 0, extra = 0, texbytes_milli = 0;
    for (int i = 0; i < 4; i++) {
        int mode = (stageprog >> (i * 5)) & 0x1F;
        if (mode == PS_TEXTUREMODES_NONE || mode == PS_TEXTUREMODES_PASSTHRU ||
            mode == PS_TEXTUREMODES_CLIPPLANE) {
            continue;
        }
        if (!(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) &
              NV_PGRAPH_TEXCTL0_0_ENABLE)) {
            continue;
        }
        fetches++;
        if (mode >= PS_TEXTUREMODES_BUMPENVMAP) {
            extra++;                        /* extra math/dependent fetch */
        }
        unsigned int fmt = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4),
                                    NV_PGRAPH_TEXFMT0_COLOR);
        long bpp_milli = texel_milli_bytes(fmt);
        {
            /* Unique-texture residency: a texture bound again within a
             * frame streams from DRAM once, so only its first bind of the
             * frame adds its bytes (+1/3 for mips) to the running total
             * the resolve caps the frame's texel traffic by. */
            static uint64_t texres_frame = ~0ull;
            static uint32_t texres_seen[256];
            static int texres_nseen;
            if (texres_frame != g_nv2a_stats.frame_count) {
                texres_frame = g_nv2a_stats.frame_count;
                texres_nseen = 0;
            }
            uint32_t off = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i * 4);
            bool seen = false;
            for (int k = 0; k < texres_nseen; k++) {
                if (texres_seen[k] == off) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                if (texres_nseen < 256) {
                    texres_seen[texres_nseen++] = off;
                }
                uint32_t f = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
                unsigned lu = GET_MASK(f, NV_PGRAPH_TEXFMT0_BASE_SIZE_U);
                unsigned lv = GET_MASK(f, NV_PGRAPH_TEXFMT0_BASE_SIZE_V);
                unsigned long long bytes =
                    ((1ull << lu) * (1ull << lv) *
                     (unsigned long long)bpp_milli) / 1000;
                xemu_cost_texres_run += bytes + bytes / 3;
            }
        }
    }
    /* Texture DRAM per fragment, not per lookup, when any stage is enabled
     * [MEASURED: the bytes per fragment the real machine needs stay within
     * +/-7 % in five of six scenes whose lookups per fragment vary 3x;
     * NVIDIA's LMA paper also bills one texture read per pixel]. */
    {
        const long texfrag = 18500;
        if (fetches > 0) {
            texbytes_milli = texfrag;
        }
    }
    /* Per pipe: 2 bilinear fetches a cycle (2 TMUs; 3-4 textures loop back
     * at half rate). The 8 combiner stages are pipelined, 1 pixel a clock
     * whatever their count (NV2x combiner architecture): no cycles. */
    long cycles = (fetches + 1) / 2 + extra;
    if (cycles < 1) {
        cycles = 1;
    }
    *compute_milli = cycles * cost_pxcycle_milli;

    long fb_milli = 0;
    if (pgraph_color_write_enabled(pg)) {
        /* Colour bytes from the surface format the game programs. */
        long fb_bpp_milli;
        switch (pg->surface_shape.color_format) {
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
            fb_bpp_milli = 2000;
            break;
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
            fb_bpp_milli = 1000;
            break;
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
            fb_bpp_milli = 2000;
            break;
        default:                            /* X8R8G8B8/A8R8G8B8 variants */
            fb_bpp_milli = 4000;
            break;
        }
        fb_milli += fb_bpp_milli;           /* color write */
        if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
            fb_milli += fb_bpp_milli;       /* destination read (blend) */
        }
    }
    {
        /* Z tile compression (rnndb nv10_pfb NV20_ZCOMP: a Z24S8 4x4 tile
         * packs 64 B into 16 B, or 8 B planar) needs a uniform stencil byte
         * per tile, so it breaks only when stencil values change (test and
         * write on, an op other than KEEP). The guest enables it (see
         * xemu_pfb_zcomp_on); the resolve picks z-accept or the compressed
         * RMW from the measured pass ratio, this exports the base rates. */
        uint32_t c2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
        bool stencil_mutates =
            (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
             NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE) &&
            (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
             NV_PGRAPH_CONTROL_0_STENCIL_WRITE_ENABLE) &&
            ((GET_MASK(c2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL) !=
              NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP) ||
             (GET_MASK(c2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL) !=
              NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP) ||
             (GET_MASK(c2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS) !=
              NV_PGRAPH_CONTROL_2_STENCIL_OP_V_KEEP));
        const long znorm_milli = 4000;
        bool zcomp = xemu_pfb_zcomp_on && !stencil_mutates;
        long zrmw_milli;
        bool z16 = pg->surface_shape.zeta_format ==
                   NV097_SET_SURFACE_FORMAT_ZETA_Z16;
        if (z16) {
            zrmw_milli = zcomp ? znorm_milli / 2 : 1000;
        } else {
            zrmw_milli = stencil_mutates ? 4000 :
                         (zcomp ? znorm_milli : 2000);
        }
        long zbase = 0;
        if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
            NV_PGRAPH_CONTROL_0_ZENABLE) {
            zbase += zrmw_milli;            /* read test */
        }
        if (pgraph_zeta_write_enabled(pg)) {
            zbase += zrmw_milli;            /* write */
        }
        *z_milli = zbase;
        *z_accept_ok = zcomp;               /* z-accept only while compressed */
    }
    /* The resolve bills survivors at full rate, alpha-killed fragments at
     * compute and texels only (no framebuffer traffic) and z-killed ones at
     * zero (NV2A zcull rejects before shading), from each segment's pass and
     * invocation counts; a segment without its own pass count takes the
     * frame's measured ratio for its class, or full rate if none. */
    *texbytes_out = texbytes_milli;
    *fb_out = fb_milli;
}

/* Real hardware speed (perf.real_hw_speed), at each FLIP_STALL: estimate how
 * long the NV2A would take to render this frame and set the deadline before
 * which the flip may not complete. With the model on, flip_stall drained the
 * host GPU (glFinish on GL), so the deadline is a floor over the host cost. */
void xemu_cost_flip_deadline(NV2AState *d)
{
    /* live UI toggle: perf.real_hw_speed */
    if (!g_config.perf.real_hw_speed) {
        return;
    }
    static unsigned long long pvt, pft, pby, prep;
    /* constants in MILLI-ns */
    const long nsdraw = 1000 * 1000, nsreport = 2000 * 1000,
               baseus = 500;
    int *c = g_nv2a_stats.frame_working.counters;
    long long vt = xemu_cost_vtime_milli - pvt;
    long long ft = xemu_cost_ftime_milli - pft;
    long long bp = (long long)(xemu_cost_bytes_milli - pby) *
                   xemu_cost_bp_num() / 100000;
    long long rep = xemu_cost_reports - prep;
    pvt = xemu_cost_vtime_milli;
    pft = xemu_cost_ftime_milli;
    pby = xemu_cost_bytes_milli;
    prep = xemu_cost_reports;
    int64_t cost;
    {
        /* Roofline: the stages overlap, the frame lasts as long as
         * its bottleneck (PROVEN mechanism). IDE DMA in the frame
         * shares the bus, 4.62 instead of 4.97 GB/s [HEURISTIC;
         * judge: footage of streaming-heavy scenes]. */
        {
            static unsigned long pidec;
            if (xemu_ide_cmds != pidec) {
                pidec = xemu_ide_cmds;
                bp = bp * 497 / 462;
            }
        }
        long long roof = vt > ft ? vt : ft;
        if (bp > roof) {
            roof = bp;
        }
        long long gpu_milli =
            roof + (long long)c[NV2A_PROF_BEGIN_ENDS] * nsdraw
            + rep * nsreport;
        cost = baseus * 1000ll + gpu_milli / 1000;
    }
    {
        /* EMA, weight 1/16 [HEURISTIC]: the real pipeline smears a
         * frame's cost forward (triple-buffer queue, DRC latency). */
        static int64_t cost_ema;
        if (!cost_ema) {
            cost_ema = cost;
        }
        cost_ema += (cost - cost_ema) / 16;
        cost = cost_ema;
    }
    /* No vblank grid: the NV2A frame rate is continuous; the game
     * loop waits for at least one vblank, a cap at 60, not a grid. */
    {
        /* Modelled GPU backlog, carried across frames: a frame
         * starts when the previous one's modelled work is done. */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        int64_t start = xemu_gpu_free_ns > xemu_flip_completed_ns ?
                        xemu_gpu_free_ns : xemu_flip_completed_ns;
        if (start < now - 50000000ll) {
            /* A pipeline restart, or the first frame: a backlog
             * older than 50 ms is dropped. */
            start = now - 50000000ll;
        }
        xemu_gpu_free_ns = start + cost;
        if (xemu_gpu_free_ns > now + 100000000ll) {
            xemu_gpu_free_ns = now + 100000000ll;
        }
        xemu_flip_ready_ns = xemu_gpu_free_ns;
    }
}
