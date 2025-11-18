/* src/highlevel/get_buffer.c
 *
 * High-level helper: GET → s3_buffer_t.
 *
 * Обёртка над низкоуровневым s3_object_get(), которая:
 *   - складывает body-данные в пользовательский s3_buffer_t;
 *   - по желанию проксирует headers_cb и done_cb пользователю;
 *   - следит за переполнением буфера (ошибка, если данных больше, чем size).
 *
 * Пользователь сам владеет буфером:
 *
 *   s3_buffer_t buf = {
 *       .data = my_mem,
 *       .size = my_mem_size,
 *       .used = 0,
 *   };
 *
 *   s3_object_get_to_buffer(client, &params, &buf, headers_cb, done_cb, udata);
 */

#include <s3/client.h>
#include <s3/types.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------
 * Внутренний контекст для GET → buffer
 * ------------------------------------------------------ */

typedef struct s3_get_buffer_ctx {
    s3_buffer_t                 *buffer;
    s3_object_get_headers_cb     user_headers_cb;
    s3_request_done_cb           user_done_cb;
    void                        *user_data;
} s3_get_buffer_ctx_t;

/* --------------------------------------------------------
 * Обёртки callback-ов
 * ------------------------------------------------------ */

/*
 * Обёртка для headers_cb: просто проксирует вызов, но с user_data
 * из контекста, а не с указателя на контекст.
 */
static void
s3_get_buffer_headers_cb(struct s3_request      *req,
                         const s3_object_info_t *info,
                         void                   *ctx_ptr)
{
    s3_get_buffer_ctx_t *ctx = (s3_get_buffer_ctx_t *)ctx_ptr;
    if (ctx->user_headers_cb != NULL) {
        ctx->user_headers_cb(req, info, ctx->user_data);
    }
}

/*
 * Обработчик тела ответа: складываем данные в буфер.
 *
 *  - data/len действительны только во время вызова;
 *  - при переполнении буфера возвращаем !=0, чтобы прервать запрос.
 */
static int
s3_get_buffer_data_cb(struct s3_request *req,
                      const uint8_t     *data,
                      size_t             len,
                      void              *ctx_ptr)
{
    (void)req;

    s3_get_buffer_ctx_t *ctx = (s3_get_buffer_ctx_t *)ctx_ptr;
    s3_buffer_t *buf = ctx->buffer;

    if (buf == NULL || buf->data == NULL || buf->size == 0) {
        /* Логическая ошибка в использовании API — останавливаем запрос. */
        return -1;
    }

    /* Проверка на переполнение: ещё len байт поместятся? */
    if (buf->used + len > buf->size) {
        /* Данных больше, чем пользователь выделил памяти. */
        return -1; /* приведёт к завершению запроса с ошибкой */
    }

    memcpy(buf->data + buf->used, data, len);
    buf->used += len;

    return 0; /* всё ок, продолжаем загрузку */
}

/*
 * Обёртка для done_cb:
 *   - освобождает внутренний контекст;
 *   - затем зовёт пользовательский done_cb (если он есть).
 */
static void
s3_get_buffer_done_cb(struct s3_request     *req,
                      const s3_error_t      *error,
                      void                  *ctx_ptr)
{
    s3_get_buffer_ctx_t *ctx = (s3_get_buffer_ctx_t *)ctx_ptr;

    s3_request_done_cb user_done  = ctx->user_done_cb;
    void              *user_data  = ctx->user_data;

    /* Сначала освобождаем наш контекст, чтобы не было утечки. */
    free(ctx);

    /* Затем уже вызываем пользовательский колбэк. */
    if (user_done != NULL) {
        user_done(req, error, user_data);
    }
}

/* --------------------------------------------------------
 * Публичное API: s3_object_get_to_buffer()
 * ------------------------------------------------------ */

s3_request_t *
s3_object_get_to_buffer(
    s3_client_t                  *client,
    const s3_object_get_params_t *params,
    s3_buffer_t                  *buffer,
    s3_object_get_headers_cb      headers_cb,
    s3_request_done_cb            done_cb,
    void                         *user_data)
{
    /* Базовая валидация аргументов. */
    if (client == NULL || params == NULL ||
        buffer == NULL || buffer->data == NULL || buffer->size == 0 ||
        params->bucket == NULL || params->key == NULL) {

        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INVALID_ARGUMENT;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get_to_buffer: invalid arguments");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    /* Начинаем всегда с пустого used — будем накапливать данные. */
    buffer->used = 0;

    /* Выделяем внутренний контекст. */
    s3_get_buffer_ctx_t *ctx = (s3_get_buffer_ctx_t *)malloc(sizeof(*ctx));
    if (ctx == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get_to_buffer: out of memory");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    ctx->buffer          = buffer;
    ctx->user_headers_cb = headers_cb;
    ctx->user_done_cb    = done_cb;
    ctx->user_data       = user_data;

    /*
     * Запускаем низкоуровневый асинхронный GET.
     *
     *  - headers_cb (если задан) оборачиваем в s3_get_buffer_headers_cb;
     *  - data_cb всегда наш (s3_get_buffer_data_cb);
     *  - done_cb — наша обёртка s3_get_buffer_done_cb.
     *
     * Внутренний user_data для низкоуровневого запроса — это ctx.
     */
    s3_request_t *req = s3_object_get(
        client,
        params,
        (headers_cb != NULL) ? s3_get_buffer_headers_cb : NULL,
        s3_get_buffer_data_cb,
        s3_get_buffer_done_cb,
        ctx);

    if (req == NULL) {
        /* Не удалось даже стартовать запрос. Освобождаем контекст и
         * синхронно сообщаем пользователю об ошибке. */
        free(ctx);

        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get_to_buffer: failed to start request");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    return req;
}
