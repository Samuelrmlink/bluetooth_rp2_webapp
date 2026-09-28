/*
 * SMP -- the Simple Management Protocol that MCUmgr speaks.
 *
 * One 8-byte header, one CBOR map. Groups are numbered (os 0, image 1, stat
 * 2, fs 8); each command is a (group, id, read|write) triple. Transports are
 * dumb: they hand a frame to smp_process() and send back whatever it fills.
 * Everything here runs from the MAIN LOOP (ota_tick), never from an lwIP
 * callback, so a handler may write flash directly.
 *
 * Errors: SMP v2 puts them in {"err":{"group":g,"rc":rc}}, v1 as {"rc":rc}.
 * A handler returns 0, a generic MGMT_ERR_* code, or SMP_GERR(rc) for a code
 * defined by its own group; the wrapper encodes it for the requester's
 * version. Successful v1 replies also carry "rc":0, which the Go client's
 * structs expect and every other client ignores.
 */
#ifndef OTA_SMP_H
#define OTA_SMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ota/cbor.h"

/* One datagram each way. mcumgr params advertises this as buf_size, and
 * smpclient sizes its chunks from it. 1 KB chunks over a 1200 MTU WireGuard
 * path is the design point; a full-size frame still fits. */
#define SMP_BUF 1600

enum { SMP_OP_READ = 0, SMP_OP_READ_RSP = 1, SMP_OP_WRITE = 2, SMP_OP_WRITE_RSP = 3 };
enum { SMP_GROUP_OS = 0, SMP_GROUP_IMAGE = 1, SMP_GROUP_STAT = 2, SMP_GROUP_FS = 8 };

/* Generic codes (mgmt/mgmt_defines.h in Zephyr). */
enum {
    MGMT_ERR_EOK = 0, MGMT_ERR_EUNKNOWN = 1, MGMT_ERR_ENOMEM = 2, MGMT_ERR_EINVAL = 3,
    MGMT_ERR_ETIMEOUT = 4, MGMT_ERR_ENOENT = 5, MGMT_ERR_EBADSTATE = 6,
    MGMT_ERR_EMSGSIZE = 7, MGMT_ERR_ENOTSUP = 8, MGMT_ERR_ECORRUPT = 9,
    MGMT_ERR_EBUSY = 10, MGMT_ERR_EACCESSDENIED = 11,
};
#define SMP_GERR(rc) (0x100 | (rc))      /* a group-specific code */

typedef struct {
    uint8_t op, ver, flags;
    uint16_t len, group;
    uint8_t seq, id;
    const uint8_t *payload;              /* the CBOR map */
    size_t plen;
} smp_req_t;

/* A handler adds its fields to the already-open response map. */
typedef int (*smp_handler_t)(const smp_req_t *req, cbor_enc_t *rsp);

typedef struct {
    uint8_t group_lo;                    /* group number (fits a byte here) */
    uint8_t id;
    bool write;
    smp_handler_t fn;
} smp_cmd_t;

/* Process one frame; returns the response length (0 = nothing to send). */
size_t smp_process(const uint8_t *frame, size_t n, uint8_t *out, size_t cap);

/* Group tables, one per file. Terminated by fn == NULL. */
extern const smp_cmd_t smp_os_cmds[];
extern const smp_cmd_t smp_img_cmds[];
extern const smp_cmd_t smp_fs_cmds[];
extern const smp_cmd_t smp_stat_cmds[];

/* Per-pass housekeeping for the groups that keep state (fs idle-close,
 * deferred reset). Called from ota_tick(). */
void smp_os_tick(uint32_t now_ms);
void smp_fs_tick(uint32_t now_ms);

/* Whole-server entry points, called from main.c. */
void ota_init(void);
void ota_tick(void);

#endif
