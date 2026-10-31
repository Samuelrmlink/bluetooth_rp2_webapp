/* smp.c always walks the fs and stat tables. This image has neither group. */
#include "ota/smp.h"

const smp_cmd_t smp_fs_cmds[] = { { 0, 0, false, NULL } };
const smp_cmd_t smp_stat_cmds[] = { { 0, 0, false, NULL } };
