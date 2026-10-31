/* Picks the trampoline for the architecture this file is assembled for. */
#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_8M_BASE__)
#include "pex_tramp_m0p.S"
#elif defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_7M__)
#include "pex_tramp_m33.S"
#elif defined(__riscv)
#include "pex_tramp_c5.S"
#else
#error "PEX: no trampoline for this architecture"
#endif
