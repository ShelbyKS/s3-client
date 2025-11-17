/* src/highlevel/put_fd.c */

#include <s3/client.h>
#include <string.h>
#include <stdio.h>

/*
 * put_fd.c
 *
 * В будущем:
 *   - реализует s3_object_put_from_fd() поверх s3_object_put():
 *       * внутренний read_cb, который читает из fd (read/coio);
 *       * учитывает offset и limit;
 *       * корректно обрабатывает EOF и ошибки I/O.
 *
 * Сейчас — заглушка:
 *   - проверяет аргументы;
 *   - вызывает done_cb с ошибкой;
 *   - возвращает NULL.
 */

s3_request_t *
s3_object_put_from_fd(
    s3_client_t                     *client,
    const s3_object_put_params_t    *params,
    int                              fd,
    int64_t                          offset,
    uint64_t                         limit,
    s3_request_done_cb               done_cb,
    void                            *user_data)
{
    (void)offset;
    (void)limit;

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

    /* TODO: реальная реализация поверх s3_object_put() с read_cb,
     *       который читает из fd, начиная с offset, максимум limit байт.
     */

    if (done_cb != NULL) {
        s3_error_t err;
        s3_error_clear(&err);
        err.code = S3_E_INTERNAL;
        (void)snprintf(err.message, sizeof(err.message),
                       "s3_object_put_from_fd: not implemented (stub)");
        done_cb(NULL, &err, user_data);
    }

    return NULL;
}
