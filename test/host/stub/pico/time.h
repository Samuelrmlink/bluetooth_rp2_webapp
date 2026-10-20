/* Host stand-in for pico/time.h: what the Lua bindings under test use. */
#ifndef HOST_PICO_TIME_H
#define HOST_PICO_TIME_H
#include <stdint.h>
typedef uint64_t absolute_time_t;
absolute_time_t get_absolute_time(void);
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000u); }
static inline uint64_t to_us_since_boot(absolute_time_t t) { return t; }
#endif
