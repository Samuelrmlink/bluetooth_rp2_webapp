/* Host stand-in for the Kconfig-generated autoconf.h.
 *
 * The host harness builds the protocol bindings and nothing else -- no
 * Bluetooth, no filesystem, no radio -- so it defines only what those files
 * actually test for. A real autoconf.h comes from tools/kconfig.cmake. */
#ifndef BTGW_HOST_AUTOCONF_H
#define BTGW_HOST_AUTOCONF_H

#define CONFIG_PROTO_ANKER 1
#define CONFIG_PROTO_ANKER_CRYPTO 1
#define CONFIG_BT_LE 0
#define CONFIG_LUA_ENABLE 1

#endif
