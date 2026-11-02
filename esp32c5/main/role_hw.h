#ifndef C5_ROLE_HW_H
#define C5_ROLE_HW_H

#include <stddef.h>
#include <stdint.h>

/* Poll console for the BLE role image. HTTP only queues lines and reads
 * the log; the poll task is what calls the PEX bt group. */
void role_hw_poll(void);
void role_hw_note(const char *line);
void role_hw_submit(const char *line);
void role_hw_console(uint32_t from, char *dst, size_t cap);
void role_hw_restart(void);

#endif
