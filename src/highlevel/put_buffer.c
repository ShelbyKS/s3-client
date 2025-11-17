/* src/highlevel/put_buffer.c */

#include <s3/client.h>
#include <string.h>
#include <stdio.h>

/*
 * put_buffer.c
 *
 * В будущем:
 *   - реализует s3_object_put_from_buffer() поверх s3_object_put():
 *       * внутренний read_cb, который читает из s3_buffer_t;
 *       * корреляция buf->used и params->content_length.
 *
 * Сейчас — заглушка:
 *   - проверяет аргументы;
 *   - вызывает done_cb с ошибкой;
 *   - возвращает NULL.
 */

s3_request_t *
s3_object_put_from_buffer(
    s3_client_t                     *client,
    const s3_object_put_params_t    *params,
    const s3_buffer_t               *buf,
    s3_request_done_cb               done_cb,
    void                            *user_data)
{
    if (client == NULL || params == NULL ||
        params->bucket == NULL || params->key == NULL ||
        buf == NULL || buf->data == NULL) {

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

    /* TODO: реальная реализация поверх s3_object_put() с read_cb,
     *       который читает из buf->data[0..buf->used).
     */

    if (done_cb != NULL) {
        s3_error_t err;
        s3_error_clear(&err);
        err.code = S3_E_INTERNAL;
        (void)snprintf(err.message, sizeof(err.message),
                       "s3_object_put_from_buffer: not implemented (stub)");
        done_cb(NULL, &err, user_data);
    }

    return NULL;
}
