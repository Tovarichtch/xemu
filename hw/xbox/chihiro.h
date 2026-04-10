#ifndef HW_XBOX_CHIHIRO_H
#define HW_XBOX_CHIHIRO_H

#include "system/block-backend.h"

/* mbcom IDE hooks — called from IDE DMA path */
void chihiro_mbcom_init(void);
bool chihiro_ide_read_sector(uint32_t lba, void *buffer);
bool chihiro_ide_write_sector(uint32_t lba, const void *buffer);
void chihiro_ide_dma_write_done(BlockBackend *blk, int64_t sector_num);

#endif
