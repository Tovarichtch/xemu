/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
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

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"

#include "qemu/fast-hash.h"
#include "data/segaboot_logo.rgba.h"
#include "qemu/lru.h"
#include "renderer.h"
#include "ui/xemu-settings.h"

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode);

static const VkImageType dimensionality_to_vk_image_type[] = {
    0,
    VK_IMAGE_TYPE_1D,
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_TYPE_3D,
};
static const VkImageViewType dimensionality_to_vk_image_view_type[] = {
    0,
    VK_IMAGE_VIEW_TYPE_1D,
    VK_IMAGE_VIEW_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_3D,
};

static VkSamplerAddressMode lookup_texture_address_mode(int idx)
{
    assert(0 < idx && idx < ARRAY_SIZE(pgraph_texture_addr_vk_map));
    return pgraph_texture_addr_vk_map[idx];
}

// FIXME: Move to common
// FIXME: We can shrink the size of this structure
// FIXME: Use simple allocator
typedef struct TextureLevel {
    unsigned int width, height, depth;
    hwaddr vram_addr;
    void *decoded_data;
    size_t decoded_size;
} TextureLevel;

typedef struct TextureLayer {
    TextureLevel levels[16];
} TextureLayer;

typedef struct TextureLayout {
    TextureLayer layers[6];
} TextureLayout;

// FIXME: Move to common
static enum S3TC_DECOMPRESS_FORMAT kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(false);
    }
}

// FIXME: Move to common
static void memcpy_image(void *dst, void *src, int min_stride, int dst_stride, int src_stride, int height)
{
    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t *src_ptr = (uint8_t *)src;

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, min_stride);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
}

// FIXME: Move to common
static size_t get_cubemap_layer_size(PGRAPHState *pg, TextureShape s)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int block_size;

    unsigned int w = s.width, h = s.height;
    size_t length = 0;

    if (!f.linear && s.border) {
        w = MAX(16, w * 2);
        h = MAX(16, h * 2);
    }

    if (is_compressed) {
        block_size =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                8 :
                16;
    }

    for (int level = 0; level < s.levels; level++) {
        if (is_compressed) {
            length += w / 4 * h / 4 * block_size;
        } else {
            length += w * h * f.bytes_per_pixel;
        }

        w /= 2;
        h /= 2;
    }

    return ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

// FIXME: Move to common
// FIXME: More refactoring
// FIXME: Possible parallelization of decoding
// FIXME: Bounds checking
static TextureLayout *get_texture_layout(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    TextureShape s = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];

    NV2A_VK_DGROUP_BEGIN("Texture %d: cubemap=%d, dimensionality=%d, color_format=0x%x, levels=%d, width=%d, height=%d, depth=%d border=%d, min_mipmap_level=%d, max_mipmap_level=%d, pitch=%d",
        texture_idx,
        s.cubemap,
        s.dimensionality,
        s.color_format,
        s.levels,
        s.width,
        s.height,
        s.depth,
        s.border,
        s.min_mipmap_level,
        s.max_mipmap_level,
        s.pitch
        );

    // Sanity checks on below assumptions
    if (f.linear) {
        assert(s.dimensionality == 2);
    }
    if (s.cubemap) {
        assert(s.dimensionality == 2);
        assert(!f.linear);
    }
    assert(s.dimensionality > 1);

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    void *texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset;

    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);
    void *palette_data_ptr = (char *)d->vram_ptr + texture_palette_vram_offset;

    unsigned int adjusted_width = s.width, adjusted_height = s.height,
                 adjusted_pitch = s.pitch, adjusted_depth = s.depth;

    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    TextureLayout *layout = g_malloc0(sizeof(TextureLayout));

    if (f.linear) {
        assert(s.pitch % f.bytes_per_pixel == 0 && "Can't handle strides unaligned to pixels");

        size_t converted_size;
        uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data_ptr, palette_data_ptr, adjusted_width,
            adjusted_height, 1, adjusted_pitch, 0, &converted_size);

        if (!converted) {
            int dst_stride = adjusted_width * f.bytes_per_pixel;
            assert(adjusted_width <= s.width);
            converted_size = dst_stride * adjusted_height;
            converted = g_malloc(converted_size);
            memcpy_image(converted, texture_data_ptr, adjusted_width * f.bytes_per_pixel, dst_stride,
                         adjusted_pitch, adjusted_height);
        }

        assert(s.levels == 1);
        layout->layers[0].levels[0] = (TextureLevel){
            .width = adjusted_width,
            .height = adjusted_height,
            .depth = 1,
            .decoded_size = converted_size,
            .decoded_data = converted,
        };

        NV2A_VK_DGROUP_END();
        return layout;
    }

    bool is_compressed = pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size = 0;
    if (is_compressed) {
        bool is_dxt1 =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        block_size = is_dxt1 ? 8 : 16;
    }

    if (s.dimensionality == 2) {
        hwaddr layer_size = s.cubemap ? get_cubemap_layer_size(pg, s) : 0;
        const int num_layers = s.cubemap ? 6 : 1;
        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset +
                               layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                NV2A_VK_DPRINTF("Layer %d Level %d @ %x", layer, level, (int)((char*)texture_data_ptr - (char*)d->vram_ptr));

                width = MAX(width, 1);
                height = MAX(height, 1);
                if (is_compressed) {
                    // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                    unsigned int tex_width = width, tex_height = height;
                    unsigned int physical_width = (width + 3) & ~3,
                                 physical_height = (height + 3) & ~3;

                    size_t converted_size = width * height * 4;
                    uint8_t *converted = s3tc_decompress_2d(
                        kelvin_format_to_s3tc_format(s.color_format),
                        texture_data_ptr, width, height);
                    assert(converted);

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.

                        // glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
                        // glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
                        tex_width = s.width;
                        tex_height = s.height;
                        // if (physical_width == width) {
                        //     glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        // }

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr +=
                        physical_width / 4 * physical_height / 4 * block_size;
                } else {
                    unsigned int pitch = width * f.bytes_per_pixel;
                    unsigned int tex_width = width, tex_height = height;

                    size_t converted_size = height * pitch;
                    uint8_t *unswizzled = (uint8_t*)g_malloc(height * pitch);
                    unswizzle_rect(texture_data_ptr, width, height,
                                   unswizzled, pitch, f.bytes_per_pixel);

                    uint8_t *converted = pgraph_convert_texture_data(
                        s, unswizzled, palette_data_ptr, width, height, 1,
                        pitch, 0, &converted_size);

                    if (converted) {
                        g_free(unswizzled);
                    } else {
                        converted = unswizzled;
                    }

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.
                        // glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        tex_width = s.width;
                        tex_height = s.height;
                        // pixel_data += 4 * f.bytes_per_pixel + 4 * pitch;

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr += width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }
    } else if (s.dimensionality == 3) {
        assert(!f.linear);
        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;

        for (int level = 0; level < s.levels; level++) {
            if (is_compressed) {
                width = MAX(width, 1);
                height = MAX(height, 1);
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                depth = MAX(depth, 1);

                size_t converted_size = width * height * depth * 4;
                uint8_t *converted = s3tc_decompress_3d(
                    kelvin_format_to_s3tc_format(s.color_format),
                    texture_data_ptr, width, height, depth);
                assert(converted);

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += physical_width / 4 * physical_height / 4 * depth * block_size;
            } else {
                width = MAX(width, 1);
                height = MAX(height, 1);
                depth = MAX(depth, 1);

                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;

                size_t unswizzled_size = slice_pitch * depth;
                uint8_t *unswizzled = g_malloc(unswizzled_size);
                unswizzle_box(texture_data_ptr, width, height, depth,
                              unswizzled, row_pitch, slice_pitch,
                              f.bytes_per_pixel);

                size_t converted_size;
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data_ptr, width, height, depth,
                    row_pitch, slice_pitch, &converted_size);

                if (converted) {
                    g_free(unswizzled);
                } else {
                    converted = unswizzled;
                    converted_size = unswizzled_size;
                }

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }

    NV2A_VK_DGROUP_END();
    return layout;
}

struct pgraph_texture_possibly_dirty_struct {
    hwaddr addr, end;
};

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node, void *opaque)
{
    struct pgraph_texture_possibly_dirty_struct *test = opaque;

    TextureBinding *tnode = container_of(node, TextureBinding, node);
    if (tnode->possibly_dirty) {
        return;
    }

    uintptr_t k_tex_addr = tnode->key.texture_vram_offset;
    uintptr_t k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
    bool overlapping = !(test->addr > k_tex_end || k_tex_addr > test->end);

    if (tnode->key.palette_length > 0) {
        uintptr_t k_pal_addr = tnode->key.palette_vram_offset;
        uintptr_t k_pal_end = k_pal_addr + tnode->key.palette_length - 1;
        overlapping |= !(test->addr > k_pal_end || k_pal_addr > test->end);
    }

    tnode->possibly_dirty |= overlapping;
}

void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    struct pgraph_texture_possibly_dirty_struct test = {
        .addr = addr,
        .end = end,
    };

    lru_visit_active(&d->pgraph.vk_renderer_state->texture_cache,
                     mark_textures_possibly_dirty_visitor,
                     &test);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

// Check if any of the pages spanned by the a texture are dirty.
static bool check_texture_possibly_dirty(NV2AState *d,
                                         hwaddr texture_vram_offset,
                                         unsigned int length,
                                         hwaddr palette_vram_offset,
                                         unsigned int palette_length)
{
    bool possibly_dirty = false;
    if (check_texture_dirty(d, texture_vram_offset, length)) {
        possibly_dirty = true;
        pgraph_vk_mark_textures_possibly_dirty(d, texture_vram_offset, length);
    }
    if (palette_length && check_texture_dirty(d, palette_vram_offset,
                                                     palette_length)) {
        possibly_dirty = true;
        pgraph_vk_mark_textures_possibly_dirty(d, palette_vram_offset,
                                            palette_length);
    }
    return possibly_dirty;
}

// FIXME: Make sure we update sampler when data matches. Should we add filtering
// options to the textureshape?
static void upload_texture_image(PGRAPHState *pg, int texture_idx,
                                 TextureBinding *binding)
{
    /* An aux upload runs before the open recording: when draws recorded
     * there sample this binding, the recording is submitted first, or they
     * would see the new content. */
    {
        PGRAPHVkState *rr = pg->vk_renderer_state;
        if (rr->in_command_buffer &&
            binding->submit_time == rr->submit_count) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_TEXTURE_UP);
        }
    }
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &binding->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    g_autofree TextureLayout *layout = NULL;
    if (binding->logo) {
        vkf = kelvin_color_format_vk_map
                  [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8];
        layout = g_malloc0(sizeof(TextureLayout));
        uint8_t *px = g_malloc(segaboot_logo_size);
        memcpy(px, segaboot_logo_data, segaboot_logo_size);
        layout->layers[0].levels[0] = (TextureLevel){
            .width = 2048, .height = 512, .depth = 1,
            .decoded_size = segaboot_logo_size, .decoded_data = px,
        };
    } else {
        layout = get_texture_layout(pg, texture_idx);
    }
    const int num_layers = state->cubemap ? 6 : 1;

    // Calculate decoded texture data size
    size_t texture_data_size = 0;
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            size_t size = layer->levels[level_idx].decoded_size;
            assert(size);
            texture_data_size += size;
        }
    }

    assert(texture_data_size <=
           r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;

    pgraph_vk_wait_for_aux(pg);
    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));

    int num_regions = num_layers * state->levels;
    g_autofree VkBufferImageCopy *regions =
        g_malloc0_n(num_regions, sizeof(VkBufferImageCopy));

    VkBufferImageCopy *region = regions;
    VkDeviceSize buffer_offset = 0;

    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        NV2A_VK_DPRINTF("Layer %d", layer_idx);
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            TextureLevel *level = &layer->levels[level_idx];
            NV2A_VK_DPRINTF(" - Level %d, w=%d h=%d d=%d @ %08" HWADDR_PRIx,
                            level_idx, level->width, level->height,
                            level->depth, buffer_offset);
            memcpy(mapped_memory_ptr + buffer_offset, level->decoded_data,
                   level->decoded_size);
            *region = (VkBufferImageCopy){
                .bufferOffset = buffer_offset,
                .bufferRowLength = 0, // Tightly packed
                .bufferImageHeight = 0,
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .imageSubresource.mipLevel = level_idx,
                .imageSubresource.baseArrayLayer = layer_idx,
                .imageSubresource.layerCount = 1,
                .imageOffset = (VkOffset3D){ 0, 0, 0 },
                .imageExtent =
                    (VkExtent3D){ level->width, level->height, level->depth },
            };
            buffer_offset += level->decoded_size;
            region++;
        }
    }
    assert(buffer_offset <= r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    // FIXME: Use nondraw. Need to fill and copy tex buffer at once
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_STAGING_SRC].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           binding->image, binding->current_layout,
                           num_regions, regions);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_4);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands_async(pg, cmd);
    // Release decoded texture data
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            g_free(layer->levels[level_idx].decoded_data);
        }
    }
}

/* The spec does not promise depth blits: RADV has no BLIT_DST for
 * D32_SFLOAT_S8_UINT and crashes when asked. Asked once per format. */
static bool format_blit_supported(PGRAPHVkState *r, VkFormat format)
{
    for (int i = 0; i < r->blit_formats_n; i++) {
        if (r->blit_formats[i].format == format) {
            return r->blit_formats[i].ok;
        }
    }
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(r->physical_device, format, &props);
    bool ok = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
              (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (r->blit_formats_n < ARRAY_SIZE(r->blit_formats)) {
        r->blit_formats[r->blit_formats_n].format = format;
        r->blit_formats[r->blit_formats_n].ok = ok;
        r->blit_formats_n++;
    }
    if (!ok) {
        fprintf(stderr, "vk: format %d cannot be blitted on this driver,"
                " native depth path off\n", format);
    }
    return ok;
}

/* A zeta surface sampled as a texture is served at native 1x, as on the
 * hardware and GL: a NEAREST blit takes each native texel's sub-sample at
 * its pixel centre (source texel floor((x + 0.5) * scale)) into a native
 * scratch image, kept here per format and size, and only that goes through
 * the copy/pack/upload chain. */
static VkImage znative_get_scratch(PGRAPHState *pg, VkFormat format,
                                   unsigned int width, unsigned int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!format_blit_supported(r, format)) {
        return VK_NULL_HANDLE; /* caller falls back to the scaled path */
    }
    int free_slot = -1;
    for (int i = 0; i < ARRAY_SIZE(r->znative_scratch); i++) {
        if (r->znative_scratch[i].image == VK_NULL_HANDLE) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (r->znative_scratch[i].format == format &&
            r->znative_scratch[i].width == width &&
            r->znative_scratch[i].height == height) {
            return r->znative_scratch[i].image;
        }
    }
    if (free_slot < 0) {
        return VK_NULL_HANDLE; /* caller falls back to the scaled path */
    }
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo aci = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage image;
    VmaAllocation allocation;
    VK_CHECK(vmaCreateImage(r->allocator, &ici, &aci, &image, &allocation,
                            NULL));
    r->znative_scratch[free_slot].image = image;
    r->znative_scratch[free_slot].allocation = allocation;
    r->znative_scratch[free_slot].format = format;
    r->znative_scratch[free_slot].width = width;
    r->znative_scratch[free_slot].height = height;
    return image;
}

static void copy_zeta_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                         TextureBinding *texture)
{
    assert(!surface->color);

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];
    bool use_compute_to_convert_depth_stencil =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool compute_needs_finish = use_compute_to_convert_depth_stencil &&
                                pgraph_vk_compute_needs_finish(r);
    if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);
    /* Before the recording below: growing settles everything. */
    pgraph_vk_buffer_ensure_capacity(pg, BUFFER_COMPUTE_DST,
                                     (VkDeviceSize)scaled_width *
                                         scaled_height * 8);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    /* Native depth: the copy/pack/upload chain below runs at cw x ch
     * (native when the scratch blit is in play, scaled otherwise). */
    VkImage znative_scratch = VK_NULL_HANDLE;
    if (pg->surface_scale_factor > 1) {
        znative_scratch = znative_get_scratch(pg, surface->host_fmt.vk_format,
                                              surface->width, surface->height);
    }
    unsigned int cw = znative_scratch ? surface->width : scaled_width;
    unsigned int ch = znative_scratch ? surface->height : scaled_height;

    size_t copied_image_size =
        cw * ch * surface->host_fmt.host_bytes_per_pixel;
    size_t stencil_buffer_offset = 0;
    size_t stencil_buffer_size = 0;

    int num_regions = 0;
    VkBufferImageCopy regions[2];
    regions[num_regions++] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0, // Tightly packed
        .bufferImageHeight = 0, // Tightly packed
        .imageSubresource.aspectMask = surface->color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){0, 0, 0},
        .imageExtent = (VkExtent3D){cw, ch, 1},
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        stencil_buffer_offset =
            ROUND_UP(cw * ch * 4,
                     r->device_props.limits.minStorageBufferOffsetAlignment);
        stencil_buffer_size = cw * ch;
        /* The depth plane copies as 32-bit texels whatever the image texel
         * size (8 bytes for D32_SFLOAT_S8_UINT, the AMD fallback): the
         * copy ends with the stencil plane. Summing the image texel size
         * and the stencil plane claimed 9 bytes a pixel against a buffer
         * sized for 8 (House of the Dead 3 on AMD Windows, assertion). */
        copied_image_size = stencil_buffer_offset + stencil_buffer_size;

        regions[num_regions++] = (VkBufferImageCopy){
            .bufferOffset = stencil_buffer_offset,
            .bufferRowLength = 0, // Tightly packed
            .bufferImageHeight = 0, // Tightly packed
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.mipLevel = 0,
            .imageSubresource.baseArrayLayer = 0,
            .imageSubresource.layerCount = 1,
            .imageOffset = (VkOffset3D){0, 0, 0},
            .imageExtent = (VkExtent3D){cw, ch, 1},
        };
    }
    StorageBuffer *dst_storage_buffer = &r->storage_buffers[BUFFER_COMPUTE_DST];
    assert(dst_storage_buffer->buffer_size >= copied_image_size);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImage copy_src_image = surface->image;
    if (znative_scratch) {
        pgraph_vk_transition_image_layout(
            pg, cmd, znative_scratch, surface->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blits[2];
        int nb = 0;
        VkImageAspectFlags aspects[2] = { VK_IMAGE_ASPECT_DEPTH_BIT,
                                          VK_IMAGE_ASPECT_STENCIL_BIT };
        for (int a = 0; a < 2; a++) {
            if (!(surface->host_fmt.aspect & aspects[a])) {
                continue;
            }
            blits[nb++] = (VkImageBlit){
                .srcSubresource.aspectMask = aspects[a],
                .srcSubresource.mipLevel = 0,
                .srcSubresource.baseArrayLayer = 0,
                .srcSubresource.layerCount = 1,
                .srcOffsets[0] = (VkOffset3D){ 0, 0, 0 },
                .srcOffsets[1] = (VkOffset3D){ scaled_width, scaled_height, 1 },
                .dstSubresource.aspectMask = aspects[a],
                .dstSubresource.mipLevel = 0,
                .dstSubresource.baseArrayLayer = 0,
                .dstSubresource.layerCount = 1,
                .dstOffsets[0] = (VkOffset3D){ 0, 0, 0 },
                .dstOffsets[1] = (VkOffset3D){ cw, ch, 1 },
            };
        }
        /* Depth/stencil blits must be NEAREST and same-format (spec):
         * exactly the point-downsample wanted. */
        vkCmdBlitImage(cmd, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       znative_scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       nb, blits, VK_FILTER_NEAREST);
        pgraph_vk_transition_image_layout(
            pg, cmd, znative_scratch, surface->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        copy_src_image = znative_scratch;
    }
    vkCmdCopyImageToBuffer(
        cmd, copy_src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_storage_buffer->buffer,
        num_regions, regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkBuffer texture_source_buffer;

    if (use_compute_to_convert_depth_stencil) {
        size_t packed_image_size = cw * ch * 4;

        VkBufferMemoryBarrier pre_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier pre_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil_dims(
            pg, surface, cmd,
            r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, cw, ch, cw, ch);

        VkBufferMemoryBarrier post_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = packed_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_dst_barrier, 0, NULL);

        texture_source_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;
    } else {
        VkBufferMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = dst_storage_buffer->buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                             1, &barrier, 0, NULL);

        texture_source_buffer = dst_storage_buffer->buffer;
    }

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    regions[0] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ cw, ch, 1 },
    };
    vkCmdCopyBufferToImage(
        cmd, texture_source_buffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions);

    VkBufferMemoryBarrier post_copy_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = texture_source_buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

/* Compatible render surfaces are sampled in place, not copied per bind:
 * the transition to shader-read is the snapshot point, and the one back
 * at the next write orders the reads first (WAR). Feedback (the bound
 * render target) keeps the copy, as upstream does. */
static void s2t_borrow_sync(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (surface->in_shader_read) {
        return;
    }
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pgraph_vk_end_nondraw_commands(pg, cmd);
    surface->in_shader_read = true;
    surface->borrow_submit = r->submit_count;
}

/* Leaving shader-read (read-back, upload, blit, retarget): units sampling
 * the image in place bind again, so s2t_borrow_sync records the transition
 * before the next draw. */
void pgraph_vk_surface_left_shader_read(PGRAPHState *pg,
                                        SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    surface->in_shader_read = false;
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *b = r->texture_bindings[i];
        if (b && b->s2t_borrowed && b->image == surface->image) {
            pg->texture_dirty[i] = true;
        }
    }
}

struct DropBorrowedCtx {
    PGRAPHState *pg;
    PGRAPHVkState *r;
    VkImage image;
};

static void drop_borrowed_visitor(Lru *lru, LruNode *node, void *opaque)
{
    struct DropBorrowedCtx *ctx = opaque;
    TextureBinding *snode = container_of(node, TextureBinding, node);

    if (!snode->s2t_borrowed || snode->image != ctx->image) {
        return;
    }
    texture_cache_release_node_resources(ctx->r, snode);
    /* A unit still bound to it binds again at the next draw. */
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (ctx->r->texture_bindings[i] == snode) {
            ctx->r->texture_bindings[i] = NULL;
            ctx->pg->texture_dirty[i] = true;
        }
    }
}

void pgraph_vk_texture_drop_borrowed(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHState *pg = &d->pgraph;
    struct DropBorrowedCtx ctx = {
        .pg = pg, .r = pg->vk_renderer_state, .image = surface->image
    };

    lru_visit_active(&ctx.r->texture_cache, drop_borrowed_visitor, &ctx);
}

/* One surface into one layer of a texture in TRANSFER_DST: the surface
 * leaves its layout for the copy and returns as an attachment. */
static void record_surface_copy(PGRAPHState *pg, VkCommandBuffer cmd,
                                SurfaceBinding *surface,
                                TextureBinding *texture, int layer)
{
    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        pgraph_vk_surface_base_layout(surface),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    pgraph_vk_surface_left_shader_read(pg, surface);

    VkImageCopy region = {
        .srcSubresource.aspectMask = surface->host_fmt.aspect,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = surface->host_fmt.aspect,
        .dstSubresource.baseArrayLayer = layer,
        .dstSubresource.layerCount = 1,
        .extent.width = surface->width,
        .extent.height = surface->height,
        .extent.depth = 1,
    };
    pgraph_apply_scaling_factor(pg, &region.extent.width,
                                &region.extent.height);
    vkCmdCopyImage(cmd, surface->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                   texture->current_layout, 1, &region);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

// FIXME: Should be able to skip the copy and sample the original surface image
static void copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture)
{
    if (!surface->color) {
        copy_zeta_surface_to_texture(pg, surface, texture);
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    record_surface_copy(pg, cmd, surface, texture, 0);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

/* Cube faces: six per-face render surfaces feed one cubemap, which
 * the hardware samples in place: one GPU batch of layer copies with
 * copy_surface_to_texture's snapshot semantics, not six trips via RAM. */
static void copy_cube_faces_to_texture(PGRAPHState *pg,
                                       SurfaceBinding *faces[6],
                                       TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    int max_draw_time = 0;
    for (int f = 0; f < 6; f++) {
        record_surface_copy(pg, cmd, faces[f], texture, f);
        if (faces[f]->draw_time > max_draw_time) {
            max_draw_time = faces[f]->draw_time;
        }
    }

    pgraph_vk_transition_image_layout(
        pg, cmd, texture->image, vkf.vk_format, texture->current_layout,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = max_draw_time;
}

static unsigned int vk_format_texel_size(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:                return 1;
    case VK_FORMAT_R8G8_UNORM:              return 2;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:   return 2;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:     return 2;
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:   return 2;
    case VK_FORMAT_R16_UNORM:               return 2;
    case VK_FORMAT_R8G8B8_SNORM:            return 3;
    case VK_FORMAT_B8G8R8A8_UNORM:          return 4;
    case VK_FORMAT_R8G8B8A8_UNORM:          return 4;
    case VK_FORMAT_R32_UINT:                return 4;
    default:                                return 0;
    }
}

static bool check_surface_to_texture_compatiblity(const SurfaceBinding *surface,
                                                  const TextureShape *shape)
{
    if ((!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height ||
        shape->cubemap ||
        shape->levels > 1) {
        return false;
    }

    if (!surface->color) {
        return true;
    }

    VkColorFormatInfo tex_vkf = kelvin_color_format_vk_map[shape->color_format];
    return tex_vkf.vk_format &&
           surface->host_fmt.host_bytes_per_pixel == vk_format_texel_size(tex_vkf.vk_format);
}

/* Texture images retired by the CoW rule below or by cache eviction are
 * kept for reuse instead of freed. A feedback pass retires the same few
 * images many times per frame, and at high scales each one is a large
 * dedicated device allocation. Semantics of a fresh image: contents
 * undefined, layout UNDEFINED. The pool only keeps what the device can
 * spare: over its heap's budget (VK_EXT_memory_budget, or VMA's estimate
 * without it), a retired image gives the pool's oldest ones of that heap
 * back first, then itself. */
#define IMAGE_POOL_MAX_BYTES ((size_t)1536 << 20)

static bool image_ci_equal(const VkImageCreateInfo *a,
                           const VkImageCreateInfo *b)
{
    return a->imageType == b->imageType && a->format == b->format &&
           a->extent.width == b->extent.width &&
           a->extent.height == b->extent.height &&
           a->extent.depth == b->extent.depth &&
           a->mipLevels == b->mipLevels && a->arrayLayers == b->arrayLayers &&
           a->samples == b->samples && a->tiling == b->tiling &&
           a->usage == b->usage && a->flags == b->flags &&
           a->sharingMode == b->sharingMode;
}

static void image_pool_drop(PGRAPHVkState *r, int i)
{
    struct XemuVkPooledImage *p = &r->image_pool[i];
    vmaDestroyImage(r->allocator, p->image, p->allocation);
    r->image_pool_bytes -= p->bytes;
    r->image_pool[i] = r->image_pool[--r->image_pool_n];
}

static bool image_pool_heap_over_budget(PGRAPHVkState *r, uint32_t heap)
{
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];

    vmaGetHeapBudgets(r->allocator, budgets);
    return budgets[heap].usage > budgets[heap].budget;
}

bool pgraph_vk_image_pool_put(PGRAPHVkState *r, VkImage image,
                              VmaAllocation allocation,
                              const VkImageCreateInfo *ci)
{
    if (allocation == VK_NULL_HANDLE) {
        return false;
    }
    VmaAllocationInfo ai;
    vmaGetAllocationInfo(r->allocator, allocation, &ai);
    if (ai.size > IMAGE_POOL_MAX_BYTES / 4) {
        return false;
    }
    const VkPhysicalDeviceMemoryProperties *props;
    vmaGetMemoryProperties(r->allocator, &props);
    uint32_t heap = props->memoryTypes[ai.memoryType].heapIndex;
    while (image_pool_heap_over_budget(r, heap)) {
        int oldest = -1;
        for (int i = 0; i < r->image_pool_n; i++) {
            if (r->image_pool[i].heap == heap &&
                (oldest < 0 ||
                 r->image_pool[i].seq < r->image_pool[oldest].seq)) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            return false;
        }
        image_pool_drop(r, oldest);
    }
    while (r->image_pool_n > 0 &&
           (r->image_pool_n == ARRAY_SIZE(r->image_pool) ||
            r->image_pool_bytes + ai.size > IMAGE_POOL_MAX_BYTES)) {
        int oldest = 0;
        for (int i = 1; i < r->image_pool_n; i++) {
            if (r->image_pool[i].seq < r->image_pool[oldest].seq) {
                oldest = i;
            }
        }
        image_pool_drop(r, oldest);
    }
    struct XemuVkPooledImage *p = &r->image_pool[r->image_pool_n++];
    p->image = image;
    p->allocation = allocation;
    p->ci = *ci;
    p->bytes = ai.size;
    p->heap = heap;
    p->seq = ++r->image_pool_seq;
    r->image_pool_bytes += ai.size;
    return true;
}

bool pgraph_vk_image_pool_get(PGRAPHVkState *r, const VkImageCreateInfo *ci,
                              VkImage *image, VmaAllocation *allocation)
{
    for (int i = 0; i < r->image_pool_n; i++) {
        if (image_ci_equal(&r->image_pool[i].ci, ci)) {
            *image = r->image_pool[i].image;
            *allocation = r->image_pool[i].allocation;
            r->image_pool_bytes -= r->image_pool[i].bytes;
            r->image_pool[i] = r->image_pool[--r->image_pool_n];
            return true;
        }
    }
    return false;
}

void pgraph_vk_image_pool_flush(PGRAPHVkState *r)
{
    while (r->image_pool_n > 0) {
        image_pool_drop(r, r->image_pool_n - 1);
    }
}

/* The sampled view of a texture node's image, from its creation info; the
 * view type follows the guest shape, which a doge logo node keeps. */
static void create_texture_view(PGRAPHVkState *r, TextureBinding *snode,
                                const VkColorFormatInfo *vkf)
{
    const TextureShape *state = &snode->key.state;
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = snode->image,
        .viewType = state->cubemap ?
            VK_IMAGE_VIEW_TYPE_CUBE :
            dimensionality_to_vk_image_view_type[state->dimensionality],
        .format = snode->image_ci.format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = snode->image_ci.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = snode->image_ci.arrayLayers,
        .components = vkf->component_map,
    };
    VK_CHECK(vkCreateImageView(r->device, &vci, NULL, &snode->image_view));
}

/* Copy-on-write: while a recorded reference is not retired (the open
 * recording, or a submission past the settled mark), an update moves the
 * image to the trash, where recorded descriptors keep their content, and
 * goes on in a fresh one. open_recording_counts: an aux upload runs ahead
 * of the open recording, so a reference there counts; a surface copy,
 * recorded behind those draws and ordered by its barrier, refills in place. */
static void texture_cow_if_referenced(PGRAPHState *pg, TextureBinding *snode,
                                      bool open_recording_counts)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (snode->image == VK_NULL_HANDLE) {
        return;
    }
    bool in_open_recording =
        r->in_command_buffer && snode->submit_time == r->submit_count;
    bool referenced =
        (snode->submit_time > r->settled_submit && !in_open_recording) ||
        (open_recording_counts && in_open_recording);
    if (!referenced) {
        return;
    }

    struct XemuVkTrashEntry e = { .image_view = snode->image_view,
                                  .image = snode->image,
                                  .allocation = snode->allocation,
                                  .image_ci = snode->image_ci,
                                  .pool_ok = true };
    pgraph_vk_trash_push(r, &e);

    /* The new image is the retired one's twin: its own creation info, not
     * one rebuilt from the guest shape (a doge logo node is larger). */
    TextureShape *state = &snode->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map
        [snode->logo ? NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8 :
                       state->color_format];
    VkImageCreateInfo ici = snode->image_ci;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    if (!pgraph_vk_image_pool_get(r, &ici, &snode->image,
                                  &snode->allocation)) {
        VK_CHECK(vmaCreateImage(r->allocator, &ici, &aci, &snode->image,
                                &snode->allocation, NULL));
    }
    snode->image_ci = ici;
    create_texture_view(r, snode, &vkf);

    snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    snode->seq = ++r->texture_binding_seq;
}

static void create_dummy_texture(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = 16,
        .extent.height = 16,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkImage texture_image;
    VmaAllocation texture_allocation;

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &texture_image,
                            &texture_allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R },
    };
    VkImageView texture_image_view;
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &texture_image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VkSampler texture_sampler;
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &texture_sampler));

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;
    size_t texture_data_size =
        image_create_info.extent.width * image_create_info.extent.height;

    pgraph_vk_wait_for_aux(pg);
    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));
    memset(mapped_memory_ptr, 0xff, texture_data_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, texture_image, VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ image_create_info.extent.width,
                                     image_create_info.extent.height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, texture_image,
                                      VK_FORMAT_R8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands_async(pg, cmd);

    r->dummy_texture = (TextureBinding){
        .key.scale = 1.0,
        .image = texture_image,
        .current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .allocation = texture_allocation,
        .image_view = texture_image_view,
        .sampler = texture_sampler,
    };
}

static void destroy_dummy_texture(PGRAPHVkState *r)
{
    texture_cache_release_node_resources(r, &r->dummy_texture);
}

static void set_texture_label(PGRAPHState *pg, TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
        texture->key.texture_vram_offset, texture->key.state.color_format,
        texture->key.state.width, texture->key.state.height,
        texture->key.state.depth, texture->key.state.levels);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)texture->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    /* In-place sampling: borrowed images have no allocation of their own. */
    if (texture->allocation != VK_NULL_HANDLE) {
        vmaSetAllocationName(r->allocator, texture->allocation, label);
    }
}

static bool is_linear_filter_supported_for_format(PGRAPHVkState *r,
                                                  int kelvin_format)
{
    return r->texture_format_properties[kelvin_format].optimalTilingFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}


static VkSampler create_sampler_for_state(PGRAPHState *pg,
                                          const TextureShape *state,
                                          uint32_t filter, uint32_t address,
                                          uint32_t border_color_pack32,
                                          uint32_t max_anisotropy);

static VkSampler sampler_cache_get(PGRAPHState *pg, const TextureShape *state,
                                   uint32_t filter, uint32_t address,
                                   uint32_t border_color_pack32,
                                   uint32_t max_anisotropy)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* memset first: an initializer may leave the padding unspecified, and
     * the memcmp below compares every byte. */
    VkSamplerCacheKey k;
    memset(&k, 0, sizeof(k));
    k.filter = filter;
    k.address = address;
    k.border_color = border_color_pack32;
    k.max_anisotropy = max_anisotropy;
    k.color_format = state->color_format;
    k.dimensionality = state->dimensionality;
    k.levels = state->levels;
    k.min_mip = state->min_mipmap_level;
    k.max_mip = state->max_mipmap_level;
    unsigned h = (unsigned)(fast_hash((const uint8_t *)&k, sizeof(k)) %
                            VK_SAMPLER_CACHE_SIZE);
    for (unsigned i = 0; i < VK_SAMPLER_CACHE_SIZE; i++) {
        unsigned s = (h + i) % VK_SAMPLER_CACHE_SIZE;
        if (!r->sampler_cache[s].used) {
            r->sampler_cache[s].used = true;
            r->sampler_cache[s].key = k;
            r->sampler_cache[s].sampler = create_sampler_for_state(
                pg, state, filter, address, border_color_pack32,
                max_anisotropy);
            r->sampler_cache_n++;
            return r->sampler_cache[s].sampler;
        }
        if (memcmp(&r->sampler_cache[s].key, &k, sizeof(k)) == 0) {
            return r->sampler_cache[s].sampler;
        }
    }
    /* Table full (not expected): a one-off sampler would leak, so the home
     * slot's sampler is reused, degraded, with one warning. */
    static bool warned;
    if (!warned) {
        warned = true;
        fprintf(stderr, "nv2a: vk sampler cache full (%u)\n",
                r->sampler_cache_n);
    }
    return r->sampler_cache[h].sampler;
}

static void sampler_cache_destroy(PGRAPHVkState *r)
{
    for (unsigned i = 0; i < VK_SAMPLER_CACHE_SIZE; i++) {
        if (r->sampler_cache[i].used) {
            vkDestroySampler(r->device, r->sampler_cache[i].sampler, NULL);
            r->sampler_cache[i].used = false;
            r->sampler_cache[i].sampler = VK_NULL_HANDLE;
        }
    }
    r->sampler_cache_n = 0;
}

/* Resolve the unit's sampler for the texture just bound. Both exits of
 * create_texture must call it, the cache hit too, or the unit keeps the
 * sampler of the last texture created on it. */
static void resolve_unit_sampler(PGRAPHState *pg, int texture_idx,
                                 const TextureShape *state, uint32_t filter,
                                 uint32_t address,
                                 uint32_t border_color_pack32,
                                 uint32_t max_anisotropy)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* Sampler state (filter, address, border, anisotropy) stays out of the
     * image key, as on GL: one image serves every sampler it is read with. */
    r->texture_unit_sampler[texture_idx] = sampler_cache_get(
        pg, state, filter, address, border_color_pack32, max_anisotropy);
}

/* Every input comes from the shape and the four sampler registers. */
static VkSampler create_sampler_for_state(PGRAPHState *pg,
                                          const TextureShape *state_in,
                                          uint32_t filter, uint32_t address,
                                          uint32_t border_color_pack32,
                                          uint32_t max_anisotropy)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    const TextureShape state = *state_in;
    BasicColorFormatInfo f_basic =
        kelvin_color_format_info_map[state.color_format];
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state.color_format];
    uint32_t mip_levels = f_basic.linear ? 1 : state.levels;

    void *sampler_next_struct = NULL;

    VkSamplerCustomBorderColorCreateInfoEXT custom_border_color_create_info;
    VkBorderColor vk_border_color;

    bool is_integer_type = vkf.vk_format == VK_FORMAT_R32_UINT;

    if (r->custom_border_color_extension_enabled) {
        vk_border_color = is_integer_type ? VK_BORDER_COLOR_INT_CUSTOM_EXT :
                                            VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        custom_border_color_create_info =
            (VkSamplerCustomBorderColorCreateInfoEXT){
                .sType =
                    VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                .format = vkf.vk_format,
                .pNext = sampler_next_struct
            };
        if (is_integer_type) {
            float rgba[4];
            pgraph_argb_pack32_to_rgba_float(border_color_pack32, rgba);
            for (int i = 0; i < 4; i++) {
                custom_border_color_create_info.customBorderColor.uint32[i] =
                    (uint32_t)((double)rgba[i] * (double)0xffffffff);
            }
        } else {
            pgraph_argb_pack32_to_rgba_float(
                border_color_pack32,
                custom_border_color_create_info.customBorderColor.float32);
        }
        sampler_next_struct = &custom_border_color_create_info;
    } else {
        // FIXME: Handle custom color in shader
        if (is_integer_type) {
            vk_border_color = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0x00000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0xff000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        } else {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        }
    }

    if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

    VkFilter vk_min_filter, vk_mag_filter;
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    assert(mag_filter < ARRAY_SIZE(pgraph_texture_mag_filter_vk_map));

    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    assert(min_filter < ARRAY_SIZE(pgraph_texture_min_filter_vk_map));

    if (is_linear_filter_supported_for_format(r, state.color_format)) {
        vk_mag_filter = pgraph_texture_min_filter_vk_map[mag_filter];
        vk_min_filter = pgraph_texture_min_filter_vk_map[min_filter];
    } else {
        vk_mag_filter = vk_min_filter = VK_FILTER_NEAREST;
    }

    bool mipmap_en =
        !f_basic.linear &&
        !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);

    bool mipmap_nearest =
        f_basic.linear || mip_levels == 1 ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

    float lod_bias = pgraph_convert_lod_bias_to_float(
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS));
    if (lod_bias > r->device_props.limits.maxSamplerLodBias) {
        lod_bias = r->device_props.limits.maxSamplerLodBias;
    } else if (lod_bias < -r->device_props.limits.maxSamplerLodBias) {
        lod_bias = -r->device_props.limits.maxSamplerLodBias;
    }
    uint32_t sampler_max_anisotropy =
        MIN(r->device_props.limits.maxSamplerAnisotropy, max_anisotropy);

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_mag_filter,
        .minFilter = vk_min_filter,
        .addressModeU = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .addressModeV = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .addressModeW = (state.dimensionality > 2) ? lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)) : 0,
        .anisotropyEnable =
            r->enabled_physical_device_features.samplerAnisotropy &&
            sampler_max_anisotropy > 1,
        .maxAnisotropy = sampler_max_anisotropy,
        .borderColor = vk_border_color,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = mipmap_nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST :
                                       VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .minLod = mipmap_en ? MIN(state.min_mipmap_level, state.levels - 1) : 0.0,
        .maxLod = mipmap_en ? MIN(state.max_mipmap_level, state.levels - 1) : 0.0,
        .mipLodBias = lod_bias,
        .pNext = sampler_next_struct,
    };

    VkSampler sampler;
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL, &sampler));
    return sampler;
}

static void create_texture(PGRAPHState *pg, int texture_idx)
{
    NV2A_VK_DGROUP_BEGIN("Creating texture %d", texture_idx);

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx); // FIXME: Check for pad issues
    BasicColorFormatInfo f_basic = kelvin_color_format_info_map[state.color_format];

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    size_t texture_length = pgraph_get_texture_length(pg, &state);
    hwaddr texture_palette_vram_offset = 0;
    size_t texture_palette_data_size = 0;

    uint32_t filter =
        pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    uint32_t address =
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    uint32_t border_color_pack32 =
        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + texture_idx * 4);
    bool is_indexed = (state.color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
    uint32_t max_anisotropy =
        1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + texture_idx*4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = texture_vram_offset;
    key.texture_length = texture_length;
    if (is_indexed) {
        texture_palette_vram_offset =
            pgraph_get_texture_palette_phys_addr_length(
                pg, texture_idx, &texture_palette_data_size);
        key.palette_vram_offset = texture_palette_vram_offset;
        key.palette_length = texture_palette_data_size;
    }
    key.scale = 1;
    /* Split samplers: the sampler fields stay zero in the key; the
     * unit's sampler comes from the sampler cache at the end. */

    bool possibly_dirty = false;
    bool possibly_dirty_checked = false;
    bool surface_to_texture = false;
    bool surface_to_texture_cube = false;
    SurfaceBinding *cube_faces[6] = { 0 };

    // Check active surfaces to see if this texture was a render target
    SurfaceBinding *surface = pgraph_vk_surface_get(d, texture_vram_offset);
    if (surface && state.levels == 1) {
        surface_to_texture =
            check_surface_to_texture_compatiblity(surface, &state);

        if (!surface_to_texture && surface->color) {
            trace_nv2a_pgraph_surface_texture_compat_failed(
                surface->shape.color_format,
                state.color_format);
        }

        if (surface_to_texture && surface->upload_pending) {
            pgraph_vk_upload_surface_data(d, surface, false);
        }
    }

    /* A cubemap whose six faces are live, compatible color surfaces takes
     * them by GPU copy. */
    if (!surface_to_texture && state.cubemap && state.levels == 1) {
        size_t face_len = texture_length / 6;
        TextureShape flat = state;
        flat.cubemap = false;
        bool all = true;
        for (int f = 0; f < 6; f++) {
            SurfaceBinding *fs = pgraph_vk_surface_get(
                d, texture_vram_offset + f * face_len);
            if (!fs || !fs->color ||
                !check_surface_to_texture_compatiblity(fs, &flat)) {
                all = false;
                break;
            }
            cube_faces[f] = fs;
        }
        if (all) {
            surface_to_texture_cube = true;
            for (int f = 0; f < 6; f++) {
                if (cube_faces[f]->upload_pending) {
                    pgraph_vk_upload_surface_data(d, cube_faces[f],
                                                  false);
                }
            }
        }
    }

    if (!surface_to_texture && !surface_to_texture_cube) {
        // FIXME: Restructure to support rendering surfaces to cubemap faces

        // Writeback any surfaces which this texture may index
        pgraph_vk_download_surfaces_in_range_if_dirty(
            pg, texture_vram_offset, texture_length);
    }

    /* Native depth: a sampled zeta surface is served at native 1x:
     * unscaled key and image, texScale 1 for linear formats, as on GL. */
    /* The native depth image exists only if the upload path will have a
     * scratch image to shrink into (a blittable format and a free slot):
     * taking the slot here is what makes the two decisions agree, since a
     * slot, once taken, stays. */
    bool znative_zeta = surface_to_texture && !surface->color &&
                        pg->surface_scale_factor > 1 &&
                        znative_get_scratch(pg, surface->host_fmt.vk_format,
                                            surface->width, surface->height) !=
                            VK_NULL_HANDLE;
    if ((surface_to_texture || surface_to_texture_cube) &&
        pg->surface_scale_factor > 1 && !znative_zeta) {
        key.scale = pg->surface_scale_factor;
    }

    uint64_t key_hash = fast_hash((void*)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    bool binding_found = snode->image != VK_NULL_HANDLE;

    /* In-place sampling: a borrowed node holds only while it aliases the
     * live surface here, not bound as the color target; else rebuilt. */
    if (binding_found && snode->s2t_borrowed &&
        (!surface_to_texture || snode->image != surface->image ||
         surface == r->color_binding)) {
        texture_cache_release_node_resources(r, snode);
        binding_found = false;
    }

    if (binding_found) {
        NV2A_VK_DPRINTF("Cache hit");
        r->texture_bindings[texture_idx] = snode;
        possibly_dirty |= snode->possibly_dirty;
    } else {
        possibly_dirty = true;
    }

    if (!surface_to_texture && !surface_to_texture_cube &&
        !possibly_dirty_checked) {
        possibly_dirty |= check_texture_possibly_dirty(
            d, texture_vram_offset, texture_length, texture_palette_vram_offset,
            texture_palette_data_size);
    }

    // Calculate hash of texture data, if necessary
    void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
    void *palette_data = (char*)d->vram_ptr + texture_palette_vram_offset;

    /* HEURISTIC: the content hash, the costly part of revalidation, runs at
     * most once per frame per binding: bind_textures stamps
     * validation_frame after this call (-1 on a fresh node, which always
     * hashes). The sampler is still resolved on every bind below. */
    uint64_t content_hash = 0;
    if (!surface_to_texture && !surface_to_texture_cube && possibly_dirty &&
        snode->validation_frame != pg->frame_time) {
        content_hash = fast_hash(texture_data, texture_length);
        if (is_indexed) {
            content_hash ^= fast_hash(palette_data, texture_palette_data_size);
        }
    }

    /* A doge logo node holds only while the guest data is the logo's: new
     * data makes it an ordinary texture again, rebuilt below. */
    if (binding_found && snode->logo && content_hash &&
        content_hash != snode->hash) {
        /* A new seq makes every unit bind it again. */
        texture_cache_release_node_resources(r, snode);
        snode->seq = ++r->texture_binding_seq;
        binding_found = false;
        possibly_dirty = true;
    }

    if (binding_found) {
        if (surface_to_texture_cube) {
            unsigned int maxdt = 0;
            for (int f = 0; f < 6; f++) {
                if ((unsigned int)cube_faces[f]->draw_time > maxdt) {
                    maxdt = cube_faces[f]->draw_time;
                }
            }
            if (maxdt != snode->draw_time) {
                texture_cow_if_referenced(pg, snode, false);
                copy_cube_faces_to_texture(pg, cube_faces, snode);
            }
        } else if (surface_to_texture && snode->s2t_borrowed) {
            /* Live alias, nothing to copy: record the barrier again after
             * new draws, or if the image left shader-read without one. */
            if (!surface->in_shader_read ||
                surface->draw_time != snode->draw_time) {
                s2t_borrow_sync(pg, surface);
                snode->draw_time = surface->draw_time;
            }
        } else if (surface_to_texture) {
            // FIXME: Add draw time tracking
            if (surface->draw_time != snode->draw_time) {
                texture_cow_if_referenced(pg, snode, false);
                copy_surface_to_texture(pg, surface, snode);
            }
        } else if (snode->validation_frame != pg->frame_time) {
            if (possibly_dirty && content_hash != snode->hash) {
                texture_cow_if_referenced(pg, snode, true);
                upload_texture_image(pg, texture_idx, snode);
                snode->hash = content_hash;
            }
            /* Validated for this frame: dirty pages found later in its RAM
             * mark it again (mark_textures_possibly_dirty). */
            snode->possibly_dirty = false;
        }

        resolve_unit_sampler(pg, texture_idx, &state, filter, address,
                             border_color_pack32, max_anisotropy);

        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");

    memcpy(&snode->key, &key, sizeof(key));
    snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    snode->possibly_dirty = false;
    snode->hash = content_hash;

    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state.color_format];
    assert(vkf.vk_format != 0);
    assert(0 < state.dimensionality);
    assert(state.dimensionality < ARRAY_SIZE(dimensionality_to_vk_image_type));
    assert(state.dimensionality <
           ARRAY_SIZE(dimensionality_to_vk_image_view_type));

    /* DOGE (not upstream): SEGABOOT's "Chihiro" logo becomes the embedded
     * image, the same picture at four times the texel count (see the GL
     * renderer). The cache key keeps the guest's shape; only this image
     * and its upload change. */
    snode->logo = false;
    if (xemu_doge_mode() && content_hash == 0xec2bbfd4787e2b7dULL &&
        !surface_to_texture &&
        state.dimensionality == 2 && !state.cubemap &&
        state.width == 512 && state.height == 128) {
        snode->logo = true;
        state.color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8;
        state.width = 2048;
        state.height = 512;
        state.levels = 1;
        vkf = kelvin_color_format_vk_map[state.color_format];
        fprintf(stderr, "nv2a: SEGABOOT logo replaced (vk)\n");
    }

    /* In-place sampling: in place for the class the copy serves, minus
     * feedback, with the surface's exact VkFormat (no MUTABLE_FORMAT). */
    bool s2t_borrow =
        surface_to_texture &&
        surface != r->color_binding &&
        state.dimensionality == 2 && !state.cubemap &&
        kelvin_color_format_vk_map[state.color_format].vk_format ==
            surface->host_fmt.vk_format;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimensionality_to_vk_image_type[state.dimensionality],
        .extent.width = state.width, // FIXME: Use adjusted size?
        .extent.height = state.height,
        .extent.depth = state.depth,
        .mipLevels = f_basic.linear ? 1 : state.levels,
        .arrayLayers = state.cubemap ? 6 : 1,
        .format = vkf.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = (state.cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
    };

    if ((surface_to_texture || surface_to_texture_cube) && !znative_zeta) {
        pgraph_apply_scaling_factor(pg, &image_create_info.extent.width,
                                        &image_create_info.extent.height);
    }

    if (s2t_borrow) {
        snode->image = surface->image;
        snode->allocation = VK_NULL_HANDLE;
        snode->s2t_borrowed = true;
        snode->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        VmaAllocationCreateInfo alloc_create_info = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };

        if (!pgraph_vk_image_pool_get(r, &image_create_info, &snode->image,
                                      &snode->allocation)) {
            VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                                    &alloc_create_info, &snode->image,
                                    &snode->allocation, NULL));
        }
    }
    snode->image_ci = image_create_info;
    create_texture_view(r, snode, &vkf);

    snode->sampler = create_sampler_for_state(pg, &state, filter, address,
                                              border_color_pack32,
                                              max_anisotropy);

    set_texture_label(pg, snode);

    r->texture_bindings[texture_idx] = snode;

    if (surface_to_texture_cube) {
        copy_cube_faces_to_texture(pg, cube_faces, snode);
    } else if (s2t_borrow) {
        s2t_borrow_sync(pg, surface);
        snode->draw_time = surface->draw_time;
    } else if (surface_to_texture) {
        copy_surface_to_texture(pg, surface, snode);
    } else {
        upload_texture_image(pg, texture_idx, snode);
        snode->draw_time = 0;
    }

    resolve_unit_sampler(pg, texture_idx, &state, filter, address,
                         border_color_pack32, max_anisotropy);

    NV2A_VK_DGROUP_END();
}

static bool check_textures_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!r->texture_bindings[i] || pg->texture_dirty[i]) {
            return true;
        }
        /* A binding whose RAM may have changed (possibly_dirty) revalidates
         * at most once per frame. HEURISTIC: a RAM change after its first
         * bind in a frame shows from the next frame on. */
        if (r->texture_bindings[i]->possibly_dirty &&
            r->texture_bindings[i]->validation_frame != pg->frame_time) {
            return true;
        }
    }
    return false;
}

static void update_timestamps(PGRAPHVkState *r)
{
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i]) {
            r->texture_bindings[i]->submit_time = r->submit_count;
        }
    }
}

void pgraph_vk_bind_textures(NV2AState *d)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Check for modifications on bind fastpath (CPU hook)
    // FIXME: Mark textures that are sourced from surfaces so we can track them

    r->texture_bindings_changed = false;

    if (!check_textures_dirty(pg)) {
        NV2A_VK_DPRINTF("Not dirty");
        NV2A_VK_DGROUP_END();
        update_timestamps(r);
        return;
    }

    /* Games re-send identical texture state before every draw: report a
     * change only if the bound instance differs. Node pointers and Vulkan
     * handles are both recycled, so the creation sequence id is compared. */
    TextureBinding *old_bindings[NV2A_MAX_TEXTURES];
    uint64_t old_seqs[NV2A_MAX_TEXTURES];
    VkSampler old_samplers[NV2A_MAX_TEXTURES];
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        old_bindings[i] = r->texture_bindings[i];
        old_seqs[i] = r->texture_bindings[i] ? r->texture_bindings[i]->seq : 0;
        /* Split samplers: a filter/wrap change alone keeps the image
         * node (same seq) but must still rewrite the descriptors. */
        old_samplers[i] = r->texture_unit_sampler[i];
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            r->texture_bindings[i] = &r->dummy_texture;
            r->texture_unit_sampler[i] = r->dummy_texture.sampler;
            /* Cleared so a unit left off does not rerun this loop. */
            pg->texture_dirty[i] = false;
            continue;
        }

        create_texture(pg, i);
        r->texture_bindings[i]->validation_frame = pg->frame_time;

        pg->texture_dirty[i] = false; // FIXME: Move to renderer?
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (r->texture_bindings[i] != old_bindings[i] ||
            r->texture_bindings[i]->seq != old_seqs[i] ||
            r->texture_unit_sampler[i] != old_samplers[i]) {
            r->texture_bindings_changed = true;
            break;
        }
    }
    update_timestamps(r);
    NV2A_VK_DGROUP_END();
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
    snode->image_view = VK_NULL_HANDLE;
    snode->sampler = VK_NULL_HANDLE;
    /* Entries come from a non-zeroed g_malloc_n, and the release path
     * reads this flag even on a node that never borrowed. */
    snode->s2t_borrowed = false;
    snode->seq = ++r->texture_binding_seq;
    snode->validation_frame = (unsigned int)-1;
}

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode)
{
    /* In-place sampling: a borrowed image belongs to its surface; only the
     * view and sampler are released (a borrowed node has no allocation). */
    bool borrowed = snode->s2t_borrowed &&
                    snode->allocation == VK_NULL_HANDLE;
    snode->s2t_borrowed = false;

    /* An in-flight slot may still sample this binding: defer the destruction
     * until every slot fence settles. */
    struct XemuVkTrashEntry e = { .sampler = snode->sampler,
                                  .image_view = snode->image_view,
                                  .image = borrowed ? VK_NULL_HANDLE :
                                                      snode->image,
                                  .allocation = borrowed ?
                                      VK_NULL_HANDLE : snode->allocation,
                                  .image_ci = snode->image_ci,
                                  .pool_ok = !borrowed };
    pgraph_vk_trash_push(r, &e);
    snode->sampler = VK_NULL_HANDLE;
    snode->image_view = VK_NULL_HANDLE;
    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    // FIXME: Simplify. We don't really need to check bindings


    // Currently bound
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i] == snode) {
            return false;
        }
    }

    // Used in command buffer
    if (r->in_command_buffer && snode->submit_time == r->submit_count) {
        return false;
    }

    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    texture_cache_release_node_resources(r, snode);
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    return memcmp(&snode->key, key, sizeof(TextureKey));
}

static void texture_cache_init(PGRAPHVkState *r)
{
    const size_t texture_cache_size = 1024;
    lru_init(&r->texture_cache);
    r->texture_cache_entries = g_malloc_n(texture_cache_size, sizeof(TextureBinding));
    assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;
}

static void texture_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->texture_cache);
    g_free(r->texture_cache_entries);
    r->texture_cache_entries = NULL;
}

void pgraph_vk_trim_texture_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Allow specifying some amount to trim by

    int num_to_evict = r->texture_cache.num_used / 4;
    int num_evicted = 0;

    while (num_to_evict-- && lru_try_evict_one(&r->texture_cache)) {
        num_evicted += 1;
    }

    NV2A_VK_DPRINTF("Evicted %d textures, %d remain", num_evicted, r->texture_cache.num_used);
}

void pgraph_vk_init_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    texture_cache_init(r);
    create_dummy_texture(pg);

    r->texture_format_properties = g_malloc0_n(
        ARRAY_SIZE(kelvin_color_format_vk_map), sizeof(VkFormatProperties));
    for (int i = 0; i < ARRAY_SIZE(kelvin_color_format_vk_map); i++) {
        vkGetPhysicalDeviceFormatProperties(
            r->physical_device, kelvin_color_format_vk_map[i].vk_format,
            &r->texture_format_properties[i]);
    }
}

void pgraph_vk_finalize_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_command_buffer);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }

    destroy_dummy_texture(r);
    texture_cache_finalize(r);
    sampler_cache_destroy(r);
    for (int i = 0; i < ARRAY_SIZE(r->znative_scratch); i++) {
        if (r->znative_scratch[i].image != VK_NULL_HANDLE) {
            vmaDestroyImage(r->allocator, r->znative_scratch[i].image,
                            r->znative_scratch[i].allocation);
            r->znative_scratch[i].image = VK_NULL_HANDLE;
        }
    }

    assert(r->texture_cache.num_used == 0);

    g_free(r->texture_format_properties);
    r->texture_format_properties = NULL;
}
