/*
 * Real hardware speed: the guest CPU runs no faster than the Xbox's
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

#ifndef QEMU_CPU_PACE_H
#define QEMU_CPU_PACE_H

/*
 * The CPU side of the "Real hardware speed" setting (perf.real_hw_speed).
 * Translated blocks draw their instructions from a budget and stop when it
 * is spent (translator.c); cpu-exec.c refills it once the Xbox CPU would
 * have run them. Off, nothing is emitted and the guest is uncapped.
 */
extern bool xemu_cpu_pace_on;

/* Follows the setting; blocks are translated again when it changes. */
void xemu_cpu_pace_sync(bool want);

/*
 * Time the vCPU thread lost to the emulator itself: a lock another thread
 * held, a block being translated. The real CPU would have gone on, so the
 * guest may make this time up, and only this time. vCPU thread only.
 */
extern int64_t xemu_cpu_pace_stall_ns;

#endif
