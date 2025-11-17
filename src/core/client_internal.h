/* src/core/client_internal.h */

#ifndef S3_CLIENT_INTERNAL_H
#define S3_CLIENT_INTERNAL_H

#include <curl/curl.h>

#include <s3/client.h>
#include <s3/reactor.h>

/*
 * Внутреннее определение структуры s3_client.
 *
 * Снаружи пользователь видит только opaque-тип s3_client_t,
 * а здесь — реальная структура для core/curl слоёв.
 */
struct s3_client {
    s3_client_config_t       cfg;           /* конфигурация клиента */
    CURLM                   *multi;         /* libcurl multi handle */
    int                      still_running; /* сколько запросов в работе */

    s3_reactor_timer_handle_t timer_handle; /* таймер CURLM через reactor */
};

/*
 * Внутреннее определение структуры s3_request.
 *
 * Здесь мы храним:
 *   - ссылку на клиента;
 *   - CURL easy handle;
 *   - callbacks и user_data;
 *   - флаг отмены.
 *
 * Для простоты GET и PUT используют одну и ту же структуру:
 *   - для GET заполняем get_headers_cb / get_data_cb;
 *   - для PUT заполняем put_read_cb;
 *   - остальные остаются NULL.
 */
struct s3_request {
    s3_client_t        *client;
    CURL               *easy;

    /* Коллбек завершения и контекст пользователя. */
    s3_request_done_cb  done_cb;
    void               *user_data;

    /* Коллбеки для GET. */
    s3_object_get_headers_cb get_headers_cb;
    s3_object_get_data_cb    get_data_cb;

    /* Коллбек для PUT. */
    s3_object_put_read_cb    put_read_cb;

    /* Состояние GET-заголовков. */
    s3_object_info_t    obj_info;                 /* метаданные */
    char                etag_buf[128];            /* локальный буфер под ETag */
    char                content_type_buf[128];    /* локальный буфер под Content-Type */
    int                 headers_sent;             /* 0 — ещё не вызывали get_headers_cb */


    int                 cancelled;
};

/*
 * Внутренние функции инициализации/чистки CURLM.
 */
int  s3_client_multi_init(s3_client_t *client);
void s3_client_multi_cleanup(s3_client_t *client);

#endif /* S3_CLIENT_INTERNAL_H */
