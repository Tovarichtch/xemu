/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"
#include "qemu/cpu-boost.h"

/* CPU boost widens the jump cache to 16384 entries (stock: 4096); the array
 * is sized for the wider one. */
#define TB_JMP_CACHE_BITS (xemu_cpu_boost ? 14 : 12)
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)
#define TB_JMP_CACHE_SIZE_MAX (1 << 14)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
    } array[TB_JMP_CACHE_SIZE_MAX];
} CPUJumpCache;

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
