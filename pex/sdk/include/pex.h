/*
 * pex.h -- the PEX module SDK (spec docs/pex-format.md).
 *
 * A module is ordinary freestanding C. It talks to the host ONLY through the
 * host API declared in pex_api.h (generated from pex/host_api.json); it never
 * calls the pico-sdk or touches registers. Build it with pex_add_module()
 * (pex/sdk/cmake/pex.cmake), which links the import veneers and runs pexlink.
 *
 *   #include "pex.h"
 *   int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
 *       if (reason == PEX_INIT) pex_log(PEX_LOG_INFO, "hello");
 *       return PEX_OK;
 *   }
 *
 * This header is shared by modules and hosts: the format constants below are
 * the contract.
 */
#ifndef PEX_H
#define PEX_H

#include <stddef.h>
#include <stdint.h>
#include "pex_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- format (spec 4) ----------------------------------------------------- */
#define PEX_MAGIC          0x1A584550u     /* 'P' 'E' 'X' 0x1A, little-endian */
#define PEX_FORMAT_MAJOR   1
#define PEX_FORMAT_MINOR   0
#define PEX_HEADER_SIZE    92u

/* targets (spec 5) */
#define PEX_TARGET_ARMV6M        0x0001u   /* m0p: Cortex-M0+, RP2040 */
#define PEX_TARGET_ARMV8M_MAIN   0x0002u   /* m33: Cortex-M33 softfp, RP2350 */
#define PEX_TARGET_ARMV8M_BASE   0x0003u   /* m23 (reserved) */
#define PEX_TARGET_ARMV7EM_FP    0x0004u   /* m4f */

/* target_features */
#define PEX_FEAT_FPU_SP   (1u << 0)
#define PEX_FEAT_FPU_DP   (1u << 1)
#define PEX_FEAT_DSP      (1u << 2)
#define PEX_FEAT_CMSE     (1u << 3)

/* header flags */
#define PEX_F_PIC            (1u << 0)   /* code model B: ROPI + RWPI, r9 = RW base */
#define PEX_F_XIP_OK         (1u << 1)
#define PEX_F_RAM_ONLY       (1u << 2)
#define PEX_F_LUA_MODULE     (1u << 3)
#define PEX_F_APP            (1u << 4)
#define PEX_F_REENTRANT      (1u << 5)   /* with PIC: instances may share one code copy */
#define PEX_F_NEEDS_SIG      (1u << 6)
#define PEX_F_UNLOAD_ON_EXIT (1u << 7)

/* section types (spec 4.3) */
#define PEX_SEC_RELOC    1u
#define PEX_SEC_IMPORTS  2u
#define PEX_SEC_EXPORTS  3u
#define PEX_SEC_META     4u
#define PEX_SEC_DEPS     5u
#define PEX_SEC_DEBUG    6u
#define PEX_SEC_INIT     7u   /* 0.6: constructor offsets (was .init_array) */
#define PEX_SEC_FINI     8u   /* 0.6: destructor offsets, run at unload */
#define PEX_SEC_SIG      0xF0u
#define PEX_SECF_OPTIONAL 1u

#define PEX_RELOC_TO_RW  0x80000000u     /* entry bit 31: the word points into RW */
#define PEX_IMP_WEAK     1u

/* META tags */
#define PEX_META_NAME     1
#define PEX_META_DESC     2
#define PEX_META_AUTHOR   3
#define PEX_META_URL      4
#define PEX_META_LICENSE  5
#define PEX_META_LUA_NAME 6
#define PEX_META_MIN_RAM  7
#define PEX_META_VERSION  8
#define PEX_META_TOOL     9

#define PEX_SIG_ECDSA_P256_SHA256 1

/* ---- entry (spec 8) ------------------------------------------------------- */
#define PEX_INIT      1u   /* once per load, after linking */
#define PEX_LUA_OPEN  2u   /* arg = lua_State *: push the module table, return 1 */
#define PEX_APP_RUN   3u   /* arg = config JSON (const char *, may be NULL): start; return quickly */
#define PEX_DEINIT    4u   /* before unload: free, cancel */
#define PEX_APP_STOP  5u   /* 0.6: the host asks a running app to stop */

/* errors: negative */
#define PEX_OK         0
#define PEX_EINVAL    (-1)
#define PEX_ENOMEM    (-2)
#define PEX_EFORMAT   (-3)    /* bad magic / layout / unknown required section */
#define PEX_EVERSION  (-4)    /* format major */
#define PEX_ETARGET   (-5)    /* wrong architecture or missing CPU feature */
#define PEX_EAPI      (-6)    /* api/group version, or a missing strong import */
#define PEX_ECRC      (-7)
#define PEX_ESIG      (-8)    /* bad signature */
#define PEX_EUNSIGNED (-9)    /* policy requires a signature */
#define PEX_EBUSY     (-10)   /* unload refused: active / referenced / running */
#define PEX_ENOENT    (-11)
#define PEX_EIO       (-12)
#define PEX_EORDER    (-13)   /* scratch arenas ended out of LIFO order */
#define PEX_ERELOC    (-14)   /* a relocation the model forbids (RO list under PIC) */
#define PEX_EINIT     (-15)   /* pex_main(INIT) failed */
#define PEX_ENOTSUP   (-16)
#define PEX_EEXIST    (-17)
#define PEX_ESTATE    (-18)
#define PEX_EFAULT    (-19)   /* the module faulted earlier */
#define PEX_EAGAIN    (-20)   /* would block: retry later, or pass a callback */
#define PEX_EPENDING  (-21)   /* accepted: the callback delivers the result later */

typedef struct pex_host {
    uint32_t size;          /* sizeof(pex_host_t) as the host knows it */
    uint16_t api_major, api_minor;
    uint16_t target, format_minor;
    const char *host_name;  /* "btgw", "pex-test", ... */
    pex_mod_t *self;        /* this instance */
} pex_host_t;

int pex_main(const pex_host_t *host, uint32_t reason, void *arg);

/* ---- module-side helpers ---------------------------------------------------
 * PEX_EXPORT(fn): list fn in EXPORTS (callable by the host by name, through the
 * trampoline). File scope. The record lives in a metadata section pexlink reads
 * and strips; nothing of it is loaded.                                         */
#define PEX__STR(x) #x
#define PEX_EXPORT(fn) \
    __asm__(".pushsection .pex.exports,\"a\",%progbits\n.balign 4\n.word " PEX__STR(fn) \
            "\n.asciz \"" PEX__STR(fn) "\"\n.balign 4\n.popsection")

/* PEX_WEAK_IMPORT(sym): the module runs without sym (a host without that group):
 * its GOT slot is then 0; test with pex_avail(sym) before calling.             */
#define PEX_WEAK_IMPORT(sym) \
    __asm__(".pushsection .pex.weak,\"a\",%progbits\n.asciz \"" PEX__STR(sym) "\"\n.popsection")
#define pex_avail(sym) (__extension__({ extern void *__pex_got_##sym; __pex_got_##sym != 0; }))

/* PEX_META_STR("key", "value"): extra META strings (name/version normally come
 * from pex_add_module()). key: description, author, url, license, lua_name.   */
#define PEX_META_STR(key, val) \
    __asm__(".pushsection .pex.meta,\"a\",%progbits\n.asciz \"" key "\"\n.asciz \"" val "\"\n.popsection")

/* A table of pointers (a function table, "const char *names[]") cannot be
 * read-only in model B: ROPI code must not hold absolute addresses, and pexlink
 * refuses one in .rodata. Declare such tables non-const with PEX_PTR_TABLE in
 * front:  static PEX_PTR_TABLE pexlua_reg_t fns[] = {...};
 * It lives in RW and the loader fixes it up from the RW reloc list. (Without
 * the attribute clang may still promote a never-written static table to
 * rodata -- it is what keeps the object writable.) clang defines __ARM_ROPI
 * under -fropi; in model A the macro is empty.                                */
#if defined(__ARM_ROPI)
#define PEX_PTR_TABLE __attribute__((used))
#else
#define PEX_PTR_TABLE
#endif

#define pex_assert(e)((e) ? (void)0 : pex_assert_fail(#e, __FILE__, __LINE__))

#ifdef __cplusplus
}
#endif
#endif
