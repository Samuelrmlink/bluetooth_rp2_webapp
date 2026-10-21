/*
 * pex_port.h -- what a host gives the PEX runtime. That is all of it: the
 * runtime needs no RTOS, no Lua, no file system and no allocator of its own.
 * port/pico/pex_port_pico.c implements these for any pico-sdk firmware.
 */
#ifndef PEX_PORT_H
#define PEX_PORT_H

#include <stdint.h>

uint32_t pex_port_ms(void);
uint64_t pex_port_us(void);
/* One complete log line, no trailing newline. */
void pex_port_log(int level, const char *line);
/* Code was written to [p, p+n): make it executable (barriers, cache). */
void pex_port_code_sync(void *p, uint32_t n);
/* The current stack's bounds for the unload safety-net scan; 0 = unknown. */
int pex_port_stack_bounds(uintptr_t *lo, uintptr_t *hi);

#endif
