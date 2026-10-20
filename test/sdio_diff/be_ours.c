/* backend.h on the gateway's SD driver (src/sd: sd_card + sd_sdio + the PIO
 * wire), exactly as the firmware builds it. */
#include "backend.h"

#include <string.h>

#include "sd/sd_card.h"

static int cls(int e) {
    switch (e) {
    case SD_OK:       return R_OK;
    case SD_ETIMEOUT: return R_TIMEOUT;
    case SD_ECRC:     return R_CRC;
    case SD_ENOCARD:  return R_NOCARD;
    case SD_EPARAM:   return R_PARAM;
    default:          return R_IO;
    }
}

const char *be_name(void) { return "ours"; }
bool be_present(void) { return sd_card_present(); }

int be_init(be_info_t *info) {
    int e = sd_card_init();
    memset(info, 0, sizeof *info);
    if (e == SD_OK) {
        info->sectors = sd_card_info()->sectors;
        info->cid_ok = sd_card_info()->cid_ok;
        memcpy(info->cid, sd_card_info()->cid, 16);
    }
    return cls(e);
}

int be_read(uint32_t lba, uint8_t *buf, uint32_t n) { return cls(sd_card_read(lba, buf, n)); }
int be_write(uint32_t lba, const uint8_t *buf, uint32_t n) { return cls(sd_card_write(lba, buf, n)); }
int be_sync(void) { return cls(sd_card_sync()); }
