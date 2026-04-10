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
#include "system/address-spaces.h"
#include "chihiro.h"

/*
 * Chihiro Mediaboard LPC I/O
 *
 * The Chihiro baseboard exposes a set of I/O registers at 0x4000-0x40FF
 * on the LPC/ISA bus. These are used by SEGABOOT to detect the baseboard,
 * query firmware version, DIMM size, and board type.
 *
 * Register map (from MAME chihiro.cpp + CXBX MediaBoard.cpp):
 *   0x1E: Firmware version (16-bit, e.g. 0x0317 = v3.17)
 *   0x20: XBAM string byte 1-2 (0x00A0)
 *   0x22: XBAM string byte 3-4 (0x4258 = "BX")
 *   0x24: XBAM string byte 5-6 (0x4D41 = "MA")
 *   0xE0: IRQ10 acknowledge (write clears IRQ10)
 *   0xF0: Chip revision / board type (0x0000 = Type-1, 0x0100 = Type-3)
 *   0xF4: DIMM size (0=128M, 1=256M, 2=512M, 3=1024M)
 *
 * SEGABOOT checks for the "XBAM" string at 0x4020-0x4024 to confirm
 * the mediaboard is present. If absent, Chihiro boot path is skipped.
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
    bool error02_hack_applied;
    bool segaboot_hack_applied;
    bool checkbootid_hack_applied;

    /* Persistent re-patching */
    uint32_t error02_patch_addr;
    uint32_t segaboot_patch_addr;
    uint32_t checkbootid_patch_addr;
    int repatch_count;

    /* IRQ10 for baseboard → SEGABOOT communication */
    qemu_irq irq10;
    QEMUTimer *irq10_timer;
} ChihiroLPCState;

#define CHIHIRO_LPC_DEVICE(obj) \
    OBJECT_CHECK(ChihiroLPCState, (obj), "chihiro-lpc")

static uint64_t chihiro_lpc_io_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    uint64_t r = 0;

    switch (addr) {
    case SEGA_FIRMWARE_VERSION:
        r = 0x0317;     /* Firmware v3.17 */
        break;
    case SEGA_XBAM_STRING_0:
        r = 0x00A0;     /* XBAM identifier part 1 */
        break;
    case SEGA_XBAM_STRING_1:
        r = 0x4258;     /* "BX" */
        break;
    case SEGA_XBAM_STRING_2:
        r = 0x4D41;     /* "MA" → full string reads as "XBAM" */
        break;
    case SEGA_CHIP_REVISION:
        r = SEGA_CHIP_REVISION_TYPE1;   /* Type-1 baseboard */
        break;
    case SEGA_DIMM_SIZE:
        r = SEGA_DIMM_SIZE_512M;        /* 512MB DIMM (matches MAME default) */
        break;
    default:
        break;
    }

    if (CHIHIRO_LOG) {
        printf("chihiro lpc read  [0x%04x] -> 0x%04x (size=%d)\n",
               (unsigned)(addr + 0x4000), (unsigned)r, size);
    }
    return r;
}

static void chihiro_lpc_io_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    if (CHIHIRO_LOG) {
        printf("chihiro lpc write [0x%04x] = 0x%04x (size=%d)\n",
               (unsigned)(addr + 0x4000), (unsigned)val, size);
    }

    switch (addr) {
    case SEGA_IRQ10_ACK:
        /* Clear IRQ10 — SEGABOOT writes here after handling baseboard IRQ */
        {
            ChihiroLPCState *s = opaque;
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

/*
 * IRQ10 periodic pulse — signals SEGABOOT that a baseboard response is ready.
 * On real hardware, IRQ10 fires after each mbcom command is processed.
 * We pulse it periodically since we pre-fill the response sector.
 */
static void chihiro_irq10_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;

    if (s->eeprom_hack_applied) {
        qemu_irq_raise(s->irq10);
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
        /* Phase 1: Patch kernel EEPROM validation */
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
            printf("Chihiro: Applied EEPROM validation hack "
                   "(arcdkrnl @ 0x8003B744)\n");
        }
        /* Retry until kernel is decrypted */
        timer_mod(s->eeprom_hack_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
        return;
    }

    if (!s->error02_hack_applied || !s->segaboot_hack_applied ||
        !s->checkbootid_hack_applied) {
        /* Phase 2+3+4: Scan RAM for three SEGABOOT patterns.
         *
         * #1 Error 02 USB presence (CLogo::Update state machine):
         *    85 C0 75 0F 39 6B 10 75 0A C7 43 10 02
         *    Patch byte+2: 75→EB (JNE→JMP, skip USB AN2131QC check)
         *
         * #2 CheckErrors:  75 0E 68 B0 1F 02 00
         *    Patch byte+0: 75 0E→90 90 (NOP, always skip)
         *
         * #3 CheckBootId:  75 10 68 D0 1F 02 00
         *    Patch byte+0: 75 10→90 90 (NOP, always skip)
         */
        uint8_t pat_error02[] = { 0x85, 0xC0, 0x75, 0x0F, 0x39, 0x6B, 0x10,
                                  0x75, 0x0A, 0xC7, 0x43, 0x10, 0x02 };
        uint8_t pat_errors[]  = { 0x75, 0x0E, 0x68, 0xB0, 0x1F, 0x02, 0x00 };
        uint8_t pat_bootid[]  = { 0x75, 0x10, 0x68, 0xD0, 0x1F, 0x02, 0x00 };

        uint8_t *block = g_malloc(0x400000);
        for (uint32_t base = 0; base < 0x8000000; base += 0x400000) {
            address_space_read(&address_space_memory, base,
                               MEMTXATTRS_UNSPECIFIED, block, 0x400000);
            for (uint32_t off = 0; off < 0x400000 - 13; off++) {
                if (!s->error02_hack_applied &&
                    memcmp(block + off, pat_error02, 13) == 0) {
                    s->error02_patch_addr = base + off + 2; /* the JNE byte */
                    uint8_t jmp = 0xEB;
                    address_space_write(&address_space_memory,
                                        s->error02_patch_addr,
                                        MEMTXATTRS_UNSPECIFIED, &jmp, 1);
                    s->error02_hack_applied = true;
                    s->repatch_count = 0;
                    printf("Chihiro: Applied Error 02 USB bypass "
                           "(phys @ 0x%08X)\n", s->error02_patch_addr);
                }
                if (!s->segaboot_hack_applied &&
                    memcmp(block + off, pat_errors, 7) == 0) {
                    s->segaboot_patch_addr = base + off;
                    uint8_t nop2[] = { 0x90, 0x90 };
                    address_space_write(&address_space_memory,
                                        s->segaboot_patch_addr,
                                        MEMTXATTRS_UNSPECIFIED, nop2, 2);
                    s->segaboot_hack_applied = true;
                    printf("Chihiro: Applied CheckErrors skip hack "
                           "(phys @ 0x%08X)\n", s->segaboot_patch_addr);
                }
                if (!s->checkbootid_hack_applied &&
                    memcmp(block + off, pat_bootid, 7) == 0) {
                    s->checkbootid_patch_addr = base + off;
                    uint8_t nop2[] = { 0x90, 0x90 };
                    address_space_write(&address_space_memory,
                                        s->checkbootid_patch_addr,
                                        MEMTXATTRS_UNSPECIFIED, nop2, 2);
                    s->checkbootid_hack_applied = true;
                    printf("Chihiro: Applied CheckBootId skip hack "
                           "(phys @ 0x%08X)\n", s->checkbootid_patch_addr);
                }
            }
            if (s->error02_hack_applied && s->segaboot_hack_applied &&
                s->checkbootid_hack_applied) {
                break;
            }
        }
        g_free(block);

        if (!s->error02_hack_applied || !s->segaboot_hack_applied ||
            !s->checkbootid_hack_applied) {
            timer_mod(s->eeprom_hack_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
        } else {
            /* All found — start re-patch loop */
            timer_mod(s->eeprom_hack_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
        }
        return;
    }

    /* Re-patch loop — keep hammering for 2 seconds */
    if (s->repatch_count < 200) {
        uint8_t jmp = 0xEB;
        uint8_t nop2[] = { 0x90, 0x90 };
        address_space_write(&address_space_memory, s->error02_patch_addr,
                            MEMTXATTRS_UNSPECIFIED, &jmp, 1);
        address_space_write(&address_space_memory, s->segaboot_patch_addr,
                            MEMTXATTRS_UNSPECIFIED, nop2, 2);
        address_space_write(&address_space_memory, s->checkbootid_patch_addr,
                            MEMTXATTRS_UNSPECIFIED, nop2, 2);
        s->repatch_count++;
        timer_mod(s->eeprom_hack_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }
}

static void chihiro_lpc_realize(DeviceState *dev, Error **errp)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(dev);
    ISADevice *isa = ISA_DEVICE(dev);

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
    s->error02_hack_applied = false;
    s->segaboot_hack_applied = false;
    s->checkbootid_hack_applied = false;
    s->error02_patch_addr = 0;
    s->segaboot_patch_addr = 0;
    s->checkbootid_patch_addr = 0;
    s->repatch_count = 0;
    s->eeprom_hack_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                         chihiro_eeprom_hack_cb, s);
    timer_mod(s->eeprom_hack_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);

    /* Initialize IRQ10 for baseboard communication */
    s->irq10 = isa_get_irq(isa, 10);
    s->irq10_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                   chihiro_irq10_timer_cb, s);
    timer_mod(s->irq10_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);

    /* Initialize mbcom protocol handler */
    chihiro_mbcom_init();

    printf("Chihiro: Mediaboard LPC I/O initialized at 0x4000-0x40FF\n");
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

static uint8_t chihiro_mbcom_response[512];
static bool chihiro_mbcom_enabled = false;

void chihiro_mbcom_init(void)
{
    memset(chihiro_mbcom_response, 0, sizeof(chihiro_mbcom_response));
    chihiro_mbcom_enabled = true;
    printf("Chihiro: mbcom protocol handler initialized\n");
}

/* Process an mbcom command and generate response */
static void chihiro_mbcom_process(const uint8_t *cmd_data)
{
    uint16_t cmd_echo = cmd_data[0] | (cmd_data[1] << 8);
    uint16_t cmd_code = cmd_data[2] | (cmd_data[3] << 8);

    memset(chihiro_mbcom_response, 0, sizeof(chihiro_mbcom_response));

    /* Echo command ID + set response marker 0x8001 */
    chihiro_mbcom_response[0] = cmd_data[0];
    chihiro_mbcom_response[1] = cmd_data[1];
    chihiro_mbcom_response[2] = 0x01;
    chihiro_mbcom_response[3] = 0x80;

    printf("Chihiro mbcom: cmd=0x%04X echo=0x%04X\n", cmd_code, cmd_echo);

    switch (cmd_code) {
    case 0x0001: /* DIMM_SIZE */
        chihiro_mbcom_response[4] = 0x00;
        chihiro_mbcom_response[5] = 0x00;
        chihiro_mbcom_response[6] = 0xF0;
        chihiro_mbcom_response[7] = 0x00;
        break;
    case 0x0100: /* STATUS → READY(5), completion 0% */
        chihiro_mbcom_response[4] = 5;
        chihiro_mbcom_response[5] = 0;
        chihiro_mbcom_response[6] = 0;
        chihiro_mbcom_response[7] = 0;
        chihiro_mbcom_response[8] = 0;
        chihiro_mbcom_response[9] = 0;
        chihiro_mbcom_response[10] = 0;
        chihiro_mbcom_response[11] = 0;
        break;
    case 0x0101: /* FIRMWARE_VERSION → 12.34 */
        chihiro_mbcom_response[4] = 0x34;
        chihiro_mbcom_response[5] = 0x12;
        chihiro_mbcom_response[6] = 0x67;
        chihiro_mbcom_response[7] = 0x45;
        break;
    case 0x0102: /* SYSTEM_TYPE → 0 (retail) */
        chihiro_mbcom_response[4] = 0;
        chihiro_mbcom_response[5] = 0;
        chihiro_mbcom_response[6] = 0;
        chihiro_mbcom_response[7] = 0;
        break;
    case 0x0103: /* SERIAL_NUMBER */
        memcpy(chihiro_mbcom_response + 4, "-abc-abc12345678", 16);
        break;
    default:
        printf("Chihiro mbcom: unknown command 0x%04X\n", cmd_code);
        break;
    }
}

/*
 * Called from IDE DMA read path. Returns true if the sector was handled
 * (mbcom response), false for normal disk read.
 */
bool chihiro_ide_read_sector(uint32_t lba, void *buffer)
{
    if (!chihiro_mbcom_enabled) return false;

    if (lba == CHIHIRO_MBCOM_RESPONSE) {
        memcpy(buffer, chihiro_mbcom_response, 512);
        printf("Chihiro mbcom: read response @ LBA 0x%X\n", lba);
        return true;
    }
    if (lba == CHIHIRO_MBCOM_COMMAND) {
        memset(buffer, 0, 512);
        return true;
    }
    return false;
}

/*
 * Called from IDE DMA write path. Returns true if handled.
 */
bool chihiro_ide_write_sector(uint32_t lba, const void *buffer)
{
    if (!chihiro_mbcom_enabled) return false;

    if (lba == CHIHIRO_MBCOM_COMMAND) {
        const uint8_t *cmd = (const uint8_t *)buffer;
        if (cmd[0] != 0 || cmd[1] != 0) {
            chihiro_mbcom_process(cmd);
            /* TODO: trigger IRQ10 via LPC */
        }
        printf("Chihiro mbcom: write command @ LBA 0x%X\n", lba);
        return true;
    }
    if (lba == CHIHIRO_MBCOM_RESPONSE) {
        memcpy(chihiro_mbcom_response, buffer, 512);
        return true;
    }
    return false;
}
