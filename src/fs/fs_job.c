#include "fs_job.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "api/api.h"

typedef struct {
    bool dirty;
    uint32_t id;
    char kind[8];
    char state[8];
    int index;
    int total;
    char path[128];
    uint32_t size;
} fs_job_note_t;

static fs_job_note_t note;

void fs_job_set(uint32_t id, const char *kind, const char *state,
                int index, int total, const char *path, uint32_t size) {
    if (!id) { return; }
    note.id = id;
    snprintf(note.kind, sizeof note.kind, "%s", kind ? kind : "");
    snprintf(note.state, sizeof note.state, "%s", state ? state : "run");
    note.index = index;
    note.total = total;
    snprintf(note.path, sizeof note.path, "%s", path ? path : "");
    note.size = size;
    note.dirty = true;
}

void fs_job_poll(void) {
    if (!note.dirty) { return; }
    note.dirty = false;
    char *dst = api_scratch();
    size_t cap = api_scratch_size();
    int n = snprintf(dst, cap, "{\"id\":%lu,\"kind\":\"%s\",\"state\":\"%s\",\"index\":%d,\"total\":%d,\"path\":\"",
                     (unsigned long)note.id, note.kind, note.state, note.index, note.total);
    if (n < 0 || (size_t)n >= cap) { return; }
    size_t u = (size_t)n;
    size_t e = api_json_escape(dst + u, cap - u, note.path);
    u += e;
    int t = snprintf(dst + u, cap - u, "\",\"size\":%lu}", (unsigned long)note.size);
    if (t < 0 || u + (size_t)t >= cap) { return; }
    api_emit("fs.job", dst);
}
