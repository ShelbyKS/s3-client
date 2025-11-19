/* src/tarantool/sync_get_fd.c
 *
 * Синхронная обёртка над s3_object_get_to_fd() для Tarantool-модуля.
 *
 *  - Не управляем event loop;
 *  - ждём завершения async GET через fiber_cond.
 */

#include <s3-tarantool.h>
#include <s3/client.h>
#include <s3/types.h>

#include <tarantool/module.h>
#include <stdbool.h>
#include <string.h>

/* Контекст ожидания завершения GET */
typedef struct s3_sync_get_ctx {
    s3_error_t          err;
    bool                done;
    struct fiber_cond  *cond;
} s3_sync_get_ctx_t;

static void
s3_sync_get_done_cb(struct s3_request     *req,
                    const struct s3_error *error,
                    void                  *user_data)
{
    (void)req;
    s3_sync_get_ctx_t *ctx = (s3_sync_get_ctx_t *)user_data;
    if (ctx == NULL || ctx->cond == NULL)
        return;

    if (error != NULL) {
        ctx->err = *error;
    } else {
        s3_error_clear(&ctx->err);
        ctx->err.code = S3_OK;
    }

    ctx->done = true;
    fiber_cond_signal(ctx->cond);
}

s3_tarantool_rc_t
s3_sync_get_to_fd(s3_client_t    *client,
                  const char     *bucket,
                  const char     *key,
                  int             fd,
                  int64_t         offset)
{
    if (client == NULL || bucket == NULL || key == NULL || fd < 0) {
        return S3_E_INVALID_ARGUMENT;
    }

    s3_object_get_params_t params;
    memset(&params, 0, sizeof(params));
    params.bucket    = bucket;
    params.key       = key;
    params.use_range = 0; /* без Range */

    s3_sync_get_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    s3_error_clear(&ctx.err);
    ctx.done = false;
    ctx.cond = fiber_cond_new();
    if (ctx.cond == NULL) {
        return S3_E_INTERNAL;
    }

    s3_request_t *req = s3_object_get_to_fd(
        client,
        &params,
        fd,
        offset,
        /* headers_cb = */ NULL,
        s3_sync_get_done_cb,
        &ctx);

    if (req == NULL) {
        if (ctx.err.code == S3_OK)
            ctx.err.code = S3_E_INTERNAL;
        fiber_cond_delete(ctx.cond);
        return ctx.err.code;
    }

    while (!ctx.done) {
        fiber_cond_wait(ctx.cond);
    }

    fiber_cond_delete(ctx.cond);
    return ctx.err.code;
}
