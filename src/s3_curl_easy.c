/*
 * s3_curl_easy.c
 *
 * Реализация HTTP backend на основе curl_easy_perform().
 */

#include "s3_http_backend.h"
#include "s3_error.h"
#include "s3_internal.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

/* ----------------------------------------------------------------------
 * Внутренний helper для установки ошибки
 * ---------------------------------------------------------------------- */

static int
s3_http_set_error(struct s3_error_info *err, int code, const char *msg)
{
    if (err != NULL) {
        s3_error_reset(err);
        err->code = code;
        err->http_status = 0;
        if (msg != NULL) {
            size_t n = strlen(msg);
            if (n >= sizeof(err->msg))
                n = sizeof(err->msg) - 1;
            memcpy(err->msg, msg, n);
            err->msg[n] = '\0';
        }
    }
    return code;
}

/* ----------------------------------------------------------------------
 * Вспомогательные callback'и: чтение/запись через fd
 * ---------------------------------------------------------------------- */

/* CURL получает тело для PUT через этот callback */
static size_t
read_from_fd_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    int fd = (int)(intptr_t)userdata;
    ssize_t r = read(fd, buffer, size * nitems);
    if (r < 0)
        return CURL_READFUNC_ABORT;
    return (size_t)r;
}

/* CURL отправляет тело GET в этот callback */
static size_t
write_to_fd_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    int fd = (int)(intptr_t)userdata;
    ssize_t w = write(fd, ptr, size * nmemb);
    if (w < 0)
        return 0;
    return (size_t)w;
}

/* ----------------------------------------------------------------------
 * Реализация backend
 * ---------------------------------------------------------------------- */

struct curl_easy_backend {
    struct s3_http_backend iface;  /* публичные методы */
    s3_client_t *client;
};

/* Основной метод: выполнить HTTP-запрос */
static int
curl_easy_backend_perform(struct s3_http_backend *backend,
                          struct s3_http_request *req,
                          struct s3_error_info *err)
{
    struct curl_easy_backend *b = (struct curl_easy_backend *)backend;
    s3_client_t *client = b->client;
    s3_env_t *env = client->env; /* пока не нужен, но пригодится для timeout/HTTPS */
    (void)env;

    int rc_code = S3_OK;

    CURL *h = curl_easy_init();
    if (!h) {
        return s3_http_set_error(err, S3_ECURL, "curl_easy_init failed");
    }

    struct curl_slist *header_list = NULL;

    /* URL */
    CURLcode rc = curl_easy_setopt(h, CURLOPT_URL, req->url);
    if (rc != CURLE_OK) {
        s3_http_set_error(err, S3_ECURL, "failed to set URL");
        rc_code = S3_ECURL;
        goto cleanup;
    }

    /* Метод */
    switch (req->method) {
    case S3_HTTP_GET:
        rc = curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
        break;
    case S3_HTTP_PUT:
        rc = curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
        break;
    case S3_HTTP_DELETE:
        rc = curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
        break;
    case S3_HTTP_HEAD:
        rc = curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
        break;
    case S3_HTTP_POST:
        rc = curl_easy_setopt(h, CURLOPT_POST, 1L);
        break;
    default:
        rc = CURLE_UNSUPPORTED_PROTOCOL;
        break;
    }
    if (rc != CURLE_OK) {
        s3_http_set_error(err, S3_ECURL, "failed to set HTTP method");
        rc_code = S3_ECURL;
        goto cleanup;
    }

    /* Заголовки */
    for (size_t i = 0; i < req->header_count; i++) {
        header_list = curl_slist_append(header_list, req->headers[i]);
    }
    if (header_list != NULL) {
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, header_list);
    }

    /* Тело (PUT) */
    if (req->method == S3_HTTP_PUT && req->src_fd >= 0) {
        curl_easy_setopt(h, CURLOPT_READFUNCTION, read_from_fd_callback);
        curl_easy_setopt(h, CURLOPT_READDATA, (void*)(intptr_t)req->src_fd);
    }

    /* Тело (GET) */
    if (req->method == S3_HTTP_GET && req->dst_fd >= 0) {
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_to_fd_callback);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, (void*)(intptr_t)req->dst_fd);
    }

        /* ----------------------------------------------------------
     * AWS SigV4 через CURLOPT_AWS_SIGV4 (если доступно в libcurl)
     * ---------------------------------------------------------- */
#ifdef CURLAUTH_AWS_SIGV4
    if (client->config.access_key != NULL &&
        client->config.secret_key != NULL &&
        client->config.region != NULL &&
        client->config.region[0] != '\0') {

        /* Формируем строку "aws:amz:<region>:s3" */
        char sigv4_buf[256];
        int n = snprintf(sigv4_buf, sizeof(sigv4_buf),
                         "aws:amz:%s:s3", client->config.region);
        if (n < 0 || (size_t)n >= sizeof(sigv4_buf)) {
            s3_http_set_error(err, S3_EINVAL,
                              "SigV4 string too long");
            rc_code = S3_EINVAL;
            goto cleanup;
        }

        rc = curl_easy_setopt(h, CURLOPT_AWS_SIGV4, sigv4_buf);
        if (rc != CURLE_OK) {
            s3_http_set_error(err, S3_ECURL,
                              "failed to set CURLOPT_AWS_SIGV4");
            rc_code = S3_ECURL;
            goto cleanup;
        }

        /* Креды: access_key / secret_key */
        rc = curl_easy_setopt(h, CURLOPT_USERNAME, client->config.access_key);
        if (rc != CURLE_OK) {
            s3_http_set_error(err, S3_ECURL,
                              "failed to set access_key (CURLOPT_USERNAME)");
            rc_code = S3_ECURL;
            goto cleanup;
        }

        rc = curl_easy_setopt(h, CURLOPT_PASSWORD, client->config.secret_key);
        if (rc != CURLE_OK) {
            s3_http_set_error(err, S3_ECURL,
                              "failed to set secret_key (CURLOPT_PASSWORD)");
            rc_code = S3_ECURL;
            goto cleanup;
        }
    }
#else
    (void)client; /* чтобы не ругался компилятор, если нет SigV4 */
#endif

    /* Выполнить */
    rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        char buf[256];
        snprintf(buf, sizeof(buf), "curl_easy_perform failed: %s",
                 curl_easy_strerror(rc));
        s3_http_set_error(err, S3_ECURL, buf);
        rc_code = S3_ECURL;
        goto cleanup;
    }

    /* HTTP-статус */
    {
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        req->http_status = status;
    }

    if (err != NULL)
        s3_error_reset(err);
    rc_code = S3_OK;

cleanup:
    curl_slist_free_all(header_list);
    curl_easy_cleanup(h);
    return rc_code;
}

/* Освободить backend */
static void
curl_easy_backend_destroy(struct s3_http_backend *backend)
{
    struct curl_easy_backend *b = (struct curl_easy_backend *)backend;
    s3_env_t *env = b->client->env;
    env->alloc.free_fn(env->alloc.ud, b);
}

struct s3_http_backend *
s3_curl_easy_backend_create(s3_client_t *client, struct s3_error_info *err)
{
    s3_env_t *env = client->env;

    struct curl_easy_backend *b =
        (struct curl_easy_backend *)env->alloc.malloc_fn(env->alloc.ud,
                                                         sizeof(*b));
    if (!b) {
        s3_http_set_error(err, S3_ENOMEM,
                          "failed to allocate curl_easy_backend");
        return NULL;
    }

    memset(b, 0, sizeof(*b));
    b->client = client;

    b->iface.perform = curl_easy_backend_perform;
    b->iface.destroy = curl_easy_backend_destroy;

    if (err)
        s3_error_reset(err);

    return &b->iface;
}
