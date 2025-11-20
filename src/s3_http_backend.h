#ifndef S3_HTTP_BACKEND_H_INCLUDED
#define S3_HTTP_BACKEND_H_INCLUDED

/*
 * s3_http_backend.h
 *
 * Внутренний интерфейс HTTP-бэкенда.
 *
 * Он предоставляет минимальные абстракции:
 *   - подготовка HTTP-запроса (method, URL, headers);
 *   - указание источника данных (src_fd) или приёмника (dst_fd);
 *   - выполнение запроса (блокирующее);
 *   - получение HTTP-статуса и сообщения об ошибке.
 *
 * Реализации:
 *   - s3_curl_easy.c (синхронный backend на curl_easy_perform)
 *   - потом появится s3_curl_multi.c
 */

#include "s3_internal.h"  /* s3_env, s3_client */
#include "s3_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP-метод */
enum s3_http_method {
    S3_HTTP_GET,
    S3_HTTP_PUT,
    S3_HTTP_HEAD,
    S3_HTTP_POST,
    S3_HTTP_DELETE,
};

/*
 * Описание HTTP-запроса, которое S3-слой передаёт в backend.
 * Эти поля будут заполняться внутри s3_client_* функций.
 */
struct s3_http_request {
    enum s3_http_method method;
    const char *url;

    const char * const *headers;
    size_t header_count;

    int src_fd;  /* для PUT: читать тело из fd */
    int dst_fd;  /* для GET: писать тело в fd */

    /* Будет заполняться бэкендом */
    long http_status;

	/*
     * Длина тела запроса (для PUT, POST).
     * -1  => длина неизвестна (curl может решить сам, но S3 обычно не любит).
     * >=0 => используем как значение для CURLOPT_INFILESIZE_LARGE.
     */
    long long content_length;
};

/*
 * API backend'а.
 * Бэкенд создаётся для каждого клиента (или может быть один на весь env).
 */

struct s3_http_backend {
    /* Выполнить HTTP-запрос. Блокирующе. */
    int (*perform)(struct s3_http_backend *backend,
                   struct s3_http_request *req,
                   struct s3_error_info *err);

    /* Освободить backend */
    void (*destroy)(struct s3_http_backend *backend);
};

/* Создать backend на curl_easy */
struct s3_http_backend *
s3_curl_easy_backend_create(s3_client_t *client, struct s3_error_info *err);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S3_HTTP_BACKEND_H_INCLUDED */
