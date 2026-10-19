/* The SD card over HTTP: status, and file transfers that run in the storage
 * worker. Ported from km003c_rp2_webapp (src/log/sd_api.h); the HTTP side is
 * src/net/http_sd.c. It runs in the main context and never touches the card:
 *
 *   sd_api_gate()        refuse at once (503/...) from the published state
 *   sd_xfer_start()      claim a transfer slot and hand the request to the
 *                        worker; -1 = no slot (503 card busy)
 *   sd_xfer_state()      OPENING -> STREAM (headers can go: size, type)
 *                        -> DONE / FAIL
 *   sd_xfer_peek/consume download and listing bytes, as the socket takes them
 *   sd_xfer_put/end_input  upload bytes, as the worker takes them
 *   sd_xfer_release()    the HTTP side is done with it (or the connection
 *                        went away): the worker stops and cleans up
 *
 * Each slot owns a ring buffer: the worker fills it from the card (GET) or
 * drains it to the card (PUT), so neither side waits on the other for more
 * than a buffer's worth. */
#ifndef BTGW_SD_API_H
#define BTGW_SD_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "autoconf.h"

#ifdef CONFIG_SD_XFERS
#define SD_XFERS CONFIG_SD_XFERS
#define SD_XFER_BUF CONFIG_SD_XFER_BUF
#endif
#ifndef SD_XFERS
#define SD_XFERS 2         /* transfers at once */
#endif
#ifndef SD_XFER_BUF
#define SD_XFER_BUF 4096   /* per transfer; a power of two */
#endif
#define SD_PATH_MAX 96

enum sd_op {
    SD_OP_GET = 1, /* a file's contents, or a directory's JSON listing */
    SD_OP_PUT,     /* upload; creates parent directories */
    SD_OP_DELETE,  /* a file or an empty directory */
    SD_OP_RENAME,
    SD_OP_BENCH, /* the card alone: write then read a file, no network */
    SD_OP_MKDIR, /* a folder and its parents */
};

enum sd_xstate {
    SDX_FREE = 0,
    SDX_OPENING, /* posted; the worker has not looked yet */
    SDX_STREAM,  /* GET: headers known, bytes flowing; PUT: taking bytes */
    SDX_DONE,    /* finished; sd_xfer_result() is the answer */
    SDX_FAIL,    /* failed: before STREAM, reply with the result; after, abort the connection */
};

/* An answer the HTTP side sends as {"ok":false,"error":"...","card":"..."}
 * (or {"ok":true,...} for status 200 with body). */
struct sd_reply {
    int status;      /* HTTP status */
    int retry_after; /* seconds, 0 = no header */
    char error[64];
    const char *card; /* card state name */
};

/* ---- main context ---- */

/* The card's state as one word: "none", "settling", "error", "formatting",
 * "ok". */
const char *sd_card_state(void);
/* False, with *r filled, when the card cannot take a file request now. */
bool sd_api_gate(struct sd_reply *r);
/* GET /api/sd. Always answerable. Returns length, or -1 if it did not fit. */
int sd_api_status_json(char *buf, size_t cap);
/* GET /api/sd?reset=1: the maxima in "worker" start over after this read. */
void sd_api_stats_reset(void);

/* Validates and normalises an API path ("logs/2026", "" = the root) in
 * place: no "..", no "\\", no empty segments, no control characters, no
 * leading/trailing '/'. Percent-escapes are decoded. */
bool sd_path_clean(char *path, size_t cap);

/* -1 with *r filled (busy, bad path) or a slot. `len` is the upload's
 * Content-Length (PUT). path2 is the rename target. */
int sd_xfer_start(enum sd_op op, const char *path, const char *path2, uint64_t len, struct sd_reply *r);
enum sd_xstate sd_xfer_state(int x);
/* GET, from SDX_STREAM: UINT64_MAX for a listing (no Content-Length, the
 * body ends with the connection). */
uint64_t sd_xfer_size(int x);
const char *sd_xfer_mime(int x);
const char *sd_xfer_name(int x); /* the file's own name, for Content-Disposition */
/* DONE / FAIL: the answer. A PUT's 200 body is built by the caller from
 * sd_xfer_size() (bytes written). */
void sd_xfer_result(int x, struct sd_reply *r);
/* Download: the readable bytes, contiguous; 0 when none just now. */
size_t sd_xfer_peek(int x, const uint8_t **p);
void sd_xfer_consume(int x, size_t n);
/* Upload: bytes taken (0 when the buffer is full just now). */
size_t sd_xfer_put(int x, const uint8_t *p, size_t n);
void sd_xfer_end_input(int x);
void sd_xfer_release(int x);
/* Transfers in flight (for the status). */
int sd_xfer_active(void);
/* Diagnostics: how the HTTP side saw a transfer's connection end
 * ("lwip err -14", "client closed", "poll timeout", ...). Shown in
 * GET /api/sd worker.last_conn_end with the bytes the transfer had moved. */
void sd_xfer_note_end(int x, const char *why, int code);

/* ---- format ---- */

enum { SDF_IDLE = 0, SDF_RUNNING, SDF_DONE, SDF_ERROR };
struct sd_format_status {
    int state;
    const char *phase; /* "closing files", "writing", "mounting" */
    uint32_t done, total; /* sectors (total is an estimate) */
    int fs;               /* LS_FMT_* */
    uint32_t cluster;
    char error[48];
};
void sd_format_get(struct sd_format_status *out);
/* The "format", "card", "defaults" and "options" members of GET /api/sd
 * (no braces around them). */
int sd_format_json(char *buf, size_t cap, const struct sd_format_status *f);
/* POST /api/sd/format. fs: "fat32" / "exfat"; cluster: bytes, 0 = default.
 * True = 202 accepted; false with *r: 409 already running, 400 bad
 * choice, 503 no card. */
bool sd_format_start(const char *fs, uint32_t cluster, struct sd_reply *r);
bool sd_format_running(void);

/* POST /api/sd/bench {"mb":N}: the card's own speed through this driver;
 * results in GET /api/sd "bench". */
bool sd_bench_start(uint32_t mb, struct sd_reply *r);
/* Defaults for a card of `bytes`. */
int sd_default_fs(uint64_t bytes);
uint32_t sd_default_cluster(int fs, uint64_t bytes);
uint32_t sd_default_align(uint64_t bytes);
/* Whether FatFs will take that cluster size for that card, by its count
 * limits; err says why not. */
bool sd_cluster_ok(int fs, uint32_t cluster, uint64_t bytes, const char **err);

/* ---- worker (sd_core.c's loop) ---- */
void sd_xfer_run(int x);
void sd_format_run(void);

#endif
