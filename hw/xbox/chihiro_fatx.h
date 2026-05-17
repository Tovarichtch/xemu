#ifndef HW_XBOX_CHIHIRO_FATX_H
#define HW_XBOX_CHIHIRO_FATX_H

#include <stdint.h>
#include <stdbool.h>

/* Build FATX image in memory from game directory.
 * Returns malloc'd buffer (caller frees), sets *out_size. */
uint8_t *chihiro_fatx_build(const char *game_dir, uint32_t *out_size,
                            uint32_t partition_sectors);

#endif
