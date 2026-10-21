/* needboard: a STRONG import of the optional board group. A host without the
 * group must refuse it at load, naming the symbol. */
#include "pex.h"

const char *needboard_name(void) { return pex_board_name(); }
PEX_EXPORT(needboard_name);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) { return PEX_OK; }
