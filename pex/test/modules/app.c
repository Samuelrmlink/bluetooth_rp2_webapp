/* app: a cooperative app (PEX_APP_RUN) on the host's main loop through the
 * sched group. Config JSON: {"ticks":N,"period":ms}. It ends itself after N
 * ticks, or when the host asks it to stop (PEX_APP_STOP). */
#include "pex.h"

static int ticks, target = 5, posted, task_id, stopped;
static uint32_t last_ms;

static int json_int(const char *json, const char *key, int def) {
    if (!json) { return def; }
    size_t kl = strlen(key);
    for (const char *p = json; *p; p++) {
        if (*p == '"' && strncmp(p + 1, key, kl) == 0 && p[1 + kl] == '"') {
            p += kl + 2;
            while (*p == ' ' || *p == ':') { p++; }
            int v = 0, neg = 0;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); }
            return neg ? -v : v;
        }
    }
    return def;
}

static int on_post(void *arg) { posted = (int)(intptr_t)arg; return PEX_TASK_DONE; }

static int on_tick(void *arg) {
    ticks++;
    last_ms = pex_ms();
    if (ticks >= target) {
        pex_log(PEX_LOG_INFO, "app: %d ticks, done", ticks);
        return PEX_TASK_DONE;
    }
    return PEX_TASK_AGAIN;
}

int app_ticks(void) { return ticks; }
PEX_EXPORT(app_ticks);
int app_posted(void) { return posted; }
PEX_EXPORT(app_posted);
int app_stopped(void) { return stopped; }
PEX_EXPORT(app_stopped);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    switch (reason) {
    case PEX_APP_RUN: {
        const char *cfg = arg;
        target = json_int(cfg, "ticks", 5);
        int period = json_int(cfg, "period", 10);
        ticks = 0;
        stopped = 0;
        task_id = pex_sched_every((uint32_t)period, on_tick, NULL);
        if (task_id <= 0) { return task_id; }
        pex_sched_post(on_post, (void *)(intptr_t)77);
        pex_log(PEX_LOG_INFO, "app: running, %d ticks every %d ms", target, period);
        return PEX_OK;
    }
    case PEX_APP_STOP:
        stopped = 1;
        pex_sched_cancel(task_id);
        return PEX_OK;
    }
    return PEX_OK;
}
