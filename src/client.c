#include "s3/client.h"
#include "s3/alloc.h"
#include "s3_internal.h"

#include <sys/types.h>
#include <string.h>
#include <errno.h>

#include <tarantool/module.h>

/* ----------------- Вспомогательные функции для ошибок ----------------- */

void
s3_error_clear(s3_error_t *err)
{
    if (err == NULL)
        return;
    memset(err, 0, sizeof(*err));
    err->code = S3_E_OK;
}

void
s3_error_set(s3_error_t *err, s3_error_code_t code,
             const char *msg, int os_error,
             int http_status, long curl_code)
{
    if (err == NULL)
        return;

    memset(err, 0, sizeof(*err));
    err->code = code;
    err->os_error = os_error;
    err->http_status = http_status;
    err->curl_code = curl_code;

    if (msg != NULL && msg[0] != '\0') {
        /* Обрежем строку, чтобы точно влезла с '\0'. */
        size_t n = strlen(msg);
        if (n >= sizeof(err->message))
            n = sizeof(err->message) - 1;
        memcpy(err->message, msg, n);
        err->message[n] = '\0';
    }
}

const char *
s3_error_code_str(s3_error_code_t code)
{
    switch (code) {
    case S3_E_OK:           return "S3_E_OK";
    case S3_E_INVALID_ARG:  return "S3_E_INVALID_ARG";
    case S3_E_NOMEM:        return "S3_E_NOMEM";
    case S3_E_INIT:         return "S3_E_INIT";
    case S3_E_CURL:         return "S3_E_CURL";
    case S3_E_HTTP:         return "S3_E_HTTP";
    case S3_E_SIGV4:        return "S3_E_SIGV4";
    case S3_E_IO:           return "S3_E_IO";
    case S3_E_TIMEOUT:      return "S3_E_TIMEOUT";
    case S3_E_NOT_FOUND:    return "S3_E_NOT_FOUND";
    case S3_E_AUTH:         return "S3_E_AUTH";
    case S3_E_ACCESS_DENIED:return "S3_E_ACCESS_DENIED";
    case S3_E_CANCELLED:    return "S3_E_CANCELLED";
    case S3_E_INTERNAL:     return "S3_E_INTERNAL";
    default:                return "S3_E_UNKNOWN";
    }
}

const char *
s3_error_message(const s3_error_t *err)
{
    if (err == NULL)
        return "S3 internal error (NULL error pointer)";

    if (err->message[0] != '\0')
        return err->message;

    switch (err->code) {
    case S3_E_OK:
        return "Success";
    case S3_E_INVALID_ARG:
        return "Invalid argument";
    case S3_E_NOMEM:
        return "Out of memory";
    case S3_E_INIT:
        return "Initialization error";
    case S3_E_CURL:
        return "libcurl error";
    case S3_E_HTTP:
        return "HTTP error";
    case S3_E_SIGV4:
        return "AWS SigV4 signing error";
    case S3_E_IO:
        return "I/O error";
    case S3_E_TIMEOUT:
        return "Operation timed out";
    case S3_E_NOT_FOUND:
        return "Object or bucket not found";
    case S3_E_AUTH:
        return "Authentication error";
    case S3_E_ACCESS_DENIED:
        return "Access denied";
    case S3_E_CANCELLED:
        return "Operation cancelled";
    case S3_E_INTERNAL:
    default:
        return "Internal error";
    }
}

/* ----------------- Реализация внутренних helper’ов ----------------- */

char *
s3_strdup_a(const s3_allocator_t *a, const char *src, s3_error_t *err)
{
    if (src == NULL)
        return NULL;

    size_t len = strlen(src);
    char *dst = (char *)s3_alloc(a, len + 1);
    if (dst == NULL) {
        s3_error_set(err, S3_E_NOMEM, "Out of memory in s3_strdup_a",
                     ENOMEM, 0, 0);
        return NULL;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

void
s3_client_set_error(struct s3_client *client, const s3_error_t *src)
{
    if (client == NULL)
        return;

    if (src == NULL) {
        s3_error_t tmp = S3_ERROR_INIT;
        tmp.code = S3_E_INTERNAL;
        s3_error_set(&client->last_error, tmp.code,
                     "Internal error", 0, 0, 0);
        return;
    }

    client->last_error = *src;
}

/* ----------------- API: last_error ----------------- */

void
s3_client_last_error(const s3_client_t *client, s3_error_t *err)
{
    if (err == NULL)
        return;
    if (client == NULL) {
        s3_error_set(err, S3_E_INTERNAL, "Client is NULL", 0, 0, 0);
        return;
    }
    *err = client->last_error;
}

/* ----------------- Инициализация / уничтожение клиента ----------------- */

static void
s3_client_init_defaults(struct s3_client *c, const s3_client_opts_t *opts)
{
    /* backend уже выбран до этого (при создании) */
    c->backend_type = opts->backend;

    c->connect_timeout_ms = opts->connect_timeout_ms > 0 ?
                            opts->connect_timeout_ms : 5000;


    c->request_timeout_ms = opts->request_timeout_ms > 0 ?
                            opts->request_timeout_ms : 30000;

    c->max_total_connections = opts->max_total_connections > 0 ?
                               opts->max_total_connections : 64;

    c->max_connections_per_host = opts->max_connections_per_host > 0 ?
                                  opts->max_connections_per_host : 16;

    c->multi_idle_timeout_ms = opts->multi_idle_timeout_ms > 0 ?
                               opts->multi_idle_timeout_ms : 50;

    c->flags = opts->flags;
    c->require_sigv4 = opts->require_sigv4;
}

static void
s3_client_free_strings(struct s3_client *c)
{
    if (c == NULL)
        return;

    s3_free(&c->alloc, c->endpoint);
    s3_free(&c->alloc, c->region);
    s3_free(&c->alloc, c->access_key);
    s3_free(&c->alloc, c->secret_key);
    s3_free(&c->alloc, c->session_token);
    s3_free(&c->alloc, c->default_bucket);

    c->endpoint = NULL;
    c->region = NULL;
    c->access_key = NULL;
    c->secret_key = NULL;
    c->session_token = NULL;
    c->default_bucket = NULL;
    c->ca_file = NULL;
    c->ca_path = NULL;
    c->proxy = NULL;
}

s3_error_code_t
s3_client_new(const s3_client_opts_t *opts,
              s3_client_t **out_client,
              s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;
    s3_error_clear(err);

    if (out_client == NULL || opts == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "opts or out_client is NULL", 0, 0, 0);
        return err->code;
    }

    if (opts->endpoint == NULL || opts->region == NULL ||
        opts->access_key == NULL || opts->secret_key == NULL)
    {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "endpoint, region, access_key and secret_key must be set",
                     0, 0, 0);
        return err->code;
    }

    s3_error_code_t rc_init = s3_curl_global_init(err);
    if (rc_init != S3_E_OK)
        return rc_init;

    const s3_allocator_t *a = opts->allocator;
    if (a == NULL)
        a = s3_allocator_default();

    struct s3_client *c = (struct s3_client *)s3_alloc(a, sizeof(*c));
    if (c == NULL) {
        s3_error_set(err, S3_E_NOMEM, "Failed to allocate s3_client",
                     ENOMEM, 0, 0);
        return err->code;
    }
    memset(c, 0, sizeof(*c));
    c->alloc = *a;
    c->last_error = (s3_error_t)S3_ERROR_INIT;

    /* Копируем строки. */
    c->endpoint = s3_strdup_a(&c->alloc, opts->endpoint, err);
    if (opts->endpoint != NULL && c->endpoint == NULL)
        goto fail;

    c->region = s3_strdup_a(&c->alloc, opts->region, err);
    if (opts->region != NULL && c->region == NULL)
        goto fail;

    c->access_key = s3_strdup_a(&c->alloc, opts->access_key, err);
    if (opts->access_key != NULL && c->access_key == NULL)
        goto fail;

    c->secret_key = s3_strdup_a(&c->alloc, opts->secret_key, err);
    if (opts->secret_key != NULL && c->secret_key == NULL)
        goto fail;

    if (opts->session_token != NULL) {
        c->session_token = s3_strdup_a(&c->alloc, opts->session_token, err);
        if (c->session_token == NULL)
            goto fail;
    }

    if (opts->default_bucket != NULL) {
        c->default_bucket = s3_strdup_a(&c->alloc, opts->default_bucket, err);
        if (c->default_bucket == NULL)
            goto fail;
    }

    if (opts->ca_file != NULL) {
        c->ca_file = s3_strdup_a(&c->alloc, opts->ca_file, err);
        if (c->ca_file == NULL)
            goto fail;
    }

    if (opts->ca_path != NULL) {
        c->ca_path = s3_strdup_a(&c->alloc, opts->ca_path, err);
        if (c->ca_path == NULL)
            goto fail;
    }

    if (opts->proxy != NULL) {
        c->proxy = s3_strdup_a(&c->alloc, opts->proxy, err);
        if (c->proxy == NULL)
            goto fail;
    }

    s3_client_init_defaults(c, opts);

    /* Создаём backend. */
    switch (opts->backend) {
    case S3_HTTP_BACKEND_CURL_EASY:
        c->backend = s3_http_easy_backend_new(c, err);
        break;
    case S3_HTTP_BACKEND_CURL_MULTI:
        c->backend = s3_http_multi_backend_new(c, err);
        break;
    default:
        s3_error_set(err, S3_E_INVALID_ARG,
                     "Unknown HTTP backend type", 0, 0, 0);
        c->backend = NULL;
        break;
    }

    if (c->backend == NULL)
        goto fail;

    *out_client = c;
    s3_client_set_error(c, err); /* last_error = OK */
    return S3_E_OK;

fail:
    s3_client_set_error(c, err);
    if (c->backend != NULL && c->backend->vtbl != NULL &&
        c->backend->vtbl->destroy != NULL)
    {
        c->backend->vtbl->destroy(c->backend);
    }
    s3_client_free_strings(c);
    s3_free(&c->alloc, c);
    return err->code;
}

void
s3_client_delete(s3_client_t *client)
{
    if (client == NULL)
        return;

    if (client->backend != NULL && client->backend->vtbl != NULL &&
        client->backend->vtbl->destroy != NULL)
    {
        client->backend->vtbl->destroy(client->backend);
    }

    s3_client_free_strings(client);
    s3_free(&client->alloc, client);
}

/* ----------------- PUT / GET через coio_call ----------------- */

struct s3_put_task {
    s3_client_t *client;
    s3_put_opts_t opts;
    int fd;
    off_t offset;
    size_t size;
    size_t bytes_written;

    s3_error_t err;
    s3_error_code_t code;
};

static ssize_t
s3_client_put_fd_worker(va_list ap)
{
    struct s3_put_task *t = va_arg(ap, struct s3_put_task *);
    struct s3_http_backend_impl *b = t->client->backend;

    t->code = b->vtbl->put_fd(b, &t->opts,
                              t->fd, t->offset, t->size,
                              &t->err);
    return 0;
}

s3_error_code_t
s3_client_put_fd(s3_client_t *client,
                 const s3_put_opts_t *opts,
                 int fd, off_t offset, size_t size,
                 s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;
    s3_error_clear(err);

    if (client == NULL || opts == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client or opts is NULL", 0, 0, 0);
        if (client != NULL)
            s3_client_set_error(client, err);
        return err->code;
    }

    struct s3_put_task task;
    memset(&task, 0, sizeof(task));
    task.client = client;
    task.opts = *opts;
    task.fd = fd;
    task.offset = offset;
    task.size = size;
    s3_error_clear(&task.err);
    task.code = S3_E_OK;

    coio_call(s3_client_put_fd_worker, &task);

    *err = task.err;
    s3_client_set_error(client, &task.err);
    return task.code;
}

struct s3_get_task {
    s3_client_t *client;
    s3_get_opts_t opts;
    int fd;
    off_t offset;
    size_t max_size;
    size_t bytes_written;

    s3_error_t err;
    s3_error_code_t code;
};

static ssize_t
s3_client_get_fd_worker(va_list ap)
{
    struct s3_get_task *t = va_arg(ap, struct s3_get_task *);
    struct s3_http_backend_impl *b = t->client->backend;

    t->code = b->vtbl->get_fd(b, &t->opts,
                              t->fd, t->offset, t->max_size,
                              &t->bytes_written,
                              &t->err);
    return 0;
}

s3_error_code_t
s3_client_get_fd(s3_client_t *client,
                 const s3_get_opts_t *opts,
                 int fd, off_t offset, size_t max_size,
                 size_t *bytes_written,
                 s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;
    s3_error_clear(err);

    if (client == NULL || opts == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client or opts is NULL", 0, 0, 0);
        if (client != NULL)
            s3_client_set_error(client, err);
        return err->code;
    }

    struct s3_get_task task;
    memset(&task, 0, sizeof(task));
    task.client = client;
    task.opts = *opts;
    task.fd = fd;
    task.offset = offset;
    task.max_size = max_size;
    s3_error_clear(&task.err);
    task.code = S3_E_OK;

    coio_call(s3_client_get_fd_worker, &task);

    if (bytes_written != NULL)
        *bytes_written = task.bytes_written;

    *err = task.err;
    s3_client_set_error(client, &task.err);
    return task.code;
}

struct s3_create_bucket_task {
    s3_client_t *client;
    s3_create_bucket_opts_t opts;

    s3_error_t err;
    s3_error_code_t code;
};

static ssize_t
s3_client_create_bucket_worker(va_list ap)
{
    struct s3_create_bucket_task *t = va_arg(ap, struct s3_create_bucket_task *);
    struct s3_http_backend_impl *b = t->client->backend;
    t->code = b->vtbl->create_bucket(b, &t->opts, &t->err);

    return 0;
}

s3_error_code_t
s3_client_create_bucket(s3_client_t *client,
                        const s3_create_bucket_opts_t *opts,
                        s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;
    s3_error_clear(err);

    if (client == NULL || opts == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client or opts is NULL", 0, 0, 0);
        if (client != NULL)
            s3_client_set_error(client, err);
        return err->code;
    }

    struct s3_create_bucket_task task;
    memset(&task, 0, sizeof(task));
    task.client = client;
    task.opts = *opts;
    s3_error_clear(&task.err);
    task.code = S3_E_OK;

    coio_call(s3_client_create_bucket_worker, &task);

    *err = task.err;
    s3_client_set_error(client, &task.err);
    return task.code;
}


struct s3_list_objects_task {
    s3_client_t              *client;
    s3_list_objects_opts_t     opts;
    s3_list_objects_result_t  *out;

    s3_error_t         err;
    s3_error_code_t    code;
};

static ssize_t
s3_client_list_objects_worker(va_list ap)
{
    struct s3_list_objects_task *t = va_arg(ap, struct s3_list_objects_task *);
    struct s3_http_backend_impl *b = t->client->backend;
    t->code = b->vtbl->list_objects(b, &t->opts, t->out, &t->err);

    return 0;
}

s3_error_code_t
s3_client_list_objects(s3_client_t *client,
               const s3_list_objects_opts_t *opts,
               s3_list_objects_result_t *out,
               s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;
    s3_error_clear(err);

    if (client == NULL || opts == NULL || out == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts or out is NULL in list_objects", 0, 0, 0);
        if (client != NULL)
            s3_client_set_error(client, err);
        return err->code;
    }

    memset(out, 0, sizeof(*out));

    struct s3_list_objects_task task;
    memset(&task, 0, sizeof(task));
    task.client = client;
    task.opts = *opts;
    task.out = out;
    s3_error_clear(&task.err);
    task.code = S3_E_OK;

    coio_call(s3_client_list_objects_worker, &task);

    *err = task.err;
    s3_client_set_error(client, &task.err);
    return task.code;
}

void
s3_list_objects_result_destroy(s3_client_t *client, s3_list_objects_result_t *res)
{
    if (client == NULL || res == NULL)
        return;

    s3_allocator_t *a = &client->alloc;

    if (res->objects != NULL) {
        for (size_t i = 0; i < res->count; i++) {
            s3_object_info_t *o = &res->objects[i];
            if (o->key)           s3_free(a, o->key);
            if (o->etag)          s3_free(a, o->etag);
            if (o->last_modified) s3_free(a, o->last_modified);
            if (o->storage_class) s3_free(a, o->storage_class);
        }
        s3_free(a, res->objects);
    }

    if (res->next_continuation_token)
        s3_free(a, res->next_continuation_token);

    memset(res, 0, sizeof(*res));
}

struct s3_delete_objects_task {
    s3_client_t *client;
    s3_delete_objects_opts_t opts;

    s3_error_t err;
    s3_error_code_t code;
};

static ssize_t
s3_client_delete_objects_worker(va_list ap)
{
    struct s3_delete_objects_task *t = va_arg(ap, struct s3_delete_objects_task *);
    struct s3_http_backend_impl *b = t->client->backend;
    t->code = b->vtbl->delete_objects(b, &t->opts, &t->err);

    return 0;
}

s3_error_code_t
s3_client_delete_objects(s3_client_t *client,
                         const s3_delete_objects_opts_t *opts,
                         s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;
    s3_error_clear(err);

    if (client == NULL || opts == NULL ||
        opts->objects == NULL || opts->count == 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client, opts, objects or count is invalid in delete_objects",
                     0, 0, 0);
        return err->code;
    }

    struct s3_delete_objects_task task;
    memset(&task, 0, sizeof(task));
    task.client = client;
    task.opts   = *opts;       /* копируем саму структуру */
    s3_error_clear(&task.err);
    task.code   = S3_E_OK;

    coio_call(s3_client_delete_objects_worker, &task);

    *err = task.err;
    s3_client_set_error(client, &task.err);
    return task.code;
}