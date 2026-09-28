#include "ota/smp.h"
#include <stdio.h>
#include <string.h>

static const smp_cmd_t *find(uint16_t group, uint8_t id, bool write) {
    const smp_cmd_t *tables[] = { smp_os_cmds, smp_img_cmds, smp_stat_cmds, smp_fs_cmds };
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++) {
        for (const smp_cmd_t *c = tables[t]; c->fn; c++) {
            if (c->group_lo == group && c->id == id && c->write == write) return c;
        }
    }
    return NULL;
}

static bool group_known(uint16_t group) {
    return group == SMP_GROUP_OS || group == SMP_GROUP_IMAGE ||
           group == SMP_GROUP_STAT || group == SMP_GROUP_FS;
}

static size_t finish(uint8_t *out, const smp_req_t *req, size_t body) {
    out[0] = (uint8_t)((req->op + 1) & 7) | (uint8_t)(req->ver << 3);
    out[1] = 0;
    out[2] = (uint8_t)(body >> 8);
    out[3] = (uint8_t)body;
    out[4] = (uint8_t)(req->group >> 8);
    out[5] = (uint8_t)req->group;
    out[6] = req->seq;
    out[7] = req->id;
    return 8 + body;
}

static size_t error_rsp(uint8_t *out, size_t cap, const smp_req_t *req, int rc) {
    cbor_enc_t e;
    cbor_enc_init(&e, out + 8, cap - 8);
    cbor_map_open(&e);
    if (req->ver >= 1) {
        cbor_tstr(&e, "err");
        cbor_map_open(&e);
        /* A group-specific code names its group; a generic one is "group 0"
         * by convention (the SMP/OS group owns MGMT_ERR_*). */
        cbor_kv_uint(&e, "group", (rc & 0x100) ? req->group : 0);
        cbor_kv_uint(&e, "rc", (uint64_t)(rc & 0xff));
        cbor_close(&e);
    } else {
        /* v1 has no group codes: anything specific collapses to EUNKNOWN. */
        cbor_kv_uint(&e, "rc", (rc & 0x100) ? MGMT_ERR_EUNKNOWN : (uint64_t)rc);
    }
    cbor_close(&e);
    return finish(out, req, e.len);
}

size_t smp_process(const uint8_t *frame, size_t n, uint8_t *out, size_t cap) {
    if (n < 8 || cap < 8 + 16) return 0;
    smp_req_t req = {
        .op = frame[0] & 7, .ver = (frame[0] >> 3) & 3, .flags = frame[1],
        .len = (uint16_t)((frame[2] << 8) | frame[3]),
        .group = (uint16_t)((frame[4] << 8) | frame[5]),
        .seq = frame[6], .id = frame[7],
        .payload = frame + 8, .plen = n - 8,
    };
    if (req.op != SMP_OP_READ && req.op != SMP_OP_WRITE) return 0;   /* a response: not ours */
    if (req.len > req.plen) return error_rsp(out, cap, &req, MGMT_ERR_EINVAL);
    req.plen = req.len;

    if (!group_known(req.group)) return error_rsp(out, cap, &req, MGMT_ERR_ENOTSUP);
    const smp_cmd_t *c = find(req.group, req.id, req.op == SMP_OP_WRITE);
    if (!c) return error_rsp(out, cap, &req, MGMT_ERR_ENOTSUP);

    cbor_enc_t e;
    cbor_enc_init(&e, out + 8, cap - 8);
    cbor_map_open(&e);
    if (req.ver == 0) cbor_kv_uint(&e, "rc", 0);
    int rc = c->fn(&req, &e);
    if (rc != 0) return error_rsp(out, cap, &req, rc);
    cbor_close(&e);
    if (e.err) return error_rsp(out, cap, &req, MGMT_ERR_EMSGSIZE);
    return finish(out, &req, e.len);
}
