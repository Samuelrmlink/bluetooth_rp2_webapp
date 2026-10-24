/* Who called panic, kept where a reverted trial can still read it.
 *
 * 1.251 stored the return address and it landed in last_crumb: 0x1005bc7e,
 * the nop after `bl __wrap_panic` inside pico_malloc's __wrap_malloc
 * (check_alloc, "Out of memory"). That names the heap check, not the
 * allocation. __wrap_malloc's frame at that bl is exactly
 *
 *     push {r4, lr}     ; [sp] = requested size, [sp+4] = caller
 *
 * with nothing else pushed (the mutex is compiled out). The size stays in
 * r4 (the stacked word is the caller's r4, not the size: 1.252's ring was
 * 1). scratch[0] becomes last_crumb (the malloc caller). The size is
 * ring[0] of the crumb record, which the hardfault handler does not clear
 * and the next boot prints in sys.fault. Scratch 2 and 3 belong to the
 * ROM; scratch[1] is the fault PC.
 *
 * Re-check that prologue on the elf before flashing. A different frame
 * stores garbage.
 */
#include <stdint.h>

void __attribute__((noreturn)) __real_panic(const char *fmt, ...);

void __attribute__((naked, noreturn)) __wrap_panic(const char *fmt, ...) {
    __asm volatile(
        "stmdb sp!, {r2, r3, r12}\n"
        "mov   r2,  r4\n"                 /* live r4: requested size */
        "ldr   r3,  [sp, #16]\n"          /* saved lr: malloc's caller */
        "movw  r12, #0x8000\n"
        "movt  r12, #0x400d\n"            /* watchdog base 0x400d8000 */
        "str   r3,  [r12, #12]\n"         /* scratch[0] */
        "movw  r12, #0x0100\n"
        "movt  r12, #0x2008\n"            /* CRUMB_REC 0x20080100 */
        "str   r2,  [r12, #8]\n"          /* ring[0] = size */
        "movs  r2,  #1\n"
        "str   r2,  [r12, #4]\n"          /* n = 1 */
        "ldmia sp!, {r2, r3, r12}\n"
        "b.w   __real_panic\n");
}
