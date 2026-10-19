/* backend.h on carlk3's no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (fetched by this
 * test's CMake; never part of the gateway). Same pins, its own config. */
#include "backend.h"

#include <string.h>

#include "hw_config.h"
#include "sd_card.h"

static sd_sdio_if_t sdio_if = {
    .CMD_gpio = SDDIFF_CMD,
    .D0_gpio = SDDIFF_D0,                 /* CLK = D0 - 2 (its PIO program's rule) */
    .baud_rate = 125 * 1000 * 1000 / 6,   /* its example's ~20.8 MHz, as near ours as it goes */
};
static sd_card_t card = {
    .type = SD_IF_SDIO,
    .sdio_if_p = &sdio_if,
    .use_card_detect = true,
    .card_detect_gpio = SDDIFF_DETECT,
    .card_detected_true = 0,
    .card_detect_use_pull = true,
    .card_detect_pull_hi = true,
};

size_t sd_get_num(void) { return 1; }
sd_card_t *sd_get_by_num(size_t num) { return num == 0 ? &card : NULL; }

static int cls_blk(int e) {
    if (e == SD_BLOCK_DEVICE_ERROR_NONE) return R_OK;
    if (e & SD_BLOCK_DEVICE_ERROR_NO_RESPONSE) return R_TIMEOUT;
    if (e & SD_BLOCK_DEVICE_ERROR_CRC) return R_CRC;
    if (e & (SD_BLOCK_DEVICE_ERROR_NO_DEVICE | SD_BLOCK_DEVICE_ERROR_NO_INIT)) return R_NOCARD;
    if (e & SD_BLOCK_DEVICE_ERROR_PARAMETER) return R_PARAM;
    return R_IO;
}

const char *be_name(void) { return "carlk3"; }
bool be_present(void) { return sd_card_detect(&card); }

int be_init(be_info_t *info) {
    static bool drv;
    if (!drv) { sd_init_driver(); drv = true; }
    memset(info, 0, sizeof *info);
    DSTATUS st = card.init(&card);
    if (st & STA_NODISK) return R_NOCARD;
    if (st & STA_NOINIT) return R_TIMEOUT;
    info->sectors = card.get_num_sectors(&card);
    memcpy(info->cid, &card.state.CID, 16);
    info->cid_ok = true;
    return R_OK;
}

int be_read(uint32_t lba, uint8_t *buf, uint32_t n) { return cls_blk(card.read_blocks(&card, buf, lba, n)); }
int be_write(uint32_t lba, const uint8_t *buf, uint32_t n) { return cls_blk(card.write_blocks(&card, buf, lba, n)); }
int be_sync(void) { return cls_blk(card.sync(&card)); }
