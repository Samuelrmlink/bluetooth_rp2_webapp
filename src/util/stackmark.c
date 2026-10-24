#include "util/stackmark.h"

#define PAINT 0x5AC4C0DEu
/* Above the file-watch paths. See STACKMARK_PAINT_FROM. */
#define PAINT_FROM STACKMARK_PAINT_FROM

extern uint32_t __StackTop;

void stackmark_paint(void) {
    uint32_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    for (volatile uint32_t *p = (volatile uint32_t *)PAINT_FROM; (uint32_t)p < sp - 64; p++) { *p = PAINT; }
}

uint32_t stackmark_max_used(void) {
    const volatile uint32_t *p = (const volatile uint32_t *)PAINT_FROM;
    uint32_t top = (uint32_t)&__StackTop;
    while ((uint32_t)p < top && *p == PAINT) { p++; }
    return top - (uint32_t)p;
}
