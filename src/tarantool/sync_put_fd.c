/* src/tarantool/sync_put_fd.c
 *
 * Синхронная обёртка над s3_object_put_from_fd() для использования в Tarantool-модуле.
 *
 * ВАЖНО:
 *   - Используем только module.h-API: fiber_cond_new/delete/wait/signal.
 *   - Не дергаем ev_run(), event loop крутит сам Tarantool.
 *   - Блокируется только текущий fiber.
 */

#include <s3-tarantool.h>
#include <s3/client.h>
#include <s3/types.h>

#include <tarantool/module.h>  /* fiber_cond_* из модульного API */
#include <stdbool.h>
#include <string.h>

/* Контекст ожидания завершения PUT */
typedef struct s3_sync_put_ctx {
    s3_error_t          err;
    bool                done;
    struct fiber_cond  *cond;   /* opaque, создаётся через fiber_cond_new() */
} s3_sync_put_ctx_t;

/* done_cb для async-слоя */
static void
s3_sync_put_done_cb(struct s3_request     *req,
                    const struct s3_error *error,
                    void                  *user_data)
{
    (void)req;
    s3_sync_put_ctx_t *ctx = (s3_sync_put_ctx_t *)user_data;
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
s3_sync_put_from_fd(s3_client_t  *client,
                    const char   *bucket,
                    const char   *key,
                    int           fd,
                    int64_t       offset,
                    uint64_t      limit)
{
    if (client == NULL || bucket == NULL || key == NULL || fd < 0) {
        return S3_E_INVALID_ARGUMENT;
    }

    /* Готовим параметры PUT. */
    s3_object_put_params_t params;
    memset(&params, 0, sizeof(params));
    params.bucket = bucket;
    params.key    = key;

    if (limit != (uint64_t)-1)
        params.content_length = limit;
    else
        params.content_length = (uint64_t)-1;

    params.content_type = NULL; /* можно расширить API позже */

    s3_sync_put_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    s3_error_clear(&ctx.err);
    ctx.done = false;
    ctx.cond = fiber_cond_new();
    if (ctx.cond == NULL) {
        /* Не смогли создать условную переменную. */
        return S3_E_INTERNAL;
    }

    /* Стартуем асинхронный PUT из fd. */
    s3_request_t *req = s3_object_put_from_fd(
        client,
        &params,
        fd,
        offset,
        limit,
        s3_sync_put_done_cb,
        &ctx);

    if (req == NULL) {
        if (ctx.err.code == S3_OK)
            ctx.err.code = S3_E_INTERNAL;
        fiber_cond_delete(ctx.cond);
        return ctx.err.code;
    }

    /* Ожидаем завершения в текущем fiber. */
    while (!ctx.done) {
        fiber_cond_wait(ctx.cond);
    }

    fiber_cond_delete(ctx.cond);
    return ctx.err.code;
}
