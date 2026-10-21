/*
 * pex_rt.h -- the PEX runtime, host side (spec docs/pex-format.md).
 *
 * Portable C11 with no dependency on Lua or on any gateway subsystem: what a
 * host must provide is listed in port/pex_port.h (time, a log line sink, a
 * code-sync barrier, optionally stack bounds and a signature verifier). The
 * per-target part is one small assembly file (the trampoline, arch/).
 *
 *   static uint8_t pool[48 * 1024] __attribute__((aligned(8)));
 *   pex_config_t cfg = { .pool = pool, .pool_size = sizeof pool,
 *                        .policy = PEX_POLICY_ALLOW_UNSIGNED, .host_name = "me" };
 *   pex_init(&cfg);
 *   pex_mod_t *m = pex_load_mem(buf, len, NULL, &err);   // PEX_INIT runs here
 *   pex_start(m, "{\"ticks\":5}");                       // PEX_APP_RUN
 *   for (;;) { pex_poll(); ... }                         // sched tasks
 *   pex_stop(m); pex_unload(m, why, sizeof why);
 *
 * Single-threaded: call everything from the one main loop (core 0). A module
 * runs only inside pex_call()/pex_poll()/the load and unload calls.
 */
#ifndef PEX_RT_H
#define PEX_RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef PEX_HOST_SIDE
#define PEX_HOST_SIDE 1      /* pex_api.h: libc comes from <string.h> here */
#endif
#include "pex.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PEX_RT_VERSION "0.6"

/* ---- host API tables (generated from host_api.json: pex_host_<group>.c) -- */
typedef struct { uint32_t hash; const void *addr; } pex_hsym_t;
typedef struct pex_group {
    uint16_t id;
    uint8_t major, minor;
    uint16_t count;
    const pex_hsym_t *syms;     /* sorted by hash */
    const char *name;
} pex_group_t;

extern const pex_group_t pex_group_core, pex_group_log, pex_group_mem, pex_group_sched, pex_group_pex,
    pex_group_json;

/* A host adds the optional groups it implements (board, lua, bt, fs, ...). */
int pex_register_group(const pex_group_t *g);
const pex_group_t *pex_find_group(uint16_t id);

/* ---- configuration ------------------------------------------------------- */
#define PEX_POLICY_ALLOW_UNSIGNED 1u    /* development: unsigned modules load (say so loudly) */

/* The PEX flash partition (XIP, spec 16): installed modules run in place.
 * Offsets are partition offsets. erase: one 4 KB sector; prog: n bytes, n a
 * multiple of 256 at a 256-aligned offset (erased before). Both return 0 on
 * success; they must leave the flash readable (and the cache coherent) at
 * read_base and executable at exec_base when they return. */
typedef struct {
    uint32_t size;               /* bytes, a multiple of 4096, >= 16 KB */
    uintptr_t exec_base;         /* partition byte 0 as code is fetched (cached XIP) */
    uintptr_t read_base;         /* partition byte 0 as data is read (may equal exec_base) */
    int (*erase)(void *ctx, uint32_t off);
    int (*prog)(void *ctx, uint32_t off, const void *src, uint32_t n);
    void *ctx;
} pex_flash_t;

typedef int (*pex_verify_fn)(uint16_t alg, uint16_t key_id, const uint8_t digest[32],
                             const uint8_t *sig, uint32_t sig_len);

typedef struct {
    void *pool;                 /* the PEX pool (8-aligned): images, RW, long-term, scratch */
    uint32_t pool_size;
    uint32_t policy;            /* PEX_POLICY_* */
    pex_verify_fn verify;       /* NULL: a signed module cannot be verified -> refused */
    const char *host_name;      /* passed to modules in pex_host_t */
    volatile uint32_t *trace;   /* optional: holds the first word of the executing module's
                                   build id while module code runs (survives a reset: hang attribution) */
    uint32_t max_stack;         /* 0 = no check of the header's stack_hint */
    /* Called at unload (also of a faulted module), after PEX_DEINIT, before its
     * memory goes: host groups release what the module registered with them. */
    void (*on_unload)(pex_mod_t *m);
    const pex_flash_t *flash;   /* NULL: no XIP partition (RAM mode only) */
    bool stack_scan;            /* debug option: also refuse unload when a word on the stack looks like a
                                   return address into the image. The trampoline's active count is the
                                   real check; live frames can hold STALE return addresses, so this
                                   safety net gives false refusals and is off by default. */
} pex_config_t;

int pex_init(const pex_config_t *cfg);
void pex_set_policy(uint32_t policy);
uint32_t pex_get_policy(void);

/* ---- loading -------------------------------------------------------------- */
/* Random-access reader: a file, flash, a buffer. read() returns bytes read. */
typedef struct {
    int (*read)(void *ctx, uint32_t off, void *dst, uint32_t n);
    void *ctx;
    uint32_t size;              /* file size, or 0 if unknown */
} pex_reader_t;

typedef struct {
    const char *name;           /* instance name; default META name (then "<name>#2" ...) */
    bool no_init;               /* link only, do not call pex_main(INIT) */
} pex_load_opts_t;

pex_mod_t *pex_load(const pex_reader_t *rd, const pex_load_opts_t *opts, int *err);
pex_mod_t *pex_load_mem(const void *buf, uint32_t len, const pex_load_opts_t *opts, int *err);
/* Why the last load/unload/start failed, in words (names the symbol, group, ...). */
const char *pex_errmsg(void);
/* For host code that refuses on the runtime's behalf (shows up in pex_errmsg). */
void pex_seterr(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const char *pex_strerror(int err);

/* Check a file without loading it (prefix, header, target, api, CRC, signature). */
int pex_check(const pex_reader_t *rd);

/* ---- lifecycle ------------------------------------------------------------ */
int pex_start(pex_mod_t *m, const char *cfg_json);  /* PEX_APP_RUN */
int pex_stop(pex_mod_t *m);                         /* PEX_APP_STOP + cancel its tasks */
/* Refused (PEX_EBUSY, why in errmsg) while active on a stack, referenced, or running. */
int pex_unload(pex_mod_t *m);
int pex_unload_all(void);                            /* stop + unload everything */

pex_mod_t *pex_find(const char *name);
pex_mod_t *pex_first(void);
pex_mod_t *pex_next(pex_mod_t *m);

/* ---- calling into a module ---------------------------------------------- */
uint32_t pex_sym(pex_mod_t *m, const char *name);    /* export address (Thumb bit set) or 0 */
/* Up to four word arguments; returns r0 | r1 << 32. Through the trampoline:
 * bumps active, sets r9 for PIC, keeps the executing-module stack. */
uint64_t pex_call(pex_mod_t *m, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
#define pex_call0(m, f) ((uint32_t)pex_call((m), (f), 0, 0, 0, 0))
#define pex_call1(m, f, a) ((uint32_t)pex_call((m), (f), (uint32_t)(a), 0, 0, 0))
#define pex_call2(m, f, a, b) ((uint32_t)pex_call((m), (f), (uint32_t)(a), (uint32_t)(b), 0, 0))
/* pex_main(reason, arg) of m. */
int pex_entry(pex_mod_t *m, uint32_t reason, void *arg);

/* Nesting depth of module calls right now, and unwinding after a longjmp out of
 * module code (a Lua error raised inside a module, a recovered fault). */
uint32_t pex_depth(void);
void pex_unwind(uint32_t depth);

/* ---- scheduler (the sched group) ------------------------------------------ */
/* Run due tasks and deferred work. Call once per main-loop pass. */
void pex_poll(void);

/* ---- introspection -------------------------------------------------------- */
typedef enum { PEX_ST_LOADED = 0, PEX_ST_RUNNING, PEX_ST_EXITED, PEX_ST_STOPPED, PEX_ST_FAULTED } pex_state_t;

typedef struct {
    const char *name, *desc;
    pex_state_t state;
    uint32_t flags, version, target;
    const uint8_t *build_id;     /* 16 bytes */
    uintptr_t code, rw;          /* where they are */
    uint32_t code_size, rw_size;
    bool code_shared;            /* model B: another instance owns the same code copy */
    bool xip;                    /* the code runs in place from the PEX flash partition */
    int active, refs, tasks;
    int exit_code;
    uint32_t n_exports;
    pex_mem_stats_t mem;         /* the module's long-term / scratch accounting */
} pex_info_t;
int pex_info(pex_mod_t *m, pex_info_t *out);
const char *pex_state_name(pex_state_t s);
const char *pex_export_name(pex_mod_t *m, uint32_t i);   /* i < n_exports */
int pex_pool_stats(pex_mem_stats_t *out);                /* the whole pool */
int pex_list_json(char *dst, size_t cap);                /* [{"name":..,"state":..}, ...] */
int pex_info_json(pex_mod_t *m, char *dst, size_t cap);

/* Pool memory owned by the host (not by whichever module is executing). */
void *pex_alloc_host(size_t size);

/* ---- faults --------------------------------------------------------------- */
typedef struct {
    bool valid;
    char name[24];
    uint8_t build_id[16];
    uint32_t pc, lr;
    uint32_t offset;             /* of the faulting PC in the image (or of LR, in_host) */
    bool in_host;                /* PC was in host code called from the module */
} pex_fault_t;
/* From a HardFault handler: frame = the stacked r0-r3,r12,lr,pc,xpsr. If the
 * fault belongs to a module (PC in an image, or a module on the call stack),
 * fills *out, marks the module FAULTED, logs "PEX FAULT ..." and returns true. */
bool pex_fault_attribute(const uint32_t *frame, pex_fault_t *out);
const pex_fault_t *pex_last_fault(void);
/* Which module (if any) contains addr; *off = image offset. */
pex_mod_t *pex_mod_at(uint32_t addr, uint32_t *off);

/* ---- XIP: install into the flash partition, run in place (spec 16) ------- *
 * The partition holds an index (two copies with sequence numbers: a power cut
 * during any update leaves the previous index valid) and whole .pex files,
 * each at a 4 KB boundary. Model B is stored as signed; model A is stored
 * with its RO words relocated to its flash address (and to a fixed RW
 * address at the top of the pool), and is re-relocated when it moves.
 * Every update is copy-then-commit; compaction slides files down with a
 * journal in the index, resumed by pex_xip_mount() after a power cut. */
#define PEX_INSTALL_NO_COMPACT 1u    /* fail with PEX_ENOSPC rather than move other files */

/* Install a file (validated first: CRC, signature, policy -- an unsigned
 * file is refused under signed-only). Same build already installed: returns
 * its slot. An installed module of the same name is replaced (refused while
 * it is loaded from flash). Returns the slot (>= 0) or a PEX_E* code. */
int pex_install(const pex_reader_t *rd, uint32_t flags);
int pex_install_mem(const void *buf, uint32_t len, uint32_t flags);
int pex_uninstall(const char *name);  /* PEX_EBUSY while loaded from flash */

typedef struct {
    char name[24];
    uint8_t build_id[16];
    uint32_t version, flags, target;
    uint32_t offset, file_size;    /* in the partition */
    uint32_t code_size, rw_size;
    uintptr_t code;                /* where its code runs */
    bool signed_, loaded;          /* loaded: a module runs from this copy now */
    bool xip_ready;                /* model A: its RW address is reserved (else it loads into RAM until a reboot) */
} pex_installed_t;
int pex_installed(int i, pex_installed_t *out);            /* i-th; PEX_ENOENT past the end */
int pex_installed_find(const char *name, pex_installed_t *out);   /* slot, or PEX_ENOENT */
/* Load an installed module: code in place, RW from the pool. Falls back to a
 * RAM copy (logged) when the copy cannot run in place (model A whose RW
 * address is taken). */
pex_mod_t *pex_load_installed(const char *name, const pex_load_opts_t *opts, int *err);
int pex_xip_verify(const char *name);  /* CRC + signature of the installed copy, in place */
int pex_xip_compact(void);             /* files moved, or PEX_E*; loaded ones stay */

typedef struct {
    bool mounted;
    uint32_t size, data_size;      /* partition, the part files go in */
    uint32_t used, free, largest;  /* bytes, in whole sectors */
    uint32_t entries, seq;
    uint32_t moves, recovered;     /* since mount: files moved; 1 if a move was resumed at mount */
    uint32_t rw_area;              /* bytes reserved at the pool's top for model A RW */
} pex_xip_stats_t;
int pex_xip_stats(pex_xip_stats_t *out);
/* Read the index again (pex_init does this). Resumes an interrupted move.
 * Refused while a module runs from flash. */
int pex_xip_mount(void);
int pex_installed_json(char *dst, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
