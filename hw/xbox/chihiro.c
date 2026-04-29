/*
 * QEMU Chihiro emulation
 *
 * Copyright (c) 2013 espes
 * Copyright (c) 2018-2021 Matt Borgerson
 * Copyright (c) 2025 Tovarichtch (Réda)
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
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/isa/isa.h"
#include "hw/boards.h"
#include "system/memory.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "chihiro.h"
#include "chihiro_fatx.h"
#include "system/blockdev.h"
#include "hw/usb.h"
#include "target/i386/cpu.h"

/*
 * Chihiro Mediaboard LPC I/O
 *
 * The Chihiro baseboard exposes a set of I/O registers at 0x4000-0x40FF
 * on the LPC/ISA bus. These are used by SEGABOOT to detect the baseboard,
 * query firmware version, DIMM size, and board type.
 *
 * Register map (from MAME chihiro.cpp + CXBX MediaBoard.cpp + RE of 0x3DF40):
 *   0x1E: DIMM base address low word (combined with 0x20 for FC800/FC801 offset calc)
 *   0x20: DIMM base address high word (0x0100 → base = 0x01000000)
 *   0x22: XBAM string byte 3-4 (0x4258 = "BX")
 *   0x24: XBAM string byte 5-6 (0x4D41 = "MA")
 *   0xE0: IRQ10 acknowledge (write clears IRQ10)
 *   0xF0: Chip revision / board type (0x0000 = Type-1, 0x0100 = Type-3)
 *   0xF4: DIMM size (0=128M, 1=256M, 2=512M, 3=1024M)
 *
 * SEGABOOT 0x3DF40 checks for "XBAM" at ports 0x4022-0x4024 to confirm
 * the mediaboard is present. Ports 0x401E/0x4020 provide the DIMM base
 * address used to compute FC800/FC801 mbcom file offsets.
 */

#define SEGA_FIRMWARE_VERSION               0x1E
#define SEGA_XBAM_STRING_0                  0x20
#define SEGA_XBAM_STRING_1                  0x22
#define SEGA_XBAM_STRING_2                  0x24
#define SEGA_IRQ10_ACK                      0xE0
#define SEGA_CHIP_REVISION                  0xF0
#   define SEGA_CHIP_REVISION_TYPE1             0x0000
#   define SEGA_CHIP_REVISION_TYPE3             0x0100
#define SEGA_DIMM_SIZE                      0xF4
#   define SEGA_DIMM_SIZE_128M                  0
#   define SEGA_DIMM_SIZE_256M                  1
#   define SEGA_DIMM_SIZE_512M                  2
#   define SEGA_DIMM_SIZE_1024M                 3

/* mbcom command IDs (from CXBX MediaBoard.cpp + MAME chihiro.cpp) */
#define MB_CMD_DIMM_SIZE            0x0001
#define MB_CMD_STATUS               0x0100
#define MB_CMD_FIRMWARE_VERSION     0x0101
#define MB_CMD_SYSTEM_TYPE          0x0102
#define MB_CMD_SERIAL_NUMBER        0x0103
#define MB_CMD_HARDWARE_TEST        0x0301

#define MB_STATUS_READY             5

/* #define DEBUG_CHIHIRO */

/* Always log LPC accesses during development */
#define CHIHIRO_LOG 1

typedef struct ChihiroLPCState {
    ISADevice dev;
    MemoryRegion ioport;

    /* mbcom communication buffers (baseboard command/response protocol) */
    uint8_t mbcom_read_buffer[32];
    uint8_t mbcom_write_buffer[32];

    /* EEPROM validation hack timer */
    QEMUTimer *eeprom_hack_timer;
    bool eeprom_hack_applied;
    uint32_t lpc_reg_addr;        /* MediaBoard register address (set via port 0x4004) */
    uint32_t lpc_reg_data;        /* MediaBoard register data (read via port 0x4000) */

    /* IRQ10 for baseboard → SEGABOOT communication */
    qemu_irq irq10;
    QEMUTimer *irq10_timer;

    /* USB hotplug timers (simulates staggered AN2131 I2C firmware boot) */
    QEMUTimer *usb_hotplug_timer;     /* QC at T+1500ms */
    QEMUTimer *usb_hotplug_sc_timer;  /* SC at T+1700ms */
    QEMUTimer *usb_poll_patch_timer;  /* Patch UsbPollQC/SC to return 0 */
    bool usb_poll_patched;

    /* Diagnostic: periodic state machine dump */
    QEMUTimer *diag_timer;
    uint32_t diag_state_pa;    /* PA of CheckErrors state [VA 0x87AFC] */
    uint32_t diag_counter_pa;  /* PA of CheckErrors counter [VA 0x87AE8] */
    uint32_t diag_ready_pa;    /* PA of CheckErrors ready [VA 0x87AF8] */
    uint32_t diag_gate_pa;     /* PA of gate variable [VA 0x89C38] */
    uint32_t diag_bootstate_pa;/* PA of MbcomBootSequence state [VA 0x89C48] */

    /* LPC port read counters for v136 instrumentation */
    uint32_t lpc_40f0_reads;   /* MbcomNegotiate (state 1) — port 0x40F0 */
    uint32_t lpc_401e_reads;   /* MbcomCommand (state 2) — port 0x401E (firmware) */
    uint32_t lpc_4084_reads;   /* MbcomCommand (state 2) — port 0x4084 (session) */
    uint32_t last_bootstate;   /* previous bootstate to detect changes */
    uint16_t lpc_scratch_4026;    /* Port 0x4026 read-write scratch register */
    bool     mbcom_handshake_done; /* True after SEGABOOT writes 0x0102 to port 0x4026 */
    uint8_t  mbcom_e0_status;     /* Port 0x40E0 status bits: bit0=data, bit2=cmd_complete */

    /* Baseboard DMA register state (indirect access via 0x4004/0x4000) */
    uint32_t bb_reg_addr;       /* 0xA0000020: indirect address pointer */
    uint32_t bb_reg_status;     /* 0xA0000040: DMA status/enable */
    bool     bb_dma_active;     /* true when 0xA0000040 bit31 set (burst mode) */
    uint32_t bb_dma_count;      /* dwords written in current burst */
    bool     bb_event_pending;  /* baseboard has an event for SEGABOOT */

    /* DIMM board mailbox: commands at 0x84000020, responses at 0x84000000 */
    uint32_t dimm_cmd[8];      /* 8-dword command block written by SEGABOOT */
    uint32_t dimm_resp[8];     /* 8-dword response block read by SEGABOOT */
    uint32_t dimm_cmd_idx;     /* current dword index in command write */
    bool     dimm_resp_ready;  /* true when response buffer has new data */
    uint32_t dimm_cmd_count;   /* total commands processed */
    uint16_t dimm_next_seq;    /* next sequence number for unsolicited events */
    QEMUTimer *dimm_event_timer; /* fires after handshake to inject STATUS event */
} ChihiroLPCState;

#define CHIHIRO_LPC_DEVICE(obj) \
    OBJECT_CHECK(ChihiroLPCState, (obj), "chihiro-lpc")

static bool chihiro_active;
bool chihiro_game_running;  /* Set after QuickReboot — disables SEGABOOT DMA scan */
static bool chihiro_boot3_reached; /* Set when SEGABOOT reaches boot=3 (checks complete) */
static char chihiro_game_filename[64]; /* Game XBE filename saved at boot=3 */
char chihiro_game_dir[1024];   /* Game directory path (from dvd_path) */
static uint32_t chihiro_ldp_pa;  /* v208: LDP physical address, captured at boot=3 */
static ChihiroLPCState *chihiro_lpc_global;
uint32_t chihiro_usb_sm_pa;  /* PA of USB state machine globals at VA 0xC3F10 */

/* USB devices for delayed hotplug (simulates AN2131 I2C firmware boot) */
static USBDevice *chihiro_usb_qc = NULL;
static USBDevice *chihiro_usb_sc = NULL;
static bool chihiro_usb_hotplug_scheduled = false;

/*
 * v205: Called from ohci_bus_start() when OHCI goes OPERATIONAL.
 * Schedule USB device attachment 150ms after BUS START so that:
 * 1. The kernel has already enabled RHSC interrupts
 * 2. Fresh CSC events will trigger the RHSC handler
 * 3. The handler will do full enumeration: PortReset → GET_DESC → SET_ADDRESS
 *    → GET_CONFIG_DESC → SET_CONFIG (unconditional, per standard USB flow)
 *
 * On real hardware, AN2131 chips boot in ~200ms and are present BEFORE the
 * kernel starts OHCI. The kernel's first port scan sees CSC=1 and enumerates.
 * In our emulation, we attach AFTER BUS START to ensure the RHSC handler
 * sees fresh CSC=1 events (not stale ones cleared during OHCI init).
 */
void chihiro_on_ohci_bus_start(void)
{
    if (!chihiro_active || chihiro_usb_hotplug_scheduled) {
        return;  /* Not Chihiro, or already scheduled */
    }

    /* Only trigger on first BUS START (kernel init, not SEGABOOT re-init) */
    if (chihiro_usb_qc && chihiro_usb_qc->attached) {
        return;  /* Devices already attached from a previous BUS START */
    }

    ChihiroLPCState *s = chihiro_lpc_global;
    if (!s) {
        return;
    }

    chihiro_usb_hotplug_scheduled = true;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    printf("[%07lld] Chihiro USB: OHCI BUS START detected — scheduling hotplug "
           "QC at +50ms, SC at +100ms\n", TS_MS);

    /* Schedule QC hotplug at BUS_START + 50ms */
    timer_mod(s->usb_hotplug_timer, now + 50);

    /* Schedule SC hotplug at BUS_START + 100ms (50ms gap for separate RHSC) */
    timer_mod(s->usb_hotplug_sc_timer, now + 100);
}

/* v202: Counter accessors from chihiro-usb.c */
extern void chihiro_usb_get_counters(USBDevice *dev, uint32_t *nak, uint32_t *bulk_in, uint32_t *bulk_out);
extern void chihiro_usb_reset_counters(USBDevice *dev);

void chihiro_usb_set_devices(USBDevice *qc, USBDevice *sc)
{
    chihiro_usb_qc = qc;
    chihiro_usb_sc = sc;
}

/*
 * Simulates AN2131 firmware boot from I2C EEPROM.
 * On real hardware, the AN2131 chips take ~200-500ms to load firmware
 * from ic10/pc20 EEPROMs before appearing on the USB bus.
 * This timer fires after the kernel's initial USB scan is complete,
 * causing a hot-plug event that triggers re-enumeration.
 */
/*
 * QC hotplug — fires first (T+1500ms).
 * On real hardware, QC (ic10 EEPROM, 6864 bytes firmware) boots first.
 */
static void chihiro_usb_hotplug_qc_cb(void *opaque)
{
    printf("[%07lld] Chihiro USB HOTPLUG: QC firmware boot complete\n", TS_MS);

    if (chihiro_usb_qc && !chihiro_usb_qc->attached) {
        usb_device_attach(chihiro_usb_qc, &error_abort);
        printf("[%07lld] Chihiro USB HOTPLUG: QC attached to port %d\n", TS_MS,
               chihiro_usb_qc->port ? chihiro_usb_qc->port->index : -1);
    }
}

/*
 * SC hotplug — fires 200ms after QC (T+1700ms).
 * On real hardware, SC (pc20 EEPROM, 6731 bytes firmware) boots independently.
 * The 200ms gap ensures the kernel processes QC's RHSC event completely
 * (port reset → GET_DESC → SET_ADDRESS → GET_DESC config → SET_CONFIG)
 * before SC's RHSC event arrives as a separate enumeration cycle.
 */
static void chihiro_usb_hotplug_sc_cb(void *opaque)
{
    printf("[%07lld] Chihiro USB HOTPLUG: SC firmware boot complete\n", TS_MS);

    if (chihiro_usb_sc && !chihiro_usb_sc->attached) {
        usb_device_attach(chihiro_usb_sc, &error_abort);
        printf("[%07lld] Chihiro USB HOTPLUG: SC attached to port %d\n", TS_MS,
               chihiro_usb_sc->port ? chihiro_usb_sc->port->index : -1);
    }
}

/*
 * SEGABOOT Physical RAM Patcher
 * =============================
 *
 * SEGABOOT is loaded by the Xbox kernel into paged virtual memory.
 * Physical addresses are unknown. We scan RAM for unique byte signatures
 * and patch conditional jumps to force the success path.
 *
 * baseboard_init (VA 0x41EF0) structure:
 *   1. UsbEnumPoll()         → if fail → return 5  (Error 02)
 *   2. RegisterClassDriver() → if fail → return 2  (Error 02)
 *   3. ReadEEPROM(0)         → no fatal check
 *   4. ReadEEPROM(1)         → no fatal check
 *   5. InitMbcom()           → initializes mediaboard communication
 *   6. return 0              → SUCCESS
 */

typedef struct {
    const uint8_t *bytes;
    int length;
    int patch_offset;
    const uint8_t *patch_bytes;
    int patch_len;
    uint32_t sig_va;  /* Known VA of this signature in SEGABOOT */
    const char *name;
    bool applied;
} ChihiroPatch;

/*
 * Walk x86 page tables (non-PAE) to translate VA → PA.
 * Returns physical address, or 0xFFFFFFFF on failure.
 */
uint32_t chihiro_va_to_pa(uint32_t va)
{
    CPUState *cpu = first_cpu;
    if (!cpu) return 0xFFFFFFFF;

    X86CPU *x86 = X86_CPU(cpu);
    uint32_t cr3 = x86->env.cr[3] & 0xFFFFF000;
    uint32_t pde_addr = cr3 + ((va >> 22) * 4);
    uint32_t pde;
    cpu_physical_memory_read(pde_addr, &pde, 4);
    if (!(pde & 1)) return 0xFFFFFFFF;  /* not present */

    if (pde & 0x80) {
        /* 4MB page (PS bit set) */
        return (pde & 0xFFC00000) | (va & 0x003FFFFF);
    }

    uint32_t pte_addr = (pde & 0xFFFFF000) + (((va >> 12) & 0x3FF) * 4);
    uint32_t pte;
    cpu_physical_memory_read(pte_addr, &pte, 4);
    if (!(pte & 1)) return 0xFFFFFFFF;  /* not present */

    return (pte & 0xFFFFF000) | (va & 0xFFF);
}

/*
 * Diagnostic: periodically dump CheckErrors state machine variables.
 * PAs are computed via x86 page table walk at patch time.
 *   VA [0x87AFC] = state (0-10)
 *   VA [0x87AE8] = counter
 *   VA [0x87AF8] = ready flag
 */
static void chihiro_diag_timer_cb(void *opaque)
{
    ChihiroLPCState *s = (ChihiroLPCState *)opaque;

    /* After QuickReboot — comprehensive game XBE diagnostics.
     * No patches, only measurement. */
    if (chihiro_game_running) {
        static int game_diag_count = 0;
        game_diag_count++;

        /* === ONE-TIME DEEP ANALYSIS (first 2 ticks) === */
        if (game_diag_count <= 2) {
            printf("[%07lld] ====== GAME DIAG DEEP #%d ======\n", TS_MS, game_diag_count);

            /* 1. XBE HEADER at VA 0x10000 */
            uint32_t hdr_pa = chihiro_va_to_pa(0x10000);
            printf("  XBE header: VA=0x10000 PA=0x%08X\n", hdr_pa);
            uint32_t entry_addr = 0, thunk_addr = 0, base_addr = 0;
            uint32_t section_hdr_va = 0, num_sections = 0;
            uint32_t tls_va = 0;
            if (hdr_pa != 0xFFFFFFFF) {
                uint8_t hdr[0x180];
                cpu_physical_memory_read(hdr_pa, hdr, sizeof(hdr));
                printf("  Magic: %c%c%c%c\n", hdr[0], hdr[1], hdr[2], hdr[3]);
                base_addr       = *(uint32_t*)(hdr + 0x104);
                entry_addr      = *(uint32_t*)(hdr + 0x128);
                thunk_addr      = *(uint32_t*)(hdr + 0x158);
                section_hdr_va  = *(uint32_t*)(hdr + 0x120);
                num_sections    = *(uint32_t*)(hdr + 0x11C);
                tls_va          = *(uint32_t*)(hdr + 0x154);
                uint32_t init_flags = *(uint32_t*)(hdr + 0x10C);
                printf("  Base=0x%08X Entry=0x%08X Thunk=0x%08X Flags=0x%08X\n",
                       base_addr, entry_addr, thunk_addr, init_flags);
                printf("  Sections: %u at VA=0x%08X, TLS=0x%08X\n",
                       num_sections, section_hdr_va, tls_va);
            }

            /* 2. CODE at entry point (already decrypted by kernel) */
            if (entry_addr && entry_addr < 0x08000000) {
                uint32_t ep_pa = chihiro_va_to_pa(entry_addr);
                if (ep_pa != 0xFFFFFFFF) {
                    uint8_t code[32];
                    cpu_physical_memory_read(ep_pa, code, 32);
                    printf("  ENTRY @0x%08X (PA=0x%08X):\n", entry_addr, ep_pa);
                    printf("    %02X %02X %02X %02X %02X %02X %02X %02X"
                           " %02X %02X %02X %02X %02X %02X %02X %02X\n",
                           code[0],code[1],code[2],code[3],code[4],code[5],code[6],code[7],
                           code[8],code[9],code[10],code[11],code[12],code[13],code[14],code[15]);
                    printf("    %02X %02X %02X %02X %02X %02X %02X %02X"
                           " %02X %02X %02X %02X %02X %02X %02X %02X\n",
                           code[16],code[17],code[18],code[19],code[20],code[21],code[22],code[23],
                           code[24],code[25],code[26],code[27],code[28],code[29],code[30],code[31]);
                } else {
                    printf("  ENTRY @0x%08X: UNMAPPED!\n", entry_addr);
                }
            }

            /* 3. THUNK TABLE — are kernel imports resolved? */
            if (thunk_addr && thunk_addr < 0x08000000) {
                uint32_t tk_pa = chihiro_va_to_pa(thunk_addr);
                if (tk_pa != 0xFFFFFFFF) {
                    uint32_t thunks[8];
                    cpu_physical_memory_read(tk_pa, thunks, 32);
                    printf("  THUNK @0x%08X (PA=0x%08X):\n", thunk_addr, tk_pa);
                    printf("    [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X\n",
                           thunks[0], thunks[1], thunks[2], thunks[3]);
                    printf("    [4]=0x%08X [5]=0x%08X [6]=0x%08X [7]=0x%08X\n",
                           thunks[4], thunks[5], thunks[6], thunks[7]);
                    /* Thunks should be 0x80xxxxxx (kernel VA) if resolved */
                    int resolved = 0;
                    for (int i = 0; i < 8; i++)
                        if ((thunks[i] & 0x80000000) && thunks[i] != 0xFFFFFFFF) resolved++;
                    printf("    %d/8 resolved to kernel VA\n", resolved);
                }
            }

            /* 4. XBE SECTION HEADERS — map the memory layout */
            if (section_hdr_va && num_sections > 0 && num_sections <= 64) {
                uint32_t sh_pa = chihiro_va_to_pa(section_hdr_va);
                if (sh_pa != 0xFFFFFFFF) {
                    printf("  SECTIONS:\n");
                    for (uint32_t si = 0; si < num_sections && si < 8; si++) {
                        uint8_t sec[56];
                        cpu_physical_memory_read(sh_pa + si * 56, sec, 56);
                        uint32_t sec_flags = *(uint32_t*)(sec + 0x00);
                        uint32_t sec_va    = *(uint32_t*)(sec + 0x04);
                        uint32_t sec_vsz   = *(uint32_t*)(sec + 0x08);
                        uint32_t sec_raw   = *(uint32_t*)(sec + 0x0C);
                        uint32_t sec_rsz   = *(uint32_t*)(sec + 0x10);
                        uint32_t sec_name  = *(uint32_t*)(sec + 0x14);
                        /* Read first 8 bytes of section to check content */
                        uint32_t sva_pa = chihiro_va_to_pa(sec_va);
                        uint8_t peek[8] = {0};
                        if (sva_pa != 0xFFFFFFFF)
                            cpu_physical_memory_read(sva_pa, peek, 8);
                        printf("    [%u] VA=0x%08X sz=0x%X raw=0x%X flags=0x%X name@0x%X"
                               " peek=%02X%02X%02X%02X%02X%02X%02X%02X %s\n",
                               si, sec_va, sec_vsz, sec_rsz, sec_flags, sec_name,
                               peek[0],peek[1],peek[2],peek[3],
                               peek[4],peek[5],peek[6],peek[7],
                               sva_pa != 0xFFFFFFFF ? "" : "UNMAPPED");
                    }
                }
            }

            /* 5. STACK — check if any thread has used the stack */
            /* Xbox default stack top is around 0x00B7xxxx for game threads */
            uint32_t stack_probes[] = {0x00B70000, 0x00B6F000, 0x00400000, 0x00300000};
            printf("  STACK probes:\n");
            for (int sp = 0; sp < 4; sp++) {
                uint32_t sp_pa = chihiro_va_to_pa(stack_probes[sp]);
                if (sp_pa != 0xFFFFFFFF) {
                    uint8_t stk[16];
                    cpu_physical_memory_read(sp_pa, stk, 16);
                    int nonzero = 0;
                    for (int j = 0; j < 16; j++) if (stk[j]) nonzero++;
                    printf("    0x%08X: %02X%02X%02X%02X %02X%02X%02X%02X %s\n",
                           stack_probes[sp],
                           stk[0],stk[1],stk[2],stk[3],stk[4],stk[5],stk[6],stk[7],
                           nonzero ? "HAS DATA" : "ZEROS");
                } else {
                    printf("    0x%08X: UNMAPPED\n", stack_probes[sp]);
                }
            }

            /* 6. KERNEL STATE — XboxKrnl exports at known offsets */
            /* LaunchDataPage pointer is at kernel export #164 (0xA4) */
            /* KeTickCount is at export #156 — if changing, CPU is alive */
            uint32_t ke_tick_va = 0x80063B58; /* approximate, varies by kernel */
            uint32_t ke_tick_pa = chihiro_va_to_pa(ke_tick_va);
            if (ke_tick_pa != 0xFFFFFFFF) {
                uint32_t ticks;
                cpu_physical_memory_read(ke_tick_pa, &ticks, 4);
                printf("  KeTickCount@0x%08X = %u\n", ke_tick_va, ticks);
            }
            /* Try to find LaunchDataPage — scan for signature */
            uint32_t ldp_va = 0x80067A20; /* approximate for arcdkrnl */
            uint32_t ldp_pa = chihiro_va_to_pa(ldp_va);
            if (ldp_pa != 0xFFFFFFFF) {
                uint32_t ldp_ptr;
                cpu_physical_memory_read(ldp_pa, &ldp_ptr, 4);
                printf("  LaunchDataPage@0x%08X ptr=0x%08X\n", ldp_va, ldp_ptr);
                if (ldp_ptr && ldp_ptr < 0x08000000) {
                    uint32_t ldp_data_pa = chihiro_va_to_pa(ldp_ptr);
                    if (ldp_data_pa != 0xFFFFFFFF) {
                        uint8_t ldp[64];
                        cpu_physical_memory_read(ldp_data_pa, ldp, 64);
                        printf("  LDP data: %02X%02X%02X%02X %02X%02X%02X%02X"
                               " %02X%02X%02X%02X %02X%02X%02X%02X\n",
                               ldp[0],ldp[1],ldp[2],ldp[3],ldp[4],ldp[5],ldp[6],ldp[7],
                               ldp[8],ldp[9],ldp[10],ldp[11],ldp[12],ldp[13],ldp[14],ldp[15]);
                    }
                }
            }

            /* 7. NV2A STATE */
            uint32_t pfb_cfg0 = 0, pfb_cstatus = 0, pfb_nvm = 0;
            uint32_t pfifo_mode = 0, pfifo_enable = 0, pfifo_reassign = 0;
            uint32_t pgraph_status = 0, pgraph_intr = 0;
            uint32_t pmc_enable = 0, pmc_intr = 0;
            address_space_read(&address_space_memory, 0xFD100200, MEMTXATTRS_UNSPECIFIED, &pfb_cfg0, 4);
            address_space_read(&address_space_memory, 0xFD10020C, MEMTXATTRS_UNSPECIFIED, &pfb_cstatus, 4);
            address_space_read(&address_space_memory, 0xFD100214, MEMTXATTRS_UNSPECIFIED, &pfb_nvm, 4);
            address_space_read(&address_space_memory, 0xFD002504, MEMTXATTRS_UNSPECIFIED, &pfifo_mode, 4);
            address_space_read(&address_space_memory, 0xFD002200, MEMTXATTRS_UNSPECIFIED, &pfifo_enable, 4);
            address_space_read(&address_space_memory, 0xFD002000, MEMTXATTRS_UNSPECIFIED, &pfifo_reassign, 4);
            address_space_read(&address_space_memory, 0xFD400700, MEMTXATTRS_UNSPECIFIED, &pgraph_status, 4);
            address_space_read(&address_space_memory, 0xFD400100, MEMTXATTRS_UNSPECIFIED, &pgraph_intr, 4);
            address_space_read(&address_space_memory, 0xFD000200, MEMTXATTRS_UNSPECIFIED, &pmc_enable, 4);
            address_space_read(&address_space_memory, 0xFD000100, MEMTXATTRS_UNSPECIFIED, &pmc_intr, 4);
            printf("  NV2A PMC: enable=0x%08X intr=0x%08X\n", pmc_enable, pmc_intr);
            printf("  NV2A PFB: CFG0=0x%08X CSTATUS=0x%08X(%uMB) NVM=0x%08X\n",
                   pfb_cfg0, pfb_cstatus, pfb_cstatus/(1024*1024), pfb_nvm);
            printf("  NV2A PFIFO: mode=0x%08X enable=0x%08X reassign=0x%08X\n",
                   pfifo_mode, pfifo_enable, pfifo_reassign);
            printf("  NV2A PGRAPH: status=0x%08X intr=0x%08X\n", pgraph_status, pgraph_intr);

            /* 8. FRAMEBUFFER content */
            uint32_t pcrtc_start = 0;
            address_space_read(&address_space_memory, 0xFD600800, MEMTXATTRS_UNSPECIFIED, &pcrtc_start, 4);
            uint8_t fb[16];
            cpu_physical_memory_read(pcrtc_start, fb, 16);
            printf("  FB@0x%08X: %02X%02X%02X%02X %02X%02X%02X%02X"
                   " %02X%02X%02X%02X %02X%02X%02X%02X\n",
                   pcrtc_start,
                   fb[0],fb[1],fb[2],fb[3],fb[4],fb[5],fb[6],fb[7],
                   fb[8],fb[9],fb[10],fb[11],fb[12],fb[13],fb[14],fb[15]);

            /* 9. Scan kernel memory for HalReturnToFirmware action code */
            /* The kernel stores the reboot reason at a known location */
            /* Xbox kernel stores XeImageFileName at ~0x80060080 */
            uint32_t xeimg_pa = chihiro_va_to_pa(0x80060080);
            if (xeimg_pa != 0xFFFFFFFF) {
                uint8_t xeimg[64];
                cpu_physical_memory_read(xeimg_pa, xeimg, 64);
                printf("  XeImageFileName@0x80060080: ");
                for (int i = 0; i < 48 && xeimg[i]; i++) printf("%c", xeimg[i] >= 0x20 ? xeimg[i] : '.');
                printf("\n");
            }

            /* 10. VA map for game address range */
            printf("  VA map: ");
            uint32_t va_probes[] = {0x10000, 0xB8BAC, 0x100000, 0x150000, 0x1A0000,
                                    0x200000, 0x80010000, 0xD0000000, 0xD0010000, 0xFD000000};
            for (int v = 0; v < 10; v++) {
                uint32_t pa = chihiro_va_to_pa(va_probes[v]);
                if (pa != 0xFFFFFFFF)
                    printf("%X→%X ", va_probes[v], pa);
                else
                    printf("%X→X ", va_probes[v]);
            }
            printf("\n");

            /* 11. CPU STATE — where is the CPU right now? */
            {
                CPUState *cpu = first_cpu;
                if (cpu) {
                    X86CPU *x86 = X86_CPU(cpu);
                    CPUX86State *env = &x86->env;
                    printf("  CPU: EIP=0x%08X ESP=0x%08X EBP=0x%08X\n",
                           (uint32_t)env->eip, (uint32_t)env->regs[R_ESP],
                           (uint32_t)env->regs[R_EBP]);
                    printf("  EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
                           (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_EBX],
                           (uint32_t)env->regs[R_ECX], (uint32_t)env->regs[R_EDX]);
                    printf("  ESI=0x%08X EDI=0x%08X CR0=0x%08X CR3=0x%08X\n",
                           (uint32_t)env->regs[R_ESI], (uint32_t)env->regs[R_EDI],
                           (uint32_t)env->cr[0], (uint32_t)env->cr[3]);

                    /* Stack backtrace — read 16 DWORDs from ESP */
                    uint32_t esp = (uint32_t)env->regs[R_ESP];
                    uint32_t esp_pa = chihiro_va_to_pa(esp);
                    if (esp_pa != 0xFFFFFFFF) {
                        uint32_t stack[16];
                        cpu_physical_memory_read(esp_pa, stack, 64);
                        printf("  STACK @0x%08X (PA=0x%08X):\n", esp, esp_pa);
                        printf("    +00: %08X %08X %08X %08X\n",
                               stack[0], stack[1], stack[2], stack[3]);
                        printf("    +10: %08X %08X %08X %08X\n",
                               stack[4], stack[5], stack[6], stack[7]);
                        printf("    +20: %08X %08X %08X %08X\n",
                               stack[8], stack[9], stack[10], stack[11]);
                        printf("    +30: %08X %08X %08X %08X\n",
                               stack[12], stack[13], stack[14], stack[15]);
                    } else {
                        printf("  STACK @0x%08X: UNMAPPED\n", esp);
                    }

                    /* Code at EIP — what instruction is executing? */
                    uint32_t eip = (uint32_t)env->eip;
                    uint32_t eip_pa = chihiro_va_to_pa(eip);
                    if (eip_pa != 0xFFFFFFFF) {
                        uint8_t code[16];
                        cpu_physical_memory_read(eip_pa, code, 16);
                        printf("  CODE @EIP=0x%08X (PA=0x%08X): "
                               "%02X %02X %02X %02X %02X %02X %02X %02X "
                               "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                               eip, eip_pa,
                               code[0],code[1],code[2],code[3],
                               code[4],code[5],code[6],code[7],
                               code[8],code[9],code[10],code[11],
                               code[12],code[13],code[14],code[15]);
                    }

                    /* EBP chain — walk frame pointers */
                    printf("  FRAMES: ");
                    uint32_t ebp = (uint32_t)env->regs[R_EBP];
                    for (int f = 0; f < 8 && ebp > 0x10000 && ebp < 0x08000000; f++) {
                        uint32_t ebp_pa = chihiro_va_to_pa(ebp);
                        if (ebp_pa == 0xFFFFFFFF) { printf("(unmapped@%X) ", ebp); break; }
                        uint32_t frame[2]; /* saved_ebp, return_addr */
                        cpu_physical_memory_read(ebp_pa, frame, 8);
                        printf("%X→%X ", ebp, frame[1]);
                        ebp = frame[0];
                    }
                    printf("\n");
                }
            }

            /* 12. CONTIGUOUS MEMORY — D0000000 range detail */
            printf("  CONTIGUOUS D0xxxxxx: ");
            for (uint32_t d = 0xD0000000; d <= 0xD0080000; d += 0x10000) {
                uint32_t pa = chihiro_va_to_pa(d);
                if (pa != 0xFFFFFFFF)
                    printf("%X→%X ", d & 0xFFFFF, pa);
                else
                    printf("%X→X ", d & 0xFFFFF);
            }
            printf("\n");

            /* 13. PFN DATABASE — kernel MmPfnDatabase location */
            /* Xbox kernel stores MmPfnDatabase at ~0x8003FE00 area */
            /* The PFN limit tells us max usable page: 0x3FDF=64MB, 0x7FBF=128MB */
            /* Search kernel for "mov edx, 0x3FDF" (BA DF 3F 00 00) = 64MB limit */
            {
                int found_3fdf = 0, found_7fbf = 0;
                uint8_t pat64[] = {0xDF, 0x3F, 0x00, 0x00};
                uint8_t pat128[] = {0xBF, 0x7F, 0x00, 0x00};
                /* Scan kernel code (PA 0x10000-0x50000) */
                for (uint32_t pa = 0x10000; pa < 0x50000; pa += 4) {
                    uint8_t buf[4];
                    cpu_physical_memory_read(pa, buf, 4);
                    if (!found_3fdf && memcmp(buf, pat64, 4) == 0) {
                        uint8_t prev;
                        cpu_physical_memory_read(pa - 1, &prev, 1);
                        printf("  PFN: found 0x3FDF (64MB limit) at PA 0x%05X (prev_byte=0x%02X)\n", pa, prev);
                        found_3fdf = 1;
                    }
                    if (!found_7fbf && memcmp(buf, pat128, 4) == 0) {
                        uint8_t prev;
                        cpu_physical_memory_read(pa - 1, &prev, 1);
                        printf("  PFN: found 0x7FBF (128MB limit) at PA 0x%05X (prev_byte=0x%02X)\n", pa, prev);
                        found_7fbf = 1;
                    }
                }
                if (!found_3fdf && !found_7fbf) printf("  PFN: neither 0x3FDF nor 0x7FBF found in kernel\n");
            }

            /* 14. HDD PARTITION TABLE — check if kernel found partitions */
            /* Xbox partition table is at LBA 0 of HDD, but kernel accesses via IopPartitionTable */
            /* Check if \Device\Harddisk0\Partition0 is accessible */
            uint32_t part_check_va = 0x80060000; /* near kernel data */
            uint32_t part_pa = chihiro_va_to_pa(part_check_va);
            if (part_pa != 0xFFFFFFFF) {
                uint8_t kdata[32];
                cpu_physical_memory_read(part_pa, kdata, 32);
                printf("  KernData@0x%08X: %02X%02X%02X%02X %02X%02X%02X%02X"
                       " %02X%02X%02X%02X %02X%02X%02X%02X\n",
                       part_check_va,
                       kdata[0],kdata[1],kdata[2],kdata[3],
                       kdata[4],kdata[5],kdata[6],kdata[7],
                       kdata[8],kdata[9],kdata[10],kdata[11],
                       kdata[12],kdata[13],kdata[14],kdata[15]);
            }

            /* 15. SCAN KERNEL DATA for XeImageFileName ("hod3" or ".xbe") */
            {
                printf("  KERNEL STRING SCAN:\n");
                /* Scan kernel data area (PA 0x30000-0x70000) for key strings */
                int found_xbe = 0, found_cdrom = 0, found_mbfs = 0, found_launch = 0;
                for (uint32_t pa = 0x30000; pa < 0x70000 - 32; pa += 4) {
                    uint8_t buf[32];
                    cpu_physical_memory_read(pa, buf, 32);
                    /* Search for "hod3" (game name in path) */
                    for (int i = 0; i < 28; i++) {
                        if (buf[i]=='h' && buf[i+1]=='o' && buf[i+2]=='d' && buf[i+3]=='3' && !found_xbe) {
                            /* Print surrounding context */
                            uint8_t ctx[64];
                            uint32_t ctx_pa = (pa + i > 16) ? pa + i - 16 : pa + i;
                            cpu_physical_memory_read(ctx_pa, ctx, 64);
                            printf("    'hod3' at PA 0x%05X: ", pa + i);
                            for (int c = 0; c < 48 && ctx[c+16]; c++) 
                                printf("%c", ctx[c+16] >= 0x20 && ctx[c+16] < 0x7F ? ctx[c+16] : '.');
                            printf("\n");
                            found_xbe = 1;
                        }
                        if (buf[i]=='C' && buf[i+1]=='d' && buf[i+2]=='R' && buf[i+3]=='o' && !found_cdrom) {
                            uint8_t ctx[48];
                            cpu_physical_memory_read(pa + i, ctx, 48);
                            printf("    'CdRo' at PA 0x%05X: ", pa + i);
                            for (int c = 0; c < 48 && ctx[c]; c++)
                                printf("%c", ctx[c] >= 0x20 && ctx[c] < 0x7F ? ctx[c] : '.');
                            printf("\n");
                            found_cdrom = 1;
                        }
                        if (buf[i]=='m' && buf[i+1]=='b' && buf[i+2]=='f' && buf[i+3]=='s' && !found_mbfs) {
                            uint8_t ctx[48];
                            cpu_physical_memory_read(pa + i, ctx, 48);
                            printf("    'mbfs' at PA 0x%05X: ", pa + i);
                            for (int c = 0; c < 48 && ctx[c]; c++)
                                printf("%c", ctx[c] >= 0x20 && ctx[c] < 0x7F ? ctx[c] : '.');
                            printf("\n");
                            found_mbfs = 1;
                        }
                    }
                }
                if (!found_xbe) printf("    'hod3': NOT FOUND in kernel data\n");
                if (!found_cdrom) printf("    'CdRo': NOT FOUND\n");
                if (!found_mbfs) printf("    'mbfs': NOT FOUND\n");
            }

            /* 16. XboxHardwareInfo — search for the structure
             * On retail Xbox: flags=0x00000000, on Chihiro: flags should have 0x08 (ARCADE)
             * The structure is at a kernel export, typically 0x80036000-0x80038000 area */
            {
                printf("  XboxHardwareInfo scan:\n");
                /* Known Xbox kernel exports XboxHardwareInfo. It contains:
                 * ULONG Flags, UCHAR GpuRevision, UCHAR McpRevision, UCHAR Unknown1, UCHAR Unknown2 */
                for (uint32_t va = 0x80035000; va < 0x8003A000; va += 4) {
                    uint32_t pa = chihiro_va_to_pa(va);
                    if (pa == 0xFFFFFFFF) continue;
                    uint32_t val;
                    cpu_physical_memory_read(pa, &val, 4);
                    /* Look for ARCADE flag pattern: flags with bit 3 set, followed by GPU/MCP rev */
                    if ((val & 0x08) && (val & 0xFFFFFF00) == 0) {
                        uint8_t hw[8];
                        cpu_physical_memory_read(pa, hw, 8);
                        printf("    Candidate @0x%08X(PA=0x%05X): flags=0x%08X GPU=%02X MCP=%02X\n",
                               va, pa, val, hw[4], hw[5]);
                    }
                }
                /* Also check if flags=0 (non-ARCADE kernel) */
                uint32_t hwinfo_pa = chihiro_va_to_pa(0x80036000);
                if (hwinfo_pa != 0xFFFFFFFF) {
                    uint8_t hw[16];
                    cpu_physical_memory_read(hwinfo_pa, hw, 16);
                    printf("    @0x80036000: %02X%02X%02X%02X %02X%02X%02X%02X"
                           " %02X%02X%02X%02X %02X%02X%02X%02X\n",
                           hw[0],hw[1],hw[2],hw[3],hw[4],hw[5],hw[6],hw[7],
                           hw[8],hw[9],hw[10],hw[11],hw[12],hw[13],hw[14],hw[15]);
                }
            }

            /* 17. KERNEL STACK deep scan — look for NTSTATUS error codes */
            {
                CPUState *cpu = first_cpu;
                if (cpu) {
                    X86CPU *x86 = X86_CPU(cpu);
                    uint32_t esp = (uint32_t)x86->env.regs[R_ESP];
                    /* Scan 256 bytes below and above ESP for status codes */
                    uint32_t scan_start = esp - 256;
                    uint32_t scan_pa = chihiro_va_to_pa(scan_start);
                    if (scan_pa != 0xFFFFFFFF) {
                        uint32_t stk[128]; /* 512 bytes */
                        cpu_physical_memory_read(scan_pa, stk, 512);
                        printf("  STACK SCAN (ESP-256 to ESP+256):\n");
                        printf("    NTSTATUS candidates: ");
                        int ns_count = 0;
                        for (int i = 0; i < 128; i++) {
                            /* NTSTATUS: 0xC0xxxxxx = error, 0x80xxxxxx = warning (but also kernel VA) */
                            if ((stk[i] & 0xF0000000) == 0xC0000000) {
                                printf("0x%08X(@+%X) ", stk[i], (i*4) - 256);
                                if (++ns_count >= 8) break;
                            }
                        }
                        if (ns_count == 0) printf("none");
                        printf("\n");
                    }
                }
            }

            /* 18. LaunchDataPage — scan for pointer in kernel exports area */
            {
                printf("  LaunchDataPage scan:\n");
                /* The kernel stores LaunchDataPage pointer at export #164 (varies by kernel).
                 * Scan kernel data for a pointer to low memory that could be the LDP.
                 * LDP starts with dwLaunchDataType (usually 1=QuickReboot or 2=Dashboard) */
                for (uint32_t pa = 0x30000; pa < 0x50000; pa += 4) {
                    uint32_t ptr;
                    cpu_physical_memory_read(pa, &ptr, 4);
                    /* LDP is allocated in contiguous memory, usually 0xD0xxxxxx or low user VA */
                    if (ptr >= 0x00010000 && ptr < 0x01000000) {
                        uint32_t ldp_pa = chihiro_va_to_pa(ptr);
                        if (ldp_pa != 0xFFFFFFFF) {
                            uint32_t ldp_type;
                            cpu_physical_memory_read(ldp_pa, &ldp_type, 4);
                            /* QuickReboot LDP has type 1 or 0x00000001 */
                            if (ldp_type == 1 || ldp_type == 2 || ldp_type == 0) {
                                uint8_t ldp[80];
                                cpu_physical_memory_read(ldp_pa, ldp, 80);
                                printf("    LDP candidate: kern_PA=0x%05X ptr=0x%08X type=%u\n", pa, ptr, ldp_type);
                                printf("    Path: ");
                                /* LaunchPath starts at offset 4, ASCII, 520 bytes max */
                                for (int c = 4; c < 76 && ldp[c]; c++)
                                    printf("%c", ldp[c] >= 0x20 && ldp[c] < 0x7F ? ldp[c] : '.');
                                printf("\n");
                                break; /* show first match only */
                            }
                        }
                    }
                }
            }

            /* 19. XBE TLS callback — check if TLS init ran */
            if (tls_va) {
                uint32_t tls_pa = chihiro_va_to_pa(tls_va);
                if (tls_pa != 0xFFFFFFFF) {
                    uint32_t tls[4];
                    cpu_physical_memory_read(tls_pa, tls, 16);
                    printf("  TLS @0x%08X: RawStart=0x%08X RawEnd=0x%08X IdxAddr=0x%08X Callbacks=0x%08X\n",
                           tls_va, tls[0], tls[1], tls[2], tls[3]);
                }
            }

            /* 20. FULL DEVICE STRING SCAN — find ALL \Device\ paths in kernel memory */
            {
                printf("  DEVICE PATHS (PA 0x10000-0x80000):\n");
                int dev_count = 0;
                for (uint32_t pa = 0x10000; pa < 0x800000 - 16; pa++) {
                    uint8_t buf[8];
                    cpu_physical_memory_read(pa, buf, 8);
                    /* Look for "\\Device\\" or "\Device\" (ASCII) */
                    if ((buf[0]=='\\' && buf[1]=='D' && buf[2]=='e' && buf[3]=='v' &&
                         buf[4]=='i' && buf[5]=='c' && buf[6]=='e' && buf[7]=='\\') ||
                        (buf[0]=='\\' && buf[1]=='D' && buf[2]=='E' && buf[3]=='V')) {
                        uint8_t path[80];
                        cpu_physical_memory_read(pa, path, 80);
                        printf("    PA 0x%05X: ", pa);
                        for (int c = 0; c < 78 && path[c] >= 0x20 && path[c] < 0x7F; c++)
                            printf("%c", path[c]);
                        printf("\n");
                        if (++dev_count >= 20) { printf("    ... (truncated)\n"); break; }
                        pa += 8; /* skip past this match */
                    }
                }
                if (dev_count == 0) printf("    NONE FOUND\n");
            }

            /* 21. DRIVE LETTER & SYMLINK SCAN — look for D:\, T:\, mbfs:, etc */
            {
                printf("  DRIVE LETTERS / SYMLINKS:\n");
                const char *needles[] = {"D:\\", "T:\\", "U:\\", "Z:\\", "E:\\", "mbfs:", "mbcom:", "mbrom:", "\\??\\", NULL};
                for (int n = 0; needles[n]; n++) {
                    int nlen = strlen(needles[n]);
                    int found = 0;
                    for (uint32_t pa = 0x10000; pa < 0x800000 - nlen && !found; pa++) {
                        uint8_t buf[8];
                        cpu_physical_memory_read(pa, buf, nlen);
                        if (memcmp(buf, needles[n], nlen) == 0) {
                            uint8_t ctx[64];
                            cpu_physical_memory_read(pa, ctx, 64);
                            printf("    '%s' at PA 0x%05X: ", needles[n], pa);
                            for (int c = 0; c < 60 && ctx[c] >= 0x20 && ctx[c] < 0x7F; c++)
                                printf("%c", ctx[c]);
                            printf("\n");
                            found = 1;
                        }
                    }
                }
            }

            /* 22. WIDE STRING DEVICE SCAN — kernel uses Unicode (UTF-16LE) for object names */
            {
                printf("  UNICODE DEVICE SCAN (\\0D\\0e\\0v\\0i):\n");
                int udev_count = 0;
                for (uint32_t pa = 0x10000; pa < 0x800000 - 20; pa += 2) {
                    uint8_t buf[16];
                    cpu_physical_memory_read(pa, buf, 16);
                    /* UTF-16LE: \=5C00 D=4400 e=6500 v=7600 i=6900 c=6300 e=6500 */
                    if (buf[0]==0x5C && buf[1]==0x00 && buf[2]==0x44 && buf[3]==0x00 &&
                        buf[4]==0x65 && buf[5]==0x00 && buf[6]==0x76 && buf[7]==0x00 &&
                        buf[8]==0x69 && buf[9]==0x00) {
                        uint8_t wpath[128];
                        cpu_physical_memory_read(pa, wpath, 128);
                        printf("    PA 0x%05X: ", pa);
                        for (int c = 0; c < 126; c += 2) {
                            if (wpath[c] == 0 && wpath[c+1] == 0) break;
                            if (wpath[c] >= 0x20 && wpath[c] < 0x7F && wpath[c+1] == 0)
                                printf("%c", wpath[c]);
                            else
                                printf("?");
                        }
                        printf("\n");
                        if (++udev_count >= 20) { printf("    ... (truncated)\n"); break; }
                        pa += 16;
                    }
                }
                if (udev_count == 0) printf("    NONE FOUND\n");
            }

            /* 23. UNICODE CdRom + Harddisk scan — specifically look for these */
            {
                printf("  UNICODE KEY STRINGS:\n");
                /* CdRom in UTF-16LE: 43006400520068006F006D00 */
                uint8_t cdrom_u16[] = {0x43,0x00,0x64,0x00,0x52,0x00,0x6F,0x00,0x6D,0x00};
                /* Harddisk: 48006100720064006400 */
                uint8_t hddisk_u16[] = {0x48,0x00,0x61,0x00,0x72,0x00,0x64,0x00,0x64,0x00};
                int found_cd = 0, found_hd = 0;
                for (uint32_t pa = 0x10000; pa < 0x800000 - 12; pa += 2) {
                    uint8_t buf[10];
                    cpu_physical_memory_read(pa, buf, 10);
                    if (!found_cd && memcmp(buf, cdrom_u16, 10) == 0) {
                        uint8_t ctx[64];
                        cpu_physical_memory_read(pa, ctx, 64);
                        printf("    'CdRom'(u16) PA 0x%05X: ", pa);
                        for (int c = 0; c < 62; c += 2) {
                            if (ctx[c]==0 && ctx[c+1]==0) break;
                            printf("%c", (ctx[c]>=0x20 && ctx[c]<0x7F && ctx[c+1]==0) ? ctx[c] : '.');
                        }
                        printf("\n");
                        found_cd = 1;
                    }
                    if (!found_hd && memcmp(buf, hddisk_u16, 10) == 0) {
                        uint8_t ctx[64];
                        cpu_physical_memory_read(pa, ctx, 64);
                        printf("    'Hardd'(u16) PA 0x%05X: ", pa);
                        for (int c = 0; c < 62; c += 2) {
                            if (ctx[c]==0 && ctx[c+1]==0) break;
                            printf("%c", (ctx[c]>=0x20 && ctx[c]<0x7F && ctx[c+1]==0) ? ctx[c] : '.');
                        }
                        printf("\n");
                        found_hd = 1;
                    }
                }
                if (!found_cd) printf("    'CdRom'(u16): NOT FOUND\n");
                if (!found_hd) printf("    'Hardd'(u16): NOT FOUND\n");
            }

            /* 24. QuickReboot / HalReturnToFirmware state */
            {
                printf("  REBOOT STATE:\n");
                /* SMC scratch register (HalReturnToFirmware writes here) */
                /* Check PA 0x00050000-0x00060000 for QuickReboot magic */
                /* Xbox LaunchDataPage is at a fixed kernel export address */
                /* Scan for the QuickReboot magic 0x01 at various kernel locations */
                uint32_t reboot_vas[] = {0x80038000, 0x80039000, 0x8003A000, 0x8003B000,
                                         0x8003C000, 0x8003D000, 0x8003E000, 0x8003F000};
                for (int rv = 0; rv < 8; rv++) {
                    uint32_t rpa = chihiro_va_to_pa(reboot_vas[rv]);
                    if (rpa != 0xFFFFFFFF) {
                        uint8_t rd[16];
                        cpu_physical_memory_read(rpa, rd, 16);
                        /* Only print if non-zero */
                        int nz = 0;
                        for (int i = 0; i < 16; i++) if (rd[i]) nz = 1;
                        if (nz) {
                            printf("    @0x%08X: %02X%02X%02X%02X %02X%02X%02X%02X"
                                   " %02X%02X%02X%02X %02X%02X%02X%02X\n",
                                   reboot_vas[rv],
                                   rd[0],rd[1],rd[2],rd[3],rd[4],rd[5],rd[6],rd[7],
                                   rd[8],rd[9],rd[10],rd[11],rd[12],rd[13],rd[14],rd[15]);
                        }
                    }
                }
            }

            /* 25. IDE drive identity — what does the kernel see on IDE1? */
            {
                printf("  IDE1 IDENTITY CHECK:\n");
                /* Read the IDE status registers directly from PCI config space */
                /* BAR4 for IDE is at PCI 0:9.0 offset 0x20 */
                /* Check IDE secondary status at I/O 0x170-0x177 */
                /* Actually, just check if IDE unit 1 responded to IDENTIFY */
                /* Look at kernel memory for the IDENTIFY response data */
                /* The kernel stores disk geometry info after IDE enumeration */
                /* Search for "QEMU" or disk model string in kernel memory */
                int found_ident = 0;
                for (uint32_t pa = 0x10000; pa < 0x800000 - 8; pa += 2) {
                    uint8_t buf[8];
                    cpu_physical_memory_read(pa, buf, 8);
                    if (buf[0]=='Q' && buf[1]=='E' && buf[2]=='M' && buf[3]=='U') {
                        uint8_t ctx[48];
                        cpu_physical_memory_read(pa, ctx, 48);
                        printf("    'QEMU' at PA 0x%05X: ", pa);
                        for (int c = 0; c < 44; c++)
                            printf("%c", ctx[c] >= 0x20 && ctx[c] < 0x7F ? ctx[c] : '.');
                        printf("\n");
                        found_ident = 1;
                        break;
                    }
                }
                if (!found_ident) printf("    'QEMU' ident string: NOT FOUND\n");
            }

            printf("============================\n");
        }

        /* === PERIODIC SUMMARY === */
        if (game_diag_count <= 5 || (game_diag_count % 10) == 0) {
            uint32_t pgraph_status = 0, pcrtc_start = 0, pcrtc_intr = 0;
            uint32_t pramdac_vdisp = 0, pramdac_hdisp = 0;
            address_space_read(&address_space_memory, 0xFD400700, MEMTXATTRS_UNSPECIFIED, &pgraph_status, 4);
            address_space_read(&address_space_memory, 0xFD600800, MEMTXATTRS_UNSPECIFIED, &pcrtc_start, 4);
            address_space_read(&address_space_memory, 0xFD600100, MEMTXATTRS_UNSPECIFIED, &pcrtc_intr, 4);
            address_space_read(&address_space_memory, 0xFD680800, MEMTXATTRS_UNSPECIFIED, &pramdac_vdisp, 4);
            address_space_read(&address_space_memory, 0xFD680820, MEMTXATTRS_UNSPECIFIED, &pramdac_hdisp, 4);
            printf("[%07lld] GAME DIAG #%d: PGRAPH=0x%08X PCRTC=0x%08X/%u VIDEO=%ux%u\n",
                   TS_MS, game_diag_count, pgraph_status, pcrtc_start, pcrtc_intr,
                   (pramdac_hdisp & 0xFFF) + 1, (pramdac_vdisp & 0xFFF) + 1);
        }

        timer_mod(s->diag_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
        return;
    }

    uint32_t state = 0, counter = 0, ready = 0, gate = 0, bootstate = 0;
    uint32_t bootflag = 0, slotcount = 0, mainflag = 0;
    uint8_t slotflag0 = 0;
    uint32_t xbe2_d07a8 = 0, xbe2_d0798 = 0;
    uint32_t xbe2_ce_state = 0;

    /* RE-RESOLVE PAs every tick to detect page-table changes.
     * If the game changes CR3 after initial resolution, stale PAs would read wrong data. */
    uint32_t state_pa     = chihiro_va_to_pa(0x87AFC);
    uint32_t counter_pa   = chihiro_va_to_pa(0x87AE8);
    uint32_t ready_pa     = chihiro_va_to_pa(0x87AF8);
    uint32_t gate_pa      = chihiro_va_to_pa(0x89C38);
    uint32_t bootstate_pa = chihiro_va_to_pa(0x89C48);
    uint32_t bootflag_pa  = chihiro_va_to_pa(0x89C4C);  /* MbcomNegotiate flag (0x21 on success) */
    uint32_t slotcount_pa = chihiro_va_to_pa(0x896A8);  /* Mbcom slot count */
    uint32_t slotflag0_pa = chihiro_va_to_pa(0x896B4);  /* Slot[0].flag */
    uint32_t mainflag_pa  = chihiro_va_to_pa(0x8A128);  /* MainUpdate [0x8A128] */

    /* Log if any PA changed since last tick */
    if (s->diag_bootstate_pa && bootstate_pa != s->diag_bootstate_pa) {
        printf("[%07lld] DIAG: PA CHANGED! boot PA=0x%X→0x%X\n", TS_MS,
               s->diag_bootstate_pa, bootstate_pa);
    }

    s->diag_state_pa   = state_pa;
    s->diag_counter_pa = counter_pa;
    s->diag_ready_pa   = ready_pa;
    s->diag_gate_pa    = gate_pa;
    s->diag_bootstate_pa = bootstate_pa;

    if (state_pa != 0xFFFFFFFF) cpu_physical_memory_read(state_pa, &state, 4);
    if (counter_pa != 0xFFFFFFFF) cpu_physical_memory_read(counter_pa, &counter, 4);
    if (ready_pa != 0xFFFFFFFF) cpu_physical_memory_read(ready_pa, &ready, 4);
    if (gate_pa != 0xFFFFFFFF) cpu_physical_memory_read(gate_pa, &gate, 4);
    if (bootstate_pa != 0xFFFFFFFF) cpu_physical_memory_read(bootstate_pa, &bootstate, 4);
    if (bootflag_pa != 0xFFFFFFFF) cpu_physical_memory_read(bootflag_pa, &bootflag, 4);
    if (slotcount_pa != 0xFFFFFFFF) cpu_physical_memory_read(slotcount_pa, &slotcount, 4);
    if (slotflag0_pa != 0xFFFFFFFF) cpu_physical_memory_read(slotflag0_pa, &slotflag0, 1);
    if (mainflag_pa != 0xFFFFFFFF) cpu_physical_memory_read(mainflag_pa, &mainflag, 4);

    /* v202: Thread handle [0x8A134] and thread state [0x8A138] */
    uint32_t thread_handle = 0, thread_state = 0;
    uint32_t thread_handle_pa = chihiro_va_to_pa(0x8A134);
    uint32_t thread_state_pa  = chihiro_va_to_pa(0x8A138);
    if (thread_handle_pa != 0xFFFFFFFF) cpu_physical_memory_read(thread_handle_pa, &thread_handle, 4);
    if (thread_state_pa != 0xFFFFFFFF)  cpu_physical_memory_read(thread_state_pa, &thread_state, 4);

    /* v202: USB NAK/bulk counters */
    uint32_t qc_nak = 0, qc_bin = 0, qc_bout = 0;
    uint32_t sc_nak = 0, sc_bin = 0, sc_bout = 0;
    if (chihiro_usb_qc) {
        chihiro_usb_get_counters(chihiro_usb_qc, &qc_nak, &qc_bin, &qc_bout);
        chihiro_usb_reset_counters(chihiro_usb_qc);
    }
    if (chihiro_usb_sc) {
        chihiro_usb_get_counters(chihiro_usb_sc, &sc_nak, &sc_bin, &sc_bout);
        chihiro_usb_reset_counters(chihiro_usb_sc);
    }

    /* XBE2 game state — D07A8 must reach 4 for game to advance */
    uint32_t d07a8_pa = chihiro_va_to_pa(0xD07A8);
    uint32_t d0798_pa = chihiro_va_to_pa(0xD0798);
    if (d07a8_pa != 0xFFFFFFFF) cpu_physical_memory_read(d07a8_pa, &xbe2_d07a8, 4);
    if (d0798_pa != 0xFFFFFFFF) cpu_physical_memory_read(d0798_pa, &xbe2_d0798, 4);

    /* XBE2 CE state machine at [0xCB9EC] — mirrors SEGABOOT's CE at [0x87AFC] */
    uint32_t xbe2_ce_pa = chihiro_va_to_pa(0xCB9EC);
    if (xbe2_ce_pa != 0xFFFFFFFF) cpu_physical_memory_read(xbe2_ce_pa, &xbe2_ce_state, 4);

    /* Detect bootstate changes between ticks (guard against garbage VAs) */
    if (bootstate != s->last_bootstate && bootstate < 100 && s->last_bootstate < 100) {
        printf("[%07lld] DIAG: *** BOOTSTATE CHANGED %u → %u ***\n", TS_MS,
               s->last_bootstate, bootstate);
        if (bootstate == 3) {
            chihiro_boot3_reached = true;
            /* Save game filename from boot.id (loaded by SEGABOOT at PA 0x4F000).
             * boot.id structure: magic "BTID", "XBAM" at +0x20,
             * gameExecutable at +0xA0 (e.g. "\hod3xb.xbe").
             * Fallback: scan RAM for ".xbe" if boot.id not found. */
            chihiro_game_filename[0] = 0;

            /* Try boot.id first */
            {
                uint8_t btid[4];
                cpu_physical_memory_read(0x4F000, btid, 4);
                if (memcmp(btid, "BTID", 4) == 0) {
                    uint8_t xbam[4];
                    cpu_physical_memory_read(0x4F020, xbam, 4);
                    if (memcmp(xbam, "XBAM", 4) == 0) {
                        uint8_t game_exec[32] = {0};
                        cpu_physical_memory_read(0x4F0A0, game_exec, 31);
                        char *name = (char*)game_exec;
                        while (*name == '\\' || *name == '/') name++;
                        if (name[0] && strlen(name) < 60) {
                            strncpy(chihiro_game_filename, name, 63);
                            chihiro_game_filename[63] = 0;
                            printf("[%07lld] Chihiro: game filename from boot.id: '%s'\n",
                                   TS_MS, chihiro_game_filename);
                        }
                    }
                }
            }

            /* Fallback: scan for .xbe in SEGABOOT data */
            if (!chihiro_game_filename[0]) {
                for (uint32_t pa = 0x50000; pa < 0x56000; pa++) {
                    uint8_t buf[4];
                    cpu_physical_memory_read(pa, buf, 4);
                    if (memcmp(buf, ".xbe", 4) == 0 ||
                        memcmp(buf, ".XBE", 4) == 0) {
                        int start = 0;
                        uint8_t fname[64];
                        for (int back = 1; back <= 42; back++) {
                            uint8_t c;
                            cpu_physical_memory_read(pa - back, &c, 1);
                            if (c < 0x20 || c >= 0x7F || c == '\\' ||
                                c == '/' || c == ':') {
                                start = back - 1;
                                break;
                            }
                            start = back;
                        }
                        if (start > 0) {
                            cpu_physical_memory_read(pa - start, fname, start + 4);
                            fname[start + 4] = 0;
                            int len = start + 4;
                            if (len > 4 && len < 60) {
                                memcpy(chihiro_game_filename, fname, len + 1);
                                printf("[%07lld] Chihiro: game filename from scan: '%s'\n",
                                       TS_MS, chihiro_game_filename);
                                break;
                            }
                        }
                    }
                }
            }
            if (!chihiro_game_filename[0]) {
                printf("[%07lld] Chihiro: WARNING — could not find game "
                       "filename in RAM at boot=3\n", TS_MS);
            }

            /* v208: Capture LaunchDataPage PA NOW while page tables are valid.
             * v209b: Create the LDP PTE NOW so XLaunchNewImage can write to it.
             * SEGABOOT calls XLaunchNewImage after boot=3. Without the PTE,
             * the write page-faults → XLaunchNewImage fails → kernel falls
             * back to xboxdash.xbe → doesn't exist → black screen. */
            {
                uint32_t ldp_va = 0;
                cpu_physical_memory_read(0x30A10, &ldp_va, 4);
                if (ldp_va) {
                    uint32_t ldp_page = ldp_va & ~0xFFF;
                    uint32_t pte_idx = (ldp_va >> 12) & 0x3FF;
                    uint32_t pde;
                    cpu_physical_memory_read(0xF000, &pde, 4);
                    uint32_t pt_base = pde & 0xFFFFF000;
                    uint32_t pte_addr = pt_base + pte_idx * 4;
                    uint32_t pte_val;
                    cpu_physical_memory_read(pte_addr, &pte_val, 4);

                    if (!(pte_val & 1)) {
                        uint32_t new_pte = ldp_page | 0x067;
                        cpu_physical_memory_write(pte_addr, &new_pte, 4);
                        uint8_t zeros[4096] = {0};
                        cpu_physical_memory_write(ldp_page, zeros, 4096);
                        chihiro_ldp_pa = ldp_page + (ldp_va & 0xFFF);
                        printf("[%07lld] Chihiro: Created LDP PTE 0x%08X @ PA 0x%05X "
                               "(VA 0x%08X → PA 0x%08X)\n",
                               TS_MS, new_pte, pte_addr, ldp_va, chihiro_ldp_pa);

                        /* v209c: Fill the LDP NOW at boot=3, BEFORE QuickReboot.
                         * The kernel reads LDP within microseconds of reboot.
                         * If we wait until game-detect (port 0x40F0), it's 4.5s
                         * too late — kernel already fell back to xboxdash.xbe. */
                        if (chihiro_game_filename[0]) {
                            uint32_t launch_type = 1;
                            cpu_physical_memory_write(chihiro_ldp_pa, &launch_type, 4);
                            uint32_t title_id = 0;
                            cpu_physical_memory_write(chihiro_ldp_pa + 4, &title_id, 4);
                            char launch_path[520] = {0};
                            snprintf(launch_path, sizeof(launch_path),
                                     "D:\\%s", chihiro_game_filename);
                            cpu_physical_memory_write(chihiro_ldp_pa + 8, launch_path, 520);
                            printf("[%07lld] Chihiro: Pre-filled LDP at boot=3: "
                                   "type=1 path='%s'\n", TS_MS, launch_path);
                        }
                    } else {
                        chihiro_ldp_pa = (pte_val & 0xFFFFF000) | (ldp_va & 0xFFF);
                        printf("[%07lld] Chihiro: LDP already mapped PTE=0x%08X PA=0x%08X\n",
                               TS_MS, pte_val, chihiro_ldp_pa);
                    }
                } else {
                    chihiro_ldp_pa = 0xFFFFFFFF;
                    printf("[%07lld] Chihiro: LDP pointer is NULL\n", TS_MS);
                }
            }

            /* Dump RAM at boot=3 for offline analysis.
             * Covers kernel + pool memory (8MB). */
            {
                FILE *f = fopen("/tmp/ram_boot3.bin", "wb");
                if (f) {
                    uint8_t page[4096];
                    for (uint32_t pa = 0; pa < 0x800000; pa += 4096) {
                        cpu_physical_memory_read(pa, page, 4096);
                        fwrite(page, 1, 4096, f);
                    }
                    fclose(f);
                    printf("[%07lld] Chihiro: RAM dump saved to /tmp/ram_boot3.bin "
                           "(8MB, PA 0x00000-0x7FFFFF)\n", TS_MS);
                }
            }
        }
        s->last_bootstate = bootstate;
    }

    if (bootstate < 100) {
        printf("[%07lld] DIAG: CE st=%u cnt=%u rdy=%u | gate=%u boot=%u flag=0x%02X "
               "slots=%u/s0=0x%02X mflag=%u | 40F0=%u 401E=%u 4084=%u | tick=%u d7a8=%u ce2=%u %s\n",
               TS_MS, state, counter, ready, gate, bootstate,
               bootflag & 0xFF, slotcount, slotflag0, mainflag,
               s->lpc_40f0_reads, s->lpc_401e_reads, s->lpc_4084_reads,
               xbe2_d0798, xbe2_d07a8, xbe2_ce_state,
               (xbe2_ce_pa != 0xFFFFFFFF) ? "xbe2:mapped" : "xbe2:UNMAPPED");
    } else {
        static bool diag_va_warned = false;
        if (!diag_va_warned) {
            printf("[%07lld] DIAG: VAs invalid (boot=%u) — DIAG output suppressed\n",
                   TS_MS, bootstate);
            diag_va_warned = true;
        }
    }

    /* v206b: Read-only monitoring of baseboard_dev[].flags.
     * fpr-23887: baseboard_dev[] at VA 0xA3778, stride 0x218, flags at offset +4.
     * fpr-21042: channel table at VA 0xE9BD0, same stride/flags layout.
     * Auto-detect: try fpr-21042 VA first, fall back to fpr-23887. */
    {
        uint32_t dev0_base_va = 0xE9BD0;
        uint32_t dev0_test_pa = chihiro_va_to_pa(dev0_base_va);
        if (dev0_test_pa == 0xFFFFFFFF) {
            dev0_base_va = 0xA3778;  /* fpr-23887 fallback */
        }
        uint32_t dev1_base_va = dev0_base_va + 0x218;
        uint32_t dev0_flags_pa = chihiro_va_to_pa(dev0_base_va + 4);
        uint32_t dev1_flags_pa = chihiro_va_to_pa(dev1_base_va + 4);

        /* Read first 16 bytes of each baseboard_dev entry for full context */
        uint8_t dev0_raw[16] = {0}, dev1_raw[16] = {0};
        uint32_t dev0_base_pa = chihiro_va_to_pa(dev0_base_va);
        uint32_t dev1_base_pa = chihiro_va_to_pa(dev1_base_va);
        if (dev0_base_pa != 0xFFFFFFFF)
            cpu_physical_memory_read(dev0_base_pa, dev0_raw, 16);
        if (dev1_base_pa != 0xFFFFFFFF)
            cpu_physical_memory_read(dev1_base_pa, dev1_raw, 16);

        uint32_t dev0_flags = *(uint32_t *)(dev0_raw + 4);
        uint32_t dev1_flags = *(uint32_t *)(dev1_raw + 4);

        printf("[%07lld] DIAG USB-DEV: dev0[%05X→PA %X] raw=%02X%02X%02X%02X "
               "FLAGS=0x%08X %02X%02X%02X%02X %02X%02X%02X%02X | "
               "dev1[%05X→PA %X] FLAGS=0x%08X\n",
               TS_MS,
               dev0_base_va, dev0_base_pa,
               dev0_raw[0], dev0_raw[1], dev0_raw[2], dev0_raw[3],
               dev0_flags,
               dev0_raw[8], dev0_raw[9], dev0_raw[10], dev0_raw[11],
               dev0_raw[12], dev0_raw[13], dev0_raw[14], dev0_raw[15],
               dev1_base_va, dev1_base_pa,
               dev1_flags);
    }

    /* v202: Thread + USB counters (only print if any activity or thread exists) */
    if (thread_handle || thread_state || qc_nak || sc_nak || qc_bin || sc_bin || qc_bout || sc_bout) {
        printf("[%07lld] DIAG USB: thr=0x%X tst=%u | QC nak=%u in=%u out=%u | SC nak=%u in=%u out=%u\n",
               TS_MS, thread_handle, thread_state,
               qc_nak, qc_bin, qc_bout, sc_nak, sc_bin, sc_bout);
    }

    /* v159 DIAG: key addresses for boot data flow */
    if (bootstate == 2) {
        uint32_t initptr_pa = chihiro_va_to_pa(0x896AC);
        uint32_t slot0_pa   = chihiro_va_to_pa(0x89740);
        uint32_t slot0d_pa  = chihiro_va_to_pa(0x89760);
        uint32_t initptr = 0, slot0_meta = 0, slot0_data = 0;
        if (initptr_pa != 0xFFFFFFFF) cpu_physical_memory_read(initptr_pa, &initptr, 4);
        if (slot0_pa != 0xFFFFFFFF) cpu_physical_memory_read(slot0_pa, &slot0_meta, 4);
        if (slot0d_pa != 0xFFFFFFFF) cpu_physical_memory_read(slot0d_pa, &slot0_data, 4);
        static int boot2_diag_count = 0;
        boot2_diag_count++;
        if (boot2_diag_count <= 5 || (boot2_diag_count % 10) == 0) {
            printf("[%07lld] DIAG boot=2: initPtr=[896AC]=0x%08X slot0meta=[89740]=0x%08X "
                   "slot0data=[89760]=0x%08X\n",
                   TS_MS, initptr, slot0_meta, slot0_data);
        }
    }

    /* v177 DIAG: NV2A video mode at boot=3 (CAUTION state) — measure before coding */
    if (bootstate == 3) {
        static int boot3_diag_done = 0;
        if (!boot3_diag_done) {
            boot3_diag_done = 1;
            /* NV2A PRAMDAC: physical 0xFD680000 + offset */
            uint32_t pramdac_base = 0xFD680000;
            uint32_t pcrtc_base   = 0xFD600000;
            uint32_t prmcio_base  = 0xFD601000;
            uint32_t vdisp = 0, hdisp = 0, vsync = 0, vvalid = 0, hvalid = 0;
            uint32_t vpll = 0, pcrtc_cfg = 0;
            uint8_t interlace = 0;

            address_space_read(&address_space_memory, pramdac_base + 0x800,
                               MEMTXATTRS_UNSPECIFIED, &vdisp, 4);
            address_space_read(&address_space_memory, pramdac_base + 0x810,
                               MEMTXATTRS_UNSPECIFIED, &vsync, 4);
            address_space_read(&address_space_memory, pramdac_base + 0x818,
                               MEMTXATTRS_UNSPECIFIED, &vvalid, 4);
            address_space_read(&address_space_memory, pramdac_base + 0x820,
                               MEMTXATTRS_UNSPECIFIED, &hdisp, 4);
            address_space_read(&address_space_memory, pramdac_base + 0x838,
                               MEMTXATTRS_UNSPECIFIED, &hvalid, 4);
            address_space_read(&address_space_memory, pramdac_base + 0x508,
                               MEMTXATTRS_UNSPECIFIED, &vpll, 4);
            address_space_read(&address_space_memory, pcrtc_base + 0x804,
                               MEMTXATTRS_UNSPECIFIED, &pcrtc_cfg, 4);
            address_space_read(&address_space_memory, prmcio_base + 0x39,
                               MEMTXATTRS_UNSPECIFIED, &interlace, 1);

            printf("[%07lld] DIAG NV2A VIDEO MODE:\n"
                   "  VDISPLAY_END=0x%X VSYNC_END=0x%X VVALID_END=0x%X\n"
                   "  HDISPLAY_END=0x%X HVALID_END=0x%X\n"
                   "  VPLL_COEFF=0x%08X PCRTC_CONFIG=0x%08X INTERLACE=0x%02X\n",
                   TS_MS, vdisp, vsync, vvalid, hdisp, hvalid,
                   vpll, pcrtc_cfg, interlace);

            /* Dump DMA DATA slots 0-3 to see what SEGABOOT wrote */
            for (int ds = 0; ds < 4; ds++) {
                uint32_t ds_pa = chihiro_va_to_pa(0x89760 + ds * 0x40);
                if (ds_pa != 0xFFFFFFFF) {
                    uint8_t dsbuf[16];
                    cpu_physical_memory_read(ds_pa, dsbuf, 16);
                    printf("  DMA DATA slot%d: %02X %02X %02X %02X %02X %02X %02X %02X"
                           " %02X %02X %02X %02X %02X %02X %02X %02X\n",
                           ds, dsbuf[0], dsbuf[1], dsbuf[2], dsbuf[3],
                           dsbuf[4], dsbuf[5], dsbuf[6], dsbuf[7],
                           dsbuf[8], dsbuf[9], dsbuf[10], dsbuf[11],
                           dsbuf[12], dsbuf[13], dsbuf[14], dsbuf[15]);
                }
                uint32_t ms_pa = chihiro_va_to_pa(0x89740 + ds * 0x40);
                if (ms_pa != 0xFFFFFFFF) {
                    uint8_t msbuf[12];
                    cpu_physical_memory_read(ms_pa, msbuf, 12);
                    printf("  DMA META slot%d: %02X %02X %02X %02X %02X %02X %02X %02X"
                           " %02X %02X %02X %02X\n",
                           ds, msbuf[0], msbuf[1], msbuf[2], msbuf[3],
                           msbuf[4], msbuf[5], msbuf[6], msbuf[7],
                           msbuf[8], msbuf[9], msbuf[10], msbuf[11]);
                }
            }
        }
    }

    timer_mod(s->diag_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void chihiro_usb_poll_patch_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    if (s->usb_poll_patched) return;

    /* v274: UsbEnumPoll + RegisterClassDriver patches REMOVED.
     * These patches broke USB enumeration: NOPing the polling loop prevented
     * the kernel from completing SET_CONFIG → no RegisterClassDriver →
     * bit 0x20 never set → all JVS communication failed.
     * Fix: let SEGABOOT run unmodified; USB devices in chihiro-usb.c
     * handle enumeration via OHCI natively. */
    /* static const uint8_t sig_enumpoll[] = {
        0xE8, 0x64, 0xF6, 0xFF, 0xFF, 0x85, 0xC0, 0x74, 0x0F
    }; */
    /* static const uint8_t sig_enumpoll_21042[] = {
        0xE8, 0xEF, 0xF1, 0xFF, 0xFF, 0x85, 0xC0, 0x0F, 0x85
    }; */
    /* static const uint8_t sig_classdrv[] = {
        0xE8, 0x27, 0x54, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x0F
    }; */
    /* static const uint8_t sig_classdrv_21042[] = {
        0xE8, 0x33, 0x95, 0x00, 0x00, 0x85, 0xC0, 0x74, 0x0F
    }; */

    /*
     * Patch 3: GetQcStatusByte0 (VA 0x3AD80)
     *
     *   E8 2B 6E 01 00    call GetQcStatus (0x51BB0)
     *   0F B6 00          movzx eax, byte ptr [eax]
     *   C3                ret
     *
     * Patch: replace first 3 bytes with 31 C0 C3 (xor eax,eax; ret)
     * -> always returns 0 -> caller takes CreateThread path
     */
    static const uint8_t sig_qcbyte0[] = {
        0xE8, 0x2B, 0x6E, 0x01, 0x00,  /* call GetQcStatus      */
        0x0F, 0xB6, 0x00,              /* movzx eax, byte [eax]  */
        0xC3                            /* ret                    */
    };

    /* fpr-21042 variant — different call offset */
    static const uint8_t sig_qcbyte0_21042[] = {
        0xE8, 0xEB, 0x96, 0x01, 0x00,  /* call 0x44460           */
        0x0F, 0xB6, 0x00,              /* movzx eax, byte [eax]  */
        0xC3                            /* ret                    */
    };

    /*
     * Patch 4: CreateThread return check (VA 0x425DE) — REMOVED in v201
     *   Was: jne→jmp to always skip past CreateThread failure handling
     *   Now: let CreateThread execute — USB poll thread will be created
     */
    /* static const uint8_t sig_createthread[] = {
        0x85, 0xC0,
        0xA3, 0x34, 0xA1, 0x08, 0x00,
        0x5B,
        0x75, 0x12
    }; */

    /*
     * Patch 5 (DIAGNOSTIC): Error value at VA 0x2E3AB
     *   C7 07 14 00 00 00    mov [edi], 0x14  (error code)
     */
    static const uint8_t sig_errval[] = {
        0xC7, 0x07, 0x14, 0x00, 0x00, 0x00   /* mov [edi], 0x14 */
    };

    /*
     * Patch 6: UsbPollQC_inner (VA 0x51140) — REMOVED in v200
     *   Was: xor eax,eax; ret 4 → always return 0
     *   Now: let real USB poll function execute (Phase 2 Option 3)
     */
    /* static const uint8_t sig_usbpollqc[] = {
        0xE9, 0xFB, 0xFE, 0xFF, 0xFF,
        0x90, 0x90, 0x90, 0x90
    }; */

    /*
     * Patch 7: UsbPollSC_inner (VA 0x51150) — REMOVED in v200
     *   Was: xor eax,eax; ret 4 → always return 0
     *   Now: let real USB poll function execute (Phase 2 Option 3)
     */
    /* static const uint8_t sig_usbpollsc[] = {
        0x55,
        0x8B, 0xEC,
        0x83, 0xE4, 0xF8,
        0x81, 0xEC, 0x0C, 0x03, 0x00, 0x00
    }; */

    /*
     * Patch 8: EncryptionCheck USB transfer result (VA 0x3A953)
     *   33 C9        xor ecx, ecx
     *   85 C0        test eax, eax    ← patch offset 2: change to 31 C0 (xor eax,eax)
     *   0F 9D C1     setge cl
     *   5F           pop edi
     *   49           dec ecx
     *   83 E1 02     and ecx, 2
     *
     * EncryptionCheck calls USB transfer (0x51340→0x1A0B0) which fails because
     * the AN2131 class driver was never registered. This forces eax=0 (success).
     */
    static const uint8_t sig_enccheck[] = {
        0x33, 0xC9,                            /* xor ecx, ecx       */
        0x85, 0xC0,                            /* test eax, eax      */
        0x0F, 0x9D, 0xC1,                      /* setge cl           */
        0x5F,                                  /* pop edi            */
        0x49,                                  /* dec ecx            */
        0x83, 0xE1, 0x02                       /* and ecx, 2         */
    };

    /*
     * Patch 9: MbcomPollReady (VA 0x3DBC0) — always return 1
     *   8A 44 24 04       mov al, [esp+4]
     *   E8 67 FD FF FF    call FindSlot (0x3D930)
     *   85 C0             test eax, eax
     *   74 0E             je 0x3DBDB
     *   33 C9             xor ecx, ecx
     *
     * On real HW, MbcomPollReady returns 1 only when baseboard DMAs a response into
     * slot[+2]. We do not emulate that DMA, so it always returns 0 and MbcomSetParam
     * spins forever at 0x3E530. Patch makes it always return 1 so MbcomSetParam
     * completes and boot_state advances from 2 to 3.
     * v153: REMOVED — TX scan + DMA inject handles this now.
     */

    /*
     * Patch 10: GetBootData (VA 0x41880) — always return 1
     *   A1 50 9C 08 00    mov eax, [0x89C50]
     *   85 C0             test eax, eax
     *   74 0F             je 0x41898
     *   83 3D 48 9C 08 00 03  cmp dword [0x89C48], 3
     *
     * GetBootData returns [0x89C50]+0xFF000000 if boot==3 and [0x89C50]!=0, else 0.
     * With our MbcomSetParam stub, [0x89C50] is never populated with real data.
     * ErrorDisplay state=3 checks GetBootData==0 AND flag==0x21 → sets ERROR 27 after
     * 2400 ticks. Patching GetBootData to return non-zero avoids ERROR 27.
     * v153: REMOVED — let real boot data flow through.
     */

    /*
     * Patch 11: CheckMainBoardSerial (VA 0x2EC35) — return 0 instead of 3
     *
     * At the serial format validation failure path:
     *   0x2EC31: 85 C0             test eax, eax     (MatchSerialFormat result)
     *   0x2EC33: 75 D6             jne 0x2EC0B       (format OK → skip)
     *   0x2EC35: B8 03 00 00 00    mov eax, 3        ← PATCH (error code 3)
     *   0x2EC3A: 5E                pop esi
     *   0x2EC3B: C3                ret
     *
     * CheckMainBoardSerial (0x2EBF0) calls MatchSerialFormat with the expected format
     * pattern "%%%@-##@########" (3 letters, 1 alphanum, '-', 2 digits, 1 alphanum,
     * 8 digits = 16 bytes, matching e.g. "AAEE-01D44744715"). Our mbcom stub returns
     * zeros in slot data, so the serial read by the game is 16 zero bytes — fails
     * format match → returns 3 → state-machine sets err_code=3 → displays ERROR 03.
     *
     * Replace `mov eax, 3` with `xor eax, eax; nop*3` (same 5 bytes) so the function
     * returns 0 (no error) even on format mismatch.
     */
    static const uint8_t sig_check_mainserial[] = {
        0x85, 0xC0,                            /* test eax, eax      */
        0x75, 0xD6,                            /* jne 0x2EC0B        */
        0xB8, 0x03, 0x00, 0x00, 0x00,          /* mov eax, 3         */
        0x5E,                                  /* pop esi            */
        0xC3                                   /* ret                */
    };

    /* fpr-21042 variant — different jne offset + call prefix to avoid false positive */
    static const uint8_t sig_check_mainserial_21042[] = {
        0xE8, 0xB4, 0xF7, 0xFF, 0xFF,         /* call CheckSerial      */
        0x85, 0xC0,                            /* test eax, eax      */
        0x75, 0x07,                            /* jne +0x07          */
        0xB8, 0x03, 0x00, 0x00, 0x00,          /* mov eax, 3         */
        0x5E,                                  /* pop esi            */
        0xC3                                   /* ret                */
    };

    /*
     * Patch 12: CheckMediaBoardSerial (VA 0x2EC88) — return 0 instead of 4
     *
     * Same structure as main serial check, but for media board. Lives in function
     * CheckMediaBoardSerial (0x2EC40) which also calls MatchSerialFormat with
     * "%%%@-##@########":
     *   0x2EC83: 85 C0             test eax, eax
     *   0x2EC85: 75 0A             jne 0x2EC91
     *   0x2EC87: 5F                pop edi
     *   0x2EC88: B8 04 00 00 00    mov eax, 4        ← PATCH (error code 4)
     *   0x2EC8D: 5E                pop esi
     *   0x2EC8E: C2 04 00          ret 4
     *
     * Without this patch, fixing error 3 would just reveal error 4 next (Bad serial
     * number on media board). Replace same way as patch 11.
     */
    static const uint8_t sig_check_mediaserial[] = {
        0x85, 0xC0,                            /* test eax, eax      */
        0x75, 0x0A,                            /* jne 0x2EC91        */
        0x5F,                                  /* pop edi            */
        0xB8, 0x04, 0x00, 0x00, 0x00,          /* mov eax, 4         */
        0x5E,                                  /* pop esi            */
        0xC2, 0x04, 0x00                       /* ret 4              */
    };

    /* v176: REMOVED AV NOP patches (11+12). Proper fix: EEPROM video_standard
     * now includes AV_FLAGS_HDTV_480p (0x00080000) so the kernel configures
     * NV2A for progressive scan 31kHz. SEGABOOT's video check passes naturally. */

    /* v207b: TDBuilder NOP patches REMOVED — diagnostic only, proven ineffective
     * in v206. Kept as documentation of the RE finding. */

    /* Patch byte arrays */
    /* v274: patch_jmp removed — was only used by UsbEnumPoll/RegisterClassDriver patches */
    /* static const uint8_t patch_jmp[] = { 0xEB }; */
    static const uint8_t patch_and0[]  = { 0x00 };
    static const uint8_t patch_xor_ret[] = { 0x31, 0xC0, 0xC3 };
    /* v200: patch_xor_ret4 removed — was only used by UsbPollQC/SC patches */
    /* static const uint8_t patch_xor_ret4[] = { 0x31, 0xC0, 0xC2, 0x04, 0x00 }; */
    static const uint8_t patch_xor_nop3[]  = { 0x31, 0xC0, 0x90, 0x90, 0x90 }; /* xor eax, eax; nop*3 */
    /* v274: patch_nop6 removed — was only used by UsbEnumPoll 21042 patch */
    /* static const uint8_t patch_nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }; */
    /* v269: REMOVED patch_xor_ret8 + patch_mov1_ret — mbcom bypass patches reverted.
     * mbcom_main_init must run LLE. Baseboard emulation handles its IDE commands. */

    ChihiroPatch patches[] = {
        /* v274: UsbEnumPoll + RegisterClassDriver patches REMOVED — broke USB enumeration.
         * SEGABOOT runs unmodified; USB devices handle OHCI natively. */
        /* { sig_enumpoll, ..., "UsbEnumPoll check (je->jmp)", false }, */
        /* { sig_enumpoll_21042, ..., "UsbEnumPoll check (nop jne32) [21042]", false }, */
        /* { sig_classdrv, ..., "RegisterClassDriver check (je->jmp)", false }, */
        /* { sig_classdrv_21042, ..., "RegisterClassDriver check (je->jmp) [21042]", false }, */
        { sig_qcbyte0,  sizeof(sig_qcbyte0),  0,  patch_xor_ret, 3, 0x3AD80, "GetQcStatusByte0 (xor eax,eax; ret)", false },
        { sig_qcbyte0_21042, sizeof(sig_qcbyte0_21042), 0, patch_xor_ret, 3, 0x2AD70, "GetQcStatusByte0 (xor eax,eax; ret) [21042]", false },
        /* v201: CreateThread patch REMOVED — let USB poll thread be created */
        /* { sig_createthread, sizeof(sig_createthread), 8, patch_jmp, 1, 0x425D6, "CreateThread return (jne->jmp)", false }, */
        { sig_errval,   sizeof(sig_errval),   2,  patch_and0,    1, 0x2E3AB, "DIAG: error value 0x14->0x00",        false },
        /* v200: UsbPollQC/SC patches REMOVED — let real USB poll functions execute */
        /* { sig_usbpollqc, sizeof(sig_usbpollqc), 0, patch_xor_ret4, 5, 0x51140, "UsbPollQC_inner (xor eax,eax; ret 4)", false }, */
        /* { sig_usbpollsc, sizeof(sig_usbpollsc), 0, patch_xor_ret4, 5, 0x51150, "UsbPollSC_inner (xor eax,eax; ret 4)", false }, */
        { sig_enccheck,  sizeof(sig_enccheck),  2, patch_xor_ret, 2, 0x3A953, "EncryptionCheck (test->xor eax,eax)",  false },
        /* v153: REMOVED MbcomPollReady (was always return 1) — let clear-on-read deliver real responses */
        /* v153: REMOVED GetBootData (was always return 1) — let real boot data flow through */
        { sig_check_mainserial,  sizeof(sig_check_mainserial),  4, patch_xor_nop3, 5, 0x2EC35, "CheckMainBoardSerial (err 3 -> 0)",  false },
        { sig_check_mainserial_21042, sizeof(sig_check_mainserial_21042), 9, patch_xor_nop3, 5, 0x1EA9C, "CheckMainBoardSerial (err 3 -> 0) [21042]", false },
        { sig_check_mediaserial, sizeof(sig_check_mediaserial), 5, patch_xor_nop3, 5, 0x2EC88, "CheckMediaBoardSerial (err 4 -> 0)", false },
    };
    int num_patches = sizeof(patches) / sizeof(patches[0]);
    int applied = 0;

    uint8_t buf[4096];
    for (uint32_t pa = 0x10000; pa < 0x08000000 && applied < num_patches; pa += 4096) {
        address_space_read(&address_space_memory, pa,
                           MEMTXATTRS_UNSPECIFIED, buf, 4096);

        for (int p = 0; p < num_patches; p++) {
            if (patches[p].applied) continue;

            for (int i = 0; i <= 4096 - patches[p].length; i++) {
                if (i + patches[p].patch_offset + patches[p].patch_len > 4096)
                    continue;  /* Patch target would be off-page */
                if (memcmp(&buf[i], patches[p].bytes, patches[p].length) != 0)
                    continue;

                uint32_t patch_pa = pa + i + patches[p].patch_offset;
                address_space_write(&address_space_memory, patch_pa,
                                    MEMTXATTRS_UNSPECIFIED,
                                    patches[p].patch_bytes, patches[p].patch_len);

                patches[p].applied = true;
                applied++;

                printf("[%07lld] Chihiro: PATCH %d/%d '%s' — "
                       "sig at PA 0x%08X, patched %d byte(s) at PA 0x%08X\n",
                       TS_MS, applied, num_patches,
                       patches[p].name, pa + i, patches[p].patch_len, patch_pa);
                break;
            }
        }
    }

    if (applied >= 3) {
        s->usb_poll_patched = true;
        printf("[%07lld] Chihiro: %d/%d SEGABOOT patches applied. "
               "USB enumeration runs natively (no UsbEnumPoll bypass).\n",
               TS_MS, applied, num_patches);

        /* RAM dump for offline RE — page tables, SEGABOOT BSS, slot analysis */
        {
            FILE *f = fopen("/tmp/ram_postpatch.bin", "wb");
            if (f) {
                uint8_t page[4096];
                for (uint32_t pa = 0; pa < 0x01000000; pa += 4096) {
                    cpu_physical_memory_read(pa, page, 4096);
                    fwrite(page, 1, 4096, f);
                }
                fclose(f);
                printf("[%07lld] Chihiro: RAM dump saved to /tmp/ram_postpatch.bin (16MB)\n", TS_MS);
            }
        }

        /* Start diagnostic timer to monitor state machine progress */
        s->diag_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, chihiro_diag_timer_cb, s);
        timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
        return;
    }

    timer_mod(s->usb_poll_patch_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

/* Called from SMC SCRATCH write handler when value=0x02 (QuickReboot).
 * SEGABOOT writes SCRATCH=0x02 just before HalReturnToFirmware.
 * This is the version-independent trigger for QuickReboot detection. */
void chihiro_on_quickreboot_signal(void)
{
    if (!chihiro_active || chihiro_boot3_reached) return;

    printf("[%07lld] Chihiro: QuickReboot signal detected (SCRATCH=0x02)\n", TS_MS);
    chihiro_boot3_reached = true;

    /* Read boot.id from game directory to get game executable name */
    if (chihiro_game_dir[0] && !chihiro_game_filename[0]) {
        char bootid_path[1100];
        snprintf(bootid_path, sizeof(bootid_path), "%s/boot.id", chihiro_game_dir);
        FILE *f = fopen(bootid_path, "rb");
        if (f) {
            uint8_t bid[480];
            if (fread(bid, 1, 480, f) >= 0xC0) {
                /* gameExecutable at offset 0xA0, 32 bytes, backslash-prefixed */
                char *exec = (char *)&bid[0xA0];
                exec[31] = 0;
                /* Skip leading backslash */
                char *name = exec;
                while (*name == '\\' || *name == '/') name++;
                if (*name) {
                    strncpy(chihiro_game_filename, name, 63);
                    chihiro_game_filename[63] = 0;
                    printf("[%07lld] Chihiro: boot.id → game executable: '%s'\n",
                           TS_MS, chihiro_game_filename);
                }
            }
            fclose(f);
        }
    }

    /* Walk page tables to find LDP physical address.
     * LDP pointer is at kernel VA 0x8003B3C8 = PA 0x3B3C8.
     * But the actual LDP page at VA 0x002625A0 may not be mapped yet.
     * Save what we can; the post-reboot handler will re-create the PTE. */
    {
        uint32_t ldp_va = 0;
        cpu_physical_memory_read(0x30A10, &ldp_va, 4);  /* XeImageFileName area has LDP ptr */
        if (ldp_va == 0x002625A0) {
            uint32_t pa = chihiro_va_to_pa(ldp_va);
            if (pa != 0xFFFFFFFF) {
                chihiro_ldp_pa = pa & ~0xFFF;
                printf("[%07lld] Chihiro: LDP VA 0x%08X → PA 0x%05X\n",
                       TS_MS, ldp_va, pa);

                /* Fill LDP now (before reset wipes page tables) */
                uint32_t launch_type = 1;
                cpu_physical_memory_write(pa, &launch_type, 4);
                uint32_t title_id = 0;
                cpu_physical_memory_write(pa + 4, &title_id, 4);
                char launch_path[520] = {0};
                snprintf(launch_path, sizeof(launch_path),
                         "D:\\%s", chihiro_game_filename);
                cpu_physical_memory_write(pa + 8, launch_path, 520);
                printf("[%07lld] Chihiro: LDP filled: path='%s'\n",
                       TS_MS, launch_path);
            } else {
                printf("[%07lld] Chihiro: LDP VA 0x%08X → UNMAPPED "
                       "(will allocate after reboot)\n", TS_MS, ldp_va);
                chihiro_ldp_pa = 0x262000;  /* default PA for LDP page */
            }
        }
    }
}

/* Called from SMC handler when kernel writes SMC_REG_POWER (QuickReboot).
 * v209e: Let the reset happen! Combined with scratch_reg preservation
 * (smc_reset no longer clears scratch), the kernel will detect warm boot,
 * restore MmPersistContiguousMemory pages, and load the game from LDP.
 * Blocking the reset prevented the real QuickReboot from ever occurring. */
bool chihiro_intercept_reset(void)
{
    if (chihiro_active && chihiro_boot3_reached) {
        printf("[%07lld] Chihiro: QuickReboot — letting real reset proceed "
               "(scratch_reg preserved for warm boot)\n", TS_MS);
        /* DON'T set game_running here — 0x40F0 handler sets it
         * AFTER applying kernel device patches. */
        return false;  /* Allow qemu_system_reset_request */
    }
    return false;
}

/* TEMPORARY: Simulates PIC baseboard (sp5001.bin) periodic STATUS
 * notifications. On real hardware, the PIC continuously reports
 * game-ready status via shared memory at 0x84000000. SEGABOOT
 * polls for these events in writes_3bc_3d4 (FUN_0002ddd0) to
 * set the [app_obj+0x3BC] gate that unblocks game loading.
 *
 * TODO: Replace with proper PIC16 emulation running
 * sp5001.bin firmware for upstream LLE integration. */
static void chihiro_dimm_event_timer_cb(void *opaque)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);

    if (s->dimm_resp[0] != 0) {
        timer_mod(s->dimm_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
        return;
    }

    uint16_t seq = s->dimm_next_seq++;
    memset(s->dimm_resp, 0, sizeof(s->dimm_resp));
    s->dimm_resp[0] = seq;
    s->dimm_resp[1] = 0x8100;  /* cmd 0x0100 (STATUS) | 0x8000 */
    s->dimm_resp[2] = 5;       /* phase = READY */
    s->dimm_resp[3] = 100;     /* completion = 100% */
    s->dimm_resp_ready = true;

    printf("[%07lld] DIMM event: injected STATUS (seq=%u phase=5 pct=100)\n",
           TS_MS, seq);

    /* Reschedule — SEGABOOT polls for fresh STATUS events continuously */
    timer_mod(s->dimm_event_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
}

static void chihiro_dimm_process_cmd(ChihiroLPCState *s)
{
    uint16_t seq = s->dimm_cmd[0] & 0xFFFF;
    uint16_t cmd = (s->dimm_cmd[0] >> 16) & 0xFFFF;

    /* TEMPORARY: All response values below simulate the DIMM board PIC
     * (sp5001.bin) firmware responses. On real hardware these come from
     * the PIC after it loads/verifies the GDROM game image.
     * TODO: Replace with PIC16 emulation for upstream LLE. */
    memset(s->dimm_resp, 0, sizeof(s->dimm_resp));
    s->dimm_resp[0] = seq;
    s->dimm_resp[1] = cmd | 0x8000;

    switch (cmd) {
    case 0x0001: /* DIMM_SIZE — 512MB */
        s->dimm_resp[2] = 0x20000000;
        break;
    case 0x0100: /* STATUS — phase=5 (ready), completion=100% */
        s->dimm_resp[2] = 5;
        s->dimm_resp[3] = 100;
        break;
    case 0x0101: /* FW_VER */
        s->dimm_resp[2] = 0x0317;
        break;
    case 0x0102: /* SYSTEM_TYPE — low byte must be >=2 to pass board check */
        s->dimm_resp[2] = 0x8002;
        break;
    case 0x0103: /* SERIAL */
        memcpy(&s->dimm_resp[2], "-abc-abc12345678", 16);
        break;
    default:
        break;
    }

    s->dimm_resp_ready = true;
    s->dimm_next_seq = seq + 1;
    s->dimm_cmd_count++;

    printf("[%07lld] DIMM mailbox: cmd=0x%04X seq=%u resp[2]=0x%08X\n",
           TS_MS, cmd, seq, s->dimm_resp[2]);

    /* After the initial 5-command handshake (SIZE, FW_VER, SERIAL,
     * SYSTEM_TYPE, STATUS), schedule the unsolicited STATUS event */
    if (s->dimm_cmd_count == 5 && s->dimm_event_timer) {
        timer_mod(s->dimm_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
        printf("[%07lld] DIMM event: scheduled STATUS injection in 100ms\n",
               TS_MS);
    }
}

static uint64_t chihiro_lpc_io_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    uint64_t r = 0;

    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);

    switch (addr) {
    case 0x00: /* Port 0x4000: read baseboard register at lpc_reg_addr */
        switch (s->lpc_reg_addr) {
        /* TEMPORARY: Baseboard CPU-ready and comm-link status bits.
         * On real hardware, set by PIC16 firmware (sp5001.bin).
         * TODO: Replace with PIC16 emulation for upstream LLE. */
        case 0x80000140: r = 0x01; break;       /* CPU ready */
        case 0x80000160: r = 0x01; break;       /* comm link up */
        case 0xA0001E60: r = 0x00000020; break; /* DMA mode register */
        case 0xA0000000: {
            /* Indirect read: return value at address in bb_reg_addr (0xA0000020) */
            uint32_t target = s->bb_reg_addr;
            /* TEMPORARY: Same baseboard status bits via indirect DMA path.
             * TODO: Replace with PIC16 emulation for upstream LLE. */
            if (target == 0x80000140) {
                r = 0x01; /* CPU ready */
            } else if (target == 0x80000160) {
                r = 0x01; /* comm link up */
            } else if (target >= 0x84000000 && target <= 0x8400001C) {
                uint32_t idx = (target - 0x84000000) / 4;
                r = s->dimm_resp[idx];
            } else {
                r = 0;
            }
            s->bb_reg_addr += 4;  /* auto-increment */
            break;
        }
        case 0xA0000020: r = s->bb_reg_addr; break;
        case 0xA0000040: r = s->bb_reg_status; break;
        case 0x90000000: r = 0x01; break; /* shared memory status = ready */
        default: r = 0; break;
        }
        if (CHIHIRO_LOG) {
            printf("[%07lld] chihiro lpc reg read  [0x%08X] -> 0x%08X\n", TS_MS,
                   s->lpc_reg_addr, (unsigned)r);
        }
        return r;
    case SEGA_FIRMWARE_VERSION:
        /* TEMPORARY: DIMM base address low word. On real hardware, read
         * from baseboard PIC registers populated by sp5001.bin firmware.
         * TODO: Replace with PIC16 emulation for upstream LLE. */
        r = 0x0000;
        s->lpc_401e_reads++;
        break;
    case SEGA_XBAM_STRING_0:
        /* TEMPORARY: DIMM base address high word + XBAM signature.
         * TODO: Replace with PIC16 emulation for upstream LLE. */
        r = 0x0100;     /* 0x0100 << 16 = 0x01000000 */
        break;
    case SEGA_XBAM_STRING_1:
        r = 0x4258;     /* "BX" */
        break;
    case SEGA_XBAM_STRING_2:
        r = 0x4D41;     /* "MA" → full string reads as "XBAM" */
        break;
    case SEGA_CHIP_REVISION:
        /* TEMPORARY: Simulates baseboard presence and ready status.
         * On real hardware, driven by PIC16 firmware (sp5001.bin).
         * TODO: Replace with PIC16 emulation for upstream LLE. */
        if (chihiro_boot3_reached) {
            r = 0x0000;  /* Game kernel: mediaboard present */
        } else if (s->lpc_40f0_reads < 2) {
            r = 0x0000;  /* Kernel boot: mediaboard present */
        } else if (s->mbcom_handshake_done) {
            r = 0x0000;  /* Handshake complete: high byte must be 0 for Type-3 board */
        } else {
            r = 0x0000;  /* Handshake not yet done: upper byte=0 = init phase */
        }
        s->lpc_40f0_reads++;
        /* Detect game XBE reboot: if SEGABOOT already reached boot=3
         * and the kernel re-reads 0x40F0, the game XBE is initializing.
         * Disable DMA scan immediately to prevent memory corruption. */
        if (chihiro_boot3_reached && !chihiro_game_running) {
            chihiro_game_running = true;

            /* v209d: Re-create the LDP PTE in the kernel's NEW page tables.
             * QuickReboot re-initializes page tables, wiping our PTE from
             * boot=3. But the physical data at PA 0x262000 survived.
             * We must re-map it NOW, before the kernel reads the LDP
             * (which happens right after this 0x40F0 read). */
            if (chihiro_ldp_pa != 0xFFFFFFFF && chihiro_ldp_pa != 0) {
                uint32_t ldp_page = chihiro_ldp_pa & ~0xFFF;
                uint32_t pte_idx = (0x002625A0 >> 12) & 0x3FF;
                uint32_t pde;
                cpu_physical_memory_read(0xF000, &pde, 4);  /* kernel CR3 */
                uint32_t pt_base = pde & 0xFFFFF000;
                uint32_t pte_addr = pt_base + pte_idx * 4;
                uint32_t new_pte = ldp_page | 0x067;
                cpu_physical_memory_write(pte_addr, &new_pte, 4);

                /* Verify LDP data survived QuickReboot */
                uint32_t check_type = 0;
                cpu_physical_memory_read(chihiro_ldp_pa, &check_type, 4);
                if (check_type == 0) {
                    /* Data was cleared — re-fill */
                    uint32_t launch_type = 1;
                    cpu_physical_memory_write(chihiro_ldp_pa, &launch_type, 4);
                    uint32_t title_id = 0;
                    cpu_physical_memory_write(chihiro_ldp_pa + 4, &title_id, 4);
                    char lp[520] = {0};
                    snprintf(lp, sizeof(lp), "D:\\%s", chihiro_game_filename);
                    cpu_physical_memory_write(chihiro_ldp_pa + 8, lp, 520);
                    printf("[%07lld] Chihiro: Re-created PTE + re-filled LDP "
                           "after QuickReboot: path='%s'\n", TS_MS, lp);
                } else {
                    printf("[%07lld] Chihiro: Re-created PTE (LDP data survived "
                           "QuickReboot, type=%u)\n", TS_MS, check_type);
                }
            }

            /* Set the MediaBoard gate flag [VA 0x8003A93C] = 1.
             *
             * The arcdkrnl checks this flag to decide whether to call
             * MediaBoard partition init (creates \Device\MediaBoard,
             * CdRom0, D:\). On real hardware, HalReturnToFirmware sets
             * it during QuickReboot. Our SEGABOOT patches prevent the
             * proper QuickReboot path, leaving the flag at 0.
             *
             * This LPC read happens inside the SAME kernel function that
             * checks the gate 0x3A bytes later. The write is immediately
             * visible to the CPU when it reaches the check. */
            uint8_t gate_val = 1;
            address_space_write(&address_space_memory, 0x3A93C,
                                MEMTXATTRS_UNSPECIFIED, &gate_val, 1);

            /* v209d: Patch kernel to force MediaBoard driver init.
             *
             * The MediaBoard driver has two gate checks that SKIP device
             * creation when gate=1 (assumes devices persisted via
             * MmPersistContiguousMemory). We don't have persistence,
             * so we NOP these JNE instructions to force device creation.
             *
             * This creates \Device\CdRom0, D:\, and partition symlinks
             * which are needed to load game files.
             *
             * Patch 1: PA 0x30905 — JNE+37 → NOP (skips IoCreateSymbolicLink calls)
             * Patch 2: PA 0x309AD — JNE+10 → NOP (skips FATX/partition init)
             * Patch 3: PA 0x25B16 — JNE+23 → NOP (skips D:\ symlink when MB_FLAG=1)
             * Patch 4: PA 0x25AB0 — JNE+24 → NOP (skips mbcom partition when MB_FLAG=1) */
            {
                uint8_t nop2[2] = {0x90, 0x90};
                uint8_t check[2];

                cpu_physical_memory_read(0x30905, check, 2);
                if (check[0] == 0x75 && check[1] == 0x25) {
                    cpu_physical_memory_write(0x30905, nop2, 2);
                    printf("[%07lld] Chihiro: Kernel patch 1: NOP JNE @ PA 0x30905 "
                           "(force MediaBoard device creation)\n", TS_MS);
                }

                cpu_physical_memory_read(0x309AD, check, 2);
                if (check[0] == 0x75 && check[1] == 0x0A) {
                    cpu_physical_memory_write(0x309AD, nop2, 2);
                    printf("[%07lld] Chihiro: Kernel patch 2: NOP JNE @ PA 0x309AD "
                           "(force partition init)\n", TS_MS);
                }

                /* Patch 3: create_partitions skips D:\ when MB_FLAG=1.
                 * The port 0x40F0 handler sets MB_FLAG=1 BEFORE calling
                 * create_partitions, so D:\ is NEVER created. */
                cpu_physical_memory_read(0x25B1B, check, 2);
                if (check[0] == 0x75 && check[1] == 0x17) {
                    cpu_physical_memory_write(0x25B1B, nop2, 2);
                    printf("[%07lld] Chihiro: Kernel patch 3: NOP JNE @ PA 0x25B1B "
                           "(force D:\\ symlink creation)\n", TS_MS);
                }

                /* Patch 4: create_partitions skips mbcom when MB_FLAG=1 */
                cpu_physical_memory_read(0x25AAB, check, 2);
                if (check[0] == 0x75 && check[1] == 0x18) {
                    cpu_physical_memory_write(0x25AAB, nop2, 2);
                    printf("[%07lld] Chihiro: Kernel patch 4: NOP JNE @ PA 0x25AAB "
                           "(force mbcom partition creation)\n", TS_MS);
                }
            }

            /* Second RAM dump: after QuickReboot, patches applied.
             * This captures the GAME state, not SEGABOOT state. */
            {
                FILE *f = fopen("/tmp/ram_game.bin", "wb");
                if (f) {
                    uint8_t page[4096];
                    for (uint32_t pa = 0; pa < 0x800000; pa += 4096) {
                        cpu_physical_memory_read(pa, page, 4096);
                        fwrite(page, 1, 4096, f);
                    }
                    fclose(f);
                    printf("[%07lld] Chihiro: Game RAM dump saved to /tmp/ram_game.bin "
                           "(8MB)\n", TS_MS);
                }
            }

            /* v209b: Check if XLaunchNewImage filled the LDP (PTE was created
             * at boot=3). If it did, great. If not, fill it manually. */
            {
                uint32_t ldp_pa = chihiro_ldp_pa;
                if (ldp_pa != 0xFFFFFFFF && ldp_pa != 0) {
                    uint32_t existing_type = 0;
                    cpu_physical_memory_read(ldp_pa, &existing_type, 4);
                    if (existing_type != 0) {
                        char existing_path[64] = {0};
                        cpu_physical_memory_read(ldp_pa + 8, existing_path, 60);
                        printf("[%07lld] Chihiro: LDP filled by XLaunchNewImage:"
                               " type=%u path='%s'\n",
                               TS_MS, existing_type, existing_path);
                    } else if (chihiro_game_filename[0]) {
                        uint32_t launch_type = 1;
                        cpu_physical_memory_write(ldp_pa, &launch_type, 4);
                        uint32_t title_id = 0;
                        cpu_physical_memory_write(ldp_pa + 4, &title_id, 4);
                        char launch_path[520] = {0};
                        snprintf(launch_path, sizeof(launch_path),
                                 "D:\\%s", chihiro_game_filename);
                        cpu_physical_memory_write(ldp_pa + 8, launch_path, 520);
                        printf("[%07lld] Chihiro: Filled LDP manually (PA 0x%08X):"
                               " path='%s'\n", TS_MS, ldp_pa, launch_path);
                    } else {
                        printf("[%07lld] Chihiro: WARNING — LDP empty, no filename\n", TS_MS);
                    }
                } else {
                    printf("[%07lld] Chihiro: WARNING — LDP PA unavailable\n", TS_MS);
                }
            }

            printf("[%07lld] Chihiro: game XBE detected (0x40F0 re-read after boot=3)"
                   " — set gate [0x8003A93C]=1, DMA scan disabled\n", TS_MS);
        }
        break;
    case SEGA_DIMM_SIZE:
        /* TEMPORARY: Hardcoded 512MB DIMM size. On real hardware, the
         * baseboard PIC detects installed DIMM capacity.
         * TODO: Replace with PIC16 emulation for upstream LLE. */
        r = SEGA_DIMM_SIZE_512M;        /* Kernel computes mbcom LBA from this:
                                         * mbcom_start = (0x40000 << factor) - 0x8000.
                                         * Must match IDE capacity and CHIHIRO_MBCOM_BASE. */
        break;
    case 0x26:  /* Port 0x4026: scratch register (read-write) */
        r = s->lpc_scratch_4026;
        break;
    case 0xE0:  /* TEMPORARY: MB_ALIVE + DMA status bits.
                 * On real hardware, bit 0 is set by PIC16 when baseboard
                 * is alive; higher bits are cmd-complete flags.
                 * TODO: Replace with PIC16 emulation for upstream LLE. */
        r = s->mbcom_e0_status | 0x01;
        break;
    case 0x84:  /* Port 0x4084 — MbcomCommand session handle */
        r = 0x0000;
        s->lpc_4084_reads++;
        break;
    default:
        break;
    }

    if (CHIHIRO_LOG && addr != 0x26 && addr != 0xE0 && addr != 0xF0 && addr != 0xF4) {
        printf("[%07lld] chihiro lpc read  [0x%04x] -> 0x%04x (size=%d)\n", TS_MS,
               (unsigned)(addr + 0x4000), (unsigned)r, size);
    }
    return r;
}

static uint8_t chihiro_mbcom_command[512];
static void chihiro_mbcom_process(void);

static void chihiro_lpc_io_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{

    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);

    /* Log writes (suppress polling spam: 0x4026 scratch, 0x40E0 ack, 0x40E1 trigger) */
    if (addr != 0x26 && addr != 0xE0 && addr != 0xE1) {
        printf("[%07lld] chihiro lpc write [0x%04x] <- 0x%04x (size=%d)\n", TS_MS,
               (unsigned)(addr + 0x4000), (unsigned)val, size);
    }
    if (addr == 0xE1) {
        static uint32_t e1_write_count = 0;
        e1_write_count++;
        if (e1_write_count <= 3 || e1_write_count % 100 == 0) {
            printf("[%07lld] chihiro lpc write [0x40e1] <- 0x%04x (#%u)\n",
                   TS_MS, (unsigned)val, e1_write_count);
        }
    }

    switch (addr) {
    case 0x00: /* Port 0x4000: write to baseboard register at lpc_reg_addr */
        s->lpc_reg_data = (uint32_t)val;
        switch (s->lpc_reg_addr) {
        case 0xA0000020: /* Indirect address pointer */
            s->bb_reg_addr = (uint32_t)val;
            if (val == 0x84000020) {
                s->dimm_cmd_idx = 0;  /* reset command capture */
            }
            break;
        case 0xA0000040: /* DMA status/enable */
            s->bb_reg_status = (uint32_t)val;
            if (val & 0x80000000) {
                s->bb_dma_active = true;
                s->bb_dma_count = 0;
            } else {
                s->bb_dma_active = false;
            }
            break;
        case 0xA0000000: /* Data write to address in bb_reg_addr */
            if (s->bb_dma_active) {
                uint32_t wa = s->bb_reg_addr;
                if (wa >= 0x84000020 && wa <= 0x8400003C) {
                    uint32_t idx = (wa - 0x84000020) / 4;
                    if (idx < 8) {
                        s->dimm_cmd[idx] = (uint32_t)val;
                        if (idx == 7) {
                            chihiro_dimm_process_cmd(s);
                        }
                    }
                } else if (wa >= 0x84000000 && wa <= 0x8400001C) {
                    uint32_t idx = (wa - 0x84000000) / 4;
                    if (idx < 8) {
                        s->dimm_resp[idx] = (uint32_t)val;
                    }
                    if (idx == 0 && val == 0) {
                        memset(s->dimm_resp, 0, sizeof(s->dimm_resp));
                        s->dimm_resp_ready = false;
                    }
                }
                s->bb_dma_count++;
                s->bb_reg_addr += 4;  /* auto-increment */
            }
            break;
        }
        if (s->lpc_reg_addr != 0xA0000000 || s->bb_dma_count <= 8) {
            printf("[%07lld] chihiro lpc reg write [0x%08X] <- 0x%08X",
                   TS_MS, s->lpc_reg_addr, (unsigned)val);
            if (s->bb_dma_active && s->lpc_reg_addr == 0xA0000000)
                printf(" (dma #%u)", s->bb_dma_count);
            printf("\n");
        }
        return;
    case 0x04: /* Port 0x4004: set register address */
        s->lpc_reg_addr = (uint32_t)val;
        return;
    case 0x08: /* Port 0x4008: reset cycle */
        return;
    case 0x26:  /* Port 0x4026: scratch register */
        s->lpc_scratch_4026 = (uint16_t)val;
        if (val == 0x0102) {
            s->mbcom_handshake_done = true;
            printf("[%07lld] Chihiro: Handshake complete (0x4026 <- 0x0102)\n", TS_MS);
        }
        break;
    case SEGA_IRQ10_ACK:  /* 0xE0 — ack: clear specific bits */
        s->mbcom_e0_status &= ~(uint8_t)val;
        if (s->mbcom_e0_status == 0)
            qemu_irq_lower(s->irq10);
        break;
    case 0xE2:            /* 0x40E2 — IRQ10 deassert only, do NOT clear data ready */
        qemu_irq_lower(s->irq10);
        break;
    case 0xE1:            /* 0x40E1 — mbcom trigger / IRQ10 deassert */
        if (val != 0) {
            static int e1_trigger_count = 0;
            e1_trigger_count++;
            if (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0) {
                chihiro_mbcom_process();
                s->mbcom_e0_status |= 0x01;
                qemu_irq_raise(s->irq10);
                printf("[%07lld] Chihiro: LPC trigger 0x40E1=0x%04X → "
                       "PROCESS cmd=%02X%02X status=0x%02X + IRQ10 (#%d)\n",
                       TS_MS, (unsigned)val,
                       chihiro_mbcom_command[0], chihiro_mbcom_command[1],
                       s->mbcom_e0_status, e1_trigger_count);
            } else {
                if (e1_trigger_count <= 3 || e1_trigger_count % 500 == 0) {
                    printf("[%07lld] Chihiro: LPC trigger 0x40E1=0x%04X → "
                           "no pending cmd, skip (#%d)\n",
                           TS_MS, (unsigned)val, e1_trigger_count);
                }
            }
        } else {
            qemu_irq_lower(s->irq10);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps chihiro_lpc_io_ops = {
    .read = chihiro_lpc_io_read,
    .write = chihiro_lpc_io_write,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* Global IRQ10 reference for mbcom DMA write trigger */
static qemu_irq chihiro_irq10_global = NULL;

/*
 * IRQ10 periodic pulse — signals SEGABOOT that a baseboard response is ready.
 * On real hardware, IRQ10 fires after each mbcom command is processed.
 * We pulse it periodically since we pre-fill the response sector.
 */

/* mbcom state — chihiro_mbcom_command declared earlier (forward decl for LPC handler) */
static uint8_t chihiro_mbcom_response[512];
/* chihiro_mbcom_command[512] is forward-declared before chihiro_lpc_io_write */
static bool chihiro_mbcom_enabled = false;

/* Flash ROM (SEGABOOT) loaded from file — serves mbrom0/mbrom1 reads.
 * On real hardware: fpr-23887_29lv160te.ic4 (2MB flash on MediaBoard).
 * Contains the SEGABOOT XBE that boots before the game. */
static uint8_t *chihiro_flash_rom = NULL;
static uint32_t chihiro_flash_rom_size = 0;

/* Load flash ROM from a file path. Called during LPC device init.
 * Searches for fpr-23887 or fpr21042 in the same directory as the BIOS. */
void chihiro_load_flash_rom(const char *bios_path)
{
    if (chihiro_flash_rom) return; /* already loaded */

    /* Try to find flash ROM in same directory as BIOS */
    char dir[1024] = {0};
    const char *last_sep = strrchr(bios_path, '/');
    if (!last_sep) last_sep = strrchr(bios_path, '\\');
    if (last_sep) {
        int dir_len = last_sep - bios_path + 1;
        if (dir_len < (int)sizeof(dir)) {
            memcpy(dir, bios_path, dir_len);
        }
    }

    const char *flash_names[] = {
        "fpr21042_m29w160et.bin",        /* Cxbx version — matches our patches */
        "fpr-23887_29lv160te.ic4",       /* MAME version — different SEGABOOT */
        "fpr-23887.bin",
        NULL
    };

    for (int i = 0; flash_names[i]; i++) {
        char path[2048];
        snprintf(path, sizeof(path), "%s%s", dir, flash_names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            /* Also try parent directory */
            snprintf(path, sizeof(path), "%s../%s", dir, flash_names[i]);
            f = fopen(path, "rb");
        }
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz <= 4 * 1024 * 1024) {
                chihiro_flash_rom = (uint8_t *)g_malloc0(sz);
                if (fread(chihiro_flash_rom, 1, sz, f) == (size_t)sz) {
                    chihiro_flash_rom_size = sz;
                    printf("[%07lld] Chihiro: Loaded flash ROM '%s' (%u bytes)\n",
                           TS_MS, path, (unsigned)sz);
                } else {
                    g_free(chihiro_flash_rom);
                    chihiro_flash_rom = NULL;
                }
            }
            fclose(f);
            if (chihiro_flash_rom) return;
        }
    }
    printf("[%07lld] Chihiro: No flash ROM found (fpr-23887/fpr21042). "
           "Will fall through to baseboard.img for mbrom reads.\n", TS_MS);
}

static void chihiro_irq10_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;

    if (s->eeprom_hack_applied && !chihiro_game_running) {
        qemu_irq_raise(s->irq10);
    }

    /* Kernel probe: watch IRP_MJ_CREATE for MediaBoard (PA of 0x8002535A) */
    if (chihiro_mbcom_enabled && !chihiro_game_running) {
        static uint32_t irp_create_hit = 0, ntcreate_hit = 0;
        static uint32_t eip_rb = 0, eip_w3bc = 0;
        static int eip_tick = 0;
        CPUState *cs = first_cpu;
        if (cs) {
            uint32_t e = (uint32_t)X86_CPU(cs)->env.eip;
            if (e >= 0x8002535A && e < 0x80025400) {
                if (irp_create_hit++ < 5)
                    printf("[%07lld] KERNEL-IRP-CREATE: hit! EIP=0x%08X #%u\n",
                           TS_MS, e, irp_create_hit);
            }
            if (e >= 0x80025000 && e < 0x80026000) {
                if (ntcreate_hit++ < 5)
                    printf("[%07lld] KERNEL-MB-DRIVER: EIP=0x%08X #%u\n",
                           TS_MS, e, ntcreate_hit);
            }
            if (e >= 0x63330 && e < 0x63466) eip_rb++;
            if (e >= 0x2DDD0 && e < 0x2DF44) eip_w3bc++;
        }
        if (++eip_tick % 62 == 0) {
            printf("[%07lld] EIP-PROBE: irp_create=%u mb_driver=%u "
                   "w3bc=%u rb=%u (62 ticks)\n",
                   TS_MS, irp_create_hit, ntcreate_hit,
                   eip_w3bc, eip_rb);
        }
    }

    /* DMA META scan: marks mbcom slots as "ready" with responses.
     * Required for boot=0 → boot=1 — SEGABOOT polls slot markers.
     * fpr-23887: slot VA=0x89760, meta VA=0x89740, stride=0x40
     * fpr-21042: slot VA=0xAA7B0, meta VA=0xAA790, stride=0x60 */
    if (chihiro_mbcom_enabled && !chihiro_game_running) {
        static int va_diag_count = 0;
        if (va_diag_count++ % 62 == 0) { /* ~every 1 second at 16ms ticks */
            CPUState *cpu = first_cpu;
            if (cpu) {
                X86CPU *x86 = X86_CPU(cpu);
                uint32_t cr3 = x86->env.cr[3] & 0xFFFFF000;
                uint32_t test_va = 0xAA7B0;
                uint32_t pde_addr = cr3 + ((test_va >> 22) * 4);
                uint32_t pde;
                cpu_physical_memory_read(pde_addr, &pde, 4);
                if (pde & 1) {
                    uint32_t pa = 0;
                    if (pde & 0x80) {
                        pa = (pde & 0xFFC00000) | (test_va & 0x003FFFFF);
                    } else {
                        uint32_t pt_addr = (pde & 0xFFFFF000) + (((test_va >> 12) & 0x3FF) * 4);
                        uint32_t pte;
                        cpu_physical_memory_read(pt_addr, &pte, 4);
                        if (pte & 1)
                            pa = (pte & 0xFFFFF000) | (test_va & 0xFFF);
                    }
                    if (pa && (va_diag_count % 310 == 0)) {
                        /* Dump first 3 slots raw hex every ~5s */
                        uint32_t meta_pa = pa - 0x20; /* META = DATA - 0x20 */
                        printf("[%07lld] SLOT-DUMP PA=0x%05X (META=0x%05X stride=0x60):\n",
                               TS_MS, pa, meta_pa);
                        for (int sl = 0; sl < 3; sl++) {
                            uint8_t raw[96];
                            cpu_physical_memory_read(meta_pa + sl * 0x60, raw, 96);
                            printf("  slot[%d] META: ", sl);
                            for (int b = 0; b < 32; b++) printf("%02X", raw[b]);
                            printf("\n         DATA: ");
                            for (int b = 32; b < 64; b++) printf("%02X", raw[b]);
                            printf("\n         +40:  ");
                            for (int b = 64; b < 96; b++) printf("%02X", raw[b]);
                            printf("\n");
                        }
                    }
                }
            }
        }

        /* META scan: provide mbcom slot responses to SEGABOOT.
         * Only active AFTER patches applied (usb_poll_patched=true)
         * and ONLY when va_to_pa resolves correctly.
         * v239 RAM dump confirmed: fpr-21042 slot PA=0xF57B0 stride=0x60
         *
         * Guard: validate cmd_opcode is known BEFORE writing anything.
         * Previous corruption was from wrong stride or unmapped PAs. */
        if (chihiro_lpc_global && chihiro_lpc_global->usb_poll_patched) {
            static const struct { uint32_t slot_va; uint32_t meta_va; uint32_t stride; }
                slot_layouts[] = {
                    { 0xAA7B0, 0xAA790, 0x60 },  /* fpr-21042 */
                    { 0x89760, 0x89740, 0x40 },  /* fpr-23887 */
                };

            for (int layout = 0; layout < 2; layout++) {
                uint32_t slot_base_pa = chihiro_va_to_pa(slot_layouts[layout].slot_va);
                if (slot_base_pa == 0xFFFFFFFF) continue;
                uint32_t meta_base_pa = chihiro_va_to_pa(slot_layouts[layout].meta_va);
                if (meta_base_pa == 0xFFFFFFFF) continue;

                /* Sanity: PA must be in RAM range (< 8MB), not VRAM/MMIO */
                if (slot_base_pa >= 0x800000 || meta_base_pa >= 0x800000) continue;

                uint32_t stride = slot_layouts[layout].stride;

                for (int sl = 0; sl < 16; sl++) {
                    uint32_t data_pa = slot_base_pa + sl * stride;
                    uint32_t meta_pa = meta_base_pa + sl * stride;

                    /* Bounds check */
                    if (data_pa + 4 >= 0x800000 || meta_pa + 12 >= 0x800000) continue;

                    uint8_t data_byte0;
                    uint16_t meta_marker;
                    cpu_physical_memory_read(data_pa, &data_byte0, 1);
                    cpu_physical_memory_read(meta_pa + 2, &meta_marker, 2);

                    /* Only process if type is set and marker is clear */
                    if (data_byte0 == 0 || meta_marker != 0) continue;

                    uint16_t cmd_opcode = 0;
                    cpu_physical_memory_read(data_pa + 2, &cmd_opcode, 2);

                    uint32_t resp_data = 0, resp_data2 = 0;
                    switch (cmd_opcode) {
                    case 0x0001: resp_data = 0x20000000; break;
                    case 0x0100: resp_data = 5; resp_data2 = 100; break;
                    case 0x0101: resp_data = 0x0317; break;
                    case 0x0102: resp_data = 0x8002; break;
                    case 0x0103: resp_data = 0x6261632D; break;
                    default:
                        printf("[%07lld] Chihiro DMA: UNKNOWN cmd=0x%04X type=%u slot=%d "
                               "PA=0x%05X meta=0x%05X — responding with 0\n",
                               TS_MS, cmd_opcode, data_byte0, sl, data_pa, meta_pa);
                        resp_data = 0;
                        break;
                    }

                    meta_marker = 0x0001;
                    cpu_physical_memory_write(meta_pa + 2, &meta_marker, 2);
                    cpu_physical_memory_write(meta_pa + 4, &resp_data, 4);
                    if (cmd_opcode == 0x0100)
                        cpu_physical_memory_write(meta_pa + 8, &resp_data2, 4);
                    s->mbcom_e0_status |= 0x05;
                    qemu_irq_raise(s->irq10);

                    if (cmd_opcode == 0x0100) {
                        uint8_t meta_raw[16];
                        cpu_physical_memory_read(meta_pa, meta_raw, 16);
                        printf("[%07lld] META-STATUS-WRITE: meta_pa=0x%05X data_pa=0x%05X "
                               "slot=%d raw=", TS_MS, meta_pa, data_pa, sl);
                        for (int b = 0; b < 16; b++) printf("%02X", meta_raw[b]);
                        printf(" [marker=%04X phase=%u pct=%u]\n",
                               meta_marker, resp_data, resp_data2);
                    }

                    static uint16_t last_dma_cmd = 0xFFFF;
                    static uint32_t dma_repeat_count = 0;
                    if (cmd_opcode == last_dma_cmd) {
                        dma_repeat_count++;
                    } else {
                        if (dma_repeat_count > 1)
                            printf("[%07lld] Chihiro DMA: (prev cmd=0x%04X repeated %u)\n",
                                   TS_MS, last_dma_cmd, dma_repeat_count);
                        printf("[%07lld] Chihiro DMA: slot %d PA=0x%05X cmd=0x%04X resp=0x%08X\n",
                               TS_MS, sl, data_pa, cmd_opcode, resp_data);
                        last_dma_cmd = cmd_opcode;
                        dma_repeat_count = 1;
                    }
                }
                break; /* Use first working layout */
            }
        }
    }

    /* STATE MACHINE PROBE: direct read at PA 0x166150 (found by vtable scan) */
    {
        static int sm_log = 0;
        if (sm_log++ % 62 == 0) { /* ~every 1 second */
            uint32_t sm_state = 0, sm_err = 0, sm_tick = 0, sm_flagc = 0;
            uint32_t sm_vtable = 0, sm_appobj = 0, sm_done = 0, sm_errtick = 0;
            cpu_physical_memory_read(0x166150, &sm_vtable, 4);
            cpu_physical_memory_read(0x166154, &sm_appobj, 4);
            cpu_physical_memory_read(0x166158, &sm_done, 4);
            cpu_physical_memory_read(0x16615C, &sm_flagc, 4);
            cpu_physical_memory_read(0x166160, &sm_err, 4);
            cpu_physical_memory_read(0x166164, &sm_state, 4);
            cpu_physical_memory_read(0x166168, &sm_tick, 4);
            cpu_physical_memory_read(0x16616C, &sm_errtick, 4);
            printf("[%07lld] SM-REAL: vt=0x%X app=0x%08X state=%u "
                   "tick=%u err=%u flag_c=%u done=%u errtick=%u\n",
                   TS_MS, sm_vtable, sm_appobj, sm_state,
                   sm_tick, sm_err, sm_flagc, sm_done, sm_errtick);
            /* If app_obj looks valid (contiguous mem 0xD0xxxxxx or heap), read 3BC/3D4 */
            if (sm_appobj != 0 && sm_appobj != 1) {
                uint32_t aobj_pa = chihiro_va_to_pa(sm_appobj + 0x3BC);
                uint32_t aobj_pa2 = chihiro_va_to_pa(sm_appobj + 0x3D4);
                uint32_t v3bc = 0, v3d4 = 0;
                if (aobj_pa != 0xFFFFFFFF)
                    cpu_physical_memory_read(aobj_pa, &v3bc, 4);
                if (aobj_pa2 != 0xFFFFFFFF)
                    cpu_physical_memory_read(aobj_pa2, &v3d4, 4);
                printf("[%07lld] SM-APP: [+3BC]=%u [+3D4]=0x%X\n",
                       TS_MS, v3bc, v3d4);
            }
        }
    }

    /* Independent one-shot: dump SEGABOOT import thunks after boot */
    {
        static int thunks_dumped = 0;
        if (!thunks_dumped && TS_MS > 8000) {
            uint32_t thunk_pa = chihiro_va_to_pa(0x11038);
            if (thunk_pa != 0xFFFFFFFF) {
                uint32_t thunks[8];
                cpu_physical_memory_read(thunk_pa - 8, thunks, 32);
                printf("[%07lld] SEGABOOT-THUNKS @VA 0x11030: "
                       "%08X %08X [NtCreateFile@11038]=%08X %08X "
                       "%08X %08X %08X %08X\n",
                       TS_MS, thunks[0], thunks[1], thunks[2],
                       thunks[3], thunks[4], thunks[5],
                       thunks[6], thunks[7]);
                /* Also dump kernel partition state */
                uint32_t dimm_sz = 0, fpga = 0, cqd = 0;
                cpu_physical_memory_read(0x3B3CC, &dimm_sz, 4);
                cpu_physical_memory_read(0x3B3C8, &fpga, 1);
                cpu_physical_memory_read(0x3AF2C, &cqd, 1);
                printf("[%07lld] KERNEL-MB: DIMM_sz=0x%08X FPGABoard=%u "
                       "CreateQuickDone=%u\n", TS_MS, dimm_sz, fpga, cqd);
                /* Check handle at this exact moment */
                uint32_t h_pa = chihiro_va_to_pa(0xAA74C);
                if (h_pa != 0xFFFFFFFF) {
                    uint32_t h = 0;
                    cpu_physical_memory_read(h_pa, &h, 4);
                    printf("[%07lld] SEGABOOT-HANDLE: 0x%08X\n", TS_MS, h);
                }
                /* Init-progress probe: check how far mbcom_main_init got */
                {
                    uint32_t v;
                    /* DAT_000aa670: set to 3 in FUN_0003a900 */
                    uint32_t pa670 = chihiro_va_to_pa(0xAA670);
                    /* DAT_0001eb9c: set to 1 in FUN_0003a900 */
                    uint32_t pa1eb = chihiro_va_to_pa(0x1EB9C);
                    /* DAT_000aa750: DMA mode set by FUN_0003e230 */
                    uint32_t pa750 = chihiro_va_to_pa(0xAA750);
                    /* DAT_000aad98: DMA thread A handle (FUN_00040140) */
                    uint32_t pad98 = chihiro_va_to_pa(0xAAD98);
                    /* DAT_000aae18: DMA thread B handle (FUN_00040140) */
                    uint32_t pae18 = chihiro_va_to_pa(0xAAE18);
                    uint32_t g670=0, g1eb=0, g750=0, gd98=0, ge18=0;
                    if (pa670 != 0xFFFFFFFF) cpu_physical_memory_read(pa670, &g670, 4);
                    if (pa1eb != 0xFFFFFFFF) cpu_physical_memory_read(pa1eb, &g1eb, 4);
                    if (pa750 != 0xFFFFFFFF) cpu_physical_memory_read(pa750, &g750, 4);
                    if (pad98 != 0xFFFFFFFF) cpu_physical_memory_read(pad98, &gd98, 4);
                    if (pae18 != 0xFFFFFFFF) cpu_physical_memory_read(pae18, &ge18, 4);
                    /* DAT_00011414/18/1c: JVS error state from FUN_0001a4b0 */
                    uint32_t pa414 = chihiro_va_to_pa(0x11414);
                    uint32_t pa418 = chihiro_va_to_pa(0x11418);
                    uint32_t pa41c = chihiro_va_to_pa(0x1141C);
                    uint32_t g414=0, g418=0, g41c=0;
                    if (pa414 != 0xFFFFFFFF) cpu_physical_memory_read(pa414, &g414, 4);
                    if (pa418 != 0xFFFFFFFF) cpu_physical_memory_read(pa418, &g418, 4);
                    if (pa41c != 0xFFFFFFFF) cpu_physical_memory_read(pa41c, &g41c, 4);
                    /* Device table flags: slot2(QC)=0xEA004, slot3(SC)=0xEA21C */
                    uint32_t pa_qcf = chihiro_va_to_pa(0xEA004);
                    uint32_t pa_scf = chihiro_va_to_pa(0xEA21C);
                    uint8_t qcf=0, scf=0;
                    if (pa_qcf != 0xFFFFFFFF) cpu_physical_memory_read(pa_qcf, &qcf, 1);
                    if (pa_scf != 0xFFFFFFFF) cpu_physical_memory_read(pa_scf, &scf, 1);
                    /* SC slot assignment at DAT_00011410 */
                    uint32_t pa_scs = chihiro_va_to_pa(0x11410);
                    uint32_t scs = 0;
                    if (pa_scs != 0xFFFFFFFF) cpu_physical_memory_read(pa_scs, &scs, 4);
                    printf("[%07lld] INIT-PROGRESS: aa670=%u 1eb9c=%u "
                           "dma_mode(aa750)=%u threadA(aad98)=0x%X "
                           "threadB(aae18)=0x%X | "
                           "JVS_err=0x%08X JVS_str=0x%05X JVS_msg=0x%05X | "
                           "QCflags=0x%02X SCflags=0x%02X SC_slot=%u\n",
                           TS_MS, g670, g1eb, g750, gd98, ge18,
                           g414, g418, g41c, qcf, scf, scs);
                }
                thunks_dumped = 1;
            }
        }
    }

    /* Re-arm every 16ms (~60Hz) */
    timer_mod(s->irq10_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
}


/*
 * Chihiro arcade kernel (arcdkrnl) EEPROM validation hack.
 *
 * The Chihiro kernel uses a unique EEPROM key different from all
 * retail Xbox kernel versions. When booted with a standard Xbox
 * EEPROM (generated by xemu), the HMAC-SHA1 validation fails and
 * the kernel halts with LED error pattern 0xA0.
 *
 * This applies the same runtime RAM patch as MAME's hack_eeprom():
 *   - NOP the conditional jump at 0x8003B744 (skip validation fail path)
 *   - LEAVE;RET at 0x8003B766 (return success from validation function)
 *
 * The BIOS file is never modified — only RAM is patched after the
 * kernel has been decrypted and loaded by the 2BL.
 */
static void chihiro_eeprom_hack_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;

    if (!s->eeprom_hack_applied) {
        /* Patch kernel EEPROM validation — same as MAME hack_eeprom() */
        uint8_t check[2];
        address_space_read(&address_space_memory, 0x3B744,
                           MEMTXATTRS_UNSPECIFIED, check, 2);

        if (check[0] == 0x75 && check[1] == 0x22) {
            uint8_t nop2[] = { 0x90, 0x90 };
            uint8_t leave_ret[] = { 0xC9, 0xC3 };

            address_space_write(&address_space_memory, 0x3B744,
                                MEMTXATTRS_UNSPECIFIED, nop2, 2);
            address_space_write(&address_space_memory, 0x3B766,
                                MEMTXATTRS_UNSPECIFIED, leave_ret, 2);

            s->eeprom_hack_applied = true;
            printf("[%07lld] Chihiro: Applied EEPROM validation hack "
                   "(arcdkrnl @ 0x8003B744)\n", TS_MS);

            /* Apply MediaBoard kernel patches NOW, before IdexChannelCreate
             * calls IdexMediaBoardCreateQuick. These NOP gate checks that
             * skip mbcom:/D:\ symlink creation when MB_FLAG=1.
             * Without these, NtCreateFile("mbcom:") fails → handle=-1. */
            {
                uint8_t nop2k[2] = {0x90, 0x90};
                uint8_t chk[2];

                cpu_physical_memory_read(0x25AAB, chk, 2);
                if (chk[0] == 0x75 && chk[1] == 0x18) {
                    cpu_physical_memory_write(0x25AAB, nop2k, 2);
                    printf("[%07lld] Chihiro: Kernel early patch: NOP JNE @ PA 0x25AAB "
                           "(force mbcom partition)\n", TS_MS);
                }

                cpu_physical_memory_read(0x25B1B, chk, 2);
                if (chk[0] == 0x75 && chk[1] == 0x17) {
                    cpu_physical_memory_write(0x25B1B, nop2k, 2);
                    printf("[%07lld] Chihiro: Kernel early patch: NOP JNE @ PA 0x25B1B "
                           "(force D:\\ symlink)\n", TS_MS);
                }

                cpu_physical_memory_read(0x30905, chk, 2);
                if (chk[0] == 0x75 && chk[1] == 0x25) {
                    cpu_physical_memory_write(0x30905, nop2k, 2);
                    printf("[%07lld] Chihiro: Kernel early patch: NOP JNE @ PA 0x30905 "
                           "(force symlinks)\n", TS_MS);
                }

                cpu_physical_memory_read(0x309AD, chk, 2);
                if (chk[0] == 0x75 && chk[1] == 0x0A) {
                    cpu_physical_memory_write(0x309AD, nop2k, 2);
                    printf("[%07lld] Chihiro: Kernel early patch: NOP JNE @ PA 0x309AD "
                           "(force partition init)\n", TS_MS);
                }
            }

            return;
        }
        /* Retry until kernel is decrypted */
        timer_mod(s->eeprom_hack_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

static void chihiro_lpc_realize(DeviceState *dev, Error **errp)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    chihiro_active = true;
    chihiro_lpc_global = s;
    memory_region_init_io(&s->ioport, OBJECT(dev), &chihiro_lpc_io_ops, s,
                          "chihiro-lpc-io", 0x100);
    isa_register_ioport(isa, &s->ioport, 0x4000);

    /* Initialize mbcom buffers to zero */
    memset(s->mbcom_read_buffer, 0, sizeof(s->mbcom_read_buffer));
    memset(s->mbcom_write_buffer, 0, sizeof(s->mbcom_write_buffer));

    /* Schedule EEPROM validation hack.
     * The kernel is encrypted in the BIOS and gets decrypted by the 2BL
     * at runtime. We poll until the expected bytes appear in RAM. */
    s->eeprom_hack_applied = false;
    s->eeprom_hack_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                         chihiro_eeprom_hack_cb, s);
    timer_mod(s->eeprom_hack_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);

    /* Initialize IRQ10 for baseboard communication */
    s->irq10 = isa_get_irq(isa, 10);
    chihiro_irq10_global = s->irq10;
    s->irq10_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                   chihiro_irq10_timer_cb, s);
    timer_mod(s->irq10_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);

    /* Initialize mbcom protocol handler */
    chihiro_mbcom_init();

    /* DIMM board event timer (not armed — armed after handshake completes) */
    s->dimm_event_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                        chihiro_dimm_event_timer_cb, s);
    s->dimm_cmd_count = 0;
    s->dimm_next_seq = 1;

    /* USB hotplug timers — v205: NO LONGER scheduled at fixed T+1500ms.
     * Instead, timers are created but armed only when ohci_bus_start()
     * fires (via chihiro_on_ohci_bus_start callback).
     * This ensures devices attach AFTER the kernel has enabled RHSC,
     * so fresh CSC events trigger full enumeration including SET_CONFIG.
     *
     * On real hardware: AN2131 boot ~200ms, kernel OHCI ~600ms.
     * Kernel sees devices during first scan → SET_CONFIG → CONFIGURED.
     * In our emulation: attach devices 150ms AFTER BUS START for same effect. */
    s->usb_hotplug_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                         chihiro_usb_hotplug_qc_cb, s);
    /* Timer NOT armed yet — will be armed by chihiro_on_ohci_bus_start() */

    s->usb_hotplug_sc_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                            chihiro_usb_hotplug_sc_cb, s);
    /* Timer NOT armed yet — will be armed by chihiro_on_ohci_bus_start() */

    /* UsbPollQC/SC patch — retry every 1ms until SEGABOOT is loaded */
    s->usb_poll_patched = false;
    s->usb_poll_patch_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                            chihiro_usb_poll_patch_cb, s);
    timer_mod(s->usb_poll_patch_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);

    printf("[%07lld] Chihiro: Mediaboard LPC I/O initialized at 0x4000-0x40FF\n", TS_MS);
}

static void chihiro_lpc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = chihiro_lpc_realize;
    dc->desc = "Chihiro Mediaboard LPC I/O";
}

static const TypeInfo chihiro_lpc_info = {
    .name          = "chihiro-lpc",
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(ChihiroLPCState),
    .class_init    = chihiro_lpc_class_init,
};

static void chihiro_register_types(void)
{
    type_register_static(&chihiro_lpc_info);
}

type_init(chihiro_register_types)

/*
 * Chihiro MediaBoard IDE mbcom protocol handler
 *
 * The baseboard communicates with SEGABOOT via IDE sector read/write
 * at specific LBAs within the mbcom partition:
 *   - Response sector: mbcom_base + 0x4800 (read by SEGABOOT)
 *   - Command sector:  mbcom_base + 0x4801 (written by SEGABOOT)
 *
 * For DIMM size 512MB (size_factor=2):
 *   mbcom_base = (0x40000 << 2) - 0x8000 = 0xF8000
 *   Response LBA = 0xFC800
 *   Command LBA  = 0xFC801
 */

#define CHIHIRO_MBCOM_BASE      0xF8000
#define CHIHIRO_MBCOM_RESPONSE  (CHIHIRO_MBCOM_BASE + 0x4800)  /* 0xFC800 */
#define CHIHIRO_MBCOM_COMMAND   (CHIHIRO_MBCOM_BASE + 0x4801)  /* 0xFC801 */
#define CHIHIRO_MBROM0          0x8000000
#define CHIHIRO_MBROM1          0x8000800

/* v146 minimal dedup logging — avoid timing regression from stdout saturation */
static uint16_t mbcom_last_cmd_logged = 0xFFFF;
static uint32_t mbcom_cmd_repeat_count = 0;
static uint8_t mbcom_last_read_sig[4] = {0};
static uint32_t mbcom_read_repeat_count = 0;

void chihiro_mbcom_init(void)
{
    memset(chihiro_mbcom_response, 0, sizeof(chihiro_mbcom_response));
    memset(chihiro_mbcom_command, 0, sizeof(chihiro_mbcom_command));
    chihiro_mbcom_enabled = true;
    printf("[%07lld] Chihiro: mbcom protocol handler initialized (v146 MAME-aligned)\n", TS_MS);
}

/* TEMPORARY: Process mbcom command and generate hardcoded responses.
 * On real hardware, the PIC16 (sp5001.bin) handles these commands by
 * querying DIMM board state, GDROM status, and firmware registers.
 * Aligned with MAME chihiro.cpp::baseboard_ide_event().
 * TODO: Replace with PIC16 emulation for upstream LLE. */
static void chihiro_mbcom_process(void)
{
    const uint8_t *w = chihiro_mbcom_command;
    uint8_t *r = chihiro_mbcom_response;

    if (w[0] == 0 && w[1] == 0) return;  /* no command */

    uint16_t cmd_echo = w[0] | (w[1] << 8);
    uint16_t cmd_code = w[2] | (w[3] << 8);

    /* Cxbx-style response: echo sequence + command|0x8000 success flag */
    r[0] = w[0];
    r[1] = w[1];
    r[2] = w[2] | (cmd_code & 0xFF);         /* low byte of cmd | 0x8000 */
    r[3] = (w[3] & 0x7F) | 0x80;             /* high byte with bit15 set */
    /* zero out rest of 32-byte response area */
    memset(r + 4, 0, 28);

    /* dedup logging: only log new cmds, count repeats */
    if (cmd_code != mbcom_last_cmd_logged) {
        if (mbcom_cmd_repeat_count > 1) {
            printf("[%07lld] Chihiro mbcom: (prev cmd=0x%04X repeated %u times)\n",
                   TS_MS, mbcom_last_cmd_logged, mbcom_cmd_repeat_count);
        }
        printf("[%07lld] Chihiro mbcom: cmd=0x%04X echo=0x%04X\n", TS_MS, cmd_code, cmd_echo);
        mbcom_last_cmd_logged = cmd_code;
        mbcom_cmd_repeat_count = 1;
    } else {
        mbcom_cmd_repeat_count++;
    }

    switch (cmd_code) {
    case 0x0001: /* DIMM_SIZE — 512MB = 0x20000000 (matches port 0x40F4 factor=2) */
        r[4] = 0x00; r[5] = 0x00; r[6] = 0x00; r[7] = 0x20;
        break;
    case 0x0100: /* STATUS — phase=5 (READY), completion=100%
                  * CXBX: MB_STATUS_READY=5, percentage=100 */
        r[4] = 5; r[5] = 0; r[6] = 0; r[7] = 0;
        r[8] = 100; r[9] = 0; r[10] = 0; r[11] = 0;  /* completion 100% */
        break;
    case 0x0101: /* FW_VER — Cxbx: 0x0317 */
        r[4] = 0x17; r[5] = 0x03; r[6] = 0; r[7] = 0;
        break;
    case 0x0102: /* SYSTEM_TYPE — low byte must be >=2 to pass board check */
        r[4] = 0x02; r[5] = 0x80; r[6] = 0; r[7] = 0;
        break;
    case 0x0103: /* SERIAL — MAME: "-abc-abc12345678" */
        memcpy(r + 4, "-abc-abc12345678", 16);
        break;
    case 0x0104: /* Cxbx: unknown, returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case 0x0204: /* Cxbx: returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case 0x0301: /* HW_TEST — Cxbx writes "TEST OK" to result ptr */
        r[4] = w[4]; r[5] = w[5]; r[6] = w[6]; r[7] = w[7];
        /* Write "TEST OK" to the address specified in the command */
        {
            uint32_t result_ptr = w[8] | (w[9]<<8) | (w[10]<<16) | (w[11]<<24);
            if (result_ptr >= 0x80000000) {
                uint32_t result_pa = result_ptr - 0x80000000;
                cpu_physical_memory_write(result_pa, "TEST OK\0", 8);
            }
        }
        break;
    case 0x0415: /* Cxbx: returns IP 10.0.0.1 */
        r[4] = 1; r[5] = 0; r[6] = 0; r[7] = 10; /* 10.0.0.1 LE */
        break;
    case 0x0601: /* Cxbx: returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case 0x0602: /* Cxbx: returns 0xffff (triggers 0x0605) */
        r[4] = 0xFF; r[5] = 0xFF; r[6] = 0; r[7] = 0;
        break;
    case 0x0605: /* Cxbx: returns 0 */
    case 0x0606: /* Cxbx: returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case 0x0607: /* Cxbx: returns 0 */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        r[8] = 0; r[9] = 0; r[10] = 0; r[11] = 0;
        break;
    case 0x0608: /* Cxbx: returns IP 10.0.0.1 */
        r[4] = 1; r[5] = 0; r[6] = 0; r[7] = 10;
        break;
    default:
        printf("[%07lld] Chihiro mbcom: UNKNOWN cmd=0x%04X\n", TS_MS, cmd_code);
        break;
    }

    /* MAME: clear command header bytes 0-3 after processing (ack to baseboard) */
    chihiro_mbcom_command[0] = 0;
    chihiro_mbcom_command[1] = 0;
    chihiro_mbcom_command[2] = 0;
    chihiro_mbcom_command[3] = 0;

    /* v158: DMA inject into slot array REMOVED.
     * Writing word[+2] to non-zero pollutes GetMbcomType's empty-slot scan
     * at 0x3E176 (checks word[+2]==0 for "empty"). ValidateType at 0x3E4A0
     * already sets word[+2]=1, making PollReady return immediately.
     * No DMA injection needed — the slot system is self-contained. */
}

/*
 * IDE DMA read. Aligned with MAME chihiro.cpp::read_sector():
 *   LBA 0xFC800 (BASE+0x4800) → returns chihiro_mbcom_response (read_buffer)
 *   LBA 0xFC801 (BASE+0x4801) → returns chihiro_mbcom_command (write_buffer)
 *   Only first 32 bytes are meaningful; rest is zero.
 */
bool chihiro_ide_read_sector(uint32_t lba, void *buffer)
{
    if (!chihiro_mbcom_enabled) return false;

    static uint32_t ide_meta_cnt = 0, ide_fatx_cnt = 0, ide_mbcom_cnt = 0;
    if (lba >= 0x89 && lba < CHIHIRO_MBCOM_BASE) {
        ide_fatx_cnt++;
        printf("[%07lld] IDE-READ: LBA=0x%X (%u) [FATX-DATA] #%u\n",
               TS_MS, lba, lba, ide_fatx_cnt);
    } else if (lba < 0x89) {
        ide_meta_cnt++;
    } else {
        ide_mbcom_cnt++;
    }
    {
        static uint32_t ide_total = 0;
        if (++ide_total % 500 == 0)
            printf("[%07lld] IDE-STATS: total=%u meta=%u fatx=%u mbcom=%u\n",
                   TS_MS, ide_total, ide_meta_cnt, ide_fatx_cnt, ide_mbcom_cnt);
    }

    /* FATX partition: serve from in-memory image (LBA 0 to ~700K) */
    if (lba < CHIHIRO_MBCOM_BASE && chihiro_fatx_read_sector(lba, buffer)) {
        return true;
    }

    if (lba == CHIHIRO_MBCOM_RESPONSE) {
        memset(buffer, 0, 512);
        memcpy(buffer, chihiro_mbcom_response, 32);
        const uint8_t *d = (const uint8_t *)buffer;
        /* dedup: only log when response signature changes */
        if (memcmp(d, mbcom_last_read_sig, 4) != 0) {
            if (mbcom_read_repeat_count > 1) {
                printf("[%07lld] Chihiro mbcom: (prev response repeated %u times)\n",
                       TS_MS, mbcom_read_repeat_count);
            }
            printf("[%07lld] Chihiro mbcom: read FC800 response=%02X%02X%02X%02X "
                   "%02X%02X%02X%02X %02X%02X%02X%02X\n",
                   TS_MS, d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11]);
            memcpy(mbcom_last_read_sig, d, 4);
            mbcom_read_repeat_count = 1;
        } else {
            mbcom_read_repeat_count++;
        }
        return true;
    }
    if (lba == CHIHIRO_MBCOM_COMMAND) {
        memset(buffer, 0, 512);
        memcpy(buffer, chihiro_mbcom_command, 32);
        const uint8_t *d = (const uint8_t *)buffer;
        printf("[%07lld] MBCOM-IDE-READ FC801 slot=%02X%02X%02X%02X "
               "%02X%02X%02X%02X\n",
               TS_MS, d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
        return true;
    }

    /* mbrom0/mbrom1: serve from loaded flash ROM file.
     * baseboard.img layout: LBA 0x8000800 → byte 0 of fpr-23887 (full 2MB).
     * mbrom0 (LBA 0x8000000-0x80007FF): returns zeros (not needed for boot).
     * mbrom1 (LBA 0x8000800+): flash ROM from byte 0. */
    if (lba >= CHIHIRO_MBROM0 && chihiro_flash_rom) {
        memset(buffer, 0, 512);
        /* MAME behavior: (lba & 0x7FF) * 512 for both mbrom0 and mbrom1.
         * Both serve from the same first 1MB of the flash ROM. */
        uint32_t offset = (lba & 0x7FF) * 512;
        if (offset < chihiro_flash_rom_size) {
            uint32_t copy_len = 512;
            if (offset + copy_len > chihiro_flash_rom_size)
                copy_len = chihiro_flash_rom_size - offset;
            memcpy(buffer, chihiro_flash_rom + offset, copy_len);
        }
        return true;
    }
    memset(buffer, 0, 512);
    return true;
}

/*
 * IDE DMA write. Aligned with MAME chihiro.cpp::write_sector():
 *   LBA 0xFC800 → write into chihiro_mbcom_response (read_buffer)
 *   LBA 0xFC801 → write into chihiro_mbcom_command (write_buffer) THEN process + IRQ10
 */
bool chihiro_ide_write_sector(uint32_t lba, const void *buffer)
{
    if (!chihiro_mbcom_enabled) return false;

    if (lba == CHIHIRO_MBCOM_RESPONSE) {
        memcpy(chihiro_mbcom_response, buffer, 32);
        return true;
    }
    if (lba == CHIHIRO_MBCOM_COMMAND) {
        const uint8_t *d = (const uint8_t *)buffer;
        printf("[%07lld] MBCOM-IDE-WRITE FC801 cmd=%02X%02X sub=%02X%02X "
               "data=%02X%02X%02X%02X %02X%02X%02X%02X\n",
               TS_MS, d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11]);
        memcpy(chihiro_mbcom_command, buffer, 32);
        if (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0) {
            chihiro_mbcom_process();
            if (chihiro_irq10_global) {
                qemu_irq_raise(chihiro_irq10_global);
            }
        }
        return true;
    }
    if (lba >= CHIHIRO_MBCOM_BASE && lba <= CHIHIRO_MBCOM_BASE + 0x5000) {
        printf("[%07lld] MBCOM-IDE-WRITE lba=0x%X (base+0x%X)\n",
               TS_MS, lba, lba - CHIHIRO_MBCOM_BASE);
    }
    return false;
}

/*
 * Called from ide_dma_cb() when a DMA WRITE completes on IDE unit 1.
 * Checks if the write was to the mbcom command sector (LBA 0xFC801).
 * If so, reads back the command, processes it, and writes the response.
 * v146: aligned with MAME convention (FC800=response, FC801=cmd).
 */
void chihiro_ide_dma_write_done(BlockBackend *blk, int64_t sector_num)
{
    if (!chihiro_mbcom_enabled) return;

    int64_t cmd_lba = CHIHIRO_MBCOM_COMMAND;
    int64_t resp_lba = CHIHIRO_MBCOM_RESPONSE;

    printf("[%07lld] MBCOM-DMA-WRITE-DONE sector_num=0x%llX (cmd_lba=0x%llX)\n",
           TS_MS, (long long)sector_num, (long long)cmd_lba);

    if (sector_num <= cmd_lba) return;
    if (sector_num > cmd_lba + 256) return;

    /* Read back the command sector */
    uint8_t cmd_data[512];
    int ret = blk_pread(blk, cmd_lba * 512, 512, cmd_data, 0);
    if (ret < 0) return;

    if (cmd_data[0] == 0 && cmd_data[1] == 0) return;

    /* Copy into command buffer then process (MAME style) */
    memcpy(chihiro_mbcom_command, cmd_data, 32);
    chihiro_mbcom_process();

    /* Write response back to disk (for persistence through any DMA rereads) */
    uint8_t resp_sector[512];
    memset(resp_sector, 0, 512);
    memcpy(resp_sector, chihiro_mbcom_response, 32);
    blk_pwrite(blk, resp_lba * 512, 512, resp_sector, 0);

    /* Clear command sector on disk (MAME clears header bytes; we clear whole sector) */
    memset(cmd_data, 0, 512);
    blk_pwrite(blk, cmd_lba * 512, 512, cmd_data, 0);

    if (chihiro_irq10_global) {
        qemu_irq_raise(chihiro_irq10_global);
    }
}
