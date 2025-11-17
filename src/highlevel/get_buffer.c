/* src/highlevel/get_buffer.c */

#include <s3/client.h>
#include <string.h>
#include <stdio.h>

/*
 * get_buffer.c
 *
 * В будущем:
 *   - реализует s3_object_get_to_buffer() поверх s3_object_get():
 *       * внутренний data_cb, который пишет в s3_buffer_t;
 *       * проверка на переполнение buf->size;
 *       * аккуратная обработка ошибок.
 *
 * Сейчас — заглушка:
 *   - проверяет аргументы;
 *   - вызывает done_cb с ошибкой;
 *   - возвращает NULL (запрос не стартует).
 */

s3_request_t *
s3_object_get_to_buffer(
    s3_client_t                     *client,
    const s3_object_get_params_t    *params,
    s3_buffer_t                     *buf,
    s3_object_get_headers_cb         headers_cb,
    s3_request_done_cb               done_cb,
    void                            *user_data)
{
    (void)headers_cb;
    (void)buf;

    if (client == NULL || params == NULL ||
        params->bucket == NULL || params->key == NULL || buf == NULL) {

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

    /* TODO: реальная реализация поверх s3_object_get(). */

    if (done_cb != NULL) {
        s3_error_t err;
        s3_error_clear(&err);
        err.code = S3_E_INTERNAL;
        (void)snprintf(err.message, sizeof(err.message),
                       "s3_object_get_to_buffer: not implemented (stub)");
        done_cb(NULL, &err, user_data);
    }

    return NULL;
}
