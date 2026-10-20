/*
 * Lua configuration for RP2040/RP2350, force-included by CMake.
 *
 * LUA_32BITS makes lua_Integer int32 and lua_Number float. Neither part has a
 * double-precision FPU, so the stock double would be software-emulated on every
 * arithmetic operation, and it doubles the size of every number in every table.
 *
 * Everything the standard library would use to reach the host -- files,
 * processes, dynamic loading -- is compiled out rather than stubbed, so a
 * script cannot reach for it and get a confusing runtime error.
 */
#ifndef LUA_PICO_CONF_H
#define LUA_PICO_CONF_H

#define LUA_32BITS 1

/* No OS: no dlopen, no popen, no locale, no C89 fallbacks needed. */
#undef  LUA_USE_POSIX
#undef  LUA_USE_DLOPEN
#define LUA_USE_C89_OFF 1

/* The panic path must not longjmp into nothing. Errors surface as Lua errors
 * through lua_pcall; a genuine panic is a firmware bug and is logged. */
#define LUAI_MAXSTACK 15000

/* A C-stack guard in bytes (lstate.c luaE_checkcbytes, lstrlib.c match).
 * LUAI_MAXCCALLS (200 nested C calls / parser levels) assumes a desktop
 * stack; core0 has 8 KB (SCRATCH_X+Y, 0x20080000-0x20082000) with the crumb
 * record at its foot (util/crumb.h) and the C heap right below it, and a REPL
 * chunk of 40 nested parentheses already ran off the end of it. Below SOFT a
 * nested call or parser level raises "C stack overflow" (once, so the error's
 * own handling can run); below HARD it is "error in error handling". */
#if defined(__arm__)
#define LUAI_CSTACK_SOFT 0x20080900u
#define LUAI_CSTACK_HARD 0x20080500u
#endif

#endif
