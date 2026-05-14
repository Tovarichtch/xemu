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
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "chihiro.h"
#include "chihiro_fatx.h"
#include "system/blockdev.h"
#include "hw/usb.h"
#include "target/i386/cpu.h"
#include "exec/watchpoint.h"
#include "ui/input.h"
#include "chihiro-jvs.h"

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
 * base address. Game XBE checks "XBAM" at 0x401E/0x4020 instead. Values are
 * switched after QuickReboot via chihiro_game_running flag.
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

    /* Kernel-loaded detection timer (polls until 2BL decrypts kernel) */
    QEMUTimer *kernel_ready_timer;
    bool kernel_ready;
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
    QEMUTimer *dimm_event_timer; /* fires after handshake to inject STATUS event */
    QEMUTimer *dimm_resp_timer;  /* delayed IRQ10 after execute trigger (Type-3) */
    QEMUTimer *t3_heartbeat_timer; /* ')' mode: periodic IRQ10 to wake worker thread */
    bool       t3_worker_alive;    /* set when game sends first E1=0xF in ')' mode */

    /* Type-3 unsolicited handshake state:
     * 0 = idle (waiting for game's cmd 0x0100 status query)
     * 1 = scheduled: heartbeat timer will send unsolicited 0x0002
     * 2 = sent: 0x0002 delivered, game will respond via EXEC cmd 0x0001 */
    int        t3_handshake_state;

    int64_t    last_lpc_activity_ms; /* timestamp of last LPC read/write */

} ChihiroLPCState;

#define CHIHIRO_LPC_DEVICE(obj) \
    OBJECT_CHECK(ChihiroLPCState, (obj), "chihiro-lpc")

static bool chihiro_active;
bool chihiro_game_running;  /* Set after QuickReboot — disables SEGABOOT DMA scan */
static bool chihiro_boot3_reached; /* Set when SEGABOOT reaches boot=3 (checks complete) */
static bool chihiro_quickreboot_pending; /* Set at QuickReboot, consumed by port 0x40F0 handler */
static char chihiro_game_filename[64]; /* Game XBE filename saved at boot=3 */
char chihiro_game_dir[1024];   /* Game directory path (from dvd_path) */
bool chihiro_board_type3;      /* true = ASIC (Type-3), false = FPGA (Type-1) */
static ChihiroLPCState *chihiro_lpc_global;
uint32_t chihiro_usb_sm_pa;  /* PA of USB state machine globals at VA 0xC3F10 */

bool lpc_log_verbose = false;

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

void chihiro_on_ohci_bus_start(void)
{
    if (!chihiro_active) {
        return;
    }

    ChihiroLPCState *s = chihiro_lpc_global;
    if (!s) {
        return;
    }

    ohci_bus_start_count++;
    fprintf(stderr, "[%07lld] Chihiro USB: BUS START #%d\n", TS_MS, ohci_bus_start_count);

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

    if (chihiro_game_running) {
        timer_mod(s->diag_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
        return;
    }


    uint32_t state = 0, counter = 0, ready = 0, gate = 0, bootstate = 0;
    uint32_t bootflag = 0, slotcount = 0, mainflag = 0;
    uint8_t slotflag0 = 0;

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

    /* SEGABOOT state change logger — always active (error detection) */
    {
        static uint32_t prev_state = 0;
        if (state != prev_state && state_pa != 0xFFFFFFFF) {
            const char *desc = "";
            switch (state) {
                case 0: desc = "INIT"; break;
                case 1: desc = "CHECK_ERRORS"; break;
                case 2: desc = "CAUTION (boot handshake)"; break;
                case 3: desc = "CAUTION_WAIT"; break;
                case 4: desc = "GAME_READY"; break;
                case 5: desc = "GAME_RUNNING"; break;
                case 6: desc = "ERROR_DISPLAY"; break;
                case 7: desc = "SERVICE_MENU"; break;
                case 8: desc = "GAME_TEST"; break;
            }
            fprintf(stderr, "[%07lld] SEGABOOT: state %u → %u (%s)\n",
                    TS_MS, prev_state, state, desc);

            /* If entering ERROR_DISPLAY, read error code from counter */
            if (state == 6 && counter > 0 && counter <= 53) {
                static const char *err_causes[] = {
                    [1]  = "Hardware initialization failed",
                    [2]  = "USB enumeration failed (QC/SC not found)",
                    [3]  = "Main board serial invalid",
                    [4]  = "Media board serial invalid",
                    [5]  = "Region mismatch (EEPROM vs boot.id)",
                    [6]  = "DIMM board communication failed",
                    [11] = "JVS I/O board not connected",
                    [14] = "Network board error (SC EEPROM version timeout)",
                    [21] = "SYSTEM_TYPE check failed",
                    [22] = "SADDR communication timeout (V850 not present)",
                    [27] = "GetBootData returned NULL",
                    [31] = "SYSTEM_TYPE is zero",
                };
                const char *cause = (counter < 54 && err_causes[counter])
                                   ? err_causes[counter] : "unknown";
                fprintf(stderr, "[%07lld] SEGABOOT: *** ERROR %02u — %s ***\n",
                        TS_MS, counter, cause);
            }
            prev_state = state;
        }
    }

    /* Detect bootstate changes between ticks (guard against garbage VAs) */
    if (bootstate != s->last_bootstate && bootstate < 100 && s->last_bootstate < 100) {
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
                                break;
                            }
                        }
                    }
                }
            }

            /* LLE: SEGABOOT calls XLaunchNewImageA which allocates LDP,
             * marks it persistent, and fills launch data. Kernel's STICKY
             * section preserves LaunchDataPage pointer across QuickReboot. */
        }
        s->last_bootstate = bootstate;
    }

    /* Suppress unused-variable warnings for read-but-not-logged fields */
    (void)ready; (void)gate; (void)bootflag; (void)slotcount; (void)slotflag0; (void)mainflag;

    timer_mod(s->diag_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
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
 * CheckErrors state machine (VA 0x87AFC):
 *   0=INIT, 1=CHECK_ERRORS, 2=CAUTION (boot handshake), 3=CAUTION_WAIT,
 *   4=GAME_READY, 5=GAME_RUNNING, 6=ERROR_DISPLAY, 7=SERVICE_MENU, 8=GAME_TEST
 *
 * Error codes set in counter (VA 0x87AE8) when state=6:
 *   02=USB enum fail, 03=main serial bad, 04=media serial bad, 05=region mismatch,
 *   06=DIMM comm fail, 11=JVS not connected, 14=SC EEPROM version timeout,
 *   21=SYSTEM_TYPE fail, 22=SADDR timeout (no V850), 27=GetBootData NULL,
 *   31=SYSTEM_TYPE zero
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
static void chihiro_usb_poll_patch_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    if (s->usb_poll_patched) return;
    if (chihiro_game_running) return;

    /* No patches to apply — mark done and start diag timer */
    s->usb_poll_patched = true;

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

    /* Ignore SCRATCH=0x04 during first kernel init — the kernel writes
     * SCRATCH as part of normal boot before SEGABOOT even loads.
     * Only react after SEGABOOT has been patched at least once. */
    if (!chihiro_lpc_global || !chihiro_lpc_global->usb_poll_patched) {
        return;
    }

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
                }
            }
            fclose(f);
        }
    }

    chihiro_quickreboot_pending = true;

    /* Re-arm SEGABOOT patch scanner and reset port counters.
     * QuickReboot reloads SEGABOOT from flash ROM (patches lost).
     * Kernel code stays patched (QuickReboot keeps kernel in RAM). */
    if (chihiro_lpc_global) {
        ChihiroLPCState *s = chihiro_lpc_global;
        s->usb_poll_patched = false;
        timer_mod(s->usb_poll_patch_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
        s->lpc_401e_reads = 0;
        s->mbcom_handshake_done = false;
        s->lpc_40f0_reads = 0;
        s->dimm_cmd_count = 0;
        s->dimm_next_seq = 1;

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

    if (chihiro_game_running) return;

    /* Periodic DMA slot scan — same logic as the port 0x40F0 handler.
     * SEGABOOT's writes_3bc_3d4 sends STATUS commands via DMA slots,
     * but stops reading port 0x40F0 after initial boot. Without this
     * timer, those commands go unprocessed and app_obj+0x3BC is never
     * set, blocking game launch when SERVICE button is released. */

    static const struct { uint32_t slot_va; uint32_t meta_va; uint32_t stride; const char *name; }
        layouts[] = {
            { 0xAA7B0, 0xAA790, 0x60, "2.00-fpr21042" },
            { 0x89760, 0x89740, 0x40, "2.00-fpr23887" },
        };

    static int scan_fail_count = 0;

    int processed = 0;
    for (int layout = 0; layout < 2; layout++) {
        uint32_t slot_base_pa = chihiro_va_to_pa(layouts[layout].slot_va);
        if (slot_base_pa == 0xFFFFFFFF) {
            scan_fail_count++;
            if (scan_fail_count <= 5)
                fprintf(stderr, "[%07lld] DIMM-SCAN: layout '%s' VA=0x%05X → PA FAIL\n",
                        TS_MS, layouts[layout].name, layouts[layout].slot_va);
            continue;
        }
        uint32_t meta_base_pa = chihiro_va_to_pa(layouts[layout].meta_va);
        if (meta_base_pa == 0xFFFFFFFF) continue;
        if (slot_base_pa >= 0x800000 || meta_base_pa >= 0x800000) continue;

        uint32_t stride = layouts[layout].stride;
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
            case 0x0001: resp_data = 0x20000000; break;
            case 0x0100: resp_data = 5; resp_data2 = 100; break;
            case 0x0101: resp_data = 0x0317; break;
            case 0x0102: resp_data = 0x8002; break;
            case 0x0103: resp_data = 0x6261632D; break;
            default: break;
            }

            meta_marker = 0x0001;
            cpu_physical_memory_write(meta_pa + 2, &meta_marker, 2);
            cpu_physical_memory_write(meta_pa + 4, &resp_data, 4);
            if (cmd_opcode == 0x0100)
                cpu_physical_memory_write(meta_pa + 8, &resp_data2, 4);
            s->mbcom_e0_status |= 0x05;
            qemu_irq_raise(s->irq10);

            processed++;
        }
        break;
    }

    timer_mod(s->dimm_event_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

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
        case 0x0001: /* GetDIMMSize — 512MB */
            s->dimm_resp[1] = 0x20000000;
            break;
        case 0x0100: /* GetMediaBoardStatus — phase=5 (loaded), progress=100% */
            s->dimm_resp[1] = 5;
            s->dimm_resp[2] = 100;
            break;
        case 0x0101: /* GetSegaBootVersion — 3.17 */
            s->dimm_resp[1] = 0x0317;
            s->dimm_resp[2] = 1;
            break;
        case 0x0102: { /* GetSystemFlags — Dolphin byte layout */
            uint8_t *p = (uint8_t *)&s->dimm_resp[1];
            p[0] = 1;    /* flag (must be nonzero) */
            p[1] = 1;    /* media type: GDROM=1 */
            p[2] = 0;    /* development mode: 0=normal */
            p[3] = 0;
            s->dimm_resp[2] = 0; /* access count */
            break;
        }
        case 0x0103: /* GetMediaBoardSerial */
            memcpy(&s->dimm_resp[1], "AAEE-01A00000001", 16);
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
            memcpy(&s->dimm_resp[2], "AAEE-01A00000001", 16);
            break;
        default:
            break;
        }
    }

    s->dimm_resp_ready = true;
    s->dimm_next_seq = seq + 1;
    s->dimm_cmd_count++;

    if (s->dimm_cmd_count == 5 && s->dimm_event_timer) {
        timer_mod(s->dimm_event_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
    }
}

static uint64_t chihiro_lpc_io_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    uint64_t r = 0;

    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);
    s->last_lpc_activity_ms = TS_MS;

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
        case 0xA0001E60: r = 0x00000002; break; /* V850 firmware state: 2 = CRC OK / ready */
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
                r = 0x00000002; /* V850 firmware state: 2 = CRC OK / ready */
                static int a1e60_log = 0;
                if (lpc_log_verbose && a1e60_log < 20) { a1e60_log++;
                    fprintf(stderr, "[%07lld] SADDR READ 0xA0001E60 → 0x%08X (fw state)\n", TS_MS, r); }
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
    case 0xF0: /* Board mode / device count.
               * SEGABOOT checks high byte: 0 = Type-1, non-zero = Type-3.
               * Returning wrong type → SEGABOOT uses wrong comm path → error.
               * acLib v0.71+ (WMMT1): high byte also selects device type:
               *   0 → 0x21 ('!'), 1 → 0x29 (')').
               * Type ')' triggers SC search → EEPROM version read.
               * Without it, SC version stays 0 → Error 14.
               * TODO: find correct Type-1 value that satisfies both. */
        r = chihiro_board_type3 ? 0x0100 : 0x0001;
        { static int f0_log = 0; if (lpc_log_verbose && f0_log < 500) { f0_log++;
            fprintf(stderr, "[%07lld] F0 READ → 0x%04X (type3=%d)\n",
                    TS_MS, (unsigned)r, chihiro_board_type3); } }
        s->lpc_40f0_reads++;
        if (chihiro_quickreboot_pending) {
            chihiro_quickreboot_pending = false;
        }
        break;
    case SEGA_DIMM_SIZE:
        r = SEGA_DIMM_SIZE_512M;        /* Kernel computes mbcom LBA from this:
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
                    TS_MS, (unsigned)r, (r & 1), (r >> 2) & 1); }
        break;
    }
    case 0x84:  /* Port 0x4084 — MbcomCommand session handle */
        r = 0x0000;
        s->lpc_4084_reads++;
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

static uint8_t chihiro_mbcom_command[512];
static uint8_t chihiro_mbcom_response[512];
static void chihiro_mbcom_process(void);

static void chihiro_lpc_io_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);
    s->last_lpc_activity_ms = TS_MS;

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
        if (val == 0x0102) {
            s->mbcom_handshake_done = true;
        }
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
                           *   Worker writes cmd to DMA (FC801), then E1=0xF, E1=0
                           *   E1=0 = "process the DMA command" (not scratch!)
                           * scratch 0x0102 = status register (bit8=busy), NOT a command */
        static bool e1_armed = false;
        static int e1_log = 0;
        if (val != 0) {
            if (!chihiro_game_running &&
                (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0)) {
                /* SEGABOOT DMA: E1=non-zero with command in buffer → process */
                uint8_t dma0 = chihiro_mbcom_command[0];
                uint8_t dma1 = chihiro_mbcom_command[1];
                chihiro_mbcom_process();
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0x%X SEGABOOT_DMA dma=%02X%02X\n",
                            TS_MS, (unsigned)val, dma0, dma1); }
                s->mbcom_e0_status |= 0x01;
                s->mbcom_resp_ready = true;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
            } else {
                /* ARM for E1=0 trigger (game mode or SEGABOOT ack) */
                e1_armed = true;
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0x%X ARM scratch=0x%04X dma=%02X%02X\n",
                            TS_MS, (unsigned)val, s->lpc_scratch_4026,
                            chihiro_mbcom_command[0], chihiro_mbcom_command[1]); }
            }
        } else {
            if (chihiro_game_running && chihiro_board_type3 && e1_armed) {
                /* ')' mode (Type-3 game): E1=0 is the game worker's
                 * confirmation that it processed the ARM cycle. Clear E0
                 * so the worker can proceed to read SADDR. */
                if (!s->t3_worker_alive) {
                    s->t3_worker_alive = true;
                    if (lpc_log_verbose) fprintf(stderr, "[%07lld] T3 WORKER ALIVE\n", TS_MS);
                }
                s->mbcom_e0_status &= ~0x01;
                qemu_irq_lower(s->irq10);

                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 T3_ACK e0=0x%02X irq10↓\n",
                            TS_MS, s->mbcom_e0_status); }
            } else if (e1_armed && s->mbcom_resp_ready) {
                /* RESP_DELIVER ('!' mode / SEGABOOT) */
                s->mbcom_resp_ready = false;
                qemu_irq_lower(s->irq10);
                qemu_irq_raise(s->irq10);
                if (lpc_log_verbose && e1_log < 2000) { e1_log++;
                    fprintf(stderr, "[%07lld] E1=0 RESP_DELIVER scratch=0x%04X resp: %02X%02X %02X%02X\n",
                            TS_MS, s->lpc_scratch_4026,
                            chihiro_mbcom_response[0], chihiro_mbcom_response[1],
                            chihiro_mbcom_response[2], chihiro_mbcom_response[3]); }
            } else if (e1_armed && (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0)) {
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
                            TS_MS, e1_armed, s->lpc_scratch_4026); }
            }
            e1_armed = false;
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
 * Contains the SEGABOOT XBE that boots before the game. */
static uint8_t *chihiro_flash_rom = NULL;
static uint32_t chihiro_flash_rom_size = 0;

uint8_t *chihiro_ic10_data = NULL;
uint32_t chihiro_ic10_size = 0;
uint8_t *chihiro_ic11_data = NULL;
uint32_t chihiro_ic11_size = 0;
uint8_t *chihiro_pc20_data = NULL;
uint32_t chihiro_pc20_size = 0;

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
                } else {
                    g_free(chihiro_flash_rom);
                    chihiro_flash_rom = NULL;
                }
            }
            fclose(f);
            if (chihiro_flash_rom) return;
        }
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

    chihiro_ic10_data = load_eeprom_file(dir, "ic10_g24lc64.bin",
                                         8192, &chihiro_ic10_size);
    chihiro_ic11_data = load_eeprom_file(dir, "ic11_24lc024.bin",
                                         128, &chihiro_ic11_size);
    chihiro_pc20_data = load_eeprom_file(dir, "pc20_g24lc64.bin",
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

static void chihiro_t3_heartbeat_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    if (!chihiro_game_running || !chihiro_board_type3) return;

    /* V850 unsolicited 0x0002/0x0003 = RESET REQUESTS (acLibUpdateMedia
     * → XLaunchNewImageA). Must NEVER be sent. Heartbeat reserved for
     * future use if needed. */
    (void)s;
}

static void chihiro_irq10_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    /* IRQ10 for SEGABOOT baseboard communication */
    if (s->kernel_ready && !chihiro_game_running) {
        qemu_irq_raise(s->irq10);
    }

    /* Game XBE detection: entry point change signals game loaded.
     * Arm after entry is stable for 50 ticks (800ms) — avoids false
     * positive on kernel→SEGABOOT transition during first boot. */
    if (!chihiro_game_running && chihiro_active) {
        static uint32_t segaboot_entry = 0;
        static int stable_count = 0;
        static bool armed = false;
        uint32_t entry_pa = chihiro_va_to_pa(0x10000 + 0x128);
        {
            static int detect_log = 0;
            if (lpc_log_verbose && (detect_log < 10 || (detect_log < 200 && (detect_log % 50) == 0))) {
                uint32_t cur = 0;
                if (entry_pa != 0xFFFFFFFF)
                    cpu_physical_memory_read(entry_pa, &cur, 4);
                fprintf(stderr, "[%07lld] DETECT: pa=0x%08X entry=0x%08X seg=0x%08X armed=%d stable=%d\n",
                        TS_MS, entry_pa, cur, segaboot_entry, armed, stable_count);
            }
            detect_log++;
        }
        if (entry_pa != 0xFFFFFFFF) {
            uint32_t cur_entry = 0;
            cpu_physical_memory_read(entry_pa, &cur_entry, 4);
            if (cur_entry > 0x10000 && cur_entry < 0x08000000) {
                if (cur_entry == segaboot_entry) {
                    if (!armed && ++stable_count >= 50) {
                        armed = true;
                    }
                } else if (armed) {
                    chihiro_game_running = true;
                    s->lpc_scratch_4026 = 0;  /* clear stale SEGABOOT handshake value */
                    memset(chihiro_mbcom_command, 0, 32); /* clear stale SEGABOOT commands */
                    s->mbcom_resp_ready = false;
                    s->mbcom_e0_status = 0;
                    timer_del(s->dimm_event_timer);
                    fprintf(stderr, "[%07lld] *** GAME XBE DETECTED (entry 0x%08X → 0x%08X) ***\n",
                            TS_MS, segaboot_entry, cur_entry);
                } else {
                    segaboot_entry = cur_entry;
                    stable_count = 0;
                }
            }
        }
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
        static bool bootstrap_done = false;
        if (!bootstrap_done) {
            memset(chihiro_mbcom_command, 0, 32);

            if (chihiro_board_type3) {
                /* ')' mode: purely reactive. Game state machine calls
                 * FUN_0014c580 to set up connection, then sends cmd 0x0001
                 * via EXEC. Our response (0x8001+DIMM_SIZE) serves as the
                 * handshake message. No proactive heartbeat needed. */
                s->t3_worker_alive = false;
                s->t3_handshake_state = 0;
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
            bootstrap_done = true;
        }

        /* Type-3 unsolicited 0x0002 is scheduled by the EXEC handler
         * after the game's cmd 0x0100 status query. The heartbeat timer
         * delivers it 200ms later, after the status response is consumed. */
    }

    /* DMA META scan: provide mbcom slot responses to SEGABOOT */
    if (chihiro_mbcom_enabled && !chihiro_game_running
        && chihiro_lpc_global && chihiro_lpc_global->usb_poll_patched) {
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
                case 0x0001: resp_data = 0x20000000; break;
                case 0x0100: resp_data = 5; resp_data2 = 100; break;
                case 0x0101: resp_data = 0x0317; break;
                case 0x0102: resp_data = 0x8002; break;
                case 0x0103: resp_data = 0x6261632D; break;
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


/*
 * Kernel-loaded detection.
 *
 * The Chihiro kernel is encrypted in the BIOS and gets decrypted by the
 * 2BL at runtime. We poll PA 0x3B744 for the expected JNZ opcode (0x75 0x22)
 * to detect when the kernel is in RAM and set the kernel_ready flag.
 *
 * This flag gates IRQ10 delivery — baseboard interrupts must not fire
 * before the kernel's interrupt handlers are installed.
 *
 * No RAM patches are applied. All former hacks are now handled by proper
 * emulation:
 *   - EEPROM: generated with debug key (XBOX_EEPROM_VERSION_D)
 *   - XBE digests: XCCalcDigest uses SHA1(size_le32 || data), FATX data matches
 *   - SEGABOOT: all checks pass via correct baseboard/USB emulation
 */

static void chihiro_kernel_ready_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;

    if (!s->kernel_ready) {
        uint8_t check[2];
        address_space_read(&address_space_memory, 0x3B744,
                           MEMTXATTRS_UNSPECIFIED, check, 2);

        if (check[0] == 0x75 && check[1] == 0x22) {
            s->kernel_ready = true;
            return;
        }
        timer_mod(s->kernel_ready_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
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

    /* Detect when 2BL has decrypted the kernel into RAM.
     * Gates IRQ10 delivery until kernel interrupt handlers are ready. */
    s->kernel_ready = false;
    s->kernel_ready_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                         chihiro_kernel_ready_cb, s);
    timer_mod(s->kernel_ready_timer,
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
    s->dimm_resp_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       chihiro_dimm_resp_timer_cb, s);
    s->t3_heartbeat_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                          chihiro_t3_heartbeat_cb, s);
    s->t3_worker_alive = false;
    s->t3_handshake_state = 0;
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

    qemu_input_handler_register(dev, &chihiro_kbd_handler);

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

void chihiro_mbcom_init(void)
{
    memset(chihiro_mbcom_response, 0, sizeof(chihiro_mbcom_response));
    memset(chihiro_mbcom_command, 0, sizeof(chihiro_mbcom_command));
    chihiro_mbcom_enabled = true;
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
    case 0x0102: /* SYSTEM_TYPE — firmware format: board_type | (fw_ver<<8) | (mode<<16)
                  * board_type: 0=NAOMI, 3=GD-ROM, 4=Chihiro. SEGABOOT requires >=2 */
        r[4] = 0x04; r[5] = 0x17; r[6] = 0x03; r[7] = 0;
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
        if (lpc_log_verbose) fprintf(stderr, "[%07lld] MBCOM UNHANDLED cmd=0x%04X\n", TS_MS, cmd_code);
        break;
    }

    /* MAME: clear command header bytes 0-3 after processing (ack to baseboard) */
    chihiro_mbcom_command[0] = 0;
    chihiro_mbcom_command[1] = 0;
    chihiro_mbcom_command[2] = 0;
    chihiro_mbcom_command[3] = 0;

    /* Start periodic DMA slot scan after initial handshake commands.
     * SEGABOOT stops reading port 0x40F0 after boot, so the DMA scan
     * in the 0x40F0 handler stops running. This timer ensures that
     * STATUS commands from writes_3bc_3d4 (which sets app_obj+0x3BC
     * gate for game launch) are processed independently.
     * Counter resets on QuickReboot (modulo 5 check). */
    {
        static int mbcom_ide_cmd_count = 0;
        mbcom_ide_cmd_count++;
        if ((mbcom_ide_cmd_count % 5) == 0 && chihiro_lpc_global
            && chihiro_lpc_global->dimm_event_timer) {
            timer_mod(chihiro_lpc_global->dimm_event_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 200);
        }
    }
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
    static uint32_t ide_game_xbe_cnt = 0;
    static bool ide_after_quickreboot = false;
    static uint32_t ide_post_qr_flash_cnt = 0, ide_post_qr_fatx_cnt = 0;

    if (!ide_after_quickreboot) {
        uint8_t khqb = 0;
        cpu_physical_memory_read(0x3A93C, &khqb, 1);
        if (khqb) {
            ide_after_quickreboot = true;
            ide_post_qr_flash_cnt = 0;
            ide_post_qr_fatx_cnt = 0;
            ide_game_xbe_cnt = 0;
        }
    }

    if (lba >= 0x89 && lba < CHIHIRO_MBCOM_BASE) {
        ide_fatx_cnt++;
        if (ide_after_quickreboot) ide_post_qr_fatx_cnt++;
        /* Game XBE range: cluster 22674 → LBA 725672, ~4280 sectors */
        if (lba >= 725672 && lba < 730000) {
            ide_game_xbe_cnt++;
        }
    } else if (lba < 0x89) {
        ide_meta_cnt++;
    } else {
        ide_mbcom_cnt++;
    }
    {
        static uint32_t ide_total = 0;
        if (lpc_log_verbose && ++ide_total % 500 == 0)
            fprintf(stderr, "[%07lld] IDE-STATS: total=%u meta=%u fatx=%u mbcom=%u "
                   "(post-QR: flash=%u fatx=%u game_xbe=%u)\n",
                   TS_MS, ide_total, ide_meta_cnt, ide_fatx_cnt, ide_mbcom_cnt,
                   ide_post_qr_flash_cnt, ide_post_qr_fatx_cnt, ide_game_xbe_cnt);
    }

    /* FATX partition: serve from in-memory image (LBA 0 to ~700K) */
    if (lba < CHIHIRO_MBCOM_BASE && chihiro_fatx_read_sector(lba, buffer)) {
        return true;
    }

    if (lba == CHIHIRO_MBCOM_RESPONSE) {
        memset(buffer, 0, 512);
        memcpy(buffer, chihiro_mbcom_response, 32);
        const uint8_t *d = (const uint8_t *)buffer;
        static int fc800_read_count = 0;
        fc800_read_count++;
        if (lpc_log_verbose && fc800_read_count <= 20) {
            fprintf(stderr, "[%07lld] MBCOM READ FC800 #%d: %02X%02X %02X%02X "
                    "%02X%02X%02X%02X %02X%02X%02X%02X\n",
                    TS_MS, fc800_read_count,
                    d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11]);
        }
        return true;
    }
    if (lba == CHIHIRO_MBCOM_COMMAND) {
        memset(buffer, 0, 512);
        memcpy(buffer, chihiro_mbcom_command, 32);
        return true;
    }

    /* mbrom0/mbrom1: serve from loaded flash ROM file.
     * baseboard.img layout: LBA 0x8000800 → byte 0 of fpr-23887 (full 2MB).
     * mbrom0 (LBA 0x8000000-0x80007FF): returns zeros (not needed for boot).
     * mbrom1 (LBA 0x8000800+): flash ROM from byte 0. */
    if (lba >= CHIHIRO_MBROM0 && chihiro_flash_rom) {
        ide_post_qr_flash_cnt++;
        memset(buffer, 0, 512);
        uint32_t bank_offset = (lba >= CHIHIRO_MBROM1) ? 0x100000 : 0;
        static bool mbrom1_logged = false;
        if (bank_offset && !mbrom1_logged) {
            printf("[%07lld] Chihiro: mbrom1 read (SEGABOOT 2.13)\n", TS_MS);
            mbrom1_logged = true;
        }
        uint32_t offset = bank_offset + (lba & 0x7FF) * 512;
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
        if (chihiro_game_running) {
            /* Game mode: FC800 writes are the game clearing its local copy of the
             * response buffer before sending a command. On real hardware, the V850
             * response lives in separate memory (0x0C000000) — don't let the game's
             * buffer clear overwrite our pending response. Just ignore. */
        } else {
            memcpy(chihiro_mbcom_response, buffer, 32);
        }
        return true;
    }
    if (lba == CHIHIRO_MBCOM_COMMAND) {
        memcpy(chihiro_mbcom_command, buffer, 32);
        if (!chihiro_game_running &&
            (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0)) {
            /* SEGABOOT: process immediately on DMA write (no E1 trigger needed) */
            chihiro_mbcom_process();
            if (chihiro_lpc_global) {
                chihiro_lpc_global->mbcom_e0_status |= 0x01;
            }
            if (chihiro_irq10_global) {
                qemu_irq_lower(chihiro_irq10_global);
                qemu_irq_raise(chihiro_irq10_global);
            }
        }
        if (chihiro_game_running &&
            (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0)) {
            /* Game mode: process immediately — the acLib poll loop blocks
             * the update thread waiting for the response. Clear the command
             * buffer after processing to prevent the E1 handler from
             * double-processing the same command. */
            chihiro_mbcom_process();
            memset(chihiro_mbcom_command, 0, 32);
            if (chihiro_lpc_global) {
                chihiro_lpc_global->mbcom_e0_status |= 0x01;
            }
            if (chihiro_irq10_global) {
                qemu_irq_lower(chihiro_irq10_global);
                qemu_irq_raise(chihiro_irq10_global);
            }
        }
        return true;
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
