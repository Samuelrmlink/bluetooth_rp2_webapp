/* PEX export trampoline, Armv6-M (Cortex-M0+). Same contract as
 * pex_tramp_m33.S; Thumb-1 cannot push r9, so it is kept in r4. */
    .syntax unified
    .thumb
    .text
    .balign 4
    .global pex_tramp
    .type pex_tramp, %function
    .thumb_func
pex_tramp:
    push    {r4, r5, r6, lr}        @ 16 bytes: keeps the 8-byte stack alignment
    mov     r4, r9
    mov     r9, r0
    mov     r12, r1
    mov     r0, r2
    mov     r1, r3
    ldr     r2, [sp, #16]
    ldr     r3, [sp, #20]
    blx     r12
    mov     r9, r4
    pop     {r4, r5, r6, pc}
    .size pex_tramp, . - pex_tramp

    .global pex_get_sp
    .type pex_get_sp, %function
    .thumb_func
pex_get_sp:
    mov     r0, sp
    bx      lr
    .size pex_get_sp, . - pex_get_sp
