/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
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

#ifndef HW_XBOX_NV2A_PGRAPH_GL_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_GL_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/queue.h"
#include "qemu/lru.h"

#include "hw/hw.h"

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/cost.h"
#include "hw/xbox/nv2a/pgraph/surface.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"
#include "hw/xbox/nv2a/pgraph/glsl/uniform-cache.h"

#include "gloffscreen.h"
#include "constants.h"

/* Display thread handoff (gl/display.c): a DispPost is one ticket, all the
 * display thread needs to resolve a front buffer with no pointer into
 * render-thread state; a DispRef keeps a surface's texture alive while
 * tickets naming it are in flight. */
typedef struct DispRef {
    int refs;    /* tickets in flight (disp_lock) */
    bool orphan; /* the surface is gone: the last ticket deletes the texture */
    GLuint tex;
    /* UI-context fence after its last blit of tex: the render thread
     * waits for it on the GPU before writing the surface again. */
    GLsync presented;
} DispRef;

typedef struct DispPvideo {
    uint32_t buffer, size_in, base, limit, offset, point_in, ds_dx, dt_dy,
             point_out, size_out, format, color_key;
} DispPvideo;

typedef struct DispPost {
    DispRef *ref;
    GLuint tex;
    unsigned int draw_time;
    unsigned int width, height, pitch;
    GLint internal_format;
    GLenum format, type;
    int line_offset;
    int disp_width, disp_height;
    bool interlaced;
    unsigned int scale;
    bool pvideo;
    DispPvideo pv;
    GLsync rendered; /* render-context fence: the frame's draws are queued */
} DispPost;

typedef struct SurfaceBinding {
    QTAILQ_ENTRY(SurfaceBinding) entry;
    MemAccessCallback *access_cb;

    hwaddr vram_addr;

    SurfaceShape shape;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    bool cleared;
    int frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;
    /* CPU accesses that had to wait for a download: the flip reads back
     * ahead of time a surface accessed since the previous flip. */
    unsigned int cpu_reads;
    unsigned int cpu_reads_last_flip;
    bool watch_read_off;    /* the CPU read watch is disarmed */
    bool writeback_owed;    /* parked, RAM copy still owed */

    GLuint gl_buffer;
    /* Storage the texture was allocated with, so a retired one can be
     * handed to the next binding that needs the same. */
    unsigned int tex_w, tex_h;
    GLint tex_fmt;
    SurfaceFormatInfo fmt;
    /* Predictive read-back in flight: the pixels sit in the PBO behind the
     * fence; draw_time at issue marks a stale one. */
    GLuint dl_pbo;
    size_t dl_pbo_size;
    GLsync dl_fence;
    int dl_draw_time;
    bool dl_native; /* the PBO holds the guest's own pixel count */
    DispRef *disp_ref; /* display thread reference, or NULL */
} SurfaceBinding;

typedef struct TextureBinding {
    unsigned int refcnt;
    int draw_time;
    uint64_t data_hash;
    unsigned int scale;
    unsigned int min_filter;
    unsigned int mag_filter;
    uint32_t lod_bias;
    unsigned int addru;
    unsigned int addrv;
    unsigned int addrp;
    uint32_t border_color;
    bool border_color_set;
    unsigned int max_anisotropy;
    GLenum gl_target;
    GLuint gl_texture;
    /* Storage last allocated by render_surface_to_texture, so a repeat
     * conversion skips the glTexImage2D reallocation. */
    unsigned int s2t_width, s2t_height;
    GLint s2t_format;
} TextureBinding;

typedef struct ShaderModuleCacheKey {
    GLenum kind;
    union {
        struct {
            VshState state;
            GenVshGlslOptions glsl_opts;
        } vsh;
        struct {
            GeomState state;
            GenGeomGlslOptions glsl_opts;
        } geom;
        struct {
            PshState state;
            GenPshGlslOptions glsl_opts;
        } psh;
    };
} ShaderModuleCacheKey;

/* The wide-line geometry shader's uniforms (see glsl/geom.c). */
typedef struct GshUniformLocs {
    GLint surfaceSize;
    GLint lineWidth;
    GLint keepWinding;
} GshUniformLocs;

typedef struct ShaderModuleCacheEntry {
    LruNode node;
    ShaderModuleCacheKey key;
    GLuint gl_shader;
    GLuint gl_stage_program; /* separable, one stage */
    /* Link submitted but not checked, locations unresolved: resolving waits
     * on this link only, so fresh modules compile in parallel. */
    bool locs_pending;
    union {
        VshUniformLocs vsh;
        PshUniformLocs psh;
        GshUniformLocs gsh;
    } locs;
} ShaderModuleCacheEntry;

typedef struct ShaderBinding {
    LruNode node;
    bool initialized;
    /* pgraph_glsl_dynamic_gen at the last full bind: a family table change
     * re-derives the state even with no register write. */
    unsigned dyn_gen;

    bool cached;
    void *program;
    size_t program_size;
    GLenum program_format;
    ShaderState state;
    QemuThread *save_thread;

    GLuint gl_program;
    GLuint gl_pipeline;
    GLuint gl_prog_vs, gl_prog_fs, gl_prog_gs;
    GLenum gl_primitive_mode;
    unsigned long uses;

    struct {
        PshUniformLocs psh;
        VshUniformLocs vsh;
        GshUniformLocs gsh;
    } uniform_locs;
} ShaderBinding;

typedef struct VertexKey {
    size_t count;
    size_t stride;
    hwaddr addr;

    GLboolean gl_normalize;
    GLuint gl_type;
} VertexKey;

typedef struct VertexLruNode {
    LruNode node;
    VertexKey key;
    bool initialized;

    GLuint gl_buffer;
} VertexLruNode;

typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
} TextureKey;

typedef struct TextureLruNode {
    LruNode node;
    TextureKey key;
    TextureBinding *binding;
    bool possibly_dirty;
} TextureLruNode;

typedef struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
    GLuint *queries;
} QueryReport;

typedef struct PGRAPHGLState {
    GLuint gl_framebuffer;
    /* Display-thread coordination for the scanout. The surface list is
     * owned by the render thread; walking it from the display thread
     * raced against insertions and evictions. have_surfaces is
     * maintained at every list mutation, scanout_found is published by
     * pgraph_gl_sync after resolving the scanout on its own thread. */
    bool have_surfaces;
    bool scanout_found;
    /* Display handoff, one DispPost per new front buffer: disp_lock covers
     * the surface references, disp_post_lock the ticket; neither is held
     * across a wait. */
    QemuMutex disp_lock;
    QemuMutex disp_post_lock;
    bool disp_post_pending;
    DispPost disp_post;
    /* The UI thread holds a front buffer: when the CRTC leaves the
     * surfaces, a ticket without one sends it back to the VGA path. */
    bool disp_shown;
    /* The UI thread asks for a scanout re-check (guest CPU writes into
     * the front buffer, video overlay) without waiting for it. */
    bool disp_check_pending;
    GLsync flip_fence;
    SurfaceBinding *disp_last_surface;
    unsigned int disp_last_draw_time;
    int disp_last_line_offset;
    int disp_last_width, disp_last_height;
    bool disp_last_interlaced;
    unsigned int disp_last_scale;

    Lru element_cache;
    VertexLruNode *element_cache_entries;
    GLuint gl_inline_array_buffer;
    GLuint gl_memory_buffer;
    GLuint gl_vertex_array;
    GLuint gl_inline_buffer[NV2A_VERTEXSHADER_ATTRIBUTES];

    /* Shadow of each attribute slot's pointer, enable and current value,
     * to skip repeated GL calls: set only through pgraph_gl_attr_*. */
    struct AttrShadow {
        bool pointer_valid;
        bool integer;
        GLuint buffer;
        GLint count;
        GLenum type;
        GLboolean normalize;
        GLsizei stride;
        uintptr_t offset;
        bool enabled;
        bool value_valid;
        float value[4];
    } attr_shadow[NV2A_VERTEXSHADER_ATTRIBUTES];
    /* An attribute set that differs from the shadow only by K whole vertices
     * keeps its pointers, and K rides the draw as basevertex. */
    GLint draw_basevertex;

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    /* Evicted bindings that still owe their writeback (surface.c), and
     * retired surface textures kept for a binding of the same storage. */
    QTAILQ_HEAD(, SurfaceBinding) parked;
    unsigned int parked_n;
    struct {
        GLuint tex;
        hwaddr addr;
        unsigned int w, h;
        GLint fmt;
    } texpool[10];
    unsigned int texpool_n;
    SurfaceBinding *color_binding, *zeta_binding;
    /* One cached FBO per (color, zeta) pair, so a target switch is a plain
     * glBindFramebuffer and the driver keeps its per-FBO validation;
     * gl_fb_util hosts single-surface work (downloads, uploads) without
     * touching the render pair. */
    struct {
        GLuint color_tex, zeta_tex;
        GLenum zeta_attach;
        GLuint fbo;
    } fb_cache[32];
    unsigned fb_cache_n;
    /* One FBO per depth buffer with up to four colour attachments: colour
     * targets over one depth switch by glDrawBuffer, MEASURED far cheaper
     * than glBindFramebuffer. */
    struct {
        GLuint zeta_tex;
        GLenum zeta_attach;
        GLuint fbo;
        GLuint color_tex[4];
        unsigned w, h;
        unsigned n_color;
    } fb_multi[4];
    unsigned fb_multi_n;
    int fb_draw_idx;   /* attachment currently selected, -1 = unknown */

    GLuint gl_fb_util;
    /* A scaled colour surface is shrunk here to native size before its
     * read-back (surface_download_to_buffer). */
    GLuint dl_shrink_fbo, dl_shrink_tex;
    unsigned dl_shrink_w, dl_shrink_h;
    GLint dl_shrink_fmt;
    /* The same for a scaled depth surface. */
    GLuint dl_zshrink_fbo, dl_zshrink_tex;
    unsigned dl_zshrink_w, dl_zshrink_h;
    GLint dl_zshrink_fmt;
    /* FBO bound on the render context: a same-name glBindFramebuffer still
     * costs a driver revalidation. */
    GLuint fb_bound;
    /* Draws recorded since the last glFlush: a render-target switch
     * hands them to the GPU (fb_bind). */
    bool work_since_flush;
    /* Surface-update fast path key: last write masks and surface type
     * seen by the full path. */
    bool last_color_write, last_zeta_write;
    unsigned int last_surface_type;
    PGRAPHRtMemo rt_memo;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete; // common

    /* Texture bound per unit, to skip repeated binds between draws: reset
     * every flip, invalidated by any bind outside bind_textures. */
    GLuint tex_shadow_name[NV2A_MAX_TEXTURES];
    GLenum tex_shadow_target[NV2A_MAX_TEXTURES];
    bool tex_shadow_valid;
    TextureBinding *texture_binding[NV2A_MAX_TEXTURES];
    Lru texture_cache;
    TextureLruNode *texture_cache_entries;

    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    ShaderBinding *shader_binding;
    QemuMutex shader_cache_lock;
    QemuThread shader_disk_thread;

    Lru shader_module_cache;
    ShaderModuleCacheEntry *shader_module_cache_entries;

    /* Flip-time shader services (shaders.c). Disk saves of new modules wait
     * for a flip and a finished link, as glGetProgramBinary waits on the
     * link. */
    ShaderModuleCacheEntry *module_save_queue[256];
    unsigned module_save_r, module_save_w;
    /* The finished link the module cache's entry init adopts. */
    uint64_t comb_adopt_hash;
    GLuint comb_adopt_program;
    /* First meet: specialised states queued for their own modules. */
    struct {
        uint64_t hash;
        int phase;
    } spec_track[256];
    ShaderState spec_pending[64];
    unsigned spec_pending_r, spec_pending_w;
    /* Cold-boot seed pump (the seed block above struct SeedCollect). */
    struct {
        ShaderModuleCacheKey *mod_keys;
        unsigned mod_n, mod_idx;
        ShaderState *pipe_states;
        unsigned pipe_n;
        int state; /* -1 unloaded, 1 pumping, 0 done/absent */
        unsigned stall_flips, pipes_built;
    } seed;
    /* Boot prewarm of the on-disk module cache. */
    struct {
        char **files;
        unsigned count, idx;
        int state; /* -1 unscanned, 1 pumping, 0 done */
    } prewarm;
    /* Freshly created 2D textures, whose finalization the driver defers to
     * the first draw sampling them: texture.c queues them, the flip service
     * touches them in vblank slack. */
    GLuint fresh_tex[128];
    unsigned fresh_tex_w, fresh_tex_r;
    GLuint touch_prog, touch_vao;
    /* The vertex uniform values, and those each stage program was last
     * given. */
    VshUniformSource vsh_uni_src;
    UniformCache uni_cache;

    unsigned int zpass_pixel_count_result;
    unsigned int gl_zpass_pixel_count_query_count;
    GLuint *gl_zpass_pixel_count_queries;
    bool gl_zpass_query_open;
    QSIMPLEQ_HEAD(, QueryReport) report_queue;

    bool shader_cache_writeback_pending;
    QemuEvent shader_cache_writeback_complete;

    struct s2t_rndr {
        GLuint fbo, vao, vbo, prog;
        GLuint tex_loc, surface_size_loc;
        GLuint pack_prog, pack_tex_loc;  /* depth-surface -> A8R8G8B8 packing */
        GLuint depth_prog, depth_tex_loc, depth_scale_loc; /* scaled -> 1x */
        /* A8R8G8B8 bits -> depth, the inverse of pack_prog: a buffer written
         * as colour, then bound as zeta (alias_convert). */
        GLuint unpack_prog, unpack_tex_loc;
    } s2t_rndr;

    struct disp_rndr {
        GLuint fbo, vao, vbo, prog;
        GLuint display_size_loc;
        GLuint line_offset_loc;
        GLuint tex_loc;
        GLuint pvideo_tex;
        GLint pvideo_enable_loc;
        GLint pvideo_tex_loc;
        GLint pvideo_in_pos_loc;
        GLint pvideo_pos_loc;
        GLint pvideo_scale_loc;
        GLint pvideo_color_key_enable_loc;
        GLint pvideo_color_key_loc;
        GLint palette_loc[256];
    } disp_rndr;

    GLfloat supported_aliased_line_width_range[2];
    GLfloat supported_smooth_line_width_range[2];
    /* draw_begin skips its GL state block while the key it builds is
     * unchanged: whatever else changes that GL state calls
     * pgraph_gl_draw_state_invalidate(). */
    struct {
        bool valid;
        uint32_t k[13];
    } draw_state_key;
    /* Mirrors GL_LINE_SMOOTH so the line width, posed outside the cached
     * block, picks the right host range without a per-draw glIsEnabled
     * round trip. */
    bool line_smooth_on;
    /* The draw's lines are wider than the host rasterizes: the geometry
     * shader draws them as rectangles (part of the shader state key). */
    bool wide_lines;

    struct supported_extensions {
        GLboolean texture_filter_anisotropic;
    } supported_extensions;
} PGRAPHGLState;

extern GloContext *g_nv2a_context_render;
extern GloContext *g_nv2a_context_display;

unsigned int pgraph_gl_bind_inline_array(NV2AState *d);
void pgraph_gl_attr_pointer(PGRAPHGLState *r, int i, GLuint buffer,
                            GLint count, GLenum type, GLboolean normalize,
                            GLsizei stride, uintptr_t offset, bool integer);
void pgraph_gl_attr_enable(PGRAPHGLState *r, int i, bool enable);
void pgraph_gl_attr_value(PGRAPHGLState *r, int i, const float value[4]);
void pgraph_gl_bind_shaders(PGRAPHState *pg);
void pgraph_gl_wide_line_uniforms(PGRAPHState *pg);
void pgraph_gl_shaders_flip_service(NV2AState *d);
void pgraph_gl_tex_shadow_reset(PGRAPHGLState *r);
void pgraph_gl_uniform_sources_invalidate(PGRAPHGLState *r);
void pgraph_gl_tex_shadow_invalidate(PGRAPHGLState *r);
void pgraph_gl_bind_textures(NV2AState *d);

void pgraph_gl_bind_vertex_attributes(NV2AState *d, unsigned int min_element, unsigned int max_element, bool inline_data, unsigned int inline_stride, unsigned int provoking_element, bool allow_basevertex);
bool pgraph_gl_check_surface_to_texture_compatibility(const SurfaceBinding *surface, const TextureShape *shape);
GLuint pgraph_gl_compile_shader(const char *vs_src, const char *fs_src);
GLuint pgraph_gl_separable_program(GLenum kind, const char *src,
                                   GLuint *shader);
void pgraph_gl_download_dirty_surfaces(NV2AState *d);
void pgraph_gl_clear_report_value(NV2AState *d);
void pgraph_gl_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_gl_draw_begin(NV2AState *d);
void pgraph_gl_draw_state_invalidate(PGRAPHGLState *r);
void pgraph_gl_draw_end(NV2AState *d);
void pgraph_gl_flush_draw(NV2AState *d);
void pgraph_gl_get_report(NV2AState *d, uint32_t parameter);
void pgraph_gl_image_blit(NV2AState *d);
void pgraph_gl_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr, hwaddr size);
void pgraph_gl_process_pending_reports(NV2AState *d);
void pgraph_gl_drain_pending_reports(NV2AState *d);
void pgraph_gl_surface_flush(NV2AState *d);
void pgraph_gl_surface_update(NV2AState *d, bool upload, bool color_write, bool zeta_write);
void pgraph_gl_sync(NV2AState *d);
void pgraph_gl_present_scanout(NV2AState *d);
/* Recounts surfaces_any_dirty at the flip (surface.c). */
void pgraph_gl_surfaces_refresh_any_dirty(NV2AState *d);
void pgraph_gl_display_surface_gone(NV2AState *d, SurfaceBinding *surface);
void pgraph_gl_display_surface_reuse(NV2AState *d, SurfaceBinding *surface);
void pgraph_gl_update_entire_memory_buffer(NV2AState *d);
void pgraph_gl_init_display(NV2AState *d);
void pgraph_gl_finalize_display(PGRAPHState *pg);
void pgraph_gl_init_reports(NV2AState *d);
void pgraph_gl_finalize_reports(PGRAPHState *pg);
void pgraph_gl_init_shaders(PGRAPHState *pg);
void pgraph_gl_finalize_shaders(PGRAPHState *pg);
void pgraph_gl_init_surfaces(PGRAPHState *pg);
void pgraph_gl_finalize_surfaces(PGRAPHState *pg);
void pgraph_gl_init_textures(NV2AState *d);
void pgraph_gl_finalize_textures(PGRAPHState *pg);
void pgraph_gl_init_buffers(NV2AState *d);
void pgraph_gl_finalize_buffers(PGRAPHState *pg);
void pgraph_gl_process_pending_downloads(NV2AState *d);
void pgraph_gl_reload_surface_scale_factor(PGRAPHState *pg);
void pgraph_gl_render_surface_to_texture(NV2AState *d, SurfaceBinding *surface, TextureBinding *texture, TextureShape *texture_shape, int texture_unit);
void pgraph_gl_render_cube_faces_to_texture(NV2AState *d,
                                            SurfaceBinding *faces[6],
                                            TextureBinding *texture,
                                            TextureShape *shape,
                                            int texture_unit);
bool pgraph_gl_own_draw_suspend(NV2AState *d);
void pgraph_gl_own_draw_resume(NV2AState *d);
extern const XemuCostQueryOps pgraph_gl_cost_ops;
void pgraph_gl_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_gl_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface);
void pgraph_gl_surface_predownload(NV2AState *d, SurfaceBinding *surface);
void pgraph_gl_parked_materialize_range(NV2AState *d, hwaddr start,
                                        size_t size);
SurfaceBinding *pgraph_gl_surface_get(NV2AState *d, hwaddr addr);
SurfaceBinding *pgraph_gl_surface_create_blit_dest(NV2AState *d, hwaddr addr,
                                                   const SurfaceBinding *src,
                                                   unsigned int pitch,
                                                   unsigned int width,
                                                   unsigned int height);
SurfaceBinding *pgraph_gl_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_gl_surface_invalidate(NV2AState *d, SurfaceBinding *e);
void pgraph_gl_surface_mark_ahead(SurfaceBinding *s);
void pgraph_gl_unbind_surface(NV2AState *d, bool color);
void pgraph_gl_upload_surface_data(NV2AState *d, SurfaceBinding *surface, bool force);
void pgraph_gl_shader_cache_to_disk(ShaderBinding *snode);
bool pgraph_gl_shader_load_from_memory(ShaderBinding *snode);
void pgraph_gl_shader_write_cache_reload_list(PGRAPHState *pg);
void pgraph_gl_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_gl_get_surface_scale_factor(NV2AState *d);
int pgraph_gl_get_framebuffer_surface(NV2AState *d);
/**  Note: The caller must set up a clean GL context before invoking. */
void pgraph_gl_determine_gpu_properties(void);
GPUProperties *pgraph_gl_get_gpu_properties(void);

#endif
