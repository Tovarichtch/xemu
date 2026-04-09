#ifndef HW_XBOX_CHIHIRO_H
#define HW_XBOX_CHIHIRO_H

/* mbcom IDE hooks — called from IDE DMA path */
void chihiro_mbcom_init(void);
bool chihiro_ide_read_sector(uint32_t lba, void *buffer);
bool chihiro_ide_write_sector(uint32_t lba, const void *buffer);

#endif
