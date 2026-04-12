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
#include "cpu.h"
#include "system/cpus.h"
#include "system/hw_accel.h"
#include "exec/cputlb.h"
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
    bool error02_hack_applied;
    bool segaboot_hack_applied;
    bool checkbootid_hack_applied;
    bool timeout_hack_applied;
    bool fwskip_hack_applied;
    bool fwret_hack_applied;     /* hack 7: OpenUSBDevice return 1 */
    bool usbenum_hack_applied;   /* hack 8: USBEnumerate return 0 */
    bool bbready_hack_applied;   /* hack 9: IsBaseboardReady = TRUE */
    bool systype_hack_applied;   /* hack 10: MbcomQuerySysType bypass */
    bool nuclear_hack_applied;   /* hack 11: ALL errors fall-through */
    bool launchinfo_hack_applied; /* hack 12: GetLaunchInfo bypass */
    bool drivepath_hack_applied;  /* hack 13: d:%s → mbfs:%s */
    bool xbepath_hack_applied;    /* hack 14: inject XBE path */
    bool vtable_hack_applied;     /* hack 15: NOP vtable cleanup call */
    bool appcreate_hack_applied;  /* hack 16: theApp.Create bypass */
    uint32_t appcreate_patch_addr;
    bool xbeloader_hack_applied;  /* hack 17: XBE loader trampoline */
    uint32_t lpc_reg_addr;        /* MediaBoard register address (set via port 0x4004) */
    uint32_t lpc_reg_data;        /* MediaBoard register data (read via port 0x4000) */

    /* Persistent re-patching */
    uint32_t error02_patch_addr;
    uint32_t segaboot_patch_addr;
    uint32_t checkbootid_patch_addr;
    uint32_t timeout_patch_addr;
    uint32_t systype_patch_addr;
    uint32_t nuclear_patch_addr;
    uint32_t launchinfo_patch_addr;
    int repatch_count;

    /* IRQ10 for baseboard → SEGABOOT communication */
    qemu_irq irq10;
    QEMUTimer *irq10_timer;
} ChihiroLPCState;

#define CHIHIRO_LPC_DEVICE(obj) \
    OBJECT_CHECK(ChihiroLPCState, (obj), "chihiro-lpc")

/* Forward declarations for XBE loader */
static bool chihiro_active;
static bool chihiro_xbe_loaded;
static void chihiro_load_game_xbe(void);

/* Called from SMC handler when QuickReboot is triggered */
bool chihiro_intercept_reset(void)
{
    if (chihiro_active && !chihiro_xbe_loaded) {
        printf("Chihiro: Intercepting QuickReboot — loading game XBE\n");
        chihiro_load_game_xbe();
        return chihiro_xbe_loaded;
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
        r = 0x0001;  /* Type-1 transformed = 0x21, matches table */   /* Type-1 baseboard */
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

    switch (addr) {
    case 0x00: /* Port 0x4000: write register data */
        s->lpc_reg_data = (uint32_t)val;
        if (CHIHIRO_LOG) {
            printf("chihiro lpc reg write [0x%08X] <- 0x%08X\n",
                   s->lpc_reg_addr, (unsigned)val);
        }
        return;
    case 0x04: /* Port 0x4004: set register address */
        s->lpc_reg_addr = (uint32_t)val;
        return;
    case 0x08: /* Port 0x4008: command/clear */
        return;
    case 0xE0: /* Port 0x40E0: IRQ ack */
        return;
    }

    /* Port 0x40F8: Function trace — logs game init call sequence */
    if (addr == 0xF8 && chihiro_xbe_loaded) {
        static const char *trace_names[] = {
            "???",                  /* 0 */
            "crt_version_check",    /* 1 - VA 0xB973C */
            "XapiInitProcess",      /* 2 - VA 0xB8B38 */
            "_initterm",            /* 3 - VA 0xB96BB */
            "main",                 /* 4 - VA 0xB9713 */
            "game_init_1",          /* 5 - VA 0x84B90 */
            "game_init_2",          /* 6 - VA 0xFCCB0 */
            "game_init_3",          /* 7 - VA 0xB7D20 */
        };
        uint32_t id = (uint32_t)val;
        const char *name = (id < 8) ? trace_names[id] : "unknown";
        static int trace_seq = 0;
        printf("Chihiro TRACE[%02d]: %s (id=%d)\n", ++trace_seq, name, id);
        return;
    }

    /* Port 0x40FC: File I/O instrumentation — logs NtCreateFile/NtReadFile/NtClose/NtOpenFile */
    if (addr == 0xFC && chihiro_xbe_loaded) {
        uint32_t ordinal = (uint32_t)val;
        CPUState *log_cs = first_cpu;
        if (log_cs) {
            cpu_synchronize_state(log_cs);
            X86CPU *log_cpu = X86_CPU(log_cs);
            CPUX86State *log_env = &log_cpu->env;
            uint32_t esp = (uint32_t)log_env->regs[R_ESP];

            if (ordinal == 190) {
                /* NtCreateFile(ord 190): ESP+12=pObjAttrs */
                uint32_t obj_attrs_ptr = 0;
                cpu_memory_rw_debug(log_cs, esp + 12, (uint8_t *)&obj_attrs_ptr, 4, false);
                if (obj_attrs_ptr) {
                    uint32_t obj_name_ptr = 0, root_dir = 0;
                    cpu_memory_rw_debug(log_cs, obj_attrs_ptr, (uint8_t *)&root_dir, 4, false);
                    cpu_memory_rw_debug(log_cs, obj_attrs_ptr + 4, (uint8_t *)&obj_name_ptr, 4, false);
                    if (obj_name_ptr) {
                        uint16_t str_len = 0;
                        uint32_t str_buf = 0;
                        cpu_memory_rw_debug(log_cs, obj_name_ptr, (uint8_t *)&str_len, 2, false);
                        cpu_memory_rw_debug(log_cs, obj_name_ptr + 4, (uint8_t *)&str_buf, 4, false);
                        if (str_buf && str_len > 0 && str_len < 512) {
                            char path[256] = {0};
                            uint16_t read_len = (str_len > 255) ? 255 : str_len;
                            cpu_memory_rw_debug(log_cs, str_buf, (uint8_t *)path, read_len, false);
                            uint32_t access = 0, create_disp = 0;
                            cpu_memory_rw_debug(log_cs, esp + 8, (uint8_t *)&access, 4, false);
                            cpu_memory_rw_debug(log_cs, esp + 32, (uint8_t *)&create_disp, 4, false);
                            printf("Chihiro FILE: NtCreateFile(\"%s\") access=0x%X disp=0x%X root=0x%X\n",
                                   path, access, create_disp, root_dir);
                        }
                    }
                }
            } else if (ordinal == 202) {
                /* NtOpenFile(ord 202): ESP+12=pObjAttrs */
                uint32_t obj_attrs_ptr = 0;
                cpu_memory_rw_debug(log_cs, esp + 12, (uint8_t *)&obj_attrs_ptr, 4, false);
                if (obj_attrs_ptr) {
                    uint32_t obj_name_ptr = 0, root_dir = 0;
                    cpu_memory_rw_debug(log_cs, obj_attrs_ptr, (uint8_t *)&root_dir, 4, false);
                    cpu_memory_rw_debug(log_cs, obj_attrs_ptr + 4, (uint8_t *)&obj_name_ptr, 4, false);
                    if (obj_name_ptr) {
                        uint16_t str_len = 0;
                        uint32_t str_buf = 0;
                        cpu_memory_rw_debug(log_cs, obj_name_ptr, (uint8_t *)&str_len, 2, false);
                        cpu_memory_rw_debug(log_cs, obj_name_ptr + 4, (uint8_t *)&str_buf, 4, false);
                        if (str_buf && str_len > 0 && str_len < 512) {
                            char path[256] = {0};
                            uint16_t read_len = (str_len > 255) ? 255 : str_len;
                            cpu_memory_rw_debug(log_cs, str_buf, (uint8_t *)path, read_len, false);
                            uint32_t access = 0;
                            cpu_memory_rw_debug(log_cs, esp + 8, (uint8_t *)&access, 4, false);
                            printf("Chihiro FILE: NtOpenFile(\"%s\") access=0x%X root=0x%X\n",
                                   path, access, root_dir);
                        }
                    }
                }
            } else if (ordinal == 219) {
                /* NtReadFile(ord 219): ESP+4=Handle, ESP+24=Buffer, ESP+28=Len, ESP+32=pOffset */
                uint32_t handle = 0, length = 0, buf_ptr = 0;
                cpu_memory_rw_debug(log_cs, esp + 4, (uint8_t *)&handle, 4, false);
                cpu_memory_rw_debug(log_cs, esp + 24, (uint8_t *)&buf_ptr, 4, false);
                cpu_memory_rw_debug(log_cs, esp + 28, (uint8_t *)&length, 4, false);
                uint64_t offset = 0;
                uint32_t off_ptr = 0;
                cpu_memory_rw_debug(log_cs, esp + 32, (uint8_t *)&off_ptr, 4, false);
                if (off_ptr)
                    cpu_memory_rw_debug(log_cs, off_ptr, (uint8_t *)&offset, 8, false);
                printf("Chihiro FILE: NtReadFile(handle=0x%X buf=0x%X len=0x%X off=0x%llX)\n",
                       handle, buf_ptr, length, (unsigned long long)offset);
            } else if (ordinal == 187) {
                /* NtClose(ord 187): ESP+4=Handle */
                uint32_t handle = 0;
                cpu_memory_rw_debug(log_cs, esp + 4, (uint8_t *)&handle, 4, false);
                printf("Chihiro FILE: NtClose(handle=0x%X)\n", handle);
            } else if (ordinal == 0xBC) {
                /* KeBugCheckEx detour: pushad adds 32 bytes to stack.
                 * Original ESP+0=retaddr, +4=code, +8=p1, +12=p2, +16=p3, +20=p4 */
                uint32_t code = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, ret = 0;
                cpu_memory_rw_debug(log_cs, esp + 32,     (uint8_t *)&ret,  4, false);
                cpu_memory_rw_debug(log_cs, esp + 32 + 4, (uint8_t *)&code, 4, false);
                cpu_memory_rw_debug(log_cs, esp + 32 + 8, (uint8_t *)&p1,   4, false);
                cpu_memory_rw_debug(log_cs, esp + 32 + 12,(uint8_t *)&p2,   4, false);
                cpu_memory_rw_debug(log_cs, esp + 32 + 16,(uint8_t *)&p3,   4, false);
                cpu_memory_rw_debug(log_cs, esp + 32 + 20,(uint8_t *)&p4,   4, false);
                printf("Chihiro BUGCHECK: KeBugCheckEx(0x%X, 0x%X, 0x%X, 0x%X, 0x%X) from 0x%X\n",
                       code, p1, p2, p3, p4, ret);
            }
        }
        return;
    }

    /* Port 0x40FE: XBE loader trigger from SEGABOOT trampoline */
    if (addr == 0xFE && !chihiro_xbe_loaded) {
        printf("Chihiro: XBE load triggered by trampoline OUT 0x40FE\n");
        chihiro_load_game_xbe();
        return;
    }

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

/* Global IRQ10 reference for mbcom DMA write trigger */
static qemu_irq chihiro_irq10_global = NULL;

/*
 * IRQ10 periodic pulse — signals SEGABOOT that a baseboard response is ready.
 * On real hardware, IRQ10 fires after each mbcom command is processed.
 * We pulse it periodically since we pre-fill the response sector.
 */

/*
 * Chihiro XBE Loader — loads game XBE directly into guest RAM
 * Bypasses SEGABOOT's broken XBE loading path entirely.
 * Reads from baseboard.img FATX, parses XBE, maps sections.
 */

#define XBE_MAGIC       0x48454258  /* "XBEH" */
#define XBE_ENTRY_KEY   0x94859D4B  /* debug XOR key */
#define FATX_MAGIC      0x58544146  /* "FATX" */


/* Chihiro kernel (arcdkrnl) export table — 158 ordinals
 * Extracted from SEGABOOT's patched thunk table at PA 0x5C000 */
static const uint32_t chihiro_kernel_exports[380] = {
    [  1] = 0x8002EFC8, [  2] = 0x8002F826, [  3] = 0x8002FF1D,
    [  4] = 0x8002EFCE, [  8] = 0x800220B4, [ 14] = 0x800132A4,
    [ 15] = 0x80012F71, [ 16] = 0x800108AC, [ 17] = 0x80013128,
    [ 22] = 0x800108C8, [ 24] = 0x80014302, [ 29] = 0x8001448C,
    [ 23] = 0x8001415F,
    [ 30] = 0x800108F4, [ 31] = 0x8001091C, [ 40] = 0x8003B370,
    [ 41] = 0x8003B398, [ 42] = 0x8003B378, [ 44] = 0x800153A8,
    [ 46] = 0x800154DC, [ 47] = 0x80015555, [ 49] = 0x800155FD,
    [ 67] = 0x80016182, [ 69] = 0x800161FB, [ 95] = 0x800197F0,
    [ 97] = 0x8001982D, [ 98] = 0x80019930, [ 99] = 0x80019BD7,
    [100] = 0x800199B0, [107] = 0x8001A748, [108] = 0x8001A7FF,
    [109] = 0x80019A9B, [112] = 0x8001ADD7, [113] = 0x80019800,
    [119] = 0x8001A787, [124] = 0x800193FB, [125] = 0x8001A314,
    [126] = 0x80015678, [127] = 0x800156B4, [128] = 0x8001A32C,
    [129] = 0x80013ED4, [132] = 0x8001ADFB, [137] = 0x8001A766,
    [139] = 0x8001BBC4,
    [142] = 0x80019470,
    [143] = 0x80019487, [144] = 0x80019555, [145] = 0x8001A8DC,
    [149] = 0x80019916, [150] = 0x8001985C, [151] = 0x80015700,
    [153] = 0x8001BBC4,
    [156] = 0x8003A95C, [157] = 0x80010C78, [158] = 0x80019DA8,
    [159] = 0x8001A0A9, [160] = 0x80013EC4, [161] = 0x80013EF4,
    [164] = 0x8003B3D8, [165] = 0x8001E6A0, [166] = 0x8001DFF2,
    [167] = 0x8001EBB2, [168] = 0x8001E5E7, [171] = 0x8001E6B4,
    [172] = 0x8001EBD0, [173] = 0x8001DFA4, [175] = 0x8001E36D,
    [176] = 0x8001E3FC, [178] = 0x8001E1F1, [179] = 0x8001E86B,
    [180] = 0x8001E1B4, [181] = 0x8001E8CC, [182] = 0x8001EB2F,
    [184] = 0x8001F261, [185] = 0x800138BD, [186] = 0x800132B5,
    [187] = 0x80020A68, [189] = 0x800132E6, [190] = 0x80016C9C,
    [191] = 0x80016ABA, [192] = 0x800134B5, [193] = 0x80013706,
    [194] = 0x80013854, [196] = 0x80016ED0, [197] = 0x80020AF0,
    [198] = 0x80016DF3, [199] = 0x8001F695, [200] = 0x80016EFC,
    [202] = 0x80016CC5, [203] = 0x8002117B, [205] = 0x80013351,
    [206] = 0x80021B4A, [211] = 0x8001706F, [215] = 0x80021192,
    [207] = 0x8001B504,
    [217] = 0x8001EEDA, [218] = 0x8001765E, [219] = 0x800177AA,
    [220] = 0x80017976, [221] = 0x800134EF, [222] = 0x80013797,
    [223] = 0x80016B36, [224] = 0x80021BC4, [225] = 0x800133D0,
    [226] = 0x800172AA, [227] = 0x80016C5C, [228] = 0x80013C34,
    [229] = 0x80013AC6, [230] = 0x80021217, [231] = 0x80021BFF,
    [232] = 0x80016DC5, [233] = 0x80021502, [234] = 0x8002132C,
    [235] = 0x800213A1, [236] = 0x80017B6B, [237] = 0x80017D4D,
    [238] = 0x8001B2B3, [243] = 0x8002096D, [246] = 0x800208F6,
    [250] = 0x80020868, [252] = 0x8002CEAC, [253] = 0x8002CD05,
    [255] = 0x80021CCE, [258] = 0x80021F38, [259] = 0x80010DD4,
    [260] = 0x8002214D, [268] = 0x80023370, [269] = 0x800233C0,
    [277] = 0x800234F0, [279] = 0x80022A4C, [285] = 0x80023430,
    [286] = 0x800224DB, [289] = 0x800237FC, [290] = 0x80023828,
    [291] = 0x80023857, [294] = 0x800235A0, [301] = 0x80023A23,
    [302] = 0x80023AFC, [304] = 0x80023D9C, [305] = 0x80023C66,
    [306] = 0x800235D8, [308] = 0x800221EB, [312] = 0x80024022,
    [322] = 0x8003B1D0, [323] = 0x8003B088, [324] = 0x800109E8,
    [327] = 0x8002E0FE, [328] = 0x8002E16D, [335] = 0x8002EE50,
    [336] = 0x8002EE56, [337] = 0x8002EE5C, [338] = 0x8002EE62,
    [339] = 0x8002EE68, [340] = 0x8002EE6E, [344] = 0x8002EE8A,
    [345] = 0x8002EE90, [346] = 0x8002EE9A, [347] = 0x8002EEA0,
    [349] = 0x8002EEB0, [353] = 0x8003B1A8, [355] = 0x800126E0,
    [356] = 0x8003B06C, [360] = 0x80015195,
};

static int  chihiro_xbe_countdown = -1;

static void chihiro_load_game_xbe(void)
{
    printf("Chihiro: === XBE LOADER START ===\n");

    /* Get baseboard IDE block backend (drive index 1) */
    DriveInfo *dinfo = drive_get_by_index(IF_IDE, 1);
    if (!dinfo) {
        printf("Chihiro XBE: No baseboard drive found\n");
        return;
    }
    BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
    if (!blk) {
        printf("Chihiro XBE: No block backend for baseboard\n");
        return;
    }
    printf("Chihiro XBE: Got baseboard block backend\n");

    /* Step 1: Find hod3xb.xbe in FATX root directory */
    uint8_t sector[512];

    /* Read FATX superblock (LBA 0) */
    if (blk_pread(blk, 0, 512, sector, 0) < 0) {
        printf("Chihiro XBE: Failed to read FATX superblock\n");
        return;
    }
    uint32_t fatx_magic = *(uint32_t *)sector;
    if (fatx_magic != FATX_MAGIC) {
        printf("Chihiro XBE: Bad FATX magic: 0x%08X\n", fatx_magic);
        return;
    }
    uint32_t spc = *(uint32_t *)(sector + 8);  /* sectors per cluster */
    uint32_t root_cluster = *(uint32_t *)(sector + 12);
    printf("Chihiro XBE: FATX OK, spc=%u, root_cluster=%u\n", spc, root_cluster);

    /* Calculate data area start */
    /* FAT at 0x1000, estimate FAT size, data starts after */
    uint32_t partition_sectors = 0xF8000;  /* 496MB */
    uint32_t cluster_size = spc * 512;
    uint32_t est_clusters = (partition_sectors * 512) / cluster_size;
    uint32_t fat_entry_size = (est_clusters < 65525) ? 2 : 4;
    uint32_t fat_bytes = est_clusters * fat_entry_size;
    uint32_t fat_aligned = ((fat_bytes + cluster_size - 1) / cluster_size) * cluster_size;
    uint32_t data_offset = 0x1000 + fat_aligned;  /* byte offset */
    uint32_t data_lba = data_offset / 512;
    printf("Chihiro XBE: data_lba=0x%X, fat_entry_size=%u\n", data_lba, fat_entry_size);

    /* Read root directory (cluster 1 = first data cluster) */
    uint32_t root_lba = data_lba;  /* cluster 1 starts at data_lba */
    printf("Chihiro XBE: Reading root dir at LBA 0x%X\n", root_lba);

    uint32_t xbe_cluster = 0, xbe_size = 0;
    for (uint32_t s = 0; s < spc; s++) {
        if (blk_pread(blk, (root_lba + s) * 512, 512, sector, 0) < 0) break;
        for (int e = 0; e < 512; e += 64) {
            uint8_t namelen = sector[e];
            if (namelen == 0xFF || namelen == 0x00) continue;
            if (namelen == 0xE5) continue;  /* deleted */
            if (namelen > 42) continue;
            char name[43] = {0};
            memcpy(name, sector + e + 2, namelen);
            uint32_t clust = *(uint32_t *)(sector + e + 0x2C);
            uint32_t size = *(uint32_t *)(sector + e + 0x30);
            if (strcasecmp(name, "hod3xb.xbe") == 0) {
                xbe_cluster = clust;
                xbe_size = size;
                printf("Chihiro XBE: Found '%s' cluster=%u size=%u\n",
                       name, clust, size);
            }
        }
    }

    if (xbe_cluster == 0) {
        printf("Chihiro XBE: hod3xb.xbe NOT FOUND in root directory!\n");
        return;
    }

    /* Step 2: Read XBE file from FATX */
    uint32_t xbe_lba = data_lba + (xbe_cluster - 1) * spc;
    uint32_t xbe_sectors = (xbe_size + 511) / 512;
    uint8_t *xbe_data = g_malloc(xbe_size);
    printf("Chihiro XBE: Reading %u bytes from LBA 0x%X (%u sectors)\n",
           xbe_size, xbe_lba, xbe_sectors);

    for (uint32_t s = 0; s < xbe_sectors; s++) {
        if (blk_pread(blk, (int64_t)(xbe_lba + s) * 512, 512, xbe_data + s * 512, 0) < 0) {
            printf("Chihiro XBE: Read error at sector %u\n", s);
            g_free(xbe_data);
            return;
        }
    }

    /* Step 3: Parse XBE header */
    uint32_t magic = *(uint32_t *)xbe_data;
    if (magic != XBE_MAGIC) {
        printf("Chihiro XBE: Bad XBE magic: 0x%08X\n", magic);
        g_free(xbe_data);
        return;
    }

    uint32_t base_addr = *(uint32_t *)(xbe_data + 0x104);
    uint32_t entry_enc = *(uint32_t *)(xbe_data + 0x128);
    uint32_t entry_dec = entry_enc ^ XBE_ENTRY_KEY;
    uint32_t num_sections = *(uint32_t *)(xbe_data + 0x11C);
    uint32_t sect_hdr_addr = *(uint32_t *)(xbe_data + 0x120);
    uint32_t sect_hdr_off = sect_hdr_addr - base_addr;

    printf("Chihiro XBE: base=0x%08X entry_enc=0x%08X entry_dec=0x%08X\n",
           base_addr, entry_enc, entry_dec);
    printf("Chihiro XBE: %u sections at file offset 0x%X\n",
           num_sections, sect_hdr_off);

    /* Step 4: Write sections to guest physical RAM */
    for (uint32_t i = 0; i < num_sections && i < 20; i++) {
        uint32_t s_off = sect_hdr_off + i * 56;
        uint32_t s_va = *(uint32_t *)(xbe_data + s_off + 4);
        uint32_t s_vsize = *(uint32_t *)(xbe_data + s_off + 8);
        uint32_t s_raw = *(uint32_t *)(xbe_data + s_off + 12);
        uint32_t s_rsize = *(uint32_t *)(xbe_data + s_off + 16);
        uint32_t s_name_addr = *(uint32_t *)(xbe_data + s_off + 20);
        uint32_t s_name_off = s_name_addr - base_addr;
        char sname[16] = {0};
        if (s_name_off < xbe_size - 16) {
            memcpy(sname, xbe_data + s_name_off, 15);
        }

        /* Write to HIGH physical RAM to avoid overwriting kernel.
         * Offset: PA = 0x4000000 + (VA - base) */
        uint32_t pa = 0x1000000 + (s_va - base_addr);
        printf("Chihiro XBE:   [%u] '%s' VA=0x%08X PA=0x%08X "
               "raw=0x%X rsize=0x%X\n",
               i, sname, s_va, pa, s_raw, s_rsize);

        if (s_raw + s_rsize <= xbe_size && pa < 0x08000000) {
            address_space_write(&address_space_memory, pa,
                                MEMTXATTRS_UNSPECIFIED,
                                xbe_data + s_raw, s_rsize);
            /* Zero-fill remaining virtual size */
            if (s_vsize > s_rsize) {
                uint32_t zero_size = s_vsize - s_rsize;
                uint8_t *zeros = g_malloc0(zero_size);
                address_space_write(&address_space_memory, pa + s_rsize,
                                    MEMTXATTRS_UNSPECIFIED, zeros, zero_size);
                g_free(zeros);
            }
        }
    }

    /* Write XBE header to high memory too */
    uint32_t hdr_size = *(uint32_t *)(xbe_data + 0x108);
    if (hdr_size > xbe_size) hdr_size = xbe_size;
    if (hdr_size > 0x10000) hdr_size = 0x10000;
    uint32_t hdr_pa = 0x1000000;
    address_space_write(&address_space_memory, hdr_pa,
                        MEMTXATTRS_UNSPECIFIED, xbe_data, hdr_size);
    printf("Chihiro XBE: Wrote %u byte header at PA 0x%08X\n",
           hdr_size, hdr_pa);


    /* Step 4b: Resolve kernel thunks */
    uint32_t kt_enc = *(uint32_t *)(xbe_data + 0x158);
    uint32_t kt_va = kt_enc ^ 0xEFB1F152;  /* debug thunk XOR key */
    uint32_t kt_off = kt_va - base_addr;
    uint32_t kt_pa = 0x1000000 + kt_off;
    printf("Chihiro XBE: Thunk table at VA 0x%08X PA 0x%08X\n", kt_va, kt_pa);

    int thunks_resolved = 0, thunks_missing = 0;
    for (int i = 0; i < 400; i++) {
        uint32_t raw;
        address_space_read(&address_space_memory, kt_pa + i * 4,
                           MEMTXATTRS_UNSPECIFIED, &raw, 4);
        if (raw == 0) break;
        if (raw & 0x80000000) {
            uint32_t ordinal = raw & 0x1FF;
            uint32_t resolved = (ordinal < 380) ? chihiro_kernel_exports[ordinal] : 0;
            if (resolved) {
                address_space_write(&address_space_memory, kt_pa + i * 4,
                                    MEMTXATTRS_UNSPECIFIED, &resolved, 4);
                thunks_resolved++;
            } else {
                printf("Chihiro XBE:   MISSING ordinal #%u\n", ordinal);
                thunks_missing++;
            }
        }
    }
    printf("Chihiro XBE: Thunks resolved=%d missing=%d\n",
           thunks_resolved, thunks_missing);

    /* Step 4c: File I/O instrumentation trampolines at PA 0x0E80000
     * (VA 0x80E80000, kernel identity map). Each stub: OUT 0x40FC
     * then JMP to original kernel function. Temporary — for measuring. */
    {
        uint32_t tramp_pa = 0x0E80000;
        /* NtCreateFile (ord 190 → VA 0x80016C9C) */
        uint8_t t0[] = {
            0xBA,0xFC,0x40,0x00,0x00, 0xB8,0xBE,0x00,0x00,0x00, 0xEF,
            0xB8,0x9C,0x6C,0x01,0x80, 0xFF,0xE0,
        };
        /* NtReadFile (ord 219 → VA 0x800177AA) */
        uint8_t t1[] = {
            0xBA,0xFC,0x40,0x00,0x00, 0xB8,0xDB,0x00,0x00,0x00, 0xEF,
            0xB8,0xAA,0x77,0x01,0x80, 0xFF,0xE0,
        };
        /* NtClose (ord 187 → VA 0x80020A68) */
        uint8_t t2[] = {
            0xBA,0xFC,0x40,0x00,0x00, 0xB8,0xBB,0x00,0x00,0x00, 0xEF,
            0xB8,0x68,0x0A,0x02,0x80, 0xFF,0xE0,
        };
        /* NtOpenFile (ord 202 → VA 0x80016CC5) */
        uint8_t t3[] = {
            0xBA,0xFC,0x40,0x00,0x00, 0xB8,0xCA,0x00,0x00,0x00, 0xEF,
            0xB8,0xC5,0x6C,0x01,0x80, 0xFF,0xE0,
        };
        address_space_write(&address_space_memory, tramp_pa,      MEMTXATTRS_UNSPECIFIED, t0, sizeof(t0));
        address_space_write(&address_space_memory, tramp_pa + 32, MEMTXATTRS_UNSPECIFIED, t1, sizeof(t1));
        address_space_write(&address_space_memory, tramp_pa + 64, MEMTXATTRS_UNSPECIFIED, t2, sizeof(t2));
        address_space_write(&address_space_memory, tramp_pa + 96, MEMTXATTRS_UNSPECIFIED, t3, sizeof(t3));

        /* Map trampoline page: VA 0x80E80000 → PA 0x0E80000
         * PDE[0x203] PT is at PA 0x7FD3000, PTE index = 0x280 */
        uint32_t tramp_pte = 0x0E80063; /* present, rw, accessed, dirty */
        address_space_write(&address_space_memory,
                            0x7FD3000 + 0x280 * 4,
                            MEMTXATTRS_UNSPECIFIED, &tramp_pte, 4);
        CPUState *tramp_cs = first_cpu;
        if (tramp_cs) tlb_flush(tramp_cs);

        struct { uint32_t orig_va; uint32_t tramp_va; const char *name; } patches[] = {
            { 0x80016C9C, 0x80E80000, "NtCreateFile" },
            { 0x800177AA, 0x80E80020, "NtReadFile"   },
            { 0x80020A68, 0x80E80040, "NtClose"      },
            { 0x80016CC5, 0x80E80060, "NtOpenFile"   },
        };
        for (int p = 0; p < 4; p++) {
            for (int i = 0; i < 400; i++) {
                uint32_t v;
                address_space_read(&address_space_memory, kt_pa + i * 4,
                                   MEMTXATTRS_UNSPECIFIED, &v, 4);
                if (v == 0) break;
                if (v == patches[p].orig_va) {
                    address_space_write(&address_space_memory, kt_pa + i * 4,
                                        MEMTXATTRS_UNSPECIFIED, &patches[p].tramp_va, 4);
                    printf("Chihiro XBE: Instrumented %s thunk[%d] -> 0x%08X\n",
                           patches[p].name, i, patches[p].tramp_va);
                    break;
                }
            }
        }
        printf("Chihiro XBE: File I/O instrumentation installed\n");
    }

    /* Step 4b: Initialize LaunchDataPage (required by game CRT) */
    {
        uint32_t ldp_pa = 0x0F00000;
        uint8_t zeros[4096];
        memset(zeros, 0, 4096);
        address_space_write(&address_space_memory, ldp_pa,
                            MEMTXATTRS_UNSPECIFIED, zeros, 4096);
        /* Write pointer to kernel global LaunchDataPage (PA 0x3B3D8) */
        uint32_t ldp_va = 0x80F00000;
        address_space_write(&address_space_memory, 0x3B3D8,
                            MEMTXATTRS_UNSPECIFIED, &ldp_va, 4);
        printf("Chihiro XBE: LaunchDataPage at VA 0x%08X\n", ldp_va);
    }

    /* Step 4d: Initialize TLS AddressOfIndex.
     * XBE TLS directory at VA 0x1BCD60, AddressOfIndex = VA 0x1FAAD8.
     * PA = 0x1000000 + (0x1FAAD8 - 0x10000) = 0x10EAAD8.
     * XepSetupTLS is a section loader, NOT TLS init — it never writes this.
     * The game CRT _tls_init reads this index; if unset, TLS-dependent
     * constructors fail silently → scene graph nodes never created → NULL. */
    {
        uint32_t tls_index = 0;
        address_space_write(&address_space_memory, 0x10EAAD8,
                            MEMTXATTRS_UNSPECIFIED, &tls_index, 4);
        printf("Chihiro XBE: TLS AddressOfIndex written (PA 0x10EAAD8 = 0)\n");
    }

        /* Step 5: Set up page tables (EIP change done by trampoline JMP) */
    CPUState *cs = first_cpu;
    if (cs) {
        cpu_synchronize_state(cs);
        X86CPU *cpu = X86_CPU(cs);
        CPUX86State *env = &cpu->env;
        uint32_t cr3 = (uint32_t)env->cr[3];
        printf("Chihiro XBE: CPU EIP=0x%08X CR3=0x%08X\n",
               (uint32_t)env->eip, cr3);

        /* Read page directory from CR3 */
        uint32_t pd[1024];
        address_space_read(&address_space_memory, cr3,
                           MEMTXATTRS_UNSPECIFIED, pd, 4096);

        uint32_t pt_alloc_pa = 0x0F00000;

        /* PDE 0: VA 0x00000000-0x003FFFFF */
        uint32_t pt0_pa;
        if (pd[0] & 1) {
            pt0_pa = pd[0] & 0xFFFFF000;
            printf("Chihiro XBE: PDE[0] exists at PA 0x%08X\n", pt0_pa);
        } else {
            pt0_pa = pt_alloc_pa;
            pt_alloc_pa += 0x1000;
            uint8_t zeros[4096] = {0};
            address_space_write(&address_space_memory, pt0_pa,
                                MEMTXATTRS_UNSPECIFIED, zeros, 4096);
            pd[0] = pt0_pa | 0x67;
            printf("Chihiro XBE: Allocated PDE[0] at PA 0x%08X\n", pt0_pa);
        }

        uint32_t pt0[1024];
        address_space_read(&address_space_memory, pt0_pa,
                           MEMTXATTRS_UNSPECIFIED, pt0, 4096);

        /* Calculate max VA from section virtual sizes (covers BSS) */
        uint32_t max_va = base_addr + 0x1000;
        for (uint32_t i = 0; i < num_sections && i < 20; i++) {
            uint32_t s_off = sect_hdr_off + i * 56;
            uint32_t s_va = *(uint32_t *)(xbe_data + s_off + 4);
            uint32_t s_vsize = *(uint32_t *)(xbe_data + s_off + 8);
            uint32_t end_va = s_va + s_vsize;
            if (end_va > max_va) max_va = end_va;
        }
        max_va = (max_va + 0xFFF) & ~0xFFF;
        printf("Chihiro XBE: Image spans VA 0x%08X-0x%08X\n", base_addr, max_va);

        int pages_mapped = 0;
        for (uint32_t va = base_addr; va < max_va && va < 0x400000; va += 0x1000) {
            uint32_t pte_idx = (va >> 12) & 0x3FF;
            uint32_t pa = 0x1000000 + (va - base_addr);
            pt0[pte_idx] = pa | 0x67;
            pages_mapped++;
        }
        address_space_write(&address_space_memory, pt0_pa,
                            MEMTXATTRS_UNSPECIFIED, pt0, 4096);
        printf("Chihiro XBE: Mapped %d pages in PDE[0]\n", pages_mapped);

        /* PDE 1: DOLBY at VA 0x501580 */
        uint32_t pt1_pa;
        if (pd[1] & 1) {
            pt1_pa = pd[1] & 0xFFFFF000;
        } else {
            pt1_pa = pt_alloc_pa;
            pt_alloc_pa += 0x1000;
            uint8_t zeros[4096] = {0};
            address_space_write(&address_space_memory, pt1_pa,
                                MEMTXATTRS_UNSPECIFIED, zeros, 4096);
            pd[1] = pt1_pa | 0x67;
        }
        uint32_t pt1[1024];
        address_space_read(&address_space_memory, pt1_pa,
                           MEMTXATTRS_UNSPECIFIED, pt1, 4096);
        int pde1_mapped = 0;
        for (uint32_t va = 0x400000; va < max_va; va += 0x1000) {
            uint32_t pte_idx = (va >> 12) & 0x3FF;
            uint32_t pa = 0x1000000 + (va - base_addr);
            pt1[pte_idx] = pa | 0x67;
            pde1_mapped++;
        }
        address_space_write(&address_space_memory, pt1_pa,
                            MEMTXATTRS_UNSPECIFIED, pt1, 4096);
        printf("Chihiro XBE: Mapped %d pages in PDE[1] (BSS+DOLBY)\n", pde1_mapped);

        address_space_write(&address_space_memory, cr3,
                            MEMTXATTRS_UNSPECIFIED, pd, 4096);
        tlb_flush(cs);

        /* Map LaunchDataPage: VA 0x80F00000 → PA 0x0F00000 */
        uint32_t ldp_pte = 0x0F00063;
        address_space_write(&address_space_memory,
                            0x7FD3000 + 0x300 * 4,
                            MEMTXATTRS_UNSPECIFIED, &ldp_pte, 4);

        /* Reset IRQL to PASSIVE_LEVEL (0).
         * We're in the SMBus handler context which runs at elevated IRQL.
         * Game code expects PASSIVE_LEVEL — any page fault at IRQL >= 2
         * triggers BugCheck 0x0A. IRQL is stored at PA 0x36170. */
        uint8_t current_irql = 0;
        address_space_read(&address_space_memory, 0x36170,
                           MEMTXATTRS_UNSPECIFIED, &current_irql, 1);
        printf("Chihiro XBE: IRQL at intercept = %d (0x%02X)\n",
               current_irql, current_irql);
        uint8_t irql_passive = 0;
        address_space_write(&address_space_memory, 0x36170,
                            MEMTXATTRS_UNSPECIFIED, &irql_passive, 1);

        /* Set EIP to game entry point (we are in vCPU context via SMBus) */
        env->eip = entry_dec;
        tlb_flush(cs);
        printf("Chihiro XBE: EIP set to 0x%08X\n", entry_dec);
    }

    /* Step 6: Instrument KeBugCheckEx (VA 0x80019782, PA 0x19782).
     * Write a JMP detour to our trampoline at VA 0x80E80080.
     * Trampoline: pushad → OUT 0x40FC (ordinal 0xBC) → popad →
     * execute saved original bytes → JMP back to KeBugCheckEx+5. */
    {
        uint32_t bugchk_pa = 0x19782;
        uint32_t tramp_pa = 0x0E80080;
        uint32_t tramp_va = 0x80E80080;
        uint32_t bugchk_va = 0x80019782;

        /* Read and save original 5 bytes */
        uint8_t orig[5];
        address_space_read(&address_space_memory, bugchk_pa,
                           MEMTXATTRS_UNSPECIFIED, orig, 5);

        /* Build trampoline: pushad, OUT, popad, orig bytes, JMP back */
        uint8_t tramp[32] = {
            0x60,                               /* pushad              */
            0xBA, 0xFC, 0x40, 0x00, 0x00,       /* mov edx, 0x40FC    */
            0xB8, 0xBC, 0x00, 0x00, 0x00,       /* mov eax, 0xBC      */
            0xEF,                               /* out dx, eax         */
            0x61,                               /* popad               */
            /* orig[0..4] copied below at offset 13 */
            0, 0, 0, 0, 0,
            /* JMP back to KeBugCheckEx+5 */
            0x68, 0, 0, 0, 0,                   /* push imm32          */
            0xC3,                               /* ret (= jmp [esp])   */
        };
        memcpy(tramp + 13, orig, 5);
        uint32_t ret_va = bugchk_va + 5;
        memcpy(tramp + 19, &ret_va, 4);

        address_space_write(&address_space_memory, tramp_pa,
                            MEMTXATTRS_UNSPECIFIED, tramp, sizeof(tramp));

        /* Write JMP detour at KeBugCheckEx entry */
        uint8_t jmp[5];
        jmp[0] = 0xE9;
        int32_t rel = (int32_t)(tramp_va - (bugchk_va + 5));
        memcpy(jmp + 1, &rel, 4);
        address_space_write(&address_space_memory, bugchk_pa,
                            MEMTXATTRS_UNSPECIFIED, jmp, 5);

        printf("Chihiro XBE: KeBugCheckEx detour installed "
               "(PA 0x%X → VA 0x%08X, orig=%02X%02X%02X%02X%02X)\n",
               bugchk_pa, tramp_va, orig[0], orig[1], orig[2], orig[3], orig[4]);
    }

    /* Step 7: Install function trace detours at key game init points.
     * Each detour: read original bytes, write trampoline (pushad/OUT 0x40F8/
     * popad/saved bytes/JMP back), overwrite function entry with JMP. */
    {
        struct {
            uint32_t va;
            int detour_len;
            uint8_t trace_id;
            const char *name;
        } traces[] = {
            { 0x0B973C, 5, 1, "crt_version_check" },
            { 0x0B96BB, 5, 3, "_initterm"          },
            { 0x0B9713, 5, 4, "main"               },
            { 0x084B90, 6, 5, "game_init_1"        },
            { 0x0FCCB0, 5, 6, "game_init_2"        },
            { 0x0B7D20, 5, 7, "game_init_3"        },
        };
        int n_traces = sizeof(traces) / sizeof(traces[0]);

        for (int t = 0; t < n_traces; t++) {
            uint32_t func_va = traces[t].va;
            uint32_t func_pa = 0x1000000 + (func_va - 0x10000);
            int dlen = traces[t].detour_len;
            uint32_t tramp_pa = 0x0E80100 + traces[t].trace_id * 32;
            uint32_t tramp_va = 0x80E80100 + traces[t].trace_id * 32;

            /* Read original bytes */
            uint8_t orig[8] = {0};
            address_space_read(&address_space_memory, func_pa,
                               MEMTXATTRS_UNSPECIFIED, orig, dlen);

            /* Build trampoline */
            uint8_t tramp[32] = {0};
            int p = 0;
            tramp[p++] = 0x60;                         /* pushad */
            tramp[p++] = 0xBA; tramp[p++] = 0xF8;
            tramp[p++] = 0x40; tramp[p++] = 0x00;
            tramp[p++] = 0x00;                         /* mov edx, 0x40F8 */
            tramp[p++] = 0xB8; tramp[p++] = traces[t].trace_id;
            tramp[p++] = 0x00; tramp[p++] = 0x00;
            tramp[p++] = 0x00;                         /* mov eax, id */
            tramp[p++] = 0xEF;                         /* out dx, eax */
            tramp[p++] = 0x61;                         /* popad */
            memcpy(tramp + p, orig, dlen); p += dlen;  /* saved bytes */
            tramp[p++] = 0x68;                         /* push imm32 */
            uint32_t ret_va = func_va + dlen;
            memcpy(tramp + p, &ret_va, 4); p += 4;
            tramp[p++] = 0xC3;                         /* ret */

            /* PTE for trampoline page (0x80E80000 already has one from Step 4c) */
            address_space_write(&address_space_memory, tramp_pa,
                                MEMTXATTRS_UNSPECIFIED, tramp, p);

            /* Write JMP detour + NOP padding */
            uint8_t jmp[8];
            jmp[0] = 0xE9;
            int32_t rel = (int32_t)(tramp_va - (func_va + 5));
            memcpy(jmp + 1, &rel, 4);
            for (int i = 5; i < dlen; i++) jmp[i] = 0x90;
            address_space_write(&address_space_memory, func_pa,
                                MEMTXATTRS_UNSPECIFIED, jmp, dlen);

            printf("Chihiro XBE: Trace[%d] %s (VA 0x%X, %d bytes, orig=%02X%02X%02X%02X%02X)\n",
                   traces[t].trace_id, traces[t].name, func_va, dlen,
                   orig[0], orig[1], orig[2], orig[3], orig[4]);
        }
    }

    printf("Chihiro: === XBE LOADER DONE ===\n");
    chihiro_xbe_loaded = true;
    g_free(xbe_data);
}

static void chihiro_irq10_timer_cb(void *opaque)
{
    ChihiroLPCState *s = opaque;

    if (s->eeprom_hack_applied) {
        qemu_irq_raise(s->irq10);

        /* Force baseboard state variables in SEGABOOT RAM.
         * Physical addresses from gva2gpa: VA 0x89C38→PA 0xD4C38,
         * VA 0x89C48→PA 0xD4C48. */
        uint32_t usb_present = 1;
        uint32_t bb_ready = 3;
        address_space_write(&address_space_memory, 0xD4C38,
                            MEMTXATTRS_UNSPECIFIED,
                            &usb_present, sizeof(usb_present));
        address_space_write(&address_space_memory, 0xD4C48,
                            MEMTXATTRS_UNSPECIFIED,
                            &bb_ready, sizeof(bb_ready));

        /* Persistent re-patching at 60Hz — keep ALL hacks alive.
         * The state machine re-executes each frame and some code paths
         * may overwrite our patches. Hammer them continuously. */
        if (s->error02_patch_addr) {
            uint8_t jmp = 0xEB;
            address_space_write(&address_space_memory, s->error02_patch_addr,
                                MEMTXATTRS_UNSPECIFIED, &jmp, 1);
        }
        if (s->segaboot_patch_addr) {
            uint8_t nop2[] = { 0x90, 0x90 };
            address_space_write(&address_space_memory, s->segaboot_patch_addr,
                                MEMTXATTRS_UNSPECIFIED, nop2, 2);
        }
        if (s->checkbootid_patch_addr) {
            uint8_t nop2[] = { 0x90, 0x90 };
            address_space_write(&address_space_memory, s->checkbootid_patch_addr,
                                MEMTXATTRS_UNSPECIFIED, nop2, 2);
        }
        if (s->timeout_patch_addr) {
            uint8_t jmp = 0xEB;
            address_space_write(&address_space_memory, s->timeout_patch_addr,
                                MEMTXATTRS_UNSPECIFIED, &jmp, 1);
        }
        if (s->systype_patch_addr) {
            uint8_t nop5[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
            address_space_write(&address_space_memory, s->systype_patch_addr,
                                MEMTXATTRS_UNSPECIFIED, nop5, 5);
        }
        if (s->nuclear_patch_addr) {
            uint8_t nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
            address_space_write(&address_space_memory, s->nuclear_patch_addr,
                                MEMTXATTRS_UNSPECIFIED, nop6, 6);
        }
        if (s->launchinfo_patch_addr) {
            uint8_t jmp = 0xEB;
            address_space_write(&address_space_memory, s->launchinfo_patch_addr,
                                MEMTXATTRS_UNSPECIFIED, &jmp, 1);
        }
        if (s->appcreate_patch_addr) {
            uint8_t jmp = 0xEB;
            address_space_write(&address_space_memory, s->appcreate_patch_addr,
                                MEMTXATTRS_UNSPECIFIED, &jmp, 1);
        }

        /* Periodic state logging (every ~1s) */
            }


    /* XBE load is triggered by SEGABOOT trampoline OUT to port 0x40FE */


    /* Post-XBE-load diagnostics: log EIP every second */
    if (chihiro_xbe_loaded) {
        static int eip_log_count = 0;
        static int eip_log_ticks = 0;
        eip_log_ticks++;
        if (eip_log_ticks >= 60 && eip_log_count < 5) {
            eip_log_ticks = 0;
            eip_log_count++;
            CPUState *diag_cs = first_cpu;
            if (diag_cs) {
                cpu_synchronize_state(diag_cs);
                X86CPU *diag_cpu = X86_CPU(diag_cs);
                CPUX86State *diag_env = &diag_cpu->env;
                uint32_t eip = (uint32_t)diag_env->eip;
                const char *region = "UNKNOWN";
                if (eip >= 0x80010000 && eip < 0x80090000) region = "KERNEL";
                else if (eip >= 0x11000 && eip < 0x135A00) region = "GAME .text";
                else if (eip >= 0x10000 && eip < 0x220000) region = "GAME other";
                else if (eip < 0x10000) region = "LOW MEM";
                /* Read bytes at EIP from guest RAM */
                uint32_t eip_pa = eip;
                if (eip >= 0x80000000) eip_pa = eip - 0x80000000;
                uint8_t eip_bytes[8];
                address_space_read(&address_space_memory, eip_pa,
                                   MEMTXATTRS_UNSPECIFIED, eip_bytes, 8);
                if (eip_log_count == 1) {
                    /* Read KiBugCheckData (5 DWORDs at PA 0x3ABE0) */
                    uint32_t bugcheck[5];
                    address_space_read(&address_space_memory, 0x3ABE0,
                                       MEMTXATTRS_UNSPECIFIED, bugcheck, 20);
                    printf("Chihiro DIAG: KiBugCheckData: code=0x%08X "
                           "p1=0x%08X p2=0x%08X p3=0x%08X p4=0x%08X\n",
                           bugcheck[0], bugcheck[1], bugcheck[2],
                           bugcheck[3], bugcheck[4]);
                    uint32_t esp_pa = (uint32_t)diag_env->regs[R_ESP];
                    if (esp_pa >= 0xD0000000) esp_pa -= 0xD0000000;
                    else if (esp_pa >= 0x80000000) esp_pa -= 0x80000000;
                    uint32_t stack[32];
                    address_space_read(&address_space_memory, esp_pa,
                                       MEMTXATTRS_UNSPECIFIED, stack, 128);
                    printf("Chihiro DIAG: Stack at ESP PA 0x%08X:\n", esp_pa);
                    for (int s = 0; s < 32; s++) {
                        const char *tag = "";
                        if (stack[s] >= 0x80010000 && stack[s] < 0x80090000) tag = " [KERNEL]";
                        else if (stack[s] >= 0x11000 && stack[s] < 0x220000) tag = " [GAME]";
                        printf("  +%02X: 0x%08X%s\n", s*4, stack[s], tag);
                    }
                }
                printf("Chihiro DIAG[%d]: EIP=0x%08X [%s] ESP=0x%08X"
                       " code=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                       eip_log_count, eip, region,
                       (uint32_t)diag_env->regs[R_ESP],
                       eip_bytes[0], eip_bytes[1], eip_bytes[2], eip_bytes[3],
                       eip_bytes[4], eip_bytes[5], eip_bytes[6], eip_bytes[7]);
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
        !s->checkbootid_hack_applied || !s->timeout_hack_applied ||
        !s->fwskip_hack_applied || !s->fwret_hack_applied ||
        !s->usbenum_hack_applied) {
        /* Phase 2-6: Scan RAM for five SEGABOOT patterns.
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
         *
         * #4 Timeout (40s → Error 22):
         *    81 7B 18 60 09 00 00 72 0F 39 6B 10
         *    Patch byte+7: 72→EB (JB→JMP, never timeout)
         *
         * #5 Firmware upload skip (OpenUSBDevice):
         *    8A 86 AC 14 01 00 84 C0 0F 85
         *    Patch byte+6-7: 84 C0→B0 01 (test al,al → mov al,1)
         *    Forces the "firmware already cached" path
         */
        uint8_t pat_error02[] = { 0x85, 0xC0, 0x75, 0x0F, 0x39, 0x6B, 0x10,
                                  0x75, 0x0A, 0xC7, 0x43, 0x10, 0x02 };
        uint8_t pat_errors[]  = { 0x75, 0x0E, 0x68, 0xB0, 0x1F, 0x02, 0x00 };
        uint8_t pat_bootid[]  = { 0x75, 0x10, 0x68, 0xD0, 0x1F, 0x02, 0x00 };
        uint8_t pat_timeout[] = { 0x81, 0x7B, 0x18, 0x60, 0x09, 0x00, 0x00,
                                  0x72, 0x0F, 0x39, 0x6B, 0x10 };
        uint8_t pat_fwskip[]  = { 0x8A, 0x86, 0xAC, 0x14, 0x01, 0x00,
                                  0x84, 0xC0, 0x0F, 0x85 };
        uint8_t pat_fwret[]   = { 0x83, 0xC4, 0x08, 0x33, 0xC0, 0x5F, 0x5E,
                                  0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x10, 0x00 };
        uint8_t pat_usbenum[] = { 0xFD, 0xFF, 0x85, 0xC0, 0x75, 0x10, 0x5F,
                                  0x5E, 0x5D, 0xB8, 0x01, 0x00, 0x00, 0x00,
                                  0x5B, 0xC2, 0x08, 0x00 };
        uint8_t pat_bbready[] = { 0x8B, 0x0D, 0x48, 0x9C, 0x08, 0x00,
                                  0x33, 0xC0, 0x83, 0xF9, 0x03, 0x0F,
                                  0x94, 0xC0, 0xC3 };
        uint8_t pat_systype[] = { 0x25, 0xFF, 0x00, 0x00, 0x00, 0x2B, 0xC5,
                                  0x74, 0x0E, 0x48, 0x74, 0x04 };
        uint8_t pat_nuclear[] = { 0x8B, 0x4B, 0x18, 0x8B, 0x43, 0x10,
                                  0x41, 0x3B, 0xC5, 0x89, 0x4B, 0x18,
                                  0x0F, 0x87 };
        uint8_t pat_launch[]  = { 0x85, 0xC0, 0x7D, 0x07, 0x68, 0x4C,
                                  0x20, 0x02, 0x00 };
        uint8_t pat_dpath[]   = { 0x64, 0x3A, 0x25, 0x73, 0x00, 0x00,
                                  0x00, 0x00 };
        uint8_t pat_xbepath[] = { 0x74, 0x07, 0x8D, 0x84, 0x24, 0x68,
                                  0x04, 0x00, 0x00 };
        uint8_t pat_vtable[]  = { 0x8D, 0x4C, 0x24, 0x08, 0xE8, 0xD7,
                                  0x51, 0xFF, 0xFF, 0x68, 0x38, 0x20,
                                  0x02, 0x00 };
        uint8_t pat_create[]  = { 0x85, 0xC0, 0x7D, 0x07, 0x68, 0x74,
                                  0x20, 0x02, 0x00, 0xEB, 0x6E };
        uint8_t pat_stub[]    = { 0x33, 0xC0, 0xC2, 0x1C, 0x00,
                                  0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                  0x90, 0x90, 0x90, 0x90, 0x90 };

        uint8_t *block = g_malloc(0x400000);
        for (uint32_t base = 0; base < 0x8000000; base += 0x400000) {
            address_space_read(&address_space_memory, base,
                               MEMTXATTRS_UNSPECIFIED, block, 0x400000);
            for (uint32_t off = 0; off < 0x400000 - 18; off++) {
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
                if (!s->timeout_hack_applied &&
                    memcmp(block + off, pat_timeout, 12) == 0) {
                    s->timeout_patch_addr = base + off + 7; /* the JB byte */
                    uint8_t jmp = 0xEB;
                    address_space_write(&address_space_memory,
                                        s->timeout_patch_addr,
                                        MEMTXATTRS_UNSPECIFIED, &jmp, 1);
                    s->timeout_hack_applied = true;
                    printf("Chihiro: Applied timeout bypass "
                           "(phys @ 0x%08X)\n", s->timeout_patch_addr);
                }
                if (!s->fwskip_hack_applied &&
                    memcmp(block + off, pat_fwskip, 10) == 0) {
                    uint32_t phys = base + off + 6;
                    uint8_t mov_al_1[] = { 0xB0, 0x01 };
                    address_space_write(&address_space_memory, phys,
                                        MEMTXATTRS_UNSPECIFIED, mov_al_1, 2);
                    s->fwskip_hack_applied = true;
                    printf("Chihiro: Applied firmware upload skip "
                           "(phys @ 0x%08X)\n", phys);
                }
                if (!s->fwret_hack_applied &&
                    memcmp(block + off, pat_fwret, 14) == 0) {
                    uint32_t phys = base + off + 3;
                    uint8_t mov_al_1[] = { 0xB0, 0x01 };
                    address_space_write(&address_space_memory, phys,
                                        MEMTXATTRS_UNSPECIFIED, mov_al_1, 2);
                    s->fwret_hack_applied = true;
                    printf("Chihiro: Applied OpenUSBDevice ret 1 "
                           "(phys @ 0x%08X)\n", phys);
                }
                if (!s->usbenum_hack_applied &&
                    memcmp(block + off, pat_usbenum, 18) == 0) {
                    uint32_t phys = base + off + 10;
                    uint8_t zero = 0x00;
                    address_space_write(&address_space_memory, phys,
                                        MEMTXATTRS_UNSPECIFIED, &zero, 1);
                    s->usbenum_hack_applied = true;
                    printf("Chihiro: Applied USBEnumerate ret 0 "
                           "(phys @ 0x%08X)\n", phys);
                }
                if (!s->bbready_hack_applied &&
                    memcmp(block + off, pat_bbready, 15) == 0) {
                    uint32_t phys = base + off + 6;
                    uint8_t mov_al_1[] = { 0xB0, 0x01 };
                    address_space_write(&address_space_memory, phys,
                                        MEMTXATTRS_UNSPECIFIED, mov_al_1, 2);
                    s->bbready_hack_applied = true;
                    printf("Chihiro: Applied IsBaseboardReady=TRUE "
                           "(phys @ 0x%08X)\n", phys);
                }
                if (!s->systype_hack_applied &&
                    memcmp(block + off, pat_systype, 12) == 0) {
                    s->systype_patch_addr = base + off + 7;
                    uint32_t phys = s->systype_patch_addr;
                    uint8_t nop4[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
                    /* NOP both je 0x0E and je 0x04 */
                    address_space_write(&address_space_memory, phys,
                                        MEMTXATTRS_UNSPECIFIED, nop4, 5);
                    s->systype_hack_applied = true;
                    printf("Chihiro: Applied SysType/Error31 bypass "
                           "(phys @ 0x%08X)\n", phys);
                }
                if (!s->nuclear_hack_applied &&
                    memcmp(block + off, pat_nuclear, 14) == 0) {
                    s->nuclear_patch_addr = base + off + 12;
                    uint8_t nop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
                    address_space_write(&address_space_memory,
                                        s->nuclear_patch_addr,
                                        MEMTXATTRS_UNSPECIFIED, nop6, 6);
                    s->nuclear_hack_applied = true;
                    printf("Chihiro: Applied NUCLEAR error bypass "
                           "(phys @ 0x%08X)\n", s->nuclear_patch_addr);
                }
                if (!s->launchinfo_hack_applied &&
                    memcmp(block + off, pat_launch, 9) == 0) {
                    s->launchinfo_patch_addr = base + off + 2;
                    uint8_t jmp = 0xEB;
                    address_space_write(&address_space_memory,
                                        s->launchinfo_patch_addr,
                                        MEMTXATTRS_UNSPECIFIED, &jmp, 1);
                    s->launchinfo_hack_applied = true;
                    printf("Chihiro: Applied GetLaunchInfo bypass "
                           "(phys @ 0x%08X)\n", s->launchinfo_patch_addr);
                }
                if (!s->drivepath_hack_applied &&
                    memcmp(block + off, pat_dpath, 8) == 0) {
                    uint32_t phys = base + off;
                    /* "mbfs:%s\0" = 6D 62 66 73 3A 25 73 00 */
                    uint8_t mbfs[] = {0x6D,0x62,0x66,0x73,0x3A,0x25,0x73,0x00};
                    address_space_write(&address_space_memory, phys,
                                        MEMTXATTRS_UNSPECIFIED, mbfs, 8);
                    s->drivepath_hack_applied = true;
                    printf("Chihiro: Applied d:->mbfs: path redirect "
                           "(phys @ 0x%08X)\n", phys);
                }
                if (!s->xbepath_hack_applied &&
                    memcmp(block + off, pat_xbepath, 9) == 0) {
                    /* Write "\\hod3xb.xbe\0" at PA 0x47B09 (VA 0x1CB09) */
                    uint8_t xbe_str[] = "\\hod3xb.xbe";
                    address_space_write(&address_space_memory, 0x47B09,
                                        MEMTXATTRS_UNSPECIFIED, xbe_str, 13);
                    /* Patch demo LEA at off-5 (PA 0x7A34B):
                     * 8D 84 24 88 04 00 00 → B8 09 CB 01 00 90 90 */
                    uint32_t lea_demo = base + off - 7;
                    uint8_t mov_demo[] = {0xB8, 0x09, 0xCB, 0x01, 0x00, 0x90, 0x90};
                    address_space_write(&address_space_memory, lea_demo,
                                        MEMTXATTRS_UNSPECIFIED, mov_demo, 7);
                    /* Patch game LEA at off+2 (PA 0x7A354):
                     * 8D 84 24 68 04 00 00 → B8 09 CB 01 00 90 90 */
                    uint32_t lea_game = base + off + 2;
                    uint8_t mov_game[] = {0xB8, 0x09, 0xCB, 0x01, 0x00, 0x90, 0x90};
                    address_space_write(&address_space_memory, lea_game,
                                        MEMTXATTRS_UNSPECIFIED, mov_game, 7);
                    s->xbepath_hack_applied = true;
                    printf("Chihiro: Applied XBE path inject "
                           "(phys @ 0x%08X)\n", lea_demo);
                }
                if (!s->vtable_hack_applied &&
                    memcmp(block + off, pat_vtable, 14) == 0) {
                    uint32_t phys = base + off + 4;
                    uint8_t nop5[] = {0x90, 0x90, 0x90, 0x90, 0x90};
                    address_space_write(&address_space_memory, phys,
                                        MEMTXATTRS_UNSPECIFIED, nop5, 5);
                    s->vtable_hack_applied = true;
                    printf("Chihiro: Applied vtable cleanup NOP "
                           "(phys @ 0x%08X)\n", phys);
                }
                if (!s->appcreate_hack_applied &&
                    memcmp(block + off, pat_create, 11) == 0) {
                    s->appcreate_patch_addr = base + off + 2;
                    uint32_t phys = s->appcreate_patch_addr;
                    uint8_t jmp = 0xEB;
                    address_space_write(&address_space_memory, phys,
                                        MEMTXATTRS_UNSPECIFIED, &jmp, 1);
                    s->appcreate_hack_applied = true;
                    printf("Chihiro: Applied theApp.Create bypass "
                           "(phys @ 0x%08X)\n", phys);
                }
                if (!s->xbeloader_hack_applied &&
                    memcmp(block + off, pat_stub, 16) == 0) {
                    uint32_t stub_pa = base + off;
                    /* Write path string at PA 0x47B20 */
                    uint8_t path[] = "\\??\\mbfs:\\hod3xb.xbe";
                    address_space_write(&address_space_memory, 0x47B20,
                                        MEMTXATTRS_UNSPECIFIED, path, 21);
                    /* Write trampoline: OUT triggers XBE load, then JMP to entry
                     * All in SEGABOOT's thread context (64KB stack) */
                    uint8_t tramp[] = {
                        0xB0, 0x01,                            /* mov al, 1 */
                        0xBA, 0xFE, 0x40, 0x00, 0x00,         /* mov edx, 0x40FE */
                        0xEF,                                  /* out dx, eax */
                        0xA1, 0x00, 0x00, 0xF1, 0x80,         /* mov eax, [0x80F10000] */
                        0xFF, 0xE0,                            /* jmp eax */
                        0x90                                   /* nop padding */
                    };
                    address_space_write(&address_space_memory, stub_pa,
                                        MEMTXATTRS_UNSPECIFIED, tramp, 16);
                    s->xbeloader_hack_applied = true;
                    printf("Chihiro: Applied XBE loader trampoline "
                           "(phys @ 0x%08X)\n", stub_pa);
                }
            }
            if (s->error02_hack_applied && s->segaboot_hack_applied &&
                s->checkbootid_hack_applied && s->timeout_hack_applied &&
                s->fwskip_hack_applied && s->fwret_hack_applied &&
                s->usbenum_hack_applied && s->bbready_hack_applied && s->systype_hack_applied && s->nuclear_hack_applied && s->launchinfo_hack_applied && s->drivepath_hack_applied && s->xbepath_hack_applied && s->vtable_hack_applied && s->appcreate_hack_applied && s->xbeloader_hack_applied ) {
                break;
            }
        }
        g_free(block);

        if (!s->error02_hack_applied || !s->segaboot_hack_applied ||
            !s->checkbootid_hack_applied || !s->timeout_hack_applied ||
            !s->fwskip_hack_applied || !s->fwret_hack_applied ||
            !s->usbenum_hack_applied || !s->bbready_hack_applied || !s->systype_hack_applied || !s->nuclear_hack_applied || !s->launchinfo_hack_applied || !s->drivepath_hack_applied || !s->xbepath_hack_applied || !s->vtable_hack_applied || !s->appcreate_hack_applied || !s->xbeloader_hack_applied ) {
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
        address_space_write(&address_space_memory, s->timeout_patch_addr,
                            MEMTXATTRS_UNSPECIFIED, &jmp, 1);
        s->repatch_count++;
        timer_mod(s->eeprom_hack_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }
}

static void chihiro_lpc_realize(DeviceState *dev, Error **errp)
{
    ChihiroLPCState *s = CHIHIRO_LPC_DEVICE(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    chihiro_active = true;
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
    s->timeout_hack_applied = false;
    s->fwskip_hack_applied = false;
    s->fwret_hack_applied = false;
    s->usbenum_hack_applied = false;
    s->bbready_hack_applied = false;
    s->systype_hack_applied = false;
    s->systype_patch_addr = 0;
    s->nuclear_hack_applied = false;
    s->nuclear_patch_addr = 0;
    s->launchinfo_hack_applied = false;
    s->launchinfo_patch_addr = 0;
    s->drivepath_hack_applied = false;
    s->xbepath_hack_applied = false;
    s->vtable_hack_applied = false;
    s->appcreate_hack_applied = false;
    s->appcreate_patch_addr = 0;
    s->xbeloader_hack_applied = false;
    s->error02_patch_addr = 0;
    s->segaboot_patch_addr = 0;
    s->checkbootid_patch_addr = 0;
    s->timeout_patch_addr = 0;
    s->repatch_count = 0;
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
        qemu_irq_raise(chihiro_irq10_global);
    }
}
