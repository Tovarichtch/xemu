/*
 * QEMU Chihiro emulation
 *
 * Copyright (c) 2013 espes
 * Copyright (c) 2018-2021 Matt Borgerson
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
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/isa/isa.h"
#include "hw/boards.h"
#include "system/memory.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
#include "chihiro-log.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "chihiro.h"
#include "system/blockdev.h"
#include "system/system.h"
#include "block/blkmemory.h"
#include "block/block-global-state.h"
#include "block/block_int-global-state.h"
#include "migration/vmstate.h"
#include <zlib.h>
#include "qemu/main-loop.h"
#include "hw/usb.h"
#include "ui/xemu-settings.h"
#include "target/i386/cpu.h"
#include "exec/watchpoint.h"
#include "ui/input.h"
#include "chihiro-jvs.h"
#include "chihiro-card-reader.h"

/*
 * Chihiro Mediaboard LPC I/O
 *
 * The Chihiro baseboard exposes a set of I/O registers at 0x4000-0x40FF
 * on the LPC/ISA bus. These are used by SEGABOOT to detect the baseboard,
 * query firmware version, DIMM size, and board type.
 *
 * Register map (from MAME chihiro.cpp + CXBX MediaBoard.cpp + RE of 0x3DF40):
 *   0x1E: SEGABOOT: DIMM base low word | Game: "XB" (0x4258) for XBAM check
 *   0x20: SEGABOOT: DIMM base high word | Game: "AM" (0x4D41) for XBAM check
 *   0x22: XBAM string "BX" (0x4258) — checked by SEGABOOT
 *   0x24: XBAM string "MA" (0x4D41) — checked by SEGABOOT
 *   0xE0: IRQ10 acknowledge (write clears IRQ10)
 *   0xF0: Chip revision / board type (0x0000 = Type-1, 0x0100 = Type-3)
 *   0xF4: DIMM size (0=128M, 1=256M, 2=512M, 3=1024M)
 *
 * SEGABOOT checks "XBAM" at 0x4022-0x4024 and uses 0x401E/0x4020 for DIMM
 * base address. Game XBE checks "XBAM" at 0x401E/0x4020 instead.
 *
 * WORKAROUND: the two readers are told apart by counting reads of 0x401E
 * (first read answers the DIMM base, later ones answer "XBAM") rather than by
 * anything the hardware exposes. Real hardware has no such counter.
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

/* mbcom command IDs — acMediaCmd names from acLib SDK (GXTX/GXTX).
 * Full command map: 0x001-0x0FF init/events, 0x100-0x1FF info queries,
 * 0x200-0x2FF unknown, 0x300-0x3FF tests, 0x400-0x4FF network sockets,
 * 0x500-0x7FF unknown groups. */
#define MB_CMD_INIT                 0x0001  /* acMediaCmd_InitAsync — returns DIMM size */
#define MB_CMD_SEND_EVENT           0x0009  /* acMediaCmd_SendEventAsync */
#define MB_CMD_STATUS               0x0100  /* boot phase + completion% */
#define MB_CMD_GET_VERSION          0x0101  /* acMediaCmd_GetVersionAsync — fw version */
#define MB_CMD_SYSTEM_TYPE          0x0102  /* board_type | fw_ver<<8 */
#define MB_CMD_GET_SERIAL           0x0103  /* acMediaCmd_GetSerialIdAsync */
#define MB_CMD_GET_NET_PROPERTY     0x0104  /* acMediaCmd_GetNetworkPropertyAsync */
#define MB_CMD_HARDWARE_TEST        0x0301  /* writes "TEST OK" to result ptr */

#define MB_STATUS_READY             5

/* Media board state — single source of truth for all mbcom responses.
 * On real hardware: jumpers (JP1/JP2) + firmware on the media board provide
 * these values. The kernel reads DIMM factor via port 0x40F4, and the media
 * board firmware responds to mbcom commands using the same underlying state.
 * Serial comes from flash ROM MBDT header at 0xFFE10.
 * See: https://newastrocity.wordpress.com/2013/08/04/sega-chihiro/ (jumpers)
 *      GXTX: "there's a IO port which when queried returns the jumpers" */
static struct {
    uint8_t  dimm_factor;    /* 0=128M, 1=256M, 2=512M, 3=1024M (JP1/JP2 jumpers) */
    uint32_t dimm_size;      /* computed: 0x08000000 << factor (bytes) */
    uint16_t fw_version;     /* firmware version reported by mbcom 0x0101 */
    uint8_t  board_type;     /* 0=NAOMI, 3=GD-ROM, 4=Chihiro */
    uint8_t  status;         /* boot phase: 0-4=loading, 5=READY */
    uint8_t  progress;       /* loading completion: 0-100 */
    char     serial[17];     /* from flash ROM MBDT+0x10, or "0000000000000000" */
    uint32_t net_ip;         /* network IP in LE (default 10.0.0.1 = 0x0A000001) */
} mediaboard;

static void mediaboard_init(void);

/* #define DEBUG_CHIHIRO */

/* Always log LPC accesses during development */
#define CHIHIRO_LOG 1

typedef struct ChihiroLPCState {
    ISADevice dev;
    MemoryRegion ioport;

    /* mbcom communication buffers (baseboard command/response protocol) */
    uint8_t mbcom_read_buffer[32];
    uint8_t mbcom_write_buffer[32];

    /* Kernel-loaded detection timer (polls until 2BL decrypts kernel) */
    bool host_seen;   /* the guest has talked to us at least once */
    uint32_t lpc_reg_addr;        /* MediaBoard register address (set via port 0x4004) */
    uint32_t lpc_reg_data;        /* MediaBoard register data (read via port 0x4000) */

    /* IRQ10 for baseboard → SEGABOOT communication */
    qemu_irq irq10;
    QEMUTimer *irq10_timer;

    /* USB hotplug timers (simulates staggered AN2131 I2C firmware boot) */
    QEMUTimer *usb_hotplug_timer;     /* QC at T+1500ms */
    QEMUTimer *usb_hotplug_sc_timer;  /* SC at T+1700ms */
    QEMUTimer *diag_arm_timer;   /* arms the SEGABOOT diagnostic timer */
    bool diag_armed;

    /* Diagnostic: periodic state machine dump */
    QEMUTimer *diag_timer;

    /* LPC port read counters for v136 instrumentation */
    uint32_t lpc_401e_reads;   /* MbcomCommand (state 2) — port 0x401E (firmware) */
    uint32_t last_bootstate;   /* previous bootstate to detect changes */
    uint16_t lpc_scratch_4026;    /* Port 0x4026 read-write scratch register */
    uint8_t  mbcom_e0_status;     /* Port 0x40E0 interrupt source: bit0=ASIC (0x29),
                                   * bit2=Ether/NetDIMM (0xA9) — per acLib */
    bool     mbcom_resp_ready;    /* Game-mode: response pending → port 0x40F0 returns 0x0100 */

    /* Baseboard DMA register state (indirect access via 0x4004/0x4000) */
    uint32_t bb_reg_addr;       /* 0xA0000020: indirect address pointer */
    uint32_t bb_reg_status;     /* 0xA0000040: DMA status/enable */
    bool     bb_dma_active;     /* true when 0xA0000040 bit31 set (burst mode) */
    uint32_t bb_dma_count;      /* dwords written in current burst */
    bool     bb_event_pending;  /* baseboard has an event for SEGABOOT */

    /* Type-3 ASIC control registers (written by SEGABOOT after firmware upload) */
    uint32_t asic_cpu_ctrl;     /* 0x80000140: ASIC CPU start/ready latch */

    /* DIMM board mailbox: commands at 0x84000020, responses at 0x84000000 */
    uint32_t dimm_cmd[8];      /* 8-dword command block written by SEGABOOT */
    uint32_t dimm_resp[8];     /* 8-dword response block read by SEGABOOT */
    uint32_t dimm_cmd_idx;     /* current dword index in command write */
    bool     dimm_resp_ready;  /* true when response buffer has new data */
    uint32_t dimm_cmd_count;   /* total commands processed */
    uint16_t dimm_next_seq;    /* next sequence number for unsolicited events */
    QEMUTimer *dimm_resp_timer;  /* delayed IRQ10 after execute trigger (Type-3) */

    int64_t    last_lpc_activity_ms; /* timestamp of last LPC read/write */

    /* Migration shim for the machine-wide latches (chihiro_game_running &
     * co.): pre_save copies the globals here so they are serialized with
     * the device; post_load restores them. Without this a load into a
     * fresh session leaves game_running=false and the SEGABOOT IRQ10
     * poll keeps firing into a running game — harmless while the
     * mediaboard is idle, fatal while it is streaming (cinematics). */
    bool mig_game_running;
    bool mig_quickreboot_pending;
    bool mig_active;
} ChihiroLPCState;

#define CHIHIRO_LPC_DEVICE(obj) \
    OBJECT_CHECK(ChihiroLPCState, (obj), "chihiro-lpc")

static bool chihiro_active;
bool chihiro_game_running;  /* Set after QuickReboot — disables SEGABOOT DMA scan */
static int game_mode_bus_starts;  /* BUS START count since game_running became true */
static bool chihiro_boot3_reached; /* Set when SEGABOOT reaches boot=3 (checks complete) */
static bool chihiro_quickreboot_pending; /* Set at QuickReboot, consumed by port 0x40F0 handler */
static bool chihiro_mbcom_bootstrap_done; /* Reset on QuickReboot so game gets fresh DIMM_SIZE */
static bool chihiro_e1_armed; /* Reset on QuickReboot to prevent premature response delivery */
char chihiro_game_filename[64]; /* Game XBE filename saved at boot=3 */
char chihiro_game_dir[1024];   /* Game directory path (from dvd_path) */

static char chihiro_save_path[2048];
static bool chihiro_resolve_save_path(void);
static void chihiro_resolve_card_path(int player, char *out, size_t out_len);
bool chihiro_board_type3;      /* true = ASIC (Type-3), false = FPGA (Type-1) */
int chihiro_region_setting;   /* 0=JP, 1=US, 2=EX → eeprom[0x1F00] = value+1 */
bool chihiro_freeplay_setting; /* ic11 byte 0x23/0x63 = freeplay in ACBU coin struct */
static uint8_t *chihiro_flash_rom;
static uint32_t chihiro_flash_rom_size;

static void mediaboard_init(void)
{
    mediaboard.dimm_factor = SEGA_DIMM_SIZE_512M;
    mediaboard.dimm_size   = 0x08000000u << mediaboard.dimm_factor; /* 512MB */
    /* On real hardware, the SH4 on the DIMM board (VxWorks) provides these
     * values. 0x0317 = 3.17, from GXTX's real Chihiro SYSTEM INFO. */
    mediaboard.fw_version  = 0x0317;
    mediaboard.board_type  = 4; /* Chihiro */
    /* Instant READY/100% — the real SH4/VxWorks on the DIMM board progresses
     * through phases 0→5, 0→100%. This may cause MEDIA BOARD TEST in the
     * service menu to show "CHECKING 0%" / "STATUS ----" instead of the
     * full progression. Games don't care — they only check final state. */
    mediaboard.status      = MB_STATUS_READY;
    mediaboard.progress    = 100;
    mediaboard.net_ip      = 0x0100000A; /* 10.0.0.1 LE */
    memset(mediaboard.serial, 0, sizeof(mediaboard.serial));

    /* Read serial from flash ROM MBDT header if available */
    if (chihiro_flash_rom && chihiro_flash_rom_size > 0xFFE20 &&
        memcmp(chihiro_flash_rom + 0xFFE00, "MBDT", 4) == 0) {
        memcpy(mediaboard.serial, chihiro_flash_rom + 0xFFE10, 16);
    }
}

static ChihiroLPCState *chihiro_lpc_global;
uint32_t chihiro_usb_sm_pa;  /* PA of USB state machine globals at VA 0xC3F10 */

unsigned chihiro_log_mask;
bool lpc_log_verbose = false;

static uint8_t chihiro_mbcom_command[512];
static uint8_t chihiro_mbcom_response[512];

/* USB devices for delayed hotplug (simulates AN2131 I2C firmware boot) */
static USBDevice *chihiro_usb_qc = NULL;
static USBDevice *chihiro_usb_sc = NULL;

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
static int ohci_bus_start_count = 0;
static bool ohci_bus_running;

void chihiro_on_ohci_bus_start(void)
{
    ohci_bus_running = true;

    if (!chihiro_active) {
        return;
    }

    ChihiroLPCState *s = chihiro_lpc_global;
    if (!s) {
        return;
    }

    ohci_bus_start_count++;
    fprintf(stderr, "[%07lld] Chihiro USB: BUS START #%d\n", TS_MS, ohci_bus_start_count);

    /* Detect QuickReboot back to SEGABOOT: when in game mode, the game's own
     * BUS START is the first one (game_mode_bus_starts goes 0→1). The second
     * BUS START means SEGABOOT reloaded after a soft reinit (QuickReboot). */
    if (chihiro_game_running) {
        game_mode_bus_starts++;
        if (game_mode_bus_starts >= 2) {
            fprintf(stderr, "[%07lld] QUICKREBOOT (BUS START #%d in game mode)\n",
                    TS_MS, game_mode_bus_starts);
            chihiro_game_running = false;
            chihiro_boot3_reached = false;
            chihiro_quickreboot_pending = true;
            chihiro_mbcom_bootstrap_done = false;
            chihiro_e1_armed = false;
            memset(chihiro_mbcom_command, 0, 32);
            memset(chihiro_mbcom_response, 0, 32);
            if (s) {
                s->diag_armed = false;
                timer_mod(s->diag_arm_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
                s->lpc_401e_reads = 0;
                s->lpc_scratch_4026 = 0;
                s->mbcom_e0_status = 0;
                s->mbcom_resp_ready = false;
                s->bb_dma_active = false;
                s->bb_dma_count = 0;
                s->bb_reg_addr = 0;
                s->bb_reg_status = 0;
                s->bb_event_pending = false;
                s->dimm_cmd_count = 0;
                s->dimm_next_seq = 1;
                s->dimm_cmd_idx = 0;
                s->dimm_resp_ready = false;
                memset(s->dimm_cmd, 0, sizeof(s->dimm_cmd));
                memset(s->dimm_resp, 0, sizeof(s->dimm_resp));
                timer_del(s->dimm_resp_timer);
                timer_mod(s->diag_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
            }
        }
    }

    /* First BUS START: schedule initial hotplug (devices not yet attached) */
    /* Subsequent BUS STARTs (game kernel): detach + re-attach for fresh CSC */
    if (chihiro_usb_qc && chihiro_usb_qc->attached) {
        usb_device_detach(chihiro_usb_qc);
    }
    if (chihiro_usb_sc && chihiro_usb_sc->attached) {
        usb_device_detach(chihiro_usb_sc);
    }

    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    /* Schedule QC hotplug at BUS_START + 50ms */
    timer_mod(s->usb_hotplug_timer, now + 50);

    /* Schedule SC hotplug at BUS_START + 100ms */
    timer_mod(s->usb_hotplug_sc_timer, now + 100);
}

/* SEGABOOT tears the bus down to hand the machine over to the game. */
void chihiro_on_ohci_bus_stop(void)
{
    bool was_running = ohci_bus_running;
    ohci_bus_running = false;

    ChihiroLPCState *s = chihiro_lpc_global;
    if (!was_running || !chihiro_active || chihiro_game_running || !s) {
        return;
    }

    chihiro_game_running = true;
    game_mode_bus_starts = 0;
    memset(chihiro_mbcom_command, 0, 32);
    s->mbcom_resp_ready = false;
    s->mbcom_e0_status = 0;
    CHIHIRO_LOGF(BOOT, "game started (bus stopped by SEGABOOT)\n");
}


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
    if (chihiro_usb_qc && !chihiro_usb_qc->attached) {
        usb_device_attach(chihiro_usb_qc, &error_abort);
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
    if (chihiro_usb_sc && !chihiro_usb_sc->attached) {
        usb_device_attach(chihiro_usb_sc, &error_abort);
    }
}

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

/* Card reader state: one CRP-1231 per player, driven by the SC 8051 UARTs */
static CardReaderState card_state[2];
static bool card_reader_initialized;

/* Exposed to the SC (AN2131) layer so the 8051 UART drives the readers
 * directly. [0] = MIDI/UART1 reader, [1] = RS-232C/UART0 reader. */
CardReaderState *chihiro_card_reader_global = card_state;
/* A card game owns the SC MIDI (UART1) channel: the OutRun 2 drive board must
 * not intercept it (only OR2 uses the drive board, and it has no card reader).
 * Before this the FFB drive board answered on MIDI for every game and its
 * bytes corrupted player 1's card channel. */
bool chihiro_card_reader_enabled;

/* Physical card-insertion microswitch state for the JVS input path. */
bool chihiro_card_reader_present(int player)
{
    if (player < 0 || player > 1) return false;
    return card_state[player].card_present;
}

/*
 * Card reader support. The serial data path is fully LLE: the game's USB
 * vendor requests reach the real SC 8051 firmware, which talks to the
 * CRP-1231 readers over its two UARTs (chihiro-an2131.c). This timer only
 * manages card insertion (the UI assignment) and forces the operator
 * settings the card screen gates on.
 */
static void chihiro_card_force_operator_settings(void)
{
    /* serial_flag=0: keeps module alive (sf==3 kills it) */
    uint32_t sf_pa = chihiro_va_to_pa(0x467fd0);
    if (sf_pa != 0xFFFFFFFF) {
        uint32_t v = 0;
        cpu_physical_memory_write(sf_pa, &v, 4);
    }
    /* card_en=1: tells game card reader HW is present */
    uint32_t ce_pa = chihiro_va_to_pa(0x447265);
    if (ce_pa != 0xFFFFFFFF) {
        uint8_t v = 1;
        cpu_physical_memory_write(ce_pa, &v, 1);
    }
    /* sched_mode=0: disable closing-time card scheduler.
     * DAT_00447270=1 → FUN_00033120()=1 → detection SM sets bit8
     * per player → module bit3 set → FUN_0002c750()=0 → no card screen */
    uint32_t sm_pa = chihiro_va_to_pa(0x447270);
    if (sm_pa != 0xFFFFFFFF) {
        uint32_t v = 0;
        cpu_physical_memory_write(sm_pa, &v, 4);
    }
}

/*
 * module 0x22 bit0=1: card_module_init_cb reads card_en at boot (before our
 * force), so the module header bit0 stays 0 for the US region. Walk the
 * hardware module linked list to find and force it. Re-scan each tick until
 * the game has initialized the data block (flags != 0 after masking bit 0),
 * since the module list may be rebuilt across SEGABOOT → game transitions.
 */
static void chihiro_card_force_module22(int tick)
{
    static uint32_t mod22_data_va;

    uint32_t node_va = 0x451030;
    uint32_t found_data = 0;
    for (int i = 0; i < 32 && node_va != 0; i++) {
        uint32_t node_pa = chihiro_va_to_pa(node_va);
        if (node_pa == 0xFFFFFFFF) break;
        uint32_t id;
        cpu_physical_memory_read(node_pa, &id, 4);
        if (id == 0x22) {
            cpu_physical_memory_read(node_pa + 0x10, &found_data, 4);
            break;
        }
        cpu_physical_memory_read(node_pa + 0x38, &node_va, 4);
    }

    if (found_data == 0)
        return;

    mod22_data_va = found_data;

    uint32_t data_pa = chihiro_va_to_pa(mod22_data_va);
    if (data_pa == 0xFFFFFFFF)
        return;

    uint32_t flags;
    cpu_physical_memory_read(data_pa, &flags, 4);

    uint32_t want = (flags | 1) & ~8u;
    if (flags != want) {
        if (tick % 300 == 0)
            CHIHIRO_LOGF(CARD, "module 0x22 flags=0x%08X → 0x%08X\n",
                         flags, want);
        cpu_physical_memory_write(data_pa, &want, 4);
    }

    for (int p = 0; p < 2; p++) {
        uint32_t pp_pa = chihiro_va_to_pa(mod22_data_va + 8 + p * 0x12a0);
        if (pp_pa == 0xFFFFFFFF) continue;
        uint32_t pf;
        cpu_physical_memory_read(pp_pa, &pf, 4);
        if (!(pf & 1)) {
            pf |= 1;
            cpu_physical_memory_write(pp_pa, &pf, 4);
        }
    }
}

static void chihiro_card_reader_tick(void)
{
    static int card_tick;
    static char card_last_path[2][1200];
    card_tick++;

    if (!card_reader_initialized) {
        card_reader_init(&card_state[0]);
        card_reader_init(&card_state[1]);
        char path[1200];
        for (int p = 0; p < 2; p++) {
            chihiro_resolve_card_path(p, path, sizeof(path));
            snprintf(card_last_path[p], sizeof(card_last_path[p]), "%s", path);
            card_reader_insert(&card_state[p], path);
        }
        card_reader_initialized = true;
        fprintf(stderr, "Chihiro: card reader enabled\n");
    }

    chihiro_card_force_operator_settings();
    chihiro_card_force_module22(card_tick);

    /* Inserting a card is the player's gesture: the UI assignment IS the
     * inserted card. Changing it swaps cards (the old one is flushed and
     * ejected first), clearing it ejects with no replacement. CARD_IN is set
     * via JVS sw1 |= 0x20 (PUSH5) in xemu-input.c. */
    for (int p = 0; p < 2; p++) {
        char now_path[1200];
        chihiro_resolve_card_path(p, now_path, sizeof(now_path));
        if (strcmp(now_path, card_last_path[p]) != 0) {
            snprintf(card_last_path[p], sizeof(card_last_path[p]), "%s",
                     now_path);
            card_reader_remove(&card_state[p]);
            card_state[p].card_path[0] = '\0';
            if (now_path[0])
                card_reader_insert(&card_state[p], now_path);
            fprintf(stderr, "Chihiro: card P%d %s\n", p + 1,
                    now_path[0] ? "inserted" : "ejected");
        }
    }
}

/* Only the OutRun 2 family (outrun2.xbe, OR2SP) talks to a drive board. */
static bool chihiro_cabinet_has_driveboard(void)
{
    return strncasecmp(chihiro_game_filename, "outrun2", 7) == 0;
}

/* Execution VAs of the SEGABOOT the board boots: the second megabyte of the
 * flash. Other dumps carry other builds. */
#define SEGABOOT_VERSION    "2.13.0"
#define SEGABOOT_LOGO_VTABLE 0x0001F60C  /* CLogo vtable */
#define SEGABOOT_LOGO_UPDATE 0x000259D0  /* its second entry, CLogo::Update */
#define SEGABOOT_APP_VTABLE  0x0001F0C0  /* application object vtable */
#define SEGABOOT_APP_LOGO    0x440       /* app field holding the CLogo pointer */

static bool sb_read_va(uint32_t va, void *buf, unsigned len)
{
    uint8_t *p = buf;

    while (len) {
        uint32_t pa = chihiro_va_to_pa(va);
        if (pa == 0xFFFFFFFF)
            return false;
        unsigned n = 0x1000 - (va & 0xFFF);
        if (n > len)
            n = len;
        cpu_physical_memory_read(pa, p, n);
        va += n;
        p += n;
        len -= n;
    }
    return true;
}

static bool sb_resident(void)
{
    uint32_t update;

    return sb_read_va(SEGABOOT_LOGO_VTABLE + 4, &update, 4) &&
           update == SEGABOOT_LOGO_UPDATE;
}

/* Located by vtable, not by address: operator new moves it. The two hops
 * back through the app object rule out a stray copy of the constant. */
static uint32_t sb_find_logo(void)
{
    for (uint32_t va = 0x00010000; va < 0x00800000; va += 0x1000) {
        uint32_t pa = chihiro_va_to_pa(va);
        if (pa == 0xFFFFFFFF)
            continue;

        uint32_t page[1024];
        cpu_physical_memory_read(pa, page, sizeof(page));

        for (unsigned i = 0; i < ARRAY_SIZE(page); i++) {
            if (page[i] != SEGABOOT_LOGO_VTABLE)
                continue;

            uint32_t obj = va + i * 4, app, app_vtable, back;
            if (!sb_read_va(obj + 4, &app, 4) ||
                !sb_read_va(app, &app_vtable, 4) ||
                app_vtable != SEGABOOT_APP_VTABLE ||
                !sb_read_va(app + SEGABOOT_APP_LOGO, &back, 4) || back != obj)
                continue;

            return obj;
        }
    }
    return 0;
}

/* SEGABOOT's own table, VA 0x1CEE0. */
static const char *sb_error_message(uint32_t code)
{
    static const char *messages[] = {
        [1]  = "This game is not acceptable by main board.",
        [2]  = "Main board malfunctioning.",
        [3]  = "Bad serial number on main board.",
        [4]  = "Bad serial number on media board.",
        [5]  = "This game is not acceptable by main board.",
        [6]  = "This game is not available on this system.",
        [11] = "JVS I/O board is not connected to main board.",
        [12] = "JVS I/O board does not fulfill the game spec.",
        [13] = "Communication error occurred between main board and JVS I/O board.",
        [14] = "Network firmware version does not fulfill the game spec.",
        [21] = "This game is not acceptable by main board.",
        [22] = "Communication error occurred between main board and media board.",
        [23] = "GD-ROM drive cover is open.",
        [24] = "GD-ROM is not found.",
        [25] = "Cannot access GD-ROM drive.",
        [26] = "Media board malfunctioning.",
        [27] = "DIMM memory is not enough.",
        [31] = "This game is not acceptable by main board.",
        [32] = "DIMM memory is not enough.",
        [33] = "Gateway is not found.",
        [34] = "Gateway cannot be found.",
        [51] = "Wrong video output setting of horizontal scanning frequency.",
        [52] = "Wrong video output setting of horizontal/vertical screen.",
        [53] = "Wrong DIMM memory size setting.",
    };

    if (code < ARRAY_SIZE(messages) && messages[code])
        return messages[code];
    return "Unknown error occurred.";
}

/* Report SEGABOOT state transitions and the error it puts on screen. */
static void chihiro_segaboot_poll(void)
{
    static const char *const states[] = {
        "logo fade in", "init", "logo hold", "waiting for media board",
        "verifying game", "fade out", "launching", "ERROR DISPLAY", "done"
    };
    static uint32_t logo_va, prev_state = UINT32_MAX, prev_error;
    static unsigned attempts;

    if (!sb_resident()) {
        logo_va = 0;
        attempts = 0;
        return;
    }
    if (!logo_va) {
        if (attempts++ > 100)
            return;
        logo_va = sb_find_logo();
        if (!logo_va)
            return;
        CHIHIRO_LOGF(BOOT, "SEGABOOT: state machine at VA %#x\n", logo_va);
        prev_state = UINT32_MAX;
        prev_error = 0;
    }

    uint32_t f[2];  /* CLogo +0x10: error code then state */
    if (!sb_read_va(logo_va + 0x10, f, sizeof(f))) {
        logo_va = 0;
        return;
    }
    uint32_t error = f[0], state = f[1];

    if (state != prev_state && state < ARRAY_SIZE(states)) {
        CHIHIRO_LOGF(BOOT, "SEGABOOT: state %u — %s\n", state, states[state]);
        prev_state = state;
    }
    if (error && error != prev_error) {
        CHIHIRO_ERRF("SEGABOOT: *** %s %02u — %s ***\n",
                     error < 50 ? "Error" : "Caution", error,
                     sb_error_message(error));
        prev_error = error;
    }
}

/*
 * SEGABOOT reached boot=3 (checks complete). Recover the game executable
 * name so the save file can be resolved: boot.id is loaded at PA 0x4F000
 * with magic "BTID", "XBAM" at +0x20 and the executable at +0xA0
 * (e.g. "\hod3xb.xbe"). Fall back to scanning SEGABOOT data for ".xbe".
 */
/* Sega netboot boot.id: "BTID" at 0, "XBAM" at 0x20, game executable at
 * 0xA0 (31 chars, backslash-prefixed). The one parser for every source. */
bool chihiro_bootid_executable(const uint8_t *bid, char *out, size_t out_len)
{
    if (memcmp(bid, "BTID", 4) != 0 || memcmp(bid + 0x20, "XBAM", 4) != 0) {
        return false;
    }
    char exec[32];
    memcpy(exec, bid + 0xA0, 31);
    exec[31] = 0;
    const char *name = exec;
    while (*name == '\\' || *name == '/') name++;
    if (!*name) {
        return false;
    }
    g_strlcpy(out, name, out_len);
    return true;
}

/* The single writer of the game executable name; every consumer (save
 * file, JVS profile, drive board) reads chihiro_game_filename. */
void chihiro_set_game_executable(const char *name)
{
    if (strcmp(chihiro_game_filename, name) == 0) {
        return;
    }
    g_strlcpy(chihiro_game_filename, name, sizeof(chihiro_game_filename));
    printf("Chihiro: game → '%s'\n", chihiro_game_filename);
}

/* A game launched from a directory carries its boot.id as a file. */
static void chihiro_capture_game_filename_from_dir(void)
{
    if (chihiro_game_filename[0] || !chihiro_game_dir[0]) {
        return;
    }
    char bootid_path[1100];
    snprintf(bootid_path, sizeof(bootid_path), "%s/boot.id", chihiro_game_dir);
    FILE *f = fopen(bootid_path, "rb");
    if (!f) {
        return;
    }
    uint8_t bid[CHIHIRO_BOOTID_LEN];
    char name[64];
    if (fread(bid, 1, sizeof(bid), f) == sizeof(bid) &&
        chihiro_bootid_executable(bid, name, sizeof(name))) {
        chihiro_set_game_executable(name);
    }
    fclose(f);
}

/* SEGABOOT loads boot.id at PA 0x4F000 before it reaches boot=3: the
 * fallback when the image was not parsed on the host. */
static void chihiro_capture_game_filename(void)
{
    if (chihiro_game_filename[0]) {
        return;
    }
    uint8_t bid[CHIHIRO_BOOTID_LEN];
    char name[64];
    cpu_physical_memory_read(0x4F000, bid, sizeof(bid));
    if (chihiro_bootid_executable(bid, name, sizeof(name))) {
        chihiro_set_game_executable(name);
    }
}

static void chihiro_on_boot3(void)
{
    chihiro_boot3_reached = true;
    chihiro_capture_game_filename();

    /* LLE: SEGABOOT calls XLaunchNewImageA which allocates LDP, marks it
     * persistent, and fills launch data. Kernel's STICKY section preserves
     * the LaunchDataPage pointer across QuickReboot. */

    if (!chihiro_game_filename[0] || chihiro_save_path[0])
        return;

    if (!chihiro_resolve_save_path()) {
        fprintf(stderr, "Chihiro: save path resolve failed (dir='%s' file='%s')\n",
                chihiro_game_dir, chihiro_game_filename);
        return;
    }
    if (!chihiro_usb_save_load(chihiro_save_path))
        fprintf(stderr, "Chihiro: no save found at %s\n", chihiro_save_path);
}

/*
 * Periodic tick: drives the card reader workaround while a game runs, and
 * watches the SEGABOOT state machine before that.
 */
static void chihiro_diag_timer_cb(void *opaque)
{
    ChihiroLPCState *s = (ChihiroLPCState *)opaque;

    if (chihiro_game_running) {
        /* Cabinet wiring: the OutRun 2 cabinets hang the FFB drive board off
         * SC UART1 and have no card readers; the card cabinets wire a
         * CRP-1231 there instead — never both. Re-evaluated every tick so the
         * UI toggle takes effect live and hands MIDI back. */
        chihiro_card_reader_enabled = g_config.chihiro.card_reader.enable &&
                                      !chihiro_cabinet_has_driveboard();
        if (chihiro_card_reader_enabled)
            chihiro_card_reader_tick();
        timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
        return;
    }

    chihiro_segaboot_poll();

    /* Re-resolve PAs every tick: the game may change CR3, and stale PAs
     * would read the wrong data. */
    uint32_t bootstate = 0;
    uint32_t bootstate_pa = chihiro_va_to_pa(0x89C48);

    if (bootstate_pa != 0xFFFFFFFF)
        cpu_physical_memory_read(bootstate_pa, &bootstate, 4);

    /* Detect bootstate changes between ticks (guard against garbage VAs) */
    if (bootstate != s->last_bootstate && bootstate < 100 &&
        s->last_bootstate < 100) {
        if (bootstate == 3)
            chihiro_on_boot3();
        s->last_bootstate = bootstate;
    }

    timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
}

/*
 * SEGABOOT Initialization (RE reference — baseboard_init at VA 0x41EF0)
 *
 * SEGABOOT's baseboard_init runs this sequence:
 *   1. UsbEnumPoll()         → polls OHCI for QC/SC USB devices → Error 02 if fail
 *   2. RegisterClassDriver() → registers AN2131 class driver    → Error 02 if fail
 *   3. ReadEEPROM(0)         → reads ic10 EEPROM via vendor 0x16
 *   4. ReadEEPROM(1)         → reads ic11 EEPROM via vendor 0x17
 *   5. InitMbcom()           → initializes mediaboard DMA communication
 *   6. return 0              → SUCCESS
 *
 * Boot state machine (CLogo::Update at VA 0x2EC00, decompiled):
 *   0 logo fade in → 1 init → 2 logo hold → 3 wait for the media board
 *   → 4 verify the game → 5 fade out → 6 launch → 8 done.
 *   Any error jumps to 7, which is the screen that shows the code.
 *   State 3 gives up after 0x95F ticks with error 22; state 4 runs the
 *   serial, region and boot.id checks. See chihiro_segaboot_poll().
 *
 * Serial format: "%%%@-##@########" (e.g. "BEER-01A00000001")
 *   Main serial from ic10 EEPROM [0x1F10], media serial from mbcom CMD 0x0103.
 *
 * All former RAM patches have been removed — LLE handles everything natively.
 * The RE findings below document the SEGABOOT functions we had to understand
 * to reach full LLE. Preserved for reference.
 *
 * === SEGABOOT Function Map (fpr-23887 / fpr-21042 variants) ===
 *
 * GetQcStatusByte0 (VA 0x3AD80 / 0x2AD70):
 *   E8 xx xx xx xx    call GetQcStatus (0x51BB0)
 *   0F B6 00          movzx eax, byte ptr [eax]   → DAT_000c5d01
 *   C3                ret
 *   Returns status buffer byte. 0 = OK → caller takes CreateThread path.
 *   LLE: buffer stays 0 natively (QC emulation correct).
 *
 * CreateThread return check (VA 0x425DE):
 *   85 C0             test eax, eax
 *   A3 34 A1 08 00    mov [0x8A134], eax          → thread handle
 *   5B                pop ebx
 *   75 12             jne +0x12                    → success path
 *   LLE: CreateThread succeeds natively.
 *
 * UsbPollQC_inner / UsbPollSC_inner (VA 0x51140 / 0x51150):
 *   USB poll functions called from the poll thread. Return 0 = no new data.
 *   LLE: real OHCI USB transactions handle polling.
 *
 * EncryptionCheck (VA 0x3A953):
 *   33 C9             xor ecx, ecx
 *   85 C0             test eax, eax               → USB transfer result
 *   0F 9D C1          setge cl
 *   5F 49 83 E1 02    ...
 *   Calls USB transfer (0x51340 → 0x1A0B0) via registered class driver.
 *   LLE: class driver registered natively, transfer succeeds.
 *
 * MbcomPollReady (VA 0x3DBC0):
 *   8A 44 24 04       mov al, [esp+4]             → slot index
 *   E8 67 FD FF FF    call FindSlot (0x3D930)
 *   85 C0             test eax, eax
 *   74 0E             je 0x3DBDB
 *   Returns 1 when baseboard DMAs response into slot[+2].
 *   LLE: DMA inject + clear-on-read delivers real responses.
 *
 * GetBootData (VA 0x41880):
 *   A1 50 9C 08 00    mov eax, [0x89C50]          → boot data pointer
 *   85 C0             test eax, eax
 *   74 0F             je 0x41898
 *   83 3D 48 9C 08 00 03  cmp dword [0x89C48], 3  → bootstate == 3?
 *   Returns [0x89C50]+0xFF000000 if boot==3 && ptr!=0, else 0.
 *   If 0 at state=3 with flag==0x21 → ERROR 27 after 2400 ticks.
 *   LLE: real boot data flows through mbcom.
 *
 * CheckMainBoardSerial (VA 0x2EBF0) / CheckMediaBoardSerial (VA 0x2EC40):
 *   Call MatchSerialFormat with pattern "%%%@-##@########"
 *   (3 letters, 1 alphanum, '-', 2 digits, 1 alphanum, 8 digits = 16 chars).
 *   Example: "AAEE-01D44744715", "BEER-01A00000001".
 *   Main serial failure → Error 03, media serial failure → Error 04.
 *   LLE: valid serials provided from ic10 EEPROM [0x1F10] and mbcom CMD 0x0103.
 *
 * AV / video mode (v176 finding):
 *   SEGABOOT checks NV2A video mode. Fixed by setting EEPROM video_standard
 *   with AV_FLAGS_HDTV_480p (0x00080000) → kernel configures NV2A for
 *   progressive scan 31kHz → SEGABOOT's video check passes naturally.
 *
 * Error value diagnostic (VA 0x2E3AB):
 *   C7 07 14 00 00 00    mov [edi], 0x14             → error code 20 (decimal)
 *   Used to trace error injection path. Not reached with correct emulation.
 *
 * === Byte Signatures (for future RE / other SEGABOOT versions) ===
 *
 * UsbEnumPoll:          E8 64 F6 FF FF 85 C0 74 0F           (fpr-23887)
 *                       E8 EF F1 FF FF 85 C0 0F 85           (fpr-21042)
 * RegisterClassDriver:  E8 27 54 00 00 85 C0 74 0F           (fpr-23887)
 *                       E8 33 95 00 00 85 C0 74 0F           (fpr-21042)
 * GetQcStatusByte0:     E8 2B 6E 01 00 0F B6 00 C3           (fpr-23887)
 *                       E8 EB 96 01 00 0F B6 00 C3           (fpr-21042)
 * EncryptionCheck:      33 C9 85 C0 0F 9D C1 5F 49 83 E1 02
 * CheckMainSerial:      85 C0 75 D6 B8 03 00 00 00 5E C3     (fpr-23887)
 *                       E8 B4 F7 FF FF 85 C0 75 07 B8 03 ... (fpr-21042)
 * CheckMediaSerial:     85 C0 75 0A 5F B8 04 00 00 00 5E C2 04 00
 *
 * === SEGABOOT Data Addresses ===
 *
 * baseboard_dev[] array:
 *   VA 0xA3778 (fpr-23887) / VA 0xE9BD0 (fpr-21042), stride 0x218, flags at +4.
 *
 * Key variables:
 *   [0x87AFC] = CheckErrors state (0-8)
 *   [0x87AE8] = error counter
 *   [0x87AF8] = ready flag
 *   [0x89C38] = gate
 *   [0x89C48] = bootstate (0-3)
 *   [0x89C4C] = MbcomNegotiate flag (0x21 on success)
 *   [0x89C50] = boot data pointer
 *   [0x896A8] = mbcom slot count
 *   [0x896B4] = slot[0].flag
 *   [0x8A128] = MainUpdate flag
 *   [0x8A134] = USB poll thread handle
 *   [0x8A138] = USB poll thread state
 *   [0xD07A8] = XBE2 game state (must reach 4)
 *   [0xCB9EC] = XBE2 CE state machine (mirrors 0x87AFC)
 *
 * Kernel addresses:
 *   VA 0x8003B1D8 (PA 0x3B1D8) = XboxGameRegion, STICKY section
 *     ROM value 0x80000000 (manufacturing bit). Game cert region at cert+0xA0.
 *
 * LaunchDataPage (LDP) integrity:
 *   Pointer at PA 0x3B3D8, canary at PA 0x3B400.
 *   LDP page layout: [+0x00] type (0=none, 1=error, 2=game), [+0x08] path (255 chars).
 *   Error info: [+0x400] error context, [+0x408] error type (1=generic 3=region 5=media).
 *   Error LDP page also at PA 0x0E000 (kernel error dump area).
 *
 * NV2A video registers (physical MMIO):
 *   PRAMDAC base 0xFD680000: +0x800 VDISPLAY_END, +0x810 VSYNC_END, +0x818 VVALID_END,
 *     +0x820 HDISPLAY_END, +0x838 HVALID_END, +0x508 VPLL_COEFF.
 *   PCRTC base 0xFD600000: +0x804 PCRTC_CONFIG.
 *   PRMCIO base 0xFD601000: +0x39 interlace flag.
 *
 * DMA slot layout (boot data flow):
 *   initPtr [0x896AC], slot META at [0x89740] stride 0x40, slot DATA at [0x89760] stride 0x40.
 *   Up to 4 slots. MbcomPollReady checks slot[index+2] for baseboard DMA response.
 *
 * === v274 Discovery: Why UsbEnumPoll Patches Broke USB ===
 *
 * NOPing UsbEnumPoll prevented the kernel from completing the USB enumeration
 * sequence (PortReset → GET_DESC → SET_ADDRESS → GET_CONFIG_DESC → SET_CONFIG).
 * Without SET_CONFIG, RegisterClassDriver never set bit 0x20 in baseboard_dev
 * flags → all JVS communication failed. Fix: let SEGABOOT run unmodified,
 * USB devices in chihiro-usb.c handle enumeration via OHCI natively.
 */
static void chihiro_arm_diag_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    if (s->diag_armed) return;
    if (chihiro_game_running) return;

    /* SEGABOOT is up: arm the diagnostic timer */
    s->diag_armed = true;

    /* Start diagnostic timer to monitor SEGABOOT state machine */
    s->diag_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, chihiro_diag_timer_cb, s);
    timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

/* Called from SMC SCRATCH write handler when value=0x04 (QuickReboot).
 * HalReturnToFirmware(QuickReboot) writes SCRATCH=0x04 before system reset.
 * Multiple QuickReboots happen: service menu exit, then game boot.
 * We DON'T set boot3_reached here — game_running is detected by XBE change. */
void chihiro_on_quickreboot_signal(void)
{
    if (!chihiro_active) return;

    fprintf(stderr, "[%07lld] === QUICKREBOOT === game_running=%d boot3=%d\n",
            TS_MS, chihiro_game_running, chihiro_boot3_reached);

    /* Ignore SCRATCH=0x04 during first kernel init — the kernel writes
     * SCRATCH as part of normal boot before SEGABOOT even loads.
     * Only react after SEGABOOT has been patched at least once. */
    if (!chihiro_lpc_global || !chihiro_lpc_global->diag_armed) {
        return;
    }

    chihiro_capture_game_filename_from_dir();

    chihiro_quickreboot_pending = true;
    chihiro_game_running = false;
    chihiro_boot3_reached = false;
    chihiro_mbcom_bootstrap_done = false;
    chihiro_e1_armed = false;
    memset(chihiro_mbcom_command, 0, 32);
    memset(chihiro_mbcom_response, 0, 32);

    /* Re-arm SEGABOOT patch scanner and reset ALL mbcom/DMA state.
     * QuickReboot reloads SEGABOOT from flash ROM (patches lost).
     * Kernel code stays patched (QuickReboot keeps kernel in RAM). */
    if (chihiro_lpc_global) {
        ChihiroLPCState *s = chihiro_lpc_global;
        s->diag_armed = false;
        timer_mod(s->diag_arm_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
        s->lpc_401e_reads = 0;
        s->lpc_scratch_4026 = 0;
        s->mbcom_e0_status = 0;
        s->mbcom_resp_ready = false;

        /* Reset DMA burst state */
        s->bb_dma_active = false;
        s->bb_dma_count = 0;
        s->bb_reg_addr = 0;
        s->bb_reg_status = 0;
        s->bb_event_pending = false;

        /* Reset DIMM mailbox state */
        s->dimm_cmd_count = 0;
        s->dimm_next_seq = 1;
        s->dimm_cmd_idx = 0;
        s->dimm_resp_ready = false;
        memset(s->dimm_cmd, 0, sizeof(s->dimm_cmd));
        memset(s->dimm_resp, 0, sizeof(s->dimm_resp));
        timer_del(s->dimm_resp_timer);

        timer_mod(s->diag_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
    }
}

/* Called from SMC handler when kernel writes SMC_REG_POWER (QuickReboot).
 * v209e: Let the reset happen! Combined with scratch_reg preservation
 * (smc_reset no longer clears scratch), the kernel will detect warm boot,
 * restore MmPersistContiguousMemory pages, and load the game from LDP.
 * Blocking the reset prevented the real QuickReboot from ever occurring. */
bool chihiro_intercept_reset(void)
{
    /* DON'T set game_running here — 0x40F0 handler sets it
     * AFTER applying kernel device patches. */
    return false;  /* Always allow qemu_system_reset_request */
}

/* warmboot_diag_cb removed — kernel handles QuickReboot natively via
 * STICKY section (LaunchDataPage) and MmPersistContiguousMemory.
 * See project_quickreboot_mechanism.md for details. */

static void chihiro_dimm_process_cmd(ChihiroLPCState *s)
{
    uint16_t seq = s->dimm_cmd[0] & 0xFFFF;
    uint16_t cmd = (s->dimm_cmd[0] >> 16) & 0xFFFF;

    memset(s->dimm_resp, 0, sizeof(s->dimm_resp));

    if (chihiro_board_type3) {
        /* Type-3 (ASIC) response format — matches Dolphin AMMediaboard.cpp:
         * resp[0] = command_word | 0x80000000 (bit 31 = response valid flag)
         * resp[1+] = response data */
        s->dimm_resp[0] = s->dimm_cmd[0] | 0x80000000;

        switch (cmd) {
        case MB_CMD_INIT:
            s->dimm_resp[1] = mediaboard.dimm_size;
            break;
        case MB_CMD_STATUS:
            s->dimm_resp[1] = mediaboard.status;
            s->dimm_resp[2] = mediaboard.progress;
            break;
        case MB_CMD_GET_VERSION:
            s->dimm_resp[1] = mediaboard.fw_version;
            s->dimm_resp[2] = 1;
            break;
        case MB_CMD_SYSTEM_TYPE: { /* Dolphin byte layout */
            uint8_t *p = (uint8_t *)&s->dimm_resp[1];
            p[0] = 1;    /* flag (must be nonzero) */
            p[1] = 1;    /* media type: GDROM=1 */
            p[2] = 0;    /* development mode: 0=normal */
            p[3] = 0;
            s->dimm_resp[2] = 0; /* access count */
            break;
        }
        case MB_CMD_GET_SERIAL:
            memcpy(&s->dimm_resp[1], mediaboard.serial, 16);
            break;
        default:
            break;
        }

        if (lpc_log_verbose) fprintf(stderr, "[%07lld] DIMM T3: cmd=0x%04X seq=%u resp: %08X %08X %08X %08X\n",
                TS_MS, cmd, seq, s->dimm_resp[0], s->dimm_resp[1],
                s->dimm_resp[2], s->dimm_resp[3]);
    } else {
        /* Type-1 (FPGA) response format — 16-bit word packed:
         * resp[0] = seq, resp[1] = cmd | 0x8000, resp[2+] = data */
        s->dimm_resp[0] = seq;
        s->dimm_resp[1] = cmd | 0x8000;

        switch (cmd) {
        case MB_CMD_INIT:
            s->dimm_resp[2] = mediaboard.dimm_size;
            break;
        case MB_CMD_STATUS:
            s->dimm_resp[2] = mediaboard.status;
            s->dimm_resp[3] = mediaboard.progress;
            break;
        case MB_CMD_GET_VERSION:
            s->dimm_resp[2] = mediaboard.fw_version;
            break;
        case MB_CMD_SYSTEM_TYPE: /* low byte must be >=2 to pass board check */
            s->dimm_resp[2] = 0x8002;
            break;
        case MB_CMD_GET_SERIAL:
            memcpy(&s->dimm_resp[2], mediaboard.serial, 16);
            break;
        default:
            break;
        }
    }

    s->dimm_resp_ready = true;
    s->dimm_next_seq = seq + 1;
    s->dimm_cmd_count++;
}


static uint64_t chihiro_lpc_io_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    uint64_t r = 0;

    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);
    s->last_lpc_activity_ms = TS_MS;
    s->host_seen = true;

    if (chihiro_game_running) {
        static int game_lpc_read_log = 0;
        if (lpc_log_verbose && game_lpc_read_log < 500) {
            game_lpc_read_log++;
            fprintf(stderr, "[%07lld] GAME LPC READ port=0x%04X (reg=0x%08X)\n",
                   TS_MS, (int)(0x4000 + addr), s->lpc_reg_addr);
        }
    }

    switch (addr) {
    case 0x00: /* Port 0x4000: read baseboard register at lpc_reg_addr */
        switch (s->lpc_reg_addr) {
        case 0x80000140:
            r = s->asic_cpu_ctrl;
            if (chihiro_game_running) {
                static int cpu_rdy_log = 0;
                if (lpc_log_verbose && cpu_rdy_log < 30) { cpu_rdy_log++;
                    fprintf(stderr, "[%07lld] GAME READ 0x80000140 → 0x%08X (cpu_ctrl)\n",
                            TS_MS, (unsigned)r); }
            }
            break;
        case 0x80000160: r = 0x00; break; /* bit0=0 → Ethernet present */
        case 0x80000164: r = 0x01; break;
        /* DMA control register (acLib indexes the table {0,1,2,4,8,0x10,0x20});
         * the game writes 8 here. We answer a constant instead of latching. */
        case 0xA0001E60: r = 0x00000002; break;
        case 0xA0000000: {
            /* Indirect read: return value at address in bb_reg_addr (0xA0000020) */
            uint32_t target = s->bb_reg_addr;
            if (target == 0x80000140) {
                r = s->asic_cpu_ctrl;
            } else if (target == 0x80000160) {
                r = 0x00; /* bit0=0 → Ethernet present */
                if (chihiro_game_running) {
                    static int pcistat_log = 0;
                    if (lpc_log_verbose && pcistat_log < 50) { pcistat_log++;
                        fprintf(stderr, "[%07lld] GAME SADDR READ 0x80000160 → 0x00 (Ether present)\n", TS_MS); }
                }
            } else if (target == 0x80000164) {
                r = 0x01;
            } else if (target >= 0x84000000 && target <= 0x8400001C) {
                static int saddr_resp_read_count = 0;
                saddr_resp_read_count++;
                if (lpc_log_verbose && saddr_resp_read_count <= 500) {
                    fprintf(stderr, "[%07lld] SADDR READ 0x%08X (resp buf) → 0x%08X\n",
                            TS_MS, target, s->dimm_resp[(target - 0x84000000) / 4]);
                }
                uint32_t idx = (target - 0x84000000) / 4;
                r = s->dimm_resp[idx];
            } else if (target == 0xA0001E60) {
                r = 0x00000002; /* DMA control register — see above */
                static int a1e60_log = 0;
                if (lpc_log_verbose && a1e60_log < 20) { a1e60_log++;
                    fprintf(stderr, "[%07lld] SADDR READ 0xA0001E60 → 0x%08X (DMA ctrl)\n", TS_MS, (unsigned)r); }
            } else {
                static int unknown_saddr_log = 0;
                if (lpc_log_verbose && unknown_saddr_log < 200) {
                    fprintf(stderr, "[%07lld] SADDR READ UNKNOWN: bb_reg=0x%08X → 0\n",
                            TS_MS, target);
                    unknown_saddr_log++;
                }
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
        if (CHIHIRO_LOG && lpc_log_verbose) {
            static int lpc_read_log = 0;
            if (lpc_read_log < 500) {
                fprintf(stderr, "[%07lld] LPC REG READ [0x%08X] -> 0x%08X\n", TS_MS,
                       s->lpc_reg_addr, (unsigned)r);
                lpc_read_log++;
            }
        }
        return r;
    case SEGA_FIRMWARE_VERSION:
        /* SEGABOOT reads these once for DIMM base address calculation.
         * Game XBE reads them again and checks for "XBAM" signature.
         * First read pair returns DIMM base, subsequent reads return XBAM. */
        if (s->lpc_401e_reads > 0) {
            r = 0x4258;     /* "XB" — game checks CONCAT22(0x4020,0x401E) == "XBAM" */
        } else {
            r = 0x0000;     /* DIMM base address low word for SEGABOOT */
        }
        s->lpc_401e_reads++;
        break;
    case SEGA_XBAM_STRING_0:
        if (s->lpc_401e_reads > 1) {
            r = 0x4D41;     /* "AM" — completes "XBAM" at 0x401E-0x4020 */
        } else {
            r = 0x0100;     /* DIMM base address high word for SEGABOOT */
        }
        break;
    case SEGA_XBAM_STRING_1:
        r = 0x4258;     /* "BX" */
        break;
    case SEGA_XBAM_STRING_2:
        r = 0x4D41;     /* "MA" → full string reads as "XBAM" */
        break;
    case 0xF0: /* Board mode. SEGABOOT checks high byte: 0=Type-1, non-zero=Type-3.
               * acLib v0.71+ (WMMT1, Type-3): high byte selects device type
               * '!' vs ')'. Type ')' triggers SC search → EEPROM version read. */
        r = chihiro_board_type3 ? 0x0100 : 0x0001;
        { static int f0_log = 0; if (lpc_log_verbose && f0_log < 500) { f0_log++;
            fprintf(stderr, "[%07lld] F0 READ → 0x%04X (type3=%d)\n",
                    TS_MS, (unsigned)r, chihiro_board_type3); } }
        if (chihiro_quickreboot_pending) {
            chihiro_quickreboot_pending = false;
        }
        break;
    case SEGA_DIMM_SIZE:
        r = mediaboard.dimm_factor;     /* JP1/JP2 jumpers. Kernel computes mbcom LBA:
                                         * mbcom_start = (0x40000 << factor) - 0x8000.
                                         * Must match IDE capacity and CHIHIRO_MBCOM_BASE. */
        if (chihiro_board_type3)
            r |= 0x40;  /* Type-3: dip switch register on media board */
        break;
    case 0x26:  /* Port 0x4026: scratch register (read-write) */
        r = s->lpc_scratch_4026;
        break;
    case 0xE0: {
        r = s->mbcom_e0_status;
        static int e0_log = 0; if (lpc_log_verbose && e0_log < 2000) { e0_log++;
            fprintf(stderr, "[%07lld] E0 READ → 0x%02X (bit0=%d bit2=%d)\n",
                    TS_MS, (unsigned)r, (int)(r & 1), (int)((r >> 2) & 1)); }
        break;
    }
    case 0x84:  /* Port 0x4084 — baseboard status word (acLib names it Status) */
        r = 0x0000;
        break;
    default: {
        static int unknown_port_log = 0;
        if (lpc_log_verbose && unknown_port_log < 30) {
            fprintf(stderr, "[%07lld] LPC READ UNKNOWN port 0x%04X → 0\n",
                    TS_MS, (unsigned)(addr + 0x4000));
            unknown_port_log++;
        }
        break;
    }
    }

    return r;
}

static void chihiro_mbcom_process(void);

static void chihiro_lpc_io_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);
    s->last_lpc_activity_ms = TS_MS;
    s->host_seen = true;

    if (chihiro_game_running) {
        static int game_lpc_write_log = 0;
        if (lpc_log_verbose && game_lpc_write_log < 300) {
            game_lpc_write_log++;
            fprintf(stderr, "[%07lld] GAME LPC WRITE port=0x%04X val=0x%08X\n",
                   TS_MS, (int)(0x4000 + addr), (unsigned)val);
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
            {
                static int addr_log = 0;
                if (chihiro_game_running) {
                    static int gaddr_log = 0;
                    if (lpc_log_verbose && gaddr_log < 500) { gaddr_log++;
                        fprintf(stderr, "[%07lld] GAME SADDR ADDR: bb_reg_addr=0x%08X\n",
                                TS_MS, (unsigned)val); }
                } else if (lpc_log_verbose && addr_log < 200) {
                    fprintf(stderr, "[%07lld] SADDR ADDR: bb_reg_addr=0x%08X\n",
                            TS_MS, (unsigned)val);
                    addr_log++;
                }
            }
            break;
        case 0xA0000040: /* DMA status/enable */
            s->bb_reg_status = (uint32_t)val;
            if (val & 0x80000000) {
                s->bb_dma_active = true;
                s->bb_dma_count = 0;
                static int burst_on_log = 0;
                if (lpc_log_verbose && burst_on_log < 10) {
                    fprintf(stderr, "[%07lld] SADDR BURST ON: target=0x%08X\n",
                            TS_MS, s->bb_reg_addr);
                    burst_on_log++;
                }
            } else {
                if (s->bb_dma_active) {
                    static int burst_off_log = 0;
                    if (lpc_log_verbose && burst_off_log < 10) {
                        fprintf(stderr, "[%07lld] SADDR BURST OFF: wrote %u dwords\n",
                                TS_MS, s->bb_dma_count);
                        burst_off_log++;
                    }
                }
                s->bb_dma_active = false;
            }
            break;
        case 0xA0000000: /* Data write to address in bb_reg_addr */
            if (s->bb_dma_active) {
                uint32_t wa = s->bb_reg_addr;
                {
                    static int burst_data_log = 0;
                    if (lpc_log_verbose && burst_data_log < 40) {
                        fprintf(stderr, "[%07lld] SADDR BURST DATA: [0x%08X] <- 0x%08X (dma#%u)\n",
                                TS_MS, wa, (unsigned)val, s->bb_dma_count);
                        burst_data_log++;
                    }
                }
                if (wa >= 0x84000020 && wa <= 0x8400003C) {
                    uint32_t idx = (wa - 0x84000020) / 4;
                    if (idx < 8) {
                        s->dimm_cmd[idx] = (uint32_t)val;
                        /* Data stored — processing deferred to EXEC trigger
                         * at 0x84000040 which the game writes after the burst. */
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
            } else {
                /* Register-mode: single-word write to target address */
                uint32_t wa = s->bb_reg_addr;
                static int regmode_log_count = 0;
                if (lpc_log_verbose && regmode_log_count < 200) {
                    fprintf(stderr, "[%07lld] SADDR REG-WRITE: target=0x%08X val=0x%08X\n",
                            TS_MS, wa, (unsigned)val);
                    regmode_log_count++;
                }
                if (wa >= 0x84000020 && wa <= 0x8400003C) {
                    uint32_t idx = (wa - 0x84000020) / 4;
                    if (idx < 8) {
                        s->dimm_cmd[idx] = (uint32_t)val;
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
                } else if (wa == 0x80000140) {
                    s->asic_cpu_ctrl = (uint32_t)val;
                } else if (wa == 0xA0001E60) {
                    if (lpc_log_verbose) fprintf(stderr, "[%07lld] SADDR WRITE 0xA0001E60 <- 0x%08X (fw state)\n",
                            TS_MS, (unsigned)val);
                } else if (wa == 0x84000040) {
                    /* ASIC execute trigger */
                    if (lpc_log_verbose) fprintf(stderr, "[%07lld] SADDR REG-MODE: 0x84000040 <- 0x%08X\n",
                            TS_MS, (unsigned)val);
                    if (val & 1) {
                        static int exec_dump = 0;
                        if (lpc_log_verbose && exec_dump < 30) {
                            fprintf(stderr, "[%07lld] EXEC dimm_cmd: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                                    TS_MS,
                                    s->dimm_cmd[0], s->dimm_cmd[1], s->dimm_cmd[2], s->dimm_cmd[3],
                                    s->dimm_cmd[4], s->dimm_cmd[5], s->dimm_cmd[6], s->dimm_cmd[7]);
                            exec_dump++;
                        }
                        chihiro_dimm_process_cmd(s);
                        {
                            static int game_exec_log = 0;
                            if (lpc_log_verbose && chihiro_game_running && game_exec_log < 50) {
                                fprintf(stderr, "[%07lld] GAME-EXEC #%d: cmd=%08X → resp: %08X %08X %08X %08X\n",
                                        TS_MS, game_exec_log,
                                        s->dimm_cmd[0], s->dimm_resp[0], s->dimm_resp[1],
                                        s->dimm_resp[2], s->dimm_resp[3]);
                                game_exec_log++;
                            }
                        }

                        /* V850 unsolicited 0x0002/0x0003 are RESET REQUESTS
                         * (acLibUpdateMedia → XLaunchNewImageA). Do NOT send. */

                        if (chihiro_game_running && s->dimm_resp_timer) {
                            timer_mod(s->dimm_resp_timer,
                                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2);
                        } else {
                            s->mbcom_e0_status |= 0x01;
                            qemu_irq_lower(s->irq10);
                            qemu_irq_raise(s->irq10);
                        }
                    }
                }
            }
            break;
        }
        return;
    case 0x04: /* Port 0x4004: set register address */
        s->lpc_reg_addr = (uint32_t)val;
        return;
    case 0x08: /* Port 0x4008: reset cycle */
        return;
    case 0x26:  /* Port 0x4026: scratch register */
        s->lpc_scratch_4026 = (uint16_t)val;
        break;
    case SEGA_IRQ10_ACK: { /* 0xE0 — ack: clear specific bits */
        uint8_t old_e0 = s->mbcom_e0_status;
        s->mbcom_e0_status &= ~(uint8_t)val;
        if (s->mbcom_e0_status == 0)
            qemu_irq_lower(s->irq10);
        static int e0w_log = 0;
        if (lpc_log_verbose && e0w_log < 100) { e0w_log++;
            fprintf(stderr, "[%07lld] E0 WRITE val=0x%02X e0=0x%02X→0x%02X%s\n",
                    TS_MS, (unsigned)(uint8_t)val, old_e0, s->mbcom_e0_status,
                    s->mbcom_e0_status == 0 ? " irq10↓" : ""); }
        break;
    }
    case 0xE2:            /* 0x40E2 — IRQ10 deassert only, do NOT clear data ready */
        qemu_irq_lower(s->irq10);
        break;
    case 0xE1: {          /* 0x40E1 — mbcom trigger / IRQ10 deassert
                           * Protocol (from vsg.xbe FUN_0014c120 + DPC at 0x14c0e0):
                           *   DPC writes E1=0xF (ARM)
                           *   Worker checks scratch bit 8: set=busy, clear=ready
                           */
        /*
                           *   Worker writes cmd to DMA (FC801), then E1=0xF, E1=0
                           *   E1=0 = "process the DMA command" (not scratch!)
                           * scratch 0x0102 = status register (bit8=busy), NOT a command */
        static int e1_log = 0;
        if (val != 0) {
            if (!chihiro_game_running &&
                (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0)) {
                chihiro_mbcom_process();
                s->mbcom_e0_status |= 0x01;
                s->dimm_cmd_count++;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
            } else {
                /* ARM for E1=0 trigger (game mode or SEGABOOT ack) */
                chihiro_e1_armed = true;
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0x%X ARM scratch=0x%04X dma=%02X%02X\n",
                            TS_MS, (unsigned)val, s->lpc_scratch_4026,
                            chihiro_mbcom_command[0], chihiro_mbcom_command[1]); }
            }
        } else {
            if (chihiro_game_running && chihiro_board_type3 && chihiro_e1_armed) {
                /* ')' mode (Type-3 game): E1=0 is the game worker's
                 * confirmation that it processed the ARM cycle. Clear E0
                 * so the worker can proceed to read SADDR. */
                static bool t3_worker_logged;
                if (!t3_worker_logged) {
                    t3_worker_logged = true;
                    if (lpc_log_verbose) fprintf(stderr, "[%07lld] T3 WORKER ALIVE\n", TS_MS);
                }
                s->mbcom_e0_status &= ~0x01;
                qemu_irq_lower(s->irq10);

                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 T3_ACK e0=0x%02X irq10↓\n",
                            TS_MS, s->mbcom_e0_status); }
            } else if (chihiro_e1_armed && s->mbcom_resp_ready) {
                /* RESP_DELIVER ('!' mode / SEGABOOT) */
                s->mbcom_resp_ready = false;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 RESP_DELIVER scratch=0x%04X resp: %02X%02X %02X%02X\n",
                            TS_MS, s->lpc_scratch_4026,
                            chihiro_mbcom_response[0], chihiro_mbcom_response[1],
                            chihiro_mbcom_response[2], chihiro_mbcom_response[3]); }
            } else if (chihiro_e1_armed && (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0)) {
                /* DMA_PROCESS ('!' mode / SEGABOOT) */
                const uint8_t *w = chihiro_mbcom_command;
                uint16_t seq = w[0] | (w[1] << 8);
                uint16_t cmd = w[2] | (w[3] << 8);
                s->dimm_cmd[0] = seq | ((uint32_t)cmd << 16);
                memset(&s->dimm_cmd[1], 0, 7 * sizeof(uint32_t));
                chihiro_dimm_process_cmd(s);
                chihiro_mbcom_process();
                memset(chihiro_mbcom_command, 0, 32);
                s->mbcom_e0_status |= 0x01;
                s->mbcom_resp_ready = true;
                s->lpc_scratch_4026 &= ~0x0100;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 DMA_PROCESS scratch=0x%04X resp: %02X%02X %02X%02X %02X%02X%02X%02X\n",
                            TS_MS, s->lpc_scratch_4026,
                            chihiro_mbcom_response[0], chihiro_mbcom_response[1],
                            chihiro_mbcom_response[2], chihiro_mbcom_response[3],
                            chihiro_mbcom_response[4], chihiro_mbcom_response[5],
                            chihiro_mbcom_response[6], chihiro_mbcom_response[7]); }
            } else {
                s->lpc_scratch_4026 &= ~0x0100;
                qemu_irq_lower(s->irq10);
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 LOWER armed=%d scratch=0x%04X\n",
                            TS_MS, chihiro_e1_armed, s->lpc_scratch_4026); }
            }
            chihiro_e1_armed = false;
        }
        break;
    }
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

/* mbcom state — arrays declared before chihiro_lpc_io_write for forward use */
static bool chihiro_mbcom_enabled = false;

/* Flash ROM (SEGABOOT) loaded from file — serves mbrom0/mbrom1 reads.
 * On real hardware: fpr-23887_29lv160te.ic4 (2MB flash on MediaBoard).
 * Contains the SEGABOOT XBE that boots before the game.
 * Also contains MBDT header at 0xFFE00 with media board serial. */

uint8_t *chihiro_ic10_data = NULL;
uint32_t chihiro_ic10_size = 0;
uint8_t *chihiro_ic11_data = NULL;
uint32_t chihiro_ic11_size = 0;
uint8_t *chihiro_pc20_data = NULL;
uint32_t chihiro_pc20_size = 0;

bool chihiro_flash_rom_loaded(void)
{
    return chihiro_flash_rom != NULL;
}

int chihiro_detected_game_profile(void)
{
    if (!chihiro_game_filename[0]) return -1;

    if (strcasecmp(chihiro_game_filename, "hod3xb.xbe") == 0)   return 0;
    if (strcasecmp(chihiro_game_filename, "vc3.xbe") == 0)       return 1;
    if (strcasecmp(chihiro_game_filename, "vsg.xbe") == 0)       return 2;
    if (strcasecmp(chihiro_game_filename, "ctx_ac[r].xbe") == 0) return 3;
    if (strcasecmp(chihiro_game_filename, "outrun2.xbe") == 0)   return 4;
    if (strcasecmp(chihiro_game_filename, "OllieKing.xbe") == 0) return 5;

    return -1;
}

static bool chihiro_read_flash_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz <= 4 * 1024 * 1024) {
        chihiro_flash_rom = (uint8_t *)g_malloc0(sz);
        if (fread(chihiro_flash_rom, 1, sz, f) == (size_t)sz) {
            chihiro_flash_rom_size = sz;
        } else {
            g_free(chihiro_flash_rom);
            chihiro_flash_rom = NULL;
        }
    }
    fclose(f);
    return chihiro_flash_rom != NULL;
}

/* Explicit path from Settings > Chihiro > Files wins; otherwise look for the
 * known file names next to the Xbox BIOS. */
void chihiro_load_flash_rom(const char *bios_path)
{
    if (chihiro_flash_rom) return; /* already loaded */

    const char *configured = g_config.chihiro.roms.mediaboard_path;
    if (configured && configured[0]) {
        if (chihiro_read_flash_rom(configured)) return;
        fprintf(stderr, "Chihiro: cannot read media board flash '%s'\n",
                configured);
    }

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
        "fpr21042_m29w160et.bin",
        "fpr-23887_29lv160te.ic4",
        "fpr-23887.bin",
        NULL
    };

    for (int i = 0; flash_names[i]; i++) {
        char path[2048];
        snprintf(path, sizeof(path), "%s%s", dir, flash_names[i]);
        if (chihiro_read_flash_rom(path)) return;
        snprintf(path, sizeof(path), "%s../%s", dir, flash_names[i]);
        if (chihiro_read_flash_rom(path)) return;
    }
}

static uint8_t *load_eeprom_file(const char *dir, const char *name,
                                 uint32_t expected_size, uint32_t *out_size)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (expected_size && (uint32_t)sz != expected_size)) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)g_malloc0(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        g_free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (uint32_t)sz;
    return buf;
}

/* Explicit paths from Settings > Chihiro > Files win; otherwise look for the
 * known file names next to the Xbox BIOS. */
static uint8_t *load_eeprom_configured(const char *configured, const char *dir,
                                       const char *name, uint32_t expected_size,
                                       uint32_t *out_size)
{
    if (configured && configured[0]) {
        uint8_t *buf = load_eeprom_file("", configured, expected_size, out_size);
        if (buf) return buf;
        fprintf(stderr, "Chihiro: cannot read EEPROM '%s' (expected %u bytes)\n",
                configured, expected_size);
    }
    return load_eeprom_file(dir, name, expected_size, out_size);
}

void chihiro_load_eeproms(const char *bios_path)
{
    if (chihiro_ic10_data) return;

    char dir[1024] = {0};
    const char *last_sep = strrchr(bios_path, '/');
    if (!last_sep) last_sep = strrchr(bios_path, '\\');
    if (last_sep) {
        int dir_len = last_sep - bios_path + 1;
        if (dir_len < (int)sizeof(dir))
            memcpy(dir, bios_path, dir_len);
    }

    chihiro_ic10_data = load_eeprom_configured(g_config.chihiro.roms.ic10_path,
                                               dir, "ic10_g24lc64.bin",
                                               8192, &chihiro_ic10_size);
    chihiro_ic11_data = load_eeprom_configured(g_config.chihiro.roms.ic11_path,
                                               dir, "ic11_24lc024.bin",
                                               128, &chihiro_ic11_size);
    chihiro_pc20_data = load_eeprom_configured(g_config.chihiro.roms.pc20_path,
                                               dir, "pc20_g24lc64.bin",
                                               8192, &chihiro_pc20_size);

    printf("Chihiro: EEPROMs from disk: ic10=%s (%uB), ic11=%s (%uB), pc20=%s (%uB)\n",
           chihiro_ic10_data ? "OK" : "MISSING", chihiro_ic10_size,
           chihiro_ic11_data ? "OK" : "MISSING", chihiro_ic11_size,
           chihiro_pc20_data ? "OK" : "MISSING", chihiro_pc20_size);
}

static void chihiro_dimm_resp_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    s->mbcom_e0_status |= 0x01; /* bit 0 only: ')' mode → status=2 */
    qemu_irq_lower(s->irq10);
    qemu_irq_raise(s->irq10);
}

static void chihiro_irq10_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    /* IRQ10 for SEGABOOT baseboard communication. The board only signals a
     * host that is already talking to it — the first LPC access is that
     * proof, and it is a bus event rather than a peek into guest memory. */
    if (s->host_seen && !chihiro_game_running) {
        qemu_irq_raise(s->irq10);
    }

    /* Game-mode mbcom bootstrap.
     *
     * Type-1 ('!' mode): Pre-load DIMM_SIZE (0x8001) and fire IRQ10
     * immediately. Worker uses IDE DMA polling, no handshake needed.
     *
     * Type-3 (')' mode): Start the V850 unsolicited handshake sequence.
     * Real firmware sends cmd 0x0002 (DMA state 4) then cmd 0x0003 (DMA
     * state 5). Each is a handshake: V850 sends unsolicited → game responds
     * 0x0001 → V850 ACKs with 0x8001+DIMM_SIZE. The heartbeat timer drives
     * this state machine. After both handshakes, switch to reactive mode
     * where each EXEC gets an immediate response+E0+IRQ10. */
    if (chihiro_game_running && chihiro_mbcom_enabled) {
        if (!chihiro_mbcom_bootstrap_done) {
            memset(chihiro_mbcom_command, 0, 32);

            if (chihiro_board_type3) {
                /* ')' mode: purely reactive. Game state machine calls
                 * FUN_0014c580 to set up connection, then sends cmd 0x0001
                 * via EXEC. Our response (0x8001+DIMM_SIZE) serves as the
                 * handshake message. No proactive heartbeat needed. */
                fprintf(stderr, "[%07lld] *** GAME MBCOM BOOTSTRAP T3: reactive mode (no heartbeat) ***\n", TS_MS);
            } else {
                /* '!' mode: fire DIMM_SIZE + IRQ10 immediately */
                s->dimm_cmd[0] = 1 | (0x0001 << 16);
                chihiro_dimm_process_cmd(s);
                s->dimm_resp_ready = true;
                s->mbcom_e0_status |= 0x01;
                s->mbcom_resp_ready = true;
                s->lpc_scratch_4026 &= ~0x0100;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
                fprintf(stderr, "[%07lld] *** GAME MBCOM BOOTSTRAP T1: 0x8001 loaded, IRQ10 edge fired ***\n", TS_MS);
            }
            chihiro_mbcom_bootstrap_done = true;
        }

        /* Type-3 unsolicited 0x0002 is scheduled by the EXEC handler
         * after the game's cmd 0x0100 status query. The heartbeat timer
         * delivers it 200ms later, after the status response is consumed. */
    }

    /* DMA META scan: provide mbcom slot responses to SEGABOOT */
    if (chihiro_mbcom_enabled && !chihiro_game_running
        && chihiro_lpc_global && chihiro_lpc_global->diag_armed) {
        static const struct { uint32_t slot_va; uint32_t meta_va; uint32_t stride; }
            slot_layouts[] = {
                { 0xAA7B0, 0xAA790, 0x60 },
                { 0x89760, 0x89740, 0x40 },
            };
        for (int layout = 0; layout < 2; layout++) {
            uint32_t slot_base_pa = chihiro_va_to_pa(slot_layouts[layout].slot_va);
            if (slot_base_pa == 0xFFFFFFFF) continue;
            uint32_t meta_base_pa = chihiro_va_to_pa(slot_layouts[layout].meta_va);
            if (meta_base_pa == 0xFFFFFFFF) continue;
            if (slot_base_pa >= 0x800000 || meta_base_pa >= 0x800000) continue;
            uint32_t stride = slot_layouts[layout].stride;
            for (int sl = 0; sl < 16; sl++) {
                uint32_t data_pa = slot_base_pa + sl * stride;
                uint32_t meta_pa = meta_base_pa + sl * stride;
                if (data_pa + 4 >= 0x800000 || meta_pa + 12 >= 0x800000) continue;
                uint8_t data_byte0;
                uint16_t meta_marker;
                cpu_physical_memory_read(data_pa, &data_byte0, 1);
                cpu_physical_memory_read(meta_pa + 2, &meta_marker, 2);
                if (data_byte0 == 0 || meta_marker != 0) continue;
                uint16_t cmd_opcode = 0;
                cpu_physical_memory_read(data_pa + 2, &cmd_opcode, 2);
                uint32_t resp_data = 0, resp_data2 = 0;
                switch (cmd_opcode) {
                case MB_CMD_INIT: resp_data = mediaboard.dimm_size; break;
                case MB_CMD_STATUS: resp_data = mediaboard.status; resp_data2 = mediaboard.progress; break;
                case MB_CMD_GET_VERSION: resp_data = mediaboard.fw_version; break;
                case MB_CMD_SYSTEM_TYPE: resp_data = 0x8002; break;
                case MB_CMD_GET_SERIAL: memcpy(&resp_data, mediaboard.serial, 4); break;
                default: resp_data = 0; break;
                }
                meta_marker = 0x0001;
                cpu_physical_memory_write(meta_pa + 2, &meta_marker, 2);
                cpu_physical_memory_write(meta_pa + 4, &resp_data, 4);
                if (cmd_opcode == 0x0100)
                    cpu_physical_memory_write(meta_pa + 8, &resp_data2, 4);
                s->mbcom_e0_status |= 0x05;
                s->lpc_scratch_4026 &= ~0x0100;
                qemu_irq_raise(s->irq10);
            }
            break;
        }
    }

    /* Re-arm every 16ms (~60Hz) — keep running even after game detection */
    timer_mod(s->irq10_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
}


static void chihiro_kbd_event(DeviceState *dev, QemuConsole *src,
                              InputEvent *evt)
{
    /* JVS input handled by xemu_input_update_jvs() in xemu-input.c */
    (void)dev; (void)src; (void)evt;
}

static const QemuInputHandler chihiro_kbd_handler = {
    .name  = "Chihiro JVS Keyboard",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = chihiro_kbd_event,
};

static void chihiro_lpc_realize(DeviceState *dev, Error **errp)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    chihiro_log_init();
    lpc_log_verbose = !!(chihiro_log_mask & CHIHIRO_LOG_VERBOSE);

    chihiro_active = true;
    chihiro_lpc_global = s;
    memory_region_init_io(&s->ioport, OBJECT(dev), &chihiro_lpc_io_ops, s,
                          "chihiro-lpc-io", 0x100);
    isa_register_ioport(isa, &s->ioport, 0x4000);

    setvbuf(stderr, NULL, _IONBF, 0);

    /* Initialize mbcom buffers to zero */
    memset(s->mbcom_read_buffer, 0, sizeof(s->mbcom_read_buffer));
    memset(s->mbcom_write_buffer, 0, sizeof(s->mbcom_write_buffer));

    /* ASIC CPU control: bit 0 = FPGA initialized.
     * Game's FUN_0014c240 reads 0x80000140 bit 0 — if clear,
     * FUN_0014c580 skips ALL DMA buffer setup (return 5) and
     * the game can never send/receive mbcom via SADDR. */
    s->asic_cpu_ctrl = 1;

    /* Initialize IRQ10 for baseboard communication */
    s->irq10 = isa_get_irq(isa, 10);
    chihiro_irq10_global = s->irq10;
    s->irq10_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                   chihiro_irq10_timer_cb, s);
    timer_mod(s->irq10_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);

    /* Initialize media board state (single source for all mbcom responses) */
    mediaboard_init();

    /* Initialize mbcom protocol handler */
    chihiro_mbcom_init();

    s->dimm_resp_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       chihiro_dimm_resp_timer_cb, s);
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

    /* Arm the diagnostic timer once SEGABOOT is loaded — retry every 1ms */
    s->diag_armed = false;
    s->diag_arm_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                            chihiro_arm_diag_cb, s);
    timer_mod(s->diag_arm_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);

    qemu_input_handler_register(dev, &chihiro_kbd_handler);

}

/* The whole mediaboard protocol state: without it a snapshot restored
 * into a fresh boot leaves the LPC at reset under a guest mid-protocol,
 * and the game kernel panics within a second. The globals ride along in
 * the device state (mig_* fields) so they are actually serialized. */

static int chihiro_lpc_pre_save(void *opaque)
{
    ChihiroLPCState *s = opaque;
    s->mig_game_running = chihiro_game_running;
    s->mig_quickreboot_pending = chihiro_quickreboot_pending;
    s->mig_active = chihiro_active;
    return 0;
}

static int chihiro_lpc_post_load(void *opaque, int version_id)
{
    ChihiroLPCState *s = opaque;
    if (version_id >= 3) {
        chihiro_game_running = s->mig_game_running;
        chihiro_quickreboot_pending = s->mig_quickreboot_pending;
        chihiro_active = s->mig_active;
    }
    return 0;
}

static const VMStateDescription vmstate_chihiro_lpc = {
    .name = "chihiro-lpc",
    .version_id = 3,
    .minimum_version_id = 1,
    .pre_save = chihiro_lpc_pre_save,
    .post_load = chihiro_lpc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(mbcom_read_buffer, ChihiroLPCState, 32),
        VMSTATE_UINT8_ARRAY(mbcom_write_buffer, ChihiroLPCState, 32),
        VMSTATE_BOOL(host_seen, ChihiroLPCState),
        VMSTATE_UINT32(lpc_reg_addr, ChihiroLPCState),
        VMSTATE_UINT32(lpc_reg_data, ChihiroLPCState),
        VMSTATE_BOOL(diag_armed, ChihiroLPCState),
        VMSTATE_UINT32(lpc_401e_reads, ChihiroLPCState),
        VMSTATE_UINT32(last_bootstate, ChihiroLPCState),
        VMSTATE_UINT16(lpc_scratch_4026, ChihiroLPCState),
        VMSTATE_UINT8(mbcom_e0_status, ChihiroLPCState),
        VMSTATE_BOOL(mbcom_resp_ready, ChihiroLPCState),
        VMSTATE_UINT32(bb_reg_addr, ChihiroLPCState),
        VMSTATE_UINT32(bb_reg_status, ChihiroLPCState),
        VMSTATE_BOOL(bb_dma_active, ChihiroLPCState),
        VMSTATE_UINT32(bb_dma_count, ChihiroLPCState),
        VMSTATE_BOOL(bb_event_pending, ChihiroLPCState),
        VMSTATE_UINT32(asic_cpu_ctrl, ChihiroLPCState),
        VMSTATE_UINT32_ARRAY(dimm_cmd, ChihiroLPCState, 8),
        VMSTATE_UINT32_ARRAY(dimm_resp, ChihiroLPCState, 8),
        VMSTATE_UINT32(dimm_cmd_idx, ChihiroLPCState),
        VMSTATE_BOOL(dimm_resp_ready, ChihiroLPCState),
        VMSTATE_UINT32(dimm_cmd_count, ChihiroLPCState),
        VMSTATE_UINT16(dimm_next_seq, ChihiroLPCState),
        /* Protocol one-shot timers: a snapshot taken with a mediaboard
         * transaction in flight owes the guest a response — without the
         * timer the reply never comes and the game hangs on its next
         * mediaboard poll. */
        VMSTATE_TIMER_PTR_V(irq10_timer, ChihiroLPCState, 2),
        VMSTATE_TIMER_PTR_V(dimm_resp_timer, ChihiroLPCState, 2),
        VMSTATE_BOOL_V(mig_game_running, ChihiroLPCState, 3),
        VMSTATE_BOOL_V(mig_quickreboot_pending, ChihiroLPCState, 3),
        VMSTATE_BOOL_V(mig_active, ChihiroLPCState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void chihiro_lpc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = chihiro_lpc_realize;
    dc->vmsd = &vmstate_chihiro_lpc;
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

/* ═══════════════════════════════════════════════════════════════════════
 * Save file persistence (Phase 2)
 *
 * Persists QC ic11 (512B) + extmem backup region (32KB) per game.
 * File: ~/.local/share/xemu/xemu/saves/<game>.sav
 * ═══════════════════════════════════════════════════════════════════════ */

/* Per-game data goes where the rest of xemu keeps its state (see the shader
 * cache): xemu_settings_get_base_path() honours portable mode and is correct
 * on Windows and macOS, unlike a hand-built $HOME path. */
static bool chihiro_data_dir(const char *name, char *out, size_t out_len)
{
    const char *base = xemu_settings_get_base_path();
    if (!base || !base[0]) return false;
    snprintf(out, out_len, "%s%s", base, name);
    g_mkdir_with_parents(out, 0755);
    return true;
}

/* Game name without its .xbe extension, e.g. "vsg" — the stem shared by the
 * save file and the memory cards. */
static bool chihiro_game_base_name(char *base, size_t base_len)
{
    if (!chihiro_game_dir[0]) return false;

    chihiro_capture_game_filename_from_dir();
    if (!chihiro_game_filename[0]) return false;

    strncpy(base, chihiro_game_filename, base_len - 1);
    base[base_len - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    return base[0] != 0;
}

static bool chihiro_resolve_save_path(void)
{
    char base[64];
    char saves_dir[1024];
    if (!chihiro_game_base_name(base, sizeof(base)) ||
        !chihiro_data_dir("saves", saves_dir, sizeof(saves_dir)))
        return false;

    snprintf(chihiro_save_path, sizeof(chihiro_save_path),
             "%s/%s.sav", saves_dir, base);
    return true;
}

/* ONLY an explicit per-player card file chosen in Settings > Chihiro is
 * read. With no card assigned the reader is EMPTY: the game offers to play
 * without a card, and no synthetic or auto-resolved card is fabricated. */
static void chihiro_resolve_card_path(int player, char *out, size_t out_len)
{
    const char *cfg = (player == 0) ? g_config.chihiro.card_reader.card1_path
                                    : g_config.chihiro.card_reader.card2_path;
    if (cfg && cfg[0]) {
        snprintf(out, out_len, "%s", cfg);
        return;
    }
    out[0] = '\0'; /* unassigned: no card in this reader */
}

static void chihiro_exit_notify(Notifier *notifier, void *data)
{
    (void)notifier;
    (void)data;
    if (!chihiro_active) return;
    if (!chihiro_save_path[0] && !chihiro_resolve_save_path()) return;
    chihiro_usb_save_flush(chihiro_save_path);
}

static Notifier chihiro_exit_notifier = { .notify = chihiro_exit_notify };
/* Flush the arcade backup (game save) immediately. The UI quit path skips
 * the doomed driver atexit chain with _exit(), which also skips the exit
 * notifier below — it must flush explicitly before leaving. */
void chihiro_flush_save_now(void)
{
    chihiro_exit_notify(NULL, NULL);
}

static bool chihiro_exit_notifier_registered = false;

void chihiro_save_init(void)
{
    if (chihiro_exit_notifier_registered) return;
    qemu_add_exit_notifier(&chihiro_exit_notifier);
    chihiro_exit_notifier_registered = true;

    if (chihiro_resolve_save_path()) {
        chihiro_usb_save_load(chihiro_save_path);
    }
}

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

/* MemoryRegion-backed IDE interface.
 * Only FATX goes through the block device. Flash ROM and mbcom are
 * served via synchronous hooks in core.c (chihiro_rom_io/chihiro_mbcom_io). */
#define CHIHIRO_FS_SIZE         ((uint64_t)CHIHIRO_MBCOM_BASE * 512)  /* 496 MB */
#define CHIHIRO_ROM_SIZE        (2 * 1024 * 1024)  /* 2MB */

static MemoryRegion chihiro_interface_container;
static MemoryRegion chihiro_interface_fs;
static AddressSpace chihiro_interface_as;
static bool chihiro_interface_ready = false;

/* DIMM migration: instead of ~540 MB of raw RAM, store the netboot image
 * identity (size + CRC32) plus the pages that differ from it; load
 * re-reads the image and applies the delta. */

#define DIMM_MIG_MAGIC   0x4344494d /* CDIM */
#define DIMM_MIG_VERSION 1
#define DIMM_PAGE_SIZE   4096

typedef struct ChihiroDimmMig {
    uint32_t blob_size;
    uint8_t *blob;
} ChihiroDimmMig;

static ChihiroDimmMig chihiro_dimm_mig;

static uint8_t *chihiro_dimm_read_image(uint64_t buf_size,
                                        uint64_t *file_size, uint32_t *crc)
{
    const char *path = g_config.sys.files.dvd_path;
    FILE *f = (path && path[0]) ? fopen(path, "rb") : NULL;
    if (!f) {
        return NULL;
    }
    uint8_t *buf = g_malloc0(buf_size);
    size_t n = fread(buf, 1, buf_size, f);
    fclose(f);
    *file_size = n;
    *crc = crc32(0, buf, n);
    return buf;
}

static int chihiro_dimm_pre_save(void *opaque)
{
    ChihiroDimmMig *m = opaque;
    uint32_t fs_size;
    uint8_t *fs = chihiro_fatx_get_buffer(&fs_size);
    uint64_t file_size;
    uint32_t crc;
    uint8_t *ref = fs ? chihiro_dimm_read_image(fs_size, &file_size, &crc)
                      : NULL;
    if (!ref) {
        error_report("chihiro: cannot read the game image to delta against");
        return -EINVAL;
    }

    uint32_t hdr[7] = { DIMM_MIG_MAGIC, DIMM_MIG_VERSION,
                        (uint32_t)file_size, (uint32_t)(file_size >> 32),
                        crc, DIMM_PAGE_SIZE, 0 /* npages */ };
    GByteArray *blob = g_byte_array_new();
    g_byte_array_append(blob, (uint8_t *)hdr, sizeof(hdr));

    uint32_t npages = 0;
    for (uint32_t pg = 0; pg < fs_size / DIMM_PAGE_SIZE; pg++) {
        const uint8_t *cur = fs + (size_t)pg * DIMM_PAGE_SIZE;
        if (!memcmp(cur, ref + (size_t)pg * DIMM_PAGE_SIZE, DIMM_PAGE_SIZE)) {
            continue;
        }
        g_byte_array_append(blob, (uint8_t *)&pg, 4);
        g_byte_array_append(blob, cur, DIMM_PAGE_SIZE);
        npages++;
    }
    memcpy(blob->data + 24, &npages, 4);
    g_free(ref);

    m->blob_size = blob->len;
    g_free(m->blob);
    m->blob = g_byte_array_free(blob, FALSE);

    printf("Chihiro DIMM migration: %u changed pages, %u byte blob\n",
           npages, m->blob_size);
    return 0;
}

static int chihiro_dimm_post_save(void *opaque)
{
    ChihiroDimmMig *m = opaque;
    g_free(m->blob);
    m->blob = NULL;
    m->blob_size = 0;
    return 0;
}

static int chihiro_dimm_post_load(void *opaque, int version_id)
{
    ChihiroDimmMig *m = opaque;
    int ret = -EINVAL;
    uint32_t fs_size;
    uint8_t *fs = chihiro_fatx_get_buffer(&fs_size);
    const uint8_t *p = m->blob;
    uint32_t hdr[7];
    uint64_t cur_size;
    uint32_t cur_crc;
    uint8_t *ref = NULL;

    if (!fs || !m->blob || m->blob_size < sizeof(hdr)) {
        goto out;
    }
    memcpy(hdr, p, sizeof(hdr));
    p += sizeof(hdr);
    if (hdr[0] != DIMM_MIG_MAGIC || hdr[1] != DIMM_MIG_VERSION ||
        hdr[5] != DIMM_PAGE_SIZE) {
        goto out;
    }

    ref = chihiro_dimm_read_image(fs_size, &cur_size, &cur_crc);
    if (!ref || cur_size != (hdr[2] | (uint64_t)hdr[3] << 32) ||
        cur_crc != hdr[4]) {
        error_report("chihiro: mounted game image does not match this "
                     "snapshot");
        goto out;
    }
    memcpy(fs, ref, fs_size);

    for (uint32_t i = 0; i < hdr[6]; i++) {
        uint32_t pg;
        if ((uint64_t)(p - m->blob) + 4 + DIMM_PAGE_SIZE > m->blob_size) {
            goto out;
        }
        memcpy(&pg, p, 4);
        p += 4;
        if ((uint64_t)pg * DIMM_PAGE_SIZE + DIMM_PAGE_SIZE > fs_size) {
            goto out;
        }
        memcpy(fs + (size_t)pg * DIMM_PAGE_SIZE, p, DIMM_PAGE_SIZE);
        p += DIMM_PAGE_SIZE;
    }
    ret = 0;

out:
    g_free(ref);
    g_free(m->blob);
    m->blob = NULL;
    m->blob_size = 0;
    return ret;
}

static const VMStateDescription vmstate_chihiro_dimm = {
    .name = "chihiro-dimm",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = chihiro_dimm_pre_save,
    .post_save = chihiro_dimm_post_save,
    .post_load = chihiro_dimm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(blob_size, ChihiroDimmMig),
        VMSTATE_VBUFFER_ALLOC_UINT32(blob, ChihiroDimmMig, 1, NULL,
                                     blob_size),
        VMSTATE_END_OF_LIST()
    }
};

void chihiro_ide_interface_init(void)
{
    printf("Chihiro: IDE interface init START (fs=%llu)\n",
           (unsigned long long)CHIHIRO_FS_SIZE);
    fflush(stdout);

    memory_region_init(&chihiro_interface_container, NULL,
                       "chihiro.interface", CHIHIRO_FS_SIZE);

    /* Serialized by chihiro-dimm as a delta, not by the RAM stream. */
    memory_region_init_ram_nomigrate(&chihiro_interface_fs, NULL,
                                     "chihiro.interface.filesystem",
                                     CHIHIRO_FS_SIZE, &error_fatal);
    vmstate_register(NULL, 0, &vmstate_chihiro_dimm, &chihiro_dimm_mig);

    memory_region_add_subregion(&chihiro_interface_container,
                                0, &chihiro_interface_fs);

    address_space_init(&chihiro_interface_as, &chihiro_interface_container,
                       "chihiro.interface");

    printf("Chihiro: IDE interface — MemoryRegions + AddressSpace OK\n");
    fflush(stdout);

    BlockDriverState *bs = bdrv_new();
    bdrv_memory_open(bs, &chihiro_interface_as, CHIHIRO_FS_SIZE);
    bdrv_set_monitor_owned(bs);
    printf("Chihiro: IDE interface — BDS created\n");
    fflush(stdout);

    BlockBackend *blk = blk_new(qemu_get_aio_context(),
                                BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                                BLK_PERM_ALL);
    blk_insert_bs(blk, bs, &error_fatal);
    monitor_add_blk(blk, "chihiro-interface", &error_fatal);
    printf("Chihiro: IDE interface — BlockBackend created\n");
    fflush(stdout);

    DriveInfo *dinfo = g_malloc0(sizeof(*dinfo));
    dinfo->type = IF_IDE;
    dinfo->bus = 0;
    dinfo->unit = 1;
    dinfo->media_cd = false;
    blk_set_legacy_dinfo(blk, dinfo);

    chihiro_interface_ready = true;
    printf("Chihiro: IDE interface initialized (%u MB fs, ROM via hook)\n",
           (uint32_t)(CHIHIRO_FS_SIZE / (1024 * 1024)));
    fflush(stdout);
}

static void chihiro_segaboot_identify(void)
{
    if (chihiro_flash_rom_size < 0x200000)
        return;

    const uint8_t *half = chihiro_flash_rom + 0x100000;
    const char *tag = "SegaBoot Ver.";
    const uint8_t *p = memmem(half, 0x100000, tag, strlen(tag));
    if (!p) {
        printf("Chihiro: no SEGABOOT in this flash dump\n");
        return;
    }

    const char *version = (const char *)p + strlen(tag);
    printf("Chihiro: SEGABOOT Ver.%.6s%s\n", version,
           strncmp(version, SEGABOOT_VERSION, strlen(SEGABOOT_VERSION))
               ? " — boot state reporting unavailable for this build" : "");
}

void chihiro_ide_load_rom(void)
{
    if (!chihiro_flash_rom) return;
    printf("Chihiro: flash ROM (%u bytes) ready for IDE hook\n",
           chihiro_flash_rom_size);
    chihiro_segaboot_identify();
}

uint8_t *chihiro_fatx_get_buffer(uint32_t *out_size)
{
    if (!chihiro_interface_ready) {
        fprintf(stderr, "Chihiro: ERROR — fatx_get_buffer called before interface_init\n");
        return NULL;
    }
    *out_size = (uint32_t)CHIHIRO_FS_SIZE;
    return (uint8_t *)memory_region_get_ram_ptr(&chihiro_interface_fs);
}

static void sg_write(QEMUSGList *sg, const void *src, int len)
{
    int sg_idx = 0, done = 0;
    while (done < len && sg_idx < sg->nsg) {
        int chunk = MIN(len - done, (int)sg->sg[sg_idx].len);
        dma_memory_write(&address_space_memory, sg->sg[sg_idx].base,
                         (const uint8_t *)src + done, chunk,
                         MEMTXATTRS_UNSPECIFIED);
        done += chunk;
        sg_idx++;
    }
}

bool chihiro_ide_serve(int dma_cmd, uint32_t lba, int n,
                       QEMUSGList *sg, bool *irq)
{
    *irq = true;

    if (dma_cmd == 0) { /* IDE_DMA_READ */
        /* FATX: synchronous from MemoryRegion RAM (timing-critical) */
        if (lba < CHIHIRO_MBCOM_BASE && chihiro_interface_ready) {
            uint64_t offset = (uint64_t)lba * 512;
            if (offset < CHIHIRO_FS_SIZE) {
                void *src = (uint8_t *)memory_region_get_ram_ptr(
                    &chihiro_interface_fs) + offset;
                sg_write(sg, src, n * 512);
                return true;
            }
        }
        /* mbcom response/command */
        if (chihiro_mbcom_enabled &&
            (lba == CHIHIRO_MBCOM_RESPONSE || lba == CHIHIRO_MBCOM_COMMAND)) {
            uint8_t buf[512] = {0};
            const uint8_t *src = (lba == CHIHIRO_MBCOM_RESPONSE)
                ? chihiro_mbcom_response : chihiro_mbcom_command;
            memcpy(buf, src, 32);
            sg_write(sg, buf, 512);
            return true;
        }
        /* flash ROM */
        if (lba >= CHIHIRO_MBROM0 && chihiro_flash_rom) {
            uint32_t rom_off = (lba - CHIHIRO_MBROM0) * 512;
            if (rom_off + (uint32_t)n * 512 <= chihiro_flash_rom_size) {
                sg_write(sg, chihiro_flash_rom + rom_off, n * 512);
                return true;
            }
        }
    }

    if (dma_cmd == 1) { /* IDE_DMA_WRITE */
        if (chihiro_mbcom_enabled &&
            (lba == CHIHIRO_MBCOM_RESPONSE || lba == CHIHIRO_MBCOM_COMMAND)) {
            uint8_t buf[512];
            dma_memory_read(&address_space_memory,
                            sg->sg[0].base, buf, 512,
                            MEMTXATTRS_UNSPECIFIED);
            if (lba == CHIHIRO_MBCOM_RESPONSE) {
                if (!chihiro_game_running)
                    memcpy(chihiro_mbcom_response, buf, 32);
            } else {
                memcpy(chihiro_mbcom_command, buf, 32);
                if (chihiro_game_running &&
                    (chihiro_mbcom_command[0] || chihiro_mbcom_command[1])) {
                    chihiro_mbcom_process();
                    memset(chihiro_mbcom_command, 0, 32);
                    if (chihiro_lpc_global)
                        chihiro_lpc_global->mbcom_e0_status |= 0x01;
                    if (chihiro_irq10_global) {
                        qemu_irq_lower(chihiro_irq10_global);
                        qemu_irq_raise(chihiro_irq10_global);
                    }
                }
            }
            return true;
        }
    }

    /* Any unhandled LBA: return zeros (read) or discard (write).
     * Never fall through to async block device — breaks JVS timing. */
    if (dma_cmd == 0) {
        int total = n * 512;
        int sg_idx = 0, done = 0;
        while (done < total && sg_idx < sg->nsg) {
            int chunk = MIN(total - done, (int)sg->sg[sg_idx].len);
            dma_memory_set(&address_space_memory, sg->sg[sg_idx].base,
                           0, chunk, MEMTXATTRS_UNSPECIFIED);
            done += chunk;
            sg_idx++;
        }
    }
    return true;
}

void chihiro_mbcom_init(void)
{
    memset(chihiro_mbcom_response, 0, sizeof(chihiro_mbcom_response));
    memset(chihiro_mbcom_command, 0, sizeof(chihiro_mbcom_command));
    chihiro_mbcom_enabled = true;
}

/* Process mbcom command and generate responses.
 * On real hardware, the SH4 CPU on the DIMM board (running VxWorks) handles
 * these commands via the 315-6322 ASIC. The PIC16C621A on the DIMM board only
 * provides a DES key to the SH4 at boot — it never sees mbcom traffic.
 * sp5001.bin is the JVS I/O board firmware (TMP90PH44N), unrelated to DIMM.
 * Returning instant READY/100% may cause MEDIA BOARD TEST in the service menu
 * to show "CHECKING 0%" then "STATUS ----" instead of progressing normally. */
static void chihiro_mbcom_process(void)
{
    const uint8_t *w = chihiro_mbcom_command;
    uint8_t *r = chihiro_mbcom_response;

    if (w[0] == 0 && w[1] == 0) return;  /* no command */

    uint16_t cmd_code = w[2] | (w[3] << 8);

    /* Cxbx-style response: echo sequence + command|0x8000 success flag */
    r[0] = w[0];
    r[1] = w[1];
    r[2] = w[2] | (cmd_code & 0xFF);         /* low byte of cmd | 0x8000 */
    r[3] = (w[3] & 0x7F) | 0x80;             /* high byte with bit15 set */
    /* zero out rest of 32-byte response area */
    memset(r + 4, 0, 28);

    {
        static int mbcom_cmd_log = 0;
        if (lpc_log_verbose && mbcom_cmd_log < 200) {
            mbcom_cmd_log++;
            fprintf(stderr, "[%07lld] MBCOM cmd=0x%04X seq=%02X%02X data: %02X %02X %02X %02X %02X %02X\n",
                    TS_MS, cmd_code, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }

    switch (cmd_code) {
    case MB_CMD_INIT: { /* DIMM size in bytes (from JP1/JP2 jumpers) */
        uint32_t sz = mediaboard.dimm_size;
        memcpy(r + 4, &sz, 4);
        break;
    }
    case MB_CMD_STATUS: /* Boot phase + completion percentage */
        r[4] = mediaboard.status; r[5] = 0; r[6] = 0; r[7] = 0;
        r[8] = mediaboard.progress; r[9] = 0; r[10] = 0; r[11] = 0;
        break;
    case MB_CMD_GET_VERSION: { /* Media board firmware version */
        uint16_t v = mediaboard.fw_version;
        memcpy(r + 4, &v, 2); r[6] = 0; r[7] = 0;
        break;
    }
    case MB_CMD_SYSTEM_TYPE: /* board_type | (fw_ver << 8) */
        r[4] = mediaboard.board_type;
        r[5] = mediaboard.fw_version & 0xFF;
        r[6] = (mediaboard.fw_version >> 8) & 0xFF;
        r[7] = 0;
        break;
    case MB_CMD_GET_SERIAL: /* From flash ROM MBDT+0x10 */
        memcpy(r + 4, mediaboard.serial, 16);
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
    case 0x0415: /* Network IP address */
        memcpy(r + 4, &mediaboard.net_ip, 4);
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
    case 0x0608: /* Network IP address (same as 0x0415) */
        memcpy(r + 4, &mediaboard.net_ip, 4);
        break;
    default:
        CHIHIRO_LOGF(MBCOM, "UNHANDLED cmd=0x%04X\n", cmd_code);
        break;
    }

    chihiro_mbcom_command[0] = 0;
    chihiro_mbcom_command[1] = 0;
    chihiro_mbcom_command[2] = 0;
    chihiro_mbcom_command[3] = 0;
}

