/* gwnet: the net group. APP_RUN {"url":"http://..","file_url":"http://..","file":"lfs:/.."}:
 * one request into a buffer, one into a file; op pextest.net reports. */
#include "pex.h"

static char body[256];
static char res1[96] = "pending", res2[96] = "pending";
static char file[64];
static char url2[256];

static void done2(void *arg, const pex_http_res_t *r) {
    pex_fs_entry_t st;
    int rc = pex_fs_stat(file, &st, 0, 0);
    pex_snprintf(res2, sizeof res2, "status=%d err=%d bytes=%u file=%d size=%u", r->status, r->err,
                 (unsigned)r->bytes, rc, rc == 0 ? (unsigned)st.size : 0);
    pex_app_exit(0);
}

static void done1(void *arg, const pex_http_res_t *r) {
    pex_snprintf(res1, sizeof res1, "status=%d err=%d bytes=%u body=%.40s", r->status, r->err, (unsigned)r->bytes,
                 r->status == 200 ? body : "");
    if (url2[0]) {
        pex_http_req_t q;
        memset(&q, 0, sizeof q);
        q.url = url2;
        q.to_file = file;
        if (pex_http_start(&q, done2, 0) <= 0) { pex_snprintf(res2, sizeof res2, "start failed"); pex_app_exit(1); }
    } else {
        pex_app_exit(0);
    }
}

static int op_net(void *arg, const char *params, char *reply, size_t cap) {
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "buffer", res1);
    pex_jw_str(&w, "file", res2);
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static int keepalive(void *arg) { return PEX_TASK_AGAIN; }

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    if (reason == PEX_INIT) { return pex_bus_op("pextest.net", op_net, 0); }
    if (reason == PEX_APP_RUN) {
        static char url[256];
        if (!pex_json_str(arg, "url", url, sizeof url)) { return PEX_EINVAL; }
        pex_json_str(arg, "file_url", url2, sizeof url2);
        if (!pex_json_str(arg, "file", file, sizeof file)) { pex_snprintf(file, sizeof file, "lfs:/pextest_dl.bin"); }
        pex_http_req_t q;
        memset(&q, 0, sizeof q);
        q.url = url;
        q.buf = body;
        q.buf_cap = sizeof body - 1;
        int id = pex_http_start(&q, done1, 0);
        if (id <= 0) { pex_snprintf(res1, sizeof res1, "start failed %d", id); return id; }
        pex_sched_every(500, keepalive, 0);     /* "running" until pex_app_exit */
    }
    return PEX_OK;
}
