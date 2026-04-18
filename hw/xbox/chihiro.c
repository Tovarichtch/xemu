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
} ChihiroLPCState;

#define CHIHIRO_LPC_DEVICE(obj) \
    OBJECT_CHECK(ChihiroLPCState, (obj), "chihiro-lpc")

static bool chihiro_active;
static ChihiroLPCState *chihiro_lpc_global;

/* USB devices for delayed hotplug (simulates AN2131 I2C firmware boot) */
static USBDevice *chihiro_usb_qc = NULL;
static USBDevice *chihiro_usb_sc = NULL;

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
static uint32_t chihiro_va_to_pa(uint32_t va)
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

    /* XBE2 game state — D07A8 must reach 4 for game to advance */
    uint32_t d07a8_pa = chihiro_va_to_pa(0xD07A8);
    uint32_t d0798_pa = chihiro_va_to_pa(0xD0798);
    if (d07a8_pa != 0xFFFFFFFF) cpu_physical_memory_read(d07a8_pa, &xbe2_d07a8, 4);
    if (d0798_pa != 0xFFFFFFFF) cpu_physical_memory_read(d0798_pa, &xbe2_d0798, 4);

    /* XBE2 CE state machine at [0xCB9EC] — mirrors SEGABOOT's CE at [0x87AFC] */
    uint32_t xbe2_ce_pa = chihiro_va_to_pa(0xCB9EC);
    if (xbe2_ce_pa != 0xFFFFFFFF) cpu_physical_memory_read(xbe2_ce_pa, &xbe2_ce_state, 4);

    /* Detect bootstate changes between ticks */
    if (bootstate != s->last_bootstate) {
        printf("[%07lld] DIAG: *** BOOTSTATE CHANGED %u → %u ***\n", TS_MS,
               s->last_bootstate, bootstate);
        s->last_bootstate = bootstate;
    }

    printf("[%07lld] DIAG: CE st=%u cnt=%u rdy=%u | gate=%u boot=%u flag=0x%02X "
           "slots=%u/s0=0x%02X mflag=%u | 40F0=%u 401E=%u 4084=%u | tick=%u d7a8=%u ce2=%u %s\n",
           TS_MS, state, counter, ready, gate, bootstate,
           bootflag & 0xFF, slotcount, slotflag0, mainflag,
           s->lpc_40f0_reads, s->lpc_401e_reads, s->lpc_4084_reads,
           xbe2_d0798, xbe2_d07a8, xbe2_ce_state,
           (xbe2_ce_pa != 0xFFFFFFFF) ? "xbe2:mapped" : "xbe2:UNMAPPED");

    timer_mod(s->diag_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void chihiro_usb_poll_patch_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;
    if (s->usb_poll_patched) return;

    /*
     * Patch 1: UsbEnumPoll error check (VA 0x41F57)
     *   E8 64 F6 FF FF   call UsbEnumPoll
     *   85 C0             test eax, eax
     *   74 0F             je +0x0F → success path  ← patch to EB (jmp)
     */
    static const uint8_t sig_enumpoll[] = {
        0xE8, 0x64, 0xF6, 0xFF, 0xFF,
        0x85, 0xC0,
        0x74, 0x0F
    };

    /*
     * Patch 2: RegisterClassDriver error check (VA 0x41F74)
     *   E8 27 54 00 00   call RegisterClassDriver
     *   85 C0             test eax, eax
     *   74 0F             je +0x0F → success path  ← patch to EB (jmp)
     */
    static const uint8_t sig_classdrv[] = {
        0xE8, 0x27, 0x54, 0x00, 0x00,
        0x85, 0xC0,
        0x74, 0x0F
    };

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

    /*
     * Patch 4: CreateThread return check (VA 0x425DE)
     *
     *   85 C0                test eax, eax
     *   A3 34 A1 08 00       mov [0x8A134], eax
     *   5B                   pop ebx
     *   75 12                jne +0x12 (success)    <- patch to EB
     */
    static const uint8_t sig_createthread[] = {
        0x85, 0xC0,                            /* test eax, eax      */
        0xA3, 0x34, 0xA1, 0x08, 0x00,         /* mov [0x8A134], eax */
        0x5B,                                  /* pop ebx            */
        0x75, 0x12                             /* jne +0x12          */
    };

    /*
     * Patch 5 (DIAGNOSTIC): Error value at VA 0x2E3AB
     *   C7 07 14 00 00 00    mov [edi], 0x14  (error code)
     */
    static const uint8_t sig_errval[] = {
        0xC7, 0x07, 0x14, 0x00, 0x00, 0x00   /* mov [edi], 0x14 */
    };

    /*
     * Patch 6: UsbPollQC_inner (VA 0x51140)
     *
     *   E9 FB FE FF FF    jmp 0x51040
     *   90 90 90 90        nop padding
     *
     * Patch: xor eax,eax; ret -> USB poll "succeeds"
     * CheckErrors state machine State 1 calls this.
     */
    static const uint8_t sig_usbpollqc[] = {
        0xE9, 0xFB, 0xFE, 0xFF, 0xFF,  /* jmp 0x51040  */
        0x90, 0x90, 0x90, 0x90          /* nop padding  */
    };

    /*
     * Patch 7: UsbPollSC_inner (VA 0x51150)
     *
     *   55                push ebp
     *   8B EC             mov ebp, esp
     *   83 E4 F8          and esp, -8
     *   81 EC 0C 03 00 00 sub esp, 0x30C
     *
     * Patch: xor eax,eax; ret -> USB poll "succeeds"
     */
    static const uint8_t sig_usbpollsc[] = {
        0x55,                                  /* push ebp           */
        0x8B, 0xEC,                            /* mov ebp, esp       */
        0x83, 0xE4, 0xF8,                      /* and esp, -8        */
        0x81, 0xEC, 0x0C, 0x03, 0x00, 0x00     /* sub esp, 0x30C    */
    };

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

    /* Patch byte arrays */
    static const uint8_t patch_jmp[]   = { 0xEB };
    static const uint8_t patch_and0[]  = { 0x00 };
    static const uint8_t patch_xor_ret[] = { 0x31, 0xC0, 0xC3 };
    static const uint8_t patch_xor_ret4[] = { 0x31, 0xC0, 0xC2, 0x04, 0x00 };
    static const uint8_t patch_xor_nop3[]  = { 0x31, 0xC0, 0x90, 0x90, 0x90 }; /* xor eax, eax; nop*3 */

    ChihiroPatch patches[] = {
        { sig_enumpoll, sizeof(sig_enumpoll), 7,  patch_jmp,     1, 0x41F57, "UsbEnumPoll check (je->jmp)",         false },
        { sig_classdrv, sizeof(sig_classdrv), 7,  patch_jmp,     1, 0x41F74, "RegisterClassDriver check (je->jmp)", false },
        { sig_qcbyte0,  sizeof(sig_qcbyte0),  0,  patch_xor_ret, 3, 0x3AD80, "GetQcStatusByte0 (xor eax,eax; ret)", false },
        { sig_createthread, sizeof(sig_createthread), 8, patch_jmp, 1, 0x425D6, "CreateThread return (jne->jmp)", false },
        { sig_errval,   sizeof(sig_errval),   2,  patch_and0,    1, 0x2E3AB, "DIAG: error value 0x14->0x00",        false },
        { sig_usbpollqc, sizeof(sig_usbpollqc), 0, patch_xor_ret4, 5, 0x51140, "UsbPollQC_inner (xor eax,eax; ret 4)", false },
        { sig_usbpollsc, sizeof(sig_usbpollsc), 0, patch_xor_ret4, 5, 0x51150, "UsbPollSC_inner (xor eax,eax; ret 4)", false },
        { sig_enccheck,  sizeof(sig_enccheck),  2, patch_xor_ret, 2, 0x3A953, "EncryptionCheck (test->xor eax,eax)",  false },
        /* v153: REMOVED MbcomPollReady (was always return 1) — let clear-on-read deliver real responses */
        /* v153: REMOVED GetBootData (was always return 1) — let real boot data flow through */
        { sig_check_mainserial,  sizeof(sig_check_mainserial),  4, patch_xor_nop3, 5, 0x2EC35, "CheckMainBoardSerial (err 3 -> 0)",  false },
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

    if (applied == num_patches) {
        s->usb_poll_patched = true;
        printf("[%07lld] Chihiro: All %d SEGABOOT patches applied\n", TS_MS, num_patches);
        printf("[%07lld] Chihiro: CheckErrors state machine will run naturally "
               "with UsbPollQC/SC bypassed\n", TS_MS);

        /* Start diagnostic timer to monitor state machine progress */
        s->diag_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, chihiro_diag_timer_cb, s);
        timer_mod(s->diag_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
        return;
    }

    timer_mod(s->usb_poll_patch_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

/* Called from SMC handler when kernel writes SMC_REG_POWER (QuickReboot).
 * Blocks qemu_system_reset_request for Chihiro — the kernel handles
 * soft-reset internally via HalReturnToFirmware(2).
 * TODO: once SEGABOOT passes naturally, write LaunchDataPage here. */
bool chihiro_intercept_reset(void)
{
    if (chihiro_active) {
        printf("[%07lld] Chihiro: QuickReboot intercepted (SMC cmd=0x02) — blocking QEMU reset\n", TS_MS);
        return true;  /* Block qemu_system_reset_request */
    }
    return false;
}

static uint64_t chihiro_lpc_io_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    uint64_t r = 0;

    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);

    switch (addr) {
    case 0x00: /* Port 0x4000: read register data */
        switch (s->lpc_reg_addr) {
        case 0x80000140: r = 0x01; break;       /* CPU ready */
        case 0x80000160: r = 0x01; break;       /* PCI status */
        case 0xA0001E60: r = 0x00000020; break; /* DMA mode 6 */
        case 0xA0000000: r = s->lpc_reg_data; break; /* indirect read data */
        default: r = 0; break;
        }
        if (CHIHIRO_LOG) {
            printf("[%07lld] chihiro lpc reg read  [0x%08X] -> 0x%08X\n", TS_MS,
                   s->lpc_reg_addr, (unsigned)r);
        }
        return r;
    case SEGA_FIRMWARE_VERSION:
        /* Port 0x401E: DIMM base address low word.
         * Combined with port 0x4020 (high word) by 0x3DF40 to compute
         * FC800/FC801 file offsets: edi = (4020<<16)|401E; FC800 = edi - 0x1000000 + 0x900000.
         * For correct offsets (0x900000/0x900200): edi must = 0x01000000.
         * → port 0x401E = 0x0000, port 0x4020 = 0x0100. */
        r = 0x0000;
        s->lpc_401e_reads++;
        break;
    case SEGA_XBAM_STRING_0:
        r = 0x0100;     /* DIMM base address high word (0x0100 << 16 = 0x01000000) */
        break;
    case SEGA_XBAM_STRING_1:
        r = 0x4258;     /* "BX" */
        break;
    case SEGA_XBAM_STRING_2:
        r = 0x4D41;     /* "MA" → full string reads as "XBAM" */
        break;
    case SEGA_CHIP_REVISION:
        r = 0x0001;  /* Mediaboard status: ready for mbcom negotiation */
        s->lpc_40f0_reads++;
        break;
    case SEGA_DIMM_SIZE:
        r = SEGA_DIMM_SIZE_512M;        /* 512MB DIMM (matches MAME default) */
        break;
    case 0x84:  /* Port 0x4084 — MbcomCommand session handle */
        r = 0x0000;
        s->lpc_4084_reads++;
        break;
    default:
        break;
    }

    if (CHIHIRO_LOG) {
        printf("[%07lld] chihiro lpc read  [0x%04x] -> 0x%04x (size=%d)\n", TS_MS,
               (unsigned)(addr + 0x4000), (unsigned)r, size);
    }
    return r;
}

static void chihiro_lpc_io_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{

    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);

    /* Log writes (suppress 0x4026 IRQ ACK spam) */
    if (addr != 0x26) {
        printf("[%07lld] chihiro lpc write [0x%04x] <- 0x%04x (size=%d)\n", TS_MS,
               (unsigned)(addr + 0x4000), (unsigned)val, size);
    }

    switch (addr) {
    case 0x00: /* Port 0x4000: write register data */
        s->lpc_reg_data = (uint32_t)val;
        printf("[%07lld] chihiro lpc reg write [0x%08X] <- 0x%08X\n", TS_MS,
               s->lpc_reg_addr, (unsigned)val);
        return;
    case 0x04: /* Port 0x4004: set register address */
        s->lpc_reg_addr = (uint32_t)val;
        return;
    case 0x08: /* Port 0x4008: command/clear */
        return;
    case SEGA_IRQ10_ACK:
        /* Clear IRQ10 — game writes here after handling baseboard IRQ */
        qemu_irq_lower(s->irq10);
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

/* mbcom state — defined here (before IRQ10 timer that uses them) */
static uint8_t chihiro_mbcom_response[512];
static uint8_t chihiro_mbcom_command[512];
static bool chihiro_mbcom_enabled = false;
static void chihiro_mbcom_process(void);

static void chihiro_irq10_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;

    if (s->eeprom_hack_applied) {
        qemu_irq_raise(s->irq10);
    }

    /* DMA emulation: scan game's mbcom TX slots for pending commands.
     * On real HW, the baseboard picks up commands via DMA and responds.
     * Without this, commands queued by the async mbcom path (boot data reader)
     * never reach FC801 because the mbcom tick is on the same blocked thread.
     *
     * Slot array at VA 0x89760, 16 slots, stride 0x40:
     *   byte[3] bit7 SET = command pending (SendCommand sets this)
     *   byte[3] bit7 CLEAR = free or response ready
     *   slot+0x20 = data area (32 bytes: type, flags, command, params)
     */
    if (chihiro_mbcom_enabled) {
        uint32_t slot_base_pa = chihiro_va_to_pa(0x89760);
        if (slot_base_pa != 0xFFFFFFFF) {
            /* TX scan: process any pending commands from the game */
            for (int i = 0; i < 16; i++) {
                uint32_t slot_pa = slot_base_pa + i * 0x40;
                uint8_t meta[4];
                cpu_physical_memory_read(slot_pa, meta, 4);

                if (!(meta[3] & 0x80)) continue;  /* bit7 CLEAR = not pending */

                /* Read command from data area */
                uint8_t tx_data[32];
                cpu_physical_memory_read(slot_pa + 0x20, tx_data, 32);
                if (tx_data[0] == 0 && tx_data[2] == 0 && tx_data[3] == 0)
                    continue;  /* empty slot */

                /* Process: copy TX data to command buffer, run handler */
                memcpy(chihiro_mbcom_command, tx_data, 32);
                chihiro_mbcom_process();

                /* Inject response to data area */
                cpu_physical_memory_write(slot_pa + 0x20,
                                          chihiro_mbcom_response, 32);

                /* Clear bit7 + set non-zero marker → PollReady returns 1 */
                uint16_t marker = chihiro_mbcom_response[0]
                                | (chihiro_mbcom_response[1] << 8);
                if (!marker) marker = 0x0001;
                marker &= 0x7FFF;  /* ensure bit15 clear */
                cpu_physical_memory_write(slot_pa + 2, &marker, 2);

                printf("[%07lld] Chihiro mbcom: TX scan slot %d → processed"
                       " (type=0x%02X cmd=0x%02X%02X)\n",
                       TS_MS, i, tx_data[0], tx_data[3], tx_data[2]);
            }

            /* v158: type=2 slot injection REMOVED.
             * ValidateType allocates types dynamically via GetMbcomType (0x3E0D0).
             * Our type=2 injection was wrong (game uses allocated type N, not 2)
             * and the non-zero word[+2] polluted GetMbcomType's empty-slot scan.
             * ValidateType at 0x3E4A0 sets word[+2]=1 → PollReady returns 1. */
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

    /* USB hotplug timers — staggered to simulate independent AN2131 boot.
     * QC at T+1500ms, SC at T+1700ms.
     * Each triggers a separate RHSC IRQ so the kernel enumerates
     * each device independently through the full callback chain:
     * PortReset → GET_DESC(dev) → SET_ADDRESS → GET_DESC(cfg) → SET_CONFIG */
    s->usb_hotplug_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                         chihiro_usb_hotplug_qc_cb, s);
    timer_mod(s->usb_hotplug_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1500);

    s->usb_hotplug_sc_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                            chihiro_usb_hotplug_sc_cb, s);
    timer_mod(s->usb_hotplug_sc_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1700);

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

/* Process mbcom command and generate response
 * Aligned with MAME chihiro.cpp::baseboard_ide_event() */
static void chihiro_mbcom_process(void)
{
    const uint8_t *w = chihiro_mbcom_command;
    uint8_t *r = chihiro_mbcom_response;

    if (w[0] == 0 && w[1] == 0) return;  /* no command */

    uint16_t cmd_echo = w[0] | (w[1] << 8);
    uint16_t cmd_code = w[2] | (w[3] << 8);

    /* MAME-style response: echo cmd id + set 8001 marker in second word */
    r[0] = w[0];
    r[1] = w[1];
    r[2] = 0x01;
    r[3] = 0x80;
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
    case 0x0001: /* DIMM_SIZE — MAME: dword_write_le(r+4, 0x00f00000) */
        r[4] = 0x00; r[5] = 0x00; r[6] = 0xF0; r[7] = 0x00;
        break;
    case 0x0100: /* STATUS — phase=5 (game loading), completion=0
                  * Real HW: baseboard loads GD-ROM→DIMM, reports phase=5 during load.
                  * SEGABOOT gate at 0x2E0AF needs phase >= 5 to proceed to boot.id read. */
        r[4] = 5; r[5] = 0; r[6] = 0; r[7] = 0;
        r[8] = 0; r[9] = 0; r[10] = 0; r[11] = 0;  /* completion 0% */
        break;
    case 0x0101: /* FW_VER — MAME: 0x1234 (12.34) + 0x4567 */
        r[4] = 0x34; r[5] = 0x12; r[6] = 0x67; r[7] = 0x45;
        break;
    case 0x0102: /* SYSTEM_TYPE — MAME: 0 (retail, bit 16 = devel) */
        r[4] = 0; r[5] = 0; r[6] = 0; r[7] = 0;
        break;
    case 0x0103: /* SERIAL — MAME: "-abc-abc12345678" */
        memcpy(r + 4, "-abc-abc12345678", 16);
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
        /* v148: FC801 serves dual purpose in the wire protocol:
         *   - Slot-clear check (before sending): expects zeros = "ready"
         *   - Response read (after IRQ10): expects response data
         * Clear-on-read: return response once, then zeros for next slot-clear check. */
        memset(buffer, 0, 512);
        memcpy(buffer, chihiro_mbcom_response, 32);
        /* Clear response after delivery so next read sees "slot clear" */
        memset(chihiro_mbcom_response, 0, 32);
        return true;
    }
    return false;
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
        memcpy(chihiro_mbcom_command, buffer, 32);
        if (chihiro_mbcom_command[0] != 0 || chihiro_mbcom_command[1] != 0) {
            chihiro_mbcom_process();
            if (chihiro_irq10_global) {
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
