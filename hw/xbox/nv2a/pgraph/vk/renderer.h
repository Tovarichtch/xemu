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

#ifndef HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H

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

#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <volk.h>
#include <spirv_reflect.h>
#include <vk_mem_alloc.h>

#include "debug.h"
#include "constants.h"
#include "glsl.h"

#define HAVE_EXTERNAL_MEMORY 1

typedef struct QueueFamilyIndices {
    int queue_family;
} QueueFamilyIndices;

typedef struct MemorySyncRequirement {
    hwaddr addr, size;
} MemorySyncRequirement;

/* One cached copy-on-write snapshot of vertex RAM (sync_vertex_ram_buffer). */
typedef struct VertexCowCacheEntry {
    hwaddr addr;
    VkDeviceSize size;
    VkDeviceSize arena_offset;
    uint32_t epoch;
    uint8_t slot;
    bool valid;
} VertexCowCacheEntry;

typedef struct RenderPassState {
    VkFormat color_format;
    VkFormat zeta_format;
} RenderPassState;

typedef struct RenderPass {
    RenderPassState state;
    VkRenderPass render_pass;
} RenderPass;

typedef struct PipelineKey {
    bool clear;
    RenderPassState render_pass_state;
    ShaderState shader_state;
    uint32_t regs[9];
    VkVertexInputBindingDescription binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
} PipelineKey;

typedef struct PipelineBinding {
    LruNode node;
    PipelineKey key;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkRenderPass render_pass;
    unsigned int draw_time;
    bool has_dynamic_line_width;
} PipelineBinding;

enum Buffer {
    BUFFER_STAGING_DST,
    BUFFER_STAGING_SRC,
    BUFFER_COMPUTE_DST,
    BUFFER_COMPUTE_SRC,
    BUFFER_INDEX,
    BUFFER_INDEX_STAGING,
    BUFFER_VERTEX_RAM,
    BUFFER_VERTEX_INLINE,
    BUFFER_VERTEX_INLINE_STAGING,
    BUFFER_UNIFORM,
    BUFFER_UNIFORM_STAGING,
    BUFFER_COUNT
};

/* Each staging buffer and the device buffer its sync copy fills, in the
 * order of alt_staging[] and alt_device[] (buffer.c). */
typedef struct StagedBuffer {
    int staging, device;
} StagedBuffer;
extern const StagedBuffer pgraph_vk_staged_buffers[3];

typedef struct StorageBuffer {
    VkBuffer buffer;
    VkBufferUsageFlags usage;
    VmaAllocationCreateInfo alloc_info;
    VmaAllocation allocation;
    VkMemoryPropertyFlags properties;
    size_t buffer_offset;
    size_t buffer_size;
    uint8_t *mapped;
} StorageBuffer;

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
    /* Guest CPU reads of this surface's RAM; consumed by the eviction
     * dead-writeback predicate (pgraph_writeback_dead). */
    unsigned int cpu_reads;
    /* Lazy writeback: eviction writeback owed, run only if a texture
     * load, CPU access or savevm reads that RAM (image kept meanwhile). */
    bool writeback_owed;
    /* In-place sampling: set while the image rests in SHADER_READ_ONLY
     * (sampled in place); a writer transitions it out and clears this. */
    bool in_shader_read;
    /* submit_count when the borrow transition was recorded: while equal, the
     * image still rests in its attachment layout (open recording). */
    uint32_t borrow_submit;

    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    VkImage image;
    VkImageView image_view;
    VmaAllocation allocation;

    // Used for scaling
    VkImage image_scratch;
    VkImageLayout image_scratch_current_layout;
    VmaAllocation allocation_scratch;

    bool initialized;
} SurfaceBinding;

typedef struct ShaderModuleInfo {
    int refcnt;
    char *glsl;
    GByteArray *spirv;
    VkShaderModule module;
    SpvReflectShaderModule reflect_module;
    SpvReflectDescriptorSet **descriptor_sets;
    ShaderUniformLayout uniforms;
    ShaderUniformLayout push_constants;
} ShaderModuleInfo;

typedef struct ShaderModuleCacheKey {
    VkShaderStageFlagBits kind;
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

typedef struct ShaderModuleCacheEntry {
    LruNode node;
    ShaderModuleCacheKey key;
    ShaderModuleInfo *module_info;
} ShaderModuleCacheEntry;

typedef struct ShaderBinding {
    LruNode node;
    ShaderState state;
    struct {
        ShaderModuleInfo *module_info;
        VshUniformLocs uniform_locs;
    } vsh;
    struct {
        ShaderModuleInfo *module_info;
    } geom;
    struct {
        ShaderModuleInfo *module_info;
        PshUniformLocs uniform_locs;
    } psh;
} ShaderBinding;

typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
    float scale;
    uint32_t filter;
    uint32_t address;
    uint32_t border_color;
    uint32_t max_anisotropy;
} TextureKey;

typedef struct TextureBinding {
    LruNode node;
    TextureKey key;
    VkImage image;
    VkImageLayout current_layout;
    VkImageView image_view;
    VmaAllocation allocation;
    VkSampler sampler;
    bool possibly_dirty;
    uint64_t hash;
    unsigned int draw_time;
    uint32_t submit_time;
    /* Monotonic creation id: tells a recreated texture apart from the one
     * previously in this node even if the driver recycled the Vk handles. */
    uint64_t seq;
    /* Last frame this binding's content was validated: content is re-hashed
     * at most once a frame, as nearby RAM writes keep re-flagging it dirty. */
    unsigned int validation_frame;
    /* In-place sampling: the image is the surface's own (never destroy it);
     * image_view is a private view on it, owned by this binding. */
    bool s2t_borrowed;
    bool logo; /* doge mode: the SEGABOOT logo replaced */
    VkImageCreateInfo image_ci; /* creation parameters (image pool key) */
} TextureBinding;

typedef struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
} QueryReport;

typedef struct PvideoState {
    bool enabled;
    hwaddr base;
    hwaddr limit;
    hwaddr offset;

    int pitch;
    int format;

    int in_width;
    int in_height;
    int out_width;
    int out_height;

    int in_s;
    int in_t;
    int out_x;
    int out_y;

    float scale_x;
    float scale_y;

    bool color_key_enabled;
    uint32_t color_key;
} PvideoState;

typedef struct PGRAPHVkDisplayState {
    ShaderModuleInfo *display_frag;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_set;

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    VkRenderPass render_pass;
    VkFramebuffer framebuffer;

    VkImage image;
    VkImageView image_view;
    VkDeviceMemory memory;
    VkSampler sampler;

    struct {
        PvideoState state;
        int width, height;
        VkImage image;
        VkImageView image_view;
        VmaAllocation allocation;
        VkSampler sampler;
    } pvideo;

    int width, height;
    int draw_time;

    // OpenGL Interop
#ifdef WIN32
    HANDLE handle;
#else
    int fd;
#endif
    GLuint gl_memory_obj;
    GLuint gl_texture_id;
} PGRAPHVkDisplayState;

typedef struct ComputePipelineKey {
    VkFormat host_fmt;
    bool pack;
    int workgroup_size;
} ComputePipelineKey;

typedef struct ComputePipeline {
    LruNode node;
    ComputePipelineKey key;
    VkPipeline pipeline;
} ComputePipeline;

typedef struct PGRAPHVkComputeState {
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[1024];
    int descriptor_set_index;
    VkPipelineLayout pipeline_layout;
    Lru pipeline_cache;
    ComputePipeline *pipeline_cache_entries;
} PGRAPHVkComputeState;

/* Main command buffers in the ring: a slot is recorded again two submissions
 * after its own, once its fence has settled. */
#define PGRAPH_VK_SLOTS 2

/* One sampler state of the sampler cache (texture.c). */
typedef struct VkSamplerCacheKey {
    uint32_t filter;
    uint32_t address;
    uint32_t border_color;
    uint32_t max_anisotropy;
    uint8_t color_format;
    uint8_t dimensionality;
    uint8_t levels;
    uint8_t min_mip, max_mip;
} VkSamplerCacheKey;

#define VK_SAMPLER_CACHE_SIZE 512

typedef struct PGRAPHVkState {
    uint32_t vk_api_version;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    int debug_depth;

    bool debug_utils_extension_enabled;
    bool custom_border_color_extension_enabled;
    bool memory_budget_extension_enabled;
    bool pipeline_library_extension_enabled;
    bool pipeline_library;
    bool host_query_reset;
    /* Vulkan 1.3 shaderDemoteToHelperInvocation enabled: SPIR-V 1.6 may
     * lower discard to OpDemoteToHelperInvocation. Else SPIR-V 1.5. */
    bool shader_demote;
    bool eds_extension_enabled;
    bool eds3_extension_enabled;
    bool vertex_input_dynamic_extension_enabled;

    /* Resolved dynamic-state capabilities (extension + feature bit). */
    bool dyn_cull_front;
    bool dyn_blend;
    bool dyn_vertex_input;
    /* Pipeline libraries: shader halves compiled once, linked per draw,
     * kept for the renderer's life (draw.c). */
    GArray *gpl_pre, *gpl_frag;
    /* Library pump progress per render pass (draw.c): the next seed state
     * and the states whose SPIR-V was not ready yet. The libraries die
     * with the renderer, so a new renderer pumps again. */
    struct {
        RenderPassState rp[4];
        unsigned n, pos[4], rpos[4];
        GArray *retry[4];
        uint64_t *state_hash;
        const ShaderState *state_hash_base;
        unsigned state_hash_n;
    } pump;

    /* Last emitted dynamic state, to skip redundant vkCmdSet*. */
    struct {
        bool valid;
        uint32_t blend, blendcolor, control_0, setupraster;
        uint32_t control_1, control_2, topology;
        float line_width;
    } dyn_last;
    /* Vertex input last set in this command buffer: vkCmdSetVertexInputEXT
     * rebuilds fetch state on some drivers (MEASURED on AMD Windows). */
    struct {
        bool valid;
        int nb, na;
        VkVertexInputBindingDescription bind[NV2A_VERTEXSHADER_ATTRIBUTES];
        VkVertexInputAttributeDescription attr[NV2A_VERTEXSHADER_ATTRIBUTES];
    } vi_last;

    VkPhysicalDevice physical_device;
    VkPhysicalDeviceFeatures enabled_physical_device_features;
    VkPhysicalDeviceProperties device_props;
    VkDevice device;
    VmaAllocator allocator;
    uint32_t allocator_last_submit_index;

    VkQueue queue;
    VkCommandPool command_pool;
    /* The mains and the syncs (one of each per slot), then the aux. */
    VkCommandBuffer command_buffers[2 * PGRAPH_VK_SLOTS + 1];

    VkCommandBuffer command_buffer;
    bool budget_check_pending;
    unsigned int command_buffer_start_time;
    bool in_command_buffer;
    uint32_t submit_count;
    /* submit_count at the last teardown_after_settle: every submission at or
     * below it has retired. */
    uint32_t settled_submit;

    VkCommandBuffer aux_command_buffer;
    bool in_aux_command_buffer;
    VkFence aux_fence;
    bool aux_in_flight;

    /* Rotation: an async finish submits this slot and records on into the
     * next without waiting. */
    int slot;
    VkCommandBuffer slot_main[PGRAPH_VK_SLOTS];
    VkCommandBuffer slot_sync[PGRAPH_VK_SLOTS];
    VkFence slot_fence[PGRAPH_VK_SLOTS];
    bool slot_fence_pending[PGRAPH_VK_SLOTS];
    /* EMA of the vkBeginCommandBuffer cost in us (varies by driver). */
    double cb_begin_us;
    /* One sync->main semaphore per slot: re-signalling a binary semaphore
     * whose previous wait is still pending is undefined in Vulkan. */
    VkSemaphore slot_sem[PGRAPH_VK_SLOTS];
    /* Second set of the staged buffers and of their device destinations,
     * swapped in at rotation after a submission that staged data: Vulkan
     * orders no memory between submissions, so the next sync copy must not
     * overwrite what a main still reads. */
    StorageBuffer alt_staging[3];
    StorageBuffer alt_device[3];

    /* Deferred destruction of what a slot or the open recording may still
     * use (pipelines, framebuffers, and the images, views and samplers that
     * textures and surfaces retire), drained once all has settled: at a
     * synchronous finish, the flip's at the latest. */
    struct XemuVkTrashEntry {
        VkPipeline pipeline;
        VkPipelineLayout layout;
        VkSampler sampler;
        VkFramebuffer framebuffer;
        VkImageView image_view;
        VkImage image;
        VmaAllocation allocation;
        VkImageCreateInfo image_ci; /* texture images: pool key */
        bool pool_ok;
    } *trash;                       /* grown as needed: it never overflows */
    int trash_n, trash_cap;
    /* Retired texture images kept for reuse (pgraph_vk_image_pool_put). */
    struct XemuVkPooledImage {
        VkImage image;
        VmaAllocation allocation;
        VkImageCreateInfo ci;
        size_t bytes;
        uint32_t heap;
        uint64_t seq;
    } image_pool[48];
    int image_pool_n;
    size_t image_pool_bytes;
    uint64_t image_pool_seq;

    /* Framebuffers cached by attachment views, render pass and size, so a
     * switch back to a live target reuses its framebuffer. Entries retire
     * through the trash when a view dies or the cache is full. */
    struct XemuVkFramebufferEntry {
        VkImageView color, zeta;
        VkRenderPass render_pass;
        uint32_t width, height;
        VkFramebuffer framebuffer;
        uint64_t last_use;
    } framebuffer_cache[64];
    int framebuffer_cache_n;
    uint64_t framebuffer_use_counter;
    VkFramebuffer framebuffer; /* the bound one; VK_NULL_HANDLE when stale */
    bool framebuffer_dirty;
    /* surface_update fast path (as GL): the write masks and surface type
     * of the last full pass. */
    bool last_color_write, last_zeta_write;
    unsigned int last_surface_type;
    /* Dropped when the surface leaves the valid list. */
    PGRAPHRtMemo rt_memo;
    /* Surface expiry is frame-granular: run it once per frame. */
    unsigned int expire_frame_time;
    /* Eager submission at render-target switches (surface.c): how many
     * a frame is decided from the costs measured on this machine. */
    unsigned rt_switches;
    unsigned eager_interval, eager_count;
    unsigned eager_draw_count; /* draws since the last eager submit */
    double submit_us; /* EMA of the CPU cost of one queue submission */
    /* EMA of the wait at slot reuse that a submission causes later: the
     * slot's next recording first waits for this buffer's fence. */
    double reuse_wait_us;
    /* GPU busy time of the frame (draw.c pgraph_vk_frame_gpu_*). */
    struct {
        VkQueryPool pool;
        unsigned capacity; /* pairs */
        unsigned w;        /* pairs written this frame */
        uint64_t *scratch;
        double busy_us;    /* EMA over frames */
    } frame_gpu;

    VkRenderPass render_pass;
    GArray *render_passes; // RenderPass
    bool in_render_pass;
    bool in_draw;

    Lru pipeline_cache;
    VkPipelineCache vk_pipeline_cache;
    /* Pipeline cache persistence helper (pgraph_vk_save_pipeline_cache) */
    QemuThread pcache_thread;
    bool pcache_thread_spawned;
    bool pcache_busy;
    size_t pcache_saved_len;
    PipelineBinding *pipeline_cache_entries;
    PipelineBinding *pipeline_binding;
    bool pipeline_binding_changed;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    /* One set per draw, sized so a full frame of a draw-heavy game (Silent
     * Scope Complete, MEASURED) fits without a mid-frame finish. */
    VkDescriptorSet descriptor_sets[8192];
    int descriptor_set_index;

    StorageBuffer storage_buffers[BUFFER_COUNT];

    MemorySyncRequirement vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;
    /* Vertex RAM copy on write. consumed: pages unsettled work reads, never
     * overwritten in the mirror; stale: mirror pages behind guest RAM (after
     * a COW, or when another consumer took the NV2A dirty bit). New content
     * of a consumed page goes to an arena, for the draw being built only. */
    unsigned long *vertex_consumed_bitmap;
    unsigned long *vertex_stale_bitmap;
    size_t bitmap_size;
    StorageBuffer vertex_arena[2];
    bool vertex_arena_pending; /* cow staged, bind not recorded yet */
    bool vertex_arena_used;    /* current recording binds arena content */
    MemorySyncRequirement vertex_attr_span[NV2A_VERTEXSHADER_ATTRIBUTES];
    bool vertex_attr_cow[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint8_t vertex_attr_cow_slot[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize vertex_attr_cow_offset[NV2A_VERTEXSHADER_ATTRIBUTES];
    /* Snapshots reused by later draws of unchanged content; an entry dies on
     * a newer guest or mirror write, a stale mark, or its arena's reset. */
    VertexCowCacheEntry vertex_cow_cache[512];
    unsigned vertex_cow_cache_rr;
    uint32_t vertex_arena_epoch[2];

    VkVertexInputAttributeDescription vertex_attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int vertex_attribute_to_description_location[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_attribute_descriptions;

    VkVertexInputBindingDescription vertex_binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_binding_descriptions;
    hwaddr vertex_attribute_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    QTAILQ_HEAD(, SurfaceBinding) invalid_surfaces;
    SurfaceBinding *color_binding, *zeta_binding;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete; // common

    Lru texture_cache;
    TextureBinding *texture_cache_entries;
    TextureBinding *texture_bindings[NV2A_MAX_TEXTURES];
    /* Split samplers: sampler state leaves the image cache key; each
     * unit's sampler is resolved at bind time. */
    VkSampler texture_unit_sampler[NV2A_MAX_TEXTURES];
    /* The renderer's samplers: a handful of distinct sampler states per
     * game, tiny objects, never evicted (destroyed at finalize). */
    struct {
        bool used;
        VkSamplerCacheKey key;
        VkSampler sampler;
    } sampler_cache[VK_SAMPLER_CACHE_SIZE];
    unsigned sampler_cache_n;
    TextureBinding dummy_texture;
    bool texture_bindings_changed;
    uint64_t texture_binding_seq;
    VkFormatProperties *texture_format_properties;
    /* Formats already asked whether this device blits them (texture.c). */
    struct {
        VkFormat format;
        bool ok;
    } blit_formats[8];
    int blit_formats_n;

    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    ShaderBinding *shader_binding;
    ShaderModuleInfo *quad_vert_module, *solid_frag_module;
    bool shader_bindings_changed;
    bool use_push_constants_for_uniform_attrs;

    Lru shader_module_cache;
    ShaderModuleCacheEntry *shader_module_cache_entries;

    // FIXME: Merge these into a structure
    size_t uniform_buffer_offsets[2];
    bool uniforms_changed;
    /* Tracked uniforms (shaders.c): the vertex uniform values; the last
     * values written into each module's UBO allocation, keyed by module
     * address (a module that finds no slot is pushed on every draw); and
     * which modules' values uniform_buffer_offsets hold, as a module switch
     * must re-push even with equal field values. */
    VshUniformSource vsh_uni_src;
    UniformCache uni_cache;
    const ShaderModuleInfo *uni_last_pushed[2];

    VkQueryPool query_pool;
    int max_queries_in_flight; // FIXME: Move out to constant
    int num_queries_in_flight;
    /* Query slots ended in a submitted command buffer, and slots counted into
     * zpass_pixel_count_result (reports read at availability, no WAIT). */
    int queries_submitted;
    int queries_read;
    bool new_query_needed;
    bool query_in_flight;
    uint32_t zpass_pixel_count_result;
    QSIMPLEQ_HEAD(, QueryReport) report_queue; // FIXME: Statically allocate

    /* Cost-model fragment counting (pgraph/cost.c): per cost segment, a
     * pipeline-statistics query (fragment shader invocations) and a precise
     * occlusion query (samples passed), begun late in begin_draw (draw.c). */
    VkQueryPool cost_stats_pool; /* VK_NULL_HANDLE if unsupported */
    VkQueryPool cost_occl_pool;
    bool cost_pools_reset;  /* this command buffer recorded the slot resets */
    int cost_pending_slot;                /* -1 = none */
    bool cost_pending_samples;
    bool cost_recording;                  /* our queries begun in this cmdbuf */
    bool cost_recording_samples;
    int cost_recording_slot;
    bool cost_seg_recorded[XEMU_COST_SEGS];         /* begun+ended: readable */
    bool cost_seg_recorded_samples[XEMU_COST_SEGS];

    SurfaceFormatInfo kelvin_surface_zeta_vk_map[3];

    uint32_t clear_parameter;

    /* Native depth: native-size scratch images for zeta surface-to-texture
     * copies, one per format and size; freed at finalize only, since
     * in-flight submissions may use them. */
    struct {
        VkImage image;
        VmaAllocation allocation;
        VkFormat format;
        unsigned int width, height;
    } znative_scratch[4];

    /* Seed learning: the learnt shader seed is written back at
     * shutdown, on the pfifo thread. */
    bool seed_writeback_pending;
    QemuEvent seed_writeback_complete;

    PGRAPHVkDisplayState display;
    PGRAPHVkComputeState compute;
} PGRAPHVkState;

// renderer.c
void pgraph_vk_save_pipeline_cache(PGRAPHState *pg, bool sync);
bool pgraph_vk_image_pool_put(PGRAPHVkState *r, VkImage image,
                              VmaAllocation allocation,
                              const VkImageCreateInfo *ci);
bool pgraph_vk_image_pool_get(PGRAPHVkState *r, const VkImageCreateInfo *ci,
                              VkImage *image, VmaAllocation *allocation);
void pgraph_vk_image_pool_flush(PGRAPHVkState *r);
void pgraph_vk_check_memory_budget(PGRAPHState *pg);

// debug.c
#define RGBA_RED     (float[4]){1,0,0,1}
#define RGBA_YELLOW  (float[4]){1,1,0,1}
#define RGBA_GREEN   (float[4]){0,1,0,1}
#define RGBA_BLUE    (float[4]){0,0,1,1}
#define RGBA_PINK    (float[4]){1,0,1,1}
#define RGBA_DEFAULT (float[4]){0,0,0,0}

void pgraph_vk_debug_init(void);
void pgraph_vk_insert_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                   float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_begin_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                  float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_end_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd);

// instance.c
void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp);
void pgraph_vk_finalize_instance(PGRAPHState *pg);
QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device);
uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties);

// glsl.c
extern bool pgraph_vk_spirv_1_6;
void pgraph_vk_init_glsl_compiler(void);
void pgraph_vk_finalize_glsl_compiler(void);
GByteArray *pgraph_vk_compile_glsl_to_spv(glslang_stage_t stage,
                                          const char *glsl_source);
VkShaderModule pgraph_vk_create_shader_module_from_spv(PGRAPHVkState *r,
                                                       GByteArray *spv);
ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl);
void pgraph_vk_ref_shader_module(ShaderModuleInfo *info);
void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);
void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);

// buffer.c
void pgraph_vk_init_buffers(NV2AState *d);
void pgraph_vk_finalize_buffers(NV2AState *d);
void pgraph_vk_buffer_ensure_capacity(PGRAPHState *pg, int index,
                                      VkDeviceSize needed);
bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment);
VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment);

// command.c
void pgraph_vk_init_command_buffers(PGRAPHState *pg);
void pgraph_vk_finalize_command_buffers(PGRAPHState *pg);
VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg);
void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd);
void pgraph_vk_end_single_time_commands_async(PGRAPHState *pg,
                                              VkCommandBuffer cmd);
void pgraph_vk_wait_for_aux(PGRAPHState *pg);

// image.c
void pgraph_vk_transition_image_layout(PGRAPHState *pg, VkCommandBuffer cmd,
                                       VkImage image, VkFormat format,
                                       VkImageLayout oldLayout,
                                       VkImageLayout newLayout);

// vertex.c
void pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element);
void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d);
void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset, void *data,
                                    VkDeviceSize size);
void pgraph_vk_vertex_mark_stale(PGRAPHState *pg, hwaddr addr,
                                 VkDeviceSize size);
void pgraph_vk_vertex_cow_cache_invalidate(PGRAPHVkState *r, hwaddr addr,
                                           VkDeviceSize size);
VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size);
VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count);

// surface.c
void pgraph_vk_init_surfaces(PGRAPHState *pg);
void pgraph_vk_finalize_surfaces(PGRAPHState *pg);
void pgraph_vk_surface_flush(NV2AState *d);
void pgraph_vk_process_pending_downloads(NV2AState *d);
void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface);
SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_vk_wait_for_surface_download(SurfaceBinding *e);
void pgraph_vk_download_dirty_surfaces(NV2AState *d);
void pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg, hwaddr start, hwaddr size);
void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force);
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write);
SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr);
void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d);
void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg);

// surface-compute.c
void pgraph_vk_init_compute(PGRAPHState *pg);
bool pgraph_vk_compute_needs_finish(PGRAPHVkState *r);
void pgraph_vk_compute_finish_complete(PGRAPHVkState *r);
void pgraph_vk_finalize_compute(PGRAPHState *pg);
void pgraph_vk_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                  VkCommandBuffer cmd, VkBuffer src,
                                  VkBuffer dst, bool downscale);
void pgraph_vk_pack_depth_stencil_dims(PGRAPHState *pg,
                                       SurfaceBinding *surface,
                                       VkCommandBuffer cmd, VkBuffer src,
                                       VkBuffer dst, unsigned int in_w,
                                       unsigned int in_h, unsigned int out_w,
                                       unsigned int out_h);
void pgraph_vk_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    VkCommandBuffer cmd, VkBuffer src,
                                    VkBuffer dst);

// display.c
void pgraph_vk_init_display(PGRAPHState *pg);
void pgraph_vk_finalize_display(PGRAPHState *pg);
void pgraph_vk_render_display(PGRAPHState *pg);

// texture.c
void pgraph_vk_init_textures(PGRAPHState *pg);
void pgraph_vk_finalize_textures(PGRAPHState *pg);
void pgraph_vk_bind_textures(NV2AState *d);
void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                            hwaddr size);
void pgraph_vk_trim_texture_cache(PGRAPHState *pg);

// shaders.c
void pgraph_vk_init_shaders(PGRAPHState *pg);
void pgraph_vk_finalize_shaders(PGRAPHState *pg);
void pgraph_vk_shader_seed_writeback(PGRAPHState *pg);
void pgraph_vk_update_descriptor_sets(PGRAPHState *pg);
void pgraph_vk_uniform_sources_invalidate(PGRAPHVkState *r);

void pgraph_vk_bind_shaders(PGRAPHState *pg);
void pgraph_vk_prewarm_shaders(NV2AState *d);
void pgraph_vk_ref_state_modules(PGRAPHVkState *r, const ShaderState *st,
                                 ShaderModuleInfo **geom,
                                 ShaderModuleInfo **vsh,
                                 ShaderModuleInfo **psh);

#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1
#define PSH_TEX_BINDING 2

// reports.c
void pgraph_vk_init_reports(PGRAPHState *pg);
void pgraph_vk_finalize_reports(PGRAPHState *pg);
void pgraph_vk_clear_report_value(NV2AState *d);
void pgraph_vk_get_report(NV2AState *d, uint32_t parameter);
void pgraph_vk_process_pending_reports(NV2AState *d);
void pgraph_vk_process_pending_reports_internal(NV2AState *d);

typedef enum FinishReason {
    VK_FINISH_REASON_VERTEX_BUFFER_DIRTY,
    VK_FINISH_REASON_SURFACE_CREATE,
    VK_FINISH_REASON_SURFACE_DOWN,
    VK_FINISH_REASON_NEED_BUFFER_SPACE,
    VK_FINISH_REASON_FRAMEBUFFER_DIRTY,
    VK_FINISH_REASON_PRESENTING,
    VK_FINISH_REASON_FLIP_STALL,
    VK_FINISH_REASON_FLUSH,
    VK_FINISH_REASON_STALLED,
    VK_FINISH_REASON_SURFACE_UP,
    VK_FINISH_REASON_TEXTURE_UP,
    VK_FINISH_REASON_EAGER,
} FinishReason;

// draw.c
void pgraph_vk_init_pipelines(PGRAPHState *pg);
void pgraph_vk_libpump_step(PGRAPHState *pg);
void pgraph_vk_seed_service(void);
const ShaderState *pgraph_vk_seed_pump_states(unsigned *count);
bool pgraph_vk_seed_spv_ready_hash(uint64_t hash);
void pgraph_vk_finalize_pipelines(PGRAPHState *pg);
void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_vk_draw_begin(NV2AState *d);
void pgraph_vk_draw_end(NV2AState *d);
extern const XemuCostQueryOps pgraph_vk_cost_ops;
void pgraph_vk_finish(PGRAPHState *pg, FinishReason why);
void pgraph_vk_settle_reports(PGRAPHState *pg);
void pgraph_vk_reclaim_command_buffer(PGRAPHState *pg, bool reuse);
void pgraph_vk_framebuffer_drop_view(PGRAPHVkState *r, VkImageView view);
void pgraph_vk_frame_gpu_cb_begin(PGRAPHVkState *r, VkCommandBuffer cmd);
void pgraph_vk_frame_gpu_cb_end(PGRAPHVkState *r, VkCommandBuffer cmd);
void pgraph_vk_frame_gpu_resolve(PGRAPHState *pg);
void pgraph_vk_eager_frame_update(PGRAPHState *pg);
void pgraph_vk_wait_all_slots(PGRAPHVkState *r);
void pgraph_vk_settle_all_slots(PGRAPHState *pg);
void pgraph_vk_trash_push(PGRAPHVkState *r,
                          const struct XemuVkTrashEntry *e);
void pgraph_vk_trash_drain(PGRAPHVkState *r);
void pgraph_vk_flush_draw(NV2AState *d);
void pgraph_vk_begin_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg);

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg);
void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd);

/* In-place sampling (texture.c): drop every texture binding that borrows
 * this surface's image, before that image changes owner or dies. */
void pgraph_vk_texture_drop_borrowed(NV2AState *d, SurfaceBinding *surface);
/* Clears in_shader_read and re-dirties the bindings that borrow the image. */
void pgraph_vk_surface_left_shader_read(PGRAPHState *pg,
                                        SurfaceBinding *surface);
/* Layout the surface image actually rests in outside a render pass. */
static inline VkImageLayout pgraph_vk_surface_base_layout(
    const SurfaceBinding *s)
{
    if (s->in_shader_read) {
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return s->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}
/* True while the borrow transition is still in the open recording: an aux
 * submission would run ahead of it, so such paths finish first. */
static inline bool pgraph_vk_surface_borrow_pending(const PGRAPHVkState *r,
                                                    const SurfaceBinding *s)
{
    return s->in_shader_read && r->in_command_buffer &&
           s->borrow_submit == r->submit_count;
}
/* Return a surface sampled in place to its attachment layout, in the main
 * command buffer: the transition orders earlier reads before later draws. */
void pgraph_vk_surface_ensure_attachment_layout(PGRAPHState *pg,
                                                SurfaceBinding *surface);

// blit.c
void pgraph_vk_image_blit(NV2AState *d);

// gpuprops.c
void pgraph_vk_determine_gpu_properties(NV2AState *d);
GPUProperties *pgraph_vk_get_gpu_properties(void);

#endif
