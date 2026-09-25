/*
 * Geforce NV2A PGRAPH OpenGL Renderer
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

static void perform_blit(int operation, uint8_t *source, uint8_t *dest,
                         size_t width, size_t height, size_t width_bytes,
                         size_t source_pitch, size_t dest_pitch,
                         BetaState *beta)
{
    if (operation == NV09F_SET_OPERATION_SRCCOPY) {
        for (unsigned int y = 0; y < height; y++) {
            memmove(dest, source, width_bytes);
            source += source_pitch;
            dest += dest_pitch;
        }
    } else if (operation == NV09F_SET_OPERATION_BLEND_AND) {
        uint32_t max_beta_mult = 0x7f80;
        uint32_t beta_mult = beta->beta >> 16;
        uint32_t inv_beta_mult = max_beta_mult - beta_mult;

        for (unsigned int y = 0; y < height; y++) {
            uint8_t *s = source;
            uint8_t *d = dest;
            for (unsigned int x = 0; x < width; x++) {
                for (unsigned int ch = 0; ch < 3; ch++) {
                    uint32_t a = s[x * 4 + ch] * beta_mult;
                    uint32_t b = d[x * 4 + ch] * inv_beta_mult;
                    d[x * 4 + ch] = (a + b) / max_beta_mult;
                }
            }
            source += source_pitch;
            dest += dest_pitch;
        }
    } else {
        fprintf(stderr, "Unknown blit operation: 0x%x\n", operation);
        assert(false && "Unknown blit operation");
    }
}

static void patch_alpha(uint8_t *dest, size_t width_pixels, size_t height,
                        size_t dest_pitch, uint8_t alpha_val)
{
    for (unsigned int y = 0; y < height; y++) {
        uint8_t *d = dest;
        for (unsigned int x = 0; x < width_pixels; x++) {
            d[x * 4 + 3] = alpha_val;
        }
        dest += dest_pitch;
    }
}

/* A SRCCOPY into a plain texture has no destination surface, and the
 * guest-RAM path would read the source back first: make one when the copy
 * writes all of it. */
static SurfaceBinding *blit_dest_surface(NV2AState *d,
                                         const SurfaceBinding *surf_src,
                                         hwaddr dest_addr,
                                         ContextSurfaces2DState *cs,
                                         ImageBlitState *ib,
                                         unsigned int bytes_per_pixel)
{
    SurfaceBinding *found = pgraph_gl_surface_get(d, dest_addr);
    if (found != NULL) {
        return found;
    }
    if (ib->out_x != 0 || ib->out_y != 0 || ib->width == 0 ||
        ib->height == 0 || bytes_per_pixel == 0 ||
        cs->dest_pitch != ib->width * bytes_per_pixel) {
        return NULL; /* partial or strided copy: the rest would be undefined */
    }
    if (surf_src->swizzle || !surf_src->color ||
        bytes_per_pixel != surf_src->fmt.bytes_per_pixel) {
        return NULL;
    }
    /* Making the destination evicts every surface it overlaps: never the
     * source it is about to be copied from. */
    hwaddr dest_size = (hwaddr)cs->dest_pitch * ib->height;
    if (dest_addr < surf_src->vram_addr + surf_src->size &&
        surf_src->vram_addr < dest_addr + dest_size) {
        return NULL;
    }
    return pgraph_gl_surface_create_blit_dest(d, dest_addr, surf_src,
                                              cs->dest_pitch, ib->width,
                                              ib->height);
}

void pgraph_gl_image_blit(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    ContextSurfaces2DState *context_surfaces = &pg->context_surfaces_2d;
    ImageBlitState *image_blit = &pg->image_blit;
    BetaState *beta = &pg->beta;

    pgraph_gl_surface_update(d, false, true, true);

    assert(context_surfaces->object_instance == image_blit->context_surfaces);

    unsigned int bytes_per_pixel;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        bytes_per_pixel = 1;
        break;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        bytes_per_pixel = 2;
        break;
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_Y32:
        bytes_per_pixel = 4;
        break;
    default:
        fprintf(stderr, "Unknown blit surface format: 0x%x\n",
                context_surfaces->color_format);
        assert(false);
        break;
    }

    hwaddr source_dma_len;
    uint8_t *source = (uint8_t *)nv_dma_map(
        d, context_surfaces->dma_image_source, &source_dma_len);
    assert(context_surfaces->source_offset < source_dma_len);
    source += context_surfaces->source_offset;
    hwaddr source_addr = source - d->vram_ptr;

    hwaddr dest_dma_len;
    uint8_t *dest = (uint8_t *)nv_dma_map(d, context_surfaces->dma_image_dest,
                                          &dest_dma_len);
    assert(context_surfaces->dest_offset < dest_dma_len);
    dest += context_surfaces->dest_offset;
    hwaddr dest_addr = dest - d->vram_ptr;

    SurfaceBinding *surf_src = pgraph_gl_surface_get(d, source_addr);

    /* An NV09F SRCCOPY between surfaces is a GPU copy, VRAM to VRAM like the
     * silicon; whatever the guards refuse takes the RAM path below, which
     * reads the scaled source back. */
    {
        static int blitgpu = -1;
        if (blitgpu < 0) {
            blitgpu = (epoxy_gl_version() >= 43 ||
                       epoxy_has_gl_extension("GL_ARB_copy_image")) ? 1 : 0;
        }
        SurfaceBinding *surf_dest;
        if (blitgpu &&
            image_blit->operation == NV09F_SET_OPERATION_SRCCOPY &&
            surf_src != NULL &&
            (surf_dest = blit_dest_surface(d, surf_src, dest_addr,
                                           context_surfaces, image_blit,
                                           bytes_per_pixel)) != NULL &&
            surf_dest != surf_src && surf_src->color && surf_dest->color &&
            !surf_src->swizzle && !surf_dest->swizzle &&
            surf_src->fmt.gl_internal_format ==
                surf_dest->fmt.gl_internal_format &&
            bytes_per_pixel == surf_src->fmt.bytes_per_pixel &&
            bytes_per_pixel == surf_dest->fmt.bytes_per_pixel) {
            if (pgraph_blit_gpu_fits(d, dest_addr, bytes_per_pixel,
                                     surf_src->width, surf_src->height,
                                     surf_src->pitch, surf_dest->width,
                                     surf_dest->height, surf_dest->pitch)) {
                bool full_replace = image_blit->out_x == 0 &&
                                    image_blit->out_y == 0 &&
                                    image_blit->width == surf_dest->width &&
                                    image_blit->height == surf_dest->height;
                if (surf_src->upload_pending) {
                    pgraph_gl_upload_surface_data(d, surf_src, false);
                }
                if (surf_dest->upload_pending) {
                    if (full_replace) {
                        surf_dest->upload_pending = false;
                    } else {
                        pgraph_gl_upload_surface_data(d, surf_dest, false);
                    }
                }

                unsigned int fp_scale = pg->surface_scale_factor;
                glCopyImageSubData(surf_src->gl_buffer, GL_TEXTURE_2D, 0,
                                   image_blit->in_x * fp_scale,
                                   image_blit->in_y * fp_scale, 0,
                                   surf_dest->gl_buffer, GL_TEXTURE_2D, 0,
                                   image_blit->out_x * fp_scale,
                                   image_blit->out_y * fp_scale, 0,
                                   image_blit->width * fp_scale,
                                   image_blit->height * fp_scale, 1);

                /* Left as a draw leaves it: RAM is behind, readers go through
                 * the download path, and a pending download must stay
                 * pending. RAM was not written: no set_client_dirty. */
                pg->draw_time++;
                surf_dest->draw_time = pg->draw_time;
                surf_dest->frame_time = pg->frame_time;
                surf_dest->draw_dirty = true;
                pgraph_gl_surface_mark_ahead(surf_dest);
                surf_dest->upload_pending = false;
                return;
            }
        }
    }

    if (surf_src) {
        pgraph_gl_surface_download_if_dirty(d, surf_src);
    }

    hwaddr source_offset = image_blit->in_y * context_surfaces->source_pitch +
                           image_blit->in_x * bytes_per_pixel;
    hwaddr dest_offset = image_blit->out_y * context_surfaces->dest_pitch +
                         image_blit->out_x * bytes_per_pixel;

    size_t max_row_pixels =
        MIN(context_surfaces->source_pitch, context_surfaces->dest_pitch) /
        bytes_per_pixel;
    size_t row_pixels = MIN(max_row_pixels, image_blit->width);

    hwaddr dest_size = (image_blit->height - 1) * context_surfaces->dest_pitch +
                       image_blit->width * bytes_per_pixel;

    uint8_t *source_row = source + source_offset;
    uint8_t *dest_row = dest + dest_offset;
    size_t row_bytes = row_pixels * bytes_per_pixel;

    size_t adjusted_height = image_blit->height;
    size_t leftover_bytes = 0;

    hwaddr clipped_dest_size =
        nv_clip_gpu_tile_blit(d, dest_addr + dest_offset, dest_size);

    if (clipped_dest_size < dest_size) {
        adjusted_height = clipped_dest_size / context_surfaces->dest_pitch;
        size_t consumed_bytes = adjusted_height * context_surfaces->dest_pitch;

        leftover_bytes = clipped_dest_size - consumed_bytes;
    }

    SurfaceBinding *surf_dest = pgraph_gl_surface_get(d, dest_addr);
    if (surf_dest) {
        if (adjusted_height < surf_dest->height ||
            row_pixels < surf_dest->width) {
            pgraph_gl_surface_download_if_dirty(d, surf_dest);
        } else {
            // The blit will completely replace the surface so any pending
            // download should be discarded.
            surf_dest->download_pending = false;
            surf_dest->draw_dirty = false;
        }
        surf_dest->upload_pending = true;
        pg->draw_time++;
    }

    NV2A_DPRINTF("  blit 0x%tx -> 0x%tx (Size: %llu, Clipped Height: %zu)\n",
                 source_addr, dest_addr, dest_size, adjusted_height);

    if (adjusted_height > 0) {
        perform_blit(image_blit->operation, source_row, dest_row, row_pixels,
                     adjusted_height, row_bytes, context_surfaces->source_pitch,
                     context_surfaces->dest_pitch, beta);
    }

    if (leftover_bytes > 0) {
        uint8_t *src =
            source_row + adjusted_height * context_surfaces->source_pitch;
        uint8_t *dest =
            dest_row + adjusted_height * context_surfaces->dest_pitch;

        perform_blit(image_blit->operation, src, dest,
                     leftover_bytes / bytes_per_pixel, 1, leftover_bytes,
                     context_surfaces->source_pitch,
                     context_surfaces->dest_pitch, beta);
    }

    bool needs_alpha_patching;
    uint8_t alpha_override;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0xff;
        break;
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0;
        break;
    default:
        needs_alpha_patching = false;
        alpha_override = 0;
    }

    if (needs_alpha_patching) {
        if (adjusted_height > 0) {
            patch_alpha(dest_row, row_pixels, adjusted_height,
                        context_surfaces->dest_pitch, alpha_override);
        }

        if (leftover_bytes > 0) {
            uint8_t *dest =
                dest_row + adjusted_height * context_surfaces->dest_pitch;
            patch_alpha(dest, leftover_bytes / 4, 1, 0, alpha_override);
        }
    }

    dest_addr += dest_offset;
    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_NV2A_TEX);
}
