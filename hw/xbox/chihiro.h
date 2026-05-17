#ifndef HW_XBOX_CHIHIRO_H
#define HW_XBOX_CHIHIRO_H

#include "system/block-backend.h"
#include "system/dma.h"

/* Forward declaration */
typedef struct USBDevice USBDevice;

/* MemoryRegion-backed IDE interface */
void chihiro_ide_interface_init(void);
void chihiro_ide_load_rom(void);
void chihiro_fatx_populate(const uint8_t *fatx_data, uint32_t fatx_size);
bool chihiro_ide_serve(int dma_cmd, uint32_t lba, int n,
                       QEMUSGList *sg, bool *irq);

/* mbcom state */
void chihiro_mbcom_init(void);

/* USB delayed hotplug (AN2131 firmware boot simulation) */
void chihiro_usb_set_devices(USBDevice *qc, USBDevice *sc);

/* Load baseboard flash ROM (SEGABOOT) from file */
void chihiro_load_flash_rom(const char *bios_path);

/* Load baseboard EEPROMs (ic10, ic11, pc20) from BIOS directory */
void chihiro_load_eeproms(const char *bios_path);
extern uint8_t *chihiro_ic10_data;
extern uint32_t chihiro_ic10_size;
extern uint8_t *chihiro_ic11_data;
extern uint32_t chihiro_ic11_size;
extern uint8_t *chihiro_pc20_data;
extern uint32_t chihiro_pc20_size;

/* Called from SMC when SCRATCH=0x04 (QuickReboot signal) */
void chihiro_on_quickreboot_signal(void);

/* Called from SMC POWER handler to detect QuickReboot */
bool chihiro_intercept_reset(void);

/* Save file persistence (Phase 2): ic11 + extmem backup area */
bool chihiro_usb_save_load(const char *path);
bool chihiro_usb_save_flush(const char *path);
void chihiro_save_init(void);

#endif
