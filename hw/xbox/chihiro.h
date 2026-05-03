#ifndef HW_XBOX_CHIHIRO_H
#define HW_XBOX_CHIHIRO_H

#include "system/block-backend.h"

/* Forward declaration */
typedef struct USBDevice USBDevice;

/* mbcom IDE hooks — called from IDE DMA path */
void chihiro_mbcom_init(void);
bool chihiro_ide_read_sector(uint32_t lba, void *buffer);
bool chihiro_ide_write_sector(uint32_t lba, const void *buffer);
void chihiro_ide_dma_write_done(BlockBackend *blk, int64_t sector_num);

/* USB delayed hotplug (AN2131 firmware boot simulation) */
void chihiro_usb_set_devices(USBDevice *qc, USBDevice *sc);

/* Load baseboard flash ROM (SEGABOOT) from file */
void chihiro_load_flash_rom(const char *bios_path);

/* Called from SMC when SCRATCH=0x04 (QuickReboot signal) */
void chihiro_on_quickreboot_signal(void);

/* Called from SMC POWER handler to detect QuickReboot */
bool chihiro_intercept_reset(void);

#endif
