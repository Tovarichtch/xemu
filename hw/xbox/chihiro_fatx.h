#ifndef HW_XBOX_CHIHIRO_FATX_H
#define HW_XBOX_CHIHIRO_FATX_H

#include <stdint.h>
#include <stdbool.h>

/* Build FATX image directly into a destination buffer.
 * Returns bytes written, or 0 on failure. */
uint32_t chihiro_fatx_build(const char *game_dir, uint8_t *dest,
                            uint32_t dest_size, uint32_t partition_sectors);

#endif
