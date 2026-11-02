/* File operations on the SD card for main-context callers that are not
 * HTTP transfers: the Lua `file` module and the HTTP client's to_file.
 *
 * The rule of src/sd stands -- only the storage worker touches the card -- so
 * these are JOBS: the caller posts one, the worker runs it on its next turn
 * (sd_core_worker_body), and the caller polls for the result. A Lua binding
 * yields its script while it waits, so a slow card holds up that script and
 * nothing else.
 *
 * ONE job at a time, with one 4 KiB data buffer. That is deliberate rather
 * than lazy: the buffer is not on the Lua heap, so a script that dies
 * mid-read (or a Lua VM restart, which frees the whole arena) can never leave
 * the worker writing into memory that has been reused, and the worker
 * serialises card I/O anyway. The buffer is one of the HTTP transfer rings
 * (sd_xfer_borrow), taken only by READ/WRITE/LIST for the job's lifetime: the
 * SD build had ~12 KB of C heap left and no room for another 4 KB. A second
 * caller, or every ring busy with browser transfers, makes sdj_post() fail;
 * the caller tries again next pass.
 *
 * Open files are indices into a small table (SDJ_FILES) on the worker side,
 * tagged with the mount generation: after the card is pulled and remounted a
 * stale index answers LS_EIO instead of reaching whatever reused the slot.
 * sdj_close_async() closes one without a job (from __gc, a dying script, an
 * abandoned open), on the worker's next turn.
 */
#ifndef BTGW_SD_JOBS_H
#define BTGW_SD_JOBS_H

#include <stdbool.h>
#include <stdint.h>

#define SDJ_FILES    4
#include "sd/sd_api.h"
#include "sd/sd_store.h"   /* LS_RD / LS_WR / LS_APPEND for SDJ_OPEN */
/* The data buffer is an idle HTTP transfer ring, borrowed for the job. */
#define SDJ_BUF      (SD_XFER_BUF < 4096 ? SD_XFER_BUF : 4096)
#define SDJ_PATH_MAX 128

enum sdj_op {
    SDJ_OPEN = 1,  /* path, mode (LS_RD / LS_WR / LS_APPEND) -> rc = file index */
    SDJ_READ,      /* file, len -> rc = bytes in sdj_buf() */
    SDJ_WRITE,     /* file, len bytes from sdj_buf() -> rc = bytes */
    SDJ_SEEK,      /* file, pos */
    SDJ_CLOSE,     /* file */
    SDJ_STAT,      /* path -> is_dir, size */
    SDJ_LIST,      /* path, pos = entries to skip -> rc = entries in sdj_buf(), more */
    SDJ_MKDIR,     /* path and its parents; an existing directory is success */
    SDJ_REMOVE,    /* a file or an empty directory */
    SDJ_RENAME,    /* path -> path2; LS_EEXIST if path2 exists */
    SDJ_STATFS,    /* -> free, total */
};

enum sdj_state { SDJ_IDLE = 0, SDJ_POSTED, SDJ_DONE, SDJ_FILLING };

typedef struct {
    int op;
    int file;
    int mode;
    char path[SDJ_PATH_MAX + 1];
    char path2[SDJ_PATH_MAX + 1];
    uint32_t len;
    uint64_t pos;
    /* results */
    int rc;                 /* >= 0 success (bytes / index / count), else LS_E* */
    bool is_dir, more;
    uint64_t size, tell;    /* file ops: the file's size and position afterwards */
    uint64_t free_bytes, total_bytes;
} sdj_t;

/* REMOVE of a directory that is not empty (FatFs says "denied"). */
#define SDJ_ENOTEMPTY (-20)

/* LIST entries in sdj_buf(): [u8 is_dir][u8 name_len][u32 size, LE][name]. */
#define SDJ_LIST_HDR 6

/* ---- main context ---- */
/* Claims the job slot and posts `req` (op and arguments). False when a job
 * is already in flight: try again on a later pass. *token identifies this
 * job to sdj_poll/sdj_abandon. */
bool sdj_post(const sdj_t *req, uint32_t *token);
/* A WRITE filled in place: claims the job and its buffer (state FILLING,
 * which the worker leaves alone), the caller copies up to SDJ_BUF bytes into
 * sdj_buf() as they arrive, and sdj_commit() posts it with that length. For
 * a stream (an HTTP download) this needs no buffer of the caller's own. */
bool sdj_begin_write(int file, uint32_t *token);
void sdj_commit(uint32_t token, uint32_t len);

/* The data buffer, valid from a successful sdj_post of READ/WRITE/LIST until
 * sdj_finish(): fill it right after posting a WRITE (the worker only runs
 * between main-loop passes); read it after a READ or LIST completes. */
uint8_t *sdj_buf(void);
/* SDJ_DONE with the result, SDJ_POSTED while it runs; SDJ_IDLE if the token
 * is not the current job (finished and freed, or never posted). */
enum sdj_state sdj_poll(uint32_t token, sdj_t *out);
/* The caller is done with a finished job: the slot is free again. */
void sdj_finish(uint32_t token);
/* The caller is gone (script stopped). The job runs to completion and frees
 * itself; a file it opened is closed. */
void sdj_abandon(uint32_t token);
/* Close a file without a job (worker's next turn). */
void sdj_close_async(int file);

/* ---- worker ---- */
/* Runs the posted job and any deferred closes. True when it did something. */
bool sdj_run(void);

#endif
