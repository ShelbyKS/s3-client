/* src/http/curl_easy_factory.c */

#include "s3/curl_easy_factory.h"
#include "s3_internal.h"
#include "s3/alloc.h"

#include <errno.h>
#include <string.h>
#include <unistd.h> /* pread, pwrite */
#include <stdio.h> /* snprintf */

/* Общие curl-опции ошибок, таймаутов и т.п. */

static void
s3_curl_apply_common_opts(s3_easy_handle_t *h)
{
    s3_client_t *c = h->client;
    CURL *easy = h->easy;

    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);

    if (c->connect_timeout_ms > 0)
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                         (long)c->connect_timeout_ms);

    if (c->request_timeout_ms > 0)
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                         (long)c->request_timeout_ms);

    /* FOLLOWLOCATION, SSL verify и прочее можно будет добавить опционально. */
}

/* ----------------- read/write callbacks с pread/pwrite ----------------- */

static size_t
s3_curl_read_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    s3_easy_handle_t *h = (s3_easy_handle_t *)userdata;
    s3_easy_io_t *io = &h->read_io;

    if (io->fd < 0)
        return 0;

    size_t buf_size = size * nmemb;
    if (buf_size == 0)
        return 0;

    size_t remaining = io->size_limit > 0
        ? (io->size_limit - h->read_bytes_total)
        : buf_size; /* теоретически PUT всегда знает размер, но оставим на будущее */

    if (io->size_limit > 0 && remaining == 0)
        return 0;

    size_t to_read = buf_size;
    if (io->size_limit > 0 && to_read > remaining)
        to_read = remaining;

    ssize_t rc;
    do {
        rc = pread(io->fd, ptr, to_read,
                   io->offset + (off_t)h->read_bytes_total);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        /* libcurl воспримет CURLE_READ_ERROR. */
        return CURL_READFUNC_ABORT;
    }

    if (rc == 0)
        return 0;

    h->read_bytes_total += (size_t)rc;
    return (size_t)rc;
}

static size_t
s3_curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    s3_easy_handle_t *h = (s3_easy_handle_t *)userdata;
    s3_easy_io_t *io = &h->write_io;

    if (io->fd < 0)
        return 0;

    size_t buf_size = size * nmemb;
    if (buf_size == 0)
        return 0;

    /* Если задан лимит на размер принимаемых данных — уважим его. */
    size_t remaining = io->size_limit > 0
        ? (io->size_limit - h->write_bytes_total)
        : buf_size;

    if (io->size_limit > 0 && remaining == 0)
        return 0;

    size_t to_write = buf_size;
    if (io->size_limit > 0 && to_write > remaining)
        to_write = remaining;

    ssize_t rc;
    do {
        rc = pwrite(io->fd, ptr, to_write,
                    io->offset + (off_t)h->write_bytes_total);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        return 0; /* libcurl воспримет это как CURLE_WRITE_ERROR */
    }

    h->write_bytes_total += (size_t)rc;
    return (size_t)rc;
}

/* ----------------- построение URL ----------------- */

static s3_error_code_t
s3_build_url(s3_client_t *client,
             const char *bucket,
             const char *key,
             char **out_url,
             s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    const char *endpoint = client->endpoint;
    if (bucket == NULL)
        bucket = client->default_bucket;

    if (endpoint == NULL || bucket == NULL || key == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "endpoint, bucket and key must be set", 0, 0, 0);
        return err->code;
    }

    size_t endpoint_len = strlen(endpoint);
    size_t bucket_len = strlen(bucket);
    size_t key_len = strlen(key);

    /* Пример: "https://host/bucket/key" */
    /* +2 слэша и '\0'. */
    size_t need = endpoint_len + 1 + bucket_len + 1 + key_len + 1;

    char *url = (char *)s3_alloc(&client->alloc, need);
    if (url == NULL) {
        s3_error_set(err, S3_E_NOMEM, "Out of memory in s3_build_url",
                     ENOMEM, 0, 0);
        return err->code;
    }

    /* Уберём возможный trailing slash у endpoint. */
    size_t pos = 0;
    memcpy(url, endpoint, endpoint_len);
    pos = endpoint_len;
    if (pos > 0 && url[pos - 1] == '/')
        pos--;

    url[pos++] = '/';
    memcpy(url + pos, bucket, bucket_len);
    pos += bucket_len;
    url[pos++] = '/';
    memcpy(url + pos, key, key_len);
    pos += key_len;
    url[pos] = '\0';

    *out_url = url;
    return S3_E_OK;
}

/* ----------------- AWS SigV4 через CURLOPT_AWS_SIGV4 ----------------- */

static s3_error_code_t
s3_curl_apply_sigv4(s3_easy_handle_t *h, s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

// #ifndef CURLOPT_AWS_SIGV4
#ifndef CURLAUTH_AWS_SIGV4
    s3_error_set(err, S3_E_INIT,
                 "libcurl was built without CURLOPT_AWS_SIGV4 (requires libcurl >= 7.75.0)",
                 0, 0, 0);
    return err->code;
#else
    s3_client_t *c = h->client;

    if (c->access_key == NULL || c->secret_key == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "access_key and secret_key must be set for SigV4",
                     0, 0, 0);
        return err->code;
    }

    /* Строка для CURLOPT_AWS_SIGV4:
     *   "aws:amz:<region>:s3"
     *
     * region берём из клиента. Если вдруг NULL, можно дефолтнуть, но лучше
     * считать это ошибкой конфигурации.
     */
    const char *region = c->region;
    if (region == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "region must be set for SigV4", 0, 0, 0);
        return err->code;
    }

    char sigv4_param[128];
    int n = snprintf(sigv4_param, sizeof(sigv4_param),
                     "aws:amz:%s:s3", region);
    if (n <= 0 || (size_t)n >= sizeof(sigv4_param)) {
        s3_error_set(err, S3_E_INTERNAL,
                     "region string is too long for SigV4 param", 0, 0, 0);
        return err->code;
    }

    /* libcurl копирует строку, поэтому мы можем использовать stack-буфер. */
    CURLcode cc = curl_easy_setopt(h->easy, CURLOPT_AWS_SIGV4, sigv4_param);
    if (cc != CURLE_OK) {
        s3_error_set(err, S3_E_CURL,
                     curl_easy_strerror(cc), 0, 0, (long)cc);
        return err->code;
    }

    /*
     * Креды передаются через CURLOPT_USERPWD: "ACCESS_KEY:SECRET_KEY".
     * Это описано в документации к CURLOPT_AWS_SIGV4. :contentReference[oaicite:0]{index=0}
     */
    size_t ak_len = strlen(c->access_key);
    size_t sk_len = strlen(c->secret_key);

    size_t cred_len = ak_len + 1 + sk_len + 1; /* ':' + '\0' */
    char *cred = (char *)s3_alloc(&c->alloc, cred_len);
    if (cred == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Out of memory building SigV4 credentials", ENOMEM, 0, 0);
        return err->code;
    }

    memcpy(cred, c->access_key, ak_len);
    cred[ak_len] = ':';
    memcpy(cred + ak_len + 1, c->secret_key, sk_len);
    cred[cred_len - 1] = '\0';

    cc = curl_easy_setopt(h->easy, CURLOPT_USERPWD, cred);

    /* libcurl тоже копирует эту строку, поэтому сразу освобождаем. */
    s3_free(&c->alloc, cred);

    if (cc != CURLE_OK) {
        s3_error_set(err, S3_E_CURL,
                     curl_easy_strerror(cc), 0, 0, (long)cc);
        return err->code;
    }

    /* Временные токены STS → x-amz-security-token */
    if (c->session_token != NULL) {
        char header_prefix[] = "x-amz-security-token: ";
        size_t hp_len = sizeof(header_prefix) - 1;
        size_t token_len = strlen(c->session_token);

        size_t total = hp_len + token_len + 1;
        char *hdr = (char *)s3_alloc(&c->alloc, total);
        if (hdr == NULL) {
            s3_error_set(err, S3_E_NOMEM,
                         "Out of memory building x-amz-security-token header",
                         ENOMEM, 0, 0);
            return err->code;
        }

        memcpy(hdr, header_prefix, hp_len);
        memcpy(hdr + hp_len, c->session_token, token_len);
        hdr[total - 1] = '\0';

        h->headers = curl_slist_append(h->headers, hdr);

        /* Строку hdr можно освободить сразу после append. */
        s3_free(&c->alloc, hdr);

        if (h->headers != NULL)
            curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);
    }

    return S3_E_OK;
#endif
}

/* ----------------- заголовки ----------------- */

static s3_error_code_t
s3_apply_headers_common(s3_easy_handle_t *h,
                        const s3_put_opts_t *put_opts,
                        s3_error_t *error)
{
    (void)error;

    struct curl_slist *headers = h->headers;

    if (put_opts != NULL && put_opts->content_type != NULL) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "Content-Type: %s",
                         put_opts->content_type);
        if (n > 0 && (size_t)n < sizeof(buf))
            headers = curl_slist_append(headers, buf);
    }

    /* здесь позже можно добавить ещё заголовки, если нужно */

    h->headers = headers;
    if (headers != NULL)
        curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, headers);

    return S3_E_OK;
}

/* ----------------- создание/уничтожение easy handle ----------------- */

static s3_easy_handle_t *
s3_easy_handle_alloc(s3_client_t *client)
{
    s3_easy_handle_t *h = (s3_easy_handle_t *)
        s3_alloc(&client->alloc, sizeof(*h));
    if (h == NULL)
        return NULL;

    memset(h, 0, sizeof(*h));
    h->client = client;
    h->easy = curl_easy_init();
    if (h->easy == NULL) {
        s3_free(&client->alloc, h);
        return NULL;
    }

    return h;
}

void
s3_easy_handle_destroy(s3_easy_handle_t *h)
{
    if (h == NULL)
        return;

    if (h->headers != NULL)
        curl_slist_free_all(h->headers);
    if (h->easy != NULL)
        curl_easy_cleanup(h->easy);

    s3_client_t *c = h->client;

    if (h->url != NULL && c != NULL)
        s3_free(&c->alloc, h->url);

    if (c != NULL)
        s3_free(&c->alloc, h);
}

/* ----------------- публичные фабрики PUT/GET ----------------- */

s3_error_code_t
s3_easy_factory_new_put(s3_client_t *client,
                        const s3_put_opts_t *opts,
                        const s3_easy_io_t *io,
                        s3_easy_handle_t **out_handle,
                        s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (out_handle == NULL || client == NULL || opts == NULL || io == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts, io or out_handle is NULL", 0, 0, 0);
        return err->code;
    }

    if (io->fd < 0 || io->size_limit == 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid fd or size_limit for PUT", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = s3_easy_handle_alloc(client);
    if (h == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to allocate s3_easy_handle", ENOMEM, 0, 0);
        return err->code;
    }

    h->read_io = *io;
    h->write_io.fd = -1;
    h->read_bytes_total = 0;
    h->write_bytes_total = 0;

    char *url = NULL;
    s3_error_code_t rc = s3_build_url(client,
                                      opts->bucket,
                                      opts->key,
                                      &url, err);
    if (rc != S3_E_OK) {
        s3_easy_handle_destroy(h);
        return rc;
    }
    /* Сохраняем URL внутри easy-хендла для последующего free. */
    h->url = url;

    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    /* URL можно освобождать только после выполнения запроса, поэтому оставляем
     * жизнь строки внутри h/клиента. Чтобы не усложнять, пока просто не free. */
    /* TODO: потом можно завести поле в h для хранения URL и освободить в destroy. */

    curl_easy_setopt(h->easy, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(h->easy, CURLOPT_READFUNCTION, s3_curl_read_cb);
    curl_easy_setopt(h->easy, CURLOPT_READDATA, h);

    /* Если знаем длину — скажем об этом curl. */
    curl_easy_setopt(h->easy, CURLOPT_INFILESIZE_LARGE,
                     (curl_off_t)io->size_limit);

    s3_curl_apply_common_opts(h);
    s3_apply_headers_common(h, opts, err);

    s3_error_code_t rc2 = s3_curl_apply_sigv4(h, err);
    if (rc2 != S3_E_OK) {
       s3_easy_handle_destroy(h);
       return rc2;
    }

    *out_handle = h;
    return S3_E_OK;
}

s3_error_code_t
s3_easy_factory_new_get(s3_client_t *client,
                        const s3_get_opts_t *opts,
                        const s3_easy_io_t *io,
                        s3_easy_handle_t **out_handle,
                        s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (out_handle == NULL || client == NULL || opts == NULL || io == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts, io or out_handle is NULL", 0, 0, 0);
        return err->code;
    }

    if (io->fd < 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid fd for GET", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = s3_easy_handle_alloc(client);
    if (h == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to allocate s3_easy_handle", ENOMEM, 0, 0);
        return err->code;
    }

    h->write_io = *io;
    h->read_io.fd = -1;
    h->read_bytes_total = 0;
    h->write_bytes_total = 0;

    char *url = NULL;
    s3_error_code_t rc = s3_build_url(client,
                                      opts->bucket,
                                      opts->key,
                                      &url, err);
    if (rc != S3_E_OK) {
        s3_easy_handle_destroy(h);
        return rc;
    }

    /* Сохраняем URL внутри easy-хендла для последующего free. */
    h->url = url;

    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_HTTPGET, 1L);

    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, s3_curl_write_cb);
    curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, h);

    /* Range при необходимости. */
    if (opts->range != NULL)
        curl_easy_setopt(h->easy, CURLOPT_RANGE, opts->range);

    s3_curl_apply_common_opts(h);
    s3_apply_headers_common(h, NULL, err);

    s3_error_code_t rc2 = s3_curl_apply_sigv4(h, err);
    if (rc2 != S3_E_OK) {
        s3_easy_handle_destroy(h);
        return rc2;
    }

    *out_handle = h;
    return S3_E_OK;
}
