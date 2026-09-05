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

/* Card reader emulation (CRP-1231 via ring buffer tap/injection) */

/* Save file persistence (Phase 2): ic11 + extmem backup area */
bool chihiro_usb_save_load(const char *path);
bool chihiro_usb_save_flush(const char *path);
void chihiro_save_init(void);

#endif
