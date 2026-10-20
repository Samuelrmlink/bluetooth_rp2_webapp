#ifndef BTGW_SD_CRC_H
#define BTGW_SD_CRC_H
/* SD card CRCs: CRC7 for commands, CRC16 for data, and all four CRC16s of a
 * 4-bit data block at once (see sd_crc.c). Platform-free. */
#include <stddef.h>
#include <stdint.h>

/* The 7-bit CRC of a command's first five bytes; the sixth byte on the wire
 * is (crc7 << 1) | 1. */
uint8_t  sd_crc7(const uint8_t *p, size_t n);
/* CRC16-CCITT (init 0): one data line's CRC in 1-bit mode, and SPI's. */
uint16_t sd_crc16(const uint8_t *p, size_t n);
/* The four D0-D3 CRC16s of a 4-bit block, interleaved as they are sent: the
 * 64 bits MSB first are the 16 CRC nibbles that follow the data. */
uint64_t sd_crc16x4(const uint8_t *p, size_t n);
/* One line's CRC16 out of sd_crc16x4's result (0 = D0 .. 3 = D3). */
uint16_t sd_crc16x4_line(uint64_t crc, int line);

#endif
