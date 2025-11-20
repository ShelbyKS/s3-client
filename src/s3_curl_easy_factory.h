#pragma once

#include <curl/curl.h>

#include "s3_http_backend.h"
#include "s3_internal.h"
#include "s3_error.h"

/*
 * Обёртка вокруг CURL easy handle и связанных ресурсов.
 * Используется и в easy-бэкенде, и в multi-бэкенде.
 */
struct s3_curl_easy_handle {
    CURL *easy;
    struct curl_slist *headers;
    s3_client_t *client;
};

/* Общий cleanup: освобождает curl_slist и curl_easy */
void
s3_curl_easy_cleanup(struct s3_curl_easy_handle *h);

/* ------ Фабрики под конкретные HTTP-методы ------ */

/* PUT: body из req->src_fd, длина = req->content_length (если >= 0) */
int
s3_curl_easy_setup_put(struct s3_curl_easy_handle *h,
                       s3_client_t *client,
                       struct s3_http_request *req,
                       struct s3_error_info *err);

/* GET: пишем body в req->dst_fd */
int
s3_curl_easy_setup_get(struct s3_curl_easy_handle *h,
                       s3_client_t *client,
                       struct s3_http_request *req,
                       struct s3_error_info *err);

/* Аналогично можно будет сделать: 
 *   s3_curl_easy_setup_delete(...)
 *   s3_curl_easy_setup_list(...)
 *   s3_curl_easy_setup_bucket_create(...)
 */
