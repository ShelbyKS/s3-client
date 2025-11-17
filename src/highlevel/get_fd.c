/* src/highlevel/get_fd.c */

#include <s3/client.h>
#include <string.h>
#include <stdio.h>

/*
 * get_fd.c
 *
 * В будущем:
 *   - реализует s3_object_get_to_fd() поверх s3_object_get():
 *       * внутренний data_cb, который пишет в fd (через write/coio и т.п.);
 *       * обработка частичных записей, ошибок I/O и т.д.
 *
 * Сейчас — заглушка:
 *   - проверяет аргументы;
 *   - вызывает done_cb с ошибкой;
 *   - возвращает NULL.
 */

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
    (void)offset;
    (void)headers_cb;

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

    /* TODO: реальная реализация поверх s3_object_get() с data_cb,
     *       который пишет в fd.
     */

    if (done_cb != NULL) {
        s3_error_t err;
        s3_error_clear(&err);
        err.code = S3_E_INTERNAL;
        (void)snprintf(err.message, sizeof(err.message),
                       "s3_object_get_to_fd: not implemented (stub)");
        done_cb(NULL, &err, user_data);
    }

    return NULL;
}
