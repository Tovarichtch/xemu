/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
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
#include "qemu/mstring.h"

#include "xemu-version.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "hw/xbox/nv2a/pgraph/seed.h"
#include "hw/xbox/nv2a/pgraph/glsl/uniform-cache.h"
#include "debug.h"
#include "renderer.h"

static const char *shader_gl_vendor = NULL;

/* Deleting a program frees its GL name for reuse: drop any uniform cache
 * held against that name so a future program cannot inherit it. */
static void delete_program(PGRAPHGLState *r, GLuint prog)
{
    uniform_cache_forget(&r->uni_cache, prog);
    glDeleteProgram(prog);
}
/* The GLSL generators keep mutable static state (family tables, pending
 * queues): serialize the two callers (render thread + gen worker). */
static GMutex glsl_gen_mutex;

static GLenum get_gl_primitive_mode(enum ShaderPolygonMode polygon_mode, enum ShaderPrimitiveMode primitive_mode)
{
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS: return GL_POINTS;
    case PRIM_TYPE_LINES: return GL_LINES;
    case PRIM_TYPE_LINE_LOOP: return GL_LINE_LOOP;
    case PRIM_TYPE_LINE_STRIP: return GL_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES: return GL_TRIANGLES;
    case PRIM_TYPE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS: return GL_LINES_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP: return GL_LINE_STRIP_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return GL_LINE_LOOP;
        } else if (polygon_mode == POLY_MODE_FILL) {
            return GL_TRIANGLE_FAN;
        }

        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static GLuint create_gl_shader(GLenum gl_shader_type,
                               const char *code,
                               const char *name)
{
    GLint compiled = 0;

    NV2A_GL_DGROUP_BEGIN("Creating new %s", name);

    NV2A_DPRINTF("compile new %s, code:\n%s\n", name, code);

    GLuint shader = glCreateShader(gl_shader_type);
    glShaderSource(shader, 1, &code, 0);
    glCompileShader(shader);

    /* Check it compiled */
    compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar* log;
        GLint log_length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        log = g_malloc(log_length * sizeof(GLchar));
        glGetShaderInfoLog(shader, log_length, NULL, log);
        fprintf(stderr, "%s\n\n" "nv2a: %s compilation failed: %s\n", code, name, log);
        g_free(log);

        NV2A_GL_DGROUP_END();
        abort();
    }

    NV2A_GL_DGROUP_END();

    return shader;
}

/* Set at context init (ui/xemu.c): GL 4.1 and a passing self-test (probe.c),
 * as 4.0-era drivers expose broken separable shaders (MEASURED: Intel HD
 * 520, driver 21.20.16.4526). */
int xemu_gl_opt_capable = 1;

/* With GPU boost, each stage compiles once into a separable program,
 * combined by pipeline objects, instead of one full link per stage
 * combination. */
static bool gl_sso_enabled(void)
{
    static int on;
    if (unlikely(!on)) {
        on = -1;
        if (pgraph_gpu_boost_gl()) {
            /* The stages are GLSL 4.10, which a 4.0 context may reject:
             * both GL 4.1 and the extension are required. */
            if (epoxy_gl_version() < 41) {
                fprintf(stderr, "nv2a: GL %d.%d render context, using linked"
                                " programs\n", epoxy_gl_version() / 10,
                        epoxy_gl_version() % 10);
            } else if (glo_check_extension("GL_ARB_separate_shader_objects")) {
                on = 1;
            } else {
                fprintf(stderr, "nv2a: GL_ARB_separate_shader_objects missing,"
                                " using linked programs\n");
            }
        }
    }
    return on > 0;
}

static void set_texture_sampler_uniforms(ShaderBinding *binding)
{
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        char samplerName[16];
        snprintf(samplerName, sizeof(samplerName), "texSamp%d", i);
        GLint texSampLoc =
            glGetUniformLocation(binding->gl_program, samplerName);
        if (texSampLoc >= 0) {
            glUniform1i(texSampLoc, i);
        }
    }
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    char tmp[64];

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.vsh); i++) {
        const char *name = VshUniformInfo[i].name;
        if (VshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.vsh[i] = glGetUniformLocation(binding->gl_program, name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.psh); i++) {
        const char *name = PshUniformInfo[i].name;
        if (PshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.psh[i] = glGetUniformLocation(binding->gl_program, name);
    }

    GLuint prog = binding->gl_program;
    binding->uniform_locs.gsh.surfaceSize = glGetUniformLocation(prog, "gsSurfaceSize");
    binding->uniform_locs.gsh.lineWidth = glGetUniformLocation(prog, "gsLineWidth");
    binding->uniform_locs.gsh.keepWinding = glGetUniformLocation(prog, "gsKeepWinding");
}

static char *module_get_dir(uint64_t hash)
{
    return g_strdup_printf("%s/shader_modules/%04x",
                           xemu_settings_get_base_path(),
                           (uint32_t)(hash >> 48));
}

static char *module_get_path(const char *dir, uint64_t hash)
{
    return g_strdup_printf("%s/%012" PRIx64, dir,
                           hash & ~((uint64_t)0xffff << 48));
}

/* The header of a cached module file: the xemu build and GL vendor that
 * wrote it, then its key. False when another build or driver wrote it, or
 * the file is cut short. */
static bool module_read_header(FILE *f, ShaderModuleCacheKey *key)
{
    bool ok = false;
    char *ver = NULL, *vendor = NULL;
    uint64_t len;

#define RD(p, n)                       \
    do {                               \
        if (fread(p, n, 1, f) != 1) {  \
            goto out;                  \
        }                              \
    } while (0)

    RD(&len, sizeof(len));
    if (len == 0 || len > 256) {
        goto out;
    }
    ver = g_malloc(len);
    RD(ver, len);
    ver[len - 1] = '\0';
    if (strcmp(ver, xemu_version) != 0) {
        goto out;
    }
    RD(&len, sizeof(len));
    if (len == 0 || len > 256) {
        goto out;
    }
    vendor = g_malloc(len);
    RD(vendor, len);
    vendor[len - 1] = '\0';
    if (strcmp(vendor, shader_gl_vendor) != 0) {
        goto out;
    }
    RD(&len, sizeof(len));
    if (len != sizeof(*key)) {
        goto out;
    }
    RD(key, sizeof(*key));
    ok = true;
out:
#undef RD
    g_free(ver);
    g_free(vendor);
    return ok;
}

/* Separable stage programs are cached individually: one stage feeds many
 * combinations, so the per-binding cache would store it once per pipeline. */
static bool module_load_from_disk(PGRAPHGLState *r,
                                  ShaderModuleCacheEntry *module,
                                  uint64_t hash)
{
    char *dir = module_get_dir(hash);
    char *path = module_get_path(dir, hash);
    g_free(dir);

    FILE *f = qemu_fopen(path, "rb");
    if (!f) {
        g_free(path);
        return false;
    }

    bool ok = false;
    void *bin = NULL;
    uint64_t len;
    GLenum format;
    ShaderModuleCacheKey cached_key;
    GLuint program = 0;

    if (!module_read_header(f, &cached_key) ||
        memcmp(&cached_key, &module->key, sizeof(cached_key)) != 0) {
        goto out;
    }

#define RD(p, n)                       \
    do {                               \
        if (fread(p, n, 1, f) != 1) {  \
            goto out;                  \
        }                              \
    } while (0)

    RD(&format, sizeof(format));
    RD(&len, sizeof(len));
    if (len == 0 || len > (64 << 20)) {
        goto out;
    }
    bin = g_malloc(len);
    RD(bin, len);

#undef RD

    (void)glGetError();
    program = glCreateProgram();
    glProgramBinary(program, format, bin, len);

    GLint linked = 0, separable = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glGetProgramiv(program, GL_PROGRAM_SEPARABLE, &separable);
    if (glGetError() != GL_NO_ERROR || !linked || !separable) {
        delete_program(r, program);
        goto out;
    }

    module->gl_stage_program = program;
    ok = true;

out:
    fclose(f);
    if (!ok) {
        qemu_unlink(path);
    }
    g_free(path);
    g_free(bin);
    return ok;
}

static void module_save_to_disk(ShaderModuleCacheEntry *module, uint64_t hash)
{
    GLint size = 0;
    glGetProgramiv(module->gl_stage_program, GL_PROGRAM_BINARY_LENGTH, &size);
    if (size <= 0) {
        return;
    }

    void *bin = g_malloc(size);
    GLsizei copied;
    GLenum format;
    glGetProgramBinary(module->gl_stage_program, size, &copied, &format, bin);
    if (glGetError() != GL_NO_ERROR) {
        g_free(bin);
        return;
    }

    char *parent = g_strdup_printf("%s/shader_modules",
                                   xemu_settings_get_base_path());
    qemu_mkdir(parent);
    g_free(parent);

    char *dir = module_get_dir(hash);
    qemu_mkdir(dir);
    char *path = module_get_path(dir, hash);
    g_free(dir);

    FILE *f = qemu_fopen(path, "wb");
    if (!f) {
        g_free(path);
        g_free(bin);
        return;
    }

    bool ok = true;
#define WR(p, n) ok = ok && fwrite(p, n, 1, f) == 1

    uint64_t len = strlen(xemu_version) + 1;
    WR(&len, sizeof(len));
    WR(xemu_version, len);

    len = strlen(shader_gl_vendor) + 1;
    WR(&len, sizeof(len));
    WR(shader_gl_vendor, len);

    len = sizeof(module->key);
    WR(&len, sizeof(len));
    WR(&module->key, len);

    WR(&format, sizeof(format));
    len = copied;
    WR(&len, sizeof(len));
    WR(bin, len);

#undef WR

    fclose(f);
    if (!ok) {
        qemu_unlink(path);
    }
    g_free(path);
    g_free(bin);
}

/* Uniform locations belong to the stage program, not to the combination that
 * uses it: resolve them once here instead of at every pipeline assembly. */
static void module_resolve_uniform_locs(ShaderModuleCacheEntry *module)
{
    char tmp[64];
    GLuint prog = module->gl_stage_program;

    if (module->key.kind == GL_VERTEX_SHADER) {
        for (int i = 0; i < ARRAY_SIZE(module->locs.vsh); i++) {
            const char *name = VshUniformInfo[i].name;
            if (VshUniformInfo[i].count > 1) {
                snprintf(tmp, sizeof(tmp), "%s[0]", name);
                name = tmp;
            }
            module->locs.vsh[i] = glGetUniformLocation(prog, name);
        }
        return;
    }

    if (module->key.kind == GL_GEOMETRY_SHADER) {
        GshUniformLocs *g = &module->locs.gsh;
        g->surfaceSize = glGetUniformLocation(prog, "gsSurfaceSize");
        g->lineWidth = glGetUniformLocation(prog, "gsLineWidth");
        g->keepWinding = glGetUniformLocation(prog, "gsKeepWinding");
        return;
    }

    if (module->key.kind != GL_FRAGMENT_SHADER) {
        return;
    }

    for (int i = 0; i < ARRAY_SIZE(module->locs.psh); i++) {
        const char *name = PshUniformInfo[i].name;
        if (PshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        module->locs.psh[i] = glGetUniformLocation(prog, name);
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        snprintf(tmp, sizeof(tmp), "texSamp%d", i);
        GLint loc = glGetUniformLocation(prog, tmp);
        if (loc >= 0) {
            glProgramUniform1i(prog, loc, i);
        }
    }
}

static bool gl_has_parallel_compile(void)
{
    static int has = -1;
    if (has == -1) {
        has = glo_check_extension("GL_ARB_parallel_shader_compile");
    }
    return has;
}

static void module_locs_demand(ShaderModuleCacheEntry *module);

static void module_save_queue_push(PGRAPHGLState *r,
                                   ShaderModuleCacheEntry *module)
{
    if (r->module_save_w - r->module_save_r <
        ARRAY_SIZE(r->module_save_queue)) {
        r->module_save_queue[r->module_save_w %
                             ARRAY_SIZE(r->module_save_queue)] = module;
        r->module_save_w++;
    }
}

static void module_save_queue_drain(PGRAPHGLState *r, int64_t budget_us)
{
    int64_t t0 = g_get_monotonic_time();
    while (r->module_save_r != r->module_save_w) {
        if (g_get_monotonic_time() - t0 > budget_us) {
            break;
        }
        ShaderModuleCacheEntry *module =
            r->module_save_queue[r->module_save_r %
                                 ARRAY_SIZE(r->module_save_queue)];
        /* Evicted since queueing (stage program dropped): skip. */
        if (module->gl_stage_program) {
            if (gl_has_parallel_compile()) {
                GLint done = GL_TRUE;
                glGetProgramiv(module->gl_stage_program,
                               GL_COMPLETION_STATUS_ARB, &done);
                if (!done) {
                    /* Head still linking: retry next flip. */
                    break;
                }
            }
            /* Link done: finalizing locations here is free and keeps the
             * first draw that uses this module from paying it. */
            module_locs_demand(module);
            module_save_to_disk(module,
                                fast_hash((void *)&module->key,
                                          sizeof(ShaderModuleCacheKey)));
        }
        r->module_save_r++;
    }
}

/* Waits on this module's link only, then resolves its uniform locations:
 * call it before attaching the stage to a pipeline or reading them. */
static void module_locs_demand(ShaderModuleCacheEntry *module)
{
    if (!module->locs_pending) {
        return;
    }
    GLint ok = 0;
    glGetProgramiv(module->gl_stage_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar log[2048];
        glGetProgramInfoLog(module->gl_stage_program, 2048, NULL, log);
        fprintf(stderr, "nv2a: stage program (kind %x) failed: %s\n",
                module->key.kind, log);
        abort();
    }
    module_resolve_uniform_locs(module);
    if (module->gl_shader) {
        glDetachShader(module->gl_stage_program, module->gl_shader);
        glDeleteShader(module->gl_shader);
        module->gl_shader = 0;
    }
    module->locs_pending = false;
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    PGRAPHGLState *r = container_of(lru, PGRAPHGLState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    /* Adopt only for pipelines: the linked-program path attaches the
     * shader object, which an adopted module lacks. */
    if (gl_sso_enabled() && r->comb_adopt_program &&
        fast_hash((void *)key, sizeof(ShaderModuleCacheKey)) ==
            r->comb_adopt_hash) {
        module->gl_stage_program = r->comb_adopt_program;
        r->comb_adopt_program = 0;
        module->gl_shader = 0;
        module->locs_pending = false;
        module_resolve_uniform_locs(module);
        if (g_config.perf.cache_shaders) {
            module_save_queue_push(r, module);
        }
        return;
    }

    if (gl_sso_enabled() && g_config.perf.cache_shaders) {
        uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
        bool hit = module_load_from_disk(r, module, hash);
        if (hit) {
            module->gl_shader = 0;
            module->locs_pending = false;
            module_resolve_uniform_locs(module);
            return;
        }
    }

    const char *kind_str;
    MString *code;

    /* Separable stages match by explicit interface locations, a process-wide
     * choice left out of the cache key. */
    bool locs = gl_sso_enabled();
    g_mutex_lock(&glsl_gen_mutex);
    switch (module->key.kind) {
    case GL_VERTEX_SHADER: {
        kind_str = "vertex shader";
        GenVshGlslOptions o = module->key.vsh.glsl_opts;
        o.locations = locs;
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state, o);
        break;
    }
    case GL_GEOMETRY_SHADER: {
        kind_str = "geometry shader";
        GenGeomGlslOptions o = module->key.geom.glsl_opts;
        o.locations = locs;
        code = pgraph_glsl_gen_geom(&module->key.geom.state, o);
        break;
    }
    case GL_FRAGMENT_SHADER: {
        kind_str = "fragment shader";
        GenPshGlslOptions o = module->key.psh.glsl_opts;
        o.locations = locs;
        code = pgraph_glsl_gen_psh(&module->key.psh.state, o);
        break;
    }
    default:
        assert(!"Invalid shader module kind");
        kind_str = "unknown";
        code = NULL;
    }
    g_mutex_unlock(&glsl_gen_mutex);

    if (!code) {
        /* A geometry key whose state needs no geometry stage (a stale key
         * from disk or a seed): the module stays empty, as in the worker. */
        module->gl_shader = 0;
        module->gl_stage_program = 0;
        module->locs_pending = false;
        return;
    }

    if (gl_sso_enabled()) {
        /* Compile and link without a status query, so the driver's
         * compiler threads overlap; module_locs_demand() waits. Not
         * glCreateShaderProgramv: it reads COMPILE_STATUS internally. */
        const char *src = mstring_get_str(code);
        GLuint sh;
        GLuint prog = pgraph_gl_separable_program(module->key.kind, src, &sh);
        /* Detach and delete wait for module_locs_demand(): touching the
         * shader now can make the driver finish the link inline. */
        module->gl_stage_program = prog;
        module->gl_shader = sh;
        module->locs_pending = true;
        if (g_config.perf.cache_shaders) {
            module_save_queue_push(r, module);
        }
    } else {
        module->gl_stage_program = 0;
        module->locs_pending = false;
        module->gl_shader =
            create_gl_shader(module->key.kind, mstring_get_str(code), kind_str);
    }
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    if (module->gl_stage_program) {
        /* Pipelines built from this stage still reference it; a linked
         * program owned its own copy, a separable one does not. */
        if (module->gl_shader) {
            /* Never finalized: drop the pending shader object too. */
            glDeleteShader(module->gl_shader);
            module->gl_shader = 0;
        }
        module->gl_stage_program = 0;
        module->locs_pending = false;
    } else {
        glDeleteShader(module->gl_shader);
    }
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

/* One stage compiled and linked alone as a separable program, without
 * asking the driver for any status (that would wait for the compile). */
GLuint pgraph_gl_separable_program(GLenum kind, const char *src,
                                   GLuint *shader)
{
    GLuint sh = glCreateShader(kind);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLuint prog = glCreateProgram();
    glProgramParameteri(prog, GL_PROGRAM_SEPARABLE, GL_TRUE);
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    *shader = sh;
    return prog;
}

/* The module keys of a state: geometry (kind 0 unless the state needs one),
 * vertex, fragment. Returns whether the geometry stage is needed. */
static bool state_module_keys(const ShaderState *st,
                              ShaderModuleCacheKey keys[3])
{
    bool need_gs = pgraph_glsl_need_geom(&st->geom);

    memset(keys, 0, 3 * sizeof(keys[0]));
    if (need_gs) {
        keys[0].kind = GL_GEOMETRY_SHADER;
        keys[0].geom.state = st->geom;
    }
    keys[1].kind = GL_VERTEX_SHADER;
    keys[1].vsh.state = st->vsh;
    keys[1].vsh.glsl_opts.prefix_outputs = need_gs;
    keys[2].kind = GL_FRAGMENT_SHADER;
    keys[2].psh.state = st->psh;
    return need_gs;
}

static ShaderModuleCacheEntry *get_shader_module_for_key(
    PGRAPHGLState *r, const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    return container_of(node, ShaderModuleCacheEntry, node);
}

static void generate_shaders(PGRAPHGLState *r, ShaderBinding *binding)
{
    GLuint program = gl_sso_enabled() ? 0 : glCreateProgram();

    ShaderState *state = &binding->state;
    ShaderModuleCacheKey keys[3];

    binding->gl_primitive_mode = get_gl_primitive_mode(
        state->geom.polygon_front_mode, state->geom.primitive_mode);
    binding->gl_prog_gs = 0;

    /* Submit all of this state's modules before waiting on any: new
     * modules then cost the longest link, not the sum. */
    bool need_geometry_shader = state_module_keys(state, keys);
    ShaderModuleCacheEntry *gs_mod = NULL;
    if (need_geometry_shader) {
        gs_mod = get_shader_module_for_key(r, &keys[0]);
        if (!gl_sso_enabled()) {
            glAttachShader(program, gs_mod->gl_shader);
        }
    }

    /* create the vertex shader */
    ShaderModuleCacheEntry *vs_mod = get_shader_module_for_key(r, &keys[1]);
    if (!gl_sso_enabled()) {
        glAttachShader(program, vs_mod->gl_shader);
    }

    /* generate a fragment shader from register combiners */
    ShaderModuleCacheEntry *fs_mod = get_shader_module_for_key(r, &keys[2]);
    if (!gl_sso_enabled()) {
        glAttachShader(program, fs_mod->gl_shader);
    }

    if (gl_sso_enabled()) {
        binding->uniform_locs.gsh = (GshUniformLocs){ -1, -1, -1 };
        if (gs_mod) {
            module_locs_demand(gs_mod);
            binding->gl_prog_gs = gs_mod->gl_stage_program;
            binding->uniform_locs.gsh = gs_mod->locs.gsh;
        }
        module_locs_demand(vs_mod);
        binding->gl_prog_vs = vs_mod->gl_stage_program;
        memcpy(binding->uniform_locs.vsh, vs_mod->locs.vsh,
               sizeof(VshUniformLocs));
        module_locs_demand(fs_mod);
        binding->gl_prog_fs = fs_mod->gl_stage_program;
        memcpy(binding->uniform_locs.psh, fs_mod->locs.psh,
               sizeof(PshUniformLocs));
    }

    if (gl_sso_enabled()) {
        glGenProgramPipelines(1, &binding->gl_pipeline);
        glUseProgramStages(binding->gl_pipeline, GL_VERTEX_SHADER_BIT,
                           binding->gl_prog_vs);
        glUseProgramStages(binding->gl_pipeline, GL_FRAGMENT_SHADER_BIT,
                           binding->gl_prog_fs);
        if (binding->gl_prog_gs) {
            glUseProgramStages(binding->gl_pipeline, GL_GEOMETRY_SHADER_BIT,
                               binding->gl_prog_gs);
        }
        binding->gl_program = 0;
        glUseProgram(0);
        glBindProgramPipeline(binding->gl_pipeline);
        binding->initialized = true;
        return;
    }
    /* link the program */
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLchar log[2048];
        glGetProgramInfoLog(program, 2048, NULL, log);
        fprintf(stderr, "nv2a: shader linking failed: %s\n", log);
        abort();
    }

    glUseProgram(program);

    binding->gl_program = program;
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    /* validate the program */
    GLint valid = 0;
    glValidateProgram(program);
    glGetProgramiv(program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(program, 1024, NULL, log);
        fprintf(stderr, "nv2a: shader validation failed: %s\n", log);
        abort();
    }

    update_shader_uniform_locs(binding);
}

static void shader_create_cache_folder(void)
{
    char *shader_path = g_strdup_printf("%sshaders_boost",
                                        xemu_settings_get_base_path());
    qemu_mkdir(shader_path);
    g_free(shader_path);
}

/* The stock GPU keeps its own cache under the emulator's names
 * (shader_cache_list, shaders/): each GPU would drop the other's binaries. */
static char *shader_get_lru_cache_path(void)
{
    return g_strdup_printf("%s/shader_cache_list_boost",
                           xemu_settings_get_base_path());
}

/* Cold-boot shader seeds (file I/O in pgraph/seed.c). The disk caches hold
 * driver binaries, lost with any driver or xemu update. A seed keeps the
 * driver-independent keys of the states a game drew (seed-warmed bindings
 * never count), merged into its file at shutdown up to a fixed cap. At boot
 * their module keys feed the async worker in idle flips, and pipelines are
 * built only from resident modules, never with a synchronous compile. */

struct SeedCollect {
    GArray *pipes;
    GArray *mods;
};

/* Derive exactly the module keys generate_shaders() will ask for. */
static void seed_collect_binding(Lru *lru, LruNode *node, void *opaque)
{
    struct SeedCollect *c = opaque;
    ShaderBinding *b = container_of(node, ShaderBinding, node);
    if (!b->initialized || b->uses == 0) {
        return;
    }
    g_array_append_val(c->pipes, b->state);
    ShaderModuleCacheKey keys[3];
    bool need_gs = state_module_keys(&b->state, keys);
    g_array_append_vals(c->mods, need_gs ? &keys[0] : &keys[1],
                        need_gs ? 3 : 2);
}

static void seed_write_files(PGRAPHGLState *r)
{
    if (!g_config.perf.cache_shaders || !gl_sso_enabled()) {
        return;
    }
    struct SeedCollect c = {
        .pipes = g_array_new(FALSE, FALSE, sizeof(ShaderState)),
        .mods = g_array_new(FALSE, FALSE, sizeof(ShaderModuleCacheKey)),
    };
    lru_visit_active(&r->shader_cache, seed_collect_binding, &c);
    /* Union with the file: a short session must not shrink a seed. */
    if (c.pipes->len) {
        unsigned n_pipes, n_mods;
        unsigned new_states = pgraph_seed_merge(
            PGRAPH_SEED_GL_STATES, sizeof(ShaderState), c.pipes->data,
            c.pipes->len, 4096, &n_pipes);
        unsigned new_mods = pgraph_seed_merge(
            PGRAPH_SEED_GL_MODULES, sizeof(ShaderModuleCacheKey),
            c.mods->data, c.mods->len, 8192, &n_mods);
        if (new_states || new_mods) {
            fprintf(stderr,
                    "nv2a: shader seed written (%u states, %u module keys, "
                    "%u new)\n", n_pipes, n_mods, new_states);
        }
    }
    g_array_free(c.pipes, TRUE);
    g_array_free(c.mods, TRUE);
}

static void shader_write_lru_list_entry_to_disk(Lru *lru, LruNode *node, void *opaque)
{
    FILE *lru_list_file = (FILE*) opaque;
    size_t written = fwrite(&node->hash, sizeof(uint64_t), 1, lru_list_file);
    if (written != 1) {
        fprintf(stderr, "nv2a: Failed to write shader list entry %llx to disk\n",
                (unsigned long long) node->hash);
    }
}

void pgraph_gl_shader_write_cache_reload_list(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!g_config.perf.cache_shaders) {
        qatomic_set(&r->shader_cache_writeback_pending, false);
        qemu_event_set(&r->shader_cache_writeback_complete);
        return;
    }

    char *shader_lru_path = shader_get_lru_cache_path();
    qemu_thread_join(&r->shader_disk_thread);

    FILE *lru_list = qemu_fopen(shader_lru_path, "wb");
    g_free(shader_lru_path);
    if (!lru_list) {
        fprintf(stderr, "nv2a: Failed to open shader LRU cache for writing\n");
        return;
    }

    lru_visit_active(&r->shader_cache, shader_write_lru_list_entry_to_disk, lru_list);
    fclose(lru_list);

    seed_write_files(r);

    lru_flush(&r->shader_cache);

    qatomic_set(&r->shader_cache_writeback_pending, false);
    qemu_event_set(&r->shader_cache_writeback_complete);
}

bool pgraph_gl_shader_load_from_memory(ShaderBinding *binding)
{
    assert(glGetError() == GL_NO_ERROR);

    if (!binding->program) {
        return false;
    }

    GLuint gl_program = glCreateProgram();
    glProgramBinary(gl_program, binding->program_format, binding->program,
                    binding->program_size);
    GLint gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: GL error code %d\n",
            gl_error);
        glDeleteProgram(gl_program);
        return false;
    }

    GLint link_status = GL_FALSE;
    glGetProgramiv(gl_program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: link status is FALSE\n");
        glDeleteProgram(gl_program);
        return false;
    }

    glUseProgram(gl_program);

    g_free(binding->program);

    binding->program = NULL;
    binding->gl_program = gl_program;
    binding->gl_primitive_mode =
        get_gl_primitive_mode(binding->state.geom.polygon_front_mode,
                              binding->state.geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    glValidateProgram(gl_program);
    GLint valid = 0;
    glGetProgramiv(gl_program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(gl_program, 1024, NULL, log);
        NV2A_DPRINTF("failed to load shader binary from disk: %s\n", log);
        glDeleteProgram(gl_program);
        return false;
    }

    update_shader_uniform_locs(binding);

    return true;
}

static char *shader_get_bin_directory(uint64_t hash)
{
    const char *cfg_dir = xemu_settings_get_base_path();
    char *shader_bin_dir =
        g_strdup_printf("%s/shaders_boost/%04x", cfg_dir,
                        (uint32_t)(hash >> 48));
    return shader_bin_dir;
}

static char *shader_get_binary_path(const char *shader_bin_dir, uint64_t hash)
{
    uint64_t bin_mask = (uint64_t)0xffff << 48;
    return g_strdup_printf("%s/%012" PRIx64, shader_bin_dir, hash & ~bin_mask);
}

static void shader_load_from_disk(PGRAPHState *pg, uint64_t hash)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    char *shader_bin_dir = shader_get_bin_directory(hash);
    char *shader_path = shader_get_binary_path(shader_bin_dir, hash);
    char *cached_xemu_version = NULL;
    char *cached_gl_vendor = NULL;
    void *program_buffer = NULL;

    uint64_t cached_xemu_version_len;
    uint64_t gl_vendor_len;
    GLenum program_binary_format;
    ShaderState state;
    size_t shader_size;

    g_free(shader_bin_dir);

    qemu_mutex_lock(&r->shader_cache_lock);
    if (lru_contains_hash(&r->shader_cache, hash)) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }
    qemu_mutex_unlock(&r->shader_cache_lock);

    FILE *shader_file = qemu_fopen(shader_path, "rb");
    if (!shader_file) {
        goto error;
    }

    size_t nread;
    #define READ_OR_ERR(data, data_len) \
        do { \
            nread = fread(data, data_len, 1, shader_file); \
            if (nread != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    READ_OR_ERR(&cached_xemu_version_len, sizeof(cached_xemu_version_len));

    cached_xemu_version = g_malloc(cached_xemu_version_len +1);
    READ_OR_ERR(cached_xemu_version, cached_xemu_version_len);
    if (strcmp(cached_xemu_version, xemu_version) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));

    cached_gl_vendor = g_malloc(gl_vendor_len);
    READ_OR_ERR(cached_gl_vendor, gl_vendor_len);
    if (strcmp(cached_gl_vendor, shader_gl_vendor) != 0) {
        fclose(shader_file);
        goto error;
    }

    {
        /* The cached blob embeds a raw ShaderState: reject files written by
         * a build whose layout differs, or its size fields are garbage. */
        uint64_t cached_state_size;
        READ_OR_ERR(&cached_state_size, sizeof(cached_state_size));
        if (cached_state_size != sizeof(state)) {
            fclose(shader_file);
            goto error;
        }
    }
    READ_OR_ERR(&program_binary_format, sizeof(program_binary_format));
    READ_OR_ERR(&state, sizeof(state));
    READ_OR_ERR(&shader_size, sizeof(shader_size));

    program_buffer = g_malloc(shader_size);
    READ_OR_ERR(program_buffer, shader_size);

    #undef READ_OR_ERR

    fclose(shader_file);
    g_free(shader_path);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);

    qemu_mutex_lock(&r->shader_cache_lock);
    LruNode *node = lru_lookup(&r->shader_cache, hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    /* If we happened to regenerate this shader already, then we may as well use the new one */
    if (binding->initialized) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }

    binding->program_format = program_binary_format;
    binding->program_size = shader_size;
    binding->program = program_buffer;
    binding->cached = true;
    qemu_mutex_unlock(&r->shader_cache_lock);
    return;

error:
    /* Delete the shader so it won't be loaded again */
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(program_buffer);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
}

static void *shader_reload_lru_from_disk(void *arg)
{
    if (!g_config.perf.cache_shaders) {
        return NULL;
    }

    PGRAPHState *pg = (PGRAPHState*) arg;
    char *shader_lru_path = shader_get_lru_cache_path();

    FILE *lru_shaders_list = qemu_fopen(shader_lru_path, "rb");
    g_free(shader_lru_path);
    if (!lru_shaders_list) {
        return NULL;
    }

    uint64_t hash;
    while (fread(&hash, sizeof(uint64_t), 1, lru_shaders_list) == 1) {
        shader_load_from_disk(pg, hash);
    }

    return NULL;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));
    binding->initialized = false;
    binding->cached = false;
    binding->program = NULL;
    binding->save_thread = NULL;
    binding->uses = 0;
    binding->gl_program = 0;
    binding->gl_pipeline = 0;
    binding->gl_prog_vs = 0;
    binding->gl_prog_fs = 0;
    binding->gl_prog_gs = 0;
    binding->gl_primitive_mode = 0;
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    /* The evicted entry may be the one the renderer still points at: its
     * program and pipeline are deleted right below, and a later restore
     * (the off-frame texture touch rebinds the live program) would then
     * name a deleted object -- GL_INVALID_OPERATION, reported by AMD's
     * driver as "GL error 0x502 at touch fresh textures". The next draw
     * looks the binding up again, so dropping it here costs nothing. */
    PGRAPHGLState *r = container_of(lru, PGRAPHGLState, shader_cache);
    if (r->shader_binding == binding) {
        r->shader_binding = NULL;
    }
    binding->initialized = false;

    if (binding->save_thread) {
        qemu_thread_join(binding->save_thread);
        g_free(binding->save_thread);
    }

    glDeleteProgram(binding->gl_program);
    if (binding->gl_pipeline) {
        glDeleteProgramPipelines(1, &binding->gl_pipeline);
        binding->gl_pipeline = 0;
    }
    if (binding->program) {
        g_free(binding->program);
    }

    binding->cached = false;
    binding->save_thread = NULL;
    binding->program = NULL;
    memset(&binding->state, 0, sizeof(ShaderState));
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return memcmp(&binding->state, key, sizeof(ShaderState));
}

void pgraph_gl_init_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_mutex_init(&r->shader_cache_lock);
    qemu_event_init(&r->shader_cache_writeback_complete, false);

    if (!shader_gl_vendor) {
        shader_gl_vendor = (const char *) glGetString(GL_VENDOR);
    }

    shader_create_cache_folder();

    /* FIXME: Make this configurable */
    const size_t shader_cache_size = 50*1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = malloc(shader_cache_size * sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }

    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    qemu_thread_create(&r->shader_disk_thread, "nv2a.shdcache",
                       shader_reload_lru_from_disk, pg, QEMU_THREAD_JOINABLE);

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50*1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache, &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict = shader_module_cache_entry_post_evict;

    r->seed.state = -1;
    r->prewarm.state = -1;
}

void pgraph_gl_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* What the flip services hold: the texture touch objects, the seed and
     * prewarm lists. Links in flight stay for the next GL renderer. */
    glDeleteProgram(r->touch_prog);
    glDeleteVertexArrays(1, &r->touch_vao);
    g_free(r->seed.mod_keys);
    g_free(r->seed.pipe_states);
    for (unsigned i = r->prewarm.idx; i < r->prewarm.count; i++) {
        g_free(r->prewarm.files[i]);
    }
    g_free(r->prewarm.files);

    // Clear out shader cache
    pgraph_gl_shader_write_cache_reload_list(pg); // FIXME: also flushes, rename for clarity
    free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;

    qemu_mutex_destroy(&r->shader_cache_lock);
}

static void *shader_write_to_disk(void *arg)
{
    ShaderBinding *binding = (ShaderBinding*) arg;

    char *shader_bin = shader_get_bin_directory(binding->node.hash);
    char *shader_path = shader_get_binary_path(shader_bin, binding->node.hash);

    static uint64_t gl_vendor_len;
    if (gl_vendor_len == 0) {
        gl_vendor_len = (uint64_t) (strlen(shader_gl_vendor) + 1);
    }

    static uint64_t xemu_version_len = 0;
    if (xemu_version_len == 0) {
        xemu_version_len = (uint64_t) (strlen(xemu_version) + 1);
    }

    qemu_mkdir(shader_bin);
    g_free(shader_bin);

    FILE *shader_file = qemu_fopen(shader_path, "wb");
    if (!shader_file) {
        goto error;
    }

    size_t written;
    #define WRITE_OR_ERR(data, data_size) \
        do { \
            written = fwrite(data, data_size, 1, shader_file); \
            if (written != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    WRITE_OR_ERR(&xemu_version_len, sizeof(xemu_version_len));
    WRITE_OR_ERR(xemu_version, xemu_version_len);

    WRITE_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));
    WRITE_OR_ERR(shader_gl_vendor, gl_vendor_len);

    {
        uint64_t state_size = sizeof(binding->state);
        WRITE_OR_ERR(&state_size, sizeof(state_size));
    }
    WRITE_OR_ERR(&binding->program_format, sizeof(binding->program_format));
    WRITE_OR_ERR(&binding->state, sizeof(binding->state));

    WRITE_OR_ERR(&binding->program_size, sizeof(binding->program_size));
    WRITE_OR_ERR(binding->program, binding->program_size);

    #undef WRITE_OR_ERR

    fclose(shader_file);

    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;

    return NULL;

error:
    fprintf(stderr, "nv2a: Failed to write shader binary file to %s\n", shader_path);
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;
    return NULL;
}

void pgraph_gl_shader_cache_to_disk(ShaderBinding *binding)
{
    if (binding->cached) {
        return;
    }

    GLint program_size;
    glGetProgramiv(binding->gl_program, GL_PROGRAM_BINARY_LENGTH, &program_size);

    if (binding->program) {
        g_free(binding->program);
        binding->program = NULL;
    }

    /* program_size might be zero on some systems, if no binary formats are supported */
    if (program_size == 0) {
        return;
    }

    binding->program = g_malloc(program_size);
    GLsizei program_size_copied;
    glGetProgramBinary(binding->gl_program, program_size, &program_size_copied,
                       &binding->program_format, binding->program);
    assert(glGetError() == GL_NO_ERROR);

    binding->program_size = program_size_copied;
    binding->cached = true;

    /* Fifteen characters is all a thread name may be; the low half of the
     * hash tells the workers apart well enough. */
    char name[16];
    snprintf(name, sizeof(name), "scache-%08x", (unsigned) binding->node.hash);
    binding->save_thread = g_malloc0(sizeof(QemuThread));
    qemu_thread_create(binding->save_thread, name, shader_write_to_disk, binding, QEMU_THREAD_JOINABLE);
}

/* With GPU boost, refill only the uniform sections written since, and skip
 * uniforms unchanged since the last upload to that GL program; keyed on the
 * program, as bindings share stage programs (glsl/uniform-cache.h). */
static bool uniform_dirty_enabled(void)
{
    return pgraph_gpu_boost_gl();
}

/* At every pgraph flush (snapshot load, reset, scale change): the source
 * arrays may have changed behind every cache. */
void pgraph_gl_uniform_sources_invalidate(PGRAPHGLState *r)
{
    r->vsh_uni_src.valid = false;
    r->vsh_uni_src.ff_valid = false;
    uniform_cache_invalidate(&r->uni_cache);
}

/* prog 0: the program bound with glUseProgram. */
static void gl_upload_uniform(void *opaque, const UniformInfo *info, int loc,
                              const void *value)
{
    GLuint prog = *(const GLuint *)opaque;
    int n = info->count;

    nv2a_profile_inc_counter(NV2A_PROF_UNIFORM_UP);
    switch (info->type) {
    case UniformElementType_uint:
        if (prog) {
            glProgramUniform1uiv(prog, loc, n, value);
        } else {
            glUniform1uiv(loc, n, value);
        }
        break;
    case UniformElementType_int:
        if (prog) {
            glProgramUniform1iv(prog, loc, n, value);
        } else {
            glUniform1iv(loc, n, value);
        }
        break;
    case UniformElementType_ivec2:
        if (prog) {
            glProgramUniform2iv(prog, loc, n, value);
        } else {
            glUniform2iv(loc, n, value);
        }
        break;
    case UniformElementType_ivec4:
        if (prog) {
            glProgramUniform4iv(prog, loc, n, value);
        } else {
            glUniform4iv(loc, n, value);
        }
        break;
    case UniformElementType_float:
        if (prog) {
            glProgramUniform1fv(prog, loc, n, value);
        } else {
            glUniform1fv(loc, n, value);
        }
        break;
    case UniformElementType_vec2:
        if (prog) {
            glProgramUniform2fv(prog, loc, n, value);
        } else {
            glUniform2fv(loc, n, value);
        }
        break;
    case UniformElementType_vec3:
        if (prog) {
            glProgramUniform3fv(prog, loc, n, value);
        } else {
            glUniform3fv(loc, n, value);
        }
        break;
    case UniformElementType_vec4:
        if (prog) {
            glProgramUniform4fv(prog, loc, n, value);
        } else {
            glUniform4fv(loc, n, value);
        }
        break;
    case UniformElementType_mat2:
        if (prog) {
            glProgramUniformMatrix2fv(prog, loc, n, GL_FALSE, value);
        } else {
            glUniformMatrix2fv(loc, n, GL_FALSE, value);
        }
        break;
    default:
        g_assert_not_reached();
    }
}

static void apply_uniform_updates(PGRAPHGLState *r, const UniformInfo *info,
                                  int *locs, void *values, size_t count,
                                  GLuint prog, const int8_t *sections)
{
    int cs = -1;
    if (uniform_dirty_enabled() && prog) {
        cs = uniform_cache_slot(&r->uni_cache, prog);
    }

    /* Only judge this function's own calls. */
    (void)glGetError();
    uniform_cache_apply(&r->uni_cache, cs, r->vsh_uni_src.gen, info, locs,
                        values, count, sections, gl_upload_uniform, &prog);
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "uniform error 0x%x prog=%u\n", e, prog);
    }
    assert(e == GL_NO_ERROR);
}

/* Per draw: the wide-line geometry shader needs the guest line width, the
 * surface size that turns it into clip-space units, and which winding the
 * culling state lets through (the hardware never culls a line). */
void pgraph_gl_wide_line_uniforms(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;
    ShaderBinding *b = r->shader_binding;
    GshUniformLocs *l = &b->uniform_locs.gsh;

    unsigned int aa_width = 1, aa_height = 1;
    pgraph_apply_anti_aliasing_factor(pg, &aa_width, &aa_height);
    float size[2] = { (float)pg->surface_binding_dim.width / aa_width,
                      (float)pg->surface_binding_dim.height / aa_height };
    float width = pg->line_width / 8.0f;

    /* draw.c poses glFrontFace(CW) when the FRONTFACE bit is set. */
    uint32_t setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    int keep = 2; /* either winding */
    if (setupraster & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        bool front_ccw = !(setupraster & NV_PGRAPH_SETUPRASTER_FRONTFACE);
        switch (GET_MASK(setupraster, NV_PGRAPH_SETUPRASTER_CULLCTRL)) {
        case 1: /* front faces culled: draw a back face */
            keep = front_ccw ? 0 : 1;
            break;
        case 2: /* back faces culled: draw a front face */
            keep = front_ccw ? 1 : 0;
            break;
        default:
            break;
        }
    }

    GLuint prog = gl_sso_enabled() ? b->gl_prog_gs : 0;
    if (prog) {
        glProgramUniform2fv(prog, l->surfaceSize, 1, size);
        glProgramUniform1f(prog, l->lineWidth, width);
        glProgramUniform1i(prog, l->keepWinding, keep);
    } else {
        glUniform2fv(l->surfaceSize, 1, size);
        glUniform1f(l->lineWidth, width);
        glUniform1i(l->keepWinding, keep);
    }
}

// FIXME: Consider UBO to align with VK renderer
static void update_shader_uniforms(PGRAPHState *pg, ShaderBinding *binding)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    GLuint vs_prog = gl_sso_enabled() ? binding->gl_prog_vs : 0;
    if (uniform_dirty_enabled()) {
        pgraph_vsh_uniform_source_refresh(pg, &r->vsh_uni_src,
                                          &binding->state.vsh,
                                          binding->uniform_locs.vsh);
        apply_uniform_updates(r, VshUniformInfo, binding->uniform_locs.vsh,
                              &r->vsh_uni_src.values, VshUniform__COUNT,
                              vs_prog, pgraph_vsh_uniform_section);
    } else {
        VshUniformValues vsh_values;
        pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                      binding->uniform_locs.vsh, &vsh_values);
        apply_uniform_updates(r, VshUniformInfo, binding->uniform_locs.vsh,
                              &vsh_values, VshUniform__COUNT, vs_prog, NULL);
    }

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->uniform_locs.psh, &psh_values);

    for (int i = 0; i < 4; i++) {
        if (r->texture_binding[i] != NULL) {
            float scale = r->texture_binding[i]->scale;
            psh_values.texScale[i] = scale;
        }
    }
    apply_uniform_updates(r, PshUniformInfo, binding->uniform_locs.psh,
                          &psh_values, PshUniform__COUNT,
                          gl_sso_enabled() ? binding->gl_prog_fs : 0, NULL);
}

/* Links in flight off the draw path, adopted into the module cache once
 * done; only then does a varying family switch over to its interpreter.
 * Their names belong to the render context, which lives for the whole
 * session: a link a closing GL renderer leaves in flight, the next adopts. */
static struct {
    ShaderModuleCacheKey key;
    GLuint gl_shader;
    GLuint gl_program;
    bool used;
    bool gate;
    /* Cold-boot seed key: adopt into the module cache only, with no family
     * ready/adopted notification (nothing is waiting on it). */
    bool seed;
} comb_async[32];

/* With GPU boost, an unseen specialised state draws with its dynamic
 * sibling (same image, state in uniforms) while its own module compiles. */
static bool first_meet_enabled(void)
{
    return pgraph_gpu_boost_gl();
}

#define SPEC_QUEUED 1
#define SPEC_ADOPTED 2

static int spec_track_find(PGRAPHGLState *r, uint64_t h)
{
    for (unsigned i = 0; i < ARRAY_SIZE(r->spec_track); i++) {
        if (r->spec_track[i].phase && r->spec_track[i].hash == h) {
            return i;
        }
    }
    return -1;
}

static void spec_track_adopted(PGRAPHGLState *r, const PshState *st)
{
    uint64_t h = fast_hash((const uint8_t *)st, sizeof(*st));
    int i = spec_track_find(r, h);
    if (i >= 0) {
        r->spec_track[i].phase = SPEC_ADOPTED;
    }
}

static void spec_queue(PGRAPHGLState *r, uint64_t h, const ShaderState *st)
{
    if (r->spec_pending_w - r->spec_pending_r >=
        ARRAY_SIZE(r->spec_pending)) {
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(r->spec_track); i++) {
        if (!r->spec_track[i].phase) {
            r->spec_track[i].hash = h;
            r->spec_track[i].phase = SPEC_QUEUED;
            r->spec_pending[r->spec_pending_w %
                            ARRAY_SIZE(r->spec_pending)] = *st;
            r->spec_pending_w++;
            return;
        }
    }
}

static bool comb_async_take(PGRAPHGLState *r, PshState *out, bool *gate)
{
    if (pgraph_glsl_psh_dynamic_pending_take(out)) {
        *gate = true;
        return true;
    }
    if (r->spec_pending_r != r->spec_pending_w) {
        /* Async compiles slow the synchronous ones down in the driver
         * (MEASURED): at most 12 speculative ones in flight. */
        int in_flight = 0;
        for (unsigned i = 0; i < ARRAY_SIZE(comb_async); i++) {
            in_flight += comb_async[i].used && !comb_async[i].gate;
        }
        if (in_flight >= 12) {
            return false;
        }
        *out = r->spec_pending[r->spec_pending_r %
                               ARRAY_SIZE(r->spec_pending)].psh;
        r->spec_pending_r++;
        *gate = false;
        return true;
    }
    return false;
}

/* GLSL generation worker: sources are CPU string work, kept off the render
 * thread, which only submits finished ones to GL. Jobs go by key; holding
 * no GL name, the worker and its jobs outlive a renderer. */
#define GEN_JOBS 16
static struct {
    ShaderModuleCacheKey key;
    MString *code; /* NULL until generated */
    bool used;
    bool gate;
    bool seed;
} gen_jobs[GEN_JOBS];
static QemuMutex gen_lock;
static QemuCond gen_cond;
static QemuThread gen_thr;
static bool gen_thr_up;

static void *shader_gen_worker(void *arg)
{
    qemu_mutex_lock(&gen_lock);
    for (;;) {
        bool did = false;
        for (int i = 0; i < GEN_JOBS; i++) {
            if (gen_jobs[i].used && !gen_jobs[i].code) {
                ShaderModuleCacheKey key = gen_jobs[i].key;
                qemu_mutex_unlock(&gen_lock);
                /* Locations as on the sync path, but out of the key: a
                 * module adopted under a key the bind path never looks up
                 * leaves the first-meet cover standing forever. */
                bool locs = gl_sso_enabled();
                GenVshGlslOptions vo = key.vsh.glsl_opts;
                GenGeomGlslOptions go = key.geom.glsl_opts;
                GenPshGlslOptions po = key.psh.glsl_opts;
                vo.locations = go.locations = po.locations = locs;
                g_mutex_lock(&glsl_gen_mutex);
                MString *code =
                    key.kind == GL_VERTEX_SHADER ?
                        pgraph_glsl_gen_vsh(&key.vsh.state, vo) :
                    key.kind == GL_GEOMETRY_SHADER ?
                        pgraph_glsl_gen_geom(&key.geom.state, go) :
                        pgraph_glsl_gen_psh(&key.psh.state, po);
                g_mutex_unlock(&glsl_gen_mutex);
                if (!code) {
                    /* Declined (a stale seed key's needless geom): the
                     * submitter drops the empty source. */
                    code = mstring_from_str("");
                }
                qemu_mutex_lock(&gen_lock);
                /* A used slot with code==NULL is never recycled, so the
                 * key cannot have changed under us. */
                gen_jobs[i].code = code;
                did = true;
            }
        }
        if (!did) {
            qemu_cond_wait(&gen_cond, &gen_lock);
        }
    }
    return NULL;
}

/* Under gen_lock. True if queued (or already present). */
static bool gen_job_push_locked(const ShaderModuleCacheKey *key, bool gate,
                                bool seed)
{
    for (int i = 0; i < GEN_JOBS; i++) {
        if (gen_jobs[i].used &&
            memcmp(&gen_jobs[i].key, key, sizeof(*key)) == 0) {
            return true;
        }
    }
    for (int i = 0; i < GEN_JOBS; i++) {
        if (!gen_jobs[i].used) {
            gen_jobs[i].key = *key;
            gen_jobs[i].gate = gate;
            gen_jobs[i].seed = seed;
            gen_jobs[i].code = NULL;
            gen_jobs[i].used = true;
            return true;
        }
    }
    return false;
}

static bool gen_jobs_have_free_locked(void)
{
    for (int i = 0; i < GEN_JOBS; i++) {
        if (!gen_jobs[i].used) {
            return true;
        }
    }
    return false;
}

/* Already generated, queued, or cached? Skip regenerating. */
static bool module_known(PGRAPHGLState *r, const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((const void *)key, sizeof(*key));
    return lru_contains_hash(&r->shader_module_cache, hash);
}

static void seed_load(PGRAPHGLState *r)
{
    if (!pgraph_seed_game_tag()) {
        return; /* the boot has not named the game yet: retry next flip */
    }
    r->seed.state = 0;
    /* Only the dynamic siblings of the learned states are seeded: few
     * cover programs serve many states, so they build unnoticed during the
     * boot and first_meet finds its cover resident; the full set would
     * lag the boot. */
    if (!g_config.perf.cache_shaders || !g_config.perf.shader_seeding ||
        !gl_sso_enabled()) {
        return;
    }
    r->seed.mod_keys = pgraph_seed_read(PGRAPH_SEED_GL_MODULES,
                                        sizeof(ShaderModuleCacheKey),
                                        &r->seed.mod_n);
    r->seed.pipe_states = pgraph_seed_read(PGRAPH_SEED_GL_STATES,
                                           sizeof(ShaderState),
                                           &r->seed.pipe_n);
    /* The learned file plus the embedded dictionary, which a short file
     * must not mask; the sibling reduction below deduplicates. */
    const char *origin = "local";
    {
        unsigned dn = 0;
        ShaderModuleCacheKey *dm = pgraph_seed_dict_read(
            PGRAPH_SEED_GL_MODULES, sizeof(ShaderModuleCacheKey), &dn);
        if (dm) {
            r->seed.mod_keys = g_realloc_n(r->seed.mod_keys,
                                           r->seed.mod_n + dn, sizeof(*dm));
            memcpy(&r->seed.mod_keys[r->seed.mod_n], dm, dn * sizeof(*dm));
            r->seed.mod_n += dn;
            g_free(dm);
            origin = "local + embedded dictionary";
        }
        ShaderState *dp = pgraph_seed_dict_read(PGRAPH_SEED_GL_STATES,
                                                sizeof(ShaderState), &dn);
        if (dp) {
            r->seed.pipe_states = g_realloc_n(r->seed.pipe_states,
                                              r->seed.pipe_n + dn,
                                              sizeof(*dp));
            memcpy(&r->seed.pipe_states[r->seed.pipe_n], dp, dn * sizeof(*dp));
            r->seed.pipe_n += dn;
            g_free(dp);
            origin = "local + embedded dictionary";
        }
    }
    if (r->seed.pipe_states) {
        /* Reduce to the dynamic-sibling set: same derivation as the
         * first_meet cover path, deduplicated. */
        unsigned n_sib = 0;
        ShaderState *sib = g_new0(ShaderState, r->seed.pipe_n);
        GHashTable *seen = pgraph_seed_hash_set_new();
        for (unsigned i = 0; i < r->seed.pipe_n; i++) {
            ShaderState fb = r->seed.pipe_states[i];
            if (pgraph_glsl_psh_canonicalize_dynamic(&fb.psh) &&
                pgraph_seed_hash_set_add(
                    seen, fast_hash((const uint8_t *)&fb, sizeof(fb)))) {
                sib[n_sib++] = fb;
            }
        }
        g_hash_table_destroy(seen);
        g_free(r->seed.pipe_states);
        r->seed.pipe_states = sib;
        r->seed.pipe_n = n_sib;
        /* Module keys for exactly those siblings. */
        g_free(r->seed.mod_keys);
        r->seed.mod_keys = g_new0(ShaderModuleCacheKey, n_sib * 3);
        unsigned n_keys = 0;
        seen = pgraph_seed_hash_set_new();
        for (unsigned i = 0; i < n_sib; i++) {
            ShaderModuleCacheKey keys[3];
            bool need_gs = state_module_keys(&sib[i], keys);
            for (unsigned k = need_gs ? 0 : 1; k < 3; k++) {
                if (pgraph_seed_hash_set_add(
                        seen, fast_hash((const uint8_t *)&keys[k],
                                        sizeof(keys[k])))) {
                    r->seed.mod_keys[n_keys++] = keys[k];
                }
            }
        }
        g_hash_table_destroy(seen);
        r->seed.mod_n = n_keys;
        origin = "dynamic siblings";
    }
    if (r->seed.mod_n || r->seed.pipe_n) {
        r->seed.state = 1;
        fprintf(stderr,
                "nv2a: shader seed loaded (%u module keys, %u states, %s)\n",
                r->seed.mod_n, r->seed.pipe_n, origin);
    }
}

static void seed_finish(PGRAPHGLState *r, const char *why)
{
    g_free(r->seed.mod_keys);
    r->seed.mod_keys = NULL;
    g_free(r->seed.pipe_states);
    r->seed.pipe_states = NULL;
    r->seed.state = 0;
    fprintf(stderr, "nv2a: shader seed pump %s (%u pipelines built)\n",
            why, r->seed.pipes_built);
}

/* Seed keys still travelling through the worker or the driver's compile
 * queue: leftovers are not stalled while any of these can still land. */
static bool seed_jobs_in_flight(PGRAPHGLState *r)
{
    for (int i = 0; i < GEN_JOBS; i++) {
        if (gen_jobs[i].used && gen_jobs[i].seed) {
            return true;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(comb_async); i++) {
        if (comb_async[i].used && comb_async[i].seed) {
            return true;
        }
    }
    return false;
}

/* Flip time: seeded states whose modules are all resident become
 * pipelines; the others wait, never compiled synchronously. */
static void seed_pipe_pump(PGRAPHGLState *r, int64_t budget_us)
{
    if (r->seed.state != 1) {
        return;
    }
    int64_t t0 = g_get_monotonic_time();
    bool progress = false, out_of_budget = false;
    unsigned i = 0;
    while (i < r->seed.pipe_n) {
        if (g_get_monotonic_time() - t0 > budget_us) {
            out_of_budget = true;
            break;
        }
        ShaderState *st = &r->seed.pipe_states[i];
        ShaderModuleCacheKey keys[3];
        bool need_gs = state_module_keys(st, keys);
        bool ready = (!need_gs || module_known(r, &keys[0])) &&
                     module_known(r, &keys[1]) && module_known(r, &keys[2]);
        if (!ready) {
            i++;
            continue;
        }
        qemu_mutex_lock(&r->shader_cache_lock);
        uint64_t h = fast_hash((const uint8_t *)st, sizeof(*st));
        LruNode *node = lru_lookup(&r->shader_cache, h, st);
        ShaderBinding *b = container_of(node, ShaderBinding, node);
        if (!b->initialized) {
            /* Modules resident: pipeline setup only. b->uses stays 0, so a
             * state this session never draws adds nothing to the seed. */
            generate_shaders(r, b);
        }
        qemu_mutex_unlock(&r->shader_cache_lock);
        r->seed.pipe_states[i] = r->seed.pipe_states[--r->seed.pipe_n];
        r->seed.pipes_built++;
        progress = true;
    }
    if (progress && r->shader_binding && r->shader_binding->initialized) {
        /* generate_shaders leaves its own pipeline bound; restore the
         * live one so the not-dirty bind fast path stays truthful. */
        glUseProgram(0);
        glBindProgramPipeline(r->shader_binding->gl_pipeline);
    }
    if (out_of_budget) {
        return;
    }
    if (!r->seed.pipe_n && r->seed.mod_idx >= r->seed.mod_n) {
        seed_finish(r, "complete");
    } else if (!progress && r->seed.mod_idx >= r->seed.mod_n &&
               !seed_jobs_in_flight(r)) {
        /* Leftovers wait on modules that never landed (stale seed, failed
         * compile): dropped after 1800 passes without progress. */
        if (++r->seed.stall_flips > 1800) {
            seed_finish(r, "gave up on leftovers");
        }
    } else if (progress) {
        r->seed.stall_flips = 0;
    }
}

static void comb_async_service(PGRAPHGLState *r, int64_t budget_us)
{
    /* The service builds separable stage programs; without separate
     * shader objects nothing can adopt them. */
    if (!gl_sso_enabled()) {
        return;
    }
    int64_t svc_t0 = g_get_monotonic_time();

    if (r->seed.state == -1) {
        seed_load(r);
    }

    if (!gen_thr_up) {
        qemu_mutex_init(&gen_lock);
        qemu_cond_init(&gen_cond);
        qemu_thread_create(&gen_thr, "nv2a-glslgen", shader_gen_worker,
                           NULL, QEMU_THREAD_JOINABLE);
        gen_thr_up = true;
    }

    qemu_mutex_lock(&gen_lock);

    /* 1) Submit finished sources to GL: cheap per module, the driver's
     * compiler threads take it from there. */
    for (int i = 0; i < GEN_JOBS; i++) {
        if (!(gen_jobs[i].used && gen_jobs[i].code)) {
            continue;
        }
        if (budget_us && g_get_monotonic_time() - svc_t0 > budget_us) {
            break;
        }
        int slot = -1;
        for (unsigned s = 0; s < ARRAY_SIZE(comb_async); s++) {
            if (!comb_async[s].used) {
                slot = s;
                break;
            }
        }
        if (slot < 0) {
            break;
        }
        MString *code = gen_jobs[i].code;
        ShaderModuleCacheKey key = gen_jobs[i].key;
        bool gate = gen_jobs[i].gate;
        bool seed = gen_jobs[i].seed;
        gen_jobs[i].used = false;
        gen_jobs[i].code = NULL;
        qemu_mutex_unlock(&gen_lock);

        const char *src = mstring_get_str(code);
        if (!src[0]) {
            mstring_unref(code);
            qemu_mutex_lock(&gen_lock);
            continue;
        }
        GLuint sh;
        GLuint prog = pgraph_gl_separable_program(key.kind, src, &sh);
        mstring_unref(code);
        comb_async[slot].key = key;
        comb_async[slot].gate = gate;
        comb_async[slot].seed = seed;
        comb_async[slot].gl_shader = sh;
        comb_async[slot].gl_program = prog;
        comb_async[slot].used = true;

        qemu_mutex_lock(&gen_lock);
    }

    /* 2) Feed the worker: PSH combiners from the dynamic queue... */
    bool fed = false;
    PshState st;
    bool gate;
    bool gen_tbl_ok = g_mutex_trylock(&glsl_gen_mutex);
    while (gen_tbl_ok && gen_jobs_have_free_locked() &&
           comb_async_take(r, &st, &gate)) {
        ShaderModuleCacheKey key;
        memset(&key, 0, sizeof(key));
        key.kind = GL_FRAGMENT_SHADER;
        key.psh.state = st;
        /* gated combiners must reach adoption even if cached: keep them */
        if (!gate && module_known(r, &key)) {
            /* Already cached (seeded, say): no compile, but adopt it, or
             * the state keeps its first-meet cover forever. */
            spec_track_adopted(r, &st);
            continue;
        }
        fed |= gen_job_push_locked(&key, gate, false);
    }

    if (gen_tbl_ok) {
        g_mutex_unlock(&glsl_gen_mutex);
    }

    /* ...vertex modules of queued specialised states (both variants)... */
    for (unsigned qi = r->spec_pending_r;
         qi != r->spec_pending_w && gen_jobs_have_free_locked(); qi++) {
        ShaderState *qs = &r->spec_pending[qi % ARRAY_SIZE(r->spec_pending)];
        for (int prefix = 0; prefix < 2; prefix++) {
            ShaderModuleCacheKey key;
            memset(&key, 0, sizeof(key));
            key.kind = GL_VERTEX_SHADER;
            key.vsh.state = qs->vsh;
            key.vsh.glsl_opts.prefix_outputs = prefix;
            if (!module_known(r, &key)) {
                fed |= gen_job_push_locked(&key, false, false);
            }
        }
    }

    /* ...and dynamic vertex families (both variants). */
    while (gen_jobs_have_free_locked()) {
        VshState vst;
        if (!g_mutex_trylock(&glsl_gen_mutex)) {
            break;
        }
        bool got_vst = pgraph_glsl_vsh_dynamic_pending_take(&vst);
        g_mutex_unlock(&glsl_gen_mutex);
        if (!got_vst) {
            break;
        }
        for (int prefix = 0; prefix < 2; prefix++) {
            ShaderModuleCacheKey key;
            memset(&key, 0, sizeof(key));
            key.kind = GL_VERTEX_SHADER;
            key.vsh.state = vst;
            key.vsh.glsl_opts.prefix_outputs = prefix;
            fed |= gen_job_push_locked(&key, false, false);
        }
    }

    /* ...and cold-boot seed keys, lowest priority: only leftover slots,
     * so live compile demand always wins. */
    while (r->seed.state == 1 && r->seed.mod_idx < r->seed.mod_n &&
           gen_jobs_have_free_locked()) {
        ShaderModuleCacheKey *sk = &r->seed.mod_keys[r->seed.mod_idx++];
        if (module_known(r, sk)) {
            continue;
        }
        fed |= gen_job_push_locked(sk, false, true);
    }

    if (fed) {
        qemu_cond_signal(&gen_cond);
    }
    qemu_mutex_unlock(&gen_lock);

    bool has_completion = gl_has_parallel_compile();
    for (unsigned i = 0; i < ARRAY_SIZE(comb_async); i++) {
        if (!comb_async[i].used) {
            continue;
        }
        GLint done = GL_TRUE;
        if (has_completion) {
            glGetProgramiv(comb_async[i].gl_program,
                           GL_COMPLETION_STATUS_ARB, &done);
        }
        if (!done) {
            continue;
        }
        GLint ok = 0;
        glGetProgramiv(comb_async[i].gl_program, GL_LINK_STATUS, &ok);
        glDetachShader(comb_async[i].gl_program, comb_async[i].gl_shader);
        glDeleteShader(comb_async[i].gl_shader);
        if (ok) {
            qemu_mutex_lock(&r->shader_cache_lock);
            r->comb_adopt_hash = fast_hash((void *)&comb_async[i].key,
                                           sizeof(ShaderModuleCacheKey));
            r->comb_adopt_program = comb_async[i].gl_program;
            get_shader_module_for_key(r, &comb_async[i].key);
            if (r->comb_adopt_program) {
                delete_program(r, r->comb_adopt_program);
                r->comb_adopt_program = 0;
            }
            qemu_mutex_unlock(&r->shader_cache_lock);
            if (comb_async[i].seed) {
                /* Seed keys only fill the module cache: no family waits,
                 * and a dynamic_gen bump would defeat the bind fast path. */
            } else if (comb_async[i].key.kind == GL_FRAGMENT_SHADER) {
                if (comb_async[i].gate) {
                    g_mutex_lock(&glsl_gen_mutex);
                    pgraph_glsl_psh_dynamic_ready(&comb_async[i].key.psh.state);
                    g_mutex_unlock(&glsl_gen_mutex);
                } else {
                    spec_track_adopted(r, &comb_async[i].key.psh.state);
                }
            } else if (comb_async[i].key.kind == GL_VERTEX_SHADER) {
                g_mutex_lock(&glsl_gen_mutex);
                pgraph_glsl_vsh_dynamic_ready(&comb_async[i].key.vsh.state);
                g_mutex_unlock(&glsl_gen_mutex);
            }
        } else {
            GLchar log[1024];
            glGetProgramInfoLog(comb_async[i].gl_program, sizeof(log),
                                NULL, log);
            fprintf(stderr, "nv2a: dynamic combiner link failed: %s\n", log);
            delete_program(r, comb_async[i].gl_program);
        }
        comb_async[i].used = false;
    }
}

/* Boot prewarm: the on-disk module cache is loaded into GL in idle flips,
 * so no first draw pays a glProgramBinary. Each file holds its full key,
 * so the list is just the directory. */
static void module_prewarm_scan(PGRAPHGLState *r)
{
    r->prewarm.state = 0;
    if (!g_config.perf.cache_shaders || !gl_sso_enabled()) {
        return;
    }
    char *base = g_strdup_printf("%s/shader_modules",
                                 xemu_settings_get_base_path());
    GPtrArray *arr = g_ptr_array_new();
    GDir *d1 = g_dir_open(base, 0, NULL);
    if (d1) {
        const char *sub;
        while ((sub = g_dir_read_name(d1)) != NULL) {
            char *subp = g_strdup_printf("%s/%s", base, sub);
            GDir *d2 = g_dir_open(subp, 0, NULL);
            if (d2) {
                const char *fn;
                while ((fn = g_dir_read_name(d2)) != NULL) {
                    g_ptr_array_add(arr,
                                    g_strdup_printf("%s/%s", subp, fn));
                }
                g_dir_close(d2);
            }
            g_free(subp);
        }
        g_dir_close(d1);
    }
    g_free(base);
    r->prewarm.count = arr->len;
    r->prewarm.files = (char **)g_ptr_array_free(arr, FALSE);
    if (r->prewarm.count) {
        r->prewarm.state = 1;
        fprintf(stderr, "nv2a: prewarming %u cached shader modules\n",
                r->prewarm.count);
    }
}

/* Read just the header of a cached module file to recover its key.
 * Same layout/checks as module_load_from_disk. */
static bool module_file_read_key(const char *path, ShaderModuleCacheKey *key)
{
    FILE *f = qemu_fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = module_read_header(f, key);
    fclose(f);
    return ok;
}

static void module_prewarm_pump(PGRAPHGLState *r, int64_t budget_us)
{
    if (r->prewarm.state == -1) {
        module_prewarm_scan(r);
    }
    if (r->prewarm.state != 1) {
        return;
    }
    int64_t t0 = g_get_monotonic_time();
    while (r->prewarm.idx < r->prewarm.count) {
        if (g_get_monotonic_time() - t0 > budget_us) {
            return;
        }
        char *path = r->prewarm.files[r->prewarm.idx++];
        ShaderModuleCacheKey key;
        if (module_file_read_key(path, &key)) {
            /* Init of a fresh node loads the binary from disk and
             * resolves uniform locations, off the draw path. */
            get_shader_module_for_key(r, &key);
        }
        g_free(path);
    }
    fprintf(stderr, "nv2a: shader module prewarm complete (%u modules)\n",
            r->prewarm.count);
    g_free(r->prewarm.files);
    r->prewarm.files = NULL;
    r->prewarm.state = 0;
}

/* One point sampling each fresh texture makes the driver finalize it here,
 * in vblank slack, not in the first frame that samples it. Host plumbing:
 * every write is masked off. */
static void touch_fresh_textures(PGRAPHGLState *r, int64_t budget_us)
{
    if (r->fresh_tex_r == r->fresh_tex_w) {
        return;
    }
    if (r->fresh_tex_w - r->fresh_tex_r > ARRAY_SIZE(r->fresh_tex)) {
        r->fresh_tex_r = r->fresh_tex_w - ARRAY_SIZE(r->fresh_tex);
    }
    int64_t t0 = g_get_monotonic_time();

    if (!r->touch_prog) {
        r->touch_prog = pgraph_gl_compile_shader(
            "#version 400 core\n"
            "void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n",
            "#version 400 core\n"
            "uniform sampler2D s;\n"
            "out vec4 o;\n"
            "void main() { o = texture(s, vec2(0.5)); }\n");
        glUseProgram(r->touch_prog);
        glUniform1i(glGetUniformLocation(r->touch_prog, "s"), 0);
        glUseProgram(0);
    }

    GLint prev_vao = 0, prev_fbo = 0;
    GLint prev_tex0 = 0, prev_active = 0, sbox[4];
    GLboolean cmask[4], dmask;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    GLint smask = 0;
    glGetIntegerv(GL_STENCIL_WRITEMASK, &smask);
    GLboolean scissor_was = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, sbox);

    if (!r->touch_vao) {
        glGenVertexArrays(1, &r->touch_vao);
    }
    glBindProgramPipeline(0);
    glUseProgram(r->touch_prog);
    glBindVertexArray(r->touch_vao);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);

    while (r->fresh_tex_r != r->fresh_tex_w) {
        if (g_get_monotonic_time() - t0 > budget_us) {
            break;
        }
        GLuint tex = r->fresh_tex[r->fresh_tex_r % ARRAY_SIZE(r->fresh_tex)];
        r->fresh_tex_r++;
        if (!glIsTexture(tex)) {
            continue; /* destroyed since queueing */
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_POINTS, 0, 1);
    }
    (void)glGetError();

    glBindTexture(GL_TEXTURE_2D, prev_tex0);
    glActiveTexture(prev_active);
    glBindVertexArray(prev_vao);
    /* Back to the live binding the bind fast path assumes, not read back:
     * Intel's Windows driver 21.20.16.4526 reports the bound pipeline's
     * name as GL_CURRENT_PROGRAM. */
    ShaderBinding *live =
        (r->shader_binding && r->shader_binding->initialized) ?
            r->shader_binding : NULL;
    if (gl_sso_enabled()) {
        glUseProgram(0);
        glBindProgramPipeline(live ? live->gl_pipeline : 0);
    } else {
        glUseProgram(live ? live->gl_program : 0);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_fbo);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glDepthMask(dmask);
    glStencilMask(smask);
    if (!scissor_was) {
        glDisable(GL_SCISSOR_TEST);
    }
    glScissor(sbox[0], sbox[1], sbox[2], sbox[3]);
}

/* Flip-time services on the render thread, in the slack before the vblank
 * deadline: texture touch, module compiles and saves, prewarm, seeds. */
void pgraph_gl_shaders_flip_service(NV2AState *d)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    int64_t t0 = g_get_monotonic_time();
    pgraph_gl_tex_shadow_reset(r);
    /* Spend only the slack before the vblank deadline, less 2 ms for
     * submission and swap, so this service does not cost a vblank. */
    int64_t slack_us =
        (qatomic_read(&d->vblank_deadline) -
         qemu_clock_get_ns(QEMU_CLOCK_REALTIME)) / 1000
        - 2000;
    if (slack_us > 8000) {
        slack_us = 8000;
    }
    if (slack_us > 500) {
        touch_fresh_textures(r, slack_us * 4 / 10);
        comb_async_service(r, slack_us * 4 / 10);
        int64_t left = slack_us - (g_get_monotonic_time() - t0);
        if (left > 300) {
            module_save_queue_drain(r, left / 2);
        }
        left = slack_us - (g_get_monotonic_time() - t0);
        if (left > 300) {
            module_prewarm_pump(r, left);
        }
        left = slack_us - (g_get_monotonic_time() - t0);
        if (left > 300) {
            seed_pipe_pump(r, left);
        }
    } else {
        /* No slack: a 1 us budget, in effect the harvest only. */
        comb_async_service(r, 1);
    }
}

void pgraph_gl_bind_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    bool binding_changed = false;
    if (r->shader_binding &&
        r->shader_binding->dyn_gen == pgraph_glsl_dynamic_gen &&
        r->shader_binding->state.geom.wide_lines == r->wide_lines &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
        goto update_uniforms;
    }

    ShaderBinding *old_binding = r->shader_binding;
    ShaderState state = pgraph_glsl_get_shader_state(pg);
    state.geom.wide_lines = r->wide_lines;

    NV2A_GL_DGROUP_BEGIN("%s (%s)", __func__,
                         state.vsh.is_fixed_function ? "FF" : "PROG");

    qemu_mutex_lock(&r->shader_cache_lock);

    uint64_t shader_state_hash =
        fast_hash((uint8_t *)&state, sizeof(ShaderState));

    LruNode *node = lru_lookup(&r->shader_cache, shader_state_hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (!binding->initialized && gl_sso_enabled() && first_meet_enabled()) {
        uint64_t ph =
            fast_hash((const uint8_t *)&state.psh, sizeof(state.psh));
        int t = spec_track_find(r, ph);
        if (t >= 0 && r->spec_track[t].phase == SPEC_ADOPTED) {
            /* Module adopted off-thread: build the real binding below. */
            r->spec_track[t].phase = 0;
        } else {
            ShaderState fb = state;
            if (pgraph_glsl_psh_canonicalize_dynamic(&fb.psh)) {
                uint64_t fh = fast_hash((const uint8_t *)&fb, sizeof(fb));
                LruNode *fn = lru_lookup(&r->shader_cache, fh, &fb);
                ShaderBinding *fbind = container_of(fn, ShaderBinding, node);
                if (!fbind->initialized) {
                    /* The sibling is interpreter code, never worth a sync
                     * stall: queue it, and take the normal path meanwhile. */
                    uint64_t sh = fast_hash((const uint8_t *)&fb.psh,
                                            sizeof(fb.psh));
                    int ts = spec_track_find(r, sh);
                    if (ts >= 0 && r->spec_track[ts].phase == SPEC_ADOPTED) {
                        r->spec_track[ts].phase = 0;
                        generate_shaders(r, fbind); /* modules hit: cheap */
                    } else if (ts < 0) {
                        spec_queue(r, sh, &fb);
                    }
                }
                if (fbind->initialized) {
                    if (t < 0) {
                        spec_queue(r, ph, &state);
                    }
                    nv2a_profile_inc_counter(NV2A_PROF_SHADER_FALLBACK);
                    binding = fbind;
                }
            }
        }
    }

    if (!binding->initialized &&
        (gl_sso_enabled() || !pgraph_gl_shader_load_from_memory(binding))) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
        generate_shaders(r, binding);
        if (g_config.perf.cache_shaders && !gl_sso_enabled()) {
            pgraph_gl_shader_cache_to_disk(binding);
        }
    }
    assert(binding->initialized);
    binding->uses++;
    r->shader_binding = binding;
    binding->dyn_gen = pgraph_glsl_dynamic_gen;
    pg->program_data_dirty = false;

    /* Dirty now means written since this build, as the map's only reader,
     * check_shader_state_dirty, expects (VK clears the map on its own). */
    pgraph_clear_dirty_reg_map(pg);

    qemu_mutex_unlock(&r->shader_cache_lock);

    binding_changed = (r->shader_binding != old_binding);
    if (binding_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);
        if (gl_sso_enabled()) {
            /* A bound program takes priority over a pipeline object. */
            glUseProgram(0);
            glBindProgramPipeline(r->shader_binding->gl_pipeline);
        } else {
            glUseProgram(r->shader_binding->gl_program);
        }
    }

    NV2A_GL_DGROUP_END();

update_uniforms:
    assert(r->shader_binding);
    assert(r->shader_binding->initialized);
    update_shader_uniforms(pg, r->shader_binding);
}

GLuint pgraph_gl_compile_shader(const char *vs_src, const char *fs_src)
{
    GLint status;
    char err_buf[512];

    // Compile vertex shader
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(vs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Vertex shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Compile fragment shader
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(fs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Fragment shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Link vertex and fragment shaders
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    // Flag shaders for deletion (will still be retained for lifetime of prog)
    glDeleteShader(vs);
    glDeleteShader(fs);

    return prog;
}
