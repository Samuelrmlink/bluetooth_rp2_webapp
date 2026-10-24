#include "pinecil_live.h"
#include <string.h>

static uint32_t u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int pinecil_decode(const uint8_t *b, size_t n, pinecil_live_t *out) {
    if (!b || !out || n < 56) { return 0; }
    uint32_t v[14];
    for (int i = 0; i < 14; i++) { v[i] = u32le(b + (size_t)i * 4); }
    memset(out, 0, sizeof *out);
    out->tip_c = v[0];
    out->setpoint_c = v[1];
    out->input_v = (double)v[2] / 10.0;
    out->handle_c = (double)v[3] / 10.0;
    out->power_pct = v[4];
    out->power_source = v[5];
    out->tip_ohms = (double)v[6] / 10.0;
    out->uptime_s = (double)v[7] / 10.0;
    out->last_move_s = (double)v[8] / 10.0;
    out->max_tip_c = v[9];
    out->tip_uv = v[10];
    out->hall_uT = v[11];
    out->mode = v[12];
    out->watts = (double)v[13] / 10.0;
    return 1;
}

const char *pinecil_mode_name(uint32_t mode) {
    static const char *const n[] = { "idle", "soldering", "boost", "sleeping", "settings", "debug" };
    return mode < 6 ? n[mode] : NULL;
}

const char *pinecil_power_name(uint32_t source) {
    static const char *const n[] = { "DC", "QC", "PD-VBUS", "PD" };
    return source < 4 ? n[source] : NULL;
}
