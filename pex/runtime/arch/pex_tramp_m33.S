/* PEX export trampoline, Armv8-M Mainline / Armv7-M (spec 8, 6.2).
 *
 *   uint64_t pex_tramp(uint32_t sb, uint32_t fn, uint32_t a0, uint32_t a1,
 *                      uint32_t a2, uint32_t a3);
 *
 * Calls fn(a0, a1, a2, a3) with r9 = sb (the instance's RW base: the static
 * base of a model B module; harmless for model A) and restores the caller's
 * r9 afterwards. r0:r1 come back untouched (32- or 64-bit results). The
 * bookkeeping (active count, executing-module stack) is in pex_call(). */
    .syntax unified
    .thumb
    .text
    .balign 4
    .global pex_tramp
    .type pex_tramp, %function
    .thumb_func
pex_tramp:
    push    {r4, r9, r10, lr}       @ 16 bytes: keeps the 8-byte stack alignment
    mov     r9, r0
    mov     r12, r1
    mov     r0, r2
    mov     r1, r3
    ldr     r2, [sp, #16]
    ldr     r3, [sp, #20]
    blx     r12
    pop     {r4, r9, r10, pc}
    .size pex_tramp, . - pex_tramp

    .global pex_get_sp
    .type pex_get_sp, %function
    .thumb_func
pex_get_sp:
    mov     r0, sp
    bx      lr
    .size pex_get_sp, . - pex_get_sp
