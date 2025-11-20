/*
 * s3_client.c
 *
 * Реализация публичного C-API S3-клиента:
 *   - создание/удаление клиента;
 *   - заглушки операций (create bucket, PUT, GET, list, batch delete),
 *     которые будут реализованы позже.
 *
 * ВАЖНО:
 *   - этот файл работает поверх s3_env_t;
 *   - все аллокации клиента идут через аллокатор, выбранный в env;
 *   - детали HTTP backend'а (libcurl easy/multi и т.п.) будут добавлены позже.
 */

#include "s3_client.h"
#include "s3_config.h"
#include "s3_error.h"
#include "s3_types.h"
#include "s3_internal.h"
#include "s3_http_backend.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ----------------------------------------------------------------------
 * Внутренние helpers
 * ---------------------------------------------------------------------- */

/*
 * Установить ошибку и вернуть код.
 */
static int
s3_client_set_error(struct s3_error_info *err, int code, const char *msg)
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
 * Публичные функции: создание/удаление клиента
 * ---------------------------------------------------------------------- */

int
s3_client_new(s3_env_t *env,
              const struct s3_client_config *config,
              s3_client_t **pclient,
              struct s3_error_info *err)
{
    if (pclient == NULL) {
        return s3_client_set_error(err, S3_EINVAL,
                                   "pclient must not be NULL");
    }

    *pclient = NULL;

    if (env == NULL) {
        return s3_client_set_error(err, S3_EINVAL,
                                   "env must not be NULL");
    }

    if (config == NULL) {
        return s3_client_set_error(err, S3_EINVAL,
                                   "config must not be NULL");
    }

    if (config->endpoint == NULL || config->access_key == NULL ||
        config->secret_key == NULL) {
        return s3_client_set_error(err, S3_EINVAL,
                                   "endpoint, access_key and secret_key must not be NULL");
    }

    /* Аллокация клиента через аллокатор среды */
    struct s3_client *client =
        (struct s3_client *)env->alloc.malloc_fn(env->alloc.ud,
                                                 sizeof(*client));
    if (client == NULL) {
        return s3_client_set_error(err, S3_ENOMEM,
                                   "failed to allocate s3_client");
    }

    memset(client, 0, sizeof(*client));

    client->env = env;
    client->config = *config; /* поверхностное копирование, строки остаются владением вызывающего */

    client->http = s3_curl_easy_backend_create(client, err);
    if (!client->http) {
       env->alloc.free_fn(env->alloc.ud, client);
       return err->code;
    }

    *pclient = client;

    if (err != NULL)
        s3_error_reset(err);

    return S3_OK;
}

void
s3_client_delete(s3_client_t *client)
{
    if (client == NULL)
        return;

    s3_env_t *env = client->env;

    if (client->http)
        client->http->destroy(client->http);

    if (env != NULL) {
        env->alloc.free_fn(env->alloc.ud, client);
    } else {
        /* Теоретически не должно случиться, но на всякий случай fallback */
        free(client);
    }
}

/* ----------------------------------------------------------------------
 * Публичные функции: операции с бакетом и объектами (пока заглушки)
 * ---------------------------------------------------------------------- */

/*
 * TODO: Реальная реализация будет вызывать HTTP backend (libcurl),
 *       формировать запросы S3 и т.д.
 *       Сейчас возвращаем "не реализовано", чтобы всё линковалось.
 */

int
s3_client_create_bucket(s3_client_t *client,
                        const char *bucket,
                        const struct s3_create_bucket_params *params,
                        struct s3_error_info *err)
{
    (void)client;
    (void)bucket;
    (void)params;

    return s3_client_set_error(err, S3_ECURL,
                               "s3_client_create_bucket: not implemented yet");
}

int
s3_client_put_fd(s3_client_t *client,
                 const char *bucket,
                 const char *key,
                 int src_fd,
                 const struct s3_put_params *params,
                 struct s3_error_info *err)
{
    if (client == NULL || key == NULL || params == NULL) {
        return s3_client_set_error(err, S3_EINVAL,
                                   "client, key and params must not be NULL");
    }

    if (src_fd < 0) {
        return s3_client_set_error(err, S3_EINVAL,
                                   "src_fd must be non-negative");
    }

    /* Определяем бакет: явный или default_bucket */
    const char *bucket_name = bucket;
    if (bucket_name == NULL) {
        bucket_name = client->config.default_bucket;
    }
    if (bucket_name == NULL) {
        return s3_client_set_error(err, S3_EINVAL,
                                   "bucket must be provided (no default_bucket configured)");
    }

    if (client->http == NULL) {
        return s3_client_set_error(err, S3_ECURL,
                                   "HTTP backend is not initialized");
    }

    size_t offset         = params->offset;
    size_t content_length = params->content_length;

    /* Если длина не задана — попробуем вычислить через fstat(fd). */
    if (content_length == 0) {
        struct stat st;
        if (fstat(src_fd, &st) != 0) {
            return s3_client_set_error(err, S3_EINVAL,
                                       "fstat failed for src_fd");
        }
        if ((off_t)offset > st.st_size) {
            return s3_client_set_error(err, S3_EINVAL,
                                       "offset beyond end of file");
        }
        content_length = (size_t)(st.st_size - (off_t)offset);
        if (content_length == 0) {
            return s3_client_set_error(err, S3_EINVAL,
                                       "nothing to upload (content_length == 0)");
        }
    }

    /* Устанавливаем позицию в файле на offset. */
    if (offset > 0) {
        off_t res = lseek(src_fd, (off_t)offset, SEEK_SET);
        if (res == (off_t)-1) {
            return s3_client_set_error(err, S3_EINVAL,
                                       "lseek failed for src_fd");
        }
    }

    /* Строим URL: endpoint + "/" + bucket + "/" + key */
    char url[2048];

    const char *endpoint = client->config.endpoint;
    size_t endpoint_len = strlen(endpoint);

    while (endpoint_len > 0 && endpoint[endpoint_len - 1] == '/')
        endpoint_len--;

    int n = snprintf(url, sizeof(url), "%.*s/%s/%s",
                     (int)endpoint_len, endpoint, bucket_name, key);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return s3_client_set_error(err, S3_EINVAL,
                                   "resulting URL is too long");
    }

    /* Готовим заголовки: только Content-Type (Content-Length задаст curl). */
    char content_type_hdr[256];
    const char *headers[1];
    size_t header_count = 0;

    if (params->content_type != NULL && params->content_type[0] != '\0') {
        snprintf(content_type_hdr, sizeof(content_type_hdr),
                 "Content-Type: %s", params->content_type);
        headers[header_count++] = content_type_hdr;
    }

    struct s3_http_request req;
    memset(&req, 0, sizeof(req));
    req.method         = S3_HTTP_PUT;
    req.url            = url;
    req.headers        = headers;
    req.header_count   = header_count;
    req.src_fd         = src_fd;
    req.dst_fd         = -1;
    req.http_status    = 0;
    req.content_length = (long long)content_length;

    struct s3_error_info local_err;
    struct s3_error_info *perr = err != NULL ? err : &local_err;
    s3_error_reset(perr);

    int rc = client->http->perform(client->http, &req, perr);
    if (rc != S3_OK) {
        return rc;
    }

    if (req.http_status >= 200 && req.http_status < 300) {
        if (perr == &local_err && err == NULL) {
            /* ignore */
        } else {
            s3_error_reset(perr);
        }
        return S3_OK;
    }

    if (perr != NULL) {
        if (perr->code == S3_OK)
            perr->code = S3_EHTTP;
        perr->http_status = req.http_status;
        if (perr->msg[0] == '\0') {
            snprintf(perr->msg, sizeof(perr->msg),
                     "HTTP error %ld during PUT", req.http_status);
        }
    }

    return S3_EHTTP;
}

int
s3_client_get_fd(s3_client_t *client,
                 const char *bucket,
                 const char *key,
                 int dst_fd,
                 const struct s3_get_params *params,
                 struct s3_error_info *err)
{
    (void)client;
    (void)bucket;
    (void)key;
    (void)dst_fd;
    (void)params;

    return s3_client_set_error(err, S3_ECURL,
                               "s3_client_get_fd: not implemented yet");
}

int
s3_client_list_objects(s3_client_t *client,
                       const char *bucket,
                       const struct s3_list_params *params,
                       s3_list_object_cb cb,
                       void *userdata,
                       struct s3_error_info *err)
{
    (void)client;
    (void)bucket;
    (void)params;
    (void)cb;
    (void)userdata;

    return s3_client_set_error(err, S3_ECURL,
                               "s3_client_list_objects: not implemented yet");
}

int
s3_client_delete_objects(s3_client_t *client,
                         const char *bucket,
                         const char * const *keys,
                         size_t key_count,
                         const struct s3_delete_params *params,
                         s3_delete_result_cb cb,
                         void *userdata,
                         struct s3_error_info *err)
{
    (void)client;
    (void)bucket;
    (void)keys;
    (void)key_count;
    (void)params;
    (void)cb;
    (void)userdata;

    return s3_client_set_error(err, S3_ECURL,
                               "s3_client_delete_objects: not implemented yet");
}
