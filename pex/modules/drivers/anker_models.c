/* Product tables copied from the former Lua profile. That profile is gone.
 * Unknown tags keep the unknown_* names. Width of a scalar comes from the
 * bytes, not from the map. */
#include "anker_models.h"
#include <string.h>

static int eq_ci(const char *a, const char *b) {
    if (!a || !b) { return 0; }
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') { ca = (unsigned char)(ca - 32); }
        if (cb >= 'a' && cb <= 'z') { cb = (unsigned char)(cb - 32); }
        if (ca != cb) { return 0; }
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static const struct {
    char pt[5];
    char code[8];
    const char *name;
    uint8_t inferred;
} LOOK[] = {
    { "B004", "A17C0", "SOLIX Solarbank E1600 (Gen 1)", 0 },
    { "B005", "A17B1", "SOLIX Home Power Panel (for F3800, non-EU)", 0 },
    { "B006", "A17C1", "SOLIX Solarbank 2 E1600 Pro", 0 },
    { "B007", "A17C2", "SOLIX Solarbank 2 E1600 AC", 0 },
    { "B008", "A17C3", "SOLIX Solarbank 2 E1600 Plus", 0 },
    { "B00A", "A17C5", "SOLIX Solarbank 3 E2700", 0 },
    { "B00B", "AE100", "SOLIX Power Dock for Solarbank Multisystems", 0 },
    { "B101", "A1781", "SOLIX F2600 Portable Power Station", 0 },
    { "B102", "A1772", "SOLIX F1500 Portable Power Station", 0 },
    { "B103", "A1753", "SOLIX C800(P/X) Portable Power Station", 0 },
    { "B104", "A1754", "SOLIX C800(P/X) Portable Power Station", 0 },
    { "B105", "A1761", "SOLIX C1000(X) Portable Power Station", 0 },
    { "B106", "A1790", "SOLIX F3800 Portable Power Station", 0 },
    { "B107", "A17Y0", "SOLIX Solarbank E1600 0W Output Switch", 0 },
    { "B108", "A1755", "SOLIX C800(P/X) Portable Power Station", 0 },
    { "B109", "A1722", "SOLIX C300(X) AC Portable Power Station", 0 },
    { "B10A", "A1780P", "SOLIX F2000(P) Portable Power Station (Plus)", 0 },
    { "B10B", "A1723", "SOLIX C300(X) AC Portable Power Station", 0 },
    { "B10D", "A1725", "SOLIX C200(X)(DC) Portable Power Station", 0 },
    { "B10E", "A1726", "SOLIX C300(X) DC Portable Power Station", 0 },
    { "B10F", "A1727", "SOLIX C200(X)(DC) Portable Power Station", 0 },
    { "B110", "A1782", "SOLIX F3000 Portable Power Station", 0 },
    { "B111", "A1762", "SOLIX C1000 X Portable Power Station", 0 },
    { "B112", "A1790P", "SOLIX F3800 Plus Portable Power Station", 0 },
    { "B113", "A1728", "SOLIX C300(X) DC Portable Power Station", 0 },
    { "B114", "A1729", "SOLIX C200(X)(DC) Portable Power Station", 0 },
    { "B115", "A17A3", "SOLIX Everfrost 2 23L Powered Cooler", 0 },
    { "B116", "A17A4", "SOLIX Everfrost 2 40L Powered Cooler", 0 },
    { "B117", "A17A5", "SOLIX Everfrost 2 58L Powered Cooler", 0 },
    { "B118", "A1763", "SOLIX C1000(X) Gen 2 Portable Power Station", 0 },
    { "B119", "A1765", "SOLIX C1000(X) Gen 2 Portable Power Station", 0 },
    { "B11A", "A1783", "SOLIX C2000(X) Gen 2 Portable Power Station", 0 },
    { "B11B", "A1785", "SOLIX C2000(X) Gen 2 Portable Power Station", 0 },
    { "B11C", "AX1C0", "SOLIX X1 component (unconfirmed)", 1 },
    { "B11D", "AS200", "SOLIX Alternator Charger", 0 },
    { "B11E", "AS100", "SOLIX C1000 Gen 2 LE Portable Power Station", 0 },
    { "B201", "A17X7", "Anker 3-Phase WiFi Smart Meter", 0 },
    { "B202", "A17X8", "Anker Smart Plug 2500 W", 0 },
    { "B203", "A17X7US", "Anker Smart Meter (US grid)", 0 },
    { "B204", "AE1R0", "SOLIX P1 Meter", 0 },
    { "B301", "A5101", "SOLIX X1 P6K (US) Power Module", 0 },
    { "B302", "A5102", "SOLIX X1 Energy Module 1P H(3.68-6)K", 0 },
    { "B303", "A5103", "SOLIX X1 Energy Module 3P H(5-12)K", 0 },
    { "B304", "A5341", "SOLIX X1 Backup Controller", 0 },
    { "B310", "A5190", "SOLIX V1 Smart EV Charger (variant)", 1 },
    { "B311", "A5191", "SOLIX V1 Smart EV Charger", 0 },
    { "B312", "A17E1", "SOLIX E10 (US market)", 0 },
    { "B313", "A7320", "SOLIX Smart Generator 5500", 0 },
    { "B314", "AX170", "Power Dock for SOLIX E10 (US market)", 0 },
    { "B401", "A91B2", "Anker Prime Charging Station (8-in-1, 240W)", 0 },
    { "B402", "A2345", "Anker 250W Prime Charger", 0 },
    { "B403", "A1341", "Anker Prime Power Bank 50K (300W, retractable USB-C)", 0 },
    { "B404", "A110A", "Anker Prime Power Bank 26K (300W)", 0 },
    { "B405", "A2687", "Anker Prime 160W 3-Port GaN Charger (smart display)", 0 },
    { "B406", "A110B", "Anker Prime Power Bank 20K", 0 },
    { "B407", "A1903", "Anker 150W Charging Base", 0 },
    { "B408", "A25X7", "Anker Prime Wireless Charger", 0 },
    { "B409", "A2693", "Anker charger (unconfirmed)", 1 },
    { "B40A", "A110G", "Anker Prime Power Bank 20K", 0 },
};

int anker_lookup(const char *product_type, char code[8], char name[64], int *inferred) {
    if (code) { code[0] = 0; }
    if (name) { name[0] = 0; }
    if (inferred) { *inferred = 0; }
    if (!product_type) { return 0; }
    for (size_t i = 0; i < sizeof LOOK / sizeof LOOK[0]; i++) {
        if (!eq_ci(LOOK[i].pt, product_type)) { continue; }
        if (code) {
            memcpy(code, LOOK[i].code, 8);
        }
        if (name && LOOK[i].name) {
            size_t n = strlen(LOOK[i].name);
            if (n > 63) { n = 63; }
            memcpy(name, LOOK[i].name, n);
            name[n] = 0;
        }
        if (inferred) { *inferred = LOOK[i].inferred; }
        return 1;
    }
    return 0;
}

/* Only the rows the live drivers ask about. Other SKUs answer NULL, which
 * is "not in this table", the same as country.lua. */
static const struct { char sku[4]; char cc[3]; } C1753[] = {
    { "5XG", "us" }, { "5ZA", "uk" }, { "5ZC", "jp" }, { "5ZD", "eu" }, { "5ZE", "au" },
    { "5ZF", "cn" }, { "6TU", "ca" }, { "CLC", "jp" }, { "EAB", "jp" },
};
static const struct { char sku[4]; char cc[3]; } C1761[] = {
    { "2JR", "jp" }, { "5YX", "us" }, { "5ZU", "uk" }, { "5ZV", "jp" }, { "5ZW", "za" },
    { "5ZX", "au" }, { "5ZY", "cn" }, { "5ZZ", "tw" }, { "6NX", "eu" }, { "766", "ca" },
    { "9FH", "jp" }, { "AA1", "jp" }, { "CKX", "jp" }, { "CLA", "jp" }, { "DQ0", "jp" },
    { "Q62", "jp" }, { "Q64", "jp" },
};

const char *anker_country(const char *code, const char *sku) {
    if (!code || !sku) { return NULL; }
    const void *tab = NULL;
    size_t n = 0, stride = 0;
    if (eq_ci(code, "A1753")) {
        tab = C1753; n = sizeof C1753 / sizeof C1753[0]; stride = sizeof C1753[0];
    } else if (eq_ci(code, "A1761")) {
        tab = C1761; n = sizeof C1761 / sizeof C1761[0]; stride = sizeof C1761[0];
    } else {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        const char *row = (const char *)tab + i * stride;
        if (eq_ci(row, sku)) { return row + 4; }
    }
    return NULL;
}

int anker_model_id(const char *code) {
    if (!code) { return 0; }
    if (eq_ci(code, "A1761")) { return ANKER_MODEL_A1761; }
    if (eq_ci(code, "A2687")) { return ANKER_MODEL_A2687; }
    return 0;
}

int anker_model_verified(int id) { return id == ANKER_MODEL_A2687; }
int anker_model_post_auth(int id) { return id == ANKER_MODEL_A2687; }
int anker_model_screensaver(int id) { return id == ANKER_MODEL_A2687; }
int anker_model_history(int id) { return id == ANKER_MODEL_A2687; }

uint16_t anker_model_setter_ft(int id) {
    (void)id;
    return 0x000F;
}

int anker_model_poll(int id, uint16_t *ft, uint16_t *op, uint16_t *want, uint32_t *period_ms) {
    if (id != ANKER_MODEL_A1761) { return 0; }
    if (ft) { *ft = 0x000F; }
    if (op) { *op = 0x0040; }
    if (want) { *want = 0x0840; }
    if (period_ms) { *period_ms = 5000; }
    return 1;
}

int anker_is_report(int id, uint16_t opcode) {
    if (id == ANKER_MODEL_A1761) { return opcode == 0x0840; }
    if (id == ANKER_MODEL_A2687) { return opcode == 0x0300 || opcode == 0x0A00; }
    return 1; /* no schema: any unsolicited frame is the report */
}

static const int32_t V01[] = { 0, 1 };
static const int32_t V013[] = { 0, 1, 3 };
static const int32_t V04[] = { 0, 1, 2, 3, 4 };
static const int32_t V03[] = { 0, 1, 2, 3 };
static const char *const N_AI[] = { "ai", "standard" };
static const int32_t N_AI_V[] = { 0, 1 };
static const char *const N_FIX[] = { "c1_priority", "laptop_mode" };
static const int32_t N_FIX_V[] = { 0, 1 };
static const char *const N_SOFF[] = { "30s", "1min", "5min", "30min", "12h" };
static const char *const N_DIR[] = { "up", "left", "down", "right" };
static const char *const N_LANG[] = { "en", "zh", "ja", "de" };

static const anker_setter SET_A1761[] = {
    { "ac_output_enabled", 0x004A, ANKER_JOB_INT, ANKER_FE_TYPED, ANKER_V_U8, 0, 0, 1, 2, 0, 0, 1, 0, V01, NULL, NULL, {{0}} },
    { "car_output_enabled", 0x004B, ANKER_JOB_INT, ANKER_FE_TYPED, ANKER_V_U8, 0, 0, 1, 2, 0, 0, 1, 0, V01, NULL, NULL, {{0}} },
    { "ambient_light", 0x004F, ANKER_JOB_INT, ANKER_FE_TYPED, ANKER_V_U8, 0, 0, 1, 3, 0, 0, 1, 0, V013, NULL, NULL, {{0}} },
    { "screen_enabled", 0x0052, ANKER_JOB_INT, ANKER_FE_TYPED, ANKER_V_U8, 0, 0, 1, 2, 0, 0, 1, 0, V01, NULL, NULL, {{0}} },
    { "ac_countdown_s", 0x0042, ANKER_JOB_INT, ANKER_FE_TYPED, ANKER_V_U32, 0, 0, 1, 0, 0, 0, 0, 86400, NULL, NULL, NULL, {{0}} },
    { "car_countdown_s", 0x0043, ANKER_JOB_INT, ANKER_FE_TYPED, ANKER_V_U32, 0, 0, 1, 0, 0, 0, 0, 86400, NULL, NULL, NULL, {{0}} },
    { "ac_charge_power_w", 0x0044, ANKER_JOB_INT, ANKER_FE_TYPED, ANKER_V_U16, 0, 0, 1, 0, 0, 0, 100, 1300, NULL, NULL, NULL, {{0}} },
};

static const anker_setter SET_A2687[] = {
    { "c1_on", 0x0207, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 2, 0, 2, 1, 0, V01, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 1, 0 }, { 0xa3, ANKER_V_U8, 0, 0 }, { 0 } } },
    { "c2_on", 0x0207, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 2, 0, 2, 1, 0, V01, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 1, 1 }, { 0xa3, ANKER_V_U8, 0, 0 }, { 0 } } },
    { "c3_on", 0x0207, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 2, 0, 2, 1, 0, V01, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 1, 2 }, { 0xa3, ANKER_V_U8, 0, 0 }, { 0 } } },
    { "c1_timer_s", 0x0209, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 0, 0, 2, 0, 86400, NULL, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 1, 0 }, { 0xa3, ANKER_V_U32B, 0, 0 }, { 0 } } },
    { "c2_timer_s", 0x0209, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 0, 0, 2, 0, 86400, NULL, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 1, 1 }, { 0xa3, ANKER_V_U32B, 0, 0 }, { 0 } } },
    { "c3_timer_s", 0x0209, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 0, 0, 2, 0, 86400, NULL, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 1, 2 }, { 0xa3, ANKER_V_U32B, 0, 0 }, { 0 } } },
    { "charging_protocol", 0x0206, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 2, 2, 1, 1, 0, V01, N_AI, N_AI_V,
      { { 0xa2, ANKER_V_U8, 0, 0 }, { 0 }, { 0 } } },
    { "custom_mode", 0x0206, ANKER_JOB_CUSTOM, ANKER_FE_BARE, 0, 1, 0, 1, 0, 0, 0, 1, 0, NULL, NULL, NULL, {{0}} },
    { "fixed_allocation", 0x0205, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 2, 2, 1, 1, 0, V01, N_FIX, N_FIX_V,
      { { 0xa2, ANKER_V_U8, 0, 0 }, { 0 }, { 0 } } },
    { "backlight_pct", 0x0204, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 0, 0, 1, 25, 100, NULL, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 0, 0 }, { 0 }, { 0 } } },
    { "screen_off", 0x0203, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 5, 5, 1, 1, 0, V04, N_SOFF, V04,
      { { 0xa2, ANKER_V_U8, 0, 0 }, { 0 }, { 0 } } },
    { "direction", 0x020B, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 4, 4, 1, 1, 0, V03, N_DIR, V03,
      { { 0xa2, ANKER_V_U8, 0, 0 }, { 0 }, { 0 } } },
    { "gyroscope", 0x020D, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 2, 0, 1, 1, 0, V01, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 0, 0 }, { 0 }, { 0 } } },
    { "language", 0x0202, ANKER_JOB_ITEMS, ANKER_FE_BARE, 0, 0, 0, 1, 4, 4, 1, 1, 0, V03, N_LANG, V03,
      { { 0xa2, ANKER_V_U8, 0, 0 }, { 0 }, { 0 } } },
    { "screensaver_enabled", 0x021F, ANKER_JOB_ITEMS, ANKER_FE_TYPED, 0, 0, 0, 1, 2, 0, 1, 1, 0, V01, NULL, NULL,
      { { 0xa2, ANKER_V_U8, 0, 0 }, { 0 }, { 0 } } },
    { "protocols", 0x021D, ANKER_JOB_PROTO, ANKER_FE_NONE, 0, 0, 1, 1, 0, 0, 0, 1, 0, NULL, NULL, NULL, {{0}} },
};

const anker_setter *anker_setters(int id, int *n) {
    if (id == ANKER_MODEL_A1761) {
        if (n) { *n = (int)(sizeof SET_A1761 / sizeof SET_A1761[0]); }
        return SET_A1761;
    }
    if (id == ANKER_MODEL_A2687) {
        if (n) { *n = (int)(sizeof SET_A2687 / sizeof SET_A2687[0]); }
        return SET_A2687;
    }
    if (n) { *n = 0; }
    return NULL;
}

const anker_setter *anker_setter_by_name(int id, const char *field) {
    int n = 0;
    const anker_setter *s = anker_setters(id, &n);
    if (!s || !field) { return NULL; }
    for (int i = 0; i < n; i++) {
        if (strcmp(s[i].name, field) == 0) { return &s[i]; }
    }
    return NULL;
}

static int named(const anker_setter *s, const char *text, int32_t *out) {
    if (!s->name_keys || !text) { return 0; }
    for (uint8_t i = 0; i < s->nnames; i++) {
        if (strcmp(s->name_keys[i], text) == 0) {
            *out = s->name_vals[i];
            return 1;
        }
    }
    return 0;
}

static uint8_t value_enc(const anker_setter *s) {
    if (s->enc) { return s->enc; }
    for (int i = (int)s->nitems - 1; i >= 0; i--) {
        if (!s->items[i].has_const) { return s->items[i].enc; }
    }
    return ANKER_V_U8;
}

static int check_int(const anker_setter *s, int32_t v, const char **err) {
    if (s->nvalues) {
        for (uint8_t i = 0; i < s->nvalues; i++) {
            if (s->values[i] == v) { return 0; }
        }
        *err = "not one of the allowed values";
        return -1;
    }
    if (s->maxv >= s->minv && (v < s->minv || v > s->maxv)) {
        *err = "out of range";
        return -1;
    }
    uint8_t enc = value_enc(s);
    if (enc == ANKER_V_U8 && (v < 0 || v > 255)) {
        *err = "takes 0..255";
        return -1;
    }
    if (enc == ANKER_V_U16 && (v < 0 || v > 65535)) {
        *err = "takes 0..65535";
        return -1;
    }
    return 0;
}

static int watt_ok(int32_t w) { return w == 0 || (w >= 15 && w <= 140); }

static int pd_count(int32_t m) {
    int n = 0;
    for (int b = 2; b <= 5; b++) {
        if (m & (1 << b)) { n++; }
    }
    return n;
}

static void fill_job(const anker_setter *s, anker_job *job, int32_t v) {
    memset(job, 0, sizeof *job);
    job->kind = s->kind ? s->kind : (uint8_t)ANKER_JOB_INT;
    job->fe = s->fe;
    job->enc = s->enc ? s->enc : value_enc(s);
    job->nitems = s->nitems;
    job->value = v;
    for (uint8_t i = 0; i < s->nitems && i < 3; i++) {
        job->items[i].tag = s->items[i].tag;
        job->items[i].enc = s->items[i].enc;
        job->items[i].has_const = s->items[i].has_const;
        job->items[i].konst = s->items[i].konst;
    }
}

int anker_setter_check(const anker_setter *s, const anker_val *v, anker_job *job, const char **err) {
    if (err) { *err = "bad value"; }
    if (!s || !v || !job) { return -1; }
    if (s->kind == ANKER_JOB_CUSTOM) {
        if (!v->is_obj) {
            *err = "takes {mode, auto_exit, c1_w, c2_w, c3_w}";
            return -1;
        }
        /* Lua treats 0 as missing (`v.mode or … or 1`) and defaults auto_exit to 1. */
        int32_t mode = (v->have_mode && v->mode) ? v->mode : 1;
        int32_t ax = v->have_auto ? v->auto_exit : 1;
        if (mode < 1 || mode > 254) {
            *err = "mode takes 1..254";
            return -1;
        }
        if (ax != 0 && ax != 1) {
            *err = "auto_exit takes 0 or 1";
            return -1;
        }
        if (!v->have_c1) { *err = "c1_w is missing"; return -1; }
        if (!v->have_c2) { *err = "c2_w is missing"; return -1; }
        if (!v->have_c3) { *err = "c3_w is missing"; return -1; }
        if (!watt_ok(v->c1)) { *err = "c1_w takes 0 or 15..140"; return -1; }
        if (!watt_ok(v->c2)) { *err = "c2_w takes 0 or 15..140"; return -1; }
        if (!watt_ok(v->c3)) { *err = "c3_w takes 0 or 15..140"; return -1; }
        if ((int64_t)v->c1 + v->c2 + v->c3 > 160) {
            *err = "the ports add up to more than 160 W";
            return -1;
        }
        fill_job(s, job, 0);
        job->extra[0] = mode;
        job->extra[1] = ax;
        job->extra[2] = v->c1;
        job->extra[3] = v->c2;
        job->extra[4] = v->c3;
        return 0;
    }
    if (s->kind == ANKER_JOB_PROTO) {
        if (!v->is_obj || !v->have_c1 || !v->have_c2 || !v->have_c3) {
            *err = "takes {c1, c2, c3} bit masks (0..63)";
            return -1;
        }
        int32_t c[3] = { v->c1, v->c2, v->c3 };
        static const char *const bad[] = { "c1 takes 0..63", "c2 takes 0..63", "c3 takes 0..63" };
        static const char *const pd[] = {
            "c1: at most three of PD 12V and the PPS ranges",
            "c2: at most three of PD 12V and the PPS ranges",
            "c3: at most three of PD 12V and the PPS ranges",
        };
        for (int i = 0; i < 3; i++) {
            if (c[i] < 0 || c[i] > 63) { *err = bad[i]; return -1; }
            if (pd_count(c[i]) > 3) { *err = pd[i]; return -1; }
        }
        fill_job(s, job, 0);
        job->extra[0] = c[0];
        job->extra[1] = c[1];
        job->extra[2] = c[2];
        return 0;
    }
    int32_t iv = 0;
    if (v->is_text) {
        if (!named(s, v->text, &iv)) {
            *err = s->nnames ? "not one of the named values" : "not a whole number";
            return -1;
        }
    } else if (v->is_int) {
        iv = v->i;
    } else {
        *err = s->nnames ? "not one of the named values" : "not a whole number";
        return -1;
    }
    if (check_int(s, iv, err) != 0) { return -1; }
    fill_job(s, job, iv);
    return 0;
}

/* ---- decode ---------------------------------------------------------------- */

enum {
    K_NUM = 1,
    K_STR,
    K_TENTH,
    K_PWR,
    K_CTL,
    K_SOFF,
    K_DIR,
    K_LANG,
    K_CUSTOM,
    K_FIXED,
    K_IDENT,
    K_CONN,
    K_ABILITY,
    K_SS
};

typedef struct { uint8_t tag, kind; const char *name; } field_t;

static const field_t F1761[] = {
    { 0xa2, K_NUM, "ac_countdown_s" },
    { 0xa3, K_NUM, "car_countdown_s" },
    { 0xa4, K_TENTH, "remaining_time_h" },
    { 0xa5, K_NUM, "ac_input_power_w" },
    { 0xa6, K_NUM, "ac_output_power_w" },
    { 0xa7, K_NUM, "pd1_output_power_w" },
    { 0xa8, K_NUM, "pd2_output_power_w" },
    { 0xa9, K_NUM, "usb1_output_power_w" },
    { 0xaa, K_NUM, "usb2_output_power_w" },
    { 0xab, K_NUM, "usb3_output_power_w" },
    { 0xac, K_NUM, "usb4_output_power_w" },
    { 0xad, K_NUM, "car1_output_power_w" },
    { 0xae, K_NUM, "solar_input_power_w" },
    { 0xaf, K_NUM, "total_input_power_w" },
    { 0xb0, K_NUM, "total_output_power_w" },
    { 0xb1, K_NUM, "unknown_b1" },
    { 0xb2, K_NUM, "ac_cable_plugged" },
    { 0xb3, K_NUM, "unknown_b3" },
    { 0xb4, K_NUM, "unknown_b4" },
    { 0xb5, K_NUM, "unknown_b5" },
    { 0xb6, K_NUM, "unknown_b6" },
    { 0xb7, K_NUM, "unknown_b7" },
    { 0xb8, K_NUM, "unknown_b8" },
    { 0xb9, K_NUM, "sub_package_version" },
    { 0xba, K_NUM, "firmware_version" },
    { 0xbb, K_NUM, "unknown_bb" },
    { 0xbc, K_NUM, "charge_source_state" },
    { 0xbd, K_NUM, "bms_temp_major_c" },
    { 0xbe, K_NUM, "bms_temp_minor_c" },
    { 0xbf, K_NUM, "bms_state_major" },
    { 0xc0, K_NUM, "bms_state_minor" },
    { 0xc1, K_NUM, "battery_pct" },
    { 0xc2, K_NUM, "battery_pct_minor" },
    { 0xc3, K_NUM, "unknown_c3" },
    { 0xc4, K_NUM, "unknown_c4" },
    { 0xc5, K_NUM, "sub_battery_present" },
    { 0xc6, K_NUM, "pd1_work_state" },
    { 0xc7, K_NUM, "pd2_work_state" },
    { 0xc8, K_NUM, "usb1_work_state" },
    { 0xc9, K_NUM, "usb2_work_state" },
    { 0xca, K_NUM, "usb3_work_state" },
    { 0xcb, K_NUM, "usb4_work_state" },
    { 0xcc, K_NUM, "car1_work_state" },
    { 0xcd, K_NUM, "fault_minor" },
    { 0xce, K_NUM, "device_error_code" },
    { 0xcf, K_NUM, "fault_major" },
    { 0xd0, K_STR, "serial_number" },
    { 0xd1, K_NUM, "ac_charge_power_w" },
    { 0xd2, K_NUM, "device_timeout_min" },
    { 0xd3, K_NUM, "screen_timeout_s" },
    { 0xd4, K_NUM, "inverter_wait_time" },
    { 0xd5, K_STR, "work_limits_major" },
    { 0xd6, K_STR, "work_limits_minor" },
    { 0xd7, K_NUM, "ac_output_enabled" },
    { 0xd8, K_NUM, "car_output_enabled" },
    { 0xd9, K_NUM, "lcd_brightness" },
    { 0xda, K_NUM, "ac_frequency_hz" },
    { 0xdb, K_NUM, "power_saving_mode" },
    { 0xdc, K_NUM, "ambient_light" },
    { 0xdd, K_NUM, "temp_unit" },
    { 0xde, K_NUM, "screen_enabled" },
    { 0xe3, K_NUM, "unknown_e3" },
    { 0xe5, K_NUM, "ultra_fast_charging" },
    { 0xf7, K_NUM, "memory_port_bits" },
    { 0xf8, K_STR, "capability_block" },
    { 0xf9, K_NUM, "voltage_type" },
    { 0xfd, K_STR, "sub_package" },
};

static const field_t F2687[] = {
    { 0xa2, K_NUM, "package_version" },
    { 0xa3, K_NUM, "support_function" },
    { 0xa4, K_NUM, "error_code" },
    { 0xa5, K_PWR, "c1_power" },
    { 0xa6, K_PWR, "c2_power" },
    { 0xa7, K_PWR, "c3_power" },
    { 0xa8, K_SOFF, "screen_off" },
    { 0xa9, K_NUM, "backlight_pct" },
    { 0xaa, K_NUM, "charging_protocol" },
    { 0xab, K_CUSTOM, "custom_power" },
    { 0xac, K_CTL, "c1_control" },
    { 0xad, K_CTL, "c2_control" },
    { 0xae, K_CTL, "c3_control" },
    { 0xaf, K_DIR, "direction" },
    { 0xb0, K_LANG, "language" },
    { 0xb1, K_FIXED, "fixed_allocation" },
    { 0xb2, K_NUM, "gyroscope" },
    { 0xb3, K_NUM, "custom_mode_number" },
    { 0xb4, K_IDENT, "identify_device" },
    { 0xb5, K_CONN, "connected_device" },
    { 0xe0, K_ABILITY, "ability_info" },
    { 0xe1, K_SS, "screensaver" },
};

static int after_type(const anker_in *in, const uint8_t **p, size_t *n) {
    if (!in->raw || in->raw_len < 1) {
        *p = in->val;
        *n = in->val_len;
        return *n > 0;
    }
    *p = in->raw + 1;
    *n = (size_t)in->raw_len - 1;
    return 1;
}

static int take_num(const anker_in *in, uint32_t *out) {
    if (in->has_num) {
        *out = (uint32_t)in->ival;
        return 1;
    }
    const uint8_t *p;
    size_t n;
    if (!after_type(in, &p, &n) || !p) { return 0; }
    if (n == 1) { *out = p[0]; return 1; }
    if (n == 2) { *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8); return 1; }
    if (n == 4) {
        *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        return 1;
    }
    return 0;
}

static uint16_t ru16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ru32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_str(const anker_emit *em, const char *key, const uint8_t *p, size_t n) {
    char buf[96];
    size_t m = 0;
    if (p) {
        while (m < n && m < sizeof buf - 1 && p[m]) { m++; }
    }
    memcpy(buf, p, m);
    buf[m] = 0;
    em->str(em->ctx, key, buf);
}

static void hex_bytes(char *d, const uint8_t *p, size_t n) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        d[i * 2] = H[p[i] >> 4];
        d[i * 2 + 1] = H[p[i] & 0xf];
    }
    d[n * 2] = 0;
}

static void emit_lookup(const anker_emit *em, const char *key, uint32_t n, const char *const *names, int nnames) {
    if ((int)n < nnames) { em->str(em->ctx, key, names[n]); }
    else { em->i64(em->ctx, key, (int64_t)n); }
}

static const char *const SOFF_N[] = { "30s", "1min", "5min", "30min", "12h" };
static const char *const DIR_N[] = { "up", "left", "down", "right" };
static const char *const LANG_N[] = { "en", "zh", "ja", "de" };
static const char *const CABLE_N[] = { "60W", "100W", "240W", "none" };
static const char *const ABIL[] = {
    "screensaver_local", "screensaver_text", "screensaver_network", "screensaver_custom",
    "screensaver_holiday", "tomato_clock", "custom_protocol"
};

typedef struct {
    int have_w[3];
    double watts[3];
    int have_c[3];
    int on[3];
    int active[3];
    uint32_t cd[3], rem[3];
    int have_ss, ss_on;
} derived_t;

static void emit_port(const anker_emit *em, const char *key, const uint8_t *v, size_t n, int idx, derived_t *d) {
    if (n < 7) { return; }
    uint16_t cw = ru16(v + 5);
    em->obj(em->ctx, key);
    em->boolean(em->ctx, "charging", v[0] == 1);
    em->i64(em->ctx, "mv", ru16(v + 1));
    em->i64(em->ctx, "ma", ru16(v + 3));
    em->num(em->ctx, "watts", cw / 100.0);
    em->end(em->ctx);
    if (idx >= 0 && idx < 3) {
        d->have_w[idx] = 1;
        d->watts[idx] = cw / 100.0;
    }
}

static void emit_ctl(const anker_emit *em, const char *key, const uint8_t *v, size_t n, int idx, derived_t *d) {
    if (n < 12) { return; }
    uint32_t cd = ru32(v + 2), rem = ru32(v + 6);
    int active = v[1] == 1;
    em->obj(em->ctx, key);
    em->boolean(em->ctx, "on", v[0] == 1);
    em->boolean(em->ctx, "countdown_active", active);
    em->i64(em->ctx, "countdown_s", cd);
    em->i64(em->ctx, "countdown_remain_s", rem);
    if (v[10] < 4) { em->str(em->ctx, "cable", CABLE_N[v[10]]); }
    else { em->i64(em->ctx, "cable", v[10]); }
    em->i64(em->ctx, "vid", v[11]);
    em->end(em->ctx);
    if (idx >= 0 && idx < 3) {
        d->have_c[idx] = 1;
        d->on[idx] = v[0] == 1;
        d->active[idx] = active;
        d->cd[idx] = cd;
        d->rem[idx] = rem;
    }
}

static void emit_one(const anker_emit *em, uint8_t kind, const char *name, const anker_in *in, derived_t *d) {
    const uint8_t *p;
    size_t n;
    after_type(in, &p, &n);
    uint32_t num = 0;
    int has = take_num(in, &num);
    switch (kind) {
    case K_NUM:
        if (has) { em->i64(em->ctx, name, (int64_t)num); }
        break;
    case K_TENTH:
        if (has) { em->num(em->ctx, name, (double)num * 0.1); }
        break;
    case K_STR:
        if (p && n) { put_str(em, name, p, n); }
        else if (in->val && in->val_len) { put_str(em, name, in->val, in->val_len); }
        break;
    case K_PWR:
        emit_port(em, name, p, n, (int)in->tag - 0xa5, d);
        break;
    case K_CTL:
        emit_ctl(em, name, p, n, (int)in->tag - 0xac, d);
        break;
    case K_SOFF:
        if (has) { emit_lookup(em, name, num, SOFF_N, 5); }
        break;
    case K_DIR:
        if (has) { emit_lookup(em, name, num, DIR_N, 4); }
        break;
    case K_LANG:
        if (has) { emit_lookup(em, name, num, LANG_N, 4); }
        break;
    case K_FIXED:
        if (has) { em->str(em->ctx, name, num == 0 ? "c1_priority" : "laptop_mode"); }
        break;
    case K_CUSTOM:
        if (n < 8 || !p) { break; }
        em->obj(em->ctx, name);
        em->i64(em->ctx, "mode", p[0]);
        em->boolean(em->ctx, "auto_exit", p[1] == 1);
        em->i64(em->ctx, "c1_w", p[2]);
        em->i64(em->ctx, "c2_w", p[3]);
        em->i64(em->ctx, "c3_w", p[4]);
        em->i64(em->ctx, "c1_proto", p[5]);
        em->i64(em->ctx, "c2_proto", p[6]);
        em->i64(em->ctx, "c3_proto", p[7]);
        em->end(em->ctx);
        break;
    case K_IDENT:
        if (n < 12 || !p) { break; }
        em->obj(em->ctx, name);
        for (int i = 0; i < 3; i++) {
            char k[3] = { 'c', (char)('1' + i), 0 };
            uint16_t vid = ru16(p + i * 4), pid = ru16(p + i * 4 + 2);
            em->obj(em->ctx, k);
            if (vid == 65530 || vid == 65531) { em->str(em->ctx, "vid", "unknown"); }
            else { em->i64(em->ctx, "vid", vid); }
            if (pid == 65530 || pid == 65531) { em->str(em->ctx, "pid", "unknown"); }
            else { em->i64(em->ctx, "pid", pid); }
            em->end(em->ctx);
        }
        em->end(em->ctx);
        break;
    case K_CONN:
        if (n < 12 || !p) { break; }
        em->obj(em->ctx, name);
        for (int i = 0; i < 3; i++) {
            char k[3] = { 'c', (char)('1' + i), 0 };
            uint16_t brand = ru16(p + i * 4), model = ru16(p + i * 4 + 2);
            if (brand == 65535 && model == 65535) { em->str(em->ctx, k, "none"); }
            else {
                em->obj(em->ctx, k);
                em->i64(em->ctx, "brand", brand);
                em->i64(em->ctx, "model", model);
                em->end(em->ctx);
            }
        }
        em->end(em->ctx);
        break;
    case K_ABILITY:
        if (!has) { break; }
        em->obj(em->ctx, name);
        {
            char raw[12];
            raw[0] = '0'; raw[1] = 'x';
            char tmp[9];
            int t = 0;
            uint32_t x = num;
            if (x == 0) { tmp[t++] = '0'; }
            else {
                char rev[8];
                int r = 0;
                while (x && r < 8) {
                    static const char H[] = "0123456789abcdef";
                    rev[r++] = H[x & 0xf];
                    x >>= 4;
                }
                while (r) { tmp[t++] = rev[--r]; }
            }
            tmp[t] = 0;
            memcpy(raw + 2, tmp, (size_t)t + 1);
            em->str(em->ctx, "raw", raw);
        }
        for (int i = 0; i < 7; i++) {
            em->boolean(em->ctx, ABIL[i], (num & (1u << i)) != 0);
        }
        em->end(em->ctx);
        break;
    case K_SS:
        if (!p || n < 10) {
            if (p && n && em->hex) { em->hex(em->ctx, name, p, n); }
            break;
        }
        em->obj(em->ctx, name);
        em->boolean(em->ctx, "enabled", (p[0] & 0x80) != 0);
        em->i64(em->ctx, "flags", p[0] & 0x7f);
        em->i64(em->ctx, "type", p[1]);
        em->i64(em->ctx, "id", ru32(p + 2));
        em->i64(em->ctx, "holiday_id", ru32(p + 6));
        em->end(em->ctx);
        d->have_ss = 1;
        d->ss_on = (p[0] & 0x80) != 0;
        break;
    default:
        break;
    }
}

int anker_decode(int id, anker_next_fn next, void *st, int want_unknown, const anker_emit *em, int *tlv_count) {
    const field_t *fields = NULL;
    size_t nf = 0;
    if (id == ANKER_MODEL_A1761) {
        fields = F1761; nf = sizeof F1761 / sizeof F1761[0];
    } else if (id == ANKER_MODEL_A2687) {
        fields = F2687; nf = sizeof F2687 / sizeof F2687[0];
    }
    if (!next || !em || !em->i64 || !em->str || !em->obj || !em->end) { return -1; }
    derived_t d;
    memset(&d, 0, sizeof d);
    int count = 0;
    uint8_t utag[8];
    char uhex[8][40];
    int nu = 0;
    anker_in in;
    while (next(st, &in)) {
        count++;
        if (in.tag == 0xa1 || in.tag == 0xfe) { continue; }
        const char *name = NULL;
        uint8_t kind = 0;
        for (size_t i = 0; i < nf; i++) {
            if (fields[i].tag == in.tag) {
                name = fields[i].name;
                kind = fields[i].kind;
                break;
            }
        }
        if (name) {
            emit_one(em, kind, name, &in, &d);
        } else if (want_unknown && nu < 8) {
            const uint8_t *p;
            size_t n;
            after_type(&in, &p, &n);
            if (n > 16) { n = 16; }
            utag[nu] = in.tag;
            if (p && n) { hex_bytes(uhex[nu], p, n); }
            else { uhex[nu][0] = 0; }
            nu++;
        }
    }
    if (want_unknown && nu && em->obj) {
        em->obj(em->ctx, "unknown");
        for (int i = 0; i < nu; i++) {
            char k[3];
            static const char H[] = "0123456789abcdef";
            k[0] = H[utag[i] >> 4];
            k[1] = H[utag[i] & 0xf];
            k[2] = 0;
            em->str(em->ctx, k, uhex[i]);
        }
        em->end(em->ctx);
    }
    if (id == ANKER_MODEL_A2687) {
        for (int i = 0; i < 3; i++) {
            char k[24];
            if (d.have_c[i]) {
                k[0] = 'c'; k[1] = (char)('1' + i); k[2] = 0;
                memcpy(k + 2, "_on", 4);
                em->i64(em->ctx, k, d.on[i] ? 1 : 0);
                memcpy(k + 2, "_timer_s", 9);
                em->i64(em->ctx, k, d.active[i] ? (int64_t)d.cd[i] : 0);
                memcpy(k + 2, "_timer_remain_s", 16);
                em->i64(em->ctx, k, d.active[i] ? (int64_t)d.rem[i] : 0);
            }
            if (d.have_w[i] && em->num) {
                k[0] = 'c'; k[1] = (char)('1' + i); k[2] = '_'; k[3] = 'w'; k[4] = 0;
                em->num(em->ctx, k, d.watts[i]);
            }
        }
        if (d.have_ss) { em->i64(em->ctx, "screensaver_enabled", d.ss_on ? 1 : 0); }
    }
    if (tlv_count) { *tlv_count = count; }
    return 0;
}
