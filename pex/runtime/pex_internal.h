/* Runtime internals shared between the pex_*.c files. Not for hosts. */
#ifndef PEX_INTERNAL_H
#define PEX_INTERNAL_H

#include "pex_rt.h"
#include "port/pex_port.h"

#ifndef PEX_MAX_DEPTH
#define PEX_MAX_DEPTH 16          /* nested module calls */
#endif
#ifndef PEX_MAX_TASKS
#define PEX_MAX_TASKS 32
#endif
#ifndef PEX_MAX_GROUPS
#define PEX_MAX_GROUPS 24
#endif
#ifndef PEX_MAX_SECTIONS
#define PEX_MAX_SECTIONS 24
#endif

#define PEX_OWNER_HOST 0u         /* allocations made by host code */
#define PEX_OWNER_RT   255u       /* runtime-owned shared data (shared code copies) */

typedef struct pex_code {
    uint8_t *base;
    uint32_t size;
    uint8_t build_id[16];
    uint16_t users;
    uint8_t owner;                /* the block's owner tag */
    uint8_t xip;                  /* base is in the PEX flash partition (not a pool block) */
} pex_code_t;

typedef struct { uint32_t hash, value; uint32_t name_off; } pex_export_t;

struct pex_mod {
    struct pex_mod *next;
    char name[24];
    uint8_t id;                   /* owner tag 1..254 */
    uint8_t state;                /* pex_state_t */
    uint8_t exit_pending, unload_pending;
    uint32_t flags, version, target;
    uint8_t build_id[16];
    pex_code_t *code;
    uint8_t *rw;
    uint32_t rw_size, data_size;
    uint32_t text_size, code_size, entry;
    uint32_t n_exports, n_fini;
    pex_export_t *exports;        /* in meta */
    uint32_t *fini;               /* image offsets */
    char *strings;                /* export names, description */
    const char *desc;
    void *meta;                   /* one runtime block holding exports, fini, strings */
    int active, refs, tasks;
    int exit_code;
    pex_host_t host;
    /* mem accounting (the module's own allocations) */
    uint32_t long_bytes, long_blocks, scratch_bytes, scratch_peak;
};

/* the parsed header (pex_load.c) */
typedef struct { uint16_t type, flags; uint32_t off, size; } pex__sec_t;
typedef struct {
    uint16_t fmaj, fmin, hsize, target, api_major, api_minor;
    uint32_t file_size, features, flags, version, image_size, text, ro, data, bss, stack, entry;
    uint32_t nsec, sec_off, image_off, crc;
    uint8_t build_id[16];
    pex__sec_t sec[PEX_MAX_SECTIONS];
    uint32_t sig_off;
    int sig;
} pex__hdr_t;
int pex__rread(const pex_reader_t *rd, uint32_t off, void *dst, uint32_t n);
int pex__parse_header(const pex_reader_t *rd, pex__hdr_t *h);
/* CRC (and, unless skip_sig, SHA-256 + the signature and the policy) */
int pex__integrity(const pex_reader_t *rd, const pex__hdr_t *h, bool skip_sig);
const pex__sec_t *pex__find_sec(const pex__hdr_t *h, uint16_t type);
/* META name (24 bytes incl. NUL): "" when there is none */
void pex__meta_name(const pex_reader_t *rd, const pex__hdr_t *h, char out[24]);

/* XIP (pex_xip.c): pex_load_installed() sets this around its pex_load() call */
typedef struct {
    uintptr_t code;               /* the code runs here (flash), already relocated (model A) */
    uintptr_t rw;                 /* != 0: RW must be here (model A in flash) */
    bool skip_sig;                /* verified when it was installed */
} pex__xload_t;
extern const pex__xload_t *pex__xload;
int pex__xip_mount_cfg(void);     /* from pex_init */
uint8_t *pex__pool_end(void);

/* globals */
extern pex_config_t pex__cfg;
extern pex_mod_t *pex__mods;
extern pex_mod_t *pex__stack[PEX_MAX_DEPTH];
extern uint32_t pex__depth;

pex_mod_t *pex__current(void);
uint8_t pex__current_owner(void);
void pex__err(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern int pex__errcode;
void pex__logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* mem */
void pex__mem_init(void *pool, uint32_t size);
void *pex__alloc(uint32_t size, uint32_t align, uint8_t owner, uint8_t sys);
void pex__free(void *p);
uint32_t pex__free_owner(uint8_t owner, uint32_t *bytes);   /* frees all of an owner's blocks */
uint32_t pex__scratch_release_owner(uint8_t owner);
bool pex__in_pool(const void *p);
pex_arena_t *pex__scratch_begin(uint32_t cap, uint8_t owner);
int pex__scratch_end(pex_arena_t *a);
void *pex__scratch_alloc(pex_arena_t *a, uint32_t size);
pex_mod_t *pex__mod_by_id(uint8_t id);

/* sched */
void pex__sched_init(void);
uint32_t pex__sched_cancel_owner(pex_mod_t *m);

/* the per-target trampoline (arch/pex_tramp_*.S) */
uint64_t pex_tramp(uint32_t sb, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
uintptr_t pex_get_sp(void);

/* integrity */
uint32_t pex_crc32_update(uint32_t crc, const void *p, uint32_t n);
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } pex_sha256_t;
void pex_sha256_init(pex_sha256_t *s);
void pex_sha256_update(pex_sha256_t *s, const void *p, uint32_t n);
void pex_sha256_final(pex_sha256_t *s, uint8_t out[32]);

uint32_t pex__host_target(void);
uint32_t pex__host_features(void);

#endif
