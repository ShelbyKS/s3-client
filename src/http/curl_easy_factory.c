#include "s3/curl_easy_factory.h"
#include "s3/alloc.h"

#include "s3_internal.h"
#include "http_util.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>

#ifdef S3_USE_TARANTOOL_CURL
#  pragma message(">>> using tarantool/curl.h")
#else
#  pragma message(">>> using system <curl/curl.h>")
#endif

/* Общие curl-опции ошибок, таймаутов и т.п. */

static void
s3_curl_apply_common_opts(s3_easy_handle_t *h)
{
    s3_client_t *c = h->client;
    CURL *easy = h->easy;

    if (c->connect_timeout_ms > 0) {
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                         (long)c->connect_timeout_ms);
    }

    if (c->request_timeout_ms > 0) {
#if LIBCURL_VERSION_NUM >= 0x072000 /* 7.32.0 */
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                         (long)c->request_timeout_ms);
#else
        curl_easy_setopt(easy, CURLOPT_TIMEOUT,
                         (long)((c->request_timeout_ms + 999) / 1000));
#endif
    }

    if (c->proxy != NULL) {
        curl_easy_setopt(easy, CURLOPT_PROXY, c->proxy);
    }

    if (c->ca_file != NULL) {
        curl_easy_setopt(easy, CURLOPT_CAINFO, c->ca_file);
    }
    if (c->ca_path != NULL) {
        curl_easy_setopt(easy, CURLOPT_CAPATH, c->ca_path);
    }


    if (c->flags & S3_CLIENT_F_SKIP_PEER_VERIFICATION) {
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    }

    if (c->flags & S3_CLIENT_F_SKIP_HOSTNAME_VERIF) {
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
}
/* ----------------- read/write callbacks с pread/pwrite ----------------- */

static size_t
s3_curl_read_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    s3_easy_handle_t *h  = (s3_easy_handle_t *)userdata;
    s3_easy_io_t     *io = &h->read_io;

    size_t buf_size = size * nmemb;
    if (buf_size == 0 || io->kind == S3_IO_NONE)
        return 0;

    /*
     * Сначала считаем, сколько МАКСИМУМ мы вообще можем отдать
     * с учётом size_limit. Это общая часть для всех kind'ов.
     */
    size_t max_to_read = buf_size;

    if (io->size_limit > 0) {
        /* Уже всё отдали */
        if (h->read_bytes_total >= io->size_limit)
            return 0;

        size_t left = io->size_limit - h->read_bytes_total;
        if (left < max_to_read)
            max_to_read = left;

        if (max_to_read == 0)
            return 0;
    }

    switch (io->kind) {
    case S3_IO_FD: {
        int fd = io->u.fd.fd;
        if (fd < 0)
            return 0;

        size_t to_read = max_to_read;

        ssize_t rc;
        do {
            rc = pread(fd, ptr, to_read,
                       io->u.fd.offset + (off_t)h->read_bytes_total);
        } while (rc < 0 && errno == EINTR);

        if (rc < 0) {
            /* libcurl воспримет это как CURLE_READ_ERROR. */
            return CURL_READFUNC_ABORT;
        }
        if (rc == 0)
            return 0; /* EOF */

        h->read_bytes_total += (size_t)rc;
        return (size_t)rc;
    }

    case S3_IO_MEM: {
        s3_mem_buf_t *b = io->u.mem.buf;
        if (b == NULL || b->data == NULL)
            return 0;

        size_t total = b->size;
        if (h->read_bytes_total >= total)
            return 0; /* всё уже выдали → EOF */

        size_t remaining_buf = total - h->read_bytes_total;

        /* Нельзя читать больше, чем есть в буфере. */
        size_t to_read = max_to_read;
        if (to_read > remaining_buf)
            to_read = remaining_buf;

        if (to_read == 0)
            return 0;

        memcpy(ptr, b->data + h->read_bytes_total, to_read);
        h->read_bytes_total += to_read;
        return to_read;
    }

    case S3_IO_NONE:
    default:
        return 0;
    }
}

static size_t
s3_curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    s3_easy_handle_t *h = (s3_easy_handle_t *)userdata;
    s3_easy_io_t *io = &h->write_io;

    size_t buf_size = size * nmemb;
    if (buf_size == 0)
        return 0;

    /* Если вывод никуда не нужно писать — просто "проглатываем" данные. */
    if (io->kind == S3_IO_NONE) {
        h->write_bytes_total += buf_size;
        return buf_size; /* ВСЕ байты приняты → для libcurl это успех */
    }

    size_t remaining = io->size_limit > 0
        ? (io->size_limit - h->write_bytes_total)
        : buf_size;

    if (io->size_limit > 0 && remaining == 0)
        return 0;

    size_t to_write = buf_size;
    if (io->size_limit > 0 && to_write > remaining)
        to_write = remaining;

    switch (io->kind) {
    case S3_IO_FD: {
        int fd = io->u.fd.fd;
        if (fd < 0)
            return 0;

        ssize_t rc;
        do {
            rc = pwrite(fd, ptr, to_write,
                        io->u.fd.offset + (off_t)h->write_bytes_total);
        } while (rc < 0 && errno == EINTR);

        if (rc < 0) {
            return 0; /* CURLE_WRITE_ERROR */
        }

        h->write_bytes_total += (size_t)rc;
        return (size_t)rc;
    }
    case S3_IO_MEM: {
        s3_mem_buf_t *b = io->u.mem.buf;
        if (!b)
            return 0;

        s3_client_t *c = h->client;
        size_t need = b->size + to_write + 1;
        if (s3_mem_buf_reserve(c, b, need) != 0) {
            return 0; /* curl воспримет это как CURLE_WRITE_ERROR */
        }

        memcpy(b->data + b->size, ptr, to_write);
        b->size += to_write;
        b->data[b->size] = '\0';

        h->write_bytes_total += to_write;
        return to_write;
    }
    case S3_IO_NONE:
    default:
        return 0;
    }
}

/* ----------------- AWS SigV4 через CURLOPT_AWS_SIGV4 ----------------- */

static s3_error_code_t
s3_curl_apply_sigv4(s3_easy_handle_t *h, s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    s3_client_t *c = h->client;

    if (c->access_key == NULL || c->secret_key == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "access_key and secret_key must be set for auth",
                     0, 0, 0);
        return err->code;
    }

    /*
     * 1) Если SigV4 НЕ требуется, используем обычный Basic Auth.
     */
    if (!c->require_sigv4) {
        size_t ak_len = strlen(c->access_key);
        size_t sk_len = strlen(c->secret_key);

        size_t cred_len = ak_len + 1 + sk_len + 1;
        char *cred = (char *)s3_alloc(&c->alloc, cred_len);
        if (cred == NULL) {
            s3_error_set(err, S3_E_NOMEM,
                         "Out of memory building basic auth credentials",
                         ENOMEM, 0, 0);
            return err->code;
        }

        memcpy(cred, c->access_key, ak_len);
        cred[ak_len] = ':';
        memcpy(cred + ak_len + 1, c->secret_key, sk_len);
        cred[cred_len - 1] = '\0';

        CURLcode cc;
        cc = curl_easy_setopt(h->easy, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        if (cc == CURLE_OK)
            cc = curl_easy_setopt(h->easy, CURLOPT_USERPWD, cred);

        s3_free(&c->alloc, cred);

        if (cc != CURLE_OK) {
            s3_error_set(err, S3_E_CURL,
                         curl_easy_strerror(cc), 0, 0, (long)cc);
            return err->code;
        }

        /* x-amz-security-token, если есть session_token */
        if (c->session_token != NULL) {
            const char header_prefix[] = "x-amz-security-token: ";
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
            s3_free(&c->alloc, hdr);
        }

        return S3_E_OK;
    }

    /*
     * 2) Здесь c->require_sigv4 == true → пробуем AWS SigV4.
     */
    if (c->region == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "region must be set for SigV4", 0, 0, 0);
        return err->code;
    }

    char sigv4_param[128];
    int n = snprintf(sigv4_param, sizeof(sigv4_param),
                     "aws:amz:%s:s3", c->region);
    if (n <= 0 || (size_t)n >= sizeof(sigv4_param)) {
        s3_error_set(err, S3_E_INTERNAL,
                     "region string is too long for SigV4 param", 0, 0, 0);
        return err->code;
    }

    CURLcode cc = curl_easy_setopt(h->easy, CURLOPT_AWS_SIGV4, sigv4_param);
    if (cc == CURLE_UNKNOWN_OPTION) {
        s3_error_set(err, S3_E_INIT,
                     "libcurl was built without CURLOPT_AWS_SIGV4 (requires libcurl >= 7.75.0)",
                     0, 0, (long)cc);
        return err->code;
    }
    if (cc != CURLE_OK) {
        s3_error_set(err, S3_E_CURL,
                     curl_easy_strerror(cc), 0, 0, (long)cc);
        return err->code;
    }

    /* Креды через USERPWD как раньше. */
    size_t ak_len = strlen(c->access_key);
    size_t sk_len = strlen(c->secret_key);

    size_t cred_len = ak_len + 1 + sk_len + 1;
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
    s3_free(&c->alloc, cred);

    if (cc != CURLE_OK) {
        s3_error_set(err, S3_E_CURL,
                     curl_easy_strerror(cc), 0, 0, (long)cc);
        return err->code;
    }

    if (c->session_token != NULL) {
        const char header_prefix[] = "x-amz-security-token: ";
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
        s3_free(&c->alloc, hdr);
    }

    return S3_E_OK;
}

/* ----------------- создание/уничтожение easy handle ----------------- */

static s3_easy_handle_t *
s3_easy_handle_alloc(s3_client_t *client)
{
    s3_easy_handle_t *h = (s3_easy_handle_t *)s3_alloc(&client->alloc, sizeof(*h));
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

    if (h->owned_body.data)
        s3_free(&c->alloc, h->owned_body.data);

    if (h->owned_resp.data)
        s3_free(&c->alloc, h->owned_resp.data);

    if (c != NULL)
        s3_free(&c->alloc, h);
}

/* ----------------- публичные фабрики методов ----------------- */

s3_error_code_t
s3_easy_factory_new_put_fd(s3_client_t *client,
                        const s3_put_opts_t *opts,
                        int fd, off_t offset, size_t size,
                        s3_easy_handle_t **out_handle,
                        s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (out_handle == NULL || client == NULL || opts == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts or out_handle is NULL", 0, 0, 0);
        return err->code;
    }

    if (fd < 0 || size == 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid fd or size for PUT", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = s3_easy_handle_alloc(client);
    if (h == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to allocate s3_easy_handle", ENOMEM, 0, 0);
        return err->code;
    }

    /* Настраиваем I/O: читаем тело из fd, ничего не пишем. */
    s3_easy_io_init_fd(&h->read_io, fd, offset, size);
    s3_easy_io_init_none(&h->write_io);

    h->read_bytes_total = 0;
    h->write_bytes_total = 0;

    char *url = NULL;
    s3_error_code_t rc = s3_build_url(client,
                                      opts->bucket,
                                      opts->key,
                                      &url, err);
    if (rc != S3_E_OK) {
        goto fail;
    }
    /* Сохраняем URL внутри easy-хендла для последующего free. */
    h->url = url;

    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(h->easy, CURLOPT_READFUNCTION, s3_curl_read_cb);
    curl_easy_setopt(h->easy, CURLOPT_READDATA, h);
    curl_easy_setopt(h->easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);

    s3_curl_apply_common_opts(h);
    
    if (opts->content_type != NULL) {
        // TODO: всегда ли хватит?
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "Content-Type: %s", opts->content_type);
        if (n > 0 && (size_t)n < sizeof(buf))
            h->headers = curl_slist_append(h->headers, buf);
        else {
            s3_error_set(err, S3_E_NOMEM,
                     "Failed to make Content-Type header", ENOMEM, 0, 0);
            goto fail;
        }

    }

    rc = s3_curl_apply_sigv4(h, err);
    if (rc != S3_E_OK) {
       goto fail;
    }

    if (h->headers != NULL) {
        curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);
    }

    *out_handle = h;
    return S3_E_OK;

fail:
    s3_easy_handle_destroy(h);
    return err->code;
}

s3_error_code_t
s3_easy_factory_new_get_fd(s3_client_t *client,
                        const s3_get_opts_t *opts,
                        int fd, off_t offset, size_t max_size,
                        s3_easy_handle_t **out_handle,
                        s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (out_handle == NULL || client == NULL || opts == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts or out_handle is NULL", 0, 0, 0);
        return err->code;
    }

    if (fd < 0) {
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

    /* Читаем из сети → пишем в fd.
     * max_size == 0 → без ограничения (это уже интерпретирует write_cb).
     */
    s3_easy_io_init_fd(&h->write_io, fd, offset, max_size);
    s3_easy_io_init_none(&h->read_io);

    h->read_bytes_total = 0;
    h->write_bytes_total = 0;

    char *url = NULL;
    s3_error_code_t rc = s3_build_url(client,
                                      opts->bucket,
                                      opts->key,
                                      &url, err);
    if (rc != S3_E_OK) {
        goto fail;
    }
    h->url = url;

    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, s3_curl_write_cb);
    curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, h);

    /* Range при необходимости. */
    if (opts->range != NULL)
        curl_easy_setopt(h->easy, CURLOPT_RANGE, opts->range);

    s3_curl_apply_common_opts(h);

    rc = s3_curl_apply_sigv4(h, err);
    if (rc != S3_E_OK) {
        goto fail;
    }

    if (h->headers != NULL) {
        curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);
    }

    *out_handle = h;
    return S3_E_OK;

fail:
    s3_easy_handle_destroy(h);
    return err->code;
}

s3_error_code_t
s3_easy_factory_new_create_bucket(s3_client_t *client,
                                  const s3_create_bucket_opts_t *opts,
                                  s3_easy_handle_t **out_handle,
                                  s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (out_handle == NULL || client == NULL || opts == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts, io or out_handle is NULL", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = s3_easy_handle_alloc(client);
    if (h == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to allocate s3_easy_handle", ENOMEM, 0, 0);
        return err->code;
    }

    char *url = NULL;
    s3_error_code_t rc = s3_build_url(client,
                                      opts->bucket,
                                      NULL,
                                      &url, err);
    if (rc != S3_E_OK) {
        goto fail;
    }
    h->url = url;
    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    /* PUT без тела. */
    curl_easy_setopt(h->easy, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(h->easy, CURLOPT_CUSTOMREQUEST, "PUT");

    s3_curl_apply_common_opts(h);

    s3_error_code_t rc2 = s3_curl_apply_sigv4(h, err);
    if (rc2 != S3_E_OK) {
        goto fail;
    }

    if (h->headers != NULL) {
        curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);
    }

    *out_handle = h;
    return S3_E_OK;

fail:
    s3_easy_handle_destroy(h);
    return err->code;
}

s3_error_code_t
s3_easy_factory_new_list_objects(s3_client_t *client,
                                 const s3_list_objects_opts_t *opts,
                                 s3_easy_handle_t **out_handle,
                                 s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (out_handle == NULL || client == NULL || opts == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts or out_handle is NULL", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = s3_easy_handle_alloc(client);
    if (h == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to allocate s3_easy_handle", ENOMEM, 0, 0);
        return err->code;
    }

    /* Читаем только ответ (XML), запрос без тела. */
    s3_easy_io_init_none(&h->read_io);

    /* Пишем ответ в h->owned_resp (как и в delete_objects). */
    s3_mem_buf_t *resp = &h->owned_resp;
    resp->data = NULL;
    resp->size = 0;
    resp->capacity = 0;
    s3_easy_io_init_mem(&h->write_io, resp, 0); /* 0 = без лимита */

    h->read_bytes_total = 0;
    h->write_bytes_total = 0;

    char *url = NULL;
    s3_error_code_t rc = s3_build_list_url(client, opts, &url, err);
    if (rc != S3_E_OK) {
        goto fail;
    }
    h->url = url;

    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, s3_curl_write_cb);
    curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, h);

    s3_curl_apply_common_opts(h);

    rc = s3_curl_apply_sigv4(h, err);
    if (rc != S3_E_OK) {
        goto fail;
    }

    if (h->headers != NULL) {
        curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);
    }

    *out_handle = h;
    return S3_E_OK;

fail:
    s3_easy_handle_destroy(h);
    return err->code;
}

s3_error_code_t
s3_easy_factory_new_delete_objects(s3_client_t *client,
                                   const s3_delete_objects_opts_t *opts,
                                   s3_easy_handle_t **out_handle,
                                   s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (client == NULL || opts == NULL || out_handle == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts or out_handle is NULL", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = s3_easy_handle_alloc(client);
    if (h == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to allocate s3_easy_handle", ENOMEM, 0, 0);
        return err->code;
    }

    /* Строим XML-тело в h->owned_body. */
    s3_mem_buf_t *body = &h->owned_body;
    memset(body, 0, sizeof(*body));

    /* Строим XML-тело в buf. */
    s3_error_code_t rc = s3_build_delete_body(client, opts, body, err);
    if (rc != S3_E_OK) {
        goto fail;
    }

    /* Исходящее тело: читаем XML из памяти. */
    s3_easy_io_init_mem(&h->read_io, body, body->size);

    /* Входящее тело (ответ): сразу собираем в h->owned_resp. */
    s3_mem_buf_t *resp = &h->owned_resp;
    resp->data = NULL;
    resp->size = 0;
    resp->capacity = 0;
    s3_easy_io_init_mem(&h->write_io, resp, 0); /* 0 = без лимита */

    h->read_bytes_total  = 0;
    h->write_bytes_total = 0;

    /* URL: endpoint/bucket?delete */
    char *url = NULL;
    rc = s3_build_delete_url(client, opts, &url, err);
    if (rc != S3_E_OK) {
        goto fail;
    }
    h->url = url;

    curl_easy_setopt(h->easy, CURLOPT_URL, url);
    curl_easy_setopt(h->easy, CURLOPT_POST, 1L);
    curl_easy_setopt(h->easy, CURLOPT_READFUNCTION, s3_curl_read_cb);
    curl_easy_setopt(h->easy, CURLOPT_READDATA, h);
    curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, s3_curl_write_cb);
    curl_easy_setopt(h->easy, CURLOPT_WRITEDATA,     h);
    curl_easy_setopt(h->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)body->size);

    s3_curl_apply_common_opts(h);

    // TODO: всегда ли хватит?
    char header_md5[128];
    rc = s3_build_content_md5_header(body->data, body->size,
                                    header_md5, sizeof(header_md5), err);
    if (rc != S3_E_OK) {
        goto fail;
    }

    h->headers = curl_slist_append(h->headers, "Content-Type: application/xml");
    h->headers = curl_slist_append(h->headers, header_md5);

    if (!h->headers) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to append Content-MD5 header", ENOMEM, 0, 0);
        goto fail;
    }

    rc = s3_curl_apply_sigv4(h, err);
    if (rc != S3_E_OK) {
        goto fail;
    }

    if (h->headers != NULL) {
        curl_easy_setopt(h->easy, CURLOPT_HTTPHEADER, h->headers);
    }

    *out_handle = h;
    return S3_E_OK;

fail:
    s3_easy_handle_destroy(h);
    return err->code;
}
