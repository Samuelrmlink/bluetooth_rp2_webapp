/* How deep core0's stack has ever gone: the region below the stack is
 * painted at boot and the deepest overwritten word found on demand.
 *
 * core0's stack is nominally 2 KB (SCRATCH_Y's upper half). It grows down
 * through the rest of SCRATCH_Y and SCRATCH_X. On the 1.245 image the
 * watermark reached 0x200810E8 (3864 bytes below 0x20082000). Below the
 * paint live the file-watch path block (0x20080200..0x20080DDC), the PEX
 * boot record, and the crumb record; below 0x20080000 is the C heap.
 * There is no guard: a deeper call chain overwrites the paths, then the
 * crumb record, then the heap. sys.info reports stack_max so a call chain
 * that grows (a TLS handshake inside an lwIP callback) is measured rather
 * than discovered.
 *
 * Paint starts at STACKMARK_PAINT_FROM. fs_watch.c keeps that many path
 * strings directly underneath and asserts it does not cross this line. */
#ifndef BTGW_STACKMARK_H
#define BTGW_STACKMARK_H
#include <stdint.h>
#define STACKMARK_PAINT_FROM 0x20080E00u
void stackmark_paint(void);
/* Bytes below __StackTop ever used. */
uint32_t stackmark_max_used(void);
#endif
