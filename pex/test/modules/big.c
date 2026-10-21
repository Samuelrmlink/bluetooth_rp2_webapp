/* big: a module of ~41 KB -- larger than the gateway's whole v1 pool (10 KB),
 * spanning eleven flash sectors -- for XIP install, compaction (overlapping
 * moves) and power cuts. Two 20 KB tables whose every word is checked, and
 * pointer tables that model A keeps in rodata (RO relocations, to code, to RW
 * and to rodata) and model B in RW (PEX_PTR_TABLE). */
#include "pex.h"

#define V(n) ((uint32_t)(n) * 2654435761u + 12345u)
#define R4(b) V(b), V((b) + 1), V((b) + 2), V((b) + 3)
#define R16(b) R4(b), R4((b) + 4), R4((b) + 8), R4((b) + 12)
#define R64(b) R16(b), R16((b) + 16), R16((b) + 32), R16((b) + 48)
#define R256(b) R64(b), R64((b) + 64), R64((b) + 128), R64((b) + 192)
#define R1K(b) R256(b), R256((b) + 256), R256((b) + 512), R256((b) + 768)
#define R5K(b) R1K(b), R1K((b) + 1024), R1K((b) + 2048), R1K((b) + 3072), R1K((b) + 4096)
#define N 5120

static const uint32_t tbl1[N] = { R5K(0) };
static const uint32_t tbl2[N] = { R5K(N) };

#ifdef __ARM_ROPI
#define BIG_CONST                /* model B: a pointer table cannot be const (spec 6.2) */
#else
#define BIG_CONST const          /* model A: in rodata -> RO relocations */
#endif

int g_rw[4] = { 1, 2, 3, 4 };
static int g_bss2[8];
static volatile uint32_t g_calls;

static int f_a(int x) { return x + 1; }
static int f_b(int x) { return x * 3; }
static int f_c(int x) { return x - 7; }
static int f_d(int x) { return x ^ 0x55; }

static PEX_PTR_TABLE BIG_CONST void *BIG_CONST ptrs1[5] = {
    (void *)f_a, &g_rw[1], &tbl2[100], (void *)f_b, &g_bss2[2] };
static PEX_PTR_TABLE BIG_CONST void *BIG_CONST ptrs2[4] = {
    (void *)f_c, &g_rw[3], &tbl1[4000], (void *)f_d };

/* 0 when every word and pointer is right, else what failed */
uint32_t big_check(void) {
    g_calls++;
    for (uint32_t i = 0; i < N; i++) {
        if (tbl1[i] != V(i)) { return 0x10000u + i; }
        if (tbl2[i] != V(N + i)) { return 0x20000u + i; }
    }
    if (((int (*)(int))ptrs1[0])(1) != 2 || ((int (*)(int))ptrs1[3])(5) != 15) { return 1; }
    if (((int (*)(int))ptrs2[0])(10) != 3 || ((int (*)(int))ptrs2[3])(0) != 0x55) { return 2; }
    if (*(const int *)ptrs1[1] != 2 || *(const int *)ptrs2[1] != 4) { return 3; }
    if (*(const uint32_t *)ptrs1[2] != V(N + 100) || *(const uint32_t *)ptrs2[2] != V(4000)) { return 4; }
    g_bss2[2] = 9;
    if (*(int *)ptrs1[4] != 9) { return 5; }
    g_bss2[2] = 0;
    return 0;
}
PEX_EXPORT(big_check);

uint32_t big_calls(void) { return g_calls; }
PEX_EXPORT(big_calls);

uintptr_t big_code_addr(void) { return (uintptr_t)&big_check; }
PEX_EXPORT(big_code_addr);

uintptr_t big_rw_addr(void) { return (uintptr_t)&g_rw[0]; }
PEX_EXPORT(big_rw_addr);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)arg;
    if (reason == PEX_INIT) { return big_check() == 0 ? PEX_OK : PEX_EINIT; }
    return PEX_OK;
}
