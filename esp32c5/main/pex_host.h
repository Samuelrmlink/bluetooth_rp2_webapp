#ifndef C5_PEX_HOST_H
#define C5_PEX_HOST_H

#include <stddef.h>

void pex_host_init(void);
void pex_host_lock(void);
void pex_host_unlock(void);
void pex_host_poll(void);

/* Websocket ops. `frame` is the whole {"id","op","params"} object.
 * Writes a JSON result (no envelope) into dst. Returns 0, or -1 and fills err. */
int pex_host_op(const char *op, const char *frame, char *dst, size_t cap, const char **err);

int launch_status_json(char *dst, size_t cap);
int launch_set_json(const char *frame, char *dst, size_t cap, const char **err);
void launch_now(void);

#endif
