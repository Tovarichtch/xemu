/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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
#include "renderer.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/pgraph/seed.h"
#include "hw/xbox/nv2a/pgraph/glsl/uniform-cache.h"

const size_t MAX_UNIFORM_ATTR_VALUES_SIZE = NV2A_VERTEXSHADER_ATTRIBUTES * 4 * sizeof(float);

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    size_t num_sets = ARRAY_SIZE(r->descriptor_sets);

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 2 * num_sets,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = NV2A_MAX_TEXTURES * num_sets,
        }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = ARRAY_SIZE(pool_sizes),
        .pPoolSizes = pool_sizes,
        .maxSets = ARRAY_SIZE(r->descriptor_sets),
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    r->descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[2 + NV2A_MAX_TEXTURES];

    bindings[0] = (VkDescriptorSetLayoutBinding){
        .binding = VSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    bindings[1] = (VkDescriptorSetLayoutBinding){
        .binding = PSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        bindings[2 + i] = (VkDescriptorSetLayoutBinding){
            .binding = PSH_TEX_BINDING + i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings),
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);
    r->descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layouts[ARRAY_SIZE(r->descriptor_sets)];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        layouts[i] = r->descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->descriptor_pool,
        .descriptorSetCount = ARRAY_SIZE(r->descriptor_sets),
        .pSetLayouts = layouts,
    };
    VK_CHECK(
        vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets));
}

static void destroy_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeDescriptorSets(r->device, r->descriptor_pool,
                         ARRAY_SIZE(r->descriptor_sets), r->descriptor_sets);
    for (int i = 0; i < ARRAY_SIZE(r->descriptor_sets); i++) {
        r->descriptor_sets[i] = VK_NULL_HANDLE;
    }
}

void pgraph_vk_update_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    bool need_uniform_write =
        r->uniforms_changed ||
        !r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset;

    if (!(r->shader_bindings_changed || r->texture_bindings_changed ||
          (r->descriptor_set_index == 0) || need_uniform_write)) {
        return; // Nothing changed
    }

    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };
    VkDeviceSize ubo_buffer_total_size = 0;
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_total_size += layouts[i]->total_size;
    }
    bool need_ubo_staging_buffer_reset =
        r->uniforms_changed &&
        !pgraph_vk_buffer_has_space_for(pg, BUFFER_UNIFORM_STAGING,
                                        ubo_buffer_total_size,
                                        r->device_props.limits.minUniformBufferOffsetAlignment);

    bool need_descriptor_write_reset =
        (r->descriptor_set_index >= ARRAY_SIZE(r->descriptor_sets));

    if (need_descriptor_write_reset || need_ubo_staging_buffer_reset) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        need_uniform_write = true;
    }

    VkWriteDescriptorSet descriptor_writes[2 + NV2A_MAX_TEXTURES];

    if (need_uniform_write) {
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            void *data = layouts[i]->allocation;
            VkDeviceSize size = layouts[i]->total_size;
            r->uniform_buffer_offsets[i] = pgraph_vk_append_to_buffer(
                pg, BUFFER_UNIFORM_STAGING, &data, &size, 1,
                r->device_props.limits.minUniformBufferOffsetAlignment);
        }

        r->uniforms_changed = false;
    }

    assert(r->descriptor_set_index < ARRAY_SIZE(r->descriptor_sets));

    VkDescriptorBufferInfo ubo_buffer_infos[2];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
            .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
            .offset = r->uniform_buffer_offsets[i],
            .range = layouts[i]->total_size,
        };
        descriptor_writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = i == 0 ? VSH_UBO_BINDING : PSH_UBO_BINDING,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &ubo_buffer_infos[i],
        };
    }

    VkDescriptorImageInfo image_infos[NV2A_MAX_TEXTURES];
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        /* Split samplers: the sampler rides the unit, not the image
         * node (falls back to the node's sampler while unset). */
        VkSampler smp = r->texture_unit_sampler[i] ?
                            r->texture_unit_sampler[i] :
                            r->texture_bindings[i]->sampler;
        image_infos[i] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->texture_bindings[i]->image_view,
            .sampler = smp,
        };
        descriptor_writes[2 + i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = PSH_TEX_BINDING + i,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[i],
        };
    }

    vkUpdateDescriptorSets(r->device, 6, descriptor_writes, 0, NULL);

    r->descriptor_set_index++;
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    for (int i = 0; i < ARRAY_SIZE(binding->vsh.uniform_locs); i++) {
        binding->vsh.uniform_locs[i] = uniform_index(
            &binding->vsh.module_info->uniforms, VshUniformInfo[i].name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->psh.uniform_locs); i++) {
        binding->psh.uniform_locs[i] = uniform_index(
            &binding->psh.module_info->uniforms, PshUniformInfo[i].name);
    }
}

static ShaderModuleInfo *
get_and_ref_shader_module_for_key(PGRAPHVkState *r,
                                  const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_ref_shader_module(module->module_info);
    return module->module_info;
}

/* The modules of a state's pipeline, each referenced once: geometry (NULL
 * when the state needs none), vertex, and fragment when psh is given. */
void pgraph_vk_ref_state_modules(PGRAPHVkState *r, const ShaderState *st,
                                 ShaderModuleInfo **geom,
                                 ShaderModuleInfo **vsh,
                                 ShaderModuleInfo **psh)
{
    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&st->geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        key.geom.state = st->geom;
        key.geom.glsl_opts.vulkan = true;
        *geom = get_and_ref_shader_module_for_key(r, &key);
    } else {
        *geom = NULL;
    }

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_VERTEX_BIT;
    key.vsh.state = st->vsh;
    key.vsh.glsl_opts.vulkan = true;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    key.vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
    key.vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    *vsh = get_and_ref_shader_module_for_key(r, &key);

    if (!psh) {
        return;
    }
    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    key.psh.state = st->psh;
    key.psh.glsl_opts.vulkan = true;
    key.psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
    key.psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
    *psh = get_and_ref_shader_module_for_key(r, &key);
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));

    NV2A_VK_DPRINTF("cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);

    pgraph_vk_ref_state_modules(r, &binding->state, &binding->geom.module_info,
                                &binding->vsh.module_info,
                                &binding->psh.module_info);

    update_shader_uniform_locs(binding);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *snode = container_of(node, ShaderBinding, node);

    ShaderModuleInfo *modules[] = {
        snode->vsh.module_info,
        snode->geom.module_info,
        snode->psh.module_info,
    };
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (modules[i]) {
            pgraph_vk_unref_shader_module(r, modules[i]);
        }
    }
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *snode = container_of(node, ShaderBinding, node);
    return memcmp(&snode->state, key, sizeof(ShaderState));
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    MString *code;

    switch (module->key.kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        code = NULL;
    }

    module->module_info = pgraph_vk_create_shader_module_from_glsl(
        r, module->key.kind, mstring_get_str(code));
    /* A recycled address must not find the previous module's values. */
    uniform_cache_forget(&r->uni_cache, (uintptr_t)module->module_info);
    pgraph_vk_ref_shader_module(module->module_info);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_unref_shader_module(r, module->module_info);
    module->module_info = NULL;
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static void shader_cache_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* A cache smaller than a game's working set of shader states
     * recompiles forever. */
    const size_t shader_cache_size = 16384;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = g_malloc_n(shader_cache_size, sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }
    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50 * 1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache,
                     &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict =
        shader_module_cache_entry_post_evict;
}

static void shader_cache_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->shader_cache);
    g_free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;
}

static ShaderBinding *get_shader_binding_for_state(PGRAPHVkState *r,
                                                   const ShaderState *state)
{
    uint64_t hash = fast_hash((void *)state, sizeof(*state));
    LruNode *node = lru_lookup(&r->shader_cache, hash, state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    NV2A_VK_DPRINTF("shader state hash: %016" PRIx64 " %p", hash, binding);
    return binding;
}

/* An unseen state draws with its canonical dynamic sibling (same image,
 * state carried in uniforms, as on GL) while a worker compiles its SPIR-V
 * off the draw path into the disk cache, where the next meeting finds it.
 * vk_seen holds the states whose modules are known resident (their binding
 * went through get_shader_binding_for_state at least once). Open
 * addressing; a slot lost to collision only means one extra sibling
 * detour. */
#define VK_SEEN_SLOTS 32768
static uint64_t vk_seen[VK_SEEN_SLOTS];

static bool vk_seen_contains(uint64_t h)
{
    unsigned i = h % VK_SEEN_SLOTS;
    for (unsigned p = 0; p < 8; p++, i = (i + 1) % VK_SEEN_SLOTS) {
        if (vk_seen[i] == h) {
            return true;
        }
        if (!vk_seen[i]) {
            return false;
        }
    }
    return false;
}

static void vk_seen_add(uint64_t h)
{
    unsigned i = h % VK_SEEN_SLOTS;
    for (unsigned p = 0; p < 8; p++, i = (i + 1) % VK_SEEN_SLOTS) {
        if (vk_seen[i] == h) {
            return;
        }
        if (!vk_seen[i]) {
            vk_seen[i] = h;
            return;
        }
    }
}

#define VK_SPEC_QUEUED 1
#define VK_SPEC_ADOPTED 2
static struct { uint64_t hash; int phase; } vk_spec_track[1024];
static ShaderState vk_spec_pending[512];
static unsigned vk_spec_r, vk_spec_w;
static QemuThread vk_spec_thread;
static QemuMutex vk_spec_lock;
static QemuCond vk_spec_cond;
static bool vk_spec_started;
static bool vk_spec_busy; /* the worker is compiling vk_spec_pending[r] */
static bool vk_spec_push_constants;

static int vk_spec_find(uint64_t h)
{
    for (unsigned i = 0; i < ARRAY_SIZE(vk_spec_track); i++) {
        if (vk_spec_track[i].phase && vk_spec_track[i].hash == h) {
            return i;
        }
    }
    return -1;
}

/* Compile every module of one state to SPIR-V. Safe off the render thread:
 * the generators only read the state, glslang is serialised, and the disk
 * cache is the only output; no Vulkan object or shared cache is touched. */
static void vk_spec_compile_state(const ShaderState *st)
{
    struct { glslang_stage_t stage; MString *code; } jobs[3];
    int n = 0;
    bool need_gs = pgraph_glsl_need_geom(&st->geom);
    if (need_gs) {
        GenGeomGlslOptions o = { .vulkan = true };
        jobs[n].stage = GLSLANG_STAGE_GEOMETRY;
        jobs[n].code = pgraph_glsl_gen_geom(&st->geom, o);
        n++;
    }
    {
        GenVshGlslOptions o = { .vulkan = true,
                                .prefix_outputs = need_gs,
                                .use_push_constants_for_uniform_attrs =
                                    vk_spec_push_constants,
                                .ubo_binding = VSH_UBO_BINDING };
        jobs[n].stage = GLSLANG_STAGE_VERTEX;
        jobs[n].code = pgraph_glsl_gen_vsh(&st->vsh, o);
        n++;
    }
    {
        GenPshGlslOptions o = { .vulkan = true,
                                .ubo_binding = PSH_UBO_BINDING,
                                .tex_binding = PSH_TEX_BINDING };
        jobs[n].stage = GLSLANG_STAGE_FRAGMENT;
        jobs[n].code = pgraph_glsl_gen_psh(&st->psh, o);
        n++;
    }
    for (int i = 0; i < n; i++) {
        GByteArray *spv = pgraph_vk_compile_glsl_to_spv(
            jobs[i].stage, mstring_get_str(jobs[i].code));
        if (spv) {
            g_byte_array_unref(spv);
        }
        mstring_unref(jobs[i].code);
    }
}

static void *vk_spec_worker_fn(void *opaque)
{
    qemu_mutex_lock(&vk_spec_lock);
    for (;;) {
        while (vk_spec_r == vk_spec_w) {
            qemu_cond_wait(&vk_spec_cond, &vk_spec_lock);
        }
        ShaderState st =
            vk_spec_pending[vk_spec_r % ARRAY_SIZE(vk_spec_pending)];
        vk_spec_busy = true;
        qemu_mutex_unlock(&vk_spec_lock);

        vk_spec_compile_state(&st);

        qemu_mutex_lock(&vk_spec_lock);
        vk_spec_busy = false;
        vk_spec_r++;
        uint64_t h = fast_hash((const uint8_t *)&st, sizeof(st));
        int t = vk_spec_find(h);
        if (t >= 0) {
            vk_spec_track[t].phase = VK_SPEC_ADOPTED;
        }
        qemu_cond_broadcast(&vk_spec_cond);
    }
    return NULL;
}

/* The worker's track of state h, read under its lock: 0 when untracked. */
static int vk_spec_phase(uint64_t h)
{
    if (!vk_spec_started) {
        return 0;
    }
    qemu_mutex_lock(&vk_spec_lock);
    int t = vk_spec_find(h);
    int phase = t >= 0 ? vk_spec_track[t].phase : 0;
    qemu_mutex_unlock(&vk_spec_lock);
    return phase;
}

/* Frees the track slot of a state the worker has landed. */
static void vk_spec_release_adopted(uint64_t h)
{
    qemu_mutex_lock(&vk_spec_lock);
    int t = vk_spec_find(h);
    if (t >= 0 && vk_spec_track[t].phase == VK_SPEC_ADOPTED) {
        vk_spec_track[t].phase = 0;
    }
    qemu_mutex_unlock(&vk_spec_lock);
}

/* Before glslang goes away (finalize): drop what is queued, freeing its
 * track slots, and wait for the compile in progress. The worker stays,
 * idle, for the next Vulkan renderer. */
static void vk_spec_quiesce(void)
{
    if (!vk_spec_started) {
        return;
    }
    qemu_mutex_lock(&vk_spec_lock);
    vk_spec_w = vk_spec_r + (vk_spec_busy ? 1 : 0);
    while (vk_spec_busy) {
        qemu_cond_wait(&vk_spec_cond, &vk_spec_lock);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(vk_spec_track); i++) {
        if (vk_spec_track[i].phase == VK_SPEC_QUEUED) {
            vk_spec_track[i].phase = 0;
        }
    }
    qemu_mutex_unlock(&vk_spec_lock);
}

static void vk_spec_queue(uint64_t h, const ShaderState *st)
{
    if (!vk_spec_started) {
        vk_spec_started = true;
        qemu_mutex_init(&vk_spec_lock);
        qemu_cond_init(&vk_spec_cond);
        qemu_thread_create(&vk_spec_thread, "vk-firstmeet", vk_spec_worker_fn,
                           NULL, QEMU_THREAD_DETACHED);
    }
    qemu_mutex_lock(&vk_spec_lock);
    if (vk_spec_find(h) >= 0 ||
        vk_spec_w - vk_spec_r >= ARRAY_SIZE(vk_spec_pending)) {
        qemu_mutex_unlock(&vk_spec_lock);
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(vk_spec_track); i++) {
        if (!vk_spec_track[i].phase) {
            vk_spec_track[i].hash = h;
            vk_spec_track[i].phase = VK_SPEC_QUEUED;
            vk_spec_pending[vk_spec_w % ARRAY_SIZE(vk_spec_pending)] = *st;
            vk_spec_w++;
            qemu_cond_broadcast(&vk_spec_cond);
            break;
        }
    }
    qemu_mutex_unlock(&vk_spec_lock);
}

static GArray *vk_pump_states; /* covers then states, for the library pump */

static void vk_pump_remember(const ShaderState *st)
{
    if (!vk_pump_states) {
        vk_pump_states = g_array_new(FALSE, FALSE, sizeof(ShaderState));
    }
    g_array_append_val(vk_pump_states, *st);
}

const ShaderState *pgraph_vk_seed_pump_states(unsigned *count)
{
    if (!vk_pump_states) {
        *count = 0;
        return NULL;
    }
    *count = vk_pump_states->len;
    return (const ShaderState *)vk_pump_states->data;
}

/* Queue the first-meet covers during boot: few dynamic covers serve many
 * specialised states, and SEGABOOT leaves the worker time to land their
 * .spv, so the first scene already finds them, as with the GL seed. */
static void vk_seed_siblings(void)
{
    if (!g_config.perf.shader_seeding) {
        return;
    }
    const char *tag = pgraph_seed_game_tag();
    if (!tag) {
        return;
    }
    unsigned n = 0;
    ShaderState *states = pgraph_seed_dict_read(PGRAPH_SEED_VK_STATES,
                                                sizeof(ShaderState), &n);

    /* Learn like GL: the game's own seed (Vulkan section) is queued with the
     * embedded dictionary, and what the session drew is written back at
     * shutdown (vk_seed_write). */
    unsigned n_local = 0;
    ShaderState *local = pgraph_seed_read(PGRAPH_SEED_VK_STATES,
                                          sizeof(ShaderState), &n_local);
    if (!states && !local) {
        return;
    }
    /* One queue slot per distinct cover. */
    GHashTable *seen = pgraph_seed_hash_set_new();
    unsigned queued = 0;
    for (int src = 0; src < 2; src++) {
        ShaderState *arr = src ? local : states;
        unsigned cnt = src ? n_local : n;
        for (unsigned i = 0; arr && i < cnt; i++) {
            ShaderState fb = arr[i];
            if (!pgraph_glsl_psh_canonicalize_dynamic(&fb.psh)) {
                continue;
            }
            uint64_t h = fast_hash((const uint8_t *)&fb, sizeof(fb));
            if (!pgraph_seed_hash_set_add(seen, h)) {
                continue;
            }
            vk_spec_queue(h, &fb);
            vk_pump_remember(&fb);
            queued++;
        }
    }
    /* Then the specialised states themselves, so a known state is a disk
     * hit at its first meeting rather than a synchronous compile. */
    unsigned queued_states = 0;
    for (int src = 0; src < 2; src++) {
        ShaderState *arr = src ? local : states;
        unsigned cnt = src ? n_local : n;
        for (unsigned i = 0; arr && i < cnt; i++) {
            uint64_t h = fast_hash((const uint8_t *)&arr[i], sizeof(arr[i]));
            if (!pgraph_seed_hash_set_add(seen, h)) {
                continue;
            }
            vk_spec_queue(h, &arr[i]);
            vk_pump_remember(&arr[i]);
            queued_states++;
        }
    }
    g_hash_table_destroy(seen);
    g_free(states);
    g_free(local);
    if (queued || queued_states) {
        fprintf(stderr,
                "nv2a: vk first-meet covers seeding (%u siblings + %u "
                "states: dict %u states, learned %u states)\n",
                queued, queued_states, n, n_local);
    }
}

/* True once the worker has landed this state's SPIR-V (or a binding
 * already exists): module creation is then a disk hit, never glslang. */
bool pgraph_vk_seed_spv_ready_hash(uint64_t h)
{
    if (vk_seen_contains(h)) {
        return true;
    }
    if (!vk_spec_started) {
        return false;
    }
    qemu_mutex_lock(&vk_spec_lock);
    int t = vk_spec_find(h);
    bool ready = t >= 0 && vk_spec_track[t].phase == VK_SPEC_ADOPTED;
    qemu_mutex_unlock(&vk_spec_lock);
    return ready;
}

struct VkSeedCollect {
    GArray *states;
};

static void vk_seed_collect(Lru *lru, LruNode *node, void *opaque)
{
    struct VkSeedCollect *c = opaque;
    ShaderBinding *b = container_of(node, ShaderBinding, node);
    g_array_append_val(c->states, b->state);
}

/* Union of the seed on disk and what this session drew, capped; a session
 * that met nothing new leaves the file alone (GL rule: a short session
 * must never shrink a seed learned over a long one). */
static void vk_seed_write(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!g_config.perf.shader_seeding) {
        return;
    }
    struct VkSeedCollect c = {
        .states = g_array_new(FALSE, FALSE, sizeof(ShaderState)),
    };
    lru_visit_active(&r->shader_cache, vk_seed_collect, &c);
    unsigned total = 0;
    unsigned added = c.states->len ?
        pgraph_seed_merge(PGRAPH_SEED_VK_STATES, sizeof(ShaderState),
                          c.states->data, c.states->len, 4096, &total) : 0;
    if (added) {
        fprintf(stderr, "nv2a: vk shader seed written (%u states, %u new)\n",
                total, added);
    }
    g_array_free(c.states, TRUE);
}

/* After a vmstate load/flush the source arrays changed behind every cache. */
void pgraph_vk_uniform_sources_invalidate(PGRAPHVkState *r)
{
    r->vsh_uni_src.valid = false;
    r->vsh_uni_src.ff_valid = false;
    r->uni_last_pushed[0] = NULL;
    r->uni_last_pushed[1] = NULL;
    uniform_cache_invalidate(&r->uni_cache);
}

static void vk_upload_uniform(void *opaque, const UniformInfo *info, int loc,
                              const void *value)
{
    uniform_copy(opaque, loc, (void *)value, 4,
                 info->size * info->count / 4);
}

/* Per-field compare+copy into the module's UBO allocation. Returns
 * whether the allocation content changed (= a re-push is needed). */
static bool vk_apply_uniform_updates(PGRAPHVkState *r,
                                     ShaderUniformLayout *layout,
                                     const UniformInfo *info, int *locs,
                                     void *values, size_t count,
                                     const ShaderModuleInfo *mod,
                                     const int8_t *sections)
{
    int cs = uniform_cache_slot(&r->uni_cache, (uintptr_t)mod);
    return uniform_cache_apply(&r->uni_cache, cs, r->vsh_uni_src.gen, info,
                               locs, values, count, sections,
                               vk_upload_uniform, layout);
}

static void set_psh_texscale_values(PGRAPHState *pg,
                                    PshUniformValues *psh_values)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < 4; i++) {
        assert(r->texture_bindings[i] != NULL);
        float scale = r->texture_bindings[i]->key.scale;

        BasicColorFormatInfo f_basic =
            kelvin_color_format_info_map[r->texture_bindings[i]
                                             ->key.state.color_format];
        if (!f_basic.linear) {
            scale = 1.0;
        }

        psh_values->texScale[i] = scale;
    }
}

/* Uniform sections tracked by their pgraph dirty markers
 * (glsl/uniform-cache.h; on VK only this path reads and clears them): a
 * clean section is not re-read, compared or copied, and no whole-block hash
 * runs per draw. */
static void update_shader_uniforms_tracked(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    ShaderBinding *binding = r->shader_binding;
    bool changed = false;

    pgraph_vsh_uniform_source_refresh(pg, &r->vsh_uni_src, &binding->state.vsh,
                                      binding->vsh.uniform_locs);
    changed |= vk_apply_uniform_updates(
        r, &binding->vsh.module_info->uniforms, VshUniformInfo,
        binding->vsh.uniform_locs, &r->vsh_uni_src.values, VshUniform__COUNT,
        binding->vsh.module_info, pgraph_vsh_uniform_section);

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->psh.uniform_locs,
                                       &psh_values);
    set_psh_texscale_values(pg, &psh_values);
    changed |= vk_apply_uniform_updates(
        r, &binding->psh.module_info->uniforms, PshUniformInfo,
        binding->psh.uniform_locs, &psh_values, PshUniform__COUNT,
        binding->psh.module_info, NULL);

    /* The UBO offsets hold the last pushed modules' blocks: a module
     * switch re-pushes even with equal fields. */
    if (binding->vsh.module_info != r->uni_last_pushed[0] ||
        binding->psh.module_info != r->uni_last_pushed[1]) {
        changed = true;
        r->uni_last_pushed[0] = binding->vsh.module_info;
        r->uni_last_pushed[1] = binding->psh.module_info;
    }

    r->uniforms_changed |= changed;
}

static void update_shader_uniforms(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    assert(r->shader_binding);
    update_shader_uniforms_tracked(pg);
    nv2a_profile_inc_counter(r->uniforms_changed ?
                                 NV2A_PROF_SHADER_UBO_DIRTY :
                                 NV2A_PROF_SHADER_UBO_NOTDIRTY);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_shaders(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;

    r->shader_bindings_changed = false;

    if (!r->shader_binding ||
        pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        ShaderState new_state = pgraph_glsl_get_shader_state(pg);
        if (!r->shader_binding || memcmp(&r->shader_binding->state, &new_state,
                                         sizeof(ShaderState))) {
            uint64_t h =
                fast_hash((const uint8_t *)&new_state, sizeof(new_state));
            if (!vk_seen_contains(h)) {
                int phase = vk_spec_phase(h);
                if (phase == VK_SPEC_ADOPTED) {
                    /* Worker landed the .spv on disk: the normal path
                     * below is a fast cache hit now. */
                    vk_spec_release_adopted(h);
                } else {
                    ShaderState fb = new_state;
                    if (pgraph_glsl_psh_canonicalize_dynamic(&fb.psh)) {
                        uint64_t fh = fast_hash((const uint8_t *)&fb,
                                                sizeof(fb));
                        int sphase = fh != h ? vk_spec_phase(fh) : 0;
                        bool sib_ready =
                            fh != h &&
                            (vk_seen_contains(fh) ||
                             sphase == VK_SPEC_ADOPTED);
                        if (sib_ready) {
                            if (sphase == VK_SPEC_ADOPTED) {
                                vk_spec_release_adopted(fh);
                            }
                            if (!phase) {
                                vk_spec_queue(h, &new_state);
                            }
                            nv2a_profile_inc_counter(
                                NV2A_PROF_SHADER_FALLBACK);
                            new_state = fb;
                        } else if (fh != h && !sphase) {
                            /* The sibling, interpreter code, compiles
                             * slower than the state: queue it; the
                             * normal path serves until it lands. */
                            vk_spec_queue(fh, &fb);
                        }
                    }
                }
            }
            r->shader_binding = get_shader_binding_for_state(r, &new_state);
            vk_seen_add(
                fast_hash((const uint8_t *)&new_state, sizeof(new_state)));
            r->shader_bindings_changed = true;
        }
    } else {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    }

    update_shader_uniforms(pg);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_init_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_init_glsl_compiler();
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    shader_cache_init(pg);

    r->use_push_constants_for_uniform_attrs =
        (r->device_props.limits.maxPushConstantsSize >=
         MAX_UNIFORM_ATTR_VALUES_SIZE);
    vk_spec_push_constants = r->use_push_constants_for_uniform_attrs;
    qemu_event_init(&r->seed_writeback_complete, false);
    r->seed_writeback_pending = false;
}

/* Per flip until the boot has named the game (the seeds are keyed by
 * the game executable, captured a few seconds into SEGABOOT). */
void pgraph_vk_seed_service(void)
{
    static bool done;
    if (done || !pgraph_seed_game_tag()) {
        return;
    }
    done = true;
    vk_seed_siblings();
}

/* Shutdown writeback, on the pfifo thread (see pgraph_vk_process_pending):
 * write the learned seed, then release the UI thread waiting in
 * pre_shutdown_wait. Always signals, even when learning is off. */
void pgraph_vk_shader_seed_writeback(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    vk_seed_write(pg);
    qatomic_set(&r->seed_writeback_pending, false);
    qemu_event_set(&r->seed_writeback_complete);
}

void pgraph_vk_finalize_shaders(PGRAPHState *pg)
{
    vk_seed_write(pg);
    qemu_event_destroy(&pg->vk_renderer_state->seed_writeback_complete);
    shader_cache_finalize(pg);
    destroy_descriptor_sets(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
    vk_spec_quiesce();
    pgraph_vk_finalize_glsl_compiler();
}
