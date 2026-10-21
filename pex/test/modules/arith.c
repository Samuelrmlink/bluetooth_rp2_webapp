/* arith: code generation and every kind of relocation a module has --
 * initialised data, bss, a pointer in RW to RW, a function-pointer table, a
 * string table, a constructor, 64-bit and floating-point helpers from libgcc. */
#include "pex.h"

int g_counter = 7;
int g_arr[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static int g_bss[16];
int *g_ptr = &g_counter;                     /* RW -> RW */
static volatile int g_ctor;
static volatile int g_dtor_seen;

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int op_div(int a, int b) { return b ? a / b : 0; }
static PEX_PTR_TABLE int (*ops[4])(int, int) = { op_add, op_sub, op_mul, op_div };  /* -> code */
static PEX_PTR_TABLE const char *names[4] = { "add", "sub", "mul", "div" };          /* -> rodata */

__attribute__((constructor)) static void arith_ctor(void) { g_ctor = 0x1234; }
__attribute__((destructor)) static void arith_dtor(void) { g_dtor_seen = 1; }

static volatile int vin[4] = { 1000, 7, -3, 12 };   /* defeats constant folding */
static volatile float fin = 1.5f;
static volatile double din = 2.25;

/* 0 when every check passes, else the number of the first failing one. */
int arith_selftest(void) {
    if (g_ctor != 0x1234) { return 1; }
    if (g_counter != 7 || *g_ptr != 7 || g_ptr != &g_counter) { return 2; }
    int s = 0;
    for (int i = 0; i < 8; i++) { s += g_arr[i]; }
    if (s != 36) { return 3; }
    for (int i = 0; i < 16; i++) { if (g_bss[i]) { return 4; } }
    int a = vin[0], b = vin[1];
    if (ops[0](a, b) != 1007 || ops[1](a, b) != 993 || ops[2](a, b) != 7000 || ops[3](a, b) != 142) { return 5; }
    if (strcmp(names[3], "div") != 0 || names[0][0] != 'a') { return 6; }
    int64_t big = (int64_t)a * 1000000007LL;                 /* __aeabi_lmul / umull */
    if (big != 1000000007000LL) { return 7; }
    if (big / b != 142857143857LL || big % b != 1) { return 8; }   /* __aeabi_ldivmod */
    uint32_t u = (uint32_t)vin[2];
    if (u / 10u != 429496729u || (int)vin[2] / 2 != -1) { return 9; }   /* __aeabi_uidiv, idiv */
    float f = fin * 4.0f + 0.25f;
    if (f != 6.25f) { return 10; }
    double d = din * din;
    if (d != 5.0625) { return 11; }
    if ((int)(d * 100.0) != 506) { return 12; }
    g_bss[3] = vin[3];
    *g_ptr += 1;
    if (g_counter != 8 || g_bss[3] != 12) { return 13; }
    *g_ptr -= 1;
    g_bss[3] = 0;
    return 0;
}
PEX_EXPORT(arith_selftest);

int arith_apply(int op, int a, int b) { return (op >= 0 && op < 4) ? ops[op](a, b) : -1; }
PEX_EXPORT(arith_apply);

/* Exercises more than four arguments through the trampoline (stack args). */
int arith_sum6(int a, int b, int c, int d) { return a + b * 2 + c * 3 + d * 4; }
PEX_EXPORT(arith_sum6);

int64_t arith_mul64(int32_t a, int32_t b) { return (int64_t)a * b; }
PEX_EXPORT(arith_mul64);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    if (reason == PEX_INIT) {
        return arith_selftest() == 0 ? PEX_OK : PEX_EINIT;
    }
    return PEX_OK;
}
