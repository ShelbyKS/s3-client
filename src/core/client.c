/* src/core/client.c */

#include <s3/client.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "client_internal.h"
#include "curl_easy_s3.h"

/*
 * S3 CLIENT — реализация уровня управления запросами.
 *
 * Текущий статус реализации:
 *
 *  ✔ s3_client_t создаёт и инициализирует CURLM (libcurl multi handle)
 *  ✔ CURLM полностью интегрирован с внешним reactor'ом через socket/timer callbacks
 *  ✔ s3_object_get() / s3_object_put() создают реальные CURL easy-хендлы
 *  ✔ запросы регистрируются в CURLM и обрабатываются через curl_multi_socket_action()
 *  ✔ завершение запросов отслеживается через curl_multi_info_read()
 *  ✔ вызывается пользовательский done_cb()
 *  ✔ easy-handle и s3_request корректно очищаются после выполнения
 *
 * Что ещё не реализовано (следующие шаги):
 *
 *  ☐ передача заголовков через get_headers_cb
 *  ☐ передача тела ответа через get_data_cb
 *  ☐ чтение тела PUT через put_read_cb (CURLOPT_READFUNCTION)
 *  ☐ AWS Signature V4 (или иная авторизация)
 *
 * В остальном система запроса/реактор/curl-multi уже полностью рабочая.
 */

/* ============================================================
 *              Реализация API создания/уничтожения
 * ========================================================== */

s3_client_t *
s3_client_new(const s3_client_config_t *cfg)
{
    if (cfg == NULL)
        return NULL;

    /* Минимальные проверки конфигурации. */
    if (cfg->endpoint == NULL || cfg->region == NULL) {
        return NULL;
    }
    if (cfg->reactor.vtable == NULL) {
        /* Реактор обязателен в финальной реализации. */
        return NULL;
    }

    s3_client_t *client = (s3_client_t *)malloc(sizeof(s3_client_t));
    if (client == NULL)
        return NULL;

    /* Просто копируем конфиг целиком. */
    memcpy(&client->cfg, cfg, sizeof(s3_client_config_t));

    client->multi          = NULL;
    client->still_running  = 0;
	client->timer_handle   = NULL; 

    /* Инициализируем CURLM через отдельную internal-функцию. */
    if (s3_client_multi_init(client) != 0) {
        free(client);
        return NULL;
    }

    return client;
}

void
s3_client_destroy(s3_client_t *client)
{
    if (client == NULL)
        return;

    /* В реальной реализации:
     *  - убедиться, что все запросы завершены/отменены;
     *  - закрыть все easy-хендлы;
     *  - а затем уже чистить multi.
     */

    s3_client_multi_cleanup(client);
    free(client);
}

/* ============================================================
 *                    Управление запросом
 * ========================================================== */

void
s3_request_cancel(s3_request_t *req)
{
    if (req == NULL)
        return;

    /* В stub-реализации мы только помечаем флаг.
     * В дальнейшем здесь будет логика передачи отмены libcurl/реактору.
     */
    req->cancelled = 1;
}

/* ============================================================
 *           Низкоуровневый async API
 * ========================================================== */

s3_request_t *
s3_object_get(
    s3_client_t                     *client,
    const s3_object_get_params_t    *params,
    s3_object_get_headers_cb         headers_cb,
    s3_object_get_data_cb            data_cb,
    s3_request_done_cb               done_cb,
    void                            *user_data)
{
    if (client == NULL || params == NULL ||
        params->bucket == NULL || params->key == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INVALID_ARGUMENT;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get: invalid arguments");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    s3_request_t *req = (s3_request_t *)malloc(sizeof(s3_request_t));
    if (req == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get: out of memory for request");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    req->client        = client;
    req->easy          = NULL;
    req->done_cb       = done_cb;
    req->user_data     = user_data;
    req->cancelled     = 0;

    /* Запоминаем коллбеки GET. */
    req->get_headers_cb = headers_cb;
    req->get_data_cb    = data_cb;
    /* Для GET put_read_cb не используется. */
    req->put_read_cb    = NULL;

    /* Инициализация состояния заголовков. */
    req->obj_info.content_length = 0;
    req->obj_info.etag           = NULL;
    req->obj_info.content_type   = NULL;
    req->etag_buf[0]             = '\0';
    req->content_type_buf[0]     = '\0';
    req->headers_sent            = 0;

    CURL *easy = s3_curl_easy_create_get(&client->cfg, params, req);
    if (easy == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get: failed to create CURL easy handle");
            done_cb(req, &err, user_data);
        }
        free(req);
        return NULL;
    }

    req->easy = easy;

    curl_easy_setopt(easy, CURLOPT_PRIVATE, req);

    CURLMcode mrc = curl_multi_add_handle(client->multi, easy);
    if (mrc != CURLM_OK) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_get: curl_multi_add_handle failed: %d",
                           (int)mrc);
            done_cb(req, &err, user_data);
        }
        curl_easy_cleanup(easy);
        free(req);
        return NULL;
    }

    return req;
}

s3_request_t *
s3_object_put(
    s3_client_t                     *client,
    const s3_object_put_params_t    *params,
    s3_object_put_read_cb            read_cb,
    s3_request_done_cb               done_cb,
    void                            *user_data)
{
    if (client == NULL || params == NULL ||
        params->bucket == NULL || params->key == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INVALID_ARGUMENT;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put: invalid arguments");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    s3_request_t *req = (s3_request_t *)malloc(sizeof(s3_request_t));
    if (req == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put: out of memory for request");
            done_cb(NULL, &err, user_data);
        }
        return NULL;
    }

    req->client        = client;
    req->easy          = NULL;
    req->done_cb       = done_cb;
    req->user_data     = user_data;
    req->cancelled     = 0;

    /* Для PUT: используем read_cb, GET-коллбеки не задействованы. */
    req->get_headers_cb = NULL;
    req->get_data_cb    = NULL;
    req->put_read_cb    = read_cb;

    /* На всякий случай очищаем метаданные. */
    req->obj_info.content_length = 0;
    req->obj_info.etag           = NULL;
    req->obj_info.content_type   = NULL;
    req->etag_buf[0]             = '\0';
    req->content_type_buf[0]     = '\0';
    req->headers_sent            = 0;

    CURL *easy = s3_curl_easy_create_put(&client->cfg, params, req);
    if (easy == NULL) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put: failed to create CURL easy handle");
            done_cb(req, &err, user_data);
        }
        free(req);
        return NULL;
    }

    req->easy = easy;

    curl_easy_setopt(easy, CURLOPT_PRIVATE, req);

    CURLMcode mrc = curl_multi_add_handle(client->multi, easy);
    if (mrc != CURLM_OK) {
        if (done_cb != NULL) {
            s3_error_t err;
            s3_error_clear(&err);
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "s3_object_put: curl_multi_add_handle failed: %d",
                           (int)mrc);
            done_cb(req, &err, user_data);
        }
        curl_easy_cleanup(easy);
        free(req);
        return NULL;
    }

    return req;
}
