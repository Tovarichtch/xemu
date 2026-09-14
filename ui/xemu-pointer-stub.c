/*
 * xemu per-device pointer input (light guns, mice): no backend on this host
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

#include "qemu/osdep.h"
#include "xemu-pointer.h"
#include "xemu-settings.h"
#include <SDL3/SDL.h>


const char *xemu_pointer_backend(void)
{
    return NULL;
}

bool xemu_pointer_exclusive_grab(void)
{
    return false;
}

void xemu_pointer_init(void)
{
}

void xemu_pointer_poll(void)
{
}

void xemu_pointer_rescan(void)
{
}

int xemu_pointer_count(void)
{
    return 0;
}

const XemuPointer *xemu_pointer_get(int i)
{
    return NULL;
}

bool xemu_pointer_position(const char *identity, float *x, float *y)
{
    return false;
}

uint32_t xemu_pointer_buttons(const char *identity)
{
    return 0;
}

void xemu_pointer_set_grab(bool on, const char *id1, const char *id2)
{
}
