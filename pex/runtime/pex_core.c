/*
 * Runtime core: init, the group registry, errors and logging, and the host
 * side of the core / log / pex groups.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pex_internal.h"

pex_config_t pex__cfg;
static const pex_group_t *s_groups[PEX_MAX_GROUPS];
static uint32_t s_ngroups;

/* ---- target of THIS host (spec 5: the module's target must match exactly) */
uint32_t pex__host_target(void) {
#if defined(PEX_FORCE_TARGET)           /* the runtime's unit tests on the build host */
    return PEX_FORCE_TARGET;
#elif defined(__ARM_ARCH_8M_MAIN__)
    return PEX_TARGET_ARMV8M_MAIN;
#elif defined(__ARM_ARCH_6M__)
    return PEX_TARGET_ARMV6M;
#elif defined(__ARM_ARCH_8M_BASE__)
    return PEX_TARGET_ARMV8M_BASE;
#elif defined(__ARM_ARCH_7EM__)
    return PEX_TARGET_ARMV7EM_FP;
#else
    return 0;
#endif
}

uint32_t pex__host_features(void) {
    uint32_t f = 0;
#if defined(PEX_FORCE_FEATURES)
    return PEX_FORCE_FEATURES;
#endif
#if defined(__ARM_FP) && (__ARM_FP & 4)
    f |= PEX_FEAT_FPU_SP;
#endif
#if defined(__ARM_FP) && (__ARM_FP & 8)
    f |= PEX_FEAT_FPU_DP;
#endif
#if defined(__ARM_FEATURE_DSP)
    f |= PEX_FEAT_DSP;
#endif
#if defined(__ARM_FEATURE_CMSE)
    f |= PEX_FEAT_CMSE;
#endif
    return f;
}

int pex_register_group(const pex_group_t *g) {
    if (!g) { return PEX_EINVAL; }
    for (uint32_t i = 0; i < s_ngroups; i++) {
        if (s_groups[i]->id == g->id) { s_groups[i] = g; return PEX_OK; }
    }
    if (s_ngroups >= PEX_MAX_GROUPS) { return PEX_ENOMEM; }
    s_groups[s_ngroups++] = g;
    return PEX_OK;
}

const pex_group_t *pex_find_group(uint16_t id) {
    for (uint32_t i = 0; i < s_ngroups; i++) {
        if (s_groups[i]->id == id) { return s_groups[i]; }
    }
    return NULL;
}

int pex_init(const pex_config_t *cfg) {
    if (!cfg || !cfg->pool || cfg->pool_size < 1024) { return PEX_EINVAL; }
    if (pex__mods) { return PEX_ESTATE; }
    pex__cfg = *cfg;
    if (!pex__cfg.host_name) { pex__cfg.host_name = "host"; }
    pex__mem_init(cfg->pool, cfg->pool_size);
    pex__sched_init();
    pex_register_group(&pex_group_core);
    pex_register_group(&pex_group_log);
    pex_register_group(&pex_group_mem);
    pex_register_group(&pex_group_sched);
    pex_register_group(&pex_group_pex);
    pex_register_group(&pex_group_json);
    pex__logf(PEX_LOG_INFO, "pex: runtime %s, api %u.%u, target 0x%04lx, pool %lu bytes, %s", PEX_RT_VERSION,
              PEX_API_MAJOR, PEX_API_MINOR, (unsigned long)pex__host_target(), (unsigned long)cfg->pool_size,
              (cfg->policy & PEX_POLICY_ALLOW_UNSIGNED) ? "UNSIGNED MODULES ALLOWED (development)" : "signed modules only");
    if (cfg->flash) { pex__xip_mount_cfg(); }
    return PEX_OK;
}

void pex_set_policy(uint32_t policy) { pex__cfg.policy = policy; }
uint32_t pex_get_policy(void) { return pex__cfg.policy; }

/* ---- errors ---------------------------------------------------------------- */
static char s_err[160];
int pex__errcode;

void pex__err(int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof s_err, fmt, ap);
    va_end(ap);
    pex__errcode = code;
}

const char *pex_errmsg(void) { return s_err; }

void pex_seterr(int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof s_err, fmt, ap);
    va_end(ap);
    pex__errcode = code;
}

const char *pex_strerror(int e) {
    switch (e) {
    case PEX_OK: return "ok";
    case PEX_EINVAL: return "invalid argument";
    case PEX_ENOMEM: return "out of PEX pool memory";
    case PEX_EFORMAT: return "not a valid .pex";
    case PEX_EVERSION: return "unsupported format version";
    case PEX_ETARGET: return "built for another architecture";
    case PEX_EAPI: return "host API mismatch";
    case PEX_ECRC: return "CRC mismatch (corrupted)";
    case PEX_ESIG: return "bad signature";
    case PEX_EUNSIGNED: return "unsigned (policy requires a signature)";
    case PEX_EBUSY: return "busy";
    case PEX_ENOENT: return "not found";
    case PEX_EIO: return "read error";
    case PEX_EORDER: return "out of order";
    case PEX_ERELOC: return "relocation not allowed for this code model";
    case PEX_EINIT: return "module init failed";
    case PEX_ENOTSUP: return "not supported";
    case PEX_EEXIST: return "already exists";
    case PEX_ESTATE: return "wrong state";
    case PEX_EFAULT: return "module faulted";
    case PEX_EAGAIN: return "try again";
    case PEX_EPENDING: return "pending";
    case PEX_ENOSPC: return "no room in the PEX flash partition";
    }
    return "error";
}

void pex__logf(int level, const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pex_port_log(level, line);
}

/* ---- core group -------------------------------------------------------------- */
uint32_t pex_api_version(void) { return (PEX_API_MAJOR << 16) | PEX_API_MINOR; }
uint32_t pex_has(uint32_t group) {
    const pex_group_t *g = pex_find_group((uint16_t)group);
    return g ? ((uint32_t)g->major << 16) | g->minor : 0;
}
uint32_t pex_target(void) { return pex__host_target(); }
uint32_t pex_ms(void) { return pex_port_ms(); }
uint64_t pex_us(void) { return pex_port_us(); }
int pex_vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap) { return vsnprintf(dst, cap, fmt, ap); }
int pex_snprintf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}
void pex_assert_fail(const char *expr, const char *file, int line) {
    pex_mod_t *m = pex__current();
    pex__logf(PEX_LOG_ERR, "[pex:%s] assert failed: %s (%s:%d)", m ? m->name : "host", expr, file, line);
}
void pexh_aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void pexh_aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void pexh_aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
void pexh_aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }

/* ---- log group ----------------------------------------------------------------- */
void pex_logv(int level, const char *fmt, va_list ap) {
    char line[200];
    pex_mod_t *m = pex__current();
    int n = snprintf(line, sizeof line, "[pex:%s] ", m ? m->name : "host");
    if (n < 0 || (size_t)n >= sizeof line) { n = 0; }
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    pex_port_log(level, line);
}
void pex_log(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pex_logv(level, fmt, ap);
    va_end(ap);
}
void pex_log_hex(int level, const char *what, const void *data, size_t len) {
    char line[200];
    pex_mod_t *m = pex__current();
    const uint8_t *p = data;
    size_t i = 0;
    do {
        int n = snprintf(line, sizeof line, "[pex:%s] %s +%u:", m ? m->name : "host", what ? what : "", (unsigned)i);
        for (size_t k = 0; k < 16 && i < len && n + 4 < (int)sizeof line; k++, i++) {
            n += snprintf(line + n, sizeof line - (size_t)n, " %02x", p[i]);
        }
        pex_port_log(level, line);
    } while (i < len);
}

/* ---- pex group --------------------------------------------------------------- */
pex_mod_t *pex_self(void) { return pex__current(); }
void pex_ref(pex_mod_t *m) { if (m) { m->refs++; } }
void pex_unref(pex_mod_t *m) { if (m && m->refs > 0) { m->refs--; } }
const char *pex_name(pex_mod_t *m) { return m ? m->name : "host"; }
void pex_app_exit(int code) {
    pex_mod_t *m = pex__current();
    if (m) {
        m->exit_code = code;
        m->exit_pending = 1;
    }
}
