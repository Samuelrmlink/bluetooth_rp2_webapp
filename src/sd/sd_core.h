#ifndef BTGW_SD_CORE_H
#define BTGW_SD_CORE_H
/*
 * The SD card's place in the firmware: sd_init() at boot starts the storage
 * worker; sd_poll() in the main loop gives it its turn. The worker keeps the
 * card mounted (card detect, settle, remount after a pull), runs the HTTP
 * transfers and the format, and publishes the card's state for the main
 * context (sd_pub(); single core, cooperative: plain reads are safe).
 */
#include <stdbool.h>
#include <stdint.h>

struct sd_pub {
    bool        present;
    int         media;          /* enum ls_media */
    const char *fs;             /* "fat32", ...; "-" */
    uint64_t    total_bytes, free_bytes;
    bool        formatting;
    uint32_t    errors;         /* mount/IO failures since boot */
};

void sd_init(void);
void sd_poll(void);
const struct sd_pub *sd_pub(void);

/* Worker context. */
void sd_core_media_tick(void);       /* mount / notice a pulled card */
void sd_core_release_card(void);     /* unmount for a format */
void sd_core_worker_body(void);      /* the loop (host tests call it through sdw_start) */

#endif
