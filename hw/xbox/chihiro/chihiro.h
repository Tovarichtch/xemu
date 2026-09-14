/*
 * QEMU Chihiro emulation
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
#ifndef HW_XBOX_CHIHIRO_H
#define HW_XBOX_CHIHIRO_H

#include "system/block-backend.h"
#include "system/dma.h"

/* Forward declaration */
typedef struct USBDevice USBDevice;

/* MemoryRegion-backed IDE interface */
void chihiro_ide_interface_init(void);
void chihiro_ide_load_rom(void);
uint8_t *chihiro_fatx_get_buffer(uint32_t *out_size);
bool chihiro_ide_serve(int dma_cmd, uint32_t lba, int n,
                       QEMUSGList *sg, bool *irq);

/* mbcom state */
void chihiro_mbcom_init(void);

/* USB delayed hotplug (AN2131 firmware boot simulation) */
void chihiro_usb_set_devices(USBDevice *qc, USBDevice *sc);

/* Load baseboard flash ROM (SEGABOOT) from file */
void chihiro_load_flash_rom(const char *bios_path);
bool chihiro_flash_rom_loaded(void);

/* Why the last DIMM snapshot save or load failed, NULL when it did not:
 * the vmstate hooks can only return an errno, the UI wants words. */
const char *chihiro_dimm_last_error(void);
/* Size and CRC32 of the mounted netboot image as the DIMM delta records
 * them; false when the image cannot be read. */
bool chihiro_dimm_image_identity(uint64_t *size, uint32_t *crc);

/* Load baseboard EEPROMs (ic10, ic11, pc20) from BIOS directory */
void chihiro_load_eeproms(const char *bios_path);
extern uint8_t *chihiro_ic10_data;
extern uint32_t chihiro_ic10_size;
extern uint8_t *chihiro_ic11_data;
extern uint32_t chihiro_ic11_size;
extern uint8_t *chihiro_pc20_data;
extern uint32_t chihiro_pc20_size;

/* Game state */
extern char chihiro_game_dir[1024];
extern char chihiro_game_filename[64];
/* Sega netboot boot.id: the bytes every source of the game name parses. */
#define CHIHIRO_BOOTID_LEN 0xC0
bool chihiro_bootid_executable(const uint8_t *bid, char *out, size_t out_len);
void chihiro_set_game_executable(const char *name);
extern bool chihiro_board_type3;
extern int chihiro_region_setting;
extern bool chihiro_freeplay_setting;
int chihiro_detected_game_profile(void);

/* Called from OHCI when bus starts */
void chihiro_on_ohci_bus_start(void);
void chihiro_on_ohci_bus_stop(void);
uint32_t chihiro_va_to_pa(uint32_t va);
extern uint32_t chihiro_usb_sm_pa;

/* Called from SMC when SCRATCH=0x04 (QuickReboot signal) */
void chihiro_on_quickreboot_signal(void);

/* Called from SMC POWER handler to detect QuickReboot */
bool chihiro_intercept_reset(void);

/* Card reader emulation (Sanwa CRP-1231LR-10NAB via ring buffer tap/injection) */

/* Save file persistence (Phase 2): ic11 + extmem backup area */
bool chihiro_usb_save_load(const char *path);
bool chihiro_usb_save_flush(const char *path);
void chihiro_save_init(void);

#endif
