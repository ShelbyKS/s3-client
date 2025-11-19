/* src/highlevel/get_fd.c
 *
 * High-level helper: GET → файловый дескриптор.
 *
 * Обёртка над s3_object_get(), которая:
 *   - пишет тело ответа в заданный fd;
 *   - по желанию проксирует headers_cb и done_cb пользователю;
 *   - умеет начинать запись с указанного смещения (offset);
 *
 * ВАЖНО:
 *   - реализация использует обычный blocking write(2);
 *   - в Tarantool-сценариях, если нужен неблокирующий путь,
 *     можно сделать sync-обёртку поверх coio/thread pool.
 */

#include <s3/client.h>
#include <s3/types.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h> /* write, lseek */
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>  /* off_t */

/* --------------------------------------------------------
 * Внутренний контекст для GET → fd
 * ------------------------------------------------------ */

typedef struct s3_get_fd_ctx {
    int                          fd;
    int64_t                      pos;            /* текущая позиция для pwrite */
    s3_object_get_headers_cb     user_headers_cb;
    s3_request_done_cb           user_done_cb;
    void                        *user_data;
} s3_get_fd_ctx_t;

/* --------------------------------------------------------
 * Обёртки callback-ов
 * ------------------------------------------------------ */

/* headers_cb: просто проксируем в пользовательский, если он есть. */
static void
s3_get_fd_headers_cb(struct s3_request      *req,
                     const s3_object_info_t *info,
                     void                   *ctx_ptr)
{
    s3_get_fd_ctx_t *ctx = (s3_get_fd_ctx_t *)ctx_ptr;
    if (ctx->user_headers_cb != NULL) {
        ctx->user_headers_cb(req, info, ctx->user_data);
    }
}

/*
 * data_cb: пишем body-данные в fd.
 *
 * Возвращаем:
 *   0  — всё ок, продолжаем;
 *   !=0 — ошибка записи, запрос будет прерван.
 */
static int
s3_get_fd_data_cb(struct s3_request *req,
                  const uint8_t     *data,
                  size_t             len,
                  void              *ctx_ptr)
{
    (void)req;

    s3_get_fd_ctx_t *ctx = (s3_get_fd_ctx_t *)ctx_ptr;
    int fd = ctx->fd;

    if (fd < 0 || data == NULL || len == 0)
        return -1;

    /* Если pos < 0, первый кусок — берём текущее смещение ядра. */
    if (ctx->pos < 0) {
        off_t cur = lseek(fd, 0, SEEK_CUR);
        if (cur == (off_t)-1) {
            return -1;
        }
        ctx->pos = (int64_t)cur;
    }

    size_t  total_written = 0;
    int64_t pos           = ctx->pos;

    while (total_written < len) {
        ssize_t n = pwrite(fd, data + total_written,
                           len - total_written,
                           (off_t)pos);
        if (n > 0) {
            total_written += (size_t)n;
            pos           += n;
            continue;
        }
        if (n == 0) {
            /* Неожиданный нулевой записанный размер — считаем ошибкой. */
            return -1;
        }

        if (errno == EINTR)
            continue;

        /* Любая другая ошибка — прерываем запрос. */
        return -1;
    }

    ctx->pos = pos;
    return 0;
}

/*
 * done_cb: освобождаем контекст и зовём пользовательский done_cb.
 */
static void
s3_get_fd_done_cb(struct s3_request *req,
                  const s3_error_t  *error,
                  void              *ctx_ptr)
{
    s3_get_fd_ctx_t *ctx = (s3_get_fd_ctx_t *)ctx_ptr;

    s3_request_done_cb user_done = ctx->user_done_cb;
    void              *user_data = ctx->user_data;

    free(ctx);

    if (user_done != NULL) {
        user_done(req, error, user_data);
    }
}

/* --------------------------------------------------------
 * Публичное API: s3_object_get_to_fd()
 * ------------------------------------------------------ */

s3_request_t *
s3_object_get_to_fd(
    s3_client_t                     *client,
    const s3_object_get_params_t    *params,
    int                              fd,
    int64_t                          offset,
    s3_object_get_headers_cb         headers_cb,
    s3_request_done_cb               done_cb,
    void                            *user_data)
{
    /* Базовая валидация аргументов. */
    if (client == NULL || params == NULL ||
        params->bucket == NULL || params->key == NULL ||
        fd < 0) {

        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INVALID_ARGUMENT;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get_to_fd: invalid arguments");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    /* Выделяем внутренний контекст. */
    s3_get_fd_ctx_t *ctx = (s3_get_fd_ctx_t *)malloc(sizeof(*ctx));
    if (ctx == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get_to_fd: out of memory");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    ctx->fd              = fd;
    ctx->pos             = (offset >= 0) ? offset : -1; /* -1 => значит "продолжать с текущей позиции" */
    ctx->user_headers_cb = headers_cb;
    ctx->user_done_cb    = done_cb;
    ctx->user_data       = user_data;

    /*
     * Стартуем низкоуровневый асинхронный GET.
     *
     *  - headers_cb (если задан) заворачиваем в s3_get_fd_headers_cb;
     *  - data_cb всегда наш (s3_get_fd_data_cb);
     *  - done_cb — наша обёртка s3_get_fd_done_cb.
     */
    s3_request_t *req = s3_object_get(
        client,
        params,
        (headers_cb != NULL) ? s3_get_fd_headers_cb : NULL,
        s3_get_fd_data_cb,
        s3_get_fd_done_cb,
        ctx);

    if (req == NULL) {
        /* Не удалось стартовать запрос — освобождаем контекст и
         * синхронно уведомляем пользователя. */
        free(ctx);

        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get_to_fd: failed to start request");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    return req;
}
