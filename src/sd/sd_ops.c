/*
 * The SD card on the module registry: sd.status (the same JSON as
 * GET /api/sd). Its presence in sys.info's "modules" is how the web app
 * knows the build has a card slot; files go over HTTP (/api/sd).
 */
#include "api/ops.h"
#include "api/api.h"
#include "autoconf.h"

#if CONFIG_SD
#include "sd/sd_api.h"

static void op_sd_status(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char *buf = api_scratch();
    if (sd_api_status_json(buf, api_scratch_size()) < 0) {
        api_reply_err(reply, ctx, id, "internal", "status too long");
        return;
    }
    api_reply_ok(reply, ctx, id, buf);
}

static const op_def_t sd_ops[] = {
    { "sd.status", op_sd_status },
    { NULL, NULL }
};

const op_module_t op_module_sd = {
    .ns = "sd", .ops = sd_ops, .topics = NULL, .init = NULL
};
#endif
