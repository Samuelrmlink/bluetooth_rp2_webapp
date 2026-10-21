/*
 * The sched group: cooperative tasks on the host's main loop (spec 7.1). The
 * firmware is one loop with no RTOS; pex_poll() runs whatever is due, each
 * callback through the trampoline of the module that registered it. A task
 * counts as a reference of its module (an app is "running" while it has one),
 * so unload is refused until the app ends or is stopped.
 */
#include <string.h>
#include "pex_internal.h"

enum { T_FREE = 0, T_EVERY, T_AFTER, T_POST };

typedef struct {
    int id;
    uint8_t kind;
    pex_mod_t *mod;
    pex_task_fn fn;
    void *arg;
    uint32_t period, due;
} task_t;

static task_t s_tasks[PEX_MAX_TASKS];
static int s_next_id = 1;

void pex__sched_init(void) {
    memset(s_tasks, 0, sizeof s_tasks);
}

static int add(uint8_t kind, uint32_t ms, pex_task_fn fn, void *arg) {
    pex_mod_t *m = pex__current();
    if (!fn) { return PEX_EINVAL; }
    for (int i = 0; i < PEX_MAX_TASKS; i++) {
        task_t *t = &s_tasks[i];
        if (t->kind == T_FREE) {
            t->kind = kind;
            t->mod = m;
            t->fn = fn;
            t->arg = arg;
            t->period = ms ? ms : 1;
            t->due = pex_port_ms() + (kind == T_POST ? 0 : ms);
            t->id = s_next_id++;
            if (s_next_id <= 0) { s_next_id = 1; }
            if (m) { m->tasks++; }
            return t->id;
        }
    }
    return PEX_ENOMEM;
}

static void drop(task_t *t) {
    if (t->mod) {
        t->mod->tasks--;
        if (t->mod->tasks == 0 && t->mod->state == PEX_ST_RUNNING) { t->mod->state = PEX_ST_EXITED; }
    }
    t->kind = T_FREE;
    t->mod = NULL;
}

int pex_sched_every(uint32_t period_ms, pex_task_fn fn, void *arg) { return add(T_EVERY, period_ms, fn, arg); }
int pex_sched_after(uint32_t delay_ms, pex_task_fn fn, void *arg) { return add(T_AFTER, delay_ms, fn, arg); }
int pex_sched_post(pex_task_fn fn, void *arg) { return add(T_POST, 0, fn, arg); }

int pex_sched_cancel(int id) {
    pex_mod_t *m = pex__current();
    for (int i = 0; i < PEX_MAX_TASKS; i++) {
        task_t *t = &s_tasks[i];
        if (t->kind != T_FREE && t->id == id) {
            if (m && t->mod != m) { return PEX_EINVAL; }     /* not yours */
            drop(t);
            return PEX_OK;
        }
    }
    return PEX_ENOENT;
}

uint32_t pex_sched_count(void) {
    pex_mod_t *m = pex__current();
    uint32_t n = 0;
    for (int i = 0; i < PEX_MAX_TASKS; i++) {
        if (s_tasks[i].kind != T_FREE && s_tasks[i].mod == m) { n++; }
    }
    return n;
}

uint32_t pex__sched_cancel_owner(pex_mod_t *m) {
    uint32_t n = 0;
    for (int i = 0; i < PEX_MAX_TASKS; i++) {
        if (s_tasks[i].kind != T_FREE && s_tasks[i].mod == m) { drop(&s_tasks[i]); n++; }
    }
    return n;
}

void pex_poll(void) {
    uint32_t now = pex_port_ms();
    /* Each slot at most once per pass; a task added during the pass waits. */
    int run_ids[PEX_MAX_TASKS];
    int n = 0;
    for (int i = 0; i < PEX_MAX_TASKS; i++) {
        task_t *t = &s_tasks[i];
        if (t->kind != T_FREE && (int32_t)(now - t->due) >= 0) { run_ids[n++] = t->id; }
    }
    for (int k = 0; k < n; k++) {
        task_t *t = NULL;
        for (int i = 0; i < PEX_MAX_TASKS; i++) {
            if (s_tasks[i].kind != T_FREE && s_tasks[i].id == run_ids[k]) { t = &s_tasks[i]; break; }
        }
        if (!t) { continue; }                     /* cancelled meanwhile */
        pex_mod_t *m = t->mod;
        if (m && m->state == PEX_ST_FAULTED) { drop(t); continue; }
        pex_task_fn fn = t->fn;
        void *arg = t->arg;
        int id = t->id;
        if (t->kind == T_EVERY) {
            t->due += t->period;
            if ((int32_t)(now - t->due) > 0) { t->due = now + t->period; }   /* no catch-up bursts */
        }
        int rc = m ? (int)pex_call(m, (uint32_t)(uintptr_t)fn, (uint32_t)(uintptr_t)arg, 0, 0, 0)
                   : fn(arg);
        /* the slot may have been cancelled (or reused) inside the callback */
        if (t->kind != T_FREE && t->id == id && (t->kind != T_EVERY || rc != PEX_TASK_AGAIN)) {
            drop(t);
        }
    }
    /* deferred: app exits and unload-on-exit */
    for (pex_mod_t *m = pex__mods, *nx; m; m = nx) {
        nx = m->next;
        if (m->exit_pending) {
            m->exit_pending = 0;
            pex__sched_cancel_owner(m);
            m->state = PEX_ST_EXITED;
        }
        if ((m->flags & PEX_F_UNLOAD_ON_EXIT) && m->state == PEX_ST_EXITED && m->tasks == 0 && !m->active) {
            m->unload_pending = 1;
        }
        if (m->unload_pending && !m->active) {
            m->unload_pending = 0;
            pex__logf(PEX_LOG_INFO, "pex: %s exited (%d), unloading", m->name, m->exit_code);
            pex_unload(m);
        }
    }
}
