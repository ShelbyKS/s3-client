/* src/highlevel/put_fd.c
 *
 * High-level helper: PUT ← файловый дескриптор.
 *
 * Обёртка над s3_object_put(), которая:
 *   - читает данные из fd;
 *   - по смещению offset (если задан) и с ограничением limit (если задан);
 *   - стримит их в S3 через s3_object_put_read_cb.
 *
 * ВАЖНО:
 *   - Ожидается, что fd — блокирующий (обычный файловый дескриптор).
 *   - Параллельное использование fd из других потоков/код-path-ов
 *     не рекомендуется, но мы стараемся минимизировать побочные эффекты,
 *     используя pread, когда задан offset.
 */

#include <s3/client.h>
#include <s3/types.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h> /* read, pread, lseek */
#include <errno.h>

/* --------------------------------------------------------
 * Внутренний контекст для PUT ← fd
 * ------------------------------------------------------ */

typedef struct s3_put_fd_ctx {
    int                  fd;
    int64_t              pos;         /* текущее смещение для pread; <0 => используем read() */
    uint64_t             to_send;     /* сколько максимум байт нужно отправить; (uint64_t)-1 => "до EOF" */
    uint64_t             sent;        /* сколько уже отправлено */
    int                  eof_seen;    /* встретили EOF при чтении из fd */

    s3_request_done_cb   user_done_cb;
    void                *user_data;
} s3_put_fd_ctx_t;

/* --------------------------------------------------------
 * read_cb: читаем данные из fd и отдаём curl'у
 * ------------------------------------------------------ */

static int
s3_put_fd_read_cb(struct s3_request *req,
                  uint8_t           *buf,
                  size_t             len,
                  size_t            *out_len,
                  int               *eof,
                  void              *ctx_ptr)
{
    (void)req;

    s3_put_fd_ctx_t *ctx = (s3_put_fd_ctx_t *)ctx_ptr;
    int fd = ctx->fd;

    *out_len = 0;
    *eof = 0;

    if (fd < 0 || buf == NULL || len == 0)
        return -1;

    /* Если уже всё отправили (по лимиту) или видели EOF — больше данных нет. */
    if ((ctx->to_send != (uint64_t)-1 && ctx->sent >= ctx->to_send) || ctx->eof_seen) {
        *out_len = 0;
        *eof = 1;
        return 0;
    }

    size_t max_chunk = len;

    /* Не выходим за пределы лимита, если он задан. */
    if (ctx->to_send != (uint64_t)-1) {
        uint64_t remaining = ctx->to_send - ctx->sent;
        if (remaining < (uint64_t)max_chunk)
            max_chunk = (size_t)remaining;
    }

    if (max_chunk == 0) {
        *out_len = 0;
        *eof = 1;
        return 0;
    }

    size_t total_read = 0;

    while (total_read < max_chunk) {
        ssize_t n;

        if (ctx->pos >= 0) {
            /* Читаем из фиксированного смещения, не трогая file offset ядра. */
            n = pread(fd,
                      buf + total_read,
                      max_chunk - total_read,
                      (off_t)(ctx->pos));
        } else {
            /* Используем текущее смещение fd (если offset < 0). */
            n = read(fd,
                     buf + total_read,
                     max_chunk - total_read);
        }

        if (n > 0) {
            total_read += (size_t)n;
            ctx->sent  += (uint64_t)n;
            if (ctx->pos >= 0)
                ctx->pos += n;
            continue;
        }

        if (n == 0) {
            /* EOF. */
            ctx->eof_seen = 1;
            break;
        }

        /* n < 0: ошибка. EINTR — повторяем, остальное — ошибка. */
        if (errno == EINTR)
            continue;

        /* Любая другая ошибка чтения — прерываем запрос. */
        *out_len = 0;
        *eof = 1;
        return -1;
    }

    *out_len = total_read;

    if (ctx->eof_seen ||
        (ctx->to_send != (uint64_t)-1 && ctx->sent >= ctx->to_send)) {
        *eof = 1;
    } else {
        *eof = 0;
    }

    return 0;
}

/* --------------------------------------------------------
 * done_cb: освобождаем контекст и вызываем пользовательский
 * ------------------------------------------------------ */

static void
s3_put_fd_done_cb(struct s3_request *req,
                  const s3_error_t  *error,
                  void              *ctx_ptr)
{
    s3_put_fd_ctx_t *ctx = (s3_put_fd_ctx_t *)ctx_ptr;

    s3_request_done_cb user_done = ctx->user_done_cb;
    void              *user_data = ctx->user_data;

    free(ctx);

    if (user_done != NULL) {
        user_done(req, error, user_data);
    }
}

/* --------------------------------------------------------
 * Публичное API: s3_object_put_from_fd()
 * ------------------------------------------------------ */

s3_request_t *
s3_object_put_from_fd(
    s3_client_t                  *client,
    const s3_object_put_params_t *params,
    int                           fd,
    int64_t                       offset,
    uint64_t                      limit,
    s3_request_done_cb            done_cb,
    void                         *user_data)
{
    /* Базовая проверка аргументов. */
    if (client == NULL || params == NULL ||
        params->bucket == NULL || params->key == NULL ||
        fd < 0) {

        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INVALID_ARGUMENT;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put_from_fd: invalid arguments");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    /* Определяем, сколько всего байт собираемся отправить. */
    uint64_t to_send = (uint64_t)-1;

    if (limit != (uint64_t)-1) {
        /* Если задан явный limit — он приоритетнее. */
        to_send = limit;
    } else if (params->content_length != (uint64_t)-1) {
        /* Иначе, если пользователь указал content_length, используем его. */
        to_send = params->content_length;
    } else {
        /* Иначе to_send остаётся (uint64_t)-1 => "до EOF", chunked, если бекенд позволит. */
        to_send = (uint64_t)-1;
    }

    /* Готовим локальную копию params, чтобы не трогать структуру пользователя.
     * Если to_send != (uint64_t)-1, выставляем content_length.
     */
    s3_object_put_params_t params_local = *params;
    if (to_send != (uint64_t)-1) {
        params_local.content_length = to_send;
    } else {
        /* Если to_send "до EOF" и пользователь не указал content_length,
         * оставляем params_local.content_length как есть. */
    }

    /* Инициализируем контекст. */
    s3_put_fd_ctx_t *ctx = (s3_put_fd_ctx_t *)malloc(sizeof(*ctx));
    if (ctx == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put_from_fd: out of memory");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    ctx->fd         = fd;
    ctx->sent       = 0;
    ctx->eof_seen   = 0;
    ctx->to_send    = to_send;
    ctx->user_done_cb = done_cb;
    ctx->user_data    = user_data;

    /*
     * offset интерпретируем так:
     *   - offset >= 0: читаем из этой позиции, используя pread.
     *   - offset <  0: читаем с текущей позиции fd, используя read().
     */
    if (offset >= 0) {
        ctx->pos = offset;
    } else {
        ctx->pos = -1; /* режим read() с текущего смещения */
    }

    /*
     * Стартуем низкоуровневый асинхронный PUT.
     *
     *  - read_cb — наш s3_put_fd_read_cb;
     *  - done_cb — наша обёртка s3_put_fd_done_cb;
     *  - headers_cb для PUT не нужен.
     */
    s3_request_t *req = s3_object_put(
        client,
        &params_local,
        s3_put_fd_read_cb,
        s3_put_fd_done_cb,
        ctx);

    if (req == NULL) {
        free(ctx);

        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put_from_fd: failed to start request");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    return req;
}
