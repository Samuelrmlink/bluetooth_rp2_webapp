/* ilp32 trampoline. pex_tramp(sb, fn, a0, a1, a2, a3):
 *   a0 = RW base (ignored: model A words are already absolute)
 *   a1 = function
 *   a2..a5 = the four caller arguments
 * Return is a uint64 in a0/a1. Callers use the low 32 bits.
 * The veneers are tail jumps; this one is a real call and must save ra.
 */
    .section .text.pex_tramp
    .global pex_tramp
    .type pex_tramp, @function
pex_tramp:
    addi sp, sp, -16
    sw ra, 12(sp)
    mv t0, a1
    mv a0, a2
    mv a1, a3
    mv a2, a4
    mv a3, a5
    jalr ra, t0, 0
    lw ra, 12(sp)
    addi sp, sp, 16
    ret
    .size pex_tramp, . - pex_tramp

    .global pex_get_sp
    .type pex_get_sp, @function
pex_get_sp:
    mv a0, sp
    ret
    .size pex_get_sp, . - pex_get_sp
