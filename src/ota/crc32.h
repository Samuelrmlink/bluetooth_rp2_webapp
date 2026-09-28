#ifndef OTA_CRC32_H
#define OTA_CRC32_H
#include <stddef.h>
#include <stdint.h>
/* IEEE 802.3 CRC-32 (the zlib one), incremental: start with 0. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t n);
#endif
