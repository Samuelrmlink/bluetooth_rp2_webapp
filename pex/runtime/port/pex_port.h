/*
 * pex_port.h -- what a host gives the PEX runtime. That is all of it: the
 * runtime needs no RTOS, no Lua, no file system and no allocator of its own.
 * port/pico/pex_port_pico.c implements these for any pico-sdk firmware.
 */
#ifndef PEX_PORT_H
#define PEX_PORT_H

#include <stdbool.h>
#include <stdint.h>

uint32_t pex_port_ms(void);
uint64_t pex_port_us(void);
/* One complete log line, no trailing newline. */
void pex_port_log(int level, const char *line);
/* Code was written to [p, p+n): make it executable (barriers, cache). */
void pex_port_code_sync(void *p, uint32_t n);
/* The current stack's bounds for the unload safety-net scan; 0 = unknown. */
int pex_port_stack_bounds(uintptr_t *lo, uintptr_t *hi);

/* Slice deadline (spec 8). The runtime calls arm around the outermost
 * pex_call when pex_config_t.slice_ms is set, and disarm when that call
 * returns. A port that can preempt (the pico port's hardware alarm) invokes
 * pex_slice_preempt from that IRQ. A port with no timer leaves both empty:
 * the watchdog is then the only recovery. */
void pex_port_slice_arm(uint32_t ms);
void pex_port_slice_disarm(void);

/* Implemented by the runtime. note_disarmed is called from disarm with
 * interrupts disabled. preempt rewrites the interrupted thread's stacked
 * frame (r0,r1,r2,r3,r12,lr,pc,xpsr) so exception return lands in the
 * resume stub; exc_return is the IRQ's LR. Returns false if this IRQ did
 * not interrupt thread mode, or if no slice is armed. */
void pex_slice_note_disarmed(void);
bool pex_slice_active(void);
bool pex_slice_preempt(uint32_t *frame, uint32_t exc_return);

#endif
