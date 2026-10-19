/* FatFs's disk_* on sd_card.c: one drive, 512-byte sectors. */
#include "ff.h"
#include "diskio.h"
#include "sd/sd_card.h"

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) { return STA_NOINIT; }
    if (!sd_card_present()) { return STA_NOINIT | STA_NODISK; }
    if (!sd_card_ready() && sd_card_init() != SD_OK) { return STA_NOINIT; }
    return 0;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) { return STA_NOINIT; }
    if (!sd_card_present()) {
        sd_card_forget();
        return STA_NOINIT | STA_NODISK;
    }
    return sd_card_ready() ? 0 : STA_NOINIT;
}

static DRESULT res(int e) {
    switch (e) {
    case SD_OK:      return RES_OK;
    case SD_ENOCARD: return RES_NOTRDY;
    case SD_EPARAM:  return RES_PARERR;
    default:         return RES_ERROR;
    }
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || count == 0) { return RES_PARERR; }
    return res(sd_card_read((uint64_t)sector, buff, count));
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || count == 0) { return RES_PARERR; }
    return res(sd_card_write((uint64_t)sector, buff, count));
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) { return RES_PARERR; }
    if (!sd_card_ready()) { return RES_NOTRDY; }
    switch (cmd) {
    case CTRL_SYNC:        return res(sd_card_sync());
    case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)sd_card_info()->sectors; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
    default:               return RES_PARERR;
    }
}
