/* Random-access read of an already-open SD job file. The caller borrows a
 * free transfer ring before the first read: each read posts SDJ_SEEK / SDJ_READ
 * and pumps sdw_service() until that job finishes. The window is SDJ_BUF. */
#ifndef BTGW_PEX_SD_READ_H
#define BTGW_PEX_SD_READ_H

#include <stdint.h>

#if defined(PEX_SD_READ_TEST)
#include <stdbool.h>
/* Stand-ins for sd_jobs.h. sd_jobs.h includes the firmware autoconf, so the
 * host test compiles this file with -DPEX_SD_READ_TEST instead. Layout is
 * shared with the stubs in pex_xip_unit.c. */
enum sdj_op { SDJ_OPEN = 1, SDJ_READ, SDJ_WRITE, SDJ_SEEK, SDJ_CLOSE };
enum sdj_state { SDJ_IDLE = 0, SDJ_POSTED, SDJ_DONE, SDJ_FILLING };
typedef struct {
    int op;
    int file;
    int mode;
    int rc;
    uint32_t len;
    uint64_t pos;
    uint64_t size;
} sdj_t;
bool sdj_post(const sdj_t *req, uint32_t *token);
enum sdj_state sdj_poll(uint32_t token, sdj_t *out);
void sdj_finish(uint32_t token);
uint8_t *sdj_buf(void);
void sdw_service(void);
#define SDJ_BUF 4096
#endif

typedef struct {
    int file;
    uint32_t size;
    uint32_t pos;
    int pos_valid;
} pex_sd_cursor_t;

/* pex_reader_t read(): bytes, or -1. Short reads are an error once the caller
 * asked for a range inside `size`. */
int pex_sd_pread(void *ctx, uint32_t off, void *dst, uint32_t n);

#endif
