#ifndef BTGW_FS_JOB_H
#define BTGW_FS_JOB_H

#include <stdint.h>

/* A note for the editor's operation list. set() only dirties a struct, so
 * it is safe from the tar reader inside lwIP. fs_job_poll emits fs.job from
 * the main loop. A newer note replaces one that has not been sent yet. */
void fs_job_set(uint32_t id, const char *kind, const char *state,
                int index, int total, const char *path, uint32_t size);
void fs_job_poll(void);

#endif
