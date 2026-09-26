/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2020-2021 Matt Borgerson
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

#ifndef HW_NV2A_H
#define HW_NV2A_H

void nv2a_init(PCIBus *bus, int devfn, MemoryRegion *ram);
void nv2a_context_init(void);
int nv2a_get_framebuffer_surface(void);
void nv2a_profile_present(void);
void nv2a_release_framebuffer_surface(void);
void nv2a_set_surface_scale_factor(unsigned int scale);
unsigned int nv2a_get_surface_scale_factor(void);
const uint8_t *nv2a_get_dac_palette(void);
int nv2a_get_screen_off(void);

/* The tuned GPU's own, of no use to the stock one. */
/* Driver self-test for the GL optimization pack; why = short reason. */
bool nv2a_gl_probe_optimizations(char *why, size_t why_len);
/* GL 4.1 and a passing self-test, set at GL context creation (ui/xemu.c). */
extern int xemu_gl_opt_capable;
void nv2a_fifo_kick_safety(void);
void nv2a_set_game_name(const char *name, const char *before);

/* The stock GPU (hw/xbox/nv2a-stock) is plugged in instead of this one when
 * GPU boost is off at start-up; the first group of entry points above, the
 * profiler's counter names and values and the UI's RenderDoc calls
 * (debug.h) then hand every call over to its own. */
bool nv2a_stock_active(void);
void stock_nv2a_init(PCIBus *bus, int devfn, MemoryRegion *ram);
void stock_nv2a_context_init(void);
int stock_nv2a_get_framebuffer_surface(void);
void stock_nv2a_profile_present(void);
void stock_nv2a_release_framebuffer_surface(void);
void stock_nv2a_set_surface_scale_factor(unsigned int scale);
unsigned int stock_nv2a_get_surface_scale_factor(void);
const uint8_t *stock_nv2a_get_dac_palette(void);
int stock_nv2a_get_screen_off(void);
const char *stock_nv2a_profile_get_counter_name(unsigned int cnt);
int stock_nv2a_profile_get_counter_value(unsigned int cnt);
bool stock_nv2a_dbg_renderdoc_available(void);
void stock_nv2a_dbg_renderdoc_capture_frames(int num_frames, bool trace);

#endif
