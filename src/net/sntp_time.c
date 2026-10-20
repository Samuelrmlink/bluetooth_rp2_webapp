#include "net/sntp_time.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "autoconf.h"
#include "fs/fs.h"
#include "pico/time.h"

#if CONFIG_NET_SNTP
#include "lwip/apps/sntp.h"
#endif

static bool s_valid;
static int64_t s_offset_us;       /* unix time minus time since boot */
static char s_server[64] = "pool.ntp.org";

bool sntp_time_valid(void) { return s_valid; }
const char *sntp_time_server(void) { return s_server; }

int64_t sntp_time_unix(void) {
    if (!s_valid) { return 0; }
    return ((int64_t)to_us_since_boot(get_absolute_time()) + s_offset_us) / 1000000;
}

void sntp_time_set_us(uint32_t sec, uint32_t us) {
    int64_t unix_us = (int64_t)sec * 1000000 + us;
    s_offset_us = unix_us - (int64_t)to_us_since_boot(get_absolute_time());
    /* newlib's time() too: mbedTLS checks certificate dates through it. */
    struct timeval tv = { .tv_sec = (time_t)sec, .tv_usec = (suseconds_t)us };
    settimeofday(&tv, NULL);
    if (!s_valid) { printf("sntp: clock set from %s, unix %lu\n", s_server, (unsigned long)sec); }
    s_valid = true;
}

/* {"server":"host"} -- one field, read the way sys.secret reads one. */
static void load_server(void) {
    static char buf[256];
    size_t sz = 0;
    if (!fs_stat("cfg/ntp.json", &sz) || sz >= sizeof buf) { return; }
    int h = fs_begin_read("cfg/ntp.json", NULL);
    if (h < 0) { return; }
    int n = fs_read(h, (uint8_t *)buf, sz);
    fs_end_read(h);
    if (n <= 0) { return; }
    buf[n] = 0;
    const char *k = strstr(buf, "\"server\"");
    if (!k) { return; }
    const char *v = strchr(k + 8, ':');
    if (!v) { return; }
    v++;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') { v++; }
    if (*v != '"') { return; }
    v++;
    const char *e = strchr(v, '"');
    if (!e || e == v || (size_t)(e - v) >= sizeof s_server) { return; }
    memcpy(s_server, v, (size_t)(e - v));
    s_server[e - v] = 0;
}

void sntp_time_init(void) {
#if CONFIG_NET_SNTP
    load_server();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, s_server);
    sntp_init();
    printf("sntp: polling %s\n", s_server);
#endif
}
