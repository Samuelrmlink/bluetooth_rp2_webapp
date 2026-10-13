#ifndef BTGW_MDNS_RECORDS_H
#define BTGW_MDNS_RECORDS_H

/*
 * mDNS record groups: A, PTR, SRV and TXT records for names that are NOT the
 * device's own (a printer behind the gateway, a service it relays), declared
 * from Lua with mdns.group{} and owned by the script that declared them.
 *
 * This file is the table and the wire format only -- no lwIP -- so
 * test/host/mdns_test.c checks the encoding and the answering directly.
 * src/net/mdns.c sends what it builds (from source port 5353, through the
 * hook in third_party/lwip-mdns) and announces/withdraws groups.
 *
 * Limits, chosen for ~3 KB of RAM: MREC_GROUPS groups, MREC_RECORDS records
 * across all of them (MREC_PER_GROUP in one), a name or target of up to
 * MREC_NAME_MAX characters ending in ".local", TXT items packed into
 * MREC_TXT_MAX bytes. No probing or conflict resolution: these are names the
 * script asserts.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MREC_GROUPS     8
#define MREC_RECORDS    16
#define MREC_PER_GROUP  8
#define MREC_NAME_MAX   63
#define MREC_TXT_MAX    63

#define MREC_A    1
#define MREC_PTR  12
#define MREC_TXT  16
#define MREC_SRV  33

#define MREC_TTL_HOST    120     /* A, SRV (RFC 6762 10) */
#define MREC_TTL_OTHER   4500    /* PTR, TXT */
#define MREC_TTL_LEGACY  10      /* answers to a legacy (non-5353) querier */

typedef struct {
    uint8_t  type;                     /* MREC_A/PTR/TXT/SRV; 0 = free */
    char     name[MREC_NAME_MAX + 1];
    uint8_t  a[4];                     /* A */
    uint16_t prio, weight, port;       /* SRV */
    char     data[MREC_TXT_MAX + 1];   /* PTR/SRV target, or TXT wire bytes */
    uint8_t  data_len;                 /* TXT: bytes used in data */
} mrec_t;

void mrec_reset(void);

/* A legal record name: labels of 1..63, total <= MREC_NAME_MAX, ending in
 * ".local", no empty label. */
bool mrec_name_ok(const char *name);
/* Pack "key=value" items as TXT wire data (length-prefixed). False if they
 * do not fit. */
bool mrec_txt_pack(const char *const *items, int n, mrec_t *r);

/* Add a group; returns its id (> 0), or -1 with *why. owner is a script
 * slot (or -1: nobody's, kept until removed). */
int  mrec_group_add(int owner, const mrec_t *recs, int n, const char **why);
bool mrec_group_remove(int id);
bool mrec_group_exists(int id);
int  mrec_group_count(void);
/* The ids a given owner holds, up to max. */
int  mrec_groups_of(int owner, int *ids, int max);
int  mrec_group_owner(int id);

size_t mrec_groups_json(char *dst, size_t cap);

/*
 * Answer a query (the whole UDP payload). Returns the response length, 0 if
 * no question is for one of our records. `legacy` (querier not on port 5353):
 * the ID and questions are echoed, TTLs capped, no cache-flush bit. *unicast
 * is set when a question asked for a unicast reply (the QU bit).
 */
size_t mrec_build_answer(const uint8_t *q, size_t qlen, bool legacy,
                         uint8_t *out, size_t cap, bool *unicast);

/* An unsolicited response carrying every record of group `id` with `ttl`
 * (0 = goodbye; MREC_TTL_DEFAULT = each record's normal TTL). */
#define MREC_TTL_DEFAULT 0xFFFFFFFFu
size_t mrec_build_announce(int id, uint32_t ttl, uint8_t *out, size_t cap);

#endif
