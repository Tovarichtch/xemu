/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2021 Matt Borgerson
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

#include "nv2a_int.h"

/* Set while a PFB compression region is valid: the zeta surface is compressed
 * (Chihiro D3D programs region 1 as Z24S8_GRAD). Read by the cost model. */
unsigned xemu_pfb_zcomp_on;

uint64_t pfb_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFB_CSTATUS:
        r = memory_region_size(d->vram);
        break;
    case NV_PFB_WBC:
        r = 0; /* Flush not pending. */
        break;
    default:
        r = d->pfb.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFB, addr, size, r);
    return r;
}

void pfb_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFB, addr, size, val);

    switch (addr) {
    default:
        d->pfb.regs[addr] = val;
        break;
    }

    if (addr >= 0x300 && addr < 0x320) {
        pfb_update_zcomp(d);
    }
}

void pfb_update_zcomp(NV2AState *d)
{
    unsigned on = 0;
    for (int i = 0; i < 8; i++) {
        on |= (d->pfb.regs[0x300 + i * 4] >> 31) & 1;
    }
    xemu_pfb_zcomp_on = on;
}
