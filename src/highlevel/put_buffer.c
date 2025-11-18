/* src/highlevel/put_buffer.c
 *
 * High-level helper: PUT ← s3_buffer_t.
 *
 * Обёртка над s3_object_put(), которая:
 *   - берёт данные из пользовательского s3_buffer_t;
 *   - стримит их в curl через s3_object_put_read_cb;
 *   - следит за концом данных и EOF.
 *
 * ВАЖНО:
 *   - Память под buffer->data принадлежит пользователю;
 *   - Библиотека НИЧЕГО не модифицирует в этом буфере;
 *   - Пользователь обязан не освобождать буфер до вызова done_cb.
 */

#include <s3/client.h>
#include <s3/types.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* --------------------------------------------------------
 * Внутренний контекст для PUT ← buffer
 * ------------------------------------------------------ */

typedef struct s3_put_buffer_ctx {
    const s3_buffer_t     *buffer;     /* источник данных */
    uint64_t               total_len;  /* сколько байт нужно отправить */
    uint64_t               offset;     /* сколько уже отправлено */
    s3_request_done_cb     user_done_cb;
    void                  *user_data;
} s3_put_buffer_ctx_t;

/* --------------------------------------------------------
 * read_cb: отдаём данные из буфера
 * ------------------------------------------------------ */

static int
s3_put_buffer_read_cb(struct s3_request *req,
                      uint8_t           *buf,
                      size_t             len,
                      size_t            *out_len,
                      int               *eof,
                      void              *ctx_ptr)
{
    (void)req;

    s3_put_buffer_ctx_t *ctx = (s3_put_buffer_ctx_t *)ctx_ptr;
    const s3_buffer_t *src = ctx->buffer;

    if (src == NULL || src->data == NULL || ctx->total_len > src->used) {
        /* Логическая ошибка использования API — прерываем запрос. */
        *out_len = 0;
        *eof = 1;
        return -1;
    }

    if (ctx->offset >= ctx->total_len) {
        /* Всё уже отправлено. */
        *out_len = 0;
        *eof = 1;
        return 0;
    }

    uint64_t remaining = ctx->total_len - ctx->offset;
    size_t to_send = (remaining < (uint64_t)len)
                     ? (size_t)remaining
                     : len;

    if (to_send > 0) {
        memcpy(buf, src->data + ctx->offset, to_send);
        ctx->offset += (uint64_t)to_send;
    }

    *out_len = to_send;
    *eof = (ctx->offset >= ctx->total_len) ? 1 : 0;

    return 0;
}

/* --------------------------------------------------------
 * done_cb: освобождаем контекст и вызываем пользовательский
 * ------------------------------------------------------ */

static void
s3_put_buffer_done_cb(struct s3_request *req,
                      const s3_error_t  *error,
                      void              *ctx_ptr)
{
    s3_put_buffer_ctx_t *ctx = (s3_put_buffer_ctx_t *)ctx_ptr;

    s3_request_done_cb user_done = ctx->user_done_cb;
    void              *user_data = ctx->user_data;

    free(ctx);

    if (user_done != NULL) {
        user_done(req, error, user_data);
    }
}

/* --------------------------------------------------------
 * Публичное API: s3_object_put_from_buffer()
 * ------------------------------------------------------ */

s3_request_t *
s3_object_put_from_buffer(
    s3_client_t                  *client,
    const s3_object_put_params_t *params,
    const s3_buffer_t            *buffer,
    s3_request_done_cb            done_cb,
    void                         *user_data)
{
    /* Базовая валидация аргументов. */
    if (client == NULL || params == NULL || buffer == NULL ||
        buffer->data == NULL ||
        params->bucket == NULL || params->key == NULL) {

        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INVALID_ARGUMENT;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put_from_buffer: invalid arguments");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    /*
     * Вычисляем total_len:
     *   - если params->content_length != (uint64_t)-1 — используем его,
     *     но он НЕ должен превышать buffer->used;
     *   - если == (uint64_t)-1 — считаем, что content_length = buffer->used.
     */
    uint64_t total_len = 0;

    if (params->content_length != (uint64_t)-1) {
        total_len = params->content_length;
        if (total_len > (uint64_t)buffer->used) {
            if (done_cb != NULL) {
                s3_error_t err;
                s3_error_clear(&err);
                err.code = S3_E_INVALID_ARGUMENT;
                (void)snprintf(err.message, sizeof(err.message),
                               "s3_object_put_from_buffer: content_length (%llu) > buffer->used (%zu)",
                               (unsigned long long)total_len,
                               buffer->used);
                done_cb(NULL, &err, user_data);
            }
            return NULL;
        }
    } else {
        /* Если длина не указана, отправляем ровно buffer->used байт. */
        total_len = (uint64_t)buffer->used;
    }

    /* Если total_len == 0 — это легальный кейс (пустой объект). */

    /* Готовим локальную копию params, чтобы скорректировать content_length,
     * не меняя структуру пользователя. */
    s3_object_put_params_t params_local = *params;
    params_local.content_length = total_len;

    /* Выделяем внутренний контекст. */
    s3_put_buffer_ctx_t *ctx = (s3_put_buffer_ctx_t *)malloc(sizeof(*ctx));
    if (ctx == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put_from_buffer: out of memory");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    ctx->buffer       = buffer;
    ctx->total_len    = total_len;
    ctx->offset       = 0;
    ctx->user_done_cb = done_cb;
    ctx->user_data    = user_data;

    /*
     * Стартуем низкоуровневый асинхронный PUT.
     *
     *  - read_cb всегда наш (s3_put_buffer_read_cb);
     *  - done_cb — наша обёртка s3_put_buffer_done_cb;
     *  - headers_cb здесь не нужен (для PUT мы его не поддерживаем в API).
     */
    s3_request_t *req = s3_object_put(
        client,
        &params_local,
        s3_put_buffer_read_cb,
        s3_put_buffer_done_cb,
        ctx);

    if (req == NULL) {
        free(ctx);

        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put_from_buffer: failed to start request");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    return req;
}
