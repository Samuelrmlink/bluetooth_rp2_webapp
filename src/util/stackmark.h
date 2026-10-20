/* How deep core0's stack has ever gone: the region below the stack is
 * painted at boot and the deepest overwritten word found on demand.
 *
 * core0's stack is nominally 2 KB (SCRATCH_Y's upper half); below it lie the
 * rest of SCRATCH_Y and SCRATCH_X (core1's stack: core1 is never started,
 * but the crumb record lives at 0x20080100), and below those the top of the
 * C heap. There is no guard: a deeper call chain silently overwrites the
 * crumb record and then the heap. sys.info reports stack_max so a call
 * chain that grows (a TLS handshake inside an lwIP callback) is measured
 * rather than discovered. */
#ifndef BTGW_STACKMARK_H
#define BTGW_STACKMARK_H
#include <stdint.h>
void stackmark_paint(void);
/* Bytes below __StackTop ever used. */
uint32_t stackmark_max_used(void);
#endif
