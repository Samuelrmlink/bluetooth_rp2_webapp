/* Pinecil bulk-live decode. 14 x u32 LE. Scales match lua/profiles/pinecil.lua.
 * Pure C, no pex.h. */
#ifndef PINECIL_LIVE_H
#define PINECIL_LIVE_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t tip_c, setpoint_c, power_pct, max_tip_c, tip_uv, hall_uT;
    uint32_t power_source, mode;
    double input_v, handle_c, tip_ohms, uptime_s, last_move_s, watts;
} pinecil_live_t;

int pinecil_decode(const uint8_t *b, size_t n, pinecil_live_t *out);
const char *pinecil_mode_name(uint32_t mode);         /* NULL if not in the table */
const char *pinecil_power_name(uint32_t source);
#endif
