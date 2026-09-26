/*
 * xemu per-device pointer input (light guns, mice)
 *
 * Copyright (c) 2026 Réda Chérif-Touil (Tovarichtch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_POINTER_H
#define XEMU_POINTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pointer devices read one by one by a host backend (evdev on Linux, Raw
 * Input on Windows) so that two players can aim with their own light gun or
 * mouse, which the merged system cursor cannot do. A device keeps its
 * identity across replugs: on Linux the serial number, else VID:PID, else
 * the physical port, else the name; on Windows VID:PID, else the port, as
 * in PCSX2X6. Enabled by chihiro.settings.pointer_devices; off, nothing
 * is opened.
 */

/* Room for a cabinet: a gun often shows as several pointers, and an
 * unplugged device keeps its entry. */
#define XEMU_POINTER_MAX 32

typedef struct XemuPointer {
    char identity[64];
    char name[96];    /* "Product [VID:PID]" */
    bool present;     /* false once unplugged, the entry is kept */
    bool absolute;    /* light gun or tablet; else a relative mouse */
} XemuPointer;

/* Backend name for the UI ("evdev", "raw input"), NULL where unsupported. */
const char *xemu_pointer_backend(void);

/* True where the grab takes a device away from the desktop cursor (evdev).
 * Raw Input cannot: every device keeps driving the system cursor. */
bool xemu_pointer_exclusive_grab(void);

void xemu_pointer_init(void);
/* Called from the main loop under the lock: hotplug and event drain. */
void xemu_pointer_poll(void);
void xemu_pointer_rescan(void);

int xemu_pointer_count(void);
const XemuPointer *xemu_pointer_get(int i);

/* Last position in window pixels (drawable size); false when the device is
 * absent or has not reported one yet. */
bool xemu_pointer_position(const char *identity, float *x, float *y);

/* Buttons in the SDL mouse layout: bit 0 left, 1 middle, 2 right, 3 x1,
 * 4 x2, then bits 5..12 for BTN_1..BTN_8 of guns with extra buttons. */
uint32_t xemu_pointer_buttons(const char *identity);

/* Exclusive grab of the devices aimed with, so they stop driving the
 * desktop cursor while playing (evdev); nothing where the host has no such
 * grab. Identities may be NULL. */
void xemu_pointer_set_grab(bool on, const char *id1, const char *id2);

#ifdef __cplusplus
}
#endif

#endif
