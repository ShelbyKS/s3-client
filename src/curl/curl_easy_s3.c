/* src/curl/curl_easy_s3.c */

#include <s3/client.h>
#include <s3/types.h>

#include <curl/curl.h>
#include <curl/curlver.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "client_internal.h"
#include "curl_easy_s3.h"

/*
 * curl_easy_s3.c
 *
 * На текущем этапе:
 *   - создаём и настраиваем CURL easy-хендлы для S3 GET/PUT:
 *       * собираем URL из endpoint + bucket + key;
 *       * для GET вешаем WRITEFUNCTION → s3_request_t::get_data_cb;
 *   - не реализуем SigV4;
 */

/* ------------------------------------------------------------
 * Shim для передачи данных ответа в get_data_cb
 * ------------------------------------------------------------ */

static size_t
s3_curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    s3_request_t *req = (s3_request_t *)userdata;
    size_t total = size * nmemb;

    if (req == NULL || req->get_data_cb == NULL) {
        /* Пользователь не подписался на данные — просто проглатываем. */
        return total;
    }

    int rc = req->get_data_cb(
        req,
        (const uint8_t *)ptr,
        total,
        req->user_data
    );

    if (rc != 0) {
        /* Любой ненулевой код — прерываем передачу (libcurl оборвёт запрос). */
        return 0;
    }

    return total;
}

static size_t
s3_curl_read_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    s3_request_t *req = (s3_request_t *)userdata;
    size_t capacity = size * nitems;

    if (req == NULL || req->put_read_cb == NULL || capacity == 0) {
        /* Нет данных или нет коллбэка — сообщаем EOF. */
        return 0;
    }

    size_t out_len = 0;
    int eof = 0;

    int rc = req->put_read_cb(
        req,
        (uint8_t *)buffer,
        capacity,
        &out_len,
        &eof,
        req->user_data
    );

    if (rc != 0) {
        /* Пользовательская ошибка → прервать запрос. */
        return CURL_READFUNC_ABORT;
    }

    /* Защита от “лишнего энтузиазма” коллбэка. */
    if (out_len > capacity) {
        out_len = capacity;
    }

    /* EOF + 0 байт → curl воспринимает как конец тела. */
    if (eof && out_len == 0) {
        return 0;
    }

    /* Можно вернуть last-chunk + eof=1: curl прочитает эти данные,
       а на следующем вызове read_cb мы вернём 0, что завершит upload. */
    return out_len;
}

/* ------------------------------------------------------------
 * Shim для заголовков: HTTP header line → s3_object_info_t + get_headers_cb
 * ------------------------------------------------------------ */

static void
s3_trim_header_value(char *start, size_t *len_out)
{
    char *p = start;
    char *end = start + *len_out;

    /* Уберём \r\n в конце. */
    while (end > p && (end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }

    /* Пропускаем ведущие пробелы. */
    while (p < end && (*p == ' ' || *p == '\t')) {
        ++p;
    }

    /* Уберём хвостовые пробелы. */
    char *q = end;
    while (q > p && (q[-1] == ' ' || q[-1] == '\t')) {
        --q;
    }

    /* Сдвигаем строку к началу буфера. */
    size_t new_len = (size_t)(q - p);
    if (p != start && new_len > 0) {
        memmove(start, p, new_len);
    }

    start[new_len] = '\0';
    *len_out = new_len;
}

static int
s3_header_name_eq(const char *line, size_t len, const char *name)
{
    size_t name_len = strlen(name);
    if (len < name_len + 1) /* минимум "Name:" */
        return 0;

    /* case-insensitive сравнение имени до ':' */
    for (size_t i = 0; i < name_len; ++i) {
        char c1 = line[i];
        char c2 = name[i];
        if ('A' <= c1 && c1 <= 'Z') c1 = (char)(c1 - 'A' + 'a');
        if ('A' <= c2 && c2 <= 'Z') c2 = (char)(c2 - 'A' + 'a');
        if (c1 != c2)
            return 0;
    }

    return (line[name_len] == ':');
}

static size_t
s3_curl_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    s3_request_t *req = (s3_request_t *)userdata;
    size_t total = size * nitems;

    if (req == NULL || req->get_headers_cb == NULL) {
        /* Пользователь не интересуется заголовками. */
        return total;
    }

    /* Пустая строка "\r\n" → конец заголовков. */
    if (total >= 2 && buffer[0] == '\r' && buffer[1] == '\n') {
        if (!req->headers_sent) {
            /* Финализируем указатели строк. */
            req->obj_info.etag = (req->etag_buf[0] != '\0') ? req->etag_buf : NULL;
            req->obj_info.content_type =
                (req->content_type_buf[0] != '\0') ? req->content_type_buf : NULL;

            req->get_headers_cb(req, &req->obj_info, req->user_data);
            req->headers_sent = 1;
        }
        return total;
    }

    /* Статусная строка "HTTP/1.1 200 OK" и подобные:
     * Сейчас мы её никак не используем, но на ней удобно сбросить состояние. */
    if (total >= 5 && strncmp(buffer, "HTTP/", 5) == 0) {
        /* Обнулим накопленную информацию. */
        req->obj_info.content_length = 0;
        req->etag_buf[0]             = '\0';
        req->content_type_buf[0]     = '\0';
        req->obj_info.etag           = NULL;
        req->obj_info.content_type   = NULL;
        req->headers_sent            = 0;
        return total;
    }

    /* Парсим обычную строку заголовка вида "Name: value\r\n". */
    const char *line = buffer;
    size_t len = total;

    /* Content-Length */
    if (s3_header_name_eq(line, len, "Content-Length")) {
        const char *p = memchr(line, ':', len);
        if (p != NULL && (size_t)(p - line) + 1 < len) {
            char tmp[64];
            size_t vlen = len - (size_t)(p - line) - 1;
            if (vlen >= sizeof(tmp))
                vlen = sizeof(tmp) - 1;
            memcpy(tmp, p + 1, vlen);
            tmp[vlen] = '\0';

            /* trim spaces/CRLF */
            size_t tlen = strlen(tmp);
            s3_trim_header_value(tmp, &tlen);

            if (tlen > 0) {
                /* простое strtoull, без излишних проверок */
                req->obj_info.content_length = (uint64_t)strtoull(tmp, NULL, 10);
            }
        }
        return total;
    }

    /* ETag */
    if (s3_header_name_eq(line, len, "ETag")) {
        const char *p = memchr(line, ':', len);
        if (p != NULL && (size_t)(p - line) + 1 < len) {
            size_t vlen = len - (size_t)(p - line) - 1;
            if (vlen >= sizeof(req->etag_buf))
                vlen = sizeof(req->etag_buf) - 1;
            memcpy(req->etag_buf, p + 1, vlen);
            req->etag_buf[vlen] = '\0';

            size_t tlen = vlen;
            s3_trim_header_value(req->etag_buf, &tlen);
        }
        return total;
    }

    /* Content-Type */
    if (s3_header_name_eq(line, len, "Content-Type")) {
        const char *p = memchr(line, ':', len);
        if (p != NULL && (size_t)(p - line) + 1 < len) {
            size_t vlen = len - (size_t)(p - line) - 1;
            if (vlen >= sizeof(req->content_type_buf))
                vlen = sizeof(req->content_type_buf) - 1;
            memcpy(req->content_type_buf, p + 1, vlen);
            req->content_type_buf[vlen] = '\0';

            size_t tlen = vlen;
            s3_trim_header_value(req->content_type_buf, &tlen);
        }
        return total;
    }

    return total;
}

/* ------------------------------------------------------------
 * Вспомогательный: сборка URL "scheme://endpoint/bucket/key"
 * ------------------------------------------------------------ */

static char *
s3_build_object_url(const s3_client_config_t *cfg,
                    const char               *bucket,
                    const char               *key)
{
    const char *scheme = cfg->endpoint_is_https ? "https" : "http";

    /* Простейшая сборка без URL-encoding: scheme://endpoint/bucket/key */
    size_t len = 0;
    len += strlen(scheme);
    len += 3; /* "://" */
    len += strlen(cfg->endpoint);
    len += 1; /* '/' перед bucket */
    len += strlen(bucket);
    len += 1; /* '/' перед key */
    len += strlen(key);
    len += 1; /* '\0' */

    char *url = (char *)malloc(len);
    if (url == NULL)
        return NULL;

    int n = snprintf(url, len, "%s://%s/%s/%s", scheme, cfg->endpoint, bucket, key);
    if (n < 0 || (size_t)n >= len) {
        free(url);
        return NULL;
    }

    return url;
}

// TODO: SSL, timeout'ы, verbose

/* ------------------------------------------------------------
 *  Включение AWS SigV4
 * ------------------------------------------------------------ */

static int
s3_curl_apply_sigv4(CURL *easy, const s3_client_config_t *cfg)
{
    if (!cfg->use_aws_sigv4)
        return 0;

#if !defined(CURLAUTH_AWS_SIGV4)
    // TODO: S3_E_UNSUPPORTED
    return -1;
#else
    /* Проверяем наличие ключей */
    if (!cfg->credentials.access_key_id || !cfg->credentials.secret_access_key) {
        return -1;
    }

    /* Формируем aws:amz:REGION:SERVICE */
    char sigv4_param[128];
    const char *region  = cfg->region  ? cfg->region  : "";
    const char *service = cfg->service ? cfg->service : "s3";

    if (region[0] != '\0')
        snprintf(sigv4_param, sizeof(sigv4_param), "aws:amz:%s:%s", region, service);
    else
        snprintf(sigv4_param, sizeof(sigv4_param), "aws:amz");

    curl_easy_setopt(easy, CURLOPT_AWS_SIGV4, sigv4_param);

    char userpwd[512];
    snprintf(userpwd, sizeof(userpwd), "%s:%s", cfg->credentials.access_key_id, cfg->credentials.secret_access_key);
    curl_easy_setopt(easy, CURLOPT_USERPWD, userpwd);

    return 0;
#endif
}


/* ------------------------------------------------------------
 * Фабрики CURL easy-хендлов для GET и PUT
 * ------------------------------------------------------------ */

CURL *
s3_curl_easy_create_get(const s3_client_config_t      *cfg,
                        const s3_object_get_params_t  *params,
                        s3_request_t                  *req)
{
    if (cfg == NULL || params == NULL ||
        params->bucket == NULL || params->key == NULL) {
        return NULL;
    }

    CURL *easy = curl_easy_init();
    if (easy == NULL)
        return NULL;

    /* Подключаем SigV4, если включено в конфиге. */
    s3_curl_apply_sigv4(easy, cfg);

    char *url = s3_build_object_url(cfg, params->bucket, params->key);
    if (url == NULL) {
        curl_easy_cleanup(easy);
        return NULL;
    }

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);

    /* Прокидываем body в get_data_cb. */
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, s3_curl_write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, req);

    /* Прокидываем заголовки в get_headers_cb. */
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, s3_curl_header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, req);

    /* TODO:
     *  - таймауты из cfg;
     *
     * ВОПРОС памяти:
     *  - CURLOPT_URL копирует строку в libcurl,
     *    поэтому можно free(url) сразу после curl_easy_setopt, но
     *    чтобы не усложнять на этом этапе, оставим так.
     *    Когда начнём профилировать и чистить утечки — вернёмся сюда.
     */

    return easy;
}

CURL *
s3_curl_easy_create_put(const s3_client_config_t      *cfg,
                        const s3_object_put_params_t  *params,
                        s3_request_t                  *req)
{
    if (cfg == NULL || params == NULL ||
        params->bucket == NULL || params->key == NULL) {
        return NULL;
    }

    CURL *easy = curl_easy_init();
    if (easy == NULL)
        return NULL;

    /* Подключаем SigV4, если включено в конфиге. */
    s3_curl_apply_sigv4(easy, cfg);

    char *url = s3_build_object_url(cfg, params->bucket, params->key);
    if (url == NULL) {
        curl_easy_cleanup(easy);
        return NULL;
    }

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);

    /* Если знаем размер — сообщим его curl'у. */
    if (params->content_length != (uint64_t)-1) {
        curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE,
                         (curl_off_t)params->content_length);
    }
    /* Если == (uint64_t)-1, можно оставить без размера — HTTP будет через chunked,
       это мы потом при желании донастроим заголовками. */

    /* Прокидываем чтение тела через s3_request::put_read_cb. */
    curl_easy_setopt(easy, CURLOPT_READFUNCTION, s3_curl_read_cb);
    curl_easy_setopt(easy, CURLOPT_READDATA, req);

    return easy;
}

