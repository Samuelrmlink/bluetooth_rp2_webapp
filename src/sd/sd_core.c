/*
 * The storage worker's loop: card presence and mounting, then whatever the
 * HTTP side posted (transfer slots in OPENING, a format), then idle. The
 * km003c logger did the media part (logw_tick, media_error); without a
 * logger it lives here.
 */
#include "sd/sd_core.h"

#include "sd/sd_api.h"
#include "sd/sd_jobs.h"
#include "sd/sd_port.h"
#include "sd/sd_store.h"
#include "sd/sd_worker.h"

#define MEDIA_RETRY_US 500000u      /* try to mount again this often */
#define FREE_REFRESH_US 5000000u    /* free space, now and then (FAT scan) */

static struct sd_pub pub = { .fs = "-" };
static uint64_t next_mount_us, next_free_us;

const struct sd_pub *sd_pub(void) { return &pub; }

static void publish_free(void) {
    uint64_t fr = 0, tot = 0;
    if (ls_statfs(&fr, &tot) == LS_OK) {
        pub.free_bytes = fr;
        pub.total_bytes = tot;
    }
}

void sd_core_media_tick(void) {
    sdw_set_phase("media");                          /* detect, mount, free space */
    uint64_t now = sd_port_now_us();
    pub.present = ls_present();
    if (ls_media() == LS_MEDIA_OK) {
        if (!pub.present) {                          /* pulled */
            ls_unmount();
            pub.errors++;
        } else if (now >= next_free_us) {
            publish_free();
            next_free_us = now + FREE_REFRESH_US;
        }
    } else if (now >= next_mount_us) {
        int r = ls_mount();
        next_mount_us = now + MEDIA_RETRY_US;
        if (r == LS_OK) {
            publish_free();
            next_free_us = now + FREE_REFRESH_US;
        } else if (r != LS_ENOMEDIA && r != LS_EBUSY) {
            pub.errors++;
        }
    }
    pub.media = ls_media();
    pub.fs = ls_fs_name();
    if (pub.media != LS_MEDIA_OK) { pub.free_bytes = pub.total_bytes = 0; }
}

void sd_core_release_card(void) {
    pub.formatting = true;
    ls_unmount();
    pub.media = ls_media();
    pub.fs = ls_fs_name();
    next_mount_us = 0;
}

void sd_core_worker_body(void) {
    for (;;) {
        sd_core_media_tick();
        if (sd_format_running()) {
            sdw_set_phase("format");
            sd_format_run();
            pub.formatting = false;
            next_mount_us = 0;
            next_free_us = 0;
            continue;
        }
        bool worked = false;
        for (int x = 0; x < SD_XFERS; x++) {
            if (sd_xfer_state(x) == SDX_OPENING) {
                sd_xfer_run(x);
                worked = true;
                next_free_us = 0;                    /* a write may have changed it */
            }
        }
        /* The Lua file module's and the HTTP client's jobs, after the
         * transfers: a browser upload is not held up by a script. */
        if (sdj_run()) {
            worked = true;          /* free space: the 5 s refresh catches up */
        }
        if (!worked) {
            sdw_set_phase("idle");
            sdw_idle(20000);
        }
    }
}

void sd_init(void) {
    sdw_start(sd_core_worker_body);
}

void sd_poll(void) {
    sdw_service();
}
