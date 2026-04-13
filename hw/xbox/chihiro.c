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
#include "system/block-backend.h"
#include "chihiro.h"
#include "system/blockdev.h"

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
    uint32_t lpc_reg_addr;        /* MediaBoard register address (set via port 0x4004) */
    uint32_t lpc_reg_data;        /* MediaBoard register data (read via port 0x4000) */

    /* IRQ10 for baseboard → SEGABOOT communication */
    qemu_irq irq10;
    QEMUTimer *irq10_timer;
} ChihiroLPCState;

#define CHIHIRO_LPC_DEVICE(obj) \
    OBJECT_CHECK(ChihiroLPCState, (obj), "chihiro-lpc")

static bool chihiro_active;
static ChihiroLPCState *chihiro_lpc_global;

/* Called from SMC handler when kernel writes SMC_REG_POWER (QuickReboot).
 * Blocks qemu_system_reset_request for Chihiro — the kernel handles
 * soft-reset internally via HalReturnToFirmware(2).
 * TODO: once SEGABOOT passes naturally, write LaunchDataPage here. */
bool chihiro_intercept_reset(void)
{
    if (chihiro_active) {
        printf("Chihiro: QuickReboot intercepted (SMC cmd=0x02) — blocking QEMU reset\n");
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
            printf("chihiro lpc reg read  [0x%08X] -> 0x%08X\n",
                   s->lpc_reg_addr, (unsigned)r);
        }
        return r;
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
        r = SEGA_CHIP_REVISION_TYPE3;  /* ASIC production baseboard (same as MAME) */
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

    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(opaque);

    /* Log ALL writes */
    printf("chihiro lpc write [0x%04x] <- 0x%04x (size=%d)\n",
           (unsigned)(addr + 0x4000), (unsigned)val, size);

    switch (addr) {
    case 0x00: /* Port 0x4000: write register data */
        s->lpc_reg_data = (uint32_t)val;
        printf("chihiro lpc reg write [0x%08X] <- 0x%08X\n",
               s->lpc_reg_addr, (unsigned)val);
        return;
    case 0x04: /* Port 0x4004: set register address */
        s->lpc_reg_addr = (uint32_t)val;
        return;
    case 0x08: /* Port 0x4008: command/clear */
        return;
    case SEGA_IRQ10_ACK:
        /* Clear IRQ10 — SEGABOOT writes here after handling baseboard IRQ */
        printf("chihiro IRQ10 LOWER (ack from SEGABOOT)\n");
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
            printf("Chihiro: Applied EEPROM validation hack "
                   "(arcdkrnl @ 0x8003B744)\n");
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
    case 0x0100: /* STATUS → READY(5), completion 100% */
        chihiro_mbcom_response[4] = 5;
        chihiro_mbcom_response[5] = 0;
        chihiro_mbcom_response[6] = 0;
        chihiro_mbcom_response[7] = 0;
        chihiro_mbcom_response[8] = 100;  /* completion % */
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
        const uint8_t *d = (const uint8_t *)buffer;
        printf("Chihiro mbcom: read response @ LBA 0x%X data=%02X%02X%02X%02X "
               "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
               lba, d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],
               d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15]);
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
        printf("Chihiro mbcom: write command @ LBA 0x%X data=%02X%02X%02X%02X "
               "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
               lba, cmd[0],cmd[1],cmd[2],cmd[3],cmd[4],cmd[5],cmd[6],cmd[7],
               cmd[8],cmd[9],cmd[10],cmd[11],cmd[12],cmd[13],cmd[14],cmd[15]);
        if (cmd[0] != 0 || cmd[1] != 0) {
            chihiro_mbcom_process(cmd);
        }
        return true;
    }
    if (lba == CHIHIRO_MBCOM_RESPONSE) {
        memcpy(chihiro_mbcom_response, buffer, 512);
        return true;
    }
    return false;
}

/*
 * Called from ide_dma_cb() when a DMA WRITE completes on IDE unit 1.
 * Checks if the write was to the mbcom command sector (LBA 0xFC801).
 * If so, reads back the command, processes it, and writes the response
 * to the response sector (LBA 0xFC800). The IRQ10 periodic timer
 * signals SEGABOOT that the response is ready.
 */
void chihiro_ide_dma_write_done(BlockBackend *blk, int64_t sector_num)
{
    if (!chihiro_mbcom_enabled) return;

    /* sector_num is the sector AFTER the last written sector.
     * For a 1-sector write to LBA 0xFC801, sector_num = 0xFC802.
     * Check if the write range included the command sector. */
    int64_t cmd_lba = CHIHIRO_MBCOM_COMMAND;
    int64_t resp_lba = CHIHIRO_MBCOM_RESPONSE;

    /* Check range: the write could span multiple sectors */
    if (sector_num <= cmd_lba) return;
    if (sector_num > cmd_lba + 256) return; /* sanity */

    /* Read back the command sector from the block device */
    uint8_t cmd_data[512];
    int ret = blk_pread(blk, cmd_lba * 512, 512, cmd_data, 0);
    if (ret < 0) {
        printf("Chihiro mbcom: failed to read command sector (ret=%d)\n", ret);
        return;
    }

    /* Check if it's a valid command (first two bytes non-zero) */
    if (cmd_data[0] == 0 && cmd_data[1] == 0) return;

    /* Process the mbcom command */
    chihiro_mbcom_process(cmd_data);

    /* Write the response to the response sector */
    ret = blk_pwrite(blk, resp_lba * 512, 512, chihiro_mbcom_response, 0);
    if (ret < 0) {
        printf("Chihiro mbcom: failed to write response sector (ret=%d)\n", ret);
        return;
    }

    /* Clear the command sector so we don't re-process it */
    memset(cmd_data, 0, 512);
    blk_pwrite(blk, cmd_lba * 512, 512, cmd_data, 0);

    printf("Chihiro mbcom: processed command, response written to LBA 0x%llX\n",
           (long long)resp_lba);

    /* Signal SEGABOOT that response is ready */
    if (chihiro_irq10_global) {
        printf("chihiro IRQ10 RAISE (mbcom response ready)\n");
        qemu_irq_raise(chihiro_irq10_global);
    }
}
